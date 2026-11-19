#pragma once

#import <Cocoa/Cocoa.h>
#include "ToolbarTheme.h"
#include <functional>

namespace flbr {
inline NSColor* themeColor(uint32_t rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.0
        green:((rgb >> 8) & 255) / 255.0 blue:(rgb & 255) / 255.0 alpha:1];
}
}

@interface FloopHoleLogoView : NSControl
- (void)showAbout:(id)sender;
@end

@interface FloopHoleButton : NSButton
@property(nonatomic) int icon; // 0 previous, 1 next, 2 folder, 3 diskette
@end

@interface FloopHolePresetPicker : NSControl {
@public
    std::function<void(int)> onSelect;
}
@property(nonatomic, copy) NSString* displayTitle;
- (void)setPresets:(const std::vector<flbr::FLPresetEntry>&)presets selected:(int)selected;
- (void)closeMenu;
@end
