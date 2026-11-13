#include "FLPluginInstance.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <cctype>

namespace flbr {

FLPluginInstance* FLPluginInstance::hostOwner(void* host) {
    if (host) return static_cast<HostStruct*>(host)->owner;
    fprintf(stderr, "FloopHole: native host callback without a host\n");
    return nullptr;
}

void FLPluginInstance::installHostServices() {
    const auto slot = [&](int index, void* function) {
        m_cppVmt[index] = function;
        m_pascalVmt[25 + index] = function;
    };
    slot(13, reinterpret_cast<void*>(+[](void* host, intptr_t& handle, char* file,
                                        const FLWaveFormat* format, int32_t flags) {
        auto* owner = hostOwner(host);
        return owner && owner->handleLoadSample(handle, file, format, flags);
    }));
    slot(14, reinterpret_cast<void*>(+[](void* host, intptr_t handle, int32_t& frames) -> void* {
        auto* owner = hostOwner(host);
        if (owner) return owner->handleGetSampleData(handle, frames);
        frames = 0;
        return nullptr;
    }));
    slot(15, reinterpret_cast<void*>(+[](void* host, intptr_t handle) {
        if (auto* owner = hostOwner(host)) owner->handleCloseSample(handle);
    }));
    slot(23, reinterpret_cast<void*>(+[](void* host, intptr_t handle, FLSampleInfo* info) {
        if (auto* owner = hostOwner(host)) owner->handleGetSampleInfo(handle, info);
    }));
}

bool FLPluginInstance::handleLoadSample(intptr_t& handle, char* fileName,
                                       const FLWaveFormat* format, int32_t flags) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!fileName) {
        fprintf(stderr, "FloopHole: sample loader called without a filename buffer\n");
        return false;
    }
    const size_t pathLength = strnlen(fileName, 256);
    if (pathLength == 256) {
        fprintf(stderr, "FloopHole: native sample filename is not terminated\n");
        return false;
    }
    std::string path(fileName, pathLength);
    if (flags & FHLS_ShowDialog) {
        lock.unlock();
        if (!selectSampleFile(path)) return false;
        lock.lock();
    } else if (path.empty() && (flags & FHLS_GetName)) {
        const auto found = m_samples.find(handle);
        if (found != m_samples.end()) path = found->second.fileName;
    }
    if (path.empty()) {
        fprintf(stderr, "FloopHole: sample loader called without a filename\n");
        return false;
    }
    path = reinterpret_cast<const char*>(handleLocateDataFile(reinterpret_cast<intptr_t>(path.c_str())));
    if (path.empty()) return false;
    if (path.size() >= 256) {
        fprintf(stderr, "FloopHole: sample filename exceeds the SDK's 256-byte buffer: %s\n", path.c_str());
        return false;
    }

    const int channels = format && format->channels ? format->channels : 2;
    const int bits = format && format->bitsPerSample ? format->bitsPerSample : 16;
    const int tag = format && format->tag ? format->tag : 1;
    const bool floating = tag == 3;
    if ((tag != 1 && tag != 3) || (channels != 1 && channels != 2) || (floating ? bits != 32 : bits != 16)) {
        fprintf(stderr, "FloopHole: unsupported sample format tag=%d channels=%d bits=%d\n", tag, channels, bits);
        return false;
    }
    const double rate = flags & FHLS_NoResampling ? 0 :
        format && format->sampleRate ? format->sampleRate : m_sampleRate;
    auto found = m_samples.find(handle);
    if (!(flags & (FHLS_ForceReload | FHLS_GetName)) && found != m_samples.end() && found->second.fileName == path &&
        found->second.channels == channels && found->second.format == (floating ? 1 : 0) &&
        found->second.requestedRate == rate) {
        memcpy(fileName, path.c_str(), path.size() + 1);
        ++m_sampleLoadSerial;
        return true;
    }
    HostSample sample;
    sample.fileName = path;
    sample.channels = channels;
    sample.format = floating ? 1 : 0;
    sample.requestedRate = rate;
    lock.unlock();
    if (!decodeSampleFile(path, rate, sample.floats[1], sample.sampleRate)) return false;
    const size_t frames = sample.floats[1].size() / 2;
    if (!frames || frames > INT32_MAX) {
        fprintf(stderr, "FloopHole: sample has an invalid frame count: %s\n", path.c_str());
        return false;
    }
    if (flags & FHLS_GetName) {
        memcpy(fileName, path.c_str(), path.size() + 1);
        return true;
    }
    sample.floats[0].resize(frames);
    sample.integers[0].resize(frames);
    sample.integers[1].resize(frames * 2);
    const auto quantize = [](float value) {
        return static_cast<int16_t>(std::clamp(std::lround(std::clamp(value, -1.0f, 1.0f) * 32768.0f),
                                               -32768L, 32767L));
    };
    for (size_t i = 0; i < frames; ++i) {
        const float left = sample.floats[1][2 * i];
        const float right = sample.floats[1][2 * i + 1];
        if (!std::isfinite(left) || !std::isfinite(right)) {
            fprintf(stderr, "FloopHole: sample contains non-finite audio: %s\n", path.c_str());
            return false;
        }
        const float mono = left * 0.5f + right * 0.5f;
        sample.floats[0][i] = mono;
        sample.integers[0][i] = quantize(mono);
        sample.integers[1][2 * i] = quantize(left);
        sample.integers[1][2 * i + 1] = quantize(right);
    }
    lock.lock();
    if (m_samples.find(handle) == m_samples.end()) handle = ++m_nextSampleHandle;
    m_samples[handle] = std::move(sample);
    ++m_sampleLoadSerial;
    memcpy(fileName, path.c_str(), path.size() + 1);
    return true;
}

