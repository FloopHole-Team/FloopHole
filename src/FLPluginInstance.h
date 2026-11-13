#pragma once

#include "FruitySDK.h"
#include "MasterVolume.h"
#include "FLPluginScanner.h"
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <atomic>
#include <array>
#include <map>
#include <limits>

namespace flbr {

struct EditorSizeConstraints {
    int minWidth, minHeight, maxWidth, maxHeight;
    int widthStep = 1, heightStep = 1;
    bool isResizable() const { return minWidth != maxWidth || minHeight != maxHeight; }
    void constrain(int& width, int& height) const;
};

struct VoiceSlot {
    bool active = false;
    bool released = false;
    int channel = 0;
    int midiNote = 60;
    float velocity = 0.8f;
    float relVelocity = 0.5f;
    intptr_t plugVoiceHandle = FVH_Null;
    uint64_t age = 0;
    TVoiceParams params; // Must remain at a fixed address while voice is active!
    TVoiceParamsOld legacyParams;
};

class FLPluginInstance {
public:
    explicit FLPluginInstance(bool generator = true);
    ~FLPluginInstance();

    bool loadPlugin(int pluginIndex);
    bool loadPluginByName(const std::string& name);
    void unloadPlugin();

    bool isLoaded() const { return m_plug != nullptr; }
    int getCurrentPluginIndex() const { return m_currentPluginIndex; }
    int getCurrentPresetIndex() const { return m_currentPresetIndex; }
    const std::string& getPluginName() const { return m_pluginName; }
    bool isGenerator() const { return m_isGenerator; }
    bool isInstrument() const { return m_isInstrument; }
    int getNumParams() const { return m_numParams; }

    // Audio & transport configuration
    void setSampleRate(double sampleRate);
    void setBlockSize(int32_t maxBlockSize);
    void setEnabled(bool enabled);
    double getMasterVolume() const { return m_masterVolume.load(std::memory_order_relaxed); }
    bool setMasterVolume(double value);
    double getEffectMix() const { return m_effectMix.load(std::memory_order_relaxed); }
    bool setEffectMix(double value);
    double getOutputControlValue() const { return m_isInstrument ? getMasterVolume() : getEffectMix(); }
    bool setOutputControlValue(double value) { return m_isInstrument ? setMasterVolume(value) : setEffectMix(value); }
    double getOutputControlDefault() const { return m_isInstrument ? kDefaultMasterVolume : kDefaultEffectMix; }
    std::string getOutputControlDisplay(double value) const {
        return m_isInstrument ? masterVolumeDisplay(value) : effectMixDisplay(value);
    }
    std::string getOutputControlHint() const {
        return std::string(m_isInstrument ? "Master volume: " : "Effect mix: ") +
            getOutputControlDisplay(getOutputControlValue());
    }

    // Preset & state management
    bool loadPreset(int presetIndex);
    int getNumInternalPresets() const { return m_numInternalPresets.load(std::memory_order_relaxed); }
    bool loadInternalPreset(int presetIndex);
    std::string getInternalPresetName(int presetIndex);
    bool loadPresetFromFile(const std::string& fstPath);
    bool loadNativePresetFromFile(const std::string& path);
    bool importFlexPack(const std::string& path);
    bool savePluginState(std::vector<uint8_t>& outState);
    bool restorePluginState(const uint8_t* data, size_t size);
    bool loadChannelSample(const std::string& path);
    void clearChannelSample();
    std::string getChannelSamplePath() const;
    bool loadDroppedFile(const std::string& path);

    // Parameter access (normalized 0.0 .. 1.0)
    std::string getParamName(int index);
    std::string getParamDisplay(int index);
    double getParamNormalized(int index);
    void setParamNormalized(int index, double normValue, bool fromUI = false);

    // MIDI & Voice control
    void noteOn(int channel, int midiNote, float velocity);
    void noteOff(int channel, int midiNote, float velocity);
    void allNotesOff();
    void setPitchBendSemitones(float semitones);
    int32_t sendMIDIMessage(uint8_t status, uint8_t data1, uint8_t data2);

    // Real-time audio processing
    void processAudio(const float* inL, const float* inR,
                      float* outL, float* outR,
                      int32_t numFrames,
                      double tempoBPM,
                      double projectTimeQuarters,
                      bool isPlaying,
                      double projectTimeSeconds = -std::numeric_limits<double>::infinity(),
                      int32_t numerator = 4, int32_t denominator = 4);

    // Native Cocoa GUI Hosting
    bool attachEditorToNSView(void* parentNSView, int& outWidth, int& outHeight);
    EditorSizeConstraints getEditorSizeConstraints();
    bool resizeEditor(int& width, int& height);
    void detachEditor();
    void idleEditor();
    bool isEditorOpen() const { return m_editorOpen; }
#ifdef _WIN32
    void setEditorFocus(bool focused);
    bool wantsEditorKeyboardInput() const { return m_editorWantsKeyboard.load(std::memory_order_relaxed); }
#endif

