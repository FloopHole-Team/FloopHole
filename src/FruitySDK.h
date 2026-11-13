#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <vector>
#include <string>
#include <mutex>
#include <cstdio>
#include <limits>
#ifdef _WIN32
#include <objidl.h>
#endif
#include <sys/stat.h>

namespace flbr {

// ============================================================================
// FL Studio 64-bit macOS Fruity Plug-in SDK Constants & Flags
// ============================================================================

// Plugin Info Flags (TFruityPlugInfo.Flags)
constexpr int32_t FPF_Generator       = 1 << 0;   // Plugin is a generator (instrument), not an effect
constexpr int32_t FPF_RenderVoice     = 1 << 1;
constexpr int32_t FPF_UseChanSample   = 1 << 2;
constexpr int32_t FPF_GetChanCustomShape = 1 << 3;
constexpr int32_t FPF_GetChanSample   = 1 << 19;
constexpr int32_t FPF_NewVoiceParams  = 1 << 21;  // Uses floating-point TVoiceParams
constexpr int32_t FPF_WantNewTick     = 1 << 5;   // Calls NewTick on mixing tick
constexpr int32_t FPF_NoProcess       = 1 << 6;
constexpr int32_t FPF_MIDITick        = 1 << 12;
constexpr int32_t FPF_DemoVersion     = 1 << 15;  // Demo flag (cleared by FHD_CheckProdCode)
constexpr int32_t FPF_MacCocoa        = 1 << 28;

// Plugin Dispatcher IDs (TFruityPlug::Dispatcher)
constexpr intptr_t FPD_ShowEditor       = 0;  // Value = 0 to hide, or ((intptr_t)NSView* | 1) to attach
constexpr intptr_t FPD_ProcessMode      = 1;  // Value = process mode flags
constexpr intptr_t FPD_Flush            = 2;  // Flush audio tails / voices
constexpr intptr_t FPD_SetBlockSize     = 3;  // Value = max block size in frames
constexpr intptr_t FPD_SetSampleRate    = 4;  // Value = sample rate in Hz (e.g. 44100, 48000)
constexpr intptr_t FPD_WindowMinMax     = 5; // Index = FLRect*, Value = FLPoint*; return value is unused
constexpr intptr_t FPD_KillAVoice       = 7;
constexpr intptr_t FPD_UseVoiceLevels   = 11;
constexpr intptr_t FPD_SetPreset        = 9;
constexpr intptr_t FPD_SetEnabled       = 11;
constexpr intptr_t FPD_ChanSampleChanged = 10;
constexpr intptr_t FPD_SongPosChanged   = 13;
constexpr intptr_t FPD_SetTimeSig       = 14;
constexpr intptr_t FPD_CollectFile      = 15;
constexpr intptr_t FPD_LoadFile         = 18;
constexpr intptr_t LFT_Generic          = 0;
constexpr intptr_t FPD_RoutingChanged   = 25;
constexpr intptr_t FPD_SetPlaying       = 12;
constexpr intptr_t FPD_GetParamInfo     = 26;
constexpr intptr_t FPD_UseIncreasedMIDIResolution = 50;

// Host Dispatcher IDs (TFruityPlugHost::Dispatcher)
constexpr intptr_t FHD_WantMIDIInput    = 0;
constexpr intptr_t FHD_WantParamTick    = 1;
constexpr intptr_t FHD_WantMIDITick     = 2;
constexpr intptr_t FHD_KillNotes        = 3;
constexpr intptr_t FHD_SetNumPresets    = 9;
constexpr intptr_t FHD_SelectChanSample = 12;
constexpr intptr_t FHD_SetNumParams     = 27;
constexpr intptr_t FHD_PackDataFile     = 28;
constexpr intptr_t FHD_EditorResized    = 2;
constexpr intptr_t FHD_NamesChanged     = 3;
constexpr intptr_t FHD_NoteOn           = 20;
constexpr intptr_t FHD_NoteOff          = 21;
constexpr intptr_t FHD_NoteOnOff         = 56;
constexpr intptr_t FHD_ActivateMIDI     = 23;
constexpr intptr_t FHD_CheckProdCode    = 26;
constexpr intptr_t FHD_LocateDataFile   = 14;
constexpr intptr_t FHD_TicksToTime      = 16;
constexpr intptr_t FHD_GetPath          = 29;
constexpr intptr_t FHD_SetLatency       = 30;
constexpr intptr_t FHD_CallDownloader   = 31;
constexpr intptr_t FHD_GetMixingTime    = 36;
constexpr intptr_t FHD_GetPlaybackTime  = 37;
constexpr intptr_t FHD_GetInstance      = 46;
constexpr intptr_t FHD_GetPlugPresetsPath = 47;
constexpr intptr_t FHD_GetIdleTime      = 48;
constexpr intptr_t FHD_GetPathManager   = 71; // 0x47 -> returns IPathManager**
constexpr intptr_t FHD_GetSharedData    = 86; // 0x56

constexpr intptr_t GP_ProgPath = 0;
constexpr intptr_t GP_ProjDataPath = 1;
constexpr intptr_t GP_PluginPresetPath = 3;
constexpr intptr_t GP_SharedDlls = 7;

// ProcessEvent IDs (FPE_*)
constexpr int32_t FPE_Tempo = 0; // Tempo in float bits in Value, rounded BPM in Flags
constexpr int32_t FPE_MaxPoly = 1; // Host channel voice limit, delivered through ProcessEvent
constexpr int32_t FPE_MIDI_Vol = 3; // MIDI volume in Value, normalized float bits in Flags

// ProcessParam Flags (REC_*)
constexpr int32_t REC_UpdateValue   = 1 << 0; // Write value into plugin
constexpr int32_t REC_GetValue      = 1 << 1; // Read current value from plugin
constexpr int32_t REC_ShowHint      = 1 << 2; // Show parameter hint
constexpr int32_t REC_UpdateControl = 1 << 4; // Repaint GUI control
constexpr int32_t REC_FromMIDI      = 1 << 5; // Value is 0..FromMIDI_Max, result is native
constexpr int32_t FromMIDI_Max      = 1 << 30; // FL Studio 20+ automation resolution
constexpr int32_t PI_Float          = 2; // Native parameter is a bit-cast normalized float
constexpr intptr_t FVH_Null         = -1;
// FL's 78% default wrapper volume uses a nonlinear taper: -5.2 dB, not 0.78 linear.
constexpr float kDefaultWrapperGain = 0.5495408739f;

void hostAddMonoRamp(void* host, const float* source, float* destination, int32_t frames,
                     float left, float right, float* lastLeft, float* lastRight);
void hostAddStereoRamp(void* host, const float* source, float* destination, int32_t frames,
                       float left, float right, float* lastLeft, float* lastRight);

inline float midiNoteToFruityPitch(int midiNote, float bendSemitones = 0.0f) {
    return (static_cast<float>(midiNote - 60) + bendSemitones) * 100.0f;
}

// GetName Sections (FPN_*)
constexpr int32_t FPN_Param      = 0;
constexpr int32_t FPN_ParamValue = 1;
constexpr int32_t FPN_Semitone   = 2;
constexpr int32_t FPN_Patch      = 3;
constexpr int32_t FPN_VoiceLevel = 4;
constexpr int32_t FPN_VoiceLevelHint = 5;
constexpr int32_t FPN_Preset     = 6;

// Voice_ProcessEvent IDs (FVE_*)
constexpr intptr_t FVE_Duration    = 1;
constexpr intptr_t FVE_Color       = 2; // MIDI channel (0..15)
constexpr intptr_t FVE_Velocity    = 3; // Float 0.0..1.0 bit-cast to int32_t
constexpr intptr_t FVE_RelVelocity = 4; // Float 0.0..1.0 bit-cast to int32_t
constexpr intptr_t FVE_ReleaseTime = 5;

// ============================================================================
// Fruity Plug-in SDK Data Structures (Verified against FLEngine_x64 RTTI)
// ============================================================================

#pragma pack(push, 4)
struct TFruityPlugInfo {
    int32_t SDKVersion;      // +0x00 (4 bytes)
    const char* LongName;    // +0x04 (8 bytes)
    const char* ShortName;   // +0x0C (8 bytes)
    int32_t Flags;           // +0x14 (4 bytes)
    int32_t NumParams;       // +0x18 (4 bytes)
    int32_t DefPoly;         // +0x1C (4 bytes)
    int32_t NumOutCtrls;     // +0x20 (4 bytes)
    int32_t NumOutVoices;    // +0x24 (4 bytes)
    int32_t Reserved[30];    // +0x28 (120 bytes) -> Total 160 (0xA0) bytes
};
#pragma pack(pop)

// C++ ABI plugins (e.g. Morphine, Toxic Biohazard, GMS) may use standard 8-byte alignment
struct TFruityPlugInfoAligned8 {
    int32_t SDKVersion;
    int32_t _pad0;
    const char* LongName;
    const char* ShortName;
    int32_t Flags;
    int32_t NumParams;
    int32_t DefPoly;
    int32_t NumOutCtrls;
    int32_t NumOutVoices;
    int32_t Reserved[30];
};

struct TLevelParams {
    float Pan;   // -1.0 .. +1.0 (0.0 = center)
    float Vol;   //  0.0 .. +1.0
    float Pitch; // Init: absolute note cents; Final: cents relative to middle C.
    float FCut;  // 0.0 .. 1.0
    float FRes;  // 0.0 .. 1.0
};

struct TVoiceParams {
    TLevelParams InitLevels;  // +0x00 (20 bytes)
    TLevelParams FinalLevels; // +0x14 (20 bytes) -> Total 40 (0x28) bytes
};

struct TLevelParamsOld {
    int32_t Pan;
    float Vol;
    int32_t Pitch;
    float FCut;
    float FRes;
};

struct TVoiceParamsOld {
    TLevelParamsOld InitLevels;
    TLevelParamsOld FinalLevels;
};

static_assert(sizeof(TVoiceParamsOld) == sizeof(TVoiceParams));

struct TAudioIOInfo {
    float* Buffer;       // +0x00
    int32_t ChanIndex;   // +0x08
    int32_t Flags;       // +0x0C
    int32_t Reserved[4]; // +0x10 -> Total 32 (0x20) bytes
};

struct TFPTime {
    double t;
    double t2;
};

struct FLRect {
    int32_t left, top, right, bottom;
};

struct FLPoint {
    int32_t x, y;
};

struct TSongTime {
    int32_t Bar, Step, Tick;
};

struct TTimeSigInfo {
    int32_t StepsPerBar, StepsPerBeat, PPQ;
};

constexpr int32_t GT_Beats = 0;
constexpr int32_t GT_AbsoluteMS = 1;
constexpr int32_t GT_RunningMS = 2;
constexpr int32_t GT_MSSinceStart = 3;
constexpr int32_t GT_Ticks = 4;
constexpr uint32_t GT_LocalTime = 1u << 31;
constexpr int32_t kNativePPQ = 96;

constexpr int FHLS_ShowDialog = 1;
constexpr int FHLS_ForceReload = 2;
constexpr int FHLS_GetName = 4;
constexpr int FHLS_NoResampling = 8;
constexpr intptr_t LFT_DroppedFile = 2;

#pragma pack(push, 1)
struct FLWaveFormat {
    uint16_t tag;
    uint16_t channels;
    uint32_t sampleRate;
    uint32_t bytesPerSecond;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    uint16_t extraSize;
};

struct FLSampleInfo {
    int32_t size;
    void* data;
    int32_t length;
    int32_t solidLength;
    int32_t loopStart;
    int32_t loopEnd;
    double sampleRateConversion;
    int32_t numRegions;
    float numBeats;
    float tempo;
    int32_t channels;
    int32_t format;
    int32_t reserved[13];
};
#pragma pack(pop)

// ============================================================================
// COM IStream Implementation for TFruityPlug::SaveRestoreState
// ============================================================================

class MemoryFruityStream {
public:
    void* vtablePtr;
    void* vtable[16];
    std::vector<uint8_t> data;
    size_t pos = 0;

