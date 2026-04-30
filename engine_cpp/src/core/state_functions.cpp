#include "core/state_functions.hpp"

#include <algorithm>
#include <cmath>

namespace pf {

static int signOf(Fix value) {
    return value < 0 ? -1 : 1;
}

static FighterProperties& props(World& world, FighterRuntime& fighter) {
    return world.fighterDefs[static_cast<size_t>(fighter.fighterDef)].properties;
}

static Fix groundFrictionMultiplier(const World& world, const FighterRuntime& fighter) {
    if (fighter.groundSegment < 0 || fighter.groundSegment >= static_cast<int>(world.stage.segments.size())) {
        return fx(1);
    }
    return world.stage.segments[static_cast<size_t>(fighter.groundSegment)].friction;
}

static Vec2 floorNormal(const StageSegment& segment) {
    const float dx = fxToFloat(segment.end.x - segment.start.x);
    const float dy = fxToFloat(segment.end.y - segment.start.y);
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0001f) {
        return {0, fx(1)};
    }
    Vec2 normal{fxFromFloat(-dy / len), fxFromFloat(dx / len)};
    if (normal.y < 0) {
        normal.x = -normal.x;
        normal.y = -normal.y;
    }
    return normal;
}

static void applyGroundFriction(FighterRuntime& fighter, Fix friction) {
    if (fxAbs(friction) > fxAbs(fighter.groundVelocity)) {
        fighter.groundAccel = -fighter.groundVelocity;
        return;
    }
    fighter.groundAccel = fighter.groundVelocity > 0 ? -friction : friction;
}

static void applyGroundAccelToward(FighterRuntime& fighter, const FighterProperties& attr, Fix accel, Fix targetVel, Fix friction) {
    if (targetVel == 0) {
        applyGroundFriction(fighter, friction);
        return;
    }

    if (fxMul(fighter.groundVelocity, accel) >= 0) {
        if (accel > 0 && fighter.groundVelocity + accel > targetVel) {
            accel = -friction;
            if (fighter.groundVelocity + accel < targetVel) {
                accel = targetVel - fighter.groundVelocity;
            }
            if (fighter.groundVelocity + accel > attr.groundMaxHorizontalVelocity) {
                accel = attr.groundMaxHorizontalVelocity - fighter.groundVelocity;
            }
        } else if (accel < 0 && fighter.groundVelocity + accel < targetVel) {
            accel = friction;
            if (fighter.groundVelocity + accel > targetVel) {
                accel = targetVel - fighter.groundVelocity;
            }
            if (fighter.groundVelocity + accel < -attr.groundMaxHorizontalVelocity) {
                accel = -attr.groundMaxHorizontalVelocity - fighter.groundVelocity;
            }
        }
    }

    fighter.groundAccel = accel;
}

static void updateRunAnimationRate(World& world, FighterRuntime& fighter) {
    const FighterProperties& attr = props(world, fighter);
    const Fix velocity = groundFrictionMultiplier(world, fighter) < fx(1)
        ? fighter.runAnimationVelocity
        : fighter.groundVelocity;
    if (attr.runAnimationScaling == 0 || fxMul(velocity, fighter.facing * fx(1)) <= 0) {
        fighter.animationRate = 0;
        return;
    }
    fighter.animationRate = fxDiv(fxAbs(velocity), attr.runAnimationScaling);
}

static void processAirborne(World& world, FighterRuntime& fighter);
static void fastfallCheck(World& world, FighterRuntime& fighter);
static void maintainLedge(World& world, FighterRuntime& fighter);
static bool canDropThroughCurrentFloor(const World& world, const FighterRuntime& fighter);

static void commonEnter(World& world, FighterRuntime& fighter) {
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    fighter.activeHitboxes.clear();
    fighter.fightersHitThisAction.clear();
    fighter.throwHitboxActive.fill(false);
    const size_t hurtboxCount = def.hasHsdAsset && def.hsdAsset ? def.hsdAsset->hurtboxes.size() : def.hurtboxes.size();
    fighter.hurtboxStates.assign(hurtboxCount, HurtboxState::Normal);
    fighter.commandFlags = 0;
    fighter.turnFacingAfter = 0;
    fighter.turnHasTurned = false;
    fighter.turnJustTurned = false;
    fighter.turnDashBuffered = false;
    fighter.runDirectTimer = 0;
    fighter.runBrakeTimer = 0;
    fighter.runBrakeAnimationFrozen = false;
    fighter.ledgeActionReady = false;
    fighter.guardMinHoldTimer = 0;
    fighter.guardSetoffTimer = 0;
    fighter.guardReleaseQueued = false;
    fighter.smashChargeState = 0;
    fighter.smashChargeFrames = 0;
    fighter.smashChargeHoldFrames = 0;
    fighter.smashChargeDamageMultiplier = fx(1);
    fighter.smashChargeStoredAnimationRate = fx(1);
    fighter.throwAnimationFrozen = false;
    fighter.thrownAnimationFreezeActive = false;
    fighter.thrownAnimationFreezeFrame = 0;
}

static Fix shieldLightAmount(const FighterRuntime& fighter) {
    if (!fighter.input.down(ButtonShield)) {
        return 0;
    }
    return std::clamp(fighter.input.frames[0].shieldAnalog, Fix{0}, fx(1));
}

static float normalizeAngle0(float value) {
    while (value < 0.0f) {
        value += 360.0f;
    }
    while (value >= 360.0f) {
        value -= 360.0f;
    }
    return value;
}

static float normalizeAngle180(float value) {
    value = normalizeAngle0(value);
    if (value > 180.0f) {
        value -= 360.0f;
    }
    return value;
}

static void updateGuardPose(World& world, FighterRuntime& fighter) {
    const FighterProperties& attr = props(world, fighter);
    const float inputX = fxToFloat(fighter.input.frames[0].move.x) * static_cast<float>(fighter.facing);
    const float inputY = fxToFloat(fighter.input.frames[0].move.y);
    float stickRadians = std::atan2(inputY, inputX);
    if (stickRadians < 0.0f) {
        stickRadians += 6.2831853071795864769f;
    }

    const float stickDegrees = std::clamp(stickRadians * 57.29577951308232f, 0.0f, 359.0f);
    const float previousOffset = fxToFloat(fighter.guardPoseFrame) - 10.0f;
    const float delta = normalizeAngle180(stickDegrees - previousOffset);
    const float smoothing = fxToFloat(attr.common.shieldStickSmoothingX44C);
    fighter.guardPoseFrame = fxFromFloat(10.0f + normalizeAngle0(delta * smoothing + previousOffset));

    const float stickMagnitude = std::min(1.0f, std::sqrt(inputX * inputX + inputY * inputY));
    fighter.guardPoseBlend = fxFromFloat(smoothing * (stickMagnitude - fxToFloat(fighter.guardPoseBlend)) +
                                         fxToFloat(fighter.guardPoseBlend));
}

static void drainShield(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const Fix light = shieldLightAmount(fighter);
    const Fix drainScale = attr.common.hardShieldDrainScaleX2EC +
        fxMul(light, attr.common.lightShieldDrainScaleX2F0 - attr.common.hardShieldDrainScaleX2EC);
    fighter.shieldHealth = std::max(Fix{0}, fighter.shieldHealth - fxMul(attr.common.shieldDrainRateX278, drainScale));
}

static void enterGuardOn(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.guardMinHoldTimer = attr.common.guardMinHoldFramesX268;
    fighter.guardReleaseQueued = false;
    fighter.guardPoseFrame = fx(10);
    fighter.guardPoseBlend = 0;
    fighter.groundVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
}

static void processGuard(World& world, FighterRuntime& fighter, bool opening) {
    FighterProperties& attr = props(world, fighter);
    fighter.jumpsUsed = 0;
    applyGroundFriction(fighter, attr.grFriction);
    updateGuardPose(world, fighter);

    if (fighter.input.down(ButtonShield) &&
        canDropThroughCurrentFloor(world, fighter) &&
        fighter.input.frames[0].move.y <= -attr.common.platformDropStickThresholdX464 &&
        fighter.stickYTiltTimer < attr.common.platformDropStickWindowX468)
    {
        changeFighterState(world, fighter, "Pass");
        return;
    }

    if (!fighter.input.down(ButtonShield)) {
        fighter.guardReleaseQueued = true;
    }
    if (fighter.guardMinHoldTimer > 0) {
        --fighter.guardMinHoldTimer;
    }

    drainShield(world, fighter);
    if (fighter.shieldHealth <= 0) {
        fighter.shieldHealth = 0;
        changeFighterState(world, fighter, "ShieldBreakFly");
        return;
    }
    if (fighter.guardReleaseQueued && fighter.guardMinHoldTimer <= 0) {
        changeFighterState(world, fighter, "GuardOff");
        return;
    }
    if (opening && frameInState(fighter) >= attr.common.guardMinHoldFramesX268) {
        changeFighterState(world, fighter, "Guard");
    }
}

static void processGuardOn(World& world, FighterRuntime& fighter) {
    processGuard(world, fighter, true);
}

static void processGuardHeld(World& world, FighterRuntime& fighter) {
    processGuard(world, fighter, false);
}

static void processGuardReflect(World& world, FighterRuntime& fighter) {
    processGuard(world, fighter, true);
}

static void processGuardSetoff(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    applyGroundFriction(fighter, fxMul(attr.grFriction, attr.common.shieldGroundFrictionMultiplierX3EC));
    if (!fighter.input.down(ButtonShield)) {
        fighter.guardReleaseQueued = true;
    }
    if (fighter.guardSetoffTimer > 0) {
        --fighter.guardSetoffTimer;
    }
    if (fighter.guardSetoffTimer <= 0) {
        changeFighterState(world, fighter, fighter.guardReleaseQueued ? "GuardOff" : "Guard");
    }
}

