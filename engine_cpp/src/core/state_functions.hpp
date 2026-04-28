#pragma once

#include "core/simulation.hpp"

namespace pf {

void runStateFunction(World& world, size_t fighterIndex, const FunctionCall& call);
void runStateFunctions(World& world, size_t fighterIndex, const std::vector<FunctionCall>& calls);

} // namespace pf