    MemoryFruityStream() {
        vtable[0]  = (void*)&QueryInterface;
        vtable[1]  = (void*)&AddRef;
        vtable[2]  = (void*)&Release;
        vtable[3]  = (void*)&Read;       // +0x18
        vtable[4]  = (void*)&Write;      // +0x20
        vtable[5]  = (void*)&Seek;       // +0x28
        vtable[6]  = (void*)&SetSize;    // +0x30
        vtable[7]  = (void*)&CopyTo;     // +0x38
        vtable[8]  = (void*)&Commit;     // +0x40
        vtable[9]  = (void*)&Revert;     // +0x48
        vtable[10] = (void*)&LockRegion; // +0x50
        vtable[11] = (void*)&UnlockRegion;// +0x58
        vtable[12] = (void*)&Stat;       // +0x60
        vtable[13] = (void*)&Clone;      // +0x68
        vtablePtr = vtable;
    }

    static int32_t QueryInterface(void* self, const void* riid, void** ppv) {
        if (!ppv) return static_cast<int32_t>(0x80004003);
        *ppv = nullptr;
        if (!riid) return static_cast<int32_t>(0x80004003);
        constexpr uint8_t unknown[] = {0,0,0,0,0,0,0,0,0xC0,0,0,0,0,0,0,0x46};
        constexpr uint8_t stream[] = {0x0C,0,0,0,0,0,0,0,0xC0,0,0,0,0,0,0,0x46};
        constexpr uint8_t sequential[] = {0x30,0x3A,0x73,0x0C,0x1C,0x2A,0xCE,0x11,0xAD,0xE5,0,0xAA,0,0x44,0x77,0x3D};
        if (memcmp(riid, unknown, 16) && memcmp(riid, stream, 16) && memcmp(riid, sequential, 16))
            return static_cast<int32_t>(0x80004002);
        *ppv = self;
        return 0;
    }
    static uint32_t AddRef(void* self) { return 1; }
    static uint32_t Release(void* self) { return 1; }

