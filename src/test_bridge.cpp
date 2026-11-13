#include "FLPluginInstance.h"
#include "NativePluginCatalog.h"
#include "FLPathManager.h"
#include "BridgeEditorLayout.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "CHECK FAILED: %s (line %d)\n", #expr, __LINE__); \
    std::exit(1); \
} } while (0)

namespace flbr {
namespace fs = std::filesystem;

struct FLPluginScannerTestAccess {
    static void testDiscovery(const fs::path& directory) {
        const auto root = directory / "lightweight-discovery";
        fs::create_directories(root / "Shared");
        fs::create_directories(root / "Plugins/Fruity/Generators/Display Name");
        fs::create_directories(root / "Plugins/Fruity/Effects");
#ifdef _WIN32
        const std::string extension = ".dll";
#else
        const std::string extension = ".dylib";
#endif
        const auto first = root / "Plugins/Fruity/Generators/Display Name" / ("PersistentModule_x64" + extension);
        std::ofstream(first, std::ios::binary).put('\0');
        FLPluginScanner isolated;
        isolated.m_flRootPath = (root / "").u8string();
        CHECK(isolated.discover());
        CHECK(!isolated.isInitialized() && isolated.getUserDataPath().empty());
        const auto initial = buildNativePluginClasses(isolated.getDiscoverySnapshot());
        CHECK(initial.size() == 1 && initial[0].identifier == "fl.native.persistentmodule");
        CHECK(isolated.m_plugins[0].presets.empty() && !isolated.m_plugins[0].presetsScanned);
        std::ofstream(first, std::ios::binary).put('\1');
        FLPluginScanner fresh;
        fresh.m_flRootPath = isolated.m_flRootPath;
        CHECK(fresh.discover());
        const auto updated = buildNativePluginClasses(fresh.getDiscoverySnapshot());
        CHECK(updated.size() == 1 && updated[0].cid == initial[0].cid);
        fs::create_directories(root / "Plugins/Fruity/Effects/A New Effect");
        const auto second = root / "Plugins/Fruity/Effects/A New Effect" / ("NewEffect_x64" + extension);
        std::ofstream(second, std::ios::binary).put('\0');
        CHECK(isolated.discover(true));
        const auto refreshed = buildNativePluginClasses(isolated.getDiscoverySnapshot());
        CHECK(refreshed.size() == 2 && !refreshed[0].isGenerator);
        CHECK(refreshed[1].cid == initial[0].cid);
        CHECK(fs::remove(first));
        CHECK(isolated.discover(true));
        CHECK(buildNativePluginClasses(isolated.getDiscoverySnapshot()).size() == 1);
        CHECK(isolated.findPluginIndexByIdentifier(initial[0].identifier) == -1);
        CHECK(!isolated.isInitialized());
    }

    static std::vector<std::string> findInstallations(const std::vector<std::string>& directories) {
        return FLPluginScanner::findInstallations(directories);
    }
    static void configure(const fs::path& directory) {
        auto& scanner = FLPluginScanner::getInstance();
        scanner.m_initialized = true;
        scanner.m_discovered = true;
        scanner.m_flRootPath = (directory / "factory").u8string() + static_cast<char>(fs::path::preferred_separator);
        scanner.m_flSharedPath = scanner.m_flRootPath + "Shared" + static_cast<char>(fs::path::preferred_separator);
        scanner.m_userDataPath = (directory / "Image-Line" / "FL Studio").u8string() +
            static_cast<char>(fs::path::preferred_separator);
        CHECK(scanner.prepareUserData());
    }

    static void scan(FLPluginEntry& entry) {
        FLPluginScanner::getInstance().scanPresetsForPlugin(entry);
    }

