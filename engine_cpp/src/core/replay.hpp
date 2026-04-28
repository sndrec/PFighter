#pragma once

#include "core/input.hpp"

#include <array>
#include <string>
#include <vector>

namespace pf {

struct ReplayFrame {
    std::array<InputFrame, 2> inputs{};
};

struct ReplayData {
    int p1FighterDef = 0;
    int p2FighterDef = 0;
    std::vector<ReplayFrame> frames;
};

bool saveReplay(const std::string& path, const ReplayData& replay, std::string* error = nullptr);
bool loadReplay(const std::string& path, ReplayData& replay, std::string* error = nullptr);

} // namespace pf
