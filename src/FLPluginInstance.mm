#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#import <AudioToolbox/AudioToolbox.h>
#include "FLPluginInstance.h"
#include "FLPathManager.h"
#include <dlfcn.h>
#include <stdio.h>
#include <cmath>
#include <algorithm>

@interface NSWindow (FLBridgeMacVCLHosting)
- (NSView*)savedContentView;
- (void)updateParent;
- (void)setParent:(NSWindow*)parent;
- (BOOL)isCustomWindowParented;
@end

@interface NSObject (FLBridgeMacVCLWindow)
- (NSWindow*)window;
- (BOOL)isNSViewHosted;
- (BOOL)sendMouseEvent:(NSEvent*)event;
@end

namespace flbr {

struct MacVCLPropWriter {
    void* data;
    int size;
};

static NSView* hostedEditorForWindow(NSWindow* window) {
    id windowWrapper = window.delegate;
    // Disposed auxiliary windows can keep an unowned savedContentView pointer.
    if (![windowWrapper respondsToSelector:@selector(isNSViewHosted)] ||
        ![windowWrapper isNSViewHosted] ||
        ![window respondsToSelector:@selector(savedContentView)]) return nil;
    return [window savedContentView];
}

static void hostedWindowGetProp(id windowWrapper, SEL selector, int property, int index, MacVCLPropWriter* writer) {
    Class originalClass = class_getSuperclass(object_getClass(windowWrapper));
    auto original = reinterpret_cast<void (*)(id, SEL, int, int, MacVCLPropWriter*)>(
        class_getMethodImplementation(originalClass, selector));
    original(windowWrapper, selector, property, index, writer);
    if (property != 1001 || !writer || writer->size != sizeof(NSRect) || ![windowWrapper isNSViewHosted]) return;

    NSWindow* backingWindow = [windowWrapper window];
    NSView* editor = [backingWindow savedContentView];
    if (!editor.isFlipped || !editor.superview || !editor.window || editor.window == backingWindow) return;
    NSRect screenRect = [editor.window convertRectToScreen:
        [editor.superview convertRect:editor.frame toView:nil]];
    if (screenRect.size.width <= 0) return;
    NSRect* rect = static_cast<NSRect*>(writer->data);
    const double scale = rect->size.width / screenRect.size.width;
    const double expectedY = (NSMaxY(NSScreen.screens.firstObject.frame) - NSMaxY(screenRect)) * scale;
    if (std::abs(rect->origin.y + rect->size.height - expectedY) < 0.5) {
        rect->origin.y += rect->size.height;
    }
}

static Class hostedWindowClass() {
    static Class subclass = [] {
        Class existing = NSClassFromString(@"FLBridgeHostedMVWindow");
        if (existing) return existing;
        Class original = NSClassFromString(@"MVWindow");
        if (!original) return static_cast<Class>(Nil);
        SEL selector = NSSelectorFromString(@"getProp:withIndex:writer:");
        Method method = class_getInstanceMethod(original, selector);
        if (!method) return static_cast<Class>(Nil);
        Class created = objc_allocateClassPair(original, "FLBridgeHostedMVWindow", 0);
        if (!created) return static_cast<Class>(Nil);
        class_addMethod(created, selector, reinterpret_cast<IMP>(hostedWindowGetProp), method_getTypeEncoding(method));
        objc_registerClassPair(created);
        return created;
    }();
    return subclass;
}

static char stagingWindowKey;
static char vstWindowsKey;

static void installVstWindowSafeHandlers() {
    Class vstWindowClass = NSClassFromString(@"VstWindow");
    if (!vstWindowClass) return;
    static bool installed = false;
    if (installed) return;
    installed = true;
    for (SEL selector : {@selector(windowDidBecomeKey:), @selector(windowDidResignKey:)}) {
        Method method = class_getInstanceMethod(vstWindowClass, selector);
        if (!method) continue;
        auto original = reinterpret_cast<void (*)(id, SEL, NSNotification*)>(
            method_getImplementation(method));
        IMP safeImp = imp_implementationWithBlock(^(NSWindow* window, NSNotification* notification) {
            if ([window respondsToSelector:@selector(isCustomWindowParented)] &&
                [window isCustomWindowParented]) {
                original(window, selector, notification);
            }
        });
        method_setImplementation(method, safeImp);
    }
    for (SEL selector : {@selector(draggingEntered:), @selector(draggingUpdated:)}) {
        Method method = class_getInstanceMethod(vstWindowClass, selector);
        if (!method) continue;
        auto original = reinterpret_cast<NSDragOperation (*)(id, SEL, id<NSDraggingInfo>)>(
            method_getImplementation(method));
        IMP safeImp = imp_implementationWithBlock(^NSDragOperation(NSWindow* window, id<NSDraggingInfo> sender) {
            if ([window respondsToSelector:@selector(isCustomWindowParented)] &&
                [window isCustomWindowParented]) {
                return original(window, selector, sender);
            }
            NSView* content = window.contentView;
            return [content respondsToSelector:selector]
                ? reinterpret_cast<NSDragOperation (*)(id, SEL, id<NSDraggingInfo>)>(
                    [content methodForSelector:selector])(content, selector, sender)
                : NSDragOperationNone;
        });
        method_setImplementation(method, safeImp);
    }
    for (SEL selector : {@selector(prepareForDragOperation:), @selector(performDragOperation:)}) {
        Method method = class_getInstanceMethod(vstWindowClass, selector);
        if (!method) continue;
        auto original = reinterpret_cast<BOOL (*)(id, SEL, id<NSDraggingInfo>)>(
            method_getImplementation(method));
        IMP safeImp = imp_implementationWithBlock(^BOOL(NSWindow* window, id<NSDraggingInfo> sender) {
            if ([window respondsToSelector:@selector(isCustomWindowParented)] &&
                [window isCustomWindowParented]) {
                return original(window, selector, sender);
            }
            NSView* content = window.contentView;
            return [content respondsToSelector:selector] &&
                reinterpret_cast<BOOL (*)(id, SEL, id<NSDraggingInfo>)>(
                    [content methodForSelector:selector])(content, selector, sender);
        });
        method_setImplementation(method, safeImp);
    }
}

static void synchronizeEditorWindow(NSView* parentView) {
    NSWindow* hostWindow = parentView.window;
    if (!hostWindow) return;

    NSWindow* stagingWindow = objc_getAssociatedObject(parentView, &stagingWindowKey);
    if (stagingWindow && hostWindow != stagingWindow) {
        [stagingWindow close];
        objc_setAssociatedObject(parentView, &stagingWindowKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }

    NSHashTable<NSWindow*>* vstWindows = objc_getAssociatedObject(parentView, &vstWindowsKey);
    for (NSWindow* vstWindow in vstWindows.allObjects) {
        if (hostWindow != stagingWindow && vstWindow.parentWindow != hostWindow) {
            if (vstWindow.parentWindow) [vstWindow.parentWindow removeChildWindow:vstWindow];
            if ([vstWindow respondsToSelector:@selector(setParent:)]) [vstWindow setParent:hostWindow];
            if ([vstWindow respondsToSelector:@selector(updateParent)]) [vstWindow updateParent];
        }
    }

    for (NSWindow* backingWindow in NSApp.windows) {
        if (backingWindow == hostWindow) continue;
        NSView* editor = hostedEditorForWindow(backingWindow);
        if (!editor || editor.superview != parentView) continue;

        id<NSWindowDelegate> delegate = backingWindow.delegate;
        Class subclass = hostedWindowClass();
        const bool install = subclass && object_getClass(delegate) == class_getSuperclass(subclass);
        if (install) object_setClass(delegate, subclass);
        NSRect screenRect = [hostWindow convertRectToScreen:
            [parentView convertRect:editor.frame toView:nil]];
        NSRect frame = [backingWindow frameRectForContentRect:screenRect];
        if (!install && NSEqualRects(frame, backingWindow.frame)) continue;

        [backingWindow setFrame:frame display:NO];
        if ([delegate respondsToSelector:@selector(windowDidMove:)]) {
            [delegate windowDidMove:[NSNotification notificationWithName:NSWindowDidMoveNotification
                                                                 object:backingWindow]];
        }
    }
}

static double editorCoordinateScale(NSView* editor) {
    for (NSWindow* window in NSApp.windows) {
        if (hostedEditorForWindow(window) != editor) continue;
        id delegate = window.delegate;
        SEL selector = NSSelectorFromString(@"getProp:withIndex:writer:");
        if (![delegate respondsToSelector:selector]) continue;
        NSRect rectangle{};
        MacVCLPropWriter writer{&rectangle, sizeof(rectangle)};
        reinterpret_cast<void(*)(id, SEL, int, int, MacVCLPropWriter*)>(
            [delegate methodForSelector:selector])(delegate, selector, 1001, 0, &writer);
        const double scale = rectangle.size.width / editor.frame.size.width;
        if (writer.size == sizeof(rectangle) && std::isfinite(scale) && scale > 0 &&
            std::abs(rectangle.size.height / editor.frame.size.height - scale) < 1e-6)
            return scale;
    }
    return 1.0;
}

static NSView* nativeEditorView(NSView* parent) {
    for (NSView* view in parent.subviews) {
        if (![view isKindOfClass:NSControl.class] && view.frame.size.width > 0 && view.frame.size.height > 0)
            return view;
    }
    return nil;
}

static void invalidateNativeEditor(NSView* view) {
    const SEL invalidate = NSSelectorFromString(@"invalidate");
    if ([view respondsToSelector:invalidate])
        reinterpret_cast<void(*)(id, SEL)>([view methodForSelector:invalidate])(view, invalidate);
    else [view setNeedsDisplay:YES];
    for (NSView* child in view.subviews) invalidateNativeEditor(child);
}

static void resizeNativeEditor(NSView* editor, NSSize size) {
    const NSSize oldSize = editor.frame.size;
    struct Viewport { NSView* view; NSRect frame; };
    std::vector<Viewport> viewports;
    Class nativeClass = NSClassFromString(@"D1NSView");
    for (NSView* child in editor.subviews) {
        if (nativeClass && [child isKindOfClass:nativeClass] &&
            std::abs(child.frame.size.width - oldSize.width) <= 1 &&
            std::abs(child.frame.size.height - oldSize.height) <= 1)
            viewports.push_back({child, child.frame});
    }
    [editor setFrame:NSMakeRect(editor.frame.origin.x, editor.frame.origin.y, size.width, size.height)];
    // MacVCL's hosted form has a separate, full-client D1 drawing viewport.
    for (const auto& viewport : viewports) {
        NSRect frame = viewport.frame;
        if (std::abs(frame.origin.x) > 1) frame.origin.x = 0;
        if (std::abs(frame.origin.y) > 1) frame.origin.y = 0;
        frame.size.width += size.width - oldSize.width;
        frame.size.height += size.height - oldSize.height;
        [viewport.view setFrame:frame];
    }
    synchronizeEditorWindow(editor.superview);
    for (NSWindow* backing in NSApp.windows) {
        if (hostedEditorForWindow(backing) == editor &&
            [backing.delegate respondsToSelector:@selector(windowDidResize:)])
            [backing.delegate windowDidResize:[NSNotification notificationWithName:NSWindowDidResizeNotification object:backing]];
    }
    invalidateNativeEditor(editor);
}

static char fileDropOwnerKey;
static char fileDropTypesKey;
static char fileDropViewsKey;

static Class hostedNativeViewClass(id view, SEL) {
    return class_getSuperclass(object_getClass(view));
}

static NSURL* hostedDropURL(NSView* view, id<NSDraggingInfo> sender) {
    auto* owner = static_cast<FLPluginInstance*>(
        [objc_getAssociatedObject(view, &fileDropOwnerKey) pointerValue]);
    if (!owner) return nil;
    NSArray<NSURL*>* urls = [sender.draggingPasteboard readObjectsForClasses:@[[NSURL class]]
        options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    if (urls.count != 1) return nil;
    NSURL* url = urls.firstObject;
    NSString* extension = url.pathExtension.lowercaseString;
    NSArray<NSString*>* types = owner->getPluginName() == "Harmor"
        ? @[@"png", @"jpg", @"jpeg", @"bmp"]
        : @[@"wav", @"aif", @"aiff", @"mp3", @"flac", @"ogg", @"m4a", @"wma"];
    return [types containsObject:extension] ? url : nil;
}

static NSDragOperation hostedDraggingEntered(NSView* view, SEL selector, id<NSDraggingInfo> sender) {
    if (hostedDropURL(view, sender)) return NSDragOperationCopy;
    Method original = class_getInstanceMethod(class_getSuperclass(object_getClass(view)), selector);
    return original ? reinterpret_cast<NSDragOperation (*)(id, SEL, id)>(method_getImplementation(original))(
        view, selector, sender) : NSDragOperationNone;
}

static BOOL hostedPrepareDrop(NSView* view, SEL selector, id<NSDraggingInfo> sender) {
    if (hostedDropURL(view, sender)) return YES;
    Method original = class_getInstanceMethod(class_getSuperclass(object_getClass(view)), selector);
    return original && reinterpret_cast<BOOL (*)(id, SEL, id)>(method_getImplementation(original))(
        view, selector, sender);
}

static BOOL hostedPerformDrop(NSView* view, SEL selector, id<NSDraggingInfo> sender) {
    if (NSURL* url = hostedDropURL(view, sender)) {
        auto* owner = static_cast<FLPluginInstance*>(
            [objc_getAssociatedObject(view, &fileDropOwnerKey) pointerValue]);
        if (owner->getPluginName() == "Harmor" && ![[NSImage alloc] initWithContentsOfURL:url]) {
            fprintf(stderr, "FloopHole: invalid dropped image %s\n", url.path.UTF8String);
            return NO;
        }
        return owner->loadDroppedFile(url.path.UTF8String);
    }
    Method original = class_getInstanceMethod(class_getSuperclass(object_getClass(view)), selector);
    return original && reinterpret_cast<BOOL (*)(id, SEL, id)>(method_getImplementation(original))(
        view, selector, sender);
}

static void installFileDropHandler(NSView* view, FLPluginInstance* owner, NSHashTable<NSView*>* views, bool root = true) {
    for (NSView* child in view.subviews) {
        installFileDropHandler(child, owner, views, false);
    }
    if ((!root && !view.registeredDraggedTypes.count) || objc_getAssociatedObject(view, &fileDropOwnerKey)) return;
    Class original = object_getClass(view);
    NSString* name = [@"FLBridgeFileDrop_" stringByAppendingString:NSStringFromClass(original)];
    Class subclass = NSClassFromString(name);
    if (!subclass) {
        subclass = objc_allocateClassPair(original, name.UTF8String, 0);
        if (!subclass) {
            fprintf(stderr, "FloopHole: cannot install native editor file-drop handler\n");
            return;
        }
        // MacVCL identifies controls by their reported native class.
        class_addMethod(subclass, @selector(class), reinterpret_cast<IMP>(hostedNativeViewClass), "#@:");
        for (auto method : {
            std::make_pair(@selector(draggingEntered:), reinterpret_cast<IMP>(hostedDraggingEntered)),
            std::make_pair(@selector(draggingUpdated:), reinterpret_cast<IMP>(hostedDraggingEntered)),
            std::make_pair(@selector(prepareForDragOperation:), reinterpret_cast<IMP>(hostedPrepareDrop)),
            std::make_pair(@selector(performDragOperation:), reinterpret_cast<IMP>(hostedPerformDrop))}) {
            Method inherited = class_getInstanceMethod(original, method.first);
            class_addMethod(subclass, method.first, method.second, inherited ? method_getTypeEncoding(inherited) :
                (method.first == @selector(draggingEntered:) || method.first == @selector(draggingUpdated:)
                    ? "Q@:@" : (strcmp(@encode(BOOL), "B") == 0 ? "B@:@" : "c@:@")));
        }
        objc_registerClassPair(subclass);
    }
    objc_setAssociatedObject(view, &fileDropOwnerKey, [NSValue valueWithPointer:owner], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(view, &fileDropTypesKey, view.registeredDraggedTypes, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    NSMutableArray* types = [view.registeredDraggedTypes mutableCopy];
    if (!types) types = [NSMutableArray array];
    if (![types containsObject:NSPasteboardTypeFileURL]) [types addObject:NSPasteboardTypeFileURL];
    [view registerForDraggedTypes:types];
    object_setClass(view, subclass);
    [views addObject:view];
}

static void removeFileDropHandler(NSView* view) {
    if (!objc_getAssociatedObject(view, &fileDropOwnerKey)) return;
    object_setClass(view, class_getSuperclass(object_getClass(view)));
    [view registerForDraggedTypes:objc_getAssociatedObject(view, &fileDropTypesKey) ?: @[]];
    objc_setAssociatedObject(view, &fileDropOwnerKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(view, &fileDropTypesKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

static char scrollWindowKey;
static char scrollViewsKey;

static void hostedScrollWheel(NSView* view, SEL selector, NSEvent* event) {
    NSHashTable* windowReference = objc_getAssociatedObject(view, &scrollWindowKey);
    id windowWrapper = windowReference.anyObject;
    if ([windowWrapper sendMouseEvent:event]) return;
    auto original = reinterpret_cast<void (*)(id, SEL, NSEvent*)>(
        class_getMethodImplementation(class_getSuperclass(object_getClass(view)), selector));
    original(view, selector, event);
}

static void installScrollWheelHandler(NSView* view, NSHashTable* windowReference, NSHashTable<NSView*>* views) {
    for (NSView* child in view.subviews) installScrollWheelHandler(child, windowReference, views);
    Class nativeClass = NSClassFromString(@"D1NSView");
    if (!nativeClass || ![view isKindOfClass:nativeClass] ||
        objc_getAssociatedObject(view, &scrollWindowKey)) return;
    Class original = object_getClass(view);
    NSString* name = [@"FLBridgeScrollWheel_" stringByAppendingString:NSStringFromClass(original)];
    Class subclass = NSClassFromString(name);
    if (!subclass) {
        subclass = objc_allocateClassPair(original, name.UTF8String, 0);
        if (!subclass) {
            fprintf(stderr, "FloopHole: cannot install native editor scroll-wheel handler\n");
            return;
        }
        class_addMethod(subclass, @selector(class), reinterpret_cast<IMP>(hostedNativeViewClass), "#@:");
        Method method = class_getInstanceMethod(original, @selector(scrollWheel:));
        class_addMethod(subclass, @selector(scrollWheel:), reinterpret_cast<IMP>(hostedScrollWheel),
                        method_getTypeEncoding(method));
        objc_registerClassPair(subclass);
    }
    objc_setAssociatedObject(view, &scrollWindowKey, windowReference, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    object_setClass(view, subclass);
    [views addObject:view];
}

// ============================================================================
// Global Shared WaveTables, TempBuffers, and IPathManager
// ============================================================================

static float gSharedWaveTables[10][16384];
static float gSharedTempBuffers[4][8192 * 2];
static std::once_flag gInitTablesOnce;
static thread_local FLPluginInstance* gCurrentLoadingInstance = nullptr;

static bool traceHostEnabled() {
    static const bool enabled = std::getenv("FLBR_TRACE_HOST") != nullptr;
    return enabled;
}

static void initSharedTables() {
    for (int w = 0; w < 10; ++w) {
        for (int s = 0; s < 16384; ++s) {
            double ph = (double)s / 16384.0;
            if (w == 0) {
                gSharedWaveTables[w][s] = (float)std::sin(ph * 2.0 * M_PI);
            } else if (w == 1) {
                gSharedWaveTables[w][s] = (float)(ph < 0.5 ? (ph * 4.0 - 1.0) : (3.0 - ph * 4.0));
            } else if (w == 2) {
                gSharedWaveTables[w][s] = (float)(ph < 0.5 ? 1.0 : -1.0);
            } else {
                gSharedWaveTables[w][s] = (float)(2.0 * ph - 1.0);
            }
        }
    }
}

static FLPathManager gSharedPathMgr;

// ============================================================================
// Static Host Trampolines
// ============================================================================

struct HostLayout {
    void* vmtPtr;
    int32_t HostVersion;
    int32_t Flags;
    void* AppHandle;
    float* WaveTables[10];
    float* TempBuffers[4];
    int32_t Reserved[30];
    FLPluginInstance* owner;
};

static inline FLPluginInstance* getOwner(void* self) {
    if (!self) return gCurrentLoadingInstance;
    auto* hl = reinterpret_cast<HostLayout*>(self);
    return hl->owner ? hl->owner : gCurrentLoadingInstance;
}

static intptr_t Trampoline_Dispatcher(void* self, intptr_t sender, intptr_t id, intptr_t index, intptr_t value) {
    if (auto* inst = getOwner(self)) {
        return inst->handleHostDispatcher(sender, id, index, value);
    }
    return 0;
}

static void Trampoline_OnParamChanged(void* self, intptr_t sender, int32_t index, int32_t value) {
    if (auto* inst = getOwner(self)) {
        inst->handleOnParamChanged(sender, index, value);
    }
}

static void Trampoline_OnHint(void* self, intptr_t sender, const char* text) {
    if (auto* inst = getOwner(self)) {
        inst->handleOnHint(sender, text);
    }
}

static void Trampoline_ComputeLRVol_Old(void* self, float* outL, float* outR, int32_t panInt, float vol) {
    float pan = (float)panInt / 64.0f;
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;
    if (outL) *outL = vol * (pan <= 0.0f ? 1.0f : (1.0f - pan));
    if (outR) *outR = vol * (pan >= 0.0f ? 1.0f : (1.0f + pan));
}

static void Trampoline_ComputeLRVol(void* self, float* outL, float* outR, float pan, float vol) {
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;
    if (outL) *outL = vol * (pan <= 0.0f ? 1.0f : (1.0f - pan));
    if (outR) *outR = vol * (pan >= 0.0f ? 1.0f : (1.0f + pan));
}

static void Trampoline_VoiceRelease(void* self, intptr_t senderTag) {
    if (auto* inst = getOwner(self)) {
        inst->handleHostReleaseVoice(senderTag);
    }
}

static void Trampoline_KillVoice(void* self, intptr_t senderTag, int32_t killHandleBool) {
    if (auto* inst = getOwner(self)) {
        inst->handleHostKillVoice(senderTag, killHandleBool);
    }
}

static intptr_t Trampoline_VoiceProcessEvent(void* self, intptr_t senderTag, intptr_t eventId, intptr_t eventVal, intptr_t flags) {
    if (auto* inst = getOwner(self)) {
        return inst->handleVoiceProcessEvent(senderTag, eventId, eventVal, flags);
    }
    return 0;
}

static void Trampoline_GetInBuffer(void* self, intptr_t sender, intptr_t index, void* ioBuf) {
    if (auto* inst = getOwner(self)) {
        inst->handleGetInBuffer(sender, index, ioBuf);
    }
}

static void Trampoline_GetOutBuffer(void* self, intptr_t sender, intptr_t index, void* ioBuf) {
    if (auto* inst = getOwner(self)) {
        inst->handleGetOutBuffer(sender, index, ioBuf);
    }
}

static void Trampoline_LockPlugin(void* self, intptr_t sender) {
    if (auto* inst = getOwner(self)) {
        inst->handleLockPlugin();
    }
}

static void Trampoline_UnlockPlugin(void* self, intptr_t sender) {
    if (auto* inst = getOwner(self)) {
        inst->handleUnlockPlugin();
    }
}

static intptr_t Trampoline_GetMixBuffer(void* self, int32_t index) {
    if (auto* inst = getOwner(self)) return reinterpret_cast<intptr_t>(inst->handleGetMixBuffer(index));
    return 0;
}

static intptr_t Trampoline_Noop(void*, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t) {
    return 0;
}

// ============================================================================
// FLPluginInstance Implementation
// ============================================================================

FLPluginInstance::FLPluginInstance(bool generator) : m_isGenerator(generator), m_isInstrument(generator) {
    std::call_once(gInitTablesOnce, initSharedTables);
    initHostTables();
    m_inInterleaved.resize(8192 * 2, 0.0f);
    m_outInterleaved.resize(8192 * 2, 0.0f);
    m_dryInterleaved.resize(8192 * 2, 0.0f);
}

FLPluginInstance::~FLPluginInstance() {
    unloadPlugin();
}

void FLPluginInstance::initHostTables() {
    memset(&m_pascalVmt, 0, sizeof(m_pascalVmt));
    memset(&m_cppVmt, 0, sizeof(m_cppVmt));
    memset(&m_pascalHost, 0, sizeof(m_pascalHost));
    memset(&m_cppHost, 0, sizeof(m_cppHost));

    m_pascalVmt[0] = (void*)0x100;
    m_pascalVmt[1] = (void*)(intptr_t)(-0x100);
    for (int i = 12; i < 72; ++i) m_pascalVmt[i] = (void*)Trampoline_Noop;
    for (int i = 0; i < 48; ++i) m_cppVmt[i] = (void*)Trampoline_Noop;

    auto setHostSlot = [&](int slot, void* fn) {
        m_pascalVmt[25 + slot] = fn;
        m_cppVmt[slot] = fn;
    };

    setHostSlot(0,  (void*)Trampoline_Dispatcher);
    setHostSlot(1,  (void*)Trampoline_OnParamChanged);
    setHostSlot(2,  (void*)Trampoline_OnHint);
    setHostSlot(3,  (void*)Trampoline_ComputeLRVol_Old);
    setHostSlot(4,  (void*)Trampoline_VoiceRelease);
    setHostSlot(5,  (void*)Trampoline_KillVoice);
    setHostSlot(6,  (void*)Trampoline_VoiceProcessEvent);
    setHostSlot(11, (void*)hostAddMonoRamp);
    setHostSlot(12, (void*)hostAddStereoRamp);
    setHostSlot(25, (void*)Trampoline_GetMixBuffer);
    setHostSlot(31, (void*)Trampoline_ComputeLRVol);
    setHostSlot(32, (void*)Trampoline_LockPlugin);
    setHostSlot(33, (void*)Trampoline_UnlockPlugin);
    setHostSlot(36, (void*)Trampoline_GetInBuffer);
    setHostSlot(37, (void*)Trampoline_GetOutBuffer);

    m_pascalHost.vmtPtr = m_pascalVmt;
    m_pascalHost.HostVersion = 24000000;
    m_pascalHost.owner = this;

    m_cppHost.vmtPtr = m_cppVmt;
    m_cppHost.HostVersion = 24000000;
    m_cppHost.owner = this;

    for (int w = 0; w < 10; ++w) {
        m_pascalHost.WaveTables[w] = gSharedWaveTables[w];
        m_cppHost.WaveTables[w] = gSharedWaveTables[w];
    }
    for (int b = 0; b < 4; ++b) {
        m_pascalHost.TempBuffers[b] = gSharedTempBuffers[b];
        m_cppHost.TempBuffers[b] = gSharedTempBuffers[b];
    }
    installHostServices();
}

bool FLPluginInstance::selectSampleFile(std::string& path) {
    if (![NSThread isMainThread]) {
        fprintf(stderr, "FloopHole: sample file picker must run on the editor thread\n");
        return false;
    }
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Load audio sample";
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    if (!path.empty()) panel.directoryURL = [NSURL fileURLWithPath:
        [[NSString stringWithUTF8String:path.c_str()] stringByDeletingLastPathComponent]];
    if ([panel runModal] != NSModalResponseOK) return false;
    path = panel.URL.path.UTF8String;
    return true;
}

bool FLPluginInstance::decodeSampleFile(const std::string& path, double rate,
                                       std::vector<float>& stereo, double& actualRate) {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    ExtAudioFileRef file = nullptr;
    OSStatus status = ExtAudioFileOpenURL((__bridge CFURLRef)url, &file);
    if (status != noErr) {
        fprintf(stderr, "FloopHole: cannot open audio sample %s (OSStatus=%d)\n", path.c_str(), static_cast<int>(status));
        return false;
    }
    AudioStreamBasicDescription source{};
    UInt32 size = sizeof(source);
    status = ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat, &size, &source);
    actualRate = rate > 0 ? rate : source.mSampleRate;
    AudioStreamBasicDescription target{};
    target.mSampleRate = actualRate;
    target.mFormatID = kAudioFormatLinearPCM;
    target.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    target.mChannelsPerFrame = 2;
    target.mBitsPerChannel = 32;
    target.mFramesPerPacket = 1;
    target.mBytesPerFrame = target.mBytesPerPacket = sizeof(float) * 2;
    if (status == noErr)
        status = ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof(target), &target);
    while (status == noErr) {
        float chunk[4096 * 2];
        AudioBufferList buffers{};
        buffers.mNumberBuffers = 1;
        buffers.mBuffers[0] = {2, sizeof(chunk), chunk};
        UInt32 frames = 4096;
        status = ExtAudioFileRead(file, &frames, &buffers);
        if (status != noErr || !frames) break;
        if (stereo.size() / 2 + frames > INT32_MAX) {
            status = kAudio_ParamError;
            break;
        }
        stereo.insert(stereo.end(), chunk, chunk + frames * 2);
    }
    ExtAudioFileDispose(file);
    if (status != noErr) {
        fprintf(stderr, "FloopHole: cannot decode audio sample %s (OSStatus=%d)\n", path.c_str(), static_cast<int>(status));
        return false;
    }
    return true;
}

void FLPluginInstance::unloadPlugin() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_numInternalPresets = 0;
    ++m_pluginGeneration;
    if (!m_plug) {
        m_editorSizeKnown = false;
        m_editorCoordinateScale = 1.0;
        m_samples.clear();
        m_channelSample = 0;
        return;
    }

    allNotesOff();
    if (m_fnDispatcher) m_fnDispatcher(m_plug, FPD_SetEnabled, 0, 0);
    if (m_editorOpen) {
        detachEditor();
    }

    // Clear cached function pointers before destroying instance
    auto destroyFn = m_fnDestroyObject;
    void* plugPtr = m_plug;
    m_plug = nullptr;
    m_editorSizeKnown = false;
    m_editorCoordinateScale = 1.0;
    m_fnDestroyObject = nullptr;
    m_fnDispatcher = nullptr;
    m_fnIdle = nullptr;
    m_fnSaveRestoreState = nullptr;
    m_fnGetName = nullptr;
    m_fnProcessEvent = nullptr;
    m_fnProcessParam = nullptr;
    m_fnEffRender = nullptr;
    m_fnGenRender = nullptr;
    m_fnTriggerVoice = nullptr;
    m_fnVoiceRelease = nullptr;
    m_fnVoiceKill = nullptr;
    m_fnVoiceProcessEvent = nullptr;
    m_fnNewTick = nullptr;
    m_fnMIDITick = nullptr;
    m_fnMIDIIn = nullptr;

    if (destroyFn && plugPtr) {
        @autoreleasepool {
            gCurrentLoadingInstance = this;
            destroyFn(plugPtr);
            gCurrentLoadingInstance = nullptr;
        }
    }
    m_samples.clear();
    m_channelSample = 0;
    if (m_parentNSView) {
        CFRelease(m_parentNSView);
        m_parentNSView = nullptr;
    }
    if (m_cppHost.AppHandle) {
        CFRelease(m_cppHost.AppHandle);
        m_cppHost.AppHandle = nullptr;
    }
}

bool FLPluginInstance::loadPluginByName(const std::string& name) {
    if (!FLPluginScanner::getInstance().discover()) return false;
    int idx = FLPluginScanner::getInstance().findPluginIndexByName(name);
    if (idx < 0) return false;
    return loadPlugin(idx);
}

bool FLPluginInstance::loadPlugin(int pluginIndex) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!FLPluginScanner::getInstance().preparePlugin(pluginIndex)) return false;
    const auto& plugins = FLPluginScanner::getInstance().getPlugins();
    if (pluginIndex < 0 || pluginIndex >= (int)plugins.size()) {
        return false;
    }

    unloadPlugin();

    const FLPluginEntry& entry = plugins[pluginIndex];
    m_numInternalPresets = 0;
    m_activePluginDir = entry.pluginDir;
    m_activePresetDir = FLPluginScanner::getInstance().getPluginPresetPath(entry);
    m_pluginName = entry.name;
    gCurrentLoadingInstance = this;

    // Preload companion engine.dylib in plugin folder if present (e.g. 3x Osc, BassDrum, Drumaxx, Drumpad, Sakura, Sawer)
    std::string engineDylib = entry.pluginDir + "engine.dylib";
    struct stat st;
    if (stat(engineDylib.c_str(), &st) == 0) {
        dlopen(engineDylib.c_str(), RTLD_NOW | RTLD_LOCAL);
    }

    void* hDylib = dlopen(entry.dylibPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!hDylib) {
        gCurrentLoadingInstance = nullptr;
        return false;
    }
    m_dylibHandle = hDylib;

    typedef void* (*CreatePlugInstanceFn)(void* Host, intptr_t Tag);
    auto createFn = (CreatePlugInstanceFn)dlsym(hDylib, "CreatePlugInstance");
    if (!createFn) {
        gCurrentLoadingInstance = nullptr;
        return false;
    }

    // DirectWave exports the legacy editor helper but uses the C++ audio ABI.
    m_isPascalABI = entry.name != "DirectWave" && dlsym(hDylib, "SetExternalAppHandle") != nullptr;
    void* hostPtr = m_isPascalABI ? (void*)&m_pascalHost : (void*)&m_cppHost;
    if (entry.name == "DirectWave") {
        auto createApplication = reinterpret_cast<void* (*)()>(
            dlsym(RTLD_DEFAULT, "MVApplicationCreate"));
        auto setExternalAppHandle = reinterpret_cast<void (*)(void*)>(
            dlsym(hDylib, "SetExternalAppHandle"));
        if (!createApplication || !setExternalAppHandle) {
            fprintf(stderr, "FloopHole: DirectWave requires the native MacVCL application service\n");
            gCurrentLoadingInstance = nullptr;
            return false;
        }
        // The editor keeps a process-global external application across plugin instances.
        // A selector key shares its lifetime across separately loaded bridge modules too.
        const void* key = sel_registerName("flBridgeNativeApplicationContext");
        id application = objc_getAssociatedObject(NSApplication.sharedApplication, key);
        if (!application) {
            application = CFBridgingRelease(createApplication());
            if (application)
                objc_setAssociatedObject(NSApplication.sharedApplication, key, application,
                                         OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        m_cppHost.AppHandle = application ? (__bridge_retained void*)application : nullptr;
        if (!m_cppHost.AppHandle) {
            fprintf(stderr, "FloopHole: cannot create DirectWave's native application context\n");
            gCurrentLoadingInstance = nullptr;
            return false;
        }
        setExternalAppHandle(m_cppHost.AppHandle);
    }

    void* plug = nullptr;
    @autoreleasepool {
        plug = createFn(hostPtr, 1);
    }
    if (!plug) {
        if (m_cppHost.AppHandle) {
            CFRelease(m_cppHost.AppHandle);
            m_cppHost.AppHandle = nullptr;
        }
        gCurrentLoadingInstance = nullptr;
        return false;
    }

    m_plug = plug;
    m_currentPluginIndex = pluginIndex;
    m_currentPresetIndex = -1;
    m_pluginName = entry.name;
    m_isGenerator = entry.isGenerator;
    m_isInstrument = entry.isGenerator;

    TFruityPlugInfo* info = *(TFruityPlugInfo**)((uint8_t*)plug + 0x10);
    if (info) {
        m_pluginFlags = info->Flags;
        m_numParams = std::max(0, info->NumParams);
        m_isGenerator = (info->Flags & FPF_Generator) != 0;
    } else {
        m_numParams = 0;
    }

    void** vmt = *(void***)plug;
    int base = m_isPascalABI ? 25 : 0;
    void* pascalAbstractErr = m_isPascalABI ? vmt[base + 18] : nullptr;
    auto getMethod = [&](int slot) -> void* {
        void* fn = vmt[base + slot];
        if (m_isPascalABI && fn == pascalAbstractErr) return nullptr;
        return fn;
    };

    m_fnDestroyObject     = (Plug_DestroyObjectFn)getMethod(0);
    m_fnDispatcher        = (Plug_DispatcherFn)getMethod(1);
    m_fnIdle              = (Plug_IdleFn)getMethod(2);
    m_fnSaveRestoreState  = (Plug_SaveRestoreStateFn)getMethod(3);
    m_fnGetName           = (Plug_GetNameFn)getMethod(4);
    m_fnProcessEvent      = (Plug_ProcessEventFn)getMethod(5);
    m_fnProcessParam      = (Plug_ProcessParamFn)getMethod(6);
    m_fnEffRender         = (Plug_EffRenderFn)getMethod(7);
    m_fnGenRender         = (Plug_GenRenderFn)getMethod(8);
    m_fnTriggerVoice      = (Plug_TriggerVoiceFn)getMethod(9);
    m_fnVoiceRelease      = (Plug_VoiceReleaseFn)getMethod(10);
    m_fnVoiceKill         = (Plug_VoiceKillFn)getMethod(11);
    m_fnVoiceProcessEvent = (Plug_VoiceProcessEventFn)getMethod(12);
    m_fnNewTick           = (Plug_NewTickFn)getMethod(14);
    m_fnMIDITick          = (Plug_MIDITickFn)getMethod(15);
    m_fnMIDIIn            = (Plug_MIDIInFn)getMethod(16);
    m_midiResolution = m_fnDispatcher &&
        m_fnDispatcher(m_plug, FPD_UseIncreasedMIDIResolution, 0, 0) == 1 ? FromMIDI_Max : 65536;

    // Configure sample rate, block size, routing, and process mode
    if (m_fnDispatcher) {
        m_fnDispatcher(m_plug, FPD_SetSampleRate, 0, (intptr_t)m_sampleRate);
        m_fnDispatcher(m_plug, FPD_SetBlockSize, 0, (intptr_t)m_blockSize);
        m_fnDispatcher(m_plug, FPD_RoutingChanged, 0, 0);
        m_fnDispatcher(m_plug, FPD_ProcessMode, 0, 1);
    }

    // Load Default.fst preset if available for FreePascal plugins
    if (m_isPascalABI && !entry.presets.empty() && entry.presets[0].isDefault()) {
        loadPreset(0);
    }

    initializeChannelState();
    if (entry.name == "FLEX" && m_currentPresetIndex < 0) {
        if (getNumInternalPresets() > 0) loadInternalPreset(0);
        else fprintf(stderr, "FloopHole: FLEX found no installed native presets\n");
    }
    gCurrentLoadingInstance = nullptr;
    return true;
}

void FLPluginInstance::setSampleRate(double sampleRate) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (sampleRate <= 0.0) return;
    m_sampleRate = sampleRate;
    if (m_plug && m_fnDispatcher) {
        m_fnDispatcher(m_plug, FPD_SetSampleRate, 0, (intptr_t)m_sampleRate);
    }
}

void FLPluginInstance::setBlockSize(int32_t maxBlockSize) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (maxBlockSize <= 0) return;
    m_blockSize = std::min<int32_t>(maxBlockSize, 8192);
    if ((int32_t)m_inInterleaved.size() < m_blockSize * 2) {
        m_inInterleaved.resize(m_blockSize * 2, 0.0f);
        m_outInterleaved.resize(m_blockSize * 2, 0.0f);
        m_dryInterleaved.resize(m_blockSize * 2, 0.0f);
    }
    if (m_plug && m_fnDispatcher) {
        m_fnDispatcher(m_plug, FPD_SetBlockSize, 0, (intptr_t)m_blockSize);
    }
}

bool FLPluginInstance::loadPreset(int presetIndex) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || m_currentPluginIndex < 0) return false;
    const auto& plugins = FLPluginScanner::getInstance().getPlugins();
    if (m_currentPluginIndex >= (int)plugins.size()) return false;
    const auto& presets = plugins[m_currentPluginIndex].presets;
    if (presetIndex < 0 || presetIndex >= (int)presets.size()) return false;

    if (loadPresetFromFile(presets[presetIndex].fullPath)) {
        m_currentPresetIndex = presetIndex;
        return true;
    }
    return false;
}

bool FLPluginInstance::loadPresetFromFile(const std::string& fstPath) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (FLPluginScanner::isNativePresetFile(fstPath)) return loadNativePresetFromFile(fstPath);
    if (FLPluginScanner::isFlexPackFile(fstPath)) return importFlexPack(fstPath);
    if (!m_plug || !m_fnSaveRestoreState) return false;

    FILE* fp = fopen(fstPath.c_str(), "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(fp);
        return false;
    }
    std::vector<uint8_t> bytes((size_t)sz);
    size_t nread = fread(bytes.data(), 1, (size_t)sz, fp);
    fclose(fp);
    if (nread != (size_t)sz) return false;

    std::vector<uint8_t> rawState;
    if (ExtractFstPluginState(bytes.data(), bytes.size(), rawState)) {
        return restorePluginState(rawState.data(), rawState.size());
    }
    if (bytes.size() >= 4 && memcmp(bytes.data(), "FLhd", 4) == 0) {
        fprintf(stderr, "FloopHole: invalid or missing plugin state in preset %s\n", fstPath.c_str());
        return false;
    }
    return restorePluginState(bytes.data(), bytes.size());
}

bool FLPluginInstance::savePluginState(std::vector<uint8_t>& outState) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnSaveRestoreState) return false;

    gCurrentLoadingInstance = this;
    MemoryFruityStream stream;
    m_fnSaveRestoreState(m_plug, &stream.vtablePtr, true);
    gCurrentLoadingInstance = nullptr;

    outState = std::move(stream.data);
    return true;
}

