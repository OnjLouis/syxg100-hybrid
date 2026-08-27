#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace hybrid {

template <std::size_t VoiceCount>
class VlVoiceAllocator {
public:
    static constexpr std::uint8_t unassignedChannel = 0xff;
    static constexpr std::size_t noVoice = VoiceCount;

    struct Allocation {
        std::size_t voice {noVoice};
        bool reassigned {};
        bool stoleActiveNote {};
    };

    void reset() noexcept { *this = {}; }

    bool configureVoice(std::size_t voice, std::uint8_t channel) noexcept
    {
        if (voice >= VoiceCount || channel >= 16)
            return false;
        configured = true;
        if (configuredChannels[voice] == channel)
            return false;
        configuredChannels[voice] = channel;
        slots[voice] = {};
        return true;
    }

    Allocation noteOn(std::uint8_t channel, std::uint8_t note) noexcept
    {
        if (!configured)
            return noteOnMonophonic(0, channel, note);

        const auto assignedCount = countAssigned(channel);
        if (assignedCount == 0)
            return {};
        if (assignedCount == 1)
            return noteOnMonophonic(findConfigured(channel), channel, note);

        auto voice = findIdle(channel);
        if (voice == noVoice)
            voice = findUnassigned(channel);
        if (voice == noVoice)
            voice = findOldest(channel, false);
        if (voice == noVoice)
            voice = findOldest(channel, true);
        if (voice == noVoice)
            return {};
        auto& slot = slots[voice];
        const Allocation result {
            voice, slot.channel != channel, slot.active
        };
        slot.channel = channel;
        slot.note = note;
        slot.active = true;
        slot.heldNotes = {};
        slot.age = ++clock;
        return result;
    }

    std::size_t noteOff(std::uint8_t channel, std::uint8_t note) noexcept
    {
        if (!configured)
            return noteOffMonophonic(0, channel, note);

        const auto assignedCount = countAssigned(channel);
        if (assignedCount == 0)
            return noVoice;
        if (assignedCount == 1) {
            return noteOffMonophonic(findConfigured(channel), channel, note);
        }

        auto found = noVoice;
        auto oldest = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t voice = 0; voice < slots.size(); ++voice) {
            const auto& slot = slots[voice];
            if (slot.active && slot.channel == channel && slot.note == note
                && slot.age < oldest) {
                found = voice;
                oldest = slot.age;
            }
        }
        if (found != noVoice)
            slots[found].active = false;
        return found;
    }

    [[nodiscard]] std::uint8_t channel(std::size_t voice) const noexcept
    {
        return slots[voice].channel;
    }

    [[nodiscard]] bool active(std::size_t voice) const noexcept
    {
        return slots[voice].active;
    }

    [[nodiscard]] bool hasExplicitConfiguration() const noexcept
    {
        return configured;
    }

    void release(std::size_t voice) noexcept
    {
        if (voice < slots.size())
            slots[voice] = {};
    }

private:
    struct Slot {
        std::uint8_t channel {unassignedChannel};
        std::uint8_t note {};
        bool active {};
        std::uint64_t age {};
        std::array<std::uint16_t, 128> heldNotes {};
    };

    Allocation noteOnMonophonic(std::size_t voice, std::uint8_t channel,
                                std::uint8_t note) noexcept
    {
        auto& slot = slots[voice];
        const bool reassigned = slot.channel != channel;
        const bool stoleActiveNote = slot.active;
        if (reassigned)
            slot.heldNotes = {};
        slot.channel = channel;
        slot.note = note;
        if (slot.heldNotes[note] != std::numeric_limits<std::uint16_t>::max())
            ++slot.heldNotes[note];
        slot.active = true;
        slot.age = ++clock;
        return { voice, reassigned, stoleActiveNote };
    }

    std::size_t noteOffMonophonic(std::size_t voice, std::uint8_t channel,
                                  std::uint8_t note) noexcept
    {
        if (voice == noVoice || slots[voice].channel == unassignedChannel)
            return noVoice;
        auto& slot = slots[voice];
        if (slot.channel == channel && slot.heldNotes[note] != 0)
            --slot.heldNotes[note];
        slot.active = false;
        for (const auto count : slot.heldNotes) {
            if (count != 0) {
                slot.active = true;
                break;
            }
        }
        return voice;
    }

    [[nodiscard]] std::size_t countAssigned(
        std::uint8_t channel) const noexcept
    {
        std::size_t count {};
        for (const auto assigned : configuredChannels)
            count += assigned == channel;
        return count;
    }

    [[nodiscard]] std::size_t findConfigured(
        std::uint8_t channel) const noexcept
    {
        for (std::size_t voice = 0; voice < VoiceCount; ++voice) {
            if (configuredChannels[voice] == channel)
                return voice;
        }
        return noVoice;
    }

    std::size_t findIdle(std::uint8_t channel) const noexcept
    {
        for (std::size_t voice = 0; voice < slots.size(); ++voice) {
            if (!slots[voice].active && slots[voice].channel == channel)
                return voice;
        }
        return noVoice;
    }

    std::size_t findUnassigned(std::uint8_t channel) const noexcept
    {
        for (std::size_t voice = 0; voice < slots.size(); ++voice) {
            if (configuredChannels[voice] == channel
                && slots[voice].channel == unassignedChannel) {
                return voice;
            }
        }
        return noVoice;
    }

    std::size_t findOldest(std::uint8_t channel, bool active) const noexcept
    {
        auto found = noVoice;
        auto oldest = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t voice = 0; voice < slots.size(); ++voice) {
            if (configuredChannels[voice] == channel
                && slots[voice].active == active
                && slots[voice].age < oldest) {
                found = voice;
                oldest = slots[voice].age;
            }
        }
        return found;
    }

    std::array<Slot, VoiceCount> slots {};
    std::array<std::uint8_t, VoiceCount> configuredChannels = [] {
        std::array<std::uint8_t, VoiceCount> result {};
        result.fill(unassignedChannel);
        return result;
    }();
    bool configured {};
    std::uint64_t clock {};
};

} // namespace hybrid
