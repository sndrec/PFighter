#include "core/simulation.hpp"

#include "core/state_functions.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace pf {

static void evaluatePose(const FighterDefinition& def, const FighterState& state, FighterRuntime& fighter);
static bool segmentYAtX(const StageSegment& segment, Fix x, Fix& y);
static int fallbackActionIndex(const std::string& animation);

struct HsdFighterAssetSpec {
    const char* displayName;
    const char* fileName;
    bool shieldSizeScalesWithHealth = true;
};

static const std::array<HsdFighterAssetSpec, 7>& meleeTrainingRoster() {
    static const std::array<HsdFighterAssetSpec, 7> roster{{
        {"Mario", "mario_hsd.pfighter.bin"},
        {"Donkey Kong", "donkey_kong_hsd.pfighter.bin"},
        {"Bowser", "bowser_hsd.pfighter.bin"},
        {"Captain Falcon", "captain_falcon_hsd.pfighter.bin"},
        {"Peach", "peach_hsd.pfighter.bin"},
        {"Yoshi", "yoshi_hsd.pfighter.bin", false},
        {"Game & Watch", "game_and_watch_hsd.pfighter.bin"},
    }};
    return roster;
}

static std::filesystem::path findFighterAssetPath(const std::string& fileName) {
    const std::array<std::filesystem::path, 5> candidates = {
        std::filesystem::path("engine_cpp/data/fighters") / fileName,
        std::filesystem::path("data/fighters") / fileName,
        std::filesystem::path("../data/fighters") / fileName,
        std::filesystem::path("../../engine_cpp/data/fighters") / fileName,
        std::filesystem::path("../../data/fighters") / fileName,
    };
    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

static std::shared_ptr<const HsdFighterAnimationAsset> cachedHsdFighterAsset(const std::string& fileName) {
    static std::unordered_map<std::string, std::shared_ptr<const HsdFighterAnimationAsset>> assets;
    if (const auto found = assets.find(fileName); found != assets.end()) {
        return found->second;
    }
    const std::filesystem::path assetPath = findFighterAssetPath(fileName);
    if (assetPath.empty()) {
        throw std::runtime_error("missing binary fighter asset: engine_cpp/data/fighters/" + fileName);
    }
    std::shared_ptr<const HsdFighterAnimationAsset> asset =
        std::make_shared<HsdFighterAnimationAsset>(loadHsdFighterAnimationAsset(assetPath.string()));
    assets.emplace(fileName, asset);
    return asset;
}

static std::filesystem::path findBattlefieldStagePath() {
    const std::array<std::filesystem::path, 5> candidates = {
        std::filesystem::path("engine_cpp/data/stages/battlefield_melee.pstage.bin"),
        std::filesystem::path("data/stages/battlefield_melee.pstage.bin"),
        std::filesystem::path("../data/stages/battlefield_melee.pstage.bin"),
        std::filesystem::path("../../engine_cpp/data/stages/battlefield_melee.pstage.bin"),
        std::filesystem::path("../../data/stages/battlefield_melee.pstage.bin"),
    };
    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

static std::filesystem::path findMeleeCommonDataPath() {
    const std::array<std::filesystem::path, 5> candidates = {
        std::filesystem::path("engine_cpp/data/melee_common.pcommon.bin"),
        std::filesystem::path("data/melee_common.pcommon.bin"),
        std::filesystem::path("../data/melee_common.pcommon.bin"),
        std::filesystem::path("../../engine_cpp/data/melee_common.pcommon.bin"),
        std::filesystem::path("../../data/melee_common.pcommon.bin"),
    };
    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

class StageBinaryReader {
public:
    explicit StageBinaryReader(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            throw std::runtime_error("failed to open stage asset");
        }
        file.seekg(0, std::ios::end);
        const std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        data.resize(static_cast<size_t>(size));
        if (!data.empty()) {
            file.read(reinterpret_cast<char*>(data.data()), size);
        }
    }

    bool hasMagic(const char* magic) const {
        return data.size() >= 4 && std::memcmp(data.data(), magic, 4) == 0;
    }

    void skip(size_t count) {
        require(count);
        position += count;
    }

    uint8_t readU8() {
        require(1);
        return data[position++];
    }

    bool readBool() {
        return readU8() != 0;
    }

    int32_t readI32() {
        require(4);
        int32_t value = 0;
        std::memcpy(&value, data.data() + position, 4);
        position += 4;
        return value;
    }

    float readF32() {
        require(4);
        float value = 0.0f;
        std::memcpy(&value, data.data() + position, 4);
        position += 4;
        return value;
    }

    std::string readString() {
        const int32_t size = readI32();
        require(static_cast<size_t>(size));
        std::string value(reinterpret_cast<const char*>(data.data() + position), static_cast<size_t>(size));
        position += static_cast<size_t>(size);
        return value;
    }

private:
    std::vector<uint8_t> data;
    size_t position = 0;

    void require(size_t count) const {
        if (position + count > data.size()) {
            throw std::runtime_error("truncated stage asset");
        }
    }
};

static Fix readCommonFix(StageBinaryReader& reader) {
    return fxFromFloat(reader.readF32());
}

static MeleeCommonData loadMeleeCommonData(const std::filesystem::path& path) {
    StageBinaryReader reader(path);
    if (!reader.hasMagic("PFCM")) {
        throw std::runtime_error("invalid Melee common data magic");
    }
    reader.skip(4);
    const int version = reader.readI32();
    if (version != 6) {
        throw std::runtime_error("unsupported Melee common data version");
    }

    MeleeCommonData common;
    common.stickXTiltThresholdX8 = readCommonFix(reader);
    common.stickYTiltThresholdXC = readCommonFix(reader);
    common.walkInputThresholdX24 = readCommonFix(reader);
    common.walkMiddleThresholdX28 = readCommonFix(reader);
    common.walkFastThresholdX2C = readCommonFix(reader);
    common.walkAccelScaleX30 = readCommonFix(reader);
    common.turnInputThresholdX34 = readCommonFix(reader);
    common.turnRunInputThresholdX38 = readCommonFix(reader);
    common.dashInputThresholdX3C = readCommonFix(reader);
    common.dashStickWindowX40 = reader.readI32();
    common.dashEarlyInterruptWindowX44 = reader.readI32();
    common.dashItemThrowWindowX48 = reader.readI32();
    common.dashLateInterruptWindowX4C = reader.readI32();
    common.dashDecayX54 = readCommonFix(reader);
    common.runInputThresholdX58 = readCommonFix(reader);
    common.runAccelScaleX5C = readCommonFix(reader);
    common.groundFrictionScaleX60 = readCommonFix(reader);
    common.turnFrictionScaleAboveWalkMaxX6C = readCommonFix(reader);
    common.tapJumpThresholdX70 = readCommonFix(reader);
    common.tapJumpWindowX74 = reader.readI32();
    common.jumpBackwardThresholdX78 = readCommonFix(reader);
    common.tapJumpReleaseThresholdX7C = readCommonFix(reader);
    common.aerialJumpStickThresholdX80 = readCommonFix(reader);
    common.fastfallStickThresholdX88 = readCommonFix(reader);
    common.fastfallStickWindowX8C = reader.readI32();
    common.squatStickThresholdX90 = readCommonFix(reader);
    common.squatReleaseThresholdX94 = readCommonFix(reader);
    common.startShieldHealthX260 = readCommonFix(reader);
    common.minShieldScaleX264 = readCommonFix(reader);
    common.guardMinHoldFramesX268 = reader.readI32();
    common.shieldDrainRateX278 = readCommonFix(reader);
    common.shieldRegenRateX27C = readCommonFix(reader);
    common.shieldDamageScaleX284 = readCommonFix(reader);
    common.shieldDamageBaseX288 = readCommonFix(reader);
    common.shieldSetoffScaleX28C = readCommonFix(reader);
    common.shieldSetoffBaseX290 = readCommonFix(reader);
    common.shieldPushbackScaleX294 = readCommonFix(reader);
    common.shieldPushbackMaxX298 = readCommonFix(reader);
    common.shieldReflectInputWindowX2A0 = reader.readI32();
    common.guardHitReleaseLockoutX2B8 = reader.readI32();
    common.hardShieldSizeScaleX2D4 = readCommonFix(reader);
    common.lightShieldSizeScaleX2D8 = readCommonFix(reader);
    common.hardShieldDamageScaleX2DC = readCommonFix(reader);
    common.lightShieldDamageScaleX2E0 = readCommonFix(reader);
    common.hardShieldSetoffScaleX2E4 = readCommonFix(reader);
    common.lightShieldSetoffScaleX2E8 = readCommonFix(reader);
    common.hardShieldDrainScaleX2EC = readCommonFix(reader);
    common.lightShieldDrainScaleX2F0 = readCommonFix(reader);
    common.shieldAlphaMinX2F4 = readCommonFix(reader);
    common.attackerShieldPushbackScaleX3E0 = readCommonFix(reader);
    common.attackerShieldPushbackBaseX3E4 = readCommonFix(reader);
    common.shieldKnockbackFrameDecayX3E8 = readCommonFix(reader);
    common.shieldGroundFrictionMultiplierX3EC = readCommonFix(reader);
    common.spotDodgeStickThresholdX314 = readCommonFix(reader);
    common.spotDodgeStickWindowX318 = reader.readI32();
    common.rollStickThresholdX31C = readCommonFix(reader);
    common.rollStickWindowX320 = reader.readI32();
    common.rollFromGuardFlagX324 = reader.readI32();
    common.escapeAirDeadzoneX32C.x = readCommonFix(reader);
    common.escapeAirDeadzoneX32C.y = readCommonFix(reader);
    common.escapeAirTimerX334 = reader.readI32();
    common.escapeAirForceX338 = readCommonFix(reader);
    common.escapeAirDecayX33C = readCommonFix(reader);
    common.fallSpecialDriftX340 = readCommonFix(reader);
    common.landingFallSpecialLagX344 = reader.readI32();
    common.runStopTurnLagX410 = reader.readI32();
    common.runBrakeAnimFreezeVelocityX42C = readCommonFix(reader);
    common.runDirectFramesX430 = reader.readI32();
    common.jumpMomentumYScaleX438 = readCommonFix(reader);
    common.animVelocityScaleX440 = readCommonFix(reader);
    common.sdiMinStickMagX4B0 = readCommonFix(reader);
    common.sdiStickWindowX4B4 = reader.readI32();
    common.sdiPosScaleX4B8 = readCommonFix(reader);
    common.shieldAsdiPosScaleX4BC = readCommonFix(reader);
    common.shieldSdiDistanceX4C0 = readCommonFix(reader);
    common.platformDropStickThresholdX464 = readCommonFix(reader);
    common.platformDropStickWindowX468 = reader.readI32();
    common.platformDropInitialVelocityX46C = readCommonFix(reader);
    common.platformDropAnimationFramesX470 = reader.readI32();
    common.teeterWalkInputThresholdX474 = readCommonFix(reader);
    common.teeterForwardDistanceX478 = readCommonFix(reader);
    common.teeterBackwardDistanceX47C = readCommonFix(reader);
    common.ledgeNoGrabDownThresholdX480 = readCommonFix(reader);
    common.cliffOptionStickThresholdX494 = readCommonFix(reader);
    common.ledgeCooldownX498 = reader.readI32();
    common.passiveWallTimerX760 = reader.readI32();
    common.passiveWallIntangibilityX764 = reader.readI32();
    common.wallJumpInputWindowX768 = reader.readI32();
    common.wallJumpStickThresholdX76C = readCommonFix(reader);
    common.wallJumpStickWindowX770 = reader.readI32();
    common.wallJumpStartupX774 = reader.readI32();
    common.passiveWallVelYBaseX778 = readCommonFix(reader);
    common.aerialAttackAngleTanX20 = readCommonFix(reader);
    common.aerialAttackDeadzoneXDC = readCommonFix(reader);
    common.aerialAttackDeadzoneXE0 = readCommonFix(reader);
    common.shieldStickSmoothingX44C = readCommonFix(reader);
    common.attackS3StickThresholdX98 = readCommonFix(reader);
    common.attackS3HiAngleX9C = readCommonFix(reader);
    common.attackS3HiSAngleXA0 = readCommonFix(reader);
    common.attackS3LwSAngleXA4 = readCommonFix(reader);
    common.attackS3LwAngleXA8 = readCommonFix(reader);
    common.attackHi3StickThresholdYxAC = readCommonFix(reader);
    common.attackLw3StickThresholdYxB0 = readCommonFix(reader);
    common.attackS4HiAngleXB8 = readCommonFix(reader);
    common.attackS4HiSAngleXBC = readCommonFix(reader);
    common.attackS4LwSAngleXC0 = readCommonFix(reader);
    common.attackS4LwAngleXC4 = readCommonFix(reader);
    common.attackHi4StickThresholdYxCC = readCommonFix(reader);
    common.attackHi4StickWindowXD0 = reader.readI32();
    common.attackLw4StickThresholdYxD4 = readCommonFix(reader);
    common.attackLw4StickWindowXD8 = reader.readI32();
    common.lCancelInputWindowXE4 = reader.readI32();
    common.lCancelLandingLagDivisorXE8 = readCommonFix(reader);
    return common;
}

static MeleeCommonData loadMeleeCommonData() {
    const std::filesystem::path path = findMeleeCommonDataPath();
    if (path.empty()) {
        throw std::runtime_error("missing binary Melee common data asset: engine_cpp/data/melee_common.pcommon.bin");
    }
    return loadMeleeCommonData(path);
}

static SegmentLineKind lineKindFromId(uint8_t value) {
    switch (value) {
    case 1: return SegmentLineKind::Ceiling;
    case 2: return SegmentLineKind::RightWall;
    case 3: return SegmentLineKind::LeftWall;
    default: return SegmentLineKind::Floor;
    }
}

static bool loadMeleeStageCollision(StageDefinition& stage, const std::filesystem::path& path) {
    try {
        StageBinaryReader reader(path);
        if (!reader.hasMagic("PFST")) {
            return false;
        }
        reader.skip(4);
        const int version = reader.readI32();
        if (version != 2) {
            return false;
        }
        stage.name = reader.readString();
        stage.segments.clear();
        stage.ledges.clear();
        const int32_t segmentCount = reader.readI32();
        stage.segments.reserve(static_cast<size_t>(segmentCount));
        for (int32_t i = 0; i < segmentCount; ++i) {
            const Fix x1 = fxFromFloat(reader.readF32());
            const Fix y1 = fxFromFloat(reader.readF32());
            const Fix x2 = fxFromFloat(reader.readF32());
            const Fix y2 = fxFromFloat(reader.readF32());
            const bool semisolid = reader.readU8() != 0;
            const SegmentLineKind kind = lineKindFromId(reader.readU8());
            const bool leftLedge = reader.readBool();
            const bool rightLedge = reader.readBool();
            const bool dynamic = reader.readBool();
            const int nextLine = reader.readI32();
            const int previousLine = reader.readI32();
            const Fix friction = fxFromFloat(reader.readF32());
            stage.segments.push_back({
                {x1, y1},
                {x2, y2},
                friction,
                semisolid ? SegmentType::Semisolid : SegmentType::Solid,
                leftLedge,
                rightLedge,
                kind,
                dynamic,
                nextLine,
                previousLine,
            });
        }
        return !stage.segments.empty();
    } catch (...) {
        return false;
    }
}

static int mainFloorSegmentIndex(const StageDefinition& stage) {
    int best = 0;
    Fix bestLength = -1;
    for (size_t i = 0; i < stage.segments.size(); ++i) {
        const StageSegment& segment = stage.segments[i];
        if (segment.type != SegmentType::Solid || segment.lineKind != SegmentLineKind::Floor) {
            continue;
        }
        const Fix length = fxAbs(segment.end.x - segment.start.x);
        if (length > bestLength) {
            bestLength = length;
            best = static_cast<int>(i);
        }
    }
    return best;
}

static int roundedFrames(Fix value) {
    return std::max(0, static_cast<int>(fxToFloat(value) + 0.5f));
}

static Vec3 scaledVec3(Vec3 value, Fix scale) {
    value.x = fxMul(value.x, scale);
    value.y = fxMul(value.y, scale);
    value.z = fxMul(value.z, scale);
    return value;
}

static void applyHsdFighterAttributes(FighterDefinition& def, const HsdFighterAttributes& source) {
    FighterProperties& attr = def.properties;
    attr.walkInitVel = source.initialWalkSpeed;
    attr.walkAccel = source.walkAcceleration;
    attr.walkMaxVel = source.maxWalkSpeed;
    attr.slowWalkMax = source.midWalkPoint;
    attr.midWalkPoint = source.midWalkPoint;
    attr.fastWalkMin = source.fastWalkSpeed;
    attr.grFriction = source.friction;
    attr.dashInitialVelocity = source.initialDashSpeed;
    if (source.dashRunAccelerationA != 0 || source.dashRunAccelerationB != 0) {
        attr.dashRunAccelerationA = source.dashRunAccelerationA;
        attr.dashRunAccelerationB = source.dashRunAccelerationB;
    }
    attr.dashRunTerminalVelocity = source.initialRunSpeed;
    attr.runAnimationScaling = source.runAnimationScale;
    if (source.maxRunBrakeFrames != 0) {
        attr.maxRunBrakeFrames = roundedFrames(source.maxRunBrakeFrames);
    }
    attr.groundMaxHorizontalVelocity = source.groundMaxHorizontalVelocity;
    attr.jumpHInitialVelocity = source.initialHorizontalJumpVelocity;
    attr.jumpVInitialVelocity = source.initialVerticalJumpVelocity;
    attr.hopVInitialVelocity = source.maximumShorthopVerticalVelocity;
    attr.groundToAirJumpMomentumMultiplier = source.groundToAirJumpMomentumMultiplier;
    attr.jumpHMaxVelocity = source.maximumShorthopHorizontalVelocity;
    attr.airJumpVMultiplier = source.verticalAirJumpMultiplier;
    attr.airJumpHMultiplier = source.horizontalAirJumpMultiplier;
    attr.maxJumps = source.numberOfJumps;
    attr.modelScale = source.modelScale != 0 ? source.modelScale : fx(1);
    attr.grav = source.gravity;
    attr.terminalVel = source.terminalVelocity;
    attr.airDriftStickMul = source.aerialSpeed;
    attr.aerialDriftBase = source.aerialFriction;
    attr.aerialFriction = source.aerialFriction;
    attr.maxAerialHorizontalSpeed = source.maxAerialHorizontalSpeed;
    attr.airFriction = source.airFriction;
    attr.airDriftMax = source.maxAerialHorizontalSpeed;
    if (source.airMaxHorizontalVelocity != 0) {
        attr.airMaxHorizontalVelocity = source.airMaxHorizontalVelocity;
    }
    attr.initialShieldSize = source.shieldSize;
    attr.shieldBreakInitialVelocity = source.shieldBreakInitialVelocity;
    attr.gravity = source.gravity;
    attr.terminalVelocity = source.terminalVelocity;
    attr.fastFallTerminalVelocity = source.fastFallTerminalVelocity;
    attr.normalLandingLag = roundedFrames(source.normalLandingLag);
    attr.nairLandingLag = roundedFrames(source.nairLandingLag);
    attr.fairLandingLag = roundedFrames(source.fairLandingLag);
    attr.bairLandingLag = roundedFrames(source.bairLandingLag);
    attr.uairLandingLag = roundedFrames(source.uairLandingLag);
    attr.dairLandingLag = roundedFrames(source.dairLandingLag);
    attr.framesToChangeDirectionOnStandingTurn = roundedFrames(source.framesToChangeDirectionOnStandingTurn);
    attr.weight = source.weight;
    attr.jumpStartupLag = roundedFrames(source.jumpStartupLag);
    attr.initialWalkSpeed = source.initialWalkSpeed;
    attr.initialDashSpeed = source.initialDashSpeed;
    attr.initialRunSpeed = source.initialRunSpeed;
    attr.walkAcceleration = source.walkAcceleration;
    attr.runAcceleration = source.dashRunAccelerationA != 0 ? source.dashRunAccelerationA : attr.dashRunAccelerationA;
    attr.maxWalkSpeed = source.maxWalkSpeed;
    attr.friction = source.friction;
    attr.aerialAcceleration = source.aerialSpeed;
    attr.initialHorizontalJumpVelocity = source.initialHorizontalJumpVelocity;
    attr.initialVerticalJumpVelocity = source.initialVerticalJumpVelocity;
    attr.maximumShorthopVerticalVelocity = source.maximumShorthopVerticalVelocity;
    attr.wallJumpHorizontalVelocity = source.wallJumpHorizontalVelocity;
    attr.wallJumpVerticalVelocity = source.wallJumpVerticalVelocity;
    attr.ledgeJumpHorizontalVelocity = source.ledgeJumpHorizontalVelocity;
    attr.ledgeJumpVerticalVelocity = source.ledgeJumpVerticalVelocity;
    def.shield.startSizeHardShield = source.shieldSize;
    def.shield.maxHealth = attr.common.startShieldHealthX260;
}

static void applyHsdAnimationLengths(FighterDefinition& def) {
    if (!def.hasHsdAsset || !def.hsdAsset) {
        return;
    }
    constexpr uint32_t kMeleeActionFlagAnimPhysics = 0x80000000u;
    constexpr uint32_t kMeleeActionFlagLoopAnimation = 0x40000000u;
    const auto usesGenericTransNPhysics = [](const FighterState& state, const AnimationClip& clip) {
        if ((clip.actionFlags & kMeleeActionFlagAnimPhysics) == 0) {
            return false;
        }
        // These common movement states have explicit Melee physics callbacks
        // driven by gr_vel/self_vel, not the generic TransN ground-physics path.
        if (state.name == "Dash" || state.name == "Run" || state.name == "RunDirect" ||
            state.name == "RunBrake" || state.name == "TurnRun")
        {
            return false;
        }
        return state.useAnimPhysics;
    };
    for (FighterState& state : def.states) {
        const int actionIndex = state.animationActionIndex >= 0 ? state.animationActionIndex : fallbackActionIndex(state.animation);
        if (actionIndex < 0) {
            continue;
        }
        if (const AnimationClip* clip = findClipByActionIndex(*def.hsdAsset, actionIndex)) {
            if (state.name != "JumpSquat") {
                state.animationLengthFrames = std::max(1, static_cast<int>(fxToFloat(clip->frameCount) + 0.5f));
            }
            state.defaultAnimationBlendFrames = std::max(0, clip->defaultBlendFrames);
            state.useAnimPhysics = usesGenericTransNPhysics(state, *clip);
            state.loopAnimation = state.loopAnimation || (clip->actionFlags & kMeleeActionFlagLoopAnimation) != 0;
        }
    }
}

static FighterDefinition makeHsdFighterDefinition(const HsdFighterAssetSpec& spec, const MeleeCommonData& common) {
    FighterDefinition def = makeDebugRook();
    def.name = spec.displayName;
    def.properties.common = common;
    def.shield.maxHealth = common.startShieldHealthX260;
    def.hsdAsset = cachedHsdFighterAsset(spec.fileName);
    def.hasHsdAsset = true;
    if (def.hsdAsset->hasAttributes) {
        applyHsdFighterAttributes(def, def.hsdAsset->attributes);
    }
    def.properties.shieldSizeScalesWithHealth = spec.shieldSizeScalesWithHealth;
    if (def.hsdAsset->hasEnvironmentCollision) {
        def.properties.ledgeSnapX = def.hsdAsset->environmentCollision.ledgeGrabWidth;
        def.properties.ledgeSnapY = def.hsdAsset->environmentCollision.ledgeGrabYOffset;
        def.properties.ledgeSnapHeight = def.hsdAsset->environmentCollision.ledgeGrabHeight;
    }
    applyHsdAnimationLengths(def);
    return def;
}

static std::array<Fix, 16> multiplyMatrix4(const std::array<Fix, 16>& a, const std::array<Fix, 16>& b) {
    std::array<Fix, 16> result{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            Fix value = 0;
            for (int k = 0; k < 4; ++k) {
                value += fxMul(a[static_cast<size_t>(row * 4 + k)], b[static_cast<size_t>(k * 4 + col)]);
            }
            result[static_cast<size_t>(row * 4 + col)] = value;
        }
    }
    return result;
}

static std::array<Fix, 9> rotationPart(const std::array<Fix, 16>& matrix) {
    return {
        matrix[0], matrix[1], matrix[2],
        matrix[4], matrix[5], matrix[6],
        matrix[8], matrix[9], matrix[10],
    };
}

static std::array<Fix, 16> fighterModelMatrix(const FighterDefinition& def, const FighterRuntime& fighter) {
    Fix cosZ = fx(1);
    Fix sinZ = 0;
    if (fighter.grounded) {
        const float angle = -std::atan2(fxToFloat(fighter.groundNormal.x), fxToFloat(fighter.groundNormal.y));
        cosZ = fxFromFloat(std::cos(angle));
        sinZ = fxFromFloat(std::sin(angle));
    }
    const Fix modelScale = def.properties.modelScale;
    return {
        fxMul(cosZ, modelScale), -fxMul(sinZ, modelScale), 0, fighter.position.x,
        fxMul(sinZ, modelScale), fxMul(cosZ, modelScale), 0, fighter.position.y,
        0, 0, modelScale, 0,
        0, 0, 0, fx(1),
    };
}

static std::vector<JointWorldTransform> fighterHsdWorldTransforms(const FighterDefinition& def, const FighterRuntime& fighter) {
    std::vector<JointWorldTransform> localTransforms = jointWorldTransforms(def.hsdAsset->skeleton, fighter.hsdPose);
    const std::array<Fix, 16> modelMatrix = fighterModelMatrix(def, fighter);
    for (JointWorldTransform& transform : localTransforms) {
        transform.matrix = multiplyMatrix4(modelMatrix, transform.matrix);
        transform.translation = {transform.matrix[3], transform.matrix[7], transform.matrix[11]};
        transform.rotation = rotationPart(transform.matrix);
    }
    return localTransforms;
}

static std::vector<Vec3> translationsFromTransforms(const std::vector<JointWorldTransform>& transforms) {
    std::vector<Vec3> positions;
    positions.reserve(transforms.size());
    for (const JointWorldTransform& transform : transforms) {
        positions.push_back(transform.translation);
    }
    return positions;
}

static bool stageBuildFloorLine(const StageDefinition& stage, int segmentIndex) {
    return segmentIndex >= 0 &&
        segmentIndex < static_cast<int>(stage.segments.size()) &&
        stage.segments[static_cast<size_t>(segmentIndex)].lineKind == SegmentLineKind::Floor;
}

static Vec2 meleeFloorChainLedgePoint(const StageDefinition& stage, int segmentIndex, bool leftLedge) {
    int current = segmentIndex;
    for (int guard = 0; guard < static_cast<int>(stage.segments.size()) && stageBuildFloorLine(stage, current); ++guard) {
        const StageSegment& segment = stage.segments[static_cast<size_t>(current)];
        const int linked = leftLedge ? segment.nextLine : segment.previousLine;
        if (!stageBuildFloorLine(stage, linked)) {
            break;
        }
        current = linked;
    }

    const StageSegment& segment = stage.segments[static_cast<size_t>(current)];
    return leftLedge ? segment.start : segment.end;
}

static bool meleeFloorEndpointIsExposedLedge(const StageDefinition& stage, int segmentIndex, bool leftLedge) {
    if (!stageBuildFloorLine(stage, segmentIndex)) {
        return false;
    }
    const StageSegment& segment = stage.segments[static_cast<size_t>(segmentIndex)];
    const int linked = leftLedge ? segment.nextLine : segment.previousLine;
    return !stageBuildFloorLine(stage, linked);
}

StageDefinition makeBattlefieldTrainingStage() {
    StageDefinition stage;
    if (const std::filesystem::path battlefieldPath = findBattlefieldStagePath(); !battlefieldPath.empty()) {
        if (loadMeleeStageCollision(stage, battlefieldPath)) {
            for (size_t i = 0; i < stage.segments.size(); ++i) {
                const StageSegment& segment = stage.segments[i];
                if (segment.leftLedge && meleeFloorEndpointIsExposedLedge(stage, static_cast<int>(i), true)) {
                    stage.ledges.push_back({meleeFloorChainLedgePoint(stage, static_cast<int>(i), true), -1, static_cast<int>(i)});
                }
                if (segment.rightLedge && meleeFloorEndpointIsExposedLedge(stage, static_cast<int>(i), false)) {
                    stage.ledges.push_back({meleeFloorChainLedgePoint(stage, static_cast<int>(i), false), 1, static_cast<int>(i)});
                }
            }
            stage.blastMin = {-fx(250), -fx(150)};
            stage.blastMax = {fx(250), fx(250)};
            return stage;
        }
    }
    throw std::runtime_error("missing or invalid binary Battlefield stage asset: engine_cpp/data/stages/battlefield_melee.pstage.bin");
}

static FighterRuntime makeTrainingFighter(World& world, int fighterDefIndex, Vec2 position, int facing) {
    FighterRuntime p1;
    p1.fighterDef = fighterDefIndex;
    p1.state = world.fighterDefs[static_cast<size_t>(fighterDefIndex)].stateIndex("Wait");
    p1.position = position;
    p1.previousPosition = p1.position;
    p1.facing = facing;
    p1.hsdPoseFacing = facing;
    p1.groundSegment = mainFloorSegmentIndex(world.stage);
    if (p1.groundSegment >= 0) {
        segmentYAtX(world.stage.segments[static_cast<size_t>(p1.groundSegment)], p1.position.x, p1.position.y);
    }
    p1.previousPosition = p1.position;
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighterDefIndex)];
    p1.shieldHealth = def.shield.maxHealth;
    if (def.hsdAsset) {
        p1.hsdModelPartAnimations.assign(def.hsdAsset->modelPartAnimations.size(), -1);
    }
    return p1;
}