bool FLPluginInstance::restorePluginState(const uint8_t* data, size_t size) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnSaveRestoreState || !data || size == 0) return false;

    allNotesOff();
    gCurrentLoadingInstance = this;
    MemoryFruityStream stream;
    stream.data.assign(data, data + size);
    stream.pos = 0;
    m_fnSaveRestoreState(m_plug, &stream.vtablePtr, false);
    initializeChannelState();
    gCurrentLoadingInstance = nullptr;
    return true;
}

std::string FLPluginInstance::getParamName(int index) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnGetName || index < 0 || index >= m_numParams) return "";
    char buf[256] = {0};
    m_fnGetName(m_plug, FPN_Param, index, 0, buf);
    return plainPluginText(buf);
}

std::string FLPluginInstance::getParamDisplay(int index) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnGetName || !m_fnProcessParam || index < 0 || index >= m_numParams) return "";
    int32_t rawVal = m_fnProcessParam(m_plug, index, 0, REC_GetValue);
    char buf[256] = {0};
    m_fnGetName(m_plug, FPN_ParamValue, index, rawVal, buf);
    return plainPluginText(buf);
}

void FLPluginInstance::noteOn(int channel, int midiNote, float velocity) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug) return;

    // MIDIIn can consume a message; consumed notes must not trigger a second voice.
    uint8_t vel7 = (uint8_t)std::clamp((int)std::lround(velocity * 127.0f), 1, 127);
    if (sendMIDIMessage((uint8_t)(0x90 | (channel & 0x0F)), (uint8_t)(midiNote & 0x7F), vel7) == -1) return;

    if (!m_fnTriggerVoice) return;

    // Find free voice slot or steal oldest released/active voice
    int chosen = -1;
    for (int i = 0; i < kMaxVoices; ++i) {
        if (!m_voices[i].active) {
            chosen = i;
            break;
        }
    }
    if (chosen < 0) {
        // Steal oldest released voice first, otherwise oldest active voice
        uint64_t bestAge = UINT64_MAX;
        for (int i = 0; i < kMaxVoices; ++i) {
            if (m_voices[i].released && m_voices[i].age < bestAge) {
                bestAge = m_voices[i].age;
                chosen = i;
            }
        }
        if (chosen < 0) {
            bestAge = UINT64_MAX;
            for (int i = 0; i < kMaxVoices; ++i) {
                if (m_voices[i].age < bestAge) {
                    bestAge = m_voices[i].age;
                    chosen = i;
                }
            }
        }
        if (chosen >= 0 && m_voices[chosen].plugVoiceHandle != FVH_Null && m_fnVoiceKill) {
            intptr_t oldH = m_voices[chosen].plugVoiceHandle;
            m_voices[chosen].active = false;
            m_voices[chosen].plugVoiceHandle = FVH_Null;
            m_fnVoiceKill(m_plug, oldH);
        }
    }
    if (chosen < 0) return;

    VoiceSlot& slot = m_voices[chosen];
    slot.active = true;
    slot.released = false;
    slot.channel = channel & 0x0F;
    slot.midiNote = midiNote;
    slot.velocity = std::clamp(velocity, 0.0f, 1.0f);
    slot.relVelocity = 0.5f;
    slot.age = ++m_voiceCounter;

    float initPitchCents = static_cast<float>(midiNote) * 100.0f;
    float finalPitchCents = midiNoteToFruityPitch(midiNote, m_pitchBendSemitones);

    slot.params.InitLevels.Pan = 0.0f;
    slot.params.InitLevels.Vol = slot.velocity;
    slot.params.InitLevels.Pitch = initPitchCents;
    slot.params.InitLevels.FCut = 0.0f;
    slot.params.InitLevels.FRes = 0.0f;

    slot.params.FinalLevels.Pan = 0.0f;
    slot.params.FinalLevels.Vol = slot.velocity;
    slot.params.FinalLevels.Pitch = finalPitchCents;
    slot.params.FinalLevels.FCut = 0.0f;
    slot.params.FinalLevels.FRes = 0.0f;

    intptr_t setTag = (intptr_t)(chosen + 1);
    slot.legacyParams = {
        {0, slot.velocity, static_cast<int32_t>(std::lround(initPitchCents)), 0.0f, 0.0f},
        {0, slot.velocity, static_cast<int32_t>(std::lround(finalPitchCents)), 0.0f, 0.0f}
    };
    void* voiceParams = (m_pluginFlags & FPF_NewVoiceParams) ?
        static_cast<void*>(&slot.params) : static_cast<void*>(&slot.legacyParams);
    intptr_t handle = m_fnTriggerVoice(m_plug, voiceParams, setTag);
    if (handle != FVH_Null) {
        slot.plugVoiceHandle = handle;
    } else {
        slot.active = false;
        slot.plugVoiceHandle = FVH_Null;
    }
}

