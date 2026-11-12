#include "ToolbarControls_win32.h"
#include "Win32EditorDrawing.h"
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
    const wchar_t* names[] = {L"FloopHole", L"Preset", L"Previous preset", L"Next preset", L"Load preset", L"Ready"};
    for (size_t i = 0; i < m_controls.size(); ++i) {
        m_data[i] = {this, static_cast<int>(i), false, false};
        m_controls[i] = CreateWindowExW(0, L"FloopHoleToolbarControl", names[i],
            WS_CHILD | WS_VISIBLE | (i >= 1 && i <= 4 ? WS_TABSTOP : 0), 0, 0, 38, 38,
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
    m_selected = -1;
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].preset == selected && selected >= 0) m_selected = static_cast<int>(i);
    m_title = m_selected >= 0 ? wide(m_items[m_selected].text) : L"Default / Internal Patch";
    SetWindowTextW(m_controls[1], m_title.c_str());
    InvalidateRect(m_controls[1], nullptr, FALSE);
}

void WindowsToolbar::setHint(const std::string& hint) {
    m_hint = wide(hint);
    SetWindowTextW(m_controls[5], m_hint.c_str());
    InvalidateRect(m_controls[5], nullptr, FALSE);
}

void WindowsToolbar::activate(int control) {
    if (control == 1) openMenu();
    else if (control == 2 && m_actions.previous) m_actions.previous();
    else if (control == 3 && m_actions.next) m_actions.next();
    else if (control == 4 && m_actions.load) m_actions.load();
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
            } else if (index == 5) {
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
                } else if (index == 4) {
                    Gdiplus::PointF folder[] = {{x - 6, y + 4}, {x + 6, y + 4}, {x + 6, y - 3},
                        {x - 1, y - 3}, {x - 3, y - 5}, {x - 6, y - 5}, {x - 6, y + 4}};
                    Gdiplus::Pen pen(color(ToolbarTheme::text), 1);
                    graphics.DrawLines(&pen, folder, 7);
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
            if (data->index < 1 || data->index > 4) return 0;
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
            text(graphics, wide(m_items[row].text), bounds, m_items[row].preset >= 0 ? ToolbarTheme::text : ToolbarTheme::muted);
        }
        if (static_cast<int>(m_items.size()) > m_visible) {
            const float available = rect.bottom - 8.0f;
            const float height = std::max(18.0f, available * m_visible / m_items.size());
            const float top = 4 + (available - height) * m_first / (m_items.size() - m_visible);
            Gdiplus::SolidBrush thumb(color(ToolbarTheme::muted));
            graphics.FillRectangle(&thumb, rect.right - 5.0f, top, 3.0f, height);
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
            if (self->m_menu && reinterpret_cast<HWND>(lp) != window) self->closeMenu();
            return 0;
        case WM_LBUTTONDOWN: {
            RECT rect{}; GetClientRect(window, &rect);
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!PtInRect(&rect, point)) self->closeMenu();
            return 0;
        }
        case WM_LBUTTONUP: {
            RECT rect{}; GetClientRect(window, &rect);
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (PtInRect(&rect, point)) self->chooseRow(self->m_first + (point.y - 1) / ToolbarTheme::menuRowHeight);
            else self->closeMenu();
            return 0;
        }
        case WM_MOUSEMOVE: {
            RECT rect{}; GetClientRect(window, &rect);
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            self->m_highlighted = PtInRect(&rect, point) ?
                self->m_first + (point.y - 1) / ToolbarTheme::menuRowHeight : -1;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            self->m_wheelRemainder += GET_WHEEL_DELTA_WPARAM(wp);
            const int steps = self->m_wheelRemainder / WHEEL_DELTA;
            self->m_wheelRemainder -= steps * WHEEL_DELTA;
            self->m_first = std::clamp(self->m_first - steps, 0,
                std::max(0, static_cast<int>(self->m_items.size()) - self->m_visible));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { self->closeMenu(); return 0; }
            if (wp == VK_RETURN) { self->chooseRow(self->m_highlighted); return 0; }
            if (wp == VK_UP || wp == VK_DOWN) {
                const int direction = wp == VK_DOWN ? 1 : -1;
                int row = self->m_highlighted;
                do { row += direction; }
                while (row >= 0 && row < static_cast<int>(self->m_items.size()) && self->m_items[row].preset < 0);
                if (row >= 0 && row < static_cast<int>(self->m_items.size())) {
                    self->m_highlighted = row;
                    if (row < self->m_first) self->m_first = row;
                    if (row >= self->m_first + self->m_visible) self->m_first = row - self->m_visible + 1;
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            }
            break;
    }
    return DefWindowProcW(window, message, wp, lp);
}

}
