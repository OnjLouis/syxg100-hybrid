#include <unicorn/unicorn.h>
#include <unicorn/x86.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t imageMapSize = 0x200000;
constexpr std::uint64_t stackBase = 0x300000;
constexpr std::uint64_t stackSize = 0x100000;
constexpr std::uint64_t returnAddress = 0x400000;
constexpr std::uint64_t dataBase = 0x500000;
constexpr std::uint64_t dataSize = 0x100000;
constexpr std::uint64_t sysexHeader = dataBase + 0x20000;
constexpr std::uint64_t sysexData = dataBase + 0x21000;
constexpr std::uint64_t renderDescriptor = dataBase + 0x30000;
constexpr std::uint64_t directRenderPointers = dataBase + 0x3f000;
constexpr std::uint64_t renderBuffer = dataBase + 0x40000;
constexpr std::uint32_t renderPlaneSize = 0x2000;
constexpr std::uint64_t backendGate = 0x159ee0;
constexpr std::uint32_t sampleRate = 44'100;

struct PortWrite {
    std::uint32_t port;
    int size;
    std::uint32_t value;
    std::uint32_t eip;
};

struct RenderResult {
    std::uint32_t value;
    std::array<std::vector<std::uint8_t>, 4> planes;
};

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("cannot open " + path.string());
    const auto size = input.tellg();
    if (size < 0)
        throw std::runtime_error("cannot size " + path.string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input)
        throw std::runtime_error("cannot read " + path.string());
    return bytes;
}

std::vector<std::uint8_t> parseHex(std::string_view text)
{
    if ((text.size() % 2) != 0)
        throw std::runtime_error("odd hexadecimal event length");
    std::vector<std::uint8_t> bytes(text.size() / 2);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto pair = std::string(text.substr(index * 2, 2));
        bytes[index] = static_cast<std::uint8_t>(std::stoul(pair, nullptr, 16));
    }
    return bytes;
}

