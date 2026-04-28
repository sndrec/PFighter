#pragma once

#include "core/simulation.hpp"

namespace pf {

struct FighterEditor {
    int selectedFighter = 0;
    int selectedState = 0;
    int selectedSubaction = 0;
    bool showBoxes = true;
    bool paused = false;
    bool sideView = false;

    void clampToWorld(const World& world);
};

} // namespace pf