static bool isValidFighterIndex(const World& world, int index) {
    return index >= 0 && index < static_cast<int>(world.fighters.size());
}

static size_t fighterIndexOf(World& world, FighterRuntime& fighter) {
    return static_cast<size_t>(&fighter - world.fighters.data());
}

static bool isThrowStateName(const std::string& name) {
    return name == "ThrowF" || name == "ThrowB" || name == "ThrowHi" || name == "ThrowLw";
}

static const char* thrownStateForThrow(const std::string& name) {
    if (name == "ThrowB") return "ThrownB";
    if (name == "ThrowHi") return "ThrownHi";
    if (name == "ThrowLw") return "ThrownLw";
    return "ThrownF";
}

static bool throwForwardInput(const FighterRuntime& fighter, const MeleeCommonData& common) {
    const Fix previous = fighter.input.frames[1].move.x;
    const Fix current = fighter.input.frames[0].move.x;
    if (((previous < common.attackS3StickThresholdX98 && current >= common.attackS3StickThresholdX98) ||
         (previous > -common.attackS3StickThresholdX98 && current <= -common.attackS3StickThresholdX98)) &&
        current * fighter.facing > 0)
    {
        return true;
    }
    const Fix previousC = fighter.input.frames[1].cStick.x;
    const Fix currentC = fighter.input.frames[0].cStick.x;
    return ((previousC < common.attackS3StickThresholdX98 && currentC >= common.attackS3StickThresholdX98) ||
            (previousC > -common.attackS3StickThresholdX98 && currentC <= -common.attackS3StickThresholdX98)) &&
        currentC * fighter.facing > 0;
}

static bool throwBackwardInput(const FighterRuntime& fighter, const MeleeCommonData& common) {
    const Fix previous = fighter.input.frames[1].move.x;
    const Fix current = fighter.input.frames[0].move.x;
    if (((previous < common.attackS3StickThresholdX98 && current >= common.attackS3StickThresholdX98) ||
         (previous > -common.attackS3StickThresholdX98 && current <= -common.attackS3StickThresholdX98)) &&
        current * fighter.facing <= 0)
    {
        return true;
    }
    const Fix previousC = fighter.input.frames[1].cStick.x;
    const Fix currentC = fighter.input.frames[0].cStick.x;
    return ((previousC < common.attackS3StickThresholdX98 && currentC >= common.attackS3StickThresholdX98) ||
            (previousC > -common.attackS3StickThresholdX98 && currentC <= -common.attackS3StickThresholdX98)) &&
        currentC * fighter.facing <= 0;
}

static bool throwHighInput(const FighterRuntime& fighter, const MeleeCommonData& common) {
    return (fighter.input.frames[1].move.y < common.attackHi3StickThresholdYxAC &&
            fighter.input.frames[0].move.y >= common.attackHi3StickThresholdYxAC) ||
        (fighter.input.frames[1].cStick.y < common.attackHi3StickThresholdYxAC &&
         fighter.input.frames[0].cStick.y >= common.attackHi3StickThresholdYxAC);
}

static bool throwLowInput(const FighterRuntime& fighter, const MeleeCommonData& common) {
    return (fighter.input.frames[1].move.y > common.attackLw3StickThresholdYxB0 &&
            fighter.input.frames[0].move.y <= common.attackLw3StickThresholdYxB0) ||
        (fighter.input.frames[1].cStick.y <= common.attackLw3StickThresholdYxB0 &&
         fighter.input.frames[0].cStick.y <= common.attackLw3StickThresholdYxB0);
}

static void processCatch(World& world, FighterRuntime& fighter) {
    applyGroundFriction(fighter, fxMul(props(world, fighter).common.groundFrictionScaleX60, props(world, fighter).grFriction));
    if (fighter.grabbedFighter >= 0) {
        changeFighterState(world, fighter, currentState(world, fighter).name == "CatchDash" ? "CatchDashPull" : "CatchPull");
    }
}

static void processCatchDash(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    applyGroundAccelToward(
        fighter,
        attr,
        -signOf(fighter.groundVelocity) * fxMul(attr.common.groundFrictionScaleX60, attr.grFriction),
        0,
        fxMul(attr.common.groundFrictionScaleX60, attr.grFriction));
    if (fighter.grabbedFighter >= 0) {
        changeFighterState(world, fighter, "CatchDashPull");
    }
}

static void processCatchPull(World& world, FighterRuntime& fighter) {
    applyGroundFriction(fighter, fxMul(props(world, fighter).common.groundFrictionScaleX60, props(world, fighter).grFriction));
    if (!isValidFighterIndex(world, fighter.grabbedFighter)) {
        changeFighterState(world, fighter, "Wait");
        return;
    }
    if (fighterCommandFlag(fighter, 3) || frameInState(fighter) >= currentState(world, fighter).animationLengthFrames) {
        fighter.groundVelocity = 0;
        setFighterCommandFlag(fighter, 3, false);
        changeFighterState(world, fighter, "CatchWait");
    }
}

static void processCatchWait(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    applyGroundFriction(fighter, fxMul(attr.common.groundFrictionScaleX60, attr.grFriction));
    if (!isValidFighterIndex(world, fighter.grabbedFighter)) {
        changeFighterState(world, fighter, "Wait");
        return;
    }
    if (fighter.input.justPressed(ButtonAttack)) {
        changeFighterState(world, fighter, "CatchAttack");
        return;
    }
    if (throwHighInput(fighter, attr.common)) {
        changeFighterState(world, fighter, "ThrowHi");
        return;
    }
    if (throwLowInput(fighter, attr.common)) {
        changeFighterState(world, fighter, "ThrowLw");
        return;
    }
    if (throwBackwardInput(fighter, attr.common)) {
        changeFighterState(world, fighter, "ThrowB");
        return;
    }
    if (throwForwardInput(fighter, attr.common)) {
        changeFighterState(world, fighter, "ThrowF");
    }
}

static void processCatchAttack(World& world, FighterRuntime& fighter) {
    applyGroundFriction(fighter, fxMul(props(world, fighter).common.groundFrictionScaleX60, props(world, fighter).grFriction));
    if (!isValidFighterIndex(world, fighter.grabbedFighter)) {
        changeFighterState(world, fighter, "Wait");
        return;
    }
    if (frameInState(fighter) >= currentState(world, fighter).animationLengthFrames) {
        changeFighterState(world, fighter, "CatchWait");
    }
}

static void enterThrow(World& world, FighterRuntime& fighter) {
    const std::string stateName = currentState(world, fighter).name;
    if (!isThrowStateName(stateName) || !isValidFighterIndex(world, fighter.grabbedFighter)) {
        return;
    }
    const size_t grabberIndex = fighterIndexOf(world, fighter);
    FighterRuntime& victim = world.fighters[static_cast<size_t>(fighter.grabbedFighter)];
    fighter.groundVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    victim.facing = fighter.facing;
    victim.grabberFighter = static_cast<int>(grabberIndex);
    fighter.throwAnimationFrozen = false;

    const FighterProperties& victimAttr = props(world, victim);
    const Fix weightScale = fxMul(victimAttr.weight, props(world, fighter).common.throwWeightAnimationScaleX37C);
    const Fix animRate = weightScale > 0 ? fxDiv(fx(1), weightScale) : fx(1);
    fighter.animationRate = animRate;
    beginMeleeThrowConstraint(world, grabberIndex, static_cast<size_t>(fighter.grabbedFighter));
    changeFighterState(world, victim, thrownStateForThrow(stateName), 0, kDisableAnimationBlendFrames);
    victim.animationRate = animRate;
    victim.grabberFighter = static_cast<int>(grabberIndex);
    updateMeleeCapturePosition(world, static_cast<size_t>(fighter.grabbedFighter));
}

static void processThrow(World& world, FighterRuntime& fighter) {
    if (fighterCommandFlag(fighter, 4)) {
        fighter.facing *= -1;
        setFighterCommandFlag(fighter, 4, false);
    }
    if (fighterCommandFlag(fighter, 0) && !fighter.throwAnimationFrozen) {
        fighter.throwAnimationFrozen = true;
        setFighterCommandFlag(fighter, 0, false);
        fighter.animationRate = 0;
        if (isValidFighterIndex(world, fighter.grabbedFighter)) {
            FighterRuntime& victim = world.fighters[static_cast<size_t>(fighter.grabbedFighter)];
            victim.thrownAnimationFreezeActive = true;
            victim.thrownAnimationFreezeFrame = fighter.animationFrame;
            if (victim.animationFrame == victim.thrownAnimationFreezeFrame) {
                victim.animationRate = 0;
                victim.thrownAnimationFreezeFrame = 0;
            }
        }
    }
    if (!isValidFighterIndex(world, fighter.grabbedFighter) &&
        frameInState(fighter) >= currentState(world, fighter).animationLengthFrames)
    {
        changeFighterState(world, fighter, "Wait");
    }
}

static void enterCatchCut(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    if (fighter.grounded) {
        fighter.groundVelocity = -fighter.facing * attr.common.captureCutGroundVelocityX370;
    } else {
        fighter.fighterVelocity.x = -fighter.facing * attr.common.captureJumpVelocityX374;
    }
}

static void processCatchCut(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    if (fighter.grounded) {
        applyGroundFriction(fighter, fxMul(attr.common.captureCutFrictionScaleX36C, attr.grFriction));
    } else {
        processAirborne(world, fighter);
    }
}

