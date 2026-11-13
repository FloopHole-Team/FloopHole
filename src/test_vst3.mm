#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#include "FLPluginScanner.h"
#include "FLPluginInstance.h"
#include "BridgeEditorLayout.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/gui/iplugview.h"
#include <Accelerate/Accelerate.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <cmath>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>
#include <set>
#include <mach-o/dyld.h>
#include "NativePluginCatalog.h"
#include <limits>

@interface FLBridgeTestFileDrag : NSObject
@property(nonatomic, strong) NSPasteboard* draggingPasteboard;
@end
@implementation FLBridgeTestFileDrag
@end

// Matches Ableton Live's OPluginCocoaWindowDelegate, which does not implement
// optional windowDidBecomeKey: or windowDidResignKey: selectors.
@interface FLBridgeTestMinimalWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation FLBridgeTestMinimalWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender { return YES; }
@end

static NSString* testPreferencesDomain = nil;
@interface NSUserDefaults (FLBridgeTestPreferences)
- (instancetype)initFLBridgeTestWithSuiteName:(NSString*)suite;
@end
@implementation NSUserDefaults (FLBridgeTestPreferences)
- (instancetype)initFLBridgeTestWithSuiteName:(NSString*)suite {
    if ([suite isEqualToString:@"com.flbridge.native"]) suite = testPreferencesDomain;
    return [self initFLBridgeTestWithSuiteName:suite];
}
@end

static void isolateInstallationPreferences(NSString* domain) {
    testPreferencesDomain = domain;
    method_exchangeImplementations(class_getInstanceMethod(NSUserDefaults.class, @selector(initWithSuiteName:)),
        class_getInstanceMethod(NSUserDefaults.class, @selector(initFLBridgeTestWithSuiteName:)));
}

@interface FLBridgeTestScrollEvent : NSEvent
@property(nonatomic, strong) NSEvent* source;
@property(nonatomic, strong) NSWindow* hostWindow;
@property(nonatomic) NSPoint hostLocation;
@end
@implementation FLBridgeTestScrollEvent
// CoreGraphics supplies the scroll data but does not bind it to a Cocoa window.
- (NSEventType)type { return NSEventTypeScrollWheel; }
- (NSWindow*)window { return self.hostWindow; }
- (NSInteger)windowNumber { return self.hostWindow.windowNumber; }
- (NSPoint)locationInWindow { return self.hostLocation; }
- (NSTimeInterval)timestamp { return self.source.timestamp; }
- (NSEventModifierFlags)modifierFlags { return self.source.modifierFlags; }
- (CGFloat)deltaX { return self.source.deltaX; }
- (CGFloat)deltaY { return self.source.deltaY; }
- (CGFloat)deltaZ { return self.source.deltaZ; }
- (CGFloat)scrollingDeltaX { return self.source.scrollingDeltaX; }
- (CGFloat)scrollingDeltaY { return self.source.scrollingDeltaY; }
- (BOOL)hasPreciseScrollingDeltas { return self.source.hasPreciseScrollingDeltas; }
- (BOOL)isDirectionInvertedFromDevice { return self.source.isDirectionInvertedFromDevice; }
- (NSEventPhase)phase { return self.source.phase; }
- (NSEventPhase)momentumPhase { return self.source.momentumPhase; }
- (CGEventRef)CGEvent { return self.source.CGEvent; }
@end

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "CHECK FAILED: %s (line %d)\n", #expr, __LINE__); exit(1); } } while (0)

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace flbr {
struct FLPluginScannerTestAccess {
    static int includeForProbe(const std::string& name, bool generator) {
        auto& scanner = FLPluginScanner::getInstance();
        CHECK(scanner.initialize());
        const auto directory = std::filesystem::u8path(scanner.getFLRootPath()) /
            "Plugins/Fruity" / (generator ? "Generators" : "Effects") / name;
        const auto binary = directory / (name + "_x64.dylib");
        CHECK(std::filesystem::is_regular_file(binary));
        FLPluginEntry entry;
        entry.name = name;
        entry.isGenerator = generator;
        entry.category = generator ? "Generator" : "Effect";
        entry.pluginDir = directory.u8string() + "/";
        entry.dylibPath = binary.u8string();
        scanner.scanPresetsForPlugin(entry);
        scanner.m_plugins.push_back(std::move(entry));
        return static_cast<int>(scanner.m_plugins.size()) - 1;
    }
};

struct FLPluginInstanceTestAccess {
    inline static double observedBeats = -1;
    inline static double observedMilliseconds = -1;
    inline static double observedTicks = -1;
    inline static TSongTime observedSongTime{};
    static void watchTransport(FLPluginInstance& instance) {
        const auto callback = +[](void* host, intptr_t sender, intptr_t id, intptr_t index, intptr_t value) {
            const auto result = FLPluginInstance::hostOwner(host)->handleHostDispatcher(sender, id, index, value);
            if (id == FHD_GetPlaybackTime && result && value) {
                const double time = reinterpret_cast<TFPTime*>(value)->t;
                switch (index & 255) {
                    case GT_Beats: observedBeats = time; break;
                    case GT_RunningMS: observedMilliseconds = time; break;
                    case GT_Ticks: observedTicks = time; break;
                }
            }
            if (id == FHD_TicksToTime && result && index)
                observedSongTime = *reinterpret_cast<TSongTime*>(index);
            return result;
        };
        instance.m_cppVmt[0] = reinterpret_cast<void*>(callback);
        instance.m_pascalVmt[25] = reinterpret_cast<void*>(callback);
    }
    inline static int sampleCalls = 0;
    static void traceServices(FLPluginInstance& instance) {
        const auto slot = [&](int index, void* function) {
            instance.m_cppVmt[index] = function;
            instance.m_pascalVmt[25 + index] = function;
        };
        slot(13, reinterpret_cast<void*>(+[](void* host, intptr_t& handle, char* file,
                                            const FLWaveFormat* format, int32_t flags) {
            ++sampleCalls;
            fprintf(stderr, "Sample request flags=%d file=%s channels=%d bits=%d tag=%d\n",
                    flags, file ? file : "(null)", format ? format->channels : 0,
                    format ? format->bitsPerSample : 0, format ? format->tag : 0);
            return FLPluginInstance::hostOwner(host)->handleLoadSample(handle, file, format, flags);
        }));
    }
    static void reportServices(FLPluginInstance& instance) {
        fprintf(stderr, "Services: sampleCalls=%d samples=%zu\n", sampleCalls, instance.m_samples.size());
    }
    static void describe(FLPluginInstance& instance) {
        fprintf(stderr, "Native flags=0x%x generator=%d parameters=%d\n",
                instance.m_pluginFlags, instance.m_isGenerator, instance.m_numParams);
    }
    static void loadFile(FLPluginInstance& instance, const char* path, intptr_t mode) {
        if (std::filesystem::path(path).extension() == ".fst" ||
            FLPluginScanner::isNativePresetFile(instance.getPluginName(), path)) {
            CHECK(instance.loadPresetFromFile(path));
            return;
        }
        CHECK(instance.m_fnDispatcher);
        const auto result = instance.m_fnDispatcher(instance.m_plug, FPD_LoadFile, mode,
                                                    reinterpret_cast<intptr_t>(path));
        fprintf(stderr, "Native file dispatch result=%lld\n", static_cast<long long>(result));
        instance.initializeChannelState();
    }
};
}

// Simple IBStream implementation for testing VST3 state save/restore
class TestMemoryStream : public IBStream {
public:
    std::vector<uint8_t> bytes;
    size_t pos = 0;

    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, IBStream::iid) ||
            FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
            *obj = static_cast<IBStream*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override {
        if (!buffer || numBytes <= 0) {
            if (numBytesRead) *numBytesRead = 0;
            return kResultOk;
        }
        size_t avail = (pos < bytes.size()) ? (bytes.size() - pos) : 0;
        size_t toRead = std::min<size_t>((size_t)numBytes, avail);
        if (toRead > 0) {
            memcpy(buffer, bytes.data() + pos, toRead);
            pos += toRead;
        }
        if (numBytesRead) *numBytesRead = (int32)toRead;
        return (toRead == (size_t)numBytes) ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override {
        if (!buffer || numBytes <= 0) {
            if (numBytesWritten) *numBytesWritten = 0;
            return kResultOk;
        }
        if (pos + (size_t)numBytes > bytes.size()) {
            bytes.resize(pos + (size_t)numBytes);
        }
        memcpy(bytes.data() + pos, buffer, (size_t)numBytes);
        pos += (size_t)numBytes;
        if (numBytesWritten) *numBytesWritten = numBytes;
        return kResultOk;
    }

    tresult PLUGIN_API seek(int64 posIn, int32 mode, int64* result) override {
        int64 np = 0;
        if (mode == kIBSeekSet) np = posIn;
        else if (mode == kIBSeekCur) np = (int64)pos + posIn;
        else if (mode == kIBSeekEnd) np = (int64)bytes.size() + posIn;
        if (np < 0) np = 0;
        pos = (size_t)np;
        if (result) *result = (int64)pos;
        return kResultOk;
    }

    tresult PLUGIN_API tell(int64* result) override {
        if (result) *result = (int64)pos;
        return kResultOk;
    }
};

// Simple IEventList implementation for sending MIDI NoteOn/NoteOff in VST3 process()
class TestEventList : public IEventList {
public:
    std::vector<Event> events;

    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    int32 PLUGIN_API getEventCount() override { return (int32)events.size(); }
    tresult PLUGIN_API getEvent(int32 index, Event& e) override {
        if (index < 0 || index >= (int32)events.size()) return kInvalidArgument;
        e = events[index];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Event& e) override {
        events.push_back(e);
        return kResultOk;
    }
};

class TestParameterChanges : public IParameterChanges, public IParamValueQueue {
public:
    ParamID id = 0;
    ParamValue value = 0;
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getParameterCount() override { return 1; }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override { return index == 0 ? this : nullptr; }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& param, int32& index) override {
        id = param;
        index = 0;
        return this;
    }
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return 1; }
    tresult PLUGIN_API getPoint(int32 index, int32& offset, ParamValue& result) override {
        if (index != 0) return kInvalidArgument;
        offset = 0;
        result = value;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32, ParamValue next, int32& index) override {
        value = next;
        index = 0;
        return kResultOk;
    }
};

class TestComponentHandler : public IComponentHandler {
public:
    std::vector<std::pair<ParamID, ParamValue>> edits;
    bool editing = false;
    ParamID active = 0;

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual(iid, IComponentHandler::iid)) {
            *obj = static_cast<IComponentHandler*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API beginEdit(ParamID id) override {
        CHECK(!editing);
        editing = true;
        active = id;
        return kResultOk;
    }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override {
        CHECK(editing && active == id);
        edits.emplace_back(id, value);
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(ParamID id) override {
        CHECK(editing && active == id);
        editing = false;
        return kResultOk;
    }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
};

static double dominantFrequency(const std::vector<float>& signal) {
    constexpr int frames = 32768;
    CHECK(signal.size() == frames);
    std::vector<float> real(frames / 2), imag(frames / 2), power(frames / 2);
    FFTSetup fft = vDSP_create_fftsetup(15, kFFTRadix2);
    CHECK(fft != nullptr);
    DSPSplitComplex split{real.data(), imag.data()};
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(signal.data()), 2, &split, 1, frames / 2);
    vDSP_fft_zrip(fft, &split, 1, 15, FFT_FORWARD);
    vDSP_zvmags(&split, 1, power.data(), 1, frames / 2);
    vDSP_destroy_fftsetup(fft);
    power[0] = 0;
    return std::distance(power.begin(), std::max_element(power.begin(), power.end())) * 44100.0 / frames;
}

static void verifyNativePitchAndPreview() {
    flbr::FLPluginInstance instance;
    CHECK(instance.loadPluginByName("Sytrus"));
    instance.setSampleRate(44100);
    instance.setBlockSize(512);

    constexpr int frames = 32768;
    std::vector<float> signal(frames), right(frames);
    bool pitchValid = true;
    for (int note : {24, 36, 60, 69}) {
        instance.allNotesOff();
        instance.noteOn(0, note, 1.0f);
        for (int offset = 0; offset < frames; offset += 512) {
            instance.processAudio(nullptr, nullptr, signal.data() + offset, right.data() + offset,
                                  512, 120, 0, false);
        }
        const double frequency = dominantFrequency(signal);
        // Sytrus's Default patch uses an operator frequency ratio of 2.
        const double expected = 2.0 * 440.0 * std::pow(2.0, (note - 69) / 12.0);
        printf("Sytrus MIDI %d: measured %.2f Hz, expected %.2f Hz\n", note, frequency, expected);
        pitchValid = pitchValid && std::abs(frequency - expected) < 1.5;
    }
    CHECK(pitchValid);
    instance.allNotesOff();
    CHECK(instance.handleHostDispatcher(1, 20, 60, 100) == 1);
    float peak = 0;
    for (int block = 0; block < 16; ++block) {
        instance.processAudio(nullptr, nullptr, signal.data(), right.data(), 512, 120, 0, false);
        for (int frame = 0; frame < 512; ++frame) peak = std::max(peak, std::abs(signal[frame]));
    }
    CHECK(peak > 0.01f);
    CHECK(instance.handleHostDispatcher(1, 21, 60, -1) == 1);
    printf("Sytrus preview keyboard audio peak: %.5f\n", peak);
}

static void verifyAffectedInstruments() {
    for (const char* name : {"SimSynth", "Plucked!", "BooBass", "Drumaxx", "BassDrum", "Drumpad",
                             "Sakura", "GMS", "Sawer"}) {
        flbr::FLPluginInstance instance;
        CHECK(instance.loadPluginByName(name));
        instance.setSampleRate(44100);
        instance.setBlockSize(512);
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1000, 800)
            styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        int width = 0, height = 0;
        CHECK(instance.attachEditorToNSView((__bridge void*)window.contentView, width, height));
        const int parameter = instance.getPluginName() == "GMS" ? 20 : 0;
        const double original = instance.getParamNormalized(parameter);
        for (double value : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            instance.setParamNormalized(parameter, value, true);
            CHECK(std::abs(instance.getParamNormalized(parameter) - value) < 0.011);
        }
        instance.setParamNormalized(parameter, original, true);
        std::vector<uint8_t> state;
        CHECK(instance.savePluginState(state) && !state.empty());
        CHECK(instance.restorePluginState(state.data(), state.size()));
        CHECK(std::abs(instance.getParamNormalized(parameter) - original) < 0.011);
        const auto presets = flbr::FLPluginScanner::getInstance().getPlugins()[instance.getCurrentPluginIndex()].presets;
        if (instance.getPluginName() == "SimSynth") {
            const auto blank = std::find_if(presets.begin(), presets.end(),
                [](const auto& preset) { return preset.name == "BLANK"; });
            CHECK(blank != presets.end());
            CHECK(instance.loadPreset(static_cast<int>(std::distance(presets.begin(), blank))));
            // Use an unmodulated oscillator rather than the detuned/chorused startup patch.
            instance.setParamNormalized(0, 1);
            instance.setParamNormalized(3, 0);
            instance.setParamNormalized(6, 0);
            instance.setParamNormalized(11, 1);
            instance.setParamNormalized(12, 0);
        }
        std::vector<float> left(32768), right(32768);
        const int note = instance.getPluginName() == "Drumaxx" ? 36 : 60;
        auto renderNote = [&](int pitch) {
            instance.allNotesOff();
            instance.noteOn(0, pitch, 0.8f);
            for (int offset = 0; offset < 32768; offset += 512) {
                instance.processAudio(nullptr, nullptr, left.data() + offset, right.data() + offset,
                                      512, 120, 0, false);
            }
            float peak = 0;
            for (int i = 0; i < 32768; ++i) {
                CHECK(std::isfinite(left[i]) && std::isfinite(right[i]));
                peak = std::max({peak, std::abs(left[i]), std::abs(right[i])});
            }
            CHECK(peak > 0.001f);
            return peak;
        };
        const float peak = renderNote(note);
        if (instance.getPluginName() == "Sakura" || instance.getPluginName() == "GMS" ||
            instance.getPluginName() == "Sawer") {
            for (int pitch : {36, 72}) renderNote(pitch);
            if (!presets.empty()) {
                instance.allNotesOff();
                CHECK(instance.loadPreset(0));
                renderNote(note);
            }
        }
        if (instance.getPluginName() == "SimSynth") {
            const double frequency = dominantFrequency(left);
            CHECK(std::abs(frequency - 440 * std::pow(2.0, (note - 69) / 12.0)) < 4.0);
            for (int pitch : {24, 36, 48, 72}) {
                renderNote(pitch);
                const double measured = dominantFrequency(left);
                const double expected = 440 * std::pow(2.0, (pitch - 69) / 12.0);
                printf("SimSynth MIDI %d: measured %.2f Hz, expected %.2f Hz\n", pitch, measured, expected);
                CHECK(std::abs(measured - expected) < 4.0);
            }
            for (size_t i = 0; i < presets.size(); ++i) CHECK(instance.loadPreset(static_cast<int>(i)));
            printf("SimSynth preset loads with editor attached: %zu\n", presets.size());
        }
        instance.allNotesOff();
        instance.detachEditor();
        printf("%s: native automation, state round trip and audio passed (peak %.5f)\n", name, peak);
    }
}

