#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "ToolbarTheme.h"
#include "LogoRenderer.h"
#include "Win32WindowClasses.h"
#include <array>
#include <functional>

namespace flbr {

class WindowsToolbar {
public:
    struct Actions {
        std::function<void(int)> preset;
        std::function<void()> previous;
        std::function<void()> next;
        std::function<void()> load;
    };
    WindowsToolbar(HWND parent, Actions actions);
    ~WindowsToolbar();
    bool attach();
    const std::array<HWND, 6>& controls() const { return m_controls; }
    void setPresets(const std::vector<FLPresetEntry>& presets, int selected);
    void setHint(const std::string& hint);
    void closeMenu();
    HWND menuWindow() const { return m_menu; }
    int firstVisibleRow() const { return m_first; }
    int highlightedRow() const { return m_highlighted; }

private:
    struct Control { WindowsToolbar* owner = nullptr; int index = 0; bool hover = false; bool pressed = false; };
    static LRESULT CALLBACK controlProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK menuProc(HWND, UINT, WPARAM, LPARAM);
    static Win32WindowClasses& windowClasses();
    void paintControl(HWND, int, bool, bool);
    void paintMenu();
    void activate(int);
    void openMenu();
    void chooseRow(int);
    void handleChar(wchar_t ch);
    int findMatch(const std::wstring& queryLower) const;
    void scrollToRow(int row);
    HWND m_parent = nullptr;
    HWND m_menu = nullptr;
    HINSTANCE m_module = nullptr;
    Actions m_actions;
    std::array<HWND, 6> m_controls{};
    std::array<Control, 6> m_data{};
    std::vector<PresetMenuItem> m_items;
    std::vector<std::wstring> m_wideItems;
    std::vector<std::wstring> m_lowerItems;
    std::wstring m_title = L"Default / Internal Patch";
    std::wstring m_hint = L"Ready";
    std::wstring m_searchQuery;
    DWORD m_lastSearchTime = 0;
    int m_selected = -1;
    int m_highlighted = -1;
    int m_first = 0;
    int m_visible = 0;
    int m_wheelRemainder = 0;
    bool m_thumbDragging = false;
    bool m_thumbHovered = false;
    float m_dragThumbOffsetY = 0.0f;
    LogoRaster m_logo;
};

}