    // Callbacks to VST3 Controller / UI
    std::function<void(const std::string&)> onHintChanged;
    std::function<void(int, double)> onParamChangedFromPlug;
    std::function<void(int, int)> onEditorResized;

    // Internal Host callbacks invoked by static trampolines
    intptr_t handleHostDispatcher(intptr_t sender, intptr_t id, intptr_t index, intptr_t value);
    void handleOnParamChanged(intptr_t sender, int32_t index, int32_t value);
    void handleOnHint(intptr_t sender, const char* text);
    intptr_t handleVoiceProcessEvent(intptr_t senderTag, intptr_t eventId, intptr_t eventVal, intptr_t flags);
    void handleHostReleaseVoice(intptr_t senderTag);
    void handleHostKillVoice(intptr_t senderTag, int32_t killHandleBool);
    void handleGetInBuffer(intptr_t sender, intptr_t index, void* ioBuf);
    void handleGetOutBuffer(intptr_t sender, intptr_t index, void* ioBuf);
    void handleLockPlugin();
    void handleUnlockPlugin();
    void* handleGetMixBuffer(int32_t index);
    bool handleLoadSample(intptr_t& handle, char* fileName, const FLWaveFormat* format, int32_t flags);
    void* handleGetSampleData(intptr_t handle, int32_t& frames);
    void handleGetSampleInfo(intptr_t handle, FLSampleInfo* info);
    void handleCloseSample(intptr_t handle);

    const std::string& getActivePluginDir() const { return m_activePluginDir; }
    double getTempoBPM() const { return m_tempoBPM; }

private:
    friend struct FLPluginInstanceTestAccess;

    void updateTransport(double tempo, double quarters, bool playing, double seconds,
                         int32_t numerator, int32_t denominator, int32_t frames);
    intptr_t getTransportTime(intptr_t format, intptr_t value);
    intptr_t ticksToSongTime(intptr_t destination, intptr_t ticks);
    mutable std::recursive_mutex m_mutex;

    void* m_dylibHandle = nullptr;
    void* m_plug = nullptr;
    bool m_isPascalABI = true;
    bool m_isGenerator = true;
    bool m_isInstrument = true;
    int m_currentPluginIndex = -1;
    int m_currentPresetIndex = -1;
    std::atomic<int> m_numInternalPresets{0};
    std::string m_pluginName;
    std::string m_activePluginDir;
    std::string m_activePresetDir;
    int m_numParams = 0;
    int32_t m_pluginFlags = FPF_NewVoiceParams;
    int32_t m_midiResolution = FromMIDI_Max;

    double m_sampleRate = 44100.0;
    std::atomic<double> m_masterVolume{kDefaultMasterVolume};
    double m_masterGain = kDefaultWrapperGain;
    double m_masterGainTarget = kDefaultWrapperGain;
    double m_masterGainStep = 0;
    int32_t m_masterGainRamp = 0;
    std::atomic<double> m_effectMix{kDefaultEffectMix};
    double m_currentMix = kDefaultEffectMix;
    double m_mixTarget = kDefaultEffectMix;
    double m_mixStep = 0;
    int32_t m_mixRamp = 0;
    void applyOutputControl(float* outL, float* outR, int32_t frames);
    int32_t m_blockSize = 512;
    double m_tempoBPM = 120.0;
    double m_ppqPos = 0.0;
    bool m_isPlaying = false;
    bool m_transportValid = false;
    double m_songTimeSeconds = 0.0;
    double m_expectedNextPPQ = 0.0;
    double m_runningTimeSeconds = 0.0;
    double m_previousBlockSeconds = 0.0;
    TTimeSigInfo m_timeSignature{16, 4, kNativePPQ};
    bool m_enabled = true;
    float m_pitchBendSemitones = 0.0f;
    bool m_inMIDIIn = false;
    bool m_midiNoteForwarded = false;

    bool m_editorOpen = false;
#ifdef _WIN32
    std::atomic<bool> m_editorWantsKeyboard{false};
#endif
    void* m_parentNSView = nullptr;
    int m_editorWidth = 900;
    int m_editorHeight = 600;
    double m_editorCoordinateScale = 1.0;
    bool m_editorSizeKnown = false;

    // Polyphonic voice pool (fixed addresses for TVoiceParams!)
    static constexpr int kMaxVoices = 64;
    VoiceSlot m_voices[kMaxVoices];
    uint64_t m_voiceCounter = 0;