// Simple IPlugFrame implementation to track window resize requests from FLBridgePlugView
class TestPlugFrame : public IPlugFrame {
public:
    ViewRect lastRect{0, 0, 0, 0};
    int resizeCalls = 0;
    NSWindow* window = nil;

    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, IPlugFrame::iid) ||
            FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
            *obj = static_cast<IPlugFrame*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override {
        if (newSize) {
            lastRect = *newSize;
            resizeCalls++;
            const auto result = view->onSize(newSize);
            if (window && result == kResultOk) {
                ViewRect applied{};
                CHECK(view->getSize(&applied) == kResultOk);
                [window setContentSize:NSMakeSize(applied.getWidth(), applied.getHeight())];
            }
            return result;
        }
        return kResultOk;
    }
};

typedef bool (*BundleEntryFn)(CFBundleRef);
typedef bool (*BundleExitFn)();
typedef IPluginFactory* (PLUGIN_API *GetPluginFactoryFn)();

static PClassInfo nativeClassInfo(IPluginFactory* factory, const char* name) {
    for (int32 i = 0; i < factory->countClasses(); ++i) {
        PClassInfo info{};
        CHECK(factory->getClassInfo(i, &info) == kResultOk);
        if (strcmp(info.name, name) == 0) return info;
    }
    fprintf(stderr, "Missing FloopHole class: %s\n", name);
    CHECK(false);
    return {};
}

static void replaceStateIdentity(TestMemoryStream& state, const std::string& identity) {
    uint32_t previous = 0;
    CHECK(state.bytes.size() >= 12);
    memcpy(&previous, state.bytes.data() + 8, 4);
    CHECK(previous <= state.bytes.size() - 12);
    state.bytes.erase(state.bytes.begin() + 12, state.bytes.begin() + 12 + previous);
    state.bytes.insert(state.bytes.begin() + 12, identity.begin(), identity.end());
    const uint32_t length = static_cast<uint32_t>(identity.size());
    memcpy(state.bytes.data() + 8, &length, 4);
}

static void verifyDynamicFactory() {
    const bool hadApplication = NSApp != nil;
    const auto start = std::chrono::steady_clock::now();
    void* bundle = dlopen("./build/VST3/FloopHole.vst3/Contents/MacOS/FloopHole", RTLD_NOW);
    CHECK(bundle);
    auto factoryFn = reinterpret_cast<GetPluginFactoryFn>(dlsym(bundle, "GetPluginFactory"));
    CHECK(factoryFn);
    IPluginFactory* factory = factoryFn();
    CHECK(factory && factory->countClasses() > 2);
    auto& scanner = flbr::FLPluginScanner::getInstance();
    CHECK(scanner.discover());
    CHECK(!scanner.isInitialized());
    if (!hadApplication) CHECK(NSApp == nil);
    const auto expected = flbr::buildNativePluginClasses(scanner.getDiscoverySnapshot());
    CHECK(factory->countClasses() == static_cast<int32>(expected.size()));
    IPluginFactory2* extended = nullptr;
    CHECK(factory->queryInterface(IPluginFactory2::iid, reinterpret_cast<void**>(&extended)) == kResultOk);
    std::set<std::string> ids;
    size_t instruments = 0, effects = 0;
    for (int32 i = 0; i < factory->countClasses(); ++i) {
        PClassInfo2 info{};
        CHECK(extended->getClassInfo2(i, &info) == kResultOk);
        FUID id(info.cid);
        char text[33]{};
        id.toString(text);
        CHECK(expected[i].cid == text);
        CHECK(expected[i].name == info.name);
        CHECK(ids.insert(text).second);
        CHECK((strstr(info.subCategories, "Instrument") != nullptr) == expected[i].isGenerator);
        if (expected[i].isGenerator) ++instruments;
        else ++effects;
        CHECK(strcmp(info.vendor, "FloopHole") == 0);
    }
    extended->release();
    factory->release();
    for (uint32_t image = 0; image < _dyld_image_count(); ++image) {
        const char* name = _dyld_get_image_name(image);
        CHECK(!strstr(name, "/FL/Shared/") && !strstr(name, "/Plugins/Fruity/"));
    }
    CHECK(!std::filesystem::exists("build/VST3/FloopHole.vst3/Contents/Resources/moduleinfo.json"));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    CHECK(elapsed < 2000);
    fprintf(stderr, "Dynamic factory: %zu instruments, %zu effects, stable CIDs, no native libraries, %lld ms\n",
        instruments, effects, static_cast<long long>(elapsed));
    dlclose(bundle);
}

static void clickNativeEditor(flbr::FLPluginInstance* instance, NSWindow* window, NSView* editor, NSPoint local,
                              NSPoint drag = NSZeroPoint) {
    const NSPoint savedCursor = NSEvent.mouseLocation;
    const double screenTop = NSMaxY(NSScreen.screens.firstObject.frame);
    if (!editor.isFlipped) local.y = editor.bounds.size.height - local.y;
    NSView* target = [editor hitTest:[editor convertPoint:local toView:editor.superview]] ?: editor;
    for (NSEventType type : {NSEventTypeMouseMoved, NSEventTypeLeftMouseDown,
                            NSEventTypeLeftMouseDragged, NSEventTypeLeftMouseUp}) {
        if (type == NSEventTypeLeftMouseDragged && NSEqualPoints(drag, NSZeroPoint)) continue;
        const bool dragged = type == NSEventTypeLeftMouseDragged || type == NSEventTypeLeftMouseUp;
        const NSPoint point = dragged ? NSMakePoint(local.x + drag.x, local.y + (editor.isFlipped ? drag.y : -drag.y)) : local;
        const NSPoint location = [editor convertPoint:point toView:nil];
        const NSPoint screen = [window convertPointToScreen:location];
        CGWarpMouseCursorPosition(CGPointMake(screen.x, screenTop - screen.y));
        NSEvent* event = [NSEvent mouseEventWithType:type location:location modifierFlags:0
            timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber
            context:nil eventNumber:1 clickCount:1 pressure:1];
        if (type == NSEventTypeMouseMoved) [target mouseMoved:event];
        else if (type == NSEventTypeLeftMouseDown) [target mouseDown:event];
        else if (type == NSEventTypeLeftMouseDragged) [target mouseDragged:event];
        else [target mouseUp:event];
        if (instance) instance->idleEditor();
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    CGWarpMouseCursorPosition(CGPointMake(savedCursor.x, screenTop - savedCursor.y));
}

static void verifyNativeEditorReopening() {
    for (const char* name : {"Harmor", "3x Osc", "Fruity Scratcher", "Maximus", "Fruity Limiter", "Newtone"}) {
        flbr::FLPluginInstance instance;
        CHECK(instance.loadPluginByName(name));
        const double original = instance.getParamNormalized(0);
        __weak NSView* previousParent = nil;
        for (int cycle = 0; cycle < 4; ++cycle) {
            @autoreleasepool {
                NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 1200, 900)
                    styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
                window.releasedWhenClosed = NO;
                NSView* host = [[NSView alloc] initWithFrame:window.contentView.bounds];
                [window.contentView addSubview:host];
                int width = 0, height = 0;
                fprintf(stderr, "Reopening %s cycle %d; previous parent alive=%d\n", name, cycle, previousParent != nil);
                CHECK(instance.attachEditorToNSView((__bridge void*)host, width, height));
                CHECK(std::abs(instance.getParamNormalized(0) - original) < 0.011);
                [window setContentSize:NSMakeSize(width, height)];
                [window makeKeyAndOrderFront:nil];
                for (int i = 0; i < 10; ++i) {
                    instance.idleEditor();
                    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
                }
                instance.detachEditor();
                [host removeFromSuperview];
                previousParent = host;
                [window close];
            }
        }
        instance.unloadPlugin();
    }
    fprintf(stderr, "Native editor reopening completed\n");
}

static NSView* findNativeEditor(NSView* parent) {
    for (NSView* view in parent.subviews) {
        if ([NSStringFromClass(view.class) hasPrefix:@"MV"]) return view;
        if (NSView* found = findNativeEditor(view)) return found;
    }
    return nil;
}

static NSControl* findPresetPicker(NSView* view) {
    if ([NSStringFromClass(object_getClass(view)) isEqualToString:@"FloopHolePresetPicker"]) return (NSControl*)view;
    for (NSView* child in view.subviews)
        if (NSControl* popup = findPresetPicker(child)) return popup;
    return nil;
}

static NSData* captureUtilityPixels(NSWindow* window, const std::filesystem::path& output);

static NSView* findVolumeKnob(NSView* parent) {
    if ([NSStringFromClass(object_getClass(parent)) isEqualToString:@"FloopHoleOutputKnob"]) return parent;
    for (NSView* child in parent.subviews)
        if (NSView* found = findVolumeKnob(child)) return found;
    return nil;
}

static void verifyPresetPicker(NSWindow* window, IEditController* controller, ParamID outputID,
                               const char* screenshotDirectory) {
    NSControl* picker = findPresetPicker(window.contentView);
    CHECK(picker && picker.superview.subviews.count == 7);
    NSView* logo = picker.superview.subviews[0];
    CHECK([NSStringFromClass(object_getClass(logo)) isEqualToString:@"FloopHoleLogoView"]);
    CHECK(logo.frame.size.width == 38 && logo.frame.size.height == 38);
    for (NSView* control in picker.superview.subviews) CHECK(![control isKindOfClass:NSPopUpButton.class]);
    for (size_t index : {size_t{2}, size_t{3}, size_t{4}}) {
        NSView* button = picker.superview.subviews[index];
        CHECK(button.isFlipped == picker.isFlipped && !button.isFlipped);
        const auto sample = [](NSView* control, CGFloat y) {
            NSBitmapImageRep* bitmap = [control bitmapImageRepForCachingDisplayInRect:control.bounds];
            CHECK(bitmap);
            [control cacheDisplayInRect:control.bounds toBitmapImageRep:bitmap];
            return [[bitmap colorAtX:static_cast<NSInteger>(4 * bitmap.pixelsWide / control.bounds.size.width)
                y:static_cast<NSInteger>(y * bitmap.pixelsHigh / control.bounds.size.height)]
                colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        };
        for (CGFloat y : {CGFloat{4}, CGFloat{21}}) {
            NSColor* expected = sample(picker, y);
            NSColor* actual = sample(button, y);
            CHECK(expected && actual);
            CHECK(std::abs(expected.redComponent - actual.redComponent) < 0.015);
            CHECK(std::abs(expected.greenComponent - actual.greenComponent) < 0.015);
            CHECK(std::abs(expected.blueComponent - actual.blueComponent) < 0.015);
        }
    }
    NSView* hint = picker.superview.subviews[5];
    CHECK(std::abs(NSMinY(hint.frame) - NSMinY(picker.frame) - 1) < 0.01);
    const double level = controller->getParamNormalized(outputID);
    const auto clickPicker = [&] {
        NSEvent* event = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
            location:[picker convertPoint:NSMakePoint(10, 10) toView:nil] modifierFlags:0
            timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber context:nil
            eventNumber:0 clickCount:1 pressure:1];
        SEL filter = NSSelectorFromString(@"filterMenuMouseDown:");
        CHECK([picker respondsToSelector:filter]);
        NSEvent* dispatched = reinterpret_cast<NSEvent*(*)(id, SEL, NSEvent*)>(
            [picker methodForSelector:filter])(picker, filter, event);
        if (dispatched) [picker mouseDown:dispatched];
    };
    const auto openPicker = [&] {
        [picker mouseDown:[NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
            location:[picker convertPoint:NSMakePoint(10, 10) toView:nil] modifierFlags:0
            timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber context:nil
            eventNumber:0 clickCount:1 pressure:1]];
    };
    openPicker();
    NSWindow* firstPanel = [picker valueForKey:@"panel"];
    CHECK(firstPanel.visible);
    clickPicker();
    CHECK([picker valueForKey:@"panel"] == nil && !firstPanel.visible);
    openPicker();
    NSWindow* panel = [picker valueForKey:@"panel"];
    CHECK(panel && panel.visible);
    NSView* menu = panel.contentView;
    CHECK([[menu valueForKey:@"visibleRows"] intValue] <= 16);
    const auto key = [&](unsigned short code) {
        return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0
            timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:panel.windowNumber context:nil
            characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:code];
    };
    if ([[picker valueForKey:@"items"] count] > 16) {
        CGEventRef wheel = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 1, -3);
        CHECK(wheel);
        [menu scrollWheel:[NSEvent eventWithCGEvent:wheel]];
        CFRelease(wheel);
        CHECK([[menu valueForKey:@"firstRow"] intValue] > 0);
    }
    [menu keyDown:key(125)];
    const int row = [[menu valueForKey:@"highlightedRow"] intValue];
    NSArray<NSDictionary*>* items = [picker valueForKey:@"items"];
    CHECK(row >= 0 && row < static_cast<int>(items.count) && [items[row][@"preset"] intValue] >= 0);
    NSString* expected = items[row][@"text"];
    if (screenshotDirectory)
        captureUtilityPixels(panel, std::filesystem::u8path(screenshotDirectory) / "preset-menu.png");
    [menu keyDown:key(36)];
    CHECK([picker valueForKey:@"panel"] == nil && !panel.visible);
    CHECK([[picker valueForKey:@"displayTitle"] isEqualToString:expected]);
    CHECK(controller->getParamNormalized(outputID) == level);
    NSString* title = [picker valueForKey:@"displayTitle"];
    NSButton* previous = (NSButton*)picker.superview.subviews[2];
    NSButton* next = (NSButton*)picker.superview.subviews[3];
    [next performClick:nil];
    [previous performClick:nil];
    CHECK([[picker valueForKey:@"displayTitle"] isEqualToString:title]);
}

static void verifyEffectMixAudio(IPluginFactory* factory) {
    const auto info = nativeClassInfo(factory, "Fruity Phase Inverter");
    IComponent* component = nullptr;
    IEditController* controller = nullptr;
    IAudioProcessor* processor = nullptr;
    CHECK(factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk);
    CHECK(component->initialize(nullptr) == kResultOk);
    CHECK(component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk);
    CHECK(component->queryInterface(IAudioProcessor::iid, reinterpret_cast<void**>(&processor)) == kResultOk);
    ProcessSetup setup{};
    setup.symbolicSampleSize = kSample32;
    setup.sampleRate = 44100;
    setup.maxSamplesPerBlock = 512;
    CHECK(processor->setupProcessing(setup) == kResultOk);
    std::vector<float> left(512, 0.25f), right(512, -0.5f), outLeft(512), outRight(512);
    float* inputs[] = {left.data(), right.data()};
    float* outputs[] = {outLeft.data(), outRight.data()};
    AudioBusBuffers input{}, output{};
    input.numChannels = output.numChannels = 2;
    input.channelBuffers32 = inputs;
    output.channelBuffers32 = outputs;
    ProcessData data{};
    data.symbolicSampleSize = kSample32;
    data.numSamples = 512;
    data.numInputs = data.numOutputs = 1;
    data.inputs = &input;
    data.outputs = &output;
    CHECK(controller->setParamNormalized(flbr::kParamEffectMix, 1) == kResultOk);
    CHECK(processor->process(data) == kResultOk);
    const auto wetLeft = outLeft, wetRight = outRight;
    CHECK(std::abs(wetLeft.back() - left.back()) > 0.1 || std::abs(wetRight.back() - right.back()) > 0.1);
    CHECK(controller->setParamNormalized(flbr::kParamEffectMix, 0) == kResultOk);
    CHECK(processor->process(data) == kResultOk);
    CHECK(processor->process(data) == kResultOk);
    CHECK(outLeft == left && outRight == right);
    CHECK(controller->setParamNormalized(flbr::kParamEffectMix, 0.5) == kResultOk);
    CHECK(processor->process(data) == kResultOk);
    CHECK(processor->process(data) == kResultOk);
    for (int i = 0; i < 512; ++i) {
        CHECK(std::abs(outLeft[i] - (left[i] + wetLeft[i]) * 0.5) < 1e-7);
        CHECK(std::abs(outRight[i] - (right[i] + wetRight[i]) * 0.5) < 1e-7);
    }
    CHECK(controller->setParamNormalized(flbr::kParamEffectMix, 1) == kResultOk);
    CHECK(processor->process(data) == kResultOk);
    CHECK(processor->process(data) == kResultOk);
    CHECK(outLeft == wetLeft && outRight == wetRight);
    processor->release();
    controller->release();
    component->terminate();
    component->release();
    fprintf(stderr, "Actual native FX audio: 0%% dry, 50%% blend and 100%% unity wet passed\n");
}