    static void setPlugins(std::vector<FLPluginEntry> plugins) {
        FLPluginScanner::getInstance().m_plugins = std::move(plugins);
    }
};

struct FakePlugin {
    int32_t values[7]{};
    int writes = 0;
    int32_t lastInput = 0;
    int32_t lastFlags = 0;
    int triggers = 0;
    int releases = 0;
    int kills = 0;
    int maxPoly = 0;
    int32_t tempoBits = 0;
    int32_t tempoBPM = 0;
    int32_t channelVolume = 0;
    int internalPreset = -1;
    int internalPresetLoads = 0;
    intptr_t nextHandle = 0;
    intptr_t lastReleased = FVH_Null;
    intptr_t lastTag = 0;
    TVoiceParams* voice = nullptr;
    TVoiceParamsOld* legacyVoice = nullptr;
    int32_t midiResolution = FromMIDI_Max;
    bool consumeMIDI = false;
    bool forwardMIDI = false;
    FLPluginInstance* instance = nullptr;
    std::string loadedFile;
    intptr_t fileLoadResult = 1;
    bool enabled = false;
    int enabledNotifications = 0;
    intptr_t sampleHandle = 0;
    float* customShape = nullptr;
    std::string parameterName;
    std::string parameterDisplay;
    int relocations = 0;
    int playingNotifications = 0;
    bool playing = false;
    TTimeSigInfo timeSignature{};
    bool editorLimitsProvided = false;
    float effectGain = 1;
    bool mutateEffectInput = false;
    FLRect editorLimits{};
    FLPoint editorSnap{1, 1};
};

static int32_t floatBits(float value) {
    int32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static int32_t processParam(void* self, int32_t index, int32_t value, int32_t flags) {
    auto& p = *static_cast<FakePlugin*>(self);
    p.lastInput = value;
    p.lastFlags = flags;
    if (flags & REC_FromMIDI) {
        const double norm = static_cast<double>(value) / p.midiResolution;
        switch (index) {
            case 0: value = static_cast<int32_t>(std::lround(norm * 160)); break;
            case 1: value = static_cast<int32_t>(std::lround(norm * 48)) - 24; break;
            case 2: value = floatBits(static_cast<float>(norm)); break;
            case 3: value = static_cast<int32_t>(std::lround(norm * norm * 10000)); break;
            case 4: value = 100 - static_cast<int32_t>(std::lround(norm * 100)); break;
            case 5: value = 7; break;
            case 6: value = norm >= 0.5 ? 1 : 0; break;
        }
    }
    if (flags & REC_UpdateValue) {
        p.values[index] = value;
        ++p.writes;
    }
    if (flags & REC_GetValue) value = p.values[index];
    return value;
}

static intptr_t dispatch(void* self, intptr_t id, intptr_t index, intptr_t value) {
    auto& p = *static_cast<FakePlugin*>(self);
    if (id == FPD_WindowMinMax && p.editorLimitsProvided) {
        *reinterpret_cast<FLRect*>(index) = p.editorLimits;
        *reinterpret_cast<FLPoint*>(value) = p.editorSnap;
        return 0;
    }
    if (id == FPD_SongPosChanged) ++p.relocations;
    if (id == FPD_SetPlaying) {
        p.playing = value != 0;
        ++p.playingNotifications;
    }
    if (id == FPD_SetTimeSig) p.timeSignature = *reinterpret_cast<const TTimeSigInfo*>(value);
    if (id == FPD_SetEnabled) {
        CHECK(index == 0);
        p.enabled = value != 0;
        ++p.enabledNotifications;
    }
    if (id == FPD_ChanSampleChanged) {
        p.sampleHandle = index;
        p.customShape = reinterpret_cast<float*>(value);
    }
    if (id == FPD_SetPreset) {
        auto& fake = *static_cast<FakePlugin*>(self);
        CHECK(value == 0);
        fake.internalPreset = static_cast<int>(index);
        ++fake.internalPresetLoads;
        fake.channelVolume = 0;
    }
    if (id == FPD_LoadFile) {
        auto& fake = *static_cast<FakePlugin*>(self);
        CHECK(index == LFT_Generic || index == LFT_DroppedFile);
        fake.loadedFile = reinterpret_cast<const char*>(value);
        return fake.fileLoadResult;
    }
    return id == 26 && index == 2 ? 2 : 0;
}

static int32_t processEvent(void* self, int32_t id, int32_t value, int32_t flags) {
    auto& fake = *static_cast<FakePlugin*>(self);
    if (id == FPE_Tempo) {
        fake.tempoBits = value;
        fake.tempoBPM = flags;
    } else if (id == FPE_MaxPoly) {
        CHECK(flags == 0);
        fake.maxPoly = value;
    } else {
        CHECK(id == FPE_MIDI_Vol && value == 127);
        fake.channelVolume = flags;
    }
    return 0;
}

static void getName(void* self, int32_t section, int32_t index, int32_t, char* text) {
    if (section == FPN_Param || section == FPN_ParamValue) {
        const auto& fake = *static_cast<const FakePlugin*>(self);
        const auto& value = section == FPN_Param ? fake.parameterName : fake.parameterDisplay;
        snprintf(text, 256, "%s", value.c_str());
        return;
    }
    CHECK(section == FPN_Preset);
    snprintf(text, 256, "Internal %d", index);
}

static intptr_t trigger(void* self, void* params, intptr_t tag) {
    auto& p = *static_cast<FakePlugin*>(self);
    ++p.triggers;
    p.voice = static_cast<TVoiceParams*>(params);
    p.legacyVoice = static_cast<TVoiceParamsOld*>(params);
    p.lastTag = tag;
    return p.nextHandle;
}

static void releaseVoice(void* self, intptr_t handle) {
    auto& p = *static_cast<FakePlugin*>(self);
    ++p.releases;
    p.lastReleased = handle;
}

static void killVoice(void* self, intptr_t) {
    ++static_cast<FakePlugin*>(self)->kills;
}

static void midiIn(void* self, int32_t* message) {
    auto& p = *static_cast<FakePlugin*>(self);
    if (p.forwardMIDI) {
        int status = *message & 0xF0;
        intptr_t index = ((*message & 0x0F) << 16) | ((*message >> 8) & 0x7F);
        p.instance->handleHostDispatcher(1, status == 0x90 ? 20 : 21, index, (*message >> 16) & 0x7F);
    }
    if (p.consumeMIDI) *message = -1;
}

static void renderGenerator(void*, float* destination, int32_t* frames) {
    for (int i = 0; i < *frames; ++i) {
        destination[2 * i] = 0.25f;
        destination[2 * i + 1] = -0.75f;
    }
}

static void renderEffect(void* self, float* source, float* destination, int32_t frames) {
    const auto& fake = *static_cast<const FakePlugin*>(self);
    for (int i = 0; i < 2 * frames; ++i) destination[i] = source[i] * fake.effectGain;
    if (fake.mutateEffectInput) std::fill_n(source, 2 * frames, 137);
}

struct FLPluginInstanceTestAccess {
    static void attach(FLPluginInstance& instance, FakePlugin& fake) {
        instance.m_plug = &fake;
        instance.m_numParams = 7;
        instance.m_fnDispatcher = dispatch;
        instance.m_fnGetName = getName;
        instance.m_fnProcessParam = processParam;
        instance.m_fnProcessEvent = processEvent;
        instance.m_fnTriggerVoice = trigger;
        instance.m_fnVoiceRelease = releaseVoice;
        instance.m_fnVoiceKill = killVoice;
        instance.m_fnMIDIIn = midiIn;
        fake.instance = &instance;
    }

    static bool active(const FLPluginInstance& instance, intptr_t tag) {
        return instance.m_voices[tag - 1].active;
    }

    static void testEditorSizing(FLPluginInstance& instance, FakePlugin& fake) {
        static_assert(sizeof(FLRect) == 16 && sizeof(FLPoint) == 8);
        CHECK(FPD_WindowMinMax == 5);
        instance.m_editorWidth = 320;
        instance.m_editorHeight = 200;
        auto limits = instance.getEditorSizeConstraints();
        CHECK(!limits.isResizable() && limits.minWidth == 320 && limits.minHeight == 200);
        fake.editorLimitsProvided = true;
        fake.editorLimits = {320, 200, 1280, 720};
        fake.editorSnap = {16, 8};
        limits = instance.getEditorSizeConstraints();
        CHECK(limits.isResizable() && limits.maxWidth == 1280 && limits.maxHeight == 720);
        int width = 321, height = 205;
        limits.constrain(width, height);
        CHECK(width == 320 && height == 208);
        width = std::numeric_limits<int>::min();
        height = std::numeric_limits<int>::max();
        limits.constrain(width, height);
        CHECK(width == 320 && height == 720);
        instance.m_editorCoordinateScale = 2;
        fake.editorLimits = {640, 400, 2560, 1440};
        fake.editorSnap = {32, 16};
        limits = instance.getEditorSizeConstraints();
        CHECK(limits.minWidth == 320 && limits.maxWidth == 1280 && limits.widthStep == 16);
        CHECK(limits.minHeight == 200 && limits.maxHeight == 720 && limits.heightStep == 8);
        fake.editorLimits = {960, 366, 960, 540};
        fake.editorSnap = {1, 174};
        limits = instance.getEditorSizeConstraints();
        CHECK(limits.isResizable() && limits.minWidth == 480 && limits.maxWidth == 480);
        CHECK(limits.minHeight == 183 && limits.maxHeight == 270 && limits.heightStep == 87);
        width = 900;
        height = 230;
        limits.constrain(width, height);
        CHECK(width == 480 && height == 270);
        fake.editorLimits = {639, 400, 960, 1440};
        fake.editorSnap = {3, 1};
        limits = instance.getEditorSizeConstraints();
        CHECK(limits.minWidth == 321 && limits.widthStep == 3 && limits.maxWidth == 480);
        width = 322;
        height = 200;
        limits.constrain(width, height);
        CHECK(width == 321);
        fake.editorLimits = {960, 540, 960, 540};
        limits = instance.getEditorSizeConstraints();
        CHECK(!limits.isResizable() && limits.minWidth == 480 && limits.minHeight == 270);
        fake.editorLimits = {640, 400, 0, 0};
        fake.editorSnap = {1, 1};
        limits = instance.getEditorSizeConstraints();
        CHECK(limits.isResizable() && limits.maxWidth == std::numeric_limits<int32_t>::max() - 1024);
        width = height = std::numeric_limits<int>::max();
        limits.constrain(width, height);
        CHECK(width == limits.maxWidth && height == limits.maxHeight);
        for (const FLRect invalid : {FLRect{-1, 400, 960, 540}, FLRect{640, 400, 100, 100}}) {
            fake.editorLimits = invalid;
            limits = instance.getEditorSizeConstraints();
            CHECK(!limits.isResizable() && limits.minWidth == 320 && limits.minHeight == 200);
        }
        fake.editorLimitsProvided = false;
        instance.m_editorCoordinateScale = 1;
        instance.m_editorWidth = 900;
        instance.m_editorHeight = 600;
    }
    static void testChannelState(FLPluginInstance& instance, FakePlugin& fake) {
        instance.initializeChannelState();
        CHECK(fake.maxPoly == FLPluginInstance::kMaxVoices);
        CHECK(fake.maxPoly > 0);
        CHECK(fake.channelVolume == floatBits(1.0f));
    }

    static void testSampleServices(FLPluginInstance& instance, FakePlugin& fake, const fs::path& directory) {
        static_assert(sizeof(FLWaveFormat) == 18);
        static_assert(sizeof(FLSampleInfo) == 108);
        static_assert(offsetof(FLSampleInfo, data) == 4);
        static_assert(offsetof(FLSampleInfo, length) == 12);
        static_assert(offsetof(FLSampleInfo, channels) == 48);
        std::vector<uint8_t> wave(44 + 32, 0);
        const auto put = [&](size_t offset, uint32_t value, int bytes) {
            for (int i = 0; i < bytes; ++i) wave[offset + i] = (value >> (8 * i)) & 255;
        };
        memcpy(wave.data(), "RIFF", 4);
        put(4, wave.size() - 8, 4);
        memcpy(wave.data() + 8, "WAVEfmt ", 8);
        put(16, 16, 4);
        put(20, 1, 2);
        put(22, 2, 2);
        put(24, 22050, 4);
        put(28, 88200, 4);
        put(32, 4, 2);
        put(34, 16, 2);
        memcpy(wave.data() + 36, "data", 4);
        put(40, 32, 4);
        for (size_t i = 0; i < 8; ++i) {
            put(44 + i * 4, 16384, 2);
            put(46 + i * 4, static_cast<uint16_t>(-8192), 2);
        }
        const auto path = (directory / "sample.wav").u8string();
        {
            std::ofstream file(fs::u8path(path), std::ios::binary);
            file.write(reinterpret_cast<const char*>(wave.data()), wave.size());
            CHECK(file.good());
        }
        using Load = bool(*)(void*, intptr_t&, char*, const FLWaveFormat*, int32_t);
        using Data = void*(*)(void*, intptr_t, int32_t&);
        using Close = void(*)(void*, intptr_t);
        using Info = void(*)(void*, intptr_t, FLSampleInfo*);
        for (auto* host : {&instance.m_cppHost, &instance.m_pascalHost}) {
            void** table = static_cast<void**>(host->vmtPtr) + (host == &instance.m_pascalHost ? 25 : 0);
            auto load = reinterpret_cast<Load>(table[13]);
            auto data = reinterpret_cast<Data>(table[14]);
            auto close = reinterpret_cast<Close>(table[15]);
            auto info = reinterpret_cast<Info>(table[23]);
            char file[256]{};
            CHECK(path.size() < sizeof(file));
            strcpy(file, path.c_str());
            intptr_t handle = 0;
            FLWaveFormat format{1, 2, 44100, 0, 4, 16, 0};
            CHECK(load(host, handle, file, &format, FHLS_NoResampling));
            CHECK(handle != 0 && path == file);
            int32_t frames = 0;
            auto* samples = static_cast<int16_t*>(data(host, handle, frames));
            CHECK(samples && frames == 8 && samples[0] == 16384 && samples[1] == -8192);
            FLSampleInfo sample{};
            sample.size = sizeof(sample);
            sample.channels = 1;
            sample.format = 1;
            info(host, handle, &sample);
            CHECK(sample.length == 8 && sample.solidLength == 8 && sample.loopStart == -1);
            CHECK(sample.channels == 1 && sample.format == 1);
            CHECK(sample.sampleRateConversion == 0.5);
            CHECK(std::abs(static_cast<float*>(sample.data)[0] - 0.125f) < 1e-6);
            file[0] = 0;
            CHECK(load(host, handle, file, &format, FHLS_GetName) && path == file);
            CHECK(data(host, handle, frames) == samples);
            format.tag = 2;
            CHECK(!load(host, handle, file, &format, 0));
            format.tag = 1;
            strcpy(file, (directory / "missing.wav").u8string().c_str());
            CHECK(!load(host, handle, file, &format, FHLS_ForceReload));
            CHECK(data(host, handle, frames) == samples && frames == 8);
            strcpy(file, path.c_str());
            format = FLWaveFormat{3, 1, 22050, 0, 4, 32, 0};
            CHECK(load(host, handle, file, &format, FHLS_ForceReload));
            CHECK(std::abs(static_cast<float*>(data(host, handle, frames))[0] - 0.125f) < 1e-6);
            CHECK(frames == 8);
            format.sampleRate = 44100;
            CHECK(load(host, handle, file, &format, 0));
            CHECK(data(host, handle, frames) && frames >= 14 && frames <= 18);
            close(host, handle);
            CHECK(!data(host, handle, frames) && frames == 0);
            CHECK(instance.m_samples.empty());
        }
        instance.setSampleRate(22050);
        instance.m_pluginFlags = FPF_GetChanCustomShape;
        CHECK(instance.loadChannelSample(path));
        CHECK(fake.sampleHandle != 0 && fake.customShape == instance.m_customShape.data());
        CHECK(instance.getChannelSamplePath() == path);
        CHECK(std::all_of(instance.m_customShape.begin(), instance.m_customShape.end(),
            [](float value) { return std::abs(value - 0.125f) < 1e-6; }));
        const intptr_t old = fake.sampleHandle;
        CHECK(!instance.loadChannelSample((directory / "missing.wav").u8string()));
        CHECK(fake.sampleHandle == old && instance.getChannelSamplePath() == path);
        CHECK(instance.loadChannelSample(path));
        CHECK(fake.sampleHandle != old && instance.m_samples.size() == 1);
        instance.clearChannelSample();
        CHECK(instance.m_samples.empty() && instance.getChannelSamplePath().empty());
        CHECK(fake.sampleHandle == 0 && fake.customShape[0] == 0);
        instance.m_pluginFlags = FPF_GetChanSample;
        CHECK(instance.loadChannelSample(path) && fake.sampleHandle && !fake.customShape);
        instance.clearChannelSample();
        instance.m_pluginFlags = FPF_NewVoiceParams;
        CHECK(!instance.loadChannelSample(path));
        fake.fileLoadResult = 2;
        CHECK(!instance.loadDroppedFile(path));
        CHECK(!instance.loadDroppedFile((directory / "missing.wav").u8string()));
        fake.fileLoadResult = 1;
        CHECK(instance.loadDroppedFile(path));
        fake.enabledNotifications = 0;
        instance.setEnabled(false);
        CHECK(!fake.enabled && !instance.m_enabled);
        instance.setEnabled(true);
        CHECK(fake.enabled && instance.m_enabled && fake.enabledNotifications == 2);
        instance.setSampleRate(44100);
    }

    static void testPaths(FLPluginInstance& instance) {
        const auto& root = FLPluginScanner::getInstance().getFLRootPath();
#ifdef _WIN32
        const std::string shared = root.empty() ? "" : root + "Shared\\";
#else
        const std::string shared = root.empty() ? "" : root + "Shared/";
#endif
        instance.m_activePresetDir = "bridge-preset-directory";
        using Dispatch = intptr_t(*)(void*, intptr_t, intptr_t, intptr_t, intptr_t);
        for (auto* host : {&instance.m_cppHost, &instance.m_pascalHost}) {
            void** vtable = static_cast<void**>(host->vmtPtr);
            if (host == &instance.m_pascalHost) vtable += 25;
            auto dispatch = reinterpret_cast<Dispatch>(vtable[0]);
            const char* sharedPath = reinterpret_cast<const char*>(dispatch(host, 1, FHD_GetPath, 7, 0));
            CHECK(sharedPath && shared == sharedPath);
            CHECK(root == reinterpret_cast<const char*>(dispatch(host, 1, FHD_GetPath, 0, 0)));
            CHECK(FLPluginScanner::getInstance().getUserDataPath() ==
                reinterpret_cast<const char*>(dispatch(host, 1, FHD_GetPath, 1, 0)));
            CHECK(instance.m_activePresetDir ==
                reinterpret_cast<const char*>(dispatch(host, 1, FHD_GetPath, 3, 0)));
            auto* manager = reinterpret_cast<FLPathManager*>(dispatch(host, 1, FHD_GetPathManager, 0, 0));
            char buffer[PathBufferLength];
            CHECK(manager && manager->getPath(FLData_PluginPresets, PI_Factory, buffer));
            CHECK(FLPluginScanner::getInstance().getDataPath(FLData_PluginPresets, PI_Factory) == buffer);
            CHECK(!manager->getPath(FLData_PluginPresets, 2, buffer) && buffer[0] == 0);
            CHECK(sharedPath == reinterpret_cast<const char*>(dispatch(host, 1, FHD_GetPath, 7, 0)));
            CHECK(shared == sharedPath);
        }
    }

    static void setName(FLPluginInstance& instance, const std::string& name) { instance.m_pluginName = name; }

    static void setResolution(FLPluginInstance& instance, int32_t resolution) {
        instance.m_midiResolution = resolution;
    }

    static void setLegacy(FLPluginInstance& instance, bool legacy) {
        instance.m_pluginFlags = legacy ? 0 : FPF_NewVoiceParams;
    }

    static void setRenderer(FLPluginInstance& instance, bool generator) {
        instance.m_isGenerator = generator;
        instance.m_isInstrument = generator;
        instance.m_fnGenRender = renderGenerator;
        instance.m_fnEffRender = renderEffect;
    }

    static void testUtilityRole(FLPluginInstance& instance) {
        instance.m_isInstrument = false;
        instance.m_isGenerator = true;
        const auto flags = instance.m_pluginFlags;
        instance.m_pluginFlags = FPF_NoProcess;
        const float input[] = {0.25f, -0.5f};
        float output[2]{};
        instance.processAudio(input, input, output, nullptr, 2, 120, 0, false);
        CHECK(instance.getOutputControlDefault() == 1 && instance.getEffectMix() == 1);
        CHECK(output[0] == input[0] && output[1] == input[1]);
        instance.m_isGenerator = false;
        instance.m_pluginFlags = flags;
    }

    static void setNoProcess(FLPluginInstance& instance, bool enabled) {
        instance.m_pluginFlags = enabled ? FPF_NoProcess : FPF_NewVoiceParams;
        instance.m_fnGenRender = nullptr;
        instance.m_fnEffRender = nullptr;
    }

    static void testMixCallbacks(FLPluginInstance& instance) {
        using Mix = void(*)(void*, const float*, float*, int32_t, float, float, float*, float*);
        for (void** vtable : {instance.m_cppVmt, instance.m_pascalVmt + 25}) {
            const float mono[] = {1, 2, -1, 0.5f};
            float destination[8];
            std::fill_n(destination, 8, 0.25f);
            float lastLeft = 0, lastRight = 1;
            reinterpret_cast<Mix>(vtable[11])(nullptr, mono, destination, 4, 1, 0, &lastLeft, &lastRight);
            CHECK(lastLeft == 1 && lastRight == 0);
            for (int i = 0; i < 4; ++i) {
                CHECK(destination[2 * i] == 0.25f + mono[i] * ((i + 1) / 4.0f));
                CHECK(destination[2 * i + 1] == 0.25f + mono[i] * (1 - (i + 1) / 4.0f));
            }
            const float stereo[] = {1, -1, 2, -2};
            std::fill_n(destination, 8, 0.25f);
            lastLeft = 0.5f;
            lastRight = 0.25f;
            reinterpret_cast<Mix>(vtable[12])(nullptr, stereo, destination, 2, 0.5f, 0.25f, &lastLeft, &lastRight);
            for (int i = 0; i < 2; ++i) {
                CHECK(destination[2 * i] == 0.25f + stereo[2 * i] * 0.5f);
                CHECK(destination[2 * i + 1] == 0.25f + stereo[2 * i + 1] * 0.25f);
            }
        }

    }

    static void testReleaseCallback(FLPluginInstance& instance, FakePlugin& fake) {
        using Release = void(*)(void*, intptr_t);
        for (auto* host : {&instance.m_cppHost, &instance.m_pascalHost}) {
            CHECK(offsetof(FLPluginInstance::HostStruct, owner) == 256);
            for (int32_t reserved : host->Reserved) CHECK(reserved == 0);
            instance.noteOn(0, 60, 0.75f);
            void** vtable = static_cast<void**>(host->vmtPtr);
            if (host == &instance.m_pascalHost) vtable += 25;
            reinterpret_cast<Release>(vtable[4])(host, fake.lastTag);
            CHECK(instance.m_voices[fake.lastTag - 1].released);
            CHECK(instance.m_voices[fake.lastTag - 1].active);
            const int kills = fake.kills;
            instance.allNotesOff();
            CHECK(fake.kills == kills + 1);
        }
    }
};

static void writeFixture(const fs::path& path, const std::string& bytes = "synthetic fixture") {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << bytes;
    file.close();
    CHECK(file.good());
}

struct TestPathList : FLSearchDirList {
    std::vector<std::string> files;
    std::vector<intptr_t> tags;
    bool sorted = false;
    void add(char* file, intptr_t tag) override { files.emplace_back(file); tags.push_back(tag); }
    void sort() override { std::sort(files.begin(), files.end()); sorted = true; }
};

struct TestPathSearch : FLSearchDirCallback {
    std::vector<std::string> files;
    bool stopAfterFirst = false;
    void itemFound(uint8_t index, char* name, char* path, int32_t attributes, double, int32_t& stop) override {
        CHECK(index == PI_Factory && attributes == 0x20);
        CHECK(fs::is_regular_file(fs::u8path(path) / fs::u8path(name)));
        files.emplace_back(name);
        stop = stopAfterFirst ? 1 : 0;
    }
};

static void testDataPathsAndBanks(const fs::path& directory) {
    auto& scanner = FLPluginScanner::getInstance();
    FLPathManager manager;
    char buffer[PathBufferLength];
    CHECK(manager.getPath(FLData_GeneratorPlugins, PI_User, buffer));
    CHECK(fs::u8path(buffer).lexically_normal() == (directory / "Image-Line" / "").lexically_normal());
    CHECK(manager.getPath(FLData_SharedPacks, PI_User, buffer));
    CHECK(fs::is_directory(fs::u8path(buffer) / "FLEX" / "Packs"));
    CHECK(fs::is_directory(fs::u8path(buffer) / "FLEX" / "Images"));
    CHECK(fs::u8path(scanner.getFlexDataPath(PI_User)).lexically_normal() ==
          (fs::u8path(buffer) / "FLEX" / "").lexically_normal());
    CHECK(manager.getPath(FLData_SharedPacks, PI_Factory, buffer));
    const auto factoryPack = fs::u8path(buffer) / "FLEX" / "Packs" / "Factory.flexpack";
    writeFixture(factoryPack);
    CHECK(fs::is_regular_file(fs::u8path(scanner.getFlexDataPath(PI_Factory)) / "Packs" / "Factory.flexpack"));
    CHECK(!manager.getPath(9999, PI_User, buffer) && !*buffer);

    for (const auto& bank : std::vector<std::pair<std::string, std::string>>{
             {"Toxic Biohazard", ".tbio"}, {"Morphine", ".mrp"}, {"GMS", ".gmsynth"},
             {"MiniSynth", ".flms"}, {"Soundfont Player", ".sf2"}, {"DirectWave", ".dwp"}}) {
        FLPluginEntry entry;
        entry.name = bank.first;
        const auto factory = fs::u8path(scanner.getPluginPresetPath(entry, PI_Factory));
        const auto user = fs::u8path(scanner.getPluginPresetPath(entry, PI_User));
        writeFixture(factory / ("Default" + bank.second));
        writeFixture(factory / "Pads" / ("Warm" + bank.second));
        writeFixture(factory / "Pads" / "Ignored.txt");
        if (entry.name == "DirectWave") writeFixture(factory / "Banks" / "Full bank.DWB");
        writeFixture(user / "My patch.FST");
        FLPluginScannerTestAccess::scan(entry);
        CHECK(entry.presets.size() == (entry.name == "DirectWave" ? 4 : 3) &&
              entry.presets.front().name == "Default");
        CHECK(entry.presets.back().displayName == "Pads / Warm");
        CHECK(FLPluginScanner::isNativePresetFile(entry.name, entry.presets.front().fullPath));
        CHECK(!FLPluginScanner::isNativePresetFile("Sytrus", entry.presets.front().fullPath));

        TestPathList list;
        std::string subPath = "Generators/" + bank.first;
        std::string extension = bank.second;
        CHECK(manager.scanDir(FLData_PluginPresets, subPath.data(), extension.data(), &list,
                              1 | 2 | 4 | 8 | 128, 1u << PI_Factory, nullptr));
        CHECK(list.files == std::vector<std::string>({"Default", (fs::path("Pads") / "Warm").u8string()}));
        CHECK(list.sorted && list.tags.size() == 2 && list.tags[0] == PI_Factory && list.tags[1] == PI_Factory);
        TestPathSearch search;
        std::string mask = "*" + bank.second;
        manager.searchDir(FLData_PluginPresets, subPath.data(), mask.data(), 0, &search, 2, 8);
        CHECK(search.files.size() == 2);
        search.files.clear();
        search.stopAfterFirst = true;
        manager.searchDir(FLData_PluginPresets, subPath.data(), mask.data(), 0, &search, 2, 8);
        CHECK(search.files.size() == 1);
        std::string file = subPath + "/Default" + bank.second;
        CHECK(manager.addPath(FLData_PluginPresets, file.data(), 2, 0, buffer));
        CHECK(fs::is_regular_file(fs::u8path(buffer)));
        CHECK(manager.getPathIndexFor(FLData_PluginPresets, buffer) == PI_Factory);
    }
    FLPluginEntry keys;
    keys.name = "FL Keys";
    const auto keysFactory = fs::u8path(scanner.getPluginPresetPath(keys, PI_Factory));
    const auto keysUser = fs::u8path(scanner.getPluginPresetPath(keys, PI_User));
    writeFixture(keysFactory / "DEFAULT.fst");
    writeFixture(keysFactory / "Concert Piano.fst");
    writeFixture(keysUser / "default.fst");
    FLPluginScannerTestAccess::scan(keys);
    CHECK(keys.presets.size() == 3);
    CHECK(keys.presets[0].name == "default" && keys.presets[0].isDefault());
    CHECK(keys.presets[1].name == "DEFAULT" && keys.presets[1].isDefault());
    CHECK(!keys.presets[2].isDefault());
    std::string missing = "missing/file.tbio";
    CHECK(!manager.addPath(FLData_PluginPresets, missing.data(), 2, 0, buffer) && !*buffer);
    std::string cache = "NewCache/Images/";
    CHECK(manager.addPath(FLData_GeneratorPlugins, cache.data(), 1, 1, buffer));
    CHECK(fs::is_directory(fs::u8path(buffer)));
    CHECK(!manager.addPath(FLData_GeneratorPlugins, cache.data(), 2, 1, buffer));
    CHECK(!fs::exists(fs::u8path(scanner.getDataPath(FLData_GeneratorPlugins, PI_Factory)) / "NewCache"));

    const auto factorySample = fs::u8path(scanner.getFLRootPath()) / "Data/Patches/Packs/Test.wav";
    const auto userSample = fs::u8path(scanner.getUserDataPath()) / "Presets/Packs/Test.wav";
    writeFixture(factorySample);
    writeFixture(userSample);
    const auto legacySample = fs::u8path(scanner.getDataPath(FLData_Packs, PI_Factory)) /
        "Legacy/Drums/LegacyOnly.wav";
    writeFixture(legacySample);
    CHECK(fs::u8path(scanner.resolveRelativePath(
        "D:\\Program Files\\FLStudio4\\Data\\Patches\\Packs\\Drums\\LegacyOnly.wav")) == legacySample);
    CHECK(fs::u8path(scanner.resolveRelativePath("%FLStudioFactoryData%\\Data\\Patches\\Packs\\Test.wav")) == factorySample);
    CHECK(fs::u8path(scanner.resolveRelativePath("%FLStudioUserData%/Presets/Packs/Test.wav")) == userSample);
    FLPluginInstance instance;
    for (const auto& sample : {factorySample, userSample}) {
        const std::string full = sample.u8string();
        const std::string packed = reinterpret_cast<const char*>(instance.handleHostDispatcher(
            1, FHD_PackDataFile, 0, reinterpret_cast<intptr_t>(full.c_str())));
        CHECK(packed.front() == '%');
        CHECK(fs::u8path(scanner.resolveRelativePath(packed)) == sample);
    }
    CHECK(fs::u8path(scanner.getDataPath(FLData_Scores, PI_Factory)).lexically_normal() ==
          (fs::u8path(scanner.getFLRootPath()) / "Data/Patches/Scores/").lexically_normal());
    CHECK(fs::u8path(scanner.getDataPath(FLData_SystemData, PI_Factory)).lexically_normal() ==
          (fs::u8path(scanner.getFLRootPath()) / "System/").lexically_normal());
    FLPluginEntry dance;
    dance.name = "Fruity Dance";
    dance.category = "Generator";
    dance.isGenerator = false;
    CHECK(fs::u8path(scanner.getPluginPresetPath(dance, PI_Factory)).lexically_normal() ==
          (fs::u8path(scanner.getDataPath(FLData_PluginPresets, PI_Factory)) /
           "Generators/Fruity Dance/").lexically_normal());
}

static void testPluginCatalog() {
    std::vector<FLPluginEntry> plugins;
    for (const auto& item : std::vector<std::pair<std::string, bool>>{
             {"Zulu", true}, {"Patcher", false}, {"alpha", true}, {"Alpha Effect", false},
             {"Beta", true}, {"Patcher", true}}) {
        FLPluginEntry entry;
        entry.name = item.first;
        entry.isGenerator = item.second;
        plugins.push_back(std::move(entry));
    }
    FLPluginScannerTestAccess::setPlugins(std::move(plugins));
    const auto& scanner = FLPluginScanner::getInstance();
    CHECK(scanner.getPluginIndices(true) == std::vector<int>({2, 4, 5, 0}));
    CHECK(scanner.getPluginIndices(false) == std::vector<int>({3, 1}));
    CHECK(scanner.findPluginIndexByName("Patcher", false) == 1);
    CHECK(scanner.findPluginIndexByName("Patcher", true) == 5);
    CHECK(scanner.findPluginIndexByName("Beta", false) == -1);
    CHECK(!FLPluginScanner::isPluginExcluded("Fruity DrumSynth Live"));
    CHECK(!FLPluginScanner::isPluginExcluded("Soundfont Player"));
    CHECK(!FLPluginScanner::isPluginExcluded("FPC"));
    CHECK(!FLPluginScanner::isPluginExcluded("Slicex"));
    CHECK(!FLPluginScanner::isPluginExcluded("Fruity Slicer 2"));
    CHECK(!FLPluginScanner::isPluginExcluded("Fruity Dance"));
    CHECK(!FLPluginScanner::isPluginExcluded("Fruity NoteBook 2"));
    CHECK(!FLPluginScanner::isPluginExcluded("Wave Candy"));
    CHECK(!FLPluginScanner::isPluginExcluded("DirectWave"));
    CHECK(!FLPluginScanner::isPluginExcluded("FL Keys"));
    CHECK(!FLPluginScanner::isPluginExcluded("Newtone"));
    CHECK(!FLPluginScanner::isPluginExcluded("Edison"));
    CHECK(!FLPluginScanner::isPluginExcluded("Newtime"));
    CHECK(FLPluginScanner::isPluginExcluded("Patcher"));
    FLPluginScannerTestAccess::setPlugins({});
    CHECK(scanner.getPluginIndices(true).empty());
    CHECK(scanner.getPluginIndices(false).empty());
    CHECK(scanner.findPluginIndexByName("Patcher", false) == -1);
}

static void testNativeFilesAndPacks(FLPluginInstance& instance, FakePlugin& fake, const fs::path& directory) {
    FLPluginInstanceTestAccess::setName(instance, "Toxic Biohazard");
    const auto preset = (directory / "fixtures" / "Test.tbio").u8string();
    writeFixture(fs::u8path(preset));
    CHECK(FHD_LocateDataFile == 14 && FPD_LoadFile == 18 && FHD_CallDownloader == 31);
    CHECK(preset == reinterpret_cast<const char*>(instance.handleHostDispatcher(
        1, FHD_LocateDataFile, 0, reinterpret_cast<intptr_t>(preset.c_str()))));
    CHECK(preset == reinterpret_cast<const char*>(instance.handleHostDispatcher(
        1, FHD_PackDataFile, 0, reinterpret_cast<intptr_t>(preset.c_str()))));
    CHECK(!*reinterpret_cast<const char*>(instance.handleHostDispatcher(
        1, FHD_LocateDataFile, 0, reinterpret_cast<intptr_t>(""))));
    const auto folder = (directory / "fixtures").u8string();
    CHECK(!*reinterpret_cast<const char*>(instance.handleHostDispatcher(
        1, FHD_LocateDataFile, 0, reinterpret_cast<intptr_t>(folder.c_str()))));
    const auto missing = (directory / "fixtures" / "Missing.tbio").u8string();
    CHECK(!*reinterpret_cast<const char*>(instance.handleHostDispatcher(
        1, FHD_LocateDataFile, 0, reinterpret_cast<intptr_t>(missing.c_str()))));
    CHECK(instance.loadPresetFromFile(preset) && fake.loadedFile == preset);
    fake.fileLoadResult = 2;
    CHECK(!instance.loadPresetFromFile(preset));
    fake.fileLoadResult = 1;

    FLPluginInstanceTestAccess::setName(instance, "DirectWave");
    const auto directWave = (directory / "fixtures" / "Test.dwp").u8string();
    writeFixture(fs::u8path(directWave));
    fake.fileLoadResult = 0;
    CHECK(instance.loadPresetFromFile(directWave) && fake.loadedFile == directWave);
    fake.fileLoadResult = 2;
    CHECK(!instance.loadPresetFromFile(directWave));
    CHECK(!instance.loadPresetFromFile((directory / "fixtures" / "Missing.dwp").u8string()));
    CHECK(!instance.loadPresetFromFile(preset));
    fake.fileLoadResult = 1;

    const auto pack = (directory / "fixtures" / "Test.FLEXPACK").u8string();
    writeFixture(fs::u8path(pack));
    writeFixture(directory / "fixtures" / "Test.ini", "pack metadata");
    CHECK(!instance.loadPresetFromFile(pack));
    FLPluginInstanceTestAccess::setName(instance, "FLEX");
    CHECK(!instance.loadPresetFromFile(preset));
    CHECK(instance.loadPresetFromFile(pack));
    const auto target = fs::u8path(FLPluginScanner::getInstance().getFlexDataPath(PI_User)) /
        "Packs" / "Test.FLEXPACK";
    CHECK(fs::file_size(target) == fs::file_size(fs::u8path(pack)));
    CHECK(fs::is_regular_file(target.parent_path() / "Test.ini"));
    CHECK(!instance.loadPresetFromFile(pack));
    CHECK(instance.loadPresetFromFile(target.u8string()));
    const auto empty = directory / "fixtures" / "Empty.flexpack";
    writeFixture(empty, "");
    CHECK(!instance.loadPresetFromFile(empty.u8string()));
}

static void testInternalPresets(FLPluginInstance& instance, FakePlugin& fake) {
    CHECK(FPD_SetPreset == 9 && FPD_SetPlaying == 12 && FHD_SetNumPresets == 9);
    CHECK(FPE_MIDI_Vol == 3);
    CHECK(instance.getNumInternalPresets() == 0);
    CHECK(instance.handleHostDispatcher(1, FHD_SetNumPresets, 0, 5) == 1);
    CHECK(instance.getNumInternalPresets() == 5);
    CHECK(instance.handleHostDispatcher(1, FHD_SetNumPresets, 0, -1) == 0);
    CHECK(instance.handleHostDispatcher(1, FHD_SetNumPresets, 0, static_cast<intptr_t>(INT32_MAX) + 1) == 0);
    CHECK(instance.getNumInternalPresets() == 5);
    CHECK(instance.getInternalPresetName(3) == "Internal 3");
    const int loads = fake.internalPresetLoads;
    CHECK(!instance.loadInternalPreset(-1) && !instance.loadInternalPreset(5));
    CHECK(fake.internalPresetLoads == loads);
    CHECK(instance.loadInternalPreset(3));
    CHECK(fake.internalPresetLoads == loads + 1 && fake.internalPreset == 3);
    CHECK(fake.channelVolume == floatBits(1.0f));
    CHECK(instance.handleHostDispatcher(1, FHD_SetNumPresets, 0, 0) == 1);
    CHECK(instance.getNumInternalPresets() == 0 && !instance.loadInternalPreset(0));
    CHECK(instance.handleHostDispatcher(1, FHD_SetNumPresets, 0, 5) == 1);
}

static void testAudioContention(FLPluginInstance& instance) {
    FLPluginInstanceTestAccess::setRenderer(instance, true);
    float left[64], right[64];
    std::promise<void> started;
    instance.handleLockPlugin();
    instance.handleLockPlugin();
    auto rendered = std::async(std::launch::async, [&] {
        started.set_value();
        instance.processAudio(nullptr, nullptr, left, right, 64, 120, 0, false);
    });
    started.get_future().wait();
    CHECK(rendered.wait_for(std::chrono::milliseconds(5)) == std::future_status::timeout);
    instance.handleUnlockPlugin();
    CHECK(rendered.wait_for(std::chrono::milliseconds(5)) == std::future_status::timeout);
    instance.handleUnlockPlugin();
    rendered.get();
    for (int i = 0; i < 64; ++i) {
        CHECK(left[i] == 0.25f * kDefaultWrapperGain);
        CHECK(right[i] == -0.75f * kDefaultWrapperGain);
    }
}

static void testPluginText(FLPluginInstance& instance, FakePlugin& fake) {
    const std::pair<const char*, const char*> cases[] = {
        {"^aLFO Phase: 78%", "LFO Phase: 78%"},
        {"^b^aLFO Phase", "LFO Phase"},
        {"^c^b^aLFO Phase", "LFO Phase"},
        {"^a^a^b^cName", "Name"},
        {"^cHello", "Hello"},
        {"^a", ""},
        {"^b^a", ""},
        {"", ""},
        {"LFO Phase", "LFO Phase"},
        {"^", "^"},
        {"^a^", "^"},
        {"^zName", "^zName"},
        {"^a^zName", "^zName"},
        {"^^aName", "^^aName"},
        {"2^a + 3^b", "2^a + 3^b"},
        {"  ^aName", "  ^aName"},
        {"^b^aCaf\xc3\xa9", "Caf\xc3\xa9"},
    };
    std::string hint;
    int calls = 0;
    instance.onHintChanged = [&](const std::string& value) {
        hint = value;
        ++calls;
    };
    const int writes = fake.writes;
    for (const auto& sample : cases) {
        fake.parameterName = sample.first;
        fake.parameterDisplay = sample.first;
        CHECK(instance.getParamName(1) == sample.second);
        CHECK(instance.getParamDisplay(1) == sample.second);
        const int before = calls;
        instance.handleOnHint(1, sample.first);
        CHECK(hint == sample.second && calls == before + 1);
        CHECK(fake.parameterName == sample.first && fake.parameterDisplay == sample.first);
    }
    CHECK(fake.writes == writes && instance.getNumParams() == 7);
    const int before = calls;
    instance.handleOnHint(1, nullptr);
    CHECK(calls == before);
#ifdef _WIN32
    instance.handleOnHint(1, "\x01^b^aLegacy hint");
    CHECK(hint == "Legacy hint" && calls == before + 1);
#endif
    instance.onHintChanged = nullptr;
    instance.handleOnHint(1, "^aNo callback");
}

static void testParameters(FLPluginInstance& instance, FakePlugin& fake) {
    for (int param : {0, 1, 2, 3, 4}) {
        for (double normalized : {0.0, 0.25, 0.5, 0.75, 0.875, 1.0}) {
            instance.setParamNormalized(param, normalized, true);
            CHECK(fake.lastInput == std::lround(normalized * fake.midiResolution));
            CHECK(fake.lastFlags == (REC_UpdateValue | REC_FromMIDI | REC_UpdateControl));
            const int32_t native = fake.values[param];
            const int writes = fake.writes;
            double read = instance.getParamNormalized(param);
            CHECK(std::abs(read - normalized) < 0.011);
            CHECK(fake.writes == writes);
            double callback = -1.0;
            instance.onParamChangedFromPlug = [&](int index, double value) {
                CHECK(index == param);
                callback = value;
            };
            instance.handleOnParamChanged(1, param, native);
            CHECK(std::abs(callback - read) < 1e-8);
            instance.setParamNormalized(param, callback);
            CHECK(fake.values[param] == native);
        }
    }
    instance.onParamChangedFromPlug = nullptr;
    instance.setParamNormalized(5, 1.0);
    CHECK(instance.getParamNormalized(5) == 0.0);
    for (double value : {0.0, 0.49, 0.5, 1.0}) {
        instance.setParamNormalized(6, value);
        CHECK(instance.getParamNormalized(6) == (value < 0.5 ? 0.0 : 1.0));
    }
    instance.setParamNormalized(0, -1.0);
    CHECK(fake.values[0] == 0);
    instance.setParamNormalized(0, 2.0);
    CHECK(fake.values[0] == 160);
    int writes = fake.writes;
    instance.setParamNormalized(0, std::numeric_limits<double>::quiet_NaN());
    CHECK(fake.writes == writes);
}

static void testVoices(FLPluginInstance& instance, FakePlugin& fake) {
    CHECK(FPD_SetPlaying == 12);
    CHECK(FHD_EditorResized == 2);
    CHECK(FPF_WantNewTick == (1 << 5));
    CHECK(FPF_NewVoiceParams == (1 << 21));
    for (int note : {0, 24, 36, 59, 60, 69, 127}) {
        instance.noteOn(2, note, 0.75f);
        CHECK(fake.voice->InitLevels.Pitch == note * 100.0f);
        CHECK(fake.voice->FinalLevels.Pitch == (note - 60) * 100.0f);
        CHECK(fake.voice->InitLevels.Vol == 0.75f);
        CHECK(fake.voice->FinalLevels.Vol == 0.75f);
        CHECK(instance.handleVoiceProcessEvent(fake.lastTag, FVE_Color, 0, 0) == 2);
        CHECK(instance.handleVoiceProcessEvent(fake.lastTag, FVE_Velocity, 0, 0) == floatBits(0.75f));
        CHECK(instance.handleVoiceProcessEvent(fake.lastTag, 5, 0, 0) == floatBits(1.0f));
        CHECK(instance.handleVoiceProcessEvent(fake.lastTag, 1, 0, 0) == -1);
        instance.setPitchBendSemitones(-2.0f);
        CHECK(fake.voice->FinalLevels.Pitch == (note - 62) * 100.0f);
        CHECK(fake.voice->InitLevels.Pitch == note * 100.0f);
        instance.noteOff(3, note, 0.5f);
        const int releases = fake.releases;
        instance.noteOff(2, note, 0.5f);
        CHECK(fake.releases == releases + 1);
        CHECK(fake.lastReleased == 0);
        instance.allNotesOff();
        instance.setPitchBendSemitones(0.0f);
    }
    fake.nextHandle = -1;
    instance.noteOn(0, 60, 1.0f);
    CHECK(!FLPluginInstanceTestAccess::active(instance, fake.lastTag));
    fake.nextHandle = 0;

    instance.handleHostDispatcher(1, 20, (4 << 16) | 48, 100);
    CHECK(fake.voice->InitLevels.Pitch == 4800.0f);
    CHECK(instance.handleVoiceProcessEvent(fake.lastTag, FVE_Color, 0, 0) == 4);
    int releases = fake.releases;
    instance.handleHostDispatcher(1, 21, (4 << 16) | 48, -1);
    CHECK(fake.releases == releases + 1);
    instance.allNotesOff();
    instance.handleHostDispatcher(1, 56, 24, 127);
    CHECK(fake.voice->FinalLevels.Pitch == -3600.0f);
    releases = fake.releases;
    instance.handleHostDispatcher(1, 56, 24, 0);
    CHECK(fake.releases == releases + 1);
    instance.allNotesOff();

    fake.consumeMIDI = true;
    int triggers = fake.triggers;
    instance.noteOn(0, 60, 1.0f);
    CHECK(fake.triggers == triggers);
    fake.consumeMIDI = false;
    fake.forwardMIDI = true;
    instance.noteOn(0, 60, 1.0f);
    CHECK(fake.triggers == triggers + 1);
    releases = fake.releases;
    instance.noteOff(0, 60, 0.5f);
    CHECK(fake.releases == releases + 1);
    instance.allNotesOff();
    fake.forwardMIDI = false;
}

static void testLegacyVoices(FLPluginInstance& instance, FakePlugin& fake) {
    FLPluginInstanceTestAccess::setLegacy(instance, true);
    for (int note : {0, 24, 60, 72, 127}) {
        instance.noteOn(1, note, 0.5f);
        CHECK(fake.legacyVoice->InitLevels.Pitch == note * 100);
        CHECK(fake.legacyVoice->FinalLevels.Pitch == (note - 60) * 100);
        CHECK(fake.legacyVoice->InitLevels.Vol == 0.5f);
        CHECK(fake.legacyVoice->FinalLevels.Vol == 0.5f);
        CHECK(fake.legacyVoice->FinalLevels.Pan == 0);
        instance.setPitchBendSemitones(1.25f);
        CHECK(fake.legacyVoice->InitLevels.Pitch == note * 100);
        CHECK(fake.legacyVoice->FinalLevels.Pitch == (note - 60) * 100 + 125);
        instance.noteOff(1, note, 0.5f);
        instance.allNotesOff();
        instance.setPitchBendSemitones(0);
    }
    FLPluginInstanceTestAccess::setLegacy(instance, false);
}

static void testGain(FLPluginInstance& instance) {
    CHECK(instance.getMasterVolume() == kDefaultMasterVolume);
    CHECK(masterVolumeGain(0) == 0 && masterVolumeGain(1) == 1);
    CHECK(masterVolumeGain(kDefaultMasterVolume) == kDefaultWrapperGain);
    CHECK(masterVolumeDisplay(kDefaultMasterVolume) == "78% (-5.2 dB)");
    double previous = 0;
    for (int percent = 1; percent <= 100; ++percent) {
        const double gain = masterVolumeGain(percent / 100.0);
        CHECK(gain > previous && gain <= 1);
        previous = gain;
    }
    for (double invalid : {-0.01, 1.01, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
        CHECK(!instance.setMasterVolume(invalid));
        CHECK(instance.getMasterVolume() == kDefaultMasterVolume);
    }
    CHECK(std::abs(20 * std::log10(kDefaultWrapperGain) + 5.2) < 1e-6);
    float left[4], right[4];
    FLPluginInstanceTestAccess::setRenderer(instance, true);
    for (int block = 0; block < 2; ++block) {
        instance.processAudio(nullptr, nullptr, left, right, 4, 120, 0, false);
        for (int i = 0; i < 4; ++i) {
            CHECK(std::abs(left[i] - 0.25f * kDefaultWrapperGain) < 1e-7);
            CHECK(std::abs(right[i] + 0.75f * kDefaultWrapperGain) < 1e-7);
        }
    }
    FLPluginInstanceTestAccess::setRenderer(instance, false);
    const float inputLeft[] = {0.5f, -0.5f, std::numeric_limits<float>::infinity(), 0};
    const float inputRight[] = {-0.25f, 0.25f, std::numeric_limits<float>::quiet_NaN(), 1};
    instance.processAudio(inputLeft, inputRight, left, right, 4, 120, 0, false);
    CHECK(left[0] == 0.5f);
    CHECK(right[0] == -0.25f);
    CHECK(left[2] == 0 && right[2] == 0);
    CHECK(right[3] == 1);

    for (bool generator : {true, false}) {
        FLPluginInstanceTestAccess::setRenderer(instance, generator);
        FLPluginInstanceTestAccess::setNoProcess(instance, true);
        const float gain = generator ? kDefaultWrapperGain : 1;
        instance.processAudio(inputLeft, inputRight, left, right, 4, 120, 0, false);
        CHECK(left[0] == inputLeft[0] * gain);
        CHECK(right[0] == inputRight[0] * gain);
        CHECK(left[2] == 0 && right[2] == 0);
        instance.processAudio(inputLeft, nullptr, left, right, 4, 120, 0, false);
        CHECK(left[0] == right[0] && left[0] == inputLeft[0] * gain);
        instance.processAudio(nullptr, nullptr, left, right, 4, 120, 0, false);
        for (int i = 0; i < 4; ++i) CHECK(left[i] == 0 && right[i] == 0);
    }
    FLPluginInstanceTestAccess::setRenderer(instance, true);
    FLPluginInstanceTestAccess::setNoProcess(instance, true);
    std::vector<float> constant(2048, 1), output(2048);
    CHECK(instance.setMasterVolume(1));
    instance.processAudio(constant.data(), constant.data(), output.data(), nullptr, 2048, 120, 0, false);
    CHECK(output.front() > kDefaultWrapperGain && output.front() < kDefaultWrapperGain + 0.01);
    CHECK(output.back() == 1);
    for (size_t i = 1; i < output.size(); ++i) {
        CHECK(output[i] >= output[i - 1]);
        CHECK(output[i] - output[i - 1] < 0.01);
    }
    CHECK(instance.setMasterVolume(0));
    instance.processAudio(constant.data(), constant.data(), output.data(), nullptr, 2048, 120, 0, false);
    CHECK(output.front() < 1 && output.front() > 0.99);
    CHECK(output.back() == 0);
    for (size_t i = 1; i < output.size(); ++i) {
        CHECK(output[i] <= output[i - 1]);
        CHECK(output[i - 1] - output[i] < 0.01);
    }
    CHECK(instance.setMasterVolume(kDefaultMasterVolume));
    instance.processAudio(constant.data(), constant.data(), output.data(), nullptr, 2048, 120, 0, false);
    CHECK(output.back() == kDefaultWrapperGain);
    FLPluginInstanceTestAccess::setNoProcess(instance, false);
}

static void testEffectMix(FLPluginInstance& instance, FakePlugin& fake) {
    FLPluginInstanceTestAccess::setRenderer(instance, false);
    CHECK(instance.getEffectMix() == 1);
    CHECK(effectMixDisplay(0.25) == "25% wet / 75% dry");
    for (double invalid : {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
        CHECK(!instance.setEffectMix(invalid) && instance.getEffectMix() == 1);
    }
    fake.effectGain = 2;
    fake.mutateEffectInput = true;
    instance.setSampleRate(44100);
    std::vector<float> left(441, 1), right(441, -2), outLeft(441), outRight(441);
    const auto render = [&] {
        instance.processAudio(left.data(), right.data(), outLeft.data(), outRight.data(), 441, 120, 0, false);
    };
    render();
    CHECK(outLeft.front() == 2 && outRight.front() == -4);
    CHECK(instance.setEffectMix(0));
    render();
    CHECK(outLeft.front() < 2 && outLeft.front() > 1.99);
    CHECK(outLeft.back() == 1 && outRight.back() == -2);
    for (size_t i = 1; i < outLeft.size(); ++i) CHECK(outLeft[i] <= outLeft[i - 1]);
    render();
    CHECK(outLeft.front() == 1 && outRight.front() == -2);
    CHECK(instance.setEffectMix(0.5));
    render();
    CHECK(outLeft.back() == 1.5f && outRight.back() == -3);
    render();
    CHECK(outLeft.front() == 1.5f && outRight.front() == -3);
    CHECK(instance.setEffectMix(1));
    render();
    CHECK(outLeft.back() == 2 && outRight.back() == -4);
    instance.processAudio(left.data(), nullptr, outLeft.data(), outRight.data(), 441, 120, 0, false);
    CHECK(outLeft == outRight && outLeft.front() == 2);
    instance.processAudio(left.data(), right.data(), left.data(), right.data(), 441, 120, 0, false);
    CHECK(left.front() == 2 && right.front() == -4);
    instance.processAudio(nullptr, nullptr, outLeft.data(), outRight.data(), 441, 120, 0, false);
    for (size_t i = 0; i < outLeft.size(); ++i) CHECK(outLeft[i] == 0 && outRight[i] == 0);
    left[0] = std::numeric_limits<float>::infinity();
    right[0] = std::numeric_limits<float>::quiet_NaN();
    render();
    CHECK(outLeft[0] == 0 && outRight[0] == 0);
    CHECK(instance.setEffectMix(0));
    render();
    render();
    CHECK(outLeft[0] == 0 && outRight[0] == 0);
    CHECK(instance.setEffectMix(1));
    fake.effectGain = 1;
    fake.mutateEffectInput = false;
    render();
}

static void testInstallationDiscovery(const fs::path& directory) {
    const auto applications = directory / "Applications";
    const auto imageLine = directory / "Image-Line";
    const auto makeInstallation = [&](const fs::path& root) {
        CHECK(fs::create_directories(root / "Plugins/Fruity/Generators"));
        CHECK(fs::create_directories(root / "Plugins/Fruity/Effects"));
        CHECK(fs::create_directories(root / "Shared"));
    };
    for (const char* name : {"FL Studio 2026.app", "FL Studio 2028.app", "FL Studio 2027.1.app", "FL Studio.app"})
        makeInstallation(applications / name / "Contents/Resources/FL");
    CHECK(fs::create_directories(applications / "FL Studio 9999.app"));
    makeInstallation(applications / "Unrelated.app" / "Contents/Resources/FL");
    const auto mac = FLPluginScannerTestAccess::findInstallations({applications.u8string(), applications.u8string()});
    CHECK(mac.size() == 4 && fs::u8path(mac.front()).filename() == "FL Studio 2028.app");
    CHECK(fs::u8path(mac.back()).filename() == "FL Studio.app");
    CHECK(FLPluginScanner::resolveInstallationRoot(mac.front()) ==
          FLPluginScanner::resolveInstallationRoot((applications / "FL Studio 2028.app/Contents/Resources/FL").u8string()));
    for (const char* name : {"FL Studio 99", "FL Studio 100", "FL Studio 100.1"})
        makeInstallation(imageLine / name);
    const auto windows = FLPluginScannerTestAccess::findInstallations({imageLine.u8string()});
    CHECK(windows.size() == 3 && fs::u8path(windows.front()).filename() == "FL Studio 100.1");
    CHECK(fs::u8path(windows.back()).filename() == "FL Studio 99");
    CHECK(FLPluginScanner::resolveInstallationRoot("").empty());
    CHECK(FLPluginScanner::resolveInstallationRoot("relative/FL Studio").empty());
    CHECK(FLPluginScanner::resolveInstallationRoot((applications / "FL Studio 9999.app").u8string()).empty());
    CHECK(FLPluginScanner::resolveInstallationRoot((applications / "missing.app").u8string()).empty());
}

static void testTransport(FLPluginInstance& instance, FakePlugin& fake) {
    CHECK(FPD_SongPosChanged == 13 && FPD_SetTimeSig == 14 && FHD_GetPlaybackTime == 37);
    static_assert(sizeof(TFPTime) == 16 && sizeof(TSongTime) == 12 && sizeof(TTimeSigInfo) == 12);
    instance.setSampleRate(48000);
    float left[64], right[64];
    instance.processAudio(nullptr, nullptr, left, right, 64, 120, 9.5, true, 6.25, 3, 4);
    CHECK(fake.playing && fake.timeSignature.StepsPerBar == 12);
    CHECK(fake.timeSignature.StepsPerBeat == 4 && fake.timeSignature.PPQ == 96);
    TFPTime running{};
    CHECK(instance.handleHostDispatcher(1, FHD_GetMixingTime, GT_MSSinceStart, reinterpret_cast<intptr_t>(&running)) == 1);
    const double runningStart = running.t;
    const auto query = [&](intptr_t id, intptr_t format, double expected, double offset = 0) {
        TFPTime time{offset, 12345};
        CHECK(instance.handleHostDispatcher(1, id, format, reinterpret_cast<intptr_t>(&time)) == 1);
        CHECK(std::abs(time.t - expected) < 1e-8 && time.t2 == 0);
    };
    for (auto id : {FHD_GetMixingTime, FHD_GetPlaybackTime}) {
        query(id, GT_Beats | GT_LocalTime, 9.5);
        query(id, GT_Beats | (1u << 30), 9.5);
        query(id, GT_Ticks | GT_LocalTime, 912);
        query(id, GT_AbsoluteMS, 6250);
        query(id, GT_RunningMS | GT_LocalTime, 6250);
        query(id, GT_Beats, 10.5, 24000);
    }
    TSongTime time{};
    CHECK(instance.handleHostDispatcher(1, FHD_TicksToTime, reinterpret_cast<intptr_t>(&time), 913) == 1);
    CHECK(time.Bar == 3 && time.Step == 2 && time.Tick == 1);
    const int relocations = fake.relocations;
    const int playingNotifications = fake.playingNotifications;
    instance.processAudio(nullptr, nullptr, left, right, 64, 90, 9.5 + 64.0 / 48000 * 2,
                          true, 6.25 + 64.0 / 48000, 3, 4);
    CHECK(fake.relocations == relocations && fake.playingNotifications == playingNotifications);
    running = {};
    CHECK(instance.handleHostDispatcher(1, FHD_GetMixingTime, GT_MSSinceStart, reinterpret_cast<intptr_t>(&running)) == 1);
    CHECK(std::abs(running.t - runningStart - 64.0 / 48000 * 1000) < 1e-8);
    instance.processAudio(nullptr, nullptr, left, right, 64, 90, 0, true, 0, 7, 8);
    CHECK(fake.relocations == relocations + 1 && fake.timeSignature.StepsPerBar == 14);
    instance.processAudio(nullptr, nullptr, left, right, 64, 90, 1, false, 1.75, 7, 8);
    CHECK(!fake.playing && fake.playingNotifications == playingNotifications + 1);
    query(FHD_GetPlaybackTime, GT_RunningMS, 1750);
    instance.processAudio(nullptr, nullptr, left, right, 64, 90, 1, false, 1.75, 7, 8);
    query(FHD_GetPlaybackTime, GT_RunningMS, 1750);
    CHECK(instance.handleHostDispatcher(1, FHD_GetPlaybackTime, GT_Beats, 0) == 0);
    TFPTime invalid{};
    CHECK(instance.handleHostDispatcher(1, FHD_GetPlaybackTime, 255, reinterpret_cast<intptr_t>(&invalid)) == 0);
    instance.processAudio(nullptr, nullptr, left, right, 64, 120, -0.5, true, -0.25, 4, 4);
    query(FHD_GetPlaybackTime, GT_Ticks, -48);
    CHECK(instance.handleHostDispatcher(1, FHD_TicksToTime, reinterpret_cast<intptr_t>(&time), -48) == 1);
    CHECK(time.Bar == -1 && time.Step == 14 && time.Tick == 0);
    instance.setSampleRate(44100);
}

static void testStateStream() {
    MemoryFruityStream stream;
    const uint8_t bytes[] = {1, 2, 3};
    uint32_t count = 0;
    CHECK(MemoryFruityStream::Write(&stream, bytes, 3, &count) == 0 && count == 3);
    CHECK(MemoryFruityStream::Seek(&stream, 0, 0, nullptr) == 0);
    uint8_t result[8];
    std::fill_n(result, 8, 0xA5);
    CHECK(MemoryFruityStream::Read(&stream, result, 8, &count) == 1 && count == 3);
    CHECK(std::equal(bytes, bytes + 3, result));
    for (int i = 3; i < 8; ++i) CHECK(result[i] == 0xA5);
    CHECK(MemoryFruityStream::Seek(&stream, -1, 0, nullptr) < 0);
    CHECK(stream.pos == 3);
    CHECK(MemoryFruityStream::Seek(&stream, 0, 3, nullptr) < 0);
    CHECK(stream.pos == 3);
    CHECK(MemoryFruityStream::Seek(&stream, std::numeric_limits<int64_t>::max(), 0, nullptr) == 0);
    CHECK(MemoryFruityStream::Seek(&stream, 1, 1, nullptr) < 0);
    CHECK(MemoryFruityStream::Seek(&stream, 10, 0, nullptr) == 0);
    CHECK(MemoryFruityStream::Write(&stream, bytes, 3, &count) == 0);
    CHECK(stream.data.size() == 13 && stream.data[9] == 0 && stream.data[10] == 1);
    void* object = &stream;
    uint8_t iid[16]{};
    CHECK(MemoryFruityStream::QueryInterface(&stream, iid, &object) < 0 && object == nullptr);
    iid[0] = 0x0C;
    iid[8] = 0xC0;
    iid[15] = 0x46;
    CHECK(MemoryFruityStream::QueryInterface(&stream, iid, &object) == 0 && object == &stream);
    CHECK(MemoryFruityStream::Clone(&stream, &object) < 0 && object == nullptr);
#ifdef _WIN32
    STATSTG stats;
    memset(&stats, 0xA5, sizeof(stats));
    CHECK(MemoryFruityStream::Stat(&stream, &stats, STATFLAG_NONAME) == 0);
    CHECK(stats.type == STGTY_STREAM && stats.cbSize.QuadPart == 13);
    CHECK(stats.pwcsName == nullptr && stats.grfMode == STGM_READWRITE);
#endif
}

static std::vector<uint8_t> makePreset(std::initializer_list<uint8_t> events) {
    std::vector<uint8_t> bytes = {'F','L','h','d',6,0,0,0,0,0,0,0,0,0,'F','L','d','t',0,0,0,0};
    const uint32_t length = static_cast<uint32_t>(events.size());
    memcpy(bytes.data() + 18, &length, 4);
    bytes.insert(bytes.end(), events);
    return bytes;
}

static void testNativeClassIdentity() {
    CHECK(FLPluginScanner::moduleIdentifier("/Applications/FL 2030/Harmor/Harmor_x64.dylib") == "fl.native.harmor");
    CHECK(FLPluginScanner::moduleIdentifier(R"(C:\different\HARMOR_x64.DLL)") == "fl.native.harmor");
    CHECK(FLPluginScanner::moduleIdentifier("/renamed display/HaRmOr_arm64.dylib") == "fl.native.harmor");
    CHECK(nativeClassId("fl.native.harmor") == "46A736FEE0908392A74B1B7A56F9E292");
    FLPluginEntry instrument, effect;
    instrument.name = "Display name";
    instrument.identifier = "fl.native.harmor";
    instrument.isGenerator = true;
    effect.name = "Effect";
    effect.identifier = "fl.native.maximus";
    effect.isGenerator = false;
    const auto first = buildNativePluginClasses({instrument, effect});
    CHECK(first.size() == 2 && first[0].identifier == instrument.identifier && first[0].isGenerator);
    instrument.name = "Renamed";
    instrument.pluginDir = "/another/install/version";
    const auto reordered = buildNativePluginClasses({effect, instrument});
    CHECK(reordered.size() == 2 && reordered[1].cid == first[0].cid);
    CHECK(reordered[0].cid == first[1].cid && reordered[0].cid != reordered[1].cid);
    CHECK(buildNativePluginClasses({instrument, instrument}).empty());
    instrument.available = false;
    CHECK(buildNativePluginClasses({instrument, effect}).size() == 1);
}

static void testBridgeToolbarLayout() {
    for (int width = 1; width <= 2048; ++width) {
        const auto layout = BridgeToolbarLayout::compute(width);
        CHECK(layout.height >= 54 && (layout.height - 54) % 32 == 0);
        const auto repeated = BridgeToolbarLayout::compute(width);
        for (size_t i = 0; i < layout.controls.size(); ++i) {
            const auto& rect = layout.controls[i];
            CHECK(rect.x >= 0 && rect.width > 0 && rect.x + rect.width <= width);
            CHECK(rect.y >= 0 && rect.height == (i == kBridgeMasterVolumeControl || (i == 0 && rect.y == 8) ? 38 : 26) &&
                rect.y + rect.height <= layout.height);
            const auto& copy = repeated.controls[i];
            CHECK(rect.x == copy.x && rect.y == copy.y && rect.width == copy.width);
            for (size_t other = 0; other < i; ++other) {
                const auto& earlier = layout.controls[other];
                CHECK(rect.x >= earlier.x + earlier.width || earlier.x >= rect.x + rect.width ||
                    rect.y >= earlier.y + earlier.height || earlier.y >= rect.y + rect.height);
            }
        }
        const int padding = std::min(6, (width - 1) / 2);
        CHECK(layout.controls[kBridgeMasterVolumeControl].x +
            layout.controls[kBridgeMasterVolumeControl].width == width - padding);
        CHECK(layout.controls[kBridgeMasterVolumeControl].y == 8);
        if (width >= 212) CHECK(layout.controls[5].width >= 200);
        if (width >= 100) CHECK(layout.controls[0].width == 38 && layout.controls[0].height == 38);
        CHECK(bridgeEditorHeight(width, 55, false) == 55 + layout.height);
        CHECK(bridgeEditorHeight(width, 55, true) == 55 + layout.height + kBridgeResizeFooterHeight);
    }
    CHECK(BridgeToolbarLayout::compute(595).height == 86);
    CHECK(BridgeToolbarLayout::compute(596).height == 54);
    const auto narrow = BridgeToolbarLayout::compute(596);
    const auto wide = BridgeToolbarLayout::compute(1200);
    for (size_t flexible : {size_t{1}, size_t{5}})
        CHECK(wide.controls[flexible].width > narrow.controls[flexible].width);
}

static void testPresetParsing() {
    std::vector<uint8_t> state;
    auto valid = makePreset({201, 2, 'P', 0, 213, 3, 1, 2, 3});
    std::string name;
    CHECK(ExtractFstPluginState(valid.data(), valid.size(), state, &name));
    CHECK(state == std::vector<uint8_t>({1, 2, 3}) && name == "P");
    for (const auto& invalid : {
            makePreset({}),
            makePreset({213, 0}),
            makePreset({213, 5, 1, 2, 3}),
            makePreset({213, 0x80}),
            makePreset({213, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F}),
            makePreset({213, 1, 42, 0}),
            makePreset({213, 1, 42, 64, 0}),
            makePreset({213, 1, 42, 128, 0, 0, 0})}) {
        CHECK(!ExtractFstPluginState(invalid.data(), invalid.size(), state));
    }
    valid.pop_back();
    CHECK(!ExtractFstPluginState(valid.data(), valid.size(), state));
    valid[4] = valid[5] = valid[6] = valid[7] = 0xFF;
    CHECK(!ExtractFstPluginState(valid.data(), valid.size(), state));
}

} // namespace flbr

int main() {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() /
        ("flbridge-contracts-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CHECK(fs::create_directory(directory));
    flbr::FLPluginScannerTestAccess::configure(directory);
    flbr::testDataPathsAndBanks(directory);
    flbr::testPluginCatalog();
    flbr::testInstallationDiscovery(directory);
    flbr::testBridgeToolbarLayout();
    flbr::testNativeClassIdentity();
    flbr::FLPluginScannerTestAccess::testDiscovery(directory);
    flbr::FLPluginInstance instance;
    flbr::FLPluginInstanceTestAccess::testPaths(instance);
    flbr::FakePlugin fake;
    flbr::FLPluginInstanceTestAccess::attach(instance, fake);
    flbr::FLPluginInstanceTestAccess::testEditorSizing(instance, fake);
    flbr::FLPluginInstanceTestAccess::testChannelState(instance, fake);
    flbr::testPluginText(instance, fake);
    flbr::testParameters(instance, fake);
    fake.midiResolution = 65536;
    flbr::FLPluginInstanceTestAccess::setResolution(instance, fake.midiResolution);
    flbr::testParameters(instance, fake);
    flbr::testVoices(instance, fake);
    flbr::testLegacyVoices(instance, fake);
    flbr::FLPluginInstanceTestAccess::testReleaseCallback(instance, fake);
    flbr::FLPluginInstanceTestAccess::testMixCallbacks(instance);
    flbr::testGain(instance);
    flbr::testEffectMix(instance, fake);
    flbr::FLPluginInstanceTestAccess::testUtilityRole(instance);
    flbr::testTransport(instance, fake);
    flbr::testAudioContention(instance);
    flbr::testNativeFilesAndPacks(instance, fake, directory);
    flbr::testInternalPresets(instance, fake);
    flbr::FLPluginInstanceTestAccess::testSampleServices(instance, fake, directory);
    flbr::testStateStream();
    flbr::testPresetParsing();
    instance.unloadPlugin();
    CHECK(instance.getNumInternalPresets() == 0);
    CHECK(fs::remove_all(directory) > 0);
    std::puts("Automation, voices, host mixing, audio contention, data paths, native banks and FLEX pack import passed.");
    return 0;
}
