#pragma once

#include "core/input.hpp"
#include "core/math.hpp"
#include "core/animation_asset.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace pf {

enum class BoneId : uint8_t {
    Hip,
    Head,
    HandL,
    HandR,
    FootL,
    FootR,
    Extra,
    Count,
};

constexpr int kBoneCount = static_cast<int>(BoneId::Count);

struct BonePose {
    Vec3 position;
};

struct MeleeCommonData {
    // Mirrors the movement-relevant fields from Melee's ftCommonData
    // (PlCo.dat). Defaults are seeded from vanilla PlCo.dat.
    Fix stickXTiltThresholdX8 = fxFromFloat(0.25f);
    Fix stickYTiltThresholdXC = fxFromFloat(0.25f);
    int inputRepeatWindowX1C = 40;
    Fix aerialAttackAngleTanX20 = fx(1); // Melee x20_radians; name kept for binary/source compatibility.
    Fix walkInputThresholdX24 = fxFromFloat(0.18f);
    Fix walkMiddleThresholdX28 = fxFromFloat(0.4f);
    Fix walkFastThresholdX2C = fxFromFloat(0.8f);
    Fix walkAccelScaleX30 = fxFromFloat(0.5f);
    Fix turnInputThresholdX34 = fxFromFloat(-0.25f);
    Fix turnRunInputThresholdX38 = fxFromFloat(-0.375f);
    Fix dashInputThresholdX3C = fxFromFloat(0.8f);
    int dashStickWindowX40 = 2;
    int dashEarlyInterruptWindowX44 = 4;
    int dashItemThrowWindowX48 = 3;
    int dashLateInterruptWindowX4C = 20;
    Fix attackDashFrictionScaleX50 = fxFromFloat(1.0f);
    Fix dashDecayX54 = fxFromFloat(0.75f);
    Fix runInputThresholdX58 = fxFromFloat(0.625f);
    Fix runAccelScaleX5C = fxFromFloat(0.4f);
    Fix groundFrictionScaleX60 = fxFromFloat(1.0f);
    Fix catchCutFrictionScaleX64 = fxFromFloat(1.0f);
    int attackDashGrabBufferFramesX68 = 0;
    Fix turnFrictionScaleAboveWalkMaxX6C = fxFromFloat(2.0f);
    Fix tapJumpThresholdX70 = fxFromFloat(0.6625f);
    int tapJumpWindowX74 = 4;
    Fix jumpBackwardThresholdX78 = fxFromFloat(0.125f);
    Fix tapJumpReleaseThresholdX7C = fxFromFloat(0.3f);
    Fix aerialJumpStickThresholdX80 = fxFromFloat(0.5625f);
    Fix fastfallStickThresholdX88 = fxFromFloat(0.6625f);
    int fastfallStickWindowX8C = 4;
    Fix squatStickThresholdX90 = fxFromFloat(0.6875f);
    Fix squatReleaseThresholdX94 = fxFromFloat(0.625f);
    Fix attackS3StickThresholdX98 = fxFromFloat(0.35f);
    Fix attackS3HiAngleX9C = fxFromFloat(0.5f);
    Fix attackS3HiSAngleXA0 = fxFromFloat(0.25f);
    Fix attackS3LwSAngleXA4 = -fxFromFloat(0.25f);
    Fix attackS3LwAngleXA8 = -fxFromFloat(0.5f);
    Fix attackHi3StickThresholdYxAC = fxFromFloat(0.5f);
    Fix attackLw3StickThresholdYxB0 = -fxFromFloat(0.5f);
    Fix attackS4HiAngleXB8 = fxFromFloat(0.5f);
    Fix attackS4HiSAngleXBC = fxFromFloat(0.25f);
    Fix attackS4LwSAngleXC0 = -fxFromFloat(0.25f);
    Fix attackS4LwAngleXC4 = -fxFromFloat(0.5f);
    Fix attackHi4StickThresholdYxCC = fxFromFloat(0.7f);
    int attackHi4StickWindowXD0 = 4;
    Fix attackLw4StickThresholdYxD4 = -fxFromFloat(0.7f);
    int attackLw4StickWindowXD8 = 4;
    int lCancelInputWindowXE4 = 7;
    Fix lCancelLandingLagDivisorXE8 = fx(2);
    Fix knockbackWeightScaleXF4 = fxFromFloat(0.01f);
    Fix knockbackWeightDecayXF8 = fx(2);
    Fix damageVelocityScaleX100 = fxFromFloat(0.03f);
    Fix knockbackMaxX108 = fx(2500);
    Fix throwKnockbackWeightX10C = fx(100);
    Fix knockbackDamageBaseX110 = fxFromFloat(0.1f);
    Fix knockbackDamageScaleX114 = fxFromFloat(0.05f);
    Fix knockbackWeightSetScaleX118 = fxFromFloat(0.1f);
    Fix knockbackScaleX11C = fxFromFloat(1.4f);
    Fix knockbackBaseX120 = fx(18);
    Fix damageSakuraiAngleAirX144 = fxFromFloat(0.7679449f);
    Fix damageSakuraiAngleScaleX148 = fx(44);
    Fix damageSakuraiAngleLowX14C = fxFromFloat(32.0f);
    Fix damageSakuraiAngleHighX150 = fxFromFloat(32.1f);
    Fix hitstunMultiplierX154 = fxFromFloat(0.4f);
    Fix damageLevelThresholdX158 = fxFromFloat(8.0f);
    Fix damageLevelThresholdX15C = fxFromFloat(14.0f);
    Fix damageLevelThresholdX160 = fxFromFloat(20.0f);
    Fix damageGroundKnockbackClampX164 = fx(3);
    Fix damageAirVelocityScaleX190 = fx(1);
    Fix damageWallBounceMinVelocityX1B0 = fxFromFloat(2.4f);
    Fix damageWallBounceDampingX1BC = fxFromFloat(0.8f);
    int damageSurfaceLockoutX1C0 = 6;
    Fix thrownHitboxClearVelocityX1C8 = fxFromFloat(0.5f);
    Fix damageWallBounceMinVelocityX1E0 = fxFromFloat(2.4f);
    Fix damageLandingMinVelocityX1E4 = fxFromFloat(1.0f);
    Fix damageGroundBounceAngleX1E8 = fxFromFloat(0.7853982f);
    Fix damageGroundBounceDampingX1EC = fxFromFloat(0.8f);
    Fix groundKnockbackFrictionScaleX200 = fx(1);
    Fix knockbackFrameDecayX204 = fxFromFloat(0.051f);
    Fix damageFallStickThresholdX210 = fxFromFloat(0.7f);
    int damageFallStickWindowX214 = 3;
    Fix specialSStickThresholdX218 = fxFromFloat(0.2875f);
    Fix specialLwHiStickThresholdX21C = fxFromFloat(0.2875f);
    Fix specialSReverseThresholdX220 = fxFromFloat(0.2875f);
    Fix specialNReverseFramesX224 = fx(6);
    Fix damageFlyTopAngleMinX234 = fxFromFloat(1.2217305f);
    Fix damageFlyTopAngleMaxX238 = fxFromFloat(1.9198622f);
    int damageFlyRollPercentX23C = 100;
    Fix damageFlyRollChanceX240 = fxFromFloat(0.5f);
    Fix downStandStickThresholdX244 = fxFromFloat(0.7f);
    Fix downRollStickThresholdX248 = fxFromFloat(0.7f);
    int downAttackInputWindowX24C = 8;
    int passiveInputWindowX250 = 20;
    Fix passiveStandStickThresholdX254 = fxFromFloat(0.5f);
    Fix downAttackCStickThresholdX7F4 = fxFromFloat(0.8f);
    Fix startShieldHealthX260 = fx(60);
    Fix minShieldScaleX264 = fxFromFloat(0.15f);
    int guardMinHoldFramesX268 = 8;
    Fix shieldDrainRateX278 = fxFromFloat(0.14f);
    Fix shieldRegenRateX27C = fxFromFloat(0.07f);
    Fix shieldDamageScaleX284 = fx(1);
    Fix shieldDamageBaseX288 = 0;
    Fix shieldSetoffScaleX28C = fxFromFloat(1.5f);
    Fix shieldSetoffBaseX290 = fx(2);
    Fix shieldPushbackScaleX294 = fxFromFloat(0.2f);
    Fix shieldPushbackMaxX298 = fx(2);
    int shieldReflectInputWindowX2A0 = 2;
    int guardHitReleaseLockoutX2B8 = 4;
    Fix hardShieldSizeScaleX2D4 = fx(1);
    Fix lightShieldSizeScaleX2D8 = fxFromFloat(0.5f);
    Fix hardShieldDamageScaleX2DC = fxFromFloat(0.1f);
    Fix lightShieldDamageScaleX2E0 = fxFromFloat(0.3f);
    Fix hardShieldSetoffScaleX2E4 = fxFromFloat(0.05f);
    Fix lightShieldSetoffScaleX2E8 = fxFromFloat(0.7f);
    Fix hardShieldDrainScaleX2EC = fxFromFloat(0.1f);
    Fix lightShieldDrainScaleX2F0 = fx(2);
    Fix shieldAlphaMinX2F4 = fx(64);
    Fix furafuraTimerBaseX2F8 = fxFromFloat(300.0f);
    Fix furafuraTimerMinX2FC = fxFromFloat(90.0f);
    Fix furafuraTimerDecrementX300 = fx(1);
    Fix furafuraMashDecrementX304 = fxFromFloat(6.0f);
    Fix furafuraShieldHealthX280 = fxFromFloat(30.0f);
    Fix attackerShieldPushbackScaleX3E0 = fxFromFloat(0.07f);
    Fix attackerShieldPushbackBaseX3E4 = fxFromFloat(0.02f);
    Fix shieldKnockbackFrameDecayX3E8 = fxFromFloat(0.05f);
    Fix shieldGroundFrictionMultiplierX3EC = fxFromFloat(1.1f);
    Fix grabMashStickThresholdX308 = fxFromFloat(0.8f);
    Fix grabTimerBaseX354 = fxFromFloat(76.0f);
    Fix grabTimerHandicapScaleX358 = fxFromFloat(1.0f);
    Fix grabTimerHandicapBaseX35C = fxFromFloat(9.0f);
    Fix grabTimerPortScaleX360 = fxFromFloat(0.0f);
    Fix grabTimerPortBaseX364 = fxFromFloat(0.0f);
    Fix grabTimerPercentScaleX368 = fxFromFloat(1.7f);
    Fix captureCutFrictionScaleX36C = fx(1);
    Fix captureCutGroundVelocityX370 = fxFromFloat(1.0f);
    Fix captureJumpVelocityX374 = fxFromFloat(1.0f);
    Fix captureJumpVelocityYx378 = fxFromFloat(1.5f);
    Fix throwWeightAnimationScaleX37C = fxFromFloat(0.01f);
    Fix captureTimerDecrementX3A4 = fx(1);
    Fix captureMashDecrementX3A8 = fxFromFloat(6.0f);
    Fix captureJumpButtonWindowX3AC = fxFromFloat(15.0f);
    Fix captureMashAnimHoldFramesX3B0 = fxFromFloat(8.0f);
    Fix captureMashAnimRateX3B4 = fxFromFloat(0.5f);
    Fix captureJumpGravityThresholdX3B8 = fxFromFloat(0.0f);
    Fix captureFloorSnapMaxX3BC = fxFromFloat(2.0f);
    Fix captureHighThresholdX3C4 = fxFromFloat(0.0f);
    Fix thrownMashDecrementX3C8 = fxFromFloat(6.0f);
    Fix reboundDamageScaleX3D0 = fx(1);
    Fix reboundDamageBaseX3D4 = 0;
    Fix reboundAccelScaleX3D8 = fxFromFloat(0.03f);
    Fix reboundAccelBaseX3DC = fxFromFloat(0.02f);
    Fix damageSongBaseX624 = fxFromFloat(120.0f);
    Fix damageSongHandicapScaleX628 = fxFromFloat(1.0f);
    Fix damageSongHandicapBaseX62C = fxFromFloat(9.0f);
    Fix damageSongPortScaleX630 = fxFromFloat(0.0f);
    Fix damageSongPortBaseX634 = fxFromFloat(0.0f);
    Fix damageSongPercentScaleX638 = fxFromFloat(1.0f);
    Fix damageSongTimerDecrementX63C = fx(1);
    Fix damageSongMashDecrementX640 = fxFromFloat(6.0f);
    Fix damageSongElement7TimerMultiplierX644 = fx(1);
    Fix damageBindBaseX658 = fxFromFloat(120.0f);
    Fix damageBindHandicapScaleX65C = fxFromFloat(1.0f);
    Fix damageBindHandicapBaseX660 = fxFromFloat(9.0f);
    Fix damageBindPortScaleX664 = fxFromFloat(0.0f);
    Fix damageBindPortBaseX668 = fxFromFloat(0.0f);
    Fix damageBindPercentScaleX66C = fxFromFloat(1.0f);
    Fix damageBindTimerDecrementX670 = fx(1);
    Fix damageBindMashDecrementX674 = fxFromFloat(6.0f);
    int burySubmergeFramesX5F4 = 30;
    Fix buryBaseX5F8 = fxFromFloat(90.0f);
    Fix buryHandicapScaleX5FC = fxFromFloat(1.0f);
    Fix buryHandicapBaseX600 = fxFromFloat(9.0f);
    Fix buryPortScaleX604 = fxFromFloat(0.0f);
    Fix buryPortBaseX608 = fxFromFloat(0.0f);
    Fix buryPercentScaleX60C = fxFromFloat(1.0f);
    Fix buryTimerDecrementX610 = fx(1);
    Fix buryMashDecrementX614 = fxFromFloat(6.0f);
    Fix buryJumpVelocityYx618 = fxFromFloat(3.0f);
    Fix buryJumpGravityThresholdX61C = fx(0);
    int buryJumpCollisionFramesX620 = 10;
    Fix spotDodgeStickThresholdX314 = -fxFromFloat(0.7f);
    int spotDodgeStickWindowX318 = 4;
    Fix rollStickThresholdX31C = fxFromFloat(0.7f);
    int rollStickWindowX320 = 4;
    int rollFromGuardFlagX324 = 5;
    Vec2 escapeAirDeadzoneX32C{fxFromFloat(0.25f), fxFromFloat(0.25f)};
    int escapeAirTimerX334 = 3;
    Fix escapeAirForceX338 = fxFromFloat(3.1f);
    Fix escapeAirDecayX33C = fxFromFloat(0.9f);
    Fix fallSpecialDriftX340 = fxFromFloat(0.6f);
    int landingFallSpecialLagX344 = 10;
    Fix fallSpecialPlatformStickThresholdX25C = -fxFromFloat(0.5f);
    Fix itemScrewJumpMultiplierX800 = fx(1);
    int runStopTurnLagX410 = 6;
    int downWaitAutoStandFramesX424 = 60;
    int downDamageThresholdX428 = 10;
    Fix runBrakeAnimFreezeVelocityX42C = 0;
    int runDirectFramesX430 = 10;
    Fix jumpMomentumYScaleX438 = fxFromFloat(0.3f);
    Fix animVelocityScaleX440 = fxFromFloat(1.3f);
    Fix fallAnimationDriftThresholdX444 = fxFromFloat(0.25f);
    Fix fallAnimationBlendRateX448 = fxFromFloat(0.25f);
    Fix shieldStickSmoothingX44C = fxFromFloat(0.15f);
    Fix sdiMinStickMagX4B0 = fxFromFloat(0.7f);
    int sdiStickWindowX4B4 = 4;
    Fix sdiPosScaleX4B8 = fx(6);
    Fix shieldAsdiPosScaleX4BC = fx(3);
    Fix shieldSdiDistanceX4C0 = fxFromFloat(0.66f);
    Fix platformDropStickThresholdX464 = fxFromFloat(0.66f);
    int platformDropStickWindowX468 = 6;
    Fix platformDropInitialVelocityX46C = -fxFromFloat(0.5f);
    int platformDropAnimationFramesX470 = 2;
    Fix teeterWalkInputThresholdX474 = fxFromFloat(0.5f);
    Fix teeterForwardDistanceX478 = fxFromFloat(1.33f);
    Fix teeterBackwardDistanceX47C = fxFromFloat(0.66f);
    Fix ledgeNoGrabDownThresholdX480 = fxFromFloat(0.66f);
    int cliffActionPercentThresholdX488 = 100;
    int cliffWaitAutoReleaseFramesQuickX48C = 300;
    int cliffWaitAutoReleaseFramesSlowX490 = 300;
    Fix cliffOptionStickThresholdX494 = fxFromFloat(0.25f);
    Fix cliffCStickAttackThresholdX7F8 = fxFromFloat(0.8f);
    Fix cliffCStickEscapeThresholdX7FC = fxFromFloat(0.8f);
    int ledgeCooldownX498 = 30;
    Fix damageIceGravityMultiplierX77C = fxFromFloat(0.25f);
    Fix damageIceTimerDamageScaleX790 = fxFromFloat(20.0f);
    Fix damageIceTimerDecrementX794 = fx(1);
    Fix damageIceMashDecrementX798 = fxFromFloat(6.0f);
    Fix damageIceHitDamageTimerReductionX79C = fx(1);
    Fix damageIceJumpEscapeFramesX7A4 = fxFromFloat(30.0f);
    int passiveWallTimerX760 = 5;
    int passiveWallIntangibilityX764 = 14;
    int wallJumpInputWindowX768 = 130;
    Fix wallJumpStickThresholdX76C = fxFromFloat(0.8f);
    int wallJumpStickWindowX770 = 3;
    int wallJumpStartupX774 = 5;
    Fix passiveWallVelYBaseX778 = fxFromFloat(0.975f);
    Fix aerialAttackDeadzoneXDC = fxFromFloat(0.2875f);
    Fix aerialAttackDeadzoneXE0 = fxFromFloat(0.2875f);
};