static void verifyMasterVolume(const char* screenshotDirectory) {
    void* bundle = dlopen("./build/VST3/FloopHole.vst3/Contents/MacOS/FloopHole", RTLD_NOW);
    CHECK(bundle);
    auto entry = reinterpret_cast<BundleEntryFn>(dlsym(bundle, "bundleEntry"));
    auto bundleExit = reinterpret_cast<BundleExitFn>(dlsym(bundle, "bundleExit"));
    auto factoryFn = reinterpret_cast<GetPluginFactoryFn>(dlsym(bundle, "GetPluginFactory"));
    CHECK(entry && bundleExit && factoryFn && entry(nullptr));
    IPluginFactory* factory = factoryFn();
    CHECK(factory);
    for (int classIndex : {0, 1}) {
        const ParamID controlID = classIndex == 0 ? flbr::kParamMasterVolume : flbr::kParamEffectMix;
        const double defaultValue = classIndex == 0 ? flbr::kDefaultMasterVolume : flbr::kDefaultEffectMix;
        const auto controlHint = [&](double value) {
            return std::string(classIndex == 0 ? "Master volume: " : "Effect mix: ") +
                (classIndex == 0 ? flbr::masterVolumeDisplay(value) : flbr::effectMixDisplay(value));
        };
        PClassInfo info{};
        info = nativeClassInfo(factory, classIndex == 0 ? "Harmor" : "Gross Beat");
        IComponent* component = nullptr;
        CHECK(factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk);
        CHECK(component->initialize(nullptr) == kResultOk);
        IEditController* controller = nullptr;
        CHECK(component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk);
        CHECK(controller->getParameterCount() == 2053);
        ParameterInfo volume{};
        CHECK(controller->getParameterInfo(2052, volume) == kResultOk);
        CHECK(volume.id == controlID && volume.defaultNormalizedValue == defaultValue);
        CHECK(volume.flags & ParameterInfo::kCanAutomate);
        CHECK(controller->getParameterInfo(132, volume) == kResultOk && volume.id == 128);
        CHECK(controller->getParameterInfo(2051, volume) == kResultOk && volume.id == 2047);
        CHECK(controller->getParamNormalized(controlID) == defaultValue);
        String128 display{};
        CHECK(controller->getParamStringByValue(controlID, defaultValue, display) == kResultOk);
        const std::string expected = classIndex == 0 ? "78% (-5.2 dB)" : "100% wet / 0% dry";
        CHECK(std::equal(expected.begin(), expected.end(), display));
        TestComponentHandler handler;
        CHECK(controller->setComponentHandler(&handler) == kResultOk);
        TestPlugFrame frame;
        IPlugView* view = controller->createView(ViewType::kEditor);
        CHECK(view && view->setFrame(&frame) == kResultOk);
        for (int cycle = 0; cycle < 2; ++cycle) {
            NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 1400, 900)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            window.releasedWhenClosed = NO;
            frame.window = window;
            CHECK(view->attached((__bridge void*)window.contentView, kPlatformTypeNSView) == kResultOk);
            ViewRect size{};
            CHECK(view->getSize(&size) == kResultOk);
            [window setContentSize:NSMakeSize(size.getWidth(), size.getHeight())];
            [window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            NSView* knob = findVolumeKnob(window.contentView);
            CHECK(knob);
            CHECK(std::lround(NSMaxX(knob.frame)) == size.getWidth() - 6);
            CHECK([[knob valueForKey:@"volume"] doubleValue] == controller->getParamNormalized(controlID));
            const size_t edits = handler.edits.size();
            const double before = controller->getParamNormalized(controlID);
            clickNativeEditor(nullptr, window, knob, NSMakePoint(20, 22), NSMakePoint(0, 30));
            CHECK(std::abs(controller->getParamNormalized(controlID) - (before - 0.2)) < 1e-6);
            CHECK(handler.edits.size() > edits && handler.edits.back().first == controlID && !handler.editing);
            NSPoint location = [knob convertPoint:NSMakePoint(20, 22) toView:nil];
            NSEvent* reset = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:location
                modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
                windowNumber:window.windowNumber context:nil eventNumber:0 clickCount:2 pressure:1];
            [knob mouseDown:reset];
            CHECK(controller->getParamNormalized(controlID) == defaultValue);
            CHECK(!handler.editing);
            NSView* hintView = knob.superview.subviews[5];
            CHECK([hintView isKindOfClass:NSTextField.class]);
            NSTextField* hint = (NSTextField*)hintView;
            const size_t editsBeforeHover = handler.edits.size();
            NSEvent* entered = [NSEvent enterExitEventWithType:NSEventTypeMouseEntered location:location
                modifierFlags:0 timestamp:reset.timestamp windowNumber:window.windowNumber
                context:nil eventNumber:0 trackingNumber:0 userData:nullptr];
            [knob mouseEntered:entered];
            CHECK([hint.stringValue isEqualToString:[NSString stringWithUTF8String:controlHint(defaultValue).c_str()]]);
            CHECK([knob.accessibilityLabel isEqualToString:classIndex == 0 ? @"Master volume" : @"Effect mix"]);
            CHECK(handler.edits.size() == editsBeforeHover);
            const NSPoint savedCursor = NSEvent.mouseLocation;
            const double screenTop = NSMaxY(NSScreen.screens.firstObject.frame);
            const NSPoint screen = [window convertPointToScreen:location];
            CGWarpMouseCursorPosition(CGPointMake(screen.x, screenTop - screen.y));
            CHECK(controller->setParamNormalized(controlID, 0.42) == kResultOk);
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
            CHECK(std::abs([[knob valueForKey:@"volume"] doubleValue] - 0.42) < 1e-12);
            CHECK([hint.stringValue isEqualToString:[NSString stringWithUTF8String:
                controlHint(0.42).c_str()]]);
            CHECK(hint.frame.size.width >= [hint.stringValue sizeWithAttributes:
                @{NSFontAttributeName:hint.font}].width);
            if (screenshotDirectory && cycle == 0) {
                const auto directory = std::filesystem::u8path(screenshotDirectory);
                CHECK(std::filesystem::create_directories(directory) || std::filesystem::is_directory(directory));
                captureUtilityPixels(window, directory / (classIndex == 0 ? "instrument-volume.png" : "effect-mix.png"));
            }
            if (cycle == 0) verifyPresetPicker(window, controller, controlID, screenshotDirectory);
            NSWindow* pendingMenu = nil;
            if (cycle == 1) {
                NSControl* picker = findPresetPicker(window.contentView);
                [picker mouseDown:reset];
                pendingMenu = [picker valueForKey:@"panel"];
                CHECK(pendingMenu.visible);
            }
            CGWarpMouseCursorPosition(CGPointMake(savedCursor.x, screenTop - savedCursor.y));
            CHECK(view->removed() == kResultOk);
            CHECK(!pendingMenu.visible);
            frame.window = nil;
            [window close];
        }
        IAudioProcessor* processor = nullptr;
        CHECK(component->queryInterface(IAudioProcessor::iid, reinterpret_cast<void**>(&processor)) == kResultOk);
        if (classIndex == 1) verifyEffectMixAudio(factory);
        TestParameterChanges automation;
        automation.id = controlID;
        automation.value = 0.31;
        ProcessData flush{};
        flush.inputParameterChanges = &automation;
        CHECK(processor->process(flush) == kResultOk);
        CHECK(controller->getParamNormalized(controlID) == 0.31);
        processor->release();
        TestMemoryStream state;
        CHECK(component->getState(&state) == kResultOk);
        CHECK(state.bytes.size() >= 8 && state.bytes[4] == 5);
        view->release();
        controller->setComponentHandler(nullptr);
        controller->release();
        component->terminate();
        component->release();
        CHECK(factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk);
        CHECK(component->initialize(nullptr) == kResultOk);
        state.pos = 0;
        CHECK(component->setState(&state) == kResultOk);
        CHECK(component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk);
        CHECK(controller->getParamNormalized(controlID) == 0.31);
        const auto validState = state.bytes;
        const auto otherClass = nativeClassInfo(factory, classIndex == 0 ? "Sytrus" : "Fruity Phase Inverter");
        IComponent* other = nullptr;
        CHECK(factory->createInstance(otherClass.cid, IComponent::iid, reinterpret_cast<void**>(&other)) == kResultOk);
        CHECK(other->initialize(nullptr) == kResultOk);
        state.pos = 0;
        CHECK(other->setState(&state) == kResultFalse);
        other->terminate();
        other->release();
        for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity(), -0.1, 1.1}) {
            state.bytes = validState;
            memcpy(state.bytes.data() + state.bytes.size() - sizeof(invalid), &invalid, sizeof(invalid));
            state.pos = 0;
            CHECK(component->setState(&state) == kResultFalse);
            CHECK(controller->getParamNormalized(controlID) == 0.31);
        }
        state.bytes = validState;
        state.bytes.pop_back();
        state.pos = 0;
        CHECK(component->setState(&state) == kResultFalse);
        CHECK(controller->getParamNormalized(controlID) == 0.31);
        for (uint32_t legacyVersion : {1u, 2u, 3u, 4u}) {
            CHECK(controller->setParamNormalized(controlID, 0.11) == kResultOk);
            state.bytes = validState;
            replaceStateIdentity(state, info.name);
            memcpy(state.bytes.data() + 4, &legacyVersion, sizeof(legacyVersion));
            if (legacyVersion == 2) state.bytes.resize(state.bytes.size() - sizeof(double));
            else if (legacyVersion == 1) {
                uint32_t nameLength = 0, rawLength = 0;
                memcpy(&nameLength, state.bytes.data() + 8, sizeof(nameLength));
                CHECK(state.bytes.size() >= 20 + nameLength);
                memcpy(&rawLength, state.bytes.data() + 16 + nameLength, sizeof(rawLength));
                CHECK(state.bytes.size() >= 20 + nameLength + rawLength);
                state.bytes.resize(20 + nameLength + rawLength);
            }
            state.pos = 0;
            CHECK(component->setState(&state) == kResultOk);
            const double expected = legacyVersion >= 4 || (legacyVersion == 3 && classIndex == 0) ? 0.31 : defaultValue;
            CHECK(controller->getParamNormalized(controlID) == expected);
        }
        controller->release();
        component->terminate();
        component->release();
    }
    factory->release();
    CHECK(bundleExit());
    dlclose(bundle);
    fprintf(stderr, "Instrument volume / FX MIX, gestures, hover text, automation, reopening and state migration passed\n");
}

static NSView* findResizeGrip(NSView* parent) {
    if ([NSStringFromClass(object_getClass(parent)) isEqualToString:@"FloopHoleResizeGrip"]) return parent;
    for (NSView* child in parent.subviews)
        if (NSView* found = findResizeGrip(child)) return found;
    return nil;
}

static void checkBridgeEditorLayout(NSWindow* window, int width, int height, bool resizable) {
    NSView* editor = findNativeEditor(window.contentView);
    NSControl* popup = findPresetPicker(window.contentView);
    NSView* grip = findResizeGrip(window.contentView);
    CHECK(editor && popup && grip && grip.hidden == !resizable);
    const auto layout = flbr::BridgeToolbarLayout::compute(width);
    NSView* toolbar = popup.superview;
    CHECK(std::lround(toolbar.frame.size.width) == width);
    CHECK(std::lround(toolbar.frame.size.height) == layout.height);
    CHECK(toolbar.subviews.count == flbr::kBridgeToolbarControlCount);
    for (NSView* control in toolbar.subviews) CHECK(NSContainsRect(toolbar.bounds, control.frame));
    CHECK(std::lround(editor.frame.size.width) == width);
    CHECK(std::lround(editor.frame.size.height) == height);
    CHECK(NSEqualPoints(editor.frame.origin, NSZeroPoint));
    const int footer = resizable ? flbr::kBridgeResizeFooterHeight : 0;
    CHECK(std::lround(editor.superview.frame.origin.y) == footer);
    CHECK(std::lround(toolbar.frame.origin.y) == height + footer);
    Class drawingClass = NSClassFromString(@"D1NSView");
    for (NSView* child in editor.subviews) {
        if (!drawingClass || ![child isKindOfClass:drawingClass]) continue;
        CHECK(NSContainsRect(NSInsetRect(editor.bounds, -1, -1), child.frame));
    }
    if (resizable) CHECK(NSContainsRect(grip.superview.bounds, grip.frame));
}

