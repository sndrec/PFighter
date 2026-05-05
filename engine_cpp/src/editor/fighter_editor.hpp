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

enum class FighterEditorTimelineMarkerKind : uint8_t {
    Subaction,
    Hitbox,
    ThrowHitbox,
    Callback,
    AnimationKey,
    Interruptible,
    InterruptEnable,
    InterruptDisable,
};

enum class FighterEditorStateCallbackSlot : uint8_t {
    Enter,
    Frame,
    Landing,
    Airborne,
};

enum class FighterEditorObjectStateCallbackSlot : uint8_t {
    Enter,
    Frame,
    Physics,
    Collision,
};

enum class FighterEditorObjectEventCallbackSlot : uint8_t {
    Spawned,
    Destroyed,
    PickedUp,
    Dropped,
    Thrown,
    DamageDealt,
    DamageReceived,
    Clanked,
    Reflected,
    Absorbed,
    ShieldBounced,
    HitShield,
    EnteredAir,
    EnteredHitlag,
    ExitedHitlag,
    Accessory,
    Touched,
    JumpedOn,
    GrabDealt,
    GrabbedForVictim,
    Interaction,
};

enum class FighterEditorSelectionKind : uint8_t {
    State,
    Subaction,
    Interrupt,
    Callback,
    ObjectCallback,
    Script,
    Instruction,
    Variable,
    Object,
    Animation,
    Viewport,
};

enum class FighterEditorStateGroupFilter : uint8_t {
    All,
    Ground,
    Air,
    Attack,
    Special,
    Throw,
    Ledge,
    Damage,
    Other,
};

struct FighterEditorTimelineMarker {
    FighterEditorTimelineMarkerKind kind = FighterEditorTimelineMarkerKind::Subaction;
    int frame = 0;
    int sourceIndex = -1;
    SubactionType subactionType = SubactionType::SyncTimer;
    InterruptCondition interruptCondition = InterruptCondition::JumpPressed;
    FighterEditorStateCallbackSlot callbackSlot = FighterEditorStateCallbackSlot::Frame;
    int animationClipIndex = -1;
    int animationTrackIndex = -1;
    int animationKeyIndex = -1;
};

struct FighterEditorStateTimeline {
    int animationLengthFrames = 0;
    int actionLengthFrames = 0;
    int frameCount = 0;
    int initialInterruptibleFrame = 0;
    std::vector<int> subactionFrames;
    std::vector<FighterEditorTimelineMarker> markers;
};

struct FighterEditorPreviewFrame {
    int frame = 0;
    uint32_t rngState = 0x4D454C45;
    std::vector<FighterRuntime> fighters;
    std::vector<GameObjectRuntime> objects;
};

struct FighterEditor {
    int selectedFighter = 0;
    int selectedState = 0;
    int selectedSubaction = 0;
    int selectedInterrupt = 0;
    FighterEditorStateCallbackSlot selectedStateCallbackSlot = FighterEditorStateCallbackSlot::Enter;
    int selectedStateCallback = 0;
    int selectedPackageVariable = 0;
    int selectedPackageScript = 0;
    int selectedPackageInstruction = 0;
    int selectedPackageGraphNode = 0;
    FighterEditorSelectionKind selectionKind = FighterEditorSelectionKind::State;
    int selectedObjectDef = 0;
    int selectedObjectState = 0;
    int selectedObjectHitbox = 0;
    int selectedObjectHurtbox = 0;
    int selectedObjectTouchbox = 0;
    int selectedObjectStateCallback = 0;
    int selectedObjectEventCallback = 0;
    int selectedObjectCallback = 0;
    bool selectedObjectCallbackEvent = false;
    ObjectEditorPanel objectPanel = ObjectEditorPanel::Logic;
    int selectedAnimationClip = 0;
    int selectedAnimationJoint = 0;
    int selectedAnimationTrack = 0;
    int selectedAnimationKey = 0;
    int animationScrubFrame = 0;
    int selectedAuthoredMeshVertex = 0;
    int selectedHurtbox = 0;
    bool showBoxes = true;
    bool showModel = true;
    bool showHitboxes = true;
    bool showHurtboxes = true;
    bool showEcb = true;
    bool showLedgeBoxes = true;
    bool showSkeleton = true;
    bool paused = false;
    bool sideView = false;
    bool testMode = false;
    bool animationPreviewActive = false;
    bool uiRefreshPending = false;
    bool previewCacheDirty = true;
    bool previewCacheValid = false;
    int previewCacheFighter = -1;
    int previewCacheState = -1;
    int previewCacheFrame = 0;
    int previewCacheFrameCount = 0;
    StageDefinition previewCacheStage;
    std::vector<FighterDefinition> previewCacheFighterDefs;
    std::vector<GameObjectDefinition> previewCacheObjectDefs;
    std::vector<FighterEditorPreviewFrame> previewCacheFrames;
    std::string previewCacheMessage;
    EditorWorkspace workspace = EditorWorkspace::Moveset;
    FighterEditorStateGroupFilter stateGroupFilter = FighterEditorStateGroupFilter::All;
    std::string stateSearch;
    int stateListScroll = 0;
    int editorOpenListScroll = 0;
    int timelineLaneScroll = 0;
    int inspectorScroll = 0;
    bool diagnosticsCollapsed = false;
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
    int selectedPackageScript = 0;
    int selectedPackageInstruction = 0;
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
    std::string* error = nullptr);
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

