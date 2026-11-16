#import "ToolbarControls_mac.h"
#include "LogoRenderer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace flbr;

static void drawSurface(NSRect bounds, BOOL hover, BOOL pressed) {
    NSBezierPath* path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(bounds, 0.5, 0.5)
        xRadius:ToolbarTheme::radius yRadius:ToolbarTheme::radius];
    NSGradient* gradient = [[NSGradient alloc] initWithStartingColor:
        themeColor(hover ? ToolbarTheme::hover : ToolbarTheme::controlTop)
        endingColor:themeColor(pressed ? ToolbarTheme::border : ToolbarTheme::controlBottom)];
    [gradient drawInBezierPath:path angle:270];
    [themeColor(ToolbarTheme::border) setStroke];
    [path stroke];
}

@implementation FloopHoleLogoView {
    LogoRaster _raster;
}
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.toolTip = @"FloopHole";
        self.accessibilityElement = YES;
        self.accessibilityRole = NSAccessibilityImageRole;
        self.accessibilityLabel = @"FloopHole";
    }
    return self;
}
- (void)viewDidChangeBackingProperties { [self setNeedsDisplay:YES]; }
- (void)drawRect:(NSRect)dirty {
    const CGFloat size = std::min(self.bounds.size.width, self.bounds.size.height);
    const int pixels = std::max(1, static_cast<int>(std::lround(size * self.window.backingScaleFactor)));
    if (_raster.size != pixels) _raster = renderLogo(pixels);
    if (_raster.pixels.empty()) return;
    CGDataProviderRef data = CGDataProviderCreateWithData(nullptr, _raster.pixels.data(), _raster.pixels.size(), nullptr);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image = CGImageCreate(pixels, pixels, 8, 32, _raster.stride, space,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, data, nullptr, false, kCGRenderingIntentDefault);
    if (image) {
        NSImage* logo = [[NSImage alloc] initWithCGImage:image size:NSMakeSize(size, size)];
        [logo drawInRect:NSMakeRect((self.bounds.size.width - size) / 2,
            (self.bounds.size.height - size) / 2, size, size)];
        CGImageRelease(image);
    }
    CGColorSpaceRelease(space);
    CGDataProviderRelease(data);
}
@end

@implementation FloopHoleButton {
    BOOL _hover;
    NSTrackingArea* _tracking;
}
- (BOOL)isFlipped { return NO; }
- (void)updateTrackingAreas {
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseEnteredAndExited |
        NSTrackingActiveInActiveApp | NSTrackingInVisibleRect owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}
- (void)mouseEntered:(NSEvent*)event { _hover = YES; [self setNeedsDisplay:YES]; }
- (void)mouseExited:(NSEvent*)event { _hover = NO; [self setNeedsDisplay:YES]; }
- (void)drawRect:(NSRect)dirty {
    drawSurface(self.bounds, _hover, self.highlighted);
    [themeColor(self.enabled ? ToolbarTheme::text : ToolbarTheme::muted) set];
    const CGFloat x = NSMidX(self.bounds), y = NSMidY(self.bounds);
    NSBezierPath* icon = [NSBezierPath bezierPath];
    if (_icon == 2) {
        [icon moveToPoint:NSMakePoint(x - 6, y - 4)];
        [icon lineToPoint:NSMakePoint(x + 6, y - 4)];
        [icon lineToPoint:NSMakePoint(x + 6, y + 3)];
        [icon lineToPoint:NSMakePoint(x - 1, y + 3)];
        [icon lineToPoint:NSMakePoint(x - 3, y + 5)];
        [icon lineToPoint:NSMakePoint(x - 6, y + 5)];
        [icon closePath];
        icon.lineWidth = 1;
        [icon stroke];
    } else {
        const CGFloat direction = _icon == 0 ? -1 : 1;
        [icon moveToPoint:NSMakePoint(x + 3 * direction, y)];
        [icon lineToPoint:NSMakePoint(x - 2 * direction, y + 4)];
        [icon lineToPoint:NSMakePoint(x - 2 * direction, y - 4)];
        [icon closePath];
        [icon fill];
    }
    if (self.window.firstResponder == self) {
        [themeColor(ToolbarTheme::selected) setStroke];
        [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 1.5, 1.5) xRadius:3 yRadius:3] stroke];
    }
}
@end