static void verifyVSTEditorResizing(const char* screenshotDirectory) {
    void* bundle = dlopen("./build/VST3/FloopHole.vst3/Contents/MacOS/FloopHole", RTLD_NOW);
    CHECK(bundle);
    auto entry = reinterpret_cast<BundleEntryFn>(dlsym(bundle, "bundleEntry"));
    auto factoryFn = reinterpret_cast<GetPluginFactoryFn>(dlsym(bundle, "GetPluginFactory"));
    auto bundleExit = reinterpret_cast<BundleExitFn>(dlsym(bundle, "bundleExit"));
    CHECK(entry && factoryFn && bundleExit && entry(nullptr));
    IPluginFactory* factory = factoryFn();
    CHECK(factory);
    const std::filesystem::path directory = screenshotDirectory ?
        std::filesystem::u8path(screenshotDirectory) : std::filesystem::path{};
    if (!directory.empty()) CHECK(std::filesystem::create_directories(directory) || std::filesystem::is_directory(directory));
    struct Case {
        const char* name;
        int classIndex;
        bool resizable;
        int width, height, minWidth, minHeight, maxWidth, maxHeight;
    };
    const Case cases[] = {
        {"DirectWave", 0, true, 990, 628, 990, 600, 32768, 32768},
        {"Fruity Big Clock", 1, true, 200, 150, 200, 130, 32768, 32768},
        {"Wave Candy", 1, false, 592, 315, 592, 315, 592, 315},
        {"FL Keys", 0, true, 480, 270, 480, 183, 480, 270},
        {"Sytrus", 0, true, 846, 450, 850, 450, 2112, 1125},
        {"Emphasis", 1, true, 740, 366, 740, 366, 1480, 732},
        {"Frequency Splitter", 1, true, 770, 340, 770, 340, 32767, 32767},
        {"Fruity PanOMatic", 1, true, 300, 160, 300, 160, 32768, 32768},
        {"Fruity Parametric EQ 2", 1, true, 700, 350, 600, 285, 32767, 32767},
        {"Fruity Spectroman", 1, true, 285, 159, 285, 159, 32768, 32768},
        {"Low Lifter", 1, true, 680, 280, 680, 280, 32768, 32768},
        {"Multiband Delay", 1, true, 560, 290, 560, 290, 32768, 32768},
        {"Spreader", 1, true, 540, 255, 540, 255, 32768, 32768},
        {"Tuner", 1, true, 300, 360, 200, 200, 32767, 32767},
        {"Fruity Bass Boost", 1, false, 335, 55, 335, 55, 335, 55},
        {"Fruity Center", 1, false, 170, 55, 170, 55, 170, 55},
        {"Fruity Fast LP", 1, false, 335, 55, 335, 55, 335, 55},
        {"Fruity Mute 2", 1, false, 335, 55, 335, 55, 335, 55},
        {"Fruity Phase Inverter", 1, false, 170, 55, 170, 55, 170, 55},
        {"Fruity dB meter", 1, true, 100, 270, 100, 270, 32768, 32768},
        {"LuxeVerb", 1, true, 940, 450, 940, 225, 940, 450},
        {"Newtone", 1, true, 728, 582, 657, 296, 32768, 32768}};
    for (const auto& test : cases) {
        PClassInfo info{};
        info = nativeClassInfo(factory, test.name);
        IComponent* component = nullptr;
        CHECK(factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk);
        CHECK(component && component->initialize(nullptr) == kResultOk);
        TestMemoryStream state;
        const auto write = [&](const void* bytes, int32 size) {
            CHECK(state.write(const_cast<void*>(bytes), size, nullptr) == kResultOk);
        };
        const uint32_t version = 1, nameSize = static_cast<uint32_t>(strlen(test.name)), rawSize = 0;
        const int32_t preset = -1;
        write("FLBR", 4);
        write(&version, 4);
        write(&nameSize, 4);
        write(test.name, nameSize);
        write(&preset, 4);
        write(&rawSize, 4);
        state.pos = 0;
        CHECK(component->setState(&state) == kResultOk);
        IEditController* controller = nullptr;
        CHECK(component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk);
        TestComponentHandler handler;
        CHECK(controller->setComponentHandler(&handler) == kResultOk);
        TestPlugFrame frame;
        IPlugView* view = controller->createView(ViewType::kEditor);
        CHECK(view && view->setFrame(&frame) == kResultOk);
        CHECK(view->canResize() == (test.resizable ? kResultTrue : kResultFalse));
        int previousWidth = test.width, previousHeight = test.height;
        for (int cycle = 0; cycle < 3; ++cycle) {
            @autoreleasepool {
                NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 1200, 900)
                    styleMask:NSWindowStyleMaskTitled
                    backing:NSBackingStoreBuffered defer:NO];
                window.releasedWhenClosed = NO;
                frame.window = window;
                CHECK(view->attached((__bridge void*)window.contentView, kPlatformTypeNSView) == kResultOk);
                NSView* editor = findNativeEditor(window.contentView);
                CHECK(editor);
                fprintf(stderr, "%s resize cycle %d initial view %s; expected %d x %d\n",
                    test.name, cycle, NSStringFromRect(editor.frame).UTF8String, previousWidth, previousHeight);
                CHECK(std::lround(editor.frame.size.width) == previousWidth);
                CHECK(std::lround(editor.frame.size.height) == previousHeight);
                NSView* viewport = nil;
                NSSize viewportInset = NSZeroSize;
                Class drawingClass = NSClassFromString(@"D1NSView");
                for (NSView* child in editor.subviews) {
                    if (drawingClass && [child isKindOfClass:drawingClass] &&
                        std::abs(child.frame.size.width - previousWidth) <= 1 &&
                        std::abs(child.frame.size.height - previousHeight) <= 1) {
                        viewport = child;
                        viewportInset = NSMakeSize(previousWidth - child.frame.size.width,
                            previousHeight - child.frame.size.height);
                        break;
                    }
                }
                if (strcmp(test.name, "Fruity Big Clock") != 0) CHECK(viewport);
                CHECK(view->canResize() == (test.resizable ? kResultTrue : kResultFalse));
                checkBridgeEditorLayout(window, previousWidth, previousHeight, test.resizable);
                ViewRect size{};
                CHECK(view->getSize(&size) == kResultOk && view->onSize(&size) == kResultOk);
                CHECK(std::lround(editor.frame.size.width) == previousWidth);
                CHECK(std::lround(editor.frame.size.height) == previousHeight);
                [window setContentSize:NSMakeSize(size.getWidth(), size.getHeight())];
                [window makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
                [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
                ViewRect minimum(7, 11, 8, 12);
                CHECK(view->checkSizeConstraint(&minimum) == kResultOk);
                CHECK(minimum.left == 7 && minimum.top == 11);
                CHECK(minimum.getWidth() == test.minWidth);
                CHECK(minimum.getHeight() == flbr::bridgeEditorHeight(test.minWidth, test.minHeight, test.resizable));
                ViewRect maximum(0, 0, 100000, 100000);
                CHECK(view->checkSizeConstraint(&maximum) == kResultOk);
                CHECK(maximum.getWidth() == test.maxWidth);
                CHECK(maximum.getHeight() == flbr::bridgeEditorHeight(test.maxWidth, test.maxHeight, test.resizable));
                const auto resize = [&](int width, int height, int expectedWidth, int expectedHeight) {
                    ViewRect requested(0, 0, width, flbr::bridgeEditorHeight(expectedWidth, height, test.resizable));
                    CHECK(view->checkSizeConstraint(&requested) == kResultOk);
                    CHECK(view->onSize(&requested) == kResultOk);
                    CHECK(view->getSize(&size) == kResultOk);
                    fprintf(stderr, "%s requested native %d x %d; constrained wrapper %d x %d; actual %d x %d, native %s\n",
                        test.name, width, height, requested.getWidth(), requested.getHeight(),
                        size.getWidth(), size.getHeight(), NSStringFromRect(editor.frame).UTF8String);
                    CHECK(size.getWidth() == expectedWidth);
                    CHECK(size.getHeight() == flbr::bridgeEditorHeight(expectedWidth, expectedHeight, test.resizable));
                    CHECK(std::lround(editor.frame.size.width) == expectedWidth);
                    CHECK(std::lround(editor.frame.size.height) == expectedHeight);
                    [window setContentSize:NSMakeSize(size.getWidth(), size.getHeight())];
                    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
                    CHECK(view->onSize(&size) == kResultOk);
                    const int calls = frame.resizeCalls;
                    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
                    CHECK(frame.resizeCalls == calls);
                    checkBridgeEditorLayout(window, expectedWidth, expectedHeight, test.resizable);
                    if (viewport) {
                        CHECK(std::abs(viewport.frame.size.width + viewportInset.width - expectedWidth) <= 1);
                        CHECK(std::abs(viewport.frame.size.height + viewportInset.height - expectedHeight) <= 1);
                    }
                    previousWidth = expectedWidth;
                    previousHeight = expectedHeight;
                    if (!directory.empty() && cycle == 0)
                        captureUtilityPixels(window, directory / (std::string(test.name) + "-" +
                            std::to_string(expectedWidth) + "x" + std::to_string(expectedHeight) + ".png"));
                };
                if (strcmp(test.name, "DirectWave") == 0) {
                    resize(1240, 800, 1240, 800);
                    const size_t edits = handler.edits.size();
                    clickNativeEditor(nullptr, window, editor, NSMakePoint(225, 800 - 192), NSMakePoint(0, 25));
                    CHECK(handler.edits.size() > edits);
                    CHECK(handler.edits.back().first < 2048);
                    resize(1, 1, 990, 600);
                    resize(1100, 700, 1100, 700);
                } else if (strcmp(test.name, "Fruity Big Clock") == 0) {
                    resize(1100, previousHeight, 1100, previousHeight);
                    resize(1100, 600, 1100, 600);
                    resize(1, 1, 200, 130);
                } else if (strcmp(test.name, "FL Keys") == 0) {
                    resize(900, 226, 480, 183);
                    resize(900, 227, 480, 270);
                } else if (strcmp(test.name, "Sytrus") == 0) {
                    const int calls = frame.resizeCalls;
                    clickNativeEditor(nullptr, window, editor, NSMakePoint(previousWidth - 15, 16));
                    CHECK(view->getSize(&size) == kResultOk);
                    CHECK(editor.frame.size.height < previousHeight - 50);
                    CHECK(size.getHeight() == flbr::bridgeEditorHeight(previousWidth,
                        std::lround(editor.frame.size.height), true));
                    CHECK(frame.resizeCalls > calls && view->onSize(&size) == kResultOk);
                    clickNativeEditor(nullptr, window, editor, NSMakePoint(previousWidth - 15, 16));
                    CHECK(std::lround(editor.frame.size.height) == previousHeight);
                    resize(1100, 600, 1100, 600);
                    resize(1, 1, 850, 450);
                } else if (strcmp(test.name, "LuxeVerb") == 0) {
                    resize(1100, 225, 940, 225);
                    resize(700, 450, 940, 450);
                } else if (test.resizable) {
                    resize(test.width + 100, test.height + 60, test.width + 100, test.height + 60);
                    resize(1, 1, test.minWidth, test.minHeight);
                } else {
                    resize(1100, 800, test.width, test.height);
                    ViewRect forced(0, 0, 1100, 800);
                    CHECK(view->onSize(&forced) == kResultOk);
                    checkBridgeEditorLayout(window, test.width, test.height, false);
                }
                if (test.resizable) {
                    NSView* grip = findResizeGrip(window.contentView);
                    CHECK(grip && !grip.hidden && !(window.styleMask & NSWindowStyleMaskResizable));
                    [grip resetCursorRects];
                    const int calls = frame.resizeCalls;
                    CHECK(view->getSize(&size) == kResultOk);
                    const int startHeight = size.getHeight();
                    clickNativeEditor(nullptr, window, grip, NSMakePoint(7, 7), NSMakePoint(100, 60));
                    CHECK(frame.resizeCalls > calls);
                    CHECK(view->getSize(&size) == kResultOk);
                    CHECK(size.getWidth() >= previousWidth);
                    CHECK(editor.frame.size.height >= previousHeight);
                    if (test.maxWidth > previousWidth) CHECK(size.getWidth() == previousWidth + 100);
                    if (strcmp(test.name, "FL Keys") != 0 && strcmp(test.name, "LuxeVerb") != 0)
                        CHECK(size.getHeight() == startHeight + 60);
                    previousWidth = std::lround(editor.frame.size.width);
                    previousHeight = std::lround(editor.frame.size.height);
                    checkBridgeEditorLayout(window, previousWidth, previousHeight, true);
                }
                CHECK(view->removed() == kResultOk);
                frame.window = nil;
                [window close];
            }
        }
        TestMemoryStream saved;
        CHECK(component->getState(&saved) == kResultOk);
        view->release();
        controller->setComponentHandler(nullptr);
        controller->release();
        component->terminate();
        component->release();
        if (strcmp(test.name, "DirectWave") == 0) {
            CHECK(factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk);
            CHECK(component->initialize(nullptr) == kResultOk);
            saved.pos = 0;
            CHECK(component->setState(&saved) == kResultOk);
            CHECK(component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk);
            view = controller->createView(ViewType::kEditor);
            CHECK(view && view->setFrame(&frame) == kResultOk);
            NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 1200, 900)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            window.releasedWhenClosed = NO;
            CHECK(view->attached((__bridge void*)window.contentView, kPlatformTypeNSView) == kResultOk);
            NSView* editor = findNativeEditor(window.contentView);
            CHECK(editor && std::lround(editor.frame.size.width) == test.width &&
                  std::lround(editor.frame.size.height) == test.height);
            CHECK(view->removed() == kResultOk);
            [window close];
            view->release();
            controller->release();
            component->terminate();
            component->release();
        }
        fprintf(stderr, "%s: native constraints, host resize, acknowledgement and reopening passed\n", test.name);
    }
    factory->release();
    CHECK(bundleExit());
    dlclose(bundle);
}

static void checkScrollViewClasses(NSView* view, bool installed) {
    if ([view isKindOfClass:NSClassFromString(@"D1NSView")]) {
        Class actual = object_getClass(view);
        CHECK([NSStringFromClass(actual) hasPrefix:@"FLBridgeScrollWheel_"] == installed);
        CHECK(view.class == (installed ? class_getSuperclass(actual) : actual));
    }
    for (NSView* child in view.subviews) checkScrollViewClasses(child, installed);
}

static void scrollNativeEditor(NSWindow* window, NSView* editor, int32_t delta, bool precise,
                               CGScrollPhase phase = CGScrollPhase{},
                               CGMomentumScrollPhase momentum = kCGMomentumScrollPhaseNone, bool nativeOnly = false) {
    const NSPoint local = NSMakePoint(editor.bounds.size.width - 130, 240);
    const NSPoint location = [editor convertPoint:local toView:nil];
    NSPoint screen = [window convertPointToScreen:location];
    screen.y = NSMaxY(NSScreen.screens.firstObject.frame) - screen.y;
    NSView* target = [editor hitTest:[editor convertPoint:local toView:editor.superview]];
    CHECK(target);
    const NSPoint savedCursor = NSEvent.mouseLocation;
    CGWarpMouseCursorPosition(screen);
    CGEventRef cg = CGEventCreateScrollWheelEvent(nullptr,
        precise ? kCGScrollEventUnitPixel : kCGScrollEventUnitLine, 1, delta);
    CHECK(cg);
    CGEventSetLocation(cg, screen);
    CGEventSetIntegerValueField(cg, kCGScrollWheelEventScrollPhase, phase);
    CGEventSetIntegerValueField(cg, kCGScrollWheelEventMomentumPhase, momentum);
    FLBridgeTestScrollEvent* event = [FLBridgeTestScrollEvent new];
    event.source = [NSEvent eventWithCGEvent:cg];
    event.hostWindow = window;
    event.hostLocation = location;
    CHECK(event.hasPreciseScrollingDeltas == precise);
    CHECK(std::abs(event.scrollingDeltaY - delta) < 0.001);
    CHECK(event.phase == (phase == kCGScrollPhaseBegan ? NSEventPhaseBegan :
        phase == kCGScrollPhaseChanged ? NSEventPhaseChanged :
        phase == kCGScrollPhaseEnded ? NSEventPhaseEnded : NSEventPhaseNone));
    CHECK(event.momentumPhase == (momentum == kCGMomentumScrollPhaseBegin ? NSEventPhaseBegan :
        momentum == kCGMomentumScrollPhaseContinue ? NSEventPhaseChanged :
        momentum == kCGMomentumScrollPhaseEnd ? NSEventPhaseEnded : NSEventPhaseNone));
    if (nativeOnly) {
        auto original = reinterpret_cast<void (*)(id, SEL, NSEvent*)>(
            class_getMethodImplementation(class_getSuperclass(object_getClass(target)), @selector(scrollWheel:)));
        original(target, @selector(scrollWheel:), event);
    } else {
        [target scrollWheel:event];
    }
    CFRelease(cg);
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.03]];
    CGWarpMouseCursorPosition(CGPointMake(savedCursor.x, NSMaxY(NSScreen.screens.firstObject.frame) - savedCursor.y));
}

