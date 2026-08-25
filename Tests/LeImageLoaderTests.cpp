#include "LeImageLoader.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

void write16(std::vector<std::uint8_t>& bytes, std::size_t offset,
             std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void write32(std::vector<std::uint8_t>& bytes, std::size_t offset,
             std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
}

std::filesystem::path writeFixture()
{
    constexpr std::size_t header = 0x80;
    constexpr std::size_t objectTable = header + 0xc4;
    constexpr std::size_t pageTable = header + 0xdc;
    constexpr std::size_t fixupPages = header + 0xe4;
    constexpr std::size_t fixupRecords = header + 0xf0;
    constexpr std::size_t dataPages = 0x200;
    std::vector<std::uint8_t> bytes(0x220);
    write32(bytes, 0x3c, header);
    bytes[header] = 'L';
    bytes[header + 1] = 'E';
    write32(bytes, header + 0x14, 2);
    write32(bytes, header + 0x28, 16);
    write32(bytes, header + 0x2c, 8);
    write32(bytes, header + 0x40, 0xc4);
    write32(bytes, header + 0x44, 1);
    write32(bytes, header + 0x48, 0xdc);
    write32(bytes, header + 0x68, 0xe4);
    write32(bytes, header + 0x6c, 0xf0);
    write32(bytes, header + 0x80, dataPages);
    write32(bytes, objectTable, 24);
    write32(bytes, objectTable + 12, 1);
    write32(bytes, objectTable + 16, 2);
    bytes[pageTable + 2] = 1;
    bytes[pageTable + 6] = 2;
    write32(bytes, fixupPages, 0);
    write32(bytes, fixupPages + 4, 9);
    write32(bytes, fixupPages + 8, 21);

    auto record = fixupRecords;
    bytes[record++] = 7;
    bytes[record++] = 0x10;
    write16(bytes, record, 2);
    record += 2;
    bytes[record++] = 1;
    write32(bytes, record, 0x20);
    record += 4;
    bytes[record++] = 0x27;
    bytes[record++] = 0x10;
    bytes[record++] = 2;
    bytes[record++] = 1;
    write32(bytes, record, 0x30);
    record += 4;
    write16(bytes, record, 0xfffe);
    record += 2;
    write16(bytes, record, 4);

    for (std::uint8_t index = 0; index < 24; ++index)
        bytes[dataPages + index] = index;
    const auto path = std::filesystem::temp_directory_path()
                    / "syxg100-le-loader-test.bin";
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return path;
}

std::filesystem::path writeMultiObjectFixture()
{
    constexpr std::size_t header = 0x80;
    constexpr std::size_t objectTable = header + 0xc4;
    constexpr std::size_t pageTable = header + 0xf4;
    constexpr std::size_t fixupPages = header + 0xfc;
    constexpr std::size_t fixupRecords = header + 0x108;
    constexpr std::size_t dataPages = 0x200;
    std::vector<std::uint8_t> bytes(0x220);
    write32(bytes, 0x3c, header);
    bytes[header] = 'L';
    bytes[header + 1] = 'E';
    write32(bytes, header + 0x14, 2);
    write32(bytes, header + 0x28, 16);
    write32(bytes, header + 0x2c, 16);
    write32(bytes, header + 0x40, 0xc4);
    write32(bytes, header + 0x44, 2);
    write32(bytes, header + 0x48, 0xf4);
    write32(bytes, header + 0x68, 0xfc);
    write32(bytes, header + 0x6c, 0x108);
    write32(bytes, header + 0x80, dataPages);

    write32(bytes, objectTable, 16);
    write32(bytes, objectTable + 12, 1);
    write32(bytes, objectTable + 16, 1);
    write32(bytes, objectTable + 24, 16);
    write32(bytes, objectTable + 24 + 12, 2);
    write32(bytes, objectTable + 24 + 16, 1);
    bytes[pageTable + 2] = 1;
    bytes[pageTable + 6] = 2;
    write32(bytes, fixupPages, 0);
    write32(bytes, fixupPages + 4, 9);
    write32(bytes, fixupPages + 8, 18);

    auto record = fixupRecords;
    bytes[record++] = 7;
    bytes[record++] = 0x10;
    write16(bytes, record, 0);
    record += 2;
    bytes[record++] = 2;
    write32(bytes, record, 4);
    record += 4;
    bytes[record++] = 8;
    bytes[record++] = 0x10;
    write16(bytes, record, 4);
    record += 2;
    bytes[record++] = 1;
    write32(bytes, record, 8);

    for (std::uint8_t index = 0; index < 32; ++index)
        bytes[dataPages + index] = index;
    const auto path = std::filesystem::temp_directory_path()
                    / "syxg100-le-loader-multi-test.bin";
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return path;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2) {
        try {
            const auto image = hybrid::loadLeImage(argv[1], 0x20000000);
            std::printf("loaded LE image bytes=%zu\n", image.size());
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "LE image load failed: %s\n", error.what());
            return 1;
        }
    }
    if (argc != 1)
        return 64;
    const auto path = writeFixture();
    try {
        const auto image = hybrid::loadLeImage(path, 0x1000);
        std::filesystem::remove(path);
        const std::array<std::uint8_t, 24> expected {
            0, 1, 0x20, 0x10, 0, 0, 6, 7,
            8, 9, 10, 11, 12, 13, 14, 15,
            0, 0, 18, 19, 0x30, 0x10, 0, 0,
        };
        if (!std::equal(image.begin(), image.end(), expected.begin(), expected.end())) {
            std::fprintf(stderr, "LE loader output did not match the fixture\n");
            return 1;
        }

        const auto multiPath = writeMultiObjectFixture();
        const auto multi = hybrid::loadLeImage(multiPath, 0x20000000);
        std::filesystem::remove(multiPath);
        if (multi.size() != 0x100010
            || *reinterpret_cast<const std::uint32_t*>(multi.data())
                != 0x20100004
            || *reinterpret_cast<const std::uint32_t*>(
                   multi.data() + 0x100004) != 0xfff00000) {
            std::fprintf(stderr, "multi-object LE fixups did not match\n");
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove(path);
        std::fprintf(stderr, "LE loader test failed: %s\n", error.what());
        return 1;
    }
}
