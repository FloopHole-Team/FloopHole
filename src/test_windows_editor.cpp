#include "Win32EditorDrawing.h"
#include "ToolbarControls_win32.h"
#include "BridgeEditorLayout.h"
#include <algorithm>
#include <cstdlib>
#include <memory>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "CHECK FAILED: %s (line %d, Windows error %lu)\n", \
        #condition, __LINE__, GetLastError()); std::exit(1); \
} } while (0)

using namespace flbr;

class GraphicsSession {
public:
    GraphicsSession() {
        Gdiplus::GdiplusStartupInput input;
        input.SuppressExternalCodecs = TRUE;
        CHECK(Gdiplus::GdiplusStartup(&m_token, &input, nullptr) == Gdiplus::Ok);
    }
    ~GraphicsSession() { stop(); }
    void stop() {
        if (m_token) Gdiplus::GdiplusShutdown(m_token);
        m_token = 0;
    }
private:
    ULONG_PTR m_token = 0;
};

static void paintToolbar(WindowsToolbar& toolbar) {
    const auto layout = BridgeToolbarLayout::compute(640);
    for (size_t i = 0; i < toolbar.controls().size(); ++i) {
        const auto& rect = layout.controls[i];
        HWND control = toolbar.controls()[i];
        CHECK(MoveWindow(control, rect.x, rect.y, rect.width, rect.height, FALSE));
        CHECK(InvalidateRect(control, nullptr, FALSE));
        SendMessageW(control, WM_PAINT, 0, 0);
    }
    Gdiplus::Bitmap bitmap(40, 38, PixelFormat32bppPARGB);
    Gdiplus::Graphics graphics(&bitmap);
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, 7, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush foreground(Gdiplus::Color(255, 220, 220, 220));
    Gdiplus::StringFormat format = editorStringFormat();
    CHECK(format.GetLastStatus() == Gdiplus::Ok);
    CHECK(format.SetAlignment(Gdiplus::StringAlignmentCenter) == Gdiplus::Ok);
    CHECK(format.SetLineAlignment(Gdiplus::StringAlignmentCenter) == Gdiplus::Ok);
    CHECK(graphics.DrawString(L"VOL", -1, &font, Gdiplus::RectF(0, 29, 40, 9), &format, &foreground) == Gdiplus::Ok);
}

static LRESULT CALLBACK testProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(window, message, wp, lp);
}

static void testClassLifetime() {
    Win32WindowClasses classes({{L"FloopHoleTestLifecycle", testProcedure, 0, LoadCursor(nullptr, IDC_ARROW)}});
    HINSTANCE module = classes.acquire();
    CHECK(module && classes.acquire() == module);
    WNDCLASSEXW info{};
    info.cbSize = sizeof(info);
    CHECK(GetClassInfoExW(module, L"FloopHoleTestLifecycle", &info));
    CHECK(info.hInstance == module && info.lpfnWndProc == testProcedure);
    HWND window = CreateWindowExW(0, L"FloopHoleTestLifecycle", L"", WS_POPUP, 0, 0, 10, 10,
        nullptr, nullptr, module, nullptr);
    CHECK(window);
    classes.release();
    CHECK(GetClassInfoExW(module, L"FloopHoleTestLifecycle", &info));
    CHECK(DestroyWindow(window));
    classes.release();
    CHECK(!GetClassInfoExW(module, L"FloopHoleTestLifecycle", &info));
}

static void testGripColors() {
    HDC dc = CreateCompatibleDC(nullptr);
    CHECK(dc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 14;
    info.bmiHeader.biHeight = -14;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* memory = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &memory, nullptr, 0);
    CHECK(bitmap && memory);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    RECT rect{0, 0, 14, 14};
    drawEditorResizeGrip(dc, rect);
    GdiFlush();
    const auto* pixels = static_cast<const uint32_t*>(memory);
    CHECK((pixels[0] & 0xFFFFFF) == ToolbarTheme::background);
    int strokes = 0;
    for (int i = 0; i < 14 * 14; ++i) {
        const auto color = pixels[i] & 0xFFFFFF;
        CHECK(color == ToolbarTheme::background || color == ToolbarTheme::muted);
        strokes += color == ToolbarTheme::muted;
    }
    CHECK(strokes > 0);
    SelectObject(dc, previous);
    CHECK(DeleteObject(bitmap));
    CHECK(DeleteDC(dc));
}

int main() {
    testClassLifetime();
    testGripColors();
    HWND parent = CreateWindowExW(0, L"STATIC", L"FloopHole lifecycle test", WS_POPUP,
        0, 0, 640, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(parent);
    FLPresetEntry preset;
    preset.displayName = preset.name = "Default";
    for (int cycle = 0; cycle < 32; ++cycle) {
        GraphicsSession first, second;
        auto one = std::make_unique<WindowsToolbar>(parent, WindowsToolbar::Actions{});
        auto two = std::make_unique<WindowsToolbar>(parent, WindowsToolbar::Actions{});
        CHECK(one->attach() && two->attach());
        one->setPresets({preset}, 0);
        two->setPresets({preset}, 0);
        paintToolbar(*one);
        paintToolbar(*two);
        one.reset();
        first.stop();
        paintToolbar(*two);
        two.reset();
        second.stop();
        WNDCLASSEXW info{};
        info.cbSize = sizeof(info);
        CHECK(!GetClassInfoExW(GetModuleHandleW(nullptr), L"FloopHoleToolbarControl", &info));
        CHECK(!GetClassInfoExW(GetModuleHandleW(nullptr), L"FloopHolePresetMenu", &info));
    }
    CHECK(DestroyWindow(parent));
    std::puts("Windows editor: repeated graphics sessions, overlapping toolbars, class cleanup and dark grip passed");
    return 0;
}