void FLPluginInstance::noteOff(int channel, int midiNote, float velocity) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug) return;

    uint8_t vel7 = (uint8_t)std::clamp((int)std::lround(velocity * 127.0f), 0, 127);
    if (sendMIDIMessage((uint8_t)(0x80 | (channel & 0x0F)), (uint8_t)(midiNote & 0x7F), vel7) == -1) return;

    if (!m_fnVoiceRelease) return;

    for (int i = 0; i < kMaxVoices; ++i) {
        if (m_voices[i].active && !m_voices[i].released &&
            m_voices[i].midiNote == midiNote &&
            (channel < 0 || m_voices[i].channel == (channel & 0x0F))) {
            m_voices[i].released = true;
            m_voices[i].relVelocity = std::clamp(velocity, 0.0f, 1.0f);
            if (m_voices[i].plugVoiceHandle != FVH_Null) {
                m_fnVoiceRelease(m_plug, m_voices[i].plugVoiceHandle);
            }
        }
    }
}

void FLPluginInstance::allNotesOff() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug) return;

    for (int i = 0; i < kMaxVoices; ++i) {
        if (m_voices[i].active) {
            intptr_t vh = m_voices[i].plugVoiceHandle;
            m_voices[i].active = false;
            m_voices[i].released = false;
            m_voices[i].plugVoiceHandle = FVH_Null;
            if (vh != FVH_Null && m_fnVoiceKill) {
                m_fnVoiceKill(m_plug, vh);
            }
        }
    }
    if (m_fnDispatcher) {
        m_fnDispatcher(m_plug, FPD_Flush, 0, 0);
    }
}