struct FighterProperties {
    // Melee-style common attributes. Character data should eventually map
    // directly into these names.
    Fix walkInitVel = fxFromFloat(0.1f);
    Fix walkAccel = fxFromFloat(0.1f);
    Fix walkMaxVel = fxFromFloat(1.1f);
    Fix slowWalkMax = fxFromFloat(0.18f);
    Fix midWalkPoint = fxFromFloat(0.44f);
    Fix fastWalkMin = fxFromFloat(0.7f);
    Fix grFriction = fxFromFloat(0.06f);
    Fix dashInitialVelocity = fxFromFloat(1.5f);
    Fix dashRunAccelerationA = fxFromFloat(0.06f);
    Fix dashRunAccelerationB = fxFromFloat(0.02f);
    Fix dashRunTerminalVelocity = fxFromFloat(1.5f);
    Fix groundMaxHorizontalVelocity = fxFromFloat(3.0f);
    Fix runAnimationScaling = fxFromFloat(1.45f);
    int maxRunBrakeFrames = 30;
    Fix jumpHInitialVelocity = fxFromFloat(1.0f);
    Fix jumpVInitialVelocity = fxFromFloat(2.3f);
    Fix damageScrewVerticalVelocity = fxFromFloat(2.3f);
    Fix hopVInitialVelocity = fxFromFloat(1.4f);
    Fix groundToAirJumpMomentumMultiplier = fxFromFloat(0.8f);
    Fix jumpHMaxVelocity = fxFromFloat(1.5f);
    Fix airJumpVMultiplier = fxFromFloat(1.0f);
    Fix airJumpHMultiplier = fxFromFloat(0.9f);
    Fix grav = fxFromFloat(0.095f);
    Fix terminalVel = fxFromFloat(1.7f);
    Fix airDriftStickMul = fxFromFloat(0.025f);
    Fix aerialDriftBase = fxFromFloat(0.02f);
    Fix airFriction = fxFromFloat(0.016f);
    Fix airDriftMax = fxFromFloat(0.86f);
    Fix airMaxHorizontalVelocity = fxFromFloat(3.0f);
    Fix ledgeSnapX = fxFromFloat(0.75f);
    Fix ledgeSnapY = fxFromFloat(1.25f);
    Fix ledgeSnapHeight = fxFromFloat(2.6f);
    Fix ledgeHangX = fxFromFloat(0.62f);
    Fix ledgeHangY = fxFromFloat(-1.45f);
    Fix ledgeClimbX = fxFromFloat(1.05f);
    Fix ledgeEscapeX = fxFromFloat(1.75f);
    Fix ledgeJumpHorizontalVelocity = fxFromFloat(1.0f);
    Fix ledgeJumpVerticalVelocity = fxFromFloat(2.3f);
    Fix ledgeDropHorizontalVelocity = fxFromFloat(0.35f);
    Fix ledgeDropVerticalVelocity = fxFromFloat(-0.45f);
    Fix passiveWallHorizontalVelocity = fxFromFloat(1.3f);
    Fix wallJumpHorizontalVelocity = fxFromFloat(1.3f);
    Fix wallJumpVerticalVelocity = fxFromFloat(2.3f);
    Fix passiveCeilHorizontalVelocity = fxFromFloat(1.0f);
    Fix damageIceJumpVelocityY = fxFromFloat(2.3f);
    Fix damageIceJumpVelocityXMultiplier = fxFromFloat(1.0f);
    Fix initialShieldSize = fxFromFloat(1.35f);
    bool shieldSizeScalesWithHealth = true;
    Fix shieldBreakInitialVelocity = fxFromFloat(2.5f);
    Fix gravity = fxFromFloat(0.095f);
    Fix terminalVelocity = fxFromFloat(1.7f);
    Fix fastFallTerminalVelocity = fxFromFloat(2.3f);
    Fix noImpactLandingVelocity = -fxFromFloat(0.5f);
    int normalLandingLag = 4;
    int nairLandingLag = 16;
    int fairLandingLag = 21;
    int bairLandingLag = 15;
    int uairLandingLag = 15;
    int dairLandingLag = 23;
    int framesToChangeDirectionOnStandingTurn = 4;
    Fix weight = fx(100);
    int weightIndependentThrowsMask = 0;
    int rapidJabWindow = 5;
    int maxJumps = 2;
    bool canWallJump = true;
    int jumpStartupLag = 4;
    Fix modelScale = fx(1);
    MeleeCommonData common;
    Fix initialWalkSpeed = fxFromFloat(0.1f);
    Fix initialDashSpeed = fxFromFloat(1.5f);
    Fix initialRunSpeed = fxFromFloat(1.5f);
    Fix walkAcceleration = fxFromFloat(0.1f);
    Fix runAcceleration = fxFromFloat(0.06f);
    Fix maxWalkSpeed = fxFromFloat(1.1f);
    Fix friction = fxFromFloat(0.06f);
    Fix aerialAcceleration = fxFromFloat(0.025f);
    Fix aerialFriction = fxFromFloat(0.016f);
    Fix maxAerialHorizontalSpeed = fxFromFloat(0.86f);
    Fix initialHorizontalJumpVelocity = fxFromFloat(1.0f);
    Fix initialVerticalJumpVelocity = fxFromFloat(2.3f);
    Fix maximumShorthopVerticalVelocity = fxFromFloat(1.4f);
};

