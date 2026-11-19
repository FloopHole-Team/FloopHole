#include "FLPluginInstance.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include "FLPathManager.h"

namespace flbr {
namespace fs = std::filesystem;

std::string FLPluginInstance::preparePresetSaveDirectory() const {
    if (!isLoaded()) return {};
    auto& scanner = FLPluginScanner::getInstance();
    const auto plugins = scanner.getPlugins();
    const int index = getCurrentPluginIndex();
    if (index < 0 || static_cast<size_t>(index) >= plugins.size()) return {};
    const auto path = scanner.getPluginPresetPath(plugins[index], PI_User);
    if (path.empty()) return {};
    std::error_code error;
    fs::create_directories(fs::u8path(path), error);
    if (error) {
        fprintf(stderr, "FloopHole: cannot create preset directory %s: %s\n", path.c_str(), error.message().c_str());
        return {};
    }
    return path;
}

bool FLPluginInstance::savePresetToFile(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto destination = fs::u8path(path);
    auto extension = destination.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; });
    if (!destination.is_absolute() || extension != ".fst" || m_currentPluginIndex < 0) return false;

    std::vector<uint8_t> state;
    if (!savePluginState(state) || state.empty()) return false;

    // Native FST header (format 0x30), followed by plugin name and state events.
    std::vector<uint8_t> bytes{'F','L','h','d', 6,0,0,0, 0x30,0,1,0,96,0, 'F','L','d','t', 0,0,0,0};
    const auto eventHeader = [&](uint8_t id, uint32_t length) {
        bytes.push_back(id);
        while (length >= 128) {
            bytes.push_back(static_cast<uint8_t>(length | 0x80));
            length >>= 7;
        }
        bytes.push_back(static_cast<uint8_t>(length));
    };
    if (state.size() > UINT32_MAX - m_pluginName.size() - 13) return false;
    eventHeader(201, static_cast<uint32_t>(m_pluginName.size() + 1));
    bytes.insert(bytes.end(), m_pluginName.begin(), m_pluginName.end());
    bytes.push_back(0);
    eventHeader(213, static_cast<uint32_t>(state.size()));
    bytes.insert(bytes.end(), state.begin(), state.end());
    const auto dataLength = static_cast<uint32_t>(bytes.size() - 22);
    for (int i = 0; i < 4; ++i) bytes[18 + i] = static_cast<uint8_t>(dataLength >> (8 * i));

    // Stage beside the destination so a failed write cannot truncate an existing preset.
    const auto staging = destination.parent_path() / (".floophole-save-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    if (!fs::create_directory(staging, error)) {
        fprintf(stderr, "FloopHole: cannot stage preset %s: %s\n", path.c_str(),
                error ? error.message().c_str() : "temporary directory already exists");
        return false;
    }
    const auto temporary = staging / "preset.tmp";
    std::ofstream output(temporary, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (output) fs::rename(temporary, destination, error);
    const bool saved = output && !error;
    if (!saved) {
        fprintf(stderr, "FloopHole: cannot write preset %s: %s\n", path.c_str(),
                error ? error.message().c_str() : "stream write failed");
    }
    std::error_code cleanupError;
    fs::remove(temporary, cleanupError);
    fs::remove(staging, cleanupError);
    if (!saved) return false;

    m_currentPresetIndex = FLPluginScanner::getInstance().registerSavedPreset(m_currentPluginIndex, path);
    return m_currentPresetIndex >= 0;
}

bool FLPluginInstance::setMasterVolume(double value) {
    if (!std::isfinite(value) || value < 0 || value > 1) {
        std::fprintf(stderr, "FloopHole: invalid master volume %.6g\n", value);
        return false;
    }
    m_masterVolume.store(value, std::memory_order_relaxed);
    return true;
}

bool FLPluginInstance::setEffectMix(double value) {
    if (!std::isfinite(value) || value < 0 || value > 1) {
        std::fprintf(stderr, "FloopHole: invalid effect mix %.6g\n", value);
        return false;
    }
    m_effectMix.store(value, std::memory_order_relaxed);
    return true;
}