World makeTrainingWorld(int p1FighterDef, int p2FighterDef) {
    World world;
    world.stage = makeBattlefieldTrainingStage();
    const MeleeCommonData common = loadMeleeCommonData();
    for (const HsdFighterAssetSpec& spec : meleeTrainingRoster()) {
        world.fighterDefs.push_back(makeHsdFighterDefinition(spec, common));
    }
    p1FighterDef = std::clamp(p1FighterDef, 0, static_cast<int>(world.fighterDefs.size()) - 1);
    p2FighterDef = std::clamp(p2FighterDef, 0, static_cast<int>(world.fighterDefs.size()) - 1);
    world.fighters = {
        makeTrainingFighter(world, p1FighterDef, {-fx(2), 0}, 1),
        makeTrainingFighter(world, p2FighterDef, {fx(2), 0}, -1),
    };
    for (FighterRuntime& fighter : world.fighters) {
        evaluatePose(world.fighterDefs[static_cast<size_t>(fighter.fighterDef)], currentState(world, fighter), fighter);
        calculateEcb(world.fighterDefs[static_cast<size_t>(fighter.fighterDef)], fighter, true);
        calculateEcb(world.fighterDefs[static_cast<size_t>(fighter.fighterDef)], fighter, true);
        if (fighter.groundSegment >= 0 && fighter.groundSegment < static_cast<int>(world.stage.segments.size())) {
            Fix floorY = 0;
            if (segmentYAtX(world.stage.segments[static_cast<size_t>(fighter.groundSegment)], fighter.position.x + fighter.ecb.points[3].x, floorY)) {
                fighter.position.y = floorY - fighter.ecb.points[3].y;
                fighter.previousPosition = fighter.position;
                evaluatePose(world.fighterDefs[static_cast<size_t>(fighter.fighterDef)], currentState(world, fighter), fighter);
                calculateEcb(world.fighterDefs[static_cast<size_t>(fighter.fighterDef)], fighter, true);
            }
        }
    }
    runStateFunctions(world, 0, currentState(world, world.fighters[0]).onEnter);
    runStateFunctions(world, 1, currentState(world, world.fighters[1]).onEnter);
    return world;
}

World makeTrainingWorld() {
    return makeTrainingWorld(0, 0);
}

const FighterState& currentState(const World& world, const FighterRuntime& fighter) {
    return world.fighterDefs[static_cast<size_t>(fighter.fighterDef)].states[static_cast<size_t>(fighter.state)];
}

int frameInState(const FighterRuntime& fighter) {
    return fighter.internalFrame - fighter.lastStateChangeFrame;
}

static void validateDesiredEcb(Ecb& ecb) {
    if (fxAbs(ecb.points[1].y - ecb.points[3].y) < fx(1)) {
        ecb.points[1].y += fx(1);
        const Fix mid = fxMul(ecb.points[1].y + ecb.points[3].y, fxFromFloat(0.5f));
        ecb.points[0].y = mid;
        ecb.points[2].y = mid;
    }
    ecb.points[1].y = std::max(ecb.points[1].y, fx(1));
    ecb.points[0].x = std::min(ecb.points[0].x, -fx(1));
    ecb.points[2].x = std::max(ecb.points[2].x, fx(1));
    if (ecb.points[1].y < ecb.points[3].y) {
        ecb.points[1].y = ecb.points[3].y + fx(1);
    }
    if (ecb.points[2].y > ecb.points[1].y || ecb.points[2].y < ecb.points[3].y) {
        const Fix mid = fxMul(ecb.points[1].y + ecb.points[3].y, fxFromFloat(0.5f));
        ecb.points[0].y = mid;
        ecb.points[2].y = mid;
    }
    if (ecb.points[1].y - ecb.points[2].y < fxFromFloat(0.001f) ||
        ecb.points[2].y - ecb.points[3].y < fxFromFloat(0.001f)) {
        ecb.points[2].y = fxMul(ecb.points[1].y + ecb.points[3].y, fxFromFloat(0.5f));
    }
    if (ecb.points[1].y - ecb.points[0].y < fxFromFloat(0.001f) ||
        ecb.points[0].y - ecb.points[3].y < fxFromFloat(0.001f)) {
        ecb.points[0].y = fxMul(ecb.points[1].y + ecb.points[3].y, fxFromFloat(0.5f));
    }
}

static void refreshEcbMetadata(Ecb& ecb, const FighterRuntime& fighter) {
    ecb.worldBottom = fighter.position + ecb.points[3];
    ecb.floorIndex = fighter.groundSegment;
}

static bool calculateHsdDesiredEcb(const FighterDefinition& def, FighterRuntime& fighter, Ecb& out) {
    if (!def.hsdAsset || !def.hsdAsset->hasEnvironmentCollision || fighter.hsdJointWorldPositions.empty()) {
        return false;
    }

    const HsdEnvironmentCollision& source = def.hsdAsset->environmentCollision;
    for (int bone : source.bones) {
        if (bone < 0 || static_cast<size_t>(bone) >= fighter.hsdJointWorldPositions.size()) {
            return false;
        }
    }

    const Vec3 topN = fighter.hsdJointWorldPositions.size() > 1
        ? fighter.hsdJointWorldPositions[1]
        : Vec3{fighter.position.x, fighter.position.y, 0};
    const Vec3 first = fighter.hsdJointWorldPositions[static_cast<size_t>(source.bones[0])];
    Fix minHorizontal = first.z;
    Fix maxHorizontal = first.z;
    Fix minY = first.y;
    Fix maxY = first.y;
    for (size_t i = 1; i < source.bones.size(); ++i) {
        const Vec3 joint = fighter.hsdJointWorldPositions[static_cast<size_t>(source.bones[i])];
        minHorizontal = std::min(minHorizontal, joint.z);
        maxHorizontal = std::max(maxHorizontal, joint.z);
        minY = std::min(minY, joint.y);
        maxY = std::max(maxY, joint.y);
    }

    const Fix halfWidth = fxMul(fxAbs(maxHorizontal - minHorizontal), fxFromFloat(0.5f));
    const Fix bottom = (fighter.grounded ? topN.y : minY) - fighter.position.y;
    const Fix top = maxY - fighter.position.y;
    const Fix sideY = source.multiplier + fxMul(bottom + top, fxFromFloat(0.5f));
    const Fix centerX = fighter.facing * topN.z;
    out.points[0] = {centerX - halfWidth, sideY};
    out.points[1] = {centerX, top};
    out.points[2] = {centerX + halfWidth, sideY};
    out.points[3] = {centerX, bottom};
    validateDesiredEcb(out);
    refreshEcbMetadata(out, fighter);
    return true;
}

static Ecb calculateDesiredEcb(const FighterDefinition& def, FighterRuntime& fighter) {
    Ecb desired;
    if (calculateHsdDesiredEcb(def, fighter, desired)) {
        return desired;
    }
    Fix highest = 0;
    Fix left = 0;
    Fix right = 0;
    for (const BonePose& bone : fighter.bones) {
        left = std::min(left, bone.position.x);
        right = std::max(right, bone.position.x);
        highest = std::max(highest, bone.position.y);
    }

    left -= fxFromFloat(0.2f);
    right += fxFromFloat(0.2f);
    highest = std::max(highest, fxFromFloat(1.0f));

    desired.points[0] = {left, fxMul(highest, fxFromFloat(0.5f))};
    desired.points[1] = {0, highest};
    desired.points[2] = {right, fxMul(highest, fxFromFloat(0.5f))};
    desired.points[3] = {0, 0};
    refreshEcbMetadata(desired, fighter);
    return desired;
}

static void loadDesiredEcb(const FighterDefinition& def, FighterRuntime& fighter) {
    fighter.desiredEcb = calculateDesiredEcb(def, fighter);
    if (fighter.ecbLockTimer > 0) {
        fighter.desiredEcb.points[3] = fighter.ecbLockBottom;
        validateDesiredEcb(fighter.desiredEcb);
        refreshEcbMetadata(fighter.desiredEcb, fighter);
    }
}

static void interpolateEcb(FighterRuntime& fighter, Fix time) {
    fighter.previousEcb = fighter.ecb;
    for (size_t i = 0; i < fighter.ecb.points.size(); ++i) {
        Vec2& current = fighter.ecb.points[i];
        const Vec2& desired = fighter.desiredEcb.points[i];
        current.x += fxMul(time, desired.x - current.x);
        current.y += fxMul(time, desired.y - current.y);
    }
    refreshEcbMetadata(fighter.ecb, fighter);
}

void calculateEcb(const FighterDefinition& def, FighterRuntime& fighter, bool updatePrevious) {
    if (updatePrevious) {
        fighter.previousEcb = fighter.ecb;
    }
    loadDesiredEcb(def, fighter);
    fighter.ecb = fighter.desiredEcb;
}

bool fighterFlag(const FighterRuntime& fighter, int flag) {
    return (fighter.stateFlags & (uint32_t{1} << flag)) != 0;
}

void setFighterFlag(FighterRuntime& fighter, int flag, bool value) {
    const uint32_t mask = uint32_t{1} << flag;
    if (value) {
        fighter.stateFlags |= mask;
    } else {
        fighter.stateFlags &= ~mask;
    }
}

bool fighterCommandFlag(const FighterRuntime& fighter, int flag) {
    return (fighter.commandFlags & (uint32_t{1} << flag)) != 0;
}

void setFighterCommandFlag(FighterRuntime& fighter, int flag, bool value) {
    const uint32_t mask = uint32_t{1} << flag;
    if (value) {
        fighter.commandFlags |= mask;
    } else {
        fighter.commandFlags &= ~mask;
    }
}

void lockFighterEcb(FighterRuntime& fighter, int frames) {
    fighter.ecbLockTimer = std::max(frames, 0);
    fighter.ecbLockBottom = fighter.desiredEcb.points[3];
}

void unlockFighterEcb(FighterRuntime& fighter) {
    fighter.ecbLockTimer = 0;
}

void changeFighterState(World& world, FighterRuntime& fighter, const std::string& stateName, int lagFrames, int blendFrames) {
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const int target = def.stateIndex(stateName);
    if (target < 0) {
        return;
    }
    const FighterState& targetState = def.states[static_cast<size_t>(target)];
    int resolvedBlendFrames = blendFrames;
    if (resolvedBlendFrames == kUseDefaultAnimationBlendFrames) {
        resolvedBlendFrames = targetState.defaultAnimationBlendFrames;
    } else if (resolvedBlendFrames == kDisableAnimationBlendFrames) {
        resolvedBlendFrames = 0;
    }
    if (resolvedBlendFrames > 0 && !fighter.hsdPose.joints.empty()) {
        fighter.hsdBlendFromPose = fighter.hsdPose;
        fighter.hsdBlendFrames = resolvedBlendFrames;
        fighter.hsdBlendElapsed = 0;
    } else {
        fighter.hsdBlendFromPose.joints.clear();
        fighter.hsdBlendFrames = 0;
        fighter.hsdBlendElapsed = 0;
    }
    fighter.hsdTransN = {};
    fighter.previousHsdTransN = {};
    fighter.hsdTransNOffset = {};
    fighter.animationFrame = 0;
    fighter.animationRate = fx(1);
    fighter.lastActionFrameExecuted = -1;
    fighter.hsdPoseFacing = fighter.facing;
    fighter.state = target;
    fighter.lastStateChangeFrame = fighter.internalFrame;
    fighter.interruptibleFrame = lagFrames > 0 ? lagFrames : targetState.initialInterruptibleFrame;
    fighter.stateAnimationLengthOverride = lagFrames > 0 ? lagFrames : 0;
    fighter.activeHitboxes.clear();
    fighter.fightersHitThisAction.clear();
    if (def.hsdAsset) {
        fighter.hsdModelPartAnimations.assign(def.hsdAsset->modelPartAnimations.size(), -1);
    } else {
        fighter.hsdModelPartAnimations.clear();
    }
    const auto it = std::find_if(world.fighters.begin(), world.fighters.end(), [&](const FighterRuntime& item) {
        return &item == &fighter;
    });
    if (it != world.fighters.end()) {
        runStateFunctions(world, static_cast<size_t>(std::distance(world.fighters.begin(), it)), currentState(world, fighter).onEnter);
    }
}

static bool ruleActive(const InterruptRule& rule, const FighterRuntime& fighter) {
    if (rule.alwaysActive) {
        return true;
    }
    const int frame = frameInState(fighter);
    if (frame < fighter.interruptibleFrame) {
        return false;
    }
    if (rule.startActive) {
        if (rule.disableFrame != 0 && frame >= rule.disableFrame) {
            return rule.enableFrame != 0 && frame >= rule.enableFrame;
        }
        return true;
    }
    if (frame < rule.enableFrame) {
        return false;
    }
    return rule.disableFrame == 0 || frame < rule.disableFrame;
}

static bool groundAllowed(GroundRequirement required, const FighterRuntime& fighter) {
    if (required == GroundRequirement::OnlyGrounded) return fighter.grounded;
    if (required == GroundRequirement::OnlyAirborne) return !fighter.grounded;
    return true;
}

static int signOf(Fix value) {
    return value < 0 ? -1 : 1;
}

static int incrementTiltTimer(int value) {
    return std::min(value + 1, 254);
}

static int updateAxisTiltTimer(Fix current, Fix previous, Fix threshold, int timer) {
    if (current >= threshold) {
        return previous >= threshold ? incrementTiltTimer(timer) : 0;
    }
    if (current <= -threshold) {
        return previous <= -threshold ? incrementTiltTimer(timer) : 0;
    }
    return 254;
}

static void updateStickTiltTimers(const World& world, FighterRuntime& fighter) {
    const FighterProperties& attr = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)].properties;
    fighter.stickXTiltTimer = updateAxisTiltTimer(
        fighter.input.frames[0].move.x,
        fighter.input.frames[1].move.x,
        attr.common.stickXTiltThresholdX8,
        fighter.stickXTiltTimer);
    fighter.stickYTiltTimer = updateAxisTiltTimer(
        fighter.input.frames[0].move.y,
        fighter.input.frames[1].move.y,
        attr.common.stickYTiltThresholdXC,
        fighter.stickYTiltTimer);
}

enum class AerialAttackDirection {
    Neutral,
    Forward,
    Back,
    High,
    Low,
};

static float stickAngle(Vec2 stick) {
    return std::atan2(fxToFloat(stick.y), std::abs(fxToFloat(stick.x)));
}

static bool cStickAerialAttackInput(const FighterRuntime& fighter, const MeleeCommonData& common) {
    return (fxAbs(fighter.input.frames[1].cStick.x) < common.aerialAttackDeadzoneXDC &&
            fxAbs(fighter.input.frames[0].cStick.x) >= common.aerialAttackDeadzoneXDC) ||
           (fxAbs(fighter.input.frames[1].cStick.y) < common.aerialAttackDeadzoneXE0 &&
            fxAbs(fighter.input.frames[0].cStick.y) >= common.aerialAttackDeadzoneXE0);
}

static AerialAttackDirection aerialAttackDirection(const FighterRuntime& fighter, const MeleeCommonData& common) {
    const Vec2 stick = cStickAerialAttackInput(fighter, common) ? fighter.input.frames[0].cStick : fighter.input.frames[0].move;
    const Fix x = stick.x;
    const Fix y = stick.y;
    if (fxAbs(x) < common.aerialAttackDeadzoneXDC && fxAbs(y) < common.aerialAttackDeadzoneXE0) {
        return AerialAttackDirection::Neutral;
    }
    const float angle = stickAngle(stick);
    const float verticalAngle = fxToFloat(common.aerialAttackAngleTanX20);
    if (angle > verticalAngle) {
        return AerialAttackDirection::High;
    }
    if (angle < -verticalAngle) {
        return AerialAttackDirection::Low;
    }
    return x * fighter.facing >= 0 ? AerialAttackDirection::Forward : AerialAttackDirection::Back;
}

static bool hasActionClip(const FighterDefinition& def, int actionIndex) {
    return !def.hasHsdAsset || !def.hsdAsset || findClipByActionIndex(*def.hsdAsset, actionIndex) != nullptr;
}

static bool cStickSideSmashInput(const FighterRuntime& fighter, const MeleeCommonData& common) {
    return fxAbs(fighter.input.frames[1].cStick.x) < common.dashInputThresholdX3C &&
           fxAbs(fighter.input.frames[0].cStick.x) >= common.dashInputThresholdX3C;
}

static bool sideSmashInput(const FighterRuntime& fighter, const MeleeCommonData& common, int& facing, float& angle) {
    if (fighter.input.justPressed(ButtonAttack) &&
        fxAbs(fighter.input.frames[0].move.x) >= common.dashInputThresholdX3C &&
        fighter.stickXTiltTimer < common.dashStickWindowX40)
    {
        facing = signOf(fighter.input.frames[0].move.x);
        angle = stickAngle(fighter.input.frames[0].move);
        return true;
    }
    if (cStickSideSmashInput(fighter, common)) {
        facing = signOf(fighter.input.frames[0].cStick.x);
        angle = stickAngle(fighter.input.frames[0].cStick);
        return true;
    }
    return false;
}

