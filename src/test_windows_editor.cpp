#include "Win32EditorDrawing.h"
#include "ToolbarControls_win32.h"
#include "BridgeEditorLayout.h"
#include "Win32AudioFileDrag.h"
#include "Win32EditorKeyboard.h"
#include <algorithm>
#include <array>
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

static void testEditorKeyboard() {
    using namespace Steinberg;
    struct Input {
        bool consume = false;
        int downs = 0, ups = 0;
        int rawDowns = 0, rawUps = 0, characters = 0;
        LPARAM data = 0;
        std::array<BYTE, 256> modifiers{};
    } wrapperInput, childInput;
    Win32WindowClasses classes({{L"FloopHoleTestKeyboard",
        +[](HWND window, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
            auto* input = reinterpret_cast<Input*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (input && (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR)) {
                if (message == WM_KEYDOWN) ++input->rawDowns;
                if (message == WM_KEYUP) ++input->rawUps;
                if (message == WM_CHAR) ++input->characters;
                return 0;
            }
            if (input && (message == 0xbc00 + WM_KEYDOWN || message == 0xbc00 + WM_KEYUP)) {
                CHECK(GetKeyboardState(input->modifiers.data()));
                input->data = lp;
                if (message == 0xbc00 + WM_KEYUP) {
                    ++input->ups;
                    return 0;
                }
                ++input->downs;
                return input->consume && (input->modifiers[VK_CONTROL] & 0x80) &&
                    (wp == 'C' || wp == 'V' || wp == VK_DELETE);
            }
            return DefWindowProcW(window, message, wp, lp);
        }, 0, nullptr}});
    HINSTANCE module = classes.acquire();
    CHECK(module);
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 10, 10,
        nullptr, nullptr, module, nullptr);
    HWND container = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 10, 10,
        parent, nullptr, module, nullptr);
    HWND wrapper = CreateWindowExW(0, L"FloopHoleTestKeyboard", L"", WS_CHILD, 0, 0, 10, 10,
        container, nullptr, module, nullptr);
    HWND child = CreateWindowExW(0, L"FloopHoleTestKeyboard", L"", WS_CHILD, 0, 0, 10, 10,
        wrapper, nullptr, module, nullptr);
    HWND toolbar = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 10, 10,
        parent, nullptr, module, nullptr);
    CHECK(parent && container && wrapper && child && toolbar);
    SetWindowLongPtrW(wrapper, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&wrapperInput));
    SetWindowLongPtrW(child, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&childInput));
    wrapperInput.consume = true;
    SetFocus(child);
    CHECK(GetFocus() == child);
    Win32EditorKeyboard keyboard;
    std::array<BYTE, 256> before{}, after{};
    CHECK(GetKeyboardState(before.data()));
    CHECK(!keyboard.keyDown(nullptr, u'c', 0, kCommandKey));
    CHECK(keyboard.keyDown(container, u'c', 0, kCommandKey));
    CHECK(wrapperInput.downs == 1 && childInput.downs == 1);
    CHECK(wrapperInput.modifiers[VK_CONTROL] & 0x80);
    CHECK(!(wrapperInput.modifiers[VK_SHIFT] & 0x80));
    CHECK(!(wrapperInput.data & (LPARAM(1) << 30)));
    CHECK(keyboard.keyDown(container, u'\x03', 0, kCommandKey));
    CHECK(wrapperInput.downs == 2 && (wrapperInput.data & (LPARAM(1) << 30)));
    CHECK(keyboard.keyUp(container, u'c', 0, 0));
    CHECK(wrapperInput.ups == 1 && childInput.ups == 0);
    CHECK((wrapperInput.data & 0xc0000000LL) == 0xc0000000LL);
    CHECK(!keyboard.keyUp(container, u'c', 0, 0));
    CHECK(GetKeyboardState(after.data()) && before == after);

    CHECK(!keyboard.keyDown(container, u'c', 0, kControlKey));
    CHECK(!keyboard.keyDown(container, u'q', 0, kCommandKey));
    CHECK(!keyboard.keyUp(container, u'q', 0, kCommandKey));
    CHECK(keyboard.keyDown(container, 0, CharToVirtualKeyCode('V'), kCommandKey | kShiftKey));
    CHECK(wrapperInput.modifiers[VK_SHIFT] & 0x80);
    CHECK(keyboard.keyUp(container, u'v', 0, 0));
    CHECK(keyboard.keyDown(container, 0, KEY_DELETE, kCommandKey));
    CHECK(wrapperInput.data & (LPARAM(1) << 24));
    CHECK(keyboard.keyUp(container, 0, KEY_DELETE, 0));
    CHECK(Win32EditorKeyboard::virtualKey(0, KEY_F24) == VK_F24);
    CHECK(Win32EditorKeyboard::virtualKey(0, KEY_ENTER) == VK_RETURN);
    CHECK(Win32EditorKeyboard::virtualKey(0, -1) == 0);

    wrapperInput.consume = false;
    CHECK(keyboard.keyDown(container, u'c', 0, kCommandKey, true));
    CHECK(childInput.rawDowns == 1 && childInput.characters == 0);
    CHECK(keyboard.keyUp(container, u'c', 0, 0) && childInput.rawUps == 1);
    CHECK(keyboard.keyDown(container, u'v', 0, kCommandKey, true));
    CHECK(childInput.rawDowns == 2 && childInput.characters == 0);
    CHECK(keyboard.keyUp(container, u'v', 0, 0) && childInput.rawUps == 2);
    CHECK(keyboard.keyDown(container, u'q', 0, 0, true));
    CHECK(childInput.rawDowns == 3 && childInput.characters == 1);
    CHECK(keyboard.keyUp(container, u'q', 0, 0));
    CHECK(wrapperInput.rawDowns == 0 && wrapperInput.rawUps == 0);
    const int childUps = childInput.ups;
    childInput.consume = true;
    const int wrapperDowns = wrapperInput.downs;
    CHECK(keyboard.keyDown(container, u'c', 0, kCommandKey));
    CHECK(wrapperInput.downs == wrapperDowns);
    CHECK(keyboard.keyUp(container, u'c', 0, 0) && childInput.ups == childUps + 1);
    CHECK(keyboard.keyDown(container, u'v', 0, kCommandKey));
    keyboard.reset();
    CHECK(!keyboard.keyUp(container, u'v', 0, 0));
    SetFocus(toolbar);
    CHECK(GetFocus() == toolbar && !keyboard.keyDown(container, u'c', 0, kCommandKey, true));
    SetFocus(container);
    CHECK(!keyboard.keyDown(container, u'c', 0, kCommandKey));
    CHECK(GetKeyboardState(after.data()) && before == after);
    CHECK(DestroyWindow(parent));
    classes.release();
}

