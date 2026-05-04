#pragma once

#include "core/action.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pf {

enum class SegmentType : uint8_t {
    Solid,
    Semisolid,
};

enum class SegmentLineKind : uint8_t {
    Floor,
    Ceiling,
    RightWall,
    LeftWall,
};

struct StageSegment {
    Vec2 start;
    Vec2 end;
    Fix friction = fx(1);
    SegmentType type = SegmentType::Solid;
    bool leftLedge = false;
    bool rightLedge = false;
    SegmentLineKind lineKind = SegmentLineKind::Floor;
    bool dynamicLineKind = false;
    int nextLine = -1;
    int previousLine = -1;
};

struct StageLedge {
    Vec2 position;
    int direction = 1;
    int segmentIndex = -1;
};

struct StageDefinition {
    std::string name;
    std::vector<StageSegment> segments;
    std::vector<StageLedge> ledges;
    Vec2 blastMin{-fx(30), -fx(20)};
    Vec2 blastMax{fx(30), fx(25)};
};

struct Ecb {
    // Relative to fighter.position, matching the Melee/Godot diamond shape.
    std::array<Vec2, 4> points{};
    Vec2 worldBottom;
    int floorIndex = -1;
};

struct ActiveHitbox {
    HitboxDefinition def;
    bool firstFrame = true;
    Vec3 current;
    Vec3 previous;
};

enum class GameObjectKind : uint8_t {
    Item,
    Projectile,
};

struct GameObjectStateDefinition {
    std::string name;
    int animationLengthFrames = 0;
    bool loopAnimation = false;
    std::vector<FunctionCall> onEnter;
    std::vector<FunctionCall> onFrame;
    std::vector<FunctionCall> onPhysics;
    std::vector<FunctionCall> onCollision;
};

struct GameObjectHurtboxDefinition {
    Vec3 startOffset;
    Vec3 endOffset;
    Fix radius = fx(1);
    HurtboxState state = HurtboxState::Normal;
};

struct GameObjectTouchboxDefinition {
    Vec3 startOffset;
    Vec3 endOffset;
    Fix radius = fx(1);
    bool touchFighters = true;
    bool touchObjects = true;
};

struct GameObjectDefinition {
    std::string name;
    GameObjectKind kind = GameObjectKind::Projectile;
    int initialState = 0;
    int lifetimeFrames = 120;
    Fix gravity = 0;
    Fix terminalVelocity = fx(5);
    Fix maxDamage = 0;
    bool destroyOnHit = true;
    bool destroyOnShield = true;
    bool hitOwner = false;
    std::vector<PackageVariableDefinition> packageVariables;
    std::vector<PackageScript> packageScripts;
    std::vector<GameObjectStateDefinition> states;
    std::vector<FunctionCall> onSpawned;
    std::vector<FunctionCall> onDestroyed;
    std::vector<FunctionCall> onPickedUp;
    std::vector<FunctionCall> onDropped;
    std::vector<FunctionCall> onThrown;
    std::vector<FunctionCall> onDamageDealt;
    std::vector<FunctionCall> onDamageReceived;
    std::vector<FunctionCall> onClanked;
    std::vector<FunctionCall> onReflected;
    std::vector<FunctionCall> onAbsorbed;
    std::vector<FunctionCall> onShieldBounced;
    std::vector<FunctionCall> onHitShield;
    std::vector<FunctionCall> onEnteredAir;
    std::vector<FunctionCall> onEnteredHitlag;
    std::vector<FunctionCall> onExitedHitlag;
    std::vector<FunctionCall> onAccessory;
    std::vector<FunctionCall> onTouched;
    std::vector<FunctionCall> onJumpedOn;
    std::vector<FunctionCall> onGrabDealt;
    std::vector<FunctionCall> onGrabbedForVictim;
    std::vector<FunctionCall> onInteraction;
    std::vector<HitboxDefinition> hitboxes;
    std::vector<GameObjectHurtboxDefinition> hurtboxes;
    std::vector<GameObjectTouchboxDefinition> touchboxes;
};

struct GameObjectRuntime {
    int objectDef = 0;
    int state = 0;
    int lastStateChangeFrame = 0;
    int ownerFighter = -1;
    int heldByFighter = -1;
    int grabVictimFighter = -1;
    int lastInteractionFighter = -1;
    int lastInteractionObject = -1;
    int internalFrame = 0;
    int hitlag = 0;
    Fix animationFrame = 0;
    Fix animationRate = fx(1);
    Fix damageTaken = 0;
    int facing = 1;
    bool active = true;
    bool destroyEventDispatched = false;
    bool grounded = false;
    int groundSegment = -1;
    Vec2 position;
    Vec2 previousPosition;
    Vec2 velocity;
    std::vector<int32_t> packageVars;
    std::vector<ActiveHitbox> activeHitboxes;
    std::vector<int> fightersHit;
};

