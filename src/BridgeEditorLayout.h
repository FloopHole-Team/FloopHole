#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace flbr {

constexpr int kBridgeResizeFooterHeight = 14;
constexpr size_t kBridgeToolbarControlCount = 8;
constexpr size_t kBridgeLoadPresetControl = 4;
constexpr size_t kBridgeSavePresetControl = 5;
constexpr size_t kBridgeHintControl = 6;
constexpr size_t kBridgeMasterVolumeControl = 7;

struct BridgeToolbarLayout {
    struct Rect { int x, y, width, height; };
    std::array<Rect, kBridgeToolbarControlCount> controls{};
    int height = 54;

    static BridgeToolbarLayout compute(int width) {
        width = std::max(1, width);
        const int padding = std::min(6, (width - 1) / 2);
        const int available = width - padding * 2;
        constexpr std::array<int, kBridgeMasterVolumeControl> minimum{38, 180, 28, 28, 34, 34, 200};
        constexpr std::array<bool, kBridgeMasterVolumeControl> flexible{false, true, false, false, false, false, true};
        BridgeToolbarLayout layout;
        const int knobWidth = std::min(40, available);
        layout.controls[kBridgeMasterVolumeControl] = {width - padding - knobWidth, 8, knobWidth, 38};
        const int firstRowAvailable = available - knobWidth - 6;
        size_t first = 0;
        int row = firstRowAvailable > 0 ? 0 : 1;
        while (first < minimum.size()) {
            const int rowAvailable = row == 0 ? firstRowAvailable : available;
            size_t last = first;
            int used = 0, growing = 0;
            while (last < minimum.size()) {
                const int itemWidth = std::min(minimum[last], rowAvailable);
                const int next = used + (last > first ? 6 : 0) + itemWidth;
                if (last > first && next > rowAvailable) break;
                if (last == kBridgeLoadPresetControl && last > first &&
                    available >= minimum[last] + 6 + minimum[kBridgeSavePresetControl] &&
                    next + 6 + minimum[kBridgeSavePresetControl] > rowAvailable) break;
                used = next;
                growing += flexible[last] ? 1 : 0;
                ++last;
            }
            int extra = rowAvailable - used;
            int x = padding;
            for (size_t i = first; i < last; ++i) {
                int itemWidth = std::min(minimum[i], rowAvailable);
                if (flexible[i]) {
                    const int added = extra / growing;
                    itemWidth += added;
                    extra -= added;
                    --growing;
                }
                const bool logo = i == 0 && row == 0;
                layout.controls[i] = {x, logo ? 8 : row == 0 ? 14 : 52 + (row - 1) * 32,
                    itemWidth, logo ? 38 : 26};
                x += itemWidth + 6;
            }
            ++row;
            first = last;
        }
        layout.height = 54 + (row - 1) * 32;
        return layout;
    }
};

inline int bridgeEditorHeight(int width, int nativeHeight, bool resizable) {
    return nativeHeight + BridgeToolbarLayout::compute(width).height +
        (resizable ? kBridgeResizeFooterHeight : 0);
}

}