void writeWav(const std::filesystem::path& path,
              std::span<const std::uint8_t> pcm)
{
    std::ofstream output(path, std::ios::binary);
    if (!output)
        throw std::runtime_error("cannot create " + path.string());
    const auto write16 = [&output](std::uint16_t value) {
        output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    const auto write32 = [&output](std::uint32_t value) {
        output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    output.write("RIFF", 4);
    write32(static_cast<std::uint32_t>(36 + pcm.size()));
    output.write("WAVEfmt ", 8);
    write32(16);
    write16(1);
    write16(2);
    write32(sampleRate);
    write32(sampleRate * 4);
    write16(4);
    write16(16);
    output.write("data", 4);
    write32(static_cast<std::uint32_t>(pcm.size()));
    output.write(reinterpret_cast<const char*>(pcm.data()),
                 static_cast<std::streamsize>(pcm.size()));
}

class Emulator {
public:
    explicit Emulator(const std::filesystem::path& deviceState)
    {
        check(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), "uc_open");
        check(uc_mem_map(uc, 0, imageMapSize, UC_PROT_ALL), "map image");
        check(uc_mem_map(uc, stackBase, stackSize, UC_PROT_ALL), "map stack");
        check(uc_mem_map(uc, returnAddress, 0x1000, UC_PROT_ALL), "map return");
        const std::uint8_t halt = 0xf4;
        write(returnAddress, &halt, sizeof(halt));
        check(uc_mem_map(uc, dataBase, dataSize, UC_PROT_ALL), "map data");

        const auto state = readFile(deviceState);
        if (state.size() != imageMapSize)
            throw std::runtime_error("device state must be exactly 0x200000 bytes");
        write(0, state.data(), state.size());

        check(uc_hook_add(uc, &codeHookHandle, UC_HOOK_CODE,
                          reinterpret_cast<void*>(codeHook), this,
                          returnAddress, returnAddress),
              "hook code");
        check(uc_hook_add(uc, &interruptHookHandle, UC_HOOK_INTR,
                          reinterpret_cast<void*>(interruptHook), this, 1, 0),
              "hook interrupt");
        check(uc_hook_add(uc, &memoryHookHandle, UC_HOOK_MEM_INVALID,
                          reinterpret_cast<void*>(invalidMemoryHook), this, 1, 0),
              "hook invalid memory");
        check(uc_hook_add(uc, &inputHookHandle, UC_HOOK_INSN,
                          reinterpret_cast<void*>(inputHook), this, 1, 0,
                          UC_X86_INS_IN),
              "hook input");
        check(uc_hook_add(uc, &outputHookHandle, UC_HOOK_INSN,
                          reinterpret_cast<void*>(outputHook), this, 1, 0,
                          UC_X86_INS_OUT),
              "hook output");
    }

    ~Emulator()
    {
        if (uc != nullptr)
            uc_close(uc);
    }

    Emulator(const Emulator&) = delete;
    Emulator& operator=(const Emulator&) = delete;

    std::uint32_t call(std::uint32_t address,
                       std::initializer_list<std::uint32_t> arguments,
                       std::size_t instructionLimit = 2'000'000)
    {
        const auto stack = static_cast<std::uint32_t>(stackBase + stackSize - 0x100);
        std::vector<std::uint32_t> values;
        values.reserve(arguments.size() + 1);
        values.push_back(static_cast<std::uint32_t>(returnAddress));
        values.insert(values.end(), arguments.begin(), arguments.end());
        write(stack, values.data(), values.size() * sizeof(std::uint32_t));
        writeRegister(UC_X86_REG_ESP, stack);
        writeRegister(UC_X86_REG_EIP, address);
        fault.clear();
        const auto error = uc_emu_start(uc, address, returnAddress + 1, 0,
                                        instructionLimit);
        if (error != UC_ERR_OK)
            throw std::runtime_error(std::string("emulation failed: ")
                                     + uc_strerror(error) + " " + fault);
        return readRegister(UC_X86_REG_EAX);
    }

    void stabiliseRuntimeCallback()
    {
        write32(0x0e4200, 0x095ee0);
        call(0x197620, {}, 100'000'000);
        const auto callback = read32(0x0f1f60);
        const auto stableCallback = static_cast<std::uint32_t>(dataBase + 0x90000);
        std::array<std::uint8_t, 256> code {};
        read(callback, code.data(), code.size());
        write(stableCallback, code.data(), code.size());
        write32(0x0f1f60, stableCallback);
        call(0x195730, {}, 100'000'000);
    }

    std::uint32_t sendShort(std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty() || bytes.size() > 4)
            throw std::runtime_error("invalid short MIDI message");
        std::uint32_t packed = 0;
        std::memcpy(&packed, bytes.data(), bytes.size());
        return call(0x1721b0, { 5, packed });
    }

    std::uint32_t sendSysex(std::span<const std::uint8_t> bytes)
    {
        if (bytes.size() > 0xf000)
            throw std::runtime_error("SysEx exceeds worker buffer");
        write(sysexData, bytes.data(), bytes.size());
        const std::array<std::uint32_t, 2> header {
            static_cast<std::uint32_t>(sysexData),
            static_cast<std::uint32_t>(bytes.size()),
        };
        write(sysexHeader, header.data(), sizeof(header));
        return call(0x1721b0, { 6, static_cast<std::uint32_t>(sysexHeader) },
                    20'000'000);
    }

    RenderResult render(std::uint32_t frames)
    {
        std::array<std::uint8_t, renderPlaneSize * 4> empty {};
        write(renderBuffer, empty.data(), empty.size());
        const std::array<std::uint32_t, 5> descriptor {
            0, frames, 0, static_cast<std::uint32_t>(renderBuffer), 0,
        };
        write(renderDescriptor, descriptor.data(), sizeof(descriptor));
        const auto result = call(0x1721b0,
                                 { 7, static_cast<std::uint32_t>(renderDescriptor) },
                                 500'000'000);
        RenderResult output { result };
        for (std::size_t index = 0; index < output.planes.size(); ++index) {
            output.planes[index].resize(frames * 2);
            read(renderBuffer + index * renderPlaneSize,
                 output.planes[index].data(), output.planes[index].size());
        }
        return output;
    }

    RenderResult directRender(std::uint32_t command, std::uint32_t frames)
    {
        if (frames * 4 > renderPlaneSize)
            throw std::runtime_error("direct render block exceeds plane size");
        std::array<std::uint32_t, 4> addresses {};
        std::array<std::uint8_t, renderPlaneSize> empty {};
        for (std::size_t index = 0; index < addresses.size(); ++index) {
            addresses[index] = static_cast<std::uint32_t>(
                renderBuffer + index * renderPlaneSize);
            write(addresses[index], empty.data(), empty.size());
        }
        write(directRenderPointers, addresses.data(), sizeof(addresses));
        write32(backendGate, 0);
        const auto result = call(command,
                                 { static_cast<std::uint32_t>(directRenderPointers),
                                   0, frames },
                                 500'000'000);
        RenderResult output { result };
        for (std::size_t index = 0; index < output.planes.size(); ++index) {
            output.planes[index].resize(frames * 4);
            read(addresses[index], output.planes[index].data(),
                 output.planes[index].size());
        }
        return output;
    }

    void beginBackendCapture()
    {
        backendWrites.clear();
        captureBackend = true;
        write32(backendGate, 1);
    }

    std::uint32_t capturedRenderCommand() const
    {
        std::array<std::uint8_t, 4> bytes {};
        std::size_t count = 0;
        for (const auto& item : backendWrites) {
            if (item.eip == 0x1a6e13 || item.eip == 0x1a6e17
                || item.eip == 0x1a6e1b || item.eip == 0x1a6e1f) {
                if (count < bytes.size())
                    bytes[count++] = static_cast<std::uint8_t>(item.value);
            }
        }
        if (count != bytes.size())
            throw std::runtime_error("backend did not submit four command bytes");
        std::uint32_t command = 0;
        std::memcpy(&command, bytes.data(), sizeof(command));
        return command;
    }

private:
    static void codeHook(uc_engine* engine, std::uint64_t address,
                         std::uint32_t, void*)
    {
        if (address == returnAddress)
            uc_emu_stop(engine);
    }

    static void interruptHook(uc_engine* engine, std::uint32_t interrupt,
                              void*)
    {
        if (interrupt != 0x20)
            return;
        std::uint32_t eip = 0;
        uc_reg_read(engine, UC_X86_REG_EIP, &eip);
        std::uint32_t service = 0;
        uc_mem_read(engine, eip, &service, sizeof(service));
        if (service == 0x000100c2) {
            eip += 4;
            uc_reg_write(engine, UC_X86_REG_EIP, &eip);
        }
    }

    static bool invalidMemoryHook(uc_engine* engine, uc_mem_type type,
                                  std::uint64_t address, int size,
                                  std::int64_t, void* userData)
    {
        auto* self = static_cast<Emulator*>(userData);
        std::uint32_t eip = 0;
        uc_reg_read(engine, UC_X86_REG_EIP, &eip);
        std::ostringstream message;
        message << "memory type=" << static_cast<int>(type) << " address=0x"
                << std::hex << address << " size=" << std::dec << size
                << " eip=0x" << std::hex << eip;
        self->fault = message.str();
        return false;
    }

    static std::uint32_t inputHook(uc_engine*, std::uint32_t, int,
                                   void* userData)
    {
        return static_cast<Emulator*>(userData)->captureBackend ? 5u : 0u;
    }

    static void outputHook(uc_engine* engine, std::uint32_t port, int size,
                           std::uint32_t value, void* userData)
    {
        auto* self = static_cast<Emulator*>(userData);
        std::uint32_t eip = 0;
        uc_reg_read(engine, UC_X86_REG_EIP, &eip);
        self->backendWrites.push_back({ port, size, value, eip });
        if (self->captureBackend && eip == 0x1a6e1f)
            uc_emu_stop(engine);
    }

    void check(uc_err error, const char* operation) const
    {
        if (error != UC_ERR_OK)
            throw std::runtime_error(std::string(operation) + ": "
                                     + uc_strerror(error));
    }

    void write(std::uint64_t address, const void* bytes, std::size_t size)
    {
        check(uc_mem_write(uc, address, bytes, size), "memory write");
    }

    void read(std::uint64_t address, void* bytes, std::size_t size) const
    {
        check(uc_mem_read(uc, address, bytes, size), "memory read");
    }

    void write32(std::uint64_t address, std::uint32_t value)
    {
        write(address, &value, sizeof(value));
    }

    std::uint32_t read32(std::uint64_t address) const
    {
        std::uint32_t value = 0;
        read(address, &value, sizeof(value));
        return value;
    }

    void writeRegister(int id, std::uint32_t value)
    {
        check(uc_reg_write(uc, id, &value), "register write");
    }

    std::uint32_t readRegister(int id) const
    {
        std::uint32_t value = 0;
        check(uc_reg_read(uc, id, &value), "register read");
        return value;
    }

    uc_engine* uc {};
    uc_hook codeHookHandle {};
    uc_hook interruptHookHandle {};
    uc_hook memoryHookHandle {};
    uc_hook inputHookHandle {};
    uc_hook outputHookHandle {};
    bool captureBackend {};
    std::string fault;
    std::vector<PortWrite> backendWrites;
};

