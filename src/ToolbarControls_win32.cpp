#include "ToolbarControls_win32.h"
#include "Win32EditorDrawing.h"
#include "FloopHoleAssets.h"
#include <shellapi.h>
#include <objidl.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace flbr {
namespace {

Gdiplus::Color color(uint32_t rgb) {
    return Gdiplus::Color(255, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) {
        fprintf(stderr, "FloopHole: invalid UTF-8 toolbar text (Windows error %lu)\n", GetLastError());
        return L"(invalid text)";
    }
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

void rounded(Gdiplus::GraphicsPath& path, float x, float y, float width, float height, float radius = 3) {
    const float diameter = std::min({radius * 2, width, height});
    path.AddArc(x, y, diameter, diameter, 180, 90);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
}

void text(Gdiplus::Graphics& graphics, const std::wstring& value, Gdiplus::RectF bounds, uint32_t rgb, float size = 12) {
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, size, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush brush(color(rgb));
    Gdiplus::StringFormat format = editorStringFormat();
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    graphics.DrawString(value.c_str(), static_cast<int>(value.size()), &font, bounds, &format, &brush);
}

}

WindowsToolbar::WindowsToolbar(HWND parent, Actions actions) : m_parent(parent), m_actions(std::move(actions)) {}
WindowsToolbar::~WindowsToolbar() {
    if (m_about) EndDialog(m_about, IDCANCEL);
    closeMenu();
    for (HWND window : m_controls) if (window && IsWindow(window)) DestroyWindow(window);
    if (m_module) windowClasses().release();
}

Win32WindowClasses& WindowsToolbar::windowClasses() {
    static Win32WindowClasses classes({
        {L"FloopHoleToolbarControl", controlProc, 0, LoadCursor(nullptr, IDC_ARROW)},
        {L"FloopHolePresetMenu", menuProc, CS_DROPSHADOW, LoadCursor(nullptr, IDC_ARROW)}
    });
    return classes;
}

bool WindowsToolbar::attach() {
    if (m_module) {
        fprintf(stderr, "FloopHole: toolbar is already attached\n");
        return false;
    }
    m_module = windowClasses().acquire();
    if (!m_module) return false;
    const wchar_t* names[] = {L"About FloopHole", L"Preset", L"Previous preset", L"Next preset",
        L"Load preset", L"Save preset as...", L"Ready"};
    for (size_t i = 0; i < m_controls.size(); ++i) {
        m_data[i] = {this, static_cast<int>(i), false, false};
        m_controls[i] = CreateWindowExW(0, L"FloopHoleToolbarControl", names[i],
            WS_CHILD | WS_VISIBLE | (i <= kBridgeSavePresetControl ? WS_TABSTOP : 0), 0, 0, 38, 38,
            m_parent, nullptr, m_module, &m_data[i]);
        if (!m_controls[i]) {
            fprintf(stderr, "FloopHole: toolbar control creation failed (%lu)\n", GetLastError());
            return false;
        }
    }
    return true;
}

void WindowsToolbar::setPresets(const std::vector<FLPresetEntry>& presets, int selected) {
    closeMenu();
    m_items = presetMenuItems(presets);
    m_wideItems.clear();
    m_lowerItems.clear();
    m_wideItems.reserve(m_items.size());
    m_lowerItems.reserve(m_items.size());
    for (const auto& item : m_items) {
        std::wstring w = wide(item.text);
        m_wideItems.push_back(w);
        for (auto& c : w) c = towlower(c);
        m_lowerItems.push_back(std::move(w));
    }
    m_selected = -1;
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].preset == selected && selected >= 0) m_selected = static_cast<int>(i);
    m_title = (m_selected >= 0 && m_selected < static_cast<int>(m_wideItems.size())) ?
        m_wideItems[m_selected] : L"Default / Internal Patch";
    SetWindowTextW(m_controls[1], m_title.c_str());
    InvalidateRect(m_controls[1], nullptr, FALSE);
}

void WindowsToolbar::setHint(const std::string& hint) {
    m_hint = wide(hint);
    SetWindowTextW(m_controls[kBridgeHintControl], m_hint.c_str());
    InvalidateRect(m_controls[kBridgeHintControl], nullptr, FALSE);
}

