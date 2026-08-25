#include "LeImageLoader.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace hybrid {
namespace {

constexpr std::size_t dosHeaderOffset = 0x3c;
constexpr std::size_t lePageCount = 0x14;
constexpr std::size_t lePageSize = 0x28;
constexpr std::size_t leLastPageSize = 0x2c;
constexpr std::size_t leObjectTable = 0x40;
constexpr std::size_t leObjectCount = 0x44;
constexpr std::size_t lePageTable = 0x48;
constexpr std::size_t leFixupPageTable = 0x68;
constexpr std::size_t leFixupRecordTable = 0x6c;
constexpr std::size_t leDataPages = 0x80;
constexpr std::size_t objectRecordSize = 24;
constexpr std::size_t objectAlignment = 0x100000;
constexpr std::uint8_t sourceTypeMask = 0x0f;
constexpr std::uint8_t sourceListFlag = 0x20;
constexpr std::uint8_t source32BitOffset = 7;
constexpr std::uint8_t source32BitRelative = 8;
constexpr std::uint8_t targetTypeMask = 0x03;
constexpr std::uint8_t targetInternal = 0;
constexpr std::uint8_t targetAdditive = 0x04;
constexpr std::uint8_t target32BitOffset = 0x10;
constexpr std::uint8_t target32BitAdditive = 0x20;
constexpr std::uint8_t target16BitObject = 0x40;

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("cannot open " + path.string());
    const auto size = input.tellg();
    if (size <= 0)
        throw std::runtime_error("cannot size " + path.string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input)
        throw std::runtime_error("cannot read " + path.string());
    return bytes;
}

void requireRange(const std::vector<std::uint8_t>& bytes, std::size_t offset,
                  std::size_t size)
{
    if (offset > bytes.size() || size > bytes.size() - offset)
        throw std::runtime_error("truncated LE image");
}

std::uint8_t read8(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
{
    requireRange(bytes, offset, 1);
    return bytes[offset++];
}

std::uint16_t read16(const std::vector<std::uint8_t>& bytes,
                     std::size_t offset)
{
    requireRange(bytes, offset, 2);
    return static_cast<std::uint16_t>(bytes[offset])
         | static_cast<std::uint16_t>(bytes[offset + 1]) << 8;
}

std::uint32_t read32(const std::vector<std::uint8_t>& bytes,
                     std::size_t offset)
{
    requireRange(bytes, offset, 4);
    return static_cast<std::uint32_t>(bytes[offset])
         | static_cast<std::uint32_t>(bytes[offset + 1]) << 8
         | static_cast<std::uint32_t>(bytes[offset + 2]) << 16
         | static_cast<std::uint32_t>(bytes[offset + 3]) << 24;
}

std::uint16_t consume16(const std::vector<std::uint8_t>& bytes,
                        std::size_t& offset)
{
    const auto value = read16(bytes, offset);
    offset += 2;
    return value;
}

std::uint32_t consume32(const std::vector<std::uint8_t>& bytes,
                        std::size_t& offset)
{
    const auto value = read32(bytes, offset);
    offset += 4;
    return value;
}

std::uint32_t readPageNumber(const std::vector<std::uint8_t>& bytes,
                             std::size_t offset)
{
    requireRange(bytes, offset, 4);
    return static_cast<std::uint32_t>(bytes[offset]) << 16
         | static_cast<std::uint32_t>(bytes[offset + 1]) << 8
         | static_cast<std::uint32_t>(bytes[offset + 2]);
}

void writePartial32(std::vector<std::uint8_t>& image, std::size_t pageOffset,
                    std::uint32_t pageSize, std::int16_t sourceOffset,
                    std::uint32_t value)
{
    for (std::size_t byte = 0; byte < 4; ++byte) {
        const auto withinPage = static_cast<std::int32_t>(sourceOffset)
                              + static_cast<std::int32_t>(byte);
        if (withinPage < 0 || withinPage >= static_cast<std::int32_t>(pageSize))
            continue;
        const auto destination = pageOffset + static_cast<std::size_t>(withinPage);
        if (destination >= image.size())
            throw std::runtime_error("LE fixup exceeds object image");
        image[destination] = static_cast<std::uint8_t>(value >> (byte * 8));
    }
}

std::size_t alignUp(std::size_t value, std::size_t alignment)
{
    if (value > std::numeric_limits<std::size_t>::max() - (alignment - 1))
        throw std::runtime_error("LE object layout is too large");
    return (value + alignment - 1) & ~(alignment - 1);
}

struct ObjectLayout {
    std::uint32_t size {};
    std::uint32_t declaredBase {};
    std::uint32_t firstPage {};
    std::uint32_t pageCount {};
    std::size_t imageOffset {};
};

} // namespace

