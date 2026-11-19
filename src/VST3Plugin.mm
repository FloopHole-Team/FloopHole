#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#include <Carbon/Carbon.h>
#include "FLPluginInstance.h"
#include "FLPathManager.h"
#include "FLPluginScanner.h"
#include "BridgeEditorLayout.h"
#include "FloopHoleFactory.h"
#import "ToolbarControls_mac.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/base/keycodes.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/gui/iplugview.h"

#include <atomic>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <set>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace flbr {

static char notebookTargetKey;
static char notebookEventResultsKey;
static void notebookPlatformKey(NSView* view, SEL selector, NSEvent* event);
static BOOL notebookKeyEquivalent(NSView* view, SEL selector, NSEvent* event);
static void notebookMouseUp(NSView* view, SEL selector, NSEvent* event);

static constexpr int32 kToolbarHeight = 54;
static constexpr int32 kLegacyAutomatableParams = 128;
static constexpr int32 kMaxAutomatableParams = 2048;

static constexpr ParamID kParamPitchBend    = 20000;
static constexpr ParamID kParamModWheel     = 20001;
static constexpr ParamID kParamAftertouch   = 20002;
static constexpr ParamID kParamSustain      = 20003;

static void utf8ToString128(const std::string& src, String128 dst) {
    memset(dst, 0, sizeof(String128));
    size_t n = std::min<size_t>(src.size(), 127);
    for (size_t i = 0; i < n; ++i) {
        dst[i] = (TChar)(uint8_t)src[i];
    }
}

class FLBridgeComponent;
class FLBridgePlugView;

} // namespace flbr

// ============================================================================
// Objective-C Handler for the VST3 Top Toolbar Preset Controls
// ============================================================================

@interface FloopHoleToolbarTarget : NSObject
@property (nonatomic, assign) flbr::FLBridgePlugView* plugView;
- (void)onPrevPreset:(id)sender;
- (void)onNextPreset:(id)sender;
- (void)onLoadFstFile:(id)sender;
- (void)onSaveFstFile:(id)sender;
- (void)onIdleTimer:(NSTimer*)timer;
@end

@interface FloopHoleResizeGrip : NSView
@property (nonatomic, assign) flbr::FLBridgePlugView* plugView;
@property (nonatomic) NSPoint startPoint;
@property (nonatomic) NSSize startSize;
@property (nonatomic) BOOL dragging;
@end

@interface FloopHoleOutputKnob : NSControl
@property(nonatomic, assign) flbr::FLBridgePlugView* plugView;
@property(nonatomic) double volume;
@property(nonatomic) double startVolume;
@property(nonatomic) CGFloat startY;
@property(nonatomic) BOOL dragging;
@property(nonatomic, strong) NSTrackingArea* hoverArea;
@end

@interface NSObject (FloopHoleNotebookWindow)
- (NSWindow*)window;
- (BOOL)sendKeyEvent:(NSEvent*)event;
@end

namespace flbr {

static BOOL notebookAcceptsFirstResponder(id, SEL) { return YES; }

static Class notebookNativeClass(id view, SEL) {
    return class_getSuperclass(object_getClass(view));
}

static void notebookMouseDown(NSView* view, SEL selector, NSEvent* event) {
    const BOOL focused = [view.window makeFirstResponder:view];
    if (!focused)
        fprintf(stderr, "FloopHole: could not focus native notebook editor\n");
    Method original = class_getInstanceMethod(class_getSuperclass(object_getClass(view)), selector);
    reinterpret_cast<void (*)(id, SEL, NSEvent*)>(method_getImplementation(original))(view, selector, event);
}

static void installNotebookFocus(NSView* view, NSHashTable<NSView*>* views, NSHashTable<FloopHoleToolbarTarget*>* target) {
    for (NSView* child in view.subviews) installNotebookFocus(child, views, target);
    Class native = NSClassFromString(@"D1NSView");
    if (!native || ![view isKindOfClass:native]) return;
    // MacVCL's custom writing surfaces otherwise leave Cocoa focus in the DAW.
    Class original = object_getClass(view);
    NSString* name = [@"FLBridgeNotebook_" stringByAppendingString:NSStringFromClass(original)];
    Class subclass = NSClassFromString(name);
    if (!subclass) {
        subclass = objc_allocateClassPair(original, name.UTF8String, 0);
        if (!subclass) {
            fprintf(stderr, "FloopHole: could not install notebook keyboard focus\n");
            return;
        }
        Method mouse = class_getInstanceMethod(original, @selector(mouseDown:));
        class_addMethod(subclass, @selector(mouseDown:), reinterpret_cast<IMP>(notebookMouseDown), method_getTypeEncoding(mouse));
        Method mouseUp = class_getInstanceMethod(original, @selector(mouseUp:));
        class_addMethod(subclass, @selector(mouseUp:), reinterpret_cast<IMP>(notebookMouseUp), method_getTypeEncoding(mouseUp));
        class_addMethod(subclass, @selector(acceptsFirstResponder), reinterpret_cast<IMP>(notebookAcceptsFirstResponder),
            method_getTypeEncoding(class_getInstanceMethod(original, @selector(acceptsFirstResponder))));
        class_addMethod(subclass, @selector(class), reinterpret_cast<IMP>(notebookNativeClass), "#@:");
        // Live delivers Cocoa keys here without invoking the VST key callbacks.
        for (SEL selector : {@selector(keyDown:), @selector(keyUp:), @selector(flagsChanged:)}) {
            Method inherited = class_getInstanceMethod(original, selector);
            if (inherited) class_addMethod(subclass, selector, reinterpret_cast<IMP>(notebookPlatformKey),
                method_getTypeEncoding(inherited));
        }
        Method equivalent = class_getInstanceMethod(original, @selector(performKeyEquivalent:));
        if (equivalent) class_addMethod(subclass, @selector(performKeyEquivalent:),
            reinterpret_cast<IMP>(notebookKeyEquivalent), method_getTypeEncoding(equivalent));
        objc_registerClassPair(subclass);
    }
    objc_setAssociatedObject(view, &notebookTargetKey, target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    object_setClass(view, subclass);
    [views addObject:view];
}

static bool notebookTextCommand(unichar character, bool navigation) {
    if (character > 0 && character <= 26) character += u'a' - 1;
    if (character >= u'A' && character <= u'Z') character += u'a' - u'A';
    return navigation || character == u'a' || character == u'c' || character == u'v' ||
        character == u'x' || character == u'z';
}

static NSEvent* notebookKeyEvent(NSWindow* window, bool down, char16 key, int16 keyCode, int16 modifiers) {
    unichar character = key;
    if (!character && keyCode >= 0 && keyCode <= VKEY_FIRST_ASCII + 'Z' - '0')
        character = VirtualKeyCodeToChar(static_cast<uint8>(keyCode));
    if ((modifiers & kCommandKey) && character > 0 && character <= 26) character += u'a' - 1;
    const bool navigation = keyCode == KEY_BACK || keyCode == KEY_DELETE || keyCode == KEY_HOME ||
        keyCode == KEY_END || (keyCode >= KEY_LEFT && keyCode <= KEY_DOWN);
    if ((modifiers & kCommandKey) && !notebookTextCommand(character, navigation)) return nil;
    unsigned short nativeCode = 0;
    switch (keyCode) {
        case KEY_BACK: character = NSDeleteCharacter; nativeCode = kVK_Delete; break;
        case KEY_DELETE: character = NSDeleteFunctionKey; nativeCode = kVK_ForwardDelete; break;
        case KEY_TAB: character = NSTabCharacter; nativeCode = kVK_Tab; break;
        case KEY_RETURN: character = NSCarriageReturnCharacter; nativeCode = kVK_Return; break;
        case KEY_ENTER: character = NSEnterCharacter; nativeCode = kVK_ANSI_KeypadEnter; break;
        case KEY_ESCAPE: character = 0x1b; nativeCode = kVK_Escape; break;
        case KEY_LEFT: character = NSLeftArrowFunctionKey; nativeCode = kVK_LeftArrow; break;
        case KEY_RIGHT: character = NSRightArrowFunctionKey; nativeCode = kVK_RightArrow; break;
        case KEY_UP: character = NSUpArrowFunctionKey; nativeCode = kVK_UpArrow; break;
        case KEY_DOWN: character = NSDownArrowFunctionKey; nativeCode = kVK_DownArrow; break;
        case KEY_HOME: character = NSHomeFunctionKey; nativeCode = kVK_Home; break;
        case KEY_END: character = NSEndFunctionKey; nativeCode = kVK_End; break;
        case KEY_PAGEUP: character = NSPageUpFunctionKey; nativeCode = kVK_PageUp; break;
        case KEY_PAGEDOWN: character = NSPageDownFunctionKey; nativeCode = kVK_PageDown; break;
        default: {
            if (!character || character < 0x20) return nil;
            static constexpr unsigned short letters[] = {
                kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F, kVK_ANSI_G,
                kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N,
                kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R, kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U,
                kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y, kVK_ANSI_Z
            };
            const unichar lower = character >= u'A' && character <= u'Z' ? character + u'a' - u'A' : character;
            if (lower >= u'a' && lower <= u'z') nativeCode = letters[lower - u'a'];
            else if (character == u' ') nativeCode = kVK_Space;
            else {
                static constexpr struct { char plain, shifted; unsigned short code; } keys[] = {
                    {'0', ')', kVK_ANSI_0}, {'1', '!', kVK_ANSI_1}, {'2', '@', kVK_ANSI_2},
                    {'3', '#', kVK_ANSI_3}, {'4', '$', kVK_ANSI_4}, {'5', '%', kVK_ANSI_5},
                    {'6', '^', kVK_ANSI_6}, {'7', '&', kVK_ANSI_7}, {'8', '*', kVK_ANSI_8},
                    {'9', '(', kVK_ANSI_9}, {'-', '_', kVK_ANSI_Minus}, {'=', '+', kVK_ANSI_Equal},
                    {'[', '{', kVK_ANSI_LeftBracket}, {']', '}', kVK_ANSI_RightBracket},
                    {'\\', '|', kVK_ANSI_Backslash}, {';', ':', kVK_ANSI_Semicolon},
                    {'\'', '"', kVK_ANSI_Quote}, {',', '<', kVK_ANSI_Comma},
                    {'.', '>', kVK_ANSI_Period}, {'/', '?', kVK_ANSI_Slash}, {'`', '~', kVK_ANSI_Grave}
                };
                for (const auto& entry : keys)
                    if (character == entry.plain || character == entry.shifted) nativeCode = entry.code;
            }
            break;
        }
    }
    NSEventModifierFlags flags = 0;
    if (modifiers & kShiftKey) flags |= NSEventModifierFlagShift;
    if (modifiers & kAlternateKey) flags |= NSEventModifierFlagOption;
    if (modifiers & kCommandKey) flags |= NSEventModifierFlagCommand;
    if (modifiers & kControlKey) flags |= NSEventModifierFlagControl;
    NSEvent* current = NSApp.currentEvent;
    const NSEventType type = down ? NSEventTypeKeyDown : NSEventTypeKeyUp;
    constexpr NSEventModifierFlags mask = NSEventModifierFlagShift | NSEventModifierFlagOption |
        NSEventModifierFlagCommand | NSEventModifierFlagControl;
    if (current.type == type && current.window == window && (current.modifierFlags & mask) == flags &&
        ((current.characters.length == 1 && [current.characters characterAtIndex:0] == character) ||
         current.keyCode == nativeCode))
        return current;
    NSString* text = [NSString stringWithCharacters:&character length:1];
    return [NSEvent keyEventWithType:type
        location:NSZeroPoint modifierFlags:flags timestamp:NSProcessInfo.processInfo.systemUptime
        windowNumber:window.windowNumber context:nil characters:text charactersIgnoringModifiers:text
        isARepeat:NO keyCode:nativeCode];
}

// ============================================================================
// FLBridgePlugView: Steinberg::IPlugView hosting Toolbar + Native FL NSView
// ============================================================================

class FLBridgePlugView : public IPlugView {
public:
    FLBridgePlugView(FLBridgeComponent* owner, FLPluginInstance* flInstance);
    virtual ~FLBridgePlugView();

    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override;
    uint32 PLUGIN_API addRef() override { return ++m_refCount; }
    uint32 PLUGIN_API release() override {
        uint32 r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }

    // IPlugView
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override;
    tresult PLUGIN_API attached(void* parent, FIDString type) override;
    tresult PLUGIN_API removed() override;
    tresult PLUGIN_API onWheel(float distance) override { return kResultFalse; }
    tresult PLUGIN_API onKeyDown(char16 key, int16 keyCode, int16 modifiers) override;
    tresult PLUGIN_API onKeyUp(char16 key, int16 keyCode, int16 modifiers) override;
    tresult PLUGIN_API getSize(ViewRect* size) override;
    tresult PLUGIN_API onSize(ViewRect* newSize) override;
    tresult PLUGIN_API onFocus(TBool state) override {
        m_notebookHasFocus = state != 0;
        if (!state) m_notebookKeys.clear();
        updateNotebookNativeFocus();
        return kResultOk;
    }
    tresult PLUGIN_API setFrame(IPlugFrame* frame) override { m_plugFrame = frame; return kResultOk; }
    tresult PLUGIN_API canResize() override;
    tresult PLUGIN_API checkSizeConstraint(ViewRect* rect) override;

    // UI Actions
    void selectPresetFromMenu(int presetIndex);
    void stepPreset(int delta);
    void promptLoadFstFile();
    void promptSaveFstFile();
    void onTimerTick();
    void refreshToolbarState();
    void setHintText(const std::string& text);
    void handlePluginResized(int plugW, int plugH);
    void resizeFromCorner(int width, int height);
    double getOutputControlValue() const { return m_flInstance->getOutputControlValue(); }
    double getOutputControlDefault() const { return m_flInstance->getOutputControlDefault(); }
    std::string getOutputControlHint() const { return m_flInstance->getOutputControlHint(); }
    bool isEffect() const { return !m_flInstance->isInstrument(); }
    void editOutputControl(double value, bool begin, bool end);
    bool handleNotebookEvent(NSEvent* event);
    void updateNotebookNativeFocus(bool refresh = false);

private:
    void rebuildPresetMenu();
    void layoutSubviews(int plugW, int plugH);
    void constrainEditorDimensions(int& width, int& height);
    bool sendNotebookKey(bool down, char16 key, int16 keyCode, int16 modifiers);

    std::atomic<uint32> m_refCount{1};
    FLBridgeComponent* m_owner = nullptr;
    FLPluginInstance* m_flInstance = nullptr;
    IPlugFrame* m_plugFrame = nullptr;
    bool m_resizingEditor = false;

    int32 m_plugWidth = 1024;
    int32 m_nativeWidth = 1024;
    int32 m_nativeHeight = 600;
    int32 m_plugHeight = 680;
    int32 m_toolbarHeight = 54;
    int32 m_resizeFooterHeight = 0;

    NSView* m_rootContainerView = nil;
    NSView* m_toolbarView = nil;
    NSView* m_pluginHostView = nil;
    FloopHolePresetPicker* m_presetPopup = nil;
    NSButton* m_prevPresetBtn = nil;
    NSButton* m_nextPresetBtn = nil;
    NSButton* m_loadFstBtn = nil;
    NSTextField* m_hintLabel = nil;
    FloopHoleToolbarTarget* m_target = nil;
    NSTimer* m_idleTimer = nil;
    NSArray<NSView*>* m_toolbarControls = nil;
    FloopHoleResizeGrip* m_resizeGrip = nil;
    FloopHoleOutputKnob* m_volumeKnob = nil;
    NSHashTable<NSView*>* m_notebookFocusViews = nil;
    __weak id m_notebookWindowWrapper = nil;
    std::set<unsigned short> m_notebookKeys;
    bool m_notebookHasFocus = true;
    bool m_notebookNativeFocused = false;
    id m_edisonKeyboardMonitor = nil;
    bool m_edisonKeyboardFocused = false;
};

// ============================================================================
// FLBridgeComponent: Combined VST3 Processor + EditController
// ============================================================================

class FLBridgeComponent : public IComponent,
                          public IAudioProcessor,
                          public IEditController,
                          public IEditController2,
                          public IMidiMapping {
public:
    explicit FLBridgeComponent(const NativePluginClass& plugin);
    virtual ~FLBridgeComponent();

    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override;
    uint32 PLUGIN_API addRef() override { return ++m_refCount; }
    uint32 PLUGIN_API release() override {
        uint32 r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }

    // IPluginBase
    tresult PLUGIN_API initialize(FUnknown* context) override;
    tresult PLUGIN_API terminate() override;

    // IComponent
    tresult PLUGIN_API getControllerClassId(TUID classId) override;
    tresult PLUGIN_API setIoMode(IoMode mode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override;
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override;
    tresult PLUGIN_API getRoutingInfo(RoutingInfo& inInfo, RoutingInfo& outInfo) override;
    tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index, TBool state) override;
    tresult PLUGIN_API setActive(TBool state) override;
    tresult PLUGIN_API setState(IBStream* state) override;
    tresult PLUGIN_API getState(IBStream* state) override;

    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                          SpeakerArrangement* outputs, int32 numOuts) override;
    tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) override;
    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override;
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override;
    tresult PLUGIN_API setProcessing(TBool state) override;
    tresult PLUGIN_API process(ProcessData& data) override;
    uint32 PLUGIN_API getTailSamples() override { return kNoTail; }

