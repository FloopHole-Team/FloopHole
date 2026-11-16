#pragma once

#include <cstdint>

namespace flbr {

constexpr uint8_t PI_User = 0;
constexpr uint8_t PI_Factory = 1;
constexpr uint32_t FLData_PluginPresets = 1011;
constexpr uint32_t FLData_Scores = 1017;
constexpr uint32_t FLData_EffectPlugins = 1019;
constexpr uint32_t FLData_GeneratorPlugins = 1020;
constexpr uint32_t FLData_Internet = 1021;
constexpr uint32_t FLData_SystemData = 1025;
constexpr uint32_t FLData_Packs = 1037;
constexpr uint32_t FLData_SharedPacks = 1045;
constexpr uint32_t FLData_AudioScripts = 1046;
constexpr uint32_t PathBufferLength = 1024;

struct FLScanDirCallback {
    virtual void itemFound(int32_t index, uint8_t pathIndex, char* fileName, intptr_t& tag) = 0;
};
struct FLSearchDirCallback {
    virtual void itemFound(uint8_t pathIndex, char* name, char* path, int32_t attributes,
                           double timeStamp, int32_t& stop) = 0;
};
struct FLSearchDirList {
    virtual void add(char* item, intptr_t tag) = 0;
    virtual void sort() = 0;
};

// Method order and argument widths are the Fruity SDK IPathManager ABI.
class FLPathManager {
public:
    virtual int32_t getPath(uint32_t id, uint8_t index, char* buffer);
    virtual uint8_t getPathIndexFor(uint32_t id, char* fileName);
    virtual int32_t scanDir(uint32_t id, char* subPath, char* extensions, FLSearchDirList* list,
                            uint32_t options, uint32_t indexes, FLScanDirCallback* callback);
    virtual void searchDir(uint32_t id, char* subPath, char* mask, int32_t attributes,
                           FLSearchDirCallback* callback, uint32_t indexes, uint32_t options);
    virtual int32_t locate(uint32_t id, char* fileName, int32_t (*progress)(), uint32_t flags,
                           uint32_t indexes, char* buffer);
    virtual void randomDirFile(uint32_t id, char* subPath, char* extensions, uint32_t options,
                               uint32_t indexes, double value, char* buffer);
    virtual int32_t addPath(uint32_t id, char* fileName, uint32_t indexes, uint32_t options, char* buffer);
    virtual int32_t addPathWithExt(uint32_t id, char* fileName, char* extension, uint32_t indexes,
                                  uint32_t options, char* buffer);
    virtual void removePath(uint32_t id, char* fileName, char* buffer);
    virtual int32_t decomposeFileName(char* fileName, char* context, uint32_t& id,
                                     uint8_t& index, char* buffer);
    virtual int32_t getRootPath(uint8_t pathIndex, uint32_t index, char* buffer);
    virtual void getPathIDAndIndexFor(char* fileName, char* context, uint32_t& id, uint8_t& index);
};

} // namespace flbr