    static int32_t Read(void* self, void* pv, uint32_t cb, uint32_t* pcbRead) {
        auto* s = reinterpret_cast<MemoryFruityStream*>(self);
        if (pcbRead) *pcbRead = 0;
        if (cb == 0) return 0;
        if (!pv) return static_cast<int32_t>(0x80030009);
        size_t avail = (s->pos < s->data.size()) ? (s->data.size() - s->pos) : 0;
        size_t toRead = (cb < avail) ? cb : avail;
        if (toRead > 0) {
            memcpy(pv, s->data.data() + s->pos, toRead);
            s->pos += toRead;
        }
        if (pcbRead) *pcbRead = (uint32_t)toRead;
        return (toRead == cb) ? 0 : 1; // S_OK or S_FALSE
    }

    static int32_t Write(void* self, const void* pv, uint32_t cb, uint32_t* pcbWritten) {
        auto* s = reinterpret_cast<MemoryFruityStream*>(self);
        if (pcbWritten) *pcbWritten = 0;
        if (cb == 0) return 0;
        if (!pv) return static_cast<int32_t>(0x80030009);
        if (s->pos > s->data.max_size() || cb > s->data.max_size() - s->pos)
            return static_cast<int32_t>(0x80030070);
        if (s->pos + cb > s->data.size()) {
            s->data.resize(s->pos + cb);
        }
        memcpy(s->data.data() + s->pos, pv, cb);
        s->pos += cb;
        if (pcbWritten) *pcbWritten = cb;
        return 0; // S_OK
    }