void* FLPluginInstance::handleGetSampleData(intptr_t handle, int32_t& frames) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto found = m_samples.find(handle);
    if (found == m_samples.end()) {
        fprintf(stderr, "FloopHole: invalid native sample handle %lld\n", static_cast<long long>(handle));
        frames = 0;
        return nullptr;
    }
    auto& sample = found->second;
    frames = static_cast<int32_t>(sample.floats[0].size());
    const int channelIndex = sample.channels - 1;
    return sample.format ? static_cast<void*>(sample.floats[channelIndex].data()) :
        static_cast<void*>(sample.integers[channelIndex].data());
}

void FLPluginInstance::handleGetSampleInfo(intptr_t handle, FLSampleInfo* info) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!info || info->size < static_cast<int32_t>(offsetof(FLSampleInfo, reserved))) {
        fprintf(stderr, "FloopHole: invalid native sample info buffer\n");
        return;
    }
    FLSampleInfo result{};
    result.size = info->size;
    result.loopStart = -1;
    const auto found = m_samples.find(handle);
    if (found != m_samples.end()) {
        auto& sample = found->second;
        result.channels = info->channels == -1 ? sample.channels : info->channels;
        result.format = info->format == -1 ? sample.format : info->format;
        if ((result.channels != 1 && result.channels != 2) || (result.format != 0 && result.format != 1)) {
            fprintf(stderr, "FloopHole: invalid native sample info format\n");
            return;
        }
        result.length = result.solidLength = static_cast<int32_t>(sample.floats[0].size());
        result.loopEnd = result.length;
        result.sampleRateConversion = sample.sampleRate / m_sampleRate;
        const int index = result.channels - 1;
        result.data = result.format ? static_cast<void*>(sample.floats[index].data()) :
            static_cast<void*>(sample.integers[index].data());
    } else {
        fprintf(stderr, "FloopHole: invalid native sample info handle %lld\n", static_cast<long long>(handle));
    }
    memcpy(info, &result, std::min(static_cast<size_t>(info->size), sizeof(result)));
}

void FLPluginInstance::handleCloseSample(intptr_t handle) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_samples.erase(handle) && handle)
        fprintf(stderr, "FloopHole: cannot close unknown native sample handle %lld\n", static_cast<long long>(handle));
    if (handle == m_channelSample) m_channelSample = 0;
}