void WindowsToolbar::activate(int control) {
    if (control == 0) showAbout();
    else if (control == 1) openMenu();
    else if (control == 2 && m_actions.previous) m_actions.previous();
    else if (control == 3 && m_actions.next) m_actions.next();
    else if (control == kBridgeLoadPresetControl && m_actions.load) m_actions.load();
    else if (control == kBridgeSavePresetControl && m_actions.save) m_actions.save();
}

void WindowsToolbar::showAbout() {
    closeMenu();
    struct {
        DLGTEMPLATE dialog;
        WORD menu = 0, windowClass = 0, title = 0;
    } layout{};
    layout.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    layout.dialog.cx = 270;
    layout.dialog.cy = 180;
    if (DialogBoxIndirectParamW(m_module, &layout.dialog, GetAncestor(m_parent, GA_ROOT),
            aboutProc, reinterpret_cast<LPARAM>(this)) == -1)
        fprintf(stderr, "FloopHole: could not open About window (%lu)\n", GetLastError());
}

INT_PTR CALLBACK WindowsToolbar::aboutProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* toolbar = reinterpret_cast<WindowsToolbar*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        toolbar = reinterpret_cast<WindowsToolbar*>(lp);
        SetWindowLongPtrW(window, DWLP_USER, lp);
        toolbar->m_about = window;
        SetWindowTextW(window, L"About FloopHole");
        RECT bounds{0, 0, 420, 280};
        AdjustWindowRectEx(&bounds, GetWindowLongW(window, GWL_STYLE), FALSE, GetWindowLongW(window, GWL_EXSTYLE));
        SetWindowPos(window, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
            SWP_NOMOVE | SWP_NOZORDER);
        HWND link = CreateWindowExW(0, L"BUTTON", wide(kFloopHoleRepository).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_FLAT, 10, 180, 400, 30,
            window, reinterpret_cast<HMENU>(100), toolbar->m_module, nullptr);
        HWND close = CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 170, 232, 80, 28,
            window, reinterpret_cast<HMENU>(IDOK), toolbar->m_module, nullptr);
        if (!link || !close) {
            fprintf(stderr, "FloopHole: could not create About controls (%lu)\n", GetLastError());
            EndDialog(window, IDCANCEL);
        }
        return TRUE;
    }
    if (!toolbar) return FALSE;
    if (message == WM_COMMAND) {
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) EndDialog(window, LOWORD(wp));
        else if (LOWORD(wp) == 100) {
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", wide(kFloopHoleRepository).c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                MessageBoxW(window, L"Could not open the repository in your browser.", L"FloopHole", MB_ICONERROR);
        }
        return TRUE;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        {
            Gdiplus::Graphics graphics(dc);
            graphics.Clear(color(ToolbarTheme::background));
            auto logo = renderLogo(86);
            if (!logo.pixels.empty()) {
                Gdiplus::Bitmap image(86, 86, logo.stride, PixelFormat32bppPARGB, logo.pixels.data());
                graphics.DrawImage(&image, 167, 20);
            }
            graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            Gdiplus::StringFormat centered = editorStringFormat();
            centered.SetAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::Font heading(L"Segoe UI", 24, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::Font body(L"Segoe UI", 13, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush foreground(color(ToolbarTheme::text));
            graphics.DrawString(L"FloopHole", -1, &heading, Gdiplus::RectF(10, 118, 400, 32), &centered, &foreground);
            graphics.DrawString(wide(std::string("Version ") + kFloopHoleVersion).c_str(), -1, &body,
                Gdiplus::RectF(10, 155, 400, 22), &centered, &foreground);
        }
        EndPaint(window, &paint);
        return TRUE;
    }
    if (message == WM_DESTROY) toolbar->m_about = nullptr;
    return FALSE;
}