void FLPluginInstance::applyOutputControl(float* outL, float* outR, int32_t frames) {
    if (!m_isInstrument) {
        const double target = getEffectMix();
        if (target != m_mixTarget) {
            m_mixTarget = target;
            m_mixRamp = std::max(1, static_cast<int32_t>(std::lround(m_sampleRate * 0.01)));
            m_mixStep = (target - m_currentMix) / m_mixRamp;
        }
        for (int32_t i = 0; i < frames; ++i) {
            if (m_mixRamp > 0) {
                m_currentMix += m_mixStep;
                if (--m_mixRamp == 0) m_currentMix = m_mixTarget;
            }
            const auto blend = [&](int channel) {
                const float dry = m_dryInterleaved[i * 2 + channel];
                const float wet = m_outInterleaved[i * 2 + channel];
                return static_cast<float>((std::isfinite(dry) ? dry : 0) * (1 - m_currentMix) +
                    (std::isfinite(wet) ? wet : 0) * m_currentMix);
            };
            if (outL) outL[i] = blend(0);
            if (outR) outR[i] = blend(1);
        }
        return;
    }
    const double target = masterVolumeGain(getMasterVolume());
    if (target != m_masterGainTarget) {
        m_masterGainTarget = target;
        m_masterGainRamp = std::max(1, static_cast<int32_t>(std::lround(m_sampleRate * 0.01)));
        m_masterGainStep = (target - m_masterGain) / m_masterGainRamp;
    }
    for (int32_t i = 0; i < frames; ++i) {
        if (m_masterGainRamp > 0) {
            m_masterGain += m_masterGainStep;
            if (--m_masterGainRamp == 0) m_masterGain = m_masterGainTarget;
        }
        float left = m_outInterleaved[i * 2], right = m_outInterleaved[i * 2 + 1];
        if (!std::isfinite(left)) left = 0;
        if (!std::isfinite(right)) right = 0;
        const float gain = static_cast<float>(m_masterGain);
        if (outL) outL[i] = left * gain;
        if (outR) outR[i] = right * gain;
    }
}

void EditorSizeConstraints::constrain(int& width, int& height) const {
    const auto snapped = [](int value, int minimum, int maximum, int step) {
        const int64_t bounded = std::clamp(value, minimum, maximum);
        const int64_t count = (bounded - minimum + step / 2) / step;
        const int64_t last = (static_cast<int64_t>(maximum) - minimum) / step;
        return static_cast<int>(minimum + std::min(count, last) * step);
    };
    width = snapped(width, minWidth, maxWidth, widthStep);
    height = snapped(height, minHeight, maxHeight, heightStep);
}

EditorSizeConstraints FLPluginInstance::getEditorSizeConstraints() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const EditorSizeConstraints fixed{m_editorWidth, m_editorHeight, m_editorWidth, m_editorHeight};
    if (!m_plug || !m_fnDispatcher) return fixed;
    const double scale = m_editorCoordinateScale;
    FLRect bounds{
        static_cast<int32_t>(std::lround(m_editorWidth * scale)),
        static_cast<int32_t>(std::lround(m_editorHeight * scale)),
        static_cast<int32_t>(std::lround(m_editorWidth * scale)),
        static_cast<int32_t>(std::lround(m_editorHeight * scale))};
    FLPoint snap{1, 1};
    m_fnDispatcher(m_plug, FPD_WindowMinMax, reinterpret_cast<intptr_t>(&bounds), reinterpret_cast<intptr_t>(&snap));
    if (bounds.left < 0 || bounds.top < 0 || bounds.right < 0 || bounds.bottom < 0 || snap.x < 0 || snap.y < 0) {
        fprintf(stderr, "FloopHole: %s returned invalid editor sizing constraints\n", m_pluginName.c_str());
        return fixed;
    }
    const auto maximum = [&](int32_t value) {
        return value ? static_cast<int>(std::floor(value / scale)) : std::numeric_limits<int32_t>::max() - 1024;
    };
    const auto normalize = [&](int32_t minimum, int32_t increment, int& logicalMinimum, int& logicalStep) {
        increment = std::max(1, increment);
        const int64_t pixelsPerPoint = std::lround(scale);
        if (pixelsPerPoint < 1 || std::abs(scale - pixelsPerPoint) > 1e-6) return false;
        const int64_t divisor = std::gcd(pixelsPerPoint, static_cast<int64_t>(increment));
        if (minimum % divisor != 0) return false;
        // Choose the first native snap position representable in whole host points.
        int64_t first = minimum;
        while (first % pixelsPerPoint != 0) first += increment;
        logicalStep = static_cast<int>(increment / divisor);
        logicalMinimum = static_cast<int>(first / pixelsPerPoint);
        if (logicalMinimum == 0) logicalMinimum += logicalStep;
        return true;
    };
    EditorSizeConstraints result{};
    result.maxWidth = maximum(bounds.right);
    result.maxHeight = maximum(bounds.bottom);
    if (!normalize(bounds.left, snap.x, result.minWidth, result.widthStep) ||
        !normalize(bounds.top, snap.y, result.minHeight, result.heightStep)) {
        fprintf(stderr, "FloopHole: %s editor sizing grid cannot be represented in host coordinates\n", m_pluginName.c_str());
        return fixed;
    }
    if (result.maxWidth < result.minWidth || result.maxHeight < result.minHeight) {
        fprintf(stderr, "FloopHole: %s returned reversed editor sizing limits\n", m_pluginName.c_str());
        return fixed;
    }
    return result;
}

