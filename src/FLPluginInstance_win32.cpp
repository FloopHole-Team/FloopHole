#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "FLPluginInstance.h"
#include "FLPathManager.h"
#include "Win32NewtoneDrag.h"
#include <stdio.h>
#include <cmath>
#include <algorithm>
#include <sys/stat.h>
#include <filesystem>

namespace flbr {

// ============================================================================
// Global Shared WaveTables, TempBuffers, and IPathManager (Win64)
// ============================================================================

static float gSharedWaveTables[10][16384];
static float gSharedTempBuffers[4][8192 * 2];
static std::once_flag gInitTablesOnce;
static thread_local FLPluginInstance* gCurrentLoadingInstance = nullptr;
static constexpr int32_t kWindowsHostVersion = 24002099;

static bool traceHostEnabled() {
    static const bool enabled = GetEnvironmentVariableA("FLBR_TRACE_HOST", nullptr, 0) != 0;
    return enabled;
}

static void initSharedTables() {
    for (int w = 0; w < 10; ++w) {
        for (int s = 0; s < 16384; ++s) {
            double ph = (double)s / 16384.0;
            if (w == 0) {
                gSharedWaveTables[w][s] = (float)std::sin(ph * 2.0 * 3.14159265358979323846);
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
    if (traceHostEnabled()) {
        fprintf(stderr, "host=%p sender=%lld id=%lld index=%lld value=0x%llx\n",
                self, (long long)sender, (long long)id, (long long)index,
                (unsigned long long)value);
        fflush(stderr);
    }
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

static intptr_t Trampoline_GetMixBuffer(void* self, int32_t index) {
    if (auto* inst = getOwner(self)) return reinterpret_cast<intptr_t>(inst->handleGetMixBuffer(index));
    return 0;
}

static void Trampoline_LockPlugin(void* self, intptr_t) {
    if (auto* inst = getOwner(self)) inst->handleLockPlugin();
}

static void Trampoline_UnlockPlugin(void* self, intptr_t) {
    if (auto* inst = getOwner(self)) inst->handleUnlockPlugin();
}

static intptr_t Trampoline_Noop(void*, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t) {
    return 0;
}

// ============================================================================
// FLPluginInstance Implementation (Win64)
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

    for (int i = 0; i < 72; ++i) m_pascalVmt[i] = (void*)Trampoline_Noop;
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
    m_pascalHost.HostVersion = kWindowsHostVersion;
    m_pascalHost.owner = this;

    m_cppHost.vmtPtr = m_cppVmt;
    m_cppHost.HostVersion = kWindowsHostVersion;
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
    wchar_t file[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = static_cast<HWND>(m_parentNSView);
    dialog.lpstrTitle = L"Load audio sample";
    dialog.lpstrFilter = L"Audio files\0*.wav;*.aif;*.aiff;*.mp3;*.flac;*.ogg;*.m4a;*.wma\0All files\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = sizeof(file) / sizeof(file[0]);
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) {
        const DWORD error = CommDlgExtendedError();
        if (error) fprintf(stderr, "FloopHole: sample picker failed (error=%lu)\n", static_cast<unsigned long>(error));
        return false;
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, file, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        fprintf(stderr, "FloopHole: cannot convert selected sample filename\n");
        return false;
    }
    std::vector<char> utf8(size);
    WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8.data(), size, nullptr, nullptr);
    path = utf8.data();
    return true;
}

bool FLPluginInstance::decodeSampleFile(const std::string& path, double rate,
                                       std::vector<float>& stereo, double& actualRate) {
    using Microsoft::WRL::ComPtr;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "FloopHole: cannot initialize sample decoder COM (HRESULT=%lx)\n", static_cast<unsigned long>(com));
        return false;
    }
    HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    const bool started = SUCCEEDED(result);
    {
        ComPtr<IMFSourceReader> reader;
        ComPtr<IMFMediaType> source, target;
        const auto wide = std::filesystem::u8path(path).wstring();
        if (SUCCEEDED(result)) result = MFCreateSourceReaderFromURL(wide.c_str(), nullptr, &reader);
        if (SUCCEEDED(result)) result = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &source);
        UINT32 sourceRate = 0;
        if (SUCCEEDED(result)) result = source->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sourceRate);
        actualRate = rate > 0 ? rate : sourceRate;
        if (SUCCEEDED(result)) result = MFCreateMediaType(&target);
        if (SUCCEEDED(result)) result = target->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(result)) result = target->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (SUCCEEDED(result)) result = target->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        if (SUCCEEDED(result)) result = target->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        if (SUCCEEDED(result)) result = target->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(actualRate));
        if (SUCCEEDED(result)) result = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, target.Get());
        while (SUCCEEDED(result)) {
            DWORD flags = 0;
            ComPtr<IMFSample> sample;
            result = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample);
            if (SUCCEEDED(result) && (flags & MF_SOURCE_READERF_ERROR)) result = E_FAIL;
            if (FAILED(result)) break;
            if (!sample) {
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
                continue;
            }
            ComPtr<IMFMediaBuffer> buffer;
            result = sample->ConvertToContiguousBuffer(&buffer);
            BYTE* bytes = nullptr;
            DWORD count = 0;
            if (SUCCEEDED(result)) result = buffer->Lock(&bytes, nullptr, &count);
            if (SUCCEEDED(result)) {
                if (count % (2 * sizeof(float)) || stereo.size() / 2 + count / (2 * sizeof(float)) > INT32_MAX)
                    result = E_INVALIDARG;
                else {
                    const auto* values = reinterpret_cast<const float*>(bytes);
                    stereo.insert(stereo.end(), values, values + count / sizeof(float));
                }
                buffer->Unlock();
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
    }
    if (started) MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    if (FAILED(result)) {
        fprintf(stderr, "FloopHole: cannot decode audio sample %s (HRESULT=%lx)\n",
                path.c_str(), static_cast<unsigned long>(result));
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
        gCurrentLoadingInstance = this;
        destroyFn(plugPtr);
        gCurrentLoadingInstance = nullptr;
    }
    m_samples.clear();
    m_channelSample = 0;
    m_parentNSView = nullptr;
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

    // Preload companion engine_x64.dll in plugin folder if present
    std::string engineDll = entry.pluginDir + "engine_x64.dll";
    if (GetFileAttributesW(std::filesystem::u8path(engineDll).c_str()) != INVALID_FILE_ATTRIBUTES) {
        LoadLibraryExW(std::filesystem::u8path(engineDll).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    HMODULE hDll = LoadLibraryExW(std::filesystem::u8path(entry.dylibPath).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hDll) {
        gCurrentLoadingInstance = nullptr;
        return false;
    }
    m_dylibHandle = (void*)hDll;

    typedef void* (*CreatePlugInstanceFn)(void* Host, intptr_t Tag);
    auto createFn = (CreatePlugInstanceFn)GetProcAddress(hDll, "CreatePlugInstance");
    if (!createFn) {
        gCurrentLoadingInstance = nullptr;
        return false;
    }

    // On Windows x64, both Delphi 64-bit (with virtual; stdcall; methods) and C++ x64
    // place the virtual method table at index 0 (+0x00) of the vtable pointer.
    // Detect if the plugin uses a FreePascal/Delphi negative-offset VMT or standard index-0 vtable.
    m_isPascalABI = false;
    void* hostPtr = (void*)&m_cppHost;

    if (traceHostEnabled()) {
        fprintf(stderr, "CreatePlugInstance: %s host=%p vtable=%p\n",
                entry.dylibPath.c_str(), hostPtr, m_cppHost.vmtPtr);
        fflush(stderr);
    }
    void* plug = createFn(hostPtr, 1);
    if (!plug) {
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
    // Check if vmt[0] looks like a small integer instance size (FreePascal TObject VMT) or a code pointer
    bool plugUsesOffset25 = ((uintptr_t)vmt[0] < 0x100000);
    int base = plugUsesOffset25 ? 25 : 0;
    void* abstractErr = vmt[base + 18];

    auto getMethod = [&](int slot) -> void* {
        void* fn = vmt[base + slot];
        if (!fn || (uintptr_t)fn < 0x10000) return nullptr;
        if (fn == abstractErr && slot != 18) return nullptr;
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

    if (m_fnDispatcher) {
        m_fnDispatcher(m_plug, FPD_SetSampleRate, 0, (intptr_t)m_sampleRate);
        m_fnDispatcher(m_plug, FPD_SetBlockSize, 0, (intptr_t)m_blockSize);
        m_fnDispatcher(m_plug, FPD_RoutingChanged, 0, 0);
        m_fnDispatcher(m_plug, FPD_ProcessMode, 0, 1);
    }

    if (!entry.presets.empty() && entry.presets[0].isDefault()) {
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
    char nameBuf[256] = {0};
    m_fnGetName(m_plug, FPN_Param, index, 0, nameBuf);
    return plainPluginText(nameBuf);
}

std::string FLPluginInstance::getParamDisplay(int index) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnGetName || !m_fnProcessParam || index < 0 || index >= m_numParams) return "";
    int32_t curVal = m_fnProcessParam(m_plug, index, 0, REC_GetValue);
    char valBuf[256] = {0};
    m_fnGetName(m_plug, FPN_ParamValue, index, curVal, valBuf);
    return plainPluginText(valBuf);
}

void FLPluginInstance::noteOn(int channel, int midiNote, float velocity) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug) return;

    uint8_t vel7 = (uint8_t)std::clamp((int)std::lround(velocity * 127.0f), 1, 127);
    if (sendMIDIMessage((uint8_t)(0x90 | (channel & 0x0F)), (uint8_t)(midiNote & 0x7F), vel7) == -1) return;

    if (!m_fnTriggerVoice) return;

    int chosen = -1;
    for (int i = 0; i < kMaxVoices; ++i) {
        if (!m_voices[i].active) {
            chosen = i;
            break;
        }
    }
    if (chosen < 0) {
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

    intptr_t hostTag = (intptr_t)(chosen + 1);
    slot.legacyParams = {
        {0, slot.velocity, static_cast<int32_t>(std::lround(initPitchCents)), 0.0f, 0.0f},
        {0, slot.velocity, static_cast<int32_t>(std::lround(finalPitchCents)), 0.0f, 0.0f}
    };
    void* voiceParams = (m_pluginFlags & FPF_NewVoiceParams) ?
        static_cast<void*>(&slot.params) : static_cast<void*>(&slot.legacyParams);
    slot.plugVoiceHandle = m_fnTriggerVoice(m_plug, voiceParams, hostTag);
    if (slot.plugVoiceHandle == FVH_Null) {
        slot.active = false;
    }
}

void FLPluginInstance::noteOff(int channel, int midiNote, float velocity) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug) return;

    uint8_t vel7 = (uint8_t)std::clamp((int)std::lround(velocity * 127.0f), 0, 127);
    if (sendMIDIMessage((uint8_t)(0x80 | (channel & 0x0F)), (uint8_t)(midiNote & 0x7F), vel7) == -1) return;

    for (int i = 0; i < kMaxVoices; ++i) {
        if (m_voices[i].active && !m_voices[i].released && m_voices[i].midiNote == midiNote &&
            (channel < 0 || m_voices[i].channel == (channel & 0x0F))) {
            m_voices[i].released = true;
            m_voices[i].relVelocity = std::clamp(velocity, 0.0f, 1.0f);
            if (m_voices[i].plugVoiceHandle != FVH_Null && m_fnVoiceRelease) {
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

    if (numFrames > m_blockSize) {
        setBlockSize(numFrames);
    }

    updateTransport(tempoBPM, projectTimeQuarters, isPlaying, projectTimeSeconds,
                    numerator, denominator, numFrames);

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

bool FLPluginInstance::attachEditorToNSView(void* parentHwnd, int& outWidth, int& outHeight) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnDispatcher || !parentHwnd) return false;

    m_parentNSView = parentHwnd;
    HWND hParent = (HWND)parentHwnd;

    // On Windows, FPD_ShowEditor takes the parent HWND directly and sets m_plug->EditorHandle (+0x18)
    m_fnDispatcher(m_plug, FPD_ShowEditor, 0, (intptr_t)hParent);
    m_editorOpen = true;

    outWidth = 900;
    outHeight = 600;
    HWND hEditor = *(HWND*)((uint8_t*)m_plug + 0x18);
    if (!hEditor || !IsWindow(hEditor)) {
        hEditor = GetWindow(hParent, GW_CHILD);
    }
    if (m_pluginName == "Newtone" && !newtoneDrag::enable(hEditor)) {
        fprintf(stderr, "FloopHole: cannot enable Newtone file drag export: unsupported native editor metadata\n");
        if (onHintChanged) onHintChanged("Newtone file drag export is unavailable for this native editor version.");
    }
    if (hEditor && IsWindow(hEditor)) {
        RECT rc{};
        if (GetWindowRect(hEditor, &rc)) {
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            if (w > 0 && h > 0) {
                outWidth = w;
                outHeight = h;
                SetWindowPos(hEditor, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    m_editorWidth = outWidth;
    m_editorHeight = outHeight;
    return true;
}

bool FLPluginInstance::resizeEditor(int& width, int& height) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_editorOpen || !m_parentNSView) {
        fprintf(stderr, "FloopHole: cannot resize an unattached native editor\n");
        return false;
    }
    getEditorSizeConstraints().constrain(width, height);
    HWND editor = *(HWND*)((uint8_t*)m_plug + 0x18);
    if (!editor || !IsWindow(editor)) editor = GetWindow((HWND)m_parentNSView, GW_CHILD);
    RECT rectangle{};
    if (!editor || !IsWindow(editor) ||
        !SetWindowPos(editor, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE) ||
        !GetWindowRect(editor, &rectangle)) {
        fprintf(stderr, "FloopHole: could not resize %s (Windows error %lu)\n", m_pluginName.c_str(), GetLastError());
        return false;
    }
    width = rectangle.right - rectangle.left;
    height = rectangle.bottom - rectangle.top;
    m_editorWidth = width;
    m_editorHeight = height;
    return true;
}

void FLPluginInstance::detachEditor() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnDispatcher || !m_editorOpen) return;
    m_fnDispatcher(m_plug, FPD_ShowEditor, 0, 0);
    m_editorOpen = false;
}

void FLPluginInstance::idleEditor() {
    std::unique_lock<std::recursive_mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !m_plug || !m_editorOpen) return;
    if (m_fnIdle) {
        m_fnIdle(m_plug);
    }
    if (m_parentNSView) {
        HWND hParent = (HWND)m_parentNSView;
        HWND hEditor = *(HWND*)((uint8_t*)m_plug + 0x18);
        if (!hEditor || !IsWindow(hEditor)) {
            hEditor = GetWindow(hParent, GW_CHILD);
        }
        if (hEditor && IsWindow(hEditor)) {
            RECT rc{};
            if (GetWindowRect(hEditor, &rc)) {
                int w = rc.right - rc.left;
                int h = rc.bottom - rc.top;
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
}

intptr_t FLPluginInstance::handleHostDispatcher(intptr_t sender, intptr_t id, intptr_t index, intptr_t value) {
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
                *pFlags &= ~0x8000;
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
            return 1;
        default:
            return 0;
    }
}

void FLPluginInstance::handleOnHint(intptr_t sender, const char* text) {
    if (!text || !onHintChanged) return;
    if (*text && static_cast<uint8_t>(*text) < 32) {
        ++text;
    }
    onHintChanged(plainPluginText(text));
}

intptr_t FLPluginInstance::handleVoiceProcessEvent(intptr_t senderTag, intptr_t eventId, intptr_t eventVal, intptr_t flags) {
    int slotIdx = (int)senderTag - 1;
    const VoiceSlot* slot = (slotIdx >= 0 && slotIdx < kMaxVoices && m_voices[slotIdx].active)
                            ? &m_voices[slotIdx]
                            : nullptr;
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
            float v = slot ? slot->velocity : 0.8f;
            int32_t bits = 0;
            memcpy(&bits, &v, sizeof(float));
            return (intptr_t)(uint32_t)bits;
        }
        case FVE_RelVelocity: {
            float v = slot ? slot->relVelocity : 0.5f;
            int32_t bits = 0;
            memcpy(&bits, &v, sizeof(float));
            return (intptr_t)(uint32_t)bits;
        }
        default:
            return 0;
    }
}