    // Interleaved audio buffers for Fruity SDK rendering
    std::vector<float> m_inInterleaved;
    std::vector<float> m_outInterleaved;
    std::vector<float> m_dryInterleaved;

    struct HostSample {
        std::string fileName;
        double sampleRate = 44100;
        double requestedRate = 0;
        int32_t channels = 2;
        int32_t format = 0;
        std::array<std::vector<float>, 2> floats;
        std::array<std::vector<int16_t>, 2> integers;
    };
    std::map<intptr_t, HostSample> m_samples;
    intptr_t m_nextSampleHandle = 0;
    intptr_t m_channelSample = 0;
    uint64_t m_sampleLoadSerial = 0;
    uint64_t m_pluginGeneration = 0;
    std::array<float, 16384> m_customShape{};

    // Per-instance Dual-ABI Host tables
    void* m_pascalVmt[72];
    void* m_cppVmt[48];

    struct HostStruct {
        void* vmtPtr;
        int32_t HostVersion;
        int32_t Flags;
        void* AppHandle;
        float* WaveTables[10];
        float* TempBuffers[4];
        int32_t Reserved[30];
        FLPluginInstance* owner;
        uint8_t pad[256];
    } m_pascalHost, m_cppHost;

    void initHostTables();
    void installHostServices();
    static FLPluginInstance* hostOwner(void* host);
    bool selectSampleFile(std::string& path);
    bool decodeSampleFile(const std::string& path, double rate, std::vector<float>& stereo, double& actualRate);
    void initializeChannelState();
    static std::string plainPluginText(const char* text);
    double nativeParamToNormalized(int index, int32_t value);
    intptr_t handlePreviewNote(intptr_t id, intptr_t index, intptr_t value);
    intptr_t handleGetPath(intptr_t index);
    intptr_t handleLocateDataFile(intptr_t value);
    intptr_t handlePackDataFile(intptr_t value);

    // Cached plugin VMT method pointers
    typedef void (*Plug_DestroyObjectFn)(void* self);
    typedef intptr_t (*Plug_DispatcherFn)(void* self, intptr_t id, intptr_t index, intptr_t value);
    typedef void (*Plug_IdleFn)(void* self);
    typedef void (*Plug_SaveRestoreStateFn)(void* self, void* stream, bool save);
    typedef void (*Plug_GetNameFn)(void* self, int32_t section, int32_t index, int32_t value, char* nameBuf);
    typedef int32_t (*Plug_ProcessEventFn)(void* self, int32_t eventId, int32_t eventVal, int32_t flags);
    typedef int32_t (*Plug_ProcessParamFn)(void* self, int32_t index, int32_t value, int32_t recFlags);
    typedef void (*Plug_EffRenderFn)(void* self, float* srcBuf, float* dstBuf, int32_t length);
    typedef void (*Plug_GenRenderFn)(void* self, float* dstBuf, int32_t* length);
    typedef intptr_t (*Plug_TriggerVoiceFn)(void* self, void* voiceParams, intptr_t setTag);
    typedef void (*Plug_VoiceReleaseFn)(void* self, intptr_t handle);
    typedef void (*Plug_VoiceKillFn)(void* self, intptr_t handle);
    typedef int32_t (*Plug_VoiceProcessEventFn)(void* self, intptr_t handle, intptr_t eventId, intptr_t eventVal, intptr_t flags);
    typedef void (*Plug_NewTickFn)(void* self);
    typedef void (*Plug_MIDITickFn)(void* self);
    typedef void (*Plug_MIDIInFn)(void* self, int32_t* msg);

    Plug_DestroyObjectFn m_fnDestroyObject = nullptr;
    Plug_DispatcherFn m_fnDispatcher = nullptr;
    Plug_IdleFn m_fnIdle = nullptr;
    Plug_SaveRestoreStateFn m_fnSaveRestoreState = nullptr;
    Plug_GetNameFn m_fnGetName = nullptr;
    Plug_ProcessEventFn m_fnProcessEvent = nullptr;
    Plug_ProcessParamFn m_fnProcessParam = nullptr;
    Plug_EffRenderFn m_fnEffRender = nullptr;
    Plug_GenRenderFn m_fnGenRender = nullptr;
    Plug_TriggerVoiceFn m_fnTriggerVoice = nullptr;
    Plug_VoiceReleaseFn m_fnVoiceRelease = nullptr;
    Plug_VoiceKillFn m_fnVoiceKill = nullptr;
    Plug_VoiceProcessEventFn m_fnVoiceProcessEvent = nullptr;
    Plug_NewTickFn m_fnNewTick = nullptr;
    Plug_MIDITickFn m_fnMIDITick = nullptr;
    Plug_MIDIInFn m_fnMIDIIn = nullptr;
};

} // namespace flbr
