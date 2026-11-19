#include "FLPluginScanner.h"
#include "FLPathManager.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <cctype>
#include <mutex>
#include <set>

namespace flbr {
namespace fs = std::filesystem;

std::string FLPluginScanner::moduleIdentifier(const std::string& modulePath) {
    std::string module = modulePath.substr(modulePath.find_last_of("/\\") + 1);
    const auto extension = module.find_last_of('.');
    if (extension != std::string::npos) module.resize(extension);
    std::transform(module.begin(), module.end(), module.begin(),
        [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; });
    for (const char* architecture : {"_x64", "_arm64", "_x86", "_aarch64"}) {
        const std::string suffix = architecture;
        if (module.size() > suffix.size() && module.compare(module.size() - suffix.size(), suffix.size(), suffix) == 0) {
            module.resize(module.size() - suffix.size());
            break;
        }
    }
    std::string identifier;
    for (unsigned char c : module) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) identifier += static_cast<char>(c);
    }
    return identifier.empty() ? "" : "fl.native." + identifier;
}

std::vector<FLPluginEntry> FLPluginScanner::getPlugins() const {
    std::lock_guard<std::recursive_mutex> lock(m_scanMutex);
    return m_plugins;
}

std::vector<FLPluginEntry> FLPluginScanner::getDiscoverySnapshot() const {
    std::lock_guard<std::recursive_mutex> lock(m_scanMutex);
    std::vector<FLPluginEntry> result;
    for (const auto& entry : m_plugins) {
        if (!entry.available) continue;
        FLPluginEntry metadata;
        metadata.name = entry.name;
        metadata.identifier = entry.identifier;
        metadata.category = entry.category;
        metadata.isGenerator = entry.isGenerator;
        metadata.dylibPath = entry.dylibPath;
        metadata.pluginDir = entry.pluginDir;
        result.push_back(std::move(metadata));
    }
    return result;
}

int FLPluginScanner::findPluginIndexByIdentifier(const std::string& identifier) const {
    std::lock_guard<std::recursive_mutex> lock(m_scanMutex);
    int result = -1;
    for (size_t i = 0; i < m_plugins.size(); ++i) {
        if (!m_plugins[i].available || m_plugins[i].identifier != identifier) continue;
        if (result >= 0) {
            fprintf(stderr, "FloopHole: ambiguous native module identifier %s\n", identifier.c_str());
            return -1;
        }
        result = static_cast<int>(i);
    }
    return result;
}

bool FLPluginScanner::preparePlugin(int index) {
    std::lock_guard<std::recursive_mutex> lock(m_scanMutex);
    if (!initialize() || index < 0 || static_cast<size_t>(index) >= m_plugins.size() || !m_plugins[index].available) {
        fprintf(stderr, "FloopHole: cannot prepare native plugin index %d\n", index);
        return false;
    }
    auto& entry = m_plugins[index];
    if (!entry.presetsScanned) {
        scanPresetsForPlugin(entry);
        entry.presetsScanned = true;
    }
    return true;
}

std::string FLPluginScanner::resolveInstallationRoot(const std::string& path) {
    if (path.empty()) return {};
    const auto selected = fs::u8path(path);
    if (!selected.is_absolute()) return {};
    for (auto root : {selected, selected / "Contents/Resources/FL"}) {
        std::error_code error;
        if (fs::is_directory(root / "Plugins/Fruity/Generators", error) &&
            fs::is_directory(root / "Plugins/Fruity/Effects", error) &&
            fs::is_directory(root / "Shared", error)) {
            root = root.lexically_normal();
            return (root / "").make_preferred().u8string();
        }
    }
    return {};
}

std::vector<std::string> FLPluginScanner::findInstallations(const std::vector<std::string>& directories) {
    std::vector<std::string> found;
    for (const auto& directory : directories) {
        std::error_code error;
        if (!fs::is_directory(fs::u8path(directory), error)) continue;
        fs::directory_iterator current(fs::u8path(directory), error), end;
        while (!error && current != end) {
            std::string name = current->path().filename().u8string();
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (name.compare(0, 9, "fl studio") == 0 &&
                !resolveInstallationRoot(current->path().u8string()).empty())
                found.push_back(current->path().u8string());
            current.increment(error);
        }
        if (error) fprintf(stderr, "FloopHole: cannot search %s: %s\n", directory.c_str(), error.message().c_str());
    }
    sortInstallations(found);
    return found;
}