static void enterCaptureCut(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const int grabberIndex = fighter.grabberFighter;
    if (isValidFighterIndex(world, grabberIndex)) {
        releaseMeleeCaptureConstraint(world, static_cast<size_t>(grabberIndex), static_cast<int>(fighterIndexOf(world, fighter)), true);
    }
    if (fighter.grounded) {
        fighter.groundVelocity = -fighter.facing * attr.common.captureCutGroundVelocityX370;
    } else {
        fighter.fighterVelocity.x = -fighter.facing * attr.common.captureCutGroundVelocityX370;
    }
    fighter.grabberFighter = -1;
    fighter.grabbedFighter = -1;
    fighter.captureConstraintActive = false;
    fighter.captureConstraintOffset = {};
    fighter.captureOriginalXRotNTranslation = {};
    fighter.thrownAnimationFreezeActive = false;
}

static void processCaptureCut(World& world, FighterRuntime& fighter) {
    processCatchCut(world, fighter);
}

static void enterCaptureJump(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const int grabberIndex = fighter.grabberFighter;
    if (isValidFighterIndex(world, grabberIndex)) {
        releaseMeleeCaptureConstraint(world, static_cast<size_t>(grabberIndex), static_cast<int>(fighterIndexOf(world, fighter)), true);
    }
    fighter.grounded = false;
    fighter.groundSegment = -1;
    fighter.fighterVelocity.x = -fighter.facing * attr.common.captureJumpVelocityX374;
    fighter.fighterVelocity.y = attr.common.captureJumpVelocityYx378;
    fighter.grabberFighter = -1;
    fighter.grabbedFighter = -1;
    fighter.captureConstraintActive = false;
    fighter.captureConstraintOffset = {};
    fighter.captureOriginalXRotNTranslation = {};
    fighter.thrownAnimationFreezeActive = false;
}

static void processCapture(World& world, FighterRuntime& fighter) {
    fighter.fighterVelocity = {};
    fighter.knockbackVelocity = {};
    fighter.groundVelocity = 0;
    if (!isValidFighterIndex(world, fighter.grabberFighter)) {
        changeFighterState(world, fighter, "Fall");
    }
}

static int mashAxis(Fix value, Fix threshold) {
    if (value <= -threshold) return -1;
    if (value >= threshold) return 1;
    return 0;
}

static bool applyGrabMash(FighterRuntime& fighter, const MeleeCommonData& common, Fix decrement) {
    bool result = false;
    if (fighter.input.justPressed(ButtonAttack | ButtonJump | ButtonShield | ButtonGrab)) {
        fighter.grabTimer -= decrement;
        result = true;
    }
    const int stickX = mashAxis(fighter.input.frames[0].move.x, common.stickXTiltThresholdX8);
    const int stickY = mashAxis(fighter.input.frames[0].move.y, common.stickYTiltThresholdXC);
    if ((stickX != 0 && stickX != fighter.grabMashStickX) ||
        (stickY != 0 && stickY != fighter.grabMashStickY))
    {
        fighter.grabTimer -= decrement;
        result = true;
    }
    if (stickX != 0) fighter.grabMashStickX = stickX;
    if (stickY != 0) fighter.grabMashStickY = stickY;
    return result;
}

static void escapeCapture(World& world, FighterRuntime& fighter, bool jumpOut) {
    const int grabberIndex = fighter.grabberFighter;
    if (isValidFighterIndex(world, grabberIndex)) {
        FighterRuntime& grabber = world.fighters[static_cast<size_t>(grabberIndex)];
        releaseMeleeCaptureConstraint(world, static_cast<size_t>(grabberIndex), static_cast<int>(fighterIndexOf(world, fighter)), true);
        if (currentState(world, grabber).name == "CatchWait" || currentState(world, grabber).name == "CatchAttack") {
            changeFighterState(world, grabber, "CatchCut");
        }
    }
    changeFighterState(world, fighter, jumpOut ? "CaptureJump" : "CaptureCut");
}

static void processCaptureWait(World& world, FighterRuntime& fighter) {
    const MeleeCommonData& common = props(world, fighter).common;
    fighter.captureWaitTimer += fx(1);
    fighter.grabTimer -= common.captureTimerDecrementX3A4;
    const bool mashed = applyGrabMash(fighter, common, common.captureMashDecrementX3A8);
    if (fighter.captureWaitTimer < common.captureJumpButtonWindowX3AC && fighter.input.justPressed(ButtonJump)) {
        fighter.captureJumpQueued = true;
    }
    if (mashed) {
        fighter.captureMashAnimTimer = common.captureMashAnimHoldFramesX3B0;
        fighter.animationRate = common.captureMashAnimRateX3B4;
    } else if (fighter.captureMashAnimTimer > 0) {
        fighter.captureMashAnimTimer -= fx(1);
        if (fighter.captureMashAnimTimer <= 0) {
            fighter.animationRate = fx(1);
        }
    }
    processCapture(world, fighter);
    if (currentState(world, fighter).name != "CaptureWaitHi" && currentState(world, fighter).name != "CaptureWaitLw") {
        return;
    }
    if (fighter.grabTimer <= 0) {
        escapeCapture(world, fighter, fighter.captureJumpQueued || fighter.input.frames[0].move.y >= common.tapJumpThresholdX70);
    }
}

static void processCaptureDamage(World& world, FighterRuntime& fighter) {
    fighter.grabTimer -= props(world, fighter).common.captureTimerDecrementX3A4;
    applyGrabMash(fighter, props(world, fighter).common, props(world, fighter).common.captureMashDecrementX3A8);
    processCapture(world, fighter);
}

static void processThrown(World& world, FighterRuntime& fighter) {
    if (!isValidFighterIndex(world, fighter.grabberFighter)) {
        return;
    }
    if (fighter.thrownAnimationFreezeActive && fighter.thrownAnimationFreezeFrame > 0 &&
        fighter.animationFrame == fighter.thrownAnimationFreezeFrame)
    {
        fighter.animationRate = 0;
        fighter.thrownAnimationFreezeFrame = 0;
    }
    applyGrabMash(fighter, props(world, fighter).common, props(world, fighter).common.thrownMashDecrementX3C8);
    if (fighter.grabTimer <= 0) {
        escapeCapture(world, fighter, fighter.captureJumpQueued || fighter.input.frames[0].move.y >= props(world, fighter).common.tapJumpThresholdX70);
    }
}

static void catchAirborne(World& world, FighterRuntime& fighter) {
    if (isValidFighterIndex(world, fighter.grabbedFighter)) {
        FighterRuntime& victim = world.fighters[static_cast<size_t>(fighter.grabbedFighter)];
        releaseMeleeCaptureConstraint(world, fighterIndexOf(world, fighter), fighter.grabbedFighter, true);
        changeFighterState(world, victim, "Fall");
    }
    fighter.grabbedFighter = -1;
    changeFighterState(world, fighter, "Fall");
}

static void throwAirborne(World& world, FighterRuntime& fighter) {
    if (isValidFighterIndex(world, fighter.grabbedFighter)) {
        FighterRuntime& victim = world.fighters[static_cast<size_t>(fighter.grabbedFighter)];
        releaseMeleeCaptureConstraint(world, fighterIndexOf(world, fighter), fighter.grabbedFighter, true);
        changeFighterState(world, victim, "Fall");
    }
    fighter.grabbedFighter = -1;
    changeFighterState(world, fighter, "Fall");
}

static void enterShieldBreakFly(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.grounded = false;
    fighter.groundSegment = -1;
    lockFighterEcb(fighter, 10);
    fighter.fighterVelocity.x = 0;
    fighter.fighterVelocity.y = attr.shieldBreakInitialVelocity;
    fighter.knockbackVelocity = {};
    fighter.attackerShieldKnockback = {};
    fighter.groundVelocity = 0;
    fighter.groundKnockbackVelocity = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.shieldHealth = 0;
}

static void processShieldBreakAir(World& world, FighterRuntime& fighter) {
    processAirborne(world, fighter);
}

static void shieldBreakLanding(World& world, FighterRuntime& fighter) {
    fighter.fighterVelocity.y = 0;
    fighter.knockbackVelocity = {};
    fighter.attackerShieldKnockback = {};
    fighter.groundVelocity = 0;
    fighter.groundKnockbackVelocity = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    changeFighterState(world, fighter, "ShieldBreakDown");
}

static void enterDash(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    setFighterCommandFlag(fighter, 0, false);
    const Fix initVel = fighter.facing * attr.dashInitialVelocity;
    if (fxMul(fighter.groundVelocity, fighter.facing * fx(1)) < 0) {
        fighter.groundAccelSecondary = initVel;
    } else {
        fighter.groundAccelSecondary = initVel - fighter.groundVelocity;
    }
    fighter.stickXTiltTimer = 254;
}

static void processDash(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    if (frameInState(fighter) <= 0) {
        return;
    }
    const Fix inputX = fighter.input.frames[0].move.x;
    if (fighterCommandFlag(fighter, 0) && inputX * fighter.facing >= attr.common.runInputThresholdX58) {
        changeFighterState(world, fighter, "Run");
        return;
    }
    Fix accel = fxMul(inputX, attr.dashRunAccelerationA);
    accel += inputX > 0 ? attr.dashRunAccelerationB : -attr.dashRunAccelerationB;
    const Fix target = fxMul(inputX, attr.dashRunTerminalVelocity);
    applyGroundAccelToward(fighter, attr, accel, target, fxMul(attr.grFriction, attr.common.groundFrictionScaleX60));
    if (frameInState(fighter) > currentState(world, fighter).animationLengthFrames) {
        if (inputX * fighter.facing >= attr.common.runInputThresholdX58 && fighterCommandFlag(fighter, 0)) {
            changeFighterState(world, fighter, "Run");
        } else {
            changeFighterState(world, fighter, "Wait");
        }
    }
}