@class FloopHoleMenuView;
@interface FloopHolePresetPicker () <NSWindowDelegate>
@property(nonatomic, strong) NSPanel* panel;
@property(nonatomic, strong) FloopHoleMenuView* menuView;
@property(nonatomic, strong) id monitor;
@property(nonatomic, strong) NSArray<NSDictionary*>* items;
- (void)chooseRow:(NSInteger)row;
- (NSEvent*)filterMenuMouseDown:(NSEvent*)event;
@end

@interface FloopHoleMenuView : NSView
@property(nonatomic, weak) FloopHolePresetPicker* picker;
@property(nonatomic) NSInteger firstRow;
@property(nonatomic) NSInteger highlightedRow;
@property(nonatomic) NSInteger visibleRows;
@end

@implementation FloopHoleMenuView {
    NSTrackingArea* _tracking;
    CGFloat _scrollDelta;
    NSString* _searchQuery;
    NSTimeInterval _lastSearchTime;
}
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (void)updateTrackingAreas {
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseMoved |
        NSTrackingActiveInActiveApp | NSTrackingInVisibleRect owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}
- (void)drawRect:(NSRect)dirty {
    NSBezierPath* outline = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 0.5, 0.5)
        xRadius:3 yRadius:3];
    [themeColor(ToolbarTheme::menu) setFill];
    [outline fill];
    [NSGraphicsContext saveGraphicsState];
    [outline addClip];
    const auto items = self.picker.items;
    for (NSInteger row = _firstRow; row < MIN((NSInteger)items.count, _firstRow + _visibleRows); ++row) {
        NSRect rect = NSMakeRect(1, 1 + (row - _firstRow) * ToolbarTheme::menuRowHeight,
            self.bounds.size.width - 2, ToolbarTheme::menuRowHeight);
        const BOOL enabled = [items[row][@"preset"] intValue] >= 0;
        if (enabled && row == _highlightedRow) {
            [themeColor(ToolbarTheme::selected) setFill];
            NSRectFill(rect);
        }
        NSDictionary* attributes = @{NSFontAttributeName:[NSFont systemFontOfSize:12],
            NSForegroundColorAttributeName:themeColor(enabled ? ToolbarTheme::text : ToolbarTheme::muted),
            NSParagraphStyleAttributeName:[NSParagraphStyle defaultParagraphStyle]};
        [items[row][@"text"] drawInRect:NSInsetRect(rect, 9, 4) withAttributes:attributes];
    }
    if (items.count > static_cast<NSUInteger>(_visibleRows)) {
        const CGFloat height = self.bounds.size.height - 8;
        const CGFloat thumb = std::max<CGFloat>(18, height * _visibleRows / items.count);
        const CGFloat top = 4 + (height - thumb) * _firstRow / (items.count - _visibleRows);
        [themeColor(ToolbarTheme::muted) setFill];
        [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(self.bounds.size.width - 5, top, 3, thumb)
            xRadius:1.5 yRadius:1.5] fill];
    }
    [NSGraphicsContext restoreGraphicsState];
    [themeColor(ToolbarTheme::border) setStroke];
    [outline stroke];
}
- (NSInteger)rowAtEvent:(NSEvent*)event {
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    if (!NSPointInRect(point, self.bounds)) return -1;
    const NSInteger row = _firstRow + static_cast<NSInteger>((point.y - 1) / ToolbarTheme::menuRowHeight);
    return row >= 0 && row < (NSInteger)self.picker.items.count ? row : -1;
}
- (void)mouseMoved:(NSEvent*)event {
    self.highlightedRow = [self rowAtEvent:event];
    [self setNeedsDisplay:YES];
}
- (void)mouseUp:(NSEvent*)event {
    const NSInteger row = [self rowAtEvent:event];
    if (row >= 0) [self.picker chooseRow:row];
}
- (void)scrollWheel:(NSEvent*)event {
    _searchQuery = nil;
    NSInteger steps = 0;
    if (event.hasPreciseScrollingDeltas) {
        _scrollDelta += event.scrollingDeltaY;
        steps = static_cast<NSInteger>(_scrollDelta / ToolbarTheme::menuRowHeight);
        _scrollDelta -= steps * ToolbarTheme::menuRowHeight;
    } else steps = static_cast<NSInteger>(std::lround(event.scrollingDeltaY));
    _firstRow = std::clamp<NSInteger>(_firstRow - steps, 0, MAX(0, (NSInteger)self.picker.items.count - _visibleRows));
    [self setNeedsDisplay:YES];
}
- (void)highlightRow:(NSInteger)row {
    _highlightedRow = row;
    if (row < _firstRow) _firstRow = row;
    if (row >= _firstRow + _visibleRows) _firstRow = row - _visibleRows + 1;
    [self setNeedsDisplay:YES];
}
- (NSInteger)findMatch:(NSString*)query {
    if (query.length == 0) return -1;
    NSArray<NSDictionary*>* items = self.picker.items;
    for (int pass = 0; pass < 2; ++pass) {
        const NSStringCompareOptions options = NSCaseInsensitiveSearch | (pass == 0 ? NSAnchoredSearch : 0);
        for (NSInteger row = 0; row < (NSInteger)items.count; ++row) {
            if ([items[row][@"preset"] intValue] < 0) continue;
            if ([items[row][@"text"] rangeOfString:query options:options].location != NSNotFound) return row;
        }
    }
    return -1;
}
- (void)searchForCharacters:(NSString*)characters timestamp:(NSTimeInterval)now {
    if ([characters isEqualToString:@" "] && _searchQuery.length == 0) return;
    const BOOL expired = now - _lastSearchTime > 1.0 || now < _lastSearchTime;
    NSArray<NSDictionary*>* items = self.picker.items;
    if (!expired && _searchQuery.length == 1 && characters.length == 1 &&
        [_searchQuery compare:characters options:NSCaseInsensitiveSearch] == NSOrderedSame) {
        _lastSearchTime = now;
        for (NSInteger offset = 1; offset <= (NSInteger)items.count; ++offset) {
            const NSInteger row = (_highlightedRow + offset) % (NSInteger)items.count;
            if ([items[row][@"preset"] intValue] >= 0 &&
                [items[row][@"text"] rangeOfString:characters
                    options:NSCaseInsensitiveSearch | NSAnchoredSearch].location != NSNotFound) {
                [self highlightRow:row];
                return;
            }
        }
        return;
    }
    NSString* query = expired || _searchQuery.length == 0 ? characters : [_searchQuery stringByAppendingString:characters];
    const NSInteger row = [self findMatch:query];
    if (row >= 0) {
        _searchQuery = query;
        _lastSearchTime = now;
        [self highlightRow:row];
    } else if (expired) {
        _searchQuery = nil;
        _lastSearchTime = now;
    }
}
- (void)keyDown:(NSEvent*)event {
    if (event.keyCode == 53) { [self.picker closeMenu]; return; }
    if (event.keyCode == 36 || event.keyCode == 76) { [self.picker chooseRow:_highlightedRow]; return; }
    if (event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) {
        [super keyDown:event];
        return;
    }
    if (event.keyCode == 51 || event.keyCode == 117) {
        if (_searchQuery.length) {
            _searchQuery = [_searchQuery substringToIndex:
                [_searchQuery rangeOfComposedCharacterSequenceAtIndex:_searchQuery.length - 1].location];
            _lastSearchTime = event.timestamp;
            const NSInteger row = [self findMatch:_searchQuery];
            if (row >= 0) [self highlightRow:row];
        }
        return;
    }
    if (event.keyCode != 125 && event.keyCode != 126) {
        NSString* characters = event.characters;
        if (characters.length &&
            [characters rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location == NSNotFound &&
            !([characters characterAtIndex:0] >= NSUpArrowFunctionKey &&
              [characters characterAtIndex:0] <= NSModeSwitchFunctionKey)) {
            [self searchForCharacters:characters timestamp:event.timestamp];
            return;
        }
        [super keyDown:event];
        return;
    }
    _searchQuery = nil;
    const NSInteger direction = event.keyCode == 125 ? 1 : -1;
    NSInteger row = _highlightedRow;
    do { row += direction; }
    while (row >= 0 && row < (NSInteger)self.picker.items.count && [self.picker.items[row][@"preset"] intValue] < 0);
    if (row < 0 || row >= (NSInteger)self.picker.items.count) return;
    [self highlightRow:row];
}
@end

@interface FloopHoleMenuPanel : NSPanel
@end
@implementation FloopHoleMenuPanel
- (BOOL)canBecomeKeyWindow { return YES; }
@end

@implementation FloopHolePresetPicker {
    BOOL _hover;
    NSTrackingArea* _tracking;
    NSInteger _selectedRow;
}
- (BOOL)acceptsFirstResponder { return YES; }
- (void)setPresets:(const std::vector<FLPresetEntry>&)presets selected:(int)selected {
    [self closeMenu];
    NSMutableArray* choices = [NSMutableArray array];
    _selectedRow = -1;
    for (const auto& entry : presetMenuItems(presets)) {
        if (entry.preset == selected && selected >= 0) _selectedRow = choices.count;
        [choices addObject:@{@"text":[NSString stringWithUTF8String:entry.text.c_str()], @"preset":@(entry.preset)}];
    }
    self.items = choices;
    self.displayTitle = _selectedRow >= 0 ? self.items[_selectedRow][@"text"] : @"Default / Internal Patch";
    self.accessibilityRole = NSAccessibilityPopUpButtonRole;
    self.accessibilityLabel = @"Preset";
    self.accessibilityValue = self.displayTitle;
    self.toolTip = self.displayTitle;
    [self setNeedsDisplay:YES];
}
- (void)updateTrackingAreas {
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseEnteredAndExited |
        NSTrackingActiveInActiveApp | NSTrackingInVisibleRect owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}
- (void)mouseEntered:(NSEvent*)event { _hover = YES; [self setNeedsDisplay:YES]; }
- (void)mouseExited:(NSEvent*)event { _hover = NO; [self setNeedsDisplay:YES]; }
- (void)drawRect:(NSRect)dirty {
    drawSurface(self.bounds, _hover, _panel != nil);
    NSMutableParagraphStyle* paragraph = [NSMutableParagraphStyle new];
    paragraph.lineBreakMode = NSLineBreakByTruncatingTail;
    NSDictionary* attributes = @{NSFontAttributeName:[NSFont systemFontOfSize:12],
        NSForegroundColorAttributeName:themeColor(ToolbarTheme::text), NSParagraphStyleAttributeName:paragraph};
    [self.displayTitle drawInRect:NSMakeRect(9, (self.bounds.size.height - 15) / 2,
        std::max<CGFloat>(0, self.bounds.size.width - 30), 15) withAttributes:attributes];
    const CGFloat x = self.bounds.size.width - 12, y = NSMidY(self.bounds);
    NSBezierPath* arrow = [NSBezierPath bezierPath];
    [arrow moveToPoint:NSMakePoint(x - 4, y + 2)];
    [arrow lineToPoint:NSMakePoint(x + 4, y + 2)];
    [arrow lineToPoint:NSMakePoint(x, y - 2)];
    [arrow closePath];
    [themeColor(ToolbarTheme::muted) setFill];
    [arrow fill];
}
- (void)mouseDown:(NSEvent*)event { [self openMenu]; }
- (void)keyDown:(NSEvent*)event {
    if (event.keyCode == 49 || event.keyCode == 125 || event.keyCode == 126) [self openMenu];
    else [super keyDown:event];
}
- (void)openMenu {
    if (_panel) { [self closeMenu]; return; }
    if (!self.window || self.items.count == 0) return;
    CGFloat width = std::max<CGFloat>(self.bounds.size.width, 240);
    const NSRect screen = [self.window convertRectToScreen:[self convertRect:self.bounds toView:nil]];
    if (!self.window.screen) {
        fprintf(stderr, "FloopHole: cannot position the preset menu without a screen\n");
        return;
    }
    const NSRect visible = self.window.screen.visibleFrame;
    const NSInteger rows = std::min<NSInteger>({static_cast<NSInteger>(self.items.count), ToolbarTheme::maxVisibleRows,
        std::max<NSInteger>(1, (visible.size.height - 10) / ToolbarTheme::menuRowHeight)});
    width = std::min(width, visible.size.width);
    NSRect frame = NSMakeRect(std::clamp(screen.origin.x, NSMinX(visible), NSMaxX(visible) - width),
        NSMinY(screen) - rows * ToolbarTheme::menuRowHeight - 5, width, rows * ToolbarTheme::menuRowHeight + 2);
    if (NSMinY(frame) < NSMinY(visible)) frame.origin.y = NSMaxY(screen) + 3;
    frame.origin.y = std::clamp(frame.origin.y, NSMinY(visible), NSMaxY(visible) - frame.size.height);
    _panel = [[FloopHoleMenuPanel alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskBorderless
        backing:NSBackingStoreBuffered defer:NO];
    _panel.releasedWhenClosed = NO;
    _panel.opaque = NO;
    _panel.backgroundColor = NSColor.clearColor;
    _panel.hasShadow = YES;
    _panel.hidesOnDeactivate = YES;
    _panel.level = NSPopUpMenuWindowLevel;
    _panel.delegate = self;
    _menuView = [[FloopHoleMenuView alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width, frame.size.height)];
    _menuView.picker = self;
    _menuView.visibleRows = rows;
    _menuView.highlightedRow = _selectedRow;
    _menuView.firstRow = std::clamp<NSInteger>(_selectedRow - rows / 2, 0, MAX(0, (NSInteger)self.items.count - rows));
    _panel.contentView = _menuView;
    [self.window addChildWindow:_panel ordered:NSWindowAbove];
    [_panel makeKeyAndOrderFront:nil];
    [_panel makeFirstResponder:_menuView];
    __weak FloopHolePresetPicker* weakSelf = self;
    _monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown |
        NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown handler:^NSEvent*(NSEvent* event) {
            FloopHolePresetPicker* picker = weakSelf;
            return picker ? [picker filterMenuMouseDown:event] : event;
        }];
    [self setNeedsDisplay:YES];
}
- (NSEvent*)filterMenuMouseDown:(NSEvent*)event {
    if (_panel && event.window != _panel) {
        const BOOL toggle = event.type == NSEventTypeLeftMouseDown && event.window == self.window &&
            NSPointInRect([self convertPoint:event.locationInWindow fromView:nil], self.bounds);
        [self closeMenu];
        if (toggle) return nil;
    }
    return event;
}
- (void)chooseRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)self.items.count) return;
    const int preset = [self.items[row][@"preset"] intValue];
    if (preset < 0) return;
    [self closeMenu];
    if (onSelect) onSelect(preset);
}
- (void)windowDidResignKey:(NSNotification*)notification { [self closeMenu]; }
- (void)closeMenu {
    if (_monitor) { [NSEvent removeMonitor:_monitor]; _monitor = nil; }
    NSPanel* panel = _panel;
    _panel = nil;
    panel.delegate = nil;
    [panel.parentWindow removeChildWindow:panel];
    [panel orderOut:nil];
    [panel close];
    _menuView = nil;
    [self setNeedsDisplay:YES];
}
- (void)dealloc {
    if (_monitor) [NSEvent removeMonitor:_monitor];
    _panel.delegate = nil;
    [_panel.parentWindow removeChildWindow:_panel];
    [_panel close];
}
@end