void WindowsToolbar::paintControl(HWND window, int index, bool hover, bool pressed) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT rect{};
    GetClientRect(window, &rect);
    const int width = rect.right, height = rect.bottom;
    if (width > 0 && height > 0) {
        Gdiplus::Bitmap buffer(width, height, PixelFormat32bppPARGB);
        {
            Gdiplus::Graphics graphics(&buffer);
            graphics.Clear(color(ToolbarTheme::background));
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            if (index == 0) {
                const int size = std::min(width, height);
                if (m_logo.size != size) m_logo = renderLogo(size);
                if (!m_logo.pixels.empty()) {
                    Gdiplus::Bitmap logo(size, size, m_logo.stride, PixelFormat32bppPARGB, m_logo.pixels.data());
                    graphics.DrawImage(&logo, (width - size) / 2, (height - size) / 2);
                }
            } else if (index == kBridgeHintControl) {
                text(graphics, m_hint, Gdiplus::RectF(0, 0, width, height), ToolbarTheme::muted, 11);
            } else {
                Gdiplus::GraphicsPath path;
                rounded(path, 0.5f, 0.5f, width - 1.0f, height - 1.0f);
                Gdiplus::LinearGradientBrush fill(Gdiplus::PointF(0, 0), Gdiplus::PointF(0, height),
                    color(hover ? ToolbarTheme::hover : ToolbarTheme::controlTop),
                    color(pressed ? ToolbarTheme::border : ToolbarTheme::controlBottom));
                graphics.FillPath(&fill, &path);
                Gdiplus::Pen outline(color(ToolbarTheme::border), 1);
                graphics.DrawPath(&outline, &path);
                Gdiplus::SolidBrush foreground(color(ToolbarTheme::text));
                const float x = width / 2.0f, y = height / 2.0f;
                if (index == 1) {
                    text(graphics, m_title, Gdiplus::RectF(9, 0, std::max(0, width - 30), height), ToolbarTheme::text);
                    Gdiplus::PointF arrow[] = {{width - 16.0f, y - 2}, {width - 8.0f, y - 2}, {width - 12.0f, y + 2}};
                    graphics.FillPolygon(&foreground, arrow, 3);
                } else if (index == kBridgeLoadPresetControl) {
                    Gdiplus::PointF folder[] = {{x - 6, y + 4}, {x + 6, y + 4}, {x + 6, y - 3},
                        {x - 1, y - 3}, {x - 3, y - 5}, {x - 6, y - 5}, {x - 6, y + 4}};
                    Gdiplus::Pen pen(color(ToolbarTheme::text), 1);
                    graphics.DrawLines(&pen, folder, 7);
                } else if (index == kBridgeSavePresetControl) {
                    Gdiplus::PointF disk[] = {{x - 6, y - 6}, {x + 3, y - 6}, {x + 6, y - 3},
                        {x + 6, y + 6}, {x - 6, y + 6}, {x - 6, y - 6}};
                    Gdiplus::Pen pen(color(ToolbarTheme::text), 1);
                    graphics.DrawLines(&pen, disk, 6);
                    graphics.DrawRectangle(&pen, x - 3, y + 1, 6.0f, 5.0f);
                    graphics.DrawLine(&pen, x + 1, y - 5, x + 1, y - 2);
                } else {
                    const float direction = index == 2 ? -1 : 1;
                    Gdiplus::PointF arrow[] = {{x + 3 * direction, y},
                        {x - 2 * direction, y - 4}, {x - 2 * direction, y + 4}};
                    graphics.FillPolygon(&foreground, arrow, 3);
                }
                if (GetFocus() == window) {
                    Gdiplus::Pen focus(color(ToolbarTheme::selected), 1);
                    graphics.DrawPath(&focus, &path);
                }
            }
        }
        {
            Gdiplus::Graphics target(dc);
            target.DrawImage(&buffer, 0, 0);
        }
    }
    EndPaint(window, &paint);
}