    // IEditController
    tresult PLUGIN_API setComponentState(IBStream* state) override;
    int32 PLUGIN_API getParameterCount() override;
    tresult PLUGIN_API getParameterInfo(int32 paramIndex, ParameterInfo& info) override;
    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue valueNormalized, String128 string) override;
    tresult PLUGIN_API getParamValueByString(ParamID id, TChar* string, ParamValue& valueNormalized) override;
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue valueNormalized) override { return valueNormalized; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue plainValue) override { return plainValue; }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override;
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override;
    tresult PLUGIN_API setComponentHandler(IComponentHandler* handler) override;
    IPlugView* PLUGIN_API createView(FIDString name) override;

    // IEditController2
    tresult PLUGIN_API setKnobMode(KnobMode mode) override { return kResultOk; }
    tresult PLUGIN_API openHelp(TBool onlyCheck) override { return kResultFalse; }
    tresult PLUGIN_API openAboutBox(TBool onlyCheck) override { return kResultFalse; }

    // IMidiMapping
    tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16 channel,
                                                   CtrlNumber midiControllerNumber, ParamID& id) override;

    FLPluginInstance& getFLInstance() { return m_flInstance; }
    ParamID getOutputControlID() const { return m_defaultIsEffect ? kParamEffectMix : kParamMasterVolume; }
    void editOutputControl(double value, bool begin, bool end);
    void onViewDestroyed(FLBridgePlugView* v) {
        if (m_activeView == v) m_activeView = nullptr;
    }

private:
    std::atomic<uint32> m_refCount{1};
    const NativePluginClass m_pluginClass;
    bool m_defaultIsEffect = false;
    bool m_initialized = false;
    FLPluginInstance m_flInstance;
    IComponentHandler* m_componentHandler = nullptr;
    FLBridgePlugView* m_activeView = nullptr;

    double m_pitchBendNorm = 0.5;
    double m_modWheelNorm = 0.0;
    double m_aftertouchNorm = 0.0;
    double m_sustainNorm = 0.0;
    double m_transportSampleRate = 44100.0;
    double m_nextProjectQuarters = 0.0;
    double m_nextProjectSeconds = 0.0;
    int32 m_timeSigNumerator = 4;
    int32 m_timeSigDenominator = 4;
};

} // namespace flbr

// ============================================================================
// FloopHoleToolbarTarget Implementation
// ============================================================================

@implementation FloopHoleToolbarTarget
- (void)onPrevPreset:(id)sender {
    if (_plugView) _plugView->stepPreset(-1);
}

- (void)onNextPreset:(id)sender {
    if (_plugView) _plugView->stepPreset(+1);
}

- (void)onLoadFstFile:(id)sender {
    if (_plugView) _plugView->promptLoadFstFile();
}

- (void)onSaveFstFile:(id)sender {
    if (_plugView) _plugView->promptSaveFstFile();
}

- (void)onIdleTimer:(NSTimer*)timer {
    if (_plugView) _plugView->onTimerTick();
}
@end