std::string FLPluginInstance::plainPluginText(const char* text) {
    // FL's leading hint-icon markers are metadata, not part of the label.
    while (text[0] == '^' && (text[1] == 'a' || text[1] == 'b' || text[1] == 'c')) {
        text += 2;
    }
    return std::string(text);
}

void FLPluginInstance::initializeChannelState() {
    m_transportValid = false;
    setEnabled(m_enabled);
    if (!m_plug || !m_fnProcessEvent) return;
    const float tempo = static_cast<float>(m_tempoBPM);
    int32_t tempoBits = 0;
    memcpy(&tempoBits, &tempo, sizeof(tempoBits));
    m_fnProcessEvent(m_plug, FPE_Tempo, tempoBits, static_cast<int32_t>(std::lround(m_tempoBPM)));
    m_fnProcessEvent(m_plug, FPE_MaxPoly, kMaxVoices, 0);
    // FLEX starts muted without this event; wrapper gain is applied after rendering.
    const float volume = 1.0f;
    int32_t bits;
    memcpy(&bits, &volume, sizeof(bits));
    m_fnProcessEvent(m_plug, FPE_MIDI_Vol, 127, bits);
}

void FLPluginInstance::updateTransport(double tempo, double quarters, bool playing, double seconds,
                                       int32_t numerator, int32_t denominator, int32_t frames) {
    const double previousTempo = m_tempoBPM;
    if (std::isfinite(tempo) && tempo > 0) m_tempoBPM = tempo;
    else fprintf(stderr, "FloopHole: invalid transport tempo; retaining the previous tempo\n");
    if ((!m_transportValid || m_tempoBPM != previousTempo) && m_fnProcessEvent) {
        const float tempoFloat = static_cast<float>(m_tempoBPM);
        int32_t tempoBits = 0;
        memcpy(&tempoBits, &tempoFloat, sizeof(tempoBits));
        m_fnProcessEvent(m_plug, FPE_Tempo, tempoBits, static_cast<int32_t>(std::lround(m_tempoBPM)));
    }
    if (!std::isfinite(quarters)) {
        fprintf(stderr, "FloopHole: invalid musical position; using the predicted transport position\n");
        quarters = m_expectedNextPPQ;
    }
    const bool relocated = !m_transportValid || std::abs(quarters - m_expectedNextPPQ) > 1e-5;
    m_ppqPos = quarters;
    m_songTimeSeconds = std::isfinite(seconds) ? seconds : quarters * 60.0 / m_tempoBPM;
    const bool validSignature = numerator > 0 && denominator > 0 &&
        (denominator & (denominator - 1)) == 0;
    int32_t stepsPerBeat = validSignature ? std::max(4, denominator / 4) : 4;
    int64_t stepsPerBar = validSignature ? static_cast<int64_t>(numerator) * stepsPerBeat * 4 / denominator : 16;
    if (!validSignature || stepsPerBar > std::numeric_limits<int32_t>::max()) {
        fprintf(stderr, "FloopHole: invalid transport time signature %d/%d\n", numerator, denominator);
        stepsPerBeat = 4;
        stepsPerBar = 16;
    }
    const TTimeSigInfo signature{static_cast<int32_t>(stepsPerBar), stepsPerBeat, kNativePPQ};
    if (!m_transportValid || signature.StepsPerBar != m_timeSignature.StepsPerBar ||
        signature.StepsPerBeat != m_timeSignature.StepsPerBeat) {
        m_timeSignature = signature;
        if (m_fnDispatcher)
            m_fnDispatcher(m_plug, FPD_SetTimeSig, 0, reinterpret_cast<intptr_t>(&m_timeSignature));
    }
    if ((!m_transportValid || playing != m_isPlaying) && m_fnDispatcher)
        m_fnDispatcher(m_plug, FPD_SetPlaying, 0, playing ? 1 : 0);
    m_isPlaying = playing;
    if (relocated && m_fnDispatcher) m_fnDispatcher(m_plug, FPD_SongPosChanged, 0, 0);
    m_expectedNextPPQ = quarters + (playing ? frames / m_sampleRate * m_tempoBPM / 60.0 : 0.0);
    m_runningTimeSeconds += m_previousBlockSeconds;
    m_previousBlockSeconds = frames / m_sampleRate;
    m_transportValid = true;
}