static void verifyFlexScrolling(const char* screenshot) {
    void* bundle = dlopen("./build/VST3/FloopHole.vst3/Contents/MacOS/FloopHole", RTLD_NOW);
    CHECK(bundle);
    auto entry = reinterpret_cast<BundleEntryFn>(dlsym(bundle, "bundleEntry"));
    auto factoryFn = reinterpret_cast<GetPluginFactoryFn>(dlsym(bundle, "GetPluginFactory"));
    auto bundleExit = reinterpret_cast<BundleExitFn>(dlsym(bundle, "bundleExit"));
    CHECK(entry && factoryFn && bundleExit && entry(nullptr));
    IPluginFactory* factory = factoryFn();
    PClassInfo info{};
    CHECK(factory);
    info = nativeClassInfo(factory, "FLEX");
    IComponent* component = nullptr;
    CHECK(factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk);
    CHECK(component && component->initialize(nullptr) == kResultOk);
    IEditController* controller = nullptr;
    CHECK(component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk);
    TestComponentHandler handler;
    CHECK(controller->setComponentHandler(&handler) == kResultOk);
    std::vector<double> initial;
    const auto changedParameters = [&] {
        int changed = 0;
        for (size_t i = 0; i < initial.size(); ++i)
            if (std::abs(controller->getParamNormalized(static_cast<ParamID>(i)) - initial[i]) > 0.011) ++changed;
        return changed;
    };
    TestPlugFrame frame;
    IPlugView* view = controller->createView(ViewType::kEditor);
    CHECK(view && view->setFrame(&frame) == kResultOk);
    for (int cycle = 0; cycle < 3; ++cycle) {
        @autoreleasepool {
            NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100 + cycle * 35, 100, 1200, 900)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            window.releasedWhenClosed = NO;
            NSView* host = [[NSView alloc] initWithFrame:NSMakeRect(17, 23, 1150, 850)];
            [window.contentView addSubview:host];
            CHECK(view->attached((__bridge void*)host, kPlatformTypeNSView) == kResultOk);
            ViewRect size{};
            CHECK(view->getSize(&size) == kResultOk);
            [host setFrameSize:NSMakeSize(size.getWidth(), size.getHeight())];
            [window setContentSize:NSMakeSize(size.getWidth() + 34, size.getHeight() + 46)];
            [window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
            NSView* editor = findNativeEditor(host);
            CHECK(editor && editor.isFlipped);
            checkScrollViewClasses(editor, true);
            clickNativeEditor(nullptr, window, editor, NSMakePoint(editor.bounds.size.width - 150, 160));
            if (cycle == 0) {
                for (int i = 0; i < controller->getParameterCount() - 5; ++i)
                    initial.push_back(controller->getParamNormalized(i));
                for (int i = 0; i < 10; ++i)
                    scrollNativeEditor(window, editor, -3, false, CGScrollPhase{}, kCGMomentumScrollPhaseNone, true);
                clickNativeEditor(nullptr, window, editor, NSMakePoint(editor.bounds.size.width - 150, 160));
                CHECK(changedParameters() == 0);
                fprintf(stderr, "FLEX original native wheel handler: no preset-list movement\n");
            }
            for (bool precise : {false, true}) {
                if (precise) scrollNativeEditor(window, editor, 0, true, kCGScrollPhaseBegan);
                for (int i = 0; i < 10; ++i)
                    scrollNativeEditor(window, editor, precise ? -20 : -3, precise,
                        precise ? kCGScrollPhaseChanged : CGScrollPhase{});
                if (precise) {
                    scrollNativeEditor(window, editor, 0, true, kCGScrollPhaseEnded);
                    scrollNativeEditor(window, editor, -15, true, CGScrollPhase{}, kCGMomentumScrollPhaseBegin);
                    scrollNativeEditor(window, editor, -10, true, CGScrollPhase{}, kCGMomentumScrollPhaseContinue);
                    scrollNativeEditor(window, editor, 0, true, CGScrollPhase{}, kCGMomentumScrollPhaseEnd);
                }
                CHECK(changedParameters() == 0);
                clickNativeEditor(nullptr, window, editor, NSMakePoint(editor.bounds.size.width - 150, 160));
                fprintf(stderr, "FLEX %s cycle %d: revealed preset changes %d parameters\n",
                    precise ? "trackpad" : "wheel", cycle, changedParameters());
                CHECK(changedParameters() > 0);
                if (screenshot && cycle == 0 && precise) {
                    NSTask* capture = [NSTask new];
                    capture.executableURL = [NSURL fileURLWithPath:@"/usr/sbin/screencapture"];
                    capture.arguments = @[@"-x", @"-o", @"-l", [NSString stringWithFormat:@"%ld", (long)window.windowNumber],
                        [NSString stringWithUTF8String:screenshot]];
                    NSError* error = nil;
                    CHECK([capture launchAndReturnError:&error]);
                    [capture waitUntilExit];
                    CHECK(capture.terminationStatus == 0);
                }
                if (precise) scrollNativeEditor(window, editor, 0, true, kCGScrollPhaseBegan);
                for (int i = 0; i < 40; ++i)
                    scrollNativeEditor(window, editor, precise ? 20 : 3, precise,
                        precise ? kCGScrollPhaseChanged : CGScrollPhase{});
                if (precise) scrollNativeEditor(window, editor, 0, true, kCGScrollPhaseEnded);
                clickNativeEditor(nullptr, window, editor, NSMakePoint(editor.bounds.size.width - 150, 160));
                for (size_t i = 0; i < initial.size(); ++i)
                    if (std::abs(controller->getParamNormalized(static_cast<ParamID>(i)) - initial[i]) > 0.011)
                        fprintf(stderr, "Return to first preset: %zu initial=%.4f now=%.4f\n", i, initial[i],
                            controller->getParamNormalized(static_cast<ParamID>(i)));
                CHECK(changedParameters() == 0);
            }
            const ParamID macro = 10;
            CHECK(controller->setParamNormalized(macro, 0.25) == kResultOk);
            const size_t edits = handler.edits.size();
            clickNativeEditor(nullptr, window, editor, NSMakePoint(63, 525), NSMakePoint(0, -25));
            CHECK(controller->getParamNormalized(macro) > 0.26 && handler.edits.size() > edits);
            CHECK(controller->setParamNormalized(macro, initial[macro]) == kResultOk);
            CHECK(view->removed() == kResultOk);
            checkScrollViewClasses(editor, false);
            CHECK(view->removed() == kResultOk);
            [window close];
        }
    }
    view->release();
    controller->release();
    component->terminate();
    component->release();
    factory->release();
    CHECK(bundleExit());
    dlclose(bundle);
    fprintf(stderr, "FLEX wheel, trackpad, bidirectional preset scrolling, knob input, reopening in its fixed class passed\n");
}

static void verifyHarmorPartAutomation(const char* screenshot) {
    flbr::FLPluginInstance native;
    CHECK(native.loadPluginByName("Harmor"));
    const ParamID partA = 4;
    const std::string nameA = native.getParamName(partA);
    CHECK(nameA.compare(0, 4, "A - ") == 0);
    const std::string nameB = "B - " + nameA.substr(4);
    int partB = -1;
    for (int i = 0; i < native.getNumParams(); ++i)
        if (native.getParamName(i) == nameB) partB = i;
    CHECK(partB > 127);
    const double initialB = native.getParamNormalized(partB);
    fprintf(stderr, "Harmor native controls: A=%u (%s), B=%d (%s), total=%d\n",
        partA, nameA.c_str(), partB, nameB.c_str(), native.getNumParams());

    void* bundle = dlopen("./build/VST3/FloopHole.vst3/Contents/MacOS/FloopHole", RTLD_NOW);
    CHECK(bundle);
    auto entry = reinterpret_cast<BundleEntryFn>(dlsym(bundle, "bundleEntry"));
    auto factoryFn = reinterpret_cast<GetPluginFactoryFn>(dlsym(bundle, "GetPluginFactory"));
    auto bundleExit = reinterpret_cast<BundleExitFn>(dlsym(bundle, "bundleExit"));
    CHECK(entry && factoryFn && bundleExit && entry(nullptr));
    IPluginFactory* factory = factoryFn();
    PClassInfo info{};
    CHECK(factory);
    info = nativeClassInfo(factory, "Harmor");
    IComponent* component = nullptr;
    CHECK(factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk);
    CHECK(component && component->initialize(nullptr) == kResultOk);
    IEditController* controller = nullptr;
    CHECK(component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk);
    TestComponentHandler handler;
    CHECK(controller->setComponentHandler(&handler) == kResultOk);
    TestPlugFrame frame;
    IPlugView* view = controller->createView(ViewType::kEditor);
    CHECK(view && view->setFrame(&frame) == kResultOk);
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 1100, 800)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed = NO;
    CHECK(view->attached((__bridge void*)window.contentView, kPlatformTypeNSView) == kResultOk);
    ViewRect size{};
    CHECK(view->getSize(&size) == kResultOk);
    [window setContentSize:NSMakeSize(size.getWidth(), size.getHeight())];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
    NSView* editor = findNativeEditor(window.contentView);
    CHECK(editor);
    clickNativeEditor(nullptr, window, editor, NSMakePoint(183, 40), NSMakePoint(0, -25));
    CHECK(!handler.edits.empty() && handler.edits.back().first == partA);
    fprintf(stderr, "Harmor A gesture: id=%u value=%.4f\n", handler.edits.back().first, handler.edits.back().second);
    const size_t beforeB = handler.edits.size();
    clickNativeEditor(nullptr, window, editor, NSMakePoint(350, 350));
    clickNativeEditor(nullptr, window, editor, NSMakePoint(183, 40), NSMakePoint(0, -25));
    TestMemoryStream gestureState;
    CHECK(component->getState(&gestureState) == kResultOk);
    uint32_t nameSize = 0, rawSize = 0;
    CHECK(gestureState.bytes.size() >= 20);
    memcpy(&nameSize, gestureState.bytes.data() + 8, 4);
    CHECK(nameSize <= gestureState.bytes.size() - 20);
    memcpy(&rawSize, gestureState.bytes.data() + 16 + nameSize, 4);
    const size_t offset = 20 + nameSize;
    CHECK(rawSize <= gestureState.bytes.size() - offset);
    CHECK(native.restorePluginState(gestureState.bytes.data() + offset, rawSize));
    fprintf(stderr, "Harmor B native value=%.4f; host edits=%zu\n",
        native.getParamNormalized(partB), handler.edits.size() - beforeB);
    CHECK(native.getParamNormalized(partB) > initialB + 0.01);
    CHECK(handler.edits.size() > beforeB && handler.edits.back().first == static_cast<ParamID>(partB));
    CHECK(!handler.editing);
    if (screenshot) {
        NSTask* capture = [NSTask new];
        capture.executableURL = [NSURL fileURLWithPath:@"/usr/sbin/screencapture"];
        capture.arguments = @[@"-x", @"-o", @"-l", [NSString stringWithFormat:@"%ld", (long)window.windowNumber],
            [NSString stringWithUTF8String:screenshot]];
        NSError* error = nil;
        CHECK([capture launchAndReturnError:&error]);
        [capture waitUntilExit];
        CHECK(capture.terminationStatus == 0);
    }

    CHECK(controller->getParameterCount() == 2053);
    std::vector<bool> exposed(controller->getParameterCount() - 5, false);
    for (int i = 0; i < controller->getParameterCount(); ++i) {
        ParameterInfo parameter{};
        CHECK(controller->getParameterInfo(i, parameter) == kResultOk);
        if (parameter.id < 20000) {
            CHECK(parameter.id < exposed.size());
            CHECK(!exposed[parameter.id]);
            exposed[parameter.id] = true;
            if (parameter.id == partA || parameter.id == static_cast<ParamID>(partB)) {
                const std::string expected = parameter.id == partA ? nameA : nameB;
                CHECK(std::equal(expected.begin(), expected.end(), parameter.title));
                CHECK(parameter.title[expected.size()] == 0 && (parameter.flags & ParameterInfo::kCanAutomate));
            }
        }
    }
    CHECK(std::all_of(exposed.begin(), exposed.end(), [](bool value) { return value; }));
    for (int i = 0; i < 132; ++i) {
        ParameterInfo parameter{};
        CHECK(controller->getParameterInfo(i, parameter) == kResultOk);
        CHECK(parameter.id == static_cast<ParamID>(i < 128 ? i : 20000 + i - 128));
    }
    CHECK(std::abs(controller->getParamNormalized(partB) - native.getParamNormalized(partB)) < 1e-6);
    IAudioProcessor* processor = nullptr;
    CHECK(component->queryInterface(IAudioProcessor::iid, reinterpret_cast<void**>(&processor)) == kResultOk);
    TestParameterChanges changes;
    ProcessData data{};
    data.inputParameterChanges = &changes;
    for (ParamID id : {partA, static_cast<ParamID>(partB), ParamID(792)}) {
        changes.id = id;
        for (double value : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            changes.value = value;
            CHECK(processor->process(data) == kResultOk);
            CHECK(std::abs(controller->getParamNormalized(id) - value) < 1e-6);
        }
    }
    CHECK(controller->setParamNormalized(partA, 0.25) == kResultOk);
    CHECK(controller->setParamNormalized(partB, 0.75) == kResultOk);
    CHECK(std::abs(controller->getParamNormalized(partA) - 0.25) < 1e-6);
    CHECK(std::abs(controller->getParamNormalized(partB) - 0.75) < 1e-6);
    TestMemoryStream saved;
    CHECK(component->getState(&saved) == kResultOk);
    CHECK(view->removed() == kResultOk);
    view->release();
    [window close];
    processor->release();
    controller->release();
    component->terminate();
    component->release();

    CHECK(factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk);
    CHECK(component->initialize(nullptr) == kResultOk);
    saved.pos = 0;
    CHECK(component->setState(&saved) == kResultOk);
    CHECK(component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk);
    CHECK(std::abs(controller->getParamNormalized(partA) - 0.25) < 1e-6);
    CHECK(std::abs(controller->getParamNormalized(partB) - 0.75) < 1e-6);
    controller->release();
    component->terminate();
    component->release();
    native.unloadPlugin();
    factory->release();
    CHECK(bundleExit());
    dlclose(bundle);
    fprintf(stderr, "Harmor Part A/B gestures, complete parameter exposure, automation and state restore passed\n");
}

static void checkSytrusLayout(NSView* editor) {
    CHECK(editor && editor.subviews.count > 0);
    NSView* canvas = editor.subviews.firstObject;
    CHECK(canvas.subviews.count >= 4);
    NSRect main = canvas.subviews[0].frame;
    NSView* matrixView = nil;
    // Operator pages have a different child order from the main page.
    for (NSView* panel in canvas.subviews) {
        if (NSMinX(panel.frame) > editor.bounds.size.width / 2 &&
            panel.frame.size.width > 100 && panel.frame.size.height > 100) {
            CHECK(matrixView == nil);
            matrixView = panel;
        }
    }
    CHECK(matrixView);
    NSRect matrix = matrixView.frame;
    const double gap = NSMinX(matrix) - NSMaxX(main);
    fprintf(stderr, "Sytrus layout: editor=%.1f main=%.1f matrix=%.1f gap=%.1f\n",
        editor.bounds.size.width, main.size.width, matrix.size.width, gap);
    CHECK(main.size.width > editor.bounds.size.width / 2);
    CHECK(gap >= 0 && gap < 16);
}

