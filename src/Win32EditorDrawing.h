#pragma once

#include "Win32WindowClasses.h"
#include <objidl.h>
#include <gdiplus.h>
#include "ToolbarTheme.h"

namespace flbr {

inline Gdiplus::StringFormat editorStringFormat() {
    // MinGW's Generic* format wrappers become dangling after GdiplusShutdown.
    return Gdiplus::StringFormat(Gdiplus::StringFormatFlagsNoWrap);
}

inline void drawEditorResizeGrip(HDC dc, const RECT& bounds) {
    const auto rgb = [](uint32_t value) { return RGB((value >> 16) & 255, (value >> 8) & 255, value & 255); };
    const COLORREF oldColor = SetDCBrushColor(dc, rgb(ToolbarTheme::background));
    FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetDCBrushColor(dc, oldColor);
    HPEN pen = CreatePen(PS_SOLID, 1, rgb(ToolbarTheme::muted));
    if (!pen) {
        fprintf(stderr, "FloopHole: cannot create resize-grip pen (%lu)\n", GetLastError());
        return;
    }
    HGDIOBJ previous = SelectObject(dc, pen);
    if (!previous || previous == HGDI_ERROR) {
        fprintf(stderr, "FloopHole: cannot select resize-grip pen (%lu)\n", GetLastError());
        DeleteObject(pen);
        return;
    }
    for (int i = 0; i < 3; ++i) {
        MoveToEx(dc, bounds.right - 10 + i * 3, bounds.bottom - 2, nullptr);
        LineTo(dc, bounds.right - 2, bounds.bottom - 10 + i * 3);
    }
    SelectObject(dc, previous);
    DeleteObject(pen);
}

}
