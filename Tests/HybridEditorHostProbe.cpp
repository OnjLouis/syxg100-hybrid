#include "Vst2Abi.h"

#include <windows.h>
#include <commctrl.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

constexpr int legacyPanelId = 1010;

vst2::IntPtr hostCallback(vst2::AEffect*, std::int32_t opcode,
                          std::int32_t, vst2::IntPtr, void*, float)
{
    if (opcode == vst2::hostVersion)
        return 2400;
    if (opcode == vst2::hostGetSampleRate)
        return 48'000;
    if (opcode == vst2::hostGetBlockSize)
        return 256;
    return 0;
}

std::uint32_t packed(std::uint8_t operation, std::uint8_t channel,
                     std::uint8_t data1, std::uint8_t data2 = 0)
{
    return static_cast<std::uint32_t>(operation | channel)
        | (static_cast<std::uint32_t>(data1) << 8)
        | (static_cast<std::uint32_t>(data2) << 16);
}

void sendShort(vst2::AEffect* effect, std::uint32_t value)
{
    vst2::MidiEvent midi;
    std::memcpy(midi.midiData, &value, sizeof(midi.midiData));
    struct EventBlock {
        std::int32_t numEvents {1};
        vst2::IntPtr reserved {};
        vst2::Event* events[2] {};
    } block;
    block.events[0] = reinterpret_cast<vst2::Event*>(&midi);
    effect->dispatcher(effect, vst2::processEvents, 0, 0, &block, 0.0f);
}

HWND findDescendantByClass(HWND parent, const wchar_t* className)
{
    if (const auto direct = FindWindowExW(parent, nullptr, className, nullptr))
        return direct;
    for (auto child = GetWindow(parent, GW_CHILD); child != nullptr;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (const auto nested = findDescendantByClass(child, className))
            return nested;
    }
    return nullptr;
}

bool saveWindowBitmap(HWND window, const std::filesystem::path& path)
{
    RECT bounds {};
    if (!GetWindowRect(window, &bounds))
        return false;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const auto screen = GetDC(nullptr);
    const auto memory = CreateCompatibleDC(screen);
    const auto bitmap = CreateCompatibleBitmap(screen, width, height);
    const auto previous = SelectObject(memory, bitmap);
    const bool rendered = PrintWindow(window, memory, PW_CLIENTONLY) != FALSE;

    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::string pixels(static_cast<std::size_t>(width) * height * 4, '\0');
    const bool copied = GetDIBits(memory, bitmap, 0, height, pixels.data(),
                                  &info, DIB_RGB_COLORS) != 0;

    BITMAPFILEHEADER header {};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool written = false;
    if (file != INVALID_HANDLE_VALUE && rendered && copied) {
        DWORD count {};
        written = WriteFile(file, &header, sizeof(header), &count, nullptr)
            && count == sizeof(header)
            && WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &count,
                         nullptr)
            && count == sizeof(info.bmiHeader)
            && WriteFile(file, pixels.data(), static_cast<DWORD>(pixels.size()),
                         &count, nullptr)
            && count == pixels.size();
        CloseHandle(file);
    } else if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return written;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        std::fwprintf(stderr, L"Usage: HybridEditorHostProbe <plugin.dll> [capture.bmp]\n");
        return 2;
    }
    const auto module = LoadLibraryW(argv[1]);
    if (module == nullptr)
        return 3;
    const auto entry = reinterpret_cast<vst2::EntryPoint>(
        GetProcAddress(module, "VSTPluginMain"));
    if (entry == nullptr) {
        FreeLibrary(module);
        return 4;
    }
    auto* effect = entry(hostCallback);
    if (effect == nullptr) {
        FreeLibrary(module);
        return 5;
    }
    effect->dispatcher(effect, vst2::open, 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, vst2::setSampleRate, 0, 0, nullptr, 48'000.0f);
    effect->dispatcher(effect, vst2::setBlockSize, 0, 256, nullptr, 0.0f);

    vst2::ERect* rectangle = nullptr;
    if (effect->dispatcher(effect, vst2::editGetRect, 0, 0, &rectangle, 0.0f)
            == 0 || rectangle == nullptr) {
        effect->dispatcher(effect, vst2::close, 0, 0, nullptr, 0.0f);
        FreeLibrary(module);
        return 6;
    }
    const int width = rectangle->right - rectangle->left;
    const int height = rectangle->bottom - rectangle->top;
    const auto host = CreateWindowExW(
        0, L"STATIC", L"S-YXG100 Hybrid editor probe",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
        width + 16, height + 39, nullptr, nullptr, GetModuleHandleW(nullptr),
        nullptr);
    if (host == nullptr
        || effect->dispatcher(effect, vst2::editOpen, 0, 0, host, 0.0f) == 0) {
        if (host != nullptr)
            DestroyWindow(host);
        effect->dispatcher(effect, vst2::close, 0, 0, nullptr, 0.0f);
        FreeLibrary(module);
        return 7;
    }

    sendShort(effect, packed(0xb0, 2, 0, 33));
    sendShort(effect, packed(0xc0, 2, 11));
    sendShort(effect, packed(0xb0, 2, 2, 92));
    sendShort(effect, packed(0x90, 2, 64, 100));
    effect->dispatcher(effect, vst2::editIdle, 0, 0, nullptr, 0.0f);

    MSG message {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    const auto editor = GetWindow(host, GW_CHILD);
    const auto list = findDescendantByClass(editor, WC_LISTVIEWW);
    const auto legacyPanel = GetDlgItem(editor, legacyPanelId);
    std::array<wchar_t, 64> engine {};
    if (list != nullptr) {
        ListView_GetItemText(list, 2, 1, engine.data(),
                             static_cast<int>(engine.size()));
        ListView_SetItemState(list, 2, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        effect->dispatcher(effect, vst2::editIdle, 0, 0, nullptr, 0.0f);
        UpdateWindow(editor);
    }
    const bool statusOkay = list != nullptr
        && std::wstring(engine.data()) == L"VL/PVL";
    const bool legacyOkay = legacyPanel != nullptr
        && GetWindow(legacyPanel, GW_CHILD) != nullptr;
    bool captureOkay = true;
    if (argc >= 3)
        captureOkay = saveWindowBitmap(editor, argv[2]);

    bool reopenOkay = true;
    for (int iteration = 0; iteration < 5 && reopenOkay; ++iteration) {
        reopenOkay = effect->dispatcher(
            effect, vst2::editClose, 0, 0, nullptr, 0.0f) != 0
            && effect->dispatcher(
                effect, vst2::editOpen, 0, 0, host, 0.0f) != 0;
    }

    std::wprintf(L"size=%dx%d status=%ls legacy=%ls capture=%ls reopen=%ls\n",
                 width, height, statusOkay ? L"ok" : L"failed",
                 legacyOkay ? L"ok" : L"failed",
                 captureOkay ? L"ok" : L"failed",
                 reopenOkay ? L"ok" : L"failed");
    effect->dispatcher(effect, vst2::editClose, 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, vst2::close, 0, 0, nullptr, 0.0f);
    DestroyWindow(host);
    FreeLibrary(module);
    return statusOkay && legacyOkay && captureOkay && reopenOkay ? 0 : 8;
}
