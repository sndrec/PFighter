#pragma once

#include "core/simulation.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pf {

struct FighterPackage {
    std::string name;
    uint32_t version = 1;
    std::vector<FighterDefinition> fighters;
    std::vector<GameObjectDefinition> objects;
    std::vector<std::shared_ptr<const HsdFighterAnimationAsset>> hsdAssets;
};

struct FighterPackageDescriptor {
    std::string name;
    uint32_t version = 0;
    size_t byteSize = 0;
    uint32_t checksum = 0;
    std::string rootFighterName;
    std::vector<std::string> fighterNames;
    std::vector<std::string> objectNames;
    std::vector<std::string> assetNames;
    std::vector<std::string> fighterScriptNames;
    std::vector<std::string> objectScriptNames;
};

struct FighterPackageCacheEntry {
    FighterPackageDescriptor descriptor;
    std::vector<uint8_t> bytes;
};

class FighterPackageCache {
public:
    bool store(
        const std::vector<uint8_t>& bytes,
        FighterPackageDescriptor* descriptor = nullptr,
        std::string* error = nullptr);
    bool storeExpected(
        const std::vector<uint8_t>& bytes,
        const FighterPackageDescriptor& expected,
        FighterPackageDescriptor* descriptor = nullptr,
        std::string* error = nullptr);
    const FighterPackageCacheEntry* find(uint32_t checksum) const;
    const FighterPackageDescriptor* descriptor(uint32_t checksum) const;
    const std::vector<uint8_t>* packageBytes(uint32_t checksum) const;
    bool contains(uint32_t checksum) const;
    size_t size() const;

private:
    std::vector<FighterPackageCacheEntry> entries_;
};

bool fighterPackageDescriptorMatches(
    const FighterPackageDescriptor& expected,
    const FighterPackageDescriptor& actual);
bool validateFighterPackage(const FighterPackage& package, std::string* error = nullptr);
bool describeFighterPackage(
    const FighterPackage& package,
    FighterPackageDescriptor& descriptor,
    const std::vector<uint8_t>& bytes = {},
    std::string* error = nullptr);
bool describeFighterPackageBytes(
    const std::vector<uint8_t>& bytes,
    FighterPackageDescriptor& descriptor,
    std::string* error = nullptr);
std::vector<uint8_t> writeFighterPackage(const FighterPackage& package, std::string* error = nullptr);
bool saveFighterPackage(const std::string& path, const FighterPackage& package, std::string* error = nullptr);
bool readFighterPackage(
    const std::vector<uint8_t>& bytes,
    FighterPackage& package,
    std::string* error = nullptr,
    const std::vector<std::shared_ptr<const HsdFighterAnimationAsset>>& hsdAssetPool = {});
bool loadFighterPackage(
    const std::string& path,
    FighterPackage& package,
    std::string* error = nullptr,
    const std::vector<std::shared_ptr<const HsdFighterAnimationAsset>>& hsdAssetPool = {});
uint32_t fighterPackageChecksum(const std::vector<uint8_t>& bytes);

} // namespace pf