LRESULT CALLBACK WindowsToolbar::controlProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* data = reinterpret_cast<Control*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!data) return DefWindowProcW(window, message, wp, lp);
    switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: data->owner->paintControl(window, data->index, data->hover, data->pressed); return 0;
        case WM_GETDLGCODE: return DLGC_WANTARROWS;
        case WM_LBUTTONDOWN:
            if (data->index < 0 || data->index > kBridgeSavePresetControl) return 0;
            SetFocus(window); SetCapture(window); data->pressed = true; InvalidateRect(window, nullptr, FALSE); return 0;
        case WM_LBUTTONUP: {
            const bool pressed = data->pressed;
            data->pressed = false;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            RECT rect{}; GetClientRect(window, &rect);
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pressed && PtInRect(&rect, point)) data->owner->activate(data->index);
            return 0;
        }
        case WM_CAPTURECHANGED: data->pressed = false; InvalidateRect(window, nullptr, FALSE); return 0;
        case WM_MOUSEMOVE:
            if (!data->hover) {
                data->hover = true;
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
                TrackMouseEvent(&tracking);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSELEAVE: data->hover = false; InvalidateRect(window, nullptr, FALSE); return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS: InvalidateRect(window, nullptr, FALSE); return 0;
        case WM_KEYDOWN:
            if (wp == VK_SPACE || wp == VK_RETURN || (data->index == 1 && (wp == VK_UP || wp == VK_DOWN))) {
                data->owner->activate(data->index);
                return 0;
            }
            break;
    }
    return DefWindowProcW(window, message, wp, lp);
}

void WindowsToolbar::openMenu() {
    if (m_menu) { closeMenu(); return; }
    if (m_items.empty()) return;
    RECT anchor{}; GetWindowRect(m_controls[1], &anchor);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(m_parent, MONITOR_DEFAULTTONEAREST), &monitor)) {
        fprintf(stderr, "FloopHole: cannot position preset menu (%lu)\n", GetLastError());
        return;
    }
    const int workHeight = static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top);
    const int workWidth = static_cast<int>(monitor.rcWork.right - monitor.rcWork.left);
    m_visible = std::min({static_cast<int>(m_items.size()), ToolbarTheme::maxVisibleRows,
        std::max(1, (workHeight - 10) / ToolbarTheme::menuRowHeight)});
    const int width = std::min(std::max(static_cast<int>(anchor.right - anchor.left), 240), workWidth);
    const int height = m_visible * ToolbarTheme::menuRowHeight + 2;
    const int x = std::clamp(anchor.left, monitor.rcWork.left, monitor.rcWork.right - width);
    const int y = std::clamp(anchor.bottom + height + 3 <= monitor.rcWork.bottom ? anchor.bottom + 3 : anchor.top - height - 3,
        monitor.rcWork.top, monitor.rcWork.bottom - height);
    m_first = std::clamp(m_selected - m_visible / 2, 0, std::max(0, static_cast<int>(m_items.size()) - m_visible));
    m_highlighted = m_selected;
    m_thumbDragging = false;
    m_thumbHovered = false;
    m_searchQuery.clear();
    m_lastSearchTime = 0;
    m_menu = CreateWindowExW(WS_EX_TOOLWINDOW, L"FloopHolePresetMenu", L"Presets", WS_POPUP,
        x, y, width, height, GetAncestor(m_parent, GA_ROOT), nullptr, m_module, this);
    if (!m_menu) {
        fprintf(stderr, "FloopHole: cannot create preset menu (%lu)\n", GetLastError());
        return;
    }
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 6, 6);
    if (!region) {
        fprintf(stderr, "FloopHole: cannot create rounded preset menu region\n");
        closeMenu();
        return;
    }
    if (!SetWindowRgn(m_menu, region, FALSE)) {
        const DWORD error = GetLastError();
        DeleteObject(region);
        fprintf(stderr, "FloopHole: cannot apply rounded preset menu region (%lu)\n", error);
        closeMenu();
        return;
    }
    ShowWindow(m_menu, SW_SHOW);
    SetFocus(m_menu);
    SetCapture(m_menu);
}

void WindowsToolbar::closeMenu() {
    m_thumbDragging = false;
    m_thumbHovered = false;
    m_searchQuery.clear();
    m_lastSearchTime = 0;
    HWND menu = m_menu;
    m_menu = nullptr;
    if (!menu) return;
    const bool focused = GetFocus() == menu;
    if (GetCapture() == menu) ReleaseCapture();
    DestroyWindow(menu);
    if (focused && IsWindow(m_controls[1])) SetFocus(m_controls[1]);
}

void WindowsToolbar::chooseRow(int row) {
    if (row < 0 || row >= static_cast<int>(m_items.size()) || m_items[row].preset < 0) return;
    const int preset = m_items[row].preset;
    closeMenu();
    if (m_actions.preset) m_actions.preset(preset);
}

