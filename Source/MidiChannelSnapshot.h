#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hybrid {

class MidiChannelSnapshot {
public:
    static constexpr std::size_t maxParameters = 64;

    void reset() noexcept { *this = {}; }

    void observe(std::uint32_t message) noexcept
    {
        const auto operation = static_cast<std::uint8_t>(message & 0xf0);
        const auto data1 = static_cast<std::uint8_t>((message >> 8) & 0x7f);
        const auto data2 = static_cast<std::uint8_t>((message >> 16) & 0x7f);
        if (operation == 0xb0) {
            controllers[data1] = data2;
            controllerSeen[data1] = true;
            observeParameterController(data1, data2);
        } else if (operation == 0xc0) {
            program = message;
            programSeen = true;
        } else if (operation == 0xd0) {
            channelPressure = message;
            channelPressureSeen = true;
        } else if (operation == 0xe0) {
            pitchBend = message;
            pitchBendSeen = true;
        } else if (operation == 0xa0) {
            polyPressure[data1] = message;
            polyPressureSeen[data1] = true;
        }
    }

    template <typename Send>
    void replay(Send&& send) const
    {
        sendControllerIfSeen(send, 0);
        sendControllerIfSeen(send, 32);
        if (programSeen)
            send(program);
        for (std::uint8_t controller = 1; controller < 128; ++controller) {
            if (controller == 6 || controller == 32 || controller == 38
                || (controller >= 96 && controller <= 101)) {
                continue;
            }
            sendControllerIfSeen(send, controller);
        }
        for (std::size_t index = 0; index < parameterCount; ++index) {
            const auto& parameter = parameters[index];
            sendController(send, parameter.nrpn ? 99 : 101, parameter.msb);
            sendController(send, parameter.nrpn ? 98 : 100, parameter.lsb);
            if (parameter.dataMsbSeen)
                sendController(send, 6, parameter.dataMsb);
            if (parameter.dataLsbSeen)
                sendController(send, 38, parameter.dataLsb);
        }
        replayCurrentParameterSelection(send);
        if (pitchBendSeen)
            send(pitchBend);
        if (channelPressureSeen)
            send(channelPressure);
        for (std::size_t note = 0; note < polyPressure.size(); ++note) {
            if (polyPressureSeen[note])
                send(polyPressure[note]);
        }
    }

private:
    struct Parameter {
        bool nrpn {};
        std::uint8_t msb {};
        std::uint8_t lsb {};
        std::uint8_t dataMsb {};
        std::uint8_t dataLsb {};
        bool dataMsbSeen {};
        bool dataLsbSeen {};
    };

    enum class ParameterKind : std::uint8_t { none, rpn, nrpn };

    static constexpr std::uint32_t controllerMessage(
        std::uint8_t controller, std::uint8_t value) noexcept
    {
        return 0xb2u | (static_cast<std::uint32_t>(controller) << 8)
            | (static_cast<std::uint32_t>(value) << 16);
    }

    template <typename Send>
    static void sendController(Send& send, std::uint8_t controller,
                               std::uint8_t value)
    {
        send(controllerMessage(controller, value));
    }

    template <typename Send>
    void sendControllerIfSeen(Send& send, std::uint8_t controller) const
    {
        if (controllerSeen[controller])
            sendController(send, controller, controllers[controller]);
    }

    Parameter* selectedParameter() noexcept
    {
        if (selectionKind == ParameterKind::none
            || (selectionMsb == 127 && selectionLsb == 127)) {
            return nullptr;
        }
        const bool nrpn = selectionKind == ParameterKind::nrpn;
        for (std::size_t index = 0; index < parameterCount; ++index) {
            auto& parameter = parameters[index];
            if (parameter.nrpn == nrpn && parameter.msb == selectionMsb
                && parameter.lsb == selectionLsb) {
                return &parameter;
            }
        }
        if (parameterCount == parameters.size())
            return nullptr;
        auto& parameter = parameters[parameterCount++];
        parameter.nrpn = nrpn;
        parameter.msb = selectionMsb;
        parameter.lsb = selectionLsb;
        return &parameter;
    }

    void observeParameterController(std::uint8_t controller,
                                    std::uint8_t value) noexcept
    {
        switch (controller) {
        case 101:
            selectionKind = ParameterKind::rpn;
            selectionMsb = value;
            return;
        case 100:
            selectionKind = ParameterKind::rpn;
            selectionLsb = value;
            return;
        case 99:
            selectionKind = ParameterKind::nrpn;
            selectionMsb = value;
            return;
        case 98:
            selectionKind = ParameterKind::nrpn;
            selectionLsb = value;
            return;
        default:
            break;
        }
        auto* parameter = selectedParameter();
        if (parameter == nullptr)
            return;
        if (controller == 6) {
            parameter->dataMsb = value;
            parameter->dataMsbSeen = true;
        } else if (controller == 38) {
            parameter->dataLsb = value;
            parameter->dataLsbSeen = true;
        } else if (controller == 96 || controller == 97) {
            auto data = static_cast<std::uint16_t>(parameter->dataMsb) << 7;
            data |= parameter->dataLsb;
            data = controller == 96
                ? static_cast<std::uint16_t>(data == 0x3fff ? data : data + 1)
                : static_cast<std::uint16_t>(data == 0 ? 0 : data - 1);
            parameter->dataMsb = static_cast<std::uint8_t>(data >> 7);
            parameter->dataLsb = static_cast<std::uint8_t>(data & 0x7f);
            parameter->dataMsbSeen = true;
            parameter->dataLsbSeen = true;
        }
    }

    template <typename Send>
    void replayCurrentParameterSelection(Send& send) const
    {
        if (selectionKind == ParameterKind::rpn) {
            sendController(send, 101, selectionMsb);
            sendController(send, 100, selectionLsb);
        } else if (selectionKind == ParameterKind::nrpn) {
            sendController(send, 99, selectionMsb);
            sendController(send, 98, selectionLsb);
        }
    }

    std::array<std::uint8_t, 128> controllers {};
    std::array<bool, 128> controllerSeen {};
    std::array<std::uint32_t, 128> polyPressure {};
    std::array<bool, 128> polyPressureSeen {};
    std::array<Parameter, maxParameters> parameters {};
    std::size_t parameterCount {};
    std::uint32_t program {};
    std::uint32_t channelPressure {};
    std::uint32_t pitchBend {};
    ParameterKind selectionKind {ParameterKind::none};
    std::uint8_t selectionMsb {127};
    std::uint8_t selectionLsb {127};
    bool programSeen {};
    bool channelPressureSeen {};
    bool pitchBendSeen {};
};

} // namespace hybrid
