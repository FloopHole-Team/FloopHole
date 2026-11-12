#import <Cocoa/Cocoa.h>
#include "FLPluginScanner.h"
#include <dlfcn.h>
#include <sys/stat.h>
#include <algorithm>
#include <set>
#include <cstdlib>
#include <cstdio>

namespace flbr {

static bool pathExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

FLPluginScanner& FLPluginScanner::getInstance() {
    static FLPluginScanner s_instance;
    return s_instance;
}

std::string FLPluginScanner::savedInstallationPath() {
    NSUserDefaults* settings = [[NSUserDefaults alloc] initWithSuiteName:@"com.flbridge.native"];
    NSString* path = [settings stringForKey:@"FLStudioPath"];
    return path ? path.UTF8String : "";
}

bool FLPluginScanner::saveInstallationPath(const std::string& path) {
    NSUserDefaults* settings = [[NSUserDefaults alloc] initWithSuiteName:@"com.flbridge.native"];
    [settings setObject:[NSString stringWithUTF8String:path.c_str()] forKey:@"FLStudioPath"];
    if (![settings synchronize]) {
        fprintf(stderr, "FloopHole: could not save the selected FL Studio folder\n");
        return false;
    }
    return true;
}

bool FLPluginScanner::initialize(bool loadNative, bool rescan) {
    std::lock_guard<std::recursive_mutex> lock(m_scanMutex);
    if (!rescan && (loadNative ? m_initialized : m_discovered)) return !m_flRootPath.empty();

    @autoreleasepool {
        const char* explicitPath = std::getenv("FLOOPHOLE_FL_STUDIO");
        if (!explicitPath || !*explicitPath) explicitPath = std::getenv("FLBR_FL_STUDIO");
        if (m_flRootPath.empty() && explicitPath && *explicitPath) {
            m_flRootPath = resolveInstallationRoot(explicitPath);
            if (m_flRootPath.empty()) {
                fprintf(stderr, "FloopHole: invalid FL Studio installation override: %s\n", explicitPath);
                return false;
            }
            m_flAppPath = explicitPath;
        }
        if (m_flRootPath.empty()) {
            const auto saved = savedInstallationPath();
            if (!saved.empty()) {
                m_flRootPath = resolveInstallationRoot(saved);
                if (!m_flRootPath.empty()) m_flAppPath = saved;
                else fprintf(stderr, "FloopHole: saved FL Studio folder is no longer valid: %s\n", saved.c_str());
            }
        }
        if (m_flRootPath.empty()) {
            auto candidates = findInstallations({"/Applications", std::string(NSHomeDirectory().UTF8String) + "/Applications",
                                                  "/Applications/Image-Line"});
            for (const auto& candidate : candidates) {
                m_flRootPath = resolveInstallationRoot(candidate);
                if (m_flRootPath.empty()) continue;
                m_flAppPath = candidate;
                break;
            }
            if (m_flRootPath.empty()) {
                NSURL* registered = [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:@"com.image-line.flstudio"];
                if (registered) {
                    m_flRootPath = resolveInstallationRoot(registered.path.UTF8String);
                    if (!m_flRootPath.empty()) m_flAppPath = registered.path.UTF8String;
                }
            }
        }
        if (m_flRootPath.empty()) {
            fprintf(stderr, "FloopHole: FL Studio was not found. Set FLOOPHOLE_FL_STUDIO and perform a full DAW rescan.\n");
            return false;
        }

        m_flSharedPath = m_flRootPath + "Shared/";
        if (loadNative && !m_initialized) {
            const char* userData = std::getenv("FLOOPHOLE_USER_DATA");
            if (!userData || !*userData) userData = std::getenv("FLBR_USER_DATA");
            if (userData && *userData) {
                m_userDataPath = userData;
            } else {
                NSString* documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
                m_userDataPath = std::string(documents.UTF8String) + "/Image-Line/FL Studio";
            }
            if (m_userDataPath.back() != '/') m_userDataPath += '/';
            if (!prepareUserData()) return false;

            // 2. Preload FL Studio Shared libraries with RTLD_GLOBAL so Fruity plugins resolve dependencies
            m_flSharedPath = m_flRootPath + "Shared/";
            const auto& sharedDir = m_flSharedPath;
            const char* preloadLibs[] = {
                "libwinbridge_x64.dylib",
                "MacVclCocoa_x64.framework/Versions/A/MacVclCocoa_x64",
                "libfreetype_x64.dylib",
                "libblend2d.dylib",
                "libQuickFontCachev2_x64.dylib",
                "dsp_ippv2_x64.dylib",
                "ildsp_x64.dylib",
                "SGv2_x64.dylib",
                "ILSvg_x64.dylib",
                "ILTools_x64.dylib",
                "elastique_x64.dylib",
                "libreverb_x64.dylib",
                "wavpack_x64.dylib",
                "liboggio_x64.dylib",
                "EasyFLAC_x64.dylib",
                "libmp3lame_x64.dylib",
                nullptr
            };
            for (int i = 0; preloadLibs[i]; ++i) {
                std::string libPath = sharedDir + preloadLibs[i];
                if (pathExists(libPath)) {
                    dlopen(libPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
                }
            }
            m_initialized = true;
        }

        if (!m_discovered || rescan) {
            for (auto& entry : m_plugins) entry.available = false;
            scanDirectory(m_flRootPath + "Plugins/Fruity/Generators", true);
            scanDirectory(m_flRootPath + "Plugins/Fruity/Effects", false);
            m_discovered = true;
        }
    }
    return true;
}

void FLPluginScanner::scanDirectory(const std::string& baseDir, bool isGenerator) {
    // Internal FL Studio host-only utilities/wrappers to exclude
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* nsBase = [NSString stringWithUTF8String:baseDir.c_str()];
    NSError* error = nil;
    NSArray<NSString*>* folders = [fm contentsOfDirectoryAtPath:nsBase error:&error];
    if (!folders) {
        fprintf(stderr, "FloopHole: cannot discover %s: %s\n", baseDir.c_str(), error.localizedDescription.UTF8String);
        return;
    }

    std::vector<FLPluginEntry> found;
    for (NSString* folder in folders) {
        std::string name = [folder UTF8String];
        if (name.empty() || name[0] == '.') continue;
        if (isPluginExcluded(name)) continue;

        std::string plugDir = baseDir + "/" + name;
        std::string dylibPath = plugDir + "/" + name + "_x64.dylib";
        if (!pathExists(dylibPath)) {
            // Check case-insensitive or alternative *_x64.dylib in folder
            NSArray<NSString*>* files = [fm contentsOfDirectoryAtPath:[NSString stringWithUTF8String:plugDir.c_str()] error:nil];
            std::vector<std::string> alternatives;
            for (NSString* file in files) {
                if ([[file lowercaseString] isEqualToString:[[NSString stringWithFormat:@"%@_x64.dylib", folder] lowercaseString]]) {
                    dylibPath = plugDir + "/" + [file UTF8String];
                    break;
                }
                NSString* lower = file.lowercaseString;
                if (([lower hasSuffix:@"_x64.dylib"] || [lower hasSuffix:@"_arm64.dylib"]) &&
                    ![lower hasPrefix:@"engine_"]) alternatives.push_back(plugDir + "/" + file.UTF8String);
            }
            if (!pathExists(dylibPath) && alternatives.size() == 1) dylibPath = alternatives.front();
            else if (!pathExists(dylibPath) && alternatives.size() > 1)
                fprintf(stderr, "FloopHole: ambiguous native modules in %s\n", plugDir.c_str());
        }
        if (!pathExists(dylibPath)) continue;

        FLPluginEntry entry;
        entry.name = name;
        entry.isGenerator = isGenerator && name != "Fruity Dance";
        entry.category = isGenerator ? "Generator" : "Effect";
        entry.dylibPath = dylibPath;
        entry.pluginDir = plugDir + "/";
        entry.identifier = moduleIdentifier(entry.dylibPath);
        found.push_back(std::move(entry));
    }

    // Sort with priority instruments/effects first (Harmor #1 for Generators!)
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
            if (n == "FL Keys") return 13;
            if (n == "Slicex") return 14;
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