static int sideSmashActionIndex(const FighterDefinition& def, const FighterRuntime& fighter, const MeleeCommonData& common) {
    int facing = fighter.facing;
    float angle = 0.0f;
    if (!sideSmashInput(fighter, common, facing, angle)) {
        return -1;
    }
    int selected = 62;
    // These availability guards mirror Melee's ftData_80085FD4 checks, which test related action slots rather than the selected slot.
    if (angle > fxToFloat(common.attackS4HiAngleXB8) && hasActionClip(def, 62)) {
        selected = 60;
    } else if (angle > fxToFloat(common.attackS4HiSAngleXBC) && hasActionClip(def, 63)) {
        selected = 61;
    } else if (angle < fxToFloat(common.attackS4LwAngleXC4) && hasActionClip(def, 67)) {
        selected = 64;
    } else if (angle < fxToFloat(common.attackS4LwSAngleXC0) && hasActionClip(def, 66)) {
        selected = 63;
    }
    return hasActionClip(def, selected) ? selected : 62;
}

static bool sideTiltInput(const FighterRuntime& fighter, const MeleeCommonData& common, float& angle) {
    if (!fighter.input.justPressed(ButtonAttack) ||
        fighter.input.frames[0].move.x * fighter.facing < common.attackS3StickThresholdX98)
    {
        return false;
    }
    angle = stickAngle(fighter.input.frames[0].move);
    return std::abs(angle) < fxToFloat(common.aerialAttackAngleTanX20);
}

static int sideTiltActionIndex(const FighterDefinition& def, const FighterRuntime& fighter, const MeleeCommonData& common) {
    float angle = 0.0f;
    if (!sideTiltInput(fighter, common, angle)) {
        return -1;
    }
    int selected = 55;
    // These availability guards mirror Melee's ftData_80085FD4 checks, which test related action slots rather than the selected slot.
    if (angle > fxToFloat(common.attackS3HiAngleX9C) && hasActionClip(def, 55)) {
        selected = 53;
    } else if (angle > fxToFloat(common.attackS3HiSAngleXA0) && hasActionClip(def, 56)) {
        selected = 54;
    } else if (angle < fxToFloat(common.attackS3LwAngleXA8) && hasActionClip(def, 59)) {
        selected = 57;
    } else if (angle < fxToFloat(common.attackS3LwSAngleXA4) && hasActionClip(def, 58)) {
        selected = 56;
    }
    return hasActionClip(def, selected) ? selected : 55;
}

static bool upSmashInput(const FighterRuntime& fighter, const MeleeCommonData& common) {
    return (fighter.input.justPressed(ButtonAttack) &&
            fighter.input.frames[0].move.y >= common.attackHi4StickThresholdYxCC &&
            fighter.stickYTiltTimer < common.attackHi4StickWindowXD0) ||
           (fighter.input.frames[1].cStick.y < common.attackHi4StickThresholdYxCC &&
            fighter.input.frames[0].cStick.y >= common.attackHi4StickThresholdYxCC);
}

static bool downSmashInput(const FighterRuntime& fighter, const MeleeCommonData& common) {
    return (fighter.input.justPressed(ButtonAttack) &&
            fighter.input.frames[0].move.y <= common.attackLw4StickThresholdYxD4 &&
            fighter.stickYTiltTimer < common.attackLw4StickWindowXD8) ||
           (fighter.input.frames[1].cStick.y > common.attackLw4StickThresholdYxD4 &&
            fighter.input.frames[0].cStick.y <= common.attackLw4StickThresholdYxD4);
}

static bool upTiltInput(const FighterRuntime& fighter, const MeleeCommonData& common) {
    return fighter.input.justPressed(ButtonAttack) &&
           fighter.input.frames[0].move.y >= common.attackHi3StickThresholdYxAC &&
           stickAngle(fighter.input.frames[0].move) > fxToFloat(common.aerialAttackAngleTanX20);
}

static bool downTiltInput(const FighterRuntime& fighter, const MeleeCommonData& common) {
    return fighter.input.justPressed(ButtonAttack) &&
           fighter.input.frames[0].move.y <= common.attackLw3StickThresholdYxB0 &&
           stickAngle(fighter.input.frames[0].move) < -fxToFloat(common.aerialAttackAngleTanX20);
}

static bool ledgeStickActive(Vec2 stick, const MeleeCommonData& common) {
    return fxAbs(stick.x) >= common.cliffOptionStickThresholdX494 ||
           fxAbs(stick.y) >= common.cliffOptionStickThresholdX494;
}

static bool ledgeStickChoosesClimb(const FighterRuntime& fighter, const MeleeCommonData& common) {
    const Vec2 stick = fighter.input.frames[0].move;
    const float angle = std::atan2(fxToFloat(stick.y), std::abs(fxToFloat(stick.x)));
    const float cliffAngle = fxToFloat(common.aerialAttackAngleTanX20);
    return angle > cliffAngle ||
           (angle > -cliffAngle && stick.x * fighter.facing >= 0);
}

static bool isSideSmashCondition(InterruptCondition condition) {
    return condition == InterruptCondition::AttackS4HiPressed ||
           condition == InterruptCondition::AttackS4HiSPressed ||
           condition == InterruptCondition::AttackS4Pressed ||
           condition == InterruptCondition::AttackS4LwSPressed ||
           condition == InterruptCondition::AttackS4LwPressed;
}

static bool conditionMet(const World& world, InterruptCondition condition, const FighterRuntime& fighter) {
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const FighterProperties& attr = def.properties;
    const MeleeCommonData& common = attr.common;
    const Fix x = fighter.input.frames[0].move.x;
    const bool aerialAttackInput = !fighter.grounded &&
        (fighter.input.justPressed(ButtonAttack) || cStickAerialAttackInput(fighter, common));
    const bool aerialJumpInput =
        !fighter.grounded && fighter.jumpsUsed < attr.maxJumps &&
        (fighter.input.justPressed(ButtonJump) ||
         (fighter.input.frames[0].move.y >= common.aerialJumpStickThresholdX80 &&
          fighter.stickYTiltTimer < common.tapJumpWindowX74));
    switch (condition) {
        case InterruptCondition::JumpPressed:
            return fighter.input.justPressed(ButtonJump) ||
                   (fighter.input.frames[0].move.y >= common.tapJumpThresholdX70 &&
                    fighter.stickYTiltTimer < common.tapJumpWindowX74);
        case InterruptCondition::RunJumpPressed:
            return fighter.input.justPressed(ButtonJump) ||
                   (fighter.input.frames[0].move.y >= common.aerialJumpStickThresholdX80 &&
                    fighter.stickYTiltTimer < common.tapJumpWindowX74);
        case InterruptCondition::AerialJumpForwardPressed:
            return aerialJumpInput && x * fighter.facing > -common.jumpBackwardThresholdX78;
        case InterruptCondition::AerialJumpBackwardPressed:
            return aerialJumpInput && x * fighter.facing <= -common.jumpBackwardThresholdX78;
        case InterruptCondition::AirDodgePressed:
            return !fighter.grounded && fighter.input.justPressed(ButtonShield);
        case InterruptCondition::WallJumpInput:
            if (!attr.canWallJump || fighter.grounded || fighter.wallContactSide == 0 ||
                fighter.wallContactTimer >= common.wallJumpInputWindowX768 ||
                fighter.stickXTiltTimer >= common.wallJumpStickWindowX770)
            {
                return false;
            }
            return (fighter.wallContactSide < 0 && x >= common.wallJumpStickThresholdX76C) ||
                   (fighter.wallContactSide > 0 && x <= -common.wallJumpStickThresholdX76C);
        case InterruptCondition::SquatInput:
            return fighter.grounded && fighter.input.frames[0].move.y < -common.squatStickThresholdX90;
        case InterruptCondition::SquatReleaseInput:
            return fighter.grounded && fighter.input.frames[0].move.y > -common.squatReleaseThresholdX94;
        case InterruptCondition::AttackPressed:
            return fighter.input.justPressed(ButtonAttack);
        case InterruptCondition::JabFollowupPressed:
            return fighter.input.justPressed(ButtonAttack) && fighterCommandFlag(fighter, 2);
        case InterruptCondition::AttackDashPressed:
            return fighter.input.justPressed(ButtonAttack);
        case InterruptCondition::AttackS4HiPressed:
            return sideSmashActionIndex(def, fighter, common) == 60;
        case InterruptCondition::AttackS4HiSPressed:
            return sideSmashActionIndex(def, fighter, common) == 61;
        case InterruptCondition::AttackS4Pressed:
            return sideSmashActionIndex(def, fighter, common) == 62;
        case InterruptCondition::AttackS4LwSPressed:
            return sideSmashActionIndex(def, fighter, common) == 63;
        case InterruptCondition::AttackS4LwPressed:
            return sideSmashActionIndex(def, fighter, common) == 64;
        case InterruptCondition::AttackHi4Pressed:
            return upSmashInput(fighter, common);
        case InterruptCondition::AttackLw4Pressed:
            return downSmashInput(fighter, common);
        case InterruptCondition::AttackS3HiPressed:
            return sideTiltActionIndex(def, fighter, common) == 53;
        case InterruptCondition::AttackS3HiSPressed:
            return sideTiltActionIndex(def, fighter, common) == 54;
        case InterruptCondition::AttackS3Pressed:
            return sideTiltActionIndex(def, fighter, common) == 55;
        case InterruptCondition::AttackS3LwSPressed:
            return sideTiltActionIndex(def, fighter, common) == 56;
        case InterruptCondition::AttackS3LwPressed:
            return sideTiltActionIndex(def, fighter, common) == 57;
        case InterruptCondition::AttackHi3Pressed:
            return upTiltInput(fighter, common);
        case InterruptCondition::AttackLw3Pressed:
            return downTiltInput(fighter, common);
        case InterruptCondition::AerialAttackNPressed:
            return aerialAttackInput &&
                   aerialAttackDirection(fighter, common) == AerialAttackDirection::Neutral;
        case InterruptCondition::AerialAttackFPressed:
            return aerialAttackInput &&
                   aerialAttackDirection(fighter, common) == AerialAttackDirection::Forward;
        case InterruptCondition::AerialAttackBPressed:
            return aerialAttackInput &&
                   aerialAttackDirection(fighter, common) == AerialAttackDirection::Back;
        case InterruptCondition::AerialAttackHiPressed:
            return aerialAttackInput &&
                   aerialAttackDirection(fighter, common) == AerialAttackDirection::High;
        case InterruptCondition::AerialAttackLwPressed:
            return aerialAttackInput &&
                   aerialAttackDirection(fighter, common) == AerialAttackDirection::Low;
        case InterruptCondition::DashInput:
            return x * fighter.facing >= common.dashInputThresholdX3C &&
                   fighter.stickXTiltTimer < common.dashStickWindowX40;
        case InterruptCondition::ReverseDashInput:
            return x * fighter.facing <= -common.dashInputThresholdX3C &&
                   fighter.stickXTiltTimer < common.dashStickWindowX40;
        case InterruptCondition::RunInput:
            return x * fighter.facing >= common.runInputThresholdX58 && fighterCommandFlag(fighter, 0);
        case InterruptCondition::TeeterWalkInput:
            return x * fighter.facing >= common.teeterWalkInputThresholdX474 &&
                   x * fighter.facing >= common.walkInputThresholdX24;
        case InterruptCondition::HorizontalWalkSlow:
            return x * fighter.facing >= common.walkInputThresholdX24 &&
                   fxAbs(fighter.groundVelocity) < fxMul(common.walkMiddleThresholdX28, attr.walkMaxVel);
        case InterruptCondition::HorizontalWalkMiddle:
            return x * fighter.facing >= common.walkInputThresholdX24 &&
                   fxAbs(fighter.groundVelocity) >= fxMul(common.walkMiddleThresholdX28, attr.walkMaxVel) &&
                   fxAbs(fighter.groundVelocity) < fxMul(common.walkFastThresholdX2C, attr.walkMaxVel);
        case InterruptCondition::HorizontalWalkFast:
            return x * fighter.facing >= common.walkInputThresholdX24 &&
                   fxAbs(fighter.groundVelocity) >= fxMul(common.walkFastThresholdX2C, attr.walkMaxVel);
        case InterruptCondition::TurnInput:
            return x * fighter.facing <= common.turnInputThresholdX34;
        case InterruptCondition::TurnRunInput:
            return x * fighter.facing <= common.turnRunInputThresholdX38;
        case InterruptCondition::RunBrakeInput:
            return fxAbs(x) < common.runInputThresholdX58;
        case InterruptCondition::WaitInput:
            return x * fighter.facing < common.walkInputThresholdX24;
        case InterruptCondition::BecameAirborne:
            return !fighter.grounded;
        case InterruptCondition::ShieldReflectInput:
            return fighter.grounded && fighter.shieldHealth > 0 &&
                   fighter.input.justPressed(ButtonShield) &&
                   fighter.input.frames[0].shieldAnalog >= 0;
        case InterruptCondition::ShieldPressed:
            return fighter.input.justPressed(ButtonShield);
        case InterruptCondition::ShieldHeld:
            return fighter.grounded && fighter.shieldHealth > 0 && fighter.input.down(ButtonShield);
        case InterruptCondition::ShieldJumpPressed:
            return fighter.grounded &&
                   (fighter.input.justPressed(ButtonJump) ||
                    (fighter.input.frames[0].move.y >= common.tapJumpThresholdX70 &&
                     fighter.stickYTiltTimer < common.tapJumpWindowX74) ||
                    fighter.input.frames[0].cStick.y >= common.tapJumpThresholdX70);
        case InterruptCondition::SpotDodgeInput:
            return fighter.grounded && fighter.input.down(ButtonShield) &&
                   ((fighter.input.frames[0].move.y <= common.spotDodgeStickThresholdX314 &&
                     fighter.stickYTiltTimer < common.spotDodgeStickWindowX318) ||
                    fighter.input.frames[0].cStick.y <= common.spotDodgeStickThresholdX314);
        case InterruptCondition::RollForwardInput:
            return fighter.grounded && fighter.input.down(ButtonShield) &&
                   ((fxAbs(x) >= common.rollStickThresholdX31C &&
                     fighter.stickXTiltTimer < common.rollStickWindowX320 &&
                     x * fighter.facing >= 0) ||
                    (fxAbs(fighter.input.frames[0].cStick.x) >= common.rollStickThresholdX31C &&
                     fighter.input.frames[0].cStick.x * fighter.facing >= 0));
        case InterruptCondition::RollBackwardInput:
            return fighter.grounded && fighter.input.down(ButtonShield) &&
                   ((fxAbs(x) >= common.rollStickThresholdX31C &&
                     fighter.stickXTiltTimer < common.rollStickWindowX320 &&
                     x * fighter.facing < 0) ||
                    (fxAbs(fighter.input.frames[0].cStick.x) >= common.rollStickThresholdX31C &&
                     fighter.input.frames[0].cStick.x * fighter.facing < 0));
        case InterruptCondition::LedgeClimbInput:
            return fighter.grabbedLedge >= 0 &&
                   fighter.ledgeActionReady &&
                   ledgeStickActive(fighter.input.frames[0].move, common) &&
                   ledgeStickChoosesClimb(fighter, common);
        case InterruptCondition::LedgeDropInput:
            return fighter.grabbedLedge >= 0 &&
                   fighter.ledgeActionReady &&
                   ledgeStickActive(fighter.input.frames[0].move, common) &&
                   !ledgeStickChoosesClimb(fighter, common);
    }
    return false;
}

static int fallbackActionIndex(const std::string& animation) {
    if (animation == "Wait") return 2;
    if (animation == "WalkSlow") return 7;
    if (animation == "WalkMiddle") return 8;
    if (animation == "WalkFast") return 9;
    if (animation == "Turn") return 10;
    if (animation == "TurnRun") return 11;
    if (animation == "Dash") return 12;
    if (animation == "Run") return 13;
    if (animation == "RunBrake") return 14;
    if (animation == "JumpSquat" || animation == "KneeBend") return 15;
    if (animation == "JumpF") return 16;
    if (animation == "JumpB") return 17;
    if (animation == "JumpAerialF") return 18;
    if (animation == "JumpAerialB") return 19;
    if (animation == "Fall") return 20;
    if (animation == "FallSpecial") return 26;
    if (animation == "Landing") return 35;
    if (animation == "LandingFallSpecial") return 36;
    if (animation == "GuardOn" || animation == "GuardReflect") return 37;
    if (animation == "Guard") return 38;
    if (animation == "GuardOff") return 39;
    if (animation == "GuardSetOff") return 40;
    if (animation == "EscapeN") return 41;
    if (animation == "EscapeF") return 42;
    if (animation == "EscapeB") return 43;
    if (animation == "EscapeAir") return 44;
    if (animation == "Jab" || animation == "Attack11") return 46;
    if (animation == "Attack12") return 47;
    if (animation == "Attack13") return 48;
    if (animation == "AttackDash") return 52;
    if (animation == "AttackS3Hi") return 53;
    if (animation == "AttackS3HiS") return 54;
    if (animation == "AttackS3") return 55;
    if (animation == "AttackS3LwS") return 56;
    if (animation == "AttackS3Lw") return 57;
    if (animation == "AttackHi3") return 58;
    if (animation == "AttackLw3") return 59;
    if (animation == "AttackS4Hi") return 60;
    if (animation == "AttackS4HiS") return 61;
    if (animation == "AttackS4") return 62;
    if (animation == "AttackS4LwS") return 63;
    if (animation == "AttackS4Lw") return 64;
    if (animation == "AttackHi4") return 66;
    if (animation == "AttackLw4") return 67;
    if (animation == "AirAttackN") return 68;
    if (animation == "AirAttackF") return 69;
    if (animation == "AirAttackB") return 70;
    if (animation == "AirAttackHi") return 71;
    if (animation == "AirAttackLw") return 72;
    if (animation == "LandingAirN") return 73;
    if (animation == "LandingAirF") return 74;
    if (animation == "LandingAirB") return 75;
    if (animation == "LandingAirHi") return 76;
    if (animation == "LandingAirLw") return 77;
    if (animation == "Squat") return 30;
    if (animation == "SquatWait") return 31;
    if (animation == "SquatRv") return 34;
    if (animation == "Pass") return 209;
    if (animation == "Ottotto") return 210;
    if (animation == "OttottoWait") return 211;
    if (animation == "PassiveWallJump") return 203;
    if (animation == "CliffCatch") return 216;
    if (animation == "CliffWait") return 217;
    if (animation == "CliffClimbQuick") return 220;
    if (animation == "CliffAttackQuick") return 222;
    if (animation == "CliffEscapeQuick") return 224;
    if (animation == "CliffJumpQuick1") return 227;
    if (animation == "CliffJumpQuick2") return 228;
    return -1;
}