static void verifySytrusLayout(const char* screenshot) {
    flbr::FLPluginInstance instance;
    CHECK(instance.loadPluginByName("Sytrus"));
    std::vector<double> parameters;
    for (int i = 0; i < instance.getNumParams(); ++i) parameters.push_back(instance.getParamNormalized(i));
    std::vector<uint8_t> saved;
    CHECK(instance.savePluginState(saved));
    int fullHeight = 0, hiddenHeight = 0;
    for (int cycle = 0; cycle < 4; ++cycle) {
        @autoreleasepool {
            if (cycle == 2) {
                instance.unloadPlugin();
                CHECK(instance.loadPluginByName("Sytrus"));
                CHECK(instance.restorePluginState(saved.data(), saved.size()));
            }
            NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 1100, 800)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            window.releasedWhenClosed = NO;
            instance.onEditorResized = [window](int w, int h) { [window setContentSize:NSMakeSize(w, h)]; };
            int width = 0, height = 0;
            CHECK(instance.attachEditorToNSView((__bridge void*)window.contentView, width, height));
            NSView* editor = findNativeEditor(window.contentView);
            checkSytrusLayout(editor);
            if (cycle == 0) fullHeight = height;
            CHECK(height == (cycle == 1 ? hiddenHeight : fullHeight));
            CHECK(instance.getNumParams() == static_cast<int>(parameters.size()));
            for (int i = 0; i < instance.getNumParams(); ++i)
                CHECK(std::abs(instance.getParamNormalized(i) - parameters[i]) < 1e-8);
            [window setContentSize:NSMakeSize(width, height)];
            [window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            for (int i = 0; i < 10; ++i) {
                instance.idleEditor();
                [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            }
            if (cycle == 0 && screenshot) {
                NSTask* capture = [NSTask new];
                capture.executableURL = [NSURL fileURLWithPath:@"/usr/sbin/screencapture"];
                capture.arguments = @[@"-x", @"-o", @"-l", [NSString stringWithFormat:@"%ld", (long)window.windowNumber],
                    [NSString stringWithUTF8String:screenshot]];
                NSError* error = nil;
                CHECK([capture launchAndReturnError:&error]);
                [capture waitUntilExit];
                CHECK(capture.terminationStatus == 0);
            }
            if (cycle < 2) {
                clickNativeEditor(&instance, window, editor, NSMakePoint(width - 15, 16));
                checkSytrusLayout(editor);
                if (cycle == 0) {
                    hiddenHeight = static_cast<int>(std::lround(editor.frame.size.height));
                    CHECK(hiddenHeight < fullHeight - 50);
                } else CHECK(std::abs(editor.frame.size.height - fullHeight) < 0.5);
            }
            instance.detachEditor();
            instance.onEditorResized = nullptr;
            [window close];
        }
    }
    fprintf(stderr, "Sytrus initial layout, keyboard visibility, state restore and reopening passed\n");
}

static void verifyVSTEditorReopening() {
    void* bundle = dlopen("./build/VST3/FloopHole.vst3/Contents/MacOS/FloopHole", RTLD_NOW);
    CHECK(bundle);
    auto entry = reinterpret_cast<BundleEntryFn>(dlsym(bundle, "bundleEntry"));
    auto factoryFn = reinterpret_cast<GetPluginFactoryFn>(dlsym(bundle, "GetPluginFactory"));
    auto bundleExit = reinterpret_cast<BundleExitFn>(dlsym(bundle, "bundleExit"));
    CHECK(entry && factoryFn && bundleExit && entry(nullptr));
    IPluginFactory* factory = factoryFn();
    PClassInfo info{};
    CHECK(factory);
    info = nativeClassInfo(factory, "Harmor");
    IComponent* component = nullptr;
    CHECK(factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk);
    CHECK(component && component->initialize(nullptr) == kResultOk);
    IEditController* controller = nullptr;
    CHECK(component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk);
    TestPlugFrame frame;
    IPlugView* view = controller->createView(ViewType::kEditor);
    CHECK(view && view->setFrame(&frame) == kResultOk);
    IPlugView* retiredView = nullptr;
    for (int cycle = 0; cycle < 6; ++cycle) {
        @autoreleasepool {
            NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 1200, 900)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            window.releasedWhenClosed = NO;
            NSView* host = [[NSView alloc] initWithFrame:window.contentView.bounds];
            [window.contentView addSubview:host];
            fprintf(stderr, "VST editor reopening cycle %d\n", cycle);
            CHECK(view->attached((__bridge void*)host, kPlatformTypeNSView) == kResultOk);
            [window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
            if (retiredView) {
                retiredView->release();
                retiredView = nullptr;
            }
            NSView* editor = findNativeEditor(host);
            CHECK(editor != nil);
            CHECK(editor.window == window && !editor.hiddenOrHasHiddenAncestor);
            CHECK([NSStringFromClass(object_getClass(editor)) hasPrefix:@"FLBridgeFileDrop_"]);
            const double before = controller->getParamNormalized(792);
            clickNativeEditor(nullptr, window, editor, NSMakePoint(945, 349));
            clickNativeEditor(nullptr, window, editor, NSMakePoint(482, 408), NSMakePoint(0, -25));
            CHECK(controller->getParamNormalized(792) > before + 0.01);
            CHECK(view->removed() == kResultOk);
            CHECK(view->removed() == kResultOk);
            [host removeFromSuperview];
            [window close];
        }
        if (cycle >= 2 && cycle < 5) {
            retiredView = view;
            view = controller->createView(ViewType::kEditor);
            CHECK(view && view->setFrame(&frame) == kResultOk);
        }
    }
    view->release();
    controller->release();
    component->terminate();
    component->release();

    // Verify Ableton Live's OPluginCocoaWindow + OPluginCocoaWindowDelegate sequence
    // (where the host NSWindow delegate does not implement windowDidBecomeKey:/windowDidResignKey:)
    // as well as deferred NSView window attachment.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
    info = nativeClassInfo(factory, "Newtone");
    CHECK(factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk);
    CHECK(component && component->initialize(nullptr) == kResultOk);
    CHECK(component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk);
    for (int cycle = 0; cycle < 3; ++cycle) {
        @autoreleasepool {
            view = controller->createView(ViewType::kEditor);
            CHECK(view && view->setFrame(&frame) == kResultOk);
            FLBridgeTestMinimalWindowDelegate* delegate = [FLBridgeTestMinimalWindowDelegate new];
            CHECK(![delegate respondsToSelector:@selector(windowDidBecomeKey:)]);
            CHECK(![delegate respondsToSelector:@selector(windowDidResignKey:)]);
            NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(120, 120, 900, 700)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            window.releasedWhenClosed = NO;
            window.delegate = delegate;
            [window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            NSView* hostView = nil;
            if (cycle == 0) {
                // Ableton Live attaches directly to [OPluginCocoaWindow contentView] while it is key.
                hostView = window.contentView;
                CHECK(view->attached((__bridge void*)hostView, kPlatformTypeNSView) == kResultOk);
            } else {
                // Also verify deferred attachment where hostView is added to window after attached().
                hostView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 900, 700)];
                CHECK(hostView.window == nil);
                CHECK(view->attached((__bridge void*)hostView, kPlatformTypeNSView) == kResultOk);
                [window.contentView addSubview:hostView];
            }
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
            NSView* editor = findNativeEditor(hostView);
            CHECK(editor != nil && editor.window == window && !editor.hiddenOrHasHiddenAncestor);
            NSWindow* vstWindow = nil;
            for (NSWindow* candidate in NSApp.windows) {
                if ([NSStringFromClass(object_getClass(candidate)) isEqualToString:@"VstWindow"] &&
                    candidate.parentWindow == window) {
                    vstWindow = candidate;
                    break;
                }
            }
            CHECK(vstWindow != nil && vstWindow.contentView != nil);
            // Explicitly trigger VstWindow's key-window delegate notifications against the minimal host delegate.
            [(id<NSWindowDelegate>)vstWindow.delegate windowDidBecomeKey:
                [NSNotification notificationWithName:NSWindowDidBecomeKeyNotification object:vstWindow]];
            [(id<NSWindowDelegate>)vstWindow.delegate windowDidResignKey:
                [NSNotification notificationWithName:NSWindowDidResignKeyNotification object:vstWindow]];
            CHECK(view->removed() == kResultOk);
            view->release();
            if (hostView != window.contentView) [hostView removeFromSuperview];
            window.delegate = nil;
            [window close];
        }
    }
    controller->release();
    component->terminate();
    component->release();
    factory->release();
    CHECK(bundleExit());
    dlclose(bundle);
    fprintf(stderr, "VST editor reopening and mouse-input checks completed\n");
}

static void verifyChannelSampleState(const char* samplePath) {
    void* bundle = dlopen("./build/VST3/FloopHole.vst3/Contents/MacOS/FloopHole", RTLD_NOW);
    CHECK(bundle);
    auto entry = reinterpret_cast<BundleEntryFn>(dlsym(bundle, "bundleEntry"));
    auto factoryFn = reinterpret_cast<GetPluginFactoryFn>(dlsym(bundle, "GetPluginFactory"));
    auto bundleExit = reinterpret_cast<BundleExitFn>(dlsym(bundle, "bundleExit"));
    CHECK(entry && factoryFn && bundleExit && entry(nullptr));
    IPluginFactory* factory = factoryFn();
    PClassInfo info{};
    CHECK(factory);
    info = nativeClassInfo(factory, "3x Osc");
    const auto initialState = [&](uint32_t version, const std::string& path) {
        TestMemoryStream stream;
        const char magic[] = "FLBR";
        const std::string name = "3x Osc";
        uint32_t length = name.size(), rawSize = 0, pathSize = path.size();
        int32_t preset = -1;
        stream.write(const_cast<char*>(magic), 4, nullptr);
        stream.write(&version, 4, nullptr);
        stream.write(&length, 4, nullptr);
        stream.write(const_cast<char*>(name.data()), length, nullptr);
        stream.write(&preset, 4, nullptr);
        stream.write(&rawSize, 4, nullptr);
        if (version == 2) {
            stream.write(&pathSize, 4, nullptr);
            stream.write(const_cast<char*>(path.data()), pathSize, nullptr);
        }
        stream.pos = 0;
        return stream;
    };
    TestMemoryStream saved;
    for (int pass = 0; pass < 2; ++pass) {
        IComponent* component = nullptr;
        CHECK(factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk);
        CHECK(component && component->initialize(nullptr) == kResultOk);
        TestMemoryStream input = pass == 0 ? initialState(2, samplePath) : saved;
        input.pos = 0;
        CHECK(component->setState(&input) == kResultOk);
        IEditController* controller = nullptr;
        CHECK(component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk);
        CHECK(controller->getParamNormalized(flbr::kParamMasterVolume) == (pass == 0 ? flbr::kDefaultMasterVolume : 0.37));
        CHECK(controller->setParamNormalized(flbr::kParamMasterVolume, 0.37) == kResultOk);
        TestMemoryStream output;
        CHECK(component->getState(&output) == kResultOk);
        uint32_t version = 0, nameSize = 0, rawSize = 0, pathSize = 0;
        memcpy(&version, output.bytes.data() + 4, 4);
        memcpy(&nameSize, output.bytes.data() + 8, 4);
        memcpy(&rawSize, output.bytes.data() + 16 + nameSize, 4);
        const size_t pathOffset = 20 + nameSize + rawSize;
        CHECK(version == 5 && output.bytes.size() >= pathOffset + 4);
        memcpy(&pathSize, output.bytes.data() + pathOffset, 4);
        CHECK(output.bytes.size() == pathOffset + 4 + pathSize + sizeof(double));
        CHECK(std::string(output.bytes.begin() + pathOffset + 4,
            output.bytes.begin() + pathOffset + 4 + pathSize) == samplePath);
        double masterVolume = 0;
        memcpy(&masterVolume, output.bytes.data() + pathOffset + 4 + pathSize, sizeof(masterVolume));
        CHECK(masterVolume == 0.37);
        saved = output;
        TestMemoryStream truncated = saved;
        truncated.pos = 0;
        truncated.bytes.pop_back();
        CHECK(component->setState(&truncated) == kResultFalse);
        truncated = saved;
        truncated.pos = 0;
        truncated.bytes.resize(pathOffset + 4 + pathSize - 1);
        CHECK(component->setState(&truncated) == kResultFalse);
        auto legacy = initialState(1, "");
        CHECK(component->setState(&legacy) == kResultOk);
        CHECK(controller->getParamNormalized(flbr::kParamMasterVolume) == flbr::kDefaultMasterVolume);
        TestMemoryStream cleared;
        CHECK(component->getState(&cleared) == kResultOk);
        memcpy(&version, cleared.bytes.data() + 4, 4);
        CHECK(version == 5);
        controller->release();
        component->terminate();
        component->release();
    }
    factory->release();
    CHECK(bundleExit());
    dlclose(bundle);
    fprintf(stderr, "Channel sample VST state: fresh-instance restore, legacy clearing and truncated-state rejection passed\n");
}