static void processGrounded(World& world, FighterRuntime& fighter, bool alwaysTraction) {
    FighterProperties& attr = props(world, fighter);
    fighter.jumpsUsed = 0;
    const Fix inputX = fighter.input.frames[0].move.x;
    if (alwaysTraction || frameInState(fighter) < fighter.interruptibleFrame || fxAbs(inputX) < fxFromFloat(0.01f)) {
        applyGroundFriction(fighter, attr.grFriction);
    }
}

static void processWalk(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    Fix inputX = fighter.input.frames[0].move.x;
    if (inputX * fighter.facing < attr.common.walkInputThresholdX24) {
        return;
    }

    Fix accel = fxMul(inputX, attr.walkInitVel);
    accel += inputX > 0 ? attr.walkAccel : -attr.walkAccel;
    Fix target = fxMul(inputX, attr.walkMaxVel);
    if (target != 0) {
        Fix ratio = fxDiv(fighter.groundVelocity, target);
        if (ratio > 0 && ratio < fx(1)) {
            accel = fxMul(accel, fxMul(fx(1) - ratio, attr.common.walkAccelScaleX30));
        }
    }
    applyGroundAccelToward(fighter, attr, accel, target, attr.grFriction);
}

static void processRun(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const Fix inputX = fighter.input.frames[0].move.x;
    if (fighter.runDirectTimer > 0) {
        --fighter.runDirectTimer;
    }
    if (fxAbs(inputX) < attr.common.runInputThresholdX58) {
        fighter.runAnimationVelocity = 0;
        updateRunAnimationRate(world, fighter);
        applyGroundFriction(fighter, fxMul(attr.grFriction, attr.common.groundFrictionScaleX60));
        return;
    }
    Fix accel = fxMul(inputX, attr.dashRunAccelerationA);
    accel += inputX > 0 ? attr.dashRunAccelerationB : -attr.dashRunAccelerationB;
    const Fix target = fxMul(inputX, attr.dashRunTerminalVelocity);
    if (target != 0) {
        Fix ratio = fxDiv(fighter.groundVelocity, target);
        if (ratio > 0 && ratio < fx(1)) {
            accel = fxMul(accel, fxMul(fx(1) - ratio, attr.common.runAccelScaleX5C));
        }
    }
    fighter.runAnimationVelocity = fxMul(target, attr.common.animVelocityScaleX440);
    updateRunAnimationRate(world, fighter);
    applyGroundAccelToward(fighter, attr, accel, target, fxMul(attr.grFriction, attr.common.groundFrictionScaleX60));
}

static void enterRun(World& world, FighterRuntime& fighter) {
    (void) world;
    fighter.runAnimationVelocity = fighter.groundVelocity;
}

static void enterRunDirect(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    enterRun(world, fighter);
    fighter.runDirectTimer = attr.common.runDirectFramesX430;
}

static void processRunDirect(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    if (fighter.runDirectTimer > 0) {
        processRun(world, fighter);
    }
    if (fighter.runDirectTimer <= 0) {
        const Fix inputX = fighter.input.frames[0].move.x;
        if (inputX * fighter.facing <= attr.common.turnRunInputThresholdX38) {
            changeFighterState(world, fighter, "TurnRun");
        } else if (inputX * fighter.facing >= attr.common.runInputThresholdX58) {
            changeFighterState(world, fighter, "Run");
        } else if (fxAbs(inputX) < attr.common.runInputThresholdX58) {
            changeFighterState(world, fighter, "RunBrake");
        } else {
            changeFighterState(world, fighter, "Wait");
        }
        return;
    }
}

static void enterRunBrake(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.runBrakeTimer = attr.maxRunBrakeFrames;
    fighter.runBrakeAnimationFrozen = false;
}

static void processAirborne(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const Fix inputX = fighter.input.frames[0].move.x;
    if (inputX != 0) {
        Fix accel = fxMul(inputX, attr.airDriftStickMul);
        accel += inputX > 0 ? attr.aerialDriftBase : -attr.aerialDriftBase;
        const Fix target = fxMul(inputX, attr.airDriftMax);
        if (target != 0 && fxMul(fighter.fighterVelocity.x, accel) >= 0) {
            if (accel > 0 && fighter.fighterVelocity.x + accel > target) {
                accel = -attr.airFriction;
                if (fighter.fighterVelocity.x + accel < target) {
                    accel = target - fighter.fighterVelocity.x;
                }
                if (fighter.fighterVelocity.x + accel > attr.airMaxHorizontalVelocity) {
                    accel = attr.airMaxHorizontalVelocity - fighter.fighterVelocity.x;
                }
            } else if (accel < 0 && fighter.fighterVelocity.x + accel < target) {
                accel = attr.airFriction;
                if (fighter.fighterVelocity.x + accel > target) {
                    accel = target - fighter.fighterVelocity.x;
                }
                if (fighter.fighterVelocity.x + accel < -attr.airMaxHorizontalVelocity) {
                    accel = -attr.airMaxHorizontalVelocity - fighter.fighterVelocity.x;
                }
            }
        }
        fighter.fighterVelocity.x += accel;
    } else {
        fighter.fighterVelocity.x = fxApproach(fighter.fighterVelocity.x, 0, attr.airFriction);
    }

    const Fix terminal = fighterFlag(fighter, 12) ? attr.fastFallTerminalVelocity : attr.terminalVel;
    fighter.fighterVelocity.y = std::max(fighter.fighterVelocity.y - attr.grav, -terminal);
}

static void processDamageAirborne(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const Fix terminal = fighterFlag(fighter, 12) ? attr.fastFallTerminalVelocity : attr.terminalVel;
    fighter.fighterVelocity.y = std::max(fighter.fighterVelocity.y - attr.grav, -terminal);
}

static void processFallSpecial(World& world, FighterRuntime& fighter) {
    processAirborne(world, fighter);
    fastfallCheck(world, fighter);
}

static void enterPass(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.floorSkipSegment = fighter.groundSegment;
    fighter.grounded = false;
    fighter.groundSegment = -1;
    lockFighterEcb(fighter, 10);
    fighter.fighterVelocity.x = fighter.groundVelocity;
    fighter.fighterVelocity.y = attr.common.platformDropInitialVelocityX46C;
    fighter.groundVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.platformDropTimer = 0;
    fighter.stickYTiltTimer = 254;
    fighter.ledgeCooldown = attr.common.platformDropAnimationFramesX470;
}

static void fastfallCheck(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    if (!fighterFlag(fighter, 12) && fighter.fighterVelocity.y <= 0 &&
        fighter.input.frames[0].move.y <= -attr.common.fastfallStickThresholdX88 &&
        fighter.stickYTiltTimer < attr.common.fastfallStickWindowX8C)
    {
        fighter.fighterVelocity.y = -attr.fastFallTerminalVelocity;
        fighter.stickYTiltTimer = 254;
        setFighterFlag(fighter, 12, true);
    }
}

static void processJumpSquat(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    applyGroundFriction(fighter, attr.grFriction);
    if (frameInState(fighter) < attr.jumpStartupLag) {
        return;
    }

    fighter.grounded = false;
    fighter.jumpsUsed = 1;
    lockFighterEcb(fighter, 10);
    setFighterFlag(fighter, 12, false);
    const bool fullHop = fighter.input.down(ButtonJump) ||
                         fighter.input.frames[0].move.y >= attr.common.tapJumpReleaseThresholdX7C;
    fighter.fighterVelocity.y = fullHop ? attr.jumpVInitialVelocity : attr.hopVInitialVelocity;
    fighter.fighterVelocity.x = fxMul(fxMul(fighter.groundNormal.y, fighter.groundVelocity), attr.groundToAirJumpMomentumMultiplier);

    const Fix hInit = fxMul(fighter.input.frames[0].move.x, attr.jumpHInitialVelocity);
    fighter.fighterVelocity.x += hInit;
    fighter.fighterVelocity.x = std::clamp(fighter.fighterVelocity.x, -attr.jumpHMaxVelocity, attr.jumpHMaxVelocity);
    changeFighterState(world, fighter, fighter.input.frames[0].move.x * fighter.facing > -attr.common.jumpBackwardThresholdX78 ? "JumpF" : "JumpB");
}

static void enterAirJump(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.grounded = false;
    fighter.groundSegment = -1;
    lockFighterEcb(fighter, 10);
    fighter.jumpsUsed = std::min(fighter.jumpsUsed + 1, attr.maxJumps);
    fighter.fighterVelocity.x = fxMul(fighter.input.frames[0].move.x, attr.airJumpHMultiplier);
    fighter.fighterVelocity.y = fxMul(attr.jumpVInitialVelocity, attr.airJumpVMultiplier);
    fighter.knockbackVelocity = {};
    fighter.attackerShieldKnockback = {};
    fighter.groundVelocity = 0;
    fighter.groundKnockbackVelocity = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.stickYTiltTimer = 254;
    setFighterFlag(fighter, 12, false);
}

