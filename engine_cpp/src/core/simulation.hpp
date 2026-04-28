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

struct FighterRuntime {
    int fighterDef = 0;
    int state = 0;
    int lastStateChangeFrame = 0;
    int internalFrame = 0;
    int interruptibleFrame = 0;
    int stateAnimationLengthOverride = 0;
    Fix animationFrame = 0;
    Fix animationRate = fx(1);
    Fix runAnimationVelocity = 0;
    int facing = 1;
    int jumpsUsed = 0;
    bool grounded = true;
    Fix percent = 0;
    Fix shieldHealth = fx(60);
    Vec2 position;
    Vec2 previousPosition;
    Vec2 fighterVelocity;
    Vec2 knockbackVelocity;
    Vec2 attackerShieldKnockback;
    Fix groundVelocity = 0;
    Fix groundAttackerShieldKnockbackVelocity = 0;
    Fix lastLandingVelocityY = 0;
    Fix groundAccel = 0;
    Fix groundAccelSecondary = 0;
    Vec2 groundNormal{0, fx(1)};
    int hitlag = 0;
    int hitstun = 0;
    uint32_t stateFlags = 0;
    uint32_t commandFlags = 0;
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
    int runoffSegment = -1;
    int runoffDirection = 0;
    int wallContactSide = 0;
    int wallContactSegment = -1;
    int wallContactTimer = 254;
    int wallJumpsUsed = 0;
    int turnFramesToChangeDirection = 0;
    int turnRunInitialFacing = 1;
    bool turnHasTurned = false;
    bool turnDashBuffered = false;
    int runDirectTimer = 0;
    int runBrakeTimer = 0;
    bool runBrakeAnimationFrozen = false;
    int guardMinHoldTimer = 0;
    int guardSetoffTimer = 0;
    bool guardReleaseQueued = false;
    Fix guardPoseFrame = fx(10);
    Fix guardPoseBlend = 0;
    int stickXTiltTimer = 254;
    int stickYTiltTimer = 254;
    InputBuffer input;
    std::array<BonePose, kBoneCount> bones{};
    AnimationPose hsdPose;
    AnimationPose hsdBlendFromPose;
    int hsdBlendFrames = 0;
    Vec3 hsdTransN = {};
    Vec3 previousHsdTransN = {};
    Vec3 hsdTransNOffset = {};
    std::vector<JointWorldTransform> hsdJointWorldTransforms;
    std::vector<Vec3> hsdJointWorldPositions;
    std::vector<Capsule> hsdHurtboxCapsules;
    std::vector<HurtboxState> hurtboxStates;
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
    Fix runAnimationVelocity = 0;
    int facing = 1;
    int jumpsUsed = 0;
    bool grounded = true;
    Fix percent = 0;
    Fix shieldHealth = fx(60);
    Vec2 position;
    Vec2 previousPosition;
    Vec2 fighterVelocity;
    Vec2 knockbackVelocity;
    Vec2 attackerShieldKnockback;
    Fix groundVelocity = 0;
    Fix groundAttackerShieldKnockbackVelocity = 0;
    Fix lastLandingVelocityY = 0;
    Fix groundAccel = 0;
    Fix groundAccelSecondary = 0;
    Vec2 groundNormal{0, fx(1)};
    int hitlag = 0;
    int hitstun = 0;
    uint32_t stateFlags = 0;
    uint32_t commandFlags = 0;
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
    int wallContactSide = 0;
    int wallContactSegment = -1;
    int wallContactTimer = 254;
    int wallJumpsUsed = 0;
    int turnFramesToChangeDirection = 0;
    int turnRunInitialFacing = 1;
    bool turnHasTurned = false;
    bool turnDashBuffered = false;
    int runDirectTimer = 0;
    int runBrakeTimer = 0;
    bool runBrakeAnimationFrozen = false;
    int guardMinHoldTimer = 0;
    int guardSetoffTimer = 0;
    bool guardReleaseQueued = false;
    Fix guardPoseFrame = fx(10);
    Fix guardPoseBlend = 0;
    int stickXTiltTimer = 254;
    int stickYTiltTimer = 254;
    InputBuffer input;
    std::array<BonePose, kBoneCount> bones{};
    AnimationPose hsdPose;
    AnimationPose hsdBlendFromPose;
    int hsdBlendFrames = 0;
    Vec3 hsdTransN = {};
    Vec3 previousHsdTransN = {};
    Vec3 hsdTransNOffset = {};
    std::vector<JointWorldTransform> hsdJointWorldTransforms;
    std::vector<Vec3> hsdJointWorldPositions;
    std::vector<Capsule> hsdHurtboxCapsules;
    std::vector<HurtboxState> hurtboxStates;
    std::vector<ActiveHitbox> activeHitboxes;
    std::vector<int> fightersHitThisAction;
};

struct WorldSnapshot {
    int frame = 0;
    std::vector<FighterSnapshot> fighters;
};

struct World {
    int frame = 0;
    StageDefinition stage;
    std::vector<FighterDefinition> fighterDefs;
    std::vector<FighterRuntime> fighters;
};

StageDefinition makeBattlefieldTrainingStage();
World makeTrainingWorld();
World makeTrainingWorld(int p1FighterDef, int p2FighterDef);
void tickWorld(World& world, const std::vector<InputFrame>& inputs);
void changeFighterState(World& world, FighterRuntime& fighter, const std::string& stateName, int lagFrames = 0, int blendFrames = 0);
WorldSnapshot saveWorld(const World& world);
void loadWorld(World& world, const WorldSnapshot& snapshot);
const FighterState& currentState(const World& world, const FighterRuntime& fighter);
int frameInState(const FighterRuntime& fighter);
bool fighterFlag(const FighterRuntime& fighter, int flag);
void setFighterFlag(FighterRuntime& fighter, int flag, bool value);
bool fighterCommandFlag(const FighterRuntime& fighter, int flag);
void setFighterCommandFlag(FighterRuntime& fighter, int flag, bool value);
void lockFighterEcb(FighterRuntime& fighter, int frames);
void unlockFighterEcb(FighterRuntime& fighter);
void calculateEcb(const FighterDefinition& def, FighterRuntime& fighter, bool updatePrevious);

} // namespace pf
