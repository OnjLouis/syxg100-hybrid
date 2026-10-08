#include "MuExternalInsertions.h"
#include "XgEffectsBridge.h"
#include <cmath>
#include <cstdio>
#include <vector>

vst2::IntPtr host(vst2::AEffect*, int opcode, int, vst2::IntPtr, void*, float)
{
    if (opcode == vst2::hostVersion) return 2400;
    if (opcode == vst2::hostGetSampleRate) return 48000;
    if (opcode == vst2::hostGetBlockSize) return 128;
    return 0;
}

int main(int argc, char** argv)
{
    hybrid::MuExternalInsertions state;
    int failures {};
    auto expect = [&](bool condition, const char* name) {
        if (!condition) { std::printf("FAIL: %s\n", name); ++failures; }
    };
    auto message = [&](int slot, int address, std::initializer_list<int> values) {
        std::vector<std::uint8_t> bytes {0xf0,0x43,0x10,0x4c,3,
            static_cast<std::uint8_t>(slot),static_cast<std::uint8_t>(address)};
        for (auto value : values) bytes.push_back(value);
        bytes.push_back(0xf7);
        state.observe(bytes);
    };
    message(1,0,{0x49,0});
    expect(!state.target(0), "unassigned type stays bypassed");
    message(1,0x0c,{0});
    expect(state.target(0)==1, "FatPizz insertion 2 routes channel 1");
    expect(!state.target(1), "other part stays unchanged");
    message(1,2,{74,40,64,60,110,0,0,64,0,96});
    expect(state.slot(1).parameters[0]==74, "Drive maps first parameter");
    expect(state.slot(1).parameters[9]==96, "dry/wet maps parameter 10");
    message(3,0,{0x4a,0});
    message(3,0x0c,{4});
    expect(state.target(4)==3, "insertion 4 routes separately");
    expect(state.soleTarget(1)==1, "SG single part can route");
    expect(!state.soleTarget(3), "SG multiple parts cannot be misrouted");
    message(0,0,{0x49,0});
    message(0,0x0c,{0});
    expect(!state.target(0), "ambiguous chain bypasses safely");
    message(0,0x0c,{127});
    expect(state.target(0)==1, "off restores unambiguous route");
    message(1,0x0d,{30});
    expect(!state.target(0), "unsupported controller modulation bypasses");
    message(1,0x0d,{0});
    expect(state.target(0)==1, "neutral controller modulation restores route");
    const auto before = state.slot(1).revision;
    const std::uint8_t bad[] {0xf0,0x43,0x10,0x4c,3,1,2,0x80,0xf7};
    expect(!state.observe(bad), "invalid payload rejected");
    expect(state.slot(1).revision==before, "invalid payload cannot change state");
    message(1,0,{0x05,0});
    expect(!state.target(0), "unsupported algorithm bypassed");
    state.reset();
    expect(!state.target(4), "reset clears insertion assignments");
    message(2,0,{0x4a,0});
    message(2,0x0c,{7});
    expect(state.target(7)==2, "overdrive assignment after reset");
    message(2,0x0c,{8});
    expect(!state.target(7) && state.target(8)==2,
           "midstream reassignment releases the previous part");
    expect(state.soleTarget(1u<<8)==2, "single SG route follows reassignment");
    expect(!state.soleTarget((1u<<8)|(1u<<9)),
           "multi-part SG stays out of the independent insertion");
    message(2,2,{127});
    message(2,0,{0x49,0});
    expect(!state.slot(2).parameterSeen[0],
           "type change clears parameters from the previous algorithm");
    state.reset();
    if (argc < 2) return failures;

    auto module = LoadLibraryA(argv[1]);
    expect(module!=nullptr,"load engine");
    if (!module) return 1;
    expect(hybrid::XgEffectsBridge::acquire(module), "acquire bridge");
    auto entry = reinterpret_cast<vst2::EntryPoint>(GetProcAddress(module,"main"));
    {
        hybrid::XgExternalInsertion processor(entry,host,48000);
        message(1,0,{0x49,0});
        message(1,0x0c,{0});
        message(1,2,{74});
        message(1,6,{110});
        message(1,0x0b,{96});
        constexpr std::size_t frames=128;
        std::array<std::array<float,frames>,8> input{}, output{};
        std::array<float*,8> sources{}, mixes{};
        for (int bus=0;bus<8;++bus) {
            sources[bus]=input[bus].data(); mixes[bus]=output[bus].data();
        }
        double difference {},energy {};
        float peak {};
        for (int block=0;block<200;++block) {
            for (int frame=0;frame<128;++frame) {
                const float sine=0.03f*std::sin(2.0*3.141592653589793*220*(block*128+frame)/48000);
                input[0][frame]=sine; input[1][frame]=sine*0.5f;
                input[2][frame]=sine*0.2f; input[3][frame]=sine*0.2f;
                input[6][frame]=sine*0.3f; input[7][frame]=sine*0.3f;
            }
            for(auto& bus:output) bus.fill(0);
            expect(processor.render(state.slot(1),sources,mixes,frames,3.5f),"render quantum");
            for(int frame=0;frame<128;++frame) {
                const auto value=output[0][frame];
                expect(std::isfinite(value),"finite DSP output");
                peak=std::max(peak,std::abs(value));
                difference += std::pow(value-input[0][frame],2);
                energy += value*value;
                expect(std::abs(output[1][frame]-value*0.5f)<1e-5f,"panning retained");
                expect(std::abs(output[2][frame]-value*0.2f)<1e-5f,"post distortion reverb send");
                expect(std::abs(output[6][frame]-value*0.3f)<1e-5f,"post distortion delay send");
            }
        }
        std::printf("DSP peak=%f energy=%f dryDifference=%f\n",peak,energy,difference);
        expect(energy>1,"distortion output audible");
        expect(difference>0.1,"distortion is not a dry passthrough");
        expect(!processor.render(state.slot(1),sources,mixes,96,3.5f),"non-quantized input refused");
    }
    {
        hybrid::MuExternalInsertions::Slot slot;
        slot.msb = 0x49; slot.part = 0;
        slot.parameters[0] = 74; slot.parameterSeen[0] = true;
        slot.parameters[4] = 110; slot.parameterSeen[4] = true;
        slot.parameterSeen[9] = true;
        std::array<double, 2> energy {}, dryDifference {};
        std::array<std::array<float, 128>, 8> input {}, output {};
        std::array<float*, 8> sources {}, mixes {};
        for (std::size_t bus = 0; bus < sources.size(); ++bus) {
            sources[bus] = input[bus].data(); mixes[bus] = output[bus].data();
        }
        for (std::size_t mode = 0; mode < 2; ++mode) {
            hybrid::XgExternalInsertion processor(entry, host, 48000);
            slot.parameters[9] = mode == 0 ? 1 : 127;
            for (int block = 0; block < 200; ++block) {
                for (int frame = 0; frame < 128; ++frame) {
                    const float sine = 0.03f * std::sin(2.0 * 3.141592653589793
                        * 220 * (block * 128 + frame) / 48000);
                    input[0][frame] = sine; input[1][frame] = sine;
                }
                for (auto& bus : output) bus.fill(0);
                expect(processor.render(slot, sources, mixes, 128, 3.5f),
                       "dry/wet regression quantum");
                if (block < 20) continue;
                for (int frame = 0; frame < 128; ++frame) {
                    energy[mode] += output[0][frame] * output[0][frame];
                    dryDifference[mode] += std::pow(output[0][frame] - input[0][frame], 2);
                }
            }
        }
        std::printf("Dry/Wet energy dry=%f wet=%f differences dry=%f wet=%f\n",
                    energy[0], energy[1], dryDifference[0], dryDifference[1]);
        expect(energy[0] > 1 && energy[1] > energy[0] * 2,
               "independent insertion honours dry/wet rather than forcing wet-only");
    }
    {
        hybrid::XgExternalInsertion processor(entry, host, 48000);
        hybrid::MuExternalInsertions::Slot slot;
        slot.msb = 0x49; slot.part = 0;
        slot.parameters[0] = 74; slot.parameterSeen[0] = true;
        slot.parameters[4] = 110; slot.parameterSeen[4] = true;
        slot.parameters[9] = 127; slot.parameterSeen[9] = true;
        std::array<std::array<float, 128>, 8> input {}, output {};
        std::array<float*, 8> sources {}, mixes {};
        for (std::size_t bus = 0; bus < sources.size(); ++bus) {
            sources[bus] = input[bus].data(); mixes[bus] = output[bus].data();
        }
        std::array<double, 3> energies {};
        for (int mode = 0; mode < 3; ++mode) {
            if (mode == 1) {
                slot.msb = 0x4a; ++slot.typeRevision; ++slot.revision;
            }
            if (mode == 2) processor.reset();
            for (int block = 0; block < 80; ++block) {
                for (int frame = 0; frame < 128; ++frame) {
                    const float sine = 0.03f * std::sin(2.0 * 3.141592653589793
                        * 220 * (block * 128 + frame) / 48000);
                    input[0][frame] = sine; input[1][frame] = sine;
                }
                for (auto& bus : output) bus.fill(0);
                expect(processor.render(slot, sources, mixes, 128, 3.5f),
                       "midstream type/reset render");
                for (float value : output[0]) {
                    expect(std::isfinite(value), "midstream output remains finite");
                    if (block >= 10) energies[mode] += value * value;
                }
            }
        }
        expect(energies[1] > 1, "overdrive output is audible");
        expect(std::abs(energies[1] - energies[2]) < energies[1] * 0.01,
               "reset reapplies the current overdrive configuration");
    }
    hybrid::XgEffectsBridge::release(module);
    FreeLibrary(module);
    std::printf("failures=%d\n",failures);
    return failures ? 1 : 0;
}