struct ShieldDefinition {
    Fix startSizeHardShield = fx(15);
    Fix endSizeHardShield = fx(5);
    Fix startSizeLightShield = fx(30);
    Fix endSizeLightShield = fx(10);
    Fix maxHealth = fx(60);
};

enum class HurtboxState : uint8_t {
    Normal,
    Invincible,
    Intangible,
};

struct HurtboxDefinition {
    BoneId bone = BoneId::Hip;
    Vec3 startOffset;
    Vec3 endOffset;
    Fix radius = fx(1);
    HurtboxState state = HurtboxState::Normal;
    bool grabbable = true;
};

struct FighterEcbDefinition {
    bool enabled = false;
    std::array<Vec2, 4> points{
        Vec2{-fxFromFloat(0.55f), fxFromFloat(1.0f)},
        Vec2{0, fxFromFloat(2.0f)},
        Vec2{fxFromFloat(0.55f), fxFromFloat(1.0f)},
        Vec2{0, 0},
    };
};

struct HitboxDefinition {
    BoneId bone = BoneId::Hip;
    int hsdBone = -1;
    int hitboxId = 0;
    Vec3 offset;
    Fix radius = fx(1);
    Fix damage = 0;
    Fix damageShield = 0;
    Fix knockbackAngleDegrees = 0;
    bool knockbackAngleFixed = false;
    Fix knockbackBase = 0;
    Fix knockbackGrowth = 0;
    Fix knockbackWeightSet = 0;
    int element = 0;
    bool isGrab = false;
    bool canClank = false;
    bool reboundsOnClank = false;
    bool hitFighters = true;
    bool onlyHitGrabbed = false;
    bool requiresThrownHitboxOwner = false;
    bool hitGrounded = true;
    bool hitAirborne = true;
};

