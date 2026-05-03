#pragma once

#include "core/simulation.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace pf {

enum class EditorWorkspace : uint8_t {
    Moveset,
    Logic,
    Assets,
    Animation,
    TestLab,
};

enum class ObjectEditorPanel : uint8_t {
    Logic,
    Boxes,
};

struct FighterEditor {
    int selectedFighter = 0;
    int selectedState = 0;
    int selectedSubaction = 0;
    int selectedInterrupt = 0;
    int selectedPackageVariable = 0;
    int selectedPackageScript = 0;
    int selectedPackageInstruction = 0;
    int selectedObjectDef = 0;
    int selectedObjectState = 0;
    int selectedObjectHitbox = 0;
    int selectedObjectHurtbox = 0;
    int selectedObjectTouchbox = 0;
    int selectedObjectStateCallback = 0;
    int selectedObjectEventCallback = 0;
    ObjectEditorPanel objectPanel = ObjectEditorPanel::Logic;
    int selectedAnimationClip = 0;
    int selectedAnimationJoint = 0;
    int selectedAnimationTrack = 0;
    int selectedAnimationKey = 0;
    int animationScrubFrame = 0;
    int selectedHurtbox = 0;
    bool showBoxes = true;
    bool paused = false;
    bool sideView = false;
    bool testMode = false;
    bool animationPreviewActive = false;
    EditorWorkspace workspace = EditorWorkspace::Moveset;
    std::string packagePath = "editor_last.pfpkg";
    std::string lastPackageName;
    size_t lastPackageBytes = 0;
    uint32_t lastPackageChecksum = 0;
    int lastPackageFighters = 0;
    int lastPackageObjects = 0;
    int lastPackageAssets = 0;
    bool lastPackageValid = false;
    std::string lastPackageMessage;
    std::string activeTextField;
    std::string textEditBuffer;
    std::string status = "Editor: T or Test launches current in-memory fighter on Battlefield";

    void clampToWorld(const World& world);
};

} // namespace pf
