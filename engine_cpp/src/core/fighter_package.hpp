#pragma once

#include "core/simulation.hpp"

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