struct FunctionCall {
    std::string name;
    bool boolParam = false;
    int intParam = 0;
    Fix fixParam = 0;
};

struct PackageVariableDefinition {
    std::string name;
    int32_t initialValue = 0;
};

enum class PackageScriptOp : uint8_t {
    Nop,
    SetVarImmediate,
    SetVarFromVar,
    AddVarImmediate,
    AddVar,
    ScaleVarFixed,
    SetVarRandom,
    SetVarFrame,
    SetVarStateFrame,
    SetVarStateIndex,
    SetVarGrounded,
    SetVarFacing,
    SetVarFighterIndex,
    SetVarObjectIndex,
    SetVarFighterStateFrame,
    SetVarFighterStateIndex,
    SetVarFighterGrounded,
    SetVarFighterFacing,
    SetVarFighterJumpsUsed,
    SetVarFighterJumpsRemaining,
    SetFighterJumpsUsed,
    SetFighterJumpsUsedFromVar,
    SetVarFighterCommandVar,
    SetFighterCommandVarImmediate,
    SetFighterCommandVarFromVar,
    SetVarFighterThrowFlag,
    SetFighterThrowFlagImmediate,
    SetFighterThrowFlagFromVar,
    SetVarFighterHeldObject,
    SetVarFighterGrabbedFighter,
    SetVarFighterGrabberFighter,
    SetVarFighterHitlag,
    SetVarFighterHitstun,
    SetVarFighterDamageHitboxOwner,
    SetVarFighterThrownHitboxOwner,
    SetVarFighterPercent,
    SetVarFighterShield,
    SetVarFighterPositionX,
    SetVarFighterPositionY,
    SetVarFighterGroundVelocity,
    SetVarFighterAirVelocityX,
    SetVarFighterAirVelocityY,
    SetVarFighterAnimationFrame,
    SetVarFighterAnimationRate,
    SetVarObjectOwner,
    SetVarObjectHeldBy,
    SetVarObjectGrabVictim,
    SetVarObjectLastFighter,
    SetVarObjectLastObject,
    SetVarObjectDamage,
    SetObjectDamage,
    SetObjectDamageFromVar,
    SetVarObjectHitlag,
    SetObjectHitlag,
    SetObjectHitlagFromVar,
    SetVarObjectGroundSegment,
    SetVarObjectPositionX,
    SetVarObjectPositionY,
    SetVarObjectVelocityX,
    SetVarObjectVelocityY,
    SetVarObjectAnimationFrame,
    SetVarObjectAnimationRate,
    SetObjectOwner,
    SetObjectOwnerFromVar,
    SetVarOwnedObjectCount,
    SetVarOwnerFighterVar,
    SetOwnerFighterVarImmediate,
    SetOwnerFighterVarFromVar,
    SetVarButtonDown,
    SetVarButtonPressed,
    SetVarStickX,
    SetVarStickY,
    SetVarCStickX,
    SetVarCStickY,
    SetVarShield,
    SetGroundVelocity,
    SetAirVelocityX,
    SetAirVelocityY,
    SetPositionX,
    SetPositionY,
    SetFacing,
    SetGroundVelocityFromVar,
    SetAirVelocityXFromVar,
    SetAirVelocityYFromVar,
    SetPositionXFromVar,
    SetPositionYFromVar,
    SetFacingFromVar,
    ChangeState,
    SpawnObject,
    SpawnObjectFromVars,
    SpawnProjectile,
    SpawnProjectileFromVars,
    SpawnObjectSetVar,
    SpawnProjectileSetVar,
    SpawnObjectFromVarsSetVar,
    SpawnProjectileFromVarsSetVar,
    DestroyObject,
    DestroyObjectFromVar,
    SetVarPickUpObjectFromVar,
    SetVarDropObjectFromVar,
    SetVarThrowObjectFromVar,
    SetVarReflectObjectFromVar,
    SetVarAbsorbObjectFromVar,
    SetVarShieldBounceObjectFromVar,
    SetVarInteractObjectFromVar,
    SetVarInteractObjectsFromVars,
    DestroyOwnedObjects,
    SkipIfVarLessThanImmediate,
    SkipIfVarLessThanVar,
    SkipIfVarEqualImmediate,
    SkipIfVarEqualVar,
    JumpRelative,
    CallScript,
    SwitchFighterDefinition,
    SpawnFighter,
    SetAnimationRate,
    SetAnimationRateFromVar,
    SetAnimationFrame,
    SetAnimationFrameFromVar,
    SpawnFighterSetVar,
    SetVarIndexedFighterStateIndex,
    SetVarIndexedFighterPositionX,
    SetVarIndexedFighterPositionY,
    SetIndexedFighterStateFromVar,
    SetIndexedFighterPositionFromVars,
    SetIndexedFighterFacingFromVar,
    SetVarIndexedFighterVar,
    SetIndexedFighterVarImmediate,
    SetIndexedFighterVarFromVar,
    SetVarIndexedObjectVar,
    SetIndexedObjectVarImmediate,
    SetIndexedObjectVarFromVar,
};