void replayTrace(Emulator& emulator, const std::filesystem::path& path)
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
        if (line[0] == 'M')
            emulator.sendShort(bytes);
        else if (line[0] == 'S')
            emulator.sendSysex(bytes);
        else
            throw std::runtime_error("unknown event trace record");
        ++events;
    }
    std::cout << "replayed events=" << events << '\n';
}

void printStats(const RenderResult& result)
{
    std::cout << "render result=0x" << std::hex << result.value << std::dec;
    for (std::size_t plane = 0; plane < result.planes.size(); ++plane) {
        std::size_t nonzero = 0;
        int peak = 0;
        const auto& bytes = result.planes[plane];
        for (std::size_t offset = 0; offset + 1 < bytes.size(); offset += 2) {
            std::int16_t sample = 0;
            std::memcpy(&sample, bytes.data() + offset, sizeof(sample));
            if (sample != 0)
                ++nonzero;
            peak = std::max(peak, std::abs(static_cast<int>(sample)));
        }
        std::cout << " plane" << (plane + 1) << "=" << nonzero << '/' << peak;
    }
    std::cout << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::cerr << "usage: VlWorker <pv-device-state.bin> <events.pvte.txt> "
                     "<output-directory>\n";
        return 2;
    }
    try {
        const auto startTime = std::chrono::steady_clock::now();
        const auto elapsedMilliseconds = [&startTime] {
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - startTime).count();
        };
        const std::filesystem::path outputDirectory = argv[3];
        std::filesystem::create_directories(outputDirectory);
        Emulator emulator(argv[1]);
        emulator.stabiliseRuntimeCallback();
        std::cout << "runtime callback stabilised\n";
        emulator.call(0x1721b0, { 13, 8 });
        emulator.call(0x16d9c1, {}, 100'000'000);
        emulator.call(0x1721b0, { 4, sampleRate, sampleRate });
        replayTrace(emulator, argv[2]);
        std::cout << "initialisation milliseconds=" << elapsedMilliseconds() << '\n';

        emulator.render(512);
        emulator.beginBackendCapture();
        emulator.render(512);
        const auto command = emulator.capturedRenderCommand();
        std::cout << "generated render command=0x" << std::hex << command
                  << std::dec << '\n';
        const auto firstRenderStart = std::chrono::steady_clock::now();
        printStats(emulator.directRender(command, 512));
        std::cout << "first 512-frame render milliseconds="
                  << std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - firstRenderStart).count()
                  << '\n';

        const std::array<std::uint8_t, 3> retrigger { 0x90, 58, 100 };
        emulator.sendShort(retrigger);
        const auto audibleRenderStart = std::chrono::steady_clock::now();
        const auto audible = emulator.directRender(command, 2048);
        std::cout << "audible 2048-frame render milliseconds="
                  << std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - audibleRenderStart).count()
                  << '\n';
        printStats(audible);
        for (std::size_t index = 0; index < audible.planes.size(); ++index)
            writeWav(outputDirectory / ("vl-plane" + std::to_string(index + 1)
                                        + ".wav"),
                     audible.planes[index]);
        emulator.call(0x1721b0, { 3 });
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "VL worker failed: " << error.what() << '\n';
        return 1;
    }
}