void FLPluginInstance::setPitchBendSemitones(float semitones) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_pitchBendSemitones = semitones;
    for (int i = 0; i < kMaxVoices; ++i) {
        if (m_voices[i].active) {
            m_voices[i].params.FinalLevels.Pitch = midiNoteToFruityPitch(m_voices[i].midiNote, m_pitchBendSemitones);
            m_voices[i].legacyParams.FinalLevels.Pitch = static_cast<int32_t>(
                std::lround(m_voices[i].params.FinalLevels.Pitch));
        }
    }
}

void FLPluginInstance::processAudio(const float* inL, const float* inR,
                                    float* outL, float* outR,
                                    int32_t numFrames,
                                    double tempoBPM,
                                    double projectTimeQuarters,
                                    bool isPlaying, double projectTimeSeconds,
                                    int32_t numerator, int32_t denominator) {
    if (numFrames <= 0) return;

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug) {
        if (outL) memset(outL, 0, sizeof(float) * numFrames);
        if (outR) memset(outR, 0, sizeof(float) * numFrames);
        return;
    }

    updateTransport(tempoBPM, projectTimeQuarters, isPlaying, projectTimeSeconds,
                    numerator, denominator, numFrames);

    if ((int32_t)m_inInterleaved.size() < numFrames * 2) {
        m_inInterleaved.resize(numFrames * 2, 0.0f);
        m_outInterleaved.resize(numFrames * 2, 0.0f);
        m_dryInterleaved.resize(numFrames * 2, 0.0f);
    }

    // Interleave input stereo audio
    if (inL && inR) {
        for (int32_t i = 0; i < numFrames; ++i) {
            m_inInterleaved[i * 2]     = inL[i];
            m_inInterleaved[i * 2 + 1] = inR[i];
        }
    } else if (inL) {
        for (int32_t i = 0; i < numFrames; ++i) {
            m_inInterleaved[i * 2]     = inL[i];
            m_inInterleaved[i * 2 + 1] = inL[i];
        }
    } else {
        memset(m_inInterleaved.data(), 0, sizeof(float) * numFrames * 2);
    }

    if (!m_isInstrument) std::copy_n(m_inInterleaved.data(), numFrames * 2, m_dryInterleaved.data());
    memset(m_outInterleaved.data(), 0, sizeof(float) * numFrames * 2);

    if (m_fnNewTick && (m_pluginFlags & FPF_WantNewTick)) {
        m_fnNewTick(m_plug);
    }

    int32_t renderLen = numFrames;
    if (m_pluginFlags & FPF_NoProcess) {
        std::copy_n(m_inInterleaved.data(), numFrames * 2, m_outInterleaved.data());
    } else if (m_isGenerator && m_fnGenRender) {
        m_fnGenRender(m_plug, m_outInterleaved.data(), &renderLen);
    } else if (m_fnEffRender) {
        m_fnEffRender(m_plug, m_inInterleaved.data(), m_outInterleaved.data(), renderLen);
    }

    applyOutputControl(outL, outR, numFrames);
}

