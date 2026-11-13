#pragma once

#include "FLPluginScanner.h"

namespace flbr {

namespace ToolbarTheme {
constexpr uint32_t background = 0x2F323A;
constexpr uint32_t controlTop = 0x384248;
constexpr uint32_t controlBottom = 0x272F34;
constexpr uint32_t hover = 0x46545D;
constexpr uint32_t border = 0x171E23;
constexpr uint32_t text = 0xDDE3E7;
constexpr uint32_t muted = 0xA6B0B5;
constexpr uint32_t menu = 0x1D2529;
constexpr uint32_t selected = 0x607D8B;
constexpr int radius = 3;
constexpr int menuRowHeight = 24;
constexpr int maxVisibleRows = 16;
}

struct PresetMenuItem {
    std::string text;
    int preset = -1;
};

inline std::vector<PresetMenuItem> presetMenuItems(const std::vector<FLPresetEntry>& presets) {
    std::vector<PresetMenuItem> result;
    std::string category;
    for (size_t i = 0; i < presets.size(); ++i) {
        if (!presets[i].category.empty() && presets[i].category != category) {
            category = presets[i].category;
            result.push_back({category, -1});
        }
        const auto& name = presets[i].name.empty() ? presets[i].displayName : presets[i].name;
        result.push_back({name, static_cast<int>(i)});
    }
    if (result.empty()) result.push_back({"Default / Internal Patch", -1});
    return result;
}

}
