#include "ConvertedMuVoiceMap.h"
#include <cstdio>
int main() {
    int failures = 0;
    const auto expect = [&](bool ok, const char* text) {
        if (!ok) { std::fprintf(stderr,"FAIL: %s\n",text); ++failures; }
    };
    std::vector<std::uint8_t> bytes(145, 255);
    std::memcpy(bytes.data(), "HYMUVM01", 8);
    bytes[8 + 88] = 59;
    bytes[136] = 1; bytes[137] = bytes[138] = bytes[139] = 0;
    const auto key = (48u << 14) | (88u << 7) | 5u;
    for (unsigned i = 0; i < 4; ++i) bytes[140 + i] = (key >> (8*i)) & 255;
    bytes[144] = 3;
    const hybrid::ConvertedMuVoiceMap map(bytes);
    expect(map.valid(), "metadata accepted");
    expect(map.hasVoice(48,88,5,false) && map.hasVoice(48,88,5,true), "both voice maps advertised");
    expect(!map.hasVoice(48,88,6,false), "missing patch not advertised");
    expect(map.engineBank(48,88) == 59, "exclusive bank uses its private row");
    expect(map.engineBank(48,0) == 48, "existing exclusive base bank unchanged");
    expect(map.engineBank(48,1) == 1, "undefined exclusive row remains silent");
    expect(map.engineBank(59,0) == 1, "private row is not a new public MIDI bank");
    hybrid::ConvertedMuBankRouter router;
    router.observeShort(0x003000b1);
    router.observeShort(0x005820b1);
    expect(router.needsTranslation(map,1) && router.engineBank(map,1) == 59, "CC bank selection tracked per channel");
    expect(!router.needsTranslation(map,2), "other channel unaffected");
    router.reset(); expect(!router.needsTranslation(map,1), "reset clears routing state");
    const std::array<std::uint8_t,10> setup {0xf0,0x43,0x10,0x4c,8,1,1,48,88,0xf7};
    router.observeSysex(setup);
    expect(router.engineBank(map,1) == 59, "contiguous SysEx bank fields tracked");
    bytes[8+88] = 33;
    expect(!hybrid::ConvertedMuVoiceMap(bytes).valid(), "reserved VL bank rejected");
    bytes[8+88] = 59; bytes[144] = 4;
    expect(!hybrid::ConvertedMuVoiceMap(bytes).valid(), "unsupported flags rejected");
    expect(!hybrid::ConvertedMuVoiceMap(std::span(bytes).first(144)).valid(), "truncated metadata rejected");
    const hybrid::ConvertedMuVoiceMap legacy;
    expect(legacy.engineBank(48,88) == 48, "legacy engines are not remapped");
    return failures ? 1 : 0;
}
