#include "core/simulation.hpp"

#include "core/fighter_package.hpp"
#include "core/state_functions.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace pf {

static void evaluatePose(const FighterDefinition& def, const FighterState& state, FighterRuntime& fighter);
static void updateAndCheckHitboxes(World& world, size_t attackerIndex);
static bool segmentYAtX(const StageSegment& segment, Fix x, Fix& y);
static int fallbackActionIndex(const std::string& animation);
static int commonPartBone(const FighterDefinition& def, const FighterRuntime& fighter, int commonPart);
static Vec3 boneWorld(const FighterRuntime& fighter, BoneId bone, Vec3 offset = {});
static Fix vectorMagnitude(Vec2 value);

enum class GameObjectEvent : uint8_t {
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
    Touched,
    JumpedOn,
    GrabDealt,
    GrabbedForVictim,
    Interaction,
};

static void runGameObjectEvent(World& world, size_t objectIndex, GameObjectEvent event);
static void deactivateGameObject(World& world, size_t objectIndex);
static bool validGameObjectIndex(const World& world, int objectIndex);
static const GameObjectStateDefinition* currentGameObjectState(const World& world, const GameObjectRuntime& object);
static void runGameObjectFunctions(World& world, size_t objectIndex, const std::vector<FunctionCall>& functions);
static void setGameObjectHitlag(World& world, size_t objectIndex, int hitlagFrames);
static int hitlagFramesForHitbox(const HitboxDefinition& hitbox);
static void applyGameObjectGrab(World& world, size_t objectIndex, size_t victimIndex);
static void clearGameObjectFighterReference(World& world, size_t objectIndex, int fighterIndex);

struct NativeRosterFighterSpec {
    const char* displayName;
};

static const std::array<NativeRosterFighterSpec, 27>& nativeTrainingRoster() {
    static const std::array<NativeRosterFighterSpec, 27> roster{{
        {"Mario"},
        {"Donkey Kong"},
        {"Link"},
        {"Samus"},
        {"Yoshi"},
        {"Kirby"},
        {"Fox"},
        {"Pikachu"},
        {"Luigi"},
        {"Captain Falcon"},
        {"Ness"},
        {"Bowser"},
        {"Peach"},
        {"Zelda"},
        {"Sheik"},
        {"Ice Climbers"},
        {"Marth"},
        {"Game & Watch"},
        {"Falco"},
        {"Ganondorf"},
        {"Young Link"},
        {"Dr. Mario"},
        {"Roy"},
        {"Pichu"},
        {"Mewtwo"},
        {"Jigglypuff"},
        {"Sandbag"},
    }};
    return roster;
}

static std::string nativePackageFileStem(std::string name) {
    std::string out;
    out.reserve(name.size() + 7);
    bool previousUnderscore = false;
    for (char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            previousUnderscore = false;
        } else if (!previousUnderscore && !out.empty()) {
            out.push_back('_');
            previousUnderscore = true;
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.empty()) {
        out = "fighter";
    }
    out += "_native";
    return out;
}

static std::string nativePackageFileName(const NativeRosterFighterSpec& spec) {
    return nativePackageFileStem(spec.displayName) + ".pfpkg";
}

static std::filesystem::path findNativeFighterPackagePath(const NativeRosterFighterSpec& spec) {
    const std::string fileName = nativePackageFileName(spec);
    const std::array<std::filesystem::path, 5> candidates = {
        std::filesystem::path("engine_cpp/data/packages") / fileName,
        std::filesystem::path("data/packages") / fileName,
        std::filesystem::path("../data/packages") / fileName,
        std::filesystem::path("../../engine_cpp/data/packages") / fileName,
        std::filesystem::path("../../data/packages") / fileName,
    };
    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
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
    common.attackDashFrictionScaleX50 = readCommonFix(reader);
    common.dashDecayX54 = readCommonFix(reader);
    common.runInputThresholdX58 = readCommonFix(reader);
    common.runAccelScaleX5C = readCommonFix(reader);
    common.groundFrictionScaleX60 = readCommonFix(reader);
    common.catchCutFrictionScaleX64 = readCommonFix(reader);
    common.attackDashGrabBufferFramesX68 = reader.readI32();
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
    common.grabTimerBaseX354 = readCommonFix(reader);
    common.grabTimerHandicapScaleX358 = readCommonFix(reader);
    common.grabTimerHandicapBaseX35C = readCommonFix(reader);
    common.grabTimerPortScaleX360 = readCommonFix(reader);
    common.grabTimerPortBaseX364 = readCommonFix(reader);
    common.grabTimerPercentScaleX368 = readCommonFix(reader);
    common.captureCutFrictionScaleX36C = readCommonFix(reader);
    common.captureCutGroundVelocityX370 = readCommonFix(reader);
    common.captureJumpVelocityX374 = readCommonFix(reader);
    common.captureJumpVelocityYx378 = readCommonFix(reader);
    common.throwWeightAnimationScaleX37C = readCommonFix(reader);
    common.captureTimerDecrementX3A4 = readCommonFix(reader);
    common.captureMashDecrementX3A8 = readCommonFix(reader);
    common.captureJumpButtonWindowX3AC = readCommonFix(reader);
    common.captureMashAnimHoldFramesX3B0 = readCommonFix(reader);
    common.captureMashAnimRateX3B4 = readCommonFix(reader);
    common.captureJumpGravityThresholdX3B8 = readCommonFix(reader);
    common.captureFloorSnapMaxX3BC = readCommonFix(reader);
    common.captureHighThresholdX3C4 = readCommonFix(reader);
    common.thrownMashDecrementX3C8 = readCommonFix(reader);
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
    common.downWaitAutoStandFramesX424 = reader.readI32();
    common.runBrakeAnimFreezeVelocityX42C = readCommonFix(reader);
    common.runDirectFramesX430 = reader.readI32();
    common.jumpMomentumYScaleX438 = readCommonFix(reader);
    common.animVelocityScaleX440 = readCommonFix(reader);
    common.fallAnimationDriftThresholdX444 = readCommonFix(reader);
    common.fallAnimationBlendRateX448 = readCommonFix(reader);
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
    common.knockbackWeightScaleXF4 = readCommonFix(reader);
    common.knockbackWeightDecayXF8 = readCommonFix(reader);
    common.damageVelocityScaleX100 = readCommonFix(reader);
    common.knockbackMaxX108 = readCommonFix(reader);
    common.throwKnockbackWeightX10C = readCommonFix(reader);
    common.knockbackDamageBaseX110 = readCommonFix(reader);
    common.knockbackDamageScaleX114 = readCommonFix(reader);
    common.knockbackWeightSetScaleX118 = readCommonFix(reader);
    common.knockbackScaleX11C = readCommonFix(reader);
    common.knockbackBaseX120 = readCommonFix(reader);
    common.damageSakuraiAngleAirX144 = readCommonFix(reader);
    common.damageSakuraiAngleScaleX148 = readCommonFix(reader);
    common.damageSakuraiAngleLowX14C = readCommonFix(reader);
    common.damageSakuraiAngleHighX150 = readCommonFix(reader);
    common.hitstunMultiplierX154 = readCommonFix(reader);
    common.damageLevelThresholdX158 = readCommonFix(reader);
    common.damageLevelThresholdX15C = readCommonFix(reader);
    common.damageLevelThresholdX160 = readCommonFix(reader);
    common.damageAirVelocityScaleX190 = readCommonFix(reader);
    common.damageWallBounceMinVelocityX1B0 = readCommonFix(reader);
    common.damageWallBounceDampingX1BC = readCommonFix(reader);
    common.damageSurfaceLockoutX1C0 = reader.readI32();
    common.damageWallBounceMinVelocityX1E0 = readCommonFix(reader);
    common.damageLandingMinVelocityX1E4 = readCommonFix(reader);
    common.damageGroundBounceAngleX1E8 = readCommonFix(reader);
    common.damageGroundBounceDampingX1EC = readCommonFix(reader);
    common.groundKnockbackFrictionScaleX200 = readCommonFix(reader);
    common.knockbackFrameDecayX204 = readCommonFix(reader);
    common.damageFallStickThresholdX210 = readCommonFix(reader);
    common.damageFallStickWindowX214 = reader.readI32();
    common.specialSStickThresholdX218 = readCommonFix(reader);
    common.specialLwHiStickThresholdX21C = readCommonFix(reader);
    common.specialSReverseThresholdX220 = readCommonFix(reader);
    common.specialNReverseFramesX224 = readCommonFix(reader);
    common.damageFlyTopAngleMinX234 = readCommonFix(reader);
    common.damageFlyTopAngleMaxX238 = readCommonFix(reader);
    common.damageFlyRollPercentX23C = reader.readI32();
    common.damageFlyRollChanceX240 = readCommonFix(reader);
    common.downStandStickThresholdX244 = readCommonFix(reader);
    common.downRollStickThresholdX248 = readCommonFix(reader);
    common.downAttackInputWindowX24C = reader.readI32();
    common.passiveInputWindowX250 = reader.readI32();
    common.passiveStandStickThresholdX254 = readCommonFix(reader);
    common.cliffActionPercentThresholdX488 = reader.readI32();
    common.cliffWaitAutoReleaseFramesQuickX48C = reader.readI32();
    common.cliffWaitAutoReleaseFramesSlowX490 = reader.readI32();
    common.cliffCStickAttackThresholdX7F8 = readCommonFix(reader);
    common.cliffCStickEscapeThresholdX7FC = readCommonFix(reader);
    common.downAttackCStickThresholdX7F4 = readCommonFix(reader);
    common.downDamageThresholdX428 = reader.readI32();
    common.grabMashStickThresholdX308 = readCommonFix(reader);
    common.thrownHitboxClearVelocityX1C8 = readCommonFix(reader);
    common.damageSongBaseX624 = readCommonFix(reader);
    common.damageSongHandicapScaleX628 = readCommonFix(reader);
    common.damageSongHandicapBaseX62C = readCommonFix(reader);
    common.damageSongPortScaleX630 = readCommonFix(reader);
    common.damageSongPortBaseX634 = readCommonFix(reader);
    common.damageSongPercentScaleX638 = readCommonFix(reader);
    common.damageSongTimerDecrementX63C = readCommonFix(reader);
    common.damageSongMashDecrementX640 = readCommonFix(reader);
    common.damageSongElement7TimerMultiplierX644 = readCommonFix(reader);
    common.damageBindBaseX658 = readCommonFix(reader);
    common.damageBindHandicapScaleX65C = readCommonFix(reader);
    common.damageBindHandicapBaseX660 = readCommonFix(reader);
    common.damageBindPortScaleX664 = readCommonFix(reader);
    common.damageBindPortBaseX668 = readCommonFix(reader);
    common.damageBindPercentScaleX66C = readCommonFix(reader);
    common.damageBindTimerDecrementX670 = readCommonFix(reader);
    common.damageBindMashDecrementX674 = readCommonFix(reader);
    common.burySubmergeFramesX5F4 = reader.readI32();
    common.buryBaseX5F8 = readCommonFix(reader);
    common.buryHandicapScaleX5FC = readCommonFix(reader);
    common.buryHandicapBaseX600 = readCommonFix(reader);
    common.buryPortScaleX604 = readCommonFix(reader);
    common.buryPortBaseX608 = readCommonFix(reader);
    common.buryPercentScaleX60C = readCommonFix(reader);
    common.buryTimerDecrementX610 = readCommonFix(reader);
    common.buryMashDecrementX614 = readCommonFix(reader);
    common.buryJumpVelocityYx618 = readCommonFix(reader);
    common.buryJumpGravityThresholdX61C = readCommonFix(reader);
    common.buryJumpCollisionFramesX620 = reader.readI32();
    common.reboundDamageScaleX3D0 = readCommonFix(reader);
    common.reboundDamageBaseX3D4 = readCommonFix(reader);
    common.reboundAccelScaleX3D8 = readCommonFix(reader);
    common.reboundAccelBaseX3DC = readCommonFix(reader);
    common.furafuraTimerBaseX2F8 = readCommonFix(reader);
    common.furafuraTimerMinX2FC = readCommonFix(reader);
    common.furafuraTimerDecrementX300 = readCommonFix(reader);
    common.furafuraMashDecrementX304 = readCommonFix(reader);
    common.furafuraShieldHealthX280 = readCommonFix(reader);
    common.inputRepeatWindowX1C = reader.readI32();
    common.damageGroundKnockbackClampX164 = readCommonFix(reader);
    common.fallSpecialPlatformStickThresholdX25C = readCommonFix(reader);
    common.itemScrewJumpMultiplierX800 = readCommonFix(reader);
    common.damageIceGravityMultiplierX77C = readCommonFix(reader);
    common.damageIceTimerDamageScaleX790 = readCommonFix(reader);
    common.damageIceTimerDecrementX794 = readCommonFix(reader);
    common.damageIceMashDecrementX798 = readCommonFix(reader);
    common.damageIceHitDamageTimerReductionX79C = readCommonFix(reader);
    common.damageIceJumpEscapeFramesX7A4 = readCommonFix(reader);
    return common;
}

