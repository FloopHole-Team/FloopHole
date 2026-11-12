#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include "FLPluginScanner.h"
#include <sys/stat.h>
#include <algorithm>
#include <set>
#include <string>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <cstdio>

namespace flbr {

static bool pathExists(const std::string& p) {
    DWORD attr = GetFileAttributesW(std::filesystem::u8path(p).c_str());
    return (attr != INVALID_FILE_ATTRIBUTES);
}

static bool dirExists(const std::string& p) {
    DWORD attr = GetFileAttributesW(std::filesystem::u8path(p).c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

static std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

static std::string queryRegistryFLPath(HKEY rootKey, const char* subKey, const char* valueName,
                                       REGSAM view = KEY_WOW64_64KEY) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(rootKey, std::filesystem::u8path(subKey).c_str(), 0, KEY_READ | view, &hKey) != ERROR_SUCCESS) {
        return "";
    }
    wchar_t buf[32768] = {0};
    DWORD bufSize = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    std::string res;
    if (RegQueryValueExW(hKey, std::filesystem::u8path(valueName).c_str(), nullptr, &type, (LPBYTE)buf, &bufSize) == ERROR_SUCCESS) {
        if (type == REG_SZ || type == REG_EXPAND_SZ) {
            res = std::filesystem::path(buf).u8string();
            if (type == REG_EXPAND_SZ) {
                wchar_t expanded[32768]{};
                const DWORD size = ExpandEnvironmentStringsW(buf, expanded, 32768);
                if (size > 0 && size <= 32768) res = std::filesystem::path(expanded).u8string();
                else fprintf(stderr, "FloopHole: could not expand registry installation path\n");
            }
        }
    }
    RegCloseKey(hKey);
    return res;
}

static std::string ensureTrailingSlash(std::string p) {
    for (char& c : p) {
        if (c == '/') c = '\\';
    }
    if (!p.empty() && p.back() != '\\') {
        p.push_back('\\');
    }
    return p;
}

FLPluginScanner& FLPluginScanner::getInstance() {
    static FLPluginScanner s_instance;
    return s_instance;
}

std::string FLPluginScanner::savedInstallationPath() {
    return queryRegistryFLPath(HKEY_CURRENT_USER, "Software\\FLBridge", "FLStudioPath");
}

bool FLPluginScanner::saveInstallationPath(const std::string& path) {
    HKEY key = nullptr;
    LSTATUS result = RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\FLBridge", 0, nullptr, 0,
                                     KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (result == ERROR_SUCCESS) {
        const auto wide = std::filesystem::u8path(path).wstring();
        result = RegSetValueExW(key, L"FLStudioPath", 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(wide.c_str()), static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    if (result != ERROR_SUCCESS)
        fprintf(stderr, "FloopHole: cannot save the selected FL Studio folder (Windows error %ld)\n", result);
    return result == ERROR_SUCCESS;
}

bool FLPluginScanner::initialize(bool loadNative, bool rescan) {
    std::lock_guard<std::recursive_mutex> lock(m_scanMutex);
    if (!rescan && (loadNative ? m_initialized : m_discovered)) return !m_flRootPath.empty();

    const auto usePath = [&](std::string path) {
        if (toLowerAscii(std::filesystem::u8path(path).extension().u8string()) == ".exe")
            path = std::filesystem::u8path(path).parent_path().u8string();
        const auto root = resolveInstallationRoot(path);
        if (root.empty()) return false;
        m_flAppPath = path;
        m_flRootPath = root;
        return true;
    };
    const char* explicitPath = std::getenv("FLOOPHOLE_FL_STUDIO");
    if (!explicitPath || !*explicitPath) explicitPath = std::getenv("FLBR_FL_STUDIO");
    if (m_flRootPath.empty() && explicitPath && *explicitPath && !usePath(explicitPath)) {
        fprintf(stderr, "FloopHole: invalid FL Studio installation override: %s\n", explicitPath);
        return false;
    }
    if (m_flRootPath.empty()) {
        const auto saved = savedInstallationPath();
        if (!saved.empty() && !usePath(saved))
            fprintf(stderr, "FloopHole: saved FL Studio folder is no longer valid: %s\n", saved.c_str());
    }
    std::vector<std::string> registered;
    if (m_flRootPath.empty()) {
        for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
                if (m_flRootPath.empty())
                    usePath(queryRegistryFLPath(hive, "Software\\Image-Line\\Shared\\Paths", "FL Studio", view));
                HKEY imageLine = nullptr;
                if (RegOpenKeyExA(hive, "Software\\Image-Line", 0, KEY_READ | view, &imageLine) != ERROR_SUCCESS)
                    continue;
                for (DWORD i = 0;; ++i) {
                    char name[256]{};
                    DWORD length = sizeof(name);
                    const LSTATUS result = RegEnumKeyExA(imageLine, i, name, &length, nullptr, nullptr, nullptr, nullptr);
                    if (result == ERROR_NO_MORE_ITEMS) break;
                    if (result != ERROR_SUCCESS) {
                        fprintf(stderr, "FloopHole: cannot enumerate Image-Line registry keys (error %ld)\n", result);
                        break;
                    }
                    if (toLowerAscii(name).compare(0, 9, "fl studio") != 0) continue;
                    for (const char* value : {"InstallPath", "Path", "FL Studio"}) {
                        auto path = queryRegistryFLPath(hive, (std::string("Software\\Image-Line\\") + name).c_str(), value, view);
                        if (!path.empty()) registered.push_back(std::move(path));
                    }
                }
                RegCloseKey(imageLine);
            }
        }
    }
    if (m_flRootPath.empty()) {
        std::vector<std::string> bases;
        for (const char* variable : {"ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"}) {
            const char* path = std::getenv(variable);
            if (path && *path) bases.push_back((std::filesystem::u8path(path) / "Image-Line").u8string());
        }
        const DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(drives & (1u << i))) continue;
            std::string drive = "A:\\";
            drive[0] += i;
            if (GetDriveTypeA(drive.c_str()) != DRIVE_FIXED) continue;
            bases.push_back(drive + "Program Files\\Image-Line");
            bases.push_back(drive + "Program Files (x86)\\Image-Line");
        }
        auto candidates = findInstallations(bases);
        candidates.insert(candidates.end(), registered.begin(), registered.end());
        sortInstallations(candidates);
        for (const auto& candidate : candidates) if (usePath(candidate)) break;
    }

    if (m_flRootPath.empty()) {
        fprintf(stderr, "FloopHole: FL Studio was not found. Set FLOOPHOLE_FL_STUDIO and perform a full DAW rescan.\n");
        return false;
    }

    m_flSharedPath = m_flRootPath + "Shared\\";
    if (loadNative && !m_initialized) {
        const char* userData = std::getenv("FLOOPHOLE_USER_DATA");
        if (!userData || !*userData) userData = std::getenv("FLBR_USER_DATA");
        if (userData && *userData) {
            m_userDataPath = ensureTrailingSlash(userData);
        } else {
            wchar_t documents[MAX_PATH]{};
            const HRESULT result = SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documents);
            if (FAILED(result)) {
                fprintf(stderr, "FloopHole: cannot locate Documents folder (HRESULT=0x%08lx)\n", (unsigned long)result);
                return false;
            }
            m_userDataPath = ensureTrailingSlash((std::filesystem::path(documents) / L"Image-Line" / L"FL Studio").u8string());
        }
        if (!prepareUserData()) return false;

        // 4. Add Image-Line Shared DLL directories to the Windows DLL search path and preload shared libraries
        const char* sharedCandidates[] = {
            "C:\\Program Files (x86)\\Image-Line\\Shared",
            "C:\\Program Files\\Common Files\\Image-Line\\Shared",
            "C:\\Program Files\\Image-Line\\Shared",
            nullptr
        };
        m_flSharedPath = m_flRootPath + "Shared\\";
        const auto& flShared = m_flSharedPath;

        typedef BOOL (WINAPI *SetDllDirectoryWFn)(LPCWSTR);
        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
        auto pSetDllDir = hKernel32 ? (SetDllDirectoryWFn)GetProcAddress(hKernel32, "SetDllDirectoryW") : nullptr;

        std::vector<std::string> sharedDirs;
        if (dirExists(flShared)) sharedDirs.push_back(ensureTrailingSlash(flShared));
        for (int i = 0; sharedCandidates[i]; ++i) {
            if (dirExists(sharedCandidates[i])) {
                sharedDirs.push_back(ensureTrailingSlash(sharedCandidates[i]));
            }
        }

        const char* preloadDlls[] = {
            "QuickFontCache_x64.dll",
            "QuickFontCachev2_x64.dll",
            "freetype_x64.dll",
            "dsp_ippv2_x64.dll",
            "dsp_ipp_x64.dll",
            "ildsp_x64.dll",
            "SG_x64.dll",
            "SGv2_x64.dll",
            "ILSvg_x64.dll",
            "ILTools_x64.dll",
            "elastique_x64.dll",
            "reverb_x64.dll",
            "wavpack_x64.dll",
            "oggio_x64.dll",
            "EasyFLAC_x64.dll",
            "mp3lame_x64.dll",
            nullptr
        };

        for (const auto& sdir : sharedDirs) {
            if (pSetDllDir) pSetDllDir(std::filesystem::u8path(sdir).c_str());
            for (int i = 0; preloadDlls[i]; ++i) {
                std::string dllPath = sdir + preloadDlls[i];
                if (pathExists(dllPath)) {
                    LoadLibraryExW(std::filesystem::u8path(dllPath).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                }
            }
        }
        m_initialized = true;
    }

    if (!m_discovered || rescan) {
        for (auto& entry : m_plugins) entry.available = false;
        scanDirectory(m_flRootPath + "Plugins\\Fruity\\Generators", true);
        scanDirectory(m_flRootPath + "Plugins\\Fruity\\Effects", false);
        m_discovered = true;
    }
    return true;
}