bool FLPluginInstance::attachEditorToNSView(void* parentNSView, int& outWidth, int& outHeight) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnDispatcher || !parentNSView) return false;
    if (m_editorOpen) detachEditor();
    const int previousWidth = m_editorWidth, previousHeight = m_editorHeight;

    NSView* parentView = (__bridge NSView*)parentNSView;
    void* previousParent = nullptr;
    if (m_parentNSView != parentNSView) {
        previousParent = m_parentNSView;
        m_parentNSView = (void*)CFRetain((CFTypeRef)parentNSView);
    }

    // DirectWave has a C++ audio ABI but embeds a Pascal/MacVCL editor.
    const bool macVclEditor = m_isPascalABI || m_pluginName == "DirectWave";
    intptr_t editorHandle = macVclEditor ? (((intptr_t)parentNSView) | 1) : (intptr_t)parentNSView;
    @autoreleasepool {
        if (m_pluginName == "Newtone") {
            installVstWindowSafeHandlers();
            void* form = *(void**)((uint8_t*)m_plug + 0xc8);
            void* mainPanel = form ? *(void**)((uint8_t*)form + 0x8e0) : nullptr;
            if (!mainPanel) return false;
            NSView* topRoot = parentView;
            while (!topRoot.window && topRoot.superview) topRoot = topRoot.superview;
            NSWindow* stagingWindow = nil;
            if (!topRoot.window) {
                const NSRect rect = topRoot.frame.size.width > 0 && topRoot.frame.size.height > 0
                    ? NSMakeRect(-10000, -10000, topRoot.frame.size.width, topRoot.frame.size.height)
                    : NSMakeRect(-10000, -10000, 900, 600);
                stagingWindow = [[NSWindow alloc] initWithContentRect:rect
                    styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
                stagingWindow.releasedWhenClosed = NO;
                [stagingWindow.contentView addSubview:topRoot];
                objc_setAssociatedObject(parentView, &stagingWindowKey, stagingWindow,
                                         OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            }
            auto setParentWindow = *(void (**)(void*, intptr_t))(*(uintptr_t*)mainPanel + 0x578);
            setParentWindow(mainPanel, (intptr_t)parentNSView);
            editorHandle = *(intptr_t*)((uint8_t*)mainPanel + 0x618);
            if (!editorHandle) {
                if (stagingWindow) {
                    [topRoot removeFromSuperview];
                    [stagingWindow close];
                    objc_setAssociatedObject(parentView, &stagingWindowKey, nil,
                                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                }
                return false;
            }
            synchronizeEditorWindow(parentView);
            m_fnDispatcher(m_plug, FPD_ShowEditor, 0, editorHandle);
            if (void* audioObj = *(void**)((uint8_t*)m_plug + 0x200)) {
                auto getEngineCount = reinterpret_cast<int32_t (*)(void*)>(
                    reinterpret_cast<uintptr_t>(m_fnDispatcher) - 0x679b0 + 0x6d690);
                auto getEngine = reinterpret_cast<void* (*)(void*, int32_t)>(
                    reinterpret_cast<uintptr_t>(m_fnDispatcher) - 0x679b0 + 0x6d6b0);
                NSHashTable<NSWindow*>* vstWindows = [NSHashTable weakObjectsHashTable];
                const int32_t count = getEngineCount(audioObj);
                for (int32_t i = 0; i < count; ++i) {
                    void* engine = getEngine(audioObj, i);
                    NSView* content = engine ? (__bridge NSView*)(*(void**)((uint8_t*)engine + 0x50)) : nil;
                    if (NSWindow* vstWindow = content.window) {
                        [vstWindows addObject:vstWindow];
                    }
                }
                objc_setAssociatedObject(parentView, &vstWindowsKey, vstWindows,
                                         OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            }
            if (stagingWindow) {
                // Keep topRoot in stagingWindow until the DAW moves its parent NSView into the host NSWindow;
                // [NSWindow.contentView addSubview:...] automatically reparents topRoot out of stagingWindow.
                [stagingWindow orderOut:nil];
            }
        } else {
            m_fnDispatcher(m_plug, FPD_ShowEditor, 0, editorHandle);
        }
    }
    // MacVCL still inspects its previous host view while reparenting.
    if (previousParent) CFRelease(previousParent);
    m_editorOpen = true;

    outWidth = 900;
    outHeight = 600;
    NSView* editorView = nil;
    for (NSView* sv in [parentView subviews]) {
        NSRect f = [sv frame];
        if (![sv isKindOfClass:NSControl.class] && f.size.width > 0 && f.size.height > 0) {
            editorView = sv;
            outWidth = (int)std::lround(f.size.width);
            outHeight = (int)std::lround(f.size.height);
            [sv setFrameOrigin:NSMakePoint(0, 0)];
            if (m_pluginName == "Harmor" || m_pluginName == "Fruity Scratcher" ||
                m_pluginName == "3x Osc" || m_pluginName == "Newtone" || m_pluginName == "Edison") {
                NSHashTable<NSView*>* views = [NSHashTable weakObjectsHashTable];
                objc_setAssociatedObject(parentView, &fileDropViewsKey, views, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                installFileDropHandler(sv, this, views);
                NSHashTable<NSWindow*>* vstWindows = objc_getAssociatedObject(parentView, &vstWindowsKey);
                for (NSWindow* vstWindow in vstWindows.allObjects) {
                    if (vstWindow.contentView) installFileDropHandler(vstWindow.contentView, this, views);
                }
            }
            break;
        }
    }
    m_editorWidth = outWidth;
    m_editorHeight = outHeight;
    synchronizeEditorWindow(parentView);
    m_editorCoordinateScale = editorView ? editorCoordinateScale(editorView) : 1.0;
    const bool retainedLogicalSize = m_editorSizeKnown && outWidth == previousWidth && outHeight == previousHeight;
    if (m_pluginName == "DirectWave" && editorView && m_editorCoordinateScale > 1.0 && !retainedLogicalSize) {
        // The C++ frontend passes its editor's pixel dimensions as Cocoa points.
        [editorView setFrameSize:NSMakeSize(outWidth / m_editorCoordinateScale, outHeight / m_editorCoordinateScale)];
        outWidth = static_cast<int>(std::lround(editorView.frame.size.width));
        outHeight = static_cast<int>(std::lround(editorView.frame.size.height));
        synchronizeEditorWindow(parentView);
    }
    if (editorView) resizeNativeEditor(editorView, editorView.frame.size);
    m_editorWidth = outWidth;
    m_editorHeight = outHeight;
    m_editorSizeKnown = true;
    if (m_pluginName == "FLEX" && editorView) {
        NSHashTable<NSView*>* views = [NSHashTable weakObjectsHashTable];
        objc_setAssociatedObject(parentView, &scrollViewsKey, views, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        for (NSWindow* backingWindow in NSApp.windows) {
            if (hostedEditorForWindow(backingWindow) == editorView &&
                [backingWindow.delegate respondsToSelector:@selector(sendMouseEvent:)]) {
                // MacVCL routes wheel/trackpad events in its own window, not its embedded views.
                NSHashTable* windowReference = [NSHashTable weakObjectsHashTable];
                [windowReference addObject:backingWindow.delegate];
                installScrollWheelHandler(editorView, windowReference, views);
                break;
            }
        }
        if (views.count == 0) fprintf(stderr, "FloopHole: FLEX native scroll-wheel routing is unavailable\n");
    }
    if (m_pluginName == "Sytrus" && editorView) {
        // Sytrus needs a real size-change pass to initialize its hosted panel layout.
        const NSSize size = editorView.frame.size;
        [editorView setFrameSize:NSMakeSize(size.width + 1, size.height)];
        [editorView setFrameSize:size];
    }
    return true;
}

bool FLPluginInstance::resizeEditor(int& width, int& height) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_editorOpen || !m_parentNSView) {
        fprintf(stderr, "FloopHole: cannot resize an unattached native editor\n");
        return false;
    }
    getEditorSizeConstraints().constrain(width, height);
    NSView* parent = (__bridge NSView*)m_parentNSView;
    if (NSView* editor = nativeEditorView(parent)) {
        resizeNativeEditor(editor, NSMakeSize(width, height));
        width = static_cast<int>(std::lround(editor.frame.size.width));
        height = static_cast<int>(std::lround(editor.frame.size.height));
        m_editorWidth = width;
        m_editorHeight = height;
        return true;
    }
    fprintf(stderr, "FloopHole: %s has no native view to resize\n", m_pluginName.c_str());
    return false;
}

void FLPluginInstance::detachEditor() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnDispatcher || !m_editorOpen) return;
    NSHashTable<NSView*>* scrollViews = objc_getAssociatedObject((__bridge NSView*)m_parentNSView, &scrollViewsKey);
    for (NSView* view in scrollViews.allObjects) {
        object_setClass(view, class_getSuperclass(object_getClass(view)));
        objc_setAssociatedObject(view, &scrollWindowKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    objc_setAssociatedObject((__bridge NSView*)m_parentNSView, &scrollViewsKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    NSHashTable<NSView*>* views = objc_getAssociatedObject((__bridge NSView*)m_parentNSView, &fileDropViewsKey);
    for (NSView* view in views.allObjects) removeFileDropHandler(view);
    objc_setAssociatedObject((__bridge NSView*)m_parentNSView, &fileDropViewsKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    Class subclass = NSClassFromString(@"FLBridgeHostedMVWindow");
    for (NSWindow* backingWindow in NSApp.windows) {
        if (object_getClass(backingWindow.delegate) == subclass &&
            hostedEditorForWindow(backingWindow).superview == (__bridge NSView*)m_parentNSView) {
            object_setClass(backingWindow.delegate, class_getSuperclass(subclass));
        }
    }
    @autoreleasepool {
        if (m_pluginName == "Newtone" && m_parentNSView) {
            NSView* parentView = (__bridge NSView*)m_parentNSView;
            NSHashTable<NSWindow*>* vstWindows = objc_getAssociatedObject(parentView, &vstWindowsKey);
            for (NSWindow* vstWindow in vstWindows.allObjects) {
                if (vstWindow.parentWindow) [vstWindow.parentWindow removeChildWindow:vstWindow];
                if ([vstWindow respondsToSelector:@selector(setParent:)]) [vstWindow setParent:nil];
            }
            objc_setAssociatedObject(parentView, &vstWindowsKey, nil,
                                     OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        m_fnDispatcher(m_plug, FPD_ShowEditor, 0, 0);
        if (m_pluginName == "Newtone") {
            void* form = *(void**)((uint8_t*)m_plug + 0xc8);
            void* mainPanel = form ? *(void**)((uint8_t*)form + 0x8e0) : nullptr;
            if (mainPanel) {
                auto setParentWindow = *(void (**)(void*, intptr_t))(*(uintptr_t*)mainPanel + 0x578);
                setParentWindow(mainPanel, 0);
            }
            if (m_parentNSView) {
                NSView* parentView = (__bridge NSView*)m_parentNSView;
                if (NSWindow* stagingWindow = objc_getAssociatedObject(parentView, &stagingWindowKey)) {
                    [stagingWindow close];
                    objc_setAssociatedObject(parentView, &stagingWindowKey, nil,
                                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                }
            }
        }
    }
    m_editorOpen = false;
}

void FLPluginInstance::idleEditor() {
    std::unique_lock<std::recursive_mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !m_plug || !m_editorOpen) return;
    if (m_parentNSView) synchronizeEditorWindow((__bridge NSView*)m_parentNSView);
    if (m_fnIdle) {
        m_fnIdle(m_plug);
    }
    if (m_parentNSView) {
        NSView* parentView = (__bridge NSView*)m_parentNSView;
        if (NSView* sv = nativeEditorView(parentView)) {
            NSRect f = [sv frame];
            int w = (int)std::lround(f.size.width);
            int h = (int)std::lround(f.size.height);
            if (w > 0 && h > 0 && (w != m_editorWidth || h != m_editorHeight)) {
                m_editorWidth = w;
                m_editorHeight = h;
                if (onEditorResized) {
                    onEditorResized(w, h);
                }
            }
        }
    }
}

// ============================================================================
// Host Callback Handlers
// ============================================================================

intptr_t FLPluginInstance::handleHostDispatcher(intptr_t sender, intptr_t id, intptr_t index, intptr_t value) {
    if (traceHostEnabled()) fprintf(stderr, "host sender=%lld id=%lld index=%lld value=0x%llx\n",
                                   (long long)sender, (long long)id, (long long)index, (unsigned long long)value);
    switch (id) {
        case FHD_SetNumPresets:
            if (value < 0 || value > INT32_MAX) {
                fprintf(stderr, "FloopHole: invalid native preset count %lld\n", (long long)value);
                return 0;
            }
            m_numInternalPresets = static_cast<int>(value);
            return 1;
        case FHD_NoteOn:
        case FHD_NoteOff:
        case FHD_NoteOnOff:
            return handlePreviewNote(id, index, value);
        case FHD_CheckProdCode: {
            if (value) {
                uint16_t* pFlags = reinterpret_cast<uint16_t*>(value);
                *pFlags &= ~0x8000; // Clear FPF_DemoVersion bit
            }
            return 1;
        }
        case FHD_LocateDataFile:
            return handleLocateDataFile(value);
        case FHD_PackDataFile:
            return handlePackDataFile(value);
        case FHD_SelectChanSample:
            return loadChannelSample("");
        case FHD_GetPath:
            return handleGetPath(index);
        case FHD_GetMixingTime:
        case FHD_GetPlaybackTime:
            return getTransportTime(index, value);
        case FHD_TicksToTime:
            return ticksToSongTime(index, value);
        case 50: // FHD_GetNumInOut
            if (index == 0) return m_isGenerator ? 0 : 1;
            return 1;
        case FHD_GetPathManager:
            return (intptr_t)&gSharedPathMgr;
        case FHD_EditorResized:
            // The editor timer polls its size; do not re-enter the plugin's Idle callback here.
            return 1;
        default:
            return 0;
    }
}

void FLPluginInstance::handleOnHint(intptr_t sender, const char* text) {
    if (onHintChanged && text) {
        onHintChanged(plainPluginText(text));
    }
}

intptr_t FLPluginInstance::handleVoiceProcessEvent(intptr_t senderTag, intptr_t eventId, intptr_t eventVal, intptr_t flags) {
    int slotIdx = (int)senderTag - 1;
    const VoiceSlot* slot = (slotIdx >= 0 && slotIdx < kMaxVoices) ? &m_voices[slotIdx] : nullptr;

    switch (eventId) {
        case FVE_Duration:
            return -1;
        case FVE_ReleaseTime: {
            float multiplier = 1.0f;
            uint32_t bits = 0;
            memcpy(&bits, &multiplier, sizeof(bits));
            return bits;
        }
        case FVE_Color:
            return slot ? slot->channel : 0;
        case FVE_Velocity: {
            float v = slot ? slot->velocity : 0.85f;
            uint32_t bits = 0;
            memcpy(&bits, &v, 4);
            return (intptr_t)bits;
        }
        case FVE_RelVelocity: {
            float v = slot ? slot->relVelocity : 0.5f;
            uint32_t bits = 0;
            memcpy(&bits, &v, 4);
            return (intptr_t)bits;
        }
        default:
            return 0;
    }
}

void FLPluginInstance::handleHostKillVoice(intptr_t senderTag, int32_t killHandleBool) {
    int slotIdx = (int)senderTag - 1;
    if (slotIdx < 0 || slotIdx >= kMaxVoices) return;

    VoiceSlot& slot = m_voices[slotIdx];
    intptr_t vh = slot.plugVoiceHandle;
    slot.active = false;
    slot.released = false;
    slot.plugVoiceHandle = FVH_Null;

    if (killHandleBool != 0 && vh != FVH_Null && m_plug && m_fnVoiceKill) {
        m_fnVoiceKill(m_plug, vh);
    }
}

void FLPluginInstance::handleGetInBuffer(intptr_t sender, intptr_t index, void* ioBuf) {
    if (!ioBuf) return;
    struct RawIOBuf { float* buf; uint32_t flags; };
    auto* b = reinterpret_cast<RawIOBuf*>(ioBuf);
    b->buf = m_inInterleaved.data();
    b->flags = 1;
}

void FLPluginInstance::handleGetOutBuffer(intptr_t sender, intptr_t index, void* ioBuf) {
    if (!ioBuf) return;
    struct RawIOBuf { float* buf; uint32_t flags; };
    auto* b = reinterpret_cast<RawIOBuf*>(ioBuf);
    b->buf = m_outInterleaved.data();
    b->flags = 1;
}

void FLPluginInstance::handleLockPlugin() {
    m_mutex.lock();
}

void FLPluginInstance::handleUnlockPlugin() {
    m_mutex.unlock();
}

} // namespace flbr
