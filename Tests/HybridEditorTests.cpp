#include "HybridEditor.h"

#include <commctrl.h>
#include <oleacc.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <string>

namespace {

HWND legacyControl {};
int editorOpenCount {};
int editorCloseCount {};
int editorIdleCount {};

vst2::IntPtr mockDispatcher(vst2::AEffect*, std::int32_t opcode,
                            std::int32_t, vst2::IntPtr, void* data, float)
{
    static vst2::ERect rectangle {0, 0, 200, 320};
    if (opcode == vst2::editGetRect) {
        *static_cast<vst2::ERect**>(data) = &rectangle;
        return 1;
    }
    if (opcode == vst2::editOpen) {
        ++editorOpenCount;
        legacyControl = CreateWindowExW(
            0, L"BUTTON", L"Legacy Yamaha control", WS_CHILD | WS_VISIBLE
                | WS_TABSTOP | BS_PUSHBUTTON,
            8, 8, 180, 28, static_cast<HWND>(data), nullptr,
            GetModuleHandleW(nullptr), nullptr);
        return legacyControl != nullptr;
    }
    if (opcode == vst2::editClose) {
        ++editorCloseCount;
        if (legacyControl != nullptr) {
            DestroyWindow(legacyControl);
            legacyControl = nullptr;
        }
        return 1;
    }
    if (opcode == vst2::editIdle) {
        ++editorIdleCount;
        return 1;
    }
    return 0;
}

std::wstring text(HWND window)
{
    const auto length = GetWindowTextLengthW(window);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    if (length != 0)
        GetWindowTextW(window, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

HWND findChildByText(HWND parent, const wchar_t* expected)
{
    for (auto child = GetWindow(parent, GW_CHILD); child != nullptr;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (text(child) == expected)
            return child;
    }
    return nullptr;
}

std::wstring accessibleName(HWND window)
{
    IAccessible* accessible {};
    const auto result = AccessibleObjectFromWindow(
        window, OBJID_CLIENT, IID_IAccessible,
        reinterpret_cast<void**>(&accessible));
    assert(SUCCEEDED(result));
    assert(accessible != nullptr);
    VARIANT self;
    VariantInit(&self);
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    BSTR name {};
    assert(SUCCEEDED(accessible->get_accName(self, &name)));
    const std::wstring value = name == nullptr ? L"" : name;
    SysFreeString(name);
    accessible->Release();
    return value;
}

std::uint32_t message(std::uint8_t operation, std::uint8_t channel,
                      std::uint8_t data1, std::uint8_t data2 = 0)
{
    return static_cast<std::uint32_t>(operation | channel)
        | (static_cast<std::uint32_t>(data1) << 8)
        | (static_cast<std::uint32_t>(data2) << 16);
}

} // namespace

int main()
{
    vst2::AEffect child {};
    child.dispatcher = mockDispatcher;
    hybrid::HybridStatus status;
    status.setAvailability(true, true);
    status.setEffectsBridgeAvailable(true);
    status.setSampleRate(48'000);

    constexpr hybrid::HybridEditorConfig editorConfig {
        L"SYXG100HybridEditorTest",
        L"S-YXG100 Hybrid test",
        L"XG50",
        L"",
        L"Test routing description."
    };
    hybrid::HybridEditor editor(
        GetModuleHandleW(nullptr), &child, status, editorConfig);
    vst2::ERect* rectangle = nullptr;
    const auto getRectResult = editor.getRect(&rectangle);
    assert(getRectResult == 1);
    assert(rectangle != nullptr);
    assert(rectangle->right >= 860);
    assert(rectangle->bottom >= 620);

    const auto parent = CreateWindowExW(
        0, L"STATIC", L"Hybrid editor test host", WS_OVERLAPPEDWINDOW,
        -32000, -32000, rectangle->right, rectangle->bottom,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    assert(parent != nullptr);
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    const auto openResult = editor.open(parent);
    assert(openResult == 1);
    assert(editorOpenCount == 1);

    const auto root = GetWindow(parent, GW_CHILD);
    assert(root != nullptr);
    const auto statusButton = findChildByText(root, L"&Status");
    const auto routingButton = findChildByText(root, L"&Under the hood");
    const auto legacyButton = findChildByText(root, L"&Yamaha editor");
    const auto refreshButton = findChildByText(root, L"&Refresh (F5)");
    const auto copyButton = findChildByText(root, L"&Copy report");
    assert(statusButton != nullptr);
    assert(routingButton != nullptr);
    assert(legacyButton != nullptr);
    assert(refreshButton != nullptr);
    assert(copyButton != nullptr);
    assert(accessibleName(statusButton) == L"Status");
    assert((GetWindowLongPtrW(root, GWL_EXSTYLE) & WS_EX_CONTROLPARENT) != 0);
    const auto nextAfterStatus = GetNextDlgTabItem(
        root, statusButton, FALSE);
    assert(nextAfterStatus == refreshButton);

    const auto list = FindWindowExW(root, nullptr, WC_LISTVIEWW, nullptr);
    assert(list != nullptr);
    assert((GetWindowLongPtrW(list, GWL_STYLE) & WS_TABSTOP) != 0);
    assert(ListView_GetItemCount(list) == 16);
    assert(Header_GetItemCount(ListView_GetHeader(list)) == 11);
    assert(accessibleName(list) == L"MIDI channels");
    const auto summary = FindWindowExW(root, nullptr, L"EDIT", nullptr);
    const auto activity = FindWindowExW(root, summary, L"EDIT", nullptr);
    const auto routing = FindWindowExW(root, activity, L"EDIT", nullptr);
    assert(accessibleName(summary) == L"Runtime summary");
    assert(accessibleName(activity) == L"Selected channel details");
    assert(accessibleName(routing) == L"Routing and worker details");

    status.observeShortMessage(message(0xb0, 2, 0, 33), true, false);
    status.observeShortMessage(message(0xc0, 2, 11), true, false);
    status.observeShortMessage(message(0xb0, 2, 2, 92), true, false);
    status.observeShortMessage(message(0x90, 2, 64, 100), true, false);
    editor.idle();
    std::array<wchar_t, 64> listText {};
    ListView_GetItemText(list, 2, 1, listText.data(),
                         static_cast<int>(listText.size()));
    assert(std::wstring(listText.data()) == L"VL/PVL");
    ListView_GetItemText(list, 2, 4, listText.data(),
                         static_cast<int>(listText.size()));
    assert(std::wstring(listText.data()) == L"12");
    assert(editorIdleCount != 0);

    ListView_SetItemState(list, 2, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    SetFocus(list);
    for (std::uint8_t value = 0; value < 100; ++value) {
        status.observeShortMessage(message(0xb0, 2, 2, value), true, false);
        editor.idle();
    }
    assert(GetFocus() == list);
    assert(ListView_GetNextItem(list, -1, LVNI_SELECTED) == 2);
    assert(ListView_GetItemCount(list) == 16);

    SetFocus(statusButton);
    SendMessageW(statusButton, WM_KEYDOWN, VK_TAB, 0);
    assert(GetFocus() == refreshButton);
    SetFocus(statusButton);
    SendMessageW(statusButton, WM_KEYDOWN, VK_RIGHT, 0);
    assert(GetFocus() == routingButton);
    assert(!IsWindowVisible(list));
    SendMessageW(routingButton, WM_SYSCHAR, L's', 0);
    assert(IsWindowVisible(list));
    SendMessageW(statusButton, WM_SYSCHAR, L'u', 0);
    assert(!IsWindowVisible(list));
    SendMessageW(routingButton, WM_SYSCHAR, L'y', 0);
    assert(IsWindowVisible(legacyControl));

    const auto closeResult = editor.close();
    assert(closeResult == 1);
    assert(editorCloseCount == 1);
    const auto gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const auto userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (int iteration = 0; iteration < 25; ++iteration) {
        assert(editor.open(parent) == 1);
        assert(editor.idle() == 1);
        assert(editor.close() == 1);
    }
    assert(editorOpenCount == 26);
    assert(editorCloseCount == 26);
    assert(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)
           <= gdiBefore + 2);
    assert(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)
           <= userBefore + 2);
    DestroyWindow(parent);
    return 0;
}
