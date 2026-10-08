#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace hybrid {

// Optional, versioned conversion metadata. Legacy engines remain unchanged.
class ConvertedMuVoiceMap {
public:
    ConvertedMuVoiceMap() { aliases.fill(0xff); }
    explicit ConvertedMuVoiceMap(std::span<const std::uint8_t> data) : ConvertedMuVoiceMap() {
        if (data.size() < headerSize || std::memcmp(data.data(), "HYMUVM01", 8) != 0)
            return;
        const auto count = read32(data.data() + 136);
        if (count > maximumEntries || data.size() != headerSize + count * 5)
            return;
        std::array<std::uint8_t, 128> pendingAliases;
        std::copy_n(data.data() + 8, pendingAliases.size(), pendingAliases.begin());
        for (const auto bank : pendingAliases)
            if (bank != 0xff && (bank < 49 || bank > 63))
                return;
        std::vector<Entry> pending;
        pending.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto* bytes = data.data() + headerSize + i * 5;
            const auto key = read32(bytes);
            const auto flags = bytes[4];
            if (key >= (1u << 21) || flags == 0 || flags > 3
                || (!pending.empty() && key <= pending.back().key))
                return;
            pending.push_back({key, flags});
        }
        aliases = pendingAliases;
        entries = std::move(pending);
        available = true;
    }
    static ConvertedMuVoiceMap load(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) return {};
        input.seekg(0, std::ios::end);
        const auto length = input.tellg();
        if (length < 4) return {};
        input.seekg(length - std::streamoff(4));
        std::array<std::uint8_t, 4> trailer {};
        input.read(reinterpret_cast<char*>(trailer.data()), 4);
        const auto size = read32(trailer.data());
        if (size < headerSize || size > headerSize + maximumEntries * 5
            || static_cast<std::streamoff>(size) > length - std::streamoff(4))
            return {};
        input.seekg(length - std::streamoff(size + 4));
        std::vector<std::uint8_t> bytes(size);
        input.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!input) return {};
        return ConvertedMuVoiceMap(bytes);
    }
    bool valid() const noexcept { return available; }
    bool hasVoice(std::uint8_t msb, std::uint8_t lsb, std::uint8_t program, bool basic) const noexcept {
        const auto key = (std::uint32_t(msb) << 14) | (std::uint32_t(lsb) << 7) | program;
        const auto found = std::lower_bound(entries.begin(), entries.end(), key,
            [](const Entry& entry, std::uint32_t value) { return entry.key < value; });
        return found != entries.end() && found->key == key
            && (found->flags & (basic ? 2 : 1)) != 0;
    }
    std::uint8_t engineBank(std::uint8_t msb, std::uint8_t lsb) const noexcept {
        if (!available) return msb;
        if (msb == 48) {
            if (aliases[lsb] != 0xff) return aliases[lsb];
            if (lsb != 0) return 1;
        }
        // Conversion-private banks must not become new public MIDI addresses.
        if (std::find(aliases.begin(), aliases.end(), msb) != aliases.end())
            return 1; // The converted MU table's original silent bank.
        return msb;
    }
private:
    struct Entry { std::uint32_t key; std::uint8_t flags; };
    static constexpr std::size_t headerSize = 140;
    static constexpr std::size_t maximumEntries = 32768;
    static std::uint32_t read32(const std::uint8_t* data) noexcept {
        return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8)
            | (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
    }
    bool available {};
    std::array<std::uint8_t, 128> aliases;
    std::vector<Entry> entries;
};

class ConvertedMuBankRouter {
public:
    void reset() noexcept { msb.fill(0); lsb.fill(0); }
    void observeShort(std::uint32_t message) noexcept {
        const auto channel = message & 15;
        if ((message & 0xf0) != 0xb0) return;
        const auto controller = (message >> 8) & 127;
        if (controller == 0) msb[channel] = (message >> 16) & 127;
        if (controller == 32) lsb[channel] = (message >> 16) & 127;
    }
    void observeSysex(std::span<const std::uint8_t> bytes) noexcept {
        if (bytes.size() < 9 || bytes[0] != 0xf0 || bytes.back() != 0xf7
            || bytes[1] != 0x43 || (bytes[2] & 0xf0) != 0x10
            || bytes[3] != 0x4c || bytes[4] != 8 || bytes[5] >= 16)
            return;
        for (std::size_t i = 7; i + 1 < bytes.size(); ++i) {
            const auto address = bytes[6] + i - 7;
            if (address == 1) msb[bytes[5]] = bytes[i] & 127;
            if (address == 2) lsb[bytes[5]] = bytes[i] & 127;
        }
    }
    std::uint8_t engineBank(const ConvertedMuVoiceMap& map, std::uint8_t channel) const noexcept {
        return map.engineBank(msb[channel], lsb[channel]);
    }
    bool needsTranslation(const ConvertedMuVoiceMap& map, std::uint8_t channel) const noexcept {
        return engineBank(map, channel) != msb[channel];
    }
private:
    std::array<std::uint8_t, 16> msb {}, lsb {};
};
} // namespace hybrid