intptr_t FLPluginInstance::getTransportTime(intptr_t format, intptr_t value) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!value) {
        fprintf(stderr, "FloopHole: missing native transport time destination\n");
        return 0;
    }
    auto& time = *reinterpret_cast<TFPTime*>(value);
    // The caller supplies an optional sample offset in t.
    if (!std::isfinite(time.t)) {
        fprintf(stderr, "FloopHole: invalid native transport sample offset\n");
        return 0;
    }
    const double offset = time.t / m_sampleRate;
    switch (format & 0xff) {
        case GT_Beats: time.t = m_ppqPos + offset * m_tempoBPM / 60.0; break;
        case GT_AbsoluteMS:
        case GT_RunningMS: time.t = (m_songTimeSeconds + offset) * 1000.0; break;
        case GT_MSSinceStart: time.t = (m_runningTimeSeconds + offset) * 1000.0; break;
        case GT_Ticks: time.t = (m_ppqPos + offset * m_tempoBPM / 60.0) * kNativePPQ; break;
        default:
            fprintf(stderr, "FloopHole: unsupported native time format %lld\n", static_cast<long long>(format));
            return 0;
    }
    time.t2 = 0.0;
    return 1;
}

intptr_t FLPluginInstance::ticksToSongTime(intptr_t destination, intptr_t ticks) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!destination) {
        fprintf(stderr, "FloopHole: missing native bar/step/tick destination\n");
        return 0;
    }
    auto& time = *reinterpret_cast<TSongTime*>(destination);
    const double ticksPerStep = static_cast<double>(kNativePPQ) / m_timeSignature.StepsPerBeat;
    const double ticksPerBar = ticksPerStep * m_timeSignature.StepsPerBar;
    const double bar = std::floor(ticks / ticksPerBar);
    const double withinBar = ticks - bar * ticksPerBar;
    time.Bar = static_cast<int32_t>(bar);
    time.Step = static_cast<int32_t>(std::floor(withinBar / ticksPerStep));
    time.Tick = static_cast<int32_t>(withinBar - time.Step * ticksPerStep);
    return 1;
}

void hostAddMonoRamp(void*, const float* source, float* destination, int32_t frames,
                     float left, float right, float* lastLeft, float* lastRight) {
    if (frames <= 0) return;
    const float leftStep = (left - *lastLeft) / frames;
    const float rightStep = (right - *lastRight) / frames;
    for (int32_t i = 0; i < frames; ++i) {
        destination[2 * i] += source[i] * (*lastLeft + leftStep * (i + 1));
        destination[2 * i + 1] += source[i] * (*lastRight + rightStep * (i + 1));
    }
    *lastLeft = left;
    *lastRight = right;
}