static const AnimationClip* clipForState(const FighterDefinition& def, const FighterState& state) {
    if (!def.hasHsdAsset || !def.hsdAsset) {
        return nullptr;
    }
    const int actionIndex = state.animationActionIndex >= 0 ? state.animationActionIndex : fallbackActionIndex(state.animation);
    if (actionIndex >= 0) {
        if (const AnimationClip* clip = findClipByActionIndex(*def.hsdAsset, actionIndex)) {
            return clip;
        }
    }
    const std::string suffix = "_ACTION_" + state.animation + "_figatree";
    for (const AnimationClip& clip : def.hsdAsset->clips) {
        if (clip.name.size() >= suffix.size() &&
            clip.name.compare(clip.name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return &clip;
        }
    }
    return nullptr;
}

static Fix blendAngle(Fix from, Fix to, Fix t) {
    constexpr float pi = 3.14159265359f;
    constexpr float twoPi = 6.28318530718f;
    float delta = fxToFloat(to - from);
    while (delta > pi) {
        delta -= twoPi;
    }
    while (delta < -pi) {
        delta += twoPi;
    }
    return from + fxMul(fxFromFloat(delta), t);
}

static AnimationPose blendedPose(const AnimationPose& from, const AnimationPose& to, Fix t) {
    AnimationPose result = to;
    const size_t count = std::min(from.joints.size(), to.joints.size());
    for (size_t i = 0; i < count; ++i) {
        JointPose& out = result.joints[i];
        const JointPose& a = from.joints[i];
        const JointPose& b = to.joints[i];
        out.translation.x = a.translation.x + fxMul(b.translation.x - a.translation.x, t);
        out.translation.y = a.translation.y + fxMul(b.translation.y - a.translation.y, t);
        out.translation.z = a.translation.z + fxMul(b.translation.z - a.translation.z, t);
        if (!a.useQuaternion && !b.useQuaternion &&
            fxAbs(a.rotation.x - b.rotation.x) <= fxFromFloat(0.0001f) &&
            fxAbs(a.rotation.y - b.rotation.y) <= fxFromFloat(0.0001f) &&
            fxAbs(a.rotation.z - b.rotation.z) <= fxFromFloat(0.0001f))
        {
            out.rotation = b.rotation;
            out.quaternion = {};
            out.useQuaternion = false;
        } else {
            const Quaternion fromQuat = a.useQuaternion ? a.quaternion : eulerToQuaternion(a.rotation);
            const Quaternion toQuat = b.useQuaternion ? b.quaternion : eulerToQuaternion(b.rotation);
            out.quaternion = slerpQuaternion(fromQuat, toQuat, t);
            out.useQuaternion = true;
        }
        out.scale.x = a.scale.x + fxMul(b.scale.x - a.scale.x, t);
        out.scale.y = a.scale.y + fxMul(b.scale.y - a.scale.y, t);
        out.scale.z = a.scale.z + fxMul(b.scale.z - a.scale.z, t);
    }
    return result;
}

static bool usesShieldPose(const FighterState& state) {
    return state.name == "GuardOn" || state.name == "Guard" || state.name == "GuardReflect";
}

static void extractTransNForModelPose(AnimationPose& pose);

static void applyShieldPose(const FighterDefinition& def, const FighterState& state, FighterRuntime& fighter) {
    if (!usesShieldPose(state) || !def.hsdAsset || !def.hsdAsset->hasShieldPose ||
        def.hsdAsset->shieldPose.joints.size() != fighter.hsdPose.joints.size())
    {
        return;
    }

    AnimationPose target = def.hsdAsset->shieldPose;
    if (const AnimationClip* guardClip = findClipByActionIndex(*def.hsdAsset, 38)) {
        const Fix stickBlend = std::clamp(fighter.guardPoseBlend, Fix{0}, fx(1));
        if (stickBlend > 0) {
            AnimationPose stickPose = evaluateClip(def.hsdAsset->skeleton, *guardClip, fighter.guardPoseFrame);
            extractTransNForModelPose(stickPose);
            target = blendedPose(target, stickPose, stickBlend);
        }
    }

    Fix openingBlend = fx(1);
    if (state.name == "GuardOn" || state.name == "GuardReflect") {
        const int guardOpenFrames = std::max(1, def.properties.common.guardMinHoldFramesX268);
        openingBlend = std::min(fx(1), fxDiv(fx(frameInState(fighter) + 1), fx(guardOpenFrames)));
    }
    fighter.hsdPose = blendedPose(fighter.hsdPose, target, openingBlend);
}

static void applyAnimationChannel(JointPose& pose, AnimationChannel channel, Fix value) {
    switch (channel) {
    case AnimationChannel::TranslateX: pose.translation.x = value; break;
    case AnimationChannel::TranslateY: pose.translation.y = value; break;
    case AnimationChannel::TranslateZ: pose.translation.z = value; break;
    case AnimationChannel::RotateX: pose.rotation.x = value; pose.useQuaternion = false; break;
    case AnimationChannel::RotateY: pose.rotation.y = value; pose.useQuaternion = false; break;
    case AnimationChannel::RotateZ: pose.rotation.z = value; pose.useQuaternion = false; break;
    case AnimationChannel::ScaleX: pose.scale.x = value; break;
    case AnimationChannel::ScaleY: pose.scale.y = value; break;
    case AnimationChannel::ScaleZ: pose.scale.z = value; break;
    }
}

static void applyModelPartAnimations(const FighterDefinition& def, FighterRuntime& fighter) {
    if (!def.hsdAsset || fighter.hsdModelPartAnimations.empty()) {
        return;
    }
    const std::vector<HsdModelPartAnimationSet>& sets = def.hsdAsset->modelPartAnimations;
    for (size_t partIndex = 0; partIndex < sets.size() && partIndex < fighter.hsdModelPartAnimations.size(); ++partIndex) {
        const int animIndex = fighter.hsdModelPartAnimations[partIndex];
        if (animIndex < 0 || static_cast<size_t>(animIndex) >= sets[partIndex].animations.size()) {
            continue;
        }
        const AnimationClip& clip = sets[partIndex].animations[static_cast<size_t>(animIndex)];
        for (const AnimationTrack& track : clip.tracks) {
            const int joint = track.joint;
            if (joint < 0 || static_cast<size_t>(joint) >= fighter.hsdPose.joints.size()) {
                continue;
            }
            applyAnimationChannel(
                fighter.hsdPose.joints[static_cast<size_t>(joint)],
                track.channel,
                sampleTrack(track, 0));
        }
    }
}

static bool clipAnimatesTopNYRotation(const AnimationClip& clip) {
    return std::any_of(clip.tracks.begin(), clip.tracks.end(), [](const AnimationTrack& track) {
        return track.joint == 0 && track.channel == AnimationChannel::RotateY && !track.keys.empty();
    });
}

static void extractTransNForModelPose(AnimationPose& pose) {
    if (pose.joints.size() > 1) {
        pose.joints[1].translation = {};
    }
}

static void evaluateImportedHurtboxes(const FighterDefinition& def, FighterRuntime& fighter) {
    fighter.hsdHurtboxCapsules.clear();
    if (!def.hsdAsset || fighter.hsdJointWorldTransforms.empty()) {
        return;
    }
    fighter.hsdHurtboxCapsules.reserve(def.hsdAsset->hurtboxes.size());
    for (const HsdHurtbox& hurtbox : def.hsdAsset->hurtboxes) {
        if (hurtbox.bone < 0 || static_cast<size_t>(hurtbox.bone) >= fighter.hsdJointWorldTransforms.size()) {
            continue;
        }
        const JointWorldTransform& transform = fighter.hsdJointWorldTransforms[static_cast<size_t>(hurtbox.bone)];
        Capsule capsule;
        capsule.a = transformPoint(transform, hurtbox.start);
        capsule.b = transformPoint(transform, hurtbox.end);
        capsule.radius = hurtbox.radius;
        fighter.hsdHurtboxCapsules.push_back(capsule);
    }
}

static bool hsdRelativeBonePosition(const FighterRuntime& fighter, int joint, Vec3& out) {
    if (joint < 0 || static_cast<size_t>(joint) >= fighter.hsdJointWorldPositions.size()) {
        return false;
    }
    const Vec3 world = fighter.hsdJointWorldPositions[static_cast<size_t>(joint)];
    out = {world.x - fighter.position.x, world.y - fighter.position.y, world.z};
    return true;
}

static void applyImportedBoneAliases(const FighterDefinition& def, FighterRuntime& fighter) {
    if (!def.hasHsdAsset || !def.hsdAsset || fighter.hsdJointWorldPositions.empty()) {
        return;
    }

    // These aliases come from Melee's fighter/model lookup tables exported
    // from Pl*.dat. The decomp resolves common parts through ftParts_GetBoneIndex
    // before touching fp->parts[].joint; keep this as data-table plumbing, not
    // a hand-authored anatomy guess.
    const HsdFighterBoneTable& bones = def.hsdAsset->fighterBones;
    Vec3 position{};
    if (hsdRelativeBonePosition(fighter, 0, position)) {
        fighter.bones[static_cast<size_t>(BoneId::Hip)].position = position;
    }
    if (hsdRelativeBonePosition(fighter, bones.head, position)) {
        fighter.bones[static_cast<size_t>(BoneId::Head)].position = position;
    } else if (hsdRelativeBonePosition(fighter, bones.topOfHead, position)) {
        fighter.bones[static_cast<size_t>(BoneId::Head)].position = position;
    }
    if (hsdRelativeBonePosition(fighter, bones.leftArm, position)) {
        fighter.bones[static_cast<size_t>(BoneId::HandL)].position = position;
    }
    if (hsdRelativeBonePosition(fighter, bones.itemHold, position) ||
        hsdRelativeBonePosition(fighter, bones.rightArm, position))
    {
        fighter.bones[static_cast<size_t>(BoneId::HandR)].position = position;
    }
    if (hsdRelativeBonePosition(fighter, bones.leftFoot, position) ||
        hsdRelativeBonePosition(fighter, bones.leftLeg, position))
    {
        fighter.bones[static_cast<size_t>(BoneId::FootL)].position = position;
    }
    if (hsdRelativeBonePosition(fighter, bones.rightFoot, position) ||
        hsdRelativeBonePosition(fighter, bones.rightLeg, position))
    {
        fighter.bones[static_cast<size_t>(BoneId::FootR)].position = position;
    }
    if (hsdRelativeBonePosition(fighter, bones.shield, position)) {
        fighter.bones[static_cast<size_t>(BoneId::Extra)].position = position;
    }
}

static void evaluatePose(const FighterDefinition& def, const FighterState& state, FighterRuntime& fighter) {
    const AnimationClip* clip = clipForState(def, state);
    if (clip) {
        constexpr float kHalfPi = 1.57079632679f;
        const AnimationPose previousVisiblePose = fighter.hsdPose;
        Fix frame = fighter.animationFrame;
        if (state.loopAnimation && clip->frameCount > 0) {
            const int loopFrames = std::max(1, static_cast<int>(std::round(fxToFloat(clip->frameCount))));
            frame %= fx(loopFrames);
        } else if (fighter.stateAnimationLengthOverride > 0 && clip->frameCount > 0) {
            frame = fxMul(clip->frameCount, fxDiv(fx(frameInState(fighter)), fx(fighter.stateAnimationLengthOverride)));
        }
        fighter.hsdPose = evaluateClip(def.hsdAsset->skeleton, *clip, frame);
        fighter.previousHsdTransN = fighter.hsdTransN;
        fighter.hsdTransN = fighter.hsdPose.joints.size() > 1 ?
            scaledVec3(fighter.hsdPose.joints[1].translation, def.properties.modelScale) :
            Vec3{};
        if (frameInState(fighter) <= 1) {
            fighter.hsdTransNOffset = {};
        } else {
            fighter.hsdTransNOffset = {
                fighter.hsdTransN.x - fighter.previousHsdTransN.x,
                fighter.hsdTransN.y - fighter.previousHsdTransN.y,
                fighter.hsdTransN.z - fighter.previousHsdTransN.z,
            };
        }
        extractTransNForModelPose(fighter.hsdPose);
        applyShieldPose(def, state, fighter);
        applyModelPartAnimations(def, fighter);
        if (!fighter.hsdPose.joints.empty() && !clipAnimatesTopNYRotation(*clip)) {
            const float facing = fighter.hsdPoseFacing >= 0 ? 1.0f : -1.0f;
            fighter.hsdPose.joints[0].rotation.y = fxFromFloat(kHalfPi * facing);
            fighter.hsdPose.joints[0].useQuaternion = false;
        }
        if (fighter.hsdBlendFrames > 0 &&
            previousVisiblePose.joints.size() == fighter.hsdPose.joints.size())
        {
            const Fix rate = fighter.animationRate > 0 ? fighter.animationRate : 0;
            fighter.hsdBlendElapsed += rate;
            if (fighter.hsdBlendElapsed >= fx(fighter.hsdBlendFrames)) {
                fighter.hsdBlendFrames = 0;
                fighter.hsdBlendElapsed = 0;
            } else if (rate > 0) {
                const Fix remaining = fx(fighter.hsdBlendFrames) - fighter.hsdBlendElapsed;
                const Fix t = fxDiv(rate, rate + remaining);
                fighter.hsdPose = blendedPose(previousVisiblePose, fighter.hsdPose, t);
            } else {
                fighter.hsdPose = previousVisiblePose;
            }
        }
        fighter.hsdJointWorldTransforms = fighterHsdWorldTransforms(def, fighter);
        fighter.hsdJointWorldPositions = translationsFromTransforms(fighter.hsdJointWorldTransforms);
        evaluateImportedHurtboxes(def, fighter);
    } else if (fighter.hsdPose.joints.empty()) {
        fighter.hsdPose = {};
        fighter.previousHsdTransN = fighter.hsdTransN;
        fighter.hsdTransN = {};
        fighter.hsdTransNOffset = {};
        fighter.hsdJointWorldTransforms.clear();
        fighter.hsdJointWorldPositions.clear();
        fighter.hsdHurtboxCapsules.clear();
    } else {
        fighter.hsdJointWorldTransforms = fighterHsdWorldTransforms(def, fighter);
        fighter.hsdJointWorldPositions = translationsFromTransforms(fighter.hsdJointWorldTransforms);
        evaluateImportedHurtboxes(def, fighter);
    }

    const int frame = frameInState(fighter);
    const Fix bob = fxFromFloat(std::sin(static_cast<float>(frame) * 0.22f) * 0.08f);
    const Fix facing = fighter.facing >= 0 ? fx(1) : -fx(1);

    fighter.bones[static_cast<size_t>(BoneId::Hip)].position = {0, fxFromFloat(1.25f) + bob, 0};
    fighter.bones[static_cast<size_t>(BoneId::Head)].position = {0, fxFromFloat(2.45f) + bob, 0};
    fighter.bones[static_cast<size_t>(BoneId::FootL)].position = {fxFromFloat(-0.35f), fxFromFloat(0.15f), 0};
    fighter.bones[static_cast<size_t>(BoneId::FootR)].position = {fxFromFloat(0.35f), fxFromFloat(0.15f), 0};
    fighter.bones[static_cast<size_t>(BoneId::Extra)].position = {0, fxFromFloat(1.7f), 0};

    Fix reach = fxFromFloat(0.65f);
    if (state.name == "Attack11" || state.name == "AirAttackN") {
        reach = fxFromFloat(frame >= 3 && frame <= 8 ? 1.35f : 0.75f);
    }
    fighter.bones[static_cast<size_t>(BoneId::HandR)].position = {fxMul(reach, facing), fxFromFloat(1.75f), 0};
    fighter.bones[static_cast<size_t>(BoneId::HandL)].position = {fxMul(-fxFromFloat(0.65f), facing), fxFromFloat(1.75f), 0};
    applyImportedBoneAliases(def, fighter);
}

static void applyAnimationGroundVelocity(const FighterState& state, FighterRuntime& fighter) {
    if (!state.useAnimPhysics || !fighter.grounded || frameInState(fighter) <= 1) {
        return;
    }
    const Fix target = fighter.facing * fighter.hsdTransNOffset.z;
    fighter.groundAccel = target - fighter.groundVelocity;
    fighter.groundAccelSecondary = 0;
}

static void advanceAnimationFrame(const FighterDefinition& def, const FighterState& state, FighterRuntime& fighter) {
    fighter.animationFrame += fighter.animationRate;
    const AnimationClip* clip = clipForState(def, state);
    if (!state.loopAnimation || !clip || clip->frameCount <= 0) {
        return;
    }
    while (fighter.animationFrame >= clip->frameCount) {
        fighter.animationFrame -= clip->frameCount;
    }
    while (fighter.animationFrame < 0) {
        fighter.animationFrame += clip->frameCount;
    }
}

static bool segmentYAtX(const StageSegment& segment, Fix x, Fix& y) {
    const Fix minX = std::min(segment.start.x, segment.end.x);
    const Fix maxX = std::max(segment.start.x, segment.end.x);
    if (x < minX || x > maxX) {
        return false;
    }
    const Fix dx = segment.end.x - segment.start.x;
    if (dx == 0) {
        return false;
    }
    const Fix t = fxDiv(x - segment.start.x, dx);
    y = segment.start.y + fxMul(segment.end.y - segment.start.y, t);
    return true;
}

static bool segmentXAtY(const StageSegment& segment, Fix y, Fix& x) {
    const Fix minY = std::min(segment.start.y, segment.end.y);
    const Fix maxY = std::max(segment.start.y, segment.end.y);
    if (y < minY || y > maxY) {
        return false;
    }
    const Fix dy = segment.end.y - segment.start.y;
    if (dy == 0) {
        return false;
    }
    const Fix t = fxDiv(y - segment.start.y, dy);
    x = segment.start.x + fxMul(segment.end.x - segment.start.x, t);
    return true;
}

static Vec2 segmentNormal(const StageSegment& segment) {
    const float dx = fxToFloat(segment.end.x - segment.start.x);
    const float dy = fxToFloat(segment.end.y - segment.start.y);
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0001f) {
        return {0, fx(1)};
    }
    float nx = -dy / len;
    float ny = dx / len;
    if (ny < 0.0f) {
        nx = -nx;
        ny = -ny;
    }
    return {fxFromFloat(nx), fxFromFloat(ny)};
}

static Vec2 groundTangent(Vec2 normal) {
    return {normal.y, -normal.x};
}

static Fix velocityAlongGround(Vec2 velocity, Vec2 normal);

static void projectGroundVelocity(FighterRuntime& fighter) {
    const Vec2 tangent = groundTangent(fighter.groundNormal);
    fighter.fighterVelocity.x = fxMul(tangent.x, fighter.groundVelocity);
    fighter.fighterVelocity.y = fxMul(tangent.y, fighter.groundVelocity);
}

static void projectGroundAttackerShieldKnockback(FighterRuntime& fighter) {
    const Vec2 tangent = groundTangent(fighter.groundNormal);
    fighter.attackerShieldKnockback.x = fxMul(tangent.x, fighter.groundAttackerShieldKnockbackVelocity);
    fighter.attackerShieldKnockback.y = fxMul(tangent.y, fighter.groundAttackerShieldKnockbackVelocity);
}

static Fix groundFrictionMultiplier(const World& world, const FighterRuntime& fighter) {
    if (fighter.groundSegment < 0 || fighter.groundSegment >= static_cast<int>(world.stage.segments.size())) {
        return fx(1);
    }
    return world.stage.segments[static_cast<size_t>(fighter.groundSegment)].friction;
}

static void approachGroundAttackerShieldKnockback(const World& world, FighterRuntime& fighter) {
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    Fix friction = fxMul(def.properties.grFriction, def.properties.common.shieldGroundFrictionMultiplierX3EC);
    friction = fxMul(friction, groundFrictionMultiplier(world, fighter));
    fighter.groundAttackerShieldKnockbackVelocity = fxApproach(fighter.groundAttackerShieldKnockbackVelocity, 0, friction);
}

static void updateAttackerShieldKnockback(const World& world, FighterRuntime& fighter) {
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    if (fighter.attackerShieldKnockback.x == 0 && fighter.attackerShieldKnockback.y == 0) {
        fighter.groundAttackerShieldKnockbackVelocity = 0;
        return;
    }

    if (fighter.grounded) {
        if (fighter.groundAttackerShieldKnockbackVelocity == 0) {
            fighter.groundAttackerShieldKnockbackVelocity = velocityAlongGround(fighter.attackerShieldKnockback, fighter.groundNormal);
        }
        approachGroundAttackerShieldKnockback(world, fighter);
        projectGroundAttackerShieldKnockback(fighter);
        return;
    }

    fighter.groundAttackerShieldKnockbackVelocity = 0;
    const float x = fxToFloat(fighter.attackerShieldKnockback.x);
    const float y = fxToFloat(fighter.attackerShieldKnockback.y);
    const float len = std::sqrt(x * x + y * y);
    const float decay = fxToFloat(def.properties.common.shieldKnockbackFrameDecayX3E8);
    if (len <= decay || len <= 0.0001f) {
        fighter.attackerShieldKnockback = {};
        return;
    }
    const float scale = (len - decay) / len;
    fighter.attackerShieldKnockback.x = fxFromFloat(x * scale);
    fighter.attackerShieldKnockback.y = fxFromFloat(y * scale);
}

static void consumeGroundAcceleration(const World& world, FighterRuntime& fighter) {
    const Fix frictionMultiplier = groundFrictionMultiplier(world, fighter);
    if (frictionMultiplier < fx(1)) {
        fighter.groundAccel = fxMul(fighter.groundAccel, frictionMultiplier);
        fighter.groundAccelSecondary = fxMul(fighter.groundAccelSecondary, frictionMultiplier);
    }
    fighter.groundVelocity += fighter.groundAccel + fighter.groundAccelSecondary;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    projectGroundVelocity(fighter);
}

static Fix velocityAlongGround(Vec2 velocity, Vec2 normal) {
    const Vec2 tangent = groundTangent(normal);
    return fxMul(velocity.x, tangent.x) + fxMul(velocity.y, tangent.y);
}

static Fix clamp01(Fix value) {
    return std::clamp(value, Fix{0}, fx(1));
}

static Fix segmentMinX(const StageSegment& segment) {
    return std::min(segment.start.x, segment.end.x);
}

static Fix segmentMaxX(const StageSegment& segment) {
    return std::max(segment.start.x, segment.end.x);
}

static Fix segmentMinY(const StageSegment& segment) {
    return std::min(segment.start.y, segment.end.y);
}

static Fix segmentMaxY(const StageSegment& segment) {
    return std::max(segment.start.y, segment.end.y);
}

static SegmentLineKind classifyDynamicLineKind(const StageSegment& segment) {
    const Fix dx = segment.end.x - segment.start.x;
    const Fix dy = segment.end.y - segment.start.y;
    constexpr Fix tan30 = 577;
    constexpr Fix tan60 = 1732;

    if (dx > 0) {
        const Fix slope = fxDiv(dy, dx);
        if (slope > tan60) {
            return SegmentLineKind::LeftWall;
        }
        if (slope < -tan60) {
            return SegmentLineKind::RightWall;
        }
        return SegmentLineKind::Floor;
    }

    if (dx < 0) {
        const Fix slope = fxDiv(dy, dx);
        if (slope > tan30) {
            return SegmentLineKind::RightWall;
        }
        if (slope < -tan30) {
            return SegmentLineKind::LeftWall;
        }
        return SegmentLineKind::Ceiling;
    }

    if (dy > 0) {
        return SegmentLineKind::LeftWall;
    }
    if (dy < 0) {
        return SegmentLineKind::RightWall;
    }
    return SegmentLineKind::Floor;
}

static SegmentLineKind effectiveLineKind(const StageSegment& segment) {
    return segment.dynamicLineKind ? classifyDynamicLineKind(segment) : segment.lineKind;
}

static bool isFloorLine(const StageSegment& segment) {
    return effectiveLineKind(segment) == SegmentLineKind::Floor;
}

static bool isCeilingLine(const StageSegment& segment) {
    return effectiveLineKind(segment) == SegmentLineKind::Ceiling;
}

static bool isWallLine(const StageSegment& segment) {
    const SegmentLineKind kind = effectiveLineKind(segment);
    return kind == SegmentLineKind::LeftWall || kind == SegmentLineKind::RightWall;
}

static void updateFloorSkip(const World& world, FighterRuntime& fighter) {
    if (fighter.floorSkipSegment < 0 || fighter.floorSkipSegment >= static_cast<int>(world.stage.segments.size())) {
        fighter.floorSkipSegment = -1;
        return;
    }

    const StageSegment& segment = world.stage.segments[static_cast<size_t>(fighter.floorSkipSegment)];
    if (segment.type != SegmentType::Semisolid || !isFloorLine(segment)) {
        fighter.floorSkipSegment = -1;
        return;
    }

    const Vec2 bottom = fighter.position + fighter.ecb.points[3];
    Fix y = 0;
    if (!segmentYAtX(segment, bottom.x, y) || bottom.y < y - fxFromFloat(0.2f)) {
        fighter.floorSkipSegment = -1;
    }
}

static bool canStandOnSegment(const FighterRuntime& fighter, const FighterProperties& attr, const StageSegment& segment, int segmentIndex) {
    (void) attr;
    if (!isFloorLine(segment)) {
        return false;
    }
    if (fighter.floorSkipSegment == segmentIndex) {
        return false;
    }
    return true;
}

struct MeleeLineHit {
    bool hit = false;
    int segment = -1;
    Fix distanceSquared = std::numeric_limits<Fix>::max();
    Vec2 contact = {};
};

static MeleeLineHit meleeCheckFloor(const World& world, const FighterRuntime& fighter, Vec2 sweepStart, Vec2 sweepEnd, int lineIdSkip = -1);
static MeleeLineHit meleeCheckCeiling(const World& world, Vec2 sweepStart, Vec2 sweepEnd);
static bool canMaintainGroundOnSegment(const FighterRuntime& fighter, const StageSegment& segment, int segmentIndex);

struct MeleeSurfaceProjection {
    bool hit = false;
    int segment = -1;
    Vec2 point = {};
    Vec2 normal = {};
    Fix push = 0;
};

static constexpr Fix kMeleeSurfaceTolerance = 100;
static constexpr Fix kMeleeFloorBias = 1;
static constexpr Fix kMeleeCeilingBias = 1;

static bool endpointsTouch(Vec2 a, Vec2 b) {
    return fxAbs(a.x - b.x) <= kMeleeSurfaceTolerance && fxAbs(a.y - b.y) <= kMeleeSurfaceTolerance;
}

static Vec2 segmentLeftEndpoint(const StageSegment& segment) {
    return segment.start.x <= segment.end.x ? segment.start : segment.end;
}

static Vec2 segmentRightEndpoint(const StageSegment& segment) {
    return segment.start.x > segment.end.x ? segment.start : segment.end;
}

static int linkedSameKindLineAtEndpoint(const World& world, int segmentIndex, Vec2 endpoint, SegmentLineKind kind) {
    if (segmentIndex < 0 || segmentIndex >= static_cast<int>(world.stage.segments.size())) {
        return -1;
    }

    const StageSegment& segment = world.stage.segments[static_cast<size_t>(segmentIndex)];
    for (int candidate : {segment.previousLine, segment.nextLine}) {
        if (candidate < 0 || candidate >= static_cast<int>(world.stage.segments.size())) {
            continue;
        }
        const StageSegment& linked = world.stage.segments[static_cast<size_t>(candidate)];
        if (linked.type != segment.type || effectiveLineKind(linked) != kind) {
            continue;
        }
        if (endpointsTouch(endpoint, linked.start) || endpointsTouch(endpoint, linked.end)) {
            return candidate;
        }
    }
    return -1;
}

static int linkedNonFloorLineAtEndpoint(const World& world, int segmentIndex, Vec2 endpoint) {
    if (segmentIndex < 0 || segmentIndex >= static_cast<int>(world.stage.segments.size())) {
        return -1;
    }

    const StageSegment& segment = world.stage.segments[static_cast<size_t>(segmentIndex)];
    for (int candidate : {segment.previousLine, segment.nextLine}) {
        if (candidate < 0 || candidate >= static_cast<int>(world.stage.segments.size())) {
            continue;
        }
        const StageSegment& linked = world.stage.segments[static_cast<size_t>(candidate)];
        if (effectiveLineKind(linked) == SegmentLineKind::Floor) {
            continue;
        }
        if (endpointsTouch(endpoint, linked.start) || endpointsTouch(endpoint, linked.end)) {
            return candidate;
        }
    }
    return -1;
}