struct FighterRuntime {
    int fighterDef = 0;
    int state = 0;
    int lastStateChangeFrame = 0;
    int internalFrame = 0;
    int interruptibleFrame = 0;
    int stateAnimationLengthOverride = 0;
    Fix animationFrame = 0;
    Fix animationRate = fx(1);
    int animationActionIndexOverride = -1;
    int lastActionFrameExecuted = -1;
    Fix runAnimationVelocity = 0;
    int facing = 1;
    int hsdPoseFacing = 1;
    int jumpsUsed = 0;
    bool grounded = true;
    Fix percent = 0;
    Fix shieldHealth = fx(60);
    Vec2 position;
    Vec2 previousPosition;
    Vec2 fighterVelocity;
    Vec2 knockbackVelocity;
    Vec2 knockbackDecay;
    Vec2 attackerShieldKnockback;
    Fix groundVelocity = 0;
    Fix groundKnockbackVelocity = 0;
    Fix groundAttackerShieldKnockbackVelocity = 0;
    Fix lastLandingVelocityY = 0;
    Fix groundAccel = 0;
    Fix groundAccelSecondary = 0;
    Vec2 groundNormal{0, fx(1)};
    int hitlag = 0;
    int hitstun = 0;
    int damageLevel = 0;
    int damageHurtboxRegion = 1;
    Fix damageKnockback = 0;
    Fix damageLaunchAngle = 0;
    bool damageTumble = false;
    Fix reboundDamageVelocity = 0;
    Fix reboundAccel = 0;
    Fix reboundAnimationRate = fx(1);
    int reboundFacingDir = 1;
    int damageSurfaceTimer = 0;
    int downWaitTimer = 0;
    int damageHitboxOwner = -1;
    int thrownHitboxOwner = -1;
    int heldObject = -1;
    int grabbedFighter = -1;
    int grabberFighter = -1;
    Fix grabTimer = 0;
    Fix captureWaitTimer = 0;
    Fix captureMashAnimTimer = 0;
    int burySubmergeTimer = 0;
    int grabMashStickX = 0;
    int grabMashStickY = 0;
    bool captureJumpQueued = false;
    Vec3 captureConstraintOffset = {};
    Vec3 captureOriginalXRotNTranslation = {};
    bool captureConstraintActive = false;
    bool throwAnimationFrozen = false;
    bool thrownAnimationFreezeActive = false;
    Fix thrownAnimationFreezeFrame = 0;
    std::array<HitboxDefinition, 2> throwHitboxes{};
    std::array<bool, 2> throwHitboxActive{};
    uint32_t stateFlags = 0;
    std::array<uint32_t, 4> commandVars{};
    std::vector<int32_t> packageVars;
    uint32_t commandFlags = 0;
    uint32_t throwFlags = 0;
    bool jabFollowupEnabled = false;
    bool rapidJabEnabled = false;
    bool fighterInvisible = false;
    Ecb ecb;
    Ecb desiredEcb;
    Ecb previousEcb;
    int ecbLockTimer = 0;
    Vec2 ecbLockBottom{};
    int groundSegment = -1;
    int floorSkipSegment = -1;
    int platformDropTimer = 0;
    int grabbedLedge = -1;
    int ledgeCooldown = 0;
    bool ledgeActionReady = false;
    int ledgeWaitTimer = 0;
    int runoffSegment = -1;
    int runoffDirection = 0;
    int pendingFallSpecialLandingLag = 0;
    bool pendingFallSpecialLandingInterruptible = false;
    bool pendingFallSpecialForceLanding = true;
    bool pendingFallSpecialLimitDrift = false;
    bool pendingFallSpecialUseFastFallTerminal = false;
    Fix pendingFallSpecialDriftMax = 0;
    int fallSpecialLandingLag = 0;
    bool fallSpecialLandingInterruptible = false;
    bool fallSpecialForceLanding = true;
    bool fallSpecialLimitDrift = false;
    bool fallSpecialUseFastFallTerminal = false;
    Fix fallSpecialDriftMax = 0;
    int wallContactSide = 0;
    int wallContactSegment = -1;
    int wallContactTimer = 254;
    int wallJumpsUsed = 0;
    int turnFramesToChangeDirection = 0;
    int turnRunInitialFacing = 1;
    int turnFacingAfter = 0;
    bool turnHasTurned = false;
    bool turnJustTurned = false;
    bool turnDashBuffered = false;
    uint16_t turnBufferedButtons = 0;
    int runDirectTimer = 0;
    int runBrakeTimer = 0;
    bool runBrakeAnimationFrozen = false;
    int attackDashGrabBufferTimer = 0;
    bool attackLw3RepeatQueued = false;
    int attackRapidInputCount = 0;
    bool attack100CanEnd = false;
    bool attack100ContinuePressed = false;
    int guardMinHoldTimer = 0;
    int guardSetoffTimer = 0;
    int guardCatchDashBufferTimer = 0;
    bool guardReleaseQueued = false;
    Fix guardPoseFrame = fx(10);
    Fix guardPoseBlend = 0;
    int smashChargeState = 0;
    Fix smashChargeFrames = 0;
    Fix smashChargeHoldFrames = 0;
    Fix smashChargeDamageMultiplier = fx(1);
    Fix smashChargeStoredAnimationRate = fx(1);
    int stickXTiltTimer = 254;
    int stickYTiltTimer = 254;
    InputBuffer input;
    std::array<BonePose, kBoneCount> bones{};
    AnimationPose hsdPose;
    AnimationPose hsdBlendFromPose;
    int hsdBlendFrames = 0;
    Fix hsdBlendElapsed = 0;
    Vec3 hsdTransN = {};
    Vec3 previousHsdTransN = {};
    Vec3 hsdTransNOffset = {};
    std::vector<JointWorldTransform> hsdJointWorldTransforms;
    std::vector<Vec3> hsdJointWorldPositions;
    std::vector<Capsule> hsdHurtboxCapsules;
    std::vector<int> hsdModelVisibilityDefaultStates;
    std::vector<int> hsdModelVisibilityStates;
    std::vector<int> hsdModelPartAnimations;
    std::vector<HurtboxState> hurtboxStates;
    HurtboxState bodyCollisionState = HurtboxState::Normal;
    std::vector<ActiveHitbox> activeHitboxes;
    std::vector<int> fightersHitThisAction;
};

