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
static void processAirborneFastfall(World& world, FighterRuntime& fighter);
static void regularAirborne(World& world, FighterRuntime& fighter);
static void fastfallCheck(World& world, FighterRuntime& fighter);
static void processLanding(World& world, FighterRuntime& fighter);
static void maintainLedge(World& world, FighterRuntime& fighter);
static void releaseLedge(FighterRuntime& fighter, const FighterProperties& attr);
static bool canDropThroughCurrentFloor(const World& world, const FighterRuntime& fighter);
static bool currentAnimationFinished(World& world, const FighterRuntime& fighter);

static void commonEnter(World& world, FighterRuntime& fighter) {
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    fighter.activeHitboxes.clear();
    fighter.fightersHitThisAction.clear();
    fighter.throwHitboxActive.fill(false);
    const size_t hurtboxCount = def.hasHsdAsset && def.hsdAsset ? def.hsdAsset->hurtboxes.size() : def.hurtboxes.size();
    fighter.hurtboxStates.assign(hurtboxCount, HurtboxState::Normal);
    fighter.bodyCollisionState = HurtboxState::Normal;
    fighter.commandVars.fill(0);
    fighter.commandFlags = 0;
    fighter.throwFlags = 0;
    fighter.jabFollowupEnabled = false;
    fighter.rapidJabEnabled = false;
    fighter.fighterInvisible = false;
    fighter.turnFacingAfter = 0;
    fighter.turnHasTurned = false;
    fighter.turnJustTurned = false;
    fighter.turnDashBuffered = false;
    fighter.turnBufferedButtons = 0;
    fighter.runDirectTimer = 0;
    fighter.runBrakeTimer = 0;
    fighter.runBrakeAnimationFrozen = false;
    fighter.attackDashGrabBufferTimer = 0;
    fighter.attackLw3RepeatQueued = false;
    fighter.ledgeActionReady = false;
    fighter.ledgeWaitTimer = 0;
    fighter.guardMinHoldTimer = 0;
    fighter.guardSetoffTimer = 0;
    fighter.guardCatchDashBufferTimer = 0;
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
    if (frameInState(fighter) > 0 && fighter.guardCatchDashBufferTimer > 0) {
        --fighter.guardCatchDashBufferTimer;
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

static const char* thrownStateForThrow(World& world, const FighterRuntime& thrower, const FighterRuntime& victim, const std::string& name) {
    if (name == "ThrowB") return "ThrownB";
    if (name == "ThrowHi") return "ThrownHi";
    if (name == "ThrowLw") {
        const FighterDefinition& throwerDef = world.fighterDefs[static_cast<size_t>(thrower.fighterDef)];
        const FighterDefinition& victimDef = world.fighterDefs[static_cast<size_t>(victim.fighterDef)];
        const bool bowserThrower = throwerDef.name == "Bowser" || throwerDef.name == "Giga Bowser";
        const bool womenDownThrowVictim = victimDef.name == "Peach" || victimDef.name == "Zelda";
        if (bowserThrower && womenDownThrowVictim) {
            return "ThrownLwWomen";
        }
        return "ThrownLw";
    }
    return "ThrownF";
}

static bool throwHorizontalStickInput(Fix previous, Fix current, const MeleeCommonData& common) {
    return (previous < common.attackS3StickThresholdX98 && current >= common.attackS3StickThresholdX98) ||
        (previous > -common.attackS3StickThresholdX98 && current <= -common.attackS3StickThresholdX98);
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

static const char* catchWaitThrowInput(const FighterRuntime& fighter, const MeleeCommonData& common) {
    if (throwHorizontalStickInput(fighter.input.frames[1].move.x, fighter.input.frames[0].move.x, common)) {
        return fighter.input.frames[0].move.x * fighter.facing > 0 ? "ThrowF" : "ThrowB";
    }
    if (throwHorizontalStickInput(fighter.input.frames[1].cStick.x, fighter.input.frames[0].cStick.x, common)) {
        return fighter.input.frames[0].cStick.x * fighter.facing > 0 ? "ThrowF" : "ThrowB";
    }
    if (throwHighInput(fighter, common)) {
        return "ThrowHi";
    }
    if (throwLowInput(fighter, common)) {
        return "ThrowLw";
    }
    return nullptr;
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
    if (fighterThrowFlag(fighter, 3) || frameInState(fighter) >= currentState(world, fighter).animationLengthFrames) {
        fighter.groundVelocity = 0;
        setFighterThrowFlag(fighter, 3, false);
        changeFighterState(world, fighter, "CatchWait");
    }
}

static void enterAttackDash(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.attackDashGrabBufferTimer = attr.common.attackDashGrabBufferFramesX68;
}

static void processAttackDash(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const bool animationPhysicsWillRun =
        currentState(world, fighter).useAnimPhysics && fighter.grounded && frameInState(fighter) > 1;
    if (!animationPhysicsWillRun) {
        applyGroundFriction(fighter, fxMul(attr.grFriction, attr.common.attackDashFrictionScaleX50));
    }
    if (frameInState(fighter) > 0 && fighter.attackDashGrabBufferTimer > 0) {
        --fighter.attackDashGrabBufferTimer;
    }
}

static void enterAttack11(World&, FighterRuntime& fighter) {
    fighter.attackRapidInputCount = 0;
}

static void enterAttack100Start(World&, FighterRuntime& fighter) {
    fighter.attack100CanEnd = false;
    fighter.attack100ContinuePressed = false;
}

static void processAttack100Loop(World& world, FighterRuntime& fighter) {
    processLanding(world, fighter);
    if (currentState(world, fighter).name != "Attack100Loop") {
        return;
    }
    if (fighter.animationFrame >= 0 && fighter.animationFrame < fighter.animationRate) {
        fighter.attack100CanEnd = true;
        fighter.activeHitboxes.clear();
        fighter.fightersHitThisAction.clear();
    }
    if (fighter.input.justPressed(ButtonAttack) || fighter.input.down(ButtonAttack)) {
        fighter.attack100ContinuePressed = true;
    }
    if (fighterThrowFlag(fighter, 3)) {
        setFighterThrowFlag(fighter, 3, false);
        if (fighter.attack100CanEnd && !fighter.attack100ContinuePressed) {
            changeFighterState(world, fighter, "Attack100End");
            return;
        }
        fighter.attack100ContinuePressed = false;
    }
}

static void enterAttackLw3(World&, FighterRuntime& fighter) {
    fighter.attackLw3RepeatQueued = false;
}

static void processAttackLw3(World& world, FighterRuntime& fighter) {
    processLanding(world, fighter);
    if (currentState(world, fighter).name != "AttackLw3" || frameInState(fighter) <= 0) {
        return;
    }
    if (fighter.input.justPressed(ButtonAttack)) {
        fighter.attackLw3RepeatQueued = true;
    }
    if (fighter.attackLw3RepeatQueued && fighterCommandFlag(fighter, 0)) {
        changeFighterState(world, fighter, "AttackLw3");
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
    if (const char* throwState = catchWaitThrowInput(fighter, attr.common)) {
        changeFighterState(world, fighter, throwState);
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
    const int throwIndex =
        stateName == "ThrowB" ? 1 :
        stateName == "ThrowHi" ? 2 :
        stateName == "ThrowLw" ? 3 :
        0;
    const FighterProperties& attackerAttr = props(world, fighter);
    const bool weightIndependentThrow = (attackerAttr.weightIndependentThrowsMask & (1 << throwIndex)) != 0;
    const Fix weightScale = fxMul(victimAttr.weight, attackerAttr.common.throwWeightAnimationScaleX37C);
    const Fix animRate = weightIndependentThrow || weightScale <= 0 ? fx(1) : fxDiv(fx(1), weightScale);
    fighter.animationRate = animRate;
    beginMeleeThrowConstraint(world, grabberIndex, static_cast<size_t>(fighter.grabbedFighter));
    changeFighterState(world, victim, thrownStateForThrow(world, fighter, victim, stateName), 0, kDisableAnimationBlendFrames);
    victim.animationRate = animRate;
    victim.grabberFighter = static_cast<int>(grabberIndex);
    const FighterDefinition& attackerDef = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    if (!(attackerDef.name == "Kirby" && throwIndex <= 1)) {
        victim.grabTimer = 0;
    }
    updateMeleeCapturePosition(world, static_cast<size_t>(fighter.grabbedFighter));
}

static void processThrow(World& world, FighterRuntime& fighter) {
    if (fighterThrowFlag(fighter, 4)) {
        fighter.facing *= -1;
        setFighterThrowFlag(fighter, 4, false);
    }
    if (fighterCommandFlag(fighter, 0) && !fighter.throwAnimationFrozen) {
        fighter.throwAnimationFrozen = true;
        setFighterCommandVar(fighter, 0, 0);
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
        changeFighterState(world, fighter, fighter.grounded ? "Wait" : "Fall");
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
        const bool captureCut = currentState(world, fighter).name == "CaptureCut";
        const Fix frictionScale = captureCut
            ? attr.common.captureCutFrictionScaleX36C
            : attr.common.catchCutFrictionScaleX64;
        applyGroundFriction(fighter, fxMul(frictionScale, attr.grFriction));
    } else {
        processAirborneFastfall(world, fighter);
    }
    if (currentAnimationFinished(world, fighter)) {
        changeFighterState(world, fighter, fighter.grounded ? "Wait" : "Fall");
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
    fighter.groundVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.jumpsUsed = 1;
    lockFighterEcb(fighter, 10);
    fighter.fighterVelocity.x = -fighter.facing * attr.common.captureJumpVelocityX374;
    fighter.fighterVelocity.y = attr.common.captureJumpVelocityYx378;
    fighter.grabberFighter = -1;
    fighter.grabbedFighter = -1;
    fighter.captureConstraintActive = false;
    fighter.captureConstraintOffset = {};
    fighter.captureOriginalXRotNTranslation = {};
    fighter.thrownAnimationFreezeActive = false;
}

static void processCaptureJump(World& world, FighterRuntime& fighter) {
    fighter.captureWaitTimer += fx(1);
    processAirborne(world, fighter);
}

static void captureJumpLanding(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.jumpsUsed = 0;
    setFighterFlag(fighter, 12, false);
    fighter.fighterVelocity.y = 0;
    changeFighterState(world, fighter, "Landing");
    fighter.interruptibleFrame = attr.normalLandingLag;
    fighter.stateAnimationLengthOverride = 0;
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
    if (value < -threshold) return -1;
    if (value > threshold) return 1;
    return 0;
}

static bool anyButtonJustPressed(const InputBuffer& input, uint16_t buttons) {
    return ((input.frames[0].buttons & buttons) & ~(input.frames[1].buttons & buttons)) != 0;
}

static bool applyGrabMash(FighterRuntime& fighter, const MeleeCommonData& common, Fix decrement) {
    bool result = false;
    if (anyButtonJustPressed(fighter.input, ButtonAttack | ButtonSpecial | ButtonJump | ButtonShield | ButtonGrab)) {
        fighter.grabTimer -= decrement;
        result = true;
    }
    const int stickX = mashAxis(fighter.input.frames[0].move.x, common.grabMashStickThresholdX308);
    const int stickY = mashAxis(fighter.input.frames[0].move.y, common.grabMashStickThresholdX308);
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
        const std::string& grabberState = currentState(world, grabber).name;
        if (grabberState == "CatchWait" || grabberState == "CatchAttack" || isThrowStateName(grabberState)) {
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
    const MeleeCommonData& common = props(world, fighter).common;
    if (fighter.grabTimer > 0) {
        fighter.grabTimer -= common.captureTimerDecrementX3A4;
        applyGrabMash(fighter, common, common.thrownMashDecrementX3C8);
        if (fighter.grabTimer <= 0) {
            escapeCapture(world, fighter, fighter.captureJumpQueued || fighter.input.frames[0].move.y >= common.tapJumpThresholdX70);
        }
    }
}

static void catchAirborne(World& world, FighterRuntime& fighter) {
    if (isValidFighterIndex(world, fighter.grabbedFighter)) {
        FighterRuntime& victim = world.fighters[static_cast<size_t>(fighter.grabbedFighter)];
        releaseMeleeCaptureConstraint(world, fighterIndexOf(world, fighter), fighter.grabbedFighter, true);
        regularAirborne(world, victim);
    }
    fighter.grabbedFighter = -1;
    regularAirborne(world, fighter);
}

static void throwAirborne(World& world, FighterRuntime& fighter) {
    if (isValidFighterIndex(world, fighter.grabbedFighter)) {
        FighterRuntime& victim = world.fighters[static_cast<size_t>(fighter.grabbedFighter)];
        releaseMeleeCaptureConstraint(world, fighterIndexOf(world, fighter), fighter.grabbedFighter, true);
        regularAirborne(world, victim);
    }
    fighter.grabbedFighter = -1;
    regularAirborne(world, fighter);
}

static void captureCutAirborne(World& world, FighterRuntime& fighter) {
    (void)world;
    fighter.grounded = false;
    fighter.groundSegment = -1;
    fighter.groundVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.jumpsUsed = 1;
    lockFighterEcb(fighter, 10);
}

static void enterShieldBreakFly(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.grounded = false;
    fighter.groundSegment = -1;
    lockFighterEcb(fighter, 10);
    fighter.jumpsUsed = 1;
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

static void enterShieldBreakFall(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.fighterVelocity.x = std::clamp(fighter.fighterVelocity.x, -attr.airDriftMax, attr.airDriftMax);
}

static void processShieldBreakAir(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.fighterVelocity.x = fxApproach(fighter.fighterVelocity.x, 0, attr.airFriction);
    fighter.fighterVelocity.y = std::max(fighter.fighterVelocity.y - attr.grav, -attr.terminalVel);
}

static bool meleeDownBoundFaceUpPose(const World& world, const FighterRuntime& fighter);

static void shieldBreakLanding(World& world, FighterRuntime& fighter) {
    fighter.fighterVelocity.y = 0;
    fighter.knockbackVelocity = {};
    fighter.attackerShieldKnockback = {};
    fighter.groundVelocity = 0;
    fighter.groundKnockbackVelocity = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    changeFighterState(world, fighter, meleeDownBoundFaceUpPose(world, fighter) ? "ShieldBreakDownU" : "ShieldBreakDownD");
}

static void enterDash(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    setFighterCommandVar(fighter, 0, 0);
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
    if (frameInState(fighter) <= 1) {
        return;
    }
    const Fix inputX = fighter.input.frames[0].move.x;
    if (fighterCommandFlag(fighter, 0) && inputX * fighter.facing >= attr.common.runInputThresholdX58) {
        changeFighterState(world, fighter, "Run");
        return;
    }
    // Match ftCo_Dash_Phys. The x54 dash decay lives in ftCo_Dash_IASA and is
    // skipped by the normal cmd_var/run-input return path.
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
    processRun(world, fighter);
    if (fighter.runDirectTimer > 0) {
        return;
    }
    const Fix inputX = fighter.input.frames[0].move.x;
    if (inputX * fighter.facing >= attr.common.runInputThresholdX58) {
        changeFighterState(world, fighter, "Run");
    } else if (inputX * fighter.facing < 0 || fxAbs(inputX) < attr.common.walkInputThresholdX24) {
        changeFighterState(world, fighter, "Wait");
    }
}

static void enterRunBrake(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.runBrakeTimer = attr.maxRunBrakeFrames;
    fighter.runBrakeAnimationFrozen = false;
}

static void updateFallAnimationVariant(World& world, FighterRuntime& fighter, int neutralAction, int forwardAction, int backwardAction) {
    const FighterProperties& attr = props(world, fighter);
    if (attr.airDriftMax <= 0) {
        fighter.animationActionIndexOverride = neutralAction;
        return;
    }
    Fix driftFraction = fxDiv(fighter.fighterVelocity.x, attr.airDriftMax);
    driftFraction = std::clamp(driftFraction, -fx(1), fx(1));
    if (fxAbs(driftFraction) <= attr.common.fallAnimationDriftThresholdX444) {
        fighter.animationActionIndexOverride = neutralAction;
        return;
    }
    fighter.animationActionIndexOverride = fxMul(driftFraction, fighter.facing * fx(1)) > 0
        ? forwardAction
        : backwardAction;
}

static void processAirDrift(FighterRuntime& fighter, const FighterProperties& attr, Fix targetLimit) {
    const Fix inputX = fighter.input.frames[0].move.x;
    if (inputX != 0) {
        Fix accel = fxMul(inputX, attr.airDriftStickMul);
        accel += inputX > 0 ? attr.aerialDriftBase : -attr.aerialDriftBase;
        Fix target = fxMul(inputX, attr.airDriftMax);
        if (targetLimit > 0 && fxAbs(target) > targetLimit) {
            target = target < 0 ? -targetLimit : targetLimit;
        }
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
}

static void updateAirborneFallAnimation(World& world, FighterRuntime& fighter) {
    const std::string& stateName = currentState(world, fighter).name;
    if (stateName == "Fall") {
        updateFallAnimationVariant(world, fighter, 20, 21, 22);
    } else if (stateName == "FallAerial") {
        updateFallAnimationVariant(world, fighter, 23, 24, 25);
    } else if (stateName == "FallSpecial") {
        updateFallAnimationVariant(world, fighter, 26, 27, 28);
    }
}

static void processAirborne(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    processAirDrift(fighter, attr, 0);
    const Fix terminal = fighterFlag(fighter, 12) ? attr.fastFallTerminalVelocity : attr.terminalVel;
    fighter.fighterVelocity.y = std::max(fighter.fighterVelocity.y - attr.grav, -terminal);
    updateAirborneFallAnimation(world, fighter);
}

static void processDamageAirborne(World& world, FighterRuntime& fighter) {
    processAirborneFastfall(world, fighter);
}

static void processFallSpecial(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fastfallCheck(world, fighter);
    const Fix driftLimit = fighter.fallSpecialLimitDrift ? fighter.fallSpecialDriftMax : Fix{0};
    processAirDrift(fighter, attr, driftLimit);
    const Fix neutralTerminal = fighter.fallSpecialUseFastFallTerminal ? attr.fastFallTerminalVelocity : attr.terminalVel;
    const Fix terminal = fighterFlag(fighter, 12) ? attr.fastFallTerminalVelocity : neutralTerminal;
    fighter.fighterVelocity.y = std::max(fighter.fighterVelocity.y - attr.grav, -terminal);
    updateAirborneFallAnimation(world, fighter);
}

static void processItemScrew(World& world, FighterRuntime& fighter) {
    if (frameInState(fighter) <= 1) {
        return;
    }
    processAirborneFastfall(world, fighter);
}

static void enterFallSpecial(World& world, FighterRuntime& fighter) {
    const FighterProperties& attr = props(world, fighter);
    fighter.fallSpecialLandingLag = fighter.pendingFallSpecialLandingLag > 0
        ? fighter.pendingFallSpecialLandingLag
        : attr.common.landingFallSpecialLagX344;
    fighter.fallSpecialLandingInterruptible = fighter.pendingFallSpecialLandingInterruptible;
    fighter.fallSpecialForceLanding = fighter.pendingFallSpecialForceLanding;
    fighter.fallSpecialLimitDrift = fighter.pendingFallSpecialLimitDrift;
    fighter.fallSpecialUseFastFallTerminal = fighter.pendingFallSpecialUseFastFallTerminal;
    fighter.fallSpecialDriftMax = fighter.pendingFallSpecialDriftMax > 0
        ? fighter.pendingFallSpecialDriftMax
        : fxMul(attr.airDriftMax, attr.common.fallSpecialDriftX340);
    fighter.pendingFallSpecialLandingLag = 0;
    fighter.pendingFallSpecialLandingInterruptible = false;
    fighter.pendingFallSpecialForceLanding = true;
    fighter.pendingFallSpecialLimitDrift = false;
    fighter.pendingFallSpecialUseFastFallTerminal = false;
    fighter.pendingFallSpecialDriftMax = 0;
    if (fighter.grounded) {
        // ftCo_FallSpecial's common entry calls ftCommon_8007D60C from ground,
        // spending all jumps and using the shorter special-fall ECB lock.
        fighter.grounded = false;
        fighter.groundSegment = -1;
        fighter.groundVelocity = 0;
        fighter.groundAccel = 0;
        fighter.groundAccelSecondary = 0;
        fighter.groundAttackerShieldKnockbackVelocity = 0;
        lockFighterEcb(fighter, 5);
        fighter.jumpsUsed = attr.maxJumps;
    } else {
        fighter.jumpsUsed = attr.maxJumps;
    }
}

static void enterPass(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.floorSkipSegment = fighter.groundSegment;
    fighter.grounded = false;
    fighter.groundSegment = -1;
    lockFighterEcb(fighter, 10);
    fighter.jumpsUsed = 1;
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
    if (!fighterFlag(fighter, 12) && fighter.fighterVelocity.y < 0 &&
        fighter.input.frames[0].move.y <= -attr.common.fastfallStickThresholdX88 &&
        fighter.stickYTiltTimer < attr.common.fastfallStickWindowX8C)
    {
        fighter.fighterVelocity.y = -attr.fastFallTerminalVelocity;
        fighter.stickYTiltTimer = 254;
        setFighterFlag(fighter, 12, true);
    }
}

static void processAirborneFastfall(World& world, FighterRuntime& fighter) {
    fastfallCheck(world, fighter);
    processAirborne(world, fighter);
}

static void processGroundJump(World& world, FighterRuntime& fighter) {
    if (frameInState(fighter) <= 1) {
        return;
    }
    processAirborneFastfall(world, fighter);
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
    // ftCo_JumpAerial_Enter_Basic calls ftCommon_8007D5D4 first, which marks
    // the ground jump spent, then ftCo_800CBAC4 increments for the air jump.
    fighter.jumpsUsed = std::min(std::max(1, fighter.jumpsUsed) + 1, attr.maxJumps);
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

static void enterItemScrew(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const Fix jumpMul = attr.common.itemScrewJumpMultiplierX800;
    fighter.grounded = false;
    fighter.groundSegment = -1;
    fighter.floorSkipSegment = -1;
    // ftCo_ItemScrew_Enter calls ftCommon_8007D5D4 before Jump_Phys_Inner.
    lockFighterEcb(fighter, 10);
    fighter.jumpsUsed = 1;
    fighter.fighterVelocity.x = fxMul(fxMul(fighter.fighterVelocity.x, attr.groundToAirJumpMomentumMultiplier), jumpMul);
    fighter.fighterVelocity.y = fxMul(fighter.fighterVelocity.y, attr.common.jumpMomentumYScaleX438);
    fighter.fighterVelocity.x += fxMul(fxMul(fighter.input.frames[0].move.x, attr.jumpHInitialVelocity), jumpMul);
    const Fix maxHorizontalVelocity = fxMul(attr.jumpHMaxVelocity, jumpMul);
    fighter.fighterVelocity.x = std::clamp(fighter.fighterVelocity.x, -maxHorizontalVelocity, maxHorizontalVelocity);
    fighter.fighterVelocity.y = fxMul(attr.jumpVInitialVelocity, jumpMul);
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

static void processPassiveWall(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const int frame = frameInState(fighter);
    if (frame < attr.common.passiveWallTimerX760) {
        if (currentState(world, fighter).name == "PassiveWall" &&
            (fighter.input.down(ButtonJump) ||
             fighter.input.frames[0].move.y >= attr.common.tapJumpThresholdX70))
        {
            setFighterCommandVar(fighter, 0, 1);
        }
        fighter.fighterVelocity = {};
        return;
    }
    if (frame == attr.common.passiveWallTimerX760) {
        if (fighterCommandFlag(fighter, 0)) {
            changeFighterState(world, fighter, "PassiveWallJump");
            fighter.lastStateChangeFrame = fighter.internalFrame - frame;
            fighter.fighterVelocity.x = fighter.facing * attr.wallJumpHorizontalVelocity;
            fighter.fighterVelocity.y = attr.wallJumpVerticalVelocity;
        } else {
            fighter.fighterVelocity.x = fighter.facing * attr.passiveWallHorizontalVelocity;
        }
    }
    fastfallCheck(world, fighter);
    fighter.fighterVelocity.x = fxApproach(fighter.fighterVelocity.x, 0, attr.airFriction);
    const Fix terminal = fighterFlag(fighter, 12) ? attr.fastFallTerminalVelocity : attr.terminalVel;
    fighter.fighterVelocity.y = std::max(fighter.fighterVelocity.y - attr.grav, -terminal);
}

static void processPassiveCeil(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    if (fighterThrowFlag(fighter, 3)) {
        fighter.fighterVelocity.x = fxMul(fighter.input.frames[0].move.x, attr.passiveCeilHorizontalVelocity);
        setFighterThrowFlag(fighter, 3, false);
    }
    processAirborneFastfall(world, fighter);
}

static void processEscapeAir(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    if (currentAnimationFinished(world, fighter)) {
        fighter.pendingFallSpecialLandingLag = attr.common.landingFallSpecialLagX344;
        fighter.pendingFallSpecialLandingInterruptible = false;
        fighter.pendingFallSpecialForceLanding = true;
        fighter.pendingFallSpecialLimitDrift = true;
        fighter.pendingFallSpecialUseFastFallTerminal = false;
        fighter.pendingFallSpecialDriftMax = fxMul(attr.airDriftMax, attr.common.fallSpecialDriftX340);
        changeFighterState(world, fighter, "FallSpecial");
        return;
    }
    if (fighterCommandFlag(fighter, 0)) {
        processAirborneFastfall(world, fighter);
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
    fighter.turnBufferedButtons = 0;
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
    if (fighter.input.justPressed(ButtonAttack)) {
        fighter.turnBufferedButtons |= ButtonAttack;
    }
    if (fighter.input.justPressed(ButtonSpecial)) {
        fighter.turnBufferedButtons |= ButtonSpecial;
    }

    const int facingAfterTurn = fighter.turnFacingAfter == 0 ? -fighter.facing : fighter.turnFacingAfter;
    bool turnedThisFrame = false;
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
        turnedThisFrame = true;
    }

    if (fighter.turnJustTurned &&
        fighter.turnDashBuffered &&
        fighter.input.frames[0].move.x * facingAfterTurn >= attr.common.dashInputThresholdX3C)
    {
        changeFighterState(world, fighter, "Dash");
        return;
    }

    if (fighter.turnJustTurned && !turnedThisFrame) {
        fighter.turnJustTurned = false;
        fighter.turnBufferedButtons = 0;
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

static void processEscapeGround(World& world, FighterRuntime& fighter) {
    applyGroundFriction(fighter, props(world, fighter).grFriction);
    if (fighterThrowFlag(fighter, 3)) {
        fighter.facing *= -1;
        setFighterThrowFlag(fighter, 3, false);
    }
    if (frameInState(fighter) >= currentState(world, fighter).animationLengthFrames) {
        fighter.groundVelocity = 0;
        changeFighterState(world, fighter, "Wait");
    }
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
            setFighterCommandVar(fighter, 1, 0);
        }
    }
    if (fighter.runBrakeTimer > 0) {
        --fighter.runBrakeTimer;
        if (fighter.runBrakeTimer <= 0) {
            changeFighterState(world, fighter, "Wait");
            return;
        }
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
            setFighterCommandVar(fighter, 1, 0);
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
    Fix accel = fxMul(inputX, attr.dashRunAccelerationA);
    accel += inputX > 0 ? attr.dashRunAccelerationB : -attr.dashRunAccelerationB;
    const Fix target = fxMul(inputX, attr.dashRunTerminalVelocity);

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
    fighter.jumpsUsed = 1;
    lockFighterEcb(fighter, 10);
    changeFighterState(world, fighter, "Fall");
}

static void damageGroundToAir(World&, FighterRuntime& fighter) {
    fighter.groundVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.jumpsUsed = 1;
    lockFighterEcb(fighter, 10);
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
    const Fix edgeX = fighter.runoffDirection < 0
        ? std::min(segment.start.x, segment.end.x)
        : std::max(segment.start.x, segment.end.x);
    const bool pastEdge = fighter.runoffDirection < 0 ? fighter.position.x <= edgeX : fighter.position.x >= edgeX;
    if (!pastEdge || fighter.facing != fighter.runoffDirection ||
        fighter.input.frames[0].move.x * fighter.runoffDirection >= fxFromFloat(0.75f))
    {
        regularAirborne(world, fighter);
        return;
    }

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

static void enterMissFoot(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.knockbackVelocity.y = 0;
    fighter.fighterVelocity.x = std::clamp(fighter.fighterVelocity.x, -attr.airDriftMax, attr.airDriftMax);
    fighter.grounded = false;
    fighter.groundSegment = -1;
    fighter.groundVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.jumpsUsed = 1;
    lockFighterEcb(fighter, 10);
    changeFighterState(world, fighter, "MissFoot");
}

static void missFootOrAirborne(World& world, FighterRuntime& fighter) {
    if (fighter.runoffSegment >= 0 &&
        fighter.runoffSegment < static_cast<int>(world.stage.segments.size()) &&
        fighter.runoffDirection != 0 &&
        fighter.facing == -fighter.runoffDirection)
    {
        enterMissFoot(world, fighter);
        return;
    }
    regularAirborne(world, fighter);
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
    if (fxAbs(fighter.position.x - edgeX) > attr.common.teeterForwardDistanceX478 + attr.common.teeterBackwardDistanceX47C) {
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

static void enterLandingAirWithLag(World& world, FighterRuntime& fighter, const std::string& stateName, int lag) {
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    changeFighterState(world, fighter, stateName, lag);

    const FighterState& state = currentState(world, fighter);
    Fix clipLength = fx(state.animationLengthFrames);
    if (def.hasHsdAsset && def.hsdAsset) {
        if (const AnimationClip* clip = findClipByActionIndex(*def.hsdAsset, state.animationActionIndex)) {
            if (clip->frameCount > 0) {
                clipLength = clip->frameCount;
            }
        }
    }
    fighter.animationRate = fxDiv(clipLength + fxFromFloat(0.1f), fx(std::max(1, lag)));
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
        enterLandingAirWithLag(world, fighter, "LandingAirF", lCancelAdjustedLandingLag(fighter, attr, attr.fairLandingLag));
    } else if (stateName == "AirAttackB") {
        enterLandingAirWithLag(world, fighter, "LandingAirB", lCancelAdjustedLandingLag(fighter, attr, attr.bairLandingLag));
    } else if (stateName == "AirAttackHi") {
        enterLandingAirWithLag(world, fighter, "LandingAirHi", lCancelAdjustedLandingLag(fighter, attr, attr.uairLandingLag));
    } else if (stateName == "AirAttackLw") {
        enterLandingAirWithLag(world, fighter, "LandingAirLw", lCancelAdjustedLandingLag(fighter, attr, attr.dairLandingLag));
    } else {
        enterLandingAirWithLag(world, fighter, "LandingAirN", lCancelAdjustedLandingLag(fighter, attr, attr.nairLandingLag));
    }
}

static void processCliffJump(World& world, FighterRuntime& fighter) {
    maintainLedge(world, fighter);
    const std::string& stateName = currentState(world, fighter).name;
    if (stateName != "CliffJump" && stateName != "CliffJumpQuick1" && stateName != "CliffJumpSlow1") {
        return;
    }
    if (frameInState(fighter) >= currentState(world, fighter).animationLengthFrames - 1) {
        if (stateName == "CliffJumpQuick1") {
            changeFighterState(world, fighter, "CliffJumpQuick2");
        } else if (stateName == "CliffJumpSlow1") {
            changeFighterState(world, fighter, "CliffJumpSlow2");
        } else {
            changeFighterState(world, fighter, "CliffJumpAir");
        }
    }
}

static void processCliffJumpAir(World& world, FighterRuntime& fighter) {
    if (frameInState(fighter) <= 1) {
        return;
    }
    processAirborneFastfall(world, fighter);
}

static void escapeAirLanding(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.jumpsUsed = 0;
    setFighterFlag(fighter, 12, false);
    fighter.fighterVelocity.y = 0;
    if (!fighter.fallSpecialForceLanding && fighter.lastLandingVelocityY >= attr.noImpactLandingVelocity) {
        changeFighterState(world, fighter, "Wait", 0);
        return;
    }
    const int lag = fighter.fallSpecialLandingLag > 0
        ? fighter.fallSpecialLandingLag
        : attr.common.landingFallSpecialLagX344;
    changeFighterState(
        world,
        fighter,
        fighter.fallSpecialLandingInterruptible ? "LandingFallSpecialInterruptible" : "LandingFallSpecial",
        lag);
}

static bool currentAnimationFinished(World& world, const FighterRuntime& fighter) {
    const FighterState& state = currentState(world, fighter);
    const int animationLengthFrames = fighter.stateAnimationLengthOverride > 0
        ? fighter.stateAnimationLengthOverride
        : state.animationLengthFrames;
    return frameInState(fighter) > animationLengthFrames;
}

static Fix vecMagnitude(Vec2 value) {
    return fxFromFloat(std::sqrt(std::pow(fxToFloat(value.x), 2.0f) + std::pow(fxToFloat(value.y), 2.0f)));
}

static void processDamage(World& world, FighterRuntime& fighter) {
    if (fighter.grounded) {
        processLanding(world, fighter);
    } else {
        processDamageAirborne(world, fighter);
    }

    if (fighter.hitstun > 0) {
        return;
    }

    if (!currentAnimationFinished(world, fighter)) {
        return;
    }

    if (fighter.grounded) {
        changeFighterState(world, fighter, "Wait");
        return;
    }
    changeFighterState(world, fighter, "Fall");
}

static void processDamageBind(World& world, FighterRuntime& fighter) {
    const MeleeCommonData& common = props(world, fighter).common;
    applyGroundFriction(fighter, props(world, fighter).grFriction);
    fighter.grabTimer -= common.damageBindTimerDecrementX670;
    applyGrabMash(fighter, common, common.damageBindMashDecrementX674);
    if (fighter.grabTimer <= 0) {
        changeFighterState(world, fighter, fighter.grounded ? "Wait" : "Fall");
    }
}

static void enterDamageIce(World& world, FighterRuntime& fighter) {
    (void) world;
    std::fill(fighter.hurtboxStates.begin(), fighter.hurtboxStates.end(), HurtboxState::Intangible);
    fighter.hitstun = 0;
    fighter.grabMashStickX = 0;
    fighter.grabMashStickY = 0;
    fighter.captureWaitTimer = 0;
    fighter.captureMashAnimTimer = 0;
    fighter.captureJumpQueued = false;
    if (!fighter.grounded) {
        lockFighterEcb(fighter, 10);
    }
}

static void enterDamageIceJump(World& world, FighterRuntime& fighter) {
    const FighterProperties& attr = props(world, fighter);
    const MeleeCommonData& common = attr.common;
    fighter.grounded = false;
    fighter.groundSegment = -1;
    fighter.groundVelocity = 0;
    fighter.groundKnockbackVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.jumpsUsed = 1;
    fighter.knockbackVelocity = {};
    fighter.knockbackDecay = {};
    fighter.fighterVelocity.x = fxMul(fighter.input.frames[0].move.x, attr.damageIceJumpVelocityXMultiplier);
    fighter.fighterVelocity.y = attr.damageIceJumpVelocityY;
    fighter.grabTimer = std::max(Fix{1}, common.damageIceJumpEscapeFramesX7A4);
    lockFighterEcb(fighter, 10);
}

static void enterBuryJump(World& world, FighterRuntime& fighter) {
    const MeleeCommonData& common = props(world, fighter).common;
    fighter.grounded = false;
    fighter.groundSegment = -1;
    fighter.groundVelocity = 0;
    fighter.groundKnockbackVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.jumpsUsed = 1;
    fighter.knockbackVelocity = {};
    fighter.knockbackDecay = {};
    fighter.fighterVelocity.x = 0;
    fighter.fighterVelocity.y = common.buryJumpVelocityYx618;
    lockFighterEcb(fighter, 10);
}

static void processDamageIce(World& world, FighterRuntime& fighter) {
    const FighterProperties& attr = props(world, fighter);
    const MeleeCommonData& common = attr.common;
    if (fighter.grounded) {
        processLanding(world, fighter);
    } else {
        const Fix gravity = fxMul(attr.grav, common.damageIceGravityMultiplierX77C);
        fighter.fighterVelocity.y = std::max(fighter.fighterVelocity.y - gravity, -attr.terminalVel);
    }
    fighter.grabTimer -= common.damageIceTimerDecrementX794;
    applyGrabMash(fighter, common, common.damageIceMashDecrementX798);
    if (fighter.grabTimer <= 0) {
        changeFighterState(world, fighter, "DamageIceJump");
    }
}

static void processDamageIceJump(World& world, FighterRuntime& fighter) {
    processAirborne(world, fighter);
    fighter.grabTimer -= fx(1);
    if (fighter.grabTimer <= 0) {
        changeFighterState(world, fighter, "Fall");
    }
}

static void releaseBury(World& world, FighterRuntime& fighter) {
    changeFighterState(world, fighter, "BuryJump");
}

static bool tickBuryTimer(World& world, FighterRuntime& fighter) {
    const MeleeCommonData& common = props(world, fighter).common;
    applyGroundFriction(fighter, props(world, fighter).grFriction);
    fighter.grabTimer -= common.buryTimerDecrementX610;
    applyGrabMash(fighter, common, common.buryMashDecrementX614);
    if (fighter.grabTimer <= 0) {
        releaseBury(world, fighter);
        return true;
    }
    return false;
}

static void processBury(World& world, FighterRuntime& fighter) {
    if (tickBuryTimer(world, fighter)) {
        return;
    }
    --fighter.burySubmergeTimer;
    if (fighter.burySubmergeTimer <= 0) {
        changeFighterState(world, fighter, "BuryWait");
    }
}

static void processBuryWait(World& world, FighterRuntime& fighter) {
    tickBuryTimer(world, fighter);
}

static void processBuryJump(World& world, FighterRuntime& fighter) {
    processAirborne(world, fighter);
    if (currentAnimationFinished(world, fighter)) {
        changeFighterState(world, fighter, "Fall");
    }
}

static void enterReboundStop(World& world, FighterRuntime& fighter) {
    const MeleeCommonData& common = props(world, fighter).common;
    const Fix reboundVelocity = std::max(fxFromFloat(0.01f), fighter.reboundDamageVelocity);
    fighter.reboundAnimationRate = fxDiv(fxFromFloat(0.1f), reboundVelocity);
    fighter.reboundAccel =
        -fighter.reboundFacingDir *
        (fxMul(reboundVelocity, common.reboundAccelScaleX3D8) + common.reboundAccelBaseX3DC);
    fighter.groundAccelSecondary = fighter.reboundAccel;
}

static void processReboundStop(World& world, FighterRuntime& fighter) {
    const Fix animationRate = fighter.reboundAnimationRate;
    changeFighterState(world, fighter, "Rebound");
    fighter.animationRate = animationRate;
}

static void processRebound(World& world, FighterRuntime& fighter) {
    if (fighter.reboundAccel != 0) {
        fighter.reboundAccel = 0;
    } else {
        processLanding(world, fighter);
    }
    if (currentAnimationFinished(world, fighter)) {
        changeFighterState(world, fighter, "Wait");
    }
}

static void processMissFoot(World& world, FighterRuntime& fighter) {
    processAirborneFastfall(world, fighter);
    if (currentAnimationFinished(world, fighter)) {
        changeFighterState(world, fighter, "DamageFall");
    }
}

static bool tickDamageSongTimer(World& world, FighterRuntime& fighter) {
    const MeleeCommonData& common = props(world, fighter).common;
    applyGroundFriction(fighter, props(world, fighter).grFriction);
    fighter.grabTimer -= common.damageSongTimerDecrementX63C;
    applyGrabMash(fighter, common, common.damageSongMashDecrementX640);
    if (fighter.grabTimer <= 0) {
        changeFighterState(world, fighter, "DamageSongRv");
        return true;
    }
    return false;
}

static void processDamageSong(World& world, FighterRuntime& fighter) {
    if (!tickDamageSongTimer(world, fighter) && currentAnimationFinished(world, fighter)) {
        changeFighterState(world, fighter, "DamageSongWait");
    }
}

static void processDamageSongWait(World& world, FighterRuntime& fighter) {
    tickDamageSongTimer(world, fighter);
}

static void processDamageSongRv(World& world, FighterRuntime& fighter) {
    applyGroundFriction(fighter, props(world, fighter).grFriction);
    if (currentAnimationFinished(world, fighter)) {
        changeFighterState(world, fighter, fighter.grounded ? "Wait" : "Fall");
    }
}

static void enterFurafura(World& world, FighterRuntime& fighter) {
    const MeleeCommonData& common = props(world, fighter).common;
    fighter.shieldHealth = common.furafuraShieldHealthX280;
    fighter.grabTimer = std::max(Fix{0}, common.furafuraTimerBaseX2F8 - fighter.percent) +
        common.furafuraTimerMinX2FC;
    fighter.grabMashStickX = 0;
    fighter.grabMashStickY = 0;
}

static void processFurafura(World& world, FighterRuntime& fighter) {
    const MeleeCommonData& common = props(world, fighter).common;
    fighter.shieldHealth = common.furafuraShieldHealthX280;
    fighter.grabTimer -= common.furafuraTimerDecrementX300;
    applyGrabMash(fighter, common, common.furafuraMashDecrementX304);
    processLanding(world, fighter);
    if (fighter.grabTimer <= 0) {
        changeFighterState(world, fighter, "Wait");
    }
}

static void processDamageScrew(World& world, FighterRuntime& fighter) {
    processAirborneFastfall(world, fighter);
    if (currentAnimationFinished(world, fighter)) {
        const FighterProperties& attr = props(world, fighter);
        fighter.pendingFallSpecialLandingLag = attr.normalLandingLag;
        fighter.pendingFallSpecialLandingInterruptible = true;
        fighter.pendingFallSpecialForceLanding = false;
        fighter.pendingFallSpecialLimitDrift = false;
        fighter.pendingFallSpecialUseFastFallTerminal = false;
        fighter.pendingFallSpecialDriftMax = attr.airDriftMax;
        changeFighterState(world, fighter, "FallSpecial");
    }
}

static void processDamageFly(World& world, FighterRuntime& fighter) {
    processDamageAirborne(world, fighter);
    if (fighter.thrownHitboxOwner >= 0 &&
        vecMagnitude(fighter.knockbackVelocity) < props(world, fighter).common.thrownHitboxClearVelocityX1C8)
    {
        fighter.activeHitboxes.clear();
        fighter.fightersHitThisAction.clear();
    }
    if (fighter.hitstun <= 0) {
        if (currentState(world, fighter).name == "DamageFlyRoll" || currentAnimationFinished(world, fighter)) {
            changeFighterState(world, fighter, "DamageFall");
        }
        return;
    }
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
    const int window = common.passiveInputWindowX250;
    const int maxAge = std::min(std::max(0, window - 1), InputBuffer::kSize - 2);
    for (int age = 0; age <= maxAge; ++age) {
        if (techPressAtAge(fighter.input, age)) {
            return techPressHasMeleeRepeatGap(fighter.input, age, common.inputRepeatWindowX1C);
        }
    }
    return false;
}

static int currentActionCommonBone(const World& world, const FighterRuntime& fighter, int commonPart) {
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    if (!def.hasHsdAsset || !def.hsdAsset || commonPart < 0) {
        return -1;
    }

    const FighterState& state = currentState(world, fighter);
    if (const HsdActionScript* script = findActionScriptByActionIndex(*def.hsdAsset, state.animationActionIndex)) {
        if (commonPart < static_cast<int>(script->commonBoneLookup.size())) {
            const int mapped = script->commonBoneLookup[static_cast<size_t>(commonPart)];
            if (mapped >= 0) {
                return mapped;
            }
        }
    }

    if (commonPart < static_cast<int>(def.hsdAsset->commonBoneLookup.size())) {
        return def.hsdAsset->commonBoneLookup[static_cast<size_t>(commonPart)];
    }
    return -1;
}

static bool meleeDownBoundFaceUpPose(const World& world, const FighterRuntime& fighter) {
    const int hipN = currentActionCommonBone(world, fighter, 4);
    if (hipN >= 0 && static_cast<size_t>(hipN) < fighter.hsdJointWorldTransforms.size()) {
        return fighter.hsdJointWorldTransforms[static_cast<size_t>(hipN)].matrix[5] > 0;
    }
    return true;
}

static void enterDownBoundFromDamagePose(World& world, FighterRuntime& fighter) {
    fighter.fighterVelocity.y = 0;
    fighter.knockbackVelocity.y = 0;
    changeFighterState(world, fighter, meleeDownBoundFaceUpPose(world, fighter) ? "DownBoundU" : "DownBoundD");
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

static Fix velocityAlongCurrentGround(Vec2 velocity, Vec2 normal) {
    const Vec2 tangent{normal.y, -normal.x};
    return fxMul(velocity.x, tangent.x) + fxMul(velocity.y, tangent.y);
}

static Vec2 groundKnockbackVector(Fix groundVelocity, Vec2 normal) {
    const Vec2 tangent{normal.y, -normal.x};
    return {fxMul(tangent.x, groundVelocity), fxMul(tangent.y, groundVelocity)};
}

static void enterTechPreserveKnockback(World&, FighterRuntime& fighter) {
    fighter.hitstun = 0;
    fighter.damageTumble = false;
    fighter.damageSurfaceTimer = 0;
    fighter.damageHitboxOwner = -1;
    fighter.thrownHitboxOwner = -1;
    fighter.fighterVelocity = {};
    fighter.attackerShieldKnockback = {};
    fighter.groundVelocity = 0;
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    fighter.groundAccel = 0;
    fighter.groundAccelSecondary = 0;
}

static void enterPassive(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    Fix preservedGroundKnockback = fighter.groundKnockbackVelocity;
    Vec2 preservedKnockback = fighter.knockbackVelocity;
    if (fighter.grounded && preservedGroundKnockback == 0) {
        preservedGroundKnockback = std::clamp(
            velocityAlongCurrentGround(fighter.knockbackVelocity, fighter.groundNormal),
            -attr.common.damageGroundKnockbackClampX164,
            attr.common.damageGroundKnockbackClampX164);
        preservedKnockback = groundKnockbackVector(preservedGroundKnockback, fighter.groundNormal);
    }

    enterTechPreserveKnockback(world, fighter);
    fighter.groundKnockbackVelocity = preservedGroundKnockback;
    fighter.knockbackVelocity = preservedKnockback;
}

static void clearAttackSpecialInputHistory(FighterRuntime& fighter) {
    for (InputFrame& frame : fighter.input.frames) {
        frame.buttons &= static_cast<uint16_t>(~(ButtonAttack | ButtonSpecial));
    }
}

static void enterDownBound(World& world, FighterRuntime& fighter) {
    enterPassive(world, fighter);
    clearAttackSpecialInputHistory(fighter);
}

static void damageLanding(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.fighterVelocity.y = 0;
    const Fix speed = vecMagnitude(fighter.knockbackVelocity);
    if (fighter.damageTumble || speed >= attr.common.damageWallBounceMinVelocityX1E0) {
        enterDownBoundFromDamagePose(world, fighter);
        return;
    }
    if (speed >= attr.common.damageLandingMinVelocityX1E4) {
        regularLanding(world, fighter);
    }
}

static void damageFlyLanding(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.fighterVelocity.y = 0;
    if (recentTechInput(fighter, attr.common)) {
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
    enterDownBoundFromDamagePose(world, fighter);
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
    const InputFrame& previous = fighter.input.frames[1];
    const float cStickAngle = std::atan2(fxToFloat(input.cStick.y), std::abs(fxToFloat(input.cStick.x)));
    if (fxAbs(previous.cStick.x) < attr.common.downRollStickThresholdX248 &&
        fxAbs(input.cStick.x) >= attr.common.downRollStickThresholdX248 &&
        cStickAngle < fxToFloat(attr.common.aerialAttackAngleTanX20))
    {
        stickX = input.cStick.x;
        return true;
    }
    const float stickAngle = std::atan2(fxToFloat(input.move.y), std::abs(fxToFloat(input.move.x)));
    if (fxAbs(input.move.x) >= attr.common.downRollStickThresholdX248 &&
        stickAngle < fxToFloat(attr.common.aerialAttackAngleTanX20))
    {
        stickX = input.move.x;
        return true;
    }
    return false;
}

static bool downAttackInput(const FighterRuntime& fighter, const FighterProperties& attr) {
    const bool attackPressed =
        (fighter.input.frames[0].buttons & ButtonAttack) != 0 &&
        (fighter.input.frames[0].buttons & ButtonGrab) == 0 &&
        ((fighter.input.frames[1].buttons & ButtonAttack) == 0 ||
         (fighter.input.frames[1].buttons & ButtonGrab) != 0);
    if (attackPressed || fighter.input.justPressed(ButtonSpecial)) {
        return true;
    }
    return fighter.input.frames[1].cStick.y < attr.common.downAttackCStickThresholdX7F4 &&
           fighter.input.frames[0].cStick.y >= attr.common.downAttackCStickThresholdX7F4;
}

static bool buttonJustPressedAtAge(const InputBuffer& input, uint16_t button, int age) {
    const size_t current = static_cast<size_t>(age);
    const size_t previous = static_cast<size_t>(age + 1);
    return (input.frames[current].buttons & button) != 0 &&
           (input.frames[previous].buttons & button) == 0;
}

static bool buttonJustPressedInWindow(const InputBuffer& input, uint16_t button, int window) {
    const int maxAge = std::min(std::max(0, window - 1), InputBuffer::kSize - 2);
    for (int age = 0; age <= maxAge; ++age) {
        if (buttonJustPressedAtAge(input, button, age)) {
            return true;
        }
    }
    return false;
}

static bool physicalAttackJustPressedAtAge(const InputBuffer& input, int age) {
    const size_t current = static_cast<size_t>(age);
    const size_t previous = static_cast<size_t>(age + 1);
    const bool currentAttack = (input.frames[current].buttons & ButtonAttack) != 0 &&
                               (input.frames[current].buttons & ButtonGrab) == 0;
    const bool previousAttack = (input.frames[previous].buttons & ButtonAttack) != 0 &&
                                (input.frames[previous].buttons & ButtonGrab) == 0;
    return currentAttack && !previousAttack;
}

static bool physicalAttackJustPressedInWindow(const InputBuffer& input, int window) {
    const int maxAge = std::min(std::max(0, window - 1), InputBuffer::kSize - 2);
    for (int age = 0; age <= maxAge; ++age) {
        if (physicalAttackJustPressedAtAge(input, age)) {
            return true;
        }
    }
    return false;
}

static bool downBoundAttackInput(const FighterRuntime& fighter, const FighterProperties& attr) {
    if (physicalAttackJustPressedInWindow(fighter.input, attr.common.downAttackInputWindowX24C) ||
        buttonJustPressedInWindow(fighter.input, ButtonSpecial, attr.common.downAttackInputWindowX24C))
    {
        return true;
    }
    return fighter.input.frames[1].cStick.y < attr.common.downAttackCStickThresholdX7F4 &&
           fighter.input.frames[0].cStick.y >= attr.common.downAttackCStickThresholdX7F4;
}

static bool downStandStickInput(const FighterRuntime& fighter, const FighterProperties& attr) {
    const InputFrame& input = fighter.input.frames[0];
    const float stickAngle = std::atan2(fxToFloat(input.move.y), std::abs(fxToFloat(input.move.x)));
    return input.move.y >= attr.common.downStandStickThresholdX244 &&
           stickAngle >= fxToFloat(attr.common.aerialAttackAngleTanX20);
}

static void enterDownWait(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    fighter.downWaitTimer = attr.common.downWaitAutoStandFramesX424;
    enterTechPreserveKnockback(world, fighter);
}

static void processDownWait(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    const std::string& stateName = currentState(world, fighter).name;
    applyGroundFriction(fighter, attr.grFriction);

    if (downAttackInput(fighter, attr)) {
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
    const InputFrame& previous = fighter.input.frames[1];
    const bool triggerStand =
        ((input.buttons & ButtonShield) != 0 && (input.buttons & ButtonGrab) == 0 &&
         ((previous.buttons & ButtonShield) == 0 || (previous.buttons & ButtonGrab) != 0)) ||
        (input.shieldAnalog > 0 && previous.shieldAnalog <= 0) ||
        downStandStickInput(fighter, attr);
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

static void processDownBound(World& world, FighterRuntime& fighter) {
    processLanding(world, fighter);
    if (frameInState(fighter) <= currentState(world, fighter).animationLengthFrames) {
        return;
    }

    const std::string& stateName = currentState(world, fighter).name;
    if (downBoundAttackInput(fighter, props(world, fighter))) {
        changeFighterState(world, fighter, downVariantState(stateName, "DownAttackU", "DownAttackD"));
        return;
    }

    Fix stickX = 0;
    if (downRollInput(fighter, props(world, fighter), stickX)) {
        const bool forward = fxMul(stickX, fx(fighter.facing)) >= 0;
        changeFighterState(world, fighter, downVariantState(
            stateName,
            forward ? "DownForwardU" : "DownBackU",
            forward ? "DownForwardD" : "DownBackD"));
        return;
    }

    changeFighterState(world, fighter, downVariantState(stateName, "DownWaitU", "DownWaitD"));
}

static void processDownDamage(World& world, FighterRuntime& fighter) {
    if (!fighter.grounded) {
        if (frameInState(fighter) > currentState(world, fighter).animationLengthFrames) {
            changeFighterState(world, fighter, "Fall");
            return;
        }
        processDamageAirborne(world, fighter);
        return;
    }

    processLanding(world, fighter);
    if (fighter.downWaitTimer > 0) {
        --fighter.downWaitTimer;
    }
    if (frameInState(fighter) > currentState(world, fighter).animationLengthFrames) {
        const std::string& stateName = currentState(world, fighter).name;
        if (fighter.downWaitTimer <= 0) {
            changeFighterState(world, fighter, downVariantState(stateName, "DownStandU", "DownStandD"));
        } else {
            const int remainingDownWaitTimer = fighter.downWaitTimer;
            changeFighterState(world, fighter, downVariantState(stateName, "DownWaitU", "DownWaitD"));
            fighter.downWaitTimer = remainingDownWaitTimer;
        }
    }
}

static void processDownGetup(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    applyGroundFriction(fighter, attr.grFriction);
}

static void processDamageSurface(World& world, FighterRuntime& fighter) {
    ++fighter.damageSurfaceTimer;
    if (fighter.hitstun > 0) {
        processAirborneFastfall(world, fighter);
        return;
    }
    if (fighter.damageSurfaceTimer > 5) {
        changeFighterState(world, fighter, "DamageFall");
    }
}

static void processDownReflect(World& world, FighterRuntime& fighter) {
    processAirborneFastfall(world, fighter);
    if (currentAnimationFinished(world, fighter)) {
        changeFighterState(world, fighter, "DamageFall");
    }
}

static void downReflectLanding(World& world, FighterRuntime& fighter) {
    enterDownBoundFromDamagePose(world, fighter);
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

static void enterCliffWait(World& world, FighterRuntime& fighter) {
    const FighterProperties& attr = props(world, fighter);
    fighter.ledgeActionReady = false;
    fighter.ledgeWaitTimer =
        fighter.percent < fx(attr.common.cliffActionPercentThresholdX488)
            ? attr.common.cliffWaitAutoReleaseFramesQuickX48C
            : attr.common.cliffWaitAutoReleaseFramesSlowX490;
}

static void processCliffWait(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    maintainLedge(world, fighter);
    if (currentState(world, fighter).name != "CliffWait") {
        return;
    }
    if (fighter.ledgeWaitTimer > 0) {
        --fighter.ledgeWaitTimer;
    }
    if (fighter.ledgeWaitTimer <= 0) {
        releaseLedge(fighter, attr);
        changeFighterState(world, fighter, "DamageFall");
        fighter.fighterVelocity.x = std::clamp(fighter.fighterVelocity.x, -attr.airDriftMax, attr.airDriftMax);
    }
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
    const std::string& stateName = currentState(world, fighter).name;
    if (stateName != "CliffClimb" && stateName != "CliffClimbQuick" && stateName != "CliffClimbSlow") {
        return;
    }
    if (frameInState(fighter) >= currentState(world, fighter).animationLengthFrames - 1) {
        changeFighterState(world, fighter, fighter.grounded ? "Wait" : "Fall");
    }
}

static void processCliffEscape(World& world, FighterRuntime& fighter) {
    processCliffClimb(world, fighter);
    const std::string& stateName = currentState(world, fighter).name;
    if (stateName != "CliffEscape" && stateName != "CliffEscapeQuick" && stateName != "CliffEscapeSlow") {
        return;
    }
    if (frameInState(fighter) >= currentState(world, fighter).animationLengthFrames - 1) {
        changeFighterState(world, fighter, fighter.grounded ? "Wait" : "Fall");
    }
}

static void processCliffAttack(World& world, FighterRuntime& fighter) {
    processCliffClimb(world, fighter);
    const std::string& stateName = currentState(world, fighter).name;
    if (stateName != "CliffAttack" && stateName != "CliffAttackQuick" && stateName != "CliffAttackSlow") {
        return;
    }
    if (frameInState(fighter) >= currentState(world, fighter).animationLengthFrames - 1) {
        changeFighterState(world, fighter, fighter.grounded ? "Wait" : "Fall");
    }
}

static void enterCliffDrop(World& world, FighterRuntime& fighter) {
    FighterProperties& attr = props(world, fighter);
    releaseLedge(fighter, attr);
    fighter.fighterVelocity = {};
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

static void ensurePackageVars(const FighterDefinition& def, FighterRuntime& fighter) {
    if (fighter.packageVars.size() == def.packageVariables.size()) {
        return;
    }
    const size_t oldSize = fighter.packageVars.size();
    fighter.packageVars.resize(def.packageVariables.size());
    for (size_t i = oldSize; i < def.packageVariables.size(); ++i) {
        fighter.packageVars[i] = def.packageVariables[i].initialValue;
    }
}

static int32_t packageVar(const FighterRuntime& fighter, int index) {
    if (index < 0 || index >= static_cast<int>(fighter.packageVars.size())) {
        return 0;
    }
    return fighter.packageVars[static_cast<size_t>(index)];
}

static void setPackageVar(FighterRuntime& fighter, int index, int32_t value) {
    if (index < 0 || index >= static_cast<int>(fighter.packageVars.size())) {
        return;
    }
    fighter.packageVars[static_cast<size_t>(index)] = value;
}

void runPackageScript(World& world, FighterRuntime& fighter, const std::string& scriptName) {
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const auto found = std::find_if(def.packageScripts.begin(), def.packageScripts.end(), [&](const PackageScript& script) {
        return script.name == scriptName;
    });
    if (found == def.packageScripts.end()) {
        return;
    }
    ensurePackageVars(def, fighter);
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
            setPackageVar(fighter, instruction.dst, instruction.intValue);
            break;
        case PackageScriptOp::AddVarImmediate:
            setPackageVar(fighter, instruction.dst, packageVar(fighter, instruction.dst) + instruction.intValue);
            break;
        case PackageScriptOp::AddVar:
            setPackageVar(fighter, instruction.dst, packageVar(fighter, instruction.srcA) + packageVar(fighter, instruction.srcB));
            break;
        case PackageScriptOp::ScaleVarFixed:
            setPackageVar(fighter, instruction.dst, fxMul(packageVar(fighter, instruction.srcA), instruction.fixValue));
            break;
        case PackageScriptOp::SetVarFrame:
            setPackageVar(fighter, instruction.dst, fighter.internalFrame);
            break;
        case PackageScriptOp::SetVarStateFrame:
            setPackageVar(fighter, instruction.dst, frameInState(fighter));
            break;
        case PackageScriptOp::SetVarGrounded:
            setPackageVar(fighter, instruction.dst, fighter.grounded ? 1 : 0);
            break;
        case PackageScriptOp::SetVarFacing:
            setPackageVar(fighter, instruction.dst, fighter.facing);
            break;
        case PackageScriptOp::SetVarFighterPercent:
            setPackageVar(fighter, instruction.dst, fighter.percent);
            break;
        case PackageScriptOp::SetVarFighterShield:
            setPackageVar(fighter, instruction.dst, fighter.shieldHealth);
            break;
        case PackageScriptOp::SetVarFighterPositionX:
            setPackageVar(fighter, instruction.dst, fighter.position.x);
            break;
        case PackageScriptOp::SetVarFighterPositionY:
            setPackageVar(fighter, instruction.dst, fighter.position.y);
            break;
        case PackageScriptOp::SetVarFighterGroundVelocity:
            setPackageVar(fighter, instruction.dst, fighter.groundVelocity);
            break;
        case PackageScriptOp::SetVarFighterAirVelocityX:
            setPackageVar(fighter, instruction.dst, fighter.fighterVelocity.x);
            break;
        case PackageScriptOp::SetVarFighterAirVelocityY:
            setPackageVar(fighter, instruction.dst, fighter.fighterVelocity.y);
            break;
        case PackageScriptOp::SetVarObjectOwner:
        case PackageScriptOp::SetVarObjectHeldBy:
        case PackageScriptOp::SetVarObjectLastFighter:
        case PackageScriptOp::SetVarObjectLastObject:
        case PackageScriptOp::SetVarObjectDamage:
        case PackageScriptOp::SetVarObjectPositionX:
        case PackageScriptOp::SetVarObjectPositionY:
        case PackageScriptOp::SetVarObjectVelocityX:
        case PackageScriptOp::SetVarObjectVelocityY:
            break;
        case PackageScriptOp::SetVarButtonDown:
            setPackageVar(fighter, instruction.dst, (fighter.input.frames[0].buttons & instruction.intValue) != 0 ? 1 : 0);
            break;
        case PackageScriptOp::SetVarButtonPressed:
            setPackageVar(fighter, instruction.dst, fighter.input.justPressed(static_cast<uint16_t>(instruction.intValue)) ? 1 : 0);
            break;
        case PackageScriptOp::SetVarStickX:
            setPackageVar(fighter, instruction.dst, fighter.input.frames[0].move.x);
            break;
        case PackageScriptOp::SetVarStickY:
            setPackageVar(fighter, instruction.dst, fighter.input.frames[0].move.y);
            break;
        case PackageScriptOp::SetVarCStickX:
            setPackageVar(fighter, instruction.dst, fighter.input.frames[0].cStick.x);
            break;
        case PackageScriptOp::SetVarCStickY:
            setPackageVar(fighter, instruction.dst, fighter.input.frames[0].cStick.y);
            break;
        case PackageScriptOp::SetVarShield:
            setPackageVar(fighter, instruction.dst, fighter.input.frames[0].shieldAnalog);
            break;
        case PackageScriptOp::SetGroundVelocity:
            fighter.groundVelocity = instruction.fixValue;
            break;
        case PackageScriptOp::SetAirVelocityX:
            fighter.fighterVelocity.x = instruction.fixValue;
            break;
        case PackageScriptOp::SetAirVelocityY:
            fighter.fighterVelocity.y = instruction.fixValue;
            break;
        case PackageScriptOp::SetFacing:
            fighter.facing = instruction.intValue < 0 ? -1 : 1;
            break;
        case PackageScriptOp::SetGroundVelocityFromVar:
            fighter.groundVelocity = packageVar(fighter, instruction.srcA);
            break;
        case PackageScriptOp::SetAirVelocityXFromVar:
            fighter.fighterVelocity.x = packageVar(fighter, instruction.srcA);
            break;
        case PackageScriptOp::SetAirVelocityYFromVar:
            fighter.fighterVelocity.y = packageVar(fighter, instruction.srcA);
            break;
        case PackageScriptOp::SetFacingFromVar:
            fighter.facing = packageVar(fighter, instruction.srcA) < 0 ? -1 : 1;
            break;
        case PackageScriptOp::ChangeState:
            changeFighterState(world, fighter, instruction.text);
            break;
        case PackageScriptOp::SpawnObject:
        case PackageScriptOp::SpawnProjectile: {
            const Vec2 position{
                fighter.position.x + fighter.facing * fxFromFloat(0.75f),
                fighter.position.y + fxFromFloat(0.7f),
            };
            const Vec2 velocity{fighter.facing * instruction.fixValue, 0};
            if (instruction.op == PackageScriptOp::SpawnProjectile) {
                spawnGameObjectOfKind(world, instruction.text, GameObjectKind::Projectile, static_cast<int>(&fighter - world.fighters.data()), position, fighter.facing, velocity);
            } else {
                spawnGameObject(world, instruction.text, static_cast<int>(&fighter - world.fighters.data()), position, fighter.facing, velocity);
            }
            break;
        }
        case PackageScriptOp::SpawnObjectFromVars:
        case PackageScriptOp::SpawnProjectileFromVars: {
            const Vec2 position{
                fighter.position.x + fighter.facing * instruction.fixValue,
                fighter.position.y + fxFromFloat(0.7f),
            };
            const Vec2 velocity{
                fighter.facing * packageVar(fighter, instruction.srcA),
                packageVar(fighter, instruction.srcB),
            };
            if (instruction.op == PackageScriptOp::SpawnProjectileFromVars) {
                spawnGameObjectOfKind(world, instruction.text, GameObjectKind::Projectile, static_cast<int>(&fighter - world.fighters.data()), position, fighter.facing, velocity);
            } else {
                spawnGameObject(world, instruction.text, static_cast<int>(&fighter - world.fighters.data()), position, fighter.facing, velocity);
            }
            break;
        }
        case PackageScriptOp::DestroyObject:
            break;
        case PackageScriptOp::SkipIfVarLessThanImmediate:
            frame.instructionIndex += packageVar(fighter, instruction.dst) < instruction.intValue ? 2 : 1;
            continue;
        case PackageScriptOp::SkipIfVarLessThanVar:
            frame.instructionIndex += packageVar(fighter, instruction.srcA) < packageVar(fighter, instruction.srcB) ? 2 : 1;
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
        case PackageScriptOp::SwitchFighterDefinition:
            switchFighterDefinition(world, fighter, instruction.text);
            return;
        case PackageScriptOp::SpawnFighter: {
            const Vec2 position{
                fighter.position.x + fighter.facing * instruction.fixValue,
                fighter.position.y,
            };
            spawnFighter(world, instruction.text, position, fighter.facing);
            return;
        }
        }
        ++frame.instructionIndex;
    }
}

void runStateFunction(World& world, size_t fighterIndex, const FunctionCall& call) {
    FighterRuntime& fighter = world.fighters[fighterIndex];
    if (call.name.starts_with("script:")) return runPackageScript(world, fighter, call.name.substr(7));
    if (call.name == "common_enter") return commonEnter(world, fighter);
    if (call.name == "enter_guard_on") return enterGuardOn(world, fighter);
    if (call.name == "process_guard_on") return processGuardOn(world, fighter);
    if (call.name == "process_guard") return processGuardHeld(world, fighter);
    if (call.name == "process_guard_reflect") return processGuardReflect(world, fighter);
    if (call.name == "process_guard_setoff") return processGuardSetoff(world, fighter);
    if (call.name == "process_catch") return processCatch(world, fighter);
    if (call.name == "process_catch_dash") return processCatchDash(world, fighter);
    if (call.name == "process_catch_pull") return processCatchPull(world, fighter);
    if (call.name == "enter_attack11") return enterAttack11(world, fighter);
    if (call.name == "enter_attack100_start") return enterAttack100Start(world, fighter);
    if (call.name == "process_attack100_loop") return processAttack100Loop(world, fighter);
    if (call.name == "enter_attack_dash") return enterAttackDash(world, fighter);
    if (call.name == "process_attack_dash") return processAttackDash(world, fighter);
    if (call.name == "enter_attack_lw3") return enterAttackLw3(world, fighter);
    if (call.name == "process_attack_lw3") return processAttackLw3(world, fighter);
    if (call.name == "process_catch_wait") return processCatchWait(world, fighter);
    if (call.name == "process_catch_attack") return processCatchAttack(world, fighter);
    if (call.name == "enter_throw") return enterThrow(world, fighter);
    if (call.name == "process_throw") return processThrow(world, fighter);
    if (call.name == "enter_catch_cut") return enterCatchCut(world, fighter);
    if (call.name == "process_catch_cut") return processCatchCut(world, fighter);
    if (call.name == "enter_capture_cut") return enterCaptureCut(world, fighter);
    if (call.name == "process_capture_cut") return processCaptureCut(world, fighter);
    if (call.name == "enter_capture_jump") return enterCaptureJump(world, fighter);
    if (call.name == "process_capture_jump") return processCaptureJump(world, fighter);
    if (call.name == "capture_jump_landing") return captureJumpLanding(world, fighter);
    if (call.name == "process_capture") return processCapture(world, fighter);
    if (call.name == "process_capture_wait") return processCaptureWait(world, fighter);
    if (call.name == "process_capture_damage") return processCaptureDamage(world, fighter);
    if (call.name == "process_thrown") return processThrown(world, fighter);
    if (call.name == "catch_airborne") return catchAirborne(world, fighter);
    if (call.name == "throw_airborne") return throwAirborne(world, fighter);
    if (call.name == "capture_cut_airborne") return captureCutAirborne(world, fighter);
    if (call.name == "enter_shield_break_fly") return enterShieldBreakFly(world, fighter);
    if (call.name == "enter_shield_break_fall") return enterShieldBreakFall(world, fighter);
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
    if (call.name == "process_airborne_fastfall") return processAirborneFastfall(world, fighter);
    if (call.name == "process_fall_special") return processFallSpecial(world, fighter);
    if (call.name == "process_item_screw") return processItemScrew(world, fighter);
    if (call.name == "enter_fall_special") return enterFallSpecial(world, fighter);
    if (call.name == "enter_pass") return enterPass(world, fighter);
    if (call.name == "fastfall_check") return fastfallCheck(world, fighter);
    if (call.name == "process_ground_jump") return processGroundJump(world, fighter);
    if (call.name == "process_jump_squat") return processJumpSquat(world, fighter);
    if (call.name == "enter_air_jump") return enterAirJump(world, fighter);
    if (call.name == "enter_item_screw") return enterItemScrew(world, fighter);
    if (call.name == "enter_escape_air") return enterEscapeAir(world, fighter);
    if (call.name == "enter_passive_wall_jump") return enterPassiveWallJump(world, fighter);
    if (call.name == "process_passive_wall_jump") return processPassiveWallJump(world, fighter);
    if (call.name == "process_passive_wall") return processPassiveWall(world, fighter);
    if (call.name == "process_passive_ceil") return processPassiveCeil(world, fighter);
    if (call.name == "enter_passive") return enterPassive(world, fighter);
    if (call.name == "enter_down_bound") return enterDownBound(world, fighter);
    if (call.name == "enter_tech_preserve_knockback") return enterTechPreserveKnockback(world, fighter);
    if (call.name == "process_escape_air") return processEscapeAir(world, fighter);
    if (call.name == "process_escape_ground") return processEscapeGround(world, fighter);
    if (call.name == "enter_turn") return enterTurn(world, fighter);
    if (call.name == "enter_turn_run") return enterTurnRun(world, fighter);
    if (call.name == "process_turn") return processTurn(world, fighter);
    if (call.name == "process_landing") return processLanding(world, fighter);
    if (call.name == "process_squat") return processSquat(world, fighter);
    if (call.name == "process_run_brake") return processRunBrake(world, fighter);
    if (call.name == "process_turn_run") return processTurnRun(world, fighter);
    if (call.name == "regular_airborne") return regularAirborne(world, fighter);
    if (call.name == "damage_airborne") return damageGroundToAir(world, fighter);
    if (call.name == "teeter_or_airborne") return teeterOrAirborne(world, fighter);
    if (call.name == "miss_foot_or_airborne") return missFootOrAirborne(world, fighter);
    if (call.name == "process_ottotto") return processOttotto(world, fighter);
    if (call.name == "regular_landing") return regularLanding(world, fighter);
    if (call.name == "aerial_landing_attack") return aerialLandingAttack(world, fighter);
    if (call.name == "escape_air_landing") return escapeAirLanding(world, fighter);
    if (call.name == "process_damage") return processDamage(world, fighter);
    if (call.name == "process_damage_bind") return processDamageBind(world, fighter);
    if (call.name == "enter_damage_ice") return enterDamageIce(world, fighter);
    if (call.name == "enter_damage_ice_jump") return enterDamageIceJump(world, fighter);
    if (call.name == "process_damage_ice") return processDamageIce(world, fighter);
    if (call.name == "process_damage_ice_jump") return processDamageIceJump(world, fighter);
    if (call.name == "enter_bury_jump") return enterBuryJump(world, fighter);
    if (call.name == "process_bury") return processBury(world, fighter);
    if (call.name == "process_bury_wait") return processBuryWait(world, fighter);
    if (call.name == "process_bury_jump") return processBuryJump(world, fighter);
    if (call.name == "release_bury") return releaseBury(world, fighter);
    if (call.name == "enter_rebound_stop") return enterReboundStop(world, fighter);
    if (call.name == "process_rebound_stop") return processReboundStop(world, fighter);
    if (call.name == "process_rebound") return processRebound(world, fighter);
    if (call.name == "process_miss_foot") return processMissFoot(world, fighter);
    if (call.name == "process_damage_song") return processDamageSong(world, fighter);
    if (call.name == "process_damage_song_wait") return processDamageSongWait(world, fighter);
    if (call.name == "process_damage_song_rv") return processDamageSongRv(world, fighter);
    if (call.name == "enter_furafura") return enterFurafura(world, fighter);
    if (call.name == "process_furafura") return processFurafura(world, fighter);
    if (call.name == "process_damage_screw") return processDamageScrew(world, fighter);
    if (call.name == "process_damage_fly") return processDamageFly(world, fighter);
    if (call.name == "process_damage_fall") return processDamageFall(world, fighter);
    if (call.name == "damage_landing") return damageLanding(world, fighter);
    if (call.name == "damage_fly_landing") return damageFlyLanding(world, fighter);
    if (call.name == "enter_clear_damage") return enterClearDamage(world, fighter);
    if (call.name == "enter_down_wait") return enterDownWait(world, fighter);
    if (call.name == "process_down_bound") return processDownBound(world, fighter);
    if (call.name == "process_down_damage") return processDownDamage(world, fighter);
    if (call.name == "process_down_wait") return processDownWait(world, fighter);
    if (call.name == "process_down_getup") return processDownGetup(world, fighter);
    if (call.name == "process_damage_surface") return processDamageSurface(world, fighter);
    if (call.name == "process_down_reflect") return processDownReflect(world, fighter);
    if (call.name == "down_reflect_landing") return downReflectLanding(world, fighter);
    if (call.name == "maintain_ledge") return maintainLedge(world, fighter);
    if (call.name == "enter_cliff_wait") return enterCliffWait(world, fighter);
    if (call.name == "process_cliff_wait") return processCliffWait(world, fighter);
    if (call.name == "process_cliff_climb") return processCliffClimb(world, fighter);
    if (call.name == "process_cliff_escape") return processCliffEscape(world, fighter);
    if (call.name == "process_cliff_attack") return processCliffAttack(world, fighter);
    if (call.name == "enter_cliff_drop") return enterCliffDrop(world, fighter);
    if (call.name == "process_cliff_jump") return processCliffJump(world, fighter);
    if (call.name == "process_cliff_jump_air") return processCliffJumpAir(world, fighter);
    if (call.name == "enter_cliff_jump_air") return enterCliffJumpAir(world, fighter);
}

void runStateFunctions(World& world, size_t fighterIndex, const std::vector<FunctionCall>& calls) {
    for (const FunctionCall& call : calls) {
        runStateFunction(world, fighterIndex, call);
    }
}

} // namespace pf