bool FLPluginInstance::loadChannelSample(const std::string& path) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnDispatcher || !(m_pluginFlags & (FPF_GetChanCustomShape | FPF_GetChanSample))) {
        fprintf(stderr, "FloopHole: %s does not accept a parent-channel sample\n", m_pluginName.c_str());
        return false;
    }
    char fileName[256]{};
    if (path.size() >= sizeof(fileName)) {
        fprintf(stderr, "FloopHole: channel sample filename exceeds the SDK buffer\n");
        return false;
    }
    memcpy(fileName, path.c_str(), path.size() + 1);
    intptr_t handle = 0;
    const auto generation = m_pluginGeneration;
    FLWaveFormat format{3, 1, static_cast<uint32_t>(m_sampleRate), 0, 4, 32, 0};
    lock.unlock();
    if (!handleLoadSample(handle, fileName, &format, path.empty() ? FHLS_ShowDialog : 0)) return false;
    lock.lock();
    if (!m_plug || m_pluginGeneration != generation) {
        handleCloseSample(handle);
        fprintf(stderr, "FloopHole: plugin changed while loading a channel sample\n");
        return false;
    }
    allNotesOff();
    const auto old = m_channelSample;
    m_channelSample = handle;
    const auto& mono = m_samples.at(handle).floats[0];
    for (size_t i = 0; i < m_customShape.size(); ++i) {
        const double position = static_cast<double>(i) * mono.size() / m_customShape.size();
        const size_t first = static_cast<size_t>(position);
        const size_t second = std::min(first + 1, mono.size() - 1);
        const float fraction = static_cast<float>(position - first);
        m_customShape[i] = mono[first] * (1 - fraction) + mono[second] * fraction;
    }
    m_fnDispatcher(m_plug, FPD_ChanSampleChanged, handle,
        m_pluginFlags & FPF_GetChanCustomShape ? reinterpret_cast<intptr_t>(m_customShape.data()) : 0);
    if (old && old != handle && m_samples.count(old)) handleCloseSample(old);
    return true;
}

bool FLPluginInstance::loadDroppedFile(const std::string& path) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_plug || !m_fnDispatcher) {
        fprintf(stderr, "FloopHole: cannot drop a file without a loaded plugin\n");
        return false;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(path), error)) {
        fprintf(stderr, "FloopHole: dropped file is unavailable: %s\n", path.c_str());
        return false;
    }
    std::string extension = std::filesystem::u8path(path).extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return std::tolower(c); });
    if (extension == ".fst" ||
        FLPluginScanner::isNativePresetFile(path) || FLPluginScanner::isFlexPackFile(path))
        return loadPresetFromFile(path);
    const auto previousLoad = m_sampleLoadSerial;
    const auto result = m_fnDispatcher(m_plug, FPD_LoadFile, LFT_DroppedFile,
                                      reinterpret_cast<intptr_t>(path.c_str()));
    if (result == 1) return true;
    if (result == 0 && m_sampleLoadSerial != previousLoad) return true;
    // Harmor imports these images but returns the legacy "not handled" result.
    if (result == 0 && m_pluginName == "Harmor" &&
        (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp"))
        return true;
    if (result == 0 && m_pluginName == "Newtone" &&
        (extension == ".wav" || extension == ".aif" || extension == ".aiff" || extension == ".mp3" ||
         extension == ".flac" || extension == ".ogg" || extension == ".m4a" || extension == ".wv"))
        return true;
    if (result == 0 && (m_pluginFlags & (FPF_GetChanCustomShape | FPF_GetChanSample))) {
        lock.unlock();
        return loadChannelSample(path);
    }
    fprintf(stderr, "FloopHole: %s rejected dropped file %s (result=%lld)\n",
            m_pluginName.c_str(), path.c_str(), static_cast<long long>(result));
    return false;
}

void FLPluginInstance::setEnabled(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_enabled = enabled;
    if (m_plug && m_fnDispatcher) m_fnDispatcher(m_plug, FPD_SetEnabled, 0, enabled ? 1 : 0);
}

std::string FLPluginInstance::getChannelSamplePath() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto sample = m_samples.find(m_channelSample);
    return sample == m_samples.end() ? "" : sample->second.fileName;
}

void FLPluginInstance::clearChannelSample() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_channelSample) return;
    allNotesOff();
    m_customShape.fill(0);
    if (m_plug && m_fnDispatcher) m_fnDispatcher(m_plug, FPD_ChanSampleChanged, 0,
        m_pluginFlags & FPF_GetChanCustomShape ? reinterpret_cast<intptr_t>(m_customShape.data()) : 0);
    handleCloseSample(m_channelSample);
}

} // namespace flbr