bool editorPackageFighterNameAvailable(
    const FighterPackage& package,
    const std::string& name,
    int ignoredIndex = -1);
std::string uniqueEditorPackageFighterName(
    const FighterPackage& package,
    const std::string& prefix);
bool addEditorSessionPackageFighter(
    FighterEditorSession& session,
    const FighterDefinition& fighter,
    const std::string& requestedName = {},
    int* addedFighterIndex = nullptr,
    std::string* error = nullptr);
bool duplicateEditorSessionPackageFighter(
    FighterEditorSession& session,
    int fighterIndex,
    const std::string& requestedName = {},
    int* addedFighterIndex = nullptr,
    std::string* error = nullptr);
bool renameEditorSessionPackageFighter(
    FighterEditorSession& session,
    int fighterIndex,
    const std::string& newName,
    std::string* error = nullptr);
bool removeEditorSessionPackageFighter(
    FighterEditorSession& session,
    int fighterIndex,
    const std::string& replacementFighterName = {},
    std::string* error = nullptr);

bool createEditorSessionState(
    FighterEditorSession& session,
    const std::string& requestedName = {},
    int sourceStateIndex = -1,
    int* createdStateIndex = nullptr,
    std::string* error = nullptr);
bool createEditorSessionPackageFighterState(
    FighterEditorSession& session,
    int fighterIndex,
    const std::string& requestedName = {},
    int sourceStateIndex = -1,
    int* createdStateIndex = nullptr,
    std::string* error = nullptr);
bool duplicateEditorSessionState(
    FighterEditorSession& session,
    int sourceStateIndex = -1,
    int* createdStateIndex = nullptr,
    std::string* error = nullptr);
bool duplicateEditorSessionPackageFighterState(
    FighterEditorSession& session,
    int fighterIndex,
    int sourceStateIndex = -1,
    int* createdStateIndex = nullptr,
    std::string* error = nullptr);
bool renameEditorSessionState(
    FighterEditorSession& session,
    int stateIndex,
    const std::string& newName,
    std::string* error = nullptr);
bool renameEditorSessionPackageFighterState(
    FighterEditorSession& session,
    int fighterIndex,
    int stateIndex,
    const std::string& newName,
    std::string* error = nullptr);
bool removeEditorSessionState(
    FighterEditorSession& session,
    int stateIndex,
    const std::string& replacementStateName = {},
    std::string* error = nullptr);
bool removeEditorSessionPackageFighterState(
    FighterEditorSession& session,
    int fighterIndex,
    int stateIndex,
    const std::string& replacementStateName = {},
    std::string* error = nullptr);
bool addEditorSessionObject(
    FighterEditorSession& session,
    const std::string& requestedName,
    GameObjectKind kind,
    int* objectIndex = nullptr,
    std::string* error = nullptr);
bool buildEditorSessionStateTimeline(
    const FighterEditorSession& session,
    int stateIndex,
    FighterEditorStateTimeline& timeline,
    std::string* error = nullptr);
bool setEditorSessionStateTiming(
    FighterEditorSession& session,
    int stateIndex,
    int animationLengthFrames,
    int initialInterruptibleFrame,
    int defaultAnimationBlendFrames,
    int onAnimationFinishedBlendFrames,
    std::string* error = nullptr);