static void enterEscapeAir(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const Vec2 stick = fighter.input.frames[0].move;
    if (fxAbs(stick.x) < attr.common.escapeAirDeadzoneX32C.x &&
        fxAbs(stick.y) < attr.common.escapeAirDeadzoneX32C.y)
    {
        fighter.fighterVelocity = {};
    } else {
        const float x = fxToFloat(stick.x);
        const float y = fxToFloat(stick.y);
        const float len = std::sqrt(x * x + y * y);
        if (len > 0.0001f) {
            const float force = fxToFloat(attr.common.escapeAirForceX338);
            fighter.fighterVelocity.x = fxFromFloat((x / len) * force);
            fighter.fighterVelocity.y = fxFromFloat((y / len) * force);
        } else {
            fighter.fighterVelocity = {};
        }
    }
    fighter.knockbackVelocity = {};
    fighter.attackerShieldKnockback = {};
    fighter.groundVelocity = 0;
    fighter.groundKnockbackVelocity = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    setFighterFlag(fighter, 12, false);
}

static void enterPassiveWallJump(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const int wallSide = fighter.wallContactSide == 0 ? -fighter.facing : fighter.wallContactSide;
    fighter.grounded = false;
    fighter.groundSegment = -1;
    fighter.facing = -wallSide;
    fighter.fighterVelocity = {};
    fighter.knockbackVelocity = {};
    fighter.attackerShieldKnockback = {};
    fighter.groundKnockbackVelocity = 0;
    fighter.groundVelocity = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.wallContactSide = 0;
    fighter.wallContactTimer = 254;
    fighter.stickXTiltTimer = 254;
    fighter.stickYTiltTimer = 254;
    fighter.wallJumpsUsed = std::min(fighter.wallJumpsUsed + 1, 255);
    setFighterFlag(fighter, 12, false);
}

static void processPassiveWallJump(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    if (frameInState(fighter) == attr.common.wallJumpStartupX774) {
        fighter.fighterVelocity.x = fighter.facing * attr.wallJumpHorizontalVelocity;
        fighter.fighterVelocity.y = attr.wallJumpVerticalVelocity;
        if (fighter.wallJumpsUsed > 1) {
            for (int i = 1; i < fighter.wallJumpsUsed; ++i) {
                fighter.fighterVelocity.y = fxMul(fighter.fighterVelocity.y, attr.common.passiveWallVelYBaseX778);
            }
        }
        return;
    }
    if (frameInState(fighter) > attr.common.wallJumpStartupX774) {
        processAirborne(world, fighter);
    }
}

static void processEscapeAir(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    if (fighterCommandFlag(fighter, 0)) {
        processAirborne(world, fighter);
        fastfallCheck(world, fighter);
        return;
    }
    fighter.fighterVelocity.x = fxMul(fighter.fighterVelocity.x, attr.common.escapeAirDecayX33C);
    fighter.fighterVelocity.y = fxMul(fighter.fighterVelocity.y, attr.common.escapeAirDecayX33C);
}

static void enterTurn(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const Fix x = fighter.input.frames[0].move.x;
    const bool smashTurn = x * fighter.facing <= -attr.common.dashInputThresholdX3C &&
                           fighter.stickXTiltTimer < attr.common.dashStickWindowX40;
    fighter.turnFacingAfter = -fighter.facing;
    fighter.turnFramesToChangeDirection = smashTurn ? 0 : attr.framesToChangeDirectionOnStandingTurn;
    fighter.turnHasTurned = false;
    fighter.turnJustTurned = false;
    fighter.turnDashBuffered = smashTurn;
}

static void enterTurnRun(World&, FighterRuntime& fighter) {
    fighter.turnRunInitialFacing = fighter.facing;
    fighter.turnFacingAfter = -fighter.facing;
    fighter.turnHasTurned = false;
    fighter.turnJustTurned = false;
}

static void processTurn(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    if (frameInState(fighter) <= 0) {
        return;
    }

    const int facingAfterTurn = fighter.turnFacingAfter == 0 ? -fighter.facing : fighter.turnFacingAfter;
    if (!fighter.turnHasTurned &&
        fighter.input.frames[0].move.x * facingAfterTurn >= attr.common.dashInputThresholdX3C &&
        fighter.stickXTiltTimer < attr.common.dashStickWindowX40)
    {
        fighter.turnDashBuffered = true;
    }

    if (!fighter.turnHasTurned && frameInState(fighter) >= fighter.turnFramesToChangeDirection) {
        fighter.facing = facingAfterTurn;
        fighter.turnHasTurned = true;
        fighter.turnJustTurned = true;
    }

    if (fighter.turnJustTurned &&
        fighter.turnDashBuffered &&
        fighter.input.frames[0].move.x * facingAfterTurn >= attr.common.dashInputThresholdX3C)
    {
        changeFighterState(world, fighter, "Dash");
        return;
    }

    if (fighter.turnJustTurned) {
        fighter.turnJustTurned = false;
    }

    Fix friction = attr.grFriction;
    if (fxAbs(fighter.groundVelocity) > attr.walkMaxVel) {
        friction = fxMul(friction, attr.common.turnFrictionScaleAboveWalkMaxX6C);
    }
    applyGroundFriction(fighter, friction);
}

static void processLanding(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    Fix friction = attr.grFriction;
    if (fxAbs(fighter.groundVelocity) > attr.walkMaxVel) {
        friction = fxMul(friction, attr.common.turnFrictionScaleAboveWalkMaxX6C);
    }
    applyGroundFriction(fighter, friction);
}

static bool canDropThroughCurrentFloor(const World& world, const FighterRuntime& fighter) {
    if (!fighter.grounded || fighter.groundSegment < 0 ||
        fighter.groundSegment >= static_cast<int>(world.stage.segments.size()))
    {
        return false;
    }
    return world.stage.segments[static_cast<size_t>(fighter.groundSegment)].type == SegmentType::Semisolid;
}

static void processSquat(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    processLanding(world, fighter);

    if (fighter.platformDropTimer > 0) {
        --fighter.platformDropTimer;
        if (fighter.platformDropTimer == 0 && canDropThroughCurrentFloor(world, fighter)) {
            changeFighterState(world, fighter, "Pass");
        }
        return;
    }

    if (canDropThroughCurrentFloor(world, fighter) &&
        fighter.input.frames[0].move.y <= -attr.common.platformDropStickThresholdX464 &&
        fighter.stickYTiltTimer < attr.common.platformDropStickWindowX468)
    {
        fighter.platformDropTimer = attr.common.platformDropAnimationFramesX470;
    }
}

static void processRunBrake(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    if (fighterCommandFlag(fighter, 1)) {
        if (!fighter.runBrakeAnimationFrozen) {
            if (fxAbs(fighter.groundVelocity) >= attr.common.runBrakeAnimFreezeVelocityX42C) {
                fighter.animationRate = 0;
                fighter.runBrakeAnimationFrozen = true;
            }
        } else if (fxAbs(fighter.groundVelocity) <= attr.common.runBrakeAnimFreezeVelocityX42C) {
            fighter.animationRate = fx(1);
            fighter.runBrakeAnimationFrozen = false;
            setFighterCommandFlag(fighter, 1, false);
        }
    }
    if (fighter.runBrakeTimer > 0) {
        --fighter.runBrakeTimer;
        if (fighter.runBrakeTimer <= 0) {
            changeFighterState(world, fighter, "Wait");
            return;
        }
    }
    if (fighterCommandFlag(fighter, 0) &&
        fighter.input.frames[0].move.x * fighter.facing <= attr.common.turnRunInputThresholdX38)
    {
        const Fix animationStart = fighter.animationFrame;
        changeFighterState(world, fighter, "TurnRun");
        fighter.animationFrame = animationStart;
        return;
    }
    applyGroundFriction(fighter, fxMul(attr.grFriction, attr.common.groundFrictionScaleX60));
}

static void processTurnRun(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const int originalFacing = fighter.turnRunInitialFacing == 0 ? fighter.facing : fighter.turnRunInitialFacing;
    if (fighterCommandFlag(fighter, 1) && !fighter.turnHasTurned) {
        if (fighter.animationRate != 0) {
            fighter.animationRate = 0;
        } else if (fighter.groundVelocity * originalFacing <= fxFromFloat(0.01f)) {
            fighter.animationRate = fx(1);
            setFighterCommandFlag(fighter, 1, false);
            fighter.facing = -originalFacing;
            fighter.turnHasTurned = true;
        }
    }

    if (frameInState(fighter) >= currentState(world, fighter).animationLengthFrames) {
        if (fighter.input.frames[0].move.x * fighter.facing >= attr.common.runInputThresholdX58) {
            changeFighterState(world, fighter, "Run", 0, kDisableAnimationBlendFrames);
            fighter.runDirectTimer = attr.common.runDirectFramesX430;
        } else {
            changeFighterState(world, fighter, "Wait", 0, kDisableAnimationBlendFrames);
        }
        return;
    }

    const Fix inputX = fighter.input.frames[0].move.x;
    Fix accel = 0;
    Fix target = 0;
    if (fxAbs(inputX) >= attr.common.runInputThresholdX58) {
        accel = fxMul(inputX, attr.dashRunAccelerationA);
        accel += inputX > 0 ? attr.dashRunAccelerationB : -attr.dashRunAccelerationB;
        target = fxMul(inputX, attr.dashRunTerminalVelocity);
    }

    const Fix friction = fxMul(attr.grFriction, attr.common.groundFrictionScaleX60);
    if (target == 0 || fxMul(originalFacing * fx(1), accel) >= 0) {
        applyGroundFriction(fighter, friction);
    } else {
        if (accel > 0 && fighter.groundVelocity + accel > target) {
            accel -= friction;
            if (fighter.groundVelocity + accel < target) {
                accel = target - fighter.groundVelocity;
            }
        } else if (accel < 0 && fighter.groundVelocity + accel < target) {
            accel += friction;
            if (fighter.groundVelocity + accel > target) {
                accel = target - fighter.groundVelocity;
            }
        }
        fighter.groundAccel = accel;
    }
}

