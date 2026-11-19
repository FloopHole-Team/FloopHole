#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "pluginterfaces/base/keycodes.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace flbr {

namespace vclShortcut {

template<class T>
inline bool read(uintptr_t address, T& value) {
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                                        &value, sizeof(value), nullptr);
}

inline bool named(uintptr_t address, const char* name) {
    uint8_t length = 0;
    char text[256]{};
    const size_t expected = std::strlen(name);
    return expected < sizeof(text) && read(address, length) && length == expected &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + 1),
                          text, length, nullptr) && std::memcmp(text, name, length) == 0;
}

inline uintptr_t parentClass(uintptr_t vmt) {
    uintptr_t parent = 0, result = 0;
    return read(vmt - 120, parent) && read(parent, result) ? result : 0;
}

inline bool inheritsFrom(uintptr_t object, const char* targetClass) {
    uintptr_t vmt = 0, self = 0;
    if (!read(object, vmt) || vmt < 200 || !read(vmt - 200, self) || self != vmt) return false;
    for (int depth = 0; vmt >= 200 && depth < 64; ++depth, vmt = parentClass(vmt)) {
        uintptr_t namePtr = 0;
        if (read(vmt - 136, namePtr) && namePtr && named(namePtr, targetClass)) return true;
    }
    return false;
}

inline uintptr_t dynamicMethod(uintptr_t object, uint16_t selector) {
    uintptr_t vmt = 0, self = 0;
    if (!read(object, vmt) || vmt < 200 || !read(vmt - 200, self) || self != vmt) return 0;
    for (int depth = 0; vmt >= 200 && depth < 64; ++depth, vmt = parentClass(vmt)) {
        uintptr_t table = 0;
        if (!read(vmt - 144, table)) return 0;
        if (!table) continue;
        uint16_t count = 0;
        if (!read(table, count) || count > 4096) return 0;
        const uintptr_t ids = table + sizeof(count);
        const uintptr_t procs = ids + uintptr_t(count) * sizeof(uint16_t);
        for (uint16_t i = 0; i < count; ++i) {
            uint16_t id = 0;
            if (!read(ids + uintptr_t(i) * sizeof(uint16_t), id)) return 0;
            if (id == selector) {
                uintptr_t proc = 0;
                return read(procs + uintptr_t(i) * sizeof(uintptr_t), proc) ? proc : 0;
            }
        }
    }
    return 0;
}

inline uintptr_t publishedField(uintptr_t object, const char* fieldName) {
    uintptr_t vmt = 0, self = 0;
    uint32_t instSize = 0;
    if (!read(object, vmt) || vmt < 200 || !read(vmt - 200, self) || self != vmt ||
        !read(vmt - 128, instSize) || instSize < sizeof(uintptr_t) || instSize > 16 * 1024 * 1024)
        return 0;
    for (int depth = 0; vmt >= 200 && depth < 64; ++depth, vmt = parentClass(vmt)) {
        uintptr_t table = 0;
        if (!read(vmt - 160, table)) return 0;
        if (!table) continue;
        uint16_t count = 0;
        if (!read(table, count) || count > 4096) return 0;
        uintptr_t entry = table + sizeof(count) + sizeof(uintptr_t);
        for (uint16_t i = 0; i < count; ++i) {
            uint32_t offset = 0;
            uint8_t length = 0;
            if (!read(entry, offset) || !read(entry + 6, length)) return 0;
            if (named(entry + 6, fieldName)) {
                uintptr_t result = 0;
                return offset <= instSize - sizeof(result) && read(object + offset, result) ? result : 0;
            }
            entry += 7 + length;
        }
    }
    return 0;
}