bool setEditorSessionStateLoop(
    FighterEditorSession& session,
    int stateIndex,
    bool loopAnimation,
    std::string* error = nullptr);
bool setEditorSessionStateAnimation(
    FighterEditorSession& session,
    int stateIndex,
    const std::string& animation,
    int animationActionIndex,
    int animationLengthFrames,
    std::string* error = nullptr);
bool setEditorSessionStateAnimationFinished(
    FighterEditorSession& session,
    int stateIndex,
    const std::string& targetState,
    int blendFrames,
    std::string* error = nullptr);
bool setEditorSessionStateCollisionFlags(
    FighterEditorSession& session,
    int stateIndex,
    bool useAnimPhysics,
    bool allowSlideoff,
    bool allowLedgeGrab,
    bool allowBackwardsLedgeGrab,
    bool allowWallCollision,
    bool allowCeilingCollision,
    bool convertFloorCollisionToGround,
    std::string* error = nullptr);
bool setEditorSessionStateCallbacks(
    FighterEditorSession& session,
    int stateIndex,
    FighterEditorStateCallbackSlot slot,
    const std::vector<FunctionCall>& calls,
    std::string* error = nullptr);
bool addEditorSessionSubaction(
    FighterEditorSession& session,
    int stateIndex,
    const Subaction& subaction,
    int insertIndex = -1,
    int* addedSubactionIndex = nullptr,
    std::string* error = nullptr);
bool addEditorSessionSubactionAtFrame(
    FighterEditorSession& session,
    int stateIndex,
    const Subaction& subaction,
    int targetFrame,
    int* addedSubactionIndex = nullptr,
    std::string* error = nullptr);
bool setEditorSessionSubaction(
    FighterEditorSession& session,
    int stateIndex,
    int subactionIndex,
    const Subaction& subaction,
    std::string* error = nullptr);
bool removeEditorSessionSubaction(
    FighterEditorSession& session,
    int stateIndex,
    int subactionIndex,
    std::string* error = nullptr);
bool moveEditorSessionSubaction(
    FighterEditorSession& session,
    int stateIndex,
    int subactionIndex,
    int delta,
    int* movedSubactionIndex = nullptr,
    std::string* error = nullptr);
bool addEditorSessionInterrupt(
    FighterEditorSession& session,
    int stateIndex,
    const InterruptRule& interrupt,
    int insertIndex = -1,
    int* addedInterruptIndex = nullptr,
    std::string* error = nullptr);
bool setEditorSessionInterrupt(
    FighterEditorSession& session,
    int stateIndex,
    int interruptIndex,
    const InterruptRule& interrupt,
    std::string* error = nullptr);
bool removeEditorSessionInterrupt(
    FighterEditorSession& session,
    int stateIndex,
    int interruptIndex,
    std::string* error = nullptr);
std::string uniqueEditorPackageVariableName(const FighterDefinition& def, const std::string& prefix = "var");
std::string uniqueEditorPackageScriptName(const FighterDefinition& def, const std::string& prefix = "Script");
bool editorPackageVariableNameAvailable(const FighterDefinition& def, const std::string& name, int ignoredIndex = -1);
bool editorPackageScriptNameAvailable(const FighterDefinition& def, const std::string& name, int ignoredIndex = -1);
bool addEditorSessionPackageVariable(
    FighterEditorSession& session,
    const std::string& requestedName = {},
    int32_t initialValue = 0,
    int* addedVariableIndex = nullptr,
    std::string* error = nullptr);
bool renameEditorSessionPackageVariable(
    FighterEditorSession& session,
    int variableIndex,
    const std::string& newName,
    std::string* error = nullptr);
bool setEditorSessionPackageVariableInitialValue(
    FighterEditorSession& session,
    int variableIndex,
    int32_t initialValue,
    std::string* error = nullptr);
bool removeEditorSessionPackageVariable(
    FighterEditorSession& session,
    int variableIndex,
    std::string* error = nullptr);
bool addEditorSessionPackageScript(
    FighterEditorSession& session,
    const std::string& requestedName = {},
    int instructionBudget = 64,
    int* addedScriptIndex = nullptr,
    std::string* error = nullptr);
bool duplicateEditorSessionPackageScript(
    FighterEditorSession& session,
    int scriptIndex,
    int* addedScriptIndex = nullptr,
    std::string* error = nullptr);
bool renameEditorSessionPackageScript(
    FighterEditorSession& session,
    int scriptIndex,
    const std::string& newName,
    std::string* error = nullptr);