struct PackageScriptInstruction {
    PackageScriptOp op = PackageScriptOp::Nop;
    int dst = -1;
    int srcA = -1;
    int srcB = -1;
    int32_t intValue = 0;
    Fix fixValue = 0;
    std::string text;
};

struct PackageScript {
    std::string name;
    int instructionBudget = 64;
    std::vector<PackageScriptInstruction> instructions;
};

enum class SubactionType : uint8_t {
    SyncTimer,
    AsyncTimer,
    SetLoop,
    ExecuteLoop,
    CreateHitbox,
    RemoveHitbox,
    AdjustHitboxDamage,
    AdjustHitboxSize,
    SetHitboxInteraction,
    CreateThrowHitbox,
    ClearHitboxes,
    SetHurtboxState,
    SetBodyCollisionState,
    SetInterruptible,
    SetFlag,
    SetThrowFlag,
    SetThrowFlagLiteral,
    EnableJabFollowup,
    SetJabRapid,
    SetJumpState,
    ReverseDirection,
    StartSmashCharge,
    SetModelVisibility,
    RevertModelVisibility,
    RemoveModelVisibility,
    SetModelPartAnimation,
    SetFighterVisibility,
    SelfDamage,
    SpawnObject,
    SpawnProjectile,
    CallScript,
};