static bool meleeProjectFloorFromLine(
    const World& world,
    const FighterRuntime& fighter,
    int lineId,
    Vec2 point,
    MeleeSurfaceProjection& projection)
{
    int current = lineId;
    int direction = 0;
    Fix x = point.x;

    for (int guard = 0; guard < static_cast<int>(world.stage.segments.size()); ++guard) {
        if (current < 0 || current >= static_cast<int>(world.stage.segments.size())) {
            return false;
        }
        const StageSegment& segment = world.stage.segments[static_cast<size_t>(current)];
        if (!canMaintainGroundOnSegment(fighter, segment, current)) {
            return false;
        }

        const Vec2 left = segmentLeftEndpoint(segment);
        const Vec2 right = segmentRightEndpoint(segment);
        if (x < left.x) {
            if (direction != 1) {
                const int linked = linkedSameKindLineAtEndpoint(world, current, left, SegmentLineKind::Floor);
                if (linked < 0) {
                    if (x - left.x < -kMeleeSurfaceTolerance) {
                        return false;
                    }
                    x = left.x;
                    break;
                }
                current = linked;
                direction = -1;
                continue;
            }
            x = left.x;
            break;
        }
        if (x > right.x) {
            if (direction != -1) {
                const int linked = linkedSameKindLineAtEndpoint(world, current, right, SegmentLineKind::Floor);
                if (linked < 0) {
                    if (x - right.x > kMeleeSurfaceTolerance) {
                        return false;
                    }
                    x = right.x;
                    break;
                }
                current = linked;
                direction = 1;
                continue;
            }
            x = right.x;
            break;
        }
        break;
    }

    const StageSegment& segment = world.stage.segments[static_cast<size_t>(current)];
    Fix y = 0;
    if (!segmentYAtX(segment, x, y)) {
        return false;
    }

    projection.hit = true;
    projection.segment = current;
    projection.point = {x, y};
    projection.normal = segmentNormal(segment);
    projection.push = y - point.y + kMeleeFloorBias;
    return true;
}

static bool meleeProjectCeilingFromLine(
    const World& world,
    int lineId,
    Vec2 point,
    MeleeSurfaceProjection& projection)
{
    int current = lineId;
    int direction = 0;
    Fix x = point.x;

    for (int guard = 0; guard < static_cast<int>(world.stage.segments.size()); ++guard) {
        if (current < 0 || current >= static_cast<int>(world.stage.segments.size())) {
            return false;
        }
        const StageSegment& segment = world.stage.segments[static_cast<size_t>(current)];
        if (segment.type != SegmentType::Solid || !isCeilingLine(segment)) {
            return false;
        }

        const Vec2 left = segmentLeftEndpoint(segment);
        const Vec2 right = segmentRightEndpoint(segment);
        if (x < left.x) {
            if (direction != 1) {
                const int linked = linkedSameKindLineAtEndpoint(world, current, left, SegmentLineKind::Ceiling);
                if (linked < 0) {
                    if (x - left.x < -kMeleeSurfaceTolerance) {
                        return false;
                    }
                    x = left.x;
                    break;
                }
                current = linked;
                direction = -1;
                continue;
            }
            x = left.x;
            break;
        }
        if (x > right.x) {
            if (direction != -1) {
                const int linked = linkedSameKindLineAtEndpoint(world, current, right, SegmentLineKind::Ceiling);
                if (linked < 0) {
                    if (x - right.x > kMeleeSurfaceTolerance) {
                        return false;
                    }
                    x = right.x;
                    break;
                }
                current = linked;
                direction = 1;
                continue;
            }
            x = right.x;
            break;
        }
        break;
    }

    const StageSegment& segment = world.stage.segments[static_cast<size_t>(current)];
    Fix y = 0;
    if (!segmentYAtX(segment, x, y)) {
        return false;
    }

    projection.hit = true;
    projection.segment = current;
    projection.point = {x, y};
    projection.normal = segmentNormal(segment);
    projection.push = y - point.y - kMeleeCeilingBias;
    return true;
}

static int findLandingSegment(const World& world, const FighterRuntime& fighter, Vec2 previousBottom, Vec2 currentBottom, Vec2& contact, Fix& fraction, int lineIdSkip = -1) {
    MeleeLineHit best = meleeCheckFloor(world, fighter, previousBottom, currentBottom, lineIdSkip);
    const Vec2 previousMidBottom{
        previousBottom.x,
        fighter.previousPosition.y + fxMul(fighter.previousEcb.points[1].y + fighter.previousEcb.points[3].y, fxFromFloat(0.5f)),
    };
    const MeleeLineHit midpointHit = meleeCheckFloor(world, fighter, previousMidBottom, currentBottom, lineIdSkip);
    if (midpointHit.hit && (!best.hit || midpointHit.distanceSquared < best.distanceSquared)) {
        best = midpointHit;
    }
    if (!best.hit) {
        return -1;
    }
    contact = best.contact;
    const Fix dx = currentBottom.x - previousBottom.x;
    const Fix dy = currentBottom.y - previousBottom.y;
    if (fxAbs(dx) >= fxAbs(dy) && dx != 0) {
        fraction = clamp01(fxDiv(contact.x - previousBottom.x, dx));
    } else if (dy != 0) {
        fraction = clamp01(fxDiv(contact.y - previousBottom.y, dy));
    } else {
        fraction = 0;
    }
    return best.segment;
}

static int carryRemainingMovementOnGround(const World& world, FighterRuntime& fighter, int segmentIndex, Fix landingFraction, Vec2 attemptedDelta) {
    const Fix remainingScale = fx(1) - landingFraction;
    if (remainingScale <= 0) {
        return segmentIndex;
    }

    const Vec2 remaining{fxMul(attemptedDelta.x, remainingScale), fxMul(attemptedDelta.y, remainingScale)};
    const Vec2 tangent = groundTangent(fighter.groundNormal);
    const Fix travel = fxMul(remaining.x, tangent.x) + fxMul(remaining.y, tangent.y);
    if (travel == 0) {
        return segmentIndex;
    }

    const Fix targetX = fighter.position.x + fighter.ecb.points[3].x + fxMul(tangent.x, travel);
    int currentIndex = segmentIndex;
    for (int guard = 0; guard < static_cast<int>(world.stage.segments.size()); ++guard) {
        if (currentIndex < 0 || currentIndex >= static_cast<int>(world.stage.segments.size())) {
            break;
        }
        const StageSegment& current = world.stage.segments[static_cast<size_t>(currentIndex)];
        if (!isFloorLine(current)) {
            break;
        }

        Fix groundY = 0;
        if (segmentYAtX(current, targetX, groundY)) {
            fighter.position.x = targetX - fighter.ecb.points[3].x;
            fighter.position.y = groundY - fighter.ecb.points[3].y;
            fighter.groundNormal = segmentNormal(current);
            return currentIndex;
        }

        const std::array<int, 2> linked = targetX < segmentMinX(current)
            ? (current.start.x == segmentMinX(current)
                ? std::array<int, 2>{current.nextLine, current.previousLine}
                : std::array<int, 2>{current.previousLine, current.nextLine})
            : (current.start.x == segmentMaxX(current)
                ? std::array<int, 2>{current.nextLine, current.previousLine}
                : std::array<int, 2>{current.previousLine, current.nextLine});

        int nextFloor = -1;
        for (int candidate : linked) {
            if (candidate >= 0 && candidate < static_cast<int>(world.stage.segments.size()) &&
                isFloorLine(world.stage.segments[static_cast<size_t>(candidate)]))
            {
                nextFloor = candidate;
                break;
            }
        }
        if (nextFloor < 0) {
            const Fix edgeX = std::clamp(targetX, segmentMinX(current), segmentMaxX(current));
            if (segmentYAtX(current, edgeX, groundY)) {
                fighter.position.x = edgeX - fighter.ecb.points[3].x;
                fighter.position.y = groundY - fighter.ecb.points[3].y;
                fighter.groundNormal = segmentNormal(current);
                return currentIndex;
            }
            break;
        }
        currentIndex = nextFloor;
    }

    return segmentIndex;
}

struct SweepContact {
    bool hit = false;
    int segment = -1;
    Fix fraction = fx(1);
    Vec2 contact = {};
    Vec2 normal = {};
    Vec2 pointOffset = {};
};

static void chooseEarlierContact(SweepContact& best, int segmentIndex, Fix fraction, Vec2 contact, Vec2 normal, Vec2 pointOffset = {}) {
    fraction = clamp01(fraction);
    if (!best.hit || fraction < best.fraction) {
        best.hit = true;
        best.segment = segmentIndex;
        best.fraction = fraction;
        best.contact = contact;
        best.normal = normal;
        best.pointOffset = pointOffset;
    }
}

static int64_t crossRaw(Vec2 a, Vec2 b) {
    return static_cast<int64_t>(a.x) * b.y - static_cast<int64_t>(a.y) * b.x;
}

static int64_t integerSqrt(int64_t value) {
    if (value <= 0) {
        return 0;
    }
    uint64_t x = static_cast<uint64_t>(value);
    uint64_t result = 0;
    uint64_t bit = uint64_t{1} << 62;
    while (bit > x) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (x >= result + bit) {
            x -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<int64_t>(result);
}

static void meleeCollisionLineEndpoints(const World& world, int segmentIndex, Vec2& start, Vec2& end) {
    const StageSegment& segment = world.stage.segments[static_cast<size_t>(segmentIndex)];
    start = segment.start;
    end = segment.end;
    const int64_t dx = static_cast<int64_t>(start.x) - end.x;
    const int64_t dy = static_cast<int64_t>(start.y) - end.y;
    const int64_t length = integerSqrt(dx * dx + dy * dy);
    if (length <= 0) {
        return;
    }
    const Fix extendX = static_cast<Fix>((dx * kScale) / length);
    const Fix extendY = static_cast<Fix>((dy * kScale) / length);
    if (segment.previousLine != -1) {
        start.x += extendX;
        start.y += extendY;
    }
    if (segment.nextLine != -1) {
        end.x -= extendX;
        end.y -= extendY;
    }
}

static Fix sweepFractionForContact(Vec2 sweepStart, Vec2 sweepEnd, Vec2 contact) {
    const Fix dx = sweepEnd.x - sweepStart.x;
    const Fix dy = sweepEnd.y - sweepStart.y;
    if (fxAbs(dx) >= fxAbs(dy) && dx != 0) {
        return clamp01(fxDiv(contact.x - sweepStart.x, dx));
    }
    if (dy != 0) {
        return clamp01(fxDiv(contact.y - sweepStart.y, dy));
    }
    return 0;
}

static bool meleeLineIntersection(Vec2 a0, Vec2 a1, Vec2 b0, Vec2 b1, Vec2& intersection) {
    bool b1BelowA = false;
    bool b2AboveA = false;

    if (a0.x <= a1.x) {
        if ((b0.x < a0.x && b1.x < a0.x) || (a1.x < b0.x && a1.x < b1.x)) {
            return false;
        }
    } else if ((b0.x < a1.x && b1.x < a1.x) || (a0.x < b0.x && a0.x < b1.x)) {
        return false;
    }

    if (a0.y <= a1.y) {
        if ((b0.y < a0.y && b1.y < a0.y) || (a1.y < b0.y && a1.y < b1.y)) {
            return false;
        }
    } else if ((b0.y < a1.y && b1.y < a1.y) || (a0.y < b0.y && a0.y < b1.y)) {
        return false;
    }

    const int64_t ah = static_cast<int64_t>(a1.y) - a0.y;
    const int64_t aw = static_cast<int64_t>(a1.x) - a0.x;
    const int64_t d0x = static_cast<int64_t>(b0.x) - a0.x;
    const int64_t d0y = static_cast<int64_t>(b0.y) - a0.y;
    const int64_t hsB0A = aw * d0y - ah * d0x;
    constexpr int64_t areaTolerance = static_cast<int64_t>(100) * kScale;
    if (hsB0A < 0) {
        if (hsB0A < -areaTolerance) {
            return false;
        }
        b1BelowA = true;
    }

    const int64_t d1x = static_cast<int64_t>(b1.x) - a1.x;
    const int64_t d1y = static_cast<int64_t>(b1.y) - a1.y;
    const int64_t hsB1A = aw * d1y - ah * d1x;
    if (hsB1A > 0) {
        if (hsB1A > areaTolerance) {
            return false;
        }
        b2AboveA = true;
    }

    if (hsB0A == 0 && hsB1A == 0) {
        return false;
    }

    const int64_t det = d0x * d1y - d0y * d1x;
    if (det < hsB0A) {
        if (det < hsB1A) {
            return false;
        }
    } else if (det > hsB0A && det > hsB1A) {
        return false;
    }

    const int64_t bw = static_cast<int64_t>(b1.x) - b0.x;
    const int64_t bh = static_cast<int64_t>(b1.y) - b0.y;
    if ((bw == 0 && bh == 0) || (b1BelowA && b2AboveA) || (hsB0A >= 0 && b2AboveA)) {
        return false;
    }

    const int64_t area = bw * ah - bh * aw;
    if (area > -10 && area < 10) {
        return false;
    }

    const int64_t tScaled = ((bw * d0y - bh * d0x) * kScale) / area;
    if (tScaled > 0) {
        if (tScaled < kScale) {
            intersection = {
                static_cast<Fix>(a0.x + (aw * tScaled) / kScale),
                static_cast<Fix>(a0.y + (ah * tScaled) / kScale),
            };
        } else {
            intersection = a1;
        }
    } else {
        intersection = a0;
    }
    return true;
}

static bool meleeLineIntersectionH(Vec2 a0, Fix a1x, Vec2 b0, Vec2 b1, Vec2& intersection) {
    Fix minAx = 0;
    Fix maxAx = 0;
    if (a0.x < a1x) {
        if ((b0.x < a0.x && b1.x < a0.x) || (a1x < b0.x && a1x < b1.x)) {
            return false;
        }
        if (b0.y - a0.y < -kMeleeCeilingBias || b1.y - a0.y > kMeleeCeilingBias) {
            return false;
        }
        minAx = a0.x;
        maxAx = a1x;
    } else {
        if ((b0.x < a1x && b1.x < a1x) || (a0.x < b0.x && a0.x < b1.x)) {
            return false;
        }
        if (b1.y - a0.y < -kMeleeCeilingBias || b0.y - a0.y > kMeleeCeilingBias) {
            return false;
        }
        minAx = a1x;
        maxAx = a0.x;
    }

    const Fix dby = b1.y - b0.y;
    const Fix dbx = b1.x - b0.x;
    if (fxAbs(dby) < kMeleeCeilingBias) {
        return false;
    }
    Fix newX = b0.x + fxMul(fxDiv(dbx, dby), a0.y - b0.y);
    const Fix minDelta = newX - minAx;
    if (minDelta < 0) {
        if (minDelta < -kMeleeSurfaceTolerance) {
            return false;
        }
        newX = minAx;
    }
    const Fix maxDelta = newX - maxAx;
    if (maxDelta > 0) {
        if (maxDelta > kMeleeSurfaceTolerance) {
            return false;
        }
        newX = maxAx;
    }
    intersection = {newX, a0.y};
    return true;
}

static bool meleeLineIntersectionV(Fix a0x, Fix a0y, Fix a1y, Vec2 b0, Vec2 b1, Vec2& intersection) {
    Fix minAy = 0;
    Fix maxAy = 0;
    if (a0y < a1y) {
        if ((b0.y < a0y && b1.y < a0y) || (a1y < b0.y && a1y < b1.y)) {
            return false;
        }
        if (b1.x - a0x < -kMeleeCeilingBias || b0.x - a0x > kMeleeCeilingBias) {
            return false;
        }
        minAy = a0y;
        maxAy = a1y;
    } else {
        if ((b0.y < a1y && b1.y < a1y) || (a0y < b0.y && a0y < b1.y)) {
            return false;
        }
        if (b0.x - a0x < -kMeleeCeilingBias || b1.x - a0x > kMeleeCeilingBias) {
            return false;
        }
        minAy = a1y;
        maxAy = a0y;
    }

    const Fix dbx = b1.x - b0.x;
    const Fix dby = b1.y - b0.y;
    if (fxAbs(dbx) < kMeleeCeilingBias) {
        return false;
    }
    Fix newY = b0.y + fxMul(fxDiv(dby, dbx), a0x - b0.x);
    Fix delta = newY - minAy;
    if (delta < 0) {
        if (delta < -kMeleeSurfaceTolerance) {
            return false;
        }
        newY = minAy;
    }
    delta = newY - maxAy;
    if (delta > 0) {
        if (delta > kMeleeSurfaceTolerance) {
            return false;
        }
        newY = maxAy;
    }
    intersection = {a0x, newY};
    return true;
}

static bool meleeCheckFloorLine(const World& world, int segmentIndex, Vec2 sweepStart, Vec2 sweepEnd, Vec2& contact, Fix& fraction) {
    Vec2 a0{};
    Vec2 a1{};
    meleeCollisionLineEndpoints(world, segmentIndex, a0, a1);
    if (fxAbs(a0.y - a1.y) > kMeleeCeilingBias) {
        if (!meleeLineIntersection(a0, a1, sweepStart, sweepEnd, contact)) {
            return false;
        }
    } else {
        if (sweepStart.y < sweepEnd.y || !meleeLineIntersectionH(a0, a1.x, sweepStart, sweepEnd, contact)) {
            return false;
        }
    }
    fraction = sweepFractionForContact(sweepStart, sweepEnd, contact);
    return true;
}

static bool meleeCheckCeilingLine(const World& world, int segmentIndex, Vec2 sweepStart, Vec2 sweepEnd, Vec2& contact, Fix& fraction) {
    Vec2 a0{};
    Vec2 a1{};
    meleeCollisionLineEndpoints(world, segmentIndex, a0, a1);
    if (fxAbs(a0.y - a1.y) > kMeleeCeilingBias) {
        if (!meleeLineIntersection(a0, a1, sweepStart, sweepEnd, contact)) {
            return false;
        }
    } else {
        if (sweepStart.y > sweepEnd.y || !meleeLineIntersectionH(a0, a1.x, sweepStart, sweepEnd, contact)) {
            return false;
        }
    }
    fraction = sweepFractionForContact(sweepStart, sweepEnd, contact);
    return true;
}

static bool meleeCheckWallLine(const World& world, int segmentIndex, int movementDir, Vec2 sweepStart, Vec2 sweepEnd, Vec2& contact, Fix& fraction) {
    Vec2 a0{};
    Vec2 a1{};
    meleeCollisionLineEndpoints(world, segmentIndex, a0, a1);
    if (fxAbs(a0.x - a1.x) > kMeleeCeilingBias) {
        if (!meleeLineIntersection(a0, a1, sweepStart, sweepEnd, contact)) {
            return false;
        }
    } else if (movementDir > 0) {
        if (sweepStart.x > sweepEnd.x || !meleeLineIntersectionV(a0.x, a0.y, a1.y, sweepStart, sweepEnd, contact)) {
            return false;
        }
    } else {
        if (sweepStart.x < sweepEnd.x || !meleeLineIntersectionV(a0.x, a0.y, a1.y, sweepStart, sweepEnd, contact)) {
            return false;
        }
    }
    fraction = sweepFractionForContact(sweepStart, sweepEnd, contact);
    return true;
}

static bool segmentSweepIntersection(Vec2 sweepStart, Vec2 sweepEnd, const StageSegment& segment, Fix& fraction, Vec2& contact) {
    const Vec2 r{sweepEnd.x - sweepStart.x, sweepEnd.y - sweepStart.y};
    const Vec2 s{segment.end.x - segment.start.x, segment.end.y - segment.start.y};
    const int64_t denom = crossRaw(r, s);
    if (denom == 0) {
        return false;
    }

    const Vec2 qp{segment.start.x - sweepStart.x, segment.start.y - sweepStart.y};
    int64_t tNumer = crossRaw(qp, s);
    int64_t uNumer = crossRaw(qp, r);
    int64_t positiveDenom = denom;
    if (positiveDenom < 0) {
        positiveDenom = -positiveDenom;
        tNumer = -tNumer;
        uNumer = -uNumer;
    }
    if (tNumer < 0 || tNumer > positiveDenom || uNumer < 0 || uNumer > positiveDenom) {
        return false;
    }

    fraction = clamp01(static_cast<Fix>((tNumer * kScale) / positiveDenom));
    contact = {
        sweepStart.x + fxMul(r.x, fraction),
        sweepStart.y + fxMul(r.y, fraction),
    };
    return true;
}

static Fix distanceSquared(Vec2 a, Vec2 b) {
    const Fix dx = a.x - b.x;
    const Fix dy = a.y - b.y;
    const int64_t raw = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
    return raw > std::numeric_limits<Fix>::max() ? std::numeric_limits<Fix>::max() : static_cast<Fix>(raw);
}

static MeleeLineHit meleeCheckFloor(const World& world, const FighterRuntime& fighter, Vec2 sweepStart, Vec2 sweepEnd, int lineIdSkip) {
    MeleeLineHit best;
    const FighterProperties& attr = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)].properties;
    for (size_t i = 0; i < world.stage.segments.size(); ++i) {
        if (static_cast<int>(i) == lineIdSkip) {
            continue;
        }
        const StageSegment& segment = world.stage.segments[i];
        if (!canStandOnSegment(fighter, attr, segment, static_cast<int>(i))) {
            continue;
        }

        Vec2 contact = {};
        Fix fraction = 0;
        if (!meleeCheckFloorLine(world, static_cast<int>(i), sweepStart, sweepEnd, contact, fraction)) {
            continue;
        }

        const Fix dist2 = distanceSquared(contact, sweepStart);
        if (!best.hit || dist2 < best.distanceSquared) {
            best.hit = true;
            best.segment = static_cast<int>(i);
            best.distanceSquared = dist2;
            best.contact = contact;
        }
    }
    return best;
}

static MeleeLineHit meleeCheckCeiling(const World& world, Vec2 sweepStart, Vec2 sweepEnd) {
    MeleeLineHit best;
    for (size_t i = 0; i < world.stage.segments.size(); ++i) {
        const StageSegment& segment = world.stage.segments[i];
        if (segment.type != SegmentType::Solid || !isCeilingLine(segment)) {
            continue;
        }

        Vec2 contact = {};
        Fix fraction = 0;
        if (!meleeCheckCeilingLine(world, static_cast<int>(i), sweepStart, sweepEnd, contact, fraction)) {
            continue;
        }

        const Fix dist2 = distanceSquared(contact, sweepStart);
        if (!best.hit || dist2 < best.distanceSquared) {
            best.hit = true;
            best.segment = static_cast<int>(i);
            best.distanceSquared = dist2;
            best.contact = contact;
        }
    }
    return best;
}

static Vec2 interpolateVec2(Vec2 a, Vec2 b, Fix fraction) {
    return {
        a.x + fxMul(b.x - a.x, fraction),
        a.y + fxMul(b.y - a.y, fraction),
    };
}

static Vec2 meleeRemap2d(Vec2 a0, Vec2 a1, Vec2 b0, Vec2 b1, Vec2 point) {
    const Fix dx = a1.x - a0.x;
    const Fix dy = a1.y - a0.y;
    const Fix px = point.x - a0.x;
    const Fix py = point.y - a0.y;
    const int64_t dist2 = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
    if (dist2 > 0) {
        int64_t tRaw = ((static_cast<int64_t>(dy) * py + static_cast<int64_t>(dx) * px) * kScale) / dist2;
        if (tRaw < 0) {
            tRaw = 0;
        } else if (tRaw > kScale) {
            tRaw = kScale;
        }
        const Fix t = static_cast<Fix>(tRaw);
        return {
            point.x + fxMul(fx(1) - t, b0.x - a0.x) + fxMul(t, b1.x - a1.x),
            point.y + fxMul(fx(1) - t, b0.y - a0.y) + fxMul(t, b1.y - a1.y),
        };
    }
    return {
        point.x + (b0.x - a0.x) + (b1.x - a0.x),
        point.y + (b0.y - a0.y) + (b1.y - a0.y),
    };
}

static Vec2 pointOffsetOnCurrentEcbEdge(const FighterRuntime& fighter, int firstPoint, int secondPoint, Vec2 worldPoint);

