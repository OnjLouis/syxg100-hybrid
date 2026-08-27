#include "../Source/NativeVlEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::uint8_t> parseHex(std::string_view text)
{
    if ((text.size() % 2) != 0)
        throw std::runtime_error("odd hexadecimal event length");
    std::vector<std::uint8_t> bytes(text.size() / 2);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            std::stoul(std::string(text.substr(index * 2, 2)), nullptr, 16));
    }
    return bytes;
}

void replayTrace(hybrid::NativeVlEngine& engine,
                 const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot open " + path.string());
    std::string line;
    if (!std::getline(input, line) || line != "PVTE 1")
        throw std::runtime_error("unsupported event trace");
    std::size_t events = 0;
    while (std::getline(input, line)) {
        if (line.size() < 3 || line[1] != ' ')
            continue;
        const auto bytes = parseHex(std::string_view(line).substr(2));
        if (line[0] == 'M') {
            std::uint32_t packed = 0;
            if (bytes.empty() || bytes.size() > sizeof(packed))
                throw std::runtime_error("invalid short MIDI message");
            std::copy(bytes.begin(), bytes.end(),
                      reinterpret_cast<std::uint8_t*>(&packed));
            engine.sendShort(packed);
        } else if (line[0] == 'S') {
            engine.sendSysex(bytes);
        } else {
            throw std::runtime_error("unknown event trace record");
        }
        ++events;
    }
    std::cout << "replayed events=" << events << '\n';
}

void printStats(const hybrid::NativeVlEngine& engine, std::uint32_t frames)
{
    for (std::size_t index = 0; index < hybrid::NativeVlEngine::planeCount;
         ++index) {
        std::size_t nonzero = 0;
        int peak = 0;
        for (const auto sample : engine.plane(index, frames)) {
            nonzero += sample != 0;
            peak = std::max(peak, std::abs(static_cast<int>(sample)));
        }
        std::cout << " plane" << (index + 1) << '=' << nonzero << '/' << peak;
    }
    std::cout << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 3 || argc > 5) {
            std::cerr << "usage: VlNativeProbe <Sxgpvknl.vxd> "
                         "<events.pvte.txt> [note-channel [note]]\n";
            return 64;
        }
        hybrid::NativeVlEngine engine(argv[1], 44'100);
        replayTrace(engine, argv[2]);
        engine.prepare();
        const auto noteChannel = argc == 4
            ? static_cast<std::uint32_t>(std::stoul(argv[3]))
            : argc == 5 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 0u;
        if (noteChannel >= 16)
            throw std::runtime_error("note channel must be between 0 and 15");
        const auto note = argc == 5
            ? static_cast<std::uint32_t>(std::stoul(argv[4])) : 58u;
        if (note >= 128)
            throw std::runtime_error("note must be between 0 and 127");
        engine.sendShort(0x00640090u | (note << 8) | noteChannel);
        constexpr std::uint32_t frames = 2048;
        const auto start = std::chrono::steady_clock::now();
        engine.render(frames);
        const auto milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "native render milliseconds=" << milliseconds;
        printStats(engine, frames);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
