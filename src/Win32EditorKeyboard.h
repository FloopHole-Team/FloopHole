#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "pluginterfaces/base/keycodes.h"
#include <array>
#include <cstdio>

namespace flbr {

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
                 Steinberg::int16 modifiers, bool wantsKeyboard = false) {
        HWND focused = GetFocus();
        const UINT vk = virtualKey(key, keyCode);
        if (!container || !focused || !IsChild(container, focused) || !vk ||
            (modifiers & Steinberg::kControlKey)) return false;
        Modifiers state(modifiers);
        if (!state.applied) return false;
        const bool system = (modifiers & Steinberg::kAlternateKey) != 0;
        const UINT message = system ? WM_SYSKEYDOWN : WM_KEYDOWN;
        const LPARAM data = keyData(vk, keyCode, modifiers) |
            (m_targets[vk].window ? LPARAM(1) << 30 : 0);
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
        const UINT vk = virtualKey(key, keyCode);
        if (!vk) return false;
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