void FLPluginScanner::scanDirectory(const std::string& baseDir, bool isGenerator) {
    std::string pattern = ensureTrailingSlash(baseDir) + "*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(std::filesystem::u8path(pattern).c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    std::vector<FLPluginEntry> found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::string name = std::filesystem::path(fd.cFileName).u8string();
        if (name.empty() || name[0] == '.') continue;
        if (isPluginExcluded(name)) continue;

        std::string plugDir = ensureTrailingSlash(baseDir) + name;
        std::string dllPath = plugDir + "\\" + name + "_x64.dll";
        if (!pathExists(dllPath)) {
            std::string dllAlt = plugDir + "\\" + name + ".dll";
            if (pathExists(dllAlt)) {
                dllPath = dllAlt;
            } else {
                // Case-insensitive search for *_x64.dll inside plugDir
                WIN32_FIND_DATAW fdDll;
                HANDLE hDll = FindFirstFileW(std::filesystem::u8path(plugDir + "\\*_x64.dll").c_str(), &fdDll);
                if (hDll != INVALID_HANDLE_VALUE) {
                    std::vector<std::string> alternatives;
                    do {
                        std::string fn = std::filesystem::path(fdDll.cFileName).u8string();
                        if (toLowerAscii(fn) != "engine_x64.dll") alternatives.push_back(plugDir + "\\" + fn);
                    } while (FindNextFileW(hDll, &fdDll));
                    FindClose(hDll);
                    if (alternatives.size() == 1) dllPath = alternatives.front();
                    else if (alternatives.size() > 1)
                        fprintf(stderr, "FloopHole: ambiguous native modules in %s\n", plugDir.c_str());
                }
            }
        }
        if (!pathExists(dllPath)) continue;

        FLPluginEntry entry;
        entry.name = name;
        entry.isGenerator = isGenerator && name != "Fruity Dance";
        entry.category = isGenerator ? "Generator" : "Effect";
        entry.dylibPath = dllPath;
        entry.pluginDir = plugDir + "\\";
        entry.identifier = moduleIdentifier(entry.dylibPath);
        found.push_back(std::move(entry));
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    auto getPriority = [&](const std::string& n) -> int {
        if (isGenerator) {
            if (n == "Harmor") return 0;
            if (n == "Sytrus") return 1;
            if (n == "Harmless") return 2;
            if (n == "Ogun") return 3;
            if (n == "PoiZone") return 4;
            if (n == "Toxic Biohazard") return 5;
            if (n == "Transistor Bass") return 6;
            if (n == "Autogun") return 7;
            if (n == "SimSynth") return 8;
            if (n == "3x Osc") return 9;
            if (n == "Plucked!") return 10;
            if (n == "Kepler") return 11;
            if (n == "Fruit Kick") return 12;
            if (n == "Sakura") return 50;
            if (n == "Sawer") return 51;
            if (n == "Morphine") return 52;
            if (n == "GMS") return 53;
            return 100;
        } else {
            if (n == "Gross Beat") return 0;
            if (n == "Maximus") return 1;
            if (n == "Vocodex") return 2;
            if (n == "Fruity Love Philter") return 3;
            if (n == "LuxeVerb") return 4;
            if (n == "Fruity Parametric EQ 2") return 5;
            if (n == "Soundgoodizer") return 6;
            if (n == "Effector") return 7;
            if (n == "Fruity Delay 3") return 8;
            if (n == "Fruity Reeverb 2") return 9;
            if (n == "Fruity Convolver") return 10;
            if (n == "Fruity Limiter") return 11;
            if (n == "Fruity WaveShaper") return 12;
            if (n == "Transient Processor") return 13;
            if (n == "Pitch Shifter") return 14;
            if (n == "Frequency Shifter") return 15;
            return 100;
        }
    };

    std::sort(found.begin(), found.end(), [&](const FLPluginEntry& a, const FLPluginEntry& b) {
        int pa = getPriority(a.name);
        int pb = getPriority(b.name);
        if (pa != pb) return pa < pb;
        return a.name < b.name;
    });

    for (auto& e : found) {
        auto existing = std::find_if(m_plugins.begin(), m_plugins.end(),
            [&](const auto& entry) { return entry.dylibPath == e.dylibPath; });
        if (existing == m_plugins.end()) m_plugins.push_back(std::move(e));
        else existing->available = true;
    }
}

int FLPluginScanner::findPluginIndexByName(const std::string& name) const {
    for (size_t i = 0; i < m_plugins.size(); ++i) {
        if (m_plugins[i].name == name) return (int)i;
    }
    return -1;
}

} // namespace flbr

#endif // _WIN32
