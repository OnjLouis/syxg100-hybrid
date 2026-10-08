#include "MuExternalInsertions.h"
#include "XgEffectsBridge.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hybrid {

void MuExternalInsertions::reset() noexcept
{
    for (auto& slot : slots) {
        slot = {};
        slot.revision = ++revision;
    }
}

bool MuExternalInsertions::observe(std::span<const std::uint8_t> bytes) noexcept
{
    if (bytes.size() < 9 || bytes.front() != 0xf0 || bytes.back() != 0xf7
        || bytes[1] != 0x43 || (bytes[2] & 0xf0) != 0x10
        || bytes[3] != 0x4c || bytes[4] != 3 || bytes[5] >= slotCount
        || std::any_of(bytes.begin()+1, bytes.end()-1,
                       [](auto value) { return value > 127; }))
        return false;
    const auto address = bytes[6];
    const auto count = bytes.size()-8;
    if (address + count > 0x26)
        return false;
    auto& slot = slots[bytes[5]];
    for (std::size_t index = 0; index < count; ++index) {
        const auto offset = address + index;
        const auto value = bytes[7+index];
        if (offset == 0) {
            slot.msb = value;
            slot.typeRevision = revision + 1;
            slot.parameterSeen = {};
            slot.controlDepths = {};
        } else if (offset == 1) {
            slot.lsb = value;
            slot.typeRevision = revision + 1;
        } else if (offset >= 2 && offset <= 0x0b) {
            slot.parameters[offset-2] = value;
            slot.parameterSeen[offset-2] = true;
        } else if (offset == 0x0c) {
            slot.part = value;
        } else if (offset >= 0x0d && offset <= 0x11) {
            slot.controlDepths[offset-0x0d] = value;
        } else if (offset >= 0x20 && offset <= 0x25) {
            slot.parameters[offset-0x20+10] = value;
            slot.parameterSeen[offset-0x20+10] = true;
        }
    }
    slot.revision = ++revision;
    return true;
}

std::optional<std::size_t> MuExternalInsertions::target(
    std::uint8_t channel) const noexcept
{
    if (channel >= 16)
        return std::nullopt;
    std::optional<std::size_t> result;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (!slots[index].supported() || slots[index].part != channel)
            continue;
        // Chained insertions need a separate contract; never silently select
        // one processor when the MIDI assigns two to the same part.
        if (result)
            return std::nullopt;
        result = index;
    }
    return result;
}

std::optional<std::size_t> MuExternalInsertions::soleTarget(
    std::uint32_t mask) const noexcept
{
    if (mask == 0 || (mask & (mask-1)) != 0)
        return std::nullopt;
    for (std::uint8_t channel = 0; channel < 16; ++channel)
        if (mask == (std::uint32_t{1} << channel))
            return target(channel);
    return std::nullopt;
}

XgExternalInsertion::XgExternalInsertion(
    vst2::EntryPoint entry, vst2::HostCallback host, float rate)
{
    effect = entry(host);
    if (effect == nullptr || effect->magic != vst2::effectMagic)
        throw std::runtime_error("Cannot create independent XG insertion DSP");
    effect->dispatcher(effect, vst2::open, 0, 0, nullptr, 0);
    effect->dispatcher(effect, vst2::setSampleRate, 0, 0, nullptr, rate);
    effect->dispatcher(effect, vst2::setBlockSize, 0, quantum, nullptr, 0);
    effect->dispatcher(effect, vst2::mainsChanged, 0, 1, nullptr, 0);
}

XgExternalInsertion::~XgExternalInsertion()
{
    if (effect != nullptr) {
        effect->dispatcher(effect, vst2::mainsChanged, 0, 0, nullptr, 0);
        effect->dispatcher(effect, vst2::close, 0, 0, nullptr, 0);
    }
}

void XgExternalInsertion::setSampleRate(float rate)
{
    effect->dispatcher(effect, vst2::setSampleRate, 0, 0, nullptr, rate);
    reset();
}

void XgExternalInsertion::reset() noexcept
{
    configurationValid = false;
    weights = {};
}

void XgExternalInsertion::queue(std::span<const std::uint8_t> bytes)
{
    const auto index = static_cast<std::size_t>(events.numEvents);
    if (index >= data.size())
        throw std::runtime_error("Insertion configuration exceeds fixed capacity");
    std::copy(bytes.begin(), bytes.end(), data[index].begin());
    sysex[index] = {};
    sysex[index].dumpBytes = static_cast<std::int32_t>(bytes.size());
    sysex[index].sysexDump = reinterpret_cast<char*>(data[index].data());
    events.events[index] = reinterpret_cast<vst2::Event*>(&sysex[index]);
    ++events.numEvents;
}

void XgExternalInsertion::message(std::uint8_t address, std::uint8_t value)
{
    const std::array<std::uint8_t, 9> bytes {
        0xf0, 0x43, 0x10, 0x4c, 2, 1, address, value, 0xf7
    };
    queue(bytes);
}