@implementation FloopHoleResizeGrip
- (void)drawRect:(NSRect)dirty {
    [[NSColor colorWithCalibratedWhite:0.6 alpha:1] setStroke];
    NSBezierPath* lines = [NSBezierPath bezierPath];
    for (int i = 0; i < 3; ++i) {
        [lines moveToPoint:NSMakePoint(4 + i * 3, 2)];
        [lines lineToPoint:NSMakePoint(12, 10 - i * 3)];
    }
    [lines stroke];
}
- (void)resetCursorRects {
    if (!_plugView || _plugView->canResize() != kResultTrue) return;
    static NSCursor* cursor = nil;
    if (!cursor) {
        NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(24, 24)];
        [image lockFocus];
        NSBezierPath* arrow = [NSBezierPath bezierPath];
        [arrow moveToPoint:NSMakePoint(4, 20)];
        [arrow lineToPoint:NSMakePoint(20, 4)];
        [arrow moveToPoint:NSMakePoint(4, 13)];
        [arrow lineToPoint:NSMakePoint(4, 20)];
        [arrow lineToPoint:NSMakePoint(11, 20)];
        [arrow moveToPoint:NSMakePoint(13, 4)];
        [arrow lineToPoint:NSMakePoint(20, 4)];
        [arrow lineToPoint:NSMakePoint(20, 11)];
        [NSColor.blackColor setStroke];
        arrow.lineWidth = 3;
        [arrow stroke];
        [NSColor.whiteColor setStroke];
        arrow.lineWidth = 1;
        [arrow stroke];
        [image unlockFocus];
        cursor = [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(12, 12)];
    }
    [self addCursorRect:self.bounds cursor:cursor];
}
- (void)mouseDown:(NSEvent*)event {
    if (!_plugView || _plugView->canResize() != kResultTrue) return;
    ViewRect size{};
    _plugView->getSize(&size);
    _startSize = NSMakeSize(size.getWidth(), size.getHeight());
    _startPoint = [self.window convertPointToScreen:event.locationInWindow];
    _dragging = YES;
}
- (void)mouseDragged:(NSEvent*)event {
    if (!_dragging || !_plugView) return;
    const NSPoint point = [self.window convertPointToScreen:event.locationInWindow];
    _plugView->resizeFromCorner(static_cast<int>(std::lround(_startSize.width + point.x - _startPoint.x)),
        static_cast<int>(std::lround(_startSize.height - point.y + _startPoint.y)));
}
- (void)mouseUp:(NSEvent*)event {
    _dragging = NO;
}
@end

@implementation FloopHoleOutputKnob
- (BOOL)acceptsFirstResponder { return YES; }
- (void)setVolume:(double)value {
    if (_volume == value) return;
    _volume = value;
    self.toolTip = _plugView ? [NSString stringWithUTF8String:_plugView->getOutputControlHint().c_str()] : nil;
    [self setNeedsDisplay:YES];
    if (_plugView && self.window && NSPointInRect(
        [self convertPoint:self.window.mouseLocationOutsideOfEventStream fromView:nil], self.bounds))
        _plugView->setHintText(_plugView->getOutputControlHint());
}
- (id)accessibilityValue { return @(_volume * 100); }
- (void)updateTrackingAreas {
    if (_hoverArea) [self removeTrackingArea:_hoverArea];
    _hoverArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect
        options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInActiveApp | NSTrackingInVisibleRect
        owner:self userInfo:nil];
    [self addTrackingArea:_hoverArea];
    [super updateTrackingAreas];
}
- (void)mouseEntered:(NSEvent*)event {
    if (_plugView) _plugView->setHintText(_plugView->getOutputControlHint());
}
- (void)drawRect:(NSRect)dirty {
    const CGFloat diameter = std::min<CGFloat>(30, self.bounds.size.width - 4);
    if (diameter <= 0) return;
    const NSPoint center = NSMakePoint(NSMidX(self.bounds), self.bounds.size.height - 16);
    const CGFloat radius = diameter / 2;
    NSBezierPath* body = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(center.x - radius + 2,
        center.y - radius + 2, diameter - 4, diameter - 4)];
    NSGradient* gradient = [[NSGradient alloc] initWithStartingColor:
        [NSColor colorWithCalibratedRed:0.33 green:0.36 blue:0.39 alpha:1]
        endingColor:[NSColor colorWithCalibratedRed:0.13 green:0.15 blue:0.18 alpha:1]];
    [gradient drawInBezierPath:body angle:90];
    [[NSColor colorWithCalibratedWhite:0.08 alpha:1] setStroke];
    body.lineWidth = 1;
    [body stroke];
    NSBezierPath* track = [NSBezierPath bezierPath];
    [track appendBezierPathWithArcWithCenter:center radius:radius startAngle:225 endAngle:-45 clockwise:YES];
    const bool mix = _plugView && _plugView->isEffect();
    [(mix ? [NSColor colorWithCalibratedRed:0.4 green:0.31 blue:0.2 alpha:1] :
        [NSColor colorWithCalibratedRed:0.29 green:0.36 blue:0.34 alpha:1]) setStroke];
    track.lineWidth = 1.5;
    [track stroke];
    NSBezierPath* active = [NSBezierPath bezierPath];
    const double angle = 225 - _volume * 270;
    [active appendBezierPathWithArcWithCenter:center radius:radius startAngle:225 endAngle:angle clockwise:YES];
    [(mix ? [NSColor colorWithCalibratedRed:1 green:0.62 blue:0.16 alpha:1] :
        [NSColor colorWithCalibratedRed:0.5 green:0.94 blue:0.73 alpha:1]) setStroke];
    active.lineWidth = 1.5;
    [active stroke];
    const double radians = angle * std::acos(-1.0) / 180;
    const NSPoint marker = NSMakePoint(center.x + (radius - 5) * std::cos(radians),
        center.y + (radius - 5) * std::sin(radians));
    [[NSColor colorWithCalibratedWhite:0.94 alpha:1] setFill];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(marker.x - 1, marker.y - 1, 2, 2)] fill];
    NSDictionary* attributes = @{NSFontAttributeName:[NSFont systemFontOfSize:7 weight:NSFontWeightMedium],
        NSForegroundColorAttributeName:[NSColor colorWithCalibratedWhite:0.85 alpha:1]};
    NSString* title = mix ? @"MIX" : @"VOL";
    const NSSize label = [title sizeWithAttributes:attributes];
    [title drawAtPoint:NSMakePoint(center.x - label.width / 2, 0) withAttributes:attributes];
}
- (void)mouseDown:(NSEvent*)event {
    if (!_plugView) return;
    [self.window makeFirstResponder:self];
    if (event.clickCount == 2) {
        _plugView->editOutputControl(_plugView->getOutputControlDefault(), true, true);
        self.volume = _plugView->getOutputControlValue();
        return;
    }
    _startVolume = _plugView->getOutputControlValue();
    _startY = [self.window convertPointToScreen:event.locationInWindow].y;
    _dragging = YES;
    _plugView->editOutputControl(_startVolume, true, false);
}
- (void)mouseDragged:(NSEvent*)event {
    if (!_dragging || !_plugView) return;
    const double distance = [self.window convertPointToScreen:event.locationInWindow].y - _startY;
    const double scale = event.modifierFlags & NSEventModifierFlagShift ? 1500 : 150;
    _plugView->editOutputControl(std::clamp(_startVolume + distance / scale, 0.0, 1.0), false, false);
    self.volume = _plugView->getOutputControlValue();
}
- (void)mouseUp:(NSEvent*)event {
    if (_dragging && _plugView) _plugView->editOutputControl(_plugView->getOutputControlValue(), false, true);
    _dragging = NO;
}
- (void)scrollWheel:(NSEvent*)event {
    if (!_plugView) return;
    _plugView->editOutputControl(std::clamp(_plugView->getOutputControlValue() + event.scrollingDeltaY * 0.005, 0.0, 1.0),
        true, true);
    self.volume = _plugView->getOutputControlValue();
}
- (void)keyDown:(NSEvent*)event {
    if (!_plugView) return;
    double value = _plugView->getOutputControlValue();
    const double step = event.modifierFlags & NSEventModifierFlagShift ? 0.001 : 0.01;
    if (event.keyCode == 126 || event.keyCode == 124) value += step;
    else if (event.keyCode == 125 || event.keyCode == 123) value -= step;
    else { [super keyDown:event]; return; }
    _plugView->editOutputControl(std::clamp(value, 0.0, 1.0), true, true);
    self.volume = _plugView->getOutputControlValue();
}
@end

