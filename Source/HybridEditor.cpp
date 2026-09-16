#include "HybridEditor.h"

#include <commctrl.h>
#include <oleacc.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <sstream>
#include <string_view>

namespace hybrid {
namespace {

constexpr UINT_PTR refreshTimer = 1;
constexpr UINT refreshIntervalMs = 200;
constexpr int minimumEditorWidth = 860;
constexpr int minimumEditorHeight = 620;
constexpr int toolbarHeight = 40;
constexpr int edge = 8;

enum ControlId : int {
    statusPageId = 1001,
    routingPageId,
    legacyPageId,
    refreshId,
    copyId,
    summaryId,
    channelListId,
    activityId,
    routingId,
    legacyPanelId,
};

const wchar_t* engineName(DisplayEngine engine,
                          const wchar_t* standardEngineName) noexcept
{
    switch (engine) {
    case DisplayEngine::vl: return L"VL/PVL";
    case DisplayEngine::sg: return L"SG";
    default: return standardEngineName;
    }
}

const wchar_t* workerName(WorkerDisplayState state) noexcept
{
    switch (state) {
    case WorkerDisplayState::loaded: return L"loaded";
    case WorkerDisplayState::active: return L"active";
    case WorkerDisplayState::failed: return L"failed";
    default: return L"idle";
    }
}

const wchar_t* resetName(ResetDisplayState state) noexcept
{
    switch (state) {
    case ResetDisplayState::gm1: return L"GM1";
    case ResetDisplayState::gm2: return L"GM2";
    case ResetDisplayState::gs: return L"GS";
    case ResetDisplayState::xg: return L"XG";
    default: return L"none observed";
    }
}

std::wstring yesNo(bool value)
{
    return value ? L"yes" : L"no";
}

std::wstring runtimeState(bool available, bool active, bool failed)
{
    if (!available)
        return L"not installed";
    if (failed)
        return L"failed";
    if (active)
        return L"active";
    return L"ready";
}

void setWindowTextIfChanged(HWND window, const std::wstring& value,
                            std::wstring& previous, bool force)
{
    if (!force && previous == value)
        return;
    previous = value;
    SetWindowTextW(window, value.c_str());
}

void setListTextIfChanged(HWND list, int row, int column,
                          const std::wstring& value, bool force)
{
    if (!force) {
        std::array<wchar_t, 128> current {};
        ListView_GetItemText(list, row, column, current.data(),
                             static_cast<int>(current.size()));
        if (value == current.data())
            return;
    }
    ListView_SetItemText(list, row, column,
                         const_cast<wchar_t*>(value.c_str()));
}

std::wstring unsignedValue(unsigned value)
{
    return std::to_wstring(value);
}

} // namespace

HybridEditor::HybridEditor(HINSTANCE instanceValue, vst2::AEffect* childValue,
                           HybridStatus& statusValue,
                           HybridEditorConfig configValue) noexcept
    : instance(instanceValue), child(childValue), status(statusValue),
      config(configValue)
{
    int childWidth = 0;
    int childHeight = 0;
    if (child != nullptr && child->dispatcher != nullptr) {
        vst2::ERect* childRect = nullptr;
        if (child->dispatcher(child, vst2::editGetRect, 0, 0, &childRect, 0.0f)
            != 0 && childRect != nullptr) {
            childWidth = childRect->right - childRect->left;
            childHeight = childRect->bottom - childRect->top;
        }
    }
    editorRect.right = static_cast<std::int16_t>(
        std::max(minimumEditorWidth, childWidth + edge * 2));
    editorRect.bottom = static_cast<std::int16_t>(
        std::max(minimumEditorHeight, childHeight + toolbarHeight + edge));
}

HybridEditor::~HybridEditor()
{
    close();
}

vst2::IntPtr HybridEditor::getRect(void* data) noexcept
{
    if (data == nullptr)
        return 0;
    *static_cast<vst2::ERect**>(data) = &editorRect;
    return 1;
}

vst2::IntPtr HybridEditor::open(void* parent) noexcept
{
    if (window != nullptr)
        return 1;
    if (parent == nullptr)
        return 0;

    const auto comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    comInitialized = SUCCEEDED(comResult);
    (void)CoCreateInstance(
        CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER,
        IID_IAccPropServices,
        reinterpret_cast<void**>(&accessibilityServices));

    INITCOMMONCONTROLSEX commonControls {
        sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES
    };
    InitCommonControlsEx(&commonControls);

    WNDCLASSEXW windowClass {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = config.windowClassName;
    RegisterClassExW(&windowClass);

    window = CreateWindowExW(
        WS_EX_CONTROLPARENT, config.windowClassName, config.productName,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, editorRect.right, editorRect.bottom,
        static_cast<HWND>(parent), nullptr, instance, this);
    if (window == nullptr)
        return 0;

    createControls();
    layoutControls(editorRect.right, editorRect.bottom);
    if (child != nullptr && child->dispatcher != nullptr) {
        child->dispatcher(child, vst2::editOpen, 0, 0,
                          legacyPanel, 0.0f);
        childEditorOpen = true;
    }
    showPage(Page::status);
    SetTimer(window, refreshTimer, refreshIntervalMs, nullptr);
    refresh(true);
    return 1;
}

vst2::IntPtr HybridEditor::close() noexcept
{
    if (childEditorOpen && child != nullptr && child->dispatcher != nullptr) {
        child->dispatcher(child, vst2::editClose, 0, 0, nullptr, 0.0f);
        childEditorOpen = false;
    }
    if (window != nullptr) {
        KillTimer(window, refreshTimer);
        const auto oldWindow = window;
        window = nullptr;
        DestroyWindow(oldWindow);
    }
    if (accessibilityServices != nullptr) {
        accessibilityServices->Release();
        accessibilityServices = nullptr;
    }
    if (comInitialized) {
        CoUninitialize();
        comInitialized = false;
    }
    return 1;
}

vst2::IntPtr HybridEditor::idle() noexcept
{
    if (window != nullptr)
        refresh(false);
    if (childEditorOpen && child != nullptr && child->dispatcher != nullptr)
        child->dispatcher(child, vst2::editIdle, 0, 0, nullptr, 0.0f);
    return 1;
}

LRESULT CALLBACK HybridEditor::windowProcedure(HWND target, UINT message,
                                                WPARAM wParam, LPARAM lParam)
{
    HybridEditor* editor = reinterpret_cast<HybridEditor*>(
        GetWindowLongPtrW(target, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        editor = static_cast<HybridEditor*>(create->lpCreateParams);
        SetWindowLongPtrW(target, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(editor));
    }
    if (editor != nullptr)
        return editor->handleMessage(target, message, wParam, lParam);
    return DefWindowProcW(target, message, wParam, lParam);
}

LRESULT CALLBACK HybridEditor::controlProcedure(
    HWND target, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR referenceData)
{
    auto* editor = reinterpret_cast<HybridEditor*>(referenceData);
    if (editor != nullptr) {
        if (message == WM_KEYDOWN
            && (wParam == VK_LEFT || wParam == VK_UP
                || wParam == VK_RIGHT || wParam == VK_DOWN)
            && editor->movePage(
                target, wParam == VK_LEFT || wParam == VK_UP)) {
            return 0;
        }
        if (message == WM_KEYDOWN && wParam == VK_TAB) {
            editor->moveFocus(target, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        }
        if (message == WM_KEYDOWN && wParam == VK_F5) {
            editor->refresh(true);
            return 0;
        }
        if (message == WM_SYSCHAR
            && editor->activateMnemonic(
                static_cast<wchar_t>(std::towlower(wParam)))) {
            return 0;
        }
    }
    return DefSubclassProc(target, message, wParam, lParam);
}

LRESULT HybridEditor::handleMessage(HWND target, UINT message, WPARAM wParam,
                                    LPARAM lParam)
{
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case statusPageId: showPage(Page::status); return 0;
        case routingPageId: showPage(Page::routing); return 0;
        case legacyPageId: showPage(Page::legacy); return 0;
        case refreshId: refresh(true); return 0;
        case copyId: copyReport(); return 0;
        default: break;
        }
        break;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header != nullptr && header->hwndFrom == channelList
            && header->code == LVN_ITEMCHANGED) {
            refreshActivity(latestSnapshot, true);
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    }
    case WM_SIZE:
        layoutControls(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_TIMER:
        if (wParam == refreshTimer)
            refresh(false);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint {};
        const auto deviceContext = BeginPaint(target, &paint);
        paintVisualization(deviceContext);
        EndPaint(target, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SYSCHAR:
        if (activateMnemonic(static_cast<wchar_t>(std::towlower(wParam))))
            return 0;
        break;
    case WM_KEYDOWN:
        if (wParam == VK_F5) {
            refresh(true);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        if (window == target)
            window = nullptr;
        SetWindowLongPtrW(target, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(target, message, wParam, lParam);
}

void HybridEditor::createControls()
{
    controlFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const auto buttonStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP
        | BS_AUTORADIOBUTTON | BS_PUSHLIKE;
    statusButton = CreateWindowExW(0, L"BUTTON", L"&Status", buttonStyle
        | WS_GROUP, 0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(statusPageId), instance, nullptr);
    routingButton = CreateWindowExW(0, L"BUTTON", L"&Under the hood",
        buttonStyle, 0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(routingPageId), instance, nullptr);
    legacyButton = CreateWindowExW(0, L"BUTTON", L"&Yamaha editor",
        buttonStyle, 0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(legacyPageId), instance, nullptr);
    refreshButton = CreateWindowExW(0, L"BUTTON", L"&Refresh (F5)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window, reinterpret_cast<HMENU>(refreshId), instance,
        nullptr);
    copyButton = CreateWindowExW(0, L"BUTTON", L"&Copy report",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window, reinterpret_cast<HMENU>(copyId), instance,
        nullptr);

    const auto readOnlyEdit = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL
        | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY;
    summaryEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        readOnlyEdit, 0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(summaryId), instance, nullptr);
    channelList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"Channels",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL
            | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, window, reinterpret_cast<HMENU>(channelListId), instance,
        nullptr);
    activityEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        readOnlyEdit, 0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(activityId), instance, nullptr);
    routingEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        readOnlyEdit, 0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(routingId), instance, nullptr);
    legacyPanel = CreateWindowExW(0, L"STATIC", L"Yamaha legacy editor",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 0, 0, window, reinterpret_cast<HMENU>(legacyPanelId), instance,
        nullptr);

    const std::array<HWND, 10> controls {
        statusButton, routingButton, legacyButton, refreshButton, copyButton,
        summaryEdit, channelList, activityEdit, routingEdit, legacyPanel
    };
    for (const auto control : controls) {
        if (control == nullptr)
            continue;
        SendMessageW(control, WM_SETFONT,
                     reinterpret_cast<WPARAM>(controlFont), TRUE);
        SetWindowSubclass(control, controlProcedure, 1,
                          reinterpret_cast<DWORD_PTR>(this));
    }

    if (accessibilityServices != nullptr) {
        const auto setName = [&](HWND control, const wchar_t* name) {
            (void)accessibilityServices->SetHwndPropStr(
                control, OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME, name);
        };
        setName(summaryEdit, L"Runtime summary");
        setName(channelList, L"MIDI channels");
        setName(activityEdit, L"Selected channel details");
        setName(routingEdit, L"Routing and worker details");
        setName(legacyPanel, L"Yamaha legacy editor panel");
    }

    ListView_SetExtendedListViewStyle(
        channelList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER
            | LVS_EX_LABELTIP);
    struct Column { const wchar_t* label; int width; };
    const std::array<Column, 11> columns {{
        {L"Channel", 62}, {L"Engine", 105}, {L"Bank MSB", 72},
        {L"Bank LSB", 72}, {L"Program", 68}, {L"Notes", 54},
        {L"Last note", 70}, {L"Breath", 62}, {L"Expression", 76},
        {L"Pitch bend", 78}, {L"Sends R/C/V", 96},
    }};
    for (std::size_t index = 0; index < columns.size(); ++index) {
        LVCOLUMNW column {};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(columns[index].label);
        column.cx = columns[index].width;
        column.iSubItem = static_cast<int>(index);
        ListView_InsertColumn(channelList, static_cast<int>(index), &column);
    }
    for (int channel = 0; channel < 16; ++channel) {
        const auto label = std::to_wstring(channel + 1);
        LVITEMW item {};
        item.mask = LVIF_TEXT;
        item.iItem = channel;
        item.pszText = const_cast<wchar_t*>(label.c_str());
        ListView_InsertItem(channelList, &item);
    }
    ListView_SetItemState(channelList, 0, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
}

void HybridEditor::layoutControls(int width, int height) noexcept
{
    if (statusButton == nullptr)
        return;
    int x = edge;
    const int buttonY = 6;
    const int buttonHeight = 28;
    auto placeButton = [&](HWND button, int buttonWidth) {
        MoveWindow(button, x, buttonY, buttonWidth, buttonHeight, TRUE);
        x += buttonWidth + 6;
    };
    placeButton(statusButton, 80);
    placeButton(routingButton, 130);
    placeButton(legacyButton, 120);
    placeButton(refreshButton, 104);
    placeButton(copyButton, 104);

    const int contentTop = toolbarHeight + 2;
    const int contentWidth = std::max(0, width - edge * 2);
    const int summaryHeight = 72;
    const int activityHeight = 104;
    const int visualWidth = 300;
    MoveWindow(summaryEdit, edge, contentTop, contentWidth, summaryHeight, TRUE);
    const int listTop = contentTop + summaryHeight + 6;
    const int listHeight = std::max(80, height - listTop - activityHeight - 14);
    MoveWindow(channelList, edge, listTop, contentWidth, listHeight, TRUE);
    const int activityTop = listTop + listHeight + 6;
    MoveWindow(activityEdit, edge, activityTop,
               std::max(100, contentWidth - visualWidth - 8), activityHeight,
               TRUE);
    MoveWindow(routingEdit, edge, contentTop, contentWidth,
               std::max(0, height - contentTop - edge), TRUE);
    MoveWindow(legacyPanel, edge, contentTop, contentWidth,
               std::max(0, height - contentTop - edge), TRUE);
    InvalidateRect(window, nullptr, TRUE);
}

void HybridEditor::showPage(Page newPage) noexcept
{
    page = newPage;
    const bool showStatus = page == Page::status;
    ShowWindow(summaryEdit, showStatus ? SW_SHOW : SW_HIDE);
    ShowWindow(channelList, showStatus ? SW_SHOW : SW_HIDE);
    ShowWindow(activityEdit, showStatus ? SW_SHOW : SW_HIDE);
    ShowWindow(routingEdit, page == Page::routing ? SW_SHOW : SW_HIDE);
    ShowWindow(legacyPanel, page == Page::legacy ? SW_SHOW : SW_HIDE);
    SendMessageW(statusButton, BM_SETCHECK,
                 page == Page::status ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(routingButton, BM_SETCHECK,
                 page == Page::routing ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(legacyButton, BM_SETCHECK,
                 page == Page::legacy ? BST_CHECKED : BST_UNCHECKED, 0);
    if (page == Page::status)
        SetFocus(channelList);
    else if (page == Page::routing)
        SetFocus(routingEdit);
    else
        SetFocus(legacyPanel);
    InvalidateRect(window, nullptr, TRUE);
}

void HybridEditor::refresh(bool force)
{
    latestSnapshot = status.displaySnapshot();
    refreshSummary(latestSnapshot, force);
    refreshChannels(latestSnapshot, force);
    refreshActivity(latestSnapshot, force);
    refreshRouting(latestSnapshot, force);
    if (page == Page::status)
        InvalidateRect(window, nullptr, FALSE);
}

void HybridEditor::refreshSummary(const HybridStatusSnapshot& snapshot,
                                  bool force)
{
    unsigned activeWorkers = 0;
    unsigned failedWorkers = 0;
    for (const auto worker : snapshot.vlWorkers) {
        activeWorkers += worker == WorkerDisplayState::active;
        failedWorkers += worker == WorkerDisplayState::failed;
    }
    std::wostringstream text;
    if (snapshot.latestPlaybackInstance)
        text << L"View: latest playback instance.\r\n";
    text << L"VL/PVL: "
         << runtimeState(snapshot.vlAvailable, activeWorkers != 0,
                         failedWorkers != 0)
         << L"; active workers " << activeWorkers << L" of 8; assignments "
         << (snapshot.explicitVlAssignments ? L"explicit PVL" : L"legacy mono")
         << L".\r\nSG: "
         << runtimeState(snapshot.sgAvailable, snapshot.sgActive,
                         snapshot.sgFailed)
         << L"; route mask 0x" << std::hex << snapshot.sgRouteMask << std::dec
         << L". Effects bridge: "
         << (snapshot.effectsBridgeAvailable ? L"active" : L"dry fallback")
         << L". Primary engine: " << config.standardEngineName;
    if (config.supplementalEngineName != nullptr
        && config.supplementalEngineName[0] != L'\0') {
        text << L"; " << config.supplementalEngineName << L" runtime "
             << (snapshot.supplementalEngineAvailable
                     ? L"active" : L"unavailable");
    }
    text
         << L". Sample rate: " << snapshot.sampleRate
         << L" Hz. Last reset: " << resetName(snapshot.lastReset) << L".";
    setWindowTextIfChanged(summaryEdit, text.str(), previousSummary, force);
}

void HybridEditor::refreshChannels(const HybridStatusSnapshot& snapshot,
                                   bool force)
{
    for (int row = 0; row < 16; ++row) {
        const auto& channel = snapshot.channels[static_cast<std::size_t>(row)];
        setListTextIfChanged(channelList, row, 1,
                             engineName(channel.engine,
                                        config.standardEngineName), force);
        setListTextIfChanged(channelList, row, 2,
                             unsignedValue(channel.bankMsb), force);
        setListTextIfChanged(channelList, row, 3,
                             unsignedValue(channel.bankLsb), force);
        setListTextIfChanged(channelList, row, 4,
                             unsignedValue(channel.program + 1), force);
        setListTextIfChanged(channelList, row, 5,
                             unsignedValue(channel.activeNotes), force);
        setListTextIfChanged(channelList, row, 6,
                             channel.activeNotes == 0
                                 ? L"-" : unsignedValue(channel.lastNote), force);
        setListTextIfChanged(channelList, row, 7,
                             unsignedValue(channel.breath), force);
        setListTextIfChanged(channelList, row, 8,
                             unsignedValue(channel.expression), force);
        setListTextIfChanged(channelList, row, 9,
                             std::to_wstring(channel.pitchBend), force);
        const auto sends = unsignedValue(channel.reverb) + L"/"
            + unsignedValue(channel.chorus) + L"/"
            + unsignedValue(channel.variation);
        setListTextIfChanged(channelList, row, 10, sends, force);
    }
}

void HybridEditor::refreshActivity(const HybridStatusSnapshot& snapshot,
                                   bool force)
{
    auto selected = ListView_GetNextItem(channelList, -1, LVNI_SELECTED);
    if (selected < 0 || selected >= 16)
        selected = 0;
    const auto& channel = snapshot.channels[static_cast<std::size_t>(selected)];
    std::wostringstream text;
    text << L"Selected channel " << selected + 1 << L", "
         << engineName(channel.engine, config.standardEngineName) << L". Bank "
         << static_cast<unsigned>(channel.bankMsb) << L":"
         << static_cast<unsigned>(channel.bankLsb) << L", program "
         << static_cast<unsigned>(channel.program + 1) << L". "
         << L"Breath " << static_cast<unsigned>(channel.breath)
         << L", modulation " << static_cast<unsigned>(channel.modulation)
         << L", expression " << static_cast<unsigned>(channel.expression)
         << L", pitch bend " << channel.pitchBend << L". "
         << L"Active notes " << channel.activeNotes;
    if (channel.activeNotes != 0) {
        text << L", last note " << static_cast<unsigned>(channel.lastNote)
             << L" at velocity " << static_cast<unsigned>(channel.velocity);
    }
    text << L". Sustain " << (channel.sustain ? L"on" : L"off") << L".";
    setWindowTextIfChanged(activityEdit, text.str(), previousActivity, force);
}

void HybridEditor::refreshRouting(const HybridStatusSnapshot& snapshot,
                                  bool force)
{
    std::wostringstream text;
    text << L"Live routing order\r\n\r\n" << config.routingDescription
         << L"\r\n\r\n"
         << L"Current VL workers\r\n";
    for (std::size_t index = 0; index < snapshot.vlWorkers.size(); ++index) {
        text << L"Worker " << index + 1 << L": "
             << workerName(snapshot.vlWorkers[index]);
        if (snapshot.vlWorkerChannels[index] < 16)
            text << L", channel " << snapshot.vlWorkerChannels[index] + 1;
        text << L".\r\n";
    }
    text << L"\r\nRuntime checks\r\nVL files available: "
         << yesNo(snapshot.vlAvailable) << L".\r\nSG files available: "
         << yesNo(snapshot.sgAvailable) << L".\r\nSG worker active: "
         << yesNo(snapshot.sgActive) << L".\r\nSG worker failure: "
         << yesNo(snapshot.sgFailed) << L".\r\nYamaha effects bridge: "
         << (snapshot.effectsBridgeAvailable ? L"active" : L"unavailable")
         << L".\r\n";
    if (config.supplementalEngineName != nullptr
        && config.supplementalEngineName[0] != L'\0') {
        text << config.supplementalEngineName << L" runtime: "
             << (snapshot.supplementalEngineAvailable
                     ? L"active" : L"unavailable") << L".\r\n";
    }
    text << L"Host sample rate: " << snapshot.sampleRate << L" Hz.\r\n";
    setWindowTextIfChanged(routingEdit, text.str(), previousRouting, force);
}

void HybridEditor::paintVisualization(HDC deviceContext) noexcept
{
    RECT client {};
    GetClientRect(window, &client);
    FillRect(deviceContext, &client,
             reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    if (page != Page::status || activityEdit == nullptr)
        return;

    RECT activity {};
    GetWindowRect(activityEdit, &activity);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&activity), 2);
    RECT visual {
        activity.right + 8, activity.top,
        client.right - edge, activity.bottom
    };
    FillRect(deviceContext, &visual,
             reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
    FrameRect(deviceContext, &visual,
              reinterpret_cast<HBRUSH>(COLOR_WINDOWFRAME + 1));
    SetBkMode(deviceContext, TRANSPARENT);
    SelectObject(deviceContext, controlFont);
    auto selected = ListView_GetNextItem(channelList, -1, LVNI_SELECTED);
    if (selected < 0 || selected >= 16)
        selected = 0;
    const auto& channel = latestSnapshot.channels[
        static_cast<std::size_t>(selected)];
    const wchar_t* visualTitle = L"Channel activity visualisation";
    if (channel.engine == DisplayEngine::vl)
        visualTitle = L"VL/PVL activity visualisation";
    else if (channel.engine == DisplayEngine::sg)
        visualTitle = L"SG note activity visualisation";

    RECT title = visual;
    title.left += 8;
    title.top += 4;
    DrawTextW(deviceContext, visualTitle, -1, &title,
              DT_LEFT | DT_TOP | DT_SINGLELINE);
    const int width = visual.right - visual.left;
    const int height = visual.bottom - visual.top;
    const int centerX = visual.left + width * 3 / 4;
    const int pitchOffset = channel.pitchBend * std::max(1, height / 4) / 8192;
    const int centerY = visual.top + height * 2 / 3 - pitchOffset;
    const int radius = 10 + channel.modulation * 10 / 127;
    const int triangleEnd = visual.left + 22
        + channel.breath * std::max(1, width - 70) / 127;
    POINT triangle[3] {
        {visual.left + 14, centerY - 14},
        {visual.left + 14, centerY + 14},
        {triangleEnd, centerY},
    };
    const auto oldBrush = SelectObject(
        deviceContext, GetStockObject(LTGRAY_BRUSH));
    Polygon(deviceContext, triangle, 3);
    SelectObject(deviceContext, GetStockObject(WHITE_BRUSH));
    Ellipse(deviceContext, centerX - radius, centerY - radius,
            centerX + radius, centerY + radius);
    SelectObject(deviceContext, oldBrush);
}

void HybridEditor::copyReport()
{
    const auto text = reportText(status.displaySnapshot());
    if (!OpenClipboard(window))
        return;
    EmptyClipboard();
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
        if (auto* destination = GlobalLock(memory)) {
            std::memcpy(destination, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr)
                GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

void HybridEditor::moveFocus(HWND current, bool backwards) noexcept
{
    const auto next = GetNextDlgTabItem(window, current, backwards);
    if (next != nullptr)
        SetFocus(next);
}

bool HybridEditor::movePage(HWND current, bool backwards) noexcept
{
    const std::array<HWND, 3> buttons {
        statusButton, routingButton, legacyButton
    };
    const auto found = std::find(buttons.begin(), buttons.end(), current);
    if (found == buttons.end())
        return false;
    auto index = static_cast<std::size_t>(found - buttons.begin());
    index = backwards ? (index + buttons.size() - 1) % buttons.size()
                      : (index + 1) % buttons.size();
    if (index == 0)
        showPage(Page::status);
    else if (index == 1)
        showPage(Page::routing);
    else
        showPage(Page::legacy);
    SetFocus(buttons[index]);
    return true;
}

bool HybridEditor::activateMnemonic(wchar_t character)
{
    switch (character) {
    case L's': showPage(Page::status); return true;
    case L'u': showPage(Page::routing); return true;
    case L'y': showPage(Page::legacy); return true;
    case L'r': refresh(true); SetFocus(refreshButton); return true;
    case L'c': copyReport(); SetFocus(copyButton); return true;
    default: return false;
    }
}

std::wstring HybridEditor::reportText(
    const HybridStatusSnapshot& snapshot) const
{
    std::wostringstream text;
    text << config.productName << L" status report\r\n"
         << L"Status source: "
         << (snapshot.latestPlaybackInstance
                 ? L"latest playback instance" : L"this plugin instance")
         << L"\r\n"
         << L"Sample rate: " << snapshot.sampleRate << L" Hz\r\n"
         << L"VL available: " << yesNo(snapshot.vlAvailable) << L"\r\n"
         << L"SG available: " << yesNo(snapshot.sgAvailable) << L"\r\n"
         << L"SG active: " << yesNo(snapshot.sgActive) << L"\r\n"
         << L"Effects bridge: " << yesNo(snapshot.effectsBridgeAvailable)
         << L"\r\n";
    if (config.supplementalEngineName != nullptr
        && config.supplementalEngineName[0] != L'\0') {
        text << config.supplementalEngineName << L" runtime: "
             << yesNo(snapshot.supplementalEngineAvailable) << L"\r\n";
    }
    text << L"Last reset: " << resetName(snapshot.lastReset) << L"\r\n\r\n";
    for (std::size_t index = 0; index < snapshot.channels.size(); ++index) {
        const auto& channel = snapshot.channels[index];
        text << L"Channel " << index + 1 << L": "
             << engineName(channel.engine, config.standardEngineName)
             << L", bank " << static_cast<unsigned>(channel.bankMsb) << L":"
             << static_cast<unsigned>(channel.bankLsb) << L", program "
             << static_cast<unsigned>(channel.program + 1) << L", active notes "
             << channel.activeNotes << L", breath "
             << static_cast<unsigned>(channel.breath) << L", expression "
             << static_cast<unsigned>(channel.expression) << L", pitch bend "
             << channel.pitchBend << L".\r\n";
    }
    return text.str();
}

} // namespace hybrid
