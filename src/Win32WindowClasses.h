#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <vector>

namespace flbr {

class Win32WindowClasses {
public:
    struct Definition {
        const wchar_t* name;
        WNDPROC procedure;
        UINT style;
        HCURSOR cursor;
    };

    explicit Win32WindowClasses(std::initializer_list<Definition> definitions) : m_definitions(definitions) {}

    HINSTANCE acquire() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_users) {
            ++m_users;
            return m_module;
        }
        if (m_definitions.empty() || !GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(m_definitions.front().procedure), &m_module)) {
            fprintf(stderr, "FloopHole: cannot resolve window-class module (%lu)\n", GetLastError());
            return nullptr;
        }
        std::vector<const wchar_t*> added;
        for (const auto& definition : m_definitions) {
            WNDCLASSEXW type{};
            type.cbSize = sizeof(type);
            type.hInstance = m_module;
            type.lpfnWndProc = definition.procedure;
            type.lpszClassName = definition.name;
            type.style = definition.style;
            type.hCursor = definition.cursor;
            if (RegisterClassExW(&type)) {
                added.push_back(definition.name);
                continue;
            }
            const DWORD error = GetLastError();
            WNDCLASSEXW existing{};
            existing.cbSize = sizeof(existing);
            if (error == ERROR_CLASS_ALREADY_EXISTS && GetClassInfoExW(m_module, definition.name, &existing) &&
                existing.lpfnWndProc == definition.procedure && existing.hInstance == m_module) continue;
            fprintf(stderr, "FloopHole: window-class registration failed (%lu)\n", error);
            for (const auto* name : added) {
                if (!UnregisterClassW(name, m_module))
                    fprintf(stderr, "FloopHole: window-class rollback failed (%lu)\n", GetLastError());
            }
            m_module = nullptr;
            return nullptr;
        }
        m_users = 1;
        return m_module;
    }

    void release() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_users) {
            fprintf(stderr, "FloopHole: unbalanced window-class release\n");
            return;
        }
        if (--m_users) return;
        for (const auto& definition : m_definitions) {
            if (!UnregisterClassW(definition.name, m_module))
                fprintf(stderr, "FloopHole: window-class cleanup failed (%lu)\n", GetLastError());
        }
        m_module = nullptr;
    }

private:
    const std::vector<Definition> m_definitions;
    std::mutex m_mutex;
    HINSTANCE m_module = nullptr;
    size_t m_users = 0;
};

}