static void testAudioFileDrag() {
    struct Component {
        uintptr_t vmt = 0, owner = 0, drag = 0;
        uint8_t enabled = 0, untouched = 42;
    } form, panel, source, otherSource, nestedForm;
    struct Class {
        std::array<uintptr_t, 26> data{};
        uintptr_t vmt() { return reinterpret_cast<uintptr_t>(data.data() + 25); }
        void set(int offset, uintptr_t value) { data[25 + offset / 8] = value; }
        Class() {
            set(-200, vmt());
            set(-128, sizeof(Component));
        }
    } formClass, panelClass, sourceClass, baseClass, nestedClass;
#pragma pack(push, 1)
    struct {
        uint16_t count = 1;
        uintptr_t classes = 0;
        uint32_t offset = offsetof(Component, drag);
        uint16_t type = 0;
        uint8_t length = 7;
        char name[8] = "DragWAV";
    } fields;
    struct {
        uint8_t kind = 3, length = 7;
        char name[8] = "Boolean";
    } boolean;
    uintptr_t booleanType = reinterpret_cast<uintptr_t>(&boolean);
    struct {
        uint8_t kind = 7, nameLength = 0;
        uintptr_t vmt = 0, parentType = 0;
        uint16_t total = 1;
        uint8_t unitLength = 0;
        uint16_t count = 1;
        audioFileDrag::Property property{};
        char name[19] = "SaveFileBeforeDrag";
    } type;
#pragma pack(pop)
    type.property.type = reinterpret_cast<uintptr_t>(&booleanType);
    type.property.setter = 0xff00000000000000ULL | offsetof(Component, enabled);
    type.property.nameLength = 18;
    sourceClass.set(-168, reinterpret_cast<uintptr_t>(&type));
    formClass.set(-160, reinterpret_cast<uintptr_t>(&fields));
    form.vmt = formClass.vmt();
    form.drag = reinterpret_cast<uintptr_t>(&source);
    source.vmt = otherSource.vmt = sourceClass.vmt();
    panel.vmt = panelClass.vmt();
    panel.owner = reinterpret_cast<uintptr_t>(&form);
    HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 10, 10,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(window && !audioFileDrag::enable(window));
    CHECK(SetPropW(window, L"ControlOfsTest", &panel));
    CHECK(audioFileDrag::enable(window));
    CHECK(source.enabled == 1 && source.untouched == 42 && otherSource.enabled == 0);
    CHECK(audioFileDrag::enable(window));
    form.drag = reinterpret_cast<uintptr_t>(&otherSource);
    CHECK(audioFileDrag::enable(window));
    CHECK(otherSource.enabled == 1 && otherSource.untouched == 42 && source.enabled == 1);
    form.drag = reinterpret_cast<uintptr_t>(&source);

    CHECK(!audioFileDrag::enable(nullptr));
    CHECK(!audioFileDrag::enable(reinterpret_cast<HWND>(1)));
    source.enabled = otherSource.enabled = 0;
    formClass.set(-160, 0);
    CHECK(!audioFileDrag::enable(window));
    nestedClass.set(-160, reinterpret_cast<uintptr_t>(&fields));
    nestedForm.vmt = nestedClass.vmt();
    nestedForm.drag = reinterpret_cast<uintptr_t>(&otherSource);
    HWND child = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 10, 10,
        window, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(child && SetPropW(child, L"ControlOfsNestedEditor", &nestedForm));
    CHECK(audioFileDrag::enable(window));
    CHECK(source.enabled == 0 && otherSource.enabled == 1 && otherSource.untouched == 42);
    CHECK(DestroyWindow(child));
    CHECK(!audioFileDrag::enable(window));
    formClass.set(-160, reinterpret_cast<uintptr_t>(&fields));

    source.enabled = 0;
    const auto setter = type.property.setter;
    type.property.setter = 0xff00000000000000ULL | sizeof(Component);
    CHECK(!audioFileDrag::enable(window) && source.enabled == 0);
    type.property.setter = offsetof(Component, enabled);
    CHECK(!audioFileDrag::enable(window) && source.enabled == 0);
    type.property.setter = setter;
    boolean.kind = 1;
    CHECK(!audioFileDrag::enable(window) && source.enabled == 0);
    boolean.kind = 3;
    type.name[0] = 'X';
    CHECK(!audioFileDrag::enable(window) && source.enabled == 0);
    type.name[0] = 'S';
    fields.offset = sizeof(Component);
    CHECK(!audioFileDrag::enable(window) && source.enabled == 0);
    fields.offset = offsetof(Component, drag);

    // Published fields and properties can move into ancestor classes.
    baseClass.set(-160, reinterpret_cast<uintptr_t>(&fields));
    baseClass.set(-168, reinterpret_cast<uintptr_t>(&type));
    uintptr_t base = baseClass.vmt();
    formClass.set(-160, 0);
    formClass.set(-120, reinterpret_cast<uintptr_t>(&base));
    sourceClass.set(-168, 0);
    sourceClass.set(-120, reinterpret_cast<uintptr_t>(&base));
    CHECK(audioFileDrag::enable(window) && source.enabled == 1);
    form.owner = reinterpret_cast<uintptr_t>(&form);
    form.drag = 0;
    CHECK(!audioFileDrag::enable(window));
    CHECK(RemovePropW(window, L"ControlOfsTest") == &panel);
    CHECK(SetPropW(window, L"ControlOfsInvalid", reinterpret_cast<HANDLE>(1)));
    CHECK(!audioFileDrag::enable(window));
    CHECK(RemovePropW(window, L"ControlOfsInvalid") == reinterpret_cast<HANDLE>(1));
    CHECK(DestroyWindow(window));
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

static void testPresetMenuItems() {
    std::vector<FLPresetEntry> presets;
    FLPresetEntry p1;
    p1.category = "Basses";
    p1.name = "Sub";
    p1.displayName = "Basses / Sub";
    presets.push_back(p1);

    FLPresetEntry p2;
    p2.category = "Basses";
    p2.name = "Reese";
    p2.displayName = "Basses / Reese";
    presets.push_back(p2);

    FLPresetEntry p3;
    p3.category = "Pads";
    p3.name = "Warm Pad";
    p3.displayName = "Pads / Warm Pad";
    presets.push_back(p3);

    FLPresetEntry p4;
    p4.category = "";
    p4.name = "Init";
    p4.displayName = "Init";
    presets.push_back(p4);

    const auto items = presetMenuItems(presets);
    CHECK(items.size() == 6);
    CHECK(items[0].text == "Basses" && items[0].preset == -1);
    CHECK(items[1].text == "Sub" && items[1].preset == 0);
    CHECK(items[2].text == "Reese" && items[2].preset == 1);
    CHECK(items[3].text == "Pads" && items[3].preset == -1);
    CHECK(items[4].text == "Warm Pad" && items[4].preset == 2);
    CHECK(items[5].text == "Init" && items[5].preset == 3);
}

static void testPresetMenuInteractions() {
    GraphicsSession session;
    HWND parent = CreateWindowExW(0, L"STATIC", L"FloopHole interaction test", WS_POPUP,
        0, 0, 640, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(parent);

    int activatedPreset = -1;
    WindowsToolbar::Actions actions{};
    actions.preset = [&](int p) { activatedPreset = p; };

    auto toolbar = std::make_unique<WindowsToolbar>(parent, actions);
    CHECK(toolbar->attach());

    std::vector<FLPresetEntry> presets;
    for (int i = 0; i < 40; ++i) {
        FLPresetEntry p;
        p.category = (i < 20) ? "Arps" : "Basses";
        p.name = (i % 2 == 0) ? ("Alpha " + std::to_string(i)) : ("Beta " + std::to_string(i));
        p.displayName = p.category + " / " + p.name;
        presets.push_back(p);
    }
    toolbar->setPresets(presets, 0);

    wchar_t titleBuf[64] = {0};
    GetWindowTextW(toolbar->controls()[1], titleBuf, 64);
    CHECK(std::wstring(titleBuf) == L"Alpha 0");

    SendMessageW(toolbar->controls()[1], WM_KEYDOWN, VK_DOWN, 0);
    HWND menu = toolbar->menuWindow();
    CHECK(menu && IsWindow(menu));

    SendMessageW(menu, WM_CHAR, L'b', 0);
    SendMessageW(menu, WM_KEYDOWN, VK_RETURN, 0);
    CHECK(!toolbar->menuWindow());
    CHECK(activatedPreset == 1);

    SendMessageW(toolbar->controls()[1], WM_KEYDOWN, VK_DOWN, 0);
    menu = toolbar->menuWindow();
    CHECK(menu && IsWindow(menu));

    SendMessageW(menu, WM_CHAR, L'a', 0);
    SendMessageW(menu, WM_CHAR, L'l', 0);
    SendMessageW(menu, WM_KEYDOWN, VK_RETURN, 0);
    CHECK(!toolbar->menuWindow());
    CHECK(activatedPreset == 0);

    // Repeated single-character typing cycles through matching presets
    SendMessageW(toolbar->controls()[1], WM_KEYDOWN, VK_DOWN, 0);
    menu = toolbar->menuWindow();
    CHECK(menu && IsWindow(menu));
    SendMessageW(menu, WM_CHAR, L'a', 0);
    CHECK(toolbar->highlightedRow() == 1); // "Alpha 0"
    SendMessageW(menu, WM_CHAR, L'a', 0);
    CHECK(toolbar->highlightedRow() == 3); // "Alpha 2"
    SendMessageW(menu, WM_CHAR, L'a', 0);
    CHECK(toolbar->highlightedRow() == 5); // "Alpha 4"

    // Backspace test: clear query with backspace, then search 'b'
    SendMessageW(menu, WM_CHAR, VK_BACK, 0);
    SendMessageW(menu, WM_CHAR, L'b', 0);
    CHECK(toolbar->highlightedRow() == 2); // "Beta 1"

    // Sleep test: after 1050ms timeout, typing starts a fresh search
    Sleep(1050);
    SendMessageW(menu, WM_CHAR, L'a', 0);
    CHECK(toolbar->highlightedRow() == 1); // "Alpha 0"

    // Navigation keys test (Home / End)
    SendMessageW(menu, WM_KEYDOWN, VK_END, 0);
    CHECK(toolbar->highlightedRow() > 30);
    SendMessageW(menu, WM_KEYDOWN, VK_HOME, 0);
    CHECK(toolbar->highlightedRow() == 1); // first enabled preset

    toolbar->closeMenu();

    // Scrollbar dragging test
    SendMessageW(toolbar->controls()[1], WM_KEYDOWN, VK_DOWN, 0);
    menu = toolbar->menuWindow();
    CHECK(menu && IsWindow(menu));
    CHECK(toolbar->firstVisibleRow() == 0);

    RECT menuRect{};
    GetClientRect(menu, &menuRect);
    LPARAM downLparam = MAKELPARAM(menuRect.right - 8, 10);
    SendMessageW(menu, WM_LBUTTONDOWN, MK_LBUTTON, downLparam);

    LPARAM moveLparam = MAKELPARAM(menuRect.right - 8, 200);
    SendMessageW(menu, WM_MOUSEMOVE, MK_LBUTTON, moveLparam);
    CHECK(toolbar->firstVisibleRow() > 0); // Scrolled down!

    activatedPreset = -999;
    SendMessageW(menu, WM_LBUTTONUP, 0, moveLparam);
    CHECK(activatedPreset == -999);
    CHECK(toolbar->menuWindow() == menu);

    // Click on an enabled row
    int targetRow = toolbar->firstVisibleRow();
    while (targetRow < static_cast<int>(presets.size()) && (targetRow == 0 || targetRow == 21)) ++targetRow;
    int clickY = 1 + (targetRow - toolbar->firstVisibleRow()) * 24 + 10;
    LPARAM rowClick = MAKELPARAM(50, clickY);
    SendMessageW(menu, WM_LBUTTONUP, 0, rowClick);
    CHECK(!toolbar->menuWindow());
    CHECK(activatedPreset >= 0);

    toolbar.reset();
    CHECK(DestroyWindow(parent));
}

int main() {
    testClassLifetime();
    testEditorKeyboard();
    testAudioFileDrag();
    testGripColors();
    testPresetMenuItems();
    testPresetMenuInteractions();
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
    std::puts("Windows editor: keyboard shortcuts, audio file drag, repeated graphics sessions, overlapping toolbars, class cleanup and dark grip passed");
    return 0;
}