static void verifyEditorCoordinates() {
    for (const char* name : {"BooBass", "Sakura", "Sawer", "Harmor", "Newtone"}) {
        flbr::FLPluginInstance instance;
        CHECK(instance.loadPluginByName(name));
        auto getProp = reinterpret_cast<void (*)(void*, int, int, void*, int)>(dlsym(RTLD_DEFAULT, "MVObjectGetProp"));
        auto screenScale = reinterpret_cast<float (*)()>(dlsym(RTLD_DEFAULT, "MVScreenScale"));
        CHECK(getProp && screenScale);
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(300, 250, 1000, 800)
            styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        NSView* container = [[NSView alloc] initWithFrame:NSMakeRect(11, 17, 960, 700)];
        NSView* host = [[NSView alloc] initWithFrame:NSMakeRect(37, 53, 900, 600)];
        [window.contentView addSubview:container];
        [container addSubview:host];
        int width = 0, height = 0;
        CHECK(instance.attachEditorToNSView((__bridge void*)host, width, height));
        NSView* editor = host.subviews.firstObject;
        CHECK(editor != nil);
        NSWindow* backingWindow = nil;
        for (NSWindow* candidate in NSApp.windows) {
            if ([candidate respondsToSelector:NSSelectorFromString(@"savedContentView")] &&
                [candidate valueForKey:@"savedContentView"] == editor) {
                backingWindow = candidate;
                break;
            }
        }
        CHECK(backingWindow != nil);
        if (strcmp(name, "BooBass") == 0) {
            [window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            for (int i = 0; i < 10; ++i) {
                instance.idleEditor();
                [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            }
        }
        for (int position = 0; position < 4; ++position) {
            if (position == 1) [window setFrameOrigin:NSMakePoint(125, 150)];
            if (position == 2) [host setFrameOrigin:NSMakePoint(69, 81)];
            if (position == 3) [container setFrameOrigin:NSMakePoint(19, 29)];
            instance.idleEditor();
            NSRect screenRect = [window convertRectToScreen:[host convertRect:editor.frame toView:nil]];
            NSPoint origin = NSMakePoint(NSMinX(screenRect), NSMaxY(screenRect));
            const double scale = screenScale();
            NSPoint expected = NSMakePoint(origin.x * scale,
                (NSMaxY(NSScreen.screens.firstObject.frame) - origin.y) * scale);
            for (int property : {1001, 1002}) {
                NSRect rect = NSZeroRect;
                getProp((__bridge void*)backingWindow.delegate, property, 0, &rect, sizeof(rect));
                if (std::abs(rect.origin.x - expected.x) >= 0.5 || std::abs(rect.origin.y - expected.y) >= 0.5) {
                    fprintf(stderr, "%s position=%d property=%d origin=%s expected=%s\n", name, position,
                            property, NSStringFromPoint(rect.origin).UTF8String, NSStringFromPoint(expected).UTF8String);
                }
                CHECK(std::abs(rect.origin.x - expected.x) < 0.5);
                CHECK(std::abs(rect.origin.y - expected.y) < 0.5);
            }
            if (strcmp(name, "BooBass") != 0) continue;
            instance.setParamNormalized(0, 0.5, true);
            int parameterChanges = 0;
            instance.onParamChangedFromPlug = [&](int parameter, double) {
                if (parameter == 0) ++parameterChanges;
            };
            const NSPoint savedCursor = NSEvent.mouseLocation;
            const double screenTop = NSMaxY(NSScreen.screens.firstObject.frame);
            for (NSEventType type : {NSEventTypeMouseMoved, NSEventTypeLeftMouseDown,
                                    NSEventTypeLeftMouseDragged, NSEventTypeLeftMouseUp}) {
                NSPoint local = NSMakePoint(60, type == NSEventTypeLeftMouseDragged ? 55 : 75);
                NSPoint location = [editor convertPoint:local toView:nil];
                NSPoint screen = [window convertPointToScreen:location];
                CGWarpMouseCursorPosition(CGPointMake(screen.x, screenTop - screen.y));
                NSEvent* event = [NSEvent mouseEventWithType:type location:location modifierFlags:0
                    timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber
                    context:nil eventNumber:1 clickCount:1 pressure:1];
                if (type == NSEventTypeMouseMoved) [editor mouseMoved:event];
                else if (type == NSEventTypeLeftMouseDown) [editor mouseDown:event];
                else if (type == NSEventTypeLeftMouseDragged) [editor mouseDragged:event];
                else [editor mouseUp:event];
                instance.idleEditor();
            }
            CGWarpMouseCursorPosition(CGPointMake(savedCursor.x, screenTop - savedCursor.y));
            instance.onParamChangedFromPlug = nullptr;
            CHECK(parameterChanges > 0);
            CHECK(std::abs(instance.getParamNormalized(0) - 0.5) > 0.01);
        }
        instance.detachEditor();
        CHECK(!backingWindow.delegate || [backingWindow.delegate class] == NSClassFromString(@"MVWindow"));
        [window orderOut:nil];
        printf("%s: editor screen coordinates and moved-parent checks passed\n", name);
    }
}

static void verifyRealtimeKnobs() {
    for (const char* name : {"BooBass", "Fruity Parametric EQ 2"}) {
        flbr::FLPluginInstance instance;
        CHECK(instance.loadPluginByName(name));
        instance.setSampleRate(44100);
        instance.setBlockSize(64);
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1000, 800)
            styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        int width = 0, height = 0;
        CHECK(instance.attachEditorToNSView((__bridge void*)window.contentView, width, height));
        instance.noteOn(0, 60, 0.8f);
        std::atomic<bool> finished{false};
        std::vector<double> times;
        times.reserve(512);
        int silentBlocks = 0;
        std::thread render([&] {
            float input[64], left[64], right[64];
            for (int i = 0; i < 64; ++i) input[i] = 0.25f * std::sin(i * 0.1f);
            auto nextBlock = std::chrono::steady_clock::now();
            for (int block = 0; block < 512; ++block) {
                const auto begin = std::chrono::steady_clock::now();
                instance.processAudio(input, input, left, right, 64, 120, 0, false);
                times.push_back(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - begin).count());
                bool silent = true;
                for (int i = 0; i < 64; ++i) {
                    CHECK(std::isfinite(left[i]) && std::isfinite(right[i]));
                    silent = silent && left[i] == 0 && right[i] == 0;
                }
                if (silent) ++silentBlocks;
                nextBlock += std::chrono::nanoseconds(64 * 1000000000LL / 44100);
                std::this_thread::sleep_until(nextBlock);
            }
            finished = true;
        });
        int changes = 0;
        while (!finished) {
            instance.setParamNormalized(0, changes % 2 ? 0.25 : 0.75, true);
            instance.idleEditor();
            ++changes;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        render.join();
        std::sort(times.begin(), times.end());
        const double percentile99 = times[times.size() * 99 / 100];
        printf("%s: %d knob changes, %d silent blocks, 64-frame render p99 %.3f ms, max %.3f ms\n",
               name, changes, silentBlocks, percentile99, times.back());
        CHECK(silentBlocks == 0);
        CHECK(percentile99 < 64 * 1000.0 / 44100);
        instance.detachEditor();
    }
}

static void verifySamplerInstruments(const char* directWaveBank) {
    void* bundle = dlopen("./build/VST3/FloopHole.vst3/Contents/MacOS/FloopHole", RTLD_NOW);
    CHECK(bundle);
    auto entry = reinterpret_cast<BundleEntryFn>(dlsym(bundle, "bundleEntry"));
    auto factoryFn = reinterpret_cast<GetPluginFactoryFn>(dlsym(bundle, "GetPluginFactory"));
    auto bundleExit = reinterpret_cast<BundleExitFn>(dlsym(bundle, "bundleExit"));
    CHECK(entry && factoryFn && bundleExit && entry(nullptr));
    IPluginFactory* factory = factoryFn();
    CHECK(factory);
    for (const char* name : {"DirectWave", "FL Keys"}) {
        const auto info = nativeClassInfo(factory, name);
        TestMemoryStream state;
        {
            flbr::FLPluginInstance source;
            CHECK(source.loadPluginByName(name));
            if (strcmp(name, "DirectWave") == 0) CHECK(source.loadPresetFromFile(directWaveBank));
            std::vector<uint8_t> raw;
            CHECK(source.savePluginState(raw));
            const auto write = [&](const void* data, int32 size) {
                CHECK(state.write(const_cast<void*>(data), size, nullptr) == kResultOk);
            };
            const uint32_t version = 1, nameSize = static_cast<uint32_t>(strlen(name));
            const uint32_t rawSize = static_cast<uint32_t>(raw.size());
            const int32_t preset = -1;
            write("FLBR", 4);
            write(&version, 4);
            write(&nameSize, 4);
            write(name, nameSize);
            write(&preset, 4);
            write(&rawSize, 4);
            write(raw.data(), rawSize);
        }
        for (int cycle = 0; cycle < 2; ++cycle) {
            IComponent* component = nullptr;
            CHECK(factory->createInstance(info.cid, IComponent::iid,
                                          reinterpret_cast<void**>(&component)) == kResultOk);
            CHECK(component && component->initialize(nullptr) == kResultOk);
            state.pos = 0;
            CHECK(component->setState(&state) == kResultOk);
            IEditController* controller = nullptr;
            IAudioProcessor* processor = nullptr;
            CHECK(component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk);
            CHECK(component->queryInterface(IAudioProcessor::iid, reinterpret_cast<void**>(&processor)) == kResultOk);
            CHECK(controller->getParameterCount() == 2053);
            ParameterInfo last{};
            CHECK(controller->getParameterInfo(2051, last) == kResultOk);
            CHECK(last.id == 2047 && (last.flags & ParameterInfo::kCanAutomate));
            for (int i = 0; i < 132; ++i) {
                ParameterInfo parameter{};
                CHECK(controller->getParameterInfo(i, parameter) == kResultOk);
                CHECK(parameter.id == static_cast<ParamID>(i < 128 ? i : 20000 + i - 128));
            }
            ProcessSetup setup{kRealtime, kSample32, 256, 44100};
            CHECK(processor->setupProcessing(setup) == kResultOk);
            CHECK(component->setActive(true) == kResultOk && processor->setProcessing(true) == kResultOk);
            NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 2100, 1500)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            window.releasedWhenClosed = NO;
            TestPlugFrame frame;
            IPlugView* view = controller->createView(ViewType::kEditor);
            CHECK(view && view->setFrame(&frame) == kResultOk);
            CHECK(view->attached((__bridge void*)window.contentView, kPlatformTypeNSView) == kResultOk);
            CHECK(findNativeEditor(window.contentView));
            float left[256], right[256];
            float* channels[] = {left, right};
            AudioBusBuffers output{};
            output.numChannels = 2;
            output.channelBuffers32 = channels;
            TestEventList events;
            ProcessData data{};
            data.symbolicSampleSize = kSample32;
            data.numSamples = 256;
            data.numOutputs = 1;
            data.outputs = &output;
            data.inputEvents = &events;
            float peak = 0;
            for (int block = 0; block < 1600; ++block) {
                events.events.clear();
                if (block == 0 || (block % 32 == 0 && peak < 0.001f)) {
                    Event note{};
                    note.type = Event::kNoteOnEvent;
                    note.noteOn.pitch = 60;
                    note.noteOn.velocity = 0.8f;
                    CHECK(events.addEvent(note) == kResultOk);
                }
                CHECK(processor->process(data) == kResultOk);
                for (int i = 0; i < 256; ++i) {
                    CHECK(std::isfinite(left[i]) && std::isfinite(right[i]));
                    peak = std::max({peak, std::abs(left[i]), std::abs(right[i])});
                }
                [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
                if (block >= 63 && peak > 0.001f) break;
            }
            fprintf(stderr, "%s VST3 cycle %d: audio peak %.6f, full parameter exposure\n", name, cycle, peak);
            CHECK(peak > 0.001f);
            TestMemoryStream saved;
            CHECK(component->getState(&saved) == kResultOk);
            state.bytes = std::move(saved.bytes);
            CHECK(view->removed() == kResultOk && view->removed() == kResultOk);
            CHECK(view->attached((__bridge void*)window.contentView, kPlatformTypeNSView) == kResultOk);
            CHECK(findNativeEditor(window.contentView));
            CHECK(view->removed() == kResultOk);
            view->release();
            [window close];
            CHECK(processor->setProcessing(false) == kResultOk && component->setActive(false) == kResultOk);
            processor->release();
            controller->release();
            CHECK(component->terminate() == kResultOk);
            component->release();
        }
    }
    factory->release();
    CHECK(bundleExit());
    dlclose(bundle);
    fprintf(stderr, "Sampler VST3 catalog, audio, parameter ordering, editor reopening and fresh state reload passed\n");
}

static NSData* captureUtilityPixels(NSWindow* window, const std::filesystem::path& output = {}) {
    const auto path = output.empty() ? std::filesystem::temp_directory_path() /
        ("flbridge-utility-" + std::to_string(getpid()) + ".png") : output;
    NSTask* capture = [NSTask new];
    capture.executableURL = [NSURL fileURLWithPath:@"/usr/sbin/screencapture"];
    capture.arguments = @[@"-x", @"-o", @"-l", [NSString stringWithFormat:@"%ld", (long)window.windowNumber],
                         [NSString stringWithUTF8String:path.c_str()]];
    NSError* error = nil;
    CHECK([capture launchAndReturnError:&error]);
    [capture waitUntilExit];
    CHECK(capture.terminationStatus == 0);
    NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithData:
        [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()]]];
    CHECK(bitmap && bitmap.bitmapData && bitmap.pixelsHigh > 0);
    NSData* pixels = [NSData dataWithBytes:bitmap.bitmapData length:bitmap.bytesPerRow * bitmap.pixelsHigh];
    if (output.empty()) CHECK(std::filesystem::remove(path));
    return pixels;
}

static void checkUtilityPixelsChanged(NSData* before, NSData* after) {
    const auto* a = static_cast<const uint8_t*>(before.bytes);
    const auto* b = static_cast<const uint8_t*>(after.bytes);
    size_t differences = std::max(before.length, after.length) - std::min(before.length, after.length);
    for (size_t i = 0; i < std::min(before.length, after.length); ++i) if (a[i] != b[i]) ++differences;
    CHECK(differences > 64);
    fprintf(stderr, "Native utility image advanced: %zu pixel bytes changed\n", differences);
}