struct FighterSnapshot {
    int fighterDef = 0;
    int state = 0;
    int lastStateChangeFrame = 0;
    int internalFrame = 0;
    int interruptibleFrame = 0;
    int stateAnimationLengthOverride = 0;
    Fix animationFrame = 0;
    Fix animationRate = fx(1);
    int animationActionIndexOverride = -1;
    int lastActionFrameExecuted = -1;
    Fix runAnimationVelocity = 0;
    int facing = 1;
    int hsdPoseFacing = 1;
    int jumpsUsed = 0;
    bool grounded = true;
    Fix percent = 0;
    Fix shieldHealth = fx(60);
    Vec2 position;
    Vec2 previousPosition;
    Vec2 fighterVelocity;
    Vec2 knockbackVelocity;
    Vec2 knockbackDecay;
    Vec2 attackerShieldKnockback;
    Fix groundVelocity = 0;
    Fix groundKnockbackVelocity = 0;
    Fix groundAttackerShieldKnockbackVelocity = 0;
    Fix lastLandingVelocityY = 0;
    Fix groundAccel = 0;
    Fix groundAccelSecondary = 0;
    Vec2 groundNormal{0, fx(1)};
    int hitlag = 0;
    int hitstun = 0;
    int damageLevel = 0;
    int damageHurtboxRegion = 1;
    Fix damageKnockback = 0;
    Fix damageLaunchAngle = 0;
    bool damageTumble = false;
    Fix reboundDamageVelocity = 0;
    Fix reboundAccel = 0;
    Fix reboundAnimationRate = fx(1);
    int reboundFacingDir = 1;
    int damageSurfaceTimer = 0;
    int downWaitTimer = 0;
    int damageHitboxOwner = -1;
    int thrownHitboxOwner = -1;
    int heldObject = -1;
    int grabbedFighter = -1;
    int grabberFighter = -1;
    Fix grabTimer = 0;
    Fix captureWaitTimer = 0;
    Fix captureMashAnimTimer = 0;
    int burySubmergeTimer = 0;
    int grabMashStickX = 0;
    int grabMashStickY = 0;
    bool captureJumpQueued = false;
    Vec3 captureConstraintOffset = {};
    Vec3 captureOriginalXRotNTranslation = {};
    bool captureConstraintActive = false;
    bool throwAnimationFrozen = false;
    bool thrownAnimationFreezeActive = false;
    Fix thrownAnimationFreezeFrame = 0;
    std::array<HitboxDefinition, 2> throwHitboxes{};
    std::array<bool, 2> throwHitboxActive{};
    uint32_t stateFlags = 0;
    std::array<uint32_t, 4> commandVars{};
    std::vector<int32_t> packageVars;
    uint32_t commandFlags = 0;
    uint32_t throwFlags = 0;
    bool jabFollowupEnabled = false;
    bool rapidJabEnabled = false;
    bool fighterInvisible = false;
    Ecb ecb;
    Ecb desiredEcb;
    Ecb previousEcb;
    int ecbLockTimer = 0;
    Vec2 ecbLockBottom{};
    int groundSegment = -1;
    int floorSkipSegment = -1;
    int platformDropTimer = 0;
    int grabbedLedge = -1;
    int ledgeCooldown = 0;
    bool ledgeActionReady = false;
    int ledgeWaitTimer = 0;
    int runoffSegment = -1;
    int runoffDirection = 0;
    int pendingFallSpecialLandingLag = 0;
    bool pendingFallSpecialLandingInterruptible = false;
    bool pendingFallSpecialForceLanding = true;
    bool pendingFallSpecialLimitDrift = false;
    bool pendingFallSpecialUseFastFallTerminal = false;
    Fix pendingFallSpecialDriftMax = 0;
    int fallSpecialLandingLag = 0;
    bool fallSpecialLandingInterruptible = false;
    bool fallSpecialForceLanding = true;
    bool fallSpecialLimitDrift = false;
    bool fallSpecialUseFastFallTerminal = false;
    Fix fallSpecialDriftMax = 0;
    int wallContactSide = 0;
    int wallContactSegment = -1;
    int wallContactTimer = 254;
    int wallJumpsUsed = 0;
    int turnFramesToChangeDirection = 0;
    int turnRunInitialFacing = 1;
    int turnFacingAfter = 0;
    bool turnHasTurned = false;
    bool turnJustTurned = false;
    bool turnDashBuffered = false;
    uint16_t turnBufferedButtons = 0;
    int runDirectTimer = 0;
    int runBrakeTimer = 0;
    bool runBrakeAnimationFrozen = false;
    int attackDashGrabBufferTimer = 0;
    bool attackLw3RepeatQueued = false;
    int attackRapidInputCount = 0;
    bool attack100CanEnd = false;
    bool attack100ContinuePressed = false;
    int guardMinHoldTimer = 0;
    int guardSetoffTimer = 0;
    int guardCatchDashBufferTimer = 0;
    bool guardReleaseQueued = false;
    Fix guardPoseFrame = fx(10);
    Fix guardPoseBlend = 0;
    int smashChargeState = 0;
    Fix smashChargeFrames = 0;
    Fix smashChargeHoldFrames = 0;
    Fix smashChargeDamageMultiplier = fx(1);
    Fix smashChargeStoredAnimationRate = fx(1);
    int stickXTiltTimer = 254;
    int stickYTiltTimer = 254;
    InputBuffer input;
    std::array<BonePose, kBoneCount> bones{};
    AnimationPose hsdPose;
    AnimationPose hsdBlendFromPose;
    int hsdBlendFrames = 0;
    Fix hsdBlendElapsed = 0;
    Vec3 hsdTransN = {};
    Vec3 previousHsdTransN = {};
    Vec3 hsdTransNOffset = {};
    std::vector<JointWorldTransform> hsdJointWorldTransforms;
    std::vector<Vec3> hsdJointWorldPositions;
    std::vector<Capsule> hsdHurtboxCapsules;
    std::vector<int> hsdModelVisibilityDefaultStates;
    std::vector<int> hsdModelVisibilityStates;
    std::vector<int> hsdModelPartAnimations;
    std::vector<HurtboxState> hurtboxStates;
    HurtboxState bodyCollisionState = HurtboxState::Normal;
    std::vector<ActiveHitbox> activeHitboxes;
    std::vector<int> fightersHitThisAction;
};