MeleeCommonData loadMeleeCommonData() {
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
    int best = -1;
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

const std::vector<AnimationClip>& authoredAnimationClips(const FighterDefinition& def) {
    return def.authoredClipSource ? *def.authoredClipSource : def.authoredClips;
}

const AnimationClip* authoredAnimationClipByActionIndex(const FighterDefinition& def, int actionIndex) {
    if (actionIndex < 0) {
        return nullptr;
    }
    const std::vector<AnimationClip>& clips = authoredAnimationClips(def);
    const auto found = std::find_if(clips.begin(), clips.end(), [&](const AnimationClip& clip) {
        return clip.actionIndex == actionIndex;
    });
    return found == clips.end() ? nullptr : &*found;
}

const FighterMesh& authoredFighterMesh(const FighterDefinition& def) {
    return def.authoredMeshSource ? *def.authoredMeshSource : def.authoredMesh;
}

const std::vector<ModelPartAnimationSet>& authoredModelPartAnimations(const FighterDefinition& def) {
    return def.modelPartAnimationSource ? *def.modelPartAnimationSource : def.modelPartAnimations;
}

static bool tryLoadNativePackageFighterDefinition(const NativeRosterFighterSpec& spec, FighterDefinition& out) {
    const std::filesystem::path packagePath = findNativeFighterPackagePath(spec);
    if (packagePath.empty()) {
        return false;
    }
    static std::unordered_map<std::string, FighterDefinition> cachedDefinitions;
    const std::string cacheKey = packagePath.string();
    if (const auto found = cachedDefinitions.find(cacheKey); found != cachedDefinitions.end()) {
        out = found->second;
        return true;
    }

    FighterPackage package;
    std::string error;
    if (!loadFighterPackage(packagePath.string(), package, &error) || package.fighters.empty()) {
        throw std::runtime_error("invalid native fighter package " + packagePath.string() + ": " + error);
    }
    out = std::move(package.fighters.front());
    if (out.name != spec.displayName) {
        throw std::runtime_error(
            "native fighter package " + packagePath.string() + " contains fighter '" + out.name +
            "' but roster expected '" + spec.displayName + "'");
    }
    if (!out.authoredClips.empty()) {
        out.authoredClipSource = std::make_shared<const std::vector<AnimationClip>>(std::move(out.authoredClips));
        out.authoredClips.clear();
    }
    if (!out.authoredMesh.batches.empty() || !out.authoredMesh.textures.empty()) {
        out.authoredMeshSource = std::make_shared<const FighterMesh>(std::move(out.authoredMesh));
        out.authoredMesh = {};
    }
    if (!out.modelPartAnimations.empty()) {
        out.modelPartAnimationSource =
            std::make_shared<const std::vector<ModelPartAnimationSet>>(std::move(out.modelPartAnimations));
        out.modelPartAnimations.clear();
    }
    cachedDefinitions.emplace(cacheKey, out);
    return true;
}

static FighterDefinition makeTrainingRosterFighterDefinition(const NativeRosterFighterSpec& spec) {
    FighterDefinition native;
    if (tryLoadNativePackageFighterDefinition(spec, native)) {
        return native;
    }
    throw std::runtime_error(
        std::string("missing native fighter package for ") + spec.displayName +
        ": expected engine_cpp/data/packages/" + nativePackageFileName(spec));
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

static const std::vector<AnimationJoint>* fighterAnimationSkeleton(const FighterDefinition& def) {
    if (!def.authoredSkeleton.empty()) {
        return &def.authoredSkeleton;
    }
    return nullptr;
}

static std::vector<JointWorldTransform> fighterWorldTransforms(const FighterDefinition& def, const FighterRuntime& fighter) {
    const std::vector<AnimationJoint>* skeleton = fighterAnimationSkeleton(def);
    if (!skeleton) {
        return {};
    }
    std::vector<JointWorldTransform> localTransforms = jointWorldTransforms(*skeleton, fighter.animationPose);
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

static size_t modelVisibilityStateCount(const FighterMesh& mesh) {
    int maxIndex = -1;
    for (const FighterMeshBatch& batch : mesh.batches) {
        if (batch.hiddenByVisibilityTable && batch.modelPartIndex >= 0) {
            maxIndex = std::max(maxIndex, batch.modelPartIndex);
        }
    }
    return static_cast<size_t>(maxIndex + 1);
}

static void ensureModelVisibilityDefaults(const FighterDefinition& def, FighterRuntime& fighter) {
    const FighterMesh& mesh = authoredFighterMesh(def);
    if (mesh.batches.empty()) {
        fighter.modelVisibilityDefaultStates.clear();
        fighter.modelVisibilityStates.clear();
        return;
    }
    const size_t count = modelVisibilityStateCount(mesh);
    if (fighter.modelVisibilityDefaultStates.size() != count) {
        // HSDLib's renderer resets high-poly visibility groups to state 0.
        // Character-specific ftParts_80074A4C default overrides are ported separately.
        fighter.modelVisibilityDefaultStates.assign(count, 0);
    }
    if (fighter.modelVisibilityStates.size() != count) {
        fighter.modelVisibilityStates = fighter.modelVisibilityDefaultStates;
    }
}

static void initializePackageVars(const FighterDefinition& def, FighterRuntime& fighter) {
    fighter.packageVars.resize(def.packageVariables.size());
    for (size_t i = 0; i < def.packageVariables.size(); ++i) {
        fighter.packageVars[i] = def.packageVariables[i].initialValue;
    }
}

static void initializeGameObjectPackageVars(const GameObjectDefinition& def, GameObjectRuntime& object) {
    object.packageVars.resize(def.packageVariables.size());
    for (size_t i = 0; i < def.packageVariables.size(); ++i) {
        object.packageVars[i] = def.packageVariables[i].initialValue;
    }
}

static FighterRuntime makeTrainingFighter(World& world, int fighterDefIndex, Vec2 position, int facing) {
    FighterRuntime p1;
    p1.fighterDef = fighterDefIndex;
    p1.state = world.fighterDefs[static_cast<size_t>(fighterDefIndex)].stateIndex("Wait");
    p1.position = position;
    p1.previousPosition = p1.position;
    p1.facing = facing;
    p1.poseFacing = facing;
    p1.groundSegment = mainFloorSegmentIndex(world.stage);
    if (p1.groundSegment >= 0) {
        segmentYAtX(world.stage.segments[static_cast<size_t>(p1.groundSegment)], p1.position.x, p1.position.y);
    }
    p1.previousPosition = p1.position;
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighterDefIndex)];
    p1.shieldHealth = def.shield.maxHealth;
    initializePackageVars(def, p1);
    if (!authoredFighterMesh(def).batches.empty()) {
        ensureModelVisibilityDefaults(def, p1);
        p1.modelPartAnimations.assign(authoredModelPartAnimations(def).size(), -1);
    }
    return p1;
}

static std::vector<GameObjectDefinition> makeDefaultObjectDefinitions() {
    auto objectCall = [](std::string name) {
        FunctionCall fn;
        fn.name = std::move(name);
        return fn;
    };

    HitboxDefinition laserHitbox;
    laserHitbox.hitboxId = 0;
    laserHitbox.radius = fxFromFloat(0.28f);
    laserHitbox.damage = fx(3);
    laserHitbox.knockbackAngleDegrees = fx(361);
    laserHitbox.knockbackBase = fx(8);
    laserHitbox.knockbackGrowth = fx(20);
    laserHitbox.canClank = true;

    GameObjectDefinition trainingLaser;
    trainingLaser.name = "TrainingLaser";
    trainingLaser.kind = GameObjectKind::Projectile;
    trainingLaser.states = {{
        "Travel",
        90,
        false,
        {},
        {objectCall("object_lifetime")},
        {objectCall("object_linear_physics")},
        {objectCall("object_blast_destroy")},
    }};
    trainingLaser.lifetimeFrames = 90;
    trainingLaser.gravity = 0;
    trainingLaser.terminalVelocity = fx(8);
    trainingLaser.destroyOnHit = true;
    trainingLaser.destroyOnShield = true;
    trainingLaser.hitOwner = false;
    trainingLaser.onDamageDealt = {objectCall("object_destroy")};
    trainingLaser.onClanked = {objectCall("object_destroy")};
    trainingLaser.onAbsorbed = {objectCall("object_destroy")};
    trainingLaser.onHitShield = {objectCall("object_destroy")};
    trainingLaser.hitboxes = {laserHitbox};

    HitboxDefinition lightItemHitbox = laserHitbox;
    lightItemHitbox.radius = fxFromFloat(0.42f);
    lightItemHitbox.damage = fx(6);
    lightItemHitbox.knockbackBase = fx(18);
    lightItemHitbox.knockbackGrowth = fx(35);

    GameObjectDefinition trainingItem;
    trainingItem.name = "TrainingItem";
    trainingItem.kind = GameObjectKind::Item;
    trainingItem.states = {{
        "Fall",
        600,
        true,
        {},
        {objectCall("object_lifetime")},
        {objectCall("object_gravity_physics")},
        {objectCall("object_floor_stop"), objectCall("object_blast_destroy")},
    }};
    trainingItem.lifetimeFrames = 600;
    trainingItem.gravity = fxFromFloat(0.08f);
    trainingItem.terminalVelocity = fxFromFloat(2.5f);
    trainingItem.maxDamage = fx(4);
    trainingItem.destroyOnHit = false;
    trainingItem.destroyOnShield = false;
    trainingItem.hitOwner = true;
    trainingItem.onDamageReceived = {objectCall("object_destroy")};
    trainingItem.hitboxes = {lightItemHitbox};
    trainingItem.hurtboxes = {{
        {0, fxFromFloat(0.2f), 0},
        {0, fxFromFloat(0.8f), 0},
        fxFromFloat(0.45f),
        HurtboxState::Normal,
    }};

    GameObjectDefinition trainingAirEventItem = trainingItem;
    trainingAirEventItem.name = "TrainingAirEventItem";
    trainingAirEventItem.onEnteredAir = {objectCall("object_destroy")};

    GameObjectDefinition trainingStateEnterItem = trainingItem;
    trainingStateEnterItem.name = "TrainingStateEnterItem";
    trainingStateEnterItem.states[0].onEnter = {objectCall("object_destroy")};

    GameObjectDefinition trainingHitlagEnterItem = trainingItem;
    trainingHitlagEnterItem.name = "TrainingHitlagEnterItem";
    trainingHitlagEnterItem.maxDamage = 0;
    trainingHitlagEnterItem.onDamageReceived.clear();
    trainingHitlagEnterItem.onEnteredHitlag = {objectCall("object_destroy")};

    GameObjectDefinition trainingHitlagExitItem = trainingItem;
    trainingHitlagExitItem.name = "TrainingHitlagExitItem";
    trainingHitlagExitItem.maxDamage = 0;
    trainingHitlagExitItem.onDamageReceived.clear();
    trainingHitlagExitItem.onExitedHitlag = {objectCall("object_destroy")};

    GameObjectDefinition trainingAccessoryItem = trainingItem;
    trainingAccessoryItem.name = "TrainingAccessoryItem";
    trainingAccessoryItem.onAccessory = {objectCall("object_destroy")};

    GameObjectDefinition trainingTouchItem = trainingItem;
    trainingTouchItem.name = "TrainingTouchItem";
    trainingTouchItem.hitboxes.clear();
    trainingTouchItem.hurtboxes.clear();
    trainingTouchItem.touchboxes = {{
        {0, fxFromFloat(0.1f), 0},
        {0, fxFromFloat(0.9f), 0},
        fxFromFloat(0.6f),
        true,
        true,
    }};
    trainingTouchItem.onTouched = {objectCall("object_destroy")};

    GameObjectDefinition trainingJumpedOnItem = trainingTouchItem;
    trainingJumpedOnItem.name = "TrainingJumpedOnItem";
    trainingJumpedOnItem.onTouched.clear();
    trainingJumpedOnItem.onJumpedOn = {objectCall("object_destroy")};

    GameObjectDefinition trainingGrabDealtItem = trainingItem;
    trainingGrabDealtItem.name = "TrainingGrabDealtItem";
    trainingGrabDealtItem.hitboxes[0].isGrab = true;
    trainingGrabDealtItem.hitboxes[0].damage = 0;
    trainingGrabDealtItem.hitboxes[0].radius = fxFromFloat(3.0f);
    trainingGrabDealtItem.hitboxes[0].canClank = false;
    trainingGrabDealtItem.onGrabDealt = {objectCall("object_destroy")};

    GameObjectDefinition trainingGrabVictimItem = trainingGrabDealtItem;
    trainingGrabVictimItem.name = "TrainingGrabVictimItem";
    trainingGrabVictimItem.onGrabDealt.clear();
    trainingGrabVictimItem.onGrabbedForVictim = {objectCall("object_destroy")};

    GameObjectDefinition trainingInteractionItem = trainingItem;
    trainingInteractionItem.name = "TrainingInteractionItem";
    trainingInteractionItem.onInteraction = {objectCall("object_destroy")};

    GameObjectDefinition trainingClearReferenceItem = trainingItem;
    trainingClearReferenceItem.name = "TrainingClearReferenceItem";
    trainingClearReferenceItem.onInteraction = {objectCall("object_clear_fighter_reference")};

    return {
        trainingLaser,
        trainingItem,
        trainingAirEventItem,
        trainingStateEnterItem,
        trainingHitlagEnterItem,
        trainingHitlagExitItem,
        trainingAccessoryItem,
        trainingTouchItem,
        trainingJumpedOnItem,
        trainingGrabDealtItem,
        trainingGrabVictimItem,
        trainingInteractionItem,
        trainingClearReferenceItem,
    };
}

World makeTrainingWorld(int p1FighterDef, int p2FighterDef) {
    World world;
    world.stage = makeBattlefieldTrainingStage();
    for (const NativeRosterFighterSpec& spec : nativeTrainingRoster()) {
        world.fighterDefs.push_back(makeTrainingRosterFighterDefinition(spec));
    }
    world.objectDefs = makeDefaultObjectDefinitions();
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

void resetTrainingFighter(World& world, size_t fighterIndex, int fighterDefIndex, Vec2 position, int facing) {
    if (fighterIndex >= world.fighters.size() || world.fighterDefs.empty()) {
        return;
    }
    fighterDefIndex = std::clamp(fighterDefIndex, 0, static_cast<int>(world.fighterDefs.size()) - 1);
    world.fighters[fighterIndex] = makeTrainingFighter(world, fighterDefIndex, position, facing);
    FighterRuntime& fighter = world.fighters[fighterIndex];
    evaluatePose(world.fighterDefs[static_cast<size_t>(fighter.fighterDef)], currentState(world, fighter), fighter);
    calculateEcb(world.fighterDefs[static_cast<size_t>(fighter.fighterDef)], fighter, true);
    calculateEcb(world.fighterDefs[static_cast<size_t>(fighter.fighterDef)], fighter, true);
    runStateFunctions(world, fighterIndex, currentState(world, fighter).onEnter);
}

bool switchFighterDefinition(World& world, FighterRuntime& fighter, const std::string& fighterName, const std::string& stateName) {
    const auto found = std::find_if(world.fighterDefs.begin(), world.fighterDefs.end(), [&](const FighterDefinition& def) {
        return def.name == fighterName;
    });
    if (found == world.fighterDefs.end() || found->states.empty()) {
        return false;
    }

    const int fighterDefIndex = static_cast<int>(std::distance(world.fighterDefs.begin(), found));
    const std::string oldStateName = fighter.fighterDef >= 0 &&
            fighter.fighterDef < static_cast<int>(world.fighterDefs.size()) &&
            fighter.state >= 0 &&
            fighter.state < static_cast<int>(world.fighterDefs[static_cast<size_t>(fighter.fighterDef)].states.size())
        ? currentState(world, fighter).name
        : std::string{"Wait"};
    const std::string desiredStateName = stateName.empty() ? oldStateName : stateName;
    int targetState = found->stateIndex(desiredStateName);
    if (targetState < 0) {
        targetState = found->stateIndex("Wait");
    }
    if (targetState < 0) {
        targetState = 0;
    }

    fighter.fighterDef = fighterDefIndex;
    fighter.state = targetState;
    fighter.lastStateChangeFrame = fighter.internalFrame;
    fighter.interruptibleFrame = found->states[static_cast<size_t>(targetState)].initialInterruptibleFrame;
    fighter.stateAnimationLengthOverride = 0;
    fighter.animationFrame = 0;
    fighter.animationRate = fx(1);
    fighter.animationActionIndexOverride = -1;
    fighter.lastActionFrameExecuted = -1;
    fighter.poseFacing = fighter.facing;
    fighter.animationTransN = {};
    fighter.previousAnimationTransN = {};
    fighter.animationTransNOffset = {};
    fighter.animationBlendFromPose.joints.clear();
    fighter.animationBlendFrames = 0;
    fighter.animationBlendElapsed = 0;
    fighter.throwAnimationFrozen = false;
    fighter.thrownAnimationFreezeActive = false;
    fighter.thrownAnimationFreezeFrame = 0;
    fighter.floorSkipSegment = -1;
    fighter.activeHitboxes.clear();
    fighter.fightersHitThisAction.clear();
    initializePackageVars(*found, fighter);
    if (!authoredFighterMesh(*found).batches.empty()) {
        ensureModelVisibilityDefaults(*found, fighter);
        fighter.modelVisibilityStates = fighter.modelVisibilityDefaultStates;
        fighter.modelPartAnimations.assign(authoredModelPartAnimations(*found).size(), -1);
    } else {
        fighter.modelVisibilityDefaultStates.clear();
        fighter.modelVisibilityStates.clear();
        fighter.modelPartAnimations.clear();
    }
    evaluatePose(*found, found->states[static_cast<size_t>(targetState)], fighter);
    calculateEcb(*found, fighter, true);
    calculateEcb(*found, fighter, true);

    const auto runtime = std::find_if(world.fighters.begin(), world.fighters.end(), [&](const FighterRuntime& item) {
        return &item == &fighter;
    });
    if (runtime != world.fighters.end()) {
        runStateFunctions(world, static_cast<size_t>(std::distance(world.fighters.begin(), runtime)), currentState(world, fighter).onEnter);
    }
    return true;
}

int spawnFighter(World& world, const std::string& fighterName, Vec2 position, int facing) {
    const auto found = std::find_if(world.fighterDefs.begin(), world.fighterDefs.end(), [&](const FighterDefinition& def) {
        return def.name == fighterName;
    });
    if (found == world.fighterDefs.end() || found->states.empty()) {
        return -1;
    }

    const int fighterDefIndex = static_cast<int>(std::distance(world.fighterDefs.begin(), found));
    FighterRuntime fighter = makeTrainingFighter(world, fighterDefIndex, position, facing);
    fighter.position = position;
    fighter.previousPosition = position;
    fighter.facing = facing == 0 ? 1 : facing;
    fighter.poseFacing = fighter.facing;
    world.fighters.push_back(std::move(fighter));
    const int fighterIndex = static_cast<int>(world.fighters.size()) - 1;
    FighterRuntime& spawned = world.fighters.back();
    evaluatePose(*found, currentState(world, spawned), spawned);
    calculateEcb(*found, spawned, true);
    calculateEcb(*found, spawned, true);
    runStateFunctions(world, static_cast<size_t>(fighterIndex), currentState(world, spawned).onEnter);
    return fighterIndex;
}

int spawnGameObject(World& world, const std::string& objectName, int ownerFighter, Vec2 position, int facing, Vec2 velocity) {
    const auto found = std::find_if(world.objectDefs.begin(), world.objectDefs.end(), [&](const GameObjectDefinition& def) {
        return def.name == objectName;
    });
    if (found == world.objectDefs.end()) {
        return -1;
    }

    GameObjectRuntime object;
    object.objectDef = static_cast<int>(std::distance(world.objectDefs.begin(), found));
    object.state = std::clamp(found->initialState, 0, std::max(0, static_cast<int>(found->states.size()) - 1));
    object.lastStateChangeFrame = world.frame;
    object.ownerFighter = ownerFighter;
    object.facing = facing == 0 ? 1 : facing;
    object.position = position;
    object.previousPosition = position;
    object.velocity = velocity;
    object.active = true;
    initializeGameObjectPackageVars(*found, object);
    object.activeHitboxes.reserve(found->hitboxes.size());
    for (const HitboxDefinition& hitboxDef : found->hitboxes) {
        ActiveHitbox hitbox;
        hitbox.def = hitboxDef;
        hitbox.current = {position.x + hitboxDef.offset.x, position.y + hitboxDef.offset.y, hitboxDef.offset.z};
        hitbox.previous = hitbox.current;
        object.activeHitboxes.push_back(hitbox);
    }
    world.objects.push_back(std::move(object));
    const int objectIndex = static_cast<int>(world.objects.size() - 1);
    if (const GameObjectStateDefinition* state = currentGameObjectState(world, world.objects[static_cast<size_t>(objectIndex)])) {
        runGameObjectFunctions(world, static_cast<size_t>(objectIndex), state->onEnter);
    }
    if (validGameObjectIndex(world, objectIndex)) {
        runGameObjectEvent(world, static_cast<size_t>(objectIndex), GameObjectEvent::Spawned);
    }
    return objectIndex;
}

int spawnGameObjectOfKind(
    World& world,
    const std::string& objectName,
    GameObjectKind requiredKind,
    int ownerFighter,
    Vec2 position,
    int facing,
    Vec2 velocity)
{
    const auto found = std::find_if(world.objectDefs.begin(), world.objectDefs.end(), [&](const GameObjectDefinition& def) {
        return def.name == objectName;
    });
    if (found == world.objectDefs.end() || found->kind != requiredKind) {
        return -1;
    }
    return spawnGameObject(world, objectName, ownerFighter, position, facing, velocity);
}

int countGameObjectsOwnedBy(const World& world, int ownerFighter, const std::string& objectName) {
    if (ownerFighter < 0 || objectName.empty()) {
        return 0;
    }
    int count = 0;
    for (const GameObjectRuntime& object : world.objects) {
        if (!object.active || object.ownerFighter != ownerFighter ||
            object.objectDef < 0 || object.objectDef >= static_cast<int>(world.objectDefs.size()))
        {
            continue;
        }
        if (world.objectDefs[static_cast<size_t>(object.objectDef)].name == objectName) {
            ++count;
        }
    }
    return count;
}

static bool runtimePackageInstructionTargetsFighter(const PackageScriptInstruction& instruction) {
    return instruction.op == PackageScriptOp::SwitchFighterDefinition ||
        instruction.op == PackageScriptOp::SpawnFighter ||
        instruction.op == PackageScriptOp::SpawnFighterSetVar;
}

static bool runtimePackageInstructionTargetsObject(const PackageScriptInstruction& instruction) {
    return instruction.op == PackageScriptOp::SpawnObject ||
        instruction.op == PackageScriptOp::SpawnObjectFromVars ||
        instruction.op == PackageScriptOp::SpawnProjectile ||
        instruction.op == PackageScriptOp::SpawnProjectileFromVars ||
        instruction.op == PackageScriptOp::SpawnObjectSetVar ||
        instruction.op == PackageScriptOp::SpawnProjectileSetVar ||
        instruction.op == PackageScriptOp::SpawnObjectFromVarsSetVar ||
        instruction.op == PackageScriptOp::SpawnProjectileFromVarsSetVar ||
        instruction.op == PackageScriptOp::DestroyOwnedObjects ||
        instruction.op == PackageScriptOp::SetVarOwnedObjectCount;
}

static const FighterDefinition* runtimePackageFighterByName(const World& world, const std::string& name) {
    const auto found = std::find_if(world.fighterDefs.begin(), world.fighterDefs.end(), [&](const FighterDefinition& candidate) {
        return candidate.name == name;
    });
    return found == world.fighterDefs.end() ? nullptr : &*found;
}

static const GameObjectDefinition* runtimePackageObjectByName(const World& world, const std::string& name) {
    const auto found = std::find_if(world.objectDefs.begin(), world.objectDefs.end(), [&](const GameObjectDefinition& candidate) {
        return candidate.name == name;
    });
    return found == world.objectDefs.end() ? nullptr : &*found;
}

static bool runtimePackageHasFighter(const FighterPackage& package, const std::string& name) {
    return std::any_of(package.fighters.begin(), package.fighters.end(), [&](const FighterDefinition& fighter) {
        return fighter.name == name;
    });
}

static bool runtimePackageHasObject(const FighterPackage& package, const std::string& name) {
    return std::any_of(package.objects.begin(), package.objects.end(), [&](const GameObjectDefinition& object) {
        return object.name == name;
    });
}

static void removeOutOfRangeAnimationTracks(std::vector<AnimationClip>& clips, size_t skeletonSize) {
    for (AnimationClip& clip : clips) {
        clip.tracks.erase(
            std::remove_if(clip.tracks.begin(), clip.tracks.end(), [&](const AnimationTrack& track) {
                return track.joint < 0 || static_cast<size_t>(track.joint) >= skeletonSize;
            }),
            clip.tracks.end());
    }
}

static void appendRuntimePackageFighterDependency(FighterPackage& package, const World& world, const std::string& fighterName) {
    if (fighterName.empty() || runtimePackageHasFighter(package, fighterName)) {
        return;
    }
    if (const FighterDefinition* dependency = runtimePackageFighterByName(world, fighterName)) {
        package.fighters.push_back(makeNativePackageFighterDefinition(*dependency));
    }
}

static void appendRuntimePackageObjectDependency(FighterPackage& package, const World& world, const std::string& objectName) {
    if (objectName.empty() || runtimePackageHasObject(package, objectName)) {
        return;
    }
    if (const GameObjectDefinition* dependency = runtimePackageObjectByName(world, objectName)) {
        package.objects.push_back(*dependency);
    }
}

static void collectRuntimePackageInstructionDependencies(
    FighterPackage& package,
    const World& world,
    const PackageScriptInstruction& instruction)
{
    if (runtimePackageInstructionTargetsFighter(instruction)) {
        appendRuntimePackageFighterDependency(package, world, instruction.text);
    }
    if (runtimePackageInstructionTargetsObject(instruction)) {
        appendRuntimePackageObjectDependency(package, world, instruction.text);
    }
}

static void collectRuntimePackageFighterDependencies(FighterPackage& package, const World& world, const FighterDefinition& fighter) {
    for (const PackageScript& script : fighter.packageScripts) {
        for (const PackageScriptInstruction& instruction : script.instructions) {
            collectRuntimePackageInstructionDependencies(package, world, instruction);
        }
    }
    for (const FighterState& state : fighter.states) {
        for (const Subaction& subaction : state.action) {
            if (subaction.type == SubactionType::SpawnObject || subaction.type == SubactionType::SpawnProjectile) {
                appendRuntimePackageObjectDependency(package, world, subaction.objectName);
            }
        }
    }
}

static void collectRuntimePackageObjectDependencies(FighterPackage& package, const World& world, const GameObjectDefinition& object) {
    for (const PackageScript& script : object.packageScripts) {
        for (const PackageScriptInstruction& instruction : script.instructions) {
            collectRuntimePackageInstructionDependencies(package, world, instruction);
        }
    }
}

bool makeNativePackageFighterDefinition(const FighterDefinition& source, FighterDefinition& out, std::string* error) {
    out = source;
    if (out.authoredClips.empty() && source.authoredClipSource) {
        out.authoredClips = *source.authoredClipSource;
    }
    if (out.authoredMesh.batches.empty() && out.authoredMesh.textures.empty() && source.authoredMeshSource) {
        out.authoredMesh = *source.authoredMeshSource;
    }
    if (out.modelPartAnimations.empty() && source.modelPartAnimationSource) {
        out.modelPartAnimations = *source.modelPartAnimationSource;
    }
    out.authoredClipSource.reset();
    out.authoredMeshSource.reset();
    out.modelPartAnimationSource.reset();
    return true;
}

FighterDefinition makeNativePackageFighterDefinition(const FighterDefinition& source) {
    FighterDefinition out = source;
    std::string error;
    if (!makeNativePackageFighterDefinition(source, out, &error)) {
        throw std::runtime_error(error);
    }
    return out;
}

FighterPackage makeRuntimeFighterPackage(const World& world, int rootFighterDef, const std::string& packageName) {
    FighterPackage package;
    if (rootFighterDef < 0 || rootFighterDef >= static_cast<int>(world.fighterDefs.size())) {
        package.name = packageName.empty() ? "runtime" : packageName;
        return package;
    }

    const FighterDefinition& root = world.fighterDefs[static_cast<size_t>(rootFighterDef)];
    package.name = packageName.empty() ? root.name + "_runtime" : packageName;
    package.fighters.push_back(makeNativePackageFighterDefinition(root));

    for (size_t fighterScan = 0, objectScan = 0; fighterScan < package.fighters.size() || objectScan < package.objects.size();) {
        if (fighterScan < package.fighters.size()) {
            const FighterDefinition fighter = package.fighters[fighterScan];
            collectRuntimePackageFighterDependencies(package, world, fighter);
            ++fighterScan;
        }
        if (objectScan < package.objects.size()) {
            const GameObjectDefinition object = package.objects[objectScan];
            collectRuntimePackageObjectDependencies(package, world, object);
            ++objectScan;
        }
    }

    return package;
}

static bool setPackageInstallError(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
    return false;
}

static int upsertFighterDefinition(std::vector<FighterDefinition>& defs, const FighterDefinition& def) {
    const auto existing = std::find_if(defs.begin(), defs.end(), [&](const FighterDefinition& candidate) {
        return candidate.name == def.name;
    });
    if (existing != defs.end()) {
        *existing = def;
        return static_cast<int>(std::distance(defs.begin(), existing));
    }
    defs.push_back(def);
    return static_cast<int>(defs.size()) - 1;
}

static int upsertGameObjectDefinition(std::vector<GameObjectDefinition>& defs, const GameObjectDefinition& def) {
    const auto existing = std::find_if(defs.begin(), defs.end(), [&](const GameObjectDefinition& candidate) {
        return candidate.name == def.name;
    });
    if (existing != defs.end()) {
        *existing = def;
        return static_cast<int>(std::distance(defs.begin(), existing));
    }
    defs.push_back(def);
    return static_cast<int>(defs.size()) - 1;
}

static int fighterDefinitionIndexByName(const World& world, const std::string& name, int fallback) {
    const auto found = std::find_if(world.fighterDefs.begin(), world.fighterDefs.end(), [&](const FighterDefinition& candidate) {
        return candidate.name == name;
    });
    if (found == world.fighterDefs.end()) {
        return fallback;
    }
    return static_cast<int>(std::distance(world.fighterDefs.begin(), found));
}

static bool installValidatedFighterPackage(World& world, const FighterPackage& package, int* rootFighterDef, std::string* error) {
    if (rootFighterDef) {
        *rootFighterDef = -1;
    }
    if (package.fighters.empty()) {
        return setPackageInstallError(error, "fighter package has no root fighter");
    }

    for (const GameObjectDefinition& object : package.objects) {
        upsertGameObjectDefinition(world.objectDefs, object);
    }

    int installedRoot = -1;
    for (size_t i = 0; i < package.fighters.size(); ++i) {
        const int installed = upsertFighterDefinition(world.fighterDefs, package.fighters[i]);
        if (i == 0) {
            installedRoot = installed;
        }
    }

    if (installedRoot < 0) {
        return setPackageInstallError(error, "failed to install root fighter");
    }
    if (rootFighterDef) {
        *rootFighterDef = installedRoot;
    }
    if (error) {
        error->clear();
    }
    return true;
}

bool installFighterPackage(World& world, const FighterPackage& package, int* rootFighterDef, std::string* error) {
    if (rootFighterDef) {
        *rootFighterDef = -1;
    }
    if (package.fighters.empty()) {
        return setPackageInstallError(error, "fighter package has no root fighter");
    }

    std::string validationError;
    if (!validateFighterPackage(package, &validationError)) {
        return setPackageInstallError(error, validationError.empty() ? "fighter package validation failed" : validationError);
    }

    return installValidatedFighterPackage(world, package, rootFighterDef, error);
}

bool installFighterPackageBytes(
    World& world,
    const std::vector<uint8_t>& bytes,
    int* rootFighterDef,
    FighterPackageDescriptor* descriptor,
    std::string* error)
{
    if (rootFighterDef) {
        *rootFighterDef = -1;
    }
    if (descriptor) {
        *descriptor = {};
    }

    FighterPackage package;
    std::string packageError;
    if (!readFighterPackage(bytes, package, &packageError)) {
        return setPackageInstallError(error, packageError.empty() ? "fighter package read failed" : packageError);
    }

    if (descriptor && !describeFighterPackage(package, *descriptor, bytes, &packageError)) {
        return setPackageInstallError(error, packageError.empty() ? "fighter package descriptor failed" : packageError);
    }

    return installValidatedFighterPackage(world, package, rootFighterDef, error);
}

bool installCachedFighterPackage(
    World& world,
    const FighterPackageCache& cache,
    uint32_t checksum,
    int* rootFighterDef,
    FighterPackageDescriptor* descriptor,
    std::string* error)
{
    if (rootFighterDef) {
        *rootFighterDef = -1;
    }
    if (descriptor) {
        *descriptor = {};
    }

    const std::vector<uint8_t>* bytes = cache.packageBytes(checksum);
    if (!bytes) {
        return setPackageInstallError(error, "fighter package cache entry is missing");
    }
    return installFighterPackageBytes(world, *bytes, rootFighterDef, descriptor, error);
}

bool makePackageTestWorldFromBytes(
    World& world,
    const std::vector<uint8_t>& bytes,
    int* rootFighterDef,
    FighterPackageDescriptor* descriptor,
    std::string* error)
{
    if (rootFighterDef) {
        *rootFighterDef = -1;
    }
    if (descriptor) {
        *descriptor = {};
    }

    World testWorld = makeTrainingWorld();
    const int sandbagFighterDef = fighterDefinitionIndexByName(testWorld, "Sandbag", 0);
    resetTrainingFighter(testWorld, 1, sandbagFighterDef, {fx(2), 0}, -1);

    int installedRoot = -1;
    FighterPackageDescriptor installedDescriptor;
    if (!installFighterPackageBytes(testWorld, bytes, &installedRoot, &installedDescriptor, error)) {
        return false;
    }
    resetTrainingFighter(testWorld, 0, installedRoot, {-fx(2), 0}, 1);

    world = std::move(testWorld);
    if (rootFighterDef) {
        *rootFighterDef = installedRoot;
    }
    if (descriptor) {
        *descriptor = std::move(installedDescriptor);
    }
    if (error) {
        error->clear();
    }
    return true;
}

bool makeCachedPackageTestWorld(
    World& world,
    const FighterPackageCache& cache,
    uint32_t checksum,
    int* rootFighterDef,
    FighterPackageDescriptor* descriptor,
    std::string* error)
{
    if (rootFighterDef) {
        *rootFighterDef = -1;
    }
    if (descriptor) {
        *descriptor = {};
    }

    World testWorld = makeTrainingWorld();
    const int sandbagFighterDef = fighterDefinitionIndexByName(testWorld, "Sandbag", 0);
    resetTrainingFighter(testWorld, 1, sandbagFighterDef, {fx(2), 0}, -1);

    int installedRoot = -1;
    FighterPackageDescriptor installedDescriptor;
    if (!installCachedFighterPackage(testWorld, cache, checksum, &installedRoot, &installedDescriptor, error)) {
        return false;
    }
    resetTrainingFighter(testWorld, 0, installedRoot, {-fx(2), 0}, 1);

    world = std::move(testWorld);
    if (rootFighterDef) {
        *rootFighterDef = installedRoot;
    }
    if (descriptor) {
        *descriptor = std::move(installedDescriptor);
    }
    if (error) {
        error->clear();
    }
    return true;
}

int destroyGameObjectsOwnedBy(World& world, int ownerFighter, const std::string& objectName) {
    if (ownerFighter < 0 || objectName.empty()) {
        return 0;
    }
    int destroyed = 0;
    const size_t initialCount = world.objects.size();
    for (size_t objectIndex = 0; objectIndex < initialCount && objectIndex < world.objects.size(); ++objectIndex) {
        const GameObjectRuntime& object = world.objects[objectIndex];
        if (!object.active || object.ownerFighter != ownerFighter ||
            object.objectDef < 0 || object.objectDef >= static_cast<int>(world.objectDefs.size()))
        {
            continue;
        }
        if (world.objectDefs[static_cast<size_t>(object.objectDef)].name != objectName) {
            continue;
        }
        deactivateGameObject(world, objectIndex);
        ++destroyed;
    }
    return destroyed;
}

bool destroyGameObjectByIndex(World& world, int objectIndex) {
    if (objectIndex < 0 || objectIndex >= static_cast<int>(world.objects.size()) ||
        !world.objects[static_cast<size_t>(objectIndex)].active)
    {
        return false;
    }
    deactivateGameObject(world, static_cast<size_t>(objectIndex));
    return objectIndex < static_cast<int>(world.objects.size()) &&
        !world.objects[static_cast<size_t>(objectIndex)].active;
}

static bool validGameObjectIndex(const World& world, int objectIndex) {
    return objectIndex >= 0 && objectIndex < static_cast<int>(world.objects.size()) &&
        world.objects[static_cast<size_t>(objectIndex)].active;
}

static bool validObjectOwnerFighterIndex(const World& world, int fighterIndex) {
    return fighterIndex >= 0 && fighterIndex < static_cast<int>(world.fighters.size());
}

static bool gameObjectIsHeld(const GameObjectRuntime& object) {
    return object.heldByFighter >= 0;
}

static Vec2 gameObjectHoldPosition(const World& world, int fighterIndex) {
    const FighterRuntime& fighter = world.fighters[static_cast<size_t>(fighterIndex)];
    const Vec3 hand = boneWorld(fighter, BoneId::HandR, {});
    return {hand.x, hand.y};
}

static void resetGameObjectHitboxSweeps(GameObjectRuntime& object) {
    for (ActiveHitbox& hitbox : object.activeHitboxes) {
        hitbox.firstFrame = true;
    }
}

bool pickUpGameObject(World& world, int objectIndex, int fighterIndex) {
    if (!validGameObjectIndex(world, objectIndex) || !validObjectOwnerFighterIndex(world, fighterIndex)) {
        return false;
    }
    FighterRuntime& fighter = world.fighters[static_cast<size_t>(fighterIndex)];
    if (fighter.heldObject >= 0 && fighter.heldObject != objectIndex) {
        return false;
    }
    GameObjectRuntime& object = world.objects[static_cast<size_t>(objectIndex)];
    object.ownerFighter = fighterIndex;
    object.heldByFighter = fighterIndex;
    fighter.heldObject = objectIndex;
    object.velocity = {};
    object.grounded = false;
    object.groundSegment = -1;
    object.previousPosition = object.position;
    object.position = gameObjectHoldPosition(world, fighterIndex);
    resetGameObjectHitboxSweeps(object);
    runGameObjectEvent(world, static_cast<size_t>(objectIndex), GameObjectEvent::PickedUp);
    return true;
}

bool dropGameObject(World& world, int objectIndex, Vec2 velocity) {
    if (!validGameObjectIndex(world, objectIndex)) {
        return false;
    }
    GameObjectRuntime& object = world.objects[static_cast<size_t>(objectIndex)];
    if (validObjectOwnerFighterIndex(world, object.heldByFighter) &&
        world.fighters[static_cast<size_t>(object.heldByFighter)].heldObject == objectIndex)
    {
        world.fighters[static_cast<size_t>(object.heldByFighter)].heldObject = -1;
    }
    object.heldByFighter = -1;
    object.velocity = velocity;
    object.grounded = false;
    object.groundSegment = -1;
    resetGameObjectHitboxSweeps(object);
    runGameObjectEvent(world, static_cast<size_t>(objectIndex), GameObjectEvent::Dropped);
    return true;
}

bool throwGameObject(World& world, int objectIndex, int fighterIndex, Vec2 velocity) {
    if (!validGameObjectIndex(world, objectIndex) || !validObjectOwnerFighterIndex(world, fighterIndex)) {
        return false;
    }
    GameObjectRuntime& object = world.objects[static_cast<size_t>(objectIndex)];
    if (validObjectOwnerFighterIndex(world, object.heldByFighter) &&
        world.fighters[static_cast<size_t>(object.heldByFighter)].heldObject == objectIndex)
    {
        world.fighters[static_cast<size_t>(object.heldByFighter)].heldObject = -1;
    }
    world.fighters[static_cast<size_t>(fighterIndex)].heldObject = -1;
    object.ownerFighter = fighterIndex;
    object.heldByFighter = -1;
    object.facing = world.fighters[static_cast<size_t>(fighterIndex)].facing;
    object.velocity = velocity;
    object.grounded = false;
    object.groundSegment = -1;
    resetGameObjectHitboxSweeps(object);
    runGameObjectEvent(world, static_cast<size_t>(objectIndex), GameObjectEvent::Thrown);
    return true;
}

static Vec2 reflectedVelocity(Vec2 velocity, Vec2 normal) {
    const Fix mag = vectorMagnitude(normal);
    if (mag <= 0) {
        return {-velocity.x, velocity.y};
    }
    normal.x = fxDiv(normal.x, mag);
    normal.y = fxDiv(normal.y, mag);
    const Fix dot = fxMul(velocity.x, normal.x) + fxMul(velocity.y, normal.y);
    return {
        velocity.x - fxMul(fx(2), fxMul(dot, normal.x)),
        velocity.y - fxMul(fx(2), fxMul(dot, normal.y)),
    };
}

static void releaseHeldGameObjectOwner(World& world, GameObjectRuntime& object, int objectIndex) {
    if (validObjectOwnerFighterIndex(world, object.heldByFighter) &&
        world.fighters[static_cast<size_t>(object.heldByFighter)].heldObject == objectIndex)
    {
        world.fighters[static_cast<size_t>(object.heldByFighter)].heldObject = -1;
    }
    object.heldByFighter = -1;
}

bool reflectGameObject(World& world, int objectIndex, int fighterIndex, Vec2 normal) {
    if (!validGameObjectIndex(world, objectIndex) || !validObjectOwnerFighterIndex(world, fighterIndex)) {
        return false;
    }
    GameObjectRuntime& object = world.objects[static_cast<size_t>(objectIndex)];
    releaseHeldGameObjectOwner(world, object, objectIndex);
    object.ownerFighter = fighterIndex;
    object.velocity = reflectedVelocity(object.velocity, normal);
    if (object.velocity.x != 0) {
        object.facing = object.velocity.x > 0 ? 1 : -1;
    } else {
        object.facing = -object.facing;
    }
    resetGameObjectHitboxSweeps(object);
    runGameObjectEvent(world, static_cast<size_t>(objectIndex), GameObjectEvent::Reflected);
    return true;
}

bool absorbGameObject(World& world, int objectIndex, int fighterIndex) {
    if (!validGameObjectIndex(world, objectIndex) || !validObjectOwnerFighterIndex(world, fighterIndex)) {
        return false;
    }
    GameObjectRuntime& object = world.objects[static_cast<size_t>(objectIndex)];
    releaseHeldGameObjectOwner(world, object, objectIndex);
    object.ownerFighter = fighterIndex;
    object.velocity = {};
    runGameObjectEvent(world, static_cast<size_t>(objectIndex), GameObjectEvent::Absorbed);
    return true;
}

bool shieldBounceGameObject(World& world, int objectIndex, int fighterIndex, Vec2 normal) {
    if (!validGameObjectIndex(world, objectIndex) || !validObjectOwnerFighterIndex(world, fighterIndex)) {
        return false;
    }
    GameObjectRuntime& object = world.objects[static_cast<size_t>(objectIndex)];
    releaseHeldGameObjectOwner(world, object, objectIndex);
    object.ownerFighter = fighterIndex;
    object.velocity = reflectedVelocity(object.velocity, normal);
    if (object.velocity.x != 0) {
        object.facing = object.velocity.x > 0 ? 1 : -1;
    }
    resetGameObjectHitboxSweeps(object);
    runGameObjectEvent(world, static_cast<size_t>(objectIndex), GameObjectEvent::ShieldBounced);
    return true;
}

static void clearGameObjectFighterReference(World& world, size_t objectIndex, int fighterIndex) {
    if (objectIndex >= world.objects.size() || !validObjectOwnerFighterIndex(world, fighterIndex)) {
        return;
    }
    GameObjectRuntime& object = world.objects[objectIndex];
    if (object.ownerFighter == fighterIndex) {
        object.ownerFighter = -1;
    }
    if (object.grabVictimFighter == fighterIndex) {
        object.grabVictimFighter = -1;
    }
    if (object.heldByFighter == fighterIndex) {
        if (world.fighters[static_cast<size_t>(fighterIndex)].heldObject == static_cast<int>(objectIndex)) {
            world.fighters[static_cast<size_t>(fighterIndex)].heldObject = -1;
        }
        object.heldByFighter = -1;
    }
}

bool interactGameObjectWithFighter(World& world, int objectIndex, int fighterIndex) {
    if (!validGameObjectIndex(world, objectIndex) || !validObjectOwnerFighterIndex(world, fighterIndex)) {
        return false;
    }
    GameObjectRuntime& object = world.objects[static_cast<size_t>(objectIndex)];
    object.lastInteractionFighter = fighterIndex;
    object.lastInteractionObject = -1;
    runGameObjectEvent(world, static_cast<size_t>(objectIndex), GameObjectEvent::Interaction);
    return true;
}

bool interactGameObjects(World& world, int objectIndex, int referenceObjectIndex) {
    if (!validGameObjectIndex(world, objectIndex) || !validGameObjectIndex(world, referenceObjectIndex) ||
        objectIndex == referenceObjectIndex)
    {
        return false;
    }
    GameObjectRuntime& object = world.objects[static_cast<size_t>(objectIndex)];
    object.lastInteractionFighter = -1;
    object.lastInteractionObject = referenceObjectIndex;
    runGameObjectEvent(world, static_cast<size_t>(objectIndex), GameObjectEvent::Interaction);
    return true;
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

static bool calculateBoneDesiredEcb(const FighterDefinition& def, FighterRuntime& fighter, Ecb& out) {
    if (fighter.jointWorldPositions.empty()) {
        return false;
    }

    for (int bone : def.environmentCollisionBones) {
        if (bone < 0 || static_cast<size_t>(bone) >= fighter.jointWorldPositions.size()) {
            return false;
        }
    }

    const Vec3 topN = fighter.jointWorldPositions.size() > 1
        ? fighter.jointWorldPositions[1]
        : Vec3{fighter.position.x, fighter.position.y, 0};
    const Vec3 first = fighter.jointWorldPositions[static_cast<size_t>(def.environmentCollisionBones[0])];
    Fix minHorizontal = first.z;
    Fix maxHorizontal = first.z;
    Fix minY = first.y;
    Fix maxY = first.y;
    for (size_t i = 1; i < def.environmentCollisionBones.size(); ++i) {
        const Vec3 joint = fighter.jointWorldPositions[static_cast<size_t>(def.environmentCollisionBones[i])];
        minHorizontal = std::min(minHorizontal, joint.z);
        maxHorizontal = std::max(maxHorizontal, joint.z);
        minY = std::min(minY, joint.y);
        maxY = std::max(maxY, joint.y);
    }

    const Fix halfWidth = fxMul(fxAbs(maxHorizontal - minHorizontal), fxFromFloat(0.5f));
    const Fix bottom = (fighter.grounded ? topN.y : minY) - fighter.position.y;
    const Fix top = maxY - fighter.position.y;
    const Fix sideY = def.environmentCollisionMultiplier + fxMul(bottom + top, fxFromFloat(0.5f));
    const Fix centerX = fighter.facing * topN.z;
    out.points[0] = {centerX - halfWidth, sideY};
    out.points[1] = {centerX, top};
    out.points[2] = {centerX + halfWidth, sideY};
    out.points[3] = {centerX, bottom};
    validateDesiredEcb(out);
    refreshEcbMetadata(out, fighter);
    return true;
}

static bool calculateAuthoredDesiredEcb(const FighterDefinition& def, FighterRuntime& fighter, Ecb& out) {
    if (!def.authoredEcb.enabled) {
        return false;
    }
    out.points = def.authoredEcb.points;
    validateDesiredEcb(out);
    refreshEcbMetadata(out, fighter);
    return true;
}

static Ecb calculateDesiredEcb(const FighterDefinition& def, FighterRuntime& fighter) {
    Ecb desired;
    if (calculateBoneDesiredEcb(def, fighter, desired)) {
        return desired;
    }
    if (calculateAuthoredDesiredEcb(def, fighter, desired)) {
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

uint32_t fighterCommandVar(const FighterRuntime& fighter, int index) {
    if (index < 0 || index >= static_cast<int>(fighter.commandVars.size())) {
        return 0;
    }
    return fighter.commandVars[static_cast<size_t>(index)];
}

void setFighterCommandFlag(FighterRuntime& fighter, int flag, bool value) {
    const uint32_t mask = uint32_t{1} << flag;
    if (value) {
        fighter.commandFlags |= mask;
    } else {
        fighter.commandFlags &= ~mask;
    }
}

void setFighterCommandVar(FighterRuntime& fighter, int index, uint32_t value) {
    if (index < 0 || index >= static_cast<int>(fighter.commandVars.size())) {
        return;
    }
    fighter.commandVars[static_cast<size_t>(index)] = value;
    setFighterCommandFlag(fighter, index, value != 0);
}

bool fighterThrowFlag(const FighterRuntime& fighter, int flag) {
    return (fighter.throwFlags & (uint32_t{1} << flag)) != 0;
}

void setFighterThrowFlag(FighterRuntime& fighter, int flag, bool value) {
    const uint32_t mask = uint32_t{1} << flag;
    if (value) {
        fighter.throwFlags |= mask;
    } else {
        fighter.throwFlags &= ~mask;
    }
}

void lockFighterEcb(FighterRuntime& fighter, int frames) {
    fighter.ecbLockTimer = std::max(frames, 0);
    fighter.ecbLockBottom = fighter.desiredEcb.points[3];
}

void unlockFighterEcb(FighterRuntime& fighter) {
    fighter.ecbLockTimer = 0;
}

static bool stateHasImportedAnimation(const FighterDefinition& def, const std::string& stateName) {
    const int index = def.stateIndex(stateName);
    if (index < 0) {
        return false;
    }
    const FighterState& state = def.states[static_cast<size_t>(index)];
    if (state.animationActionIndex >= 0) {
        const AnimationClip* clip = authoredAnimationClipByActionIndex(def, state.animationActionIndex);
        if (clip && !clip->generatedFallback) {
            return true;
        }
    }
    const std::string suffix = "_ACTION_" + state.animation + "_figatree";
    for (const AnimationClip& clip : authoredAnimationClips(def)) {
        if (clip.generatedFallback) {
            continue;
        }
        if (clip.name == state.animation ||
            (clip.name.size() >= suffix.size() &&
             clip.name.compare(clip.name.size() - suffix.size(), suffix.size(), suffix) == 0))
        {
            return true;
        }
    }
    return false;
}

static std::string resolveCliffActionState(const FighterDefinition& def, const FighterRuntime& fighter, const std::string& stateName) {
    const bool quick = fighter.percent < fx(def.properties.common.cliffActionPercentThresholdX488);
    const char* suffix = quick ? "Quick" : "Slow";
    std::string resolved = stateName;
    if (stateName == "CliffClimb") {
        resolved = std::string("CliffClimb") + suffix;
    } else if (stateName == "CliffAttack") {
        resolved = std::string("CliffAttack") + suffix;
    } else if (stateName == "CliffEscape") {
        resolved = std::string("CliffEscape") + suffix;
    } else if (stateName == "CliffJump") {
        resolved = quick ? "CliffJumpQuick1" : "CliffJumpSlow1";
    } else if (stateName == "CliffJumpAir") {
        resolved = quick ? "CliffJumpQuick2" : "CliffJumpSlow2";
    } else if (stateName == "AppealS") {
        resolved = fighter.facing < 0 && stateHasImportedAnimation(def, "AppealSL") ? "AppealSL" : "AppealSR";
    }
    return def.stateIndex(resolved) >= 0 ? resolved : stateName;
}

void changeFighterState(World& world, FighterRuntime& fighter, const std::string& stateName, int lagFrames, int blendFrames) {
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const std::string targetStateName = resolveCliffActionState(def, fighter, stateName);
    const int target = def.stateIndex(targetStateName);
    if (target < 0) {
        return;
    }
    const std::string oldStateName = currentState(world, fighter).name;
    const auto ownsCapturedVictim = [](const std::string& name) {
        return name == "Catch" || name == "CatchDash" ||
               name == "CatchPull" || name == "CatchDashPull" ||
               name == "CatchWait" || name == "CatchAttack" ||
               name == "CatchCut" ||
               name == "ThrowF" || name == "ThrowB" ||
               name == "ThrowHi" || name == "ThrowLw" ||
               name == "CaptureMewtwo" || name == "CaptureMewtwoAir" ||
               name == "CaptureCaptain" ||
               name == "CaptureKoopa" || name == "CaptureKoopaAir";
    };
    const auto isCapturedVictimState = [](const std::string& name) {
        return name == "CapturePulledHi" || name == "CapturePulledLw" ||
               name == "CaptureWaitHi" || name == "CaptureWaitLw" ||
               name == "CaptureDamageHi" || name == "CaptureDamageLw" ||
               name == "CaptureYoshi" ||
               name == "CaptureNeck" || name == "CaptureFoot" ||
               name == "ThrownF" || name == "ThrownB" ||
               name == "ThrownHi" || name == "ThrownLw" ||
               name == "ThrownLwWomen" ||
               name == "ThrownFF" || name == "ThrownFB" ||
               name == "ThrownFHi" || name == "ThrownFLw" ||
               name == "ThrownKoopaF" || name == "ThrownKoopaB" ||
               name == "ThrownKoopaAirF" || name == "ThrownKoopaAirB" ||
               name == "ThrownMewtwo" || name == "ThrownMewtwoAir";
    };
    if (ownsCapturedVictim(oldStateName) && !ownsCapturedVictim(targetStateName) && fighter.grabbedFighter >= 0 &&
        fighter.grabbedFighter < static_cast<int>(world.fighters.size()))
    {
        FighterRuntime& victim = world.fighters[static_cast<size_t>(fighter.grabbedFighter)];
        if (victim.grabberFighter == static_cast<int>(&fighter - world.fighters.data())) {
            victim.grabberFighter = -1;
            victim.captureConstraintActive = false;
            victim.captureConstraintOffset = {};
            victim.captureOriginalXRotNTranslation = {};
        }
        fighter.grabbedFighter = -1;
    }
    if (isCapturedVictimState(oldStateName) && !isCapturedVictimState(targetStateName) && fighter.grabberFighter >= 0 &&
        fighter.grabberFighter < static_cast<int>(world.fighters.size()))
    {
        FighterRuntime& grabber = world.fighters[static_cast<size_t>(fighter.grabberFighter)];
        if (grabber.grabbedFighter == static_cast<int>(&fighter - world.fighters.data())) {
            grabber.grabbedFighter = -1;
        }
        fighter.grabberFighter = -1;
        fighter.captureConstraintActive = false;
        fighter.captureConstraintOffset = {};
        fighter.captureOriginalXRotNTranslation = {};
        fighter.thrownAnimationFreezeActive = false;
        fighter.thrownAnimationFreezeFrame = 0;
    }
    const FighterState& targetState = def.states[static_cast<size_t>(target)];
    int resolvedBlendFrames = blendFrames;
    if (resolvedBlendFrames == kUseDefaultAnimationBlendFrames) {
        resolvedBlendFrames = targetState.defaultAnimationBlendFrames;
    } else if (resolvedBlendFrames == kDisableAnimationBlendFrames) {
        resolvedBlendFrames = 0;
    }
    if (resolvedBlendFrames > 0 && !fighter.animationPose.joints.empty()) {
        fighter.animationBlendFromPose = fighter.animationPose;
        fighter.animationBlendFrames = resolvedBlendFrames;
        fighter.animationBlendElapsed = 0;
    } else {
        fighter.animationBlendFromPose.joints.clear();
        fighter.animationBlendFrames = 0;
        fighter.animationBlendElapsed = 0;
    }
    fighter.animationTransN = {};
    fighter.previousAnimationTransN = {};
    fighter.animationTransNOffset = {};
    fighter.throwAnimationFrozen = false;
    fighter.thrownAnimationFreezeActive = false;
    fighter.thrownAnimationFreezeFrame = 0;
    fighter.floorSkipSegment = -1;
    fighter.animationFrame = 0;
    fighter.animationRate = fx(1);
    fighter.animationActionIndexOverride = -1;
    fighter.lastActionFrameExecuted = -1;
    fighter.poseFacing = fighter.facing;
    fighter.state = target;
    fighter.lastStateChangeFrame = fighter.internalFrame;
    fighter.interruptibleFrame = lagFrames > 0 ? lagFrames : targetState.initialInterruptibleFrame;
    fighter.stateAnimationLengthOverride = lagFrames > 0 ? lagFrames : 0;
    fighter.activeHitboxes.clear();
    fighter.fightersHitThisAction.clear();
    if (!authoredFighterMesh(def).batches.empty()) {
        ensureModelVisibilityDefaults(def, fighter);
        fighter.modelVisibilityStates = fighter.modelVisibilityDefaultStates;
        fighter.modelPartAnimations.assign(authoredModelPartAnimations(def).size(), -1);
    } else {
        fighter.modelVisibilityDefaultStates.clear();
        fighter.modelVisibilityStates.clear();
        fighter.modelPartAnimations.clear();
    }
    const auto it = std::find_if(world.fighters.begin(), world.fighters.end(), [&](const FighterRuntime& item) {
        return &item == &fighter;
    });
    if (it != world.fighters.end()) {
        const size_t fighterIndex = static_cast<size_t>(std::distance(world.fighters.begin(), it));
        runStateFunctions(world, fighterIndex, currentState(world, fighter).onEnter);
        if (fighterIndex < world.fighters.size()) {
            FighterRuntime& current = world.fighters[fighterIndex];
            if (current.fighterDef >= 0 && current.fighterDef < static_cast<int>(world.fighterDefs.size())) {
                const FighterDefinition& currentDef = world.fighterDefs[static_cast<size_t>(current.fighterDef)];
                if (current.state >= 0 && current.state < static_cast<int>(currentDef.states.size())) {
                    evaluatePose(currentDef, currentDef.states[static_cast<size_t>(current.state)], current);
                }
            }
        }
    } else {
        evaluatePose(def, targetState, fighter);
    }
}

static bool ruleActive(const InterruptRule& rule, const FighterRuntime& fighter) {
    if (rule.requireNoHitstun && fighter.hitstun > 0) {
        return false;
    }
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

static int32_t fighterPackageVar(const FighterRuntime& fighter, int index) {
    if (index < 0 || index >= static_cast<int>(fighter.packageVars.size())) {
        return 0;
    }
    return fighter.packageVars[static_cast<size_t>(index)];
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
    const AnimationClip* clip = authoredAnimationClipByActionIndex(def, actionIndex);
    return clip != nullptr && !clip->generatedFallback;
}

static bool hasActionClipNamed(const FighterDefinition& def, const std::string& animation) {
    const std::string suffix = "_ACTION_" + animation + "_figatree";
    for (const AnimationClip& clip : authoredAnimationClips(def)) {
        if (clip.generatedFallback) {
            continue;
        }
        if (clip.name.size() >= suffix.size() &&
            clip.name.compare(clip.name.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool cStickSideSmashInput(const FighterRuntime& fighter, const MeleeCommonData& common) {
    return fxAbs(fighter.input.frames[1].cStick.x) < common.dashInputThresholdX3C &&
           fxAbs(fighter.input.frames[0].cStick.x) >= common.dashInputThresholdX3C;
}

static bool sideSmashInput(
    const FighterRuntime& fighter,
    const MeleeCommonData& common,
    int& facing,
    float& angle,
    bool attackPressed)
{
    if (attackPressed &&
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

static int sideSmashActionIndex(
    const FighterDefinition& def,
    const FighterRuntime& fighter,
    const MeleeCommonData& common,
    bool attackPressed)
{
    int facing = fighter.facing;
    float angle = 0.0f;
    if (!sideSmashInput(fighter, common, facing, angle, attackPressed)) {
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

static bool sideTiltInput(const FighterRuntime& fighter, const MeleeCommonData& common, bool attackPressed, float& angle) {
    if (!attackPressed ||
        fighter.input.frames[0].move.x * fighter.facing < common.attackS3StickThresholdX98)
    {
        return false;
    }
    angle = stickAngle(fighter.input.frames[0].move);
    return std::abs(angle) < fxToFloat(common.aerialAttackAngleTanX20);
}

static int sideTiltActionIndex(
    const FighterDefinition& def,
    const FighterRuntime& fighter,
    const MeleeCommonData& common,
    bool attackPressed)
{
    float angle = 0.0f;
    if (!sideTiltInput(fighter, common, attackPressed, angle)) {
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

static bool upSmashInput(const FighterRuntime& fighter, const MeleeCommonData& common, bool attackPressed) {
    return (attackPressed &&
            fighter.input.frames[0].move.y >= common.attackHi4StickThresholdYxCC &&
            fighter.stickYTiltTimer < common.attackHi4StickWindowXD0) ||
           (fighter.input.frames[1].cStick.y < common.attackHi4StickThresholdYxCC &&
            fighter.input.frames[0].cStick.y >= common.attackHi4StickThresholdYxCC);
}

static bool upSmashInputNoStickWindow(const FighterRuntime& fighter, const MeleeCommonData& common, bool attackPressed) {
    return (attackPressed &&
            fighter.input.frames[0].move.y >= common.attackHi4StickThresholdYxCC) ||
           (fighter.input.frames[1].cStick.y < common.attackHi4StickThresholdYxCC &&
            fighter.input.frames[0].cStick.y >= common.attackHi4StickThresholdYxCC);
}

static bool downSmashInput(const FighterRuntime& fighter, const MeleeCommonData& common, bool attackPressed) {
    return (attackPressed &&
            fighter.input.frames[0].move.y <= common.attackLw4StickThresholdYxD4 &&
            fighter.stickYTiltTimer < common.attackLw4StickWindowXD8) ||
           (fighter.input.frames[1].cStick.y > common.attackLw4StickThresholdYxD4 &&
            fighter.input.frames[0].cStick.y <= common.attackLw4StickThresholdYxD4);
}

static bool upTiltInput(const FighterRuntime& fighter, const MeleeCommonData& common, bool attackPressed) {
    return attackPressed &&
           fighter.input.frames[0].move.y >= common.attackHi3StickThresholdYxAC &&
           stickAngle(fighter.input.frames[0].move) > fxToFloat(common.aerialAttackAngleTanX20);
}

static bool downTiltInput(const FighterRuntime& fighter, const MeleeCommonData& common, bool attackPressed) {
    return attackPressed &&
           fighter.input.frames[0].move.y <= common.attackLw3StickThresholdYxB0 &&
           stickAngle(fighter.input.frames[0].move) < -fxToFloat(common.aerialAttackAngleTanX20);
}

static bool ledgeStickActive(Vec2 stick, const MeleeCommonData& common) {
    return fxAbs(stick.x) >= common.cliffOptionStickThresholdX494 ||
           fxAbs(stick.y) >= common.cliffOptionStickThresholdX494;
}

static bool ledgeStickChoosesClimb(const FighterRuntime& fighter, Vec2 stick, const MeleeCommonData& common) {
    const float angle = std::atan2(fxToFloat(stick.y), std::abs(fxToFloat(stick.x)));
    const float cliffAngle = fxToFloat(common.aerialAttackAngleTanX20);
    return angle > cliffAngle ||
           (angle > -cliffAngle && stick.x * fighter.facing >= 0);
}

static bool ledgeCStickAttackInput(const FighterRuntime& fighter, const MeleeCommonData& common) {
    return fighter.input.frames[1].cStick.y < common.cliffCStickAttackThresholdX7F8 &&
           fighter.input.frames[0].cStick.y >= common.cliffCStickAttackThresholdX7F8;
}

static bool ledgeCStickEscapeInput(const FighterRuntime& fighter, const MeleeCommonData& common) {
    return fighter.facing * fighter.input.frames[1].cStick.x < common.cliffCStickEscapeThresholdX7FC &&
           fighter.facing * fighter.input.frames[0].cStick.x >= common.cliffCStickEscapeThresholdX7FC;
}

static bool isSideSmashCondition(InterruptCondition condition) {
    return condition == InterruptCondition::AttackS4HiPressed ||
           condition == InterruptCondition::AttackS4HiSPressed ||
           condition == InterruptCondition::AttackS4Pressed ||
           condition == InterruptCondition::AttackS4LwSPressed ||
           condition == InterruptCondition::AttackS4LwPressed;
}

static bool usesPreTurnFacingForTurnInterrupt(InterruptCondition condition) {
    switch (condition) {
        case InterruptCondition::GrabPressed:
        case InterruptCondition::SpecialSInput:
        case InterruptCondition::SpecialHiInput:
        case InterruptCondition::SpecialLwInput:
        case InterruptCondition::AttackPressed:
        case InterruptCondition::AttackDashPressed:
        case InterruptCondition::AttackS4HiPressed:
        case InterruptCondition::AttackS4HiSPressed:
        case InterruptCondition::AttackS4Pressed:
        case InterruptCondition::AttackS4LwSPressed:
        case InterruptCondition::AttackS4LwPressed:
        case InterruptCondition::AttackS42Pressed:
        case InterruptCondition::AttackHi4Pressed:
        case InterruptCondition::AttackHi4NoStickWindowPressed:
        case InterruptCondition::AttackLw4Pressed:
        case InterruptCondition::AttackS3HiPressed:
        case InterruptCondition::AttackS3HiSPressed:
        case InterruptCondition::AttackS3Pressed:
        case InterruptCondition::AttackS3LwSPressed:
        case InterruptCondition::AttackS3LwPressed:
        case InterruptCondition::AttackHi3Pressed:
        case InterruptCondition::AttackLw3Pressed:
            return true;
        default:
            return false;
    }
}

static bool platformDropInputActive(const World& world, const FighterRuntime& fighter, const MeleeCommonData& common) {
    if (!fighter.grounded || fighter.groundSegment < 0 ||
        fighter.groundSegment >= static_cast<int>(world.stage.segments.size()))
    {
        return false;
    }
    return world.stage.segments[static_cast<size_t>(fighter.groundSegment)].type == SegmentType::Semisolid &&
           fighter.input.frames[0].move.y <= -common.platformDropStickThresholdX464 &&
           fighter.stickYTiltTimer < common.platformDropStickWindowX468;
}

static bool conditionMet(const World& world, InterruptCondition condition, const FighterRuntime& fighter) {
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const FighterProperties& attr = def.properties;
    const MeleeCommonData& common = attr.common;
    const Fix x = fighter.input.frames[0].move.x;
    const std::string& stateName = currentState(world, fighter).name;
    const bool attackPressed =
        fighter.input.justPressed(ButtonAttack) ||
        (stateName == "Turn" &&
         fighter.turnJustTurned &&
         (fighter.turnBufferedButtons & ButtonAttack) != 0);
    if (stateName == "Run" &&
        fighter.runDirectTimer > 1 &&
        (condition == InterruptCondition::TurnRunInput ||
         condition == InterruptCondition::RunBrakeInput))
    {
        return false;
    }
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
            return attackPressed;
        case InterruptCondition::JabFollowupPressed:
            return attackPressed && fighter.jabFollowupEnabled;
        case InterruptCondition::RapidJabReady:
            return stateName.rfind("Attack1", 0) == 0 &&
                   fighter.rapidJabEnabled &&
                   fighter.attackRapidInputCount >= attr.rapidJabWindow &&
                   hasActionClip(def, 49) &&
                   hasActionClip(def, 50) &&
                   hasActionClip(def, 51);
        case InterruptCondition::SpecialSInput:
            return fighter.input.justPressed(ButtonSpecial) &&
                   fxAbs(fighter.input.frames[0].move.x) >= common.specialSStickThresholdX218 &&
                   hasActionClipNamed(def, "SpecialS");
        case InterruptCondition::SpecialHiInput:
            return fighter.input.justPressed(ButtonSpecial) &&
                   fighter.input.frames[0].move.y >= common.specialLwHiStickThresholdX21C &&
                   hasActionClipNamed(def, "SpecialHi");
        case InterruptCondition::SpecialNInput:
            return fighter.input.justPressed(ButtonSpecial) &&
                   fxAbs(fighter.input.frames[0].move.x) < common.specialSStickThresholdX218 &&
                   fxAbs(fighter.input.frames[0].move.y) < common.specialLwHiStickThresholdX21C &&
                   hasActionClipNamed(def, "SpecialN");
        case InterruptCondition::SpecialLwInput:
            return fighter.input.justPressed(ButtonSpecial) &&
                   fighter.input.frames[0].move.y < -common.specialLwHiStickThresholdX21C &&
                   hasActionClipNamed(def, "SpecialLw");
        case InterruptCondition::SpecialAirHiInput:
            return fighter.input.justPressed(ButtonSpecial) &&
                   fighter.input.frames[0].move.y >= common.specialLwHiStickThresholdX21C &&
                   hasActionClipNamed(def, "SpecialAirHi");
        case InterruptCondition::SpecialAirLwInput:
            return fighter.input.justPressed(ButtonSpecial) &&
                   fighter.input.frames[0].move.y <= -common.specialLwHiStickThresholdX21C &&
                   hasActionClipNamed(def, "SpecialAirLw");
        case InterruptCondition::SpecialAirSInput:
            return fighter.input.justPressed(ButtonSpecial) &&
                   fxAbs(fighter.input.frames[0].move.x) >= common.specialSStickThresholdX218 &&
                   hasActionClipNamed(def, "SpecialSAir");
        case InterruptCondition::SpecialAirNInput:
            return fighter.input.justPressed(ButtonSpecial) &&
                   fxAbs(fighter.input.frames[0].move.x) < common.specialSStickThresholdX218 &&
                   fxAbs(fighter.input.frames[0].move.y) < common.specialLwHiStickThresholdX21C &&
                   hasActionClipNamed(def, "SpecialAirN");
        case InterruptCondition::AttackDashPressed:
            return attackPressed;
        case InterruptCondition::AttackDashGrabBuffer:
            return stateName == "AttackDash" &&
                   fighter.attackDashGrabBufferTimer > 0 &&
                   fighter.grounded &&
                   fighter.shieldHealth > 0 &&
                   fighter.input.down(ButtonShield);
        case InterruptCondition::AttackS4HiPressed:
            return sideSmashActionIndex(def, fighter, common, attackPressed) == 60;
        case InterruptCondition::AttackS4HiSPressed:
            return sideSmashActionIndex(def, fighter, common, attackPressed) == 61;
        case InterruptCondition::AttackS4Pressed:
            return sideSmashActionIndex(def, fighter, common, attackPressed) == 62;
        case InterruptCondition::AttackS4LwSPressed:
            return sideSmashActionIndex(def, fighter, common, attackPressed) == 63;
        case InterruptCondition::AttackS4LwPressed:
            return sideSmashActionIndex(def, fighter, common, attackPressed) == 64;
        case InterruptCondition::AttackS42Pressed:
            return stateName.rfind("AttackS4", 0) == 0 &&
                   fighterCommandFlag(fighter, 0) &&
                   attackPressed &&
                   (def.name == "Link" || def.name == "Young Link") &&
                   hasActionClip(def, 295);
        case InterruptCondition::AttackHi4Pressed:
            return upSmashInput(fighter, common, attackPressed);
        case InterruptCondition::AttackHi4NoStickWindowPressed:
            return upSmashInputNoStickWindow(fighter, common, attackPressed);
        case InterruptCondition::AttackLw4Pressed:
            return downSmashInput(fighter, common, attackPressed);
        case InterruptCondition::AttackS3HiPressed:
            return sideTiltActionIndex(def, fighter, common, attackPressed) == 53;
        case InterruptCondition::AttackS3HiSPressed:
            return sideTiltActionIndex(def, fighter, common, attackPressed) == 54;
        case InterruptCondition::AttackS3Pressed:
            return sideTiltActionIndex(def, fighter, common, attackPressed) == 55;
        case InterruptCondition::AttackS3LwSPressed:
            return sideTiltActionIndex(def, fighter, common, attackPressed) == 56;
        case InterruptCondition::AttackS3LwPressed:
            return sideTiltActionIndex(def, fighter, common, attackPressed) == 57;
        case InterruptCondition::AttackHi3Pressed:
            return upTiltInput(fighter, common, attackPressed);
        case InterruptCondition::AttackLw3Pressed:
            return downTiltInput(fighter, common, attackPressed);
        case InterruptCondition::AttackLw3Repeat:
            return stateName == "AttackLw3" &&
                   fighterCommandFlag(fighter, 0) &&
                   (fighter.attackLw3RepeatQueued || attackPressed);
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
            if (stateName == "SquatWait" &&
                (fighter.platformDropTimer > 0 || platformDropInputActive(world, fighter, common)))
            {
                return false;
            }
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
        case InterruptCondition::RunBrakeTurnRunInput:
            return fighterCommandFlag(fighter, 0) &&
                   x * fighter.facing <= common.turnRunInputThresholdX38;
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
        case InterruptCondition::GuardCatchDashPressed:
            return (stateName == "GuardOn" || stateName == "GuardReflect") &&
                   fighter.guardCatchDashBufferTimer > 0 &&
                   fighter.input.justPressed(ButtonAttack);
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
                   ledgeStickChoosesClimb(fighter, fighter.input.frames[0].move, common);
        case InterruptCondition::LedgeDropInput:
            return fighter.grabbedLedge >= 0 &&
                   fighter.ledgeActionReady &&
                   ((ledgeStickActive(fighter.input.frames[0].move, common) &&
                     !ledgeStickChoosesClimb(fighter, fighter.input.frames[0].move, common)) ||
                    (ledgeStickActive(fighter.input.frames[0].cStick, common) &&
                     !ledgeStickChoosesClimb(fighter, fighter.input.frames[0].cStick, common)));
        case InterruptCondition::LedgeAttackInput:
            return fighter.grabbedLedge >= 0 &&
                   (fighter.input.justPressed(ButtonAttack | ButtonSpecial) ||
                    ledgeCStickAttackInput(fighter, common));
        case InterruptCondition::LedgeEscapeInput:
            return fighter.grabbedLedge >= 0 &&
                   (fighter.input.justPressed(ButtonShield) ||
                    ledgeCStickEscapeInput(fighter, common));
        case InterruptCondition::GrabPressed:
            return fighter.grounded &&
                   (fighter.input.justPressed(ButtonGrab) ||
                    (fighter.input.down(ButtonShield) && fighter.input.justPressed(ButtonAttack)));
        case InterruptCondition::TauntPressed:
            return fighter.grounded && fighter.input.justPressed(ButtonTaunt);
        case InterruptCondition::PackageVarAtLeast:
            return false;
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
    if (animation == "FallF") return 21;
    if (animation == "FallB") return 22;
    if (animation == "FallAerial") return 23;
    if (animation == "FallAerialF") return 24;
    if (animation == "FallAerialB") return 25;
    if (animation == "FallSpecial") return 26;
    if (animation == "FallSpecialF") return 27;
    if (animation == "FallSpecialB") return 28;
    if (animation == "DamageFall") return 29;
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
    if (animation == "Rebound") return 45;
    if (animation == "ItemScrew") return 144;
    if (animation == "ItemScrewAir") return 145;
    if (animation == "ItemScrewDamage") return 146;
    if (animation == "ItemScrewDamageAir") return 147;
    if (animation == "Jab" || animation == "Attack11") return 46;
    if (animation == "Attack12") return 47;
    if (animation == "Attack13") return 48;
    if (animation == "Attack100Start") return 49;
    if (animation == "Attack100Loop") return 50;
    if (animation == "Attack100End") return 51;
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
    if (animation == "AttackS42") return 295;
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
    if (animation == "DamageHi1") return 165;
    if (animation == "DamageHi2") return 166;
    if (animation == "DamageHi3") return 167;
    if (animation == "DamageN1") return 168;
    if (animation == "DamageN2") return 169;
    if (animation == "DamageN3") return 170;
    if (animation == "DamageLw1") return 171;
    if (animation == "DamageLw2") return 172;
    if (animation == "DamageLw3") return 173;
    if (animation == "DamageAir1") return 174;
    if (animation == "DamageAir2") return 175;
    if (animation == "DamageAir3") return 176;
    if (animation == "DamageFlyHi") return 177;
    if (animation == "DamageFlyN") return 178;
    if (animation == "DamageFlyLw") return 179;
    if (animation == "DamageFlyTop") return 180;
    if (animation == "DamageFlyRoll") return 181;
    if (animation == "DownBoundU") return 183;
    if (animation == "DownWaitU") return 184;
    if (animation == "DownDamageU") return 185;
    if (animation == "DownBoundD") return 191;
    if (animation == "DownWaitD") return 192;
    if (animation == "DownDamageD") return 193;
    if (animation == "DownReflect") return 335;
    if (animation == "Passive") return 199;
    if (animation == "PassiveStandF") return 200;
    if (animation == "PassiveStandB") return 201;
    if (animation == "PassiveWall") return 202;
    if (animation == "PassiveWallJump") return 203;
    if (animation == "PassiveCeil") return 204;
    if (animation == "Furafura") return 205;
    if (animation == "FuraSleepStart") return 206;
    if (animation == "FuraSleepLoop") return 207;
    if (animation == "FuraSleepEnd") return 208;
    if (animation == "ShieldBreakFly") return 286;
    if (animation == "ShieldBreakFall") return 287;
    if (animation == "ShieldBreakDown" || animation == "ShieldBreakDownU") return 288;
    if (animation == "ShieldBreakDownD") return 289;
    if (animation == "ShieldBreakStand" || animation == "ShieldBreakStandU") return 290;
    if (animation == "ShieldBreakStandD") return 291;
    if (animation == "BuryJump") return 16;
    if (animation == "Catch") return 242;
    if (animation == "CatchPull") return 242;
    if (animation == "CatchDash") return 243;
    if (animation == "CatchDashPull") return 243;
    if (animation == "CatchWait") return 244;
    if (animation == "CatchAttack") return 245;
    if (animation == "CatchCut") return 246;
    if (animation == "ThrowF") return 247;
    if (animation == "ThrowB") return 248;
    if (animation == "ThrowHi") return 249;
    if (animation == "ThrowLw") return 250;
    if (animation == "CapturePulledHi") return 251;
    if (animation == "CaptureWaitHi") return 252;
    if (animation == "CaptureDamageHi") return 253;
    if (animation == "CapturePulledLw") return 254;
    if (animation == "CaptureWaitLw") return 255;
    if (animation == "CaptureDamageLw") return 256;
    if (animation == "CaptureCut") return 257;
    if (animation == "CaptureJump") return 258;
    if (animation == "CaptureNeck") return 259;
    if (animation == "CaptureFoot") return 260;
    if (animation == "ThrownF") return 262;
    if (animation == "ThrownB") return 263;
    if (animation == "ThrownHi") return 264;
    if (animation == "ThrownLw") return 265;
    if (animation == "ThrownLwWomen") return 266;
    if (animation == "ThrownFF") return 272;
    if (animation == "ThrownFB") return 273;
    if (animation == "ThrownFHi") return 274;
    if (animation == "ThrownFLw") return 275;
    if (animation == "CaptureCaptain") return 276;
    if (animation == "ThrownKoopaF") return 279;
    if (animation == "ThrownKoopaB") return 280;
    if (animation == "ThrownKoopaAirF") return 282;
    if (animation == "ThrownKoopaAirB") return 283;
    if (animation == "ThrownMewtwo") return 292;
    if (animation == "ThrownMewtwoAir") return 293;
    if (animation == "Squat") return 30;
    if (animation == "SquatWait") return 31;
    if (animation == "SquatRv") return 34;
    if (animation == "Pass") return 209;
    if (animation == "Ottotto") return 210;
    if (animation == "OttottoWait") return 211;
    if (animation == "PassiveWallJump") return 203;
    if (animation == "WallDamage") return 212;
    if (animation == "StopWall") return 213;
    if (animation == "StopCeil") return 214;
    if (animation == "MissFoot") return 215;
    if (animation == "CliffCatch") return 216;
    if (animation == "CliffWait") return 217;
    if (animation == "CliffClimbSlow") return 219;
    if (animation == "CliffClimbQuick") return 220;
    if (animation == "CliffAttackSlow") return 221;
    if (animation == "CliffAttackQuick") return 222;
    if (animation == "CliffEscapeSlow") return 223;
    if (animation == "CliffEscapeQuick") return 224;
    if (animation == "CliffJumpSlow1") return 225;
    if (animation == "CliffJumpSlow2") return 226;
    if (animation == "CliffJumpQuick1") return 227;
    if (animation == "CliffJumpQuick2") return 228;
    return -1;
}

static const AnimationClip* clipForState(const FighterDefinition& def, const FighterState& state, int actionIndexOverride = -1) {
    const int actionIndex = actionIndexOverride >= 0
        ? actionIndexOverride
        : (state.animationActionIndex >= 0 ? state.animationActionIndex : fallbackActionIndex(state.animation));
    if (const AnimationClip* clip = authoredAnimationClipByActionIndex(def, actionIndex)) {
        return clip;
    }
    const std::string suffix = "_ACTION_" + state.animation + "_figatree";
    for (const AnimationClip& clip : authoredAnimationClips(def)) {
        if (clip.name.size() >= suffix.size() &&
            clip.name.compare(clip.name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return &clip;
        }
        if (clip.name == state.animation) {
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

static AnimationPose blendedPose(const AnimationPose& from, const AnimationPose& to, Fix t, size_t startJoint = 0) {
    AnimationPose result = to;
    const size_t count = std::min(from.joints.size(), to.joints.size());
    for (size_t i = startJoint; i < count; ++i) {
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
    if (!usesShieldPose(state) || !def.hasShieldPose ||
        def.shieldPose.joints.size() != fighter.animationPose.joints.size())
    {
        return;
    }

    AnimationPose target = def.shieldPose;
    if (const AnimationClip* guardClip = authoredAnimationClipByActionIndex(def, 38)) {
        const Fix stickBlend = std::clamp(fighter.guardPoseBlend, Fix{0}, fx(1));
        if (stickBlend > 0) {
            AnimationPose stickPose = evaluateClip(def.authoredSkeleton, *guardClip, fighter.guardPoseFrame);
            extractTransNForModelPose(stickPose);
            target = blendedPose(target, stickPose, stickBlend);
        }
    }

    Fix openingBlend = fx(1);
    if (state.name == "GuardOn" || state.name == "GuardReflect") {
        const int guardOpenFrames = std::max(1, def.properties.common.guardMinHoldFramesX268);
        openingBlend = std::min(fx(1), fxDiv(fx(frameInState(fighter) + 1), fx(guardOpenFrames)));
    }
    fighter.animationPose = blendedPose(fighter.animationPose, target, openingBlend);
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
    if (fighter.modelPartAnimations.empty()) {
        return;
    }
    const std::vector<ModelPartAnimationSet>& sets = authoredModelPartAnimations(def);
    for (size_t partIndex = 0; partIndex < sets.size() && partIndex < fighter.modelPartAnimations.size(); ++partIndex) {
        const int animIndex = fighter.modelPartAnimations[partIndex];
        if (animIndex < 0 || static_cast<size_t>(animIndex) >= sets[partIndex].animations.size()) {
            continue;
        }
        const AnimationClip& clip = sets[partIndex].animations[static_cast<size_t>(animIndex)];
        for (const AnimationTrack& track : clip.tracks) {
            const int joint = track.joint;
            if (joint < 0 || static_cast<size_t>(joint) >= fighter.animationPose.joints.size()) {
                continue;
            }
            applyAnimationChannel(
                fighter.animationPose.joints[static_cast<size_t>(joint)],
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

static int poseJointForCommonPart(const FighterDefinition& def, const FighterRuntime& fighter, int commonPart, int fallback) {
    const int mapped = commonPartBone(def, fighter, commonPart);
    return mapped >= 0 ? mapped : fallback;
}

static Vec3 poseJointTranslation(const AnimationPose& pose, int joint) {
    if (joint >= 0 && static_cast<size_t>(joint) < pose.joints.size()) {
        return pose.joints[static_cast<size_t>(joint)].translation;
    }
    return {};
}

static void setPoseJointTranslation(AnimationPose& pose, int joint, Vec3 translation) {
    if (joint >= 0 && static_cast<size_t>(joint) < pose.joints.size()) {
        pose.joints[static_cast<size_t>(joint)].translation = translation;
    }
}

static void extractTransNForModelPose(AnimationPose& pose) {
    if (pose.joints.size() > 1) {
        pose.joints[1].translation = {};
    }
}

static Vec3 extractMeleeAnimTranslation(const FighterDefinition& def, const FighterRuntime& fighter, const AnimationClip& clip, AnimationPose& pose) {
    constexpr uint32_t kMeleeActionFlagSecondaryRoot = 0x04000000u; // Fighter::x594_b5
    const int transN = poseJointForCommonPart(def, fighter, 1, 1);
    const Vec3 primary = poseJointTranslation(pose, transN);
    if ((clip.actionFlags & kMeleeActionFlagSecondaryRoot) != 0) {
        const int secondaryRoot = commonPartBone(def, fighter, 0x35);
        if (secondaryRoot >= 0) {
            const Vec3 secondary = poseJointTranslation(pose, secondaryRoot);
            setPoseJointTranslation(pose, transN, primary - secondary);
            setPoseJointTranslation(pose, secondaryRoot, {});
            return secondary;
        }
    }
    setPoseJointTranslation(pose, transN, {});
    return primary;
}

static void evaluateNativePoseHurtboxes(const FighterDefinition& def, FighterRuntime& fighter) {
    fighter.poseHurtboxCapsules.clear();
    if (fighter.jointWorldTransforms.empty()) {
        return;
    }
    fighter.poseHurtboxCapsules.reserve(def.hurtboxes.size());
    for (const HurtboxDefinition& hurtbox : def.hurtboxes) {
        if (hurtbox.joint < 0 || static_cast<size_t>(hurtbox.joint) >= fighter.jointWorldTransforms.size()) {
            Capsule capsule;
            capsule.a = boneWorld(fighter, hurtbox.bone, hurtbox.startOffset);
            capsule.b = boneWorld(fighter, hurtbox.bone, hurtbox.endOffset);
            capsule.radius = hurtbox.radius;
            fighter.poseHurtboxCapsules.push_back(capsule);
            continue;
        }
        const JointWorldTransform& transform = fighter.jointWorldTransforms[static_cast<size_t>(hurtbox.joint)];
        Capsule capsule;
        capsule.a = transformPoint(transform, hurtbox.startOffset);
        capsule.b = transformPoint(transform, hurtbox.endOffset);
        capsule.radius = hurtbox.radius;
        fighter.poseHurtboxCapsules.push_back(capsule);
    }
}

static void refreshHsdWorldPose(const FighterDefinition& def, FighterRuntime& fighter) {
    if (!fighterAnimationSkeleton(def)) {
        fighter.jointWorldTransforms.clear();
        fighter.jointWorldPositions.clear();
        fighter.poseHurtboxCapsules.clear();
        return;
    }
    fighter.jointWorldTransforms = fighterWorldTransforms(def, fighter);
    fighter.jointWorldPositions = translationsFromTransforms(fighter.jointWorldTransforms);
    evaluateNativePoseHurtboxes(def, fighter);
}

static bool relativeJointPosition(const FighterRuntime& fighter, int joint, Vec3& out) {
    if (joint < 0 || static_cast<size_t>(joint) >= fighter.jointWorldPositions.size()) {
        return false;
    }
    const Vec3 world = fighter.jointWorldPositions[static_cast<size_t>(joint)];
    out = {world.x - fighter.position.x, world.y - fighter.position.y, world.z};
    return true;
}

static void applyImportedBoneAliases(const FighterDefinition& def, FighterRuntime& fighter) {
    if (fighter.jointWorldPositions.empty()) {
        return;
    }

    // These aliases come from Melee's fighter/model lookup tables exported
    // from Pl*.dat. The decomp resolves common parts through ftParts_GetBoneIndex
    // before touching fp->parts[].joint; keep this as data-table plumbing, not
    // a hand-authored anatomy guess.
    const FighterBoneTable& bones = def.fighterBones;
    Vec3 position{};
    if (relativeJointPosition(fighter, 0, position)) {
        fighter.bones[static_cast<size_t>(BoneId::Hip)].position = position;
    }
    if (relativeJointPosition(fighter, bones.head, position)) {
        fighter.bones[static_cast<size_t>(BoneId::Head)].position = position;
    } else if (relativeJointPosition(fighter, bones.topOfHead, position)) {
        fighter.bones[static_cast<size_t>(BoneId::Head)].position = position;
    }
    if (relativeJointPosition(fighter, bones.leftArm, position)) {
        fighter.bones[static_cast<size_t>(BoneId::HandL)].position = position;
    }
    if (relativeJointPosition(fighter, bones.itemHold, position) ||
        relativeJointPosition(fighter, bones.rightArm, position))
    {
        fighter.bones[static_cast<size_t>(BoneId::HandR)].position = position;
    }
    if (relativeJointPosition(fighter, bones.leftFoot, position) ||
        relativeJointPosition(fighter, bones.leftLeg, position))
    {
        fighter.bones[static_cast<size_t>(BoneId::FootL)].position = position;
    }
    if (relativeJointPosition(fighter, bones.rightFoot, position) ||
        relativeJointPosition(fighter, bones.rightLeg, position))
    {
        fighter.bones[static_cast<size_t>(BoneId::FootR)].position = position;
    }
    if (relativeJointPosition(fighter, bones.shield, position)) {
        fighter.bones[static_cast<size_t>(BoneId::Extra)].position = position;
    }
}

static void evaluatePose(const FighterDefinition& def, const FighterState& state, FighterRuntime& fighter) {
    const AnimationClip* clip = clipForState(def, state, fighter.animationActionIndexOverride);
    if (clip) {
        const std::vector<AnimationJoint>* skeleton = fighterAnimationSkeleton(def);
        if (!skeleton) {
            return;
        }
        constexpr float kHalfPi = 1.57079632679f;
        const AnimationPose previousVisiblePose = fighter.animationPose;
        Fix frame = fighter.animationFrame;
        if (state.loopAnimation && clip->frameCount > 0) {
            const int loopFrames = std::max(1, static_cast<int>(std::round(fxToFloat(clip->frameCount))));
            frame %= fx(loopFrames);
        } else if (fighter.stateAnimationLengthOverride > 0 && clip->frameCount > 0) {
            frame = fxMul(clip->frameCount, fxDiv(fx(frameInState(fighter)), fx(fighter.stateAnimationLengthOverride)));
        }
        fighter.animationPose = evaluateClip(*skeleton, *clip, frame);
        fighter.previousAnimationTransN = fighter.animationTransN;
        fighter.animationTransN = scaledVec3(extractMeleeAnimTranslation(def, fighter, *clip, fighter.animationPose), def.properties.modelScale);
        if (frameInState(fighter) <= 1) {
            fighter.animationTransNOffset = {};
        } else {
            fighter.animationTransNOffset = {
                fighter.animationTransN.x - fighter.previousAnimationTransN.x,
                fighter.animationTransN.y - fighter.previousAnimationTransN.y,
                fighter.animationTransN.z - fighter.previousAnimationTransN.z,
            };
        }
        applyShieldPose(def, state, fighter);
        applyModelPartAnimations(def, fighter);
        if (!fighter.animationPose.joints.empty() && !clipAnimatesTopNYRotation(*clip)) {
            const float facing = fighter.poseFacing >= 0 ? 1.0f : -1.0f;
            fighter.animationPose.joints[0].rotation.y = fxFromFloat(kHalfPi * facing);
            fighter.animationPose.joints[0].useQuaternion = false;
        }
        if (fighter.animationBlendFrames > 0 &&
            previousVisiblePose.joints.size() == fighter.animationPose.joints.size())
        {
            const Fix rate = fighter.animationRate > 0 ? fighter.animationRate : 0;
            fighter.animationBlendElapsed += rate;
            if (fighter.animationBlendElapsed >= fx(fighter.animationBlendFrames)) {
                fighter.animationBlendFrames = 0;
                fighter.animationBlendElapsed = 0;
            } else if (rate > 0) {
                const Fix remaining = fx(fighter.animationBlendFrames) - fighter.animationBlendElapsed;
                const Fix t = fxDiv(rate, rate + remaining);
                fighter.animationPose = blendedPose(previousVisiblePose, fighter.animationPose, t, 1);
            } else {
                fighter.animationPose = previousVisiblePose;
            }
        }
        refreshHsdWorldPose(def, fighter);
    } else if (fighter.animationPose.joints.empty()) {
        fighter.animationPose = {};
        fighter.previousAnimationTransN = fighter.animationTransN;
        fighter.animationTransN = {};
        fighter.animationTransNOffset = {};
        fighter.jointWorldTransforms.clear();
        fighter.jointWorldPositions.clear();
        fighter.poseHurtboxCapsules.clear();
    } else {
        refreshHsdWorldPose(def, fighter);
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

bool previewFighterAnimation(World& world, size_t fighterIndex, int actionIndex, Fix frame) {
    if (fighterIndex >= world.fighters.size()) {
        return false;
    }
    FighterRuntime& fighter = world.fighters[fighterIndex];
    if (fighter.fighterDef < 0 || fighter.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
        return false;
    }
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    if (fighter.state < 0 || fighter.state >= static_cast<int>(def.states.size())) {
        return false;
    }
    if (!clipForState(def, def.states[static_cast<size_t>(fighter.state)], actionIndex)) {
        return false;
    }
    fighter.animationActionIndexOverride = actionIndex;
    fighter.animationFrame = std::max(Fix{0}, frame);
    fighter.animationRate = 0;
    evaluatePose(def, def.states[static_cast<size_t>(fighter.state)], fighter);
    return true;
}

static void applyAnimationGroundVelocity(const FighterState& state, FighterRuntime& fighter) {
    if (!state.useAnimPhysics || !fighter.grounded || frameInState(fighter) <= 1) {
        return;
    }
    const Fix target = fighter.facing * fighter.animationTransNOffset.z;
    fighter.groundAccel = target - fighter.groundVelocity;
    fighter.groundAccelSecondary = 0;
}

static void advanceAnimationFrame(const FighterDefinition& def, const FighterState& state, FighterRuntime& fighter) {
    fighter.animationFrame += fighter.animationRate;
    const AnimationClip* clip = clipForState(def, state, fighter.animationActionIndexOverride);
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

static void projectGroundKnockback(FighterRuntime& fighter) {
    const Vec2 tangent = groundTangent(fighter.groundNormal);
    fighter.knockbackVelocity.x = fxMul(tangent.x, fighter.groundKnockbackVelocity);
    fighter.knockbackVelocity.y = fxMul(tangent.y, fighter.groundKnockbackVelocity);
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

static void updateKnockbackVelocity(const World& world, FighterRuntime& fighter) {
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    if (fighter.knockbackVelocity.x == 0 && fighter.knockbackVelocity.y == 0) {
        fighter.groundKnockbackVelocity = 0;
        fighter.knockbackDecay = {};
        return;
    }

    if (fighter.grounded) {
        if (fighter.groundKnockbackVelocity == 0) {
            fighter.groundKnockbackVelocity = velocityAlongGround(fighter.knockbackVelocity, fighter.groundNormal);
        }
        Fix friction = fxMul(def.properties.grFriction, def.properties.common.groundKnockbackFrictionScaleX200);
        friction = fxMul(friction, groundFrictionMultiplier(world, fighter));
        fighter.groundKnockbackVelocity = fxApproach(fighter.groundKnockbackVelocity, 0, friction);
        projectGroundKnockback(fighter);
        return;
    }

    fighter.groundKnockbackVelocity = 0;
    const float x = fxToFloat(fighter.knockbackVelocity.x);
    const float y = fxToFloat(fighter.knockbackVelocity.y);
    const float len = std::sqrt(x * x + y * y);
    const float decay = fxToFloat(def.properties.common.knockbackFrameDecayX204);
    if (len <= decay || len <= 0.0001f) {
        fighter.knockbackVelocity = {};
        fighter.knockbackDecay = {};
        return;
    }
    const float scale = (len - decay) / len;
    fighter.knockbackVelocity.x = fxFromFloat(x * scale);
    fighter.knockbackVelocity.y = fxFromFloat(y * scale);
    fighter.knockbackDecay.x = fxFromFloat((x / len) * decay);
    fighter.knockbackDecay.y = fxFromFloat((y / len) * decay);
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
    if (!segmentYAtX(segment, bottom.x, y)) {
        fighter.floorSkipSegment = -1;
    }
}

static bool usesFallSpecialPlatformLandingGate(const std::string& stateName) {
    return stateName == "Fall" ||
           stateName == "FallAerial" ||
           stateName == "JumpF" ||
           stateName == "JumpB" ||
           stateName == "JumpAerialF" ||
           stateName == "JumpAerialB" ||
           stateName == "FallSpecial" ||
           stateName == "ItemScrew" ||
           stateName == "ItemScrewAir" ||
           stateName == "PassiveWall" ||
           stateName == "PassiveWallJump" ||
           stateName == "PassiveCeil" ||
           stateName == "StopCeil" ||
           stateName == "DamageIceJump" ||
           stateName == "CliffJumpAir" ||
           stateName == "CliffJumpSlow2" ||
           stateName == "CliffJumpQuick2";
}

static bool canStandOnSegment(const World& world, const FighterRuntime& fighter, const StageSegment& segment, int segmentIndex) {
    if (!isFloorLine(segment)) {
        return false;
    }
    if (fighter.floorSkipSegment == segmentIndex) {
        return false;
    }
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const FighterState& state = def.states[static_cast<size_t>(fighter.state)];
    if (segment.type == SegmentType::Semisolid &&
        usesFallSpecialPlatformLandingGate(state.name))
    {
        return fighter.input.frames[0].move.y > def.properties.common.fallSpecialPlatformStickThresholdX25C;
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
        if (sweepStart.y > sweepEnd.y || !meleeLineIntersectionH(a0, a1.x, sweepEnd, sweepStart, contact)) {
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
    for (size_t i = 0; i < world.stage.segments.size(); ++i) {
        if (static_cast<int>(i) == lineIdSkip) {
            continue;
        }
        const StageSegment& segment = world.stage.segments[i];
        if (!canStandOnSegment(world, fighter, segment, static_cast<int>(i))) {
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
    if (def.authoredSkeleton.empty()) {
        return false;
    }
    const AnimationPose pose = evaluateClip(def.authoredSkeleton, *clip, fighter.animationFrame);
    if (pose.joints.size() <= 1) {
        return false;
    }
    const Vec3 transN = scaledVec3(pose.joints[1].translation, def.properties.modelScale);
    fighter.position.x = ledge.position.x + fighter.facing * transN.z;
    fighter.position.y = ledge.position.y + transN.y;
    return true;
}

static bool ledgeOccupiedByOtherFighter(const World& world, size_t fighterIndex, int ledgeIndex) {
    for (size_t i = 0; i < world.fighters.size(); ++i) {
        if (i != fighterIndex && world.fighters[i].grabbedLedge == ledgeIndex) {
            return true;
        }
    }
    return false;
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
        if (ledgeOccupiedByOtherFighter(world, fighterIndex, static_cast<int>(i))) {
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
        // ftCliffCommon_80081370 calls ftCommon_8007D5D4 during CliffCatch.
        fighter.jumpsUsed = 1;
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

static bool clampToFloorEdge(const World& world, FighterRuntime& fighter, int floorSegmentIndex, int side) {
    if (!validStageSegmentIndex(world, floorSegmentIndex)) {
        return false;
    }

    const StageSegment& floor = world.stage.segments[static_cast<size_t>(floorSegmentIndex)];
    const Vec2 edge = side < 0 ? segmentLeftEndpoint(floor) : segmentRightEndpoint(floor);
    fighter.groundSegment = floorSegmentIndex;
    fighter.groundNormal = segmentNormal(floor);
    fighter.position.x = edge.x - fighter.ecb.points[3].x;
    fighter.position.y = edge.y - fighter.ecb.points[3].y;
    projectGroundVelocity(fighter);
    return true;
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
    const bool pastLeftEdge = fighter.position.x < segmentMinX(floor);
    const bool pastRightEdge = fighter.position.x > segmentMaxX(floor);
    if (pastLeftEdge || pastRightEdge) {
        // Mirrors mpColl_8004B4B0/mpColl_8004A678: once the origin passes an
        // unlinked floor end, leave current-ground handling so the state's
        // collision callback can choose Ottotto or Fall from facing/stick.
        const int side = pastLeftEdge ? -1 : 1;
        const Vec2 edge = side < 0 ? segmentLeftEndpoint(floor) : segmentRightEndpoint(floor);
        const int linkedFloor = linkedSameKindLineAtEndpoint(world, floorSegmentIndex, edge, SegmentLineKind::Floor);
        const int linkedNonFloor = linkedNonFloorLineAtEndpoint(world, floorSegmentIndex, edge);
        const bool blockedByWall = validStageSegmentIndex(world, linkedNonFloor) &&
            (side < 0
                ? effectiveLineKind(world.stage.segments[static_cast<size_t>(linkedNonFloor)]) == SegmentLineKind::RightWall
                : effectiveLineKind(world.stage.segments[static_cast<size_t>(linkedNonFloor)]) == SegmentLineKind::LeftWall);
        if (linkedFloor < 0 && !blockedByWall) {
            // ft_80084104 uses the no-slideoff ground collision helper
            // (mpColl_8004B2DC) for common grounded attacks and escapes.
            // Character-specific exceptions, such as Kirby dash attack, keep
            // allowSlideoff set and use their own ground-to-air callback.
            if (!currentState(world, fighter).allowSlideoff) {
                return clampToFloorEdge(world, fighter, floorSegmentIndex, side);
            }
            return false;
        }
    }
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
        }
    } else if (fighter.position.x > right.x) {
        edge = right;
        const int nonFloor = linkedNonFloorLineAtEndpoint(world, floorSegmentIndex, edge);
        if (validStageSegmentIndex(world, nonFloor) &&
            effectiveLineKind(world.stage.segments[static_cast<size_t>(nonFloor)]) == SegmentLineKind::LeftWall)
        {
            hitWall = true;
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

static Vec3 boneWorld(const FighterRuntime& fighter, BoneId bone, Vec3 offset) {
    Vec3 base = fighter.bones[static_cast<size_t>(bone)].position;
    const Fix facing = fighter.facing >= 0 ? fx(1) : -fx(1);
    offset.x = fxMul(offset.x, facing);
    return {fighter.position.x + base.x + offset.x, fighter.position.y + base.y + offset.y, base.z + offset.z};
}

static Vec3 hitboxWorld(const FighterRuntime& fighter, const HitboxDefinition& hitbox) {
    if (hitbox.joint >= 0 && static_cast<size_t>(hitbox.joint) < fighter.jointWorldTransforms.size()) {
        return transformPoint(fighter.jointWorldTransforms[static_cast<size_t>(hitbox.joint)], hitbox.offset);
    }
    return boneWorld(fighter, hitbox.bone, hitbox.offset);
}

static Vec3 objectHitboxWorld(const GameObjectRuntime& object, const HitboxDefinition& hitbox) {
    return {
        object.position.x + object.facing * hitbox.offset.x,
        object.position.y + hitbox.offset.y,
        hitbox.offset.z,
    };
}

static Capsule objectHurtboxWorld(const GameObjectRuntime& object, const GameObjectHurtboxDefinition& hurtbox) {
    return {
        {
            object.position.x + object.facing * hurtbox.startOffset.x,
            object.position.y + hurtbox.startOffset.y,
            hurtbox.startOffset.z,
        },
        {
            object.position.x + object.facing * hurtbox.endOffset.x,
            object.position.y + hurtbox.endOffset.y,
            hurtbox.endOffset.z,
        },
        hurtbox.radius,
    };
}

static Capsule objectTouchboxWorld(const GameObjectRuntime& object, const GameObjectTouchboxDefinition& touchbox) {
    return {
        {
            object.position.x + object.facing * touchbox.startOffset.x,
            object.position.y + touchbox.startOffset.y,
            touchbox.startOffset.z,
        },
        {
            object.position.x + object.facing * touchbox.endOffset.x,
            object.position.y + touchbox.endOffset.y,
            touchbox.endOffset.z,
        },
        touchbox.radius,
    };
}

static Vec3 shieldCenterWorld(const FighterDefinition& def, const FighterRuntime& fighter) {
    const int shieldBone = def.fighterBones.shield;
    if (shieldBone >= 0 && static_cast<size_t>(shieldBone) < fighter.jointWorldTransforms.size()) {
        return transformPoint(fighter.jointWorldTransforms[static_cast<size_t>(shieldBone)], {});
    }
    return boneWorld(fighter, BoneId::Hip, {0, fxFromFloat(0.2f), 0});
}

static int commonPartBone(const FighterDefinition& def, const FighterRuntime& fighter, int commonPart) {
    if (commonPart < 0) {
        return -1;
    }
    (void)fighter;
    if (commonPart < static_cast<int>(def.commonBoneLookup.size())) {
        const int mapped = def.commonBoneLookup[static_cast<size_t>(commonPart)];
        if (mapped >= 0) {
            return mapped;
        }
    }
    return -1;
}

static Vec3 commonPartWorld(const FighterDefinition& def, const FighterRuntime& fighter, int commonPart) {
    const int bone = commonPartBone(def, fighter, commonPart);
    if (bone >= 0 && static_cast<size_t>(bone) < fighter.jointWorldTransforms.size()) {
        return transformPoint(fighter.jointWorldTransforms[static_cast<size_t>(bone)], {});
    }
    return {fighter.position.x, fighter.position.y, 0};
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

static void applySetJumpStateSubaction(const FighterDefinition& def, FighterRuntime& fighter, uint32_t state) {
    switch (state) {
    case 0:
        // ftCommon_8007D7FC returns the fighter to GA_Ground and restores jumps.
        fighter.grounded = true;
        fighter.groundVelocity = velocityAlongGround(fighter.fighterVelocity, fighter.groundNormal);
        fighter.jumpsUsed = 0;
        fighter.wallJumpsUsed = 0;
        fighter.fighterVelocity = {};
        unlockFighterEcb(fighter);
        break;
    case 1:
        // ftCommon_8007D5D4 enters GA_Air with one jump spent and a 10-frame ECB lock.
        fighter.grounded = false;
        fighter.groundSegment = -1;
        fighter.groundVelocity = 0;
        fighter.groundAccel = 0;
        fighter.groundAccelSecondary = 0;
        fighter.groundAttackerShieldKnockbackVelocity = 0;
        fighter.jumpsUsed = 1;
        lockFighterEcb(fighter, 10);
        break;
    case 2:
        // ftCommon_8007D60C enters GA_Air with every jump spent and a 5-frame ECB lock.
        fighter.grounded = false;
        fighter.groundSegment = -1;
        fighter.groundVelocity = 0;
        fighter.groundAccel = 0;
        fighter.groundAccelSecondary = 0;
        fighter.groundAttackerShieldKnockbackVelocity = 0;
        fighter.jumpsUsed = def.properties.maxJumps;
        lockFighterEcb(fighter, 5);
        break;
    default:
        break;
    }
}

static void executeSubaction(World& world, size_t fighterIndex, const FighterDefinition& def, FighterRuntime& fighter, const Subaction& sub) {
    if (sub.type == SubactionType::CallScript) {
        runPackageScript(world, fighter, sub.objectName);
        return;
    }
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
    if (sub.type == SubactionType::SetHitboxInteraction) {
        // ftAction_80071708 type 0 writes x42_b5, which fighter collision
        // checks before a hit capsule can strike another fighter.
        if (sub.flag == 0) {
            for (ActiveHitbox& active : fighter.activeHitboxes) {
                if (active.def.hitboxId == sub.hitbox.hitboxId) {
                    active.def.hitFighters = sub.flagValue != 0;
                }
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
        setFighterCommandVar(fighter, sub.flag, sub.flagValue);
        return;
    }
    if (sub.type == SubactionType::SetThrowFlag) {
        // ftAction_800718A4 writes action command 0x14 hit_idx 0 to
        // throw_flags_b3 and hit_idx 1 to throw_flags_b4. Keep those bit
        // numbers literal so throw code mirrors ftCo_800DD724's consumers.
        if (sub.flag == 0 || sub.flag == 1) {
            setFighterThrowFlag(fighter, sub.flag + 3, true);
        }
        return;
    }
    if (sub.type == SubactionType::SetThrowFlagLiteral) {
        // ftAction_80071974/908/92C set throw_flags_b0/b1/b2 directly.
        setFighterThrowFlag(fighter, sub.flag, sub.flagValue != 0);
        return;
    }
    if (sub.type == SubactionType::EnableJabFollowup) {
        // ftAction_80071AE8 sets x2218_b1 when disabled == 0. The bunny hood
        // override path depends on item state this engine does not model yet.
        if (sub.flagValue == 0) {
            fighter.jabFollowupEnabled = true;
        }
        return;
    }
    if (sub.type == SubactionType::SetJabRapid) {
        fighter.rapidJabEnabled = sub.flagValue != 0;
        return;
    }
    if (sub.type == SubactionType::SetJumpState) {
        applySetJumpStateSubaction(def, fighter, sub.flagValue);
        return;
    }
    if (sub.type == SubactionType::SetBodyCollisionState) {
        fighter.bodyCollisionState = sub.hurtboxState;
        return;
    }
    if (sub.type == SubactionType::StartSmashCharge) {
        startSmashCharge(fighter, sub.smashChargeHoldFrames, sub.smashChargeDamageMultiplier);
        return;
    }
    if (sub.type == SubactionType::CreateThrowHitbox) {
        if (sub.hitbox.hitboxId >= 0 && sub.hitbox.hitboxId < static_cast<int>(fighter.throwHitboxes.size())) {
            fighter.throwHitboxes[static_cast<size_t>(sub.hitbox.hitboxId)] = sub.hitbox;
            fighter.throwHitboxActive[static_cast<size_t>(sub.hitbox.hitboxId)] = true;
        }
        return;
    }
    if (sub.type == SubactionType::SetModelVisibility) {
        if (sub.modelPartIndex >= 0) {
            ensureModelVisibilityDefaults(def, fighter);
            if (sub.modelPartIndex >= static_cast<int>(fighter.modelVisibilityStates.size())) {
                fighter.modelVisibilityStates.resize(static_cast<size_t>(sub.modelPartIndex + 1), 0);
            }
            fighter.modelVisibilityStates[static_cast<size_t>(sub.modelPartIndex)] = sub.modelPartState;
        }
        return;
    }
    if (sub.type == SubactionType::RevertModelVisibility) {
        // ftParts_80074A8C copies every x5F4_arr.prev into x5F4_arr.idx.
        ensureModelVisibilityDefaults(def, fighter);
        fighter.modelVisibilityStates = fighter.modelVisibilityDefaultStates;
        return;
    }
    if (sub.type == SubactionType::RemoveModelVisibility) {
        // ftParts_80074ACC hides every visibility group by writing idx = -1.
        ensureModelVisibilityDefaults(def, fighter);
        std::fill(fighter.modelVisibilityStates.begin(), fighter.modelVisibilityStates.end(), -1);
        return;
    }
    if (sub.type == SubactionType::SetFighterVisibility) {
        // ftAction_80071FA0 writes x221E_b5; ftDrawCommon skips model display while set.
        fighter.fighterInvisible = sub.flagValue != 0;
        return;
    }
    if (sub.type == SubactionType::SetModelPartAnimation) {
        if (sub.modelPartIndex >= 0) {
            if (fighter.modelPartAnimations.size() != authoredModelPartAnimations(def).size()) {
                fighter.modelPartAnimations.assign(authoredModelPartAnimations(def).size(), -1);
            }
            if (sub.modelPartIndex < static_cast<int>(fighter.modelPartAnimations.size())) {
                fighter.modelPartAnimations[static_cast<size_t>(sub.modelPartIndex)] = sub.modelPartAnimation;
            }
        }
        return;
    }
    if (sub.type == SubactionType::SelfDamage) {
        // ftAction_80072BF4 calls Fighter_TakeDamage_8006CC7C with this signed
        // amount; the current runtime models the percent side of that helper.
        fighter.percent += sub.selfDamage;
        if (fighter.percent > fx(999)) {
            fighter.percent = fx(999);
        }
        return;
    }
    if (sub.type == SubactionType::SpawnObject || sub.type == SubactionType::SpawnProjectile) {
        const Vec2 position{
            fighter.position.x + fighter.facing * sub.spawnOffset.x,
            fighter.position.y + sub.spawnOffset.y,
        };
        const Vec2 velocity{
            fighter.facing * sub.spawnVelocity.x,
            sub.spawnVelocity.y,
        };
        if (sub.type == SubactionType::SpawnProjectile) {
            spawnGameObjectOfKind(world, sub.objectName, GameObjectKind::Projectile, static_cast<int>(fighterIndex), position, fighter.facing, velocity);
        } else {
            spawnGameObject(world, sub.objectName, static_cast<int>(fighterIndex), position, fighter.facing, velocity);
        }
        return;
    }
    if (sub.type == SubactionType::SetHurtboxState) {
        if (sub.joint >= 0) {
            if (fighter.hurtboxStates.size() != def.hurtboxes.size()) {
                fighter.hurtboxStates.assign(def.hurtboxes.size(), HurtboxState::Normal);
            }
            for (size_t i = 0; i < def.hurtboxes.size(); ++i) {
                if (def.hurtboxes[i].joint == sub.joint) {
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
        if (sub.hitbox.requiresThrownHitboxOwner && fighter.thrownHitboxOwner < 0) {
            return;
        }
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

static void executeActionFrame(World& world, size_t fighterIndex, const FighterDefinition& def, const FighterState& state, int actionFrame) {
    const UnfoldedAction action = unfoldAction(state.action);
    if (actionFrame < 0 || actionFrame >= static_cast<int>(action.size())) {
        return;
    }
    if (fighterIndex >= world.fighters.size()) {
        return;
    }
    const int sourceDef = world.fighters[fighterIndex].fighterDef;
    const int sourceState = world.fighters[fighterIndex].state;
    for (const Subaction& sub : action[static_cast<size_t>(actionFrame)]) {
        if (fighterIndex >= world.fighters.size()) {
            return;
        }
        FighterRuntime& fighter = world.fighters[fighterIndex];
        if (fighter.fighterDef != sourceDef || fighter.state != sourceState) {
            return;
        }
        executeSubaction(world, fighterIndex, def, fighter, sub);
    }
}

static int actionFrameForState(const FighterDefinition& def, const FighterState& state, const FighterRuntime& fighter) {
    if (clipForState(def, state)) {
        return std::max(0, static_cast<int>(fxToFloat(fighter.animationFrame)));
    }
    return frameInState(fighter);
}

static void executePendingActionFrames(World& world, size_t fighterIndex, const FighterDefinition& def, const FighterState& state, FighterRuntime& fighter) {
    const int targetFrame = actionFrameForState(def, state, fighter);
    if (targetFrame == fighter.lastActionFrameExecuted) {
        return;
    }
    const int sourceDef = fighter.fighterDef;
    const int sourceState = fighter.state;
    if (targetFrame > fighter.lastActionFrameExecuted) {
        for (int frame = std::max(0, fighter.lastActionFrameExecuted + 1); frame <= targetFrame; ++frame) {
            executeActionFrame(world, fighterIndex, def, state, frame);
            if (fighterIndex >= world.fighters.size() ||
                world.fighters[fighterIndex].fighterDef != sourceDef ||
                world.fighters[fighterIndex].state != sourceState)
            {
                return;
            }
        }
    } else {
        executeActionFrame(world, fighterIndex, def, state, targetFrame);
        if (fighterIndex >= world.fighters.size() ||
            world.fighters[fighterIndex].fighterDef != sourceDef ||
            world.fighters[fighterIndex].state != sourceState)
        {
            return;
        }
    }
    world.fighters[fighterIndex].lastActionFrameExecuted = targetFrame;
}

static void processInterrupts(World& world, FighterRuntime& fighter) {
    const FighterState& state = currentState(world, fighter);
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    if (state.name.rfind("Attack1", 0) == 0 &&
        (fighter.input.justPressed(ButtonAttack) || fighter.input.down(ButtonAttack)))
    {
        ++fighter.attackRapidInputCount;
    }
    for (const InterruptRule& rule : state.interrupts) {
        if (!ruleActive(rule, fighter) || !groundAllowed(rule.ground, fighter)) {
            continue;
        }
        const bool useTurnFacing = state.name == "Turn" &&
            !fighter.turnHasTurned &&
            usesPreTurnFacingForTurnInterrupt(rule.condition);
        const int originalFacing = fighter.facing;
        if (useTurnFacing) {
            fighter.facing = fighter.turnFacingAfter == 0 ? -fighter.facing : fighter.turnFacingAfter;
        }
        const bool packageVarCondition =
            rule.condition == InterruptCondition::PackageVarAtLeast &&
            fighterPackageVar(fighter, rule.packageVariable) >= rule.packageValue;
        if (packageVarCondition || conditionMet(world, rule.condition, fighter)) {
            if (rule.condition == InterruptCondition::DashInput && signOf(fighter.input.frames[0].move.x) != fighter.facing) {
                fighter.facing *= -1;
            }
            if ((rule.condition == InterruptCondition::SpecialSInput ||
                 rule.condition == InterruptCondition::SpecialAirSInput) &&
                fighter.input.frames[0].move.x * fighter.facing < -def.properties.common.specialSReverseThresholdX220)
            {
                fighter.facing *= -1;
            }
            if (isSideSmashCondition(rule.condition)) {
                int desiredFacing = fighter.facing;
                float unusedAngle = 0.0f;
                const bool attackPressed =
                    fighter.input.justPressed(ButtonAttack) ||
                    (state.name == "Turn" &&
                     fighter.turnJustTurned &&
                     (fighter.turnBufferedButtons & ButtonAttack) != 0);
                if (sideSmashInput(fighter, def.properties.common, desiredFacing, unusedAngle, attackPressed)) {
                    fighter.facing = desiredFacing;
                }
            }
            if (state.name == "Dash" &&
                (rule.condition == InterruptCondition::ReverseDashInput ||
                 rule.condition == InterruptCondition::TauntPressed ||
                 rule.condition == InterruptCondition::ShieldReflectInput ||
                 rule.condition == InterruptCondition::ShieldHeld))
            {
                // ftCo_Dash_IASA falls through to x54 decay after reverse dash
                // and after ftCo_800DE9D8 accepts a Dash taunt.
                fighter.groundVelocity -= fxMul(
                    fxMul(fighter.groundVelocity, def.properties.common.dashDecayX54),
                    groundFrictionMultiplier(world, fighter));
                projectGroundVelocity(fighter);
            }
            const bool seedGuardCatchDashBuffer =
                (state.name == "Dash" || state.name == "Run" || state.name == "RunDirect") &&
                (rule.targetState == "GuardOn" || rule.targetState == "GuardReflect");
            const bool preserveGuardCatchDashBuffer =
                state.name == "GuardOn" &&
                rule.targetState == "GuardReflect";
            const int previousGuardCatchDashBuffer = fighter.guardCatchDashBufferTimer;
            changeFighterState(world, fighter, rule.targetState, rule.lagFrames, rule.blendFrames);
            if (seedGuardCatchDashBuffer) {
                // ftCo_Dash/Run IASA call ftCo_80091B9C after entering guard,
                // seeding mv.co.guard.x24 from common x68 for A -> CatchDash.
                fighter.guardCatchDashBufferTimer = def.properties.common.attackDashGrabBufferFramesX68;
            } else if (preserveGuardCatchDashBuffer) {
                fighter.guardCatchDashBufferTimer = previousGuardCatchDashBuffer;
            }
            return;
        }
        if (useTurnFacing) {
            fighter.facing = originalFacing;
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
        state.name == "ShieldBreakDownU" ||
        state.name == "ShieldBreakDownD" ||
        state.name == "ShieldBreakStand" ||
        state.name == "ShieldBreakStandU" ||
        state.name == "ShieldBreakStandD" ||
        state.name == "Furafura";
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

static bool isDamageFlyStateName(const std::string& name) {
    return name == "DamageFlyHi" || name == "DamageFlyN" || name == "DamageFlyLw" ||
           name == "DamageFlyTop" || name == "DamageFlyRoll";
}

static bool isDamageSurfaceStateName(const std::string& name) {
    return name == "WallDamage" ||
           name == "FlyReflectWall" ||
           name == "FlyReflectCeil";
}

static bool isDownDamageStateName(const std::string& name) {
    return name == "DownDamageU" || name == "DownDamageD";
}

static const char* damageWallReflectStateFor(const std::string& stateName) {
    return isDownDamageStateName(stateName) ? "DownReflect" : "FlyReflectWall";
}

static bool damageFlyHitboxIgnoresVictim(const World& world, const FighterRuntime& attacker, size_t victimIndex) {
    return attacker.damageHitboxOwner == static_cast<int>(victimIndex) &&
        isDamageFlyStateName(currentState(world, attacker).name);
}

static bool thrownHitboxIgnoresOwner(const FighterRuntime& attacker, size_t victimIndex) {
    return attacker.thrownHitboxOwner == static_cast<int>(victimIndex);
}

static bool fightersAreInActiveCaptureLink(const FighterRuntime& attacker, size_t attackerIndex, const FighterRuntime& victim, size_t victimIndex) {
    return attacker.grabberFighter == static_cast<int>(victimIndex) ||
        attacker.grabbedFighter == static_cast<int>(victimIndex) ||
        victim.grabberFighter == static_cast<int>(attackerIndex) ||
        victim.grabbedFighter == static_cast<int>(attackerIndex);
}

static bool techPressAtAge(const InputBuffer& input, int age) {
    const InputFrame& current = input.frames[static_cast<size_t>(age)];
    const InputFrame& previous = input.frames[static_cast<size_t>(age + 1)];
    const bool currentPress = (current.buttons & (ButtonShield | ButtonGrab)) != 0 || current.shieldAnalog > 0;
    const bool previousPress = (previous.buttons & (ButtonShield | ButtonGrab)) != 0 || previous.shieldAnalog > 0;
    return currentPress && !previousPress;
}

static bool techPressHasMeleeRepeatGap(const InputBuffer& input, int age, int minGap) {
    for (int previousAge = age + 1; previousAge < InputBuffer::kSize - 1; ++previousAge) {
        if (techPressAtAge(input, previousAge)) {
            return previousAge - age - 1 >= minGap;
        }
    }
    return true;
}

static bool recentTechInput(const FighterRuntime& fighter, const MeleeCommonData& common) {
    const int maxAge = std::min(std::max(0, common.passiveInputWindowX250 - 1), InputBuffer::kSize - 2);
    for (int age = 0; age <= maxAge; ++age) {
        if (techPressAtAge(fighter.input, age)) {
            return techPressHasMeleeRepeatGap(fighter.input, age, common.inputRepeatWindowX1C);
        }
    }
    return false;
}

static void clearDamageVelocity(FighterRuntime& fighter) {
    fighter.fighterVelocity = {};
    fighter.knockbackVelocity = {};
    fighter.knockbackDecay = {};
    fighter.attackerShieldKnockback = {};
    fighter.groundVelocity = 0;
    fighter.groundKnockbackVelocity = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.hitstun = 0;
    fighter.damageTumble = false;
}

static void reflectDamageVelocity(FighterRuntime& fighter, Vec2 normal, Fix damping) {
    const Fix lenSq = fxMul(normal.x, normal.x) + fxMul(normal.y, normal.y);
    if (lenSq <= 0) {
        return;
    }
    const Fix invLen = fxDiv(fx(1), fxFromFloat(std::sqrt(fxToFloat(lenSq))));
    normal.x = fxMul(normal.x, invLen);
    normal.y = fxMul(normal.y, invLen);
    const Fix dotVel = fxMul(fighter.knockbackVelocity.x, normal.x) + fxMul(fighter.knockbackVelocity.y, normal.y);
    if (dotVel < 0) {
        fighter.knockbackVelocity.x = fxMul(fighter.knockbackVelocity.x - fxMul(fx(2), fxMul(dotVel, normal.x)), damping);
        fighter.knockbackVelocity.y = fxMul(fighter.knockbackVelocity.y - fxMul(fx(2), fxMul(dotVel, normal.y)), damping);
    } else {
        fighter.knockbackVelocity.x = fxMul(fighter.knockbackVelocity.x, damping);
        fighter.knockbackVelocity.y = fxMul(fighter.knockbackVelocity.y, damping);
    }
    const Fix dotDecay = fxMul(fighter.knockbackDecay.x, normal.x) + fxMul(fighter.knockbackDecay.y, normal.y);
    if (dotDecay < 0) {
        fighter.knockbackDecay.x = fighter.knockbackDecay.x - fxMul(fx(2), fxMul(dotDecay, normal.x));
        fighter.knockbackDecay.y = fighter.knockbackDecay.y - fxMul(fx(2), fxMul(dotDecay, normal.y));
    }
}

static bool handleDamageSurfaceContact(World& world, FighterRuntime& fighter, Vec2 normal, const char* damageState, const char* techState) {
    const std::string& stateName = currentState(world, fighter).name;
    const bool downDamageWallContact =
        isDownDamageStateName(stateName) && std::strcmp(damageState, "DownReflect") == 0;
    if ((!fighter.damageTumble && !downDamageWallContact) ||
        (!isDamageFlyStateName(stateName) && !isDamageSurfaceStateName(stateName) && !downDamageWallContact))
    {
        return false;
    }
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    if (isDamageSurfaceStateName(stateName) &&
        fighter.damageSurfaceTimer < def.properties.common.damageSurfaceLockoutX1C0)
    {
        return false;
    }
    if (normal.x != 0 &&
        fxAbs(fighter.knockbackVelocity.x) <= def.properties.common.damageWallBounceMinVelocityX1B0)
    {
        return false;
    }
    if (normal.y < 0 &&
        fighter.knockbackVelocity.y <= def.properties.common.damageWallBounceMinVelocityX1B0)
    {
        return false;
    }
    if (recentTechInput(fighter, def.properties.common)) {
        const InputFrame& input = fighter.input.frames[0];
        const bool wallTechJump =
            std::strcmp(techState, "PassiveWall") == 0 &&
            (fighter.input.justPressed(ButtonJump) ||
             fxAbs(input.move.x) >= def.properties.common.wallJumpStickThresholdX76C ||
             input.move.y >= def.properties.common.wallJumpStickThresholdX76C ||
             fxAbs(input.cStick.x) >= def.properties.common.wallJumpStickThresholdX76C ||
             input.cStick.y >= def.properties.common.wallJumpStickThresholdX76C);
        clearDamageVelocity(fighter);
        changeFighterState(world, fighter, wallTechJump ? "PassiveWallJump" : techState);
        return true;
    }
    const bool floorLike = normal.y > fxFromFloat(0.5f);
    reflectDamageVelocity(
        fighter,
        normal,
        floorLike ? def.properties.common.damageGroundBounceDampingX1EC
                  : def.properties.common.damageWallBounceDampingX1BC);
    fighter.damageSurfaceTimer = 0;
    changeFighterState(world, fighter, damageState);
    return true;
}

static bool shouldStartTeeterFromRunoff(
    const FighterRuntime& fighter,
    const StageSegment& previousSegment,
    int side)
{
    const Fix edgeX = side < 0 ? segmentMinX(previousSegment) : segmentMaxX(previousSegment);
    const bool pastEdge = side < 0 ? fighter.position.x <= edgeX : fighter.position.x >= edgeX;
    if (!pastEdge || fighter.facing != side) {
        return false;
    }

    return fighter.input.frames[0].move.x * side < fxFromFloat(0.75f);
}

static bool shouldEnterStopWall(const World& world, const FighterRuntime& fighter, Fix preWallGroundVelocity) {
    if (!fighter.grounded || fighter.wallContactSide == 0 || fighter.facing != fighter.wallContactSide) {
        return false;
    }
    const std::string& stateName = currentState(world, fighter).name;
    if (stateName != "Dash" && stateName != "Run") {
        return false;
    }
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    return fxAbs(preWallGroundVelocity) > def.properties.walkMaxVel;
}

static void clearKineticVelocity(FighterRuntime& fighter) {
    fighter.fighterVelocity = {};
    fighter.knockbackVelocity = {};
    fighter.attackerShieldKnockback = {};
    fighter.groundVelocity = 0;
    fighter.groundKnockbackVelocity = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
}

static bool touchesCurrentFloorEdge(const World& world, const FighterRuntime& fighter) {
    if (!validStageSegmentIndex(world, fighter.groundSegment)) {
        return false;
    }

    const StageSegment& floor = world.stage.segments[static_cast<size_t>(fighter.groundSegment)];
    constexpr Fix kEdgeTolerance = 32;
    return fxAbs(fighter.position.x - segmentMinX(floor)) <= kEdgeTolerance ||
           fxAbs(fighter.position.x - segmentMaxX(floor)) <= kEdgeTolerance;
}

static void handleGroundedEdgeContact(World& world, FighterRuntime& fighter) {
    if (currentState(world, fighter).name == "TurnRun" && touchesCurrentFloorEdge(world, fighter)) {
        clearKineticVelocity(fighter);
    }
}

static void enterStopWall(World& world, FighterRuntime& fighter) {
    clearKineticVelocity(fighter);
    changeFighterState(world, fighter, "StopWall");
}

static bool shouldEnterStopCeil(const World& world, const FighterRuntime& fighter) {
    const std::string& stateName = currentState(world, fighter).name;
    return stateName == "JumpF" ||
           stateName == "JumpB" ||
           stateName == "JumpAerialF" ||
           stateName == "JumpAerialB" ||
           stateName == "CliffJumpAir" ||
           stateName == "CliffJumpSlow2" ||
           stateName == "CliffJumpQuick2";
}

static void enterStopCeil(World& world, FighterRuntime& fighter) {
    if (fighter.fighterVelocity.y > 0) {
        fighter.fighterVelocity.y = 0;
    }
    if (fighter.knockbackVelocity.y > 0) {
        fighter.knockbackVelocity.y = 0;
    }
    if (fighter.attackerShieldKnockback.y > 0) {
        fighter.attackerShieldKnockback.y = 0;
    }
    changeFighterState(world, fighter, "StopCeil");
}

static bool collideCurrentStep(World& world, size_t fighterIndex, bool wasGrounded, int previousGroundSegment, Vec2 attemptedDelta) {
    FighterRuntime& fighter = world.fighters[fighterIndex];
    bool nowGrounded = false;
    int landedSegment = -1;
    Vec2 landingContact = {};
    Fix landingFraction = fx(1);
    bool resolvedAirCollision = false;
    bool hitCeilingThisStep = false;
    bool floorContactWithoutGround = false;

    if (wasGrounded) {
        nowGrounded = snapToCurrentGround(world, fighter);
        landedSegment = nowGrounded ? fighter.groundSegment : -1;
        if (!nowGrounded && previousGroundSegment >= 0 &&
            previousGroundSegment < static_cast<int>(world.stage.segments.size()) &&
            fighter.floorSkipSegment != previousGroundSegment)
        {
            const StageSegment& previousSegment = world.stage.segments[static_cast<size_t>(previousGroundSegment)];
            if (fighter.position.x < segmentMinX(previousSegment)) {
                fighter.runoffSegment = previousGroundSegment;
                fighter.runoffDirection = -1;
            } else if (fighter.position.x > segmentMaxX(previousSegment)) {
                fighter.runoffSegment = previousGroundSegment;
                fighter.runoffDirection = 1;
            }
        }
    }

    if (!wasGrounded && !nowGrounded) {
        hitCeilingThisStep = resolveWallAndCeiling(world, fighter, attemptedDelta);
        resolvedAirCollision = true;
        if (hitCeilingThisStep && shouldEnterStopCeil(world, fighter)) {
            enterStopCeil(world, fighter);
            return true;
        }
        if (hitCeilingThisStep &&
            handleDamageSurfaceContact(world, fighter, {0, -fx(1)}, "FlyReflectCeil", "PassiveCeil"))
        {
            return true;
        }
        if (fighter.wallContactSide != 0 &&
            handleDamageSurfaceContact(
                world,
                fighter,
                {-fighter.wallContactSide * fx(1), 0},
                damageWallReflectStateFor(currentState(world, fighter).name),
                "PassiveWall"))
        {
            return true;
        }
    }

    if (!nowGrounded && !hitCeilingThisStep) {
        const Vec2 previousBottom = fighter.previousPosition + fighter.previousEcb.points[3];
        const Vec2 currentBottom = fighter.position + fighter.ecb.points[3];
        const int landingSkip = wasGrounded ? previousGroundSegment : -1;
        landedSegment = findLandingSegment(world, fighter, previousBottom, currentBottom, landingContact, landingFraction, landingSkip);
        if (landedSegment >= 0 && attemptedDelta.y <= fx(0)) {
            const StageSegment& segment = world.stage.segments[static_cast<size_t>(landedSegment)];
            fighter.lastLandingVelocityY = attemptedDelta.y;
            fighter.groundNormal = segmentNormal(segment);
            fighter.groundVelocity = velocityAlongGround(fighter.fighterVelocity, fighter.groundNormal);
            fighter.position.x = landingContact.x - fighter.ecb.points[3].x;
            fighter.position.y = landingContact.y - fighter.ecb.points[3].y;
            landedSegment = carryRemainingMovementOnGround(world, fighter, landedSegment, landingFraction, attemptedDelta);
            projectGroundVelocity(fighter);
            if (currentState(world, fighter).convertFloorCollisionToGround) {
                nowGrounded = true;
            } else {
                floorContactWithoutGround = true;
                resolvedAirCollision = true;
            }
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
        handleGroundedEdgeContact(world, fighter);
        if (!wasGrounded) {
            if (!fighter.activeHitboxes.empty()) {
                updateAndCheckHitboxes(world, fighterIndex);
            }
            unlockFighterEcb(fighter);
            fighter.platformDropTimer = 0;
            fighter.jumpsUsed = 0;
            refreshEcbMetadata(fighter.ecb, fighter);
            runStateFunctions(world, fighterIndex, currentState(world, fighter).onLanding);
        }
    } else {
        const bool grabbedLedge = floorContactWithoutGround ? false : tryGrabLedge(world, fighterIndex);
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
        const Fix preWallGroundVelocity = fighter.groundVelocity;
        resolveWallAndCeiling(world, fighter, attemptedDelta);
        if (shouldEnterStopWall(world, fighter, preWallGroundVelocity)) {
            enterStopWall(world, fighter);
        } else if (fighter.wallContactSide != 0) {
            handleDamageSurfaceContact(
                world,
                fighter,
                {-fighter.wallContactSide * fx(1), 0},
                damageWallReflectStateFor(currentState(world, fighter).name),
                "PassiveWall");
        }
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
    updateKnockbackVelocity(world, fighter);
}

uint32_t nextWorldRandom(World& world) {
    uint32_t x = world.rngState == 0 ? 0x4D454C45 : world.rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    world.rngState = x;
    return x;
}

int32_t nextWorldRandomBounded(World& world, int32_t upperExclusive) {
    if (upperExclusive <= 0) {
        return 0;
    }
    return static_cast<int32_t>(nextWorldRandom(world) % static_cast<uint32_t>(upperExclusive));
}

static Fix nextRandomUnit(World& world) {
    return fxFromFloat(static_cast<float>(nextWorldRandom(world) & 0x00FFFFFF) / static_cast<float>(0x01000000));
}

static Fix calculateKnockbackWithWeight(const HitboxDefinition& hitbox, const MeleeCommonData& common, Fix victimPercent, Fix weight) {
    const Fix weighted = fxMul(weight, common.knockbackWeightScaleXF4);
    Fix decay = common.knockbackWeightDecayXF8;
    if (weighted + fx(1) != 0) {
        decay -= fxDiv(fxMul(weighted, decay), fx(1) + weighted);
    }

    Fix knockback = 0;
    if (hitbox.knockbackWeightSet > 0) {
        knockback = fxMul(common.knockbackWeightSetScaleX118, hitbox.knockbackWeightSet);
        knockback = fxMul(common.knockbackWeightSetScaleX118, common.knockbackDamageBaseX110) +
            fxMul(common.knockbackDamageScaleX114, knockback);
    } else {
        const Fix damage = fx(static_cast<int>(fxToFloat(victimPercent))) + hitbox.damage;
        knockback = fxMul(hitbox.damage, damage);
        knockback = fxMul(common.knockbackDamageBaseX110, damage) +
            fxMul(common.knockbackDamageScaleX114, knockback);
    }

    knockback = fxMul(decay, knockback);
    knockback = fxMul(common.knockbackScaleX11C, knockback) + common.knockbackBaseX120;
    knockback = fxMul(fxMul(hitbox.knockbackGrowth, fxFromFloat(0.01f)), knockback) + hitbox.knockbackBase;
    return std::min(knockback, common.knockbackMaxX108);
}

static Fix calculateKnockback(const HitboxDefinition& hitbox, const FighterDefinition& victimDef, const FighterRuntime& victim) {
    return calculateKnockbackWithWeight(hitbox, victimDef.properties.common, victim.percent, victimDef.properties.weight);
}

static Fix calculateThrowKnockback(const HitboxDefinition& hitbox, const FighterDefinition& victimDef, const FighterRuntime& victim) {
    return calculateKnockbackWithWeight(hitbox, victimDef.properties.common, victim.percent, victimDef.properties.common.throwKnockbackWeightX10C);
}

static Fix launchAngleDegrees(const FighterRuntime& victim, const HitboxDefinition& hitbox, const MeleeCommonData& common, Fix knockback, int side) {
    Fix angle = hitbox.knockbackAngleDegrees;
    if (angle == fx(361)) {
        if (victim.grounded && knockback < common.damageSakuraiAngleLowX14C) {
            angle = side >= 0 ? fx(0) : fx(180);
        } else {
            const Fix high = common.damageSakuraiAngleHighX150;
            const Fix span = std::max(Fix{1}, high - common.damageSakuraiAngleLowX14C);
            Fix scaled = fx(1) + fxMul(common.damageSakuraiAngleScaleX148,
                std::clamp(fxDiv(knockback - common.damageSakuraiAngleLowX14C, span), Fix{0}, fx(1)));
            scaled = std::min(scaled, common.damageSakuraiAngleScaleX148);
            if (!victim.grounded) {
                scaled = fxMul(common.damageSakuraiAngleAirX144, fxFromFloat(180.0f / 3.14159265f));
            }
            angle = side >= 0 ? scaled : fx(180) - scaled;
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

static Fix throwLaunchAngleDegrees(const FighterRuntime& victim, const HitboxDefinition& hitbox, const MeleeCommonData& common, Fix knockback) {
    Fix angle = hitbox.knockbackAngleDegrees;
    if (angle != fx(361)) {
        return angle;
    }
    if (!victim.grounded) {
        return fxMul(common.damageSakuraiAngleAirX144, fxFromFloat(180.0f / 3.14159265f));
    }
    if (knockback < common.damageSakuraiAngleLowX14C) {
        return 0;
    }
    const Fix high = common.damageSakuraiAngleHighX150;
    const Fix span = std::max(Fix{1}, high - common.damageSakuraiAngleLowX14C);
    Fix result = fx(1) + fxMul(common.damageSakuraiAngleScaleX148,
        std::clamp(fxDiv(knockback - common.damageSakuraiAngleLowX14C, span), Fix{0}, fx(1)));
    return std::min(result, common.damageSakuraiAngleScaleX148);
}

static Fix applyDirectionalInfluence(const FighterRuntime& victim, const MeleeCommonData& common, Fix angleDegrees, Fix knockback) {
    if (knockback < fx(80) && victim.grounded &&
        (angleDegrees == fx(0) || angleDegrees == fx(180)))
    {
        return angleDegrees;
    }

    Fix stickX = victim.input.frames[0].move.x;
    Fix stickY = victim.input.frames[0].move.y;
    if (fxAbs(stickX) < common.aerialAttackDeadzoneXDC) {
        stickX = 0;
    }
    if (fxAbs(stickY) < common.aerialAttackDeadzoneXE0) {
        stickY = 0;
    }
    if (stickX == 0 && stickY == 0) {
        return angleDegrees;
    }

    const float trajectory = fxToFloat(angleDegrees);
    const float diAngle = std::atan2(fxToFloat(stickY), fxToFloat(stickX)) * 180.0f / 3.14159265f;
    float relative = trajectory - (diAngle < 0.0f ? diAngle + 360.0f : diAngle);
    if (relative > 180.0f) {
        relative -= 360.0f;
    }
    const float stickMag = std::min(1.0f, std::sqrt(std::pow(fxToFloat(stickX), 2.0f) + std::pow(fxToFloat(stickY), 2.0f)));
    float offset = std::sin(relative * 3.14159265f / 180.0f) * stickMag;
    offset = std::min(18.0f, offset * offset * 18.0f) * (relative < 0.0f && relative > -180.0f ? -1.0f : 1.0f);
    return fxFromFloat(std::max(0.0f, trajectory - offset));
}

static int damageLevelFromHitstun(Fix scaledHitstun, const MeleeCommonData& common) {
    if (scaledHitstun < common.damageLevelThresholdX158) return 0;
    if (scaledHitstun < common.damageLevelThresholdX15C) return 1;
    if (scaledHitstun < common.damageLevelThresholdX160) return 2;
    return 3;
}

static int hurtboxRegionForHit(const FighterDefinition& victimDef, size_t hurtboxIndex) {
    if (hurtboxIndex < victimDef.hurtboxes.size() && !victimDef.hurtboxes[hurtboxIndex].type.empty()) {
        const std::string& type = victimDef.hurtboxes[hurtboxIndex].type;
        if (type == "Low") return 0;
        if (type == "High") return 2;
        return 1;
    }
    if (hurtboxIndex == 0) return 1;
    if (hurtboxIndex == 1) return 2;
    return 0;
}

static const char* groundedDamageStateName(int level, int region) {
    static constexpr const char* names[3][3] = {
        {"DamageLw1", "DamageN1", "DamageHi1"},
        {"DamageLw2", "DamageN2", "DamageHi2"},
        {"DamageLw3", "DamageN3", "DamageHi3"},
    };
    return names[std::clamp(level, 0, 2)][std::clamp(region, 0, 2)];
}

static bool proneDamageStateName(const std::string& name) {
    return name == "DownBoundU" || name == "DownWaitU" || name == "DownDamageU" ||
           name == "DownBoundD" || name == "DownWaitD" || name == "DownDamageD";
}

static const char* downDamageStateNameForProneState(const std::string& name) {
    // ftCo_8009F184 only checks DownWaitU; DownBoundU and DownDamageU fall
    // through to DownDamageD despite being face-up states.
    return name == "DownWaitU" ? "DownDamageU" : "DownDamageD";
}

static bool applyWeakProneDamageState(World& world, FighterRuntime& victim, const std::string& victimStateName, Fix damage, const MeleeCommonData& common) {
    if (!victim.grounded || !proneDamageStateName(victimStateName) || damage >= fx(common.downDamageThresholdX428)) {
        return false;
    }

    // ftCo_8009F0F0 catches weak damage to DownBound/DownWait/DownDamage
    // before the ordinary damage state path can launch the prone fighter.
    victim.fighterVelocity = {};
    victim.knockbackVelocity = {};
    victim.knockbackDecay = {};
    victim.groundVelocity = 0;
    victim.groundKnockbackVelocity = 0;
    victim.groundAccel = 0;
    victim.groundAccelSecondary = 0;
    changeFighterState(world, victim, downDamageStateNameForProneState(victimStateName));
    victim.downWaitTimer = std::max(1, victim.hitstun);
    return true;
}

static const char* airDamageStateName(int level) {
    static constexpr const char* names[3] = {"DamageAir1", "DamageAir2", "DamageAir3"};
    return names[std::clamp(level, 0, 2)];
}

static const char* flyDamageStateName(World& world, const FighterRuntime& victim, const MeleeCommonData& common, int region, Fix angleDegrees) {
    if (!victim.grounded) {
        const Fix angleRadians = fxMul(angleDegrees, fxFromFloat(3.14159265f / 180.0f));
        if (angleRadians > common.damageFlyTopAngleMinX234 &&
            angleRadians < common.damageFlyTopAngleMaxX238)
        {
            return "DamageFlyTop";
        }
        if (victim.percent >= fx(common.damageFlyRollPercentX23C) &&
            nextRandomUnit(world) < common.damageFlyRollChanceX240)
        {
            return "DamageFlyRoll";
        }
    }
    static constexpr const char* names[3] = {"DamageFlyLw", "DamageFlyN", "DamageFlyHi"};
    return names[std::clamp(region, 0, 2)];
}

static Fix vectorMagnitude(Vec2 value) {
    return fxFromFloat(std::sqrt(std::pow(fxToFloat(value.x), 2.0f) + std::pow(fxToFloat(value.y), 2.0f)));
}

static Fix angleBetween(Vec2 a, Vec2 b) {
    const float ax = fxToFloat(a.x);
    const float ay = fxToFloat(a.y);
    const float bx = fxToFloat(b.x);
    const float by = fxToFloat(b.y);
    const float lenA = std::sqrt(ax * ax + ay * ay);
    const float lenB = std::sqrt(bx * bx + by * by);
    if (lenA <= 0.0001f || lenB <= 0.0001f) {
        return 0;
    }
    const float dot = std::clamp((ax * bx + ay * by) / (lenA * lenB), -1.0f, 1.0f);
    return fxFromFloat(std::acos(dot));
}

static Fix initialDamageBindTimer(const FighterRuntime& victim, const MeleeCommonData& common, size_t victimIndex) {
    const Fix slot = fx(static_cast<int>(victimIndex) + 1);
    const Fix handicapTerm = fxMul(common.damageBindHandicapScaleX65C, common.damageBindHandicapBaseX660 - fx(9));
    const Fix portTerm = fxMul(common.damageBindPortScaleX664, common.damageBindPortBaseX668 - slot);
    return fxMul(victim.percent, common.damageBindPercentScaleX66C) + common.damageBindBaseX658 + handicapTerm + portTerm;
}

static Fix initialDamageSongTimer(const FighterRuntime& victim, const MeleeCommonData& common, size_t victimIndex, bool useElement7Multiplier) {
    const Fix slot = fx(static_cast<int>(victimIndex) + 1);
    const Fix handicapTerm = fxMul(common.damageSongHandicapScaleX628, common.damageSongHandicapBaseX62C - fx(9));
    const Fix portTerm = fxMul(common.damageSongPortScaleX630, common.damageSongPortBaseX634 - slot);
    Fix timer = fxMul(victim.percent, common.damageSongPercentScaleX638) + common.damageSongBaseX624 + handicapTerm + portTerm;
    if (useElement7Multiplier) {
        timer = fxMul(timer, common.damageSongElement7TimerMultiplierX644);
    }
    return timer;
}

static Fix initialBuryTimer(const FighterRuntime& victim, const MeleeCommonData& common, size_t victimIndex) {
    const Fix slot = fx(static_cast<int>(victimIndex) + 1);
    const Fix handicapTerm = fxMul(common.buryHandicapScaleX5FC, common.buryHandicapBaseX600 - fx(9));
    const Fix portTerm = fxMul(common.buryPortScaleX604, common.buryPortBaseX608 - slot);
    return fxMul(victim.percent, common.buryPercentScaleX60C) + common.buryBaseX5F8 + handicapTerm + portTerm;
}

static void applyHit(World& world, size_t attackerIndex, FighterRuntime& attacker, FighterRuntime& victim, size_t victimIndex, const HitboxDefinition& hitbox, size_t hurtboxIndex) {
    const FighterDefinition& victimDef = world.fighterDefs[static_cast<size_t>(victim.fighterDef)];
    const MeleeCommonData& common = victimDef.properties.common;
    const std::string victimStateName = currentState(world, victim).name;
    const Fix kb = calculateKnockback(hitbox, victimDef, victim);
    const bool wasGrounded = victim.grounded;
    const int side = victim.position.x >= attacker.position.x ? 1 : -1;
    const Fix launchAngle = applyDirectionalInfluence(victim, common, launchAngleDegrees(victim, hitbox, common, kb, side), kb);
    Fix resolvedLaunchAngle = launchAngle;
    const float angle = fxToFloat(resolvedLaunchAngle) * 3.14159265f / 180.0f;
    victim.percent += hitbox.damage;
    const Fix scaledVelocity = fxMul(kb, common.damageVelocityScaleX100);
    Vec2 knockbackVelocity{
        fxFromFloat(std::cos(angle) * fxToFloat(scaledVelocity)),
        fxFromFloat(std::sin(angle) * fxToFloat(scaledVelocity)),
    };
    if (kb < fx(80) && victim.grounded &&
        (hitbox.knockbackAngleDegrees == fx(0) || hitbox.knockbackAngleDegrees == fx(180)))
    {
        knockbackVelocity.y = 0;
        resolvedLaunchAngle = side >= 0 ? fx(0) : fx(180);
    }
    victim.hitlag = std::max(3, static_cast<int>(fxToFloat(hitbox.damage) / 3.0f) + 3);
    attacker.hitlag = victim.hitlag;
    const Fix scaledHitstun = fxMul(kb, common.hitstunMultiplierX154);
    victim.hitstun = std::max(1, static_cast<int>(fxToFloat(scaledHitstun)));
    victim.damageLevel = damageLevelFromHitstun(scaledHitstun, common);
    victim.damageHurtboxRegion = hurtboxRegionForHit(victimDef, hurtboxIndex);
    victim.damageKnockback = kb;
    victim.damageLaunchAngle = resolvedLaunchAngle;
    victim.damageTumble = victim.damageLevel >= 3;
    victim.damageSurfaceTimer = 0;
    victim.damageHitboxOwner = static_cast<int>(attackerIndex);
    victim.fighterVelocity = {};
    victim.groundVelocity = 0;
    victim.groundKnockbackVelocity = 0;
    victim.groundAccel = 0;
    victim.groundAccelSecondary = 0;
    if (victim.grabbedLedge >= 0) {
        victim.grabbedLedge = -1;
        victim.ledgeCooldown = common.ledgeCooldownX498;
    }
    victim.facing = -side;

    const bool shouldFly = victim.damageLevel >= 3;
    bool launchOffGround = !wasGrounded;
    bool groundBounce = false;
    if (wasGrounded) {
        const Vec2 floorNormal = victim.groundNormal;
        const Fix floorAngle = angleBetween(floorNormal, knockbackVelocity);
        if (floorAngle < fxFromFloat(1.57079632679f)) {
            launchOffGround = true;
        } else if (shouldFly && floorAngle > fxFromFloat(1.57079632679f) + common.damageGroundBounceAngleX1E8) {
            knockbackVelocity.y = fxMul(-knockbackVelocity.y, common.damageGroundBounceDampingX1EC);
            resolvedLaunchAngle = fxFromFloat(std::atan2(fxToFloat(knockbackVelocity.y), fxToFloat(knockbackVelocity.x)) * 180.0f / 3.14159265f);
            if (resolvedLaunchAngle < 0) {
                resolvedLaunchAngle += fx(360);
            }
            victim.damageLaunchAngle = resolvedLaunchAngle;
            launchOffGround = true;
            groundBounce = true;
        } else {
            victim.groundKnockbackVelocity = velocityAlongGround(knockbackVelocity, victim.groundNormal);
            knockbackVelocity.x = fxMul(victim.groundNormal.y, victim.groundKnockbackVelocity);
            knockbackVelocity.y = fxMul(-victim.groundNormal.x, victim.groundKnockbackVelocity);
        }
    }
    victim.knockbackVelocity = knockbackVelocity;
    const Fix velocityMag = vectorMagnitude(knockbackVelocity);
    if (velocityMag > 0) {
        victim.knockbackDecay.x = fxMul(fxDiv(knockbackVelocity.x, velocityMag), common.knockbackFrameDecayX204);
        victim.knockbackDecay.y = fxMul(fxDiv(knockbackVelocity.y, velocityMag), common.knockbackFrameDecayX204);
    } else {
        victim.knockbackDecay = {};
    }

    if (hitbox.element == 5 && kb > 0) {
        victim.grabTimer = std::max(Fix{1}, fxMul(hitbox.damage, common.damageIceTimerDamageScaleX790));
        victim.grabMashStickX = 0;
        victim.grabMashStickY = 0;
        victim.captureWaitTimer = 0;
        victim.captureMashAnimTimer = 0;
        victim.captureJumpQueued = false;
        if (wasGrounded && !launchOffGround) {
            victim.grounded = true;
            victim.groundVelocity = victim.groundKnockbackVelocity;
            victim.fighterVelocity = {};
        } else {
            victim.grounded = false;
            victim.groundSegment = -1;
            victim.fighterVelocity = knockbackVelocity;
            victim.groundVelocity = 0;
            victim.groundKnockbackVelocity = 0;
            victim.jumpsUsed = 1;
        }
        victim.knockbackVelocity = {};
        victim.knockbackDecay = {};
        changeFighterState(world, victim, "DamageIce");
        return;
    }

    if (hitbox.element == 12 && kb > 0) {
        victim.grabTimer = std::max(Fix{1}, initialDamageBindTimer(victim, common, victimIndex));
        victim.grabMashStickX = 0;
        victim.grabMashStickY = 0;
        victim.captureWaitTimer = 0;
        victim.captureMashAnimTimer = 0;
        victim.captureJumpQueued = false;
        if (wasGrounded) {
            victim.grounded = true;
            victim.fighterVelocity = {};
            victim.knockbackVelocity = {};
            victim.knockbackDecay = {};
            victim.groundVelocity = 0;
            victim.groundKnockbackVelocity = 0;
            victim.groundAccel = 0;
            victim.groundAccelSecondary = 0;
            changeFighterState(world, victim, "DamageBind");
        } else {
            victim.grounded = false;
            victim.groundSegment = -1;
            changeFighterState(world, victim, "DamageFall");
        }
        return;
    }

    if ((hitbox.element == 6 || hitbox.element == 7) && kb > 0) {
        victim.grabTimer = std::max(Fix{1}, initialDamageSongTimer(victim, common, victimIndex, hitbox.element == 7));
        victim.grabMashStickX = 0;
        victim.grabMashStickY = 0;
        victim.captureWaitTimer = 0;
        victim.captureMashAnimTimer = 0;
        victim.captureJumpQueued = false;
        victim.grounded = true;
        victim.fighterVelocity = {};
        victim.knockbackVelocity = {};
        victim.knockbackDecay = {};
        victim.groundVelocity = 0;
        victim.groundKnockbackVelocity = 0;
        victim.groundAccel = 0;
        victim.groundAccelSecondary = 0;
        changeFighterState(world, victim, "DamageSong");
        return;
    }

    if (hitbox.element == 9 && kb > 0 && wasGrounded) {
        victim.grabTimer = std::max(Fix{1}, initialBuryTimer(victim, common, victimIndex));
        victim.burySubmergeTimer = std::max(1, common.burySubmergeFramesX5F4);
        victim.grabMashStickX = 0;
        victim.grabMashStickY = 0;
        victim.captureWaitTimer = 0;
        victim.captureMashAnimTimer = 0;
        victim.captureJumpQueued = false;
        victim.grounded = true;
        victim.fighterVelocity = {};
        victim.knockbackVelocity = {};
        victim.knockbackDecay = {};
        victim.groundVelocity = 0;
        victim.groundKnockbackVelocity = 0;
        victim.groundAccel = 0;
        victim.groundAccelSecondary = 0;
        victim.hitstun = 0;
        changeFighterState(world, victim, "Bury");
        return;
    }

    if (hitbox.element == 14 && kb > 0) {
        victim.grounded = false;
        victim.groundSegment = -1;
        victim.fighterVelocity.x = 0;
        victim.fighterVelocity.y = victimDef.properties.damageScrewVerticalVelocity;
        victim.knockbackVelocity = {};
        victim.knockbackDecay = {};
        victim.groundVelocity = 0;
        victim.groundKnockbackVelocity = 0;
        victim.groundAccel = 0;
        victim.groundAccelSecondary = 0;
        if (wasGrounded) {
            // ftCo_800D3004 calls ftCommon_8007D5D4 before grounded DamageScrew.
            victim.jumpsUsed = 1;
            lockFighterEcb(victim, 10);
        }
        changeFighterState(world, victim, wasGrounded ? "DamageScrew" : "DamageScrewAir");
        return;
    }

    if (applyWeakProneDamageState(world, victim, victimStateName, hitbox.damage, common)) {
        return;
    }

    if (wasGrounded && launchOffGround) {
        victim.jumpsUsed = 1;
        lockFighterEcb(victim, 10);
    }

    if (shouldFly) {
        victim.grounded = !launchOffGround && !groundBounce;
        if (!victim.grounded) {
            victim.groundSegment = -1;
        }
        changeFighterState(world, victim, flyDamageStateName(world, victim, common, victim.damageHurtboxRegion, resolvedLaunchAngle));
    } else if (launchOffGround) {
        victim.grounded = false;
        victim.groundSegment = -1;
        changeFighterState(world, victim, airDamageStateName(victim.damageLevel));
    } else {
        victim.grounded = wasGrounded;
        changeFighterState(world, victim, groundedDamageStateName(victim.damageLevel, victim.damageHurtboxRegion));
    }
}

static void applyThrowReleaseDamage(World& world, size_t attackerIndex, FighterRuntime& attacker, FighterRuntime& victim, const HitboxDefinition& hitbox, bool forceDamageFlyTopState) {
    const FighterDefinition& victimDef = world.fighterDefs[static_cast<size_t>(victim.fighterDef)];
    const MeleeCommonData& common = victimDef.properties.common;
    const std::string victimStateName = currentState(world, victim).name;
    const Fix kb = calculateThrowKnockback(hitbox, victimDef, victim);
    const bool wasGrounded = victim.grounded;
    const int damageFacing = -attacker.facing;
    Fix resolvedLaunchAngle = throwLaunchAngleDegrees(victim, hitbox, common, kb);
    const float angle = fxToFloat(resolvedLaunchAngle) * 3.14159265f / 180.0f;
    const Fix scaledVelocity = fxMul(kb, common.damageVelocityScaleX100);
    Vec2 knockbackVelocity{
        fxFromFloat(-std::cos(angle) * fxToFloat(scaledVelocity) * static_cast<float>(damageFacing)),
        fxFromFloat(std::sin(angle) * fxToFloat(scaledVelocity)),
    };

    victim.percent += hitbox.damage;
    const Fix scaledHitstun = fxMul(kb, common.hitstunMultiplierX154);
    victim.hitstun = std::max(1, static_cast<int>(fxToFloat(scaledHitstun)));
    victim.damageLevel = forceDamageFlyTopState ? 3 : damageLevelFromHitstun(scaledHitstun, common);
    victim.damageHurtboxRegion = 1;
    victim.damageKnockback = kb;
    victim.damageLaunchAngle = resolvedLaunchAngle;
    victim.damageTumble = victim.damageLevel >= 3;
    victim.damageSurfaceTimer = 0;
    victim.damageHitboxOwner = static_cast<int>(attackerIndex);
    victim.fighterVelocity = {};
    victim.groundVelocity = 0;
    victim.groundKnockbackVelocity = 0;
    victim.groundAccel = 0;
    victim.groundAccelSecondary = 0;
    victim.hitlag = 0;
    attacker.hitlag = 0;
    victim.facing = damageFacing;
    if (hitbox.knockbackAngleDegrees > fx(90) && hitbox.knockbackAngleDegrees < fx(270)) {
        victim.facing = -damageFacing;
    }

    bool launchOffGround = !wasGrounded || forceDamageFlyTopState;
    bool groundBounce = false;
    if (wasGrounded) {
        const Vec2 floorNormal = victim.groundNormal;
        const Vec2 pos{knockbackVelocity.x, knockbackVelocity.y};
        const Fix floorAngle = angleBetween(floorNormal, pos);
        if (floorAngle < fxFromFloat(1.57079632679f)) {
            launchOffGround = true;
        } else if (victim.damageLevel >= 3) {
            launchOffGround = true;
            if (floorAngle > fxFromFloat(1.57079632679f) + common.damageGroundBounceAngleX1E8) {
                knockbackVelocity.y = fxMul(-knockbackVelocity.y, common.damageGroundBounceDampingX1EC);
                resolvedLaunchAngle = fxFromFloat(std::atan2(fxToFloat(knockbackVelocity.y), fxToFloat(knockbackVelocity.x)) * 180.0f / 3.14159265f);
                if (resolvedLaunchAngle < 0) {
                    resolvedLaunchAngle += fx(360);
                }
                victim.damageLaunchAngle = resolvedLaunchAngle;
                groundBounce = true;
            }
        } else {
            victim.groundKnockbackVelocity = velocityAlongGround(knockbackVelocity, victim.groundNormal);
            knockbackVelocity.x = fxMul(victim.groundNormal.y, victim.groundKnockbackVelocity);
            knockbackVelocity.y = fxMul(-victim.groundNormal.x, victim.groundKnockbackVelocity);
        }
    }

    victim.knockbackVelocity = knockbackVelocity;
    const Fix velocityMag = vectorMagnitude(knockbackVelocity);
    if (velocityMag > 0) {
        victim.knockbackDecay.x = fxMul(fxDiv(knockbackVelocity.x, velocityMag), common.knockbackFrameDecayX204);
        victim.knockbackDecay.y = fxMul(fxDiv(knockbackVelocity.y, velocityMag), common.knockbackFrameDecayX204);
    } else {
        victim.knockbackDecay = {};
    }

    if (!forceDamageFlyTopState && applyWeakProneDamageState(world, victim, victimStateName, hitbox.damage, common)) {
        return;
    }

    if (wasGrounded && launchOffGround) {
        victim.jumpsUsed = 1;
        lockFighterEcb(victim, 10);
    }

    if (forceDamageFlyTopState) {
        victim.grounded = false;
        victim.groundSegment = -1;
        changeFighterState(world, victim, "DamageFlyTop");
    } else if (victim.damageLevel >= 3) {
        victim.grounded = !launchOffGround && !groundBounce;
        if (!victim.grounded) {
            victim.groundSegment = -1;
        }
        changeFighterState(world, victim, flyDamageStateName(world, victim, common, victim.damageHurtboxRegion, resolvedLaunchAngle));
    } else if (launchOffGround) {
        victim.grounded = false;
        victim.groundSegment = -1;
        changeFighterState(world, victim, airDamageStateName(victim.damageLevel));
    } else {
        victim.grounded = wasGrounded;
        changeFighterState(world, victim, groundedDamageStateName(victim.damageLevel, victim.damageHurtboxRegion));
    }
}

static bool isHeldCaptureStateName(const std::string& name) {
    return name == "CapturePulledHi" || name == "CapturePulledLw" ||
           name == "CaptureWaitHi" || name == "CaptureWaitLw" ||
           name == "CaptureDamageHi" || name == "CaptureDamageLw" ||
           name == "CaptureYoshi" ||
           name == "CaptureNeck" || name == "CaptureFoot" ||
           name == "ThrownF" || name == "ThrownB" ||
           name == "ThrownHi" || name == "ThrownLw" ||
           name == "ThrownLwWomen" ||
           name == "ThrownFF" || name == "ThrownFB" ||
           name == "ThrownFHi" || name == "ThrownFLw" ||
           name == "ThrownKoopaF" || name == "ThrownKoopaB" ||
           name == "ThrownKoopaAirF" || name == "ThrownKoopaAirB" ||
           name == "ThrownMewtwo" || name == "ThrownMewtwoAir";
}

static bool isThrowStateNameSim(const std::string& name) {
    return name == "ThrowF" || name == "ThrowB" || name == "ThrowHi" || name == "ThrowLw";
}

static bool validFighterIndex(const World& world, int index) {
    return index >= 0 && index < static_cast<int>(world.fighters.size());
}

static Fix initialGrabTimer(const FighterRuntime& victim, const MeleeCommonData& common, size_t attackerIndex) {
    const Fix slot = fx(static_cast<int>(attackerIndex) + 1);
    const Fix portTerm = fxMul(common.grabTimerPortScaleX360, common.grabTimerPortBaseX364 - slot);
    const Fix handicapTerm = fxMul(common.grabTimerHandicapScaleX358, common.grabTimerHandicapBaseX35C - fx(9));
    return fxMul(victim.percent, common.grabTimerPercentScaleX368) + common.grabTimerBaseX354 + handicapTerm + portTerm;
}

static int xRotNBone(const FighterDefinition& def, const FighterRuntime& fighter) {
    return commonPartBone(def, fighter, 2);
}

static int transN2Bone(const FighterDefinition& def, const FighterRuntime& fighter) {
    return commonPartBone(def, fighter, 52);
}

static Vec3 boneWorldByIndex(const FighterRuntime& fighter, int bone) {
    if (bone >= 0 && static_cast<size_t>(bone) < fighter.jointWorldTransforms.size()) {
        return transformPoint(fighter.jointWorldTransforms[static_cast<size_t>(bone)], {});
    }
    return {fighter.position.x, fighter.position.y, 0};
}

static bool isJointInSubtree(const FighterDefinition& def, int joint, int root) {
    if (joint < 0 || root < 0 ||
        static_cast<size_t>(joint) >= def.authoredSkeleton.size() ||
        static_cast<size_t>(root) >= def.authoredSkeleton.size())
    {
        return false;
    }
    for (int current = joint; current >= 0 && static_cast<size_t>(current) < def.authoredSkeleton.size();
         current = def.authoredSkeleton[static_cast<size_t>(current)].parent)
    {
        if (current == root) {
            return true;
        }
    }
    return false;
}

static void translateHsdJointSubtree(const FighterDefinition& def, FighterRuntime& fighter, int root, Vec3 delta) {
    if (root < 0) {
        return;
    }
    for (size_t i = 0; i < fighter.jointWorldTransforms.size(); ++i) {
        if (!isJointInSubtree(def, static_cast<int>(i), root)) {
            continue;
        }
        JointWorldTransform& transform = fighter.jointWorldTransforms[i];
        transform.translation.x += delta.x;
        transform.translation.y += delta.y;
        transform.translation.z += delta.z;
        transform.matrix[3] += delta.x;
        transform.matrix[7] += delta.y;
        transform.matrix[11] += delta.z;
    }
    fighter.jointWorldPositions = translationsFromTransforms(fighter.jointWorldTransforms);
    evaluateNativePoseHurtboxes(def, fighter);
}

static Vec3 meleeCaptureAnchorWorld(const FighterDefinition& grabberDef, const FighterRuntime& grabber, bool constrained) {
    if (constrained) {
        const int bone = transN2Bone(grabberDef, grabber);
        if (bone >= 0) {
            return boneWorldByIndex(grabber, bone);
        }
    }
    const int shieldBone = grabberDef.fighterBones.shield;
    if (shieldBone >= 0) {
        return boneWorldByIndex(grabber, shieldBone);
    }
    return commonPartWorld(grabberDef, grabber, 52);
}

static Vec3 meleeTopNFromXRotN(const FighterDefinition& def, const FighterRuntime& fighter) {
    const Vec3 transN = commonPartWorld(def, fighter, 1);
    const Vec3 xRotN = commonPartWorld(def, fighter, 2);
    return {transN.x - xRotN.x, transN.y - xRotN.y, transN.z - xRotN.z};
}

static bool isZeroVec3(Vec3 value) {
    return value.x == 0 && value.y == 0 && value.z == 0;
}

static Vec3 meleeInitialTopNFromXRotN(const FighterDefinition& def, const FighterRuntime& fighter) {
    if (def.authoredSkeleton.empty()) {
        return {};
    }

    AnimationPose pose = bindPose(def.authoredSkeleton);
    FighterRuntime temp = fighter;
    temp.animationPose = pose;
    const std::vector<JointWorldTransform> transforms = fighterWorldTransforms(def, temp);
    const int transN = commonPartBone(def, fighter, 1);
    const int xRotN = commonPartBone(def, fighter, 2);
    if (transN < 0 || xRotN < 0 ||
        static_cast<size_t>(transN) >= transforms.size() ||
        static_cast<size_t>(xRotN) >= transforms.size())
    {
        return meleeTopNFromXRotN(def, fighter);
    }
    const Vec3 transNWorld = transformPoint(transforms[static_cast<size_t>(transN)], {});
    const Vec3 xRotNWorld = transformPoint(transforms[static_cast<size_t>(xRotN)], {});
    return {transNWorld.x - xRotNWorld.x, transNWorld.y - xRotNWorld.y, transNWorld.z - xRotNWorld.z};
}

static Vec2 meleeCurPosFromXRotN(const FighterRuntime& fighter, Vec3 xRotNWorld) {
    return {
        xRotNWorld.x + fighter.facing * fighter.captureConstraintOffset.z,
        xRotNWorld.y + fighter.captureConstraintOffset.y,
    };
}

static Vec2 meleeThrowReleaseLastPosition(const FighterRuntime& anchorOwner) {
    return {
        anchorOwner.position.x,
        anchorOwner.position.y + fxMul(anchorOwner.ecb.points[1].y + anchorOwner.ecb.points[3].y, fxFromFloat(0.5f)),
    };
}

static void storeMeleeCaptureOffsets(const FighterDefinition& victimDef, FighterRuntime& victim) {
    victim.captureConstraintOffset = meleeInitialTopNFromXRotN(victimDef, victim);
    const int xRotN = xRotNBone(victimDef, victim);
    if (xRotN >= 0 && static_cast<size_t>(xRotN) < victim.animationPose.joints.size()) {
        victim.captureOriginalXRotNTranslation = victim.animationPose.joints[static_cast<size_t>(xRotN)].translation;
    } else {
        victim.captureOriginalXRotNTranslation = {};
    }
}

void beginMeleeThrowConstraint(World& world, size_t grabberIndex, size_t victimIndex) {
    if (grabberIndex >= world.fighters.size() || victimIndex >= world.fighters.size()) {
        return;
    }
    FighterRuntime& victim = world.fighters[victimIndex];
    const FighterDefinition& victimDef = world.fighterDefs[static_cast<size_t>(victim.fighterDef)];
    storeMeleeCaptureOffsets(victimDef, victim);

    // ftCo_800DB368: constrain victim XRotN to thrower TransN2 and zero XRotN rotation.
    const int xRotN = xRotNBone(victimDef, victim);
    if (xRotN >= 0 && static_cast<size_t>(xRotN) < victim.animationPose.joints.size()) {
        JointPose& xRotNPose = victim.animationPose.joints[static_cast<size_t>(xRotN)];
        xRotNPose.rotation = {};
        xRotNPose.quaternion = {};
        xRotNPose.useQuaternion = false;
    }
    victim.captureConstraintActive = true;
    updateMeleeCapturePosition(world, victimIndex);
}

bool updateMeleeCapturePosition(World& world, size_t victimIndex) {
    if (victimIndex >= world.fighters.size()) {
        return false;
    }
    FighterRuntime& victim = world.fighters[victimIndex];
    if (!validFighterIndex(world, victim.grabberFighter)) {
        return false;
    }

    FighterRuntime& grabber = world.fighters[static_cast<size_t>(victim.grabberFighter)];
    const FighterDefinition& grabberDef = world.fighterDefs[static_cast<size_t>(grabber.fighterDef)];
    const FighterDefinition& victimDef = world.fighterDefs[static_cast<size_t>(victim.fighterDef)];
    if (isZeroVec3(victim.captureConstraintOffset)) {
        storeMeleeCaptureOffsets(victimDef, victim);
    }

    // ftCo_800DAD18 aligns non-constrained capture states by adding
    // (anchor - XRotN) to cur_pos. ftCo_800DB464 / ftCo_800DE508 use the
    // fighter-load x1A70 (TopN - XRotN) only after XRotN is constrained under
    // the thrower's TransN2.
    const Vec3 xRotN = commonPartWorld(victimDef, victim, 2);
    const Vec3 anchor = meleeCaptureAnchorWorld(grabberDef, grabber, victim.captureConstraintActive);
    const Vec3 delta{anchor.x - xRotN.x, anchor.y - xRotN.y, anchor.z - xRotN.z};
    const bool high = delta.y > victimDef.properties.common.captureHighThresholdX3C4;
    victim.previousPosition = victim.position;
    if (victim.captureConstraintActive) {
        victim.position = meleeCurPosFromXRotN(victim, anchor);
    } else {
        victim.position.x += delta.x;
        victim.position.y += delta.y;
    }
    victim.fighterVelocity = {};
    victim.knockbackVelocity = {};
    victim.groundVelocity = 0;
    victim.groundAccel = 0;
    victim.groundAccelSecondary = 0;
    victim.facing = grabber.facing;
    refreshHsdWorldPose(victimDef, victim);
    if (victim.captureConstraintActive) {
        const int xRotNRoot = xRotNBone(victimDef, victim);
        const Vec3 constrainedXRotN = boneWorldByIndex(victim, xRotNRoot);
        translateHsdJointSubtree(victimDef, victim, xRotNRoot, {
            anchor.x - constrainedXRotN.x,
            anchor.y - constrainedXRotN.y,
            anchor.z - constrainedXRotN.z,
        });
    }
    applyImportedBoneAliases(victimDef, victim);
    calculateEcb(victimDef, victim, true);
    return high;
}

void releaseMeleeCaptureConstraint(World& world, size_t ownerIndex, int capturedIndex, bool applyOffset, bool meleeThrowRelease) {
    if (ownerIndex >= world.fighters.size() || !validFighterIndex(world, capturedIndex)) {
        return;
    }
    FighterRuntime& owner = world.fighters[ownerIndex];
    FighterRuntime& captured = world.fighters[static_cast<size_t>(capturedIndex)];
    const FighterDefinition& ownerDef = world.fighterDefs[static_cast<size_t>(owner.fighterDef)];
    const FighterDefinition& capturedDef = world.fighterDefs[static_cast<size_t>(captured.fighterDef)];
    const Vec3 anchor = meleeCaptureAnchorWorld(ownerDef, owner, captured.captureConstraintActive);
    const Vec2 lastPosition = meleeThrowRelease ? meleeThrowReleaseLastPosition(owner) : captured.position;

    if (captured.captureConstraintActive) {
        const int xRotN = xRotNBone(capturedDef, captured);
        if (xRotN >= 0 && static_cast<size_t>(xRotN) < captured.animationPose.joints.size()) {
            captured.animationPose.joints[static_cast<size_t>(xRotN)].translation = captured.captureOriginalXRotNTranslation;
        }
    }
    if (applyOffset) {
        captured.position = meleeCurPosFromXRotN(captured, anchor);
    }
    captured.previousPosition = lastPosition;
    captured.captureConstraintActive = false;
    captured.captureConstraintOffset = {};
    captured.captureOriginalXRotNTranslation = {};
    captured.grabberFighter = -1;
    if (owner.grabbedFighter == capturedIndex) {
        owner.grabbedFighter = -1;
    }
    if (meleeThrowRelease) {
        unlockFighterEcb(captured);
    }
    refreshHsdWorldPose(capturedDef, captured);
    calculateEcb(capturedDef, captured, true);
    if (meleeThrowRelease && captured.grounded && snapToCurrentGround(world, captured)) {
        refreshHsdWorldPose(capturedDef, captured);
        calculateEcb(capturedDef, captured, false);
    }
}

static void breakGrabLinks(World& world, size_t grabberIndex, int victimIndex) {
    if (grabberIndex < world.fighters.size()) {
        FighterRuntime& grabber = world.fighters[grabberIndex];
        if (grabber.grabbedFighter == victimIndex) {
            grabber.grabbedFighter = -1;
        }
    }
    if (validFighterIndex(world, victimIndex)) {
        FighterRuntime& victim = world.fighters[static_cast<size_t>(victimIndex)];
        if (victim.grabberFighter == static_cast<int>(grabberIndex)) {
            victim.grabberFighter = -1;
        }
    }
}

static void maintainCapturedFighterPosition(World& world, size_t victimIndex) {
    updateMeleeCapturePosition(world, victimIndex);
}

static bool changeCaptureLowToHighAfterMeleeAlign(World& world, FighterRuntime& fighter) {
    const std::string stateName = currentState(world, fighter).name;
    const char* target = nullptr;
    if (stateName == "CapturePulledLw") {
        target = "CapturePulledHi";
    } else if (stateName == "CaptureWaitLw") {
        target = "CaptureWaitHi";
    } else if (stateName == "CaptureDamageLw") {
        target = "CaptureDamageHi";
    }
    if (!target) {
        return false;
    }

    const Fix frame = fighter.animationFrame;
    fighter.grounded = false;
    fighter.groundSegment = -1;
    fighter.groundVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.jumpsUsed = 1;
    lockFighterEcb(fighter, 10);
    unlockFighterEcb(fighter);
    changeFighterState(world, fighter, target, 0, kDisableAnimationBlendFrames);
    fighter.animationFrame = frame;
    return true;
}

static bool changeCaptureHighToLowAfterMeleeFloor(World& world, FighterRuntime& fighter) {
    const std::string stateName = currentState(world, fighter).name;
    const char* target = nullptr;
    if (stateName == "CapturePulledHi") {
        target = "CapturePulledLw";
    } else if (stateName == "CaptureWaitHi") {
        target = "CaptureWaitLw";
    } else if (stateName == "CaptureDamageHi") {
        target = "CaptureDamageLw";
    }
    if (!target) {
        return false;
    }

    const Vec2 previousBottom = fighter.previousPosition + fighter.previousEcb.points[3];
    const Vec2 currentBottom = fighter.position + fighter.ecb.points[3];
    if (currentBottom.y > previousBottom.y) {
        return false;
    }
    Vec2 contact{};
    Fix fraction = fx(1);
    const int landedSegment = findLandingSegment(world, fighter, previousBottom, currentBottom, contact, fraction);
    if (landedSegment < 0) {
        return false;
    }

    const Fix frame = fighter.animationFrame;
    fighter.position.x = contact.x - fighter.ecb.points[3].x;
    fighter.position.y = contact.y - fighter.ecb.points[3].y;
    fighter.grounded = true;
    fighter.groundSegment = landedSegment;
    fighter.groundNormal = segmentNormal(world.stage.segments[static_cast<size_t>(landedSegment)]);
    fighter.groundVelocity = velocityAlongGround(fighter.fighterVelocity, fighter.groundNormal);
    projectGroundVelocity(fighter);
    fighter.jumpsUsed = 0;
    fighter.wallJumpsUsed = 0;
    unlockFighterEcb(fighter);
    changeFighterState(world, fighter, target, 0, kDisableAnimationBlendFrames);
    fighter.animationFrame = frame;
    return true;
}

static void maintainCapturedFighterAfterPose(World& world, size_t victimIndex) {
    if (victimIndex >= world.fighters.size()) {
        return;
    }
    FighterRuntime& fighter = world.fighters[victimIndex];
    if (fighter.grabberFighter < 0 || !isHeldCaptureStateName(currentState(world, fighter).name)) {
        return;
    }
    const bool high = updateMeleeCapturePosition(world, victimIndex);
    if (high) {
        changeCaptureLowToHighAfterMeleeAlign(world, fighter);
    }
    changeCaptureHighToLowAfterMeleeFloor(world, fighter);
}

static void captureVictim(World& world, size_t attackerIndex, size_t victimIndex) {
    FighterRuntime& attacker = world.fighters[attackerIndex];
    FighterRuntime& victim = world.fighters[victimIndex];
    if (attacker.grabbedFighter >= 0 || victim.grabberFighter >= 0) {
        return;
    }
    const FighterDefinition& victimDef = world.fighterDefs[static_cast<size_t>(victim.fighterDef)];
    attacker.grabbedFighter = static_cast<int>(victimIndex);
    victim.grabberFighter = static_cast<int>(attackerIndex);
    victim.grabbedFighter = -1;
    victim.grabTimer = std::max(Fix{1}, initialGrabTimer(victim, victimDef.properties.common, attackerIndex));
    victim.captureWaitTimer = 0;
    victim.captureMashAnimTimer = 0;
    victim.grabMashStickX = 0;
    victim.grabMashStickY = 0;
    victim.captureJumpQueued = false;
    victim.hitlag = 0;
    victim.hitstun = 0;
    victim.fighterVelocity = {};
    victim.knockbackVelocity = {};
    victim.groundVelocity = 0;
    victim.groundAccel = 0;
    victim.groundAccelSecondary = 0;
    victim.facing = attacker.facing;
    storeMeleeCaptureOffsets(victimDef, victim);
    const bool high = victim.position.y >= attacker.position.y + victimDef.properties.common.captureHighThresholdX3C4;
    changeFighterState(world, victim, high ? "CapturePulledHi" : "CapturePulledLw", 0, kDisableAnimationBlendFrames);
    const Fix catchFrame = attacker.animationFrame;
    changeFighterState(world, attacker, currentState(world, attacker).name == "CatchDash" ? "CatchDashPull" : "CatchPull", 0, kDisableAnimationBlendFrames);
    attacker.animationFrame = catchFrame;
    attacker.grabbedFighter = static_cast<int>(victimIndex);
    victim.grabberFighter = static_cast<int>(attackerIndex);
    const FighterDefinition& attackerDef = world.fighterDefs[static_cast<size_t>(attacker.fighterDef)];
    evaluatePose(attackerDef, currentState(world, attacker), attacker);
    evaluatePose(victimDef, currentState(world, victim), victim);
    maintainCapturedFighterPosition(world, victimIndex);
}

static void applyCapturedDamage(World& world, size_t attackerIndex, FighterRuntime& attacker, FighterRuntime& victim, const HitboxDefinition& hitbox) {
    victim.percent += hitbox.damage;
    const int hitlagFrames = std::max(3, static_cast<int>(fxToFloat(hitbox.damage) / 3.0f) + 3);
    attacker.hitlag = std::max(attacker.hitlag, hitlagFrames);
    victim.hitlag = std::max(victim.hitlag, hitlagFrames);
    const std::string& victimState = currentState(world, victim).name;
    if (victimState == "CaptureWaitLw" || victimState == "CapturePulledLw" || victimState == "CaptureDamageLw") {
        changeFighterState(world, victim, "CaptureDamageLw", 0, kDisableAnimationBlendFrames);
    } else {
        changeFighterState(world, victim, "CaptureDamageHi", 0, kDisableAnimationBlendFrames);
    }
    victim.grabberFighter = static_cast<int>(attackerIndex);
}

static const HitboxDefinition* throwHitboxForRelease(FighterRuntime& attacker) {
    if (attacker.throwHitboxActive[0]) {
        return &attacker.throwHitboxes[0];
    }
    return nullptr;
}

static void processThrowRelease(World& world, size_t attackerIndex) {
    FighterRuntime& attacker = world.fighters[attackerIndex];
    if (!fighterThrowFlag(attacker, 3) || !isThrowStateNameSim(currentState(world, attacker).name)) {
        return;
    }
    setFighterThrowFlag(attacker, 3, false);
    const int victimIndex = attacker.grabbedFighter;
    if (!validFighterIndex(world, victimIndex)) {
        return;
    }
    const HitboxDefinition* hitbox = throwHitboxForRelease(attacker);
    if (!hitbox) {
        return;
    }
    FighterRuntime& victim = world.fighters[static_cast<size_t>(victimIndex)];
    const bool forceDamageFlyTop = currentState(world, attacker).name == "ThrowLw";
    victim.previousPosition = victim.position;
    releaseMeleeCaptureConstraint(world, attackerIndex, victimIndex, true, true);
    victim.thrownAnimationFreezeActive = false;
    victim.thrownAnimationFreezeFrame = 0;
    victim.thrownHitboxOwner = static_cast<int>(attackerIndex);
    attacker.throwAnimationFrozen = false;
    attacker.animationRate = fx(1);
    applyThrowReleaseDamage(world, attackerIndex, attacker, victim, *hitbox, forceDamageFlyTop);
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

static void updateActiveHitboxPositions(World& world, size_t fighterIndex) {
    FighterRuntime& fighter = world.fighters[fighterIndex];
    for (ActiveHitbox& hitbox : fighter.activeHitboxes) {
        const Vec3 worldPos = hitboxWorld(fighter, hitbox.def);
        if (hitbox.firstFrame) {
            hitbox.firstFrame = false;
            hitbox.current = worldPos;
            hitbox.previous = worldPos;
        } else {
            hitbox.previous = hitbox.current;
            hitbox.current = worldPos;
        }
    }
}

static const GameObjectStateDefinition* currentGameObjectState(const World& world, const GameObjectRuntime& object) {
    if (object.objectDef < 0 || object.objectDef >= static_cast<int>(world.objectDefs.size())) {
        return nullptr;
    }
    const GameObjectDefinition& def = world.objectDefs[static_cast<size_t>(object.objectDef)];
    if (object.state < 0 || object.state >= static_cast<int>(def.states.size())) {
        return nullptr;
    }
    return &def.states[static_cast<size_t>(object.state)];
}

static int frameInObjectState(const World& world, const GameObjectRuntime& object) {
    return std::max(1, world.frame - object.lastStateChangeFrame + 1);
}

static void objectLinearPhysics(GameObjectRuntime& object) {
    object.previousPosition = object.position;
    object.position += object.velocity;
}

static void objectGravityPhysics(const GameObjectDefinition& def, GameObjectRuntime& object) {
    object.previousPosition = object.position;
    object.velocity.y = std::max(object.velocity.y - def.gravity, -def.terminalVelocity);
    object.position += object.velocity;
}

static bool objectFloorContact(const World& world, const GameObjectRuntime& object, int& floorSegment, Fix& floorY) {
    if (object.position.y > object.previousPosition.y) {
        return false;
    }
    for (size_t i = 0; i < world.stage.segments.size(); ++i) {
        const StageSegment& segment = world.stage.segments[i];
        if (!isFloorLine(segment) || object.position.x < segmentMinX(segment) || object.position.x > segmentMaxX(segment)) {
            continue;
        }
        Fix y = 0;
        if (!segmentYAtX(segment, object.position.x, y)) {
            continue;
        }
        if (object.previousPosition.y >= y && object.position.y <= y) {
            floorSegment = static_cast<int>(i);
            floorY = y;
            return true;
        }
    }
    return false;
}

static void runGameObjectFunction(World& world, size_t objectIndex, const FunctionCall& fn) {
    if (objectIndex >= world.objects.size()) {
        return;
    }
    GameObjectRuntime& object = world.objects[objectIndex];
    if (!object.active || object.objectDef < 0 || object.objectDef >= static_cast<int>(world.objectDefs.size())) {
        return;
    }
    const GameObjectDefinition& def = world.objectDefs[static_cast<size_t>(object.objectDef)];
    if (fn.name.starts_with("script:")) {
        const std::string scriptName = fn.name.substr(7);
        const auto found = std::find_if(def.packageScripts.begin(), def.packageScripts.end(), [&](const PackageScript& script) {
            return script.name == scriptName;
        });
        if (found == def.packageScripts.end()) {
            return;
        }
        if (object.packageVars.size() != def.packageVariables.size()) {
            const size_t oldSize = object.packageVars.size();
            object.packageVars.resize(def.packageVariables.size());
            for (size_t i = oldSize; i < def.packageVariables.size(); ++i) {
                object.packageVars[i] = def.packageVariables[i].initialValue;
            }
        }
        auto var = [&](int index) -> int32_t {
            if (index < 0 || index >= static_cast<int>(object.packageVars.size())) {
                return 0;
            }
            return object.packageVars[static_cast<size_t>(index)];
        };
        auto setVar = [&](int index, int32_t value) {
            if (index < 0 || index >= static_cast<int>(object.packageVars.size())) {
                return;
            }
            object.packageVars[static_cast<size_t>(index)] = value;
        };
        auto setObjectOwner = [&](int32_t value) {
            object.ownerFighter = value < -1 ? -1 : static_cast<int>(value);
        };
        auto ensureObjectPackageVars = [&](GameObjectRuntime& target) {
            if (target.objectDef < 0 || target.objectDef >= static_cast<int>(world.objectDefs.size())) {
                return;
            }
            const GameObjectDefinition& targetDef = world.objectDefs[static_cast<size_t>(target.objectDef)];
            if (target.packageVars.size() == targetDef.packageVariables.size()) {
                return;
            }
            const size_t oldSize = target.packageVars.size();
            target.packageVars.resize(targetDef.packageVariables.size());
            for (size_t i = oldSize; i < targetDef.packageVariables.size(); ++i) {
                target.packageVars[i] = targetDef.packageVariables[i].initialValue;
            }
        };
        auto indexedObjectVar = [&](int objectIndex, int variableIndex) -> int32_t {
            if (objectIndex < 0 || objectIndex >= static_cast<int>(world.objects.size()) || variableIndex < 0) {
                return 0;
            }
            GameObjectRuntime& target = world.objects[static_cast<size_t>(objectIndex)];
            ensureObjectPackageVars(target);
            if (variableIndex >= static_cast<int>(target.packageVars.size())) {
                return 0;
            }
            return target.packageVars[static_cast<size_t>(variableIndex)];
        };
        auto setIndexedObjectVar = [&](int objectIndex, int variableIndex, int32_t value) {
            if (objectIndex < 0 || objectIndex >= static_cast<int>(world.objects.size()) || variableIndex < 0) {
                return;
            }
            GameObjectRuntime& target = world.objects[static_cast<size_t>(objectIndex)];
            ensureObjectPackageVars(target);
            if (variableIndex < static_cast<int>(target.packageVars.size())) {
                target.packageVars[static_cast<size_t>(variableIndex)] = value;
            }
        };
        auto setCurrentObjectVarAfterEvent = [&](int variableIndex, int32_t value) {
            if (objectIndex >= world.objects.size() || variableIndex < 0) {
                return;
            }
            GameObjectRuntime& currentObject = world.objects[objectIndex];
            if (variableIndex < static_cast<int>(currentObject.packageVars.size())) {
                currentObject.packageVars[static_cast<size_t>(variableIndex)] = value;
            }
        };
        auto ensureFighterPackageVars = [&](FighterRuntime& target) {
            if (target.fighterDef < 0 || target.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
                return;
            }
            const FighterDefinition& targetDef = world.fighterDefs[static_cast<size_t>(target.fighterDef)];
            if (target.packageVars.size() == targetDef.packageVariables.size()) {
                return;
            }
            const size_t oldSize = target.packageVars.size();
            target.packageVars.resize(targetDef.packageVariables.size());
            for (size_t i = oldSize; i < targetDef.packageVariables.size(); ++i) {
                target.packageVars[i] = targetDef.packageVariables[i].initialValue;
            }
        };
        auto indexedFighter = [&](int fighterIndex) -> FighterRuntime* {
            if (!validFighterIndex(world, fighterIndex)) {
                return nullptr;
            }
            FighterRuntime& target = world.fighters[static_cast<size_t>(fighterIndex)];
            if (target.fighterDef < 0 || target.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
                return nullptr;
            }
            ensureFighterPackageVars(target);
            return &target;
        };
        auto indexedFighterVar = [&](int fighterIndex, int variableIndex) -> int32_t {
            FighterRuntime* target = indexedFighter(fighterIndex);
            if (!target || variableIndex < 0 || variableIndex >= static_cast<int>(target->packageVars.size())) {
                return 0;
            }
            return target->packageVars[static_cast<size_t>(variableIndex)];
        };
        auto setIndexedFighterVar = [&](int fighterIndex, int variableIndex, int32_t value) {
            FighterRuntime* target = indexedFighter(fighterIndex);
            if (!target || variableIndex < 0 || variableIndex >= static_cast<int>(target->packageVars.size())) {
                return;
            }
            target->packageVars[static_cast<size_t>(variableIndex)] = value;
        };
        auto setOwnerFighterVar = [&](int index, int32_t value) {
            if (!validFighterIndex(world, object.ownerFighter) || index < 0) {
                return;
            }
            FighterRuntime& owner = world.fighters[static_cast<size_t>(object.ownerFighter)];
            if (owner.fighterDef < 0 || owner.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
                return;
            }
            const FighterDefinition& ownerDef = world.fighterDefs[static_cast<size_t>(owner.fighterDef)];
            if (owner.packageVars.size() != ownerDef.packageVariables.size()) {
                const size_t oldSize = owner.packageVars.size();
                owner.packageVars.resize(ownerDef.packageVariables.size());
                for (size_t i = oldSize; i < ownerDef.packageVariables.size(); ++i) {
                    owner.packageVars[i] = ownerDef.packageVariables[i].initialValue;
                }
            }
            if (index < static_cast<int>(owner.packageVars.size())) {
                owner.packageVars[static_cast<size_t>(index)] = value;
            }
        };
        auto ownerFighterVar = [&](int index) -> int32_t {
            if (!validFighterIndex(world, object.ownerFighter) || index < 0) {
                return 0;
            }
            FighterRuntime& owner = world.fighters[static_cast<size_t>(object.ownerFighter)];
            if (owner.fighterDef < 0 || owner.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
                return 0;
            }
            const FighterDefinition& ownerDef = world.fighterDefs[static_cast<size_t>(owner.fighterDef)];
            if (owner.packageVars.size() != ownerDef.packageVariables.size()) {
                const size_t oldSize = owner.packageVars.size();
                owner.packageVars.resize(ownerDef.packageVariables.size());
                for (size_t i = oldSize; i < ownerDef.packageVariables.size(); ++i) {
                    owner.packageVars[i] = ownerDef.packageVariables[i].initialValue;
                }
            }
            if (index >= static_cast<int>(owner.packageVars.size())) {
                return 0;
            }
            return owner.packageVars[static_cast<size_t>(index)];
        };
        auto ownerInput = [&]() -> const InputBuffer* {
            if (!validFighterIndex(world, object.ownerFighter)) {
                return nullptr;
            }
            return &world.fighters[static_cast<size_t>(object.ownerFighter)].input;
        };
        int budget = std::clamp(found->instructionBudget, 0, 1024);
        struct ScriptFrame {
            const PackageScript* script = nullptr;
            size_t instructionIndex = 0;
        };
        std::vector<ScriptFrame> scriptStack{{&*found, 0}};
        while (!scriptStack.empty()) {
            ScriptFrame& frame = scriptStack.back();
            if (frame.script == nullptr || frame.instructionIndex >= frame.script->instructions.size()) {
                scriptStack.pop_back();
                continue;
            }
            if (budget-- <= 0) {
                return;
            }
            const size_t instructionIndex = frame.instructionIndex;
            const PackageScriptInstruction& instruction = frame.script->instructions[instructionIndex];
            switch (instruction.op) {
            case PackageScriptOp::Nop:
                break;
            case PackageScriptOp::SetVarImmediate:
                setVar(instruction.dst, instruction.intValue);
                break;
            case PackageScriptOp::SetVarFromVar:
                setVar(instruction.dst, var(instruction.srcA));
                break;
            case PackageScriptOp::SetVarLessThanImmediate:
                setVar(instruction.dst, var(instruction.srcA) < instruction.intValue ? 1 : 0);
                break;
            case PackageScriptOp::SetVarLessThanVar:
                setVar(instruction.dst, var(instruction.srcA) < var(instruction.srcB) ? 1 : 0);
                break;
            case PackageScriptOp::SetVarEqualImmediate:
                setVar(instruction.dst, var(instruction.srcA) == instruction.intValue ? 1 : 0);
                break;
            case PackageScriptOp::SetVarEqualVar:
                setVar(instruction.dst, var(instruction.srcA) == var(instruction.srcB) ? 1 : 0);
                break;
            case PackageScriptOp::SetVarNotEqualImmediate:
                setVar(instruction.dst, var(instruction.srcA) != instruction.intValue ? 1 : 0);
                break;
            case PackageScriptOp::SetVarNotEqualVar:
                setVar(instruction.dst, var(instruction.srcA) != var(instruction.srcB) ? 1 : 0);
                break;
            case PackageScriptOp::SetVarGreaterThanImmediate:
                setVar(instruction.dst, var(instruction.srcA) > instruction.intValue ? 1 : 0);
                break;
            case PackageScriptOp::SetVarGreaterThanVar:
                setVar(instruction.dst, var(instruction.srcA) > var(instruction.srcB) ? 1 : 0);
                break;
            case PackageScriptOp::SetVarNot:
                setVar(instruction.dst, var(instruction.srcA) == 0 ? 1 : 0);
                break;
            case PackageScriptOp::SetVarAnd:
                setVar(instruction.dst, var(instruction.srcA) != 0 && var(instruction.srcB) != 0 ? 1 : 0);
                break;
            case PackageScriptOp::SetVarOr:
                setVar(instruction.dst, var(instruction.srcA) != 0 || var(instruction.srcB) != 0 ? 1 : 0);
                break;
            case PackageScriptOp::AddVarImmediate:
                setVar(instruction.dst, var(instruction.dst) + instruction.intValue);
                break;
            case PackageScriptOp::AddVar:
                setVar(instruction.dst, var(instruction.srcA) + var(instruction.srcB));
                break;
            case PackageScriptOp::ScaleVarFixed:
                setVar(instruction.dst, fxMul(var(instruction.srcA), instruction.fixValue));
                break;
            case PackageScriptOp::SetVarRandom:
                setVar(instruction.dst, nextWorldRandomBounded(world, instruction.intValue));
                break;
            case PackageScriptOp::SetVarFrame:
                setVar(instruction.dst, object.internalFrame);
                break;
            case PackageScriptOp::SetVarStateFrame:
                setVar(instruction.dst, frameInObjectState(world, object));
                break;
            case PackageScriptOp::SetVarStateIndex:
                setVar(instruction.dst, object.state);
                break;
            case PackageScriptOp::SetVarGrounded:
                setVar(instruction.dst, object.grounded ? 1 : 0);
                break;
            case PackageScriptOp::SetVarFacing:
                setVar(instruction.dst, object.facing);
                break;
            case PackageScriptOp::SetVarObjectIndex:
                setVar(instruction.dst, static_cast<int32_t>(objectIndex));
                break;
            case PackageScriptOp::SetVarFighterPercent:
            case PackageScriptOp::SetVarFighterShield:
            case PackageScriptOp::SetVarFighterPositionX:
            case PackageScriptOp::SetVarFighterPositionY:
            case PackageScriptOp::SetVarFighterGroundVelocity:
            case PackageScriptOp::SetVarFighterAirVelocityX:
            case PackageScriptOp::SetVarFighterAirVelocityY:
            case PackageScriptOp::SetVarFighterAnimationFrame:
            case PackageScriptOp::SetVarFighterAnimationRate:
            case PackageScriptOp::SetVarFighterStateFrame:
            case PackageScriptOp::SetVarFighterStateIndex:
            case PackageScriptOp::SetVarFighterGrounded:
            case PackageScriptOp::SetVarFighterFacing:
            case PackageScriptOp::SetVarFighterJumpsUsed:
            case PackageScriptOp::SetVarFighterJumpsRemaining:
            case PackageScriptOp::SetVarFighterCommandVar:
            case PackageScriptOp::SetVarFighterThrowFlag:
            case PackageScriptOp::SetVarFighterHeldObject:
            case PackageScriptOp::SetVarFighterGrabbedFighter:
            case PackageScriptOp::SetVarFighterGrabberFighter:
            case PackageScriptOp::SetVarFighterHitlag:
            case PackageScriptOp::SetVarFighterHitstun:
            case PackageScriptOp::SetVarFighterDamageHitboxOwner:
            case PackageScriptOp::SetVarFighterThrownHitboxOwner:
            case PackageScriptOp::SetVarFighterIndex: {
                if (instruction.op == PackageScriptOp::SetVarFighterIndex) {
                    setVar(instruction.dst, object.ownerFighter);
                    break;
                }
                if (!validFighterIndex(world, object.ownerFighter)) {
                    setVar(instruction.dst, 0);
                    break;
                }
                const FighterRuntime& owner = world.fighters[static_cast<size_t>(object.ownerFighter)];
                if (owner.fighterDef < 0 || owner.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
                    setVar(instruction.dst, 0);
                    break;
                }
                const FighterDefinition& ownerDef = world.fighterDefs[static_cast<size_t>(owner.fighterDef)];
                switch (instruction.op) {
                case PackageScriptOp::SetVarFighterStateFrame:
                    setVar(instruction.dst, frameInState(owner));
                    break;
                case PackageScriptOp::SetVarFighterStateIndex:
                    setVar(instruction.dst, owner.state);
                    break;
                case PackageScriptOp::SetVarFighterGrounded:
                    setVar(instruction.dst, owner.grounded ? 1 : 0);
                    break;
                case PackageScriptOp::SetVarFighterFacing:
                    setVar(instruction.dst, owner.facing);
                    break;
                case PackageScriptOp::SetVarFighterJumpsUsed:
                    setVar(instruction.dst, owner.jumpsUsed);
                    break;
                case PackageScriptOp::SetVarFighterJumpsRemaining:
                    setVar(instruction.dst, std::max(0, ownerDef.properties.maxJumps - owner.jumpsUsed));
                    break;
                case PackageScriptOp::SetVarFighterCommandVar:
                    setVar(instruction.dst, static_cast<int32_t>(fighterCommandVar(owner, instruction.intValue)));
                    break;
                case PackageScriptOp::SetVarFighterThrowFlag:
                    setVar(instruction.dst, fighterThrowFlag(owner, instruction.intValue) ? 1 : 0);
                    break;
                case PackageScriptOp::SetVarFighterHeldObject:
                    setVar(instruction.dst, owner.heldObject);
                    break;
                case PackageScriptOp::SetVarFighterGrabbedFighter:
                    setVar(instruction.dst, owner.grabbedFighter);
                    break;
                case PackageScriptOp::SetVarFighterGrabberFighter:
                    setVar(instruction.dst, owner.grabberFighter);
                    break;
                case PackageScriptOp::SetVarFighterHitlag:
                    setVar(instruction.dst, owner.hitlag);
                    break;
                case PackageScriptOp::SetVarFighterHitstun:
                    setVar(instruction.dst, owner.hitstun);
                    break;
                case PackageScriptOp::SetVarFighterDamageHitboxOwner:
                    setVar(instruction.dst, owner.damageHitboxOwner);
                    break;
                case PackageScriptOp::SetVarFighterThrownHitboxOwner:
                    setVar(instruction.dst, owner.thrownHitboxOwner);
                    break;
                case PackageScriptOp::SetVarFighterPercent:
                    setVar(instruction.dst, owner.percent);
                    break;
                case PackageScriptOp::SetVarFighterShield:
                    setVar(instruction.dst, owner.shieldHealth);
                    break;
                case PackageScriptOp::SetVarFighterPositionX:
                    setVar(instruction.dst, owner.position.x);
                    break;
                case PackageScriptOp::SetVarFighterPositionY:
                    setVar(instruction.dst, owner.position.y);
                    break;
                case PackageScriptOp::SetVarFighterGroundVelocity:
                    setVar(instruction.dst, owner.groundVelocity);
                    break;
                case PackageScriptOp::SetVarFighterAirVelocityX:
                    setVar(instruction.dst, owner.fighterVelocity.x);
                    break;
                case PackageScriptOp::SetVarFighterAirVelocityY:
                    setVar(instruction.dst, owner.fighterVelocity.y);
                    break;
                case PackageScriptOp::SetVarFighterAnimationFrame:
                    setVar(instruction.dst, owner.animationFrame);
                    break;
                case PackageScriptOp::SetVarFighterAnimationRate:
                    setVar(instruction.dst, owner.animationRate);
                    break;
                default:
                    break;
                }
                break;
            }
            case PackageScriptOp::SetFighterJumpsUsed:
            case PackageScriptOp::SetFighterJumpsUsedFromVar:
            case PackageScriptOp::SetFighterCommandVarImmediate:
            case PackageScriptOp::SetFighterCommandVarFromVar:
            case PackageScriptOp::SetFighterThrowFlagImmediate:
            case PackageScriptOp::SetFighterThrowFlagFromVar:
                return;
            case PackageScriptOp::SetVarObjectOwner:
                setVar(instruction.dst, object.ownerFighter);
                break;
            case PackageScriptOp::SetVarObjectHeldBy:
                setVar(instruction.dst, object.heldByFighter);
                break;
            case PackageScriptOp::SetVarObjectGrabVictim:
                setVar(instruction.dst, object.grabVictimFighter);
                break;
            case PackageScriptOp::SetVarObjectLastFighter:
                setVar(instruction.dst, object.lastInteractionFighter);
                break;
            case PackageScriptOp::SetVarObjectLastObject:
                setVar(instruction.dst, object.lastInteractionObject);
                break;
            case PackageScriptOp::SetVarObjectDamage:
                setVar(instruction.dst, object.damageTaken);
                break;
            case PackageScriptOp::SetObjectDamage:
                object.damageTaken = instruction.fixValue;
                break;
            case PackageScriptOp::SetObjectDamageFromVar:
                object.damageTaken = var(instruction.srcA);
                break;
            case PackageScriptOp::SetVarObjectHitlag:
                setVar(instruction.dst, object.hitlag);
                break;
            case PackageScriptOp::SetObjectHitlag:
                setGameObjectHitlag(world, objectIndex, instruction.intValue);
                break;
            case PackageScriptOp::SetObjectHitlagFromVar:
                setGameObjectHitlag(world, objectIndex, var(instruction.srcA));
                break;
            case PackageScriptOp::SetObjectOwner:
                setObjectOwner(instruction.intValue);
                break;
            case PackageScriptOp::SetObjectOwnerFromVar:
                setObjectOwner(var(instruction.srcA));
                break;
            case PackageScriptOp::SetVarObjectGroundSegment:
                setVar(instruction.dst, object.groundSegment);
                break;
            case PackageScriptOp::SetVarObjectPositionX:
                setVar(instruction.dst, object.position.x);
                break;
            case PackageScriptOp::SetVarObjectPositionY:
                setVar(instruction.dst, object.position.y);
                break;
            case PackageScriptOp::SetVarObjectVelocityX:
                setVar(instruction.dst, object.velocity.x);
                break;
            case PackageScriptOp::SetVarObjectVelocityY:
                setVar(instruction.dst, object.velocity.y);
                break;
            case PackageScriptOp::SetVarObjectAnimationFrame:
                setVar(instruction.dst, object.animationFrame);
                break;
            case PackageScriptOp::SetVarObjectAnimationRate:
                setVar(instruction.dst, object.animationRate);
                break;
            case PackageScriptOp::SetVarOwnedObjectCount:
                setVar(instruction.dst, countGameObjectsOwnedBy(world, object.ownerFighter, instruction.text));
                break;
            case PackageScriptOp::SetVarOwnerFighterVar:
                setVar(instruction.dst, ownerFighterVar(instruction.intValue));
                break;
            case PackageScriptOp::SetOwnerFighterVarImmediate:
                setOwnerFighterVar(instruction.dst, instruction.intValue);
                break;
            case PackageScriptOp::SetOwnerFighterVarFromVar:
                setOwnerFighterVar(instruction.dst, var(instruction.srcA));
                break;
            case PackageScriptOp::CallOwnerFighterScript:
                if (validFighterIndex(world, object.ownerFighter)) {
                    FighterRuntime& owner = world.fighters[static_cast<size_t>(object.ownerFighter)];
                    runPackageScript(world, owner, instruction.text);
                    return;
                }
                break;
            case PackageScriptOp::SetVarIndexedFighterVar:
                setVar(instruction.dst, indexedFighterVar(var(instruction.srcA), instruction.intValue));
                break;
            case PackageScriptOp::SetIndexedFighterVarImmediate:
                setIndexedFighterVar(var(instruction.srcA), instruction.dst, instruction.intValue);
                break;
            case PackageScriptOp::SetIndexedFighterVarFromVar:
                setIndexedFighterVar(var(instruction.srcA), instruction.dst, var(instruction.srcB));
                break;
            case PackageScriptOp::CallIndexedFighterScriptFromVar: {
                const int targetIndex = var(instruction.srcA);
                if (FighterRuntime* target = indexedFighter(targetIndex)) {
                    runPackageScript(world, *target, instruction.text);
                    return;
                }
                break;
            }
            case PackageScriptOp::SetVarIndexedFighterStateIndex: {
                const FighterRuntime* target = indexedFighter(var(instruction.srcA));
                setVar(instruction.dst, target ? target->state : -1);
                break;
            }
            case PackageScriptOp::SetVarIndexedFighterPositionX: {
                const FighterRuntime* target = indexedFighter(var(instruction.srcA));
                setVar(instruction.dst, target ? target->position.x : 0);
                break;
            }
            case PackageScriptOp::SetVarIndexedFighterPositionY: {
                const FighterRuntime* target = indexedFighter(var(instruction.srcA));
                setVar(instruction.dst, target ? target->position.y : 0);
                break;
            }
            case PackageScriptOp::SetIndexedFighterStateFromVar: {
                FighterRuntime* target = indexedFighter(var(instruction.dst));
                if (target) {
                    const FighterDefinition& targetDef = world.fighterDefs[static_cast<size_t>(target->fighterDef)];
                    const int stateIndex = var(instruction.srcA);
                    if (stateIndex >= 0 && stateIndex < static_cast<int>(targetDef.states.size())) {
                        changeFighterState(world, *target, targetDef.states[static_cast<size_t>(stateIndex)].name);
                        return;
                    }
                }
                break;
            }
            case PackageScriptOp::SetIndexedFighterPositionFromVars: {
                FighterRuntime* target = indexedFighter(var(instruction.dst));
                if (target) {
                    target->position = {var(instruction.srcA), var(instruction.srcB)};
                    target->previousPosition = target->position;
                    const FighterDefinition& targetDef = world.fighterDefs[static_cast<size_t>(target->fighterDef)];
                    calculateEcb(targetDef, *target, true);
                }
                break;
            }
            case PackageScriptOp::SetIndexedFighterFacingFromVar: {
                FighterRuntime* target = indexedFighter(var(instruction.dst));
                if (target) {
                    target->facing = var(instruction.srcA) < 0 ? -1 : 1;
                    target->poseFacing = target->facing;
                }
                break;
            }
            case PackageScriptOp::SetVarIndexedObjectVar:
                setVar(instruction.dst, indexedObjectVar(var(instruction.srcA), instruction.intValue));
                break;
            case PackageScriptOp::SetIndexedObjectVarImmediate:
                setIndexedObjectVar(var(instruction.srcA), instruction.dst, instruction.intValue);
                break;
            case PackageScriptOp::SetIndexedObjectVarFromVar:
                setIndexedObjectVar(var(instruction.srcA), instruction.dst, var(instruction.srcB));
                break;
            case PackageScriptOp::CallIndexedObjectScriptFromVar: {
                const int targetIndex = var(instruction.srcA);
                if (targetIndex == static_cast<int>(objectIndex)) {
                    const auto target = std::find_if(def.packageScripts.begin(), def.packageScripts.end(), [&](const PackageScript& script) {
                        return script.name == instruction.text;
                    });
                    ++frame.instructionIndex;
                    if (target != def.packageScripts.end()) {
                        scriptStack.push_back({&*target, 0});
                    }
                    continue;
                }
                if (validGameObjectIndex(world, targetIndex)) {
                    runGameObjectPackageScript(world, targetIndex, instruction.text);
                    return;
                }
                break;
            }
            case PackageScriptOp::SetVarButtonDown:
                if (const InputBuffer* input = ownerInput()) {
                    setVar(instruction.dst, (input->frames[0].buttons & instruction.intValue) != 0 ? 1 : 0);
                } else {
                    setVar(instruction.dst, 0);
                }
                break;
            case PackageScriptOp::SetVarButtonPressed:
                if (const InputBuffer* input = ownerInput()) {
                    setVar(instruction.dst, input->justPressed(static_cast<uint16_t>(instruction.intValue)) ? 1 : 0);
                } else {
                    setVar(instruction.dst, 0);
                }
                break;
            case PackageScriptOp::SetVarStickX:
                if (const InputBuffer* input = ownerInput()) {
                    setVar(instruction.dst, input->frames[0].move.x);
                } else {
                    setVar(instruction.dst, 0);
                }
                break;
            case PackageScriptOp::SetVarStickY:
                if (const InputBuffer* input = ownerInput()) {
                    setVar(instruction.dst, input->frames[0].move.y);
                } else {
                    setVar(instruction.dst, 0);
                }
                break;
            case PackageScriptOp::SetVarCStickX:
                if (const InputBuffer* input = ownerInput()) {
                    setVar(instruction.dst, input->frames[0].cStick.x);
                } else {
                    setVar(instruction.dst, 0);
                }
                break;
            case PackageScriptOp::SetVarCStickY:
                if (const InputBuffer* input = ownerInput()) {
                    setVar(instruction.dst, input->frames[0].cStick.y);
                } else {
                    setVar(instruction.dst, 0);
                }
                break;
            case PackageScriptOp::SetVarShield:
                if (const InputBuffer* input = ownerInput()) {
                    setVar(instruction.dst, input->frames[0].shieldAnalog);
                } else {
                    setVar(instruction.dst, 0);
                }
                break;
            case PackageScriptOp::SetGroundVelocity:
            case PackageScriptOp::SetAirVelocityX:
                object.velocity.x = instruction.fixValue;
                break;
            case PackageScriptOp::SetAirVelocityY:
                object.velocity.y = instruction.fixValue;
                break;
            case PackageScriptOp::SetPositionX:
                object.position.x = instruction.fixValue;
                object.previousPosition.x = object.position.x;
                break;
            case PackageScriptOp::SetPositionY:
                object.position.y = instruction.fixValue;
                object.previousPosition.y = object.position.y;
                break;
            case PackageScriptOp::SetAnimationRate:
                object.animationRate = instruction.fixValue;
                break;
            case PackageScriptOp::SetAnimationFrame:
                object.animationFrame = instruction.fixValue;
                break;
            case PackageScriptOp::SetFacing:
                object.facing = instruction.intValue < 0 ? -1 : 1;
                break;
            case PackageScriptOp::SetGroundVelocityFromVar:
            case PackageScriptOp::SetAirVelocityXFromVar:
                object.velocity.x = var(instruction.srcA);
                break;
            case PackageScriptOp::SetAirVelocityYFromVar:
                object.velocity.y = var(instruction.srcA);
                break;
            case PackageScriptOp::SetPositionXFromVar:
                object.position.x = var(instruction.srcA);
                object.previousPosition.x = object.position.x;
                break;
            case PackageScriptOp::SetPositionYFromVar:
                object.position.y = var(instruction.srcA);
                object.previousPosition.y = object.position.y;
                break;
            case PackageScriptOp::SetAnimationRateFromVar:
                object.animationRate = var(instruction.srcA);
                break;
            case PackageScriptOp::SetAnimationFrameFromVar:
                object.animationFrame = var(instruction.srcA);
                break;
            case PackageScriptOp::SetFacingFromVar:
                object.facing = var(instruction.srcA) < 0 ? -1 : 1;
                break;
            case PackageScriptOp::ChangeState:
                changeGameObjectState(world, object, instruction.text);
                return;
            case PackageScriptOp::SpawnObject: {
                const Vec2 position{object.position.x, object.position.y + instruction.intValue};
                spawnGameObject(world, instruction.text, object.ownerFighter, position, object.facing, {object.facing * instruction.fixValue, 0});
                break;
            }
            case PackageScriptOp::SpawnProjectile: {
                const Vec2 position{object.position.x, object.position.y + instruction.intValue};
                spawnGameObjectOfKind(world, instruction.text, GameObjectKind::Projectile, object.ownerFighter, position, object.facing, {object.facing * instruction.fixValue, 0});
                break;
            }
            case PackageScriptOp::SpawnObjectSetVar:
            case PackageScriptOp::SpawnProjectileSetVar: {
                const int ownerFighter = object.ownerFighter;
                const int facing = object.facing;
                const Vec2 position{object.position.x, object.position.y + instruction.intValue};
                const Vec2 velocity{facing * instruction.fixValue, 0};
                const int spawnedIndex = instruction.op == PackageScriptOp::SpawnProjectileSetVar
                    ? spawnGameObjectOfKind(world, instruction.text, GameObjectKind::Projectile, ownerFighter, position, facing, velocity)
                    : spawnGameObject(world, instruction.text, ownerFighter, position, facing, velocity);
                if (objectIndex < world.objects.size()) {
                    GameObjectRuntime& currentObject = world.objects[objectIndex];
                    if (instruction.dst >= 0 && instruction.dst < static_cast<int>(currentObject.packageVars.size())) {
                        currentObject.packageVars[static_cast<size_t>(instruction.dst)] = spawnedIndex;
                    }
                }
                return;
            }
            case PackageScriptOp::SpawnObjectFromVarsSetVar:
            case PackageScriptOp::SpawnProjectileFromVarsSetVar: {
                const int ownerFighter = object.ownerFighter;
                const int facing = object.facing;
                const Vec2 position{object.position.x + facing * instruction.fixValue, object.position.y + instruction.intValue};
                const Vec2 velocity{facing * var(instruction.srcA), var(instruction.srcB)};
                const int spawnedIndex = instruction.op == PackageScriptOp::SpawnProjectileFromVarsSetVar
                    ? spawnGameObjectOfKind(world, instruction.text, GameObjectKind::Projectile, ownerFighter, position, facing, velocity)
                    : spawnGameObject(world, instruction.text, ownerFighter, position, facing, velocity);
                if (objectIndex < world.objects.size()) {
                    GameObjectRuntime& currentObject = world.objects[objectIndex];
                    if (instruction.dst >= 0 && instruction.dst < static_cast<int>(currentObject.packageVars.size())) {
                        currentObject.packageVars[static_cast<size_t>(instruction.dst)] = spawnedIndex;
                    }
                }
                return;
            }
            case PackageScriptOp::SpawnObjectFromVars:
            case PackageScriptOp::SpawnProjectileFromVars: {
                const Vec2 position{object.position.x + object.facing * instruction.fixValue, object.position.y + instruction.intValue};
                const Vec2 velocity{object.facing * var(instruction.srcA), var(instruction.srcB)};
                if (instruction.op == PackageScriptOp::SpawnProjectileFromVars) {
                    spawnGameObjectOfKind(world, instruction.text, GameObjectKind::Projectile, object.ownerFighter, position, object.facing, velocity);
                } else {
                    spawnGameObject(world, instruction.text, object.ownerFighter, position, object.facing, velocity);
                }
                break;
            }
            case PackageScriptOp::DestroyObject:
                deactivateGameObject(world, objectIndex);
                return;
            case PackageScriptOp::DestroyObjectFromVar:
                destroyGameObjectByIndex(world, var(instruction.srcA));
                return;
            case PackageScriptOp::SetVarPickUpObjectFromVar: {
                const bool ok = pickUpGameObject(world, objectIndex, var(instruction.srcA));
                setCurrentObjectVarAfterEvent(instruction.dst, ok ? 1 : 0);
                return;
            }
            case PackageScriptOp::SetVarDropObjectFromVar: {
                const Vec2 velocity{object.facing * instruction.fixValue, instruction.intValue};
                const bool ok = dropGameObject(world, objectIndex, velocity);
                setCurrentObjectVarAfterEvent(instruction.dst, ok ? 1 : 0);
                return;
            }
            case PackageScriptOp::SetVarThrowObjectFromVar: {
                const int fighterIndex = var(instruction.srcA);
                const int velocityFacing = validObjectOwnerFighterIndex(world, fighterIndex)
                    ? world.fighters[static_cast<size_t>(fighterIndex)].facing
                    : object.facing;
                const Vec2 velocity{velocityFacing * instruction.fixValue, instruction.intValue};
                const bool ok = throwGameObject(world, objectIndex, fighterIndex, velocity);
                setCurrentObjectVarAfterEvent(instruction.dst, ok ? 1 : 0);
                return;
            }
            case PackageScriptOp::SetVarReflectObjectFromVar: {
                const int fighterIndex = var(instruction.srcA);
                const Vec2 normal{instruction.fixValue, instruction.intValue};
                const bool ok = reflectGameObject(world, objectIndex, fighterIndex, normal);
                setCurrentObjectVarAfterEvent(instruction.dst, ok ? 1 : 0);
                return;
            }
            case PackageScriptOp::SetVarAbsorbObjectFromVar: {
                const bool ok = absorbGameObject(world, objectIndex, var(instruction.srcA));
                setCurrentObjectVarAfterEvent(instruction.dst, ok ? 1 : 0);
                return;
            }
            case PackageScriptOp::SetVarShieldBounceObjectFromVar: {
                const int fighterIndex = var(instruction.srcA);
                const Vec2 normal{instruction.fixValue, instruction.intValue};
                const bool ok = shieldBounceGameObject(world, objectIndex, fighterIndex, normal);
                setCurrentObjectVarAfterEvent(instruction.dst, ok ? 1 : 0);
                return;
            }
            case PackageScriptOp::SetVarInteractObjectFromVar: {
                const bool ok = interactGameObjectWithFighter(world, objectIndex, var(instruction.srcA));
                setCurrentObjectVarAfterEvent(instruction.dst, ok ? 1 : 0);
                return;
            }
            case PackageScriptOp::SetVarInteractObjectsFromVars: {
                const bool ok = interactGameObjects(world, var(instruction.srcA), var(instruction.srcB));
                setCurrentObjectVarAfterEvent(instruction.dst, ok ? 1 : 0);
                return;
            }
            case PackageScriptOp::DestroyOwnedObjects:
                destroyGameObjectsOwnedBy(world, object.ownerFighter, instruction.text);
                if (!object.active) {
                    return;
                }
                break;
            case PackageScriptOp::SkipIfVarLessThanImmediate:
                frame.instructionIndex += var(instruction.dst) < instruction.intValue ? 2 : 1;
                continue;
            case PackageScriptOp::SkipIfVarLessThanVar:
                frame.instructionIndex += var(instruction.srcA) < var(instruction.srcB) ? 2 : 1;
                continue;
            case PackageScriptOp::SkipIfVarEqualImmediate:
                frame.instructionIndex += var(instruction.dst) == instruction.intValue ? 2 : 1;
                continue;
            case PackageScriptOp::SkipIfVarEqualVar:
                frame.instructionIndex += var(instruction.srcA) == var(instruction.srcB) ? 2 : 1;
                continue;
            case PackageScriptOp::JumpRelative: {
                const int target = static_cast<int>(instructionIndex) + instruction.intValue;
                if (target < 0 || target > static_cast<int>(frame.script->instructions.size())) {
                    return;
                }
                frame.instructionIndex = static_cast<size_t>(target);
                continue;
            }
            case PackageScriptOp::CallScript: {
                const auto target = std::find_if(def.packageScripts.begin(), def.packageScripts.end(), [&](const PackageScript& script) {
                    return script.name == instruction.text;
                });
                ++frame.instructionIndex;
                if (target != def.packageScripts.end()) {
                    scriptStack.push_back({&*target, 0});
                }
                continue;
            }
            case PackageScriptOp::SpawnFighter: {
                const Vec2 position{
                    object.position.x + object.facing * instruction.fixValue,
                    object.position.y,
                };
                spawnFighter(world, instruction.text, position, object.facing);
                return;
            }
            case PackageScriptOp::SpawnFighterSetVar: {
                const Vec2 position{
                    object.position.x + object.facing * instruction.fixValue,
                    object.position.y,
                };
                const int spawnedIndex = spawnFighter(world, instruction.text, position, object.facing);
                setCurrentObjectVarAfterEvent(instruction.dst, spawnedIndex);
                break;
            }
            case PackageScriptOp::SwitchFighterDefinition:
                return;
            }
            ++frame.instructionIndex;
        }
        return;
    }
    if (fn.name == "object_destroy") {
        deactivateGameObject(world, objectIndex);
        return;
    }
    if (fn.name == "object_lifetime") {
        const GameObjectStateDefinition* state = currentGameObjectState(world, object);
        const bool stateExpired = state && state->animationLengthFrames > 0 && !state->loopAnimation &&
            frameInObjectState(world, object) >= state->animationLengthFrames;
        const bool objectExpired = def.lifetimeFrames > 0 && object.internalFrame >= def.lifetimeFrames;
        if (stateExpired || objectExpired) {
            deactivateGameObject(world, objectIndex);
        }
        return;
    }
    if (fn.name == "object_blast_destroy") {
        if (object.position.x < world.stage.blastMin.x || object.position.x > world.stage.blastMax.x ||
            object.position.y < world.stage.blastMin.y || object.position.y > world.stage.blastMax.y)
        {
            deactivateGameObject(world, objectIndex);
        }
        return;
    }
    if (fn.name == "object_linear_physics") {
        objectLinearPhysics(object);
        return;
    }
    if (fn.name == "object_gravity_physics") {
        objectGravityPhysics(def, object);
        return;
    }
    if (fn.name == "object_floor_stop") {
        int floorSegment = -1;
        Fix floorY = 0;
        if (objectFloorContact(world, object, floorSegment, floorY)) {
            object.position.y = floorY;
            object.velocity.y = 0;
            object.grounded = true;
            object.groundSegment = floorSegment;
        } else {
            object.grounded = false;
            object.groundSegment = -1;
        }
        return;
    }
    if (fn.name == "object_clear_fighter_reference") {
        clearGameObjectFighterReference(world, objectIndex, object.lastInteractionFighter);
    }
}

void runGameObjectPackageScript(World& world, int objectIndex, const std::string& scriptName) {
    if (objectIndex < 0) {
        return;
    }
    runGameObjectFunction(world, static_cast<size_t>(objectIndex), FunctionCall{"script:" + scriptName});
}

static void runGameObjectFunctions(World& world, size_t objectIndex, const std::vector<FunctionCall>& functions) {
    for (const FunctionCall& fn : functions) {
        runGameObjectFunction(world, objectIndex, fn);
        if (objectIndex >= world.objects.size() || !world.objects[objectIndex].active) {
            return;
        }
    }
}

static const std::vector<FunctionCall>& gameObjectEventCallbacks(const GameObjectDefinition& def, GameObjectEvent event) {
    static const std::vector<FunctionCall> empty;
    switch (event) {
    case GameObjectEvent::Spawned:
        return def.onSpawned;
    case GameObjectEvent::Destroyed:
        return def.onDestroyed;
    case GameObjectEvent::PickedUp:
        return def.onPickedUp;
    case GameObjectEvent::Dropped:
        return def.onDropped;
    case GameObjectEvent::Thrown:
        return def.onThrown;
    case GameObjectEvent::DamageDealt:
        return def.onDamageDealt;
    case GameObjectEvent::DamageReceived:
        return def.onDamageReceived;
    case GameObjectEvent::Clanked:
        return def.onClanked;
    case GameObjectEvent::Reflected:
        return def.onReflected;
    case GameObjectEvent::Absorbed:
        return def.onAbsorbed;
    case GameObjectEvent::ShieldBounced:
        return def.onShieldBounced;
    case GameObjectEvent::HitShield:
        return def.onHitShield;
    case GameObjectEvent::EnteredAir:
        return def.onEnteredAir;
    case GameObjectEvent::EnteredHitlag:
        return def.onEnteredHitlag;
    case GameObjectEvent::ExitedHitlag:
        return def.onExitedHitlag;
    case GameObjectEvent::Touched:
        return def.onTouched;
    case GameObjectEvent::JumpedOn:
        return def.onJumpedOn;
    case GameObjectEvent::GrabDealt:
        return def.onGrabDealt;
    case GameObjectEvent::GrabbedForVictim:
        return def.onGrabbedForVictim;
    case GameObjectEvent::Interaction:
        return def.onInteraction;
    }
    return empty;
}

static void runGameObjectEvent(World& world, size_t objectIndex, GameObjectEvent event) {
    if (objectIndex >= world.objects.size()) {
        return;
    }
    const GameObjectRuntime& object = world.objects[objectIndex];
    if (!object.active || object.objectDef < 0 || object.objectDef >= static_cast<int>(world.objectDefs.size())) {
        return;
    }
    runGameObjectFunctions(world, objectIndex, gameObjectEventCallbacks(world.objectDefs[static_cast<size_t>(object.objectDef)], event));
}

static void deactivateGameObject(World& world, size_t objectIndex) {
    if (objectIndex >= world.objects.size()) {
        return;
    }
    GameObjectRuntime& object = world.objects[objectIndex];
    if (!object.active) {
        return;
    }
    if (object.destroyEventDispatched) {
        object.active = false;
        return;
    }
    if (validObjectOwnerFighterIndex(world, object.heldByFighter) &&
        world.fighters[static_cast<size_t>(object.heldByFighter)].heldObject == static_cast<int>(objectIndex))
    {
        world.fighters[static_cast<size_t>(object.heldByFighter)].heldObject = -1;
    }
    object.heldByFighter = -1;
    object.destroyEventDispatched = true;
    runGameObjectEvent(world, objectIndex, GameObjectEvent::Destroyed);
    if (objectIndex < world.objects.size()) {
        world.objects[objectIndex].active = false;
    }
}

static void setGameObjectHitlag(World& world, size_t objectIndex, int hitlagFrames) {
    if (objectIndex >= world.objects.size() || hitlagFrames <= 0) {
        return;
    }
    GameObjectRuntime& object = world.objects[objectIndex];
    if (!object.active) {
        return;
    }
    const bool enteringHitlag = object.hitlag <= 0;
    object.hitlag = std::max(object.hitlag, hitlagFrames);
    if (enteringHitlag) {
        runGameObjectEvent(world, objectIndex, GameObjectEvent::EnteredHitlag);
    }
}

void changeGameObjectState(World& world, GameObjectRuntime& object, const std::string& stateName) {
    if (object.objectDef < 0 || object.objectDef >= static_cast<int>(world.objectDefs.size())) {
        return;
    }
    const GameObjectDefinition& def = world.objectDefs[static_cast<size_t>(object.objectDef)];
    const auto found = std::find_if(def.states.begin(), def.states.end(), [&](const GameObjectStateDefinition& state) {
        return state.name == stateName;
    });
    if (found == def.states.end()) {
        return;
    }
    object.state = static_cast<int>(std::distance(def.states.begin(), found));
    object.lastStateChangeFrame = world.frame;
    object.animationFrame = 0;
    object.animationRate = fx(1);
    object.fightersHit.clear();
    for (ActiveHitbox& hitbox : object.activeHitboxes) {
        hitbox.firstFrame = true;
    }

    const auto objectIt = std::find_if(world.objects.begin(), world.objects.end(), [&](const GameObjectRuntime& candidate) {
        return &candidate == &object;
    });
    if (objectIt != world.objects.end()) {
        runGameObjectFunctions(world, static_cast<size_t>(std::distance(world.objects.begin(), objectIt)), found->onEnter);
    }
}

static void tickGameObjects(World& world) {
    for (size_t objectIndex = 0; objectIndex < world.objects.size(); ++objectIndex) {
        GameObjectRuntime& object = world.objects[objectIndex];
        if (!object.active || object.objectDef < 0 || object.objectDef >= static_cast<int>(world.objectDefs.size())) {
            object.active = false;
            continue;
        }
        if (object.hitlag > 0) {
            --object.hitlag;
            if (object.active && object.hitlag == 0) {
                runGameObjectEvent(world, objectIndex, GameObjectEvent::ExitedHitlag);
            }
            continue;
        }
        if (gameObjectIsHeld(object)) {
            if (!validObjectOwnerFighterIndex(world, object.heldByFighter)) {
                object.heldByFighter = -1;
            } else if (world.fighters[static_cast<size_t>(object.heldByFighter)].heldObject != static_cast<int>(objectIndex)) {
                object.heldByFighter = -1;
            } else {
                object.previousPosition = object.position;
                object.position = gameObjectHoldPosition(world, object.heldByFighter);
                object.velocity = {};
                object.grounded = false;
                object.groundSegment = -1;
                continue;
            }
        }
        const bool wasGrounded = object.grounded;
        ++object.internalFrame;
        object.animationFrame += object.animationRate;
        const GameObjectStateDefinition* state = currentGameObjectState(world, object);
        if (state) {
            runGameObjectFunctions(world, objectIndex, state->onFrame);
            if (!object.active) {
                continue;
            }
            runGameObjectFunctions(world, objectIndex, state->onPhysics);
            if (!object.active) {
                continue;
            }
            runGameObjectFunctions(world, objectIndex, state->onCollision);
            if (object.active && wasGrounded && !object.grounded) {
                runGameObjectEvent(world, objectIndex, GameObjectEvent::EnteredAir);
            }
        } else {
            objectGravityPhysics(world.objectDefs[static_cast<size_t>(object.objectDef)], object);
            runGameObjectFunction(world, objectIndex, FunctionCall{"object_lifetime"});
            if (object.active && wasGrounded && !object.grounded) {
                runGameObjectEvent(world, objectIndex, GameObjectEvent::EnteredAir);
            }
        }
        if (object.active) {
            const GameObjectDefinition& def = world.objectDefs[static_cast<size_t>(object.objectDef)];
            runGameObjectFunctions(world, objectIndex, def.onAccessory);
        }
    }
}

static void updateActiveObjectHitboxPositions(World& world) {
    for (GameObjectRuntime& object : world.objects) {
        if (!object.active) {
            continue;
        }
        for (ActiveHitbox& hitbox : object.activeHitboxes) {
            const Vec3 worldPos = objectHitboxWorld(object, hitbox.def);
            if (hitbox.firstFrame) {
                hitbox.firstFrame = false;
                hitbox.current = worldPos;
                hitbox.previous = worldPos;
            } else {
                hitbox.previous = hitbox.current;
                hitbox.current = worldPos;
            }
        }
    }
}

static int clankDamageValue(const HitboxDefinition& hitbox) {
    const int damage = static_cast<int>(fxToFloat(hitbox.damage));
    return hitbox.damage > 0 ? std::max(1, damage) : 0;
}

static void enterReboundFromClankAt(World& world, size_t fighterIndex, Vec2 otherPosition, int damageValue) {
    FighterRuntime& fighter = world.fighters[fighterIndex];
    if (!fighter.grounded || damageValue <= 0 || currentState(world, fighter).name == "ReboundStop" ||
        currentState(world, fighter).name == "Rebound")
    {
        return;
    }
    const MeleeCommonData& common = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)].properties.common;
    fighter.reboundDamageVelocity =
        fxMul(fx(damageValue), common.reboundDamageScaleX3D0) + common.reboundDamageBaseX3D4;
    fighter.reboundFacingDir = otherPosition.x > fighter.position.x ? 1 : -1;
    changeFighterState(world, fighter, "ReboundStop");
}

static void enterReboundFromClank(World& world, size_t fighterIndex, size_t otherIndex, int damageValue) {
    enterReboundFromClankAt(world, fighterIndex, world.fighters[otherIndex].position, damageValue);
}

static bool canClankHitbox(const ActiveHitbox& hitbox) {
    return hitbox.def.hitFighters && !hitbox.def.isGrab &&
        (hitbox.def.canClank || hitbox.def.reboundsOnClank);
}

static void resolveHitboxClanks(World& world) {
    for (size_t aIndex = 0; aIndex < world.fighters.size(); ++aIndex) {
        FighterRuntime& a = world.fighters[aIndex];
        for (size_t bIndex = aIndex + 1; bIndex < world.fighters.size(); ++bIndex) {
            FighterRuntime& b = world.fighters[bIndex];
            bool aRebound = false;
            bool bRebound = false;
            int aDamage = 0;
            int bDamage = 0;
            for (const ActiveHitbox& aHitbox : a.activeHitboxes) {
                if (!canClankHitbox(aHitbox)) {
                    continue;
                }
                const Capsule aCapsule{aHitbox.previous, aHitbox.current, aHitbox.def.radius};
                for (const ActiveHitbox& bHitbox : b.activeHitboxes) {
                    if (!canClankHitbox(bHitbox)) {
                        continue;
                    }
                    const Capsule bCapsule{bHitbox.previous, bHitbox.current, bHitbox.def.radius};
                    if (!capsuleCapsule(aCapsule, bCapsule)) {
                        continue;
                    }
                    aDamage = std::max(aDamage, clankDamageValue(bHitbox.def));
                    bDamage = std::max(bDamage, clankDamageValue(aHitbox.def));
                    aRebound = aRebound || aHitbox.def.reboundsOnClank;
                    bRebound = bRebound || bHitbox.def.reboundsOnClank;
                }
            }
            if (aRebound) {
                enterReboundFromClank(world, aIndex, bIndex, aDamage);
            }
            if (bRebound) {
                enterReboundFromClank(world, bIndex, aIndex, bDamage);
            }
        }
    }
}

static void resolveFighterObjectHitboxClanks(World& world) {
    for (size_t fighterIndex = 0; fighterIndex < world.fighters.size(); ++fighterIndex) {
        FighterRuntime& fighter = world.fighters[fighterIndex];
        bool fighterRebound = false;
        int fighterDamage = 0;
        int fighterHitlag = 0;
        Vec2 reboundPosition = fighter.position;
        for (const ActiveHitbox& fighterHitbox : fighter.activeHitboxes) {
            if (!canClankHitbox(fighterHitbox)) {
                continue;
            }
            const Capsule fighterCapsule{fighterHitbox.previous, fighterHitbox.current, fighterHitbox.def.radius};
            for (size_t objectIndex = 0; objectIndex < world.objects.size(); ++objectIndex) {
                GameObjectRuntime& object = world.objects[objectIndex];
                if (!object.active || gameObjectIsHeld(object)) {
                    continue;
                }
                for (const ActiveHitbox& objectHitbox : object.activeHitboxes) {
                    if (!canClankHitbox(objectHitbox)) {
                        continue;
                    }
                    const Capsule objectCapsule{objectHitbox.previous, objectHitbox.current, objectHitbox.def.radius};
                    if (!capsuleCapsule(fighterCapsule, objectCapsule)) {
                        continue;
                    }
                    fighterDamage = std::max(fighterDamage, clankDamageValue(objectHitbox.def));
                    fighterHitlag = std::max(fighterHitlag, hitlagFramesForHitbox(objectHitbox.def));
                    fighterRebound = fighterRebound || fighterHitbox.def.reboundsOnClank;
                    reboundPosition = object.position;
                    runGameObjectEvent(world, objectIndex, GameObjectEvent::Clanked);
                    if (object.active) {
                        setGameObjectHitlag(world, objectIndex, hitlagFramesForHitbox(fighterHitbox.def));
                    }
                    break;
                }
            }
        }
        if (fighterRebound) {
            enterReboundFromClankAt(world, fighterIndex, reboundPosition, fighterDamage);
        }
        if (fighterHitlag > 0) {
            fighter.hitlag = std::max(fighter.hitlag, fighterHitlag);
        }
    }
}

static void resolveObjectHitboxClanks(World& world) {
    for (size_t aIndex = 0; aIndex < world.objects.size(); ++aIndex) {
        if (!world.objects[aIndex].active || gameObjectIsHeld(world.objects[aIndex])) {
            continue;
        }
        for (size_t bIndex = aIndex + 1; bIndex < world.objects.size(); ++bIndex) {
            if (!world.objects[bIndex].active || gameObjectIsHeld(world.objects[bIndex])) {
                continue;
            }
            bool clanked = false;
            int aHitlag = 0;
            int bHitlag = 0;
            for (const ActiveHitbox& aHitbox : world.objects[aIndex].activeHitboxes) {
                if (!canClankHitbox(aHitbox)) {
                    continue;
                }
                const Capsule aCapsule{aHitbox.previous, aHitbox.current, aHitbox.def.radius};
                for (const ActiveHitbox& bHitbox : world.objects[bIndex].activeHitboxes) {
                    if (!canClankHitbox(bHitbox)) {
                        continue;
                    }
                    const Capsule bCapsule{bHitbox.previous, bHitbox.current, bHitbox.def.radius};
                    if (capsuleCapsule(aCapsule, bCapsule)) {
                        clanked = true;
                        aHitlag = std::max(aHitlag, hitlagFramesForHitbox(bHitbox.def));
                        bHitlag = std::max(bHitlag, hitlagFramesForHitbox(aHitbox.def));
                        break;
                    }
                }
                if (clanked) {
                    break;
                }
            }
            if (clanked) {
                runGameObjectEvent(world, aIndex, GameObjectEvent::Clanked);
                runGameObjectEvent(world, bIndex, GameObjectEvent::Clanked);
                if (world.objects[aIndex].active) {
                    setGameObjectHitlag(world, aIndex, aHitlag);
                }
                if (world.objects[bIndex].active) {
                    setGameObjectHitlag(world, bIndex, bHitlag);
                }
            }
        }
    }
}

static void updateAndCheckHitboxes(World& world, size_t attackerIndex) {
    FighterRuntime& attacker = world.fighters[attackerIndex];
    for (ActiveHitbox& hitbox : attacker.activeHitboxes) {
        if (!hitbox.def.hitFighters) {
            continue;
        }
        for (size_t victimIndex = 0; victimIndex < world.fighters.size(); ++victimIndex) {
            if (victimIndex == attackerIndex) {
                continue;
            }
            if (damageFlyHitboxIgnoresVictim(world, attacker, victimIndex)) {
                continue;
            }
            if (thrownHitboxIgnoresOwner(attacker, victimIndex)) {
                continue;
            }
            FighterRuntime& victim = world.fighters[victimIndex];
            if (!hitbox.def.onlyHitGrabbed && fightersAreInActiveCaptureLink(attacker, attackerIndex, victim, victimIndex)) {
                continue;
            }
            if (std::find(attacker.fightersHitThisAction.begin(), attacker.fightersHitThisAction.end(), static_cast<int>(victimIndex)) != attacker.fightersHitThisAction.end()) {
                continue;
            }
            const FighterDefinition& victimDef = world.fighterDefs[static_cast<size_t>(victim.fighterDef)];
            if (hitbox.def.onlyHitGrabbed && attacker.grabbedFighter != static_cast<int>(victimIndex)) {
                continue;
            }
            if ((victim.grounded && !hitbox.def.hitGrounded) || (!victim.grounded && !hitbox.def.hitAirborne)) {
                continue;
            }
            const bool usePoseHurtboxes = !victim.poseHurtboxCapsules.empty();
            const size_t hurtboxCount = usePoseHurtboxes ? victim.poseHurtboxCapsules.size() : victimDef.hurtboxes.size();
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
            if (hitbox.def.onlyHitGrabbed && attacker.grabbedFighter == static_cast<int>(victimIndex)) {
                attacker.fightersHitThisAction.push_back(static_cast<int>(victimIndex));
                applyCapturedDamage(world, attackerIndex, attacker, victim, hitbox.def);
                continue;
            }
            if (!hitbox.def.isGrab && isShieldActiveState(currentState(world, victim)) && victim.shieldHealth > 0) {
                const Vec3 center = shieldCenterWorld(victimDef, victim);
                const Fix radius = currentShieldRadius(victimDef, victim);
                if (capsuleCapsule(hitCapsule, {center, center, radius})) {
                    attacker.fightersHitThisAction.push_back(static_cast<int>(victimIndex));
                    applyShieldHit(world, attacker, victim, hitbox.def);
                    continue;
                }
            }
            if (usePoseHurtboxes) {
                for (size_t hurtboxIndex = 0; hurtboxIndex < victim.poseHurtboxCapsules.size(); ++hurtboxIndex) {
                    const HurtboxState capsuleState = hurtboxIndex < victim.hurtboxStates.size() ? victim.hurtboxStates[hurtboxIndex] : HurtboxState::Normal;
                    const HurtboxState state = victim.bodyCollisionState == HurtboxState::Normal ? capsuleState : victim.bodyCollisionState;
                    if (state == HurtboxState::Intangible) {
                        continue;
                    }
                    if (hitbox.def.isGrab &&
                        (hurtboxIndex >= victimDef.hurtboxes.size() ||
                         !victimDef.hurtboxes[hurtboxIndex].grabbable))
                    {
                        continue;
                    }
                    if (capsuleCapsule(hitCapsule, victim.poseHurtboxCapsules[hurtboxIndex])) {
                        attacker.fightersHitThisAction.push_back(static_cast<int>(victimIndex));
                        if (hitbox.def.isGrab) {
                            captureVictim(world, attackerIndex, victimIndex);
                            return;
                        } else if (state == HurtboxState::Invincible) {
                            applyInvincibleHit(attacker, victim, hitbox.def);
                        } else {
                            applyHit(world, attackerIndex, attacker, victim, victimIndex, hitbox.def, hurtboxIndex);
                        }
                        break;
                    }
                }
                continue;
            }
            for (size_t hurtboxIndex = 0; hurtboxIndex < victimDef.hurtboxes.size(); ++hurtboxIndex) {
                const HurtboxDefinition& hurtbox = victimDef.hurtboxes[hurtboxIndex];
                const HurtboxState capsuleState = victim.hurtboxStates[hurtboxIndex] == HurtboxState::Normal ? hurtbox.state : victim.hurtboxStates[hurtboxIndex];
                const HurtboxState state = victim.bodyCollisionState == HurtboxState::Normal ? capsuleState : victim.bodyCollisionState;
                if (state == HurtboxState::Intangible) {
                    continue;
                }
                if (hitbox.def.isGrab && !hurtbox.grabbable) {
                    continue;
                }
                Capsule hurtCapsule{boneWorld(victim, hurtbox.bone, hurtbox.startOffset), boneWorld(victim, hurtbox.bone, hurtbox.endOffset), hurtbox.radius};
                if (capsuleCapsule(hitCapsule, hurtCapsule)) {
                    attacker.fightersHitThisAction.push_back(static_cast<int>(victimIndex));
                    if (hitbox.def.isGrab) {
                        captureVictim(world, attackerIndex, victimIndex);
                        return;
                    } else if (state == HurtboxState::Invincible) {
                        applyInvincibleHit(attacker, victim, hitbox.def);
                    } else {
                        applyHit(world, attackerIndex, attacker, victim, victimIndex, hitbox.def, hurtboxIndex);
                    }
                    break;
                }
            }
        }
    }
}

static void applyObjectDamage(World& world, size_t objectIndex, const HitboxDefinition& hitbox) {
    if (objectIndex >= world.objects.size()) {
        return;
    }
    GameObjectRuntime& object = world.objects[objectIndex];
    if (!object.active || object.objectDef < 0 || object.objectDef >= static_cast<int>(world.objectDefs.size())) {
        return;
    }
    const GameObjectDefinition& def = world.objectDefs[static_cast<size_t>(object.objectDef)];
    object.damageTaken = std::min(fx(999), object.damageTaken + hitbox.damage);
    runGameObjectEvent(world, objectIndex, GameObjectEvent::DamageReceived);
    if (object.active && def.maxDamage > 0 && object.damageTaken >= def.maxDamage) {
        deactivateGameObject(world, objectIndex);
    }
}

static int hitlagFramesForHitbox(const HitboxDefinition& hitbox) {
    return std::max(3, static_cast<int>(fxToFloat(hitbox.damage) / 3.0f) + 3);
}

static bool fighterTouchesCapsule(World& world, size_t fighterIndex, const Capsule& touchCapsule) {
    if (fighterIndex >= world.fighters.size()) {
        return false;
    }
    FighterRuntime& fighter = world.fighters[fighterIndex];
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const bool usePoseHurtboxes = !fighter.poseHurtboxCapsules.empty();
    const size_t hurtboxCount = usePoseHurtboxes ? fighter.poseHurtboxCapsules.size() : def.hurtboxes.size();
    if (fighter.hurtboxStates.size() != hurtboxCount) {
        HurtboxState fillState = HurtboxState::Normal;
        if (!fighter.hurtboxStates.empty() &&
            std::all_of(fighter.hurtboxStates.begin(), fighter.hurtboxStates.end(), [&](HurtboxState state) {
                return state == fighter.hurtboxStates.front();
            })) {
            fillState = fighter.hurtboxStates.front();
        }
        fighter.hurtboxStates.assign(hurtboxCount, fillState);
    }

    if (usePoseHurtboxes) {
        for (size_t hurtboxIndex = 0; hurtboxIndex < fighter.poseHurtboxCapsules.size(); ++hurtboxIndex) {
            const HurtboxState capsuleState = hurtboxIndex < fighter.hurtboxStates.size() ? fighter.hurtboxStates[hurtboxIndex] : HurtboxState::Normal;
            const HurtboxState state = fighter.bodyCollisionState == HurtboxState::Normal ? capsuleState : fighter.bodyCollisionState;
            if (state == HurtboxState::Intangible) {
                continue;
            }
            if (capsuleCapsule(touchCapsule, fighter.poseHurtboxCapsules[hurtboxIndex])) {
                return true;
            }
        }
        return false;
    }

    for (size_t hurtboxIndex = 0; hurtboxIndex < def.hurtboxes.size(); ++hurtboxIndex) {
        const HurtboxDefinition& hurtbox = def.hurtboxes[hurtboxIndex];
        const HurtboxState capsuleState = fighter.hurtboxStates[hurtboxIndex] == HurtboxState::Normal ? hurtbox.state : fighter.hurtboxStates[hurtboxIndex];
        const HurtboxState state = fighter.bodyCollisionState == HurtboxState::Normal ? capsuleState : fighter.bodyCollisionState;
        if (state == HurtboxState::Intangible) {
            continue;
        }
        Capsule hurtCapsule{boneWorld(fighter, hurtbox.bone, hurtbox.startOffset), boneWorld(fighter, hurtbox.bone, hurtbox.endOffset), hurtbox.radius};
        if (capsuleCapsule(touchCapsule, hurtCapsule)) {
            return true;
        }
    }
    return false;
}

static void updateAndCheckHitboxesAgainstObjects(World& world, size_t attackerIndex) {
    FighterRuntime& attacker = world.fighters[attackerIndex];
    for (ActiveHitbox& hitbox : attacker.activeHitboxes) {
        if (!hitbox.def.hitFighters || hitbox.def.isGrab || hitbox.def.onlyHitGrabbed) {
            continue;
        }
        const Capsule hitCapsule{hitbox.previous, hitbox.current, hitbox.def.radius};
        for (size_t objectIndex = 0; objectIndex < world.objects.size(); ++objectIndex) {
            GameObjectRuntime& object = world.objects[objectIndex];
            if (!object.active || gameObjectIsHeld(object) ||
                object.objectDef < 0 || object.objectDef >= static_cast<int>(world.objectDefs.size())) {
                continue;
            }
            if ((object.grounded && !hitbox.def.hitGrounded) || (!object.grounded && !hitbox.def.hitAirborne)) {
                continue;
            }
            const GameObjectDefinition& objectDef = world.objectDefs[static_cast<size_t>(object.objectDef)];
            for (const GameObjectHurtboxDefinition& hurtbox : objectDef.hurtboxes) {
                if (hurtbox.state == HurtboxState::Intangible) {
                    continue;
                }
                if (!capsuleCapsule(hitCapsule, objectHurtboxWorld(object, hurtbox))) {
                    continue;
                }
                if (hurtbox.state != HurtboxState::Invincible) {
                    applyObjectDamage(world, objectIndex, hitbox.def);
                }
                if (object.active) {
                    const int hitlagFrames = hitlagFramesForHitbox(hitbox.def);
                    attacker.hitlag = std::max(attacker.hitlag, hitlagFrames);
                    setGameObjectHitlag(world, objectIndex, hitlagFrames);
                }
                break;
            }
        }
    }
}

static void updateAndCheckObjectHitboxes(World& world, size_t objectIndex) {
    if (objectIndex >= world.objects.size()) {
        return;
    }
    GameObjectRuntime& object = world.objects[objectIndex];
    if (!object.active || gameObjectIsHeld(object) ||
        object.objectDef < 0 || object.objectDef >= static_cast<int>(world.objectDefs.size())) {
        return;
    }
    const GameObjectDefinition& objectDef = world.objectDefs[static_cast<size_t>(object.objectDef)];
    for (ActiveHitbox& hitbox : object.activeHitboxes) {
        if (!hitbox.def.hitFighters) {
            continue;
        }
        const Capsule hitCapsule{hitbox.previous, hitbox.current, hitbox.def.radius};
        for (size_t victimIndex = 0; victimIndex < world.fighters.size(); ++victimIndex) {
            if (!objectDef.hitOwner && object.ownerFighter == static_cast<int>(victimIndex)) {
                continue;
            }
            FighterRuntime& victim = world.fighters[victimIndex];
            if (std::find(object.fightersHit.begin(), object.fightersHit.end(), static_cast<int>(victimIndex)) != object.fightersHit.end()) {
                continue;
            }
            if ((victim.grounded && !hitbox.def.hitGrounded) || (!victim.grounded && !hitbox.def.hitAirborne)) {
                continue;
            }
            const FighterDefinition& victimDef = world.fighterDefs[static_cast<size_t>(victim.fighterDef)];
            if (!hitbox.def.isGrab && isShieldActiveState(currentState(world, victim)) && victim.shieldHealth > 0) {
                const Vec3 center = shieldCenterWorld(victimDef, victim);
                const Fix radius = currentShieldRadius(victimDef, victim);
                if (capsuleCapsule(hitCapsule, {center, center, radius})) {
                    FighterRuntime objectProxy;
                    objectProxy.position = object.position;
                    objectProxy.previousPosition = object.previousPosition;
                    objectProxy.facing = object.facing;
                    object.fightersHit.push_back(static_cast<int>(victimIndex));
                    applyShieldHit(world, objectProxy, victim, hitbox.def);
                    runGameObjectEvent(world, objectIndex, GameObjectEvent::HitShield);
                    if (objectDef.destroyOnShield) {
                        deactivateGameObject(world, objectIndex);
                    }
                    if (object.active) {
                        setGameObjectHitlag(world, objectIndex, hitlagFramesForHitbox(hitbox.def));
                    }
                    return;
                }
            }

            const bool usePoseHurtboxes = !victim.poseHurtboxCapsules.empty();
            const size_t hurtboxCount = usePoseHurtboxes ? victim.poseHurtboxCapsules.size() : victimDef.hurtboxes.size();
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

            if (usePoseHurtboxes) {
                for (size_t hurtboxIndex = 0; hurtboxIndex < victim.poseHurtboxCapsules.size(); ++hurtboxIndex) {
                    const HurtboxState capsuleState = hurtboxIndex < victim.hurtboxStates.size() ? victim.hurtboxStates[hurtboxIndex] : HurtboxState::Normal;
                    const HurtboxState state = victim.bodyCollisionState == HurtboxState::Normal ? capsuleState : victim.bodyCollisionState;
                    if (state == HurtboxState::Intangible) {
                        continue;
                    }
                    if (hitbox.def.isGrab &&
                        (hurtboxIndex >= victimDef.hurtboxes.size() ||
                         !victimDef.hurtboxes[hurtboxIndex].grabbable))
                    {
                        continue;
                    }
                    if (!capsuleCapsule(hitCapsule, victim.poseHurtboxCapsules[hurtboxIndex])) {
                        continue;
                    }
                    object.fightersHit.push_back(static_cast<int>(victimIndex));
                    if (hitbox.def.isGrab) {
                        applyGameObjectGrab(world, objectIndex, victimIndex);
                        return;
                    } else if (state == HurtboxState::Invincible) {
                        FighterRuntime objectProxy;
                        applyInvincibleHit(objectProxy, victim, hitbox.def);
                        if (object.active) {
                            setGameObjectHitlag(world, objectIndex, hitlagFramesForHitbox(hitbox.def));
                        }
                    } else {
                        FighterRuntime objectProxy;
                        objectProxy.position = object.position;
                        objectProxy.previousPosition = object.previousPosition;
                        objectProxy.facing = object.facing;
                        const size_t ownerIndex = object.ownerFighter >= 0 ? static_cast<size_t>(object.ownerFighter) : world.fighters.size();
                        applyHit(world, ownerIndex, objectProxy, victim, victimIndex, hitbox.def, hurtboxIndex);
                        runGameObjectEvent(world, objectIndex, GameObjectEvent::DamageDealt);
                    }
                    if (objectDef.destroyOnHit) {
                        deactivateGameObject(world, objectIndex);
                    }
                    if (object.active) {
                        setGameObjectHitlag(world, objectIndex, hitlagFramesForHitbox(hitbox.def));
                    }
                    return;
                }
                continue;
            }

            for (size_t hurtboxIndex = 0; hurtboxIndex < victimDef.hurtboxes.size(); ++hurtboxIndex) {
                const HurtboxDefinition& hurtbox = victimDef.hurtboxes[hurtboxIndex];
                const HurtboxState capsuleState = victim.hurtboxStates[hurtboxIndex] == HurtboxState::Normal ? hurtbox.state : victim.hurtboxStates[hurtboxIndex];
                const HurtboxState state = victim.bodyCollisionState == HurtboxState::Normal ? capsuleState : victim.bodyCollisionState;
                if (state == HurtboxState::Intangible) {
                    continue;
                }
                if (hitbox.def.isGrab && !hurtbox.grabbable) {
                    continue;
                }
                Capsule hurtCapsule{boneWorld(victim, hurtbox.bone, hurtbox.startOffset), boneWorld(victim, hurtbox.bone, hurtbox.endOffset), hurtbox.radius};
                if (!capsuleCapsule(hitCapsule, hurtCapsule)) {
                    continue;
                }
                object.fightersHit.push_back(static_cast<int>(victimIndex));
                if (hitbox.def.isGrab) {
                    applyGameObjectGrab(world, objectIndex, victimIndex);
                    return;
                } else if (state == HurtboxState::Invincible) {
                    FighterRuntime objectProxy;
                    applyInvincibleHit(objectProxy, victim, hitbox.def);
                    if (object.active) {
                        setGameObjectHitlag(world, objectIndex, hitlagFramesForHitbox(hitbox.def));
                    }
                } else {
                    FighterRuntime objectProxy;
                    objectProxy.position = object.position;
                    objectProxy.previousPosition = object.previousPosition;
                    objectProxy.facing = object.facing;
                    const size_t ownerIndex = object.ownerFighter >= 0 ? static_cast<size_t>(object.ownerFighter) : world.fighters.size();
                    applyHit(world, ownerIndex, objectProxy, victim, victimIndex, hitbox.def, hurtboxIndex);
                    runGameObjectEvent(world, objectIndex, GameObjectEvent::DamageDealt);
                }
                if (objectDef.destroyOnHit) {
                    deactivateGameObject(world, objectIndex);
                }
                if (object.active) {
                    setGameObjectHitlag(world, objectIndex, hitlagFramesForHitbox(hitbox.def));
                }
                return;
            }
        }
    }
}

static void applyGameObjectGrab(World& world, size_t objectIndex, size_t victimIndex) {
    if (objectIndex >= world.objects.size() || victimIndex >= world.fighters.size()) {
        return;
    }
    GameObjectRuntime& object = world.objects[objectIndex];
    if (!object.active) {
        return;
    }
    object.grabVictimFighter = static_cast<int>(victimIndex);
    runGameObjectEvent(world, objectIndex, GameObjectEvent::GrabDealt);
    if (object.active) {
        runGameObjectEvent(world, objectIndex, GameObjectEvent::GrabbedForVictim);
    }
}

static bool objectTouchesObject(const GameObjectRuntime& object, const GameObjectDefinition& objectDef, const GameObjectRuntime& other, const GameObjectDefinition& otherDef) {
    for (const GameObjectTouchboxDefinition& touchbox : objectDef.touchboxes) {
        if (!touchbox.touchObjects) {
            continue;
        }
        const Capsule touchCapsule = objectTouchboxWorld(object, touchbox);
        for (const GameObjectHurtboxDefinition& hurtbox : otherDef.hurtboxes) {
            if (hurtbox.state == HurtboxState::Intangible) {
                continue;
            }
            if (capsuleCapsule(touchCapsule, objectHurtboxWorld(other, hurtbox))) {
                return true;
            }
        }
        for (const GameObjectTouchboxDefinition& otherTouchbox : otherDef.touchboxes) {
            if (!otherTouchbox.touchObjects) {
                continue;
            }
            if (capsuleCapsule(touchCapsule, objectTouchboxWorld(other, otherTouchbox))) {
                return true;
            }
        }
    }
    return false;
}

static Fix gameObjectTopY(const GameObjectRuntime& object, const GameObjectDefinition& def) {
    Fix top = object.position.y;
    bool found = false;
    for (const GameObjectTouchboxDefinition& touchbox : def.touchboxes) {
        top = found ? std::max(top, object.position.y + std::max(touchbox.startOffset.y, touchbox.endOffset.y) + touchbox.radius)
                    : object.position.y + std::max(touchbox.startOffset.y, touchbox.endOffset.y) + touchbox.radius;
        found = true;
    }
    for (const GameObjectHurtboxDefinition& hurtbox : def.hurtboxes) {
        top = found ? std::max(top, object.position.y + std::max(hurtbox.startOffset.y, hurtbox.endOffset.y) + hurtbox.radius)
                    : object.position.y + std::max(hurtbox.startOffset.y, hurtbox.endOffset.y) + hurtbox.radius;
        found = true;
    }
    return top;
}

static bool fighterCanJumpOnObject(const FighterRuntime& fighter, Fix objectTopY) {
    if (fighter.grounded || fighter.position.y >= fighter.previousPosition.y) {
        return false;
    }
    return fighter.previousPosition.y >= objectTopY && fighter.position.y <= objectTopY;
}

static void updateAndCheckObjectJumpedOn(World& world, size_t objectIndex) {
    if (objectIndex >= world.objects.size()) {
        return;
    }
    GameObjectRuntime& object = world.objects[objectIndex];
    if (!object.active || gameObjectIsHeld(object) ||
        object.objectDef < 0 || object.objectDef >= static_cast<int>(world.objectDefs.size())) {
        return;
    }
    const GameObjectDefinition& objectDef = world.objectDefs[static_cast<size_t>(object.objectDef)];
    if (objectDef.touchboxes.empty() && objectDef.hurtboxes.empty()) {
        return;
    }
    const Fix topY = gameObjectTopY(object, objectDef);
    for (const GameObjectTouchboxDefinition& touchbox : objectDef.touchboxes) {
        if (!touchbox.touchFighters) {
            continue;
        }
        const Capsule touchCapsule = objectTouchboxWorld(object, touchbox);
        for (size_t fighterIndex = 0; fighterIndex < world.fighters.size(); ++fighterIndex) {
            if (!objectDef.hitOwner && object.ownerFighter == static_cast<int>(fighterIndex)) {
                continue;
            }
            FighterRuntime& fighter = world.fighters[fighterIndex];
            if (fighterCanJumpOnObject(fighter, topY) && fighterTouchesCapsule(world, fighterIndex, touchCapsule)) {
                object.ownerFighter = static_cast<int>(fighterIndex);
                runGameObjectEvent(world, objectIndex, GameObjectEvent::JumpedOn);
                if (object.active) {
                    setGameObjectHitlag(world, objectIndex, 1);
                }
                return;
            }
        }
    }
    if (!objectDef.touchboxes.empty()) {
        return;
    }
    for (const GameObjectHurtboxDefinition& hurtbox : objectDef.hurtboxes) {
        if (hurtbox.state == HurtboxState::Intangible) {
            continue;
        }
        const Capsule hurtCapsule = objectHurtboxWorld(object, hurtbox);
        for (size_t fighterIndex = 0; fighterIndex < world.fighters.size(); ++fighterIndex) {
            if (!objectDef.hitOwner && object.ownerFighter == static_cast<int>(fighterIndex)) {
                continue;
            }
            FighterRuntime& fighter = world.fighters[fighterIndex];
            if (fighterCanJumpOnObject(fighter, topY) && fighterTouchesCapsule(world, fighterIndex, hurtCapsule)) {
                object.ownerFighter = static_cast<int>(fighterIndex);
                runGameObjectEvent(world, objectIndex, GameObjectEvent::JumpedOn);
                if (object.active) {
                    setGameObjectHitlag(world, objectIndex, 1);
                }
                return;
            }
        }
    }
}

static void updateAndCheckObjectTouches(World& world, size_t objectIndex) {
    if (objectIndex >= world.objects.size()) {
        return;
    }
    GameObjectRuntime& object = world.objects[objectIndex];
    if (!object.active || gameObjectIsHeld(object) ||
        object.objectDef < 0 || object.objectDef >= static_cast<int>(world.objectDefs.size())) {
        return;
    }
    const GameObjectDefinition& objectDef = world.objectDefs[static_cast<size_t>(object.objectDef)];
    if (objectDef.touchboxes.empty()) {
        return;
    }

    for (const GameObjectTouchboxDefinition& touchbox : objectDef.touchboxes) {
        if (!touchbox.touchFighters) {
            continue;
        }
        const Capsule touchCapsule = objectTouchboxWorld(object, touchbox);
        for (size_t fighterIndex = 0; fighterIndex < world.fighters.size(); ++fighterIndex) {
            if (!objectDef.hitOwner && object.ownerFighter == static_cast<int>(fighterIndex)) {
                continue;
            }
            if (fighterTouchesCapsule(world, fighterIndex, touchCapsule)) {
                runGameObjectEvent(world, objectIndex, GameObjectEvent::Touched);
                if (object.active) {
                    setGameObjectHitlag(world, objectIndex, 1);
                }
                return;
            }
        }
    }

    for (size_t otherIndex = 0; otherIndex < world.objects.size(); ++otherIndex) {
        if (otherIndex == objectIndex) {
            continue;
        }
        GameObjectRuntime& other = world.objects[otherIndex];
        if (!other.active || gameObjectIsHeld(other) ||
            other.objectDef < 0 || other.objectDef >= static_cast<int>(world.objectDefs.size())) {
            continue;
        }
        const GameObjectDefinition& otherDef = world.objectDefs[static_cast<size_t>(other.objectDef)];
        if (objectTouchesObject(object, objectDef, other, otherDef)) {
            runGameObjectEvent(world, objectIndex, GameObjectEvent::Touched);
            if (object.active) {
                setGameObjectHitlag(world, objectIndex, 1);
            }
            return;
        }
    }
}

static void compactGameObjects(World& world) {
    std::vector<int> remap(world.objects.size(), -1);
    std::vector<GameObjectRuntime> kept;
    kept.reserve(world.objects.size());
    for (size_t i = 0; i < world.objects.size(); ++i) {
        if (!world.objects[i].active) {
            continue;
        }
        remap[i] = static_cast<int>(kept.size());
        kept.push_back(std::move(world.objects[i]));
    }
    for (FighterRuntime& fighter : world.fighters) {
        if (fighter.heldObject < 0 || fighter.heldObject >= static_cast<int>(remap.size())) {
            fighter.heldObject = -1;
        } else {
            fighter.heldObject = remap[static_cast<size_t>(fighter.heldObject)];
        }
    }
    world.objects = std::move(kept);
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
        executePendingActionFrames(world, fighterIndex, def, currentState(world, fighter), fighter);
        if (fighterIndex >= world.fighters.size()) {
            continue;
        }
        FighterRuntime& fighterAfterActions = world.fighters[fighterIndex];
        processSmashCharge(fighterAfterActions);
        processInterrupts(world, fighterAfterActions);
        runStateFunctions(world, fighterIndex, currentState(world, fighterAfterActions).onFrame);
        if (fighterIndex >= world.fighters.size()) {
            continue;
        }
        FighterRuntime& fighterAfterFunctions = world.fighters[fighterIndex];
        const FighterDefinition& defAfterFunctions = world.fighterDefs[static_cast<size_t>(fighterAfterFunctions.fighterDef)];
        processThrowRelease(world, fighterIndex);
        evaluatePose(defAfterFunctions, currentState(world, fighterAfterFunctions), fighterAfterFunctions);
        if (fighterAfterFunctions.grabberFighter >= 0 && isHeldCaptureStateName(currentState(world, fighterAfterFunctions).name)) {
            maintainCapturedFighterAfterPose(world, fighterIndex);
        } else {
            applyAnimationGroundVelocity(currentState(world, fighterAfterFunctions), fighterAfterFunctions);
            integrateAndCollide(world, fighterIndex);
            refreshHsdWorldPose(world.fighterDefs[static_cast<size_t>(fighterAfterFunctions.fighterDef)], fighterAfterFunctions);
            applyImportedBoneAliases(world.fighterDefs[static_cast<size_t>(fighterAfterFunctions.fighterDef)], fighterAfterFunctions);
        }

        const FighterState& stateAfterPhysics = currentState(world, fighterAfterFunctions);
        const int animationLengthFrames = fighterAfterFunctions.stateAnimationLengthOverride > 0
            ? fighterAfterFunctions.stateAnimationLengthOverride
            : stateAfterPhysics.animationLengthFrames;
        if (frameInState(fighterAfterFunctions) > animationLengthFrames && !stateAfterPhysics.onAnimationFinishedState.empty()) {
            changeFighterState(world, fighterAfterFunctions, stateAfterPhysics.onAnimationFinishedState, 0, stateAfterPhysics.onAnimationFinishedBlendFrames);
        }
        advanceAnimationFrame(defAfterFunctions, currentState(world, fighterAfterFunctions), fighterAfterFunctions);
        regenerateShield(world, fighterAfterFunctions);
    }

    tickGameObjects(world);

    for (size_t i = 0; i < world.fighters.size(); ++i) {
        updateActiveHitboxPositions(world, i);
    }
    updateActiveObjectHitboxPositions(world);
    resolveHitboxClanks(world);
    resolveFighterObjectHitboxClanks(world);
    resolveObjectHitboxClanks(world);
    for (size_t i = 0; i < world.fighters.size(); ++i) {
        updateAndCheckHitboxes(world, i);
    }
    for (size_t i = 0; i < world.fighters.size(); ++i) {
        updateAndCheckHitboxesAgainstObjects(world, i);
    }
    for (size_t i = 0; i < world.objects.size(); ++i) {
        updateAndCheckObjectHitboxes(world, i);
    }
    for (size_t i = 0; i < world.objects.size(); ++i) {
        updateAndCheckObjectJumpedOn(world, i);
    }
    for (size_t i = 0; i < world.objects.size(); ++i) {
        updateAndCheckObjectTouches(world, i);
    }
    compactGameObjects(world);

    ++world.frame;
}

WorldSnapshot saveWorld(const World& world) {
    WorldSnapshot snapshot;
    snapshot.frame = world.frame;
    snapshot.rngState = world.rngState;
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
        item.animationActionIndexOverride = fighter.animationActionIndexOverride;
        item.lastActionFrameExecuted = fighter.lastActionFrameExecuted;
        item.runAnimationVelocity = fighter.runAnimationVelocity;
        item.facing = fighter.facing;
        item.poseFacing = fighter.poseFacing;
        item.jumpsUsed = fighter.jumpsUsed;
        item.grounded = fighter.grounded;
        item.percent = fighter.percent;
        item.shieldHealth = fighter.shieldHealth;
        item.position = fighter.position;
        item.previousPosition = fighter.previousPosition;
        item.fighterVelocity = fighter.fighterVelocity;
        item.knockbackVelocity = fighter.knockbackVelocity;
        item.knockbackDecay = fighter.knockbackDecay;
        item.attackerShieldKnockback = fighter.attackerShieldKnockback;
        item.groundVelocity = fighter.groundVelocity;
        item.groundKnockbackVelocity = fighter.groundKnockbackVelocity;
        item.groundAttackerShieldKnockbackVelocity = fighter.groundAttackerShieldKnockbackVelocity;
        item.lastLandingVelocityY = fighter.lastLandingVelocityY;
        item.groundAccel = fighter.groundAccel;
        item.groundAccelSecondary = fighter.groundAccelSecondary;
        item.groundNormal = fighter.groundNormal;
        item.hitlag = fighter.hitlag;
        item.hitstun = fighter.hitstun;
        item.damageLevel = fighter.damageLevel;
        item.damageHurtboxRegion = fighter.damageHurtboxRegion;
        item.damageKnockback = fighter.damageKnockback;
        item.damageLaunchAngle = fighter.damageLaunchAngle;
        item.damageTumble = fighter.damageTumble;
        item.reboundDamageVelocity = fighter.reboundDamageVelocity;
        item.reboundAccel = fighter.reboundAccel;
        item.reboundAnimationRate = fighter.reboundAnimationRate;
        item.reboundFacingDir = fighter.reboundFacingDir;
        item.damageSurfaceTimer = fighter.damageSurfaceTimer;
        item.downWaitTimer = fighter.downWaitTimer;
        item.damageHitboxOwner = fighter.damageHitboxOwner;
        item.thrownHitboxOwner = fighter.thrownHitboxOwner;
        item.heldObject = fighter.heldObject;
        item.grabbedFighter = fighter.grabbedFighter;
        item.grabberFighter = fighter.grabberFighter;
        item.grabTimer = fighter.grabTimer;
        item.captureWaitTimer = fighter.captureWaitTimer;
        item.captureMashAnimTimer = fighter.captureMashAnimTimer;
        item.burySubmergeTimer = fighter.burySubmergeTimer;
        item.grabMashStickX = fighter.grabMashStickX;
        item.grabMashStickY = fighter.grabMashStickY;
        item.captureJumpQueued = fighter.captureJumpQueued;
        item.captureConstraintOffset = fighter.captureConstraintOffset;
        item.captureOriginalXRotNTranslation = fighter.captureOriginalXRotNTranslation;
        item.captureConstraintActive = fighter.captureConstraintActive;
        item.throwAnimationFrozen = fighter.throwAnimationFrozen;
        item.thrownAnimationFreezeActive = fighter.thrownAnimationFreezeActive;
        item.thrownAnimationFreezeFrame = fighter.thrownAnimationFreezeFrame;
        item.throwHitboxes = fighter.throwHitboxes;
        item.throwHitboxActive = fighter.throwHitboxActive;
        item.stateFlags = fighter.stateFlags;
        item.commandVars = fighter.commandVars;
        item.packageVars = fighter.packageVars;
        item.commandFlags = fighter.commandFlags;
        item.throwFlags = fighter.throwFlags;
        item.jabFollowupEnabled = fighter.jabFollowupEnabled;
        item.rapidJabEnabled = fighter.rapidJabEnabled;
        item.fighterInvisible = fighter.fighterInvisible;
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
        item.ledgeWaitTimer = fighter.ledgeWaitTimer;
        item.runoffSegment = fighter.runoffSegment;
        item.runoffDirection = fighter.runoffDirection;
        item.pendingFallSpecialLandingLag = fighter.pendingFallSpecialLandingLag;
        item.pendingFallSpecialLandingInterruptible = fighter.pendingFallSpecialLandingInterruptible;
        item.pendingFallSpecialForceLanding = fighter.pendingFallSpecialForceLanding;
        item.pendingFallSpecialLimitDrift = fighter.pendingFallSpecialLimitDrift;
        item.pendingFallSpecialUseFastFallTerminal = fighter.pendingFallSpecialUseFastFallTerminal;
        item.pendingFallSpecialDriftMax = fighter.pendingFallSpecialDriftMax;
        item.fallSpecialLandingLag = fighter.fallSpecialLandingLag;
        item.fallSpecialLandingInterruptible = fighter.fallSpecialLandingInterruptible;
        item.fallSpecialForceLanding = fighter.fallSpecialForceLanding;
        item.fallSpecialLimitDrift = fighter.fallSpecialLimitDrift;
        item.fallSpecialUseFastFallTerminal = fighter.fallSpecialUseFastFallTerminal;
        item.fallSpecialDriftMax = fighter.fallSpecialDriftMax;
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
        item.turnBufferedButtons = fighter.turnBufferedButtons;
        item.runDirectTimer = fighter.runDirectTimer;
        item.runBrakeTimer = fighter.runBrakeTimer;
        item.runBrakeAnimationFrozen = fighter.runBrakeAnimationFrozen;
        item.attackDashGrabBufferTimer = fighter.attackDashGrabBufferTimer;
        item.attackLw3RepeatQueued = fighter.attackLw3RepeatQueued;
        item.attackRapidInputCount = fighter.attackRapidInputCount;
        item.attack100CanEnd = fighter.attack100CanEnd;
        item.attack100ContinuePressed = fighter.attack100ContinuePressed;
        item.guardMinHoldTimer = fighter.guardMinHoldTimer;
        item.guardSetoffTimer = fighter.guardSetoffTimer;
        item.guardCatchDashBufferTimer = fighter.guardCatchDashBufferTimer;
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
        item.animationPose = fighter.animationPose;
        item.animationBlendFromPose = fighter.animationBlendFromPose;
        item.animationBlendFrames = fighter.animationBlendFrames;
        item.animationBlendElapsed = fighter.animationBlendElapsed;
        item.animationTransN = fighter.animationTransN;
        item.previousAnimationTransN = fighter.previousAnimationTransN;
        item.animationTransNOffset = fighter.animationTransNOffset;
        item.jointWorldTransforms = fighter.jointWorldTransforms;
        item.jointWorldPositions = fighter.jointWorldPositions;
        item.poseHurtboxCapsules = fighter.poseHurtboxCapsules;
        item.modelVisibilityDefaultStates = fighter.modelVisibilityDefaultStates;
        item.modelVisibilityStates = fighter.modelVisibilityStates;
        item.modelPartAnimations = fighter.modelPartAnimations;
        item.hurtboxStates = fighter.hurtboxStates;
        item.bodyCollisionState = fighter.bodyCollisionState;
        item.activeHitboxes = fighter.activeHitboxes;
        item.fightersHitThisAction = fighter.fightersHitThisAction;
        snapshot.fighters.push_back(item);
    }
    snapshot.objects = world.objects;
    return snapshot;
}

void loadWorld(World& world, const WorldSnapshot& snapshot) {
    world.frame = snapshot.frame;
    world.rngState = snapshot.rngState;
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
        fighter.animationActionIndexOverride = item.animationActionIndexOverride;
        fighter.lastActionFrameExecuted = item.lastActionFrameExecuted;
        fighter.runAnimationVelocity = item.runAnimationVelocity;
        fighter.facing = item.facing;
        fighter.poseFacing = item.poseFacing == 0 ? item.facing : item.poseFacing;
        fighter.jumpsUsed = item.jumpsUsed;
        fighter.grounded = item.grounded;
        fighter.percent = item.percent;
        fighter.shieldHealth = item.shieldHealth;
        fighter.position = item.position;
        fighter.previousPosition = item.previousPosition;
        fighter.fighterVelocity = item.fighterVelocity;
        fighter.knockbackVelocity = item.knockbackVelocity;
        fighter.knockbackDecay = item.knockbackDecay;
        fighter.attackerShieldKnockback = item.attackerShieldKnockback;
        fighter.groundVelocity = item.groundVelocity;
        fighter.groundKnockbackVelocity = item.groundKnockbackVelocity;
        fighter.groundAttackerShieldKnockbackVelocity = item.groundAttackerShieldKnockbackVelocity;
        fighter.lastLandingVelocityY = item.lastLandingVelocityY;
        fighter.groundAccel = item.groundAccel;
        fighter.groundAccelSecondary = item.groundAccelSecondary;
        fighter.groundNormal = item.groundNormal;
        fighter.hitlag = item.hitlag;
        fighter.hitstun = item.hitstun;
        fighter.damageLevel = item.damageLevel;
        fighter.damageHurtboxRegion = item.damageHurtboxRegion;
        fighter.damageKnockback = item.damageKnockback;
        fighter.damageLaunchAngle = item.damageLaunchAngle;
        fighter.damageTumble = item.damageTumble;
        fighter.reboundDamageVelocity = item.reboundDamageVelocity;
        fighter.reboundAccel = item.reboundAccel;
        fighter.reboundAnimationRate = item.reboundAnimationRate;
        fighter.reboundFacingDir = item.reboundFacingDir;
        fighter.damageSurfaceTimer = item.damageSurfaceTimer;
        fighter.downWaitTimer = item.downWaitTimer;
        fighter.damageHitboxOwner = item.damageHitboxOwner;
        fighter.thrownHitboxOwner = item.thrownHitboxOwner;
        fighter.heldObject = item.heldObject;
        fighter.grabbedFighter = item.grabbedFighter;
        fighter.grabberFighter = item.grabberFighter;
        fighter.grabTimer = item.grabTimer;
        fighter.captureWaitTimer = item.captureWaitTimer;
        fighter.captureMashAnimTimer = item.captureMashAnimTimer;
        fighter.burySubmergeTimer = item.burySubmergeTimer;
        fighter.grabMashStickX = item.grabMashStickX;
        fighter.grabMashStickY = item.grabMashStickY;
        fighter.captureJumpQueued = item.captureJumpQueued;
        fighter.captureConstraintOffset = item.captureConstraintOffset;
        fighter.captureOriginalXRotNTranslation = item.captureOriginalXRotNTranslation;
        fighter.captureConstraintActive = item.captureConstraintActive;
        fighter.throwAnimationFrozen = item.throwAnimationFrozen;
        fighter.thrownAnimationFreezeActive = item.thrownAnimationFreezeActive;
        fighter.thrownAnimationFreezeFrame = item.thrownAnimationFreezeFrame;
        fighter.throwHitboxes = item.throwHitboxes;
        fighter.throwHitboxActive = item.throwHitboxActive;
        fighter.stateFlags = item.stateFlags;
        fighter.commandVars = item.commandVars;
        fighter.packageVars = item.packageVars;
        fighter.commandFlags = item.commandFlags;
        fighter.throwFlags = item.throwFlags;
        fighter.jabFollowupEnabled = item.jabFollowupEnabled;
        fighter.rapidJabEnabled = item.rapidJabEnabled;
        fighter.fighterInvisible = item.fighterInvisible;
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
        fighter.ledgeWaitTimer = item.ledgeWaitTimer;
        fighter.runoffSegment = item.runoffSegment;
        fighter.runoffDirection = item.runoffDirection;
        fighter.pendingFallSpecialLandingLag = item.pendingFallSpecialLandingLag;
        fighter.pendingFallSpecialLandingInterruptible = item.pendingFallSpecialLandingInterruptible;
        fighter.pendingFallSpecialForceLanding = item.pendingFallSpecialForceLanding;
        fighter.pendingFallSpecialLimitDrift = item.pendingFallSpecialLimitDrift;
        fighter.pendingFallSpecialUseFastFallTerminal = item.pendingFallSpecialUseFastFallTerminal;
        fighter.pendingFallSpecialDriftMax = item.pendingFallSpecialDriftMax;
        fighter.fallSpecialLandingLag = item.fallSpecialLandingLag;
        fighter.fallSpecialLandingInterruptible = item.fallSpecialLandingInterruptible;
        fighter.fallSpecialForceLanding = item.fallSpecialForceLanding;
        fighter.fallSpecialLimitDrift = item.fallSpecialLimitDrift;
        fighter.fallSpecialUseFastFallTerminal = item.fallSpecialUseFastFallTerminal;
        fighter.fallSpecialDriftMax = item.fallSpecialDriftMax;
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
        fighter.turnBufferedButtons = item.turnBufferedButtons;
        fighter.runDirectTimer = item.runDirectTimer;
        fighter.runBrakeTimer = item.runBrakeTimer;
        fighter.runBrakeAnimationFrozen = item.runBrakeAnimationFrozen;
        fighter.attackDashGrabBufferTimer = item.attackDashGrabBufferTimer;
        fighter.attackLw3RepeatQueued = item.attackLw3RepeatQueued;
        fighter.attackRapidInputCount = item.attackRapidInputCount;
        fighter.attack100CanEnd = item.attack100CanEnd;
        fighter.attack100ContinuePressed = item.attack100ContinuePressed;
        fighter.guardMinHoldTimer = item.guardMinHoldTimer;
        fighter.guardSetoffTimer = item.guardSetoffTimer;
        fighter.guardCatchDashBufferTimer = item.guardCatchDashBufferTimer;
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
        fighter.animationPose = item.animationPose;
        fighter.animationBlendFromPose = item.animationBlendFromPose;
        fighter.animationBlendFrames = item.animationBlendFrames;
        fighter.animationBlendElapsed = item.animationBlendElapsed;
        fighter.animationTransN = item.animationTransN;
        fighter.previousAnimationTransN = item.previousAnimationTransN;
        fighter.animationTransNOffset = item.animationTransNOffset;
        fighter.jointWorldTransforms = item.jointWorldTransforms;
        fighter.jointWorldPositions = item.jointWorldPositions;
        fighter.poseHurtboxCapsules = item.poseHurtboxCapsules;
        fighter.modelVisibilityDefaultStates = item.modelVisibilityDefaultStates;
        fighter.modelVisibilityStates = item.modelVisibilityStates;
        fighter.modelPartAnimations = item.modelPartAnimations;
        fighter.hurtboxStates = item.hurtboxStates;
        fighter.bodyCollisionState = item.bodyCollisionState;
        fighter.activeHitboxes = item.activeHitboxes;
        fighter.fightersHitThisAction = item.fightersHitThisAction;
        world.fighters.push_back(fighter);
    }
    world.objects = snapshot.objects;
}

} // namespace pf