namespace flbr {

// ============================================================================
// FLBridgePlugView Implementation
// ============================================================================

static void notebookPlatformKey(NSView* view, SEL selector, NSEvent* event) {
    NSHashTable<FloopHoleToolbarTarget*>* reference = objc_getAssociatedObject(view, &notebookTargetKey);
    FLBridgePlugView* plugView = reference.anyObject.plugView;
    const bool handled = plugView && plugView->handleNotebookEvent(event);
    if (!handled) {
        Method original = class_getInstanceMethod(class_getSuperclass(object_getClass(view)), selector);
        reinterpret_cast<void (*)(id, SEL, NSEvent*)>(method_getImplementation(original))(view, selector, event);
    }
}

static BOOL notebookKeyEquivalent(NSView* view, SEL selector, NSEvent* event) {
    NSHashTable<FloopHoleToolbarTarget*>* reference = objc_getAssociatedObject(view, &notebookTargetKey);
    FLBridgePlugView* plugView = reference.anyObject.plugView;
    BOOL handled = plugView && plugView->handleNotebookEvent(event);
    if (!handled) {
        Method original = class_getInstanceMethod(class_getSuperclass(object_getClass(view)), selector);
        handled = reinterpret_cast<BOOL (*)(id, SEL, NSEvent*)>(method_getImplementation(original))(view, selector, event);
    }
    return handled;
}

static void notebookMouseUp(NSView* view, SEL selector, NSEvent* event) {
    NSHashTable<FloopHoleToolbarTarget*>* reference = objc_getAssociatedObject(view, &notebookTargetKey);
    Method original = class_getInstanceMethod(class_getSuperclass(object_getClass(view)), selector);
    reinterpret_cast<void (*)(id, SEL, NSEvent*)>(method_getImplementation(original))(view, selector, event);
    // Native selection must finish before the insertion caret is activated.
    if (FLBridgePlugView* plugView = reference.anyObject.plugView)
        plugView->updateNotebookNativeFocus(true);
}

FLBridgePlugView::FLBridgePlugView(FLBridgeComponent* owner, FLPluginInstance* flInstance)
    : m_owner(owner), m_flInstance(flInstance) {
}

FLBridgePlugView::~FLBridgePlugView() {
    removed();
    if (m_owner) {
        m_owner->onViewDestroyed(this);
    }
}

tresult PLUGIN_API FLBridgePlugView::queryInterface(const TUID _iid, void** obj) {
    if (FUnknownPrivate::iidEqual(_iid, IPlugView::iid) ||
        FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
        addRef();
        *obj = static_cast<IPlugView*>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API FLBridgePlugView::isPlatformTypeSupported(FIDString type) {
    if (type && strcmp(type, kPlatformTypeNSView) == 0) {
        return kResultTrue;
    }
    return kResultFalse;
}

bool FLBridgePlugView::handleNotebookEvent(NSEvent* event) {
    if (!event || !m_target) return false;
    NSMapTable<FloopHoleToolbarTarget*, NSNumber*>* results = objc_getAssociatedObject(event, &notebookEventResultsKey);
    if (NSNumber* result = [results objectForKey:m_target]) {
        return result.boolValue;
    }
    if (!m_notebookHasFocus || !m_pluginHostView.window) {
        return false;
    }
    const bool edison = m_flInstance && m_flInstance->getPluginName() == "Edison";
    if (edison) {
        if (!m_edisonKeyboardFocused || event.window != m_pluginHostView.window) return false;
        NSResponder* responder = m_pluginHostView.window.firstResponder;
        if ([responder isKindOfClass:NSView.class] && [(NSView*)responder isDescendantOf:m_toolbarView])
            return false;
    } else {
        NSResponder* responder = m_pluginHostView.window.firstResponder;
        if (!m_notebookFocusViews || ![responder isKindOfClass:NSView.class] ||
            ![m_notebookFocusViews containsObject:(NSView*)responder]) return false;
    }
    const bool down = event.type == NSEventTypeKeyDown;
    const bool up = event.type == NSEventTypeKeyUp;
    if (!down && !up && event.type != NSEventTypeFlagsChanged) return false;
    if (edison) {
        const auto text = event.charactersIgnoringModifiers;
        const bool shortcut = down && (event.modifierFlags & NSEventModifierFlagCommand) &&
            notebookTextCommand(text.length ? [text characterAtIndex:0] : 0, false);
        if (!shortcut && !(up && m_notebookKeys.count(event.keyCode))) return false;
    }
    if ((down || up) && (event.modifierFlags & NSEventModifierFlagCommand)) {
        const unsigned short code = event.keyCode;
        const bool navigation = code == kVK_Delete || code == kVK_ForwardDelete ||
            code == kVK_Home || code == kVK_End || code == kVK_LeftArrow || code == kVK_RightArrow ||
            code == kVK_UpArrow || code == kVK_DownArrow;
        NSString* text = event.charactersIgnoringModifiers;
        if (!notebookTextCommand(text.length ? [text characterAtIndex:0] : 0, navigation)) {
            return false;
        }
    }
    if (!results) {
        // A host can deliver one event through Cocoa and the VST callbacks.
        results = [NSMapTable weakToStrongObjectsMapTable];
        objc_setAssociatedObject(event, &notebookEventResultsKey, results, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    [results setObject:@NO forKey:m_target];
    const bool pairedUp = up && m_notebookKeys.erase(event.keyCode) != 0;
    if (up && !pairedUp) {
        return false;
    }
    const bool nativeHandled = [m_notebookWindowWrapper sendKeyEvent:event];
    const bool handled = nativeHandled || pairedUp; // Do not leak releases for consumed key-downs to the DAW.
    if (down && handled) m_notebookKeys.insert(event.keyCode);
    [results setObject:@(handled) forKey:m_target];
    return handled;
}

bool FLBridgePlugView::sendNotebookKey(bool down, char16 key, int16 keyCode, int16 modifiers) {
    if ((!m_notebookFocusViews && !m_edisonKeyboardMonitor) || !m_pluginHostView.window) return false;
    NSEvent* event = notebookKeyEvent(m_pluginHostView.window, down, key, keyCode, modifiers);
    if (!event) {
        return false;
    }
    return handleNotebookEvent(event);
}

tresult PLUGIN_API FLBridgePlugView::onKeyDown(char16 key, int16 keyCode, int16 modifiers) {
    if (!sendNotebookKey(true, key, keyCode, modifiers)) {
        return kResultFalse;
    }
    return kResultTrue;
}

tresult PLUGIN_API FLBridgePlugView::onKeyUp(char16 key, int16 keyCode, int16 modifiers) {
    if (!sendNotebookKey(false, key, keyCode, modifiers)) {
        return kResultFalse;
    }
    return kResultTrue;
}

void FLBridgePlugView::updateNotebookNativeFocus(bool refresh) {
    if ((!m_notebookFocusViews && !m_edisonKeyboardMonitor) || !m_flInstance) return;
    NSWindow* window = m_pluginHostView.window;
    NSResponder* responder = window.firstResponder;
    const bool focused = m_notebookHasFocus && window.keyWindow &&
        (m_edisonKeyboardMonitor ? m_edisonKeyboardFocused :
            [responder isKindOfClass:NSView.class] && [m_notebookFocusViews containsObject:(NSView*)responder]);
    if (focused == m_notebookNativeFocused && !(refresh && focused)) return;
    m_notebookNativeFocused = focused;
    m_flInstance->setEditorFocus(focused);
}

tresult PLUGIN_API FLBridgePlugView::attached(void* parent, FIDString type) {
    if (!parent || isPlatformTypeSupported(type) != kResultTrue) {
        return kResultFalse;
    }

    @autoreleasepool {
        NSView* parentNSView = (__bridge NSView*)parent;
        m_notebookHasFocus = true;

        // Create our container view and subviews
        NSRect initRect = NSMakeRect(0, 0, m_plugWidth, m_plugHeight + m_toolbarHeight + m_resizeFooterHeight);
        m_rootContainerView = [[NSView alloc] initWithFrame:initRect];
        [m_rootContainerView setWantsLayer:YES];
        m_rootContainerView.layer.backgroundColor = themeColor(ToolbarTheme::background).CGColor;

        m_pluginHostView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, m_plugWidth, m_plugHeight)];

        m_toolbarView = [[NSView alloc] initWithFrame:NSMakeRect(0, m_plugHeight, m_plugWidth, kToolbarHeight)];
        [m_toolbarView setWantsLayer:YES];
        m_toolbarView.layer.backgroundColor = themeColor(ToolbarTheme::background).CGColor;
        [m_rootContainerView addSubview:m_toolbarView];

        m_target = [[FloopHoleToolbarTarget alloc] init];
        m_target.plugView = this;

        FloopHoleLogoView* logo = [[FloopHoleLogoView alloc] initWithFrame:NSZeroRect];
        [m_toolbarView addSubview:logo];
        m_presetPopup = [[FloopHolePresetPicker alloc] initWithFrame:NSZeroRect];
        m_presetPopup->onSelect = [this](int preset) { selectPresetFromMenu(preset); };
        [m_toolbarView addSubview:m_presetPopup];

        FloopHoleButton* previous = [[FloopHoleButton alloc] initWithFrame:NSZeroRect];
        previous.icon = 0;
        previous.target = m_target;
        previous.action = @selector(onPrevPreset:);
        previous.toolTip = previous.accessibilityLabel = @"Previous preset";
        m_prevPresetBtn = previous;
        [m_toolbarView addSubview:m_prevPresetBtn];

        FloopHoleButton* next = [[FloopHoleButton alloc] initWithFrame:NSZeroRect];
        next.icon = 1;
        next.target = m_target;
        next.action = @selector(onNextPreset:);
        next.toolTip = next.accessibilityLabel = @"Next preset";
        m_nextPresetBtn = next;
        [m_toolbarView addSubview:m_nextPresetBtn];

        FloopHoleButton* folder = [[FloopHoleButton alloc] initWithFrame:NSZeroRect];
        folder.icon = 2;
        folder.target = m_target;
        folder.action = @selector(onLoadFstFile:);
        folder.toolTip = folder.accessibilityLabel = @"Load preset";
        m_loadFstBtn = folder;
        [m_toolbarView addSubview:m_loadFstBtn];

        FloopHoleButton* save = [[FloopHoleButton alloc] initWithFrame:NSZeroRect];
        save.icon = 3;
        save.target = m_target;
        save.action = @selector(onSaveFstFile:);
        save.toolTip = save.accessibilityLabel = @"Save preset as...";
        [m_toolbarView addSubview:save];

        m_hintLabel = [NSTextField labelWithString:@"Ready"];
        [m_hintLabel setFont:[NSFont systemFontOfSize:11]];
        [m_hintLabel setTextColor:themeColor(ToolbarTheme::muted)];
        [[m_hintLabel cell] setLineBreakMode:NSLineBreakByTruncatingTail];
        [m_toolbarView addSubview:m_hintLabel];
        m_volumeKnob = [[FloopHoleOutputKnob alloc] initWithFrame:NSZeroRect];
        m_volumeKnob.plugView = this;
        m_volumeKnob.volume = getOutputControlValue();
        m_volumeKnob.accessibilityRole = NSAccessibilitySliderRole;
        m_volumeKnob.accessibilityLabel = isEffect() ? @"Effect mix" : @"Master volume";
        [m_toolbarView addSubview:m_volumeKnob];
        m_toolbarControls = @[logo, m_presetPopup, m_prevPresetBtn, m_nextPresetBtn,
            m_loadFstBtn, save, m_hintLabel, m_volumeKnob];
        m_resizeGrip = [[FloopHoleResizeGrip alloc] initWithFrame:NSZeroRect];
        m_resizeGrip.plugView = this;
        m_resizeGrip.toolTip = @"Resize native plugin";
        [m_rootContainerView addSubview:m_resizeGrip];

        // MacVCL's form search loses a found form when it visits later
        // non-native siblings. The toolbar/footer do not overlap this view.
        [m_rootContainerView addSubview:m_pluginHostView];

        [parentNSView addSubview:m_rootContainerView];

        rebuildPresetMenu();

        // Hook up hint and resize callbacks
        if (m_flInstance) {
            m_flInstance->onHintChanged = [this](const std::string& hint) {
                setHintText(hint);
            };
            m_flInstance->onEditorResized = [this](int w, int h) {
                handlePluginResized(w, h);
            };

            int pw = 900, ph = 600;
            if (m_flInstance->attachEditorToNSView((__bridge void*)m_pluginHostView, pw, ph)) {
                const std::string& name = m_flInstance->getPluginName();
                if (name == "Fruity NoteBook" || name == "Fruity NoteBook 2" || name == "Edison") {
                    NSView* editor = m_pluginHostView.subviews.firstObject;
                    Ivar owner = class_getInstanceVariable(NSClassFromString(@"D1NSView"), "owner");
                    if (editor && owner && [editor isKindOfClass:NSClassFromString(@"D1NSView")]) {
                        m_notebookWindowWrapper = object_getIvar(editor, owner);
                        NSHashTable<FloopHoleToolbarTarget*>* reference = [NSHashTable weakObjectsHashTable];
                        [reference addObject:m_target];
                        if (name == "Edison") {
                            m_notebookWindowWrapper = (__bridge id)m_flInstance->getEditorKeyboardWindow();
                            if (!m_notebookWindowWrapper)
                                setHintText("Edison clipboard shortcuts are unavailable for this native editor version. See the host log.");
                            // Keep Edison's existing mouse/drop subclasses intact. Library-mode
                            // MacVCL does not participate in the host's Edit-menu shortcut routing.
                            m_edisonKeyboardMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:
                                NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskLeftMouseDown |
                                NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown
                                handler:^NSEvent*(NSEvent* event) {
                                    FLBridgePlugView* view = reference.anyObject.plugView;
                                    if (!view) return event;
                                    if (event.type == NSEventTypeKeyDown || event.type == NSEventTypeKeyUp)
                                        return view->handleNotebookEvent(event) ? nil : event;
                                    view->m_edisonKeyboardFocused = event.window == view->m_pluginHostView.window &&
                                        NSPointInRect([view->m_pluginHostView convertPoint:event.locationInWindow fromView:nil],
                                            view->m_pluginHostView.bounds);
                                    view->updateNotebookNativeFocus(true);
                                    return event;
                                }];
                        } else {
                            m_notebookFocusViews = [NSHashTable weakObjectsHashTable];
                            installNotebookFocus(editor, m_notebookFocusViews, reference);
                        }
                    } else {
                        fprintf(stderr, "FloopHole: native editor keyboard routing is unavailable for %s\n", name.c_str());
                    }
                }
                layoutSubviews(pw, ph);
                if (m_plugFrame) {
                    ViewRect vr{};
                    getSize(&vr);
                    m_plugFrame->resizeView(this, &vr);
                }
            }
        }

        m_idleTimer = [NSTimer scheduledTimerWithTimeInterval:(1.0 / 60.0)
                                                       target:m_target
                                                     selector:@selector(onIdleTimer:)
                                                     userInfo:nil
                                                      repeats:YES];
        [[NSRunLoop currentRunLoop] addTimer:m_idleTimer forMode:NSRunLoopCommonModes];
    }

    return kResultOk;
}

tresult PLUGIN_API FLBridgePlugView::removed() {
    if (m_edisonKeyboardMonitor) [NSEvent removeMonitor:m_edisonKeyboardMonitor];
    m_edisonKeyboardMonitor = nil;
    m_edisonKeyboardFocused = false;
    m_notebookKeys.clear();
    if (m_notebookNativeFocused && m_flInstance) m_flInstance->setEditorFocus(false);
    m_notebookNativeFocused = false;
    NSResponder* notebookResponder = m_pluginHostView.window.firstResponder;
    if (m_notebookFocusViews && [notebookResponder isKindOfClass:NSView.class] &&
        [(NSView*)notebookResponder isDescendantOf:m_pluginHostView])
        [m_pluginHostView.window makeFirstResponder:nil];
    for (NSView* view in m_notebookFocusViews.allObjects) {
        objc_setAssociatedObject(view, &notebookTargetKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        object_setClass(view, class_getSuperclass(object_getClass(view)));
    }
    m_notebookFocusViews = nil;
    m_notebookWindowWrapper = nil;
    [m_presetPopup closeMenu];
    if (m_presetPopup) m_presetPopup->onSelect = nullptr;
    if (m_volumeKnob.dragging) editOutputControl(getOutputControlValue(), false, true);
    m_volumeKnob.plugView = nullptr;
    m_volumeKnob.dragging = NO;
    m_volumeKnob = nil;
    if (!m_rootContainerView) return kResultOk;
    @autoreleasepool {
        if (m_idleTimer) {
            [m_idleTimer invalidate];
            m_idleTimer = nil;
        }
        if (m_flInstance) {
            m_flInstance->onHintChanged = nullptr;
            m_flInstance->onEditorResized = nullptr;
            m_flInstance->detachEditor();
        }
        if (m_target) {
            m_target.plugView = nullptr;
            m_target = nil;
        }
        m_resizeGrip.plugView = nullptr;
        m_resizeGrip.dragging = NO;
        m_resizeGrip = nil;
        m_toolbarControls = nil;
        if (m_rootContainerView) {
            [m_rootContainerView removeFromSuperview];
            m_rootContainerView = nil;
        }
        m_toolbarView = nil;
        m_pluginHostView = nil;
        m_presetPopup = nil;
        m_prevPresetBtn = nil;
        m_nextPresetBtn = nil;
        m_loadFstBtn = nil;
        m_hintLabel = nil;
    }
    return kResultOk;
}

void FLBridgePlugView::layoutSubviews(int plugW, int plugH) {
    m_nativeWidth = plugW;
    m_nativeHeight = plugH;
    m_plugWidth = plugW;
    m_plugHeight = plugH;
    const auto toolbar = BridgeToolbarLayout::compute(plugW);
    m_toolbarHeight = toolbar.height;
    m_resizeFooterHeight = canResize() == kResultTrue ? kBridgeResizeFooterHeight : 0;
    if (m_rootContainerView) {
        [m_rootContainerView setFrameSize:NSMakeSize(m_plugWidth, m_plugHeight + m_toolbarHeight + m_resizeFooterHeight)];
    }
    if (m_pluginHostView) {
        [m_pluginHostView setFrame:NSMakeRect(0, m_resizeFooterHeight, plugW, plugH)];
    }
    if (m_toolbarView) {
        [m_toolbarView setFrame:NSMakeRect(0, m_plugHeight + m_resizeFooterHeight, m_plugWidth, m_toolbarHeight)];
    }
    for (size_t i = 0; i < m_toolbarControls.count; ++i) {
        const auto& frame = toolbar.controls[i];
        NSView* control = m_toolbarControls[i];
        const bool label = [control isKindOfClass:NSTextField.class];
        [control setFrame:NSMakeRect(frame.x, toolbar.height - frame.y - frame.height + (label ? 1 : 0),
            frame.width, label ? 20 : frame.height)];
    }
    m_resizeGrip.hidden = m_resizeFooterHeight == 0;
    [m_resizeGrip setFrame:NSMakeRect(std::max(0, plugW - kBridgeResizeFooterHeight), 0,
        kBridgeResizeFooterHeight, kBridgeResizeFooterHeight)];
    [m_resizeGrip.window invalidateCursorRectsForView:m_resizeGrip];
}

void FLBridgePlugView::handlePluginResized(int plugW, int plugH) {
    if (m_resizingEditor) return;
    layoutSubviews(plugW, plugH);
    if (m_plugFrame) {
        ViewRect vr{};
        getSize(&vr);
        m_plugFrame->resizeView(this, &vr);
    }
}

tresult PLUGIN_API FLBridgePlugView::getSize(ViewRect* size) {
    if (!size) return kInvalidArgument;
    size->left = 0;
    size->top = 0;
    size->right = m_plugWidth;
    size->bottom = m_plugHeight + m_toolbarHeight + m_resizeFooterHeight;
    return kResultOk;
}

tresult PLUGIN_API FLBridgePlugView::onSize(ViewRect* newSize) {
    if (!newSize) return kInvalidArgument;
    const int w = newSize->getWidth(), h = newSize->getHeight();
    if (m_resizingEditor || (w == m_plugWidth && h == m_plugHeight + m_toolbarHeight + m_resizeFooterHeight)) return kResultOk;
    if (w <= 0 || h <= 0 || !m_flInstance || !m_flInstance->isEditorOpen()) {
        fprintf(stderr, "FloopHole: cannot apply this editor size before native attachment\n");
        return kInvalidArgument;
    }
    int nativeWidth = w, nativeHeight = h;
    constrainEditorDimensions(nativeWidth, nativeHeight);
    m_resizingEditor = true;
    const bool resized = canResize() == kResultFalse || m_flInstance->resizeEditor(nativeWidth, nativeHeight);
    m_resizingEditor = false;
    if (!resized) return kResultFalse;
    layoutSubviews(nativeWidth, nativeHeight);
    if ((w != m_plugWidth || h != m_plugHeight + m_toolbarHeight + m_resizeFooterHeight) && m_plugFrame) {
        ViewRect corrected{};
        getSize(&corrected);
        m_plugFrame->resizeView(this, &corrected);
    }
    return kResultOk;
}

tresult PLUGIN_API FLBridgePlugView::canResize() {
    return m_flInstance && m_flInstance->isLoaded() && m_flInstance->getEditorSizeConstraints().isResizable()
        ? kResultTrue : kResultFalse;
}

void FLBridgePlugView::constrainEditorDimensions(int& width, int& height) {
    const auto constraints = m_flInstance->getEditorSizeConstraints();
    int retainedHeight = m_nativeHeight;
    constraints.constrain(width, retainedHeight);
    height -= BridgeToolbarLayout::compute(width).height +
        (constraints.isResizable() ? kBridgeResizeFooterHeight : 0);
    constraints.constrain(width, height);
}

tresult PLUGIN_API FLBridgePlugView::checkSizeConstraint(ViewRect* rect) {
    if (!rect) return kInvalidArgument;
    if (m_flInstance && m_flInstance->isEditorOpen()) {
        int width = rect->getWidth(), height = rect->getHeight();
        constrainEditorDimensions(width, height);
        rect->right = rect->left + width;
        rect->bottom = rect->top + bridgeEditorHeight(width, height,
            m_flInstance->getEditorSizeConstraints().isResizable());
        return kResultOk;
    }
    rect->right = rect->left + m_plugWidth;
    rect->bottom = rect->top + m_plugHeight + m_toolbarHeight + m_resizeFooterHeight;
    return kResultOk;
}

void FLBridgePlugView::resizeFromCorner(int width, int height) {
    if (!m_flInstance || canResize() != kResultTrue) return;
    constrainEditorDimensions(width, height);
    ViewRect requested(0, 0, width, bridgeEditorHeight(width, height, true));
    if (!m_plugFrame || m_plugFrame->resizeView(this, &requested) != kResultOk) {
        fprintf(stderr, "FloopHole: host rejected native editor resize\n");
        setHintText("Host rejected editor resizing.");
    }
}

void FLBridgePlugView::rebuildPresetMenu() {
    if (!m_presetPopup) return;
    const auto& plugins = FLPluginScanner::getInstance().getPlugins();
    int curPlug = m_flInstance ? m_flInstance->getCurrentPluginIndex() : -1;
    int curPreset = m_flInstance ? m_flInstance->getCurrentPresetIndex() : -1;
    const std::vector<FLPresetEntry> empty;
    [m_presetPopup setPresets:curPlug >= 0 && curPlug < (int)plugins.size() ? plugins[curPlug].presets : empty
        selected:curPreset];
}

void FLBridgePlugView::selectPresetFromMenu(int presetIndex) {
    if (!m_flInstance) return;
    if (m_flInstance->loadPreset(presetIndex)) {
        rebuildPresetMenu();
        const auto& plugins = FLPluginScanner::getInstance().getPlugins();
        int curPlug = m_flInstance->getCurrentPluginIndex();
        if (curPlug >= 0 && curPlug < (int)plugins.size() &&
            presetIndex >= 0 && presetIndex < (int)plugins[curPlug].presets.size()) {
            setHintText("Preset: " + plugins[curPlug].presets[presetIndex].displayName);
        }
    } else {
        setHintText("Could not load preset. See the host log for details.");
    }
}

void FLBridgePlugView::stepPreset(int delta) {
    if (!m_flInstance) return;
    const auto& plugins = FLPluginScanner::getInstance().getPlugins();
    int curPlug = m_flInstance->getCurrentPluginIndex();
    if (curPlug < 0 || curPlug >= (int)plugins.size()) return;
    int count = (int)plugins[curPlug].presets.size();
    if (count <= 0) return;

    int next = m_flInstance->getCurrentPresetIndex() + delta;
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;

    selectPresetFromMenu(next);
    rebuildPresetMenu();
}

void FLBridgePlugView::promptLoadFstFile() {
    if (!m_flInstance) return;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setTitle:@"Load preset or import FLEX pack"];
    [panel setAllowedFileTypes:@[@"fst", @"tbio", @"mrp", @"gmsynth", @"flms", @"sf2", @"dwp", @"dwb", @"flexpack"]];
    [panel setCanChooseFiles:YES];
    [panel setCanChooseDirectories:NO];
    [panel setAllowsMultipleSelection:NO];

    std::string presetDir = FLPluginScanner::getInstance().getFLRootPath() +
                            "Data/Patches/Plugin presets/" +
                            (m_flInstance->isGenerator() ? "Generators/" : "Effects/") +
                            m_flInstance->getPluginName();
    if (m_flInstance->getPluginName() == "FLEX")
        presetDir = FLPluginScanner::getInstance().getFlexDataPath(PI_User) + "Packs";
    [panel setDirectoryURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:presetDir.c_str()]]];

