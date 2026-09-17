#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace hybrid {

struct VlPluginVoice {
    std::uint8_t bankMsb {};
    std::uint8_t bankLsb {};
    std::uint8_t program {};
    std::uint8_t volume {127};
    std::uint8_t monoPoly {1};
    std::uint8_t pitchBendRange {2};
    std::uint8_t portamentoSwitch {};
    std::uint8_t portamentoTime {};
    std::uint8_t reverbSend {};
    std::uint8_t chorusSend {};
};

class VlPluginVoiceBulk {
public:
    [[nodiscard]] std::optional<VlPluginVoice> observe(
        std::span<const std::uint8_t> bytes) noexcept
    {
        if (!isModel64Bulk(bytes))
            return std::nullopt;

        const auto address = messageAddress(bytes);
        if (address == bulkHeaderAddress) {
            voice = {};
            transactionId = {bytes[7], bytes[8]};
            collecting = true;
            valid = true;
            haveElement = false;
            haveCommon = false;
            return std::nullopt;
        }
        if (!collecting)
            return std::nullopt;

        if (address == bulkFooterAddress) {
            const bool complete = valid && haveElement
                && bytes[7] == transactionId[0]
                && bytes[8] == transactionId[1];
            collecting = false;
            return complete ? std::optional<VlPluginVoice> {voice}
                            : std::nullopt;
        }

        if (!validDataFraming(bytes)) {
            valid = false;
            return std::nullopt;
        }
        if (address == pluginElementAddress) {
            if (dataSize(bytes) != pluginElementSize
                || !validChecksum(bytes)) {
                valid = false;
                return std::nullopt;
            }
            const auto data = messageData(bytes);
            voice.bankMsb = data[0];
            voice.bankLsb = data[1];
            voice.program = data[2];
            haveElement = true;
        } else if (address == commonAddress) {
            if (dataSize(bytes) != commonSize || !validChecksum(bytes)) {
                valid = false;
                return std::nullopt;
            }
            const auto data = messageData(bytes);
            voice.volume = data[0];
            voice.monoPoly = data[3];
            voice.pitchBendRange = data[5] >= 0x40
                ? static_cast<std::uint8_t>(data[5] - 0x40) : 0;
            voice.portamentoSwitch = data[8];
            voice.portamentoTime = data[9];
            voice.reverbSend = data[11];
            voice.chorusSend = data[12];
            haveCommon = true;
        } else if (!isPluginVoiceBlock(address)) {
            valid = false;
        }
        return std::nullopt;
    }

    [[nodiscard]] static bool isModel64Bulk(
        std::span<const std::uint8_t> bytes) noexcept
    {
        return bytes.size() >= minimumBulkSize && bytes.front() == 0xf0
            && bytes[1] == 0x43 && (bytes[2] & 0xf0) == 0
            && bytes[3] == 0x64 && bytes.back() == 0xf7;
    }

private:
    static constexpr std::size_t minimumBulkSize = 11;
    static constexpr std::size_t pluginElementSize = 35;
    static constexpr std::size_t commonSize = 13;
    static constexpr std::uint32_t bulkHeaderAddress = 0x0e1f00;
    static constexpr std::uint32_t bulkFooterAddress = 0x0f1f00;
    static constexpr std::uint32_t commonAddress = 0x4c0000;
    static constexpr std::uint32_t pluginElementAddress = 0x4c1000;

    [[nodiscard]] static std::uint32_t messageAddress(
        std::span<const std::uint8_t> bytes) noexcept
    {
        return (static_cast<std::uint32_t>(bytes[6]) << 16)
            | (static_cast<std::uint32_t>(bytes[7]) << 8) | bytes[8];
    }

    [[nodiscard]] static std::size_t dataSize(
        std::span<const std::uint8_t> bytes) noexcept
    {
        return (static_cast<std::size_t>(bytes[4]) << 7) | bytes[5];
    }

    [[nodiscard]] static std::span<const std::uint8_t> messageData(
        std::span<const std::uint8_t> bytes) noexcept
    {
        return bytes.subspan(9, dataSize(bytes));
    }

    [[nodiscard]] static bool validDataFraming(
        std::span<const std::uint8_t> bytes) noexcept
    {
        const auto size = dataSize(bytes);
        return bytes.size() == size + minimumBulkSize;
    }

    [[nodiscard]] static bool validChecksum(
        std::span<const std::uint8_t> bytes) noexcept
    {
        std::uint32_t sum {};
        for (std::size_t index = 4; index + 1 < bytes.size(); ++index)
            sum += bytes[index];
        return (sum & 0x7f) == 0;
    }

    [[nodiscard]] static bool isPluginVoiceBlock(
        std::uint32_t address) noexcept
    {
        switch (address) {
        case 0x4c7000:
        case commonAddress:
        case 0x4c0100:
        case 0x4c0200:
        case 0x4c0300:
        case 0x4c0500:
        case pluginElementAddress:
        case 0x4c2000:
            return true;
        default:
            return false;
        }
    }

    VlPluginVoice voice {};
    std::array<std::uint8_t, 2> transactionId {};
    bool collecting {};
    bool valid {};
    bool haveElement {};
    bool haveCommon {};
};

} // namespace hybrid
