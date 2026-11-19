#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

namespace flbr {

struct FLPresetEntry {
    std::string displayName; // e.g. "Arp / Chip"
    std::string category;    // e.g. "Arp"
    std::string name;        // e.g. "Chip"
    std::string fullPath;    // Absolute path to .fst or a plugin-native preset
    bool isDefault() const;
};

struct FLPluginEntry {
    std::string name;        // e.g. "Harmor"
    std::string category;    // Installation category: "Generator" or "Effect"
    std::string dylibPath;   // Absolute path to <Name>_x64.dylib
    std::string pluginDir;   // Directory containing the plugin
    bool isGenerator = true;
    std::vector<FLPresetEntry> presets;
    std::string identifier;
    bool available = true;
    bool presetsScanned = false;
};

class FLPluginScanner {
public:
    static FLPluginScanner& getInstance();

    bool initialize(bool loadNative = true, bool rescan = false);
    bool discover(bool rescan = false) { return initialize(false, rescan); }
    bool preparePlugin(int index);
    bool chooseInstallation(const std::string& path);
    bool isInitialized() const { return m_initialized; }
    static std::string resolveInstallationRoot(const std::string& path);

    const std::string& getFLAppPath() const { return m_flAppPath; }
    const std::string& getFLRootPath() const { return m_flRootPath; }
    const std::string& getFLSharedPath() const { return m_flSharedPath; }
    const std::string& getUserDataPath() const { return m_userDataPath; }
    std::vector<FLPluginEntry> getPlugins() const;
    std::vector<FLPluginEntry> getDiscoverySnapshot() const;
    static std::string moduleIdentifier(const std::string& modulePath);
    int findPluginIndexByIdentifier(const std::string& identifier) const;

    int findPluginIndexByName(const std::string& name) const;
    int findPluginIndexByName(const std::string& name, bool isGenerator) const;
    std::vector<int> getPluginIndices(bool isGenerator) const;
    static bool isPluginExcluded(const std::string& name);
    std::string resolveRelativePath(const std::string& relPath, const std::string& activePluginDir = "") const;
    std::string getDataPath(uint32_t id, uint8_t index) const;
    std::string getFlexDataPath(uint8_t index) const;
    std::string getPluginPresetPath(const FLPluginEntry& entry, uint8_t index = 1) const;
    int registerSavedPreset(int pluginIndex, const std::string& path);
    static bool isNativePresetFile(const std::string& plugin, const std::string& path);
    static bool isNativePresetFile(const std::string& path);
    static bool isFlexPackFile(const std::string& path);
    bool prepareUserData() const;

private:
    friend struct FLPluginScannerTestAccess;
    FLPluginScanner() = default;
    void scanDirectory(const std::string& baseDir, bool isGenerator);
    void scanPresetsForPlugin(FLPluginEntry& entry) const;
    static std::vector<std::string> findInstallations(const std::vector<std::string>& directories);
    static void sortInstallations(std::vector<std::string>& paths);
    static std::string savedInstallationPath();
    static bool saveInstallationPath(const std::string& path);

    bool m_initialized = false;
    bool m_discovered = false;
    mutable std::recursive_mutex m_scanMutex;
    std::string m_flAppPath;
    std::string m_flRootPath;
    std::string m_flSharedPath;
    std::string m_userDataPath;
    std::vector<FLPluginEntry> m_plugins;
};

} // namespace flbr