void FLPluginScanner::sortInstallations(std::vector<std::string>& paths) {
    const auto version = [](const std::string& path) {
        const auto name = fs::u8path(path).filename().u8string();
        std::vector<std::string> numbers;
        for (size_t i = 0; i < name.size();) {
            if (!std::isdigit(static_cast<unsigned char>(name[i]))) { ++i; continue; }
            const auto start = i;
            while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) ++i;
            auto number = name.substr(start, i - start);
            const auto first = number.find_first_not_of('0');
            numbers.push_back(first == std::string::npos ? "0" : number.substr(first));
        }
        return numbers;
    };
    std::sort(paths.begin(), paths.end(), [&](const std::string& left, const std::string& right) {
        const auto a = version(left), b = version(right);
        for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
            if (a[i].size() != b[i].size()) return a[i].size() > b[i].size();
            if (a[i] != b[i]) return a[i] > b[i];
        }
        if (a.size() != b.size()) return a.size() > b.size();
        return left < right;
    });
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
}

bool FLPluginScanner::chooseInstallation(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(m_scanMutex);
    if (m_initialized) {
        fprintf(stderr, "FloopHole: restart the host before changing an active FL Studio installation\n");
        return false;
    }
    const auto root = resolveInstallationRoot(path);
    if (root.empty()) {
        fprintf(stderr, "FloopHole: %s is not an FL Studio installation (Plugins/Fruity and Shared are required)\n",
                path.c_str());
        return false;
    }
    if (!saveInstallationPath(path)) return false;
    m_flAppPath = path;
    m_flRootPath = root;
    m_discovered = false;
    m_plugins.clear();
    return initialize();
}

static std::string extensionOf(const std::string& path) {
    std::string ext = fs::u8path(path).extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext;
}

bool FLPresetEntry::isDefault() const {
    return name.size() == 7 &&
        std::equal(name.begin(), name.end(), "default",
                   [](unsigned char a, char b) { return std::tolower(a) == b; });
}

bool FLPluginScanner::isPluginExcluded(const std::string& name) {
    static const std::set<std::string> excluded = {
        "Fruity Wrapper", "Patcher", "ReWired", "MIDI Out", "Dashboard", "Control Surface",
        "Fruity Video Player", "FL Studio Mobile", "FL Studio Mobile Rack", "FL Studio Mobile Rack FX",
        "Fruity Send", "Fruity Envelope Controller", "Fruity Keyboard Controller",
        "Fruity Voltage Controller", "Fruity Formula Controller", "Fruity Peak Controller",
        "Fruity X-Y Controller", "Fruity X-Y-Z Controller", "VFX Color Mapper", "VFX Envelope",
        "VFX Key Mapper", "VFX Keyboard Splitter", "VFX Level Scaler", "VFX Script", "VFX Sequencer",
        "Fruity Granulizer", "Fruity Slicer", "Wave Traveller"
    };
    return excluded.count(name) != 0;
}

int FLPluginScanner::findPluginIndexByName(const std::string& name, bool isGenerator) const {
    for (size_t i = 0; i < m_plugins.size(); ++i) {
        if (m_plugins[i].name == name && m_plugins[i].isGenerator == isGenerator)
            return static_cast<int>(i);
    }
    return -1;
}

std::vector<int> FLPluginScanner::getPluginIndices(bool isGenerator) const {
    std::vector<int> indices;
    for (size_t i = 0; i < m_plugins.size(); ++i) {
        if (m_plugins[i].isGenerator == isGenerator) indices.push_back(static_cast<int>(i));
    }
    std::stable_sort(indices.begin(), indices.end(), [&](int a, int b) {
        const auto lower = [](unsigned char c) { return std::tolower(c); };
        const auto& left = m_plugins[a].name;
        const auto& right = m_plugins[b].name;
        return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
            [&](unsigned char x, unsigned char y) { return lower(x) < lower(y); });
    });
    return indices;
}

