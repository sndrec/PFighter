#pragma once

#include "core/fighter_data.hpp"
#include "core/imported_fighter_asset.hpp"

#include <vector>

namespace pf {

std::vector<Subaction> decodeHsdActionScript(const HsdFighterAnimationAsset& asset, const HsdActionScript& script);

} // namespace pf