static void regularAirborne(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.fighterVelocity.x = std::clamp(fighter.fighterVelocity.x, -attr.airDriftMax, attr.airDriftMax);
    fighter.groundVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.jumpsUsed = std::max(1, fighter.jumpsUsed);
    lockFighterEcb(fighter, 10);
    changeFighterState(world, fighter, "Fall");
}

static void teeterOrAirborne(World& world, FighterRuntime& fighter) {
    if (fighter.runoffSegment < 0 ||
        fighter.runoffSegment >= static_cast<int>(world.stage.segments.size()) ||
        fighter.runoffDirection == 0)
    {
        regularAirborne(world, fighter);
        return;
    }

    const StageSegment& segment = world.stage.segments[static_cast<size_t>(fighter.runoffSegment)];
    const bool useEnd = (fighter.runoffDirection > 0) == (segment.end.x >= segment.start.x);
    const Vec2 edge = useEnd ? segment.end : segment.start;
    fighter.grounded = true;
    fighter.groundSegment = fighter.runoffSegment;
    fighter.position.x = edge.x - fighter.ecb.points[3].x;
    fighter.position.y = edge.y - fighter.ecb.points[3].y;
    fighter.previousPosition = fighter.position;
    fighter.groundNormal = floorNormal(segment);
    fighter.groundVelocity = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.fighterVelocity = {};
    fighter.knockbackVelocity = {};
    fighter.attackerShieldKnockback = {};
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.runoffSegment = -1;
    fighter.runoffDirection = 0;
    changeFighterState(world, fighter, "Ottotto");
}

static Fix segmentFacingEdgeX(const StageSegment& segment, int facing) {
    if (facing > 0) {
        return std::max(segment.start.x, segment.end.x);
    }
    return std::min(segment.start.x, segment.end.x);
}

static void processOttotto(World& world, FighterRuntime& fighter) {
    if (!fighter.grounded ||
        fighter.groundSegment < 0 ||
        fighter.groundSegment >= static_cast<int>(world.stage.segments.size()))
    {
        regularAirborne(world, fighter);
        return;
    }

    const FighterProperties& attr = props(world, fighter);
    const StageSegment& segment = world.stage.segments[static_cast<size_t>(fighter.groundSegment)];
    const Fix edgeX = segmentFacingEdgeX(segment, fighter.facing);
    const Fix bottomX = fighter.position.x + fighter.ecb.points[3].x;
    if (fxAbs(bottomX - edgeX) > attr.common.teeterForwardDistanceX478 + attr.common.teeterBackwardDistanceX47C) {
        changeFighterState(world, fighter, "Wait");
    }
}

static void regularLanding(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.jumpsUsed = 0;
    setFighterFlag(fighter, 12, false);
    fighter.fighterVelocity.y = 0;
    if (fighter.lastLandingVelocityY > attr.noImpactLandingVelocity) {
        changeFighterState(world, fighter, "Wait", 0);
        return;
    }
    changeFighterState(world, fighter, "Landing");
    fighter.interruptibleFrame = attr.normalLandingLag;
    fighter.stateAnimationLengthOverride = 0;
}

static bool lCancelInputActive(const FighterRuntime& fighter, int window) {
    if (window <= 0) {
        return false;
    }
    const int maxAge = std::min(window - 1, InputBuffer::kSize - 2);
    for (int age = 0; age <= maxAge; ++age) {
        const InputFrame& currentFrame = fighter.input.frames[static_cast<size_t>(age)];
        const InputFrame& previousFrame = fighter.input.frames[static_cast<size_t>(age + 1)];
        const bool current = (currentFrame.buttons & (ButtonShield | ButtonGrab)) != 0 ||
                             currentFrame.shieldAnalog > 0;
        const bool previous = (previousFrame.buttons & (ButtonShield | ButtonGrab)) != 0 ||
                              previousFrame.shieldAnalog > 0;
        if (current && !previous) {
            return true;
        }
    }
    return false;
}

static int lCancelAdjustedLandingLag(const FighterRuntime& fighter, const FighterProperties& attr, int lag) {
    if (!lCancelInputActive(fighter, attr.common.lCancelInputWindowXE4) ||
        attr.common.lCancelLandingLagDivisorXE8 <= 0)
    {
        return lag;
    }
    return std::max(1, static_cast<int>(fxToFloat(fxDiv(fx(lag), attr.common.lCancelLandingLagDivisorXE8))));
}

static void aerialLandingAttack(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    if (!fighterCommandFlag(fighter, 0)) {
        regularLanding(world, fighter);
        return;
    }
    fighter.jumpsUsed = 0;
    setFighterFlag(fighter, 12, false);
    fighter.fighterVelocity.y = 0;
    const std::string& stateName = currentState(world, fighter).name;
    if (stateName == "AirAttackF") {
        changeFighterState(world, fighter, "LandingAirF", lCancelAdjustedLandingLag(fighter, attr, attr.fairLandingLag));
    } else if (stateName == "AirAttackB") {
        changeFighterState(world, fighter, "LandingAirB", lCancelAdjustedLandingLag(fighter, attr, attr.bairLandingLag));
    } else if (stateName == "AirAttackHi") {
        changeFighterState(world, fighter, "LandingAirHi", lCancelAdjustedLandingLag(fighter, attr, attr.uairLandingLag));
    } else if (stateName == "AirAttackLw") {
        changeFighterState(world, fighter, "LandingAirLw", lCancelAdjustedLandingLag(fighter, attr, attr.dairLandingLag));
    } else {
        changeFighterState(world, fighter, "LandingAirN", lCancelAdjustedLandingLag(fighter, attr, attr.nairLandingLag));
    }
}

static void processCliffJump(World& world, FighterRuntime& fighter) {
    maintainLedge(world, fighter);
    if (currentState(world, fighter).name != "CliffJump") {
        return;
    }
    if (frameInState(fighter) >= currentState(world, fighter).animationLengthFrames - 1) {
        changeFighterState(world, fighter, "CliffJumpAir");
    }
}

static void escapeAirLanding(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.jumpsUsed = 0;
    setFighterFlag(fighter, 12, false);
    fighter.fighterVelocity.y = 0;
    changeFighterState(world, fighter, "LandingFallSpecial", attr.common.landingFallSpecialLagX344);
}

static void processDamage(World& world, FighterRuntime& fighter) {
    if (fighter.hitstun > 0) {
        if (fighter.grounded) {
            applyGroundFriction(fighter, props(world, fighter).grFriction);
        } else {
            processDamageAirborne(world, fighter);
        }
        return;
    }

    if (fighter.grounded) {
        changeFighterState(world, fighter, "Wait");
        return;
    }
    changeFighterState(world, fighter, "DamageFall");
}

static void processDamageFly(World& world, FighterRuntime& fighter) {
    if (fighter.hitstun <= 0) {
        changeFighterState(world, fighter, "DamageFall");
        return;
    }
    processDamageAirborne(world, fighter);
}

static void processDamageFall(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    if (fxAbs(fighter.input.frames[0].move.x) >= attr.common.damageFallStickThresholdX210 &&
        fighter.stickXTiltTimer < attr.common.damageFallStickWindowX214)
    {
        fighter.damageTumble = false;
        changeFighterState(world, fighter, "Fall");
        return;
    }
    processDamageAirborne(world, fighter);
    fastfallCheck(world, fighter);
}

static bool recentTechInput(const FighterRuntime& fighter, int window) {
    const int maxAge = std::min(std::max(0, window - 1), InputBuffer::kSize - 2);
    for (int age = 0; age <= maxAge; ++age) {
        const InputFrame& current = fighter.input.frames[static_cast<size_t>(age)];
        const InputFrame& previous = fighter.input.frames[static_cast<size_t>(age + 1)];
        const bool currentPress = (current.buttons & (ButtonShield | ButtonGrab)) != 0 || current.shieldAnalog > 0;
        const bool previousPress = (previous.buttons & (ButtonShield | ButtonGrab)) != 0 || previous.shieldAnalog > 0;
        if (currentPress && !previousPress) {
            return true;
        }
    }
    return false;
}

static void enterClearDamage(World&, FighterRuntime& fighter) {
    fighter.hitstun = 0;
    fighter.damageTumble = false;
    fighter.damageSurfaceTimer = 0;
    fighter.damageHitboxOwner = -1;
    fighter.thrownHitboxOwner = -1;
    fighter.fighterVelocity = {};
    fighter.knockbackVelocity = {};
    fighter.knockbackDecay = {};
    fighter.attackerShieldKnockback = {};
    fighter.groundVelocity = 0;
    fighter.groundKnockbackVelocity = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
}

static void damageFlyLanding(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.fighterVelocity.y = 0;
    if (fighter.damageTumble && fighter.hitstun > 0) {
        if (recentTechInput(fighter, attr.common.passiveInputWindowX250)) {
            const Fix x = fighter.input.frames[0].move.x * fighter.facing;
            if (x >= attr.common.passiveStandStickThresholdX254) {
                changeFighterState(world, fighter, "PassiveStandF");
            } else if (x <= -attr.common.passiveStandStickThresholdX254) {
                changeFighterState(world, fighter, "PassiveStandB");
            } else {
                changeFighterState(world, fighter, "Passive");
            }
            return;
        }
        changeFighterState(world, fighter, "DownBoundU");
        return;
    }
    regularLanding(world, fighter);
}

