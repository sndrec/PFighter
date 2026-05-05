#pragma once

#include "core/fighter_data.hpp"

#include <vector>

namespace pf {

using UnfoldedAction = std::vector<std::vector<Subaction>>;

UnfoldedAction unfoldAction(const std::vector<Subaction>& action);
std::vector<int> subactionFirstFrames(const std::vector<Subaction>& action);
std::vector<Subaction> makeExplicitTimelineAction(const std::vector<Subaction>& action);

} // namespace pf