static bool currentEcbEdgeIntersection(
    const World& world,
    const FighterRuntime& fighter,
    int segmentIndex,
    int movementDir,
    int firstPoint,
    int secondPoint,
    Vec2& contact,
    Vec2& pointOffset)
{
    const Vec2 currentA = fighter.position + fighter.ecb.points[static_cast<size_t>(firstPoint)];
    const Vec2 currentB = fighter.position + fighter.ecb.points[static_cast<size_t>(secondPoint)];
    Fix edgeFraction = fx(1);
    if (!meleeCheckWallLine(world, segmentIndex, movementDir, currentA, currentB, contact, edgeFraction)) {
        return false;
    }

    pointOffset = pointOffsetOnCurrentEcbEdge(fighter, firstPoint, secondPoint, contact);
    return true;
}

static Vec2 pointOffsetOnCurrentEcbEdge(const FighterRuntime& fighter, int firstPoint, int secondPoint, Vec2 worldPoint) {
    const Vec2 currentA = fighter.position + fighter.ecb.points[static_cast<size_t>(firstPoint)];
    const Vec2 currentB = fighter.position + fighter.ecb.points[static_cast<size_t>(secondPoint)];
    const Vec2 edge{currentB.x - currentA.x, currentB.y - currentA.y};
    Fix fraction = 0;
    if (fxAbs(edge.y) >= fxAbs(edge.x) && edge.y != 0) {
        fraction = fxDiv(worldPoint.y - currentA.y, edge.y);
    } else if (edge.x != 0) {
        fraction = fxDiv(worldPoint.x - currentA.x, edge.x);
    }
    return interpolateVec2(
        fighter.ecb.points[static_cast<size_t>(firstPoint)],
        fighter.ecb.points[static_cast<size_t>(secondPoint)],
        clamp01(fraction));
}

static bool sweptEcbEdgeEndpointContact(
    const FighterRuntime& fighter,
    int firstPoint,
    int secondPoint,
    const StageSegment& segment,
    Vec2& contact,
    Vec2& pointOffset)
{
    const Vec2 previousA = fighter.previousPosition + fighter.previousEcb.points[static_cast<size_t>(firstPoint)];
    const Vec2 previousB = fighter.previousPosition + fighter.previousEcb.points[static_cast<size_t>(secondPoint)];
    const Vec2 currentA = fighter.position + fighter.ecb.points[static_cast<size_t>(firstPoint)];
    const Vec2 currentB = fighter.position + fighter.ecb.points[static_cast<size_t>(secondPoint)];

    bool hit = false;
    int64_t bestDist2 = std::numeric_limits<int64_t>::max();
    for (Vec2 endpoint : {segment.start, segment.end}) {
        const Vec2 remapped = meleeRemap2d(previousA, previousB, currentA, currentB, endpoint);
        const Vec2 delta{endpoint.x - remapped.x, endpoint.y - remapped.y};
        const int64_t deltaDist2 = static_cast<int64_t>(delta.x) * delta.x + static_cast<int64_t>(delta.y) * delta.y;
        if (deltaDist2 <= static_cast<int64_t>(1)) {
            continue;
        }

        Vec2 intersection = {};
        if (!meleeLineIntersection(currentA, currentB, remapped, endpoint, intersection)) {
            continue;
        }

        const Vec2 fromEndpoint{intersection.x - endpoint.x, intersection.y - endpoint.y};
        int64_t dist2 = static_cast<int64_t>(fromEndpoint.x) * fromEndpoint.x +
            static_cast<int64_t>(fromEndpoint.y) * fromEndpoint.y;
        if (static_cast<int64_t>(delta.x) * fromEndpoint.x + static_cast<int64_t>(delta.y) * fromEndpoint.y < 0) {
            dist2 = -dist2;
        }
        if (dist2 < bestDist2) {
            bestDist2 = dist2;
            contact = intersection;
            pointOffset = pointOffsetOnCurrentEcbEdge(fighter, firstPoint, secondPoint, intersection);
            hit = true;
        }
    }
    return hit;
}

static bool isWallLinkedToCurrentFloor(const World& world, const FighterRuntime& fighter, int wallSegment) {
    if (fighter.groundSegment < 0 ||
        fighter.groundSegment >= static_cast<int>(world.stage.segments.size()))
    {
        return false;
    }
    const StageSegment& floor = world.stage.segments[static_cast<size_t>(fighter.groundSegment)];
    return floor.nextLine == wallSegment || floor.previousLine == wallSegment;
}

static SweepContact findEcbWallContact(const World& world, const FighterRuntime& fighter, int movementDir) {
    SweepContact best;
    if (movementDir == 0) {
        return best;
    }

    const std::array<int, 3> pointIndices = movementDir > 0
        ? std::array<int, 3>{2, 3, 1}
        : std::array<int, 3>{0, 3, 1};
    const std::array<std::array<int, 2>, 2> sideEdges = movementDir > 0
        ? std::array<std::array<int, 2>, 2>{std::array<int, 2>{3, 2}, std::array<int, 2>{1, 2}}
        : std::array<std::array<int, 2>, 2>{std::array<int, 2>{3, 0}, std::array<int, 2>{1, 0}};
    const SegmentLineKind blockingKind = movementDir > 0 ? SegmentLineKind::LeftWall : SegmentLineKind::RightWall;
    for (size_t i = 0; i < world.stage.segments.size(); ++i) {
        const StageSegment& segment = world.stage.segments[i];
        if (segment.type != SegmentType::Solid || effectiveLineKind(segment) != blockingKind) {
            continue;
        }

        const bool linkedToCurrentFloor = isWallLinkedToCurrentFloor(world, fighter, static_cast<int>(i));
        if (linkedToCurrentFloor) {
            continue;
        }
        for (int pointIndex : pointIndices) {
            const Vec2 previousPoint = fighter.previousPosition + fighter.previousEcb.points[static_cast<size_t>(pointIndex)];
            const Vec2 currentPoint = fighter.position + fighter.ecb.points[static_cast<size_t>(pointIndex)];
            Fix fraction = fx(1);
            Vec2 contact = {};
            if (!meleeCheckWallLine(world, static_cast<int>(i), movementDir, previousPoint, currentPoint, contact, fraction)) {
                continue;
            }
            Fix wallX = 0;
            if (segmentXAtY(segment, currentPoint.y, wallX)) {
                contact = {wallX, currentPoint.y};
            }
            chooseEarlierContact(
                best,
                static_cast<int>(i),
                fraction,
                contact,
                {movementDir > 0 ? -fx(1) : fx(1), 0},
                fighter.ecb.points[static_cast<size_t>(pointIndex)]);
        }

        for (const auto& edge : sideEdges) {
            Vec2 contact = {};
            Vec2 pointOffset = {};
            if (!currentEcbEdgeIntersection(world, fighter, static_cast<int>(i), movementDir, edge[0], edge[1], contact, pointOffset)) {
                if (!sweptEcbEdgeEndpointContact(fighter, edge[0], edge[1], segment, contact, pointOffset)) {
                    continue;
                }
            }
            chooseEarlierContact(
                best,
                static_cast<int>(i),
                fx(1),
                contact,
                {movementDir > 0 ? -fx(1) : fx(1), 0},
                pointOffset);
        }
    }
    return best;
}

static bool pointTouchesSegment(Vec2 point, const StageSegment& segment, Fix tolerance) {
    const Vec2 seg{segment.end.x - segment.start.x, segment.end.y - segment.start.y};
    const int64_t lenSq = static_cast<int64_t>(seg.x) * seg.x + static_cast<int64_t>(seg.y) * seg.y;
    if (lenSq == 0) {
        return false;
    }

    const Vec2 relative{point.x - segment.start.x, point.y - segment.start.y};
    const int64_t dot = static_cast<int64_t>(relative.x) * seg.x + static_cast<int64_t>(relative.y) * seg.y;
    if (dot < -static_cast<int64_t>(tolerance) * kScale || dot > lenSq + static_cast<int64_t>(tolerance) * kScale) {
        return false;
    }

    const int64_t cross = crossRaw(relative, seg);
    const int64_t absCross = cross < 0 ? -cross : cross;
    const float len = std::sqrt(static_cast<float>(lenSq)) / static_cast<float>(kScale);
    return absCross <= static_cast<int64_t>(fxToFloat(tolerance) * len * static_cast<float>(kScale));
}

struct WallHugContact {
    int side = 0;
    int segment = -1;
};

static WallHugContact findWallHugContact(const World& world, const FighterRuntime& fighter) {
    const Fix tolerance = fxFromFloat(0.08f);
    const Vec2 left = fighter.position + fighter.ecb.points[0];
    const Vec2 right = fighter.position + fighter.ecb.points[2];
    for (size_t i = 0; i < world.stage.segments.size(); ++i) {
        const StageSegment& segment = world.stage.segments[i];
        if (segment.type != SegmentType::Solid || !isWallLine(segment)) {
            continue;
        }
        if (pointTouchesSegment(left, segment, tolerance)) {
            return {-1, static_cast<int>(i)};
        }
        if (pointTouchesSegment(right, segment, tolerance)) {
            return {1, static_cast<int>(i)};
        }
    }
    return {};
}

static SweepContact findCeilingContact(const World& world, const FighterRuntime& fighter) {
    SweepContact best;
    const Vec2 previousTop = fighter.previousPosition + fighter.previousEcb.points[1];
    const Vec2 currentTop = fighter.position + fighter.ecb.points[1];
    const MeleeLineHit hit = meleeCheckCeiling(world, previousTop, currentTop);
    if (hit.hit) {
        Fix fraction = 0;
        const Fix dx = currentTop.x - previousTop.x;
        const Fix dy = currentTop.y - previousTop.y;
        if (fxAbs(dx) >= fxAbs(dy) && dx != 0) {
            fraction = clamp01(fxDiv(hit.contact.x - previousTop.x, dx));
        } else if (dy != 0) {
            fraction = clamp01(fxDiv(hit.contact.y - previousTop.y, dy));
        }
        chooseEarlierContact(best, hit.segment, fraction, hit.contact, {0, -fx(1)}, fighter.ecb.points[1]);
    }
    return best;
}

static int linkedCeilingAtWall(const World& world, int wallSegment) {
    if (wallSegment < 0 || wallSegment >= static_cast<int>(world.stage.segments.size())) {
        return -1;
    }
    const StageSegment& wall = world.stage.segments[static_cast<size_t>(wallSegment)];
    for (int linked : {wall.previousLine, wall.nextLine}) {
        if (linked >= 0 && linked < static_cast<int>(world.stage.segments.size())) {
            const StageSegment& segment = world.stage.segments[static_cast<size_t>(linked)];
            if (segment.type == SegmentType::Solid && isCeilingLine(segment)) {
                return linked;
            }
        }
    }
    return -1;
}

static bool resolveMeleeLinkedCeilingFromWall(World& world, FighterRuntime& fighter, int wallSegment) {
    const int ceilingIndex = linkedCeilingAtWall(world, wallSegment);
    if (ceilingIndex < 0) {
        return false;
    }

    const Vec2 top = fighter.position + fighter.ecb.points[1];
    MeleeSurfaceProjection projection;
    if (meleeProjectCeilingFromLine(world, ceilingIndex, top, projection)) {
        if (projection.push >= 0) {
            return false;
        }
        fighter.position.y += projection.push;
        if (fighter.fighterVelocity.y > 0) {
            fighter.fighterVelocity.y = 0;
        }
        if (fighter.attackerShieldKnockback.y > 0) {
            fighter.attackerShieldKnockback.y = 0;
        }
        refreshEcbMetadata(fighter.ecb, fighter);
        return true;
    }

    return false;
}

static bool resolveWallAndCeiling(World& world, FighterRuntime& fighter, Vec2 attemptedDelta) {
    const FighterState& state = currentState(world, fighter);
    int wallContactSide = 0;
    int wallContactSegment = -1;
    int pushedWallSegment = -1;
    auto resolveWallSide = [&](int side) {
        if (wallContactSide != 0) {
            return;
        }
        const SweepContact wall = findEcbWallContact(world, fighter, side);
        if (wall.hit) {
            fighter.position.x = wall.contact.x - wall.pointOffset.x;
            if ((side > 0 && fighter.fighterVelocity.x > 0) || (side < 0 && fighter.fighterVelocity.x < 0)) {
                fighter.fighterVelocity.x = 0;
            }
            if ((side > 0 && fighter.groundVelocity > 0) || (side < 0 && fighter.groundVelocity < 0)) {
                fighter.groundVelocity = 0;
            }
            if ((side > 0 && fighter.attackerShieldKnockback.x > 0) ||
                (side < 0 && fighter.attackerShieldKnockback.x < 0))
            {
                fighter.attackerShieldKnockback.x = 0;
                fighter.groundAttackerShieldKnockbackVelocity = 0;
            }
            wallContactSide = side;
            wallContactSegment = wall.segment;
            pushedWallSegment = wall.segment;
            refreshEcbMetadata(fighter.ecb, fighter);
        }
    };
    if (state.allowWallCollision) {
        if (attemptedDelta.x > 0) {
            resolveWallSide(1);
            resolveWallSide(-1);
        } else if (attemptedDelta.x < 0) {
            resolveWallSide(-1);
            resolveWallSide(1);
        } else {
            resolveWallSide(1);
            resolveWallSide(-1);
        }
    }

    if (state.allowWallCollision && wallContactSide == 0) {
        const WallHugContact hug = findWallHugContact(world, fighter);
        wallContactSide = hug.side;
        wallContactSegment = hug.segment;
    }
    fighter.wallContactSide = wallContactSide;
    fighter.wallContactSegment = wallContactSegment;
    if (fighter.wallContactSide != 0) {
        fighter.wallContactTimer = 0;
    }

    bool hitCeiling = false;
    const bool linkedCeilingResolved = state.allowCeilingCollision && !fighter.grounded &&
        attemptedDelta.y > 0 &&
        pushedWallSegment >= 0 &&
        resolveMeleeLinkedCeilingFromWall(world, fighter, pushedWallSegment);
    hitCeiling = linkedCeilingResolved;

    if (state.allowCeilingCollision && !fighter.grounded && attemptedDelta.y > 0) {
        const SweepContact ceiling = linkedCeilingResolved ? SweepContact{} : findCeilingContact(world, fighter);
        if (!linkedCeilingResolved && ceiling.hit) {
            fighter.position.y = ceiling.contact.y - ceiling.pointOffset.y;
            if (fighter.fighterVelocity.y > 0) {
                fighter.fighterVelocity.y = 0;
            }
            if (fighter.attackerShieldKnockback.y > 0) {
                fighter.attackerShieldKnockback.y = 0;
            }
            refreshEcbMetadata(fighter.ecb, fighter);
            hitCeiling = true;
        }
    }
    return hitCeiling;
}

static bool ledgeInSnapRange(const FighterRuntime& fighter, const FighterProperties& attr, const StageLedge& ledge) {
    const Fix ledgeSnapX = fxMul(attr.ledgeSnapX, attr.modelScale);
    const Fix ledgeSnapY = fxMul(attr.ledgeSnapY, attr.modelScale);
    const Fix ledgeSnapHeight = fxMul(attr.ledgeSnapHeight, attr.modelScale);
    const Fix halfHeight = fxMul(ledgeSnapHeight, fxFromFloat(0.5f));
    const Fix prevX = fighter.previousPosition.x;
    const Fix curX = fighter.position.x;
    const Fix prevY = fighter.previousPosition.y;
    const Fix curY = fighter.position.y;
    const Fix bottom = std::min(prevY, curY) + ledgeSnapY - halfHeight;
    const Fix top = std::max(prevY, curY) + ledgeSnapY + halfHeight;
    const Vec2 currentBottom = fighter.position + fighter.ecb.points[3];

    if (ledge.position.y < bottom || ledge.position.y > top) {
        return false;
    }
    if (currentBottom.y >= ledge.position.y) {
        return false;
    }

    if (ledge.direction < 0) {
        const Fix left = std::min(prevX, curX);
        const Fix right = ledgeSnapX +
            (prevX < curX ? curX + fighter.ecb.points[2].x : prevX + fighter.ecb.points[2].x);
        return ledge.position.x >= left && ledge.position.x <= right && currentBottom.x < ledge.position.x;
    }

    const Fix right = std::max(prevX, curX);
    const Fix left = -ledgeSnapX +
        (prevX > curX ? curX + fighter.ecb.points[0].x : prevX + fighter.ecb.points[0].x);
    return ledge.position.x >= left && ledge.position.x <= right && currentBottom.x > ledge.position.x;
}

static bool ledgeProbeBlockedByStage(const World& world, const StageLedge& ledge, Vec2 probeStart) {
    for (size_t i = 0; i < world.stage.segments.size(); ++i) {
        if (static_cast<int>(i) == ledge.segmentIndex) {
            continue;
        }

        const StageSegment& segment = world.stage.segments[i];
        if (segment.type != SegmentType::Solid) {
            continue;
        }

        Fix fraction = fx(1);
        Vec2 contact = {};
        if (!segmentSweepIntersection(probeStart, ledge.position, segment, fraction, contact)) {
            continue;
        }
        if (fraction > 0 && fraction < fx(1)) {
            return true;
        }
    }
    return false;
}

static bool ledgePathClear(const World& world, const FighterRuntime& fighter, const StageLedge& ledge) {
    const Vec2 currentTop = fighter.position + fighter.ecb.points[1];
    Vec2 currentBottomProbe = fighter.position + fighter.ecb.points[3];
    currentBottomProbe.y -= fx(2);
    return !ledgeProbeBlockedByStage(world, ledge, currentTop) &&
        !ledgeProbeBlockedByStage(world, ledge, currentBottomProbe);
}

static bool positionAtLedgeFromCurrentAnimation(const FighterDefinition& def, FighterRuntime& fighter, const StageLedge& ledge) {
    const FighterState& state = def.states[static_cast<size_t>(fighter.state)];
    const AnimationClip* clip = clipForState(def, state);
    if (!clip) {
        return false;
    }
    const AnimationPose pose = evaluateClip(def.hsdAsset->skeleton, *clip, fighter.animationFrame);
    if (pose.joints.size() <= 1) {
        return false;
    }
    const Vec3 transN = scaledVec3(pose.joints[1].translation, def.properties.modelScale);
    fighter.position.x = ledge.position.x + fighter.facing * transN.z;
    fighter.position.y = ledge.position.y + transN.y;
    return true;
}

static bool tryGrabLedge(World& world, size_t fighterIndex) {
    FighterRuntime& fighter = world.fighters[fighterIndex];
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const FighterState& state = currentState(world, fighter);
    const FighterProperties& attr = def.properties;
    if (!state.allowLedgeGrab || fighter.grounded || fighter.grabbedLedge >= 0 || fighter.ledgeCooldown > 0 ||
        fighter.input.frames[0].move.y <= -attr.common.ledgeNoGrabDownThresholdX480)
    {
        return false;
    }
    if (fighter.position.y >= fighter.previousPosition.y) {
        return false;
    }

    for (size_t i = 0; i < world.stage.ledges.size(); ++i) {
        const StageLedge& ledge = world.stage.ledges[i];
        if (!state.allowBackwardsLedgeGrab && fighter.facing != -ledge.direction) {
            continue;
        }
        if (!ledgeInSnapRange(fighter, attr, ledge)) {
            continue;
        }
        if (!ledgePathClear(world, fighter, ledge)) {
            continue;
        }

        fighter.grounded = false;
        fighter.groundSegment = -1;
        fighter.grabbedLedge = static_cast<int>(i);
        fighter.facing = -ledge.direction;
        fighter.fighterVelocity = {};
        fighter.knockbackVelocity = {};
        fighter.attackerShieldKnockback = {};
        fighter.groundAttackerShieldKnockbackVelocity = 0;
        fighter.jumpsUsed = 0;
        fighter.wallJumpsUsed = 0;
        setFighterFlag(fighter, 12, false);
        fighter.position.x = ledge.position.x + ledge.direction * attr.ledgeHangX;
        fighter.position.y = ledge.position.y + attr.ledgeHangY;
        refreshEcbMetadata(fighter.ecb, fighter);
        lockFighterEcb(fighter, 10);
        changeFighterState(world, fighter, "CliffCatch", 0);
        positionAtLedgeFromCurrentAnimation(def, fighter, ledge);
        refreshEcbMetadata(fighter.ecb, fighter);
        return true;
    }
    return false;
}

static bool validStageSegmentIndex(const World& world, int segmentIndex) {
    return segmentIndex >= 0 && segmentIndex < static_cast<int>(world.stage.segments.size());
}

static bool canMaintainGroundOnSegment(const FighterRuntime& fighter, const StageSegment& segment, int segmentIndex) {
    return isFloorLine(segment) && fighter.floorSkipSegment != segmentIndex;
}

static bool snapToGroundSegment(const World& world, FighterRuntime& fighter, int segmentIndex, Vec2 bottom) {
    MeleeSurfaceProjection projection;
    if (!meleeProjectFloorFromLine(world, fighter, segmentIndex, bottom, projection)) {
        return false;
    }

    fighter.groundSegment = projection.segment;
    fighter.groundNormal = projection.normal;
    fighter.position.y += projection.push;
    projectGroundVelocity(fighter);
    return true;
}

static bool meleeMaintainCurrentFloor(const World& world, FighterRuntime& fighter) {
    if (!validStageSegmentIndex(world, fighter.groundSegment)) {
        return false;
    }

    const int floorSegmentIndex = fighter.groundSegment;
    const StageSegment& floor = world.stage.segments[static_cast<size_t>(floorSegmentIndex)];
    if (!canMaintainGroundOnSegment(fighter, floor, floorSegmentIndex)) {
        return false;
    }

    const Vec2 bottom = fighter.position + fighter.ecb.points[3];
    if (snapToGroundSegment(world, fighter, floorSegmentIndex, bottom)) {
        return true;
    }

    bool hitWall = false;
    Vec2 edge = {};
    const Vec2 left = segmentLeftEndpoint(floor);
    const Vec2 right = segmentRightEndpoint(floor);
    if (fighter.position.x < left.x) {
        edge = left;
        const int nonFloor = linkedNonFloorLineAtEndpoint(world, floorSegmentIndex, edge);
        if (validStageSegmentIndex(world, nonFloor) &&
            effectiveLineKind(world.stage.segments[static_cast<size_t>(nonFloor)]) == SegmentLineKind::RightWall)
        {
            hitWall = true;
        } else {
            fighter.runoffSegment = floorSegmentIndex;
            fighter.runoffDirection = -1;
        }
    } else if (fighter.position.x > right.x) {
        edge = right;
        const int nonFloor = linkedNonFloorLineAtEndpoint(world, floorSegmentIndex, edge);
        if (validStageSegmentIndex(world, nonFloor) &&
            effectiveLineKind(world.stage.segments[static_cast<size_t>(nonFloor)]) == SegmentLineKind::LeftWall)
        {
            hitWall = true;
        } else {
            fighter.runoffSegment = floorSegmentIndex;
            fighter.runoffDirection = 1;
        }
    }

    if (!hitWall) {
        return false;
    }

    fighter.groundSegment = floorSegmentIndex;
    fighter.groundNormal = segmentNormal(floor);
    fighter.position.x = edge.x - fighter.ecb.points[3].x;
    fighter.position.y = edge.y - fighter.ecb.points[3].y;
    projectGroundVelocity(fighter);
    return true;
}

static bool isTeeterState(const World& world, const FighterRuntime& fighter) {
    const std::string& stateName = currentState(world, fighter).name;
    return stateName == "Ottotto" || stateName == "OttottoWait";
}

static bool snapToCurrentGround(const World& world, FighterRuntime& fighter) {
    return meleeMaintainCurrentFloor(world, fighter);
}