static void damageFallLanding(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.fighterVelocity.y = 0;
    if (recentTechInput(fighter, attr.common.passiveInputWindowX250)) {
        const Fix x = fighter.input.frames[0].move.x * fighter.facing;
        if (x >= attr.common.passiveStandStickThresholdX254) {
            changeFighterState(world, fighter, "PassiveStandF");
        } else if (x <= -attr.common.passiveStandStickThresholdX254) {
            changeFighterState(world, fighter, "PassiveStandB");
        } else {
            changeFighterState(world, fighter, "Passive");
        }
        return;
    }
    changeFighterState(world, fighter, "DownBoundU");
}

static bool faceUpDownState(const std::string& name) {
    return name == "DownBoundU" || name == "DownWaitU" || name == "DownDamageU" ||
           name == "DownStandU" || name == "DownAttackU" || name == "DownForwardU" ||
           name == "DownBackU" || name == "DownSpotU";
}

static const char* downVariantState(const std::string& stateName, const char* up, const char* down) {
    return faceUpDownState(stateName) ? up : down;
}

static bool downRollInput(const FighterRuntime& fighter, const FighterProperties& attr, Fix& stickX) {
    const InputFrame& input = fighter.input.frames[0];
    if (fxAbs(input.cStick.x) >= attr.common.downRollStickThresholdX248) {
        stickX = input.cStick.x;
        return true;
    }
    if (fxAbs(input.move.x) >= attr.common.downRollStickThresholdX248 &&
        fxAbs(input.move.y) <= fxAbs(input.move.x))
    {
        stickX = input.move.x;
        return true;
    }
    return false;
}

static void enterDownWait(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.downWaitTimer = attr.common.downWaitAutoStandFramesX424;
    fighter.fighterVelocity = {};
    fighter.knockbackVelocity = {};
    fighter.knockbackDecay = {};
    fighter.groundKnockbackVelocity = 0;
    fighter.attackerShieldKnockback = {};
    fighter.groundVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
}

static void processDownWait(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const std::string& stateName = currentState(world, fighter).name;
    applyGroundFriction(fighter, attr.grFriction);

    if (fighter.input.justPressed(ButtonAttack | ButtonSpecial)) {
        changeFighterState(world, fighter, downVariantState(stateName, "DownAttackU", "DownAttackD"));
        return;
    }

    Fix stickX = 0;
    if (downRollInput(fighter, attr, stickX)) {
        const bool forward = fxMul(stickX, fx(fighter.facing)) >= 0;
        changeFighterState(world, fighter, downVariantState(
            stateName,
            forward ? "DownForwardU" : "DownBackU",
            forward ? "DownForwardD" : "DownBackD"));
        return;
    }

    const InputFrame& input = fighter.input.frames[0];
    const bool triggerStand = fighter.input.justPressed(ButtonShield | ButtonGrab) ||
        input.shieldAnalog > 0 ||
        input.move.y >= attr.common.downStandStickThresholdX244;
    if (triggerStand) {
        changeFighterState(world, fighter, downVariantState(stateName, "DownStandU", "DownStandD"));
        return;
    }

    if (fighter.downWaitTimer > 0) {
        --fighter.downWaitTimer;
    }
    if (fighter.downWaitTimer <= 0) {
        changeFighterState(world, fighter, downVariantState(stateName, "DownStandU", "DownStandD"));
    }
}

static void processDownGetup(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    applyGroundFriction(fighter, attr.grFriction);
}

static void processDamageSurface(World& world, FighterRuntime& fighter) {
    ++fighter.damageSurfaceTimer;
    if (fighter.hitstun > 0) {
        processAirborne(world, fighter);
        return;
    }
    if (fighter.damageSurfaceTimer > 5) {
        changeFighterState(world, fighter, "DamageFall");
    }
}

static Fix animationFrameForState(const FighterRuntime& fighter, const FighterState& state, const AnimationClip& clip) {
    Fix frame = fighter.animationFrame;
    if (state.loopAnimation && clip.frameCount > 0) {
        const int loopFrames = std::max(1, static_cast<int>(std::round(fxToFloat(clip.frameCount))));
        frame %= fx(loopFrames);
    } else if (fighter.stateAnimationLengthOverride > 0 && clip.frameCount > 0) {
        frame = fxMul(clip.frameCount, fxDiv(fx(frameInState(fighter)), fx(fighter.stateAnimationLengthOverride)));
    }
    return frame;
}

static Vec3 scaleTransN(Vec3 transN, Fix scale) {
    transN.x = fxMul(transN.x, scale);
    transN.y = fxMul(transN.y, scale);
    transN.z = fxMul(transN.z, scale);
    return transN;
}

static bool ledgeActionTransN(const World& world, const FighterRuntime& fighter, Vec3& transN) {
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const FighterState& state = currentState(world, fighter);
    if (!def.hasHsdAsset || !def.hsdAsset || state.animationActionIndex < 0) {
        return false;
    }
    if (const AnimationClip* clip = findClipByActionIndex(*def.hsdAsset, state.animationActionIndex)) {
        const AnimationPose pose = evaluateClip(def.hsdAsset->skeleton, *clip, animationFrameForState(fighter, state, *clip));
        if (pose.joints.size() > 1) {
            transN = scaleTransN(pose.joints[1].translation, def.properties.modelScale);
            return true;
        }
    }
    return false;
}

static bool positionFromLedgeTransN(World& world, FighterRuntime& fighter, Vec3* outTransN = nullptr) {
    if (fighter.grabbedLedge < 0 || fighter.grabbedLedge >= static_cast<int>(world.stage.ledges.size())) {
        return false;
    }
    const StageLedge& ledge = world.stage.ledges[static_cast<size_t>(fighter.grabbedLedge)];
    Vec3 transN{};
    if (!ledgeActionTransN(world, fighter, transN)) {
        return false;
    }
    fighter.position.x = ledge.position.x + fighter.facing * transN.z;
    fighter.position.y = ledge.position.y + transN.y;
    if (outTransN) {
        *outTransN = transN;
    }
    return true;
}

static void maintainLedge(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    if (fighter.grabbedLedge < 0 || fighter.grabbedLedge >= static_cast<int>(world.stage.ledges.size())) {
        fighter.grabbedLedge = -1;
        fighter.ledgeCooldown = attr.common.ledgeCooldownX498;
        changeFighterState(world, fighter, "Fall");
        return;
    }

    const std::string& stateName = currentState(world, fighter).name;
    if ((stateName == "CliffCatch" || stateName == "CliffWait") &&
        fighter.input.frames[0].move.y <= -attr.common.ledgeNoGrabDownThresholdX480)
    {
        fighter.grabbedLedge = -1;
        fighter.ledgeCooldown = attr.common.ledgeCooldownX498;
        fighter.fighterVelocity.y = -fxFromFloat(0.2f);
        changeFighterState(world, fighter, "Fall");
        return;
    }

    const StageLedge& ledge = world.stage.ledges[static_cast<size_t>(fighter.grabbedLedge)];
    fighter.grounded = false;
    fighter.groundSegment = -1;
    fighter.facing = -ledge.direction;
    fighter.fighterVelocity = {};
    fighter.knockbackVelocity = {};
    fighter.attackerShieldKnockback = {};
    if (stateName == "CliffWait" &&
        fxAbs(fighter.input.frames[0].move.x) < attr.common.cliffOptionStickThresholdX494 &&
        fxAbs(fighter.input.frames[0].move.y) < attr.common.cliffOptionStickThresholdX494 &&
        fxAbs(fighter.input.frames[0].cStick.x) < attr.common.cliffOptionStickThresholdX494 &&
        fxAbs(fighter.input.frames[0].cStick.y) < attr.common.cliffOptionStickThresholdX494)
    {
        fighter.ledgeActionReady = true;
    }
    if (positionFromLedgeTransN(world, fighter)) {
        return;
    }
    fighter.position.x = ledge.position.x + ledge.direction * attr.ledgeHangX;
    fighter.position.y = ledge.position.y + attr.ledgeHangY;
}

static void releaseLedge(FighterRuntime& fighter, const FighterProperties& attr) {
    fighter.grabbedLedge = -1;
    fighter.grounded = false;
    fighter.groundSegment = -1;
    fighter.groundVelocity = 0;
    fighter.groundKnockbackVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.ledgeCooldown = attr.common.ledgeCooldownX498;
}

static void finishLedgeAction(World& world, FighterRuntime& fighter, Fix inwardOffset) {
    FighterProperties& attr = props(world, fighter);
    if (fighter.grabbedLedge < 0 || fighter.grabbedLedge >= static_cast<int>(world.stage.ledges.size())) {
        releaseLedge(fighter, attr);
        changeFighterState(world, fighter, "Fall");
        return;
    }

    const StageLedge& ledge = world.stage.ledges[static_cast<size_t>(fighter.grabbedLedge)];
    fighter.facing = -ledge.direction;
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    bool usedImportedTransN = false;
    if (def.hasHsdAsset && def.hsdAsset && currentState(world, fighter).animationActionIndex >= 0) {
        if (const AnimationClip* clip = findClipByActionIndex(*def.hsdAsset, currentState(world, fighter).animationActionIndex)) {
            const AnimationPose pose = evaluateClip(def.hsdAsset->skeleton, *clip, clip->frameCount);
            if (pose.joints.size() > 1) {
                const Vec3 transN = scaleTransN(pose.joints[1].translation, def.properties.modelScale);
                fighter.position.x = ledge.position.x + fighter.facing * transN.z;
                fighter.position.y = ledge.position.y + transN.y;
                usedImportedTransN = true;
            }
        }
    }
    if (!usedImportedTransN) {
        fighter.position.x = ledge.position.x - ledge.direction * inwardOffset;
        fighter.position.y = ledge.position.y;
    }
    fighter.fighterVelocity = {};
    fighter.knockbackVelocity = {};
    fighter.attackerShieldKnockback = {};
    fighter.groundVelocity = 0;
    fighter.groundKnockbackVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.grounded = true;
    fighter.groundSegment = ledge.segmentIndex;
    unlockFighterEcb(fighter);
    fighter.grabbedLedge = -1;
    fighter.ledgeCooldown = attr.common.ledgeCooldownX498;
    changeFighterState(world, fighter, "Wait");
}

