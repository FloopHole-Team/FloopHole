#pragma once

#include "FruitySDK.h"
#include <cmath>
#include <cstdio>
#include <string>

namespace flbr {

constexpr double kDefaultMasterVolume = 0.78;
constexpr uint32_t kParamMasterVolume = 20005;
constexpr double kDefaultEffectMix = 1.0;
constexpr uint32_t kParamEffectMix = 20006;

inline double masterVolumeGain(double value) {
    if (value == 0) return 0;
    if (value == 1) return 1;
    // Calibrate a continuous taper to the existing 78% / -5.2 dB default.
    static const double exponent = std::log(kDefaultWrapperGain) / std::log(kDefaultMasterVolume);
    return kDefaultWrapperGain * std::pow(value / kDefaultMasterVolume, exponent);
}

inline std::string masterVolumeDisplay(double value) {
    char text[64];
    if (value == 0) std::snprintf(text, sizeof(text), "0%% (mute)");
    else std::snprintf(text, sizeof(text), "%.0f%% (%.1f dB)", value * 100, 20 * std::log10(masterVolumeGain(value)));
    return text;
}

inline std::string effectMixDisplay(double value) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.0f%% wet / %.0f%% dry", value * 100, (1 - value) * 100);
    return text;
}

}
