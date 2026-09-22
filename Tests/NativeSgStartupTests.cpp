#include "NativeSgImagePatch.h"

#include <cstdio>
#include <vector>

int main()
{
    constexpr std::size_t offset = 0x1533c8;
    std::vector<std::uint8_t> image(offset + 32, 0xa5);
    const std::array<std::uint8_t, 4> original {0x80, 0x65, 0xfc, 0x00};
    const std::array<std::uint8_t, 4> corrected {0xc6, 0x45, 0xfc, 0x01};
    std::copy(original.begin(), original.end(), image.begin() + offset);
    const auto before = image;
    hybrid::patchNativeSgStartup(image);
    for (std::size_t i = 0; i < image.size(); ++i) {
        const auto expected = i >= offset && i < offset + corrected.size()
            ? corrected[i - offset] : before[i];
        if (image[i] != expected) return 1;
    }
    for (const auto size : {std::size_t(0), offset, offset + 3, offset + 32}) {
        std::vector<std::uint8_t> invalid(size, 0xa5);
        const auto unchanged = invalid;
        bool rejected = false;
        try { hybrid::patchNativeSgStartup(invalid); }
        catch (const std::runtime_error&) { rejected = true; }
        if (!rejected || invalid != unchanged) return 2;
    }
    std::puts("Native SG startup tests passed");
}
