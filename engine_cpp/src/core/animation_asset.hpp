#pragma once

#include "core/animation.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pf {

struct HsdFighterBoneTable {
    int head = -1;
    int rightArm = -1;
    int leftLeg = -1;
    int rightLeg = -1;
    int leftArm = -1;
    int itemHold = -1;
    int shield = -1;
    int topOfHead = -1;
    int leftFoot = -1;
    int rightFoot = -1;
};

struct HsdHurtbox {
    int index = -1;
    int bone = -1;
    std::string type;
    bool grabbable = true;
    Vec3 start;
    Vec3 end;
    Fix radius = 0;
};

struct HsdEnvironmentCollision {
    std::array<int, 6> bones{-1, -1, -1, -1, -1, -1};
    Fix multiplier = 0;
    Fix ledgeGrabWidth = 0;
    Fix ledgeGrabYOffset = 0;
    Fix ledgeGrabHeight = 0;
};

struct HsdFighterAttributes {
    Fix initialWalkSpeed = 0;
    Fix walkAcceleration = 0;
    Fix maxWalkSpeed = 0;
    Fix midWalkPoint = 0;
    Fix fastWalkSpeed = 0;
    Fix friction = 0;
    Fix initialDashSpeed = 0;
    Fix dashRunAccelerationA = 0;
    Fix dashRunAccelerationB = 0;
    Fix initialRunSpeed = 0;
    Fix runAnimationScale = 0;
    Fix maxRunBrakeFrames = 0;
    Fix groundMaxHorizontalVelocity = 0;
    Fix jumpStartupLag = 0;
    Fix initialHorizontalJumpVelocity = 0;
    Fix initialVerticalJumpVelocity = 0;
    Fix groundToAirJumpMomentumMultiplier = 0;
    Fix maximumShorthopHorizontalVelocity = 0;
    Fix maximumShorthopVerticalVelocity = 0;
    Fix verticalAirJumpMultiplier = 0;
    Fix horizontalAirJumpMultiplier = 0;
    int numberOfJumps = 0;
    Fix gravity = 0;
    Fix terminalVelocity = 0;
    Fix aerialSpeed = 0;
    Fix aerialFriction = 0;
    Fix maxAerialHorizontalSpeed = 0;
    Fix airFriction = 0;
    Fix fastFallTerminalVelocity = 0;
    Fix airMaxHorizontalVelocity = 0;
    Fix framesToChangeDirectionOnStandingTurn = 0;
    Fix weight = 0;
    Fix modelScale = 0;
    Fix shieldSize = 0;
    Fix shieldBreakInitialVelocity = 0;
    Fix normalLandingLag = 0;
    Fix nairLandingLag = 0;
    Fix fairLandingLag = 0;
    Fix bairLandingLag = 0;
    Fix uairLandingLag = 0;
    Fix dairLandingLag = 0;
    Fix wallJumpHorizontalVelocity = 0;
    Fix wallJumpVerticalVelocity = 0;
    Fix ledgeJumpHorizontalVelocity = 0;
    Fix ledgeJumpVerticalVelocity = 0;
};

struct HsdActionCommand {
    uint8_t code = 0;
    std::vector<uint8_t> bytes;
};

struct HsdActionScript {
    int actionIndex = -1;
    std::string name;
    std::array<int, 54> commonBoneLookup{};
    std::vector<HsdActionCommand> commands;
};

struct HsdModelPartAnimationSet {
    int startingBone = 0;
    std::vector<int> entries;
    std::vector<AnimationClip> animations;
};

struct HsdMeshTexture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

struct HsdMeshVertexInfluence {
    int bone = -1;
    float weight = 0.0f;
};

struct HsdMeshVertex {
    Vec3 position;
    Vec3 normal;
    float u = 0.0f;
    float v = 0.0f;
    std::array<uint8_t, 4> color{255, 255, 255, 255};
    std::array<HsdMeshVertexInfluence, 6> influences{};
};

struct HsdMeshBatch {
    int parentBone = -1;
    int singleBindBone = -1;
    int dobjIndex = -1;
    int modelPartIndex = -1;
    int modelPartState = -1;
    bool hiddenByVisibilityTable = false;
    uint32_t parentFlags = 0;
    uint32_t polygonFlags = 0;
    bool hasEnvelopes = false;
    bool unknown2 = false;
    bool shapeSetAverage = false;
    int texture = -1;
    int textureColorOperation = 0;
    int textureAlphaOperation = 0;
    float textureBlend = 0.0f;
    std::array<uint8_t, 4> materialColor{255, 255, 255, 255};
    std::vector<HsdMeshVertex> vertices;
};

struct HsdFighterMesh {
    std::vector<std::array<float, 16>> inverseBindMatrices;
    std::vector<HsdMeshTexture> textures;
    std::vector<HsdMeshBatch> batches;
};

struct HsdFighterAnimationAsset {
    std::string name;
    std::vector<AnimationJoint> skeleton;
    std::vector<AnimationClip> clips;
    std::vector<HsdModelPartAnimationSet> modelPartAnimations;
    HsdFighterMesh mesh;
    bool hasShieldPose = false;
    AnimationPose shieldPose;
    HsdFighterBoneTable fighterBones;
    std::array<int, 54> commonBoneLookup{};
    bool hasAttributes = false;
    HsdFighterAttributes attributes;
    std::vector<HsdHurtbox> hurtboxes;
    bool hasEnvironmentCollision = false;
    HsdEnvironmentCollision environmentCollision;
    std::vector<HsdActionScript> actionScripts;
};

HsdFighterAnimationAsset loadHsdFighterAnimationAsset(const std::string& path);
const AnimationClip* findClipByActionIndex(const HsdFighterAnimationAsset& asset, int actionIndex);
const AnimationClip* findClipByName(const HsdFighterAnimationAsset& asset, const std::string& name);
const HsdActionScript* findActionScriptByActionIndex(const HsdFighterAnimationAsset& asset, int actionIndex);

} // namespace pf
