#pragma once

#include "core/fighter_package.hpp"

#include <string>
#include <vector>

namespace pf {

std::vector<std::string> meleeTrainingRosterFighterNames();
bool makeConvertedMeleeFighterPackage(
    const std::string& fighterName,
    FighterPackage& package,
    std::string* error = nullptr);
bool saveConvertedMeleeFighterPackage(
    const std::string& fighterName,
    const std::string& path,
    std::string* error = nullptr);
bool makeConvertedMeleeFighterPackageFromAssetBin(
    const std::string& displayName,
    const std::string& sourceFileName,
    const std::string& assetBinPath,
    FighterPackage& package,
    std::string* error = nullptr);
bool saveConvertedMeleeFighterPackageFromAssetBin(
    const std::string& displayName,
    const std::string& sourceFileName,
    const std::string& assetBinPath,
    const std::string& path,
    std::string* error = nullptr);

} // namespace pf