bool removeEditorSessionPackageScript(
    FighterEditorSession& session,
    int scriptIndex,
    std::string* error = nullptr);
bool setEditorSessionPackageScriptBudget(
    FighterEditorSession& session,
    int scriptIndex,
    int instructionBudget,
    std::string* error = nullptr);
PackageScriptGraph makePackageScriptLinearGraph(const PackageScript& script);
PackageScriptGraph makePackageScriptControlFlowGraph(const PackageScript& script);
bool compilePackageScriptGraph(PackageScript& script, std::string* error = nullptr);
bool compileFighterPackageScriptGraphs(FighterPackage& package, std::string* error = nullptr);
bool setEditorSessionPackageScriptGraph(
    FighterEditorSession& session,
    int scriptIndex,
    const PackageScriptGraph& graph,
    std::string* error = nullptr);
bool addEditorSessionPackageScriptGraphNode(
    FighterEditorSession& session,
    int scriptIndex,
    const PackageScriptGraphNode& node,
    int* addedNodeId = nullptr,
    std::string* error = nullptr);
bool setEditorSessionPackageScriptGraphNode(
    FighterEditorSession& session,
    int scriptIndex,
    int nodeId,
    const PackageScriptGraphNode& node,
    std::string* error = nullptr);
bool removeEditorSessionPackageScriptGraphNode(
    FighterEditorSession& session,
    int scriptIndex,
    int nodeId,
    std::string* error = nullptr);
bool setEditorSessionPackageScriptGraphLink(
    FighterEditorSession& session,
    int scriptIndex,
    const PackageScriptGraphLink& link,
    std::string* error = nullptr);
bool removeEditorSessionPackageScriptGraphLink(
    FighterEditorSession& session,
    int scriptIndex,
    int fromNode,
    int fromSocket,
    std::string* error = nullptr);
bool compileEditorSessionPackageScriptGraph(
    FighterEditorSession& session,
    int scriptIndex,
    std::string* error = nullptr);
bool addEditorSessionPackageInstruction(
    FighterEditorSession& session,
    int scriptIndex,
    const PackageScriptInstruction& instruction,
    int insertIndex = -1,
    int* addedInstructionIndex = nullptr,
    std::string* error = nullptr);
bool setEditorSessionPackageInstruction(
    FighterEditorSession& session,
    int scriptIndex,
    int instructionIndex,
    const PackageScriptInstruction& instruction,
    std::string* error = nullptr);
bool removeEditorSessionPackageInstruction(
    FighterEditorSession& session,
    int scriptIndex,
    int instructionIndex,
    std::string* error = nullptr);
bool moveEditorSessionPackageInstruction(
    FighterEditorSession& session,
    int scriptIndex,
    int instructionIndex,
    int delta,
    int* movedInstructionIndex = nullptr,
    std::string* error = nullptr);
bool bindEditorSessionPackageScriptCallback(
    FighterEditorSession& session,
    int stateIndex,
    FighterEditorStateCallbackSlot slot,
    const std::string& scriptName,
    std::string* error = nullptr);
std::string uniqueEditorAuthoredClipName(const FighterDefinition& def, const std::string& prefix = "Clip");
std::string uniqueEditorAuthoredJointName(const FighterDefinition& def, const std::string& prefix = "Joint");
int uniqueEditorAuthoredClipActionIndex(const FighterDefinition& def);
bool editorAuthoredClipNameAvailable(const FighterDefinition& def, const std::string& name, int ignoredIndex = -1);
bool editorAuthoredJointNameAvailable(const FighterDefinition& def, const std::string& name, int ignoredIndex = -1);
bool editorAuthoredClipActionIndexAvailable(const FighterDefinition& def, int actionIndex, int ignoredIndex = -1);
FighterMesh makeFighterEditorTriangleMesh();
int editorAuthoredMeshVertexCount(const FighterMesh& mesh);
bool setEditorSessionAuthoredEcb(
    FighterEditorSession& session,
    const FighterEcbDefinition& ecb,
    bool normalize = true,
    std::string* error = nullptr);
bool addEditorSessionHurtbox(
    FighterEditorSession& session,
    const HurtboxDefinition& hurtbox,
    int insertIndex = -1,
    int* addedHurtboxIndex = nullptr,
    std::string* error = nullptr);