inline uintptr_t fromHwnd(HWND hwnd) {
    uintptr_t found = 0;
    EnumPropsExW(hwnd, +[](HWND, LPWSTR name, HANDLE value, ULONG_PTR ctx) -> int {
        if (!IS_INTRESOURCE(name) && std::wcsncmp(name, L"ControlOfs", 10) == 0) {
            *reinterpret_cast<uintptr_t*>(ctx) = reinterpret_cast<uintptr_t>(value);
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

struct KeyMessage {
    uint32_t msg;
    uint32_t msgFiller;
    uint16_t charCode;
    uint16_t unused[3];
    uint32_t keyData;
    uint32_t keyFiller;
    intptr_t result;
};
static_assert(sizeof(KeyMessage) == 32, "Delphi Win64 TWMKey layout");

inline bool invokeMenuShortCut(uintptr_t menu, UINT vk, LPARAM data) {
    if (!menu || !inheritsFrom(menu, "TMenu")) return false;
    const uintptr_t fn = dynamicMethod(menu, 0xFFEF /* TMenu.IsShortCut */);
    if (!fn) return false;
    KeyMessage msg{};
    msg.msg = 0xBD00; // CN_KEYDOWN
    msg.charCode = static_cast<uint16_t>(vk);
    msg.keyData = static_cast<uint32_t>(data);
    using Fn = uint8_t(__fastcall*)(void*, KeyMessage*);
    return reinterpret_cast<Fn>(fn)(reinterpret_cast<void*>(menu), &msg) != 0;
}

inline bool invokeFormShortCut(uintptr_t form, UINT vk, LPARAM data) {
    if (!form || !inheritsFrom(form, "TCustomForm")) return false;
    const uintptr_t fn = dynamicMethod(form, 0xFFA2 /* TCustomForm.IsShortCut */);
    if (!fn) return false;
    KeyMessage msg{};
    msg.msg = 0xBD00;
    msg.charCode = static_cast<uint16_t>(vk);
    msg.keyData = static_cast<uint32_t>(data);
    using Fn = uint8_t(__fastcall*)(void*, KeyMessage*);
    return reinterpret_cast<Fn>(fn)(reinterpret_cast<void*>(form), &msg) != 0;
}

// Win32 VCL skips TQuickPopupMenu.IsShortCut because custom-drawn menus have WindowHandle == 0.
inline bool dispatch(HWND hwnd, UINT vk, LPARAM data) {
    uintptr_t component = fromHwnd(hwnd);
    if (!component) return false;
    std::array<uintptr_t, 16> seenMenus{};
    size_t menuCount = 0;
    auto tryMenu = [&](uintptr_t menu) -> bool {
        if (!menu) return false;
        for (size_t i = 0; i < menuCount; ++i)
            if (seenMenus[i] == menu) return false;
        if (menuCount < seenMenus.size()) seenMenus[menuCount++] = menu;
        return invokeMenuShortCut(menu, vk, data);
    };

    for (int depth = 0; component && depth < 12; ++depth) {
        if (inheritsFrom(component, "TControl")) {
            if (const uintptr_t fnPopup = dynamicMethod(component, 0xFFD7 /* TControl.GetPopupMenu */)) {
                using FnGetPopup = uintptr_t(__fastcall*)(void*);
                if (tryMenu(reinterpret_cast<FnGetPopup>(fnPopup)(reinterpret_cast<void*>(component))))
                    return true;
            }
        }
        if (tryMenu(publishedField(component, "WAVOptionsMenu")) ||
            tryMenu(publishedField(component, "EnvOptionsMenu")))
            return true;
        if (invokeFormShortCut(component, vk, data)) return true;
        uintptr_t owner = 0;
        if (!read(component + sizeof(uintptr_t), owner) || owner == component) break;
        component = owner;
    }
    return false;
}

} // namespace vclShortcut

class Win32EditorKeyboard {
public:
    static UINT virtualKey(Steinberg::char16 key, Steinberg::int16 keyCode) {
        using namespace Steinberg;
        static constexpr UINT keys[] = {
            0, VK_BACK, VK_TAB, VK_CLEAR, VK_RETURN, VK_PAUSE, VK_ESCAPE, VK_SPACE,
            VK_NEXT, VK_END, VK_HOME, VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN, VK_PRIOR, VK_NEXT,
            VK_SELECT, VK_PRINT, VK_RETURN, VK_SNAPSHOT, VK_INSERT, VK_DELETE, VK_HELP,
            VK_NUMPAD0, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4,
            VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9,
            VK_MULTIPLY, VK_ADD, VK_SEPARATOR, VK_SUBTRACT, VK_DECIMAL, VK_DIVIDE,
            VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
            VK_NUMLOCK, VK_SCROLL, VK_SHIFT, VK_CONTROL, VK_MENU, VK_OEM_PLUS, VK_APPS,
            VK_MEDIA_PLAY_PAUSE, VK_MEDIA_STOP, VK_MEDIA_PREV_TRACK, VK_MEDIA_NEXT_TRACK,
            VK_VOLUME_UP, VK_VOLUME_DOWN, VK_F13, VK_F14, VK_F15, VK_F16, VK_F17, VK_F18,
            VK_F19, VK_F20, VK_F21, VK_F22, VK_F23, VK_F24, VK_LWIN
        };
        static_assert(sizeof(keys) / sizeof(keys[0]) == KEY_SUPER + 1);
        if (keyCode >= VKEY_FIRST_CODE && keyCode <= VKEY_LAST_CODE) return keys[keyCode];
        if (keyCode >= VKEY_FIRST_ASCII && keyCode <= VKEY_FIRST_ASCII + 'Z' - '0') {
            const auto character = VirtualKeyCodeToChar(static_cast<uint8>(keyCode));
            if ((character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z'))
                return static_cast<UINT>(character);
        }
        if (!key) return 0;
        if (key <= 26) return 'A' + key - 1;
        const SHORT mapped = VkKeyScanW(static_cast<WCHAR>(key));
        return mapped == -1 ? 0 : LOBYTE(mapped);
    }

    bool keyDown(HWND container, Steinberg::char16 key, Steinberg::int16 keyCode,
                 Steinberg::int16 modifiers, bool wantsKeyboard = false,
                 HWND fallbackTarget = nullptr) {
        return keyDownVk(container, virtualKey(key, keyCode), key, keyCode, modifiers,
                         wantsKeyboard, fallbackTarget);
    }

    bool keyDownVk(HWND container, UINT vk, Steinberg::char16 key, Steinberg::int16 keyCode,
                   Steinberg::int16 modifiers, bool wantsKeyboard = false,
                   HWND fallbackTarget = nullptr) {
        if (!container || vk >= m_targets.size()) return false;
        HWND focused = GetFocus();
        if (!focused || !IsChild(container, focused)) {
            if (fallbackTarget && IsChild(container, fallbackTarget)) focused = fallbackTarget;
            else return false;
        }
        if (!vk || (modifiers & Steinberg::kControlKey)) return false;
        Modifiers state(modifiers);
        if (!state.applied) return false;
        const bool system = (modifiers & Steinberg::kAlternateKey) != 0;
        const UINT message = system ? WM_SYSKEYDOWN : WM_KEYDOWN;
        const LPARAM data = keyData(vk, keyCode, modifiers) |
            (m_targets[vk].window ? LPARAM(1) << 30 : 0);
        const bool ctrlPlain = (modifiers & Steinberg::kCommandKey) &&
            !(modifiers & (Steinberg::kAlternateKey | Steinberg::kShiftKey));
        if (ctrlPlain && (vk == 'A' || vk == 'C' || vk == 'V' || vk == 'X' || vk == 'Z' || vk == 'Y') &&
            (SendMessageW(focused, WM_GETDLGCODE, vk, 0) & DLGC_HASSETSEL)) {
            // Edit and RichEdit disagree about handling Ctrl keys in WM_KEYDOWN versus WM_CHAR.
            if (vk == 'A') SendMessageW(focused, EM_SETSEL, 0, -1);
            else if (vk == 'Z') SendMessageW(focused, WM_UNDO, 0, 0);
            else if (vk == 'Y') SendMessageW(focused, WM_USER + 84 /* EM_REDO */, 0, 0);
            else SendMessageW(focused, vk == 'C' ? WM_COPY : vk == 'X' ? WM_CUT : WM_PASTE, 0, 0);
            m_targets[vk] = {focused, false, system};
            return true;
        }
        const bool maybeShortcut = (modifiers & (Steinberg::kCommandKey | Steinberg::kAlternateKey)) != 0 ||
            vk == VK_DELETE || vk == VK_INSERT || (vk >= VK_F1 && vk <= VK_F24);
        if (maybeShortcut) {
            // Embedded VCL forms do not have their own application message loop, and Win32 VCL
            // skips TQuickPopupMenu.IsShortCut because custom popup menus lack an HMENU.
            for (HWND target = focused; target && target != container; target = GetParent(target)) {
                if (SendMessageW(target, 0xb044 /* CM_ISSHORTCUT */, vk, data) ||
                    vclShortcut::dispatch(target, vk, data)) {
                    m_targets[vk] = {target, false, system};
                    return true;
                }
            }
        }
        // VCL's reflected key message runs native shortcut processing and reports consumption.
        // Edison puts this handler on the wrapper above its focused audio editor.
        for (HWND target = focused; target && target != container; target = GetParent(target)) {
            if (SendMessageW(target, kNativeBase + message, vk, data)) {
                m_targets[vk] = {target, false, system};
                return true;
            }
        }
        // Raw input belongs to the editor only while it requests keyboard ownership.
        if (wantsKeyboard) {
            m_targets[vk] = {focused, true, system};
            SendMessageW(focused, message, vk, data);
            if (IsChild(container, focused) && key &&
                !(modifiers & (Steinberg::kCommandKey | Steinberg::kAlternateKey)) &&
                !SendMessageW(focused, kNativeBase + WM_CHAR, key, data))
                SendMessageW(focused, WM_CHAR, key, data);
            return true;
        }
        return m_targets[vk].window != nullptr;
    }

    bool keyUp(HWND container, Steinberg::char16 key, Steinberg::int16 keyCode,
               Steinberg::int16 modifiers) {
        return keyUpVk(container, virtualKey(key, keyCode), keyCode, modifiers);
    }

    bool keyUpVk(HWND container, UINT vk, Steinberg::int16 keyCode,
                 Steinberg::int16 modifiers) {
        if (!vk || vk >= m_targets.size()) return false;
        const auto target = m_targets[vk];
        m_targets[vk] = {};
        if (!container || !target.window || !IsChild(container, target.window)) return false;
        Modifiers state(modifiers);
        if (!state.applied) return false;
        const UINT message = target.system ? WM_SYSKEYUP : WM_KEYUP;
        const LPARAM data = keyData(vk, keyCode, modifiers) | (LPARAM(1) << 30) | (LPARAM(1) << 31);
        if (!SendMessageW(target.window, kNativeBase + message, vk, data) && target.raw &&
            IsChild(container, target.window))
            SendMessageW(target.window, message, vk, data);
        return true;
    }

    void reset() { m_targets.fill({}); }

private:
    static constexpr UINT kNativeBase = 0xbc00;

    static LPARAM keyData(UINT vk, Steinberg::int16 keyCode, Steinberg::int16 modifiers) {
        const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
        const bool extended = (scan & 0xff00) || keyCode == Steinberg::KEY_ENTER ||
            vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
            vk == VK_PRIOR || vk == VK_NEXT || vk == VK_LEFT || vk == VK_RIGHT ||
            vk == VK_UP || vk == VK_DOWN || vk == VK_DIVIDE || vk == VK_NUMLOCK ||
            vk == VK_SNAPSHOT || vk == VK_LWIN || vk == VK_APPS;
        return 1 | (LPARAM(scan & 0xff) << 16) |
            (extended ? LPARAM(1) << 24 : 0) |
            (modifiers & Steinberg::kAlternateKey ? LPARAM(1) << 29 : 0);
    }

    struct Modifiers {
        std::array<BYTE, 256> previous{};
        bool applied = false;

        explicit Modifiers(Steinberg::int16 modifiers) {
            if (!GetKeyboardState(previous.data())) {
                std::fprintf(stderr, "FloopHole: could not read editor keyboard state (%lu)\n", GetLastError());
                return;
            }
            auto state = previous;
            const auto set = [&](int key, bool pressed) {
                state[key] = (state[key] & 0x7f) | (pressed ? 0x80 : 0);
            };
            const bool shift = (modifiers & Steinberg::kShiftKey) != 0;
            const bool control = (modifiers & Steinberg::kCommandKey) != 0;
            const bool alt = (modifiers & Steinberg::kAlternateKey) != 0;
            set(VK_SHIFT, shift); set(VK_LSHIFT, shift); set(VK_RSHIFT, false);
            set(VK_CONTROL, control); set(VK_LCONTROL, control); set(VK_RCONTROL, false);
            set(VK_MENU, alt); set(VK_LMENU, alt); set(VK_RMENU, false);
            applied = SetKeyboardState(state.data()) != FALSE;
            if (!applied)
                std::fprintf(stderr, "FloopHole: could not set editor keyboard modifiers (%lu)\n", GetLastError());
        }

        ~Modifiers() {
            if (applied && !SetKeyboardState(previous.data()))
                std::fprintf(stderr, "FloopHole: could not restore editor keyboard state (%lu)\n", GetLastError());
        }
    };

    struct Target {
        HWND window = nullptr;
        bool raw = false, system = false;
    };
    std::array<Target, 256> m_targets{};
};

} // namespace flbr
