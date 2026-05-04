#pragma once

#include "core/fighter_data.hpp"

#include <vector>

namespace pf {

std::vector<Subaction> decodeHsdActionScript(const HsdFighterAnimationAsset& asset, const HsdActionScript& script);

} // namespace pf
