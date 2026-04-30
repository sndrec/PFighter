#include "core/fighter_data.hpp"

#include <array>
#include <unordered_map>

namespace pf {

int FighterDefinition::stateIndex(const std::string& name) const {
    for (size_t i = 0; i < states.size(); ++i) {
        if (states[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

BoneId boneFromName(const std::string& name) {
    if (name == "HeadN" || name == "Head") return BoneId::Head;
    if (name == "HandL") return BoneId::HandL;
    if (name == "HandR") return BoneId::HandR;
    if (name == "FootL") return BoneId::FootL;
    if (name == "FootR") return BoneId::FootR;
    if (name == "Extra") return BoneId::Extra;
    return BoneId::Hip;
}

const char* boneName(BoneId bone) {
    switch (bone) {
        case BoneId::Hip: return "HipN";
        case BoneId::Head: return "HeadN";
        case BoneId::HandL: return "HandL";
        case BoneId::HandR: return "HandR";
        case BoneId::FootL: return "FootL";
        case BoneId::FootR: return "FootR";
        case BoneId::Extra: return "Extra";
        case BoneId::Count: break;
    }
    return "HipN";
}

static InterruptRule interrupt(std::string target, InterruptCondition condition, GroundRequirement ground = GroundRequirement::Any) {
    InterruptRule rule;
    rule.targetState = std::move(target);
    rule.condition = condition;
    rule.ground = ground;
    return rule;
}

static InterruptRule timedInterrupt(std::string target, InterruptCondition condition, GroundRequirement ground, int enableFrame, int disableFrame = 0) {
    InterruptRule rule = interrupt(std::move(target), condition, ground);
    rule.startActive = false;
    rule.enableFrame = enableFrame;
    rule.disableFrame = disableFrame;
    return rule;
}

static std::vector<InterruptRule> groundedAttackInterrupts(bool includeDashAttack = false) {
    std::vector<InterruptRule> rules = {
        interrupt("AttackS4Hi", InterruptCondition::AttackS4HiPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS4HiS", InterruptCondition::AttackS4HiSPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS4", InterruptCondition::AttackS4Pressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS4LwS", InterruptCondition::AttackS4LwSPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS4Lw", InterruptCondition::AttackS4LwPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackHi4", InterruptCondition::AttackHi4Pressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackLw4", InterruptCondition::AttackLw4Pressed, GroundRequirement::OnlyGrounded),
    };
    if (includeDashAttack) {
        rules.push_back(interrupt("AttackDash", InterruptCondition::AttackDashPressed, GroundRequirement::OnlyGrounded));
    }
    rules.insert(rules.end(), {
        interrupt("AttackS3Hi", InterruptCondition::AttackS3HiPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS3HiS", InterruptCondition::AttackS3HiSPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS3", InterruptCondition::AttackS3Pressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS3LwS", InterruptCondition::AttackS3LwSPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS3Lw", InterruptCondition::AttackS3LwPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackHi3", InterruptCondition::AttackHi3Pressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackLw3", InterruptCondition::AttackLw3Pressed, GroundRequirement::OnlyGrounded),
        interrupt("Attack11", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
    });
    return rules;
}

static void appendRules(std::vector<InterruptRule>& destination, std::vector<InterruptRule> source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

static void expandGroundedAttackShortcut(FighterState& state, bool includeDashAttack = false) {
    for (auto it = state.interrupts.begin(); it != state.interrupts.end(); ++it) {
        if (it->condition == InterruptCondition::AttackPressed &&
            (it->targetState == "Jab" || it->targetState == "Attack11") &&
            it->ground == GroundRequirement::OnlyGrounded)
        {
            const auto replacement = groundedAttackInterrupts(includeDashAttack);
            it = state.interrupts.erase(it);
            state.interrupts.insert(it, replacement.begin(), replacement.end());
            return;
        }
    }
}

static Subaction wait(int frames) {
    Subaction sub;
    sub.type = SubactionType::SyncTimer;
    sub.frames = frames;
    return sub;
}

static Subaction clearHitboxes() {
    Subaction sub;
    sub.type = SubactionType::ClearHitboxes;
    return sub;
}

static Subaction setFlag(int flag, bool on) {
    Subaction sub;
    sub.type = SubactionType::SetFlag;
    sub.flag = flag;
    sub.flagValue = on;
    return sub;
}

static Subaction setHurtboxState(int index, HurtboxState state) {
    Subaction sub;
    sub.type = SubactionType::SetHurtboxState;
    sub.hurtboxIndex = index;
    sub.hurtboxState = state;
    return sub;
}

static FunctionCall call(std::string name, bool boolParam = false) {
    FunctionCall fn;
    fn.name = std::move(name);
    fn.boolParam = boolParam;
    return fn;
}

static Subaction createHitbox(int id, BoneId bone, Vec3 offset, Fix radius, Fix damage, Fix angle, Fix base, Fix growth) {
    Subaction sub;
    sub.type = SubactionType::CreateHitbox;
    sub.hitbox.hitboxId = id;
    sub.hitbox.bone = bone;
    sub.hitbox.offset = offset;
    sub.hitbox.radius = radius;
    sub.hitbox.damage = damage;
    sub.hitbox.damageShield = 0;
    sub.hitbox.knockbackAngleDegrees = angle;
    sub.hitbox.knockbackBase = base;
    sub.hitbox.knockbackGrowth = growth;
    return sub;
}

static void assignMeleeActionIndices(FighterDefinition& fighter) {
    const std::unordered_map<std::string, int> actionByAnimation = {
        {"Wait", 2},
        {"WalkSlow", 7},
        {"WalkMiddle", 8},
        {"WalkFast", 9},
        {"Turn", 10},
        {"TurnRun", 11},
        {"Dash", 12},
        {"Run", 13},
        {"RunBrake", 14},
        {"JumpSquat", 15},
        {"KneeBend", 15},
        {"JumpF", 16},
        {"JumpB", 17},
        {"JumpAerialF", 18},
        {"JumpAerialB", 19},
        {"Fall", 20},
        {"FallSpecial", 26},
        {"Landing", 35},
        {"LandingFallSpecial", 36},
        {"GuardOn", 37},
        {"GuardReflect", 37},
        {"Guard", 38},
        {"GuardOff", 39},
        {"GuardSetOff", 40},
        {"EscapeN", 41},
        {"EscapeF", 42},
        {"EscapeB", 43},
        {"EscapeAir", 44},
        {"Jab", 46},
        {"Attack11", 46},
        {"Attack12", 47},
        {"Attack13", 48},
        {"AttackDash", 52},
        {"AttackS3Hi", 53},
        {"AttackS3HiS", 54},
        {"AttackS3", 55},
        {"AttackS3LwS", 56},
        {"AttackS3Lw", 57},
        {"AttackHi3", 58},
        {"AttackLw3", 59},
        {"AttackS4Hi", 60},
        {"AttackS4HiS", 61},
        {"AttackS4", 62},
        {"AttackS4LwS", 63},
        {"AttackS4Lw", 64},
        {"AttackHi4", 66},
        {"AttackLw4", 67},
        {"AirAttackN", 68},
        {"AirAttackF", 69},
        {"AirAttackB", 70},
        {"AirAttackHi", 71},
        {"AirAttackLw", 72},
        {"LandingAirN", 73},
        {"LandingAirF", 74},
        {"LandingAirB", 75},
        {"LandingAirHi", 76},
        {"LandingAirLw", 77},
        {"DamageHi1", 165},
        {"DamageHi2", 166},
        {"DamageHi3", 167},
        {"DamageN1", 168},
        {"DamageN2", 169},
        {"DamageN3", 170},
        {"DamageLw1", 171},
        {"DamageLw2", 172},
        {"DamageLw3", 173},
        {"DamageAir1", 174},
        {"DamageAir2", 175},
        {"DamageAir3", 176},
        {"DamageFlyHi", 177},
        {"DamageFlyN", 178},
        {"DamageFlyLw", 179},
        {"DamageFlyTop", 180},
        {"DamageFlyRoll", 181},
        {"DownBoundU", 183},
        {"DownWaitU", 184},
        {"DownDamageU", 185},
        {"DownStandU", 186},
        {"DownAttackU", 187},
        {"DownForwardU", 188},
        {"DownBackU", 189},
        {"DownSpotU", 190},
        {"DownBoundD", 191},
        {"DownWaitD", 192},
        {"DownDamageD", 193},
        {"DownStandD", 194},
        {"DownAttackD", 195},
        {"DownForwardD", 196},
        {"DownBackD", 197},
        {"DownSpotD", 198},
        {"Passive", 199},
        {"PassiveStandF", 200},
        {"PassiveStandB", 201},
        {"PassiveWall", 202},
        {"PassiveWallJump", 203},
        {"PassiveCeil", 204},
        {"ShieldBreakFly", 205},
        {"ShieldBreakFall", 206},
        {"ShieldBreakDownU", 207},
        {"ShieldBreakDownD", 208},
        {"ShieldBreakStandU", 209},
        {"ShieldBreakStandD", 210},
        {"Furafura", 205},
        {"Catch", 242},
        {"CatchPull", 242},
        {"CatchDash", 243},
        {"CatchDashPull", 243},
        {"CatchWait", 244},
        {"CatchAttack", 245},
        {"CatchCut", 246},
        {"ThrowF", 247},
        {"ThrowB", 248},
        {"ThrowHi", 249},
        {"ThrowLw", 250},
        {"CapturePulledHi", 251},
        {"CaptureWaitHi", 252},
        {"CaptureDamageHi", 253},
        {"CapturePulledLw", 254},
        {"CaptureWaitLw", 255},
        {"CaptureDamageLw", 256},
        {"CaptureCut", 257},
        {"CaptureJump", 258},
        {"ThrownF", 262},
        {"ThrownB", 263},
        {"ThrownHi", 264},
        {"ThrownLw", 265},
        {"ThrownLwWomen", 266},
        {"Pass", 209},
        {"Ottotto", 210},
        {"OttottoWait", 211},
        {"Squat", 30},
        {"SquatWait", 31},
        {"SquatRv", 34},
        {"PassiveWallJump", 203},
        {"FlyReflectWall", 247},
        {"FlyReflectCeil", 248},
        {"StopWall", 213},
        {"StopCeil", 214},
        {"MissFoot", 215},
        {"CliffCatch", 216},
        {"CliffWait", 217},
        {"CliffClimbSlow", 219},
        {"CliffClimbQuick", 220},
        {"CliffAttackSlow", 221},
        {"CliffAttackQuick", 222},
        {"CliffEscapeSlow", 223},
        {"CliffEscapeQuick", 224},
        {"CliffJumpSlow1", 225},
        {"CliffJumpSlow2", 226},
        {"CliffJumpQuick1", 227},
        {"CliffJumpQuick2", 228},
    };
    const std::unordered_map<std::string, int> actionByState = {
        {"ShieldBreakDown", 207},
        {"ShieldBreakStand", 209},
        {"CliffClimb", 220},
        {"CliffAttack", 222},
        {"CliffEscape", 224},
        {"CliffJump", 227},
        {"CliffJumpAir", 228},
    };
    for (FighterState& state : fighter.states) {
        if (const auto byState = actionByState.find(state.name); byState != actionByState.end()) {
            state.animationActionIndex = byState->second;
            continue;
        }
        if (const auto byAnimation = actionByAnimation.find(state.animation); byAnimation != actionByAnimation.end()) {
            state.animationActionIndex = byAnimation->second;
        }
    }
}

FighterDefinition makeDebugRook() {
    FighterDefinition fighter;
    fighter.name = "Rook";
    fighter.shield.maxHealth = fighter.properties.common.startShieldHealthX260;
    fighter.shield.startSizeHardShield = fighter.properties.initialShieldSize;

    fighter.hurtboxes = {
        {BoneId::Hip, {0, 0, 0}, {0, fxFromFloat(1.0f), 0}, fxFromFloat(0.55f)},
        {BoneId::Head, {0, 0, 0}, {0, fxFromFloat(0.35f), 0}, fxFromFloat(0.42f)},
        {BoneId::HandL, {0, 0, 0}, {0, 0, 0}, fxFromFloat(0.26f)},
        {BoneId::HandR, {0, 0, 0}, {0, 0, 0}, fxFromFloat(0.26f)},
    };

    FighterState waitState;
    waitState.name = "Wait";
    waitState.animation = "Wait";
    waitState.animationLengthFrames = 60;
    waitState.loopAnimation = true;
    waitState.onEnter = {call("common_enter")};
    waitState.onFrame = {call("process_grounded")};
    waitState.onAirborne = {call("teeter_or_airborne")};
    waitState.interrupts = {
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Jab", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
        interrupt("EscapeN", InterruptCondition::SpotDodgeInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        interrupt("Squat", InterruptCondition::SquatInput, GroundRequirement::OnlyGrounded),
        interrupt("Dash", InterruptCondition::DashInput, GroundRequirement::OnlyGrounded),
        interrupt("Turn", InterruptCondition::TurnInput, GroundRequirement::OnlyGrounded),
        interrupt("WalkSlow", InterruptCondition::TeeterWalkInput, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState walkSlow = waitState;
    walkSlow.name = "WalkSlow";
    walkSlow.animation = "WalkSlow";
    walkSlow.onFrame = {call("process_grounded"), call("process_walk")};
    walkSlow.interrupts = {
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Jab", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
        interrupt("EscapeN", InterruptCondition::SpotDodgeInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        interrupt("Squat", InterruptCondition::SquatInput, GroundRequirement::OnlyGrounded),
        interrupt("Dash", InterruptCondition::DashInput, GroundRequirement::OnlyGrounded),
        interrupt("Turn", InterruptCondition::TurnInput, GroundRequirement::OnlyGrounded),
        interrupt("WalkMiddle", InterruptCondition::HorizontalWalkMiddle, GroundRequirement::OnlyGrounded),
        interrupt("Wait", InterruptCondition::WaitInput, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState walkMiddle = walkSlow;
    walkMiddle.name = "WalkMiddle";
    walkMiddle.animation = "WalkMiddle";
    walkMiddle.interrupts = {
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Jab", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
        interrupt("EscapeN", InterruptCondition::SpotDodgeInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        interrupt("Squat", InterruptCondition::SquatInput, GroundRequirement::OnlyGrounded),
        interrupt("Dash", InterruptCondition::DashInput, GroundRequirement::OnlyGrounded),
        interrupt("Turn", InterruptCondition::TurnInput, GroundRequirement::OnlyGrounded),
        interrupt("WalkFast", InterruptCondition::HorizontalWalkFast, GroundRequirement::OnlyGrounded),
        interrupt("WalkSlow", InterruptCondition::HorizontalWalkSlow, GroundRequirement::OnlyGrounded),
        interrupt("Wait", InterruptCondition::WaitInput, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState walkFast = walkSlow;
    walkFast.name = "WalkFast";
    walkFast.animation = "WalkFast";
    walkFast.interrupts = {
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Jab", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
        interrupt("EscapeN", InterruptCondition::SpotDodgeInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        interrupt("Squat", InterruptCondition::SquatInput, GroundRequirement::OnlyGrounded),
        interrupt("Dash", InterruptCondition::DashInput, GroundRequirement::OnlyGrounded),
        interrupt("Turn", InterruptCondition::TurnInput, GroundRequirement::OnlyGrounded),
        interrupt("WalkMiddle", InterruptCondition::HorizontalWalkMiddle, GroundRequirement::OnlyGrounded),
        interrupt("Wait", InterruptCondition::WaitInput, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState dashState;
    dashState.name = "Dash";
    dashState.animation = "Dash";
    dashState.animationLengthFrames = fighter.properties.common.dashLateInterruptWindowX4C;
    dashState.onEnter = {call("common_enter"), call("enter_dash")};
    dashState.onFrame = {call("process_dash")};
    dashState.onAirborne = {call("regular_airborne")};
    dashState.action = {
        wait(8),
        setFlag(0, true),
    };
    dashState.interrupts = {
        interrupt("JumpSquat", InterruptCondition::RunJumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Jab", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
        interrupt("Run", InterruptCondition::RunInput, GroundRequirement::OnlyGrounded),
        timedInterrupt("Turn", InterruptCondition::ReverseDashInput, GroundRequirement::OnlyGrounded, fighter.properties.common.dashEarlyInterruptWindowX44 + 1),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState runState;
    runState.name = "Run";
    runState.animation = "Run";
    runState.animationLengthFrames = 60;
    runState.loopAnimation = true;
    runState.onEnter = {call("common_enter"), call("enter_run")};
    runState.onFrame = {call("process_run")};
    runState.onAirborne = {call("regular_airborne")};
    runState.interrupts = {
        interrupt("JumpSquat", InterruptCondition::RunJumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Jab", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        interrupt("TurnRun", InterruptCondition::TurnRunInput, GroundRequirement::OnlyGrounded),
        interrupt("RunBrake", InterruptCondition::RunBrakeInput, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState runDirect = runState;
    runDirect.name = "RunDirect";
    runDirect.animation = "Run";
    runDirect.onEnter = {call("common_enter"), call("enter_run_direct")};
    runDirect.onFrame = {call("process_run_direct")};
    runDirect.interrupts = {
        interrupt("JumpSquat", InterruptCondition::RunJumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Jab", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState runBrake;
    runBrake.name = "RunBrake";
    runBrake.animation = "RunBrake";
    runBrake.animationLengthFrames = fighter.properties.maxRunBrakeFrames;
    runBrake.onEnter = {call("common_enter"), call("enter_run_brake")};
    runBrake.onFrame = {call("process_run_brake")};
    runBrake.onAirborne = {call("teeter_or_airborne")};
    runBrake.onAnimationFinishedState = "Wait";
    runBrake.interrupts = {
        interrupt("JumpSquat", InterruptCondition::RunJumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState turnState;
    turnState.name = "Turn";
    turnState.animation = "Turn";
    turnState.animationLengthFrames = 11;
    turnState.onEnter = {call("common_enter"), call("enter_turn")};
    turnState.onFrame = {call("process_turn")};
    turnState.onAirborne = {call("teeter_or_airborne")};
    turnState.onAnimationFinishedState = "Wait";
    turnState.onAnimationFinishedBlendFrames = kDisableAnimationBlendFrames;
    turnState.interrupts = {
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Jab", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        interrupt("Squat", InterruptCondition::SquatInput, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState turnRun;
    turnRun.name = "TurnRun";
    turnRun.animation = "TurnRun";
    turnRun.animationLengthFrames = 16;
    turnRun.onEnter = {call("common_enter"), call("enter_turn_run")};
    turnRun.onFrame = {call("process_turn_run")};
    turnRun.onAnimationFinishedState = "";
    turnRun.interrupts = {
        interrupt("JumpSquat", InterruptCondition::RunJumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState jumpSquat;
    jumpSquat.name = "JumpSquat";
    jumpSquat.animation = "JumpSquat";
    jumpSquat.animationLengthFrames = fighter.properties.jumpStartupLag;
    jumpSquat.onEnter = {call("common_enter")};
    jumpSquat.onFrame = {call("process_jump_squat")};

    FighterState squat;
    squat.name = "Squat";
    squat.animation = "Squat";
    squat.animationLengthFrames = 4;
    squat.onEnter = {call("common_enter")};
    squat.onFrame = {call("process_squat")};
    squat.onAirborne = {call("teeter_or_airborne")};
    squat.onAnimationFinishedState = "SquatWait";
    squat.interrupts = {
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Jab", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState squatWait = squat;
    squatWait.name = "SquatWait";
    squatWait.animation = "SquatWait";
    squatWait.animationLengthFrames = 60;
    squatWait.loopAnimation = true;
    squatWait.onAnimationFinishedState = "";
    squatWait.interrupts = {
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Jab", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
        interrupt("Dash", InterruptCondition::DashInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        interrupt("SquatRv", InterruptCondition::SquatReleaseInput, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState squatRv;
    squatRv.name = "SquatRv";
    squatRv.animation = "SquatRv";
    squatRv.animationLengthFrames = 4;
    squatRv.onEnter = {call("common_enter")};
    squatRv.onFrame = {call("process_landing")};
    squatRv.onAirborne = {call("teeter_or_airborne")};
    squatRv.onAnimationFinishedState = "Wait";
    squatRv.interrupts = {
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Jab", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        interrupt("WalkSlow", InterruptCondition::HorizontalWalkSlow, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState fallState;
    fallState.name = "Fall";
    fallState.animation = "Fall";
    fallState.animationLengthFrames = 60;
    fallState.loopAnimation = true;
    fallState.onEnter = {call("common_enter")};
    fallState.onFrame = {call("process_airborne"), call("fastfall_check")};
    fallState.onLanding = {call("regular_landing")};
    fallState.interrupts = {
        interrupt("PassiveWallJump", InterruptCondition::WallJumpInput, GroundRequirement::OnlyAirborne),
        interrupt("EscapeAir", InterruptCondition::AirDodgePressed, GroundRequirement::OnlyAirborne),
        interrupt("JumpAerialB", InterruptCondition::AerialJumpBackwardPressed, GroundRequirement::OnlyAirborne),
        interrupt("JumpAerialF", InterruptCondition::AerialJumpForwardPressed, GroundRequirement::OnlyAirborne),
        interrupt("AirAttackHi", InterruptCondition::AerialAttackHiPressed, GroundRequirement::OnlyAirborne),
        interrupt("AirAttackLw", InterruptCondition::AerialAttackLwPressed, GroundRequirement::OnlyAirborne),
        interrupt("AirAttackF", InterruptCondition::AerialAttackFPressed, GroundRequirement::OnlyAirborne),
        interrupt("AirAttackB", InterruptCondition::AerialAttackBPressed, GroundRequirement::OnlyAirborne),
        interrupt("AirAttackN", InterruptCondition::AerialAttackNPressed, GroundRequirement::OnlyAirborne),
    };

    FighterState attack11;
    attack11.name = "Attack11";
    attack11.animation = "Attack11";
    attack11.animationLengthFrames = 28;
    attack11.onEnter = {call("common_enter")};
    attack11.onFrame = {call("process_landing")};
    attack11.onAirborne = {call("regular_airborne")};
    attack11.onAnimationFinishedState = "Wait";
    attack11.useAnimPhysics = true;
    attack11.action = {
        wait(3),
        createHitbox(0, BoneId::HandR, {fxFromFloat(0.8f), fxFromFloat(0.2f), 0}, fxFromFloat(0.45f), fx(4), fx(45), fx(22), fx(80)),
        wait(3),
        clearHitboxes(),
    };
    attack11.interrupts = {
        interrupt("Attack12", InterruptCondition::JabFollowupPressed, GroundRequirement::OnlyGrounded),
    };

    FighterState attack12 = attack11;
    attack12.name = "Attack12";
    attack12.animation = "Attack12";
    attack12.interrupts = {
        interrupt("Attack13", InterruptCondition::JabFollowupPressed, GroundRequirement::OnlyGrounded),
    };

    FighterState attack13 = attack11;
    attack13.name = "Attack13";
    attack13.animation = "Attack13";
    attack13.interrupts.clear();

    auto makeGroundedAttack = [&](std::string name) {
        FighterState state = attack11;
        state.name = name;
        state.animation = name;
        state.action.clear();
        state.interrupts = groundedAttackInterrupts(false);
        state.initialInterruptibleFrame = 1000000;
        state.onAnimationFinishedState = "Wait";
        state.useAnimPhysics = true;
        return state;
    };

    FighterState attackDash = makeGroundedAttack("AttackDash");
    FighterState attackS3Hi = makeGroundedAttack("AttackS3Hi");
    FighterState attackS3HiS = makeGroundedAttack("AttackS3HiS");
    FighterState attackS3 = makeGroundedAttack("AttackS3");
    FighterState attackS3LwS = makeGroundedAttack("AttackS3LwS");
    FighterState attackS3Lw = makeGroundedAttack("AttackS3Lw");
    FighterState attackHi3 = makeGroundedAttack("AttackHi3");
    FighterState attackLw3 = makeGroundedAttack("AttackLw3");
    FighterState attackS4Hi = makeGroundedAttack("AttackS4Hi");
    FighterState attackS4HiS = makeGroundedAttack("AttackS4HiS");
    FighterState attackS4 = makeGroundedAttack("AttackS4");
    FighterState attackS4LwS = makeGroundedAttack("AttackS4LwS");
    FighterState attackS4Lw = makeGroundedAttack("AttackS4Lw");
    FighterState attackHi4 = makeGroundedAttack("AttackHi4");
    FighterState attackLw4 = makeGroundedAttack("AttackLw4");

    FighterState airAttack;
    airAttack.name = "AirAttackN";
    airAttack.animation = "AirAttackN";
    airAttack.animationLengthFrames = 36;
    airAttack.allowLedgeGrab = false;
    airAttack.allowWallCollision = false;
    airAttack.allowCeilingCollision = false;
    airAttack.onEnter = {call("common_enter")};
    airAttack.onFrame = {call("process_airborne"), call("fastfall_check")};
    airAttack.onLanding = {call("aerial_landing_attack")};
    airAttack.onAnimationFinishedState = "Fall";
    airAttack.initialInterruptibleFrame = 1000000;
    airAttack.interrupts = fallState.interrupts;
    airAttack.action = {
        wait(4),
        createHitbox(0, BoneId::HandR, {fxFromFloat(0.65f), 0, 0}, fxFromFloat(0.5f), fx(7), fx(55), fx(24), fx(90)),
        createHitbox(1, BoneId::HandL, {fxFromFloat(-0.65f), 0, 0}, fxFromFloat(0.5f), fx(7), fx(125), fx(24), fx(90)),
        wait(5),
        clearHitboxes(),
    };

    FighterState airAttackF = airAttack;
    airAttackF.name = "AirAttackF";
    airAttackF.animation = "AirAttackF";
    airAttackF.action.clear();

    FighterState airAttackB = airAttack;
    airAttackB.name = "AirAttackB";
    airAttackB.animation = "AirAttackB";
    airAttackB.action.clear();

    FighterState airAttackHi = airAttack;
    airAttackHi.name = "AirAttackHi";
    airAttackHi.animation = "AirAttackHi";
    airAttackHi.action.clear();

    FighterState airAttackLw = airAttack;
    airAttackLw.name = "AirAttackLw";
    airAttackLw.animation = "AirAttackLw";
    airAttackLw.action.clear();

    FighterState landing;
    landing.name = "Landing";
    landing.animation = "Landing";
    landing.animationLengthFrames = 14;
    landing.onEnter = {call("common_enter")};
    landing.onFrame = {call("process_landing")};
    landing.onAirborne = {call("teeter_or_airborne")};
    landing.onAnimationFinishedState = "Wait";
    landing.interrupts = {
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Jab", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        interrupt("Squat", InterruptCondition::SquatInput, GroundRequirement::OnlyGrounded),
        interrupt("Dash", InterruptCondition::DashInput, GroundRequirement::OnlyGrounded),
        interrupt("Turn", InterruptCondition::TurnInput, GroundRequirement::OnlyGrounded),
        interrupt("WalkSlow", InterruptCondition::TeeterWalkInput, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState landingAirN;
    landingAirN.name = "LandingAirN";
    landingAirN.animation = "LandingAirN";
    landingAirN.animationLengthFrames = fighter.properties.nairLandingLag;
    landingAirN.onEnter = {call("common_enter")};
    landingAirN.onFrame = {call("process_landing")};
    landingAirN.onAirborne = {call("teeter_or_airborne")};
    landingAirN.onAnimationFinishedState = "Wait";

    FighterState landingAirF = landingAirN;
    landingAirF.name = "LandingAirF";
    landingAirF.animation = "LandingAirF";
    landingAirF.animationLengthFrames = fighter.properties.fairLandingLag;

    FighterState landingAirB = landingAirN;
    landingAirB.name = "LandingAirB";
    landingAirB.animation = "LandingAirB";
    landingAirB.animationLengthFrames = fighter.properties.bairLandingLag;

    FighterState landingAirHi = landingAirN;
    landingAirHi.name = "LandingAirHi";
    landingAirHi.animation = "LandingAirHi";
    landingAirHi.animationLengthFrames = fighter.properties.uairLandingLag;

    FighterState landingAirLw = landingAirN;
    landingAirLw.name = "LandingAirLw";
    landingAirLw.animation = "LandingAirLw";
    landingAirLw.animationLengthFrames = fighter.properties.dairLandingLag;

    FighterState landingFallSpecial;
    landingFallSpecial.name = "LandingFallSpecial";
    landingFallSpecial.animation = "LandingFallSpecial";
    landingFallSpecial.animationLengthFrames = fighter.properties.common.landingFallSpecialLagX344;
    landingFallSpecial.onEnter = {call("common_enter")};
    landingFallSpecial.onFrame = {call("process_landing")};
    landingFallSpecial.onAirborne = {call("teeter_or_airborne")};
    landingFallSpecial.onAnimationFinishedState = "Wait";

    auto makeDamageState = [&](const std::string& name, int length) {
        FighterState state;
        state.name = name;
        state.animation = name;
        state.animationLengthFrames = length;
        state.allowLedgeGrab = false;
        state.onEnter = {call("common_enter")};
        state.onFrame = {call("process_damage")};
        state.onLanding = {call("damage_fly_landing")};
        return state;
    };

    FighterState damageHi1 = makeDamageState("DamageHi1", 18);
    FighterState damageHi2 = makeDamageState("DamageHi2", 24);
    FighterState damageHi3 = makeDamageState("DamageHi3", 30);
    FighterState damageN1 = makeDamageState("DamageN1", 18);
    FighterState damageN2 = makeDamageState("DamageN2", 24);
    FighterState damageN3 = makeDamageState("DamageN3", 30);
    FighterState damageLw1 = makeDamageState("DamageLw1", 18);
    FighterState damageLw2 = makeDamageState("DamageLw2", 24);
    FighterState damageLw3 = makeDamageState("DamageLw3", 30);
    FighterState damageAir1 = makeDamageState("DamageAir1", 18);
    FighterState damageAir2 = makeDamageState("DamageAir2", 24);
    FighterState damageAir3 = makeDamageState("DamageAir3", 30);

    auto makeDamageFlyState = [&](const std::string& name) {
        FighterState state = makeDamageState(name, 40);
        state.onFrame = {call("process_damage_fly")};
        state.onLanding = {call("damage_fly_landing")};
        state.allowLedgeGrab = false;
        return state;
    };
    FighterState damageFlyHi = makeDamageFlyState("DamageFlyHi");
    FighterState damageFlyN = makeDamageFlyState("DamageFlyN");
    FighterState damageFlyLw = makeDamageFlyState("DamageFlyLw");
    FighterState damageFlyTop = makeDamageFlyState("DamageFlyTop");
    FighterState damageFlyRoll = makeDamageFlyState("DamageFlyRoll");

    FighterState damageFall = fallState;
    damageFall.name = "DamageFall";
    damageFall.animation = "DamageFall";
    damageFall.animationLengthFrames = 60;
    damageFall.loopAnimation = true;
    damageFall.allowLedgeGrab = true;
    damageFall.onEnter = {call("common_enter")};
    damageFall.onFrame = {call("process_damage_fall")};
    damageFall.onLanding = {call("damage_fall_landing")};
    damageFall.interrupts = fallState.interrupts;

    FighterState downBoundU;
    downBoundU.name = "DownBoundU";
    downBoundU.animation = "DownBoundU";
    downBoundU.animationLengthFrames = 18;
    downBoundU.onEnter = {call("common_enter"), call("enter_clear_damage")};
    downBoundU.onFrame = {call("process_landing")};
    downBoundU.onAnimationFinishedState = "DownWaitU";

    FighterState downWaitU = downBoundU;
    downWaitU.name = "DownWaitU";
    downWaitU.animation = "DownWaitU";
    downWaitU.animationLengthFrames = fighter.properties.common.downWaitAutoStandFramesX424;
    downWaitU.onEnter = {call("common_enter"), call("enter_down_wait")};
    downWaitU.onFrame = {call("process_down_wait")};
    downWaitU.onAnimationFinishedState.clear();

    FighterState downDamageU = downWaitU;
    downDamageU.name = "DownDamageU";
    downDamageU.animation = "DownDamageU";
    downDamageU.animationLengthFrames = 18;
    downDamageU.onEnter = {call("common_enter")};
    downDamageU.onFrame = {call("process_down_getup")};
    downDamageU.onAnimationFinishedState = "DownWaitU";

    auto makeDownGetupState = [&](const std::string& name, int length) {
        FighterState state;
        state.name = name;
        state.animation = name;
        state.animationLengthFrames = length;
        state.onEnter = {call("common_enter"), call("enter_clear_damage")};
        state.onFrame = {call("process_down_getup")};
        state.onAirborne = {call("regular_airborne")};
        state.onAnimationFinishedState = "Wait";
        return state;
    };
    FighterState downStandU = makeDownGetupState("DownStandU", 30);
    FighterState downAttackU = makeDownGetupState("DownAttackU", 50);
    FighterState downForwardU = makeDownGetupState("DownForwardU", 35);
    downForwardU.useAnimPhysics = true;
    FighterState downBackU = makeDownGetupState("DownBackU", 35);
    downBackU.useAnimPhysics = true;
    FighterState downSpotU = makeDownGetupState("DownSpotU", 26);
    downSpotU.onAnimationFinishedState = "DownWaitU";

    FighterState downBoundD = downBoundU;
    downBoundD.name = "DownBoundD";
    downBoundD.animation = "DownBoundD";
    downBoundD.onAnimationFinishedState = "DownWaitD";

    FighterState downWaitD = downWaitU;
    downWaitD.name = "DownWaitD";
    downWaitD.animation = "DownWaitD";

    FighterState downDamageD = downWaitU;
    downDamageD.name = "DownDamageD";
    downDamageD.animation = "DownDamageD";
    downDamageD.animationLengthFrames = 18;
    downDamageD.onEnter = {call("common_enter")};
    downDamageD.onFrame = {call("process_down_getup")};
    downDamageD.onAnimationFinishedState = "DownWaitD";

    FighterState downStandD = downStandU;
    downStandD.name = "DownStandD";
    downStandD.animation = "DownStandD";
    FighterState downAttackD = downAttackU;
    downAttackD.name = "DownAttackD";
    downAttackD.animation = "DownAttackD";
    FighterState downForwardD = downForwardU;
    downForwardD.name = "DownForwardD";
    downForwardD.animation = "DownForwardD";
    FighterState downBackD = downBackU;
    downBackD.name = "DownBackD";
    downBackD.animation = "DownBackD";
    FighterState downSpotD = downSpotU;
    downSpotD.name = "DownSpotD";
    downSpotD.animation = "DownSpotD";
    downSpotD.onAnimationFinishedState = "DownWaitD";

    auto makePassiveState = [&](const std::string& name, int length, const std::string& finished) {
        FighterState state;
        state.name = name;
        state.animation = name;
        state.animationLengthFrames = length;
        state.onEnter = {call("common_enter"), call("enter_clear_damage")};
        state.onFrame = {call("process_landing")};
        state.onAirborne = {call("regular_airborne")};
        state.onAnimationFinishedState = finished;
        return state;
    };
    FighterState passive = makePassiveState("Passive", 26, "Wait");
    FighterState passiveStandF = makePassiveState("PassiveStandF", 40, "Wait");
    passiveStandF.useAnimPhysics = true;
    FighterState passiveStandB = makePassiveState("PassiveStandB", 40, "Wait");
    passiveStandB.useAnimPhysics = true;

    FighterState passiveWall = makePassiveState("PassiveWall", 28, "Fall");
    passiveWall.onFrame = {call("process_airborne")};
    passiveWall.onLanding = {call("regular_landing")};
    FighterState passiveCeil = passiveWall;
    passiveCeil.name = "PassiveCeil";
    passiveCeil.animation = "PassiveCeil";

    FighterState wallDamage = makeDamageFlyState("WallDamage");
    wallDamage.animationLengthFrames = 16;
    wallDamage.onFrame = {call("process_damage_surface")};
    wallDamage.onLanding = {call("damage_fly_landing")};

    FighterState stopCeil = wallDamage;
    stopCeil.name = "StopCeil";
    stopCeil.animation = "StopCeil";

    FighterState pass;
    pass.name = "Pass";
    pass.animation = "Pass";
    pass.animationLengthFrames = fighter.properties.common.platformDropAnimationFramesX470;
    pass.onEnter = {call("common_enter"), call("enter_pass")};
    pass.onFrame = {call("process_airborne")};
    pass.onLanding = {call("regular_landing")};
    pass.onAnimationFinishedState = "Fall";

    FighterState jumpF = fallState;
    jumpF.name = "JumpF";
    jumpF.animation = "JumpF";
    jumpF.animationLengthFrames = 40;
    jumpF.loopAnimation = false;
    jumpF.onAnimationFinishedState = "Fall";

    FighterState jumpB = fallState;
    jumpB.name = "JumpB";
    jumpB.animation = "JumpB";
    jumpB.animationLengthFrames = 40;
    jumpB.loopAnimation = false;
    jumpB.onAnimationFinishedState = "Fall";

    FighterState jumpAerialF = fallState;
    jumpAerialF.name = "JumpAerialF";
    jumpAerialF.animation = "JumpAerialF";
    jumpAerialF.animationLengthFrames = 40;
    jumpAerialF.loopAnimation = false;
    jumpAerialF.onEnter = {call("common_enter"), call("enter_air_jump")};
    jumpAerialF.onAnimationFinishedState = "Fall";

    FighterState jumpAerialB = jumpAerialF;
    jumpAerialB.name = "JumpAerialB";
    jumpAerialB.animation = "JumpAerialB";

    FighterState escapeAir;
    escapeAir.name = "EscapeAir";
    escapeAir.animation = "EscapeAir";
    escapeAir.animationLengthFrames = 49;
    escapeAir.allowLedgeGrab = false;
    escapeAir.onEnter = {call("common_enter"), call("enter_escape_air")};
    escapeAir.onFrame = {call("process_escape_air")};
    escapeAir.onLanding = {call("escape_air_landing")};
    escapeAir.onAnimationFinishedState = "FallSpecial";

    FighterState fallSpecial;
    fallSpecial.name = "FallSpecial";
    fallSpecial.animation = "FallSpecial";
    fallSpecial.animationLengthFrames = 60;
    fallSpecial.loopAnimation = true;
    fallSpecial.allowCeilingCollision = false;
    fallSpecial.onEnter = {call("common_enter")};
    fallSpecial.onFrame = {call("process_fall_special")};
    fallSpecial.onLanding = {call("escape_air_landing")};

    FighterState passiveWallJump;
    passiveWallJump.name = "PassiveWallJump";
    passiveWallJump.animation = "PassiveWallJump";
    passiveWallJump.animationLengthFrames = 34;
    passiveWallJump.onEnter = {call("common_enter"), call("enter_passive_wall_jump")};
    passiveWallJump.onFrame = {call("process_passive_wall_jump")};
    passiveWallJump.onLanding = {call("regular_landing")};
    passiveWallJump.onAnimationFinishedState = "Fall";

    FighterState ottotto;
    ottotto.name = "Ottotto";
    ottotto.animation = "Ottotto";
    ottotto.animationLengthFrames = 25;
    ottotto.onEnter = {call("common_enter")};
    ottotto.onFrame = {call("process_ottotto")};
    ottotto.onAnimationFinishedState = "OttottoWait";
    ottotto.interrupts = {
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Jab", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        interrupt("Squat", InterruptCondition::SquatInput, GroundRequirement::OnlyGrounded),
        interrupt("Dash", InterruptCondition::DashInput, GroundRequirement::OnlyGrounded),
        interrupt("Turn", InterruptCondition::TurnInput, GroundRequirement::OnlyGrounded),
        interrupt("WalkSlow", InterruptCondition::TeeterWalkInput, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState ottottoWait = ottotto;
    ottottoWait.name = "OttottoWait";
    ottottoWait.animation = "OttottoWait";
    ottottoWait.animationLengthFrames = 60;
    ottottoWait.loopAnimation = true;
    ottottoWait.onAnimationFinishedState = "";

    const std::vector<InterruptRule> guardInterrupts = {
        interrupt("EscapeN", InterruptCondition::SpotDodgeInput, GroundRequirement::OnlyGrounded),
        interrupt("EscapeF", InterruptCondition::RollForwardInput, GroundRequirement::OnlyGrounded),
        interrupt("EscapeB", InterruptCondition::RollBackwardInput, GroundRequirement::OnlyGrounded),
        interrupt("JumpSquat", InterruptCondition::ShieldJumpPressed, GroundRequirement::OnlyGrounded),
    };

    FighterState guardOn;
    guardOn.name = "GuardOn";
    guardOn.animation = "GuardOn";
    guardOn.animationLengthFrames = fighter.properties.common.guardMinHoldFramesX268;
    guardOn.onEnter = {call("common_enter"), call("enter_guard_on")};
    guardOn.onFrame = {call("process_guard_on")};
    guardOn.onAirborne = {call("regular_airborne")};
    guardOn.onAnimationFinishedState = "Guard";
    guardOn.interrupts = guardInterrupts;

    FighterState guardReflect;
    guardReflect.name = "GuardReflect";
    guardReflect.animation = "GuardReflect";
    guardReflect.animationLengthFrames = fighter.properties.common.guardMinHoldFramesX268;
    guardReflect.onEnter = {call("common_enter"), call("enter_guard_on")};
    guardReflect.onFrame = {call("process_guard_reflect")};
    guardReflect.onAirborne = {call("regular_airborne")};
    guardReflect.onAnimationFinishedState = "Guard";
    guardReflect.interrupts = guardInterrupts;

    FighterState guard;
    guard.name = "Guard";
    guard.animation = "Guard";
    guard.animationLengthFrames = 60;
    guard.loopAnimation = true;
    guard.onEnter = {call("common_enter")};
    guard.onFrame = {call("process_guard")};
    guard.onAirborne = {call("regular_airborne")};
    guard.interrupts = guardInterrupts;

    FighterState guardOff;
    guardOff.name = "GuardOff";
    guardOff.animation = "GuardOff";
    guardOff.animationLengthFrames = 16;
    guardOff.onEnter = {call("common_enter")};
    guardOff.onFrame = {call("process_landing")};
    guardOff.onAirborne = {call("teeter_or_airborne")};
    guardOff.onAnimationFinishedState = "Wait";

    FighterState guardSetoff;
    guardSetoff.name = "GuardSetOff";
    guardSetoff.animation = "GuardSetOff";
    guardSetoff.animationLengthFrames = 30;
    guardSetoff.onEnter = {call("common_enter")};
    guardSetoff.onFrame = {call("process_guard_setoff")};
    guardSetoff.onAirborne = {call("regular_airborne")};

    auto makeCatchState = [&](const std::string& name, int length, const std::string& finished) {
        FighterState state;
        state.name = name;
        state.animation = name;
        state.animationLengthFrames = length;
        state.onEnter = {call("common_enter")};
        state.onFrame = {call("process_catch")};
        state.onAirborne = {call("catch_airborne")};
        state.onAnimationFinishedState = finished;
        return state;
    };

    FighterState catchState = makeCatchState("Catch", 30, "Wait");
    FighterState catchDash = makeCatchState("CatchDash", 40, "Wait");
    catchDash.onFrame = {call("process_catch_dash")};

    FighterState catchPull = makeCatchState("CatchPull", 18, "");
    catchPull.onFrame = {call("process_catch_pull")};
    FighterState catchDashPull = catchPull;
    catchDashPull.name = "CatchDashPull";
    catchDashPull.animation = "CatchDashPull";

    FighterState catchWait;
    catchWait.name = "CatchWait";
    catchWait.animation = "CatchWait";
    catchWait.animationLengthFrames = 60;
    catchWait.loopAnimation = true;
    catchWait.onEnter = {call("common_enter")};
    catchWait.onFrame = {call("process_catch_wait")};
    catchWait.onAirborne = {call("catch_airborne")};

    FighterState catchAttack;
    catchAttack.name = "CatchAttack";
    catchAttack.animation = "CatchAttack";
    catchAttack.animationLengthFrames = 24;
    catchAttack.onEnter = {call("common_enter")};
    catchAttack.onFrame = {call("process_catch_attack")};
    catchAttack.onAirborne = {call("catch_airborne")};
    catchAttack.onAnimationFinishedState = "CatchWait";

    FighterState catchCut;
    catchCut.name = "CatchCut";
    catchCut.animation = "CatchCut";
    catchCut.animationLengthFrames = 30;
    catchCut.onEnter = {call("common_enter"), call("enter_catch_cut")};
    catchCut.onFrame = {call("process_catch_cut")};
    catchCut.onLanding = {call("regular_landing")};
    catchCut.onAnimationFinishedState = "Wait";

    auto makeThrowState = [&](const std::string& name, int length) {
        FighterState state;
        state.name = name;
        state.animation = name;
        state.animationLengthFrames = length;
        state.onEnter = {call("common_enter"), call("enter_throw")};
        state.onFrame = {call("process_throw")};
        state.onAirborne = {call("throw_airborne")};
        state.onAnimationFinishedState = "Wait";
        return state;
    };
    FighterState throwF = makeThrowState("ThrowF", 45);
    FighterState throwB = makeThrowState("ThrowB", 45);
    FighterState throwHi = makeThrowState("ThrowHi", 45);
    FighterState throwLw = makeThrowState("ThrowLw", 45);

    auto makeCaptureState = [&](const std::string& name, int length, const std::string& finished) {
        FighterState state;
        state.name = name;
        state.animation = name;
        state.animationLengthFrames = length;
        state.onEnter = {call("common_enter")};
        state.onFrame = {call("process_capture")};
        state.onAnimationFinishedState = finished;
        state.allowLedgeGrab = false;
        state.allowWallCollision = false;
        state.allowCeilingCollision = false;
        return state;
    };
    FighterState capturePulledHi = makeCaptureState("CapturePulledHi", 18, "CaptureWaitHi");
    FighterState capturePulledLw = makeCaptureState("CapturePulledLw", 18, "CaptureWaitLw");
    FighterState captureWaitHi = makeCaptureState("CaptureWaitHi", 60, "");
    captureWaitHi.loopAnimation = true;
    captureWaitHi.onFrame = {call("process_capture_wait")};
    FighterState captureWaitLw = captureWaitHi;
    captureWaitLw.name = "CaptureWaitLw";
    captureWaitLw.animation = "CaptureWaitLw";
    FighterState captureDamageHi = makeCaptureState("CaptureDamageHi", 18, "CaptureWaitHi");
    captureDamageHi.onFrame = {call("process_capture_damage")};
    FighterState captureDamageLw = captureDamageHi;
    captureDamageLw.name = "CaptureDamageLw";
    captureDamageLw.animation = "CaptureDamageLw";
    captureDamageLw.onAnimationFinishedState = "CaptureWaitLw";

    FighterState captureCut = catchCut;
    captureCut.name = "CaptureCut";
    captureCut.animation = "CaptureCut";
    captureCut.onEnter = {call("common_enter"), call("enter_capture_cut")};
    captureCut.onFrame = {call("process_capture_cut")};

    FighterState captureJump = fallState;
    captureJump.name = "CaptureJump";
    captureJump.animation = "CaptureJump";
    captureJump.animationLengthFrames = 35;
    captureJump.loopAnimation = false;
    captureJump.allowLedgeGrab = false;
    captureJump.onEnter = {call("common_enter"), call("enter_capture_jump")};
    captureJump.onAnimationFinishedState = "Fall";

    auto makeThrownState = [&](const std::string& name) {
        FighterState state;
        state.name = name;
        state.animation = name;
        state.animationLengthFrames = 60;
        state.onEnter = {call("common_enter")};
        state.onFrame = {call("process_thrown")};
        state.allowLedgeGrab = false;
        state.allowWallCollision = false;
        state.allowCeilingCollision = false;
        return state;
    };
    FighterState thrownF = makeThrownState("ThrownF");
    FighterState thrownB = makeThrownState("ThrownB");
    FighterState thrownHi = makeThrownState("ThrownHi");
    FighterState thrownLw = makeThrownState("ThrownLw");

    FighterState escapeN;
    escapeN.name = "EscapeN";
    escapeN.animation = "EscapeN";
    escapeN.animationLengthFrames = 22;
    escapeN.onEnter = {call("common_enter")};
    escapeN.onFrame = {call("process_landing")};
    escapeN.onAirborne = {call("teeter_or_airborne")};
    escapeN.onAnimationFinishedState = "Wait";
    escapeN.action = {
        wait(1),
        setHurtboxState(-1, HurtboxState::Intangible),
        wait(14),
        setHurtboxState(-1, HurtboxState::Normal),
    };

    FighterState escapeF = escapeN;
    escapeF.name = "EscapeF";
    escapeF.animation = "EscapeF";
    escapeF.useAnimPhysics = true;
    escapeF.action.clear();

    FighterState escapeB = escapeF;
    escapeB.name = "EscapeB";
    escapeB.animation = "EscapeB";

    FighterState shieldBreakFly;
    shieldBreakFly.name = "ShieldBreakFly";
    shieldBreakFly.animation = "ShieldBreakFly";
    shieldBreakFly.animationLengthFrames = 25;
    shieldBreakFly.allowLedgeGrab = false;
    shieldBreakFly.allowWallCollision = false;
    shieldBreakFly.allowCeilingCollision = false;
    shieldBreakFly.onEnter = {call("common_enter"), call("enter_shield_break_fly")};
    shieldBreakFly.onFrame = {call("process_shield_break_air")};
    shieldBreakFly.onLanding = {call("shield_break_landing")};
    shieldBreakFly.onAnimationFinishedState = "ShieldBreakFall";

    FighterState shieldBreakFall;
    shieldBreakFall.name = "ShieldBreakFall";
    shieldBreakFall.animation = "ShieldBreakFall";
    shieldBreakFall.animationLengthFrames = 60;
    shieldBreakFall.loopAnimation = true;
    shieldBreakFall.allowLedgeGrab = false;
    shieldBreakFall.allowWallCollision = false;
    shieldBreakFall.allowCeilingCollision = false;
    shieldBreakFall.onEnter = {call("common_enter")};
    shieldBreakFall.onFrame = {call("process_shield_break_air")};
    shieldBreakFall.onLanding = {call("shield_break_landing")};

    FighterState shieldBreakDown;
    shieldBreakDown.name = "ShieldBreakDown";
    shieldBreakDown.animation = "ShieldBreakDown";
    shieldBreakDown.animationLengthFrames = 30;
    shieldBreakDown.onEnter = {call("common_enter")};
    shieldBreakDown.onFrame = {call("process_landing")};
    shieldBreakDown.onAirborne = {call("regular_airborne")};
    shieldBreakDown.onAnimationFinishedState = "ShieldBreakStand";

    FighterState shieldBreakStand;
    shieldBreakStand.name = "ShieldBreakStand";
    shieldBreakStand.animation = "ShieldBreakStand";
    shieldBreakStand.animationLengthFrames = 60;
    shieldBreakStand.onEnter = {call("common_enter")};
    shieldBreakStand.onFrame = {call("process_landing")};
    shieldBreakStand.onAirborne = {call("regular_airborne")};
    shieldBreakStand.onAnimationFinishedState = "Wait";

    FighterState cliffCatch;
    cliffCatch.name = "CliffCatch";
    cliffCatch.animation = "CliffCatch";
    cliffCatch.animationLengthFrames = 8;
    cliffCatch.onEnter = {call("common_enter")};
    cliffCatch.onFrame = {call("maintain_ledge")};
    cliffCatch.onAnimationFinishedState = "CliffWait";

    FighterState cliffWait = cliffCatch;
    cliffWait.name = "CliffWait";
    cliffWait.animation = "CliffWait";
    cliffWait.animationLengthFrames = 60;
    cliffWait.loopAnimation = true;
    cliffWait.onAnimationFinishedState = "";
    cliffWait.interrupts = {
        interrupt("CliffAttack", InterruptCondition::AttackPressed, GroundRequirement::OnlyAirborne),
        interrupt("CliffEscape", InterruptCondition::ShieldPressed, GroundRequirement::OnlyAirborne),
        interrupt("CliffClimb", InterruptCondition::LedgeClimbInput, GroundRequirement::OnlyAirborne),
        interrupt("CliffJump", InterruptCondition::JumpPressed, GroundRequirement::OnlyAirborne),
        interrupt("CliffDrop", InterruptCondition::LedgeDropInput, GroundRequirement::OnlyAirborne),
    };

    FighterState cliffClimb;
    cliffClimb.name = "CliffClimb";
    cliffClimb.animation = "CliffClimb";
    cliffClimb.animationLengthFrames = 30;
    cliffClimb.onEnter = {call("common_enter")};
    cliffClimb.onFrame = {call("process_cliff_climb")};

    FighterState cliffEscape;
    cliffEscape.name = "CliffEscape";
    cliffEscape.animation = "CliffEscape";
    cliffEscape.animationLengthFrames = 34;
    cliffEscape.onEnter = {call("common_enter")};
    cliffEscape.onFrame = {call("process_cliff_escape")};

    FighterState cliffAttack;
    cliffAttack.name = "CliffAttack";
    cliffAttack.animation = "CliffAttack";
    cliffAttack.animationLengthFrames = 38;
    cliffAttack.onEnter = {call("common_enter")};
    cliffAttack.onFrame = {call("process_cliff_attack")};
    cliffAttack.action = {
        wait(12),
        createHitbox(0, BoneId::HandR, {fxFromFloat(0.65f), fxFromFloat(0.15f), 0}, fxFromFloat(0.55f), fx(8), fx(45), fx(30), fx(90)),
        wait(5),
        clearHitboxes(),
    };

    FighterState cliffDrop;
    cliffDrop.name = "CliffDrop";
    cliffDrop.animation = "CliffDrop";
    cliffDrop.animationLengthFrames = 1;
    cliffDrop.onEnter = {call("enter_cliff_drop")};

    FighterState cliffJump;
    cliffJump.name = "CliffJump";
    cliffJump.animation = "CliffJump";
    cliffJump.animationLengthFrames = 7;
    cliffJump.onEnter = {call("common_enter")};
    cliffJump.onFrame = {call("process_cliff_jump")};

    FighterState cliffJumpAir = fallState;
    cliffJumpAir.name = "CliffJumpAir";
    cliffJumpAir.animation = "CliffJumpAir";
    cliffJumpAir.animationLengthFrames = 36;
    cliffJumpAir.loopAnimation = false;
    cliffJumpAir.onEnter = {call("common_enter"), call("enter_cliff_jump_air")};
    cliffJumpAir.onAnimationFinishedState = "Fall";

    for (FighterState* state : {
        &waitState, &walkSlow, &walkMiddle, &walkFast, &dashState, &runState, &runDirect,
        &turnState, &turnRun, &squat, &squatWait, &squatRv, &landing, &ottotto, &ottottoWait,
    }) {
        state->interrupts.insert(state->interrupts.begin(), interrupt(
            state == &dashState || state == &runState || state == &runDirect ? "CatchDash" : "Catch",
            InterruptCondition::GrabPressed,
            GroundRequirement::OnlyGrounded));
        expandGroundedAttackShortcut(*state, state == &dashState || state == &runState || state == &runDirect);
    }
    for (FighterState* state : {&guardOn, &guardReflect, &guard, &guardSetoff}) {
        state->interrupts.insert(state->interrupts.begin(), interrupt("Catch", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded));
    }

    fighter.states = {
        waitState, walkSlow, walkMiddle, walkFast, dashState, runState, runDirect, runBrake, turnState, turnRun,
        jumpSquat, squat, squatWait, squatRv, jumpF, jumpB, jumpAerialF, jumpAerialB, fallState, pass, escapeAir, fallSpecial, passiveWallJump, landing, landingAirN, landingAirF, landingAirB, landingAirHi, landingAirLw, landingFallSpecial,
        damageHi1, damageHi2, damageHi3, damageN1, damageN2, damageN3, damageLw1, damageLw2, damageLw3,
        damageAir1, damageAir2, damageAir3, damageFlyHi, damageFlyN, damageFlyLw, damageFlyTop, damageFlyRoll, damageFall,
        downBoundU, downWaitU, downDamageU, downStandU, downAttackU, downForwardU, downBackU, downSpotU,
        downBoundD, downWaitD, downDamageD, downStandD, downAttackD, downForwardD, downBackD, downSpotD,
        passive, passiveStandF, passiveStandB, passiveWall, passiveCeil, wallDamage, stopCeil,
        ottotto, ottottoWait, guardOn, guardReflect, guard, guardOff, guardSetoff,
        catchState, catchPull, catchDash, catchDashPull, catchWait, catchAttack, catchCut,
        throwF, throwB, throwHi, throwLw,
        capturePulledHi, captureWaitHi, captureDamageHi, capturePulledLw, captureWaitLw, captureDamageLw, captureCut, captureJump,
        thrownF, thrownB, thrownHi, thrownLw,
        escapeN, escapeF, escapeB, shieldBreakFly, shieldBreakFall, shieldBreakDown, shieldBreakStand, cliffCatch, cliffWait,
        cliffClimb, cliffEscape, cliffAttack, cliffDrop, cliffJump, cliffJumpAir,
        attack11, attack12, attack13, attackDash,
        attackS3Hi, attackS3HiS, attackS3, attackS3LwS, attackS3Lw, attackHi3, attackLw3,
        attackS4Hi, attackS4HiS, attackS4, attackS4LwS, attackS4Lw, attackHi4, attackLw4,
        airAttack, airAttackF, airAttackB, airAttackHi, airAttackLw
    };
    assignMeleeActionIndices(fighter);
    return fighter;
}

} // namespace pf
