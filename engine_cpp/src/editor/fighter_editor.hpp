#pragma once

#include "core/fighter_package.hpp"
#include "core/simulation.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
    int selectedAuthoredMeshVertex = 0;
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

struct FighterEditorPackageSnapshot {
    FighterPackage package;
    std::vector<uint8_t> bytes;
    FighterPackageDescriptor descriptor;
};

struct FighterEditorSession {
    FighterPackage package;
    int selectedFighter = 0;
    int selectedState = 0;
    int selectedSubaction = 0;
    int selectedInterrupt = 0;
    bool dirty = false;
    FighterPackageDescriptor lastDescriptor;
    std::vector<uint8_t> lastBytes;
    std::string lastMessage;

    void clamp();
    FighterDefinition* rootFighter();
    const FighterDefinition* rootFighter() const;
};

std::string uniqueEditorFighterName(const World& world, const std::string& prefix);
std::string uniqueEditorStateName(const FighterDefinition& def, const std::string& prefix);
bool editorFighterStateNameAvailable(const FighterDefinition& def, const std::string& name, int ignoredIndex = -1);
void remapEditorFighterStateTargets(FighterDefinition& def, const std::string& oldStateName, const std::string& newStateName);
void remapRemovedEditorFighterStateTargets(
    FighterDefinition& def,
    const std::string& removedStateName,
    const std::string& replacementStateName);
void ensureFighterEditorAuthoredRootJoint(FighterDefinition& def);
void normalizeFighterEditorAuthoredEcb(FighterDefinition& def);
FighterDefinition makeFighterEditorBlankDefinition(const std::string& name, const MeleeCommonData& common);
GameObjectDefinition makeFighterEditorObjectDefinition(const std::string& name, GameObjectKind kind);
FighterPackage makeEditorFighterPackage(const World& world, int rootFighterDef, const std::string& packageName = {});

bool beginFighterEditorSessionFromWorld(
    const World& world,
    int rootFighterDef,
    FighterEditorSession& session,
    std::string* error = nullptr,
    const std::string& packageName = {});
bool beginBlankFighterEditorSession(
    const std::string& fighterName,
    const MeleeCommonData& common,
    FighterEditorSession& session,
    std::string* error = nullptr);
bool loadFighterEditorSessionPackage(
    const std::vector<uint8_t>& bytes,
    FighterEditorSession& session,
    std::string* error = nullptr,
    const std::vector<std::shared_ptr<const HsdFighterAnimationAsset>>& hsdAssetPool = {});
bool exportFighterEditorSessionPackage(
    FighterEditorSession& session,
    FighterEditorPackageSnapshot& snapshot,
    std::string* error = nullptr);
bool makeFighterEditorSessionTestWorld(
    FighterEditorSession& session,
    World& world,
    int* rootFighterDef = nullptr,
    FighterPackageDescriptor* descriptor = nullptr,
    std::string* error = nullptr);

bool createEditorSessionState(
    FighterEditorSession& session,
    const std::string& requestedName = {},
    int sourceStateIndex = -1,
    int* createdStateIndex = nullptr,
    std::string* error = nullptr);
bool duplicateEditorSessionState(
    FighterEditorSession& session,
    int sourceStateIndex = -1,
    int* createdStateIndex = nullptr,
    std::string* error = nullptr);
bool renameEditorSessionState(
    FighterEditorSession& session,
    int stateIndex,
    const std::string& newName,
    std::string* error = nullptr);
bool removeEditorSessionState(
    FighterEditorSession& session,
    int stateIndex,
    const std::string& replacementStateName = {},
    std::string* error = nullptr);
bool addEditorSessionObject(
    FighterEditorSession& session,
    const std::string& requestedName,
    GameObjectKind kind,
    int* objectIndex = nullptr,
    std::string* error = nullptr);

} // namespace pf
