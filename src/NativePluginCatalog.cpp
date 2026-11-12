#include "NativePluginCatalog.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <CommonCrypto/CommonDigest.h>
#endif

namespace flbr {

std::optional<std::string> nativeClassId(const std::string& identifier) {
    if (identifier.empty() || identifier.size() > 4096) {
        fprintf(stderr, "FloopHole: invalid native class identifier\n");
        return std::nullopt;
    }
    // This namespace and identity encoding are permanent, independent of product versions.
    const std::string input = "org.floophole.vst3.native.v1:" + identifier;
    std::array<unsigned char, 32> digest{};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (status >= 0) status = BCryptHashData(hash,
        reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), 0);
    if (status >= 0) status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        fprintf(stderr, "FloopHole: class ID hashing failed (NTSTATUS=0x%08lx)\n", static_cast<unsigned long>(status));
        return std::nullopt;
    }
#else
    if (!CC_SHA256(input.data(), static_cast<CC_LONG>(input.size()), digest.data())) {
        fprintf(stderr, "FloopHole: class ID hashing failed\n");
        return std::nullopt;
    }
#endif
    digest[6] = (digest[6] & 15) | 0x80;
    digest[8] = (digest[8] & 63) | 0x80;
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (size_t i = 0; i < 16; ++i) {
        result += hex[digest[i] >> 4];
        result += hex[digest[i] & 15];
    }
    return result;
}

std::vector<NativePluginClass> buildNativePluginClasses(const std::vector<FLPluginEntry>& entries) {
    std::map<std::string, size_t> identities;
    for (const auto& entry : entries) if (entry.available) ++identities[entry.identifier];
    std::map<std::string, std::string> ids;
    std::vector<NativePluginClass> result;
    for (const auto& entry : entries) {
        if (!entry.available) continue;
        if (entry.identifier.empty() || identities[entry.identifier] != 1) {
            fprintf(stderr, "FloopHole: excluding missing/ambiguous native identifier '%s' (%s)\n",
                entry.identifier.c_str(), entry.name.c_str());
            continue;
        }
        const auto cid = nativeClassId(entry.identifier);
        if (!cid) continue;
        if (!ids.emplace(*cid, entry.identifier).second) {
            fprintf(stderr, "FloopHole: native class ID collision for %s\n", entry.identifier.c_str());
            return {};
        }
        result.push_back({entry.identifier, entry.name, *cid, entry.isGenerator});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        const auto asciiLower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
        const auto compare = [&](unsigned char left, unsigned char right) { return asciiLower(left) < asciiLower(right); };
        if (std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(), compare)) return true;
        if (std::lexicographical_compare(b.name.begin(), b.name.end(), a.name.begin(), a.name.end(), compare)) return false;
        return a.identifier < b.identifier;
    });
    return result;
}

std::vector<NativePluginClass> discoverNativePluginClasses() {
    auto& scanner = FLPluginScanner::getInstance();
    if (!scanner.discover(true)) return {};
    return buildNativePluginClasses(scanner.getDiscoverySnapshot());
}

}
