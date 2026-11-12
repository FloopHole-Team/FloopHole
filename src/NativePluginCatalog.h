#pragma once

#include "FLPluginScanner.h"
#include <optional>

namespace flbr {

struct NativePluginClass {
    std::string identifier;
    std::string name;
    std::string cid;
    bool isGenerator = false;
};

std::optional<std::string> nativeClassId(const std::string& identifier);
std::vector<NativePluginClass> buildNativePluginClasses(const std::vector<FLPluginEntry>& entries);
std::vector<NativePluginClass> discoverNativePluginClasses();

}