void WindowsToolbar::scrollToRow(int row) {
    if (row < 0 || row >= static_cast<int>(m_items.size())) return;
    m_highlighted = row;
    if (row < m_first) m_first = row;
    if (row >= m_first + m_visible) m_first = row - m_visible + 1;
    InvalidateRect(m_menu, nullptr, FALSE);
}

int WindowsToolbar::findMatch(const std::wstring& queryLower) const {
    if (queryLower.empty()) return -1;
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (m_items[i].preset < 0 || i >= m_lowerItems.size()) continue;
        const auto& textLower = m_lowerItems[i];
        if (textLower.size() >= queryLower.size() &&
            textLower.compare(0, queryLower.size(), queryLower) == 0) {
            return static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (m_items[i].preset < 0 || i >= m_lowerItems.size()) continue;
        const auto& textLower = m_lowerItems[i];
        if (textLower.find(queryLower) != std::wstring::npos) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void WindowsToolbar::handleChar(wchar_t ch) {
    if (m_items.empty() || !m_menu) return;
    const DWORD now = GetTickCount();

    if (ch == VK_BACK || ch == 8) {
        if (!m_searchQuery.empty()) {
            m_searchQuery.pop_back();
            m_lastSearchTime = now;
            if (m_searchQuery.empty()) return;
            std::wstring queryLower = m_searchQuery;
            for (auto& c : queryLower) c = towlower(c);
            const int matchRow = findMatch(queryLower);
            if (matchRow >= 0) scrollToRow(matchRow);
        }
        return;
    }

    if (ch < 32) return;
    if (ch == L' ' && m_searchQuery.empty()) return;

    const bool repeatSingle = (m_searchQuery.size() == 1 &&
        towlower(m_searchQuery[0]) == towlower(ch) &&
        (now - m_lastSearchTime) <= 1000);

    if (repeatSingle) {
        m_lastSearchTime = now;
        const int total = static_cast<int>(m_items.size());
        const int start = (m_highlighted >= 0 ? m_highlighted + 1 : 0);
        for (int offset = 0; offset < total; ++offset) {
            const int row = (start + offset) % total;
            if (m_items[row].preset >= 0 && row < static_cast<int>(m_lowerItems.size())) {
                const auto& textLower = m_lowerItems[row];
                if (!textLower.empty() && textLower[0] == towlower(ch)) {
                    scrollToRow(row);
                    return;
                }
            }
        }
        return;
    }

    std::wstring testQuery = (now - m_lastSearchTime > 1000) ? std::wstring(1, ch) : (m_searchQuery + ch);
    std::wstring queryLower = testQuery;
    for (auto& c : queryLower) c = towlower(c);

    const int matchRow = findMatch(queryLower);
    if (matchRow >= 0) {
        m_searchQuery = std::move(testQuery);
        m_lastSearchTime = now;
        scrollToRow(matchRow);
    } else if (now - m_lastSearchTime > 1000) {
        m_searchQuery.clear();
        m_lastSearchTime = now;
    }
}

void WindowsToolbar::paintMenu() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(m_menu, &paint);
    RECT rect{}; GetClientRect(m_menu, &rect);
    Gdiplus::Bitmap buffer(rect.right, rect.bottom, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics graphics(&buffer);
        graphics.Clear(color(ToolbarTheme::menu));
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        Gdiplus::SolidBrush highlight(color(ToolbarTheme::selected));
        for (int row = m_first; row < std::min(static_cast<int>(m_items.size()), m_first + m_visible); ++row) {
            Gdiplus::RectF bounds(1, 1 + (row - m_first) * ToolbarTheme::menuRowHeight, rect.right - 2,
                ToolbarTheme::menuRowHeight);
            if (row == m_highlighted && m_items[row].preset >= 0) graphics.FillRectangle(&highlight, bounds);
            bounds.X += 9; bounds.Width -= 18;
            const auto& rowText = (row >= 0 && row < static_cast<int>(m_wideItems.size())) ?
                m_wideItems[row] : wide(m_items[row].text);
            text(graphics, rowText, bounds, m_items[row].preset >= 0 ? ToolbarTheme::text : ToolbarTheme::muted);
        }
        if (static_cast<int>(m_items.size()) > m_visible) {
            const float available = rect.bottom - 8.0f;
            const float height = std::max(18.0f, available * m_visible / m_items.size());
            const float top = 4 + (available - height) * m_first / (m_items.size() - m_visible);
            const bool thumbActive = m_thumbDragging || m_thumbHovered;
            Gdiplus::SolidBrush thumb(color(thumbActive ? ToolbarTheme::text : ToolbarTheme::muted));
            graphics.FillRectangle(&thumb, rect.right - (thumbActive ? 6.0f : 5.0f), top, thumbActive ? 4.0f : 3.0f, height);
        }
        Gdiplus::Pen border(color(ToolbarTheme::border), 1);
        graphics.DrawRectangle(&border, 0, 0, rect.right - 1, rect.bottom - 1);
    }
    {
        Gdiplus::Graphics target(dc);
        target.DrawImage(&buffer, 0, 0);
    }
    EndPaint(m_menu, &paint);
}

LRESULT CALLBACK WindowsToolbar::menuProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<WindowsToolbar*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) return DefWindowProcW(window, message, wp, lp);
    switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: if (self->m_menu) self->paintMenu(); return 0;
        case WM_KILLFOCUS: self->closeMenu(); return 0;
        case WM_CANCELMODE: self->closeMenu(); return 0;
        case WM_CAPTURECHANGED:
            self->m_thumbDragging = false;
            if (self->m_menu && reinterpret_cast<HWND>(lp) != window) self->closeMenu();
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_LBUTTONDOWN: {
            RECT rect{}; GetClientRect(window, &rect);
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!PtInRect(&rect, point)) {
                self->closeMenu();
                return 0;
            }
            const int total = static_cast<int>(self->m_items.size());
            const bool hasScrollbar = total > self->m_visible;
            constexpr int kScrollbarWidth = 14;
            if (hasScrollbar && point.x >= rect.right - kScrollbarWidth) {
                const float available = rect.bottom - 8.0f;
                const float thumbHeight = std::max(18.0f, available * self->m_visible / total);
                const int maxFirst = total - self->m_visible;
                const float thumbTop = 4.0f + (available - thumbHeight) * self->m_first / maxFirst;

                if (point.y >= thumbTop && point.y <= thumbTop + thumbHeight) {
                    self->m_thumbDragging = true;
                    self->m_dragThumbOffsetY = point.y - thumbTop;
                } else {
                    const float newTop = std::clamp(point.y - thumbHeight / 2.0f, 4.0f, 4.0f + available - thumbHeight);
                    const float fraction = (available > thumbHeight) ? (newTop - 4.0f) / (available - thumbHeight) : 0.0f;
                    self->m_first = std::clamp(static_cast<int>(std::round(fraction * maxFirst)), 0, maxFirst);
                    self->m_thumbDragging = true;
                    self->m_dragThumbOffsetY = thumbHeight / 2.0f;
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (self->m_thumbDragging) {
                self->m_thumbDragging = false;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            RECT rect{}; GetClientRect(window, &rect);
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int total = static_cast<int>(self->m_items.size());
            const bool hasScrollbar = total > self->m_visible;
            constexpr int kScrollbarWidth = 14;
            if (PtInRect(&rect, point)) {
                if (hasScrollbar && point.x >= rect.right - kScrollbarWidth) return 0;
                self->chooseRow(self->m_first + (point.y - 1) / ToolbarTheme::menuRowHeight);
            } else {
                self->closeMenu();
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            RECT rect{}; GetClientRect(window, &rect);
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int total = static_cast<int>(self->m_items.size());
            const bool hasScrollbar = total > self->m_visible;
            constexpr int kScrollbarWidth = 14;

            if (self->m_thumbDragging && hasScrollbar) {
                const float available = rect.bottom - 8.0f;
                const float thumbHeight = std::max(18.0f, available * self->m_visible / total);
                const int maxFirst = total - self->m_visible;
                if (available > thumbHeight && maxFirst > 0) {
                    const float newTop = std::clamp(point.y - self->m_dragThumbOffsetY, 4.0f, 4.0f + available - thumbHeight);
                    const float fraction = (newTop - 4.0f) / (available - thumbHeight);
                    const int newFirst = std::clamp(static_cast<int>(std::round(fraction * maxFirst)), 0, maxFirst);
                    if (newFirst != self->m_first) {
                        self->m_first = newFirst;
                        InvalidateRect(window, nullptr, FALSE);
                    }
                }
                return 0;
            }

            const bool overScrollbar = hasScrollbar && PtInRect(&rect, point) && point.x >= rect.right - kScrollbarWidth;
            if (self->m_thumbHovered != overScrollbar) {
                self->m_thumbHovered = overScrollbar;
                InvalidateRect(window, nullptr, FALSE);
            }

            if (PtInRect(&rect, point) && !overScrollbar) {
                const int newHighlight = self->m_first + (point.y - 1) / ToolbarTheme::menuRowHeight;
                if (newHighlight != self->m_highlighted) {
                    self->m_highlighted = newHighlight;
                    self->m_searchQuery.clear();
                    self->m_lastSearchTime = 0;
                    InvalidateRect(window, nullptr, FALSE);
                }
            } else if (self->m_highlighted != -1) {
                self->m_highlighted = -1;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            self->m_searchQuery.clear();
            self->m_lastSearchTime = 0;
            self->m_wheelRemainder += GET_WHEEL_DELTA_WPARAM(wp);
            const int steps = self->m_wheelRemainder / WHEEL_DELTA;
            self->m_wheelRemainder -= steps * WHEEL_DELTA;
            self->m_first = std::clamp(self->m_first - steps, 0,
                std::max(0, static_cast<int>(self->m_items.size()) - self->m_visible));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_CHAR:
            self->handleChar(static_cast<wchar_t>(wp));
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { self->closeMenu(); return 0; }
            if (wp == VK_RETURN) { self->chooseRow(self->m_highlighted); return 0; }
            if (wp == VK_UP || wp == VK_DOWN) {
                self->m_searchQuery.clear();
                self->m_lastSearchTime = 0;
                const int direction = wp == VK_DOWN ? 1 : -1;
                int row = self->m_highlighted;
                do { row += direction; }
                while (row >= 0 && row < static_cast<int>(self->m_items.size()) && self->m_items[row].preset < 0);
                if (row >= 0 && row < static_cast<int>(self->m_items.size())) {
                    self->scrollToRow(row);
                }
                return 0;
            }
            if (wp == VK_HOME) {
                self->m_searchQuery.clear();
                self->m_lastSearchTime = 0;
                for (int row = 0; row < static_cast<int>(self->m_items.size()); ++row) {
                    if (self->m_items[row].preset >= 0) {
                        self->scrollToRow(row);
                        break;
                    }
                }
                return 0;
            }
            if (wp == VK_END) {
                self->m_searchQuery.clear();
                self->m_lastSearchTime = 0;
                for (int row = static_cast<int>(self->m_items.size()) - 1; row >= 0; --row) {
                    if (self->m_items[row].preset >= 0) {
                        self->scrollToRow(row);
                        break;
                    }
                }
                return 0;
            }
            if (wp == VK_PRIOR) {
                self->m_searchQuery.clear();
                self->m_lastSearchTime = 0;
                int target = std::max(0, self->m_highlighted - self->m_visible);
                while (target > 0 && self->m_items[target].preset < 0) --target;
                if (target < static_cast<int>(self->m_items.size()) && self->m_items[target].preset >= 0) {
                    self->scrollToRow(target);
                }
                return 0;
            }
            if (wp == VK_NEXT) {
                self->m_searchQuery.clear();
                self->m_lastSearchTime = 0;
                int target = std::min(static_cast<int>(self->m_items.size()) - 1, self->m_highlighted + self->m_visible);
                while (target < static_cast<int>(self->m_items.size()) - 1 && self->m_items[target].preset < 0) ++target;
                if (target >= 0 && self->m_items[target].preset >= 0) {
                    self->scrollToRow(target);
                }
                return 0;
            }
            break;
    }
    return DefWindowProcW(window, message, wp, lp);
}

}