    if ([panel runModal] == NSModalResponseOK && [[panel URLs] count] > 0) {
        NSURL* url = [[panel URLs] firstObject];
        const char* p = [[url path] UTF8String];
        if (p && m_flInstance->loadPresetFromFile(p)) {
            setHintText(FLPluginScanner::isFlexPackFile(p)
                ? "Imported FLEX pack. Use a new FLEX instance to refresh its library."
                : std::string("Loaded ") + [[[url path] lastPathComponent] UTF8String]);
        } else {
            setHintText("Could not load file. Check its format and permissions; existing packs are not overwritten.");
        }
    }
}

void FLBridgePlugView::promptSaveFstFile() {
    if (!m_flInstance || !m_flInstance->isLoaded()) {
        setHintText("Load a plugin before saving a preset.");
        return;
    }
    const int pluginIndex = m_flInstance->getCurrentPluginIndex();
    const auto directory = m_flInstance->preparePresetSaveDirectory();
    if (directory.empty()) {
        setHintText("Could not open the plugin's user preset folder.");
        return;
    }
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.title = @"Save preset as...";
    panel.allowedFileTypes = @[@"fst"];
    panel.allowsOtherFileTypes = NO;
    panel.extensionHidden = NO;
    panel.canCreateDirectories = YES;
    panel.nameFieldStringValue = @"New preset";
    panel.directoryURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:directory.c_str()]
                                   isDirectory:YES];
    if ([panel runModal] != NSModalResponseOK) return;

    NSString* path = panel.URL.path;
    if (path.UTF8String && m_flInstance && m_flInstance->getCurrentPluginIndex() == pluginIndex &&
        m_flInstance->savePresetToFile(path.UTF8String)) {
        rebuildPresetMenu();
        setHintText(std::string("Saved preset: ") + path.lastPathComponent.UTF8String);
    } else {
        setHintText("Could not save preset. Check the filename and folder permissions.");
    }
}