bool setEditorSessionHurtbox(
    FighterEditorSession& session,
    int hurtboxIndex,
    const HurtboxDefinition& hurtbox,
    std::string* error = nullptr);
bool removeEditorSessionHurtbox(
    FighterEditorSession& session,
    int hurtboxIndex,
    std::string* error = nullptr);
bool addEditorSessionAuthoredJoint(
    FighterEditorSession& session,
    const AnimationJoint& joint,
    int insertIndex = -1,
    int* addedJointIndex = nullptr,
    std::string* error = nullptr);
bool setEditorSessionAuthoredJoint(
    FighterEditorSession& session,
    int jointIndex,
    const AnimationJoint& joint,
    std::string* error = nullptr);
bool removeEditorSessionAuthoredJoint(
    FighterEditorSession& session,
    int jointIndex,
    std::string* error = nullptr);
bool createEditorSessionAuthoredClip(
    FighterEditorSession& session,
    const std::string& requestedName = {},
    int sourceClipIndex = -1,
    int* createdClipIndex = nullptr,
    std::string* error = nullptr);
bool duplicateEditorSessionAuthoredClip(
    FighterEditorSession& session,
    int sourceClipIndex,
    int* createdClipIndex = nullptr,
    std::string* error = nullptr);
bool renameEditorSessionAuthoredClip(
    FighterEditorSession& session,
    int clipIndex,
    const std::string& newName,
    std::string* error = nullptr);
bool setEditorSessionAuthoredClipProperties(
    FighterEditorSession& session,
    int clipIndex,
    int actionIndex,
    Fix frameCount,
    int defaultBlendFrames,
    uint32_t actionFlags,
    std::string* error = nullptr);
bool removeEditorSessionAuthoredClip(
    FighterEditorSession& session,
    int clipIndex,
    const std::string& replacementClipName = {},
    std::string* error = nullptr);
bool addEditorSessionAuthoredTrack(
    FighterEditorSession& session,
    int clipIndex,
    const AnimationTrack& track,
    int insertIndex = -1,
    int* addedTrackIndex = nullptr,
    std::string* error = nullptr);
bool setEditorSessionAuthoredTrack(
    FighterEditorSession& session,
    int clipIndex,
    int trackIndex,
    const AnimationTrack& track,
    std::string* error = nullptr);
bool removeEditorSessionAuthoredTrack(
    FighterEditorSession& session,
    int clipIndex,
    int trackIndex,
    std::string* error = nullptr);
bool addEditorSessionAuthoredKey(
    FighterEditorSession& session,
    int clipIndex,
    int trackIndex,
    const AnimationKey& key,
    int* addedKeyIndex = nullptr,
    std::string* error = nullptr);
bool setEditorSessionAuthoredKey(
    FighterEditorSession& session,
    int clipIndex,
    int trackIndex,
    int keyIndex,
    const AnimationKey& key,
    std::string* error = nullptr);
bool removeEditorSessionAuthoredKey(
    FighterEditorSession& session,
    int clipIndex,
    int trackIndex,
    int keyIndex,
    std::string* error = nullptr);
bool setEditorSessionAuthoredMesh(
    FighterEditorSession& session,
    const FighterMesh& mesh,
    std::string* error = nullptr);
bool scaleEditorSessionAuthoredMesh(
    FighterEditorSession& session,
    Fix scaleX,
    Fix scaleY,
    std::string* error = nullptr);
bool nudgeEditorSessionAuthoredMeshVertex(
    FighterEditorSession& session,
    int vertexIndex,
    Vec3 delta,
    std::string* error = nullptr);
bool bindEditorSessionAuthoredMeshToJoint(
    FighterEditorSession& session,
    int jointIndex,
    std::string* error = nullptr);
bool bindEditorSessionAuthoredMeshVertexToJoint(
    FighterEditorSession& session,
    int vertexIndex,
    int jointIndex,
    std::string* error = nullptr);
bool blendEditorSessionAuthoredMeshVertexTowardJoint(
    FighterEditorSession& session,
    int vertexIndex,
    int jointIndex,
    float amount,
    std::string* error = nullptr);
bool autoWeightEditorSessionAuthoredMeshToSkeleton(
    FighterEditorSession& session,
    std::string* error = nullptr);