    static int32_t Seek(void* self, int64_t dlibMove, int32_t dwOrigin, uint64_t* plibNewPosition) {
        auto* s = reinterpret_cast<MemoryFruityStream*>(self);
        int64_t base = 0;
        if (dwOrigin == 1) base = static_cast<int64_t>(s->pos);
        else if (dwOrigin == 2) base = static_cast<int64_t>(s->data.size());
        else if (dwOrigin != 0) return static_cast<int32_t>(0x80030001);
        if ((dlibMove < 0 && dlibMove < -base) ||
            (dlibMove > 0 && base > std::numeric_limits<int64_t>::max() - dlibMove))
            return static_cast<int32_t>(0x80030019);
        int64_t newPos = base + dlibMove;
        s->pos = (size_t)newPos;
        if (plibNewPosition) *plibNewPosition = (uint64_t)s->pos;
        return 0; // S_OK
    }

    static int32_t SetSize(void* self, uint64_t libNewSize) {
        auto* s = reinterpret_cast<MemoryFruityStream*>(self);
        if (libNewSize > s->data.max_size()) return static_cast<int32_t>(0x80030070);
        s->data.resize((size_t)libNewSize);
        return 0;
    }
    static int32_t CopyTo(void*, void*, uint64_t, uint64_t* read, uint64_t* written) {
        if (read) *read = 0;
        if (written) *written = 0;
        return static_cast<int32_t>(0x80004001);
    }
    static int32_t Commit(void*, uint32_t) { return 0; }
    static int32_t Revert(void*) { return static_cast<int32_t>(0x80004001); }
    static int32_t LockRegion(void*, uint64_t, uint64_t, uint32_t) { return static_cast<int32_t>(0x80004001); }
    static int32_t UnlockRegion(void*, uint64_t, uint64_t, uint32_t) { return static_cast<int32_t>(0x80004001); }
    static int32_t Stat(void* self, void* result, uint32_t) {
#ifdef _WIN32
        if (!result) return static_cast<int32_t>(0x80030009);
        auto* stats = static_cast<STATSTG*>(result);
        *stats = {};
        stats->type = STGTY_STREAM;
        stats->cbSize.QuadPart = static_cast<MemoryFruityStream*>(self)->data.size();
        stats->grfMode = STGM_READWRITE;
        return 0;
#else
        return static_cast<int32_t>(0x80004001);
#endif
    }
    static int32_t Clone(void*, void** result) {
        if (result) *result = nullptr;
        return static_cast<int32_t>(0x80004001);
    }
};

// ============================================================================
// FL Studio .fst Preset Parser (Extracts Event 213 / 0xD5 Plugin State Chunk)
// ============================================================================

inline bool ExtractFstPluginState(const uint8_t* fstBytes, size_t fstSize,
                                  std::vector<uint8_t>& outState,
                                  std::string* outPluginName = nullptr) {
    if (!fstBytes || fstSize < 22) return false;
    if (memcmp(fstBytes, "FLhd", 4) != 0) return false;
    uint32_t hdrLen = 0;
    memcpy(&hdrLen, fstBytes + 4, 4);
    if (hdrLen > fstSize - 8) return false;
    size_t pos = 8 + hdrLen;
    if (fstSize - pos < 8 || memcmp(fstBytes + pos, "FLdt", 4) != 0) return false;
    uint32_t dataLen = 0;
    memcpy(&dataLen, fstBytes + pos + 4, 4);
    pos += 8;
    if (dataLen > fstSize - pos) return false;
    const size_t end = pos + dataLen;

    std::vector<uint8_t> state;
    std::string pluginName;
    bool found = false;
    while (pos < end) {
        uint8_t ev = fstBytes[pos++];
        if (ev < 64) {
            if (end - pos < 1) return false;
            pos += 1;
        } else if (ev < 128) {
            if (end - pos < 2) return false;
            pos += 2;
        } else if (ev < 192) {
            if (end - pos < 4) return false;
            pos += 4;
        } else {
            uint32_t len = 0;
            int shift = 0;
            for (;;) {
                if (pos == end || shift >= 32) return false;
                uint8_t b = fstBytes[pos++];
                if ((b & 0x7F) > (std::numeric_limits<uint32_t>::max() >> shift)) return false;
                len |= static_cast<uint32_t>(b & 0x7F) << shift;
                if ((b & 0x80) == 0) break;
                shift += 7;
            }
            if (len > end - pos) return false;
            if (ev == 201 && outPluginName) { // FLP_PluginName (0xC9)
                size_t slen = strnlen((const char*)(fstBytes + pos), len);
                pluginName.assign((const char*)(fstBytes + pos), slen);
            } else if (ev == 213) { // FLP_PluginParams (0xD5)
                state.assign(fstBytes + pos, fstBytes + pos + len);
                found = true;
            }
            pos += len;
        }
    }
    if (!found || state.empty()) return false;
    outState = std::move(state);
    if (outPluginName) *outPluginName = std::move(pluginName);
    return true;
}

} // namespace flbr