void FLBridgePlugView::onTimerTick() {
    updateNotebookNativeFocus();
    m_volumeKnob.volume = getOutputControlValue();
    if (m_flInstance) {
        m_flInstance->idleEditor();
    }
}

void FLBridgePlugView::refreshToolbarState() {
    m_volumeKnob.volume = getOutputControlValue();
    rebuildPresetMenu();
}

void FLBridgePlugView::setHintText(const std::string& text) {
    if (!m_hintLabel) return;
    NSString* ns = [NSString stringWithUTF8String:text.c_str()];
    if (!ns) ns = @"";
    if ([NSThread isMainThread]) {
        [m_hintLabel setStringValue:ns];
        m_hintLabel.toolTip = ns;
    } else {
        NSTextField* lbl = m_hintLabel;
        dispatch_async(dispatch_get_main_queue(), ^{
            [lbl setStringValue:ns];
            lbl.toolTip = ns;
        });
    }
}

// ============================================================================
// FLBridgeComponent Implementation
// ============================================================================

FLBridgeComponent::FLBridgeComponent(const NativePluginClass& plugin)
    : m_pluginClass(plugin), m_defaultIsEffect(!plugin.isGenerator), m_flInstance(plugin.isGenerator) {
}

FLBridgeComponent::~FLBridgeComponent() {
    if (m_componentHandler) {
        m_componentHandler->release();
        m_componentHandler = nullptr;
    }
}