std::string uniqueEditorObjectName(const FighterPackage& package, const std::string& prefix = "Object");
std::string uniqueEditorObjectStateName(const GameObjectDefinition& object, const std::string& prefix = "State");
bool editorObjectNameAvailable(const FighterPackage& package, const std::string& name, int ignoredIndex = -1);
bool editorObjectStateNameAvailable(const GameObjectDefinition& object, const std::string& name, int ignoredIndex = -1);
bool renameEditorSessionObject(
    FighterEditorSession& session,
    int objectIndex,
    const std::string& newName,
    std::string* error = nullptr);
bool removeEditorSessionObject(
    FighterEditorSession& session,
    int objectIndex,
    const std::string& replacementObjectName = {},
    std::string* error = nullptr);
bool setEditorSessionObjectKind(
    FighterEditorSession& session,
    int objectIndex,
    GameObjectKind kind,
    std::string* error = nullptr);
bool setEditorSessionObjectProperties(
    FighterEditorSession& session,
    int objectIndex,
    int lifetimeFrames,
    Fix gravity,
    Fix terminalVelocity,
    Fix maxDamage,
    bool destroyOnHit,
    bool destroyOnShield,
    bool hitOwner,
    std::string* error = nullptr);
bool createEditorSessionObjectState(
    FighterEditorSession& session,
    int objectIndex,
    const std::string& requestedName = {},
    int sourceStateIndex = -1,
    int* createdStateIndex = nullptr,
    std::string* error = nullptr);
bool duplicateEditorSessionObjectState(
    FighterEditorSession& session,
    int objectIndex,
    int sourceStateIndex,
    int* createdStateIndex = nullptr,
    std::string* error = nullptr);
bool renameEditorSessionObjectState(
    FighterEditorSession& session,
    int objectIndex,
    int stateIndex,
    const std::string& newName,
    std::string* error = nullptr);
bool removeEditorSessionObjectState(
    FighterEditorSession& session,
    int objectIndex,
    int stateIndex,
    const std::string& replacementStateName = {},
    std::string* error = nullptr);
bool setEditorSessionObjectStateTiming(
    FighterEditorSession& session,
    int objectIndex,
    int stateIndex,
    int animationLengthFrames,
    bool loopAnimation,
    bool makeInitialState,
    std::string* error = nullptr);
bool setEditorSessionObjectStateCallbacks(
    FighterEditorSession& session,
    int objectIndex,
    int stateIndex,
    FighterEditorObjectStateCallbackSlot slot,
    const std::vector<FunctionCall>& calls,
    std::string* error = nullptr);
bool setEditorSessionObjectEventCallbacks(
    FighterEditorSession& session,
    int objectIndex,
    FighterEditorObjectEventCallbackSlot slot,
    const std::vector<FunctionCall>& calls,
    std::string* error = nullptr);
std::string uniqueEditorObjectPackageVariableName(const GameObjectDefinition& object, const std::string& prefix = "var");
std::string uniqueEditorObjectPackageScriptName(const GameObjectDefinition& object, const std::string& prefix = "Script");
bool editorObjectPackageVariableNameAvailable(const GameObjectDefinition& object, const std::string& name, int ignoredIndex = -1);
bool editorObjectPackageScriptNameAvailable(const GameObjectDefinition& object, const std::string& name, int ignoredIndex = -1);
bool addEditorSessionObjectPackageVariable(
    FighterEditorSession& session,
    int objectIndex,
    const std::string& requestedName = {},
    int32_t initialValue = 0,
    int* addedVariableIndex = nullptr,
    std::string* error = nullptr);
bool renameEditorSessionObjectPackageVariable(
    FighterEditorSession& session,
    int objectIndex,
    int variableIndex,
    const std::string& newName,
    std::string* error = nullptr);
bool setEditorSessionObjectPackageVariableInitialValue(
    FighterEditorSession& session,
    int objectIndex,
    int variableIndex,
    int32_t initialValue,
    std::string* error = nullptr);
bool removeEditorSessionObjectPackageVariable(
    FighterEditorSession& session,
    int objectIndex,
    int variableIndex,
    std::string* error = nullptr);
bool addEditorSessionObjectPackageScript(
    FighterEditorSession& session,
    int objectIndex,
    const std::string& requestedName = {},
    int instructionBudget = 64,
    int* addedScriptIndex = nullptr,
    std::string* error = nullptr);
bool duplicateEditorSessionObjectPackageScript(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int* addedScriptIndex = nullptr,
    std::string* error = nullptr);
