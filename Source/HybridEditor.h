#pragma once

#include "HybridStatus.h"
#include "Vst2Abi.h"

#include <windows.h>

#include <string>

struct IAccPropServices;

namespace hybrid {

struct HybridEditorConfig {
    const wchar_t* windowClassName;
    const wchar_t* productName;
    const wchar_t* standardEngineName;
    const wchar_t* supplementalEngineName;
    const wchar_t* routingDescription;
};

class HybridEditor {
public:
    HybridEditor(HINSTANCE instance, vst2::AEffect* child,
                 HybridStatus& status, HybridEditorConfig config) noexcept;
    ~HybridEditor();

    HybridEditor(const HybridEditor&) = delete;
    HybridEditor& operator=(const HybridEditor&) = delete;

    vst2::IntPtr getRect(void* data) noexcept;
    vst2::IntPtr open(void* parent) noexcept;
    vst2::IntPtr close() noexcept;
    vst2::IntPtr idle() noexcept;

private:
    enum class Page {
        status,
        routing,
        legacy,
    };

    static LRESULT CALLBACK windowProcedure(HWND window, UINT message,
                                            WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK controlProcedure(HWND window, UINT message,
                                             WPARAM wParam, LPARAM lParam,
                                             UINT_PTR subclassId,
                                             DWORD_PTR referenceData);
    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam,
                          LPARAM lParam);
    void createControls();
    void layoutControls(int width, int height) noexcept;
    void showPage(Page page) noexcept;
    void refresh(bool force);
    void refreshSummary(const HybridStatusSnapshot& snapshot, bool force);
    void refreshChannels(const HybridStatusSnapshot& snapshot, bool force);
    void refreshActivity(const HybridStatusSnapshot& snapshot, bool force);
    void refreshRouting(const HybridStatusSnapshot& snapshot, bool force);
    void paintVisualization(HDC deviceContext) noexcept;
    void copyReport();
    void moveFocus(HWND current, bool backwards) noexcept;
    bool movePage(HWND current, bool backwards) noexcept;
    bool activateMnemonic(wchar_t character);
    [[nodiscard]] std::wstring reportText(
        const HybridStatusSnapshot& snapshot) const;

    HINSTANCE instance {};
    vst2::AEffect* child {};
    HybridStatus& status;
    HybridEditorConfig config;
    vst2::ERect editorRect {};
    HWND window {};
    HWND statusButton {};
    HWND routingButton {};
    HWND legacyButton {};
    HWND refreshButton {};
    HWND copyButton {};
    HWND summaryEdit {};
    HWND channelList {};
    HWND activityEdit {};
    HWND routingEdit {};
    HWND legacyPanel {};
    HFONT controlFont {};
    IAccPropServices* accessibilityServices {};
    bool comInitialized {};
    Page page {Page::status};
    bool childEditorOpen {};
    std::wstring previousSummary;
    std::wstring previousActivity;
    std::wstring previousRouting;
    HybridStatusSnapshot latestSnapshot {};
};

} // namespace hybrid