static void verifyTransportUtilities(const char* screenshotDirectory) {
    auto& scanner = flbr::FLPluginScanner::getInstance();
    CHECK(scanner.initialize());
    for (const char* name : {"Fruity Big Clock", "Fruity Dance"}) {
        flbr::FLPluginInstance instance;
        flbr::FLPluginInstanceTestAccess::watchTransport(instance);
        CHECK(instance.loadPluginByName(name));
        instance.setSampleRate(48000);
        instance.setBlockSize(256);
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 700, 500)
            styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        window.releasedWhenClosed = NO;
        int width = 0, height = 0;
        CHECK(instance.attachEditorToNSView((__bridge void*)window.contentView, width, height));
        [window setContentSize:NSMakeSize(width, height)];
        [window orderFront:nil];
        const auto showPosition = [&](double beats, double seconds, bool playing, int numerator, int denominator) {
            float left[256], right[256];
            instance.processAudio(nullptr, nullptr, left, right, 256, 120, beats, playing, seconds,
                                  numerator, denominator);
            for (int i = 0; i < 10; ++i) {
                instance.idleEditor();
                [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            }
            if (strcmp(name, "Fruity Big Clock") == 0) {
                CHECK(std::abs(flbr::FLPluginInstanceTestAccess::observedMilliseconds - seconds * 1000) < 1e-8);
                CHECK(std::abs(flbr::FLPluginInstanceTestAccess::observedTicks - beats * flbr::kNativePPQ) < 1e-8);
            } else {
                CHECK(std::abs(flbr::FLPluginInstanceTestAccess::observedBeats - beats) < 1e-8);
            }
        };
        showPosition(0, 0, false, 4, 4);
        showPosition(9.5, 6.25, true, 3, 4);
        if (strcmp(name, "Fruity Big Clock") == 0) {
            const auto& time = flbr::FLPluginInstanceTestAccess::observedSongTime;
            CHECK(time.Bar == 3 && time.Step == 2 && time.Tick == 0);
        }
        NSWindow* animatedWindow = window;
        if (strcmp(name, "Fruity Dance") == 0) {
            for (NSWindow* owned in NSApp.windows) {
                if (owned == window || !owned.visible) continue;
                animatedWindow = owned;
                break;
            }
            CHECK(animatedWindow != window);
        }
        const auto screenshot = [&](const char* stage) {
            return screenshotDirectory ? std::filesystem::path(screenshotDirectory) /
                (std::string(name) + "-" + stage + ".png") : std::filesystem::path{};
        };
        NSData* firstFrame = captureUtilityPixels(animatedWindow, screenshot("first"));
        for (int step = 0; step < 32; ++step) showPosition(9.5 + step * 0.125, 6.25 + step * 0.0625, true, 3, 4);
        checkUtilityPixelsChanged(firstFrame, captureUtilityPixels(animatedWindow, screenshot("second")));
        showPosition(2, 1, true, 7, 8);
        showPosition(2, 1, false, 7, 8);
        showPosition(2, 1, false, 7, 8);
        instance.detachEditor();
        CHECK(instance.attachEditorToNSView((__bridge void*)window.contentView, width, height));
        showPosition(0, 0, true, 4, 4);
        instance.detachEditor();
        instance.unloadPlugin();
        [window close];
        fprintf(stderr, "%s native playback-time consumption, seek, stop, meter and reopening passed\n", name);
    }
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc > 1 && strcmp(argv[1], "--catalog") == 0) {
            verifyDynamicFactory();
            return 0;
        }
        [NSApplication sharedApplication];
        if (argc > 1 && strcmp(argv[1], "--master-volume") == 0) {
            verifyMasterVolume(argc > 2 ? argv[2] : nullptr);
            return 0;
        }
        if (argc > 1 && strcmp(argv[1], "--saved-installation") == 0) {
            CHECK(argc == 4);
            isolateInstallationPreferences([NSString stringWithUTF8String:argv[2]]);
            auto& scanner = flbr::FLPluginScanner::getInstance();
            CHECK(scanner.initialize() && scanner.getFLAppPath() == argv[3]);
            fprintf(stderr, "Saved installation discovered in a fresh process\n");
            return 0;
        }
        if (argc > 1 && strcmp(argv[1], "--transport-utilities") == 0) {
            verifyTransportUtilities(argc > 2 ? argv[2] : nullptr);
            return 0;
        }
        if (argc > 1 && strcmp(argv[1], "--editor-resizing") == 0) {
            verifyVSTEditorResizing(argc > 2 ? argv[2] : nullptr);
            return 0;
        }
        if (argc > 1 && strcmp(argv[1], "--sample-instruments") == 0) {
            CHECK(argc == 3);
            verifySamplerInstruments(argv[2]);
            return 0;
        }
        if (argc > 1 && strcmp(argv[1], "--flex-scroll") == 0) {
            verifyFlexScrolling(argc > 2 ? argv[2] : nullptr);
            return 0;
        }
        if (argc > 1 && strcmp(argv[1], "--harmor-parts") == 0) {
            verifyHarmorPartAutomation(argc > 2 ? argv[2] : nullptr);
            return 0;
        }
        if (argc > 1 && strcmp(argv[1], "--sytrus-layout") == 0) {
            verifySytrusLayout(argc > 2 ? argv[2] : nullptr);
            return 0;
        }
        if (argc > 1 && strcmp(argv[1], "--vst-editor-reopening") == 0) {
            verifyVSTEditorReopening();
            return 0;
        }
        if (argc > 1 && strcmp(argv[1], "--editor-reopening") == 0) {
            verifyNativeEditorReopening();
            return 0;
        }
        if (argc > 2 && strcmp(argv[1], "--channel-state") == 0) {
            verifyChannelSampleState(argv[2]);
            return 0;
        }
        if (argc > 2 && strcmp(argv[1], "--host-services") == 0) {
            flbr::FLPluginInstance instance;
            flbr::FLPluginInstanceTestAccess::traceServices(instance);
            CHECK(instance.loadPluginByName(argv[2]));
            flbr::FLPluginInstanceTestAccess::describe(instance);
            instance.setBlockSize(256);
            NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 1100, 800)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            window.title = [NSString stringWithUTF8String:argv[2]];
            int width = 0, height = 0;
            CHECK(instance.attachEditorToNSView((__bridge void*)window.contentView, width, height));
            [window setContentSize:NSMakeSize(width, height)];
            [window orderFront:nil];
            NSView* nativeEditor = window.contentView.subviews.firstObject;
            int mouseChanges = 0;
            int lastMouseParameter = -1;
            const bool harmorMouse = strcmp(argv[2], "Harmor") == 0 && argc > 5 && strcmp(argv[5], "mouse") == 0;
            if (harmorMouse) {
                [window makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
                instance.onParamChangedFromPlug = [&](int parameter, double value) {
                    ++mouseChanges;
                    lastMouseParameter = parameter;
                    fprintf(stderr, "Harmor mouse parameter %d (%s) = %.4f\n",
                        parameter, instance.getParamName(parameter).c_str(), value);
                };
                const double before = instance.getParamNormalized(792);
                clickNativeEditor(&instance, window, nativeEditor, NSMakePoint(945, 349));
                clickNativeEditor(&instance, window, nativeEditor, NSMakePoint(482, 408), NSMakePoint(0, -25));
                CHECK(instance.getParamNormalized(792) > before + 0.01 && mouseChanges > 0);
                fprintf(stderr, "Harmor FX parameter changes before drop: %d\n", mouseChanges);
            }
            if (argc > 5 && strcmp(argv[5], "picker") == 0) {
                CHECK(strcmp(argv[2], "3x Osc") == 0 || strcmp(argv[2], "Fruity Scratcher") == 0);
                [window makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
                [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
                __block bool opened = false;
                NSTimer* cancel = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer* timer) {
                    if ([NSApp.modalWindow isKindOfClass:NSOpenPanel.class]) {
                        opened = true;
                        [(NSOpenPanel*)NSApp.modalWindow cancel:nil];
                        [timer invalidate];
                    }
                }];
                [NSRunLoop.currentRunLoop addTimer:cancel forMode:NSModalPanelRunLoopMode];
                [NSRunLoop.currentRunLoop addTimer:cancel forMode:NSRunLoopCommonModes];
                clickNativeEditor(&instance, window, nativeEditor,
                    strcmp(argv[2], "3x Osc") == 0 ? NSMakePoint(169, 30) : NSMakePoint(18, 216));
                [cancel invalidate];
                CHECK(opened);
                fprintf(stderr, "Native sample button opened the system picker; cancellation passed\n");
            }
            std::vector<uint8_t> before, after;
            CHECK(instance.savePluginState(before));
            if (argc > 3 && strcmp(argv[3], "-") != 0) {
                if (argc > 5 && (strcmp(argv[5], "drop") == 0 || harmorMouse)) {
                    FLBridgeTestFileDrag* drag = [FLBridgeTestFileDrag new];
                    drag.draggingPasteboard = [NSPasteboard pasteboardWithUniqueName];
                    CHECK([drag.draggingPasteboard writeObjects:
                        @[[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[3]]]]]);
                    id sender = drag;
                    NSView* destination = [nativeEditor hitTest:NSMakePoint(100, 100)] ?: nativeEditor;
                    while (destination != nativeEditor &&
                        ![destination.registeredDraggedTypes containsObject:NSPasteboardTypeFileURL])
                        destination = destination.superview;
                    CHECK([destination draggingEntered:sender] == NSDragOperationCopy);
                    CHECK([destination prepareForDragOperation:sender]);
                    CHECK([destination performDragOperation:sender]);
                    [drag.draggingPasteboard releaseGlobally];
                    fprintf(stderr, "Hosted native editor file-drop callback passed\n");
                } else if (strcmp(argv[2], "3x Osc") == 0) CHECK(instance.loadChannelSample(argv[3]));
                else CHECK(instance.loadDroppedFile(argv[3]));
            }
            CHECK(instance.savePluginState(after));
            fprintf(stderr, "File state sizes: before=%zu after=%zu\n", before.size(), after.size());
            if (harmorMouse) {
                mouseChanges = 0;
                const double before = instance.getParamNormalized(792);
                clickNativeEditor(&instance, window, nativeEditor, NSMakePoint(945, 349));
                clickNativeEditor(&instance, window, nativeEditor, NSMakePoint(482, 408), NSMakePoint(0, -25));
                CHECK(instance.getParamNormalized(792) > before + 0.01 && mouseChanges > 0);
                fprintf(stderr, "Harmor FX parameter changes after drop: %d\n", mouseChanges);
                const double imageTime = instance.getParamNormalized(83);
                mouseChanges = 0;
                lastMouseParameter = -1;
                clickNativeEditor(&instance, window, nativeEditor, NSMakePoint(899, 349));
                clickNativeEditor(&instance, window, nativeEditor, NSMakePoint(845, 435), NSMakePoint(0, -25));
                CHECK(mouseChanges > 0 && lastMouseParameter == 83);
                CHECK(instance.getParamNormalized(83) > imageTime + 0.001);
                fprintf(stderr, "Harmor IMG mouse input after drop passed\n");
                instance.onParamChangedFromPlug = nullptr;
            }
            if ((strcmp(argv[2], "Harmor") == 0 || strcmp(argv[2], "Newtone") == 0) &&
                argc > 5 && strcmp(argv[5], "drop") == 0)
                CHECK(after.size() > before.size());
            const bool scratcher = strcmp(argv[2], "Fruity Scratcher") == 0 && argc > 3 && strcmp(argv[3], "-") != 0;
            const bool newtone = strcmp(argv[2], "Newtone") == 0 && argc > 3 && strcmp(argv[3], "-") != 0;
            if (scratcher) clickNativeEditor(&instance, window, nativeEditor, NSMakePoint(176, 184));
            if (newtone) {
                [window makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
                clickNativeEditor(&instance, window, nativeEditor, NSMakePoint(24, 30));
            }
            instance.noteOn(0, 60, 0.8f);
            float input[256], left[256], right[256];
            float peak = 0;
            for (int block = 0; block < 300; ++block) {
                for (int i = 0; i < 256; ++i)
                    input[i] = (scratcher || newtone) ? 0 : (0.1f + 0.4f * (block % 100) / 100) *
                        std::sin((block * 256 + i) * 440.0 * 2 * M_PI / 44100);
                instance.processAudio(input, input, left, right, 256, 120, 0, false);
                for (int i = 0; i < 256; ++i) {
                    CHECK(std::isfinite(left[i]) && std::isfinite(right[i]));
                    peak = std::max({peak, std::abs(left[i]), std::abs(right[i])});
                }
                instance.idleEditor();
                [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.006]];
            }
            fprintf(stderr, "Services audio peak: %.6f\n", peak);
            if (scratcher || newtone) CHECK(peak > 0.01f);
            flbr::FLPluginInstanceTestAccess::reportServices(instance);
            if (argc > 4 && strcmp(argv[4], "-") != 0) {
                NSTask* capture = [NSTask new];
                capture.executableURL = [NSURL fileURLWithPath:@"/usr/sbin/screencapture"];
                capture.arguments = @[@"-x", @"-o", @"-l", [NSString stringWithFormat:@"%ld", (long)window.windowNumber],
                                      [NSString stringWithUTF8String:argv[4]]];
                NSError* error = nil;
                CHECK([capture launchAndReturnError:&error]);
                [capture waitUntilExit];
                CHECK(capture.terminationStatus == 0);
            }
            if (scratcher || newtone) CHECK(instance.savePluginState(after));
            instance.detachEditor();
            CHECK(![NSStringFromClass(object_getClass(nativeEditor)) hasPrefix:@"FLBridgeFileDrop_"]);
            [window orderOut:nil];
            instance.unloadPlugin();
            if (scratcher) {
                flbr::FLPluginInstance restored;
                flbr::FLPluginInstanceTestAccess::traceServices(restored);
                CHECK(restored.loadPluginByName(argv[2]));
                const int calls = flbr::FLPluginInstanceTestAccess::sampleCalls;
                CHECK(restored.restorePluginState(after.data(), after.size()));
                CHECK(flbr::FLPluginInstanceTestAccess::sampleCalls > calls);
                restored.unloadPlugin();
                fprintf(stderr, "Scratcher fresh-instance sample-state reload passed\n");
            }
            if (newtone) {
                flbr::FLPluginInstance restored;
                CHECK(restored.loadPluginByName(argv[2]));
                restored.setBlockSize(256);
                CHECK(restored.restorePluginState(after.data(), after.size()));
                CHECK(restored.attachEditorToNSView((__bridge void*)window.contentView, width, height));
                [window setContentSize:NSMakeSize(width, height)];
                [window makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
                NSView* restoredEditor = window.contentView.subviews.firstObject;
                CHECK(restoredEditor != nil);
                clickNativeEditor(&restored, window, restoredEditor, NSMakePoint(24, 30));
                float restoredPeak = 0;
                memset(input, 0, sizeof(input));
                for (int block = 0; block < 64; ++block) {
                    restored.processAudio(input, input, left, right, 256, 120, 0, false);
                    for (int i = 0; i < 256; ++i) {
                        CHECK(std::isfinite(left[i]) && std::isfinite(right[i]));
                        restoredPeak = std::max({restoredPeak, std::abs(left[i]), std::abs(right[i])});
                    }
                    restored.idleEditor();
                }
                CHECK(restoredPeak > 0.01f);
                restored.detachEditor();
                [window orderOut:nil];
                restored.unloadPlugin();
                fprintf(stderr, "Newtone fresh-instance sample-state reload and playback passed (peak %.6f)\n",
                        restoredPeak);
            }
            fprintf(stderr, "Services completed\n");
            return 0;
        }
        if (argc > 1 && (strcmp(argv[1], "--probe-plugin") == 0 ||
                         strcmp(argv[1], "--probe-plugin-headless") == 0 ||
                         strcmp(argv[1], "--catalog-plugin") == 0)) {
            CHECK(argc >= 4 && (strcmp(argv[2], "Generator") == 0 || strcmp(argv[2], "Effect") == 0));
            auto& scanner = flbr::FLPluginScanner::getInstance();
            CHECK(scanner.initialize());
            const bool headless = strcmp(argv[1], "--probe-plugin-headless") == 0;
            const bool generator = strcmp(argv[2], "Generator") == 0;
            const int index = strcmp(argv[1], "--catalog-plugin") != 0 ?
                flbr::FLPluginScannerTestAccess::includeForProbe(argv[3], generator) :
                scanner.findPluginIndexByName(argv[3], generator);
            CHECK(index >= 0);
            flbr::FLPluginInstance instance;
            fprintf(stderr, "Probe constructing %s %s\n", argv[2], argv[3]);
            if (std::getenv("FLBR_TRACE_HOST")) flbr::FLPluginInstanceTestAccess::traceServices(instance);
            CHECK(instance.loadPlugin(index));
            flbr::FLPluginInstanceTestAccess::describe(instance);
            instance.setBlockSize(256);
            const auto renderProbe = [&](const char* stage) {
                const bool asynchronous = strcmp(argv[3], "FL Keys") == 0 &&
                    strcmp(stage, "before editor") != 0;
                instance.noteOn(0, strcmp(argv[3], "FPC") == 0 ? 36 : 60, 0.8f);
                float left[256], right[256], input[256];
                float peak = 0;
                for (int block = 0; block < (asynchronous ? 1600 : 64); ++block) {
                    // Wavetable selection completes in the native background loader.
                    if (asynchronous && block > 0 && block % 32 == 0 && peak < 0.001f)
                        instance.noteOn(0, 60, 0.8f);
                    for (int frame = 0; frame < 256; ++frame)
                        input[frame] = 0.1f * std::sin((block * 256 + frame) * 440.0 * 2 * M_PI / 44100);
                    instance.processAudio(input, input, left, right, 256, 120,
                                          block * 256.0 / 44100 * 2, true);
                    for (int frame = 0; frame < 256; ++frame) {
                        CHECK(std::isfinite(left[frame]) && std::isfinite(right[frame]));
                        peak = std::max({peak, std::abs(left[frame]), std::abs(right[frame])});
                    }
                    instance.idleEditor();
                    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
                    if (block >= 63 && peak > 0.001f) break;
                }
                instance.allNotesOff();
                fprintf(stderr, "Probe %s rendered peak %.6f\n", stage, peak);
                return peak;
            };
            if (std::getenv("FLBR_TRACE_HOST")) renderProbe("before editor");
            NSWindow* window = nil;
            int width = 0, height = 0;
            if (!headless) {
                window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1200, 900)
                    styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
                window.releasedWhenClosed = NO;
                CHECK(instance.attachEditorToNSView((__bridge void*)window.contentView, width, height));
                fprintf(stderr, "Probe editor attached %d x %d\n", width, height);
                for (int i = 0; i < 10; ++i) {
                    instance.idleEditor();
                    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
                }
            } else {
                fprintf(stderr, "Probe editor skipped; editor compatibility is not verified\n");
            }
            if (argc > 4 && strcmp(argv[4], "-") != 0)
                flbr::FLPluginInstanceTestAccess::loadFile(instance, argv[4], argc > 6 ? atoi(argv[6]) : 0);
            const bool sampler = !headless &&
                (strcmp(argv[3], "DirectWave") == 0 || strcmp(argv[3], "FL Keys") == 0);
            if (sampler || std::getenv("FLBR_TRACE_HOST")) {
                const float initialPeak = renderProbe("before restore");
                if (argc > 5) CHECK(initialPeak > strtod(argv[5], nullptr));
            }
            std::vector<uint8_t> state;
            CHECK(instance.savePluginState(state));
            if (!state.empty()) CHECK(instance.restorePluginState(state.data(), state.size()));
            fprintf(stderr, "Probe state round trip %zu bytes\n", state.size());
            const float peak = renderProbe("after restore");
            if (argc > 5) CHECK(peak > strtod(argv[5], nullptr));
            instance.detachEditor();
            if (sampler && argc > 5) {
                CHECK(instance.savePluginState(state));
                CHECK(instance.attachEditorToNSView((__bridge void*)window.contentView, width, height));
                CHECK(renderProbe("reopened editor") > strtod(argv[5], nullptr));
                instance.detachEditor();
                instance.unloadPlugin();
                CHECK(instance.loadPlugin(index));
                instance.setBlockSize(256);
                CHECK(instance.restorePluginState(state.data(), state.size()));
                CHECK(instance.attachEditorToNSView((__bridge void*)window.contentView, width, height));
                CHECK(renderProbe("fresh instance") > strtod(argv[5], nullptr));
                instance.detachEditor();
            }
            [window close];
            instance.unloadPlugin();
            fprintf(stderr, "Probe destruction completed\n");
            return 0;
        }
        if (argc > 1 && strcmp(argv[1], "--realtime-knobs") == 0) {
            verifyRealtimeKnobs();
            return 0;
        }
        if (argc > 1 && strcmp(argv[1], "--plugin-data") == 0) {
            flbr::FLPluginInstance instance;
            CHECK(instance.loadPluginByName(argc > 2 ? argv[2] : "FLEX"));
            instance.setBlockSize(64);
            const bool flex = instance.getPluginName() == "FLEX";
            auto renderSound = [&](const char* stage) {
                instance.noteOn(0, 60, 0.8f);
                float left[64], right[64];
                float peak = 0;
                for (int i = 0; i < 64; ++i) {
                    instance.processAudio(nullptr, nullptr, left, right, 64, 120, 0, false);
                    for (int frame = 0; frame < 64; ++frame) {
                        CHECK(std::isfinite(left[frame]) && std::isfinite(right[frame]));
                        peak = std::max({peak, std::abs(left[frame]), std::abs(right[frame])});
                    }
                }
                instance.allNotesOff();
                printf("%s: %s peak %.6f\n", instance.getPluginName().c_str(), stage, peak);
                CHECK(peak > 0.001f);
            };
            if (flex) {
                CHECK(instance.getNumInternalPresets() > 1);
                CHECK(!instance.getInternalPresetName(0).empty());
                printf("FLEX: %d native presets in initial pack; first preset %s\n",
                       instance.getNumInternalPresets(), instance.getInternalPresetName(0).c_str());
                renderSound("default before opening editor");
            }
            NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1000, 800)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            int width = 0, height = 0;
            CHECK(instance.attachEditorToNSView((__bridge void*)window.contentView, width, height));
            for (int i = 0; i < 10; ++i) instance.idleEditor();
            const auto presets = flbr::FLPluginScanner::getInstance().getPlugins()[instance.getCurrentPluginIndex()].presets;
            printf("%s: %zu scanned presets\n", instance.getPluginName().c_str(), presets.size());
            if (!presets.empty()) CHECK(instance.loadPreset(0));
            if (flex) {
                CHECK(instance.getInternalPresetName(1) != instance.getInternalPresetName(0));
                CHECK(instance.loadInternalPreset(1));
                renderSound("second internal preset");
                instance.idleEditor();
                instance.setParamNormalized(0, 0.25, true);
            }
            std::vector<double> parameters;
            if (flex) {
                for (int i = 0; i < instance.getNumParams(); ++i)
                    parameters.push_back(instance.getParamNormalized(i));
            }
            std::vector<uint8_t> state;
            CHECK(instance.savePluginState(state) && !state.empty());
            if (flex) CHECK(instance.loadInternalPreset(0));
            CHECK(instance.restorePluginState(state.data(), state.size()));
            if (flex) {
                instance.detachEditor();
                CHECK(instance.attachEditorToNSView((__bridge void*)window.contentView, width, height));
                for (int i = 0; i < 10; ++i) instance.idleEditor();
                for (int i = 0; i < instance.getNumParams(); ++i) {
                    const double restored = instance.getParamNormalized(i);
                    if (std::abs(restored - parameters[i]) >= 0.011)
                        fprintf(stderr, "FLEX parameter %d (%s): saved %.6f restored %.6f\n",
                                i, instance.getParamName(i).c_str(), parameters[i], restored);
                    CHECK(std::abs(restored - parameters[i]) < 0.011);
                }
            }
            renderSound("after state restore");
            printf("%s: state size %zu\n", instance.getPluginName().c_str(), state.size());
            instance.detachEditor();
            instance.unloadPlugin();
            CHECK(instance.getNumInternalPresets() == 0);
            fprintf(stderr, "Plugin data completed\n");
            return 0;
        }
        if (argc > 1 && strcmp(argv[1], "--editor-coordinates") == 0) {
            verifyEditorCoordinates();
            return 0;
        }
        verifyDynamicFactory();
        verifyNativePitchAndPreview();
        verifyAffectedInstruments();
        verifyEditorCoordinates();

        verifyMasterVolume(nullptr);
        verifyHarmorPartAutomation(nullptr);
        verifySytrusLayout(nullptr);
        printf("Fixed native classes, audio, automation and state checks passed\n");
        return 0;
    }
}