struct WorldSnapshot {
    int frame = 0;
    uint32_t rngState = 0x4D454C45;
    std::vector<GameObjectDefinition> objectDefs;
    std::vector<FighterSnapshot> fighters;
    std::vector<GameObjectRuntime> objects;
};

struct World {
    int frame = 0;
    uint32_t rngState = 0x4D454C45;
    StageDefinition stage;
    std::vector<FighterDefinition> fighterDefs;
    std::vector<FighterRuntime> fighters;
    std::vector<GameObjectDefinition> objectDefs;
    std::vector<GameObjectRuntime> objects;
};

struct FighterPackage;
class FighterPackageCache;
struct FighterPackageDescriptor;

StageDefinition makeBattlefieldTrainingStage();
World makeTrainingWorld();
World makeTrainingWorld(int p1FighterDef, int p2FighterDef);
void tickWorld(World& world, const std::vector<InputFrame>& inputs);
uint32_t nextWorldRandom(World& world);
int32_t nextWorldRandomBounded(World& world, int32_t upperExclusive);
void resetTrainingFighter(World& world, size_t fighterIndex, int fighterDefIndex, Vec2 position, int facing);
bool switchFighterDefinition(World& world, FighterRuntime& fighter, const std::string& fighterName, const std::string& stateName = {});
int spawnFighter(World& world, const std::string& fighterName, Vec2 position, int facing);
int spawnGameObject(World& world, const std::string& objectName, int ownerFighter, Vec2 position, int facing, Vec2 velocity = {});
int spawnGameObjectOfKind(World& world, const std::string& objectName, GameObjectKind requiredKind, int ownerFighter, Vec2 position, int facing, Vec2 velocity = {});
int countGameObjectsOwnedBy(const World& world, int ownerFighter, const std::string& objectName);
FighterPackage makeRuntimeFighterPackage(const World& world, int rootFighterDef, const std::string& packageName = {});
bool installFighterPackage(World& world, const FighterPackage& package, int* rootFighterDef = nullptr, std::string* error = nullptr);
bool installFighterPackageBytes(
    World& world,
    const std::vector<uint8_t>& bytes,
    int* rootFighterDef = nullptr,
    FighterPackageDescriptor* descriptor = nullptr,
    std::string* error = nullptr,
    const std::vector<std::shared_ptr<const HsdFighterAnimationAsset>>& hsdAssetPool = {});