tresult PLUGIN_API FLBridgeComponent::queryInterface(const TUID _iid, void** obj) {
    if (FUnknownPrivate::iidEqual(_iid, IComponent::iid) ||
        FUnknownPrivate::iidEqual(_iid, IPluginBase::iid) ||
        FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
        addRef();
        *obj = static_cast<IComponent*>(this);
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual(_iid, IAudioProcessor::iid)) {
        addRef();
        *obj = static_cast<IAudioProcessor*>(this);
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual(_iid, IEditController::iid)) {
        addRef();
        *obj = static_cast<IEditController*>(this);
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual(_iid, IEditController2::iid)) {
        addRef();
        *obj = static_cast<IEditController2*>(this);
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual(_iid, IMidiMapping::iid)) {
        addRef();
        *obj = static_cast<IMidiMapping*>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API FLBridgeComponent::initialize(FUnknown* context) {
    if (m_initialized) return kResultOk;
    auto& scanner = FLPluginScanner::getInstance();
    if (!scanner.discover()) return kResultFalse;

    m_flInstance.onParamChangedFromPlug = [this](int paramIdx, double normVal) {
        if (paramIdx >= 0 && paramIdx < kMaxAutomatableParams && m_componentHandler) {
            ParamID pid = (ParamID)paramIdx;
            if ([NSThread isMainThread]) {
                m_componentHandler->beginEdit(pid);
                m_componentHandler->performEdit(pid, normVal);
                m_componentHandler->endEdit(pid);
            }
        }
    };

    const int pluginIndex = scanner.findPluginIndexByIdentifier(m_pluginClass.identifier);
    if (pluginIndex < 0 || !m_flInstance.loadPlugin(pluginIndex)) {
        fprintf(stderr, "FloopHole: cannot initialize %s (%s)\n",
            m_pluginClass.name.c_str(), m_pluginClass.identifier.c_str());
        return kResultFalse;
    }

    m_initialized = true;
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::terminate() {
    m_flInstance.unloadPlugin();
    if (m_componentHandler) {
        m_componentHandler->release();
        m_componentHandler = nullptr;
    }
    m_initialized = false;
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::getControllerClassId(TUID classId) {
    memset(classId, 0, sizeof(TUID));
    return kResultFalse; // Single-component effect (implements IEditController directly)
}

int32 PLUGIN_API FLBridgeComponent::getBusCount(MediaType type, BusDirection dir) {
    if (type == kAudio) {
        if (dir == kOutput) return 1; // Stereo Output
        if (dir == kInput)  return 1; // Stereo Input (active for FX, sidechain/FX-mode for Instrument)
    }
    if (type == kEvent) {
        if (dir == kInput)  return 1; // MIDI Event Input
        if (dir == kOutput) return 0;
    }
    return 0;
}

tresult PLUGIN_API FLBridgeComponent::getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) {
    if (index != 0) return kInvalidArgument;
    if (type == kAudio && dir == kOutput) {
        bus.mediaType = kAudio;
        bus.direction = kOutput;
        bus.channelCount = 2;
        utf8ToString128("Stereo Out", bus.name);
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    if (type == kAudio && dir == kInput) {
        bus.mediaType = kAudio;
        bus.direction = kInput;
        bus.channelCount = 2;
        utf8ToString128("Stereo In", bus.name);
        bus.busType = m_defaultIsEffect ? kMain : kAux;
        bus.flags = m_defaultIsEffect ? BusInfo::kDefaultActive : 0;
        return kResultOk;
    }
    if (type == kEvent && dir == kInput) {
        bus.mediaType = kEvent;
        bus.direction = kInput;
        bus.channelCount = 16;
        utf8ToString128("MIDI In", bus.name);
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    return kInvalidArgument;
}

tresult PLUGIN_API FLBridgeComponent::getRoutingInfo(RoutingInfo& inInfo, RoutingInfo& outInfo) {
    outInfo.mediaType = kAudio;
    outInfo.busIndex = 0;
    outInfo.channel = -1;
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::activateBus(MediaType type, BusDirection dir, int32 index, TBool state) {
    return (index == 0) ? kResultOk : kInvalidArgument;
}

tresult PLUGIN_API FLBridgeComponent::setActive(TBool state) {
    m_flInstance.setEnabled(state);
    if (!state) {
        m_flInstance.allNotesOff();
    }
    return kResultOk;
}

// State format:
// [4 bytes] Magic "FLBR"
// [4 bytes] Version = 5
// [4 bytes] Identity Length + UTF-8 Module ID (versions 1-4 stored the plugin name)
// [4 bytes] Preset Index (int32_t)
// [4 bytes] Raw Plugin State Size + Raw Plugin State Bytes
// Versions 2+: [4 bytes] Channel Sample Path Length + UTF-8 Path
// Version 3: [8 bytes] Normalized Master Volume (double)
// Versions 4/5: [8 bytes] Normalized Instrument Volume or FX Mix (double)
tresult PLUGIN_API FLBridgeComponent::getState(IBStream* state) {
    if (!state) return kInvalidArgument;

    const char magic[4] = {'F', 'L', 'B', 'R'};
    const std::string samplePath = m_flInstance.getChannelSamplePath();
    uint32_t version = 5;
    std::string plugName = m_pluginClass.identifier;
    uint32_t nameLen = (uint32_t)plugName.size();
    int32_t presetIdx = (int32_t)m_flInstance.getCurrentPresetIndex();

    std::vector<uint8_t> rawState;
    m_flInstance.savePluginState(rawState);
    uint32_t stateLen = (uint32_t)rawState.size();

    const auto write = [&](const void* data, int32 size) {
        int32 written = 0;
        return state->write(const_cast<void*>(data), size, &written) == kResultOk && written == size;
    };
    if (!write(magic, 4) || !write(&version, 4) || !write(&nameLen, 4) ||
        (nameLen && !write(plugName.data(), nameLen)) || !write(&presetIdx, 4) || !write(&stateLen, 4) ||
        (stateLen && !write(rawState.data(), stateLen))) return kResultFalse;
    if (version >= 2) {
        const uint32_t length = static_cast<uint32_t>(samplePath.size());
        if (!write(&length, 4) || !write(samplePath.data(), length)) return kResultFalse;
    }
    const double volume = m_flInstance.getOutputControlValue();
    if (!write(&volume, sizeof(volume))) return kResultFalse;
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::setState(IBStream* state) {
    if (!state) return kInvalidArgument;

    char magic[4] = {0};
    int32 numRead = 0;
    if (state->read(magic, 4, &numRead) != kResultOk || numRead != 4 || memcmp(magic, "FLBR", 4) != 0) {
        fprintf(stderr, "FloopHole: missing or invalid project state header\n");
        return kResultFalse;
    }

    uint32_t version = 0;
    uint32_t nameLen = 0;
    const auto read = [&](void* data, int32 size) {
        return state->read(data, size, &numRead) == kResultOk && numRead == size;
    };
    if (!read(&version, 4) || version < 1 || version > 5) return kResultFalse;
    if (!read(&nameLen, 4) || nameLen > (version >= 5 ? 4096u : 256u)) return kResultFalse;

    std::string plugName(nameLen, '\0');
    if (nameLen > 0) {
        if (!read(&plugName[0], nameLen)) return kResultFalse;
    }

    int32_t presetIdx = -1;
    uint32_t stateLen = 0;
    if (!read(&presetIdx, 4) || !read(&stateLen, 4) || stateLen >= 64 * 1024 * 1024) return kResultFalse;

    std::vector<uint8_t> rawState;
    if (stateLen > 0 && stateLen < 64 * 1024 * 1024) {
        rawState.resize(stateLen);
        if (!read(rawState.data(), stateLen)) return kResultFalse;
    }
    std::string samplePath;
    if (version >= 2) {
        uint32_t length = 0;
        if (!read(&length, 4) || length >= 256) return kResultFalse;
        samplePath.resize(length);
        if (length && !read(samplePath.data(), length)) return kResultFalse;
        if (samplePath.find('\0') != std::string::npos) return kResultFalse;
    }
    double volume = m_flInstance.getOutputControlDefault();
    if (version >= 3 && (!read(&volume, sizeof(volume)) || !std::isfinite(volume) || volume < 0 || volume > 1)) {
        fprintf(stderr, "FloopHole: invalid output control in project state\n");
        return kResultFalse;
    }
    if ((version >= 5 && plugName != m_pluginClass.identifier) ||
        (version < 5 && !plugName.empty() && plugName != m_pluginClass.name)) {
        fprintf(stderr, "FloopHole: rejecting state for '%s' in fixed class '%s'\n",
            plugName.c_str(), m_pluginClass.identifier.c_str());
        return kResultFalse;
    }
    if (!m_flInstance.isLoaded()) {
        const int index = FLPluginScanner::getInstance().findPluginIndexByIdentifier(m_pluginClass.identifier);
        if (index < 0 || !m_flInstance.loadPlugin(index)) return kResultFalse;
    }
    if (m_defaultIsEffect && version <= 3) volume = kDefaultEffectMix;
    if (!m_flInstance.setOutputControlValue(volume)) return kResultFalse;

    if (!rawState.empty()) {
        if (!m_flInstance.restorePluginState(rawState.data(), rawState.size())) return kResultFalse;
    } else if (presetIdx >= 0) {
        if (!m_flInstance.loadPreset(presetIdx)) return kResultFalse;
    }
    if (samplePath.empty()) m_flInstance.clearChannelSample();
    else if (!m_flInstance.loadChannelSample(samplePath)) return kResultFalse;

    if (m_activeView) {
        m_activeView->refreshToolbarState();
    }
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::setComponentState(IBStream* state) {
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                                         SpeakerArrangement* outputs, int32 numOuts) {
    if (numOuts >= 1 && outputs[0] != SpeakerArr::kStereo) {
        return kResultFalse;
    }
    if (numIns >= 1 && inputs[0] != SpeakerArr::kStereo && inputs[0] != SpeakerArr::kEmpty) {
        return kResultFalse;
    }
    return kResultTrue;
}

tresult PLUGIN_API FLBridgeComponent::getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) {
    if (index != 0) return kInvalidArgument;
    arr = SpeakerArr::kStereo;
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::canProcessSampleSize(int32 symbolicSampleSize) {
    return (symbolicSampleSize == kSample32) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API FLBridgeComponent::setupProcessing(ProcessSetup& setup) {
    if (!std::isfinite(setup.sampleRate) || setup.sampleRate <= 0) {
        fprintf(stderr, "FloopHole: invalid processing sample rate\n");
        return kInvalidArgument;
    }
    m_transportSampleRate = setup.sampleRate;
    m_flInstance.setSampleRate(setup.sampleRate);
    m_flInstance.setBlockSize(setup.maxSamplesPerBlock);
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::setProcessing(TBool state) {
    if (!state) {
        m_flInstance.allNotesOff();
    }
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::process(ProcessData& data) {
    // 1. Process incoming parameter changes
    if (data.inputParameterChanges) {
        int32 numParamChanged = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < numParamChanged; ++i) {
            IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
            if (!queue) continue;
            int32 pts = queue->getPointCount();
            if (pts <= 0) continue;
            int32 sampleOffset = 0;
            ParamValue value = 0.0;
            if (queue->getPoint(pts - 1, sampleOffset, value) == kResultTrue) {
                ParamID pid = queue->getParameterId();
                if (pid < kMaxAutomatableParams) {
                    m_flInstance.setParamNormalized((int)pid, value, false);
                } else if (pid == kParamPitchBend) {
                    m_pitchBendNorm = value;
                    float semitones = (float)((value - 0.5) * 4.0); // +/- 2 semitones
                    m_flInstance.setPitchBendSemitones(semitones);
                } else if (pid == getOutputControlID()) {
                    if (!m_flInstance.setOutputControlValue(value)) return kInvalidArgument;
                } else if (pid == kParamModWheel) {
                    m_modWheelNorm = value;
                    uint8_t ccVal = (uint8_t)std::clamp((int)std::lround(value * 127.0), 0, 127);
                    m_flInstance.sendMIDIMessage(0xB0, 1, ccVal);
                } else if (pid == kParamSustain) {
                    m_sustainNorm = value;
                    uint8_t ccVal = (uint8_t)(value >= 0.5 ? 127 : 0);
                    m_flInstance.sendMIDIMessage(0xB0, 64, ccVal);
                }
            }
        }
    }

    // 2. Process incoming MIDI Note Events
    if (data.inputEvents) {
        int32 eventCount = data.inputEvents->getEventCount();
        Event e;
        for (int32 i = 0; i < eventCount; ++i) {
            if (data.inputEvents->getEvent(i, e) != kResultOk) continue;
            if (e.type == Event::kNoteOnEvent) {
                if (e.noteOn.velocity > 0.0f) {
                    m_flInstance.noteOn(e.noteOn.channel, e.noteOn.pitch, e.noteOn.velocity);
                } else {
                    m_flInstance.noteOff(e.noteOn.channel, e.noteOn.pitch, 0.5f);
                }
            } else if (e.type == Event::kNoteOffEvent) {
                m_flInstance.noteOff(e.noteOff.channel, e.noteOff.pitch, e.noteOff.velocity);
            }
        }
    }

    // 3. Process Audio
    if (data.numSamples <= 0 || data.numOutputs <= 0 || !data.outputs) {
        return kResultOk;
    }

    float* outL = (data.outputs[0].numChannels >= 1) ? data.outputs[0].channelBuffers32[0] : nullptr;
    float* outR = (data.outputs[0].numChannels >= 2) ? data.outputs[0].channelBuffers32[1] : nullptr;

    const float* inL = nullptr;
    const float* inR = nullptr;
    if (data.numInputs > 0 && data.inputs && data.inputs[0].channelBuffers32) {
        if (data.inputs[0].numChannels >= 1) inL = data.inputs[0].channelBuffers32[0];
        if (data.inputs[0].numChannels >= 2) inR = data.inputs[0].channelBuffers32[1];
    }

    double tempo = m_flInstance.getTempoBPM();
    double ppq = m_nextProjectQuarters;
    double seconds = m_nextProjectSeconds;
    bool playing = false;
    if (data.processContext) {
        if (data.processContext->state & ProcessContext::kTempoValid) {
            if (std::isfinite(data.processContext->tempo) && data.processContext->tempo > 0)
                tempo = data.processContext->tempo;
            else fprintf(stderr, "FloopHole: invalid host tempo; retaining the previous tempo\n");
        }
        if ((data.processContext->state & ProcessContext::kProjectTimeMusicValid) &&
            std::isfinite(data.processContext->projectTimeMusic)) {
            ppq = data.processContext->projectTimeMusic;
        } else {
            if (data.processContext->state & ProcessContext::kProjectTimeMusicValid)
                fprintf(stderr, "FloopHole: invalid host musical position; extrapolating from its sample timeline\n");
            ppq += (data.processContext->projectTimeSamples / m_transportSampleRate - seconds) * tempo / 60.0;
        }
        seconds = data.processContext->projectTimeSamples / m_transportSampleRate;
        if (data.processContext->state & ProcessContext::kTimeSigValid) {
            m_timeSigNumerator = data.processContext->timeSigNumerator;
            m_timeSigDenominator = data.processContext->timeSigDenominator;
        }
        playing = (data.processContext->state & ProcessContext::kPlaying) != 0;
    }

    m_flInstance.processAudio(inL, inR, outL, outR, data.numSamples, tempo, ppq, playing,
                              seconds, m_timeSigNumerator, m_timeSigDenominator);
    const double elapsed = playing ? data.numSamples / m_transportSampleRate : 0.0;
    m_nextProjectQuarters = ppq + elapsed * tempo / 60.0;
    m_nextProjectSeconds = seconds + elapsed;
    data.outputs[0].silenceFlags = 0;
    return kResultOk;
}

int32 PLUGIN_API FLBridgeComponent::getParameterCount() {
    return kMaxAutomatableParams + 5;
}

tresult PLUGIN_API FLBridgeComponent::getParameterInfo(int32 paramIndex, ParameterInfo& info) {
    if (paramIndex < 0 || paramIndex >= kMaxAutomatableParams + 5) return kInvalidArgument;
    memset(&info, 0, sizeof(ParameterInfo));
    info.unitId = kRootUnitId;
    if (paramIndex == kMaxAutomatableParams + 4) {
        info.id = getOutputControlID();
        utf8ToString128(m_defaultIsEffect ? "Effect Mix" : "Master Volume", info.title);
        utf8ToString128(m_defaultIsEffect ? "Mix" : "Volume", info.shortTitle);
        info.defaultNormalizedValue = m_flInstance.getOutputControlDefault();
        info.flags = ParameterInfo::kCanAutomate;
        return kResultOk;
    }

    // Append new native slots after the original native and bridge controls.
    if (paramIndex < kLegacyAutomatableParams || paramIndex >= kLegacyAutomatableParams + 4) {
        const int32 nativeIndex = paramIndex < kLegacyAutomatableParams ? paramIndex : paramIndex - 4;
        info.id = (ParamID)nativeIndex;
        std::string pname = m_flInstance.getParamName(nativeIndex);
        if (pname.empty()) {
            char fallback[64];
            snprintf(fallback, sizeof(fallback), "Param %d", nativeIndex + 1);
            pname = fallback;
        }
        utf8ToString128(pname, info.title);
        utf8ToString128(pname.substr(0, 16), info.shortTitle);
        info.stepCount = 0;
        info.defaultNormalizedValue = m_flInstance.getParamNormalized(nativeIndex);
        info.flags = ParameterInfo::kCanAutomate;
        return kResultOk;
    }

    int extra = paramIndex - kLegacyAutomatableParams;
    if (extra == 0) {
        info.id = kParamPitchBend;
        utf8ToString128("Pitch Bend", info.title);
        utf8ToString128("PitchBend", info.shortTitle);
        info.defaultNormalizedValue = 0.5;
        info.flags = ParameterInfo::kCanAutomate;
    } else if (extra == 1) {
        info.id = kParamModWheel;
        utf8ToString128("Mod Wheel", info.title);
        utf8ToString128("ModWheel", info.shortTitle);
        info.defaultNormalizedValue = 0.0;
        info.flags = ParameterInfo::kCanAutomate;
    } else if (extra == 2) {
        info.id = kParamAftertouch;
        utf8ToString128("Aftertouch", info.title);
        utf8ToString128("ATouch", info.shortTitle);
        info.defaultNormalizedValue = 0.0;
        info.flags = ParameterInfo::kCanAutomate;
    } else if (extra == 3) {
        info.id = kParamSustain;
        utf8ToString128("Sustain Pedal", info.title);
        utf8ToString128("Sustain", info.shortTitle);
        info.stepCount = 1;
        info.defaultNormalizedValue = 0.0;
        info.flags = ParameterInfo::kCanAutomate;
    }
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::getParamStringByValue(ParamID id, ParamValue valueNormalized, String128 string) {
    if (id == getOutputControlID()) {
        if (!std::isfinite(valueNormalized) || valueNormalized < 0 || valueNormalized > 1) {
            fprintf(stderr, "FloopHole: invalid output control display value\n");
            return kInvalidArgument;
        }
        utf8ToString128(m_flInstance.getOutputControlDisplay(valueNormalized), string);
        return kResultOk;
    }
    if (id < kMaxAutomatableParams) {
        std::string disp = m_flInstance.getParamDisplay((int)id);
        if (!disp.empty()) {
            utf8ToString128(disp, string);
            return kResultOk;
        }
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f%%", valueNormalized * 100.0);
    utf8ToString128(buf, string);
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::getParamValueByString(ParamID id, TChar* string, ParamValue& valueNormalized) {
    return kResultFalse;
}

ParamValue PLUGIN_API FLBridgeComponent::getParamNormalized(ParamID id) {
    if (id == getOutputControlID()) return m_flInstance.getOutputControlValue();
    if (id < kMaxAutomatableParams) {
        return m_flInstance.getParamNormalized((int)id);
    }
    if (id == kParamPitchBend)  return m_pitchBendNorm;
    if (id == kParamModWheel)   return m_modWheelNorm;
    if (id == kParamAftertouch) return m_aftertouchNorm;
    if (id == kParamSustain)    return m_sustainNorm;
    return 0.0;
}

tresult PLUGIN_API FLBridgeComponent::setParamNormalized(ParamID id, ParamValue value) {
    if (id == getOutputControlID()) return m_flInstance.setOutputControlValue(value) ? kResultOk : kInvalidArgument;
    if (id < kMaxAutomatableParams) {
        m_flInstance.setParamNormalized((int)id, value, true);
        return kResultOk;
    }
    if (id == kParamPitchBend) {
        m_pitchBendNorm = value;
        m_flInstance.setPitchBendSemitones((float)((value - 0.5) * 4.0));
        return kResultOk;
    }
    if (id == kParamModWheel)   { m_modWheelNorm = value; return kResultOk; }
    if (id == kParamAftertouch) { m_aftertouchNorm = value; return kResultOk; }
    if (id == kParamSustain)    { m_sustainNorm = value; return kResultOk; }
    return kResultFalse;
}

tresult PLUGIN_API FLBridgeComponent::setComponentHandler(IComponentHandler* handler) {
    if (m_componentHandler == handler) return kResultOk;
    if (m_componentHandler) m_componentHandler->release();
    m_componentHandler = handler;
    if (m_componentHandler) m_componentHandler->addRef();
    return kResultOk;
}

void FLBridgeComponent::editOutputControl(double value, bool begin, bool end) {
    if (!m_flInstance.setOutputControlValue(value)) return;
    if (m_componentHandler) {
        if (begin) m_componentHandler->beginEdit(getOutputControlID());
        m_componentHandler->performEdit(getOutputControlID(), value);
        if (end) m_componentHandler->endEdit(getOutputControlID());
    }
}

void FLBridgePlugView::editOutputControl(double value, bool begin, bool end) {
    m_owner->editOutputControl(value, begin, end);
    setHintText(getOutputControlHint());
}

IPlugView* PLUGIN_API FLBridgeComponent::createView(FIDString name) {
    if (name && strcmp(name, ViewType::kEditor) == 0) {
        auto* view = new FLBridgePlugView(this, &m_flInstance);
        m_activeView = view;
        return view;
    }
    return nullptr;
}

tresult PLUGIN_API FLBridgeComponent::getMidiControllerAssignment(int32 busIndex, int16 channel,
                                                                  CtrlNumber midiControllerNumber, ParamID& id) {
    if (busIndex != 0) return kResultFalse;
    switch (midiControllerNumber) {
        case kPitchBend:
            id = kParamPitchBend;
            return kResultTrue;
        case kCtrlModWheel:
            id = kParamModWheel;
            return kResultTrue;
        case kAfterTouch:
            id = kParamAftertouch;
            return kResultTrue;
        case kCtrlSustainOnOff:
            id = kParamSustain;
            return kResultTrue;
        default:
            return kResultFalse;
    }
}


} // namespace flbr

// ============================================================================
// VST3 Bundle Entry Points (macOS)
// ============================================================================

extern "C" {

SMTG_EXPORT_SYMBOL bool bundleEntry(CFBundleRef) {
    flbr::FLPluginScanner::getInstance().discover(true);
    return true;
}

SMTG_EXPORT_SYMBOL bool bundleExit() {
    return true;
}

SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory() {
    return new flbr::FloopHoleFactory<flbr::FLBridgeComponent>();
}

} // extern "C"
