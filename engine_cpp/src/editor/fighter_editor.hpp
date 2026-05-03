#pragma once

#include "core/simulation.hpp"

#include <string>

namespace pf {

enum class EditorWorkspace : uint8_t {
    Moveset,
    Logic,
    Assets,
    Animation,
    TestLab,
};

struct FighterEditor {
    int selectedFighter = 0;
    int selectedState = 0;
    int selectedSubaction = 0;
    int selectedPackageVariable = 0;
    int selectedPackageScript = 0;
    int selectedPackageInstruction = 0;
    int selectedObjectDef = 0;
    int selectedAnimationClip = 0;
    int animationScrubFrame = 0;
    bool showBoxes = true;
    bool paused = false;
    bool sideView = false;
    bool testMode = false;
    bool animationPreviewActive = false;
    EditorWorkspace workspace = EditorWorkspace::Moveset;
    std::string packagePath = "editor_last.pfpkg";
    std::string status = "Editor: T or Test launches current in-memory fighter on Battlefield";

    void clampToWorld(const World& world);
};

} // namespace pf