void hostAddStereoRamp(void*, const float* source, float* destination, int32_t frames,
                       float left, float right, float* lastLeft, float* lastRight) {
    if (frames <= 0) return;
    const float leftStep = (left - *lastLeft) / frames;
    const float rightStep = (right - *lastRight) / frames;
    for (int32_t i = 0; i < frames; ++i) {
        destination[2 * i] += source[2 * i] * (*lastLeft + leftStep * (i + 1));
        destination[2 * i + 1] += source[2 * i + 1] * (*lastRight + rightStep * (i + 1));
    }
    *lastLeft = left;
    *lastRight = right;
}

void* FLPluginInstance::handleGetMixBuffer(int32_t index) {
    return index == 0 ? m_outInterleaved.data() : nullptr;
}

void FLPluginInstance::handleHostReleaseVoice(intptr_t senderTag) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (senderTag > 0 && senderTag <= kMaxVoices && m_voices[senderTag - 1].active) {
        // Release tails still own the native handle and its parameter storage.
        m_voices[senderTag - 1].released = true;
    }
}

double FLPluginInstance::nativeParamToNormalized(int index, int32_t value) {
    if (m_fnDispatcher && (m_fnDispatcher(m_plug, FPD_GetParamInfo, index, 0) & PI_Float)) {
        float normalized = 0.0f;
        memcpy(&normalized, &value, sizeof(normalized));
        if (!std::isfinite(normalized)) {
            fprintf(stderr, "FloopHole: parameter %d returned a non-finite value\n", index);
            return 0.0;
        }
        return std::clamp(static_cast<double>(normalized), 0.0, 1.0);
    }

    // REC_FromMIDI translates without changing the parameter unless REC_UpdateValue is set.
    auto nativeAt = [&](int32_t midiValue) {
        return m_fnProcessParam(m_plug, index, midiValue, REC_FromMIDI);
    };
    const double first = nativeAt(0);
    const double last = nativeAt(m_midiResolution);
    if (first == last) return 0.0;
    const double linear = std::clamp((value - first) / (last - first), 0.0, 1.0);
    if (nativeAt(static_cast<int32_t>(std::lround(linear * m_midiResolution))) == value) {
        return linear;
    }

    // Invert nonlinear mappings too, without writing temporary values into the plugin.
    int32_t low = 0;
    int32_t high = m_midiResolution;
    while (low < high) {
        const int32_t middle = low + (high - low) / 2;
        const int32_t native = nativeAt(middle);
        if (native == value) return static_cast<double>(middle) / m_midiResolution;
        if ((native < value) == (first < last)) low = middle + 1;
        else high = middle;
    }
    return static_cast<double>(low) / m_midiResolution;
}

double FLPluginInstance::getParamNormalized(int index) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnProcessParam || index < 0 || index >= m_numParams) return 0.0;
    return nativeParamToNormalized(index, m_fnProcessParam(m_plug, index, 0, REC_GetValue));
}

void FLPluginInstance::setParamNormalized(int index, double normValue, bool fromUI) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnProcessParam || index < 0 || index >= m_numParams) return;
    if (!std::isfinite(normValue)) {
        fprintf(stderr, "FloopHole: ignoring non-finite automation for parameter %d\n", index);
        return;
    }
    const int32_t midiValue = static_cast<int32_t>(
        std::lround(std::clamp(normValue, 0.0, 1.0) * m_midiResolution));
    int32_t flags = REC_UpdateValue | REC_FromMIDI;
    if (fromUI || m_editorOpen) flags |= REC_UpdateControl;
    m_fnProcessParam(m_plug, index, midiValue, flags);
}

void FLPluginInstance::handleOnParamChanged(intptr_t sender, int32_t index, int32_t value) {
    double normalized;
    std::function<void(int, double)> callback;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!m_plug || !m_fnProcessParam || !onParamChangedFromPlug || index < 0 || index >= m_numParams) return;
        normalized = nativeParamToNormalized(index, value);
        callback = onParamChangedFromPlug;
    }
    callback(index, normalized);
}

