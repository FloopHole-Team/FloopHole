#include "FLPathManager.h"
#include "FLPluginScanner.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace flbr {
namespace {
namespace fs = std::filesystem;

bool copyPath(const std::string& path, char* buffer) {
    if (!buffer) return false;
    buffer[0] = 0;
    if (path.empty()) return false;
    if (path.size() >= PathBufferLength) {
        fprintf(stderr, "FloopHole: path exceeds the Fruity SDK buffer: %s\n", path.c_str());
        return false;
    }
    memcpy(buffer, path.c_str(), path.size() + 1);
    return true;
}

fs::path nativePath(const char* value) {
    std::string path = value ? value : "";
    std::replace(path.begin(), path.end(), '\\', '/');
    return fs::u8path(path);
}

bool matches(const char* pattern, const char* text) {
    if (!pattern || strcmp(pattern, "*.*") == 0) return true;
    if (!*pattern) return !*text;
    if (*pattern == '*') {
        return matches(pattern + 1, text) || (*text && matches(pattern, text + 1));
    }
    if (!*text) return !*pattern;
    const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; };
    return (*pattern == '?' || lower(*pattern) == lower(*text)) && matches(pattern + 1, text + 1);
}

bool matchesExtensions(const std::string& extensions, const fs::path& path) {
    if (extensions.empty()) return true;
    size_t start = 0;
    do {
        const auto end = extensions.find('|', start);
        std::string ext = extensions.substr(start, end - start);
        if (!ext.empty() && ext.front() != '*' && ext.front() != '.') ext.insert(0, ".");
        if (!ext.empty() && ext.front() == '.') ext.insert(0, "*");
        if (matches(ext.c_str(), path.filename().u8string().c_str())) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    } while (start < extensions.size());
    return false;
}

struct FoundFile {
    fs::path path;
    fs::path relative;
    uint8_t index;
    bool directory;
};

std::vector<FoundFile> findFiles(uint32_t id, const char* subPath, uint32_t indexes, uint32_t options) {
    std::vector<FoundFile> result;
    for (uint8_t index : {PI_User, PI_Factory}) {
        if (indexes && !(indexes & (1u << index))) continue;
        const auto root = FLPluginScanner::getInstance().getDataPath(id, index);
        if (root.empty()) continue;
        const auto base = fs::u8path(root) / nativePath(subPath);
        std::error_code error;
        if (!fs::is_directory(base, error)) {
            if (error && error != std::errc::no_such_file_or_directory)
                fprintf(stderr, "FloopHole: cannot scan %s: %s\n", base.u8string().c_str(), error.message().c_str());
            continue;
        }
        auto collect = [&](const fs::directory_entry& entry) {
            if (!(options & (1u << 5)) && entry.path().filename().u8string().front() == '.') return;
            const bool directory = entry.is_directory(error);
            if (!error) result.push_back({entry.path(), entry.path().lexically_relative(base), index, directory});
        };
        if (options & (1u << 3)) {
            for (fs::recursive_directory_iterator it(base, error), end; !error && it != end; it.increment(error))
                collect(*it);
        } else {
            for (fs::directory_iterator it(base, error), end; !error && it != end; it.increment(error))
                collect(*it);
        }
        if (error) fprintf(stderr, "FloopHole: cannot scan %s: %s\n", base.u8string().c_str(), error.message().c_str());
    }
    return result;
}
} // namespace

int32_t FLPathManager::getPath(uint32_t id, uint8_t index, char* buffer) {
    const auto path = FLPluginScanner::getInstance().getDataPath(id, index);
    if (std::getenv("FLBR_TRACE_HOST"))
        fprintf(stderr, "PathManager.GetPath id=%u index=%u path=%s\n", id, index, path.c_str());
    return copyPath(path, buffer);
}

uint8_t FLPathManager::getPathIndexFor(uint32_t id, char* fileName) {
    const auto file = nativePath(fileName).lexically_normal();
    for (uint8_t index : {PI_User, PI_Factory}) {
        const auto root = FLPluginScanner::getInstance().getDataPath(id, index);
        if (root.empty()) continue;
        const auto rel = file.lexically_relative(fs::u8path(root));
        if (!rel.empty() && *rel.begin() != "..") return index;
    }
    return 255;
}

int32_t FLPathManager::scanDir(uint32_t id, char* subPath, char* extensions, FLSearchDirList* list,
                              uint32_t options, uint32_t indexes, FLScanDirCallback* callback) {
    auto files = findFiles(id, subPath, indexes, options);
    int32_t count = 0;
    for (const auto& file : files) {
        if (file.directory || !matchesExtensions(extensions ? extensions : "", file.path)) continue;
        if (options & (1u << 4)) return 1;
        fs::path name = options & (1u << 1) ? file.relative : file.path;
        if (options & (1u << 2)) name.replace_extension();
        std::string text = name.u8string();
        intptr_t tag = options & (1u << 7) ? file.index :
            options & (1u << 6) ? (static_cast<intptr_t>(file.index) << 24) | count : count;
        if (callback) callback->itemFound(count, file.index, text.data(), tag);
        if (list) list->add(text.data(), tag);
        ++count;
    }
    if (list && (options & 1)) list->sort();
    return count > 0;
}

