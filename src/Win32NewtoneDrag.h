#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace flbr {
namespace newtoneDrag {

template<class T>
inline bool read(uintptr_t address, T& value) {
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                                        &value, sizeof(value), nullptr);
}

inline bool named(uintptr_t address, const char* name) {
    uint8_t length = 0;
    char text[256]{};
    return read(address, length) && length == std::strlen(name) &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + 1),
                          text, length, nullptr) && std::memcmp(text, name, length) == 0;
}

// Delphi Win64 VMT and published RTTI layouts, not offsets into a particular Newtone build.
inline bool classInfo(uintptr_t object, uintptr_t& vmt, uint32_t& size) {
    uintptr_t self = 0;
    return read(object, vmt) && vmt >= 200 && read(vmt - 200, self) && self == vmt &&
        read(vmt - 128, size) && size >= sizeof(uintptr_t) && size <= 16 * 1024 * 1024;
}

inline uintptr_t parentClass(uintptr_t vmt) {
    uintptr_t parent = 0;
    uintptr_t result = 0;
    return read(vmt - 120, parent) && read(parent, result) ? result : 0;
}

inline uintptr_t field(uintptr_t object, const char* name) {
    uintptr_t vmt = 0;
    uint32_t size = 0;
    if (!classInfo(object, vmt, size)) return 0;
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
            if (named(entry + 6, name)) {
                uintptr_t result = 0;
                return size >= sizeof(result) && offset <= size - sizeof(result) &&
                    read(object + offset, result) ? result : 0;
            }
            entry += 7 + length;
        }
    }
    return 0;
}

#pragma pack(push, 1)
struct Property {
    uintptr_t type, getter, setter, stored;
    int32_t index, defaultValue;
    uint16_t nameIndex;
    uint8_t nameLength;
};
#pragma pack(pop)

inline bool saveBeforeDrag(uintptr_t source) {
    // CF_HDROP-only targets cannot request FL Studio's deferred file rendering.
    uintptr_t vmt = 0;
    uint32_t size = 0;
    if (!classInfo(source, vmt, size)) return false;
    for (int depth = 0; vmt >= 200 && depth < 64; ++depth, vmt = parentClass(vmt)) {
        uintptr_t type = 0;
        if (!read(vmt - 168, type)) return false;
        if (!type) continue;
        uint8_t kind = 0, nameLength = 0, unitLength = 0;
        if (!read(type, kind) || kind != 7 || !read(type + 1, nameLength)) return false;
        const uintptr_t unit = type + 2 + nameLength + 2 * sizeof(uintptr_t) + sizeof(uint16_t);
        if (!read(unit, unitLength)) return false;
        uintptr_t entry = unit + 1 + unitLength;
        uint16_t count = 0;
        if (!read(entry, count) || count > 4096) return false;
        entry += sizeof(count);
        for (uint16_t i = 0; i < count; ++i) {
            Property property{};
            if (!read(entry, property)) return false;
            if (named(entry + offsetof(Property, nameLength), "SaveFileBeforeDrag")) {
                uintptr_t propertyType = 0;
                uint8_t propertyKind = 0;
                if (!read(property.type, propertyType) || !read(propertyType, propertyKind) ||
                    propertyKind != 3 || !named(propertyType + 1, "Boolean") ||
                    (property.setter >> 56) != 0xff) return false;
                const uintptr_t offset = property.setter & 0x00ffffffffffffffULL;
                if (offset >= size) return false;
                const uint8_t enabled = 1;
                return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(source + offset),
                                          &enabled, sizeof(enabled), nullptr) != FALSE;
            }
            entry += sizeof(Property) + property.nameLength;
        }
    }
    return false;
}

inline bool enable(HWND editor) {
    bool enabled = false;
    EnumPropsExW(editor, +[](HWND, LPWSTR name, HANDLE value, ULONG_PTR context) -> int {
        if (IS_INTRESOURCE(name) || std::wcsncmp(name, L"ControlOfs", 10) != 0) return TRUE;
        // The editor HWND can belong to a panel owned by the form with the drag source.
        uintptr_t component = reinterpret_cast<uintptr_t>(value);
        for (int depth = 0; component && depth < 64; ++depth) {
            if (const auto source = field(component, "DragWAV")) {
                *reinterpret_cast<bool*>(context) = saveBeforeDrag(source);
                return FALSE;
            }
            uintptr_t owner = 0;
            if (!read(component + sizeof(uintptr_t), owner) || owner == component) break;
            component = owner;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&enabled));
    return enabled;
}

} // namespace newtoneDrag
} // namespace flbr