void XgExternalInsertion::type(std::uint8_t msb, std::uint8_t lsb)
{
    const std::array<std::uint8_t, 10> bytes {
        0xf0, 0x43, 0x10, 0x4c, 2, 1, 0x40, msb, lsb, 0xf7
    };
    queue(bytes);
}

void XgExternalInsertion::configure(const MuExternalInsertions::Slot& slot)
{
    events.numEvents = 0;
    const bool initialize = !configurationValid;
    const bool typeChanged = initialize
        || configuredSlot.typeRevision != slot.typeRevision;
    const std::array<std::uint8_t, 9> resetMessage {
        0xf0, 0x43, 0x10, 0x4c, 0, 0, 0x7e, 0, 0xf7
    };
    if (initialize) {
        queue(resetMessage);
        message(0x5a, 0); // Insertion preserves the requested Dry/Wet balance.
        message(0x5b, 0); // Private voice-free part; song assignments stay separate.
        message(0x56, 64);
        message(0x57, 64);
        message(0x58, 0);
        message(0x59, 0);
        message(0x0c, 0);
        message(0x2c, 0);
        for (const auto [address, value] :
             std::array<std::array<std::uint8_t, 2>, 5>{{
                 {0x0b, 127}, {0x11, 127}, {0x12, 0}, {0x13, 0}, {0x14, 0}}}) {
            const std::array<std::uint8_t, 9> bytes {
                0xf0, 0x43, 0x10, 0x4c, 8, 0, address, value, 0xf7
            };
            queue(bytes);
        }
    }
    if (typeChanged)
        type(slot.msb, slot.lsb);
    for (std::size_t parameter = 0; parameter < slot.parameters.size(); ++parameter) {
        if (!slot.parameterSeen[parameter])
            continue;
        if (!typeChanged && configuredSlot.parameterSeen[parameter]
            && configuredSlot.parameters[parameter] == slot.parameters[parameter])
            continue;
        if (parameter < 10) {
            const std::array<std::uint8_t, 10> bytes {
                0xf0, 0x43, 0x10, 0x4c, 2, 1,
                static_cast<std::uint8_t>(0x42 + 2*parameter),
                0, slot.parameters[parameter], 0xf7
            };
            queue(bytes);
        } else {
            message(static_cast<std::uint8_t>(0x70 + parameter-10),
                    slot.parameters[parameter]);
        }
    }
    effect->dispatcher(effect, vst2::processEvents, 0, 0, &events, 0);
    configuredRevision = slot.revision;
    configuredSlot = slot;
    configurationValid = true;
}

void XgExternalInsertion::inject(void* context, float* buses,
                                 std::uint32_t frames) noexcept
{
    const auto& processor = *static_cast<XgExternalInsertion*>(context);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        // Variation consumes a mono sum; reconstruct the original panning
        // after the effect and feed the worker's post-insertion sends once.
        const float mono = (processor.input[0][processor.position+frame]
            + processor.input[1][processor.position+frame]) * 0.5f;
        buses[6*quantum+frame] += mono * processor.gain * 32768.0f;
        buses[7*quantum+frame] += mono * processor.gain * 32768.0f;
    }
}

bool XgExternalInsertion::render(const MuExternalInsertions::Slot& slot,
    std::span<float* const> source, std::span<float* const> mix,
    std::size_t frames, float nativeGain)
{
    if (!slot.supported() || source.size() != 8 || mix.size() != 8
        || frames % quantum != 0 || !std::isfinite(nativeGain) || nativeGain <= 0)
        return false;
    input = source;
    gain = nativeGain;
    if (!configurationValid || configuredRevision != slot.revision)
        configure(slot);
    float* outputs[] {output[0].data(), output[1].data()};
    for (position = 0; position < frames; position += quantum) {
        // Least-squares send/pan recovery avoids dividing by individual zero
        // crossings and retains the worker's volume and controller scaling.
        double energy {};
        std::array<double, 8> product {};
        for (std::size_t frame = position; frame < position+quantum; ++frame) {
            const double mono = (source[0][frame]+source[1][frame])*0.5;
            energy += mono*mono;
            for (std::size_t bus = 0; bus < product.size(); ++bus)
                product[bus] += mono*source[bus][frame];
        }
        if (energy > 1.0e-12)
            for (std::size_t bus = 0; bus < weights.size(); ++bus)
                weights[bus] = static_cast<float>(product[bus]/energy);
        XgEffectsBridge::beginBlock(this, inject);
        effect->processReplacing(effect, nullptr, outputs, quantum);
        XgEffectsBridge::endBlock();
        // Event pointers stay alive until the engine consumed this quantum.
        events.numEvents = 0;
        effect->dispatcher(effect, vst2::processEvents, 0, 0, &events, 0);
        for (std::size_t frame = 0; frame < quantum; ++frame) {
            const float mono = (output[0][frame]+output[1][frame])
                * 0.5f/nativeGain;
            for (std::size_t bus = 0; bus < weights.size(); ++bus)
                mix[bus][position+frame] += mono*weights[bus];
        }
    }
    input = {};
    return true;
}

} // namespace hybrid