bool renameEditorSessionObjectPackageScript(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    const std::string& newName,
    std::string* error = nullptr);
bool removeEditorSessionObjectPackageScript(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    std::string* error = nullptr);
bool setEditorSessionObjectPackageScriptBudget(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int instructionBudget,
    std::string* error = nullptr);
bool setEditorSessionObjectPackageScriptGraph(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    const PackageScriptGraph& graph,
    std::string* error = nullptr);
bool addEditorSessionObjectPackageScriptGraphNode(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    const PackageScriptGraphNode& node,
    int* addedNodeId = nullptr,
    std::string* error = nullptr);
bool setEditorSessionObjectPackageScriptGraphNode(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int nodeId,
    const PackageScriptGraphNode& node,
    std::string* error = nullptr);
bool removeEditorSessionObjectPackageScriptGraphNode(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int nodeId,
    std::string* error = nullptr);
bool setEditorSessionObjectPackageScriptGraphLink(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    const PackageScriptGraphLink& link,
    std::string* error = nullptr);
bool removeEditorSessionObjectPackageScriptGraphLink(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int fromNode,
    int fromSocket,
    std::string* error = nullptr);
bool compileEditorSessionObjectPackageScriptGraph(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    std::string* error = nullptr);
bool addEditorSessionObjectPackageInstruction(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    const PackageScriptInstruction& instruction,
    int insertIndex = -1,
    int* addedInstructionIndex = nullptr,
    std::string* error = nullptr);
bool setEditorSessionObjectPackageInstruction(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int instructionIndex,
    const PackageScriptInstruction& instruction,
    std::string* error = nullptr);
bool removeEditorSessionObjectPackageInstruction(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int instructionIndex,
    std::string* error = nullptr);
bool moveEditorSessionObjectPackageInstruction(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int instructionIndex,
    int delta,
    int* movedInstructionIndex = nullptr,
    std::string* error = nullptr);
bool bindEditorSessionObjectPackageScriptStateCallback(
    FighterEditorSession& session,
    int objectIndex,
    int stateIndex,
    FighterEditorObjectStateCallbackSlot slot,
    const std::string& scriptName,
    std::string* error = nullptr);
bool bindEditorSessionObjectPackageScriptEventCallback(
    FighterEditorSession& session,
    int objectIndex,
    FighterEditorObjectEventCallbackSlot slot,
    const std::string& scriptName,
    std::string* error = nullptr);
bool addEditorSessionObjectHitbox(
    FighterEditorSession& session,
    int objectIndex,
    const HitboxDefinition& hitbox,
    int insertIndex = -1,
    int* addedHitboxIndex = nullptr,
    std::string* error = nullptr);
bool setEditorSessionObjectHitbox(
    FighterEditorSession& session,
    int objectIndex,
    int hitboxIndex,
    const HitboxDefinition& hitbox,
    std::string* error = nullptr);
bool removeEditorSessionObjectHitbox(
    FighterEditorSession& session,
    int objectIndex,
    int hitboxIndex,
    std::string* error = nullptr);
bool addEditorSessionObjectHurtbox(
    FighterEditorSession& session,
    int objectIndex,
    const GameObjectHurtboxDefinition& hurtbox,
    int insertIndex = -1,
    int* addedHurtboxIndex = nullptr,
    std::string* error = nullptr);
bool setEditorSessionObjectHurtbox(
    FighterEditorSession& session,
    int objectIndex,
    int hurtboxIndex,
    const GameObjectHurtboxDefinition& hurtbox,
    std::string* error = nullptr);
bool removeEditorSessionObjectHurtbox(
    FighterEditorSession& session,
    int objectIndex,
    int hurtboxIndex,
    std::string* error = nullptr);
bool addEditorSessionObjectTouchbox(
    FighterEditorSession& session,
    int objectIndex,
    const GameObjectTouchboxDefinition& touchbox,
    int insertIndex = -1,
    int* addedTouchboxIndex = nullptr,
    std::string* error = nullptr);
bool setEditorSessionObjectTouchbox(
    FighterEditorSession& session,
    int objectIndex,
    int touchboxIndex,
    const GameObjectTouchboxDefinition& touchbox,
    std::string* error = nullptr);
bool removeEditorSessionObjectTouchbox(
    FighterEditorSession& session,
    int objectIndex,
    int touchboxIndex,
    std::string* error = nullptr);

} // namespace pf
