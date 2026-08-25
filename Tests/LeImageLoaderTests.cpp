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

} // namespace

int main()
{
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
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove(path);
        std::fprintf(stderr, "LE loader test failed: %s\n", error.what());
        return 1;
    }
}