std::string FLPluginScanner::resolveRelativePath(const std::string& relPath, const std::string& activePluginDir) const {
    if (relPath.empty()) return m_flRootPath;
    std::string clean = relPath;
    std::replace(clean.begin(), clean.end(), '\\', '/');
    for (const auto& token : {std::make_pair("%FLStudioFactoryData%", m_flRootPath),
                              std::make_pair("%FLStudioUserData%", m_userDataPath)}) {
        const std::string prefix = token.first;
        if (clean.compare(0, prefix.size(), prefix) == 0) {
            clean.replace(0, prefix.size(), token.second);
            break;
        }
    }
    const auto file = fs::u8path(clean).lexically_normal();
    std::error_code error;
    const auto exists = [&](const fs::path& path) { return fs::is_regular_file(path, error); };
    const auto native = [](fs::path path) { return path.make_preferred().u8string(); };
    if (file.is_absolute() && exists(file)) return native(file);
    if (!activePluginDir.empty()) {
        const auto path = fs::u8path(activePluginDir) / file;
        if (exists(path)) return native(path);
    }
    for (const char* prefix : {"Plugins/Fruity/Generators", "Plugins/Fruity/Effects",
                               "Plugins/Fruity", "Data/Patches/Plugin presets", "",
                               "Data", "Shared", "Data/Patches"}) {
        const auto path = fs::u8path(m_flRootPath) / prefix / file;
        if (exists(path)) return native(path);
    }
    FLPathManager manager;
    std::string name = file.filename().u8string();
    char located[PathBufferLength];
    if (!name.empty() && manager.locate(FLData_Packs, name.data(), nullptr, 0, 0, located))
        return located;
    return native(fs::u8path(m_flRootPath) / file);
}

bool FLPluginScanner::prepareUserData() const {
    if (!fs::u8path(m_userDataPath).is_absolute()) {
        fprintf(stderr, "FloopHole: user data directory must be an absolute path: %s\n", m_userDataPath.c_str());
        return false;
    }
    for (const auto& path : {m_userDataPath, getFlexDataPath(PI_User) + "Packs",
                             getFlexDataPath(PI_User) + "Images"}) {
        std::error_code error;
        fs::create_directories(fs::u8path(path), error);
        if (error) {
            fprintf(stderr, "FloopHole: cannot create user data directory %s: %s\n", path.c_str(), error.message().c_str());
            return false;
        }
    }
    return true;
}

std::string FLPluginScanner::getDataPath(uint32_t id, uint8_t index) const {
    if (index > PI_Factory) return "";
    const bool user = index == PI_User;
    const fs::path root = fs::u8path(user ? m_userDataPath : m_flRootPath);
    if (root.empty()) return "";
    fs::path path;
    switch (id) {
        case 0: path = root; break;
        case FLData_PluginPresets: path = root / (user ? "Presets/Plugin presets" : "Data/Patches/Plugin presets"); break;
        case FLData_GeneratorPlugins:
        case FLData_EffectPlugins:
            path = user ? (root / "..").lexically_normal() : root /
                (id == FLData_GeneratorPlugins ? "Plugins/Fruity/Generators" : "Plugins/Fruity/Effects");
            break;
        case FLData_SharedPacks:
            // FLEX appends its own "FLEX/" subdirectory to this SDK path.
            path = user ? (root / "..").lexically_normal() : root / "Data/Patches/Packs";
            break;
        case FLData_Packs: path = root / (user ? "Presets/Packs" : "Data/Patches/Packs"); break;
        case FLData_Scores: path = root / (user ? "Presets/Scores" : "Data/Patches/Scores"); break;
        case FLData_SystemData: path = root / (user ? "Settings" : "System"); break;
        case FLData_Internet: path = root / (user ? "Settings/Internet" : "System/Internet"); break;
#ifdef __APPLE__
        case FLData_AudioScripts: path = root / (user ? "Settings/Audio scripts" : "System/Config/Audio scripts"); break;
#endif
        case 1039: path = root / (user ? "Settings/Languages" : "System/Languages"); break;
        default: {
            static std::mutex warningMutex;
            static std::set<std::pair<uint32_t, uint8_t>> warnedPaths;
            std::lock_guard<std::mutex> lock(warningMutex);
            if (warnedPaths.emplace(id, index).second)
                fprintf(stderr, "FloopHole: unsupported data path id=%u index=%u\n", id, index);
            return "";
        }
    }
    path.make_preferred();
    std::string result = path.u8string();
    if (result.empty() || result.back() != fs::path::preferred_separator) result += fs::path::preferred_separator;
    return result;
}

std::string FLPluginScanner::getFlexDataPath(uint8_t index) const {
    const auto parent = getDataPath(FLData_SharedPacks, index);
    return parent.empty() ? "" : parent + "FLEX" + static_cast<char>(fs::path::preferred_separator);
}