void FLPluginInstance::handleHostKillVoice(intptr_t senderTag, int32_t killHandleBool) {
    int slotIdx = (int)senderTag - 1;
    if (slotIdx >= 0 && slotIdx < kMaxVoices && m_voices[slotIdx].active) {
        intptr_t vh = m_voices[slotIdx].plugVoiceHandle;
        m_voices[slotIdx].active = false;
        m_voices[slotIdx].released = false;
        m_voices[slotIdx].plugVoiceHandle = FVH_Null;
        if (killHandleBool && vh != FVH_Null && m_plug && m_fnVoiceKill) {
            m_fnVoiceKill(m_plug, vh);
        }
    }
}

void FLPluginInstance::handleGetInBuffer(intptr_t sender, intptr_t index, void* ioBuf) {
    auto* info = reinterpret_cast<TAudioIOInfo*>(ioBuf);
    if (!info) return;
    info->Buffer = m_inInterleaved.data();
    info->ChanIndex = 0;
    info->Flags = 0;
}

void FLPluginInstance::handleGetOutBuffer(intptr_t sender, intptr_t index, void* ioBuf) {
    auto* info = reinterpret_cast<TAudioIOInfo*>(ioBuf);
    if (!info) return;
    info->Buffer = m_outInterleaved.data();
    info->ChanIndex = 0;
    info->Flags = 0;
}

void FLPluginInstance::handleLockPlugin() { m_mutex.lock(); }
void FLPluginInstance::handleUnlockPlugin() { m_mutex.unlock(); }

} // namespace flbr

#endif // _WIN32