static void processCliffClimb(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    if (!fighter.grounded && (fighter.grabbedLedge < 0 || fighter.grabbedLedge >= static_cast<int>(world.stage.ledges.size()))) {
        releaseLedge(fighter, attr);
        changeFighterState(world, fighter, "Fall");
        return;
    }
    fighter.fighterVelocity = {};
    fighter.knockbackVelocity = {};
    fighter.attackerShieldKnockback = {};
    fighter.groundKnockbackVelocity = 0;
    if (!fighter.grounded) {
        const StageLedge& ledge = world.stage.ledges[static_cast<size_t>(fighter.grabbedLedge)];
        fighter.facing = -ledge.direction;
        Vec3 transN{};
        if (positionFromLedgeTransN(world, fighter, &transN) && transN.z >= 0 && transN.y >= 0) {
            fighter.grounded = true;
            fighter.groundSegment = ledge.segmentIndex;
            unlockFighterEcb(fighter);
            fighter.grabbedLedge = -1;
            fighter.ledgeCooldown = attr.common.ledgeCooldownX498;
        } else {
            fighter.groundSegment = -1;
        }
    }
    if (currentState(world, fighter).name != "CliffClimb") {
        return;
    }
    if (frameInState(fighter) >= currentState(world, fighter).animationLengthFrames - 1) {
        if (fighter.grounded) {
            changeFighterState(world, fighter, "Wait");
        } else {
            finishLedgeAction(world, fighter, attr.ledgeClimbX);
        }
    }
}

static void processCliffEscape(World& world, FighterRuntime& fighter) {
    maintainLedge(world, fighter);
    if (currentState(world, fighter).name != "CliffEscape") {
        return;
    }
    if (frameInState(fighter) >= currentState(world, fighter).animationLengthFrames - 1) {
        finishLedgeAction(world, fighter, props(world, fighter).ledgeEscapeX);
    }
}

static void processCliffAttack(World& world, FighterRuntime& fighter) {
    maintainLedge(world, fighter);
    if (currentState(world, fighter).name != "CliffAttack") {
        return;
    }
    if (frameInState(fighter) >= currentState(world, fighter).animationLengthFrames - 1) {
        finishLedgeAction(world, fighter, props(world, fighter).ledgeClimbX);
    }
}

static void enterCliffDrop(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const int facing = fighter.facing;
    releaseLedge(fighter, attr);
    fighter.fighterVelocity.x = -facing * attr.ledgeDropHorizontalVelocity;
    fighter.fighterVelocity.y = attr.ledgeDropVerticalVelocity;
    changeFighterState(world, fighter, "Fall");
}

static void enterCliffJumpAir(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const int facing = fighter.facing;
    releaseLedge(fighter, attr);
    fighter.fighterVelocity.x = facing * attr.ledgeJumpHorizontalVelocity;
    fighter.fighterVelocity.y = attr.ledgeJumpVerticalVelocity;
    fighter.jumpsUsed = 1;
    setFighterFlag(fighter, 12, false);
}

void runStateFunction(World& world, size_t fighterIndex, const FunctionCall& call) {
    FighterRuntime& fighter = world.fighters[fighterIndex];
    if (call.name == "common_enter") return commonEnter(world, fighter);
    if (call.name == "enter_guard_on") return enterGuardOn(world, fighter);
    if (call.name == "process_guard_on") return processGuardOn(world, fighter);
    if (call.name == "process_guard") return processGuardHeld(world, fighter);
    if (call.name == "process_guard_reflect") return processGuardReflect(world, fighter);
    if (call.name == "process_guard_setoff") return processGuardSetoff(world, fighter);
    if (call.name == "process_catch") return processCatch(world, fighter);
    if (call.name == "process_catch_dash") return processCatchDash(world, fighter);
    if (call.name == "process_catch_pull") return processCatchPull(world, fighter);
    if (call.name == "process_catch_wait") return processCatchWait(world, fighter);
    if (call.name == "process_catch_attack") return processCatchAttack(world, fighter);
    if (call.name == "enter_throw") return enterThrow(world, fighter);
    if (call.name == "process_throw") return processThrow(world, fighter);
    if (call.name == "enter_catch_cut") return enterCatchCut(world, fighter);
    if (call.name == "process_catch_cut") return processCatchCut(world, fighter);
    if (call.name == "enter_capture_cut") return enterCaptureCut(world, fighter);
    if (call.name == "process_capture_cut") return processCaptureCut(world, fighter);
    if (call.name == "enter_capture_jump") return enterCaptureJump(world, fighter);
    if (call.name == "process_capture") return processCapture(world, fighter);
    if (call.name == "process_capture_wait") return processCaptureWait(world, fighter);
    if (call.name == "process_capture_damage") return processCaptureDamage(world, fighter);
    if (call.name == "process_thrown") return processThrown(world, fighter);
    if (call.name == "catch_airborne") return catchAirborne(world, fighter);
    if (call.name == "throw_airborne") return throwAirborne(world, fighter);
    if (call.name == "enter_shield_break_fly") return enterShieldBreakFly(world, fighter);
    if (call.name == "process_shield_break_air") return processShieldBreakAir(world, fighter);
    if (call.name == "shield_break_landing") return shieldBreakLanding(world, fighter);
    if (call.name == "enter_dash") return enterDash(world, fighter);
    if (call.name == "process_dash") return processDash(world, fighter);
    if (call.name == "process_grounded") return processGrounded(world, fighter, call.boolParam);
    if (call.name == "process_walk") return processWalk(world, fighter);
    if (call.name == "process_run") return processRun(world, fighter);
    if (call.name == "enter_run") return enterRun(world, fighter);
    if (call.name == "enter_run_direct") return enterRunDirect(world, fighter);
    if (call.name == "process_run_direct") return processRunDirect(world, fighter);
    if (call.name == "enter_run_brake") return enterRunBrake(world, fighter);
    if (call.name == "process_airborne") return processAirborne(world, fighter);
    if (call.name == "process_fall_special") return processFallSpecial(world, fighter);
    if (call.name == "enter_pass") return enterPass(world, fighter);
    if (call.name == "fastfall_check") return fastfallCheck(world, fighter);
    if (call.name == "process_jump_squat") return processJumpSquat(world, fighter);
    if (call.name == "enter_air_jump") return enterAirJump(world, fighter);
    if (call.name == "enter_escape_air") return enterEscapeAir(world, fighter);
    if (call.name == "enter_passive_wall_jump") return enterPassiveWallJump(world, fighter);
    if (call.name == "process_passive_wall_jump") return processPassiveWallJump(world, fighter);
    if (call.name == "process_escape_air") return processEscapeAir(world, fighter);
    if (call.name == "enter_turn") return enterTurn(world, fighter);
    if (call.name == "enter_turn_run") return enterTurnRun(world, fighter);
    if (call.name == "process_turn") return processTurn(world, fighter);
    if (call.name == "process_landing") return processLanding(world, fighter);
    if (call.name == "process_squat") return processSquat(world, fighter);
    if (call.name == "process_run_brake") return processRunBrake(world, fighter);
    if (call.name == "process_turn_run") return processTurnRun(world, fighter);
    if (call.name == "regular_airborne") return regularAirborne(world, fighter);
    if (call.name == "teeter_or_airborne") return teeterOrAirborne(world, fighter);
    if (call.name == "process_ottotto") return processOttotto(world, fighter);
    if (call.name == "regular_landing") return regularLanding(world, fighter);
    if (call.name == "aerial_landing_attack") return aerialLandingAttack(world, fighter);
    if (call.name == "escape_air_landing") return escapeAirLanding(world, fighter);
    if (call.name == "process_damage") return processDamage(world, fighter);
    if (call.name == "process_damage_fly") return processDamageFly(world, fighter);
    if (call.name == "process_damage_fall") return processDamageFall(world, fighter);
    if (call.name == "damage_fly_landing") return damageFlyLanding(world, fighter);
    if (call.name == "damage_fall_landing") return damageFallLanding(world, fighter);
    if (call.name == "enter_clear_damage") return enterClearDamage(world, fighter);
    if (call.name == "enter_down_wait") return enterDownWait(world, fighter);
    if (call.name == "process_down_wait") return processDownWait(world, fighter);
    if (call.name == "process_down_getup") return processDownGetup(world, fighter);
    if (call.name == "process_damage_surface") return processDamageSurface(world, fighter);
    if (call.name == "maintain_ledge") return maintainLedge(world, fighter);
    if (call.name == "process_cliff_climb") return processCliffClimb(world, fighter);
    if (call.name == "process_cliff_escape") return processCliffEscape(world, fighter);
    if (call.name == "process_cliff_attack") return processCliffAttack(world, fighter);
    if (call.name == "enter_cliff_drop") return enterCliffDrop(world, fighter);
    if (call.name == "process_cliff_jump") return processCliffJump(world, fighter);
    if (call.name == "enter_cliff_jump_air") return enterCliffJumpAir(world, fighter);
}

void runStateFunctions(World& world, size_t fighterIndex, const std::vector<FunctionCall>& calls) {
    for (const FunctionCall& call : calls) {
        runStateFunction(world, fighterIndex, call);
    }
}

} // namespace pf