struct Subaction {
    SubactionType type = SubactionType::SyncTimer;
    int frames = 0;
    int loopCount = 1;
    int interruptibleFrame = 0;
    int flag = 0;
    int hurtboxIndex = -1;
    int hsdBone = -1;
    HurtboxState hurtboxState = HurtboxState::Normal;
    uint32_t flagValue = 0;
    Fix smashChargeHoldFrames = 0;
    Fix smashChargeDamageMultiplier = fx(1);
    Fix selfDamage = 0;
    std::string objectName;
    Vec2 spawnVelocity = {};
    Vec3 spawnOffset = {};
    int modelPartIndex = -1;
    int modelPartState = -1;
    int modelPartAnimation = -1;
    HitboxDefinition hitbox;
};

enum class GroundRequirement : uint8_t {
    Any,
    OnlyGrounded,
    OnlyAirborne,
};

constexpr int kUseDefaultAnimationBlendFrames = -1;
constexpr int kDisableAnimationBlendFrames = -2;

enum class InterruptCondition : uint8_t {
    JumpPressed,
    AerialJumpForwardPressed,
    AerialJumpBackwardPressed,
    AirDodgePressed,
    WallJumpInput,
    SquatInput,
    SquatReleaseInput,
    AttackPressed,
    JabFollowupPressed,
    RapidJabReady,
    SpecialNInput,
    SpecialSInput,
    SpecialHiInput,
    SpecialLwInput,
    SpecialAirNInput,
    SpecialAirSInput,
    SpecialAirHiInput,
    SpecialAirLwInput,
    AttackDashPressed,
    AttackDashGrabBuffer,
    AttackS4HiPressed,
    AttackS4HiSPressed,
    AttackS4Pressed,
    AttackS4LwSPressed,
    AttackS4LwPressed,
    AttackS42Pressed,
    AttackHi4Pressed,
    AttackHi4NoStickWindowPressed,
    AttackLw4Pressed,
    AttackS3HiPressed,
    AttackS3HiSPressed,
    AttackS3Pressed,
    AttackS3LwSPressed,
    AttackS3LwPressed,
    AttackHi3Pressed,
    AttackLw3Pressed,
    AttackLw3Repeat,
    AerialAttackNPressed,
    AerialAttackFPressed,
    AerialAttackBPressed,
    AerialAttackHiPressed,
    AerialAttackLwPressed,
    DashInput,
    ReverseDashInput,
    RunInput,
    RunJumpPressed,
    TeeterWalkInput,
    HorizontalWalkSlow,
    HorizontalWalkMiddle,
    HorizontalWalkFast,
    TurnInput,
    TurnRunInput,
    RunBrakeTurnRunInput,
    RunBrakeInput,
    WaitInput,
    BecameAirborne,
    ShieldReflectInput,
    ShieldPressed,
    ShieldHeld,
    ShieldJumpPressed,
    GuardCatchDashPressed,
    SpotDodgeInput,
    RollForwardInput,
    RollBackwardInput,
    LedgeClimbInput,
    LedgeDropInput,
    LedgeAttackInput,
    LedgeEscapeInput,
    GrabPressed,
    TauntPressed,
    PackageVarAtLeast,
};