std::vector<std::uint8_t> loadLeImage(const std::filesystem::path& path,
                                      std::uint32_t loadBase)
{
    const auto file = readFile(path);
    const auto header = static_cast<std::size_t>(read32(file, dosHeaderOffset));
    requireRange(file, header, 0x98);
    if (file[header] != 'L' || file[header + 1] != 'E'
        || file[header + 2] != 0 || file[header + 3] != 0) {
        throw std::runtime_error("unsupported executable: expected little-endian LE");
    }

    const auto objectCount = read32(file, header + leObjectCount);
    if (objectCount == 0)
        throw std::runtime_error("LE image contains no objects");
    const auto pageCount = read32(file, header + lePageCount);
    const auto pageSize = read32(file, header + lePageSize);
    const auto lastPageSize = read32(file, header + leLastPageSize);
    if (pageCount == 0 || pageSize == 0 || lastPageSize > pageSize)
        throw std::runtime_error("invalid LE page layout");

    const auto objectTable = header + read32(file, header + leObjectTable);
    if (objectCount > std::numeric_limits<std::size_t>::max()
                      / objectRecordSize) {
        throw std::runtime_error("LE object table is too large");
    }
    requireRange(file, objectTable,
                 static_cast<std::size_t>(objectCount) * objectRecordSize);
    std::vector<ObjectLayout> objects(objectCount);
    std::vector<bool> assignedPage(pageCount);
    std::size_t nextObjectOffset = 0;
    for (std::uint32_t index = 0; index < objectCount; ++index) {
        const auto record = objectTable + index * objectRecordSize;
        auto& object = objects[index];
        object.size = read32(file, record);
        object.declaredBase = read32(file, record + 4);
        object.firstPage = read32(file, record + 12);
        object.pageCount = read32(file, record + 16);
        if (object.size == 0 || object.firstPage == 0
            || object.pageCount == 0
            || object.firstPage - 1 > pageCount
            || object.pageCount > pageCount - (object.firstPage - 1)) {
            throw std::runtime_error("invalid LE object layout");
        }
        object.imageOffset = index == 0 ? 0
            : alignUp(nextObjectOffset, objectAlignment);
        if (object.imageOffset
            > std::numeric_limits<std::size_t>::max() - object.size) {
            throw std::runtime_error("LE object image is too large");
        }
        nextObjectOffset = object.imageOffset + object.size;
        for (std::uint32_t objectPage = 0; objectPage < object.pageCount;
             ++objectPage) {
            const auto page = object.firstPage - 1 + objectPage;
            if (assignedPage[page])
                throw std::runtime_error("LE objects contain overlapping pages");
            assignedPage[page] = true;
        }
    }
    if (std::find(assignedPage.begin(), assignedPage.end(), false)
        != assignedPage.end()) {
        throw std::runtime_error("LE pages are not fully assigned to objects");
    }

    const auto pageTable = header + read32(file, header + lePageTable);
    if (pageCount > std::numeric_limits<std::size_t>::max() / 4)
        throw std::runtime_error("LE page table is too large");
    requireRange(file, pageTable, static_cast<std::size_t>(pageCount) * 4);
    const auto dataPages = static_cast<std::size_t>(
        read32(file, header + leDataPages));
    std::vector<std::uint8_t> image(nextObjectOffset);
    std::vector<std::size_t> pageDestinations(pageCount);
    std::vector<std::uint32_t> pageCapacities(pageCount);
    for (const auto& object : objects) {
        for (std::uint32_t objectPage = 0; objectPage < object.pageCount;
            ++objectPage) {
            const auto page = object.firstPage - 1 + objectPage;
            const auto offsetWithinObject = static_cast<std::size_t>(objectPage)
                                          * pageSize;
            const auto destination = object.imageOffset + offsetWithinObject;
            const auto objectBytes = static_cast<std::size_t>(object.size)
                                   - std::min<std::size_t>(
                                       object.size, offsetWithinObject);
            const auto capacity = static_cast<std::uint32_t>(
                std::min<std::size_t>(pageSize, objectBytes));
            pageDestinations[page] = destination;
            pageCapacities[page] = capacity;
            const auto physicalPage = readPageNumber(file, pageTable + page * 4);
            if (physicalPage == 0)
                continue;
            const auto moduleBytes = page + 1 == pageCount
                ? lastPageSize : pageSize;
            const auto bytesOnPage = std::min(capacity, moduleBytes);
            const auto source = dataPages
                + static_cast<std::size_t>(physicalPage - 1) * pageSize;
            requireRange(file, source, bytesOnPage);
            if (destination > image.size()
                || bytesOnPage > image.size() - destination) {
                throw std::runtime_error("LE page exceeds object image");
            }
            std::copy_n(file.begin() + source, bytesOnPage,
                        image.begin() + destination);
        }
    }

    const auto fixupPages = header + read32(file, header + leFixupPageTable);
    const auto fixupRecords = header + read32(file, header + leFixupRecordTable);
    requireRange(file, fixupPages,
                 (static_cast<std::size_t>(pageCount) + 1) * 4);
    for (std::uint32_t page = 0; page < pageCount; ++page) {
        auto record = fixupRecords + read32(file, fixupPages + page * 4);
        const auto recordEnd = fixupRecords
                             + read32(file, fixupPages + (page + 1) * 4);
        if (recordEnd < record)
            throw std::runtime_error("invalid LE fixup range");
        requireRange(file, record, recordEnd - record);
        while (record < recordEnd) {
            const auto sourceType = read8(file, record);
            const auto targetFlags = read8(file, record);
            const auto sourceKind = sourceType & sourceTypeMask;
            if ((sourceKind != source32BitOffset
                 && sourceKind != source32BitRelative)
                || (targetFlags & targetTypeMask) != targetInternal) {
                throw std::runtime_error("unsupported LE fixup type");
            }

            std::vector<std::int16_t> sourceOffsets;
            std::uint8_t sourceCount = 1;
            if ((sourceType & sourceListFlag) != 0)
                sourceCount = read8(file, record);
            else
                sourceOffsets.push_back(static_cast<std::int16_t>(
                    consume16(file, record)));

            const auto objectNumber = static_cast<std::uint32_t>(
                (targetFlags & target16BitObject) != 0
                    ? consume16(file, record) : read8(file, record));
            if (objectNumber == 0 || objectNumber > objects.size())
                throw std::runtime_error("invalid LE fixup target object");
            std::uint32_t targetOffset = (targetFlags & target32BitOffset) != 0
                ? consume32(file, record) : consume16(file, record);
            if ((targetFlags & targetAdditive) != 0) {
                targetOffset += (targetFlags & target32BitAdditive) != 0
                    ? consume32(file, record) : consume16(file, record);
            }
            if ((sourceType & sourceListFlag) != 0) {
                sourceOffsets.reserve(sourceCount);
                for (std::uint8_t index = 0; index < sourceCount; ++index)
                    sourceOffsets.push_back(static_cast<std::int16_t>(
                        consume16(file, record)));
            }

            const auto& targetObject = objects[objectNumber - 1];
            const auto targetAddress = static_cast<std::uint32_t>(
                loadBase + targetObject.imageOffset
                + targetObject.declaredBase + targetOffset);
            const auto pageOffset = pageDestinations[page];
            for (const auto sourceOffset : sourceOffsets) {
                auto value = targetAddress;
                if (sourceKind == source32BitRelative) {
                    const auto sourceAddress = static_cast<std::uint32_t>(
                        loadBase + pageOffset + sourceOffset);
                    value = targetAddress - (sourceAddress + 4);
                }
                writePartial32(image, pageOffset, pageCapacities[page],
                               sourceOffset, value);
            }
        }
        if (record != recordEnd)
            throw std::runtime_error("misaligned LE fixup record");
    }
    return image;
}

} // namespace hybrid
