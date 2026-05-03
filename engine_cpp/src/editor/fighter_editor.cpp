#include "editor/fighter_editor.hpp"

#include <algorithm>

namespace pf {

void FighterEditor::clampToWorld(const World& world) {
    if (world.fighters.empty()) {
        selectedFighter = 0;
        selectedState = 0;
        selectedSubaction = 0;
        return;
    }
    selectedFighter = std::clamp(selectedFighter, 0, static_cast<int>(world.fighters.size()) - 1);
    const FighterRuntime& fighter = world.fighters[static_cast<size_t>(selectedFighter)];
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    selectedState = std::clamp(selectedState, 0, static_cast<int>(def.states.size()) - 1);
    const FighterState& state = def.states[static_cast<size_t>(selectedState)];
    selectedSubaction = std::clamp(selectedSubaction, 0, std::max(0, static_cast<int>(state.action.size()) - 1));
    selectedPackageVariable = std::clamp(
        selectedPackageVariable,
        0,
        std::max(0, static_cast<int>(def.packageVariables.size()) - 1));
    selectedPackageScript = std::clamp(
        selectedPackageScript,
        0,
        std::max(0, static_cast<int>(def.packageScripts.size()) - 1));
    if (def.packageScripts.empty()) {
        selectedPackageInstruction = 0;
    } else {
        const PackageScript& script = def.packageScripts[static_cast<size_t>(selectedPackageScript)];
        selectedPackageInstruction = std::clamp(
            selectedPackageInstruction,
            0,
            std::max(0, static_cast<int>(script.instructions.size()) - 1));
    }
    selectedObjectDef = std::clamp(
        selectedObjectDef,
        0,
        std::max(0, static_cast<int>(world.objectDefs.size()) - 1));
    if (world.objectDefs.empty()) {
        selectedObjectState = 0;
    } else {
        const GameObjectDefinition& object = world.objectDefs[static_cast<size_t>(selectedObjectDef)];
        selectedObjectState = std::clamp(
            selectedObjectState,
            0,
            std::max(0, static_cast<int>(object.states.size()) - 1));
        selectedObjectHitbox = std::clamp(
            selectedObjectHitbox,
            0,
            std::max(0, static_cast<int>(object.hitboxes.size()) - 1));
        selectedObjectHurtbox = std::clamp(
            selectedObjectHurtbox,
            0,
            std::max(0, static_cast<int>(object.hurtboxes.size()) - 1));
        selectedObjectTouchbox = std::clamp(
            selectedObjectTouchbox,
            0,
            std::max(0, static_cast<int>(object.touchboxes.size()) - 1));
    }
    const bool useImportedClips = def.hsdAsset && !def.hsdAsset->clips.empty();
    const std::vector<AnimationClip>& clips = useImportedClips ? def.hsdAsset->clips : def.authoredClips;
    const int clipCount = static_cast<int>(clips.size());
    selectedAnimationClip = std::clamp(selectedAnimationClip, 0, std::max(0, clipCount - 1));
    selectedAnimationJoint = std::clamp(
        selectedAnimationJoint,
        0,
        std::max(0, static_cast<int>(def.authoredSkeleton.size()) - 1));
    if (clipCount > 0) {
        const AnimationClip& clip = clips[static_cast<size_t>(selectedAnimationClip)];
        selectedAnimationTrack = std::clamp(
            selectedAnimationTrack,
            0,
            std::max(0, static_cast<int>(clip.tracks.size()) - 1));
        if (!clip.tracks.empty()) {
            const AnimationTrack& track = clip.tracks[static_cast<size_t>(selectedAnimationTrack)];
            selectedAnimationKey = std::clamp(
                selectedAnimationKey,
                0,
                std::max(0, static_cast<int>(track.keys.size()) - 1));
        } else {
            selectedAnimationKey = 0;
        }
    } else {
        selectedAnimationTrack = 0;
        selectedAnimationKey = 0;
    }
    if (clipCount == 0) {
        animationScrubFrame = 0;
    }
    selectedHurtbox = std::clamp(
        selectedHurtbox,
        0,
        std::max(0, static_cast<int>(def.hurtboxes.size()) - 1));
}

} // namespace pf