struct InterruptRule {
    std::string targetState;
    InterruptCondition condition = InterruptCondition::JumpPressed;
    GroundRequirement ground = GroundRequirement::Any;
    int blendFrames = kUseDefaultAnimationBlendFrames;
    int lagFrames = 0;
    bool startActive = true;
    bool alwaysActive = false;
    bool requireNoHitstun = false;
    int enableFrame = 0;
    int disableFrame = 0;
    int packageVariable = -1;
    int32_t packageValue = 1;
};

struct FighterState {
    std::string name;
    std::string animation;
    int animationActionIndex = -1;
    bool useAnimPhysics = false;
    bool allowSlideoff = true;
    bool allowLedgeGrab = true;
    bool allowBackwardsLedgeGrab = false;
    bool allowWallCollision = true;
    bool allowCeilingCollision = true;
    bool convertFloorCollisionToGround = true;
    bool loopAnimation = false;
    std::string onAnimationFinishedState;
    int onAnimationFinishedBlendFrames = kUseDefaultAnimationBlendFrames;
    int defaultAnimationBlendFrames = 0;
    int animationLengthFrames = 60;
    int initialInterruptibleFrame = 0;
    std::vector<FunctionCall> onEnter;
    std::vector<FunctionCall> onFrame;
    std::vector<FunctionCall> onLanding;
    std::vector<FunctionCall> onAirborne;
    std::vector<InterruptRule> interrupts;
    std::vector<Subaction> action;
};

struct FighterDefinition {
    std::string name;
    FighterProperties properties;
    ShieldDefinition shield;
    std::shared_ptr<const HsdFighterAnimationAsset> hsdAsset;
    bool hasHsdAsset = false;
    FighterEcbDefinition authoredEcb;
    std::vector<AnimationJoint> authoredSkeleton;
    std::vector<AnimationClip> authoredClips;
    HsdFighterMesh authoredMesh;
    std::vector<PackageVariableDefinition> packageVariables;
    std::vector<PackageScript> packageScripts;
    std::vector<HurtboxDefinition> hurtboxes;
    std::vector<FighterState> states;

    int stateIndex(const std::string& name) const;
};

FighterDefinition makeDebugRook();
BoneId boneFromName(const std::string& name);
const char* boneName(BoneId bone);

} // namespace pf