intptr_t FLPluginInstance::handlePackDataFile(intptr_t value) {
    if (!value) {
        fprintf(stderr, "FloopHole: PackDataFile called without a filename\n");
        return reinterpret_cast<intptr_t>("");
    }
    static thread_local std::string result;
    result = reinterpret_cast<const char*>(value);
    std::string clean = result;
    std::replace(clean.begin(), clean.end(), '\\', '/');
    const auto file = std::filesystem::u8path(clean).lexically_normal();
    const auto& scanner = FLPluginScanner::getInstance();
    for (const auto& root : {std::make_pair(scanner.getUserDataPath(), "%FLStudioUserData%"),
                             std::make_pair(scanner.getFLRootPath(), "%FLStudioFactoryData%")}) {
        if (root.first.empty()) continue;
        const auto relative = file.lexically_relative(std::filesystem::u8path(root.first).lexically_normal());
        if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..") {
            result = std::string(root.second) + "/" + relative.generic_u8string();
            break;
        }
    }
    return reinterpret_cast<intptr_t>(result.c_str());
}

intptr_t FLPluginInstance::handleLocateDataFile(intptr_t value) {
    if (!value || !*reinterpret_cast<const char*>(value)) {
        fprintf(stderr, "FloopHole: LocateDataFile called without a filename\n");
        return reinterpret_cast<intptr_t>("");
    }
    // The SDK only promises that this result survives until the next lookup.
    static thread_local std::string result;
    const auto* file = reinterpret_cast<const char*>(value);
    const auto& scanner = FLPluginScanner::getInstance();
    result = scanner.resolveRelativePath(file, m_activePresetDir);
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(result), error))
        result = scanner.resolveRelativePath(file, m_activePluginDir);
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(result), error)) {
        fprintf(stderr, "FloopHole: cannot locate data file %s%s%s\n", file,
                error ? ": " : "", error ? error.message().c_str() : "");
        result.clear();
    }
    return reinterpret_cast<intptr_t>(result.c_str());
}

intptr_t FLPluginInstance::handleGetPath(intptr_t index) {
    const auto& scanner = FLPluginScanner::getInstance();
    switch (index) {
        case GP_ProgPath: return reinterpret_cast<intptr_t>(scanner.getFLRootPath().c_str());
        case GP_ProjDataPath: return reinterpret_cast<intptr_t>(scanner.getUserDataPath().c_str());
        case GP_PluginPresetPath: return reinterpret_cast<intptr_t>(m_activePresetDir.c_str());
        case GP_SharedDlls: return reinterpret_cast<intptr_t>(scanner.getFLSharedPath().c_str());
        default:
            fprintf(stderr, "FloopHole: unsupported host path index=%lld\n", (long long)index);
            return reinterpret_cast<intptr_t>("");
    }
}

bool FLPluginInstance::loadInternalPreset(int presetIndex) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnDispatcher || presetIndex < 0 || presetIndex >= getNumInternalPresets()) {
        fprintf(stderr, "FloopHole: invalid internal preset %d for %s\n", presetIndex, m_pluginName.c_str());
        return false;
    }
    allNotesOff();
    const intptr_t result = m_fnDispatcher(m_plug, FPD_SetPreset, presetIndex, 0);
    if (std::getenv("FLBR_TRACE_HOST"))
        fprintf(stderr, "FloopHole: selected internal preset %d, dispatcher result=%lld\n",
                presetIndex, (long long)result);
    initializeChannelState();
    return true;
}

std::string FLPluginInstance::getInternalPresetName(int presetIndex) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnGetName || presetIndex < 0 || presetIndex >= getNumInternalPresets()) {
        fprintf(stderr, "FloopHole: invalid internal preset name request %d for %s\n", presetIndex, m_pluginName.c_str());
        return "";
    }
    char name[256]{};
    m_fnGetName(m_plug, FPN_Preset, presetIndex, 0, name);
    return name;
}