bool installCachedFighterPackage(
    World& world,
    const FighterPackageCache& cache,
    uint32_t checksum,
    int* rootFighterDef = nullptr,
    FighterPackageDescriptor* descriptor = nullptr,
    std::string* error = nullptr,
    const std::vector<std::shared_ptr<const HsdFighterAnimationAsset>>& hsdAssetPool = {});
bool destroyGameObjectByIndex(World& world, int objectIndex);
int destroyGameObjectsOwnedBy(World& world, int ownerFighter, const std::string& objectName);
bool pickUpGameObject(World& world, int objectIndex, int fighterIndex);
bool dropGameObject(World& world, int objectIndex, Vec2 velocity = {});
bool throwGameObject(World& world, int objectIndex, int fighterIndex, Vec2 velocity);
bool reflectGameObject(World& world, int objectIndex, int fighterIndex, Vec2 normal);
bool absorbGameObject(World& world, int objectIndex, int fighterIndex);
bool shieldBounceGameObject(World& world, int objectIndex, int fighterIndex, Vec2 normal);
bool interactGameObjectWithFighter(World& world, int objectIndex, int fighterIndex);
bool interactGameObjects(World& world, int objectIndex, int referenceObjectIndex);
void runGameObjectPackageScript(World& world, int objectIndex, const std::string& scriptName);
void changeGameObjectState(World& world, GameObjectRuntime& object, const std::string& stateName);
void changeFighterState(World& world, FighterRuntime& fighter, const std::string& stateName, int lagFrames = 0, int blendFrames = kUseDefaultAnimationBlendFrames);
WorldSnapshot saveWorld(const World& world);
void loadWorld(World& world, const WorldSnapshot& snapshot);
bool previewFighterAnimation(World& world, size_t fighterIndex, int actionIndex, Fix frame);
const FighterState& currentState(const World& world, const FighterRuntime& fighter);
int frameInState(const FighterRuntime& fighter);
bool fighterFlag(const FighterRuntime& fighter, int flag);
void setFighterFlag(FighterRuntime& fighter, int flag, bool value);
bool fighterCommandFlag(const FighterRuntime& fighter, int flag);
uint32_t fighterCommandVar(const FighterRuntime& fighter, int index);
void setFighterCommandFlag(FighterRuntime& fighter, int flag, bool value);
void setFighterCommandVar(FighterRuntime& fighter, int index, uint32_t value);
bool fighterThrowFlag(const FighterRuntime& fighter, int flag);
void setFighterThrowFlag(FighterRuntime& fighter, int flag, bool value);
void lockFighterEcb(FighterRuntime& fighter, int frames);
void unlockFighterEcb(FighterRuntime& fighter);
void calculateEcb(const FighterDefinition& def, FighterRuntime& fighter, bool updatePrevious);
void beginMeleeThrowConstraint(World& world, size_t grabberIndex, size_t victimIndex);
bool updateMeleeCapturePosition(World& world, size_t victimIndex);
void releaseMeleeCaptureConstraint(World& world, size_t ownerIndex, int capturedIndex, bool applyOffset, bool meleeThrowRelease = false);

} // namespace pf