std::string FLPluginScanner::getPluginPresetPath(const FLPluginEntry& entry, uint8_t index) const {
    const auto root = getDataPath(FLData_PluginPresets, index);
    if (root.empty()) return "";
    const bool generatorPresets = entry.category == "Generator" || (entry.category.empty() && entry.isGenerator);
    auto path = fs::u8path(root) / (generatorPresets ? "Generators" : "Effects") / fs::u8path(entry.name);
    path.make_preferred();
    return path.u8string() + static_cast<char>(fs::path::preferred_separator);
}

bool FLPluginScanner::isNativePresetFile(const std::string& plugin, const std::string& path) {
    const auto ext = extensionOf(path);
    return (plugin == "Toxic Biohazard" && ext == ".tbio") ||
           (plugin == "Morphine" && ext == ".mrp") ||
           (plugin == "GMS" && ext == ".gmsynth") ||
           (plugin == "MiniSynth" && ext == ".flms") ||
           (plugin == "Soundfont Player" && ext == ".sf2") ||
           (plugin == "DirectWave" && (ext == ".dwp" || ext == ".dwb"));
}

bool FLPluginScanner::isFlexPackFile(const std::string& path) {
    return extensionOf(path) == ".flexpack";
}

bool FLPluginScanner::isNativePresetFile(const std::string& path) {
    const auto ext = extensionOf(path);
    return ext == ".tbio" || ext == ".mrp" || ext == ".gmsynth" || ext == ".flms" ||
           ext == ".sf2" || ext == ".dwp" || ext == ".dwb";
}

static FLPresetEntry makePresetEntry(const fs::path& file, fs::path relative) {
    relative.replace_extension();
    const auto noExtension = relative.generic_u8string();
    const auto slash = noExtension.find('/');
    FLPresetEntry preset;
    preset.fullPath = file.u8string();
    preset.name = slash == std::string::npos ? noExtension : noExtension.substr(slash + 1);
    preset.category = slash == std::string::npos ? "" : noExtension.substr(0, slash);
    preset.displayName = preset.category.empty() ? preset.name : preset.category + " / " + preset.name;
    return preset;
}

int FLPluginScanner::registerSavedPreset(int pluginIndex, const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(m_scanMutex);
    if (pluginIndex < 0 || static_cast<size_t>(pluginIndex) >= m_plugins.size() || !m_plugins[pluginIndex].available)
        return -1;
    auto& entry = m_plugins[pluginIndex];
    const auto file = fs::u8path(path).lexically_normal().make_preferred();
    for (size_t i = 0; i < entry.presets.size(); ++i) {
        if (fs::u8path(entry.presets[i].fullPath).lexically_normal() == file)
            return static_cast<int>(i);
    }

    auto relative = file.filename();
    for (uint8_t index : {PI_User, PI_Factory}) {
        const auto root = fs::u8path(getPluginPresetPath(entry, index)).lexically_normal();
        const auto candidate = file.lexically_relative(root);
        if (!candidate.empty() && *candidate.begin() != "..") {
            relative = candidate;
            break;
        }
    }
    // Keep existing indices stable for other instances and saved VST state.
    entry.presets.push_back(makePresetEntry(file, relative));
    return static_cast<int>(entry.presets.size() - 1);
}

void FLPluginScanner::scanPresetsForPlugin(FLPluginEntry& entry) const {
    for (uint8_t index : {PI_User, PI_Factory}) {
        const auto root = fs::u8path(getPluginPresetPath(entry, index));
        std::error_code error;
        if (!fs::is_directory(root, error)) {
            if (error && error != std::errc::no_such_file_or_directory)
                fprintf(stderr, "FloopHole: cannot scan presets in %s: %s\n", root.u8string().c_str(), error.message().c_str());
            continue;
        }
        for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            const bool regular = it->is_regular_file(error);
            if (error) break;
            if (!regular) continue;
            const auto file = it->path();
            const auto ext = extensionOf(file.u8string());
            if (ext != ".fst" && !isNativePresetFile(entry.name, file.u8string())) continue;
            entry.presets.push_back(makePresetEntry(file, file.lexically_relative(root)));
        }
        if (error) fprintf(stderr, "FloopHole: cannot scan presets in %s: %s\n",
                           root.u8string().c_str(), error.message().c_str());
    }
    std::stable_sort(entry.presets.begin(), entry.presets.end(), [](const auto& a, const auto& b) {
        const bool aDefault = a.isDefault(), bDefault = b.isDefault();
        if (aDefault || bDefault) return aDefault && !bDefault;
        if ((a.category == "Default") != (b.category == "Default")) return a.category == "Default";
        return a.displayName < b.displayName;
    });
}
} // namespace flbr