bool FLPluginInstance::loadNativePresetFromFile(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnDispatcher) return false;
    if (!FLPluginScanner::isNativePresetFile(m_pluginName, path)) {
        fprintf(stderr, "FloopHole: native preset %s does not belong to %s\n", path.c_str(), m_pluginName.c_str());
        return false;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(path), error)) {
        fprintf(stderr, "FloopHole: cannot read native preset %s: %s\n", path.c_str(),
                error ? error.message().c_str() : "not a regular file");
        return false;
    }
    allNotesOff();
    const intptr_t result = m_fnDispatcher(m_plug, FPD_LoadFile, LFT_Generic,
                                           reinterpret_cast<intptr_t>(path.c_str()));
    // DirectWave loads synchronously but discards its loader's return value.
    // It displays failures in its own editor instead of acknowledging this message.
    const bool nativeNotification = m_pluginName == "DirectWave" && result == 0;
    if (result != 1 && !nativeNotification) {
        fprintf(stderr, "FloopHole: %s did not accept native preset %s (result=%lld)\n",
                m_pluginName.c_str(), path.c_str(), (long long)result);
        return false;
    }
    if (nativeNotification)
        fprintf(stderr, "FloopHole: DirectWave bank request dispatched; load errors appear in its native editor\n");
    initializeChannelState();
    m_currentPresetIndex = -1;
    return true;
}

bool FLPluginInstance::importFlexPack(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || m_pluginName != "FLEX" || !FLPluginScanner::isFlexPackFile(path)) {
        fprintf(stderr, "FloopHole: .flexpack files can only be imported with FLEX selected\n");
        return false;
    }
    namespace fs = std::filesystem;
    const auto root = FLPluginScanner::getInstance().getFlexDataPath(PI_User);
    if (root.empty()) return false;
    const auto source = fs::u8path(path);
    const auto destination = fs::u8path(root) / "Packs" / source.filename();
    std::error_code error;
    if (!fs::is_regular_file(source, error) || fs::file_size(source, error) == 0 || error) {
        fprintf(stderr, "FloopHole: cannot read FLEX pack %s: %s\n", path.c_str(),
                error ? error.message().c_str() : "empty or not a regular file");
        return false;
    }
    fs::create_directories(destination.parent_path(), error);
    if (error) {
        fprintf(stderr, "FloopHole: cannot create FLEX pack directory: %s\n", error.message().c_str());
        return false;
    }
    if (fs::equivalent(source, destination, error)) return true;
    error.clear();
    if (!fs::copy_file(source, destination, fs::copy_options::none, error)) {
        fprintf(stderr, "FloopHole: cannot import FLEX pack %s: %s\n", path.c_str(), error.message().c_str());
        return false;
    }
    auto metadata = source;
    metadata.replace_extension(".ini");
    if (fs::exists(metadata, error)) {
        auto targetMetadata = destination;
        targetMetadata.replace_extension(".ini");
        fs::copy_file(metadata, targetMetadata, fs::copy_options::skip_existing, error);
    }
    if (error) {
        fprintf(stderr, "FloopHole: pack imported but companion metadata could not be copied: %s\n", error.message().c_str());
        return false;
    }
    return true;
}

int32_t FLPluginInstance::sendMIDIMessage(uint8_t status, uint8_t data1, uint8_t data2) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    int32_t message = status | (static_cast<int32_t>(data1) << 8) | (static_cast<int32_t>(data2) << 16);
    if (m_plug && m_fnMIDIIn && !m_inMIDIIn) {
        m_inMIDIIn = true;
        m_midiNoteForwarded = false;
        m_fnMIDIIn(m_plug, &message);
        m_inMIDIIn = false;
        // Some plugins forward MIDIIn through FHD_NoteOn/Off instead of consuming it.
        if (m_midiNoteForwarded) message = -1;
    }
    return message;
}

intptr_t FLPluginInstance::handlePreviewNote(intptr_t id, intptr_t index, intptr_t value) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_inMIDIIn) m_midiNoteForwarded = true;
    const int note = static_cast<int>(index & 0xFFFF);
    const int channel = static_cast<int>((index >> 16) & 0x0F);
    const float velocity = std::clamp(value, intptr_t{0}, intptr_t{127}) / 127.0f;
    if (id == FHD_NoteOff || (id == FHD_NoteOnOff && value <= 0)) {
        noteOff(channel, note, value < 0 ? 0.5f : velocity);
    } else {
        noteOn(channel, note, value < 0 ? 100.0f / 127.0f : velocity);
    }
    return 1;
}

} // namespace flbr