static Vec3 boneWorld(const FighterRuntime& fighter, BoneId bone, Vec3 offset = {}) {
    Vec3 base = fighter.bones[static_cast<size_t>(bone)].position;
    const Fix facing = fighter.facing >= 0 ? fx(1) : -fx(1);
    offset.x = fxMul(offset.x, facing);
    return {fighter.position.x + base.x + offset.x, fighter.position.y + base.y + offset.y, base.z + offset.z};
}

static Vec3 hitboxWorld(const FighterRuntime& fighter, const HitboxDefinition& hitbox) {
    if (hitbox.hsdBone >= 0 && static_cast<size_t>(hitbox.hsdBone) < fighter.hsdJointWorldTransforms.size()) {
        return transformPoint(fighter.hsdJointWorldTransforms[static_cast<size_t>(hitbox.hsdBone)], hitbox.offset);
    }
    return boneWorld(fighter, hitbox.bone, hitbox.offset);
}

static Vec3 shieldCenterWorld(const FighterDefinition& def, const FighterRuntime& fighter) {
    if (def.hasHsdAsset && def.hsdAsset) {
        const int shieldBone = def.hsdAsset->fighterBones.shield;
        if (shieldBone >= 0 && static_cast<size_t>(shieldBone) < fighter.hsdJointWorldTransforms.size()) {
            return transformPoint(fighter.hsdJointWorldTransforms[static_cast<size_t>(shieldBone)], {});
        }
    }
    return boneWorld(fighter, BoneId::Hip, {0, fxFromFloat(0.2f), 0});
}

static Fix smashChargeDamageScale(const FighterRuntime& fighter) {
    if (fighter.smashChargeState != 3 || fighter.smashChargeHoldFrames <= 0) {
        return fx(1);
    }
    const Fix chargeRatio = std::clamp(fxDiv(fighter.smashChargeFrames, fighter.smashChargeHoldFrames), Fix{0}, fx(1));
    return fx(1) + fxMul(fighter.smashChargeDamageMultiplier - fx(1), chargeRatio);
}

static void startSmashCharge(FighterRuntime& fighter, Fix holdFrames, Fix damageMultiplier) {
    fighter.smashChargeState = 1;
    fighter.smashChargeFrames = 0;
    fighter.smashChargeHoldFrames = holdFrames;
    fighter.smashChargeDamageMultiplier = damageMultiplier;
    fighter.smashChargeStoredAnimationRate = fighter.animationRate;
}

static void releaseSmashCharge(FighterRuntime& fighter) {
    fighter.smashChargeState = 3;
    fighter.animationRate = fighter.smashChargeStoredAnimationRate;
}

static void processSmashCharge(FighterRuntime& fighter) {
    if (fighter.smashChargeState == 1) {
        if (fighter.input.down(ButtonAttack)) {
            fighter.smashChargeState = 2;
            fighter.smashChargeStoredAnimationRate = fighter.animationRate;
            fighter.animationRate = 0;
        } else {
            fighter.smashChargeState = 0;
        }
        return;
    }
    if (fighter.smashChargeState != 2) {
        return;
    }
    if (!fighter.input.down(ButtonAttack)) {
        releaseSmashCharge(fighter);
        return;
    }
    fighter.smashChargeFrames += fx(1);
    if (fighter.smashChargeHoldFrames > 0 && fighter.smashChargeFrames >= fighter.smashChargeHoldFrames) {
        fighter.smashChargeFrames = fighter.smashChargeHoldFrames;
        releaseSmashCharge(fighter);
    }
}

static void executeSubaction(const FighterDefinition& def, FighterRuntime& fighter, const Subaction& sub) {
    if (sub.type == SubactionType::ClearHitboxes) {
        fighter.activeHitboxes.clear();
        fighter.fightersHitThisAction.clear();
        return;
    }
    if (sub.type == SubactionType::RemoveHitbox) {
        fighter.activeHitboxes.erase(
            std::remove_if(fighter.activeHitboxes.begin(), fighter.activeHitboxes.end(), [&](const ActiveHitbox& active) {
                return active.def.hitboxId == sub.hitbox.hitboxId;
            }),
            fighter.activeHitboxes.end());
        return;
    }
    if (sub.type == SubactionType::AdjustHitboxDamage) {
        for (ActiveHitbox& active : fighter.activeHitboxes) {
            if (active.def.hitboxId == sub.hitbox.hitboxId) {
                active.def.damage = sub.hitbox.damage;
            }
        }
        return;
    }
    if (sub.type == SubactionType::AdjustHitboxSize) {
        for (ActiveHitbox& active : fighter.activeHitboxes) {
            if (active.def.hitboxId == sub.hitbox.hitboxId) {
                active.def.radius = sub.hitbox.radius;
            }
        }
        return;
    }
    if (sub.type == SubactionType::SetInterruptible) {
        fighter.interruptibleFrame = sub.interruptibleFrame < 0 ? frameInState(fighter) : sub.interruptibleFrame;
        return;
    }
    if (sub.type == SubactionType::ReverseDirection) {
        fighter.facing *= -1;
        return;
    }
    if (sub.type == SubactionType::SetFlag) {
        setFighterCommandFlag(fighter, sub.flag, sub.flagValue);
        return;
    }
    if (sub.type == SubactionType::StartSmashCharge) {
        startSmashCharge(fighter, sub.smashChargeHoldFrames, sub.smashChargeDamageMultiplier);
        return;
    }
    if (sub.type == SubactionType::SetModelPartAnimation) {
        if (sub.modelPartIndex >= 0) {
            if (def.hsdAsset && fighter.hsdModelPartAnimations.size() != def.hsdAsset->modelPartAnimations.size()) {
                fighter.hsdModelPartAnimations.assign(def.hsdAsset->modelPartAnimations.size(), -1);
            }
            if (sub.modelPartIndex < static_cast<int>(fighter.hsdModelPartAnimations.size())) {
                fighter.hsdModelPartAnimations[static_cast<size_t>(sub.modelPartIndex)] = sub.modelPartAnimation;
            }
        }
        return;
    }
    if (sub.type == SubactionType::SetHurtboxState) {
        if (sub.hsdBone >= 0 && def.hsdAsset) {
            if (fighter.hurtboxStates.size() != def.hsdAsset->hurtboxes.size()) {
                fighter.hurtboxStates.assign(def.hsdAsset->hurtboxes.size(), HurtboxState::Normal);
            }
            for (size_t i = 0; i < def.hsdAsset->hurtboxes.size(); ++i) {
                if (def.hsdAsset->hurtboxes[i].bone == sub.hsdBone) {
                    fighter.hurtboxStates[i] = sub.hurtboxState;
                }
            }
            return;
        }
        if (sub.hurtboxIndex < 0) {
            std::fill(fighter.hurtboxStates.begin(), fighter.hurtboxStates.end(), sub.hurtboxState);
            return;
        }
        if (sub.hurtboxIndex < static_cast<int>(fighter.hurtboxStates.size())) {
            fighter.hurtboxStates[static_cast<size_t>(sub.hurtboxIndex)] = sub.hurtboxState;
        }
        return;
    }
    if (sub.type == SubactionType::CreateHitbox) {
        ActiveHitbox hitbox;
        hitbox.def = sub.hitbox;
        hitbox.def.damage = fxMul(hitbox.def.damage, smashChargeDamageScale(fighter));
        auto existing = std::find_if(fighter.activeHitboxes.begin(), fighter.activeHitboxes.end(), [&](const ActiveHitbox& active) {
            return active.def.hitboxId == hitbox.def.hitboxId;
        });
        if (existing != fighter.activeHitboxes.end()) {
            *existing = hitbox;
        } else {
            fighter.activeHitboxes.push_back(hitbox);
            std::sort(fighter.activeHitboxes.begin(), fighter.activeHitboxes.end(), [](const ActiveHitbox& a, const ActiveHitbox& b) {
                return a.def.hitboxId < b.def.hitboxId;
            });
        }
    }
}

static void executeActionFrame(const FighterDefinition& def, const FighterState& state, FighterRuntime& fighter, int actionFrame) {
    std::vector<Subaction> actionSource = state.action;
    if (def.hasHsdAsset && def.hsdAsset) {
        const int actionIndex = state.animationActionIndex >= 0 ? state.animationActionIndex : fallbackActionIndex(state.animation);
        if (const HsdActionScript* script = actionIndex >= 0 ? findActionScriptByActionIndex(*def.hsdAsset, actionIndex) : nullptr) {
            actionSource = decodeHsdActionScript(*def.hsdAsset, *script);
        }
    }
    const UnfoldedAction action = unfoldAction(actionSource);
    if (actionFrame < 0 || actionFrame >= static_cast<int>(action.size())) {
        return;
    }
    for (const Subaction& sub : action[static_cast<size_t>(actionFrame)]) {
        executeSubaction(def, fighter, sub);
    }
}

static int actionFrameForState(const FighterDefinition& def, const FighterState& state, const FighterRuntime& fighter) {
    if (def.hasHsdAsset && def.hsdAsset && clipForState(def, state)) {
        return std::max(0, static_cast<int>(fxToFloat(fighter.animationFrame)) + 1);
    }
    return frameInState(fighter);
}

static void executePendingActionFrames(const FighterDefinition& def, const FighterState& state, FighterRuntime& fighter) {
    const int targetFrame = actionFrameForState(def, state, fighter);
    if (targetFrame == fighter.lastActionFrameExecuted) {
        return;
    }
    if (targetFrame > fighter.lastActionFrameExecuted) {
        for (int frame = std::max(0, fighter.lastActionFrameExecuted + 1); frame <= targetFrame; ++frame) {
            executeActionFrame(def, state, fighter, frame);
        }
    } else {
        executeActionFrame(def, state, fighter, targetFrame);
    }
    fighter.lastActionFrameExecuted = targetFrame;
}

static void processInterrupts(World& world, FighterRuntime& fighter) {
    const FighterState& state = currentState(world, fighter);
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    for (const InterruptRule& rule : state.interrupts) {
        if (!ruleActive(rule, fighter) || !groundAllowed(rule.ground, fighter)) {
            continue;
        }
        if (conditionMet(world, rule.condition, fighter)) {
            if (rule.condition == InterruptCondition::DashInput && signOf(fighter.input.frames[0].move.x) != fighter.facing) {
                fighter.facing *= -1;
            }
            if (isSideSmashCondition(rule.condition)) {
                int desiredFacing = fighter.facing;
                float unusedAngle = 0.0f;
                if (sideSmashInput(fighter, def.properties.common, desiredFacing, unusedAngle)) {
                    fighter.facing = desiredFacing;
                }
            }
            if (state.name == "Dash" &&
                (rule.condition == InterruptCondition::ReverseDashInput ||
                 rule.condition == InterruptCondition::ShieldReflectInput ||
                 rule.condition == InterruptCondition::ShieldHeld))
            {
                fighter.groundVelocity -= fxMul(
                    fxMul(fighter.groundVelocity, def.properties.common.dashDecayX54),
                    groundFrictionMultiplier(world, fighter));
                projectGroundVelocity(fighter);
            }
            changeFighterState(world, fighter, rule.targetState, rule.lagFrames, rule.blendFrames);
            return;
        }
    }
}

static bool isShieldActiveState(const FighterState& state) {
    return state.name == "GuardOn" || state.name == "Guard" || state.name == "GuardSetOff" || state.name == "GuardReflect";
}

static bool blocksShieldRegen(const FighterState& state) {
    return isShieldActiveState(state) ||
        state.name == "ShieldBreakFly" ||
        state.name == "ShieldBreakFall" ||
        state.name == "ShieldBreakDown" ||
        state.name == "ShieldBreakStand";
}

static void regenerateShield(const World& world, FighterRuntime& fighter) {
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    if (blocksShieldRegen(currentState(world, fighter)) || fighter.shieldHealth >= def.shield.maxHealth) {
        return;
    }
    fighter.shieldHealth = std::min(def.shield.maxHealth, fighter.shieldHealth + def.properties.common.shieldRegenRateX27C);
}

static Fix shieldLightAmount(const FighterRuntime& fighter) {
    if (!fighter.input.down(ButtonShield)) {
        return 0;
    }
    return std::clamp(fighter.input.frames[0].shieldAnalog, Fix{0}, fx(1));
}

static Fix currentShieldRadius(const FighterDefinition& def, const FighterRuntime& fighter) {
    const MeleeCommonData& common = def.properties.common;
    if (!def.properties.shieldSizeScalesWithHealth) {
        return def.properties.initialShieldSize;
    }
    const Fix healthRatio = def.shield.maxHealth > 0 ? fxDiv(fighter.shieldHealth, def.shield.maxHealth) : 0;
    const Fix light = shieldLightAmount(fighter);
    const Fix sizeScale = common.hardShieldSizeScaleX2D4 +
        fxMul(light, common.lightShieldSizeScaleX2D8 - common.hardShieldSizeScaleX2D4);
    const Fix scaledHealth = fxMul(healthRatio, sizeScale);
    const Fix clampedScale = common.minShieldScaleX264 + fxMul(fx(1) - common.minShieldScaleX264, scaledHealth);
    return fxMul(def.shield.startSizeHardShield, clampedScale);
}

static bool isShieldSetoffState(const FighterState& state) {
    return state.name == "GuardSetOff";
}

static void applyShieldSdi(World& world, FighterRuntime& fighter, Fix scale) {
    if (!fighter.grounded || fxAbs(fighter.input.frames[0].move.x) < world.fighterDefs[static_cast<size_t>(fighter.fighterDef)].properties.common.sdiMinStickMagX4B0) {
        return;
    }
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const Fix amount = fxMul(def.properties.common.shieldSdiDistanceX4C0, fxMul(fighter.input.frames[0].move.x, scale));
    const Vec2 tangent = groundTangent(fighter.groundNormal);
    fighter.position.x += fxMul(tangent.x, amount);
    fighter.position.y += fxMul(tangent.y, amount);
    calculateEcb(def, fighter, false);
}

static void processShieldHitlagSdi(World& world, FighterRuntime& fighter) {
    if (!isShieldSetoffState(currentState(world, fighter))) {
        return;
    }
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    if (fighter.stickXTiltTimer < def.properties.common.sdiStickWindowX4B4) {
        applyShieldSdi(world, fighter, def.properties.common.sdiPosScaleX4B8);
        fighter.stickXTiltTimer = 254;
    }
}

static int collisionSubstepCount(const FighterRuntime& fighter, Vec2 attemptedDelta) {
    Fix maxDistance = std::max(fxAbs(attemptedDelta.x), fxAbs(attemptedDelta.y));
    for (size_t i = 0; i < fighter.ecb.points.size(); ++i) {
        maxDistance = std::max(maxDistance, fxAbs(fighter.desiredEcb.points[i].x - fighter.ecb.points[i].x));
        maxDistance = std::max(maxDistance, fxAbs(fighter.desiredEcb.points[i].y - fighter.ecb.points[i].y));
    }
    constexpr Fix maxStep = fx(6);
    if (maxDistance <= maxStep) {
        return 1;
    }
    return std::max(1, static_cast<int>((maxDistance + maxStep - 1) / maxStep));
}

static Vec2 collisionSubstepDelta(Vec2 totalDelta, Vec2 consumedDelta, int step, int steps) {
    if (step + 1 == steps) {
        return totalDelta - consumedDelta;
    }
    return {totalDelta.x / steps, totalDelta.y / steps};
}

static bool shouldStartTeeterFromRunoff(
    const World& world,
    const FighterRuntime& fighter,
    const StageSegment& previousSegment,
    Vec2 previousBottom,
    Vec2 attemptedDelta,
    int side)
{
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const Fix edgeX = side < 0 ? segmentMinX(previousSegment) : segmentMaxX(previousSegment);
    const Fix epsilon = fxFromFloat(0.001f);
    const bool alreadyAtEdge = side < 0
        ? previousBottom.x <= edgeX + epsilon
        : previousBottom.x >= edgeX - epsilon;
    const bool movingOutward = attemptedDelta.x * side > 0 || fighter.groundVelocity * side > 0;
    const bool stickOutward = fighter.input.frames[0].move.x * side >= def.properties.common.walkInputThresholdX24;
    return !(alreadyAtEdge && movingOutward && stickOutward);
}

static bool collideCurrentStep(World& world, size_t fighterIndex, bool wasGrounded, int previousGroundSegment, Vec2 attemptedDelta) {
    FighterRuntime& fighter = world.fighters[fighterIndex];
    bool nowGrounded = false;
    int landedSegment = -1;
    Vec2 landingContact = {};
    Fix landingFraction = fx(1);
    bool resolvedAirCollision = false;
    bool hitCeilingThisStep = false;

    if (wasGrounded) {
        nowGrounded = snapToCurrentGround(world, fighter);
        landedSegment = nowGrounded ? fighter.groundSegment : -1;
        if (!nowGrounded && previousGroundSegment >= 0 &&
            previousGroundSegment < static_cast<int>(world.stage.segments.size()) &&
            fighter.floorSkipSegment != previousGroundSegment)
        {
            const StageSegment& previousSegment = world.stage.segments[static_cast<size_t>(previousGroundSegment)];
            const Fix bottomX = fighter.position.x + fighter.ecb.points[3].x;
            const Vec2 previousBottom = fighter.previousPosition + fighter.previousEcb.points[3];
            if (bottomX < segmentMinX(previousSegment) &&
                shouldStartTeeterFromRunoff(world, fighter, previousSegment, previousBottom, attemptedDelta, -1))
            {
                fighter.runoffSegment = previousGroundSegment;
                fighter.runoffDirection = -1;
            } else if (bottomX > segmentMaxX(previousSegment) &&
                       shouldStartTeeterFromRunoff(world, fighter, previousSegment, previousBottom, attemptedDelta, 1))
            {
                fighter.runoffSegment = previousGroundSegment;
                fighter.runoffDirection = 1;
            }
        }
    }

    if (!wasGrounded && !nowGrounded) {
        hitCeilingThisStep = resolveWallAndCeiling(world, fighter, attemptedDelta);
        resolvedAirCollision = true;
    }

    if (!nowGrounded && !hitCeilingThisStep) {
        const Vec2 previousBottom = fighter.previousPosition + fighter.previousEcb.points[3];
        const Vec2 currentBottom = fighter.position + fighter.ecb.points[3];
        const int landingSkip = wasGrounded ? previousGroundSegment : -1;
        landedSegment = findLandingSegment(world, fighter, previousBottom, currentBottom, landingContact, landingFraction, landingSkip);
        if (landedSegment >= 0 && fighter.fighterVelocity.y <= fxFromFloat(0.2f)) {
            const StageSegment& segment = world.stage.segments[static_cast<size_t>(landedSegment)];
            fighter.lastLandingVelocityY = fighter.fighterVelocity.y;
            fighter.groundNormal = segmentNormal(segment);
            fighter.groundVelocity = velocityAlongGround(fighter.fighterVelocity, fighter.groundNormal);
            fighter.position.x = landingContact.x - fighter.ecb.points[3].x;
            fighter.position.y = landingContact.y - fighter.ecb.points[3].y;
            landedSegment = carryRemainingMovementOnGround(world, fighter, landedSegment, landingFraction, attemptedDelta);
            projectGroundVelocity(fighter);
            nowGrounded = true;
        }
    }

    if (nowGrounded) {
        fighter.grounded = true;
        fighter.groundSegment = landedSegment;
        fighter.wallContactTimer = 254;
        fighter.wallContactSide = 0;
        fighter.wallContactSegment = -1;
        fighter.wallJumpsUsed = 0;
        fighter.grabbedLedge = -1;
        if (!wasGrounded) {
            unlockFighterEcb(fighter);
            fighter.platformDropTimer = 0;
            fighter.jumpsUsed = 0;
            refreshEcbMetadata(fighter.ecb, fighter);
            runStateFunctions(world, fighterIndex, currentState(world, fighter).onLanding);
        }
    } else {
        const bool grabbedLedge = tryGrabLedge(world, fighterIndex);
        fighter.grounded = false;
        fighter.groundSegment = -1;
        fighter.groundVelocity = velocityAlongGround(fighter.fighterVelocity, fighter.groundNormal);
        if (grabbedLedge) {
            if (wasGrounded) {
                runStateFunctions(world, fighterIndex, currentState(world, fighter).onAirborne);
            }
        } else if (wasGrounded) {
            runStateFunctions(world, fighterIndex, currentState(world, fighter).onAirborne);
        }
    }

    if (!resolvedAirCollision) {
        resolveWallAndCeiling(world, fighter, attemptedDelta);
    }

    refreshEcbMetadata(fighter.ecb, fighter);
    return hitCeilingThisStep;
}

static void integrateAndCollide(World& world, size_t fighterIndex) {
    FighterRuntime& fighter = world.fighters[fighterIndex];
    fighter.runoffSegment = -1;
    fighter.runoffDirection = 0;
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    if (fighter.ecbLockTimer > 0) {
        --fighter.ecbLockTimer;
        if (fighter.ecbLockTimer == 0) {
            unlockFighterEcb(fighter);
        }
    }
    loadDesiredEcb(def, fighter);
    updateFloorSkip(world, fighter);

    if (fighter.grounded) {
        consumeGroundAcceleration(world, fighter);
    } else {
        fighter.groundAccel = 0;
        fighter.groundAccelSecondary = 0;
    }

    updateAttackerShieldKnockback(world, fighter);

    const Vec2 attemptedDelta = fighter.fighterVelocity + fighter.knockbackVelocity + fighter.attackerShieldKnockback;
    const int steps = collisionSubstepCount(fighter, attemptedDelta);
    Vec2 consumedDelta{};
    for (int step = 0; step < steps; ++step) {
        const bool wasGrounded = fighter.grounded;
        const int previousGroundSegment = fighter.groundSegment;
        fighter.previousPosition = fighter.position;
        interpolateEcb(fighter, fxDiv(fx(1), fx(steps - step)));
        const Vec2 stepDelta = collisionSubstepDelta(attemptedDelta, consumedDelta, step, steps);
        consumedDelta += stepDelta;
        fighter.position += stepDelta;
        refreshEcbMetadata(fighter.ecb, fighter);
        const bool hitCeiling = collideCurrentStep(world, fighterIndex, wasGrounded, previousGroundSegment, stepDelta);
        if (hitCeiling || (fighter.grounded && isTeeterState(world, fighter))) {
            break;
        }
    }

    if (fighter.ledgeCooldown > 0) {
        --fighter.ledgeCooldown;
    }
    if (fighter.wallContactSide == 0) {
        fighter.wallContactTimer = incrementTiltTimer(fighter.wallContactTimer);
    }
    fighter.knockbackVelocity.x = fxApproach(fighter.knockbackVelocity.x, 0, fxFromFloat(0.051f));
    fighter.knockbackVelocity.y = fxApproach(fighter.knockbackVelocity.y, 0, fxFromFloat(0.051f));
}

static Fix calculateKnockback(const HitboxDefinition& hitbox, const FighterDefinition& victimDef, const FighterRuntime& victim) {
    const Fix weight = victimDef.properties.weight;
    const Fix weightScale = fxDiv(fx(200), weight + fx(100));
    Fix knockback = 0;
    if (hitbox.knockbackWeightSet > 0) {
        const Fix setTerm = fxMul(hitbox.knockbackWeightSet, fxFromFloat(0.5f)) + fx(1);
        knockback = fxMul(setTerm, fxMul(fxFromFloat(1.4f), fx(1))) + fx(18);
    } else {
        const Fix percent = fx(static_cast<int>(fxToFloat(victim.percent)));
        const Fix damagePercent = hitbox.damage + percent;
        const Fix damageTerm = fxMul(fxMul(hitbox.damage, damagePercent), fxFromFloat(0.05f)) +
            fxMul(damagePercent, fxFromFloat(0.1f));
        knockback = fxMul(damageTerm, fxMul(fxFromFloat(1.4f), weightScale)) + fx(18);
    }
    knockback = fxMul(knockback, fxMul(hitbox.knockbackGrowth, fxFromFloat(0.01f))) + hitbox.knockbackBase;
    return std::min(knockback, fx(2500));
}