void FLPathManager::searchDir(uint32_t id, char* subPath, char* mask, int32_t attributes,
                             FLSearchDirCallback* callback, uint32_t indexes, uint32_t options) {
    if (!callback) return;
    auto files = findFiles(id, subPath, indexes, options);
    if (options & 1) std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return a.path < b.path;
    });
    for (const auto& file : files) {
        if (file.directory && !(attributes & 0x10)) continue;
        std::string name = file.path.filename().u8string();
        if (!matches(mask, name.c_str())) continue;
        std::string path = file.path.parent_path().u8string() + static_cast<char>(fs::path::preferred_separator);
        int32_t stop = 0;
        callback->itemFound(file.index, name.data(), path.data(), file.directory ? 0x10 : 0x20, 0, stop);
        if (stop) break;
    }
}

int32_t FLPathManager::addPath(uint32_t id, char* fileName, uint32_t indexes, uint32_t options, char* buffer) {
    if (buffer) buffer[0] = 0;
    const auto file = nativePath(fileName);
    for (uint8_t index : {PI_User, PI_Factory}) {
        if (indexes && !(indexes & (1u << index))) continue;
        const auto root = FLPluginScanner::getInstance().getDataPath(id, index);
        if (root.empty()) continue;
        auto path = file.is_absolute() ? file : fs::u8path(root) / file;
        path = path.lexically_normal();
        std::error_code error;
        if (fs::exists(path, error) || (options & 4)) return copyPath(path.u8string(), buffer);
        if (error) {
            fprintf(stderr, "FloopHole: cannot access %s: %s\n", path.u8string().c_str(), error.message().c_str());
            return 0;
        }
        // Never create directories in the FL Studio installation.
        if ((options & 1) && index == PI_User) {
            fs::create_directories(path, error);
            if (!error) return copyPath(path.u8string(), buffer);
            fprintf(stderr, "FloopHole: cannot create %s: %s\n", path.u8string().c_str(), error.message().c_str());
            return 0;
        }
    }
    return 0;
}

int32_t FLPathManager::addPathWithExt(uint32_t id, char* fileName, char* extension, uint32_t indexes,
                                    uint32_t options, char* buffer) {
    std::string file = fileName ? fileName : "";
    if (extension && *extension) {
        if (*extension != '.') file += '.';
        file += extension;
    }
    return addPath(id, file.data(), indexes, options, buffer);
}

int32_t FLPathManager::locate(uint32_t id, char* fileName, int32_t (*progress)(), uint32_t,
                             uint32_t indexes, char* buffer) {
    if (addPath(id, fileName, indexes, 0, buffer)) return 1;
    for (const auto& file : findFiles(id, "", indexes, 1u << 3)) {
        if (progress && !progress()) return 0;
        if (!file.directory && matches(fileName, file.path.filename().u8string().c_str()))
            return copyPath(file.path.u8string(), buffer);
    }
    return 0;
}

void FLPathManager::randomDirFile(uint32_t id, char* subPath, char* extensions, uint32_t options,
                                 uint32_t indexes, double value, char* buffer) {
    auto files = findFiles(id, subPath, indexes, options);
    files.erase(std::remove_if(files.begin(), files.end(), [&](const auto& file) {
        return file.directory || !matchesExtensions(extensions ? extensions : "", file.path);
    }), files.end());
    if (files.empty()) { copyPath("", buffer); return; }
    const double position = std::isfinite(value) && value >= 0 ? std::clamp(value, 0.0, 1.0) :
        static_cast<double>(std::rand()) / RAND_MAX;
    const size_t index = std::min(static_cast<size_t>(position * files.size()), files.size() - 1);
    copyPath(files[index].path.u8string(), buffer);
}

void FLPathManager::removePath(uint32_t id, char* fileName, char* buffer) {
    const uint8_t index = getPathIndexFor(id, fileName);
    const auto root = FLPluginScanner::getInstance().getDataPath(id, index);
    copyPath(root.empty() ? (fileName ? fileName : "") :
        nativePath(fileName).lexically_relative(fs::u8path(root)).u8string(), buffer);
}

int32_t FLPathManager::getRootPath(uint8_t pathIndex, uint32_t index, char* buffer) {
    if (index != 0) {
        fprintf(stderr, "FloopHole: unsupported root path index %u\n", index);
        return copyPath("", buffer);
    }
    return getPath(0, pathIndex, buffer);
}

int32_t FLPathManager::decomposeFileName(char* fileName, char*, uint32_t& id,
                                       uint8_t& index, char* buffer) {
    for (uint32_t candidate : {FLData_SharedPacks, FLData_PluginPresets, FLData_Packs,
                               FLData_GeneratorPlugins, FLData_EffectPlugins, uint32_t{0}}) {
        index = getPathIndexFor(candidate, fileName);
        if (index == 255) continue;
        id = candidate;
        removePath(id, fileName, buffer);
        return 1;
    }
    id = 0;
    index = 255;
    copyPath("", buffer);
    return 0;
}

void FLPathManager::getPathIDAndIndexFor(char* fileName, char* context, uint32_t& id, uint8_t& index) {
    char buffer[PathBufferLength];
    decomposeFileName(fileName, context, id, index, buffer);
}
} // namespace flbr