static Fix launchAngleDegrees(const FighterRuntime& victim, const HitboxDefinition& hitbox, Fix knockback, int side) {
    Fix angle = hitbox.knockbackAngleDegrees;
    if (angle == fx(361)) {
        if (knockback < fxFromFloat(32.1f)) {
            angle = side >= 0 ? fx(0) : fx(180);
        } else {
            angle = side >= 0 ? fx(44) : fx(136);
        }
    } else if (side < 0) {
        angle = fx(180) - angle;
        if (angle < 0) {
            angle += fx(360);
        }
    }
    if (knockback < fx(80) && victim.grounded &&
        (hitbox.knockbackAngleDegrees == fx(0) || hitbox.knockbackAngleDegrees == fx(180)))
    {
        angle = side >= 0 ? fx(0) : fx(180);
    }
    return angle;
}

static void applyHit(World& world, FighterRuntime& attacker, FighterRuntime& victim, const HitboxDefinition& hitbox) {
    const FighterDefinition& victimDef = world.fighterDefs[static_cast<size_t>(victim.fighterDef)];
    const Fix kb = calculateKnockback(hitbox, victimDef, victim);
    const int side = victim.position.x >= attacker.position.x ? 1 : -1;
    const float angle = fxToFloat(launchAngleDegrees(victim, hitbox, kb, side)) * 3.14159265f / 180.0f;
    victim.percent += hitbox.damage;
    victim.knockbackVelocity.x = fxFromFloat(std::cos(angle) * fxToFloat(kb) * 0.03f);
    victim.knockbackVelocity.y = fxFromFloat(std::sin(angle) * fxToFloat(kb) * 0.03f);
    if (kb < fx(80) && victim.grounded &&
        (hitbox.knockbackAngleDegrees == fx(0) || hitbox.knockbackAngleDegrees == fx(180)))
    {
        victim.knockbackVelocity.y = 0;
    }
    victim.hitlag = std::max(3, static_cast<int>(fxToFloat(hitbox.damage) / 3.0f) + 3);
    attacker.hitlag = victim.hitlag;
    victim.hitstun = std::max(1, static_cast<int>(fxToFloat(kb) * 0.4f));
    victim.grounded = false;
    changeFighterState(world, victim, "Fall");
}

static void applyInvincibleHit(FighterRuntime& attacker, FighterRuntime& victim, const HitboxDefinition& hitbox) {
    const int hitlagFrames = std::max(3, static_cast<int>(fxToFloat(hitbox.damage) / 3.0f) + 3);
    attacker.hitlag = std::max(attacker.hitlag, hitlagFrames);
    victim.hitlag = std::max(victim.hitlag, hitlagFrames);
}

static int shieldSetoffFrames(const FighterProperties& attr, Fix shieldDamage, Fix light) {
    const MeleeCommonData& common = attr.common;
    const Fix setoffLightScale = common.hardShieldSetoffScaleX2E4 +
        fxMul(light, common.lightShieldSetoffScaleX2E8 - common.hardShieldSetoffScaleX2E4);
    const Fix scaled = fxMul(common.shieldSetoffScaleX28C, fxMul(shieldDamage, fx(1) - setoffLightScale));
    return std::max(1, static_cast<int>(fxToFloat(scaled + common.shieldSetoffBaseX290) + 0.5f));
}

static void applyShieldHit(World& world, FighterRuntime& attacker, FighterRuntime& defender, const HitboxDefinition& hitbox) {
    const FighterDefinition& defenderDef = world.fighterDefs[static_cast<size_t>(defender.fighterDef)];
    const FighterProperties& attr = defenderDef.properties;
    const Fix light = shieldLightAmount(defender);
    const Fix baseShieldDamage = std::max(Fix{0}, hitbox.damage + hitbox.damageShield);
    const Fix damageLightScale = attr.common.hardShieldDamageScaleX2DC +
        fxMul(light, attr.common.lightShieldDamageScaleX2E0 - attr.common.hardShieldDamageScaleX2DC);
    const Fix healthDamage = fxMul(attr.common.shieldDamageScaleX284, fxMul(baseShieldDamage, fx(1) - damageLightScale)) +
        attr.common.shieldDamageBaseX288;

    defender.shieldHealth = std::max(Fix{0}, defender.shieldHealth - healthDamage);
    const int hitlagFrames = std::max(3, static_cast<int>(fxToFloat(hitbox.damage) / 3.0f) + 3);
    attacker.hitlag = hitlagFrames;
    defender.hitlag = hitlagFrames;

    if (defender.shieldHealth <= 0) {
        changeFighterState(world, defender, "ShieldBreakFly");
        return;
    }

    const int side = defender.position.x >= attacker.position.x ? 1 : -1;
    const int setoff = shieldSetoffFrames(attr, baseShieldDamage, light);
    const Fix attackerPushback = fxMul(baseShieldDamage, attr.common.attackerShieldPushbackScaleX3E0) +
        attr.common.attackerShieldPushbackBaseX3E4;
    if (attacker.grounded) {
        attacker.groundAttackerShieldKnockbackVelocity = -side * attackerPushback;
        projectGroundAttackerShieldKnockback(attacker);
    } else {
        attacker.groundAttackerShieldKnockbackVelocity = 0;
        attacker.attackerShieldKnockback.x = -side * attackerPushback;
        attacker.attackerShieldKnockback.y = 0;
    }
    defender.guardSetoffTimer = setoff;
    defender.guardMinHoldTimer = 0;
    defender.guardReleaseQueued = false;
    defender.groundVelocity = std::clamp(
        side * fxMul(baseShieldDamage, attr.common.shieldPushbackScaleX294),
        -attr.common.shieldPushbackMaxX298,
        attr.common.shieldPushbackMaxX298);
    changeFighterState(world, defender, "GuardSetOff");
    defender.guardSetoffTimer = setoff;
    defender.guardReleaseQueued = false;
}

static void updateAndCheckHitboxes(World& world, size_t attackerIndex) {
    FighterRuntime& attacker = world.fighters[attackerIndex];
    for (ActiveHitbox& hitbox : attacker.activeHitboxes) {
        const Vec3 worldPos = hitboxWorld(attacker, hitbox.def);
        if (hitbox.firstFrame) {
            hitbox.firstFrame = false;
            hitbox.current = worldPos;
            hitbox.previous = worldPos;
        } else {
            hitbox.previous = hitbox.current;
            hitbox.current = worldPos;
        }
        for (size_t victimIndex = 0; victimIndex < world.fighters.size(); ++victimIndex) {
            if (victimIndex == attackerIndex) {
                continue;
            }
            if (std::find(attacker.fightersHitThisAction.begin(), attacker.fightersHitThisAction.end(), static_cast<int>(victimIndex)) != attacker.fightersHitThisAction.end()) {
                continue;
            }
            FighterRuntime& victim = world.fighters[victimIndex];
            const FighterDefinition& victimDef = world.fighterDefs[static_cast<size_t>(victim.fighterDef)];
            if ((victim.grounded && !hitbox.def.hitGrounded) || (!victim.grounded && !hitbox.def.hitAirborne)) {
                continue;
            }
            const bool useImportedHurtboxes = victimDef.hasHsdAsset && victimDef.hsdAsset && !victim.hsdHurtboxCapsules.empty();
            const size_t hurtboxCount = useImportedHurtboxes ? victim.hsdHurtboxCapsules.size() : victimDef.hurtboxes.size();
            if (victim.hurtboxStates.size() != hurtboxCount) {
                HurtboxState fillState = HurtboxState::Normal;
                if (!victim.hurtboxStates.empty() &&
                    std::all_of(victim.hurtboxStates.begin(), victim.hurtboxStates.end(), [&](HurtboxState state) {
                        return state == victim.hurtboxStates.front();
                    })) {
                    fillState = victim.hurtboxStates.front();
                }
                victim.hurtboxStates.assign(hurtboxCount, fillState);
            }
            Capsule hitCapsule{hitbox.previous, hitbox.current, hitbox.def.radius};
            if (isShieldActiveState(currentState(world, victim)) && victim.shieldHealth > 0) {
                const Vec3 center = shieldCenterWorld(victimDef, victim);
                const Fix radius = currentShieldRadius(victimDef, victim);
                if (capsuleCapsule(hitCapsule, {center, center, radius})) {
                    attacker.fightersHitThisAction.push_back(static_cast<int>(victimIndex));
                    applyShieldHit(world, attacker, victim, hitbox.def);
                    continue;
                }
            }
            if (useImportedHurtboxes) {
                for (size_t hurtboxIndex = 0; hurtboxIndex < victim.hsdHurtboxCapsules.size(); ++hurtboxIndex) {
                    const HurtboxState state = hurtboxIndex < victim.hurtboxStates.size() ? victim.hurtboxStates[hurtboxIndex] : HurtboxState::Normal;
                    if (state == HurtboxState::Intangible) {
                        continue;
                    }
                    if (capsuleCapsule(hitCapsule, victim.hsdHurtboxCapsules[hurtboxIndex])) {
                        attacker.fightersHitThisAction.push_back(static_cast<int>(victimIndex));
                        if (state == HurtboxState::Invincible) {
                            applyInvincibleHit(attacker, victim, hitbox.def);
                        } else {
                            applyHit(world, attacker, victim, hitbox.def);
                        }
                        break;
                    }
                }
                continue;
            }
            for (size_t hurtboxIndex = 0; hurtboxIndex < victimDef.hurtboxes.size(); ++hurtboxIndex) {
                const HurtboxDefinition& hurtbox = victimDef.hurtboxes[hurtboxIndex];
                const HurtboxState state = victim.hurtboxStates[hurtboxIndex] == HurtboxState::Normal ? hurtbox.state : victim.hurtboxStates[hurtboxIndex];
                if (state == HurtboxState::Intangible) {
                    continue;
                }
                Capsule hurtCapsule{boneWorld(victim, hurtbox.bone, hurtbox.startOffset), boneWorld(victim, hurtbox.bone, hurtbox.endOffset), hurtbox.radius};
                if (capsuleCapsule(hitCapsule, hurtCapsule)) {
                    attacker.fightersHitThisAction.push_back(static_cast<int>(victimIndex));
                    if (state == HurtboxState::Invincible) {
                        applyInvincibleHit(attacker, victim, hitbox.def);
                    } else {
                        applyHit(world, attacker, victim, hitbox.def);
                    }
                    break;
                }
            }
        }
    }
}

void tickWorld(World& world, const std::vector<InputFrame>& inputs) {
    for (size_t i = 0; i < world.fighters.size(); ++i) {
        world.fighters[i].input.push(i < inputs.size() ? inputs[i] : InputFrame{});
        updateStickTiltTimers(world, world.fighters[i]);
    }

    for (size_t fighterIndex = 0; fighterIndex < world.fighters.size(); ++fighterIndex) {
        FighterRuntime& fighter = world.fighters[fighterIndex];
        if (fighter.hitlag > 0) {
            processShieldHitlagSdi(world, fighter);
            --fighter.hitlag;
            if (fighter.hitlag == 0 && isShieldSetoffState(currentState(world, fighter))) {
                const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
                applyShieldSdi(world, fighter, def.properties.common.shieldAsdiPosScaleX4BC);
            }
            continue;
        }
        ++fighter.internalFrame;
        if (fighter.hitstun > 0) {
            --fighter.hitstun;
        }

        const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
        executePendingActionFrames(def, currentState(world, fighter), fighter);
        processSmashCharge(fighter);
        processInterrupts(world, fighter);
        runStateFunctions(world, fighterIndex, currentState(world, fighter).onFrame);
        evaluatePose(def, currentState(world, fighter), fighter);
        applyAnimationGroundVelocity(currentState(world, fighter), fighter);
        integrateAndCollide(world, fighterIndex);

        const FighterState& stateAfterPhysics = currentState(world, fighter);
        const int animationLengthFrames = fighter.stateAnimationLengthOverride > 0
            ? fighter.stateAnimationLengthOverride
            : stateAfterPhysics.animationLengthFrames;
        if (frameInState(fighter) > animationLengthFrames && !stateAfterPhysics.onAnimationFinishedState.empty()) {
            changeFighterState(world, fighter, stateAfterPhysics.onAnimationFinishedState, 0, stateAfterPhysics.onAnimationFinishedBlendFrames);
        }
        advanceAnimationFrame(def, currentState(world, fighter), fighter);
        regenerateShield(world, fighter);
    }

    for (size_t i = 0; i < world.fighters.size(); ++i) {
        updateAndCheckHitboxes(world, i);
    }

    ++world.frame;
}

WorldSnapshot saveWorld(const World& world) {
    WorldSnapshot snapshot;
    snapshot.frame = world.frame;
    for (const FighterRuntime& fighter : world.fighters) {
        FighterSnapshot item;
        item.fighterDef = fighter.fighterDef;
        item.state = fighter.state;
        item.lastStateChangeFrame = fighter.lastStateChangeFrame;
        item.internalFrame = fighter.internalFrame;
        item.interruptibleFrame = fighter.interruptibleFrame;
        item.stateAnimationLengthOverride = fighter.stateAnimationLengthOverride;
        item.animationFrame = fighter.animationFrame;
        item.animationRate = fighter.animationRate;
        item.lastActionFrameExecuted = fighter.lastActionFrameExecuted;
        item.runAnimationVelocity = fighter.runAnimationVelocity;
        item.facing = fighter.facing;
        item.hsdPoseFacing = fighter.hsdPoseFacing;
        item.jumpsUsed = fighter.jumpsUsed;
        item.grounded = fighter.grounded;
        item.percent = fighter.percent;
        item.shieldHealth = fighter.shieldHealth;
        item.position = fighter.position;
        item.previousPosition = fighter.previousPosition;
        item.fighterVelocity = fighter.fighterVelocity;
        item.knockbackVelocity = fighter.knockbackVelocity;
        item.attackerShieldKnockback = fighter.attackerShieldKnockback;
        item.groundVelocity = fighter.groundVelocity;
        item.groundAttackerShieldKnockbackVelocity = fighter.groundAttackerShieldKnockbackVelocity;
        item.lastLandingVelocityY = fighter.lastLandingVelocityY;
        item.groundAccel = fighter.groundAccel;
        item.groundAccelSecondary = fighter.groundAccelSecondary;
        item.groundNormal = fighter.groundNormal;
        item.hitlag = fighter.hitlag;
        item.hitstun = fighter.hitstun;
        item.stateFlags = fighter.stateFlags;
        item.commandFlags = fighter.commandFlags;
        item.ecb = fighter.ecb;
        item.desiredEcb = fighter.desiredEcb;
        item.previousEcb = fighter.previousEcb;
        item.ecbLockTimer = fighter.ecbLockTimer;
        item.ecbLockBottom = fighter.ecbLockBottom;
        item.groundSegment = fighter.groundSegment;
        item.floorSkipSegment = fighter.floorSkipSegment;
        item.platformDropTimer = fighter.platformDropTimer;
        item.grabbedLedge = fighter.grabbedLedge;
        item.ledgeCooldown = fighter.ledgeCooldown;
        item.ledgeActionReady = fighter.ledgeActionReady;
        item.wallContactSide = fighter.wallContactSide;
        item.wallContactSegment = fighter.wallContactSegment;
        item.wallContactTimer = fighter.wallContactTimer;
        item.wallJumpsUsed = fighter.wallJumpsUsed;
        item.turnFramesToChangeDirection = fighter.turnFramesToChangeDirection;
        item.turnRunInitialFacing = fighter.turnRunInitialFacing;
        item.turnFacingAfter = fighter.turnFacingAfter;
        item.turnHasTurned = fighter.turnHasTurned;
        item.turnJustTurned = fighter.turnJustTurned;
        item.turnDashBuffered = fighter.turnDashBuffered;
        item.runDirectTimer = fighter.runDirectTimer;
        item.runBrakeTimer = fighter.runBrakeTimer;
        item.runBrakeAnimationFrozen = fighter.runBrakeAnimationFrozen;
        item.guardMinHoldTimer = fighter.guardMinHoldTimer;
        item.guardSetoffTimer = fighter.guardSetoffTimer;
        item.guardReleaseQueued = fighter.guardReleaseQueued;
        item.guardPoseFrame = fighter.guardPoseFrame;
        item.guardPoseBlend = fighter.guardPoseBlend;
        item.smashChargeState = fighter.smashChargeState;
        item.smashChargeFrames = fighter.smashChargeFrames;
        item.smashChargeHoldFrames = fighter.smashChargeHoldFrames;
        item.smashChargeDamageMultiplier = fighter.smashChargeDamageMultiplier;
        item.smashChargeStoredAnimationRate = fighter.smashChargeStoredAnimationRate;
        item.stickXTiltTimer = fighter.stickXTiltTimer;
        item.stickYTiltTimer = fighter.stickYTiltTimer;
        item.input = fighter.input;
        item.bones = fighter.bones;
        item.hsdPose = fighter.hsdPose;
        item.hsdBlendFromPose = fighter.hsdBlendFromPose;
        item.hsdBlendFrames = fighter.hsdBlendFrames;
        item.hsdBlendElapsed = fighter.hsdBlendElapsed;
        item.hsdTransN = fighter.hsdTransN;
        item.previousHsdTransN = fighter.previousHsdTransN;
        item.hsdTransNOffset = fighter.hsdTransNOffset;
        item.hsdJointWorldTransforms = fighter.hsdJointWorldTransforms;
        item.hsdJointWorldPositions = fighter.hsdJointWorldPositions;
        item.hsdHurtboxCapsules = fighter.hsdHurtboxCapsules;
        item.hsdModelPartAnimations = fighter.hsdModelPartAnimations;
        item.hurtboxStates = fighter.hurtboxStates;
        item.activeHitboxes = fighter.activeHitboxes;
        item.fightersHitThisAction = fighter.fightersHitThisAction;
        snapshot.fighters.push_back(item);
    }
    return snapshot;
}

void loadWorld(World& world, const WorldSnapshot& snapshot) {
    world.frame = snapshot.frame;
    world.fighters.clear();
    for (const FighterSnapshot& item : snapshot.fighters) {
        FighterRuntime fighter;
        fighter.fighterDef = item.fighterDef;
        fighter.state = item.state;
        fighter.lastStateChangeFrame = item.lastStateChangeFrame;
        fighter.internalFrame = item.internalFrame;
        fighter.interruptibleFrame = item.interruptibleFrame;
        fighter.stateAnimationLengthOverride = item.stateAnimationLengthOverride;
        fighter.animationFrame = item.animationFrame;
        fighter.animationRate = item.animationRate;
        fighter.lastActionFrameExecuted = item.lastActionFrameExecuted;
        fighter.runAnimationVelocity = item.runAnimationVelocity;
        fighter.facing = item.facing;
        fighter.hsdPoseFacing = item.hsdPoseFacing == 0 ? item.facing : item.hsdPoseFacing;
        fighter.jumpsUsed = item.jumpsUsed;
        fighter.grounded = item.grounded;
        fighter.percent = item.percent;
        fighter.shieldHealth = item.shieldHealth;
        fighter.position = item.position;
        fighter.previousPosition = item.previousPosition;
        fighter.fighterVelocity = item.fighterVelocity;
        fighter.knockbackVelocity = item.knockbackVelocity;
        fighter.attackerShieldKnockback = item.attackerShieldKnockback;
        fighter.groundVelocity = item.groundVelocity;
        fighter.groundAttackerShieldKnockbackVelocity = item.groundAttackerShieldKnockbackVelocity;
        fighter.lastLandingVelocityY = item.lastLandingVelocityY;
        fighter.groundAccel = item.groundAccel;
        fighter.groundAccelSecondary = item.groundAccelSecondary;
        fighter.groundNormal = item.groundNormal;
        fighter.hitlag = item.hitlag;
        fighter.hitstun = item.hitstun;
        fighter.stateFlags = item.stateFlags;
        fighter.commandFlags = item.commandFlags;
        fighter.ecb = item.ecb;
        fighter.desiredEcb = item.desiredEcb;
        fighter.previousEcb = item.previousEcb;
        fighter.ecbLockTimer = item.ecbLockTimer;
        fighter.ecbLockBottom = item.ecbLockBottom;
        fighter.groundSegment = item.groundSegment;
        fighter.floorSkipSegment = item.floorSkipSegment;
        fighter.platformDropTimer = item.platformDropTimer;
        fighter.grabbedLedge = item.grabbedLedge;
        fighter.ledgeCooldown = item.ledgeCooldown;
        fighter.ledgeActionReady = item.ledgeActionReady;
        fighter.wallContactSide = item.wallContactSide;
        fighter.wallContactSegment = item.wallContactSegment;
        fighter.wallContactTimer = item.wallContactTimer;
        fighter.wallJumpsUsed = item.wallJumpsUsed;
        fighter.turnFramesToChangeDirection = item.turnFramesToChangeDirection;
        fighter.turnRunInitialFacing = item.turnRunInitialFacing;
        fighter.turnFacingAfter = item.turnFacingAfter;
        fighter.turnHasTurned = item.turnHasTurned;
        fighter.turnJustTurned = item.turnJustTurned;
        fighter.turnDashBuffered = item.turnDashBuffered;
        fighter.runDirectTimer = item.runDirectTimer;
        fighter.runBrakeTimer = item.runBrakeTimer;
        fighter.runBrakeAnimationFrozen = item.runBrakeAnimationFrozen;
        fighter.guardMinHoldTimer = item.guardMinHoldTimer;
        fighter.guardSetoffTimer = item.guardSetoffTimer;
        fighter.guardReleaseQueued = item.guardReleaseQueued;
        fighter.guardPoseFrame = item.guardPoseFrame;
        fighter.guardPoseBlend = item.guardPoseBlend;
        fighter.smashChargeState = item.smashChargeState;
        fighter.smashChargeFrames = item.smashChargeFrames;
        fighter.smashChargeHoldFrames = item.smashChargeHoldFrames;
        fighter.smashChargeDamageMultiplier = item.smashChargeDamageMultiplier;
        fighter.smashChargeStoredAnimationRate = item.smashChargeStoredAnimationRate;
        fighter.stickXTiltTimer = item.stickXTiltTimer;
        fighter.stickYTiltTimer = item.stickYTiltTimer;
        fighter.input = item.input;
        fighter.bones = item.bones;
        fighter.hsdPose = item.hsdPose;
        fighter.hsdBlendFromPose = item.hsdBlendFromPose;
        fighter.hsdBlendFrames = item.hsdBlendFrames;
        fighter.hsdBlendElapsed = item.hsdBlendElapsed;
        fighter.hsdTransN = item.hsdTransN;
        fighter.previousHsdTransN = item.previousHsdTransN;
        fighter.hsdTransNOffset = item.hsdTransNOffset;
        fighter.hsdJointWorldTransforms = item.hsdJointWorldTransforms;
        fighter.hsdJointWorldPositions = item.hsdJointWorldPositions;
        fighter.hsdHurtboxCapsules = item.hsdHurtboxCapsules;
        fighter.hsdModelPartAnimations = item.hsdModelPartAnimations;
        fighter.hurtboxStates = item.hurtboxStates;
        fighter.activeHitboxes = item.activeHitboxes;
        fighter.fightersHitThisAction = item.fightersHitThisAction;
        world.fighters.push_back(fighter);
    }
}

} // namespace pf
