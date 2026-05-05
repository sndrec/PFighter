#include "core/fighter_data.hpp"

#include <algorithm>
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

static InterruptRule activeUntilInterrupt(std::string target, InterruptCondition condition, GroundRequirement ground, int disableFrame) {
    InterruptRule rule = interrupt(std::move(target), condition, ground);
    rule.disableFrame = disableFrame;
    return rule;
}

static InterruptRule alwaysInterrupt(std::string target, InterruptCondition condition, GroundRequirement ground = GroundRequirement::Any) {
    InterruptRule rule = interrupt(std::move(target), condition, ground);
    rule.alwaysActive = true;
    return rule;
}

static int roundedFixFrames(Fix value) {
    return std::max(0, static_cast<int>(fxToFloat(value) + 0.5f));
}

static void delayInterrupts(std::vector<InterruptRule>& rules, int enableFrame) {
    for (InterruptRule& rule : rules) {
        rule.startActive = false;
        rule.enableFrame = enableFrame;
        rule.disableFrame = 0;
    }
}

static void requireHitstunEnded(std::vector<InterruptRule>& rules) {
    for (InterruptRule& rule : rules) {
        rule.requireNoHitstun = true;
    }
}

static std::vector<InterruptRule> groundedAttackInterrupts(bool includeDashAttack = false, bool includeJab = true) {
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
    });
    if (includeJab) {
        rules.push_back(interrupt("Attack11", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded));
    }
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

static std::vector<InterruptRule> walkInputInterrupts() {
    return {
        interrupt("WalkFast", InterruptCondition::HorizontalWalkFast, GroundRequirement::OnlyGrounded),
        interrupt("WalkMiddle", InterruptCondition::HorizontalWalkMiddle, GroundRequirement::OnlyGrounded),
        interrupt("WalkSlow", InterruptCondition::HorizontalWalkSlow, GroundRequirement::OnlyGrounded),
    };
}

static InterruptRule tauntInterrupt() {
    return interrupt("AppealS", InterruptCondition::TauntPressed, GroundRequirement::OnlyGrounded);
}

static std::vector<InterruptRule> groundedSpecialInterrupts() {
    return {
        interrupt("SpecialS", InterruptCondition::SpecialSInput, GroundRequirement::OnlyGrounded),
        interrupt("SpecialHi", InterruptCondition::SpecialHiInput, GroundRequirement::OnlyGrounded),
        interrupt("SpecialN", InterruptCondition::SpecialNInput, GroundRequirement::OnlyGrounded),
        interrupt("SpecialLw", InterruptCondition::SpecialLwInput, GroundRequirement::OnlyGrounded),
    };
}

static std::vector<InterruptRule> airborneSpecialInterrupts() {
    return {
        interrupt("SpecialAirHi", InterruptCondition::SpecialAirHiInput, GroundRequirement::OnlyAirborne),
        interrupt("SpecialAirLw", InterruptCondition::SpecialAirLwInput, GroundRequirement::OnlyAirborne),
        interrupt("SpecialAirS", InterruptCondition::SpecialAirSInput, GroundRequirement::OnlyAirborne),
        interrupt("SpecialAirN", InterruptCondition::SpecialAirNInput, GroundRequirement::OnlyAirborne),
    };
}

static std::vector<InterruptRule> waitIasaInterrupts(bool includeTaunt = false) {
    std::vector<InterruptRule> rules = groundedSpecialInterrupts();
    appendRules(rules, {
        interrupt("Catch", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded),
    });
    appendRules(rules, groundedAttackInterrupts(false));
    appendRules(rules, {
        interrupt("EscapeN", InterruptCondition::SpotDodgeInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
    });
    if (includeTaunt) {
        rules.push_back(tauntInterrupt());
    }
    appendRules(rules, {
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Dash", InterruptCondition::DashInput, GroundRequirement::OnlyGrounded),
        interrupt("Squat", InterruptCondition::SquatInput, GroundRequirement::OnlyGrounded),
        interrupt("Turn", InterruptCondition::TurnInput, GroundRequirement::OnlyGrounded),
    });
    appendRules(rules, walkInputInterrupts());
    return rules;
}

static std::vector<InterruptRule> walkIasaInterrupts(const std::string& currentWalkState) {
    std::vector<InterruptRule> rules = {
        interrupt("Catch", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded),
    };
    appendRules(rules, groundedSpecialInterrupts());
    appendRules(rules, groundedAttackInterrupts(false));
    appendRules(rules, {
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        tauntInterrupt(),
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Dash", InterruptCondition::DashInput, GroundRequirement::OnlyGrounded),
        interrupt("Squat", InterruptCondition::SquatInput, GroundRequirement::OnlyGrounded),
        interrupt("Wait", InterruptCondition::WaitInput, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    });
    for (InterruptRule rule : walkInputInterrupts()) {
        if (rule.targetState != currentWalkState) {
            rules.push_back(rule);
        }
    }
    return rules;
}

static std::vector<InterruptRule> squatIasaInterrupts() {
    std::vector<InterruptRule> rules = groundedSpecialInterrupts();
    appendRules(rules, {
        interrupt("Catch", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded),
    });
    appendRules(rules, groundedAttackInterrupts(false));
    appendRules(rules, {
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        tauntInterrupt(),
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    });
    return rules;
}

static std::vector<InterruptRule> squatWaitIasaInterrupts() {
    std::vector<InterruptRule> rules = {
        interrupt("SpecialLw", InterruptCondition::SpecialLwInput, GroundRequirement::OnlyGrounded),
        interrupt("SpecialHi", InterruptCondition::SpecialHiInput, GroundRequirement::OnlyGrounded),
    };
    appendRules(rules, groundedAttackInterrupts(false));
    appendRules(rules, {
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        tauntInterrupt(),
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Dash", InterruptCondition::DashInput, GroundRequirement::OnlyGrounded),
        interrupt("SquatRv", InterruptCondition::SquatReleaseInput, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    });
    return rules;
}

static std::vector<InterruptRule> squatRvIasaInterrupts() {
    std::vector<InterruptRule> rules = {
        interrupt("SpecialLw", InterruptCondition::SpecialLwInput, GroundRequirement::OnlyGrounded),
        interrupt("SpecialHi", InterruptCondition::SpecialHiInput, GroundRequirement::OnlyGrounded),
    };
    appendRules(rules, groundedAttackInterrupts(false));
    appendRules(rules, {
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        tauntInterrupt(),
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    });
    appendRules(rules, walkInputInterrupts());
    return rules;
}

static std::vector<InterruptRule> landingIasaInterrupts(int landingLag) {
    std::vector<InterruptRule> rules = groundedSpecialInterrupts();
    appendRules(rules, {
        interrupt("Catch", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded),
    });
    appendRules(rules, groundedAttackInterrupts(false));
    appendRules(rules, {
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        tauntInterrupt(),
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Dash", InterruptCondition::DashInput, GroundRequirement::OnlyGrounded),
        timedInterrupt("Squat", InterruptCondition::SquatInput, GroundRequirement::OnlyGrounded, landingLag, landingLag + 1),
        interrupt("Turn", InterruptCondition::TurnInput, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    });
    appendRules(rules, walkInputInterrupts());
    return rules;
}

static std::vector<InterruptRule> turnIasaInterrupts() {
    std::vector<InterruptRule> rules = {
        interrupt("SpecialS", InterruptCondition::SpecialSInput, GroundRequirement::OnlyGrounded),
        interrupt("SpecialLw", InterruptCondition::SpecialLwInput, GroundRequirement::OnlyGrounded),
        interrupt("SpecialHi", InterruptCondition::SpecialHiInput, GroundRequirement::OnlyGrounded),
        interrupt("Catch", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded),
    };
    appendRules(rules, groundedAttackInterrupts(false));
    appendRules(rules, {
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        tauntInterrupt(),
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    });
    return rules;
}

static std::vector<InterruptRule> ottottoIasaInterrupts() {
    std::vector<InterruptRule> rules = groundedSpecialInterrupts();
    appendRules(rules, {
        interrupt("Catch", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded),
    });
    appendRules(rules, groundedAttackInterrupts(false));
    appendRules(rules, {
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        tauntInterrupt(),
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Dash", InterruptCondition::DashInput, GroundRequirement::OnlyGrounded),
        interrupt("Squat", InterruptCondition::SquatInput, GroundRequirement::OnlyGrounded),
        interrupt("Turn", InterruptCondition::TurnInput, GroundRequirement::OnlyGrounded),
        interrupt("WalkSlow", InterruptCondition::TeeterWalkInput, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    });
    return rules;
}

static std::vector<InterruptRule> guardOnIasaInterrupts(int reflectInputWindow) {
    std::vector<InterruptRule> rules;
    if (reflectInputWindow > 0) {
        rules.push_back(activeUntilInterrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded, reflectInputWindow));
    }
    appendRules(rules, {
        interrupt("EscapeN", InterruptCondition::SpotDodgeInput, GroundRequirement::OnlyGrounded),
        interrupt("EscapeF", InterruptCondition::RollForwardInput, GroundRequirement::OnlyGrounded),
        interrupt("EscapeB", InterruptCondition::RollBackwardInput, GroundRequirement::OnlyGrounded),
        interrupt("CatchDash", InterruptCondition::GuardCatchDashPressed, GroundRequirement::OnlyGrounded),
        interrupt("Catch", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded),
        interrupt("JumpSquat", InterruptCondition::ShieldJumpPressed, GroundRequirement::OnlyGrounded),
    });
    return rules;
}

static std::vector<InterruptRule> jabIasaInterrupts(std::string followupState) {
    std::vector<InterruptRule> rules = groundedAttackInterrupts(false, false);
    rules.push_back(alwaysInterrupt("Attack100Start", InterruptCondition::RapidJabReady, GroundRequirement::OnlyGrounded));
    rules.push_back(alwaysInterrupt(std::move(followupState), InterruptCondition::JabFollowupPressed, GroundRequirement::OnlyGrounded));
    appendRules(rules, {
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Dash", InterruptCondition::DashInput, GroundRequirement::OnlyGrounded),
        interrupt("Squat", InterruptCondition::SquatInput, GroundRequirement::OnlyGrounded),
        interrupt("Turn", InterruptCondition::TurnInput, GroundRequirement::OnlyGrounded),
    });
    appendRules(rules, walkInputInterrupts());
    return rules;
}

static std::vector<InterruptRule> sideSmashIasaInterrupts() {
    std::vector<InterruptRule> rules = groundedSpecialInterrupts();
    appendRules(rules, {
        interrupt("Catch", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded),
        alwaysInterrupt("AttackS42", InterruptCondition::AttackS42Pressed, GroundRequirement::OnlyGrounded),
    });
    appendRules(rules, groundedAttackInterrupts(false));
    appendRules(rules, {
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        tauntInterrupt(),
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Dash", InterruptCondition::DashInput, GroundRequirement::OnlyGrounded),
        interrupt("Squat", InterruptCondition::SquatInput, GroundRequirement::OnlyGrounded),
        interrupt("Turn", InterruptCondition::TurnInput, GroundRequirement::OnlyGrounded),
    });
    appendRules(rules, walkInputInterrupts());
    return rules;
}

static std::vector<InterruptRule> downTiltIasaInterrupts() {
    std::vector<InterruptRule> rules = groundedSpecialInterrupts();
    appendRules(rules, {
        interrupt("AttackS4Hi", InterruptCondition::AttackS4HiPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS4HiS", InterruptCondition::AttackS4HiSPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS4", InterruptCondition::AttackS4Pressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS4LwS", InterruptCondition::AttackS4LwSPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS4Lw", InterruptCondition::AttackS4LwPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackHi4", InterruptCondition::AttackHi4Pressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackLw4", InterruptCondition::AttackLw4Pressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS3Hi", InterruptCondition::AttackS3HiPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS3HiS", InterruptCondition::AttackS3HiSPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS3", InterruptCondition::AttackS3Pressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS3LwS", InterruptCondition::AttackS3LwSPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackS3Lw", InterruptCondition::AttackS3LwPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackHi3", InterruptCondition::AttackHi3Pressed, GroundRequirement::OnlyGrounded),
        alwaysInterrupt("AttackLw3", InterruptCondition::AttackLw3Repeat, GroundRequirement::OnlyGrounded),
        interrupt("Attack11", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
    });
    appendRules(rules, {
        interrupt("JumpSquat", InterruptCondition::JumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Dash", InterruptCondition::DashInput, GroundRequirement::OnlyGrounded),
        interrupt("Squat", InterruptCondition::SquatInput, GroundRequirement::OnlyGrounded),
        interrupt("Turn", InterruptCondition::TurnInput, GroundRequirement::OnlyGrounded),
    });
    appendRules(rules, walkInputInterrupts());
    return rules;
}

static std::vector<InterruptRule> airborneActionInterrupts(bool includeWallJump = true) {
    std::vector<InterruptRule> rules = airborneSpecialInterrupts();
    appendRules(rules, {
        interrupt("EscapeAir", InterruptCondition::AirDodgePressed, GroundRequirement::OnlyAirborne),
        interrupt("AirAttackHi", InterruptCondition::AerialAttackHiPressed, GroundRequirement::OnlyAirborne),
        interrupt("AirAttackLw", InterruptCondition::AerialAttackLwPressed, GroundRequirement::OnlyAirborne),
        interrupt("AirAttackF", InterruptCondition::AerialAttackFPressed, GroundRequirement::OnlyAirborne),
        interrupt("AirAttackB", InterruptCondition::AerialAttackBPressed, GroundRequirement::OnlyAirborne),
        interrupt("AirAttackN", InterruptCondition::AerialAttackNPressed, GroundRequirement::OnlyAirborne),
        interrupt("JumpAerialB", InterruptCondition::AerialJumpBackwardPressed, GroundRequirement::OnlyAirborne),
        interrupt("JumpAerialF", InterruptCondition::AerialJumpForwardPressed, GroundRequirement::OnlyAirborne),
    });
    if (includeWallJump) {
        // ftCo_Fall/Jump/JumpAerial run IASA first; ft_800831CC/835B0 checks
        // ftWallJump_8008169C from collision afterward.
        rules.push_back(interrupt("PassiveWallJump", InterruptCondition::WallJumpInput, GroundRequirement::OnlyAirborne));
    }
    return rules;
}

static std::vector<InterruptRule> damageFallActionInterrupts() {
    std::vector<InterruptRule> rules = airborneSpecialInterrupts();
    appendRules(rules, {
        interrupt("AirAttackHi", InterruptCondition::AerialAttackHiPressed, GroundRequirement::OnlyAirborne),
        interrupt("AirAttackLw", InterruptCondition::AerialAttackLwPressed, GroundRequirement::OnlyAirborne),
        interrupt("AirAttackF", InterruptCondition::AerialAttackFPressed, GroundRequirement::OnlyAirborne),
        interrupt("AirAttackB", InterruptCondition::AerialAttackBPressed, GroundRequirement::OnlyAirborne),
        interrupt("AirAttackN", InterruptCondition::AerialAttackNPressed, GroundRequirement::OnlyAirborne),
        interrupt("JumpAerialB", InterruptCondition::AerialJumpBackwardPressed, GroundRequirement::OnlyAirborne),
        interrupt("JumpAerialF", InterruptCondition::AerialJumpForwardPressed, GroundRequirement::OnlyAirborne),
        interrupt("PassiveWallJump", InterruptCondition::WallJumpInput, GroundRequirement::OnlyAirborne),
    });
    return rules;
}

static std::vector<InterruptRule> damageActionInterrupts() {
    std::vector<InterruptRule> rules = waitIasaInterrupts(true);
    appendRules(rules, airborneActionInterrupts());
    return rules;
}

static std::vector<InterruptRule> attackAirIasaInterrupts() {
    std::vector<InterruptRule> rules = airborneSpecialInterrupts();
    appendRules(rules, {
        // ftCo_AttackAir*_IASA also checks item throw/pickup and tether air
        // catch helpers here; those systems are not implemented yet.
        interrupt("JumpAerialB", InterruptCondition::AerialJumpBackwardPressed, GroundRequirement::OnlyAirborne),
        interrupt("JumpAerialF", InterruptCondition::AerialJumpForwardPressed, GroundRequirement::OnlyAirborne),
    });
    return rules;
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
        {"FallF", 21},
        {"FallB", 22},
        {"FallAerial", 23},
        {"FallAerialF", 24},
        {"FallAerialB", 25},
        {"FallSpecial", 26},
        {"FallSpecialF", 27},
        {"FallSpecialB", 28},
        {"DamageFall", 29},
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
        {"Rebound", 45},
        {"AppealSR", 239},
        {"AppealSL", 240},
        {"ItemScrew", 144},
        {"ItemScrewAir", 145},
        {"ItemScrewDamage", 146},
        {"ItemScrewDamageAir", 147},
        {"Jab", 46},
        {"Attack11", 46},
        {"Attack12", 47},
        {"Attack13", 48},
        {"Attack100Start", 49},
        {"Attack100Loop", 50},
        {"Attack100End", 51},
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
        {"AttackS42", 295},
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
        {"Furafura", 205},
        {"FuraSleepStart", 206},
        {"FuraSleepLoop", 207},
        {"FuraSleepEnd", 208},
        {"ShieldBreakFly", 286},
        {"ShieldBreakFall", 287},
        {"ShieldBreakDownU", 288},
        {"ShieldBreakDownD", 289},
        {"ShieldBreakStandU", 290},
        {"ShieldBreakStandD", 291},
        {"DamageIceJump", 20},
        {"BuryJump", 16},
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
        {"CaptureNeck", 259},
        {"CaptureFoot", 260},
        {"CaptureYoshi", 254},
        {"ThrownF", 262},
        {"ThrownB", 263},
        {"ThrownHi", 264},
        {"ThrownLw", 265},
        {"ThrownLwWomen", 266},
        {"ThrownFF", 272},
        {"ThrownFB", 273},
        {"ThrownFHi", 274},
        {"ThrownFLw", 275},
        {"CaptureCaptain", 276},
        {"ThrownKoopaF", 279},
        {"ThrownKoopaB", 280},
        {"ThrownKoopaAirF", 282},
        {"ThrownKoopaAirB", 283},
        {"ThrownMewtwo", 292},
        {"ThrownMewtwoAir", 293},
        {"Pass", 209},
        {"Ottotto", 210},
        {"OttottoWait", 211},
        {"Squat", 30},
        {"SquatWait", 31},
        {"SquatRv", 34},
        {"PassiveWallJump", 203},
        {"FlyReflectWall", 247},
        {"FlyReflectCeil", 248},
        {"DownReflect", 335},
        {"WallDamage", 212},
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
        {"ShieldBreakDown", 288},
        {"ShieldBreakStand", 290},
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
        HurtboxDefinition{BoneId::Hip, -1, "", {0, 0, 0}, {0, fxFromFloat(1.0f), 0}, fxFromFloat(0.55f)},
        HurtboxDefinition{BoneId::Head, -1, "", {0, 0, 0}, {0, fxFromFloat(0.35f), 0}, fxFromFloat(0.42f)},
        HurtboxDefinition{BoneId::HandL, -1, "", {0, 0, 0}, {0, 0, 0}, fxFromFloat(0.26f)},
        HurtboxDefinition{BoneId::HandR, -1, "", {0, 0, 0}, {0, 0, 0}, fxFromFloat(0.26f)},
    };

    FighterState waitState;
    waitState.name = "Wait";
    waitState.animation = "Wait";
    waitState.animationLengthFrames = 60;
    waitState.loopAnimation = true;
    waitState.onEnter = {call("common_enter")};
    waitState.onFrame = {call("process_grounded")};
    waitState.onAirborne = {call("teeter_or_airborne")};
    waitState.interrupts = waitIasaInterrupts(true);
    waitState.interrupts.push_back(interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne));

    FighterState walkSlow = waitState;
    walkSlow.name = "WalkSlow";
    walkSlow.animation = "WalkSlow";
    walkSlow.onFrame = {call("process_grounded"), call("process_walk")};
    walkSlow.interrupts = walkIasaInterrupts("WalkSlow");

    FighterState walkMiddle = walkSlow;
    walkMiddle.name = "WalkMiddle";
    walkMiddle.animation = "WalkMiddle";
    walkMiddle.interrupts = walkIasaInterrupts("WalkMiddle");

    FighterState walkFast = walkSlow;
    walkFast.name = "WalkFast";
    walkFast.animation = "WalkFast";
    walkFast.interrupts = walkIasaInterrupts("WalkFast");

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
        interrupt("SpecialS", InterruptCondition::SpecialSInput, GroundRequirement::OnlyGrounded),
        interrupt("CatchDash", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackDash", InterruptCondition::AttackDashPressed, GroundRequirement::OnlyGrounded),
        timedInterrupt("Turn", InterruptCondition::ReverseDashInput, GroundRequirement::OnlyGrounded, fighter.properties.common.dashEarlyInterruptWindowX44 + 1),
        activeUntilInterrupt("EscapeF", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded, fighter.properties.common.dashItemThrowWindowX48 + 1),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        tauntInterrupt(),
        interrupt("JumpSquat", InterruptCondition::RunJumpPressed, GroundRequirement::OnlyGrounded),
        interrupt("Run", InterruptCondition::RunInput, GroundRequirement::OnlyGrounded),
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
        interrupt("SpecialS", InterruptCondition::SpecialSInput, GroundRequirement::OnlyGrounded),
        interrupt("SpecialHi", InterruptCondition::SpecialHiInput, GroundRequirement::OnlyGrounded),
        interrupt("SpecialN", InterruptCondition::SpecialNInput, GroundRequirement::OnlyGrounded),
        interrupt("SpecialLw", InterruptCondition::SpecialLwInput, GroundRequirement::OnlyGrounded),
        interrupt("CatchDash", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackDash", InterruptCondition::AttackDashPressed, GroundRequirement::OnlyGrounded),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        tauntInterrupt(),
        interrupt("JumpSquat", InterruptCondition::RunJumpPressed, GroundRequirement::OnlyGrounded),
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
        interrupt("SpecialS", InterruptCondition::SpecialSInput, GroundRequirement::OnlyGrounded),
        interrupt("SpecialHi", InterruptCondition::SpecialHiInput, GroundRequirement::OnlyGrounded),
        interrupt("SpecialN", InterruptCondition::SpecialNInput, GroundRequirement::OnlyGrounded),
        interrupt("SpecialLw", InterruptCondition::SpecialLwInput, GroundRequirement::OnlyGrounded),
        interrupt("CatchDash", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded),
        interrupt("AttackDash", InterruptCondition::AttackDashPressed, GroundRequirement::OnlyGrounded),
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
        tauntInterrupt(),
        interrupt("JumpSquat", InterruptCondition::RunJumpPressed, GroundRequirement::OnlyGrounded),
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
        interrupt("TurnRun", InterruptCondition::RunBrakeTurnRunInput, GroundRequirement::OnlyGrounded),
        interrupt("Squat", InterruptCondition::SquatInput, GroundRequirement::OnlyGrounded),
        interrupt("Fall", InterruptCondition::BecameAirborne, GroundRequirement::OnlyAirborne),
    };

    FighterState turnState;
    turnState.name = "Turn";
    turnState.animation = "Turn";
    turnState.animationLengthFrames = 11;
    turnState.onEnter = {call("common_enter"), call("enter_turn")};
    turnState.onFrame = {call("process_turn")};
    turnState.onAirborne = {call("regular_airborne")};
    turnState.onAnimationFinishedState = "Wait";
    turnState.onAnimationFinishedBlendFrames = kDisableAnimationBlendFrames;
    turnState.interrupts = turnIasaInterrupts();

    FighterState turnRun;
    turnRun.name = "TurnRun";
    turnRun.animation = "TurnRun";
    turnRun.animationLengthFrames = 16;
    turnRun.onEnter = {call("common_enter"), call("enter_turn_run")};
    turnRun.onFrame = {call("process_turn_run")};
    turnRun.onAirborne = {call("regular_airborne")};
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
    // ftCo_KneeBend_IASA checks catch and AttackHi4NoD0 before short-hop state.
    jumpSquat.interrupts = {
        activeUntilInterrupt("Catch", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded, fighter.properties.jumpStartupLag),
        activeUntilInterrupt("AttackHi4", InterruptCondition::AttackHi4NoStickWindowPressed, GroundRequirement::OnlyGrounded, fighter.properties.jumpStartupLag),
    };

    FighterState squat;
    squat.name = "Squat";
    squat.animation = "Squat";
    squat.animationLengthFrames = 4;
    squat.onEnter = {call("common_enter")};
    squat.onFrame = {call("process_squat")};
    squat.onAirborne = {call("regular_airborne")};
    squat.onAnimationFinishedState = "SquatWait";
    squat.interrupts = squatIasaInterrupts();

    FighterState squatWait = squat;
    squatWait.name = "SquatWait";
    squatWait.animation = "SquatWait";
    squatWait.animationLengthFrames = 60;
    squatWait.loopAnimation = true;
    squatWait.onAnimationFinishedState = "";
    squatWait.interrupts = squatWaitIasaInterrupts();

    FighterState squatRv;
    squatRv.name = "SquatRv";
    squatRv.animation = "SquatRv";
    squatRv.animationLengthFrames = 4;
    squatRv.onEnter = {call("common_enter")};
    squatRv.onFrame = {call("process_landing")};
    squatRv.onAirborne = {call("regular_airborne")};
    squatRv.onAnimationFinishedState = "Wait";
    squatRv.interrupts = squatRvIasaInterrupts();

    FighterState fallState;
    fallState.name = "Fall";
    fallState.animation = "Fall";
    fallState.animationLengthFrames = 60;
    fallState.loopAnimation = true;
    fallState.onEnter = {call("common_enter")};
    fallState.onFrame = {call("process_airborne_fastfall")};
    fallState.onLanding = {call("regular_landing")};
    fallState.interrupts = airborneActionInterrupts();

    FighterState fallAerial = fallState;
    fallAerial.name = "FallAerial";
    fallAerial.animation = "FallAerial";

    FighterState attack11;
    attack11.name = "Attack11";
    attack11.animation = "Attack11";
    attack11.animationLengthFrames = 28;
    attack11.onEnter = {call("common_enter")};
    attack11.onFrame = {call("process_landing")};
    attack11.onAirborne = {call("regular_airborne")};
    attack11.onAnimationFinishedState = "Wait";
    attack11.useAnimPhysics = true;
    // Common grounded attacks use ft_80084104, which keeps ground-only
    // collision states on the floor instead of letting TransN slide off edges.
    attack11.allowSlideoff = false;
    attack11.initialInterruptibleFrame = 1000000;
    attack11.action = {
        wait(3),
        createHitbox(0, BoneId::HandR, {fxFromFloat(0.8f), fxFromFloat(0.2f), 0}, fxFromFloat(0.45f), fx(4), fx(45), fx(22), fx(80)),
        wait(3),
        clearHitboxes(),
    };
    attack11.onEnter = {call("common_enter"), call("enter_attack11")};
    attack11.interrupts = jabIasaInterrupts("Attack12");

    FighterState attack12 = attack11;
    attack12.name = "Attack12";
    attack12.animation = "Attack12";
    attack12.onEnter = {call("common_enter")};
    attack12.interrupts = jabIasaInterrupts("Attack13");

    FighterState attack13 = attack11;
    attack13.name = "Attack13";
    attack13.animation = "Attack13";
    attack13.onEnter = {call("common_enter")};
    attack13.interrupts = waitIasaInterrupts(true);

    FighterState attack100Start = attack11;
    attack100Start.name = "Attack100Start";
    attack100Start.animation = "Attack100Start";
    attack100Start.action.clear();
    attack100Start.onEnter = {call("common_enter"), call("enter_attack100_start")};
    attack100Start.onFrame = {call("process_landing")};
    attack100Start.onAnimationFinishedState = "Attack100Loop";
    attack100Start.interrupts.clear();

    FighterState attack100Loop = attack100Start;
    attack100Loop.name = "Attack100Loop";
    attack100Loop.animation = "Attack100Loop";
    attack100Loop.onEnter = {call("common_enter")};
    attack100Loop.onFrame = {call("process_attack100_loop")};
    attack100Loop.onAnimationFinishedState.clear();
    attack100Loop.loopAnimation = true;

    FighterState attack100End = attack100Start;
    attack100End.name = "Attack100End";
    attack100End.animation = "Attack100End";
    attack100End.onEnter = {call("common_enter")};
    attack100End.onAnimationFinishedState = "Wait";

    auto makeGroundedAttack = [&](std::string name) {
        FighterState state = attack11;
        state.name = name;
        state.animation = name;
        state.action.clear();
        state.interrupts = waitIasaInterrupts(true);
        state.initialInterruptibleFrame = 1000000;
        state.onAnimationFinishedState = "Wait";
        state.useAnimPhysics = true;
        return state;
    };

    FighterState attackDash = makeGroundedAttack("AttackDash");
    attackDash.onEnter = { call("common_enter"), call("enter_attack_dash") };
    attackDash.onFrame = { call("process_attack_dash") };
    attackDash.interrupts = waitIasaInterrupts(true);
    attackDash.interrupts.insert(
        attackDash.interrupts.begin(),
        alwaysInterrupt("CatchDash", InterruptCondition::AttackDashGrabBuffer, GroundRequirement::OnlyGrounded));
    FighterState attackS3Hi = makeGroundedAttack("AttackS3Hi");
    FighterState attackS3HiS = makeGroundedAttack("AttackS3HiS");
    FighterState attackS3 = makeGroundedAttack("AttackS3");
    FighterState attackS3LwS = makeGroundedAttack("AttackS3LwS");
    FighterState attackS3Lw = makeGroundedAttack("AttackS3Lw");
    FighterState attackHi3 = makeGroundedAttack("AttackHi3");
    FighterState attackLw3 = makeGroundedAttack("AttackLw3");
    attackLw3.onEnter = {call("common_enter"), call("enter_attack_lw3")};
    attackLw3.onFrame = {call("process_attack_lw3")};
    attackLw3.interrupts = downTiltIasaInterrupts();
    FighterState attackS4Hi = makeGroundedAttack("AttackS4Hi");
    FighterState attackS4HiS = makeGroundedAttack("AttackS4HiS");
    FighterState attackS4 = makeGroundedAttack("AttackS4");
    FighterState attackS4LwS = makeGroundedAttack("AttackS4LwS");
    FighterState attackS4Lw = makeGroundedAttack("AttackS4Lw");
    for (FighterState* state : {&attackS4Hi, &attackS4HiS, &attackS4, &attackS4LwS, &attackS4Lw}) {
        state->interrupts = sideSmashIasaInterrupts();
    }
    FighterState attackS42 = makeGroundedAttack("AttackS42");
    attackS42.interrupts = waitIasaInterrupts(true);
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
    airAttack.onFrame = {call("process_airborne_fastfall")};
    airAttack.onLanding = {call("aerial_landing_attack")};
    airAttack.onAnimationFinishedState = "Fall";
    airAttack.initialInterruptibleFrame = 1000000;
    airAttack.interrupts = attackAirIasaInterrupts();
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
    landing.interrupts = landingIasaInterrupts(fighter.properties.normalLandingLag);

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

    FighterState landingFallSpecialInterruptible = landingFallSpecial;
    landingFallSpecialInterruptible.name = "LandingFallSpecialInterruptible";
    landingFallSpecialInterruptible.interrupts = landing.interrupts;

    auto makeDamageState = [&](const std::string& name, int length) {
        FighterState state;
        state.name = name;
        state.animation = name;
        state.animationLengthFrames = length;
        state.allowLedgeGrab = false;
        state.onEnter = {call("common_enter")};
        state.onFrame = {call("process_damage")};
        state.onLanding = {call("damage_landing")};
        state.onAirborne = {call("damage_airborne")};
        state.interrupts = damageActionInterrupts();
        requireHitstunEnded(state.interrupts);
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
        state.interrupts = damageFallActionInterrupts();
        requireHitstunEnded(state.interrupts);
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
    damageFall.onLanding = {call("damage_fly_landing")};
    damageFall.interrupts = damageFallActionInterrupts();

    FighterState itemScrew = fallState;
    itemScrew.name = "ItemScrew";
    itemScrew.animation = "ItemScrew";
    itemScrew.animationLengthFrames = 50;
    itemScrew.allowLedgeGrab = false;
    itemScrew.onEnter = {call("common_enter"), call("enter_item_screw")};
    itemScrew.onFrame = {call("process_item_screw")};
    itemScrew.onLanding = {call("regular_landing")};
    itemScrew.onAnimationFinishedState = "Fall";

    FighterState itemScrewAir = itemScrew;
    itemScrewAir.name = "ItemScrewAir";
    itemScrewAir.animation = "ItemScrewAir";
    itemScrewAir.onEnter = {call("common_enter")};
    itemScrewAir.onFrame = {call("process_airborne_fastfall")};
    itemScrewAir.onAnimationFinishedState = "FallAerial";

    FighterState damageScrew;
    damageScrew.name = "DamageScrew";
    damageScrew.animation = "ItemScrewDamage";
    damageScrew.animationLengthFrames = 50;
    damageScrew.allowLedgeGrab = false;
    damageScrew.convertFloorCollisionToGround = false;
    damageScrew.onEnter = {call("common_enter")};
    damageScrew.onFrame = {call("process_damage_screw")};
    damageScrew.onLanding.clear();

    FighterState damageScrewAir = damageScrew;
    damageScrewAir.name = "DamageScrewAir";
    damageScrewAir.animation = "ItemScrewDamageAir";

    FighterState damageSong;
    damageSong.name = "DamageSong";
    damageSong.animation = "FuraSleepStart";
    damageSong.animationLengthFrames = 45;
    damageSong.allowLedgeGrab = false;
    damageSong.onEnter = {call("common_enter")};
    damageSong.onFrame = {call("process_damage_song")};
    damageSong.onAirborne = {call("teeter_or_airborne")};

    FighterState damageSongWait = damageSong;
    damageSongWait.name = "DamageSongWait";
    damageSongWait.animation = "FuraSleepLoop";
    damageSongWait.loopAnimation = true;
    damageSongWait.onFrame = {call("process_damage_song_wait")};

    FighterState damageSongRv = damageSong;
    damageSongRv.name = "DamageSongRv";
    damageSongRv.animation = "FuraSleepEnd";
    damageSongRv.loopAnimation = false;
    damageSongRv.onFrame = {call("process_damage_song_rv")};

    FighterState damageBind;
    damageBind.name = "DamageBind";
    damageBind.animation = "Furafura";
    damageBind.animationLengthFrames = 60;
    damageBind.loopAnimation = true;
    damageBind.allowLedgeGrab = false;
    damageBind.onEnter = {call("common_enter")};
    damageBind.onFrame = {call("process_damage_bind")};
    damageBind.onAirborne = {call("teeter_or_airborne")};

    FighterState damageIce;
    damageIce.name = "DamageIce";
    damageIce.animation = "DamageIce";
    damageIce.animationLengthFrames = 60;
    damageIce.loopAnimation = true;
    damageIce.allowLedgeGrab = false;
    damageIce.onEnter = {call("common_enter"), call("enter_damage_ice")};
    damageIce.onFrame = {call("process_damage_ice")};
    damageIce.onAirborne = {call("damage_airborne")};

    FighterState damageIceJump = fallState;
    damageIceJump.name = "DamageIceJump";
    damageIceJump.animation = "Fall";
    damageIceJump.loopAnimation = true;
    damageIceJump.allowLedgeGrab = false;
    damageIceJump.onEnter = {call("common_enter"), call("enter_damage_ice_jump")};
    damageIceJump.onFrame = {call("process_damage_ice_jump")};
    damageIceJump.onLanding = {call("regular_landing")};
    damageIceJump.interrupts.clear();
    damageIceJump.interrupts.push_back(
        interrupt("PassiveWallJump", InterruptCondition::WallJumpInput, GroundRequirement::OnlyAirborne));

    FighterState bury;
    bury.name = "Bury";
    bury.animation = "";
    bury.animationLengthFrames = 1;
    bury.loopAnimation = true;
    bury.allowLedgeGrab = false;
    bury.onEnter = {call("common_enter")};
    bury.onFrame = {call("process_bury")};
    bury.onAirborne = {call("release_bury")};

    FighterState buryWait = bury;
    buryWait.name = "BuryWait";
    buryWait.onFrame = {call("process_bury_wait")};

    FighterState buryJump = fallState;
    buryJump.name = "BuryJump";
    buryJump.animation = "BuryJump";
    buryJump.animationLengthFrames = 35;
    buryJump.allowLedgeGrab = false;
    buryJump.onEnter = {call("common_enter"), call("enter_bury_jump")};
    buryJump.onFrame = {call("process_bury_jump")};
    buryJump.interrupts = airborneActionInterrupts(false);
    delayInterrupts(
        buryJump.interrupts,
        roundedFixFrames(fighter.properties.common.buryJumpGravityThresholdX61C) + 1);
    buryJump.onAnimationFinishedState = "Fall";

    FighterState reboundStop;
    reboundStop.name = "ReboundStop";
    reboundStop.animation = "";
    reboundStop.animationLengthFrames = 1;
    reboundStop.onEnter = {call("common_enter"), call("enter_rebound_stop")};
    reboundStop.onFrame = {call("process_rebound_stop")};
    reboundStop.onAirborne = {call("regular_airborne")};

    FighterState rebound = reboundStop;
    rebound.name = "Rebound";
    rebound.animation = "Rebound";
    rebound.animationLengthFrames = 30;
    rebound.onEnter = {call("common_enter")};
    rebound.onFrame = {call("process_rebound")};

    FighterState downBoundU;
    downBoundU.name = "DownBoundU";
    downBoundU.animation = "DownBoundU";
    downBoundU.animationLengthFrames = 18;
    downBoundU.onEnter = {call("common_enter"), call("enter_down_bound")};
    downBoundU.onFrame = {call("process_down_bound")};
    downBoundU.onAirborne = {call("regular_airborne")};

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
    downDamageU.onFrame = {call("process_down_damage")};
    downDamageU.onAirborne.clear();
    downDamageU.onAnimationFinishedState.clear();

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
    downStandU.onEnter = {call("common_enter"), call("enter_tech_preserve_knockback")};
    FighterState downAttackU = makeDownGetupState("DownAttackU", 50);
    downAttackU.onEnter = {call("common_enter"), call("enter_tech_preserve_knockback")};
    FighterState downForwardU = makeDownGetupState("DownForwardU", 35);
    downForwardU.onEnter = {call("common_enter"), call("enter_passive")};
    downForwardU.useAnimPhysics = true;
    FighterState downBackU = makeDownGetupState("DownBackU", 35);
    downBackU.onEnter = {call("common_enter"), call("enter_passive")};
    downBackU.useAnimPhysics = true;
    FighterState downSpotU = makeDownGetupState("DownSpotU", 26);
    downSpotU.onEnter = {call("common_enter"), call("enter_tech_preserve_knockback")};
    downSpotU.onAnimationFinishedState = "DownWaitU";

    FighterState downBoundD = downBoundU;
    downBoundD.name = "DownBoundD";
    downBoundD.animation = "DownBoundD";

    FighterState downWaitD = downWaitU;
    downWaitD.name = "DownWaitD";
    downWaitD.animation = "DownWaitD";

    FighterState downDamageD = downWaitU;
    downDamageD.name = "DownDamageD";
    downDamageD.animation = "DownDamageD";
    downDamageD.animationLengthFrames = 18;
    downDamageD.onEnter = {call("common_enter")};
    downDamageD.onFrame = {call("process_down_damage")};
    downDamageD.onAirborne.clear();
    downDamageD.onAnimationFinishedState.clear();

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
    passive.onEnter = {call("common_enter"), call("enter_passive")};
    FighterState passiveStandF = makePassiveState("PassiveStandF", 40, "Wait");
    passiveStandF.onEnter = {call("common_enter"), call("enter_tech_preserve_knockback")};
    passiveStandF.useAnimPhysics = true;
    FighterState passiveStandB = makePassiveState("PassiveStandB", 40, "Wait");
    passiveStandB.onEnter = {call("common_enter"), call("enter_tech_preserve_knockback")};
    passiveStandB.useAnimPhysics = true;

    FighterState passiveWall = makePassiveState("PassiveWall", 28, "Fall");
    passiveWall.onFrame = {call("process_passive_wall")};
    passiveWall.onLanding = {call("regular_landing")};
    FighterState passiveCeil = passiveWall;
    passiveCeil.name = "PassiveCeil";
    passiveCeil.animation = "PassiveCeil";
    passiveCeil.onFrame = {call("process_passive_ceil")};

    FighterState wallDamage = makeDamageFlyState("WallDamage");
    wallDamage.animationLengthFrames = 16;
    wallDamage.onFrame = {call("process_damage_surface")};
    wallDamage.onLanding = {call("damage_fly_landing")};

    FighterState flyReflectWall = wallDamage;
    flyReflectWall.name = "FlyReflectWall";
    flyReflectWall.animation = "WallDamage";

    FighterState flyReflectCeil = wallDamage;
    flyReflectCeil.name = "FlyReflectCeil";
    flyReflectCeil.animation = "StopCeil";

    FighterState downReflect;
    downReflect.name = "DownReflect";
    downReflect.animation = "WallDamage";
    downReflect.animationLengthFrames = 16;
    downReflect.allowLedgeGrab = false;
    downReflect.onEnter = {call("common_enter")};
    downReflect.onFrame = {call("process_down_reflect")};
    downReflect.onLanding = {call("down_reflect_landing")};
    downReflect.onAnimationFinishedState = "DamageFall";

    FighterState stopWall;
    stopWall.name = "StopWall";
    stopWall.animation = "StopWall";
    stopWall.animationLengthFrames = 20;
    stopWall.onEnter = {call("common_enter")};
    stopWall.onAirborne = {call("teeter_or_airborne")};
    stopWall.onAnimationFinishedState = "Wait";

    FighterState stopCeil;
    stopCeil.name = "StopCeil";
    stopCeil.animation = "StopCeil";
    stopCeil.animationLengthFrames = 20;
    stopCeil.onEnter = {call("common_enter")};
    stopCeil.onLanding = {call("regular_landing")};
    stopCeil.onAnimationFinishedState = "Fall";

    FighterState pass;
    pass.name = "Pass";
    pass.animation = "Pass";
    pass.animationLengthFrames = fighter.properties.common.platformDropAnimationFramesX470;
    pass.onEnter = {call("common_enter"), call("enter_pass")};
    pass.onFrame = {call("process_airborne_fastfall")};
    pass.onLanding = {call("regular_landing")};
    pass.onAnimationFinishedState = "Fall";
    pass.interrupts = airborneActionInterrupts(false);

    FighterState jumpF = fallState;
    jumpF.name = "JumpF";
    jumpF.animation = "JumpF";
    jumpF.animationLengthFrames = 40;
    jumpF.loopAnimation = false;
    jumpF.onFrame = {call("process_ground_jump")};
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
    jumpAerialF.onAnimationFinishedState = "FallAerial";

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

    FighterState fallSpecial;
    fallSpecial.name = "FallSpecial";
    fallSpecial.animation = "FallSpecial";
    fallSpecial.animationLengthFrames = 60;
    fallSpecial.loopAnimation = true;
    fallSpecial.allowCeilingCollision = false;
    fallSpecial.onEnter = {call("common_enter"), call("enter_fall_special")};
    fallSpecial.onFrame = {call("process_fall_special")};
    fallSpecial.onLanding = {call("escape_air_landing")};
    fallSpecial.interrupts = {
        interrupt("JumpAerialB", InterruptCondition::AerialJumpBackwardPressed, GroundRequirement::OnlyAirborne),
        interrupt("JumpAerialF", InterruptCondition::AerialJumpForwardPressed, GroundRequirement::OnlyAirborne),
    };

    FighterState specialN = attack11;
    specialN.name = "SpecialN";
    specialN.animation = "SpecialN";
    specialN.animationLengthFrames = 60;
    specialN.action.clear();
    specialN.onEnter = {call("common_enter")};
    specialN.onFrame = {call("process_landing")};
    specialN.onAirborne = {call("regular_airborne")};
    specialN.onAnimationFinishedState = "Wait";
    specialN.interrupts.clear();

    FighterState specialS = specialN;
    specialS.name = "SpecialS";
    specialS.animation = "SpecialS";

    FighterState specialHi = specialN;
    specialHi.name = "SpecialHi";
    specialHi.animation = "SpecialHi";

    FighterState specialLw = specialN;
    specialLw.name = "SpecialLw";
    specialLw.animation = "SpecialLw";

    FighterState specialAirN = specialN;
    specialAirN.name = "SpecialAirN";
    specialAirN.animation = "SpecialAirN";
    specialAirN.onFrame = {call("process_airborne_fastfall")};
    specialAirN.onAirborne.clear();
    specialAirN.onLanding = {call("regular_landing")};
    specialAirN.onAnimationFinishedState = "Fall";
    specialAirN.interrupts.clear();

    FighterState specialAirS = specialAirN;
    specialAirS.name = "SpecialAirS";
    specialAirS.animation = "SpecialSAir";

    FighterState specialAirHi = specialAirN;
    specialAirHi.name = "SpecialAirHi";
    specialAirHi.animation = "SpecialAirHi";
    specialAirHi.onAnimationFinishedState = "FallSpecial";

    FighterState specialAirLw = specialAirN;
    specialAirLw.name = "SpecialAirLw";
    specialAirLw.animation = "SpecialAirLw";

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
    ottotto.onAirborne = {call("regular_airborne")};
    ottotto.onAnimationFinishedState = "OttottoWait";
    ottotto.interrupts = ottottoIasaInterrupts();

    FighterState ottottoWait = ottotto;
    ottottoWait.name = "OttottoWait";
    ottottoWait.animation = "OttottoWait";
    ottottoWait.animationLengthFrames = 60;
    ottottoWait.loopAnimation = true;
    ottottoWait.onAnimationFinishedState = "";

    FighterState missFoot = fallState;
    missFoot.name = "MissFoot";
    missFoot.animation = "MissFoot";
    missFoot.animationLengthFrames = 30;
    missFoot.onEnter = {call("common_enter")};
    missFoot.onFrame = {call("process_miss_foot")};
    missFoot.onLanding = {call("regular_landing")};
    missFoot.onAnimationFinishedState.clear();
    missFoot.interrupts.clear();
    missFoot.interrupts.push_back(
        interrupt("PassiveWallJump", InterruptCondition::WallJumpInput, GroundRequirement::OnlyAirborne));

    const std::vector<InterruptRule> guardInterrupts = {
        interrupt("EscapeN", InterruptCondition::SpotDodgeInput, GroundRequirement::OnlyGrounded),
        interrupt("EscapeF", InterruptCondition::RollForwardInput, GroundRequirement::OnlyGrounded),
        interrupt("EscapeB", InterruptCondition::RollBackwardInput, GroundRequirement::OnlyGrounded),
        interrupt("Catch", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded),
        interrupt("JumpSquat", InterruptCondition::ShieldJumpPressed, GroundRequirement::OnlyGrounded),
    };

    FighterState guardOn;
    guardOn.name = "GuardOn";
    guardOn.animation = "GuardOn";
    guardOn.animationLengthFrames = fighter.properties.common.guardMinHoldFramesX268;
    guardOn.onEnter = {call("common_enter"), call("enter_guard_on")};
    guardOn.onFrame = {call("process_guard_on")};
    guardOn.onAirborne = {call("miss_foot_or_airborne")};
    guardOn.onAnimationFinishedState = "Guard";
    guardOn.interrupts = guardOnIasaInterrupts(fighter.properties.common.shieldReflectInputWindowX2A0);

    FighterState guardReflect;
    guardReflect.name = "GuardReflect";
    guardReflect.animation = "GuardReflect";
    guardReflect.animationLengthFrames = fighter.properties.common.guardMinHoldFramesX268;
    guardReflect.onEnter = {call("common_enter"), call("enter_guard_on")};
    guardReflect.onFrame = {call("process_guard_reflect")};
    guardReflect.onAirborne = {call("miss_foot_or_airborne")};
    guardReflect.onAnimationFinishedState = "Guard";
    guardReflect.interrupts = guardOnIasaInterrupts(0);

    FighterState guard;
    guard.name = "Guard";
    guard.animation = "Guard";
    guard.animationLengthFrames = 60;
    guard.loopAnimation = true;
    guard.onEnter = {call("common_enter")};
    guard.onFrame = {call("process_guard")};
    guard.onAirborne = {call("miss_foot_or_airborne")};
    guard.interrupts = guardInterrupts;

    FighterState guardOff;
    guardOff.name = "GuardOff";
    guardOff.animation = "GuardOff";
    guardOff.animationLengthFrames = 16;
    guardOff.onEnter = {call("common_enter")};
    guardOff.onFrame = {call("process_landing")};
    guardOff.onAirborne = {call("miss_foot_or_airborne")};
    guardOff.onAnimationFinishedState = "Wait";
    guardOff.interrupts = {
        interrupt("Catch", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded),
        interrupt("Attack11", InterruptCondition::AttackPressed, GroundRequirement::OnlyGrounded),
        interrupt("EscapeN", InterruptCondition::SpotDodgeInput, GroundRequirement::OnlyGrounded),
        interrupt("JumpSquat", InterruptCondition::ShieldJumpPressed, GroundRequirement::OnlyGrounded),
    };
    expandGroundedAttackShortcut(guardOff);

    FighterState guardSetoff;
    guardSetoff.name = "GuardSetOff";
    guardSetoff.animation = "GuardSetOff";
    guardSetoff.animationLengthFrames = 30;
    guardSetoff.onEnter = {call("common_enter")};
    guardSetoff.onFrame = {call("process_guard_setoff")};
    guardSetoff.onAirborne = {call("miss_foot_or_airborne")};

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
    catchCut.onAirborne = {call("regular_airborne")};
    catchCut.onLanding = {call("regular_landing")};
    catchCut.onAnimationFinishedState.clear();

    auto makeThrowState = [&](const std::string& name, int length) {
        FighterState state;
        state.name = name;
        state.animation = name;
        state.animationLengthFrames = length;
        state.onEnter = {call("common_enter"), call("enter_throw")};
        state.onFrame = {call("process_throw")};
        state.onAirborne = {call("throw_airborne")};
        state.onAnimationFinishedState.clear();
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
    captureCut.onAirborne = {call("capture_cut_airborne")};

    FighterState captureJump = fallState;
    captureJump.name = "CaptureJump";
    captureJump.animation = "CaptureJump";
    captureJump.animationLengthFrames = 35;
    captureJump.loopAnimation = false;
    captureJump.allowLedgeGrab = false;
    captureJump.onEnter = {call("common_enter"), call("enter_capture_jump")};
    captureJump.onFrame = {call("process_capture_jump")};
    captureJump.onLanding = {call("capture_jump_landing")};
    captureJump.interrupts = airborneActionInterrupts(false);
    delayInterrupts(
        captureJump.interrupts,
        roundedFixFrames(fighter.properties.common.captureJumpGravityThresholdX3B8) + 1);
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
    FighterState thrownLwWomen = makeThrownState("ThrownLwWomen");
    FighterState thrownFF = makeThrownState("ThrownFF");
    FighterState thrownFB = makeThrownState("ThrownFB");
    FighterState thrownFHi = makeThrownState("ThrownFHi");
    FighterState thrownFLw = makeThrownState("ThrownFLw");

    auto makeEmptyCaptureState = [&](const std::string& name, bool airborne) {
        FighterState state;
        state.name = name;
        state.animation = name;
        state.animationLengthFrames = 60;
        state.onEnter = {call("common_enter")};
        state.allowLedgeGrab = false;
        state.allowWallCollision = airborne;
        state.allowCeilingCollision = airborne;
        return state;
    };
    FighterState captureMewtwo = makeEmptyCaptureState("CaptureMewtwo", false);
    FighterState captureMewtwoAir = makeEmptyCaptureState("CaptureMewtwoAir", true);
    FighterState captureYoshi = makeEmptyCaptureState("CaptureYoshi", false);
    captureYoshi.animation = "CapturePulledLw";
    FighterState captureNeck = makeEmptyCaptureState("CaptureNeck", false);
    FighterState captureFoot = makeEmptyCaptureState("CaptureFoot", false);
    FighterState captureCaptain = makeEmptyCaptureState("CaptureCaptain", false);
    FighterState captureKoopa = makeEmptyCaptureState("CaptureKoopa", false);
    FighterState captureKoopaAir = makeEmptyCaptureState("CaptureKoopaAir", true);

    auto makeStaticThrownState = [&](const std::string& name, bool airborne) {
        FighterState state;
        state.name = name;
        state.animation = name;
        state.animationLengthFrames = 60;
        state.onEnter = {call("common_enter")};
        state.allowLedgeGrab = false;
        state.allowWallCollision = airborne;
        state.allowCeilingCollision = airborne;
        return state;
    };
    FighterState thrownMewtwo = makeStaticThrownState("ThrownMewtwo", false);
    FighterState thrownMewtwoAir = makeStaticThrownState("ThrownMewtwoAir", true);
    FighterState thrownKoopaF = makeStaticThrownState("ThrownKoopaF", false);
    FighterState thrownKoopaB = makeStaticThrownState("ThrownKoopaB", false);
    FighterState thrownKoopaAirF = makeStaticThrownState("ThrownKoopaAirF", true);
    FighterState thrownKoopaAirB = makeStaticThrownState("ThrownKoopaAirB", true);

    FighterState escapeN;
    escapeN.name = "EscapeN";
    escapeN.animation = "EscapeN";
    escapeN.animationLengthFrames = 22;
    escapeN.onEnter = {call("common_enter")};
    escapeN.onFrame = {call("process_landing")};
    escapeN.onAirborne = {call("regular_airborne")};
    escapeN.onAnimationFinishedState = "Wait";
    // ftCo_Escape*_Coll calls ft_80084104; unlike Kirby's dash attack
    // exception, rolls/spotdodge should not become airborne from root motion.
    escapeN.allowSlideoff = false;
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
    escapeF.onFrame = {call("process_escape_ground")};
    escapeF.action.clear();

    FighterState escapeB = escapeF;
    escapeB.name = "EscapeB";
    escapeB.animation = "EscapeB";

    FighterState appealSR;
    appealSR.name = "AppealSR";
    appealSR.animation = "AppealSR";
    appealSR.animationLengthFrames = 60;
    appealSR.onEnter = {call("common_enter")};
    appealSR.onFrame = {call("process_landing")};
    appealSR.onAirborne = {call("regular_airborne")};
    appealSR.onAnimationFinishedState = "Wait";
    appealSR.useAnimPhysics = true;
    appealSR.initialInterruptibleFrame = 1000000;
    appealSR.interrupts = {
        interrupt("Catch", InterruptCondition::GrabPressed, GroundRequirement::OnlyGrounded),
    };
    appendRules(appealSR.interrupts, groundedAttackInterrupts(false));
    appendRules(appealSR.interrupts, {
        interrupt("GuardReflect", InterruptCondition::ShieldReflectInput, GroundRequirement::OnlyGrounded),
        interrupt("GuardOn", InterruptCondition::ShieldHeld, GroundRequirement::OnlyGrounded),
    });

    FighterState appealSL = appealSR;
    appealSL.name = "AppealSL";
    appealSL.animation = "AppealSL";

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
    shieldBreakFall.onEnter = {call("common_enter"), call("enter_shield_break_fall")};
    shieldBreakFall.onFrame = {call("process_shield_break_air")};
    shieldBreakFall.onLanding = {call("shield_break_landing")};

    FighterState shieldBreakDown;
    shieldBreakDown.name = "ShieldBreakDownU";
    shieldBreakDown.animation = "ShieldBreakDownU";
    shieldBreakDown.animationLengthFrames = 30;
    shieldBreakDown.onEnter = {call("common_enter")};
    shieldBreakDown.onFrame = {call("process_landing")};
    shieldBreakDown.onAirborne = {call("regular_airborne")};
    shieldBreakDown.onAnimationFinishedState = "ShieldBreakStandU";

    FighterState shieldBreakDownD = shieldBreakDown;
    shieldBreakDownD.name = "ShieldBreakDownD";
    shieldBreakDownD.animation = "ShieldBreakDownD";
    shieldBreakDownD.onAnimationFinishedState = "ShieldBreakStandD";

    FighterState shieldBreakStand;
    shieldBreakStand.name = "ShieldBreakStandU";
    shieldBreakStand.animation = "ShieldBreakStandU";
    shieldBreakStand.animationLengthFrames = 60;
    shieldBreakStand.onEnter = {call("common_enter")};
    shieldBreakStand.onFrame = {call("process_landing")};
    shieldBreakStand.onAirborne = {call("regular_airborne")};
    shieldBreakStand.onAnimationFinishedState = "Furafura";

    FighterState shieldBreakStandD = shieldBreakStand;
    shieldBreakStandD.name = "ShieldBreakStandD";
    shieldBreakStandD.animation = "ShieldBreakStandD";

    FighterState furafura;
    furafura.name = "Furafura";
    furafura.animation = "Furafura";
    furafura.animationLengthFrames = 60;
    furafura.loopAnimation = true;
    furafura.onEnter = {call("common_enter"), call("enter_furafura")};
    furafura.onFrame = {call("process_furafura")};
    furafura.onAirborne = {call("regular_airborne")};

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
    cliffWait.onEnter = {call("common_enter"), call("enter_cliff_wait")};
    cliffWait.onFrame = {call("process_cliff_wait")};
    cliffWait.onAnimationFinishedState = "";
    cliffWait.interrupts = {
        interrupt("CliffAttack", InterruptCondition::LedgeAttackInput, GroundRequirement::OnlyAirborne),
        interrupt("CliffEscape", InterruptCondition::LedgeEscapeInput, GroundRequirement::OnlyAirborne),
        interrupt("CliffJump", InterruptCondition::JumpPressed, GroundRequirement::OnlyAirborne),
        interrupt("CliffClimb", InterruptCondition::LedgeClimbInput, GroundRequirement::OnlyAirborne),
        interrupt("CliffDrop", InterruptCondition::LedgeDropInput, GroundRequirement::OnlyAirborne),
    };

    FighterState cliffClimb;
    cliffClimb.name = "CliffClimb";
    cliffClimb.animation = "CliffClimb";
    cliffClimb.animationLengthFrames = 30;
    cliffClimb.onEnter = {call("common_enter")};
    cliffClimb.onFrame = {call("process_cliff_climb")};
    FighterState cliffClimbSlow = cliffClimb;
    cliffClimbSlow.name = "CliffClimbSlow";
    cliffClimbSlow.animation = "CliffClimbSlow";
    cliffClimbSlow.animationLengthFrames = 55;
    FighterState cliffClimbQuick = cliffClimb;
    cliffClimbQuick.name = "CliffClimbQuick";
    cliffClimbQuick.animation = "CliffClimbQuick";

    FighterState cliffEscape;
    cliffEscape.name = "CliffEscape";
    cliffEscape.animation = "CliffEscape";
    cliffEscape.animationLengthFrames = 34;
    cliffEscape.onEnter = {call("common_enter")};
    cliffEscape.onFrame = {call("process_cliff_escape")};
    FighterState cliffEscapeSlow = cliffEscape;
    cliffEscapeSlow.name = "CliffEscapeSlow";
    cliffEscapeSlow.animation = "CliffEscapeSlow";
    cliffEscapeSlow.animationLengthFrames = 49;
    FighterState cliffEscapeQuick = cliffEscape;
    cliffEscapeQuick.name = "CliffEscapeQuick";
    cliffEscapeQuick.animation = "CliffEscapeQuick";

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
    FighterState cliffAttackSlow = cliffAttack;
    cliffAttackSlow.name = "CliffAttackSlow";
    cliffAttackSlow.animation = "CliffAttackSlow";
    cliffAttackSlow.animationLengthFrames = 54;
    FighterState cliffAttackQuick = cliffAttack;
    cliffAttackQuick.name = "CliffAttackQuick";
    cliffAttackQuick.animation = "CliffAttackQuick";

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
    cliffJump.onAnimationFinishedState = "CliffJumpAir";
    FighterState cliffJumpSlow1 = cliffJump;
    cliffJumpSlow1.name = "CliffJumpSlow1";
    cliffJumpSlow1.animation = "CliffJumpSlow1";
    cliffJumpSlow1.animationLengthFrames = 11;
    cliffJumpSlow1.onAnimationFinishedState = "CliffJumpSlow2";
    FighterState cliffJumpQuick1 = cliffJump;
    cliffJumpQuick1.name = "CliffJumpQuick1";
    cliffJumpQuick1.animation = "CliffJumpQuick1";
    cliffJumpQuick1.onAnimationFinishedState = "CliffJumpQuick2";

    FighterState cliffJumpAir = fallState;
    cliffJumpAir.name = "CliffJumpAir";
    cliffJumpAir.animation = "CliffJumpAir";
    cliffJumpAir.animationLengthFrames = 36;
    cliffJumpAir.loopAnimation = false;
    cliffJumpAir.onEnter = {call("common_enter"), call("enter_cliff_jump_air")};
    cliffJumpAir.onFrame = {call("process_cliff_jump_air")};
    cliffJumpAir.interrupts.clear();
    cliffJumpAir.interrupts.push_back(
        interrupt("PassiveWallJump", InterruptCondition::WallJumpInput, GroundRequirement::OnlyAirborne));
    cliffJumpAir.onAnimationFinishedState = "Fall";
    FighterState cliffJumpSlow2 = cliffJumpAir;
    cliffJumpSlow2.name = "CliffJumpSlow2";
    cliffJumpSlow2.animation = "CliffJumpSlow2";
    FighterState cliffJumpQuick2 = cliffJumpAir;
    cliffJumpQuick2.name = "CliffJumpQuick2";
    cliffJumpQuick2.animation = "CliffJumpQuick2";

    auto markStates = [](
        std::initializer_list<FighterState*> states,
        FighterStateCategory category,
        FighterStatePreviewFixture fixture)
    {
        for (FighterState* state : states) {
            state->category = category;
            state->previewFixture = fixture;
        }
    };
    markStates({
        &waitState, &walkSlow, &walkMiddle, &walkFast, &dashState, &runState, &runDirect,
        &runBrake, &turnState, &turnRun, &jumpSquat, &squat, &squatWait, &squatRv,
        &landing, &landingAirN, &landingAirF, &landingAirB, &landingAirHi, &landingAirLw,
        &landingFallSpecial, &landingFallSpecialInterruptible, &ottotto, &ottottoWait,
        &missFoot, &guardOn, &guardReflect, &guard, &guardOff, &guardSetoff,
        &escapeN, &escapeF, &escapeB, &appealSR, &appealSL, &shieldBreakDown,
        &shieldBreakDownD, &shieldBreakStand, &shieldBreakStandD, &furafura,
    }, FighterStateCategory::Ground, FighterStatePreviewFixture::Grounded);
    markStates({
        &fallState, &fallAerial, &pass, &escapeAir, &fallSpecial, &passiveWallJump,
        &itemScrewAir, &damageScrewAir, &damageIceJump, &buryJump, &passiveWall,
        &passiveCeil, &wallDamage, &flyReflectWall, &flyReflectCeil, &downReflect,
        &stopWall, &stopCeil, &jumpF, &jumpB, &jumpAerialF, &jumpAerialB,
        &shieldBreakFly, &shieldBreakFall, &cliffJumpAir, &cliffJumpSlow2, &cliffJumpQuick2,
    }, FighterStateCategory::Air, FighterStatePreviewFixture::Airborne);
    markStates({
        &attack11, &attack12, &attack13, &attack100Start, &attack100Loop, &attack100End,
        &attackDash, &attackS3Hi, &attackS3HiS, &attackS3, &attackS3LwS, &attackS3Lw,
        &attackHi3, &attackLw3, &attackS4Hi, &attackS4HiS, &attackS4, &attackS4LwS,
        &attackS4Lw, &attackS42, &attackHi4, &attackLw4,
    }, FighterStateCategory::Attack, FighterStatePreviewFixture::Grounded);
    markStates({
        &airAttack, &airAttackF, &airAttackB, &airAttackHi, &airAttackLw,
    }, FighterStateCategory::Attack, FighterStatePreviewFixture::Airborne);
    markStates({
        &specialN, &specialS, &specialHi, &specialLw,
    }, FighterStateCategory::Special, FighterStatePreviewFixture::Grounded);
    markStates({
        &specialAirN, &specialAirS, &specialAirHi, &specialAirLw,
    }, FighterStateCategory::Special, FighterStatePreviewFixture::Airborne);
    markStates({
        &damageHi1, &damageHi2, &damageHi3, &damageN1, &damageN2, &damageN3,
        &damageLw1, &damageLw2, &damageLw3, &damageAir1, &damageAir2, &damageAir3,
        &damageFlyHi, &damageFlyN, &damageFlyLw, &damageFlyTop, &damageFlyRoll,
        &damageFall, &itemScrew, &damageScrew, &damageSong, &damageSongWait,
        &damageSongRv, &damageBind, &damageIce, &bury, &buryWait, &reboundStop,
        &rebound, &downBoundU, &downWaitU, &downDamageU, &downStandU, &downAttackU,
        &downForwardU, &downBackU, &downSpotU, &downBoundD, &downWaitD,
        &downDamageD, &downStandD, &downAttackD, &downForwardD, &downBackD,
        &downSpotD, &passive, &passiveStandF, &passiveStandB,
    }, FighterStateCategory::Damage, FighterStatePreviewFixture::Grounded);
    markStates({
        &damageAir1, &damageAir2, &damageAir3, &damageFlyHi, &damageFlyN, &damageFlyLw,
        &damageFlyTop, &damageFlyRoll, &damageFall,
    }, FighterStateCategory::Damage, FighterStatePreviewFixture::Airborne);
    markStates({
        &catchState, &catchPull, &catchDash, &catchDashPull, &catchWait, &catchAttack,
        &catchCut, &throwF, &throwB, &throwHi, &throwLw, &capturePulledHi,
        &captureWaitHi, &captureDamageHi, &capturePulledLw, &captureWaitLw,
        &captureDamageLw, &captureCut, &thrownF, &thrownB, &thrownHi, &thrownLw,
        &thrownLwWomen, &thrownFF, &thrownFB, &thrownFHi, &thrownFLw,
        &captureMewtwo, &captureYoshi, &captureNeck, &captureFoot, &captureCaptain,
        &captureKoopa, &thrownKoopaF, &thrownKoopaB, &thrownMewtwo,
    }, FighterStateCategory::Throw, FighterStatePreviewFixture::Grounded);
    markStates({
        &captureJump, &captureMewtwoAir, &captureKoopaAir, &thrownKoopaAirF,
        &thrownKoopaAirB, &thrownMewtwoAir,
    }, FighterStateCategory::Throw, FighterStatePreviewFixture::Airborne);
    markStates({
        &cliffCatch, &cliffWait, &cliffClimb, &cliffClimbSlow, &cliffClimbQuick,
        &cliffEscape, &cliffEscapeSlow, &cliffEscapeQuick, &cliffAttack,
        &cliffAttackSlow, &cliffAttackQuick, &cliffDrop, &cliffJump,
        &cliffJumpSlow1, &cliffJumpQuick1,
    }, FighterStateCategory::Ledge, FighterStatePreviewFixture::Ledge);

    for (FighterState* state : {
        &waitState, &walkSlow, &walkMiddle, &walkFast, &dashState, &runState, &runDirect,
        &turnState, &squat, &squatWait, &squatRv, &landing, &ottotto, &ottottoWait,
    }) {
        expandGroundedAttackShortcut(*state, state == &dashState || state == &runState || state == &runDirect);
    }

    fighter.states = {
        waitState, walkSlow, walkMiddle, walkFast, dashState, runState, runDirect, runBrake, turnState, turnRun,
        jumpSquat, squat, squatWait, squatRv, jumpF, jumpB, jumpAerialF, jumpAerialB, fallState, fallAerial, pass, escapeAir, fallSpecial,
        specialN, specialS, specialHi, specialLw, specialAirN, specialAirS, specialAirHi, specialAirLw,
        passiveWallJump, landing, landingAirN, landingAirF, landingAirB, landingAirHi, landingAirLw, landingFallSpecial, landingFallSpecialInterruptible,
        damageHi1, damageHi2, damageHi3, damageN1, damageN2, damageN3, damageLw1, damageLw2, damageLw3,
        damageAir1, damageAir2, damageAir3, damageFlyHi, damageFlyN, damageFlyLw, damageFlyTop, damageFlyRoll, damageFall,
        itemScrew, itemScrewAir, damageScrew, damageScrewAir, damageSong, damageSongWait, damageSongRv, damageBind, damageIce, damageIceJump, bury, buryWait, buryJump,
        reboundStop, rebound,
        downBoundU, downWaitU, downDamageU, downStandU, downAttackU, downForwardU, downBackU, downSpotU,
        downBoundD, downWaitD, downDamageD, downStandD, downAttackD, downForwardD, downBackD, downSpotD,
        passive, passiveStandF, passiveStandB, passiveWall, passiveCeil, wallDamage, flyReflectWall, flyReflectCeil, downReflect, stopWall, stopCeil,
        ottotto, ottottoWait, missFoot, guardOn, guardReflect, guard, guardOff, guardSetoff,
        catchState, catchPull, catchDash, catchDashPull, catchWait, catchAttack, catchCut,
        throwF, throwB, throwHi, throwLw,
        capturePulledHi, captureWaitHi, captureDamageHi, capturePulledLw, captureWaitLw, captureDamageLw, captureCut, captureJump,
        thrownF, thrownB, thrownHi, thrownLw, thrownLwWomen,
        thrownFF, thrownFB, thrownFHi, thrownFLw,
        captureMewtwo, captureMewtwoAir, captureYoshi, captureNeck, captureFoot,
        captureCaptain, captureKoopa, captureKoopaAir,
        thrownKoopaF, thrownKoopaB, thrownKoopaAirF, thrownKoopaAirB, thrownMewtwo, thrownMewtwoAir,
        escapeN, escapeF, escapeB, appealSR, appealSL, shieldBreakFly, shieldBreakFall, shieldBreakDown, shieldBreakDownD, shieldBreakStand, shieldBreakStandD, furafura, cliffCatch, cliffWait,
        cliffClimb, cliffClimbSlow, cliffClimbQuick,
        cliffEscape, cliffEscapeSlow, cliffEscapeQuick,
        cliffAttack, cliffAttackSlow, cliffAttackQuick,
        cliffDrop, cliffJump, cliffJumpSlow1, cliffJumpQuick1, cliffJumpAir, cliffJumpSlow2, cliffJumpQuick2,
        attack11, attack12, attack13, attack100Start, attack100Loop, attack100End, attackDash,
        attackS3Hi, attackS3HiS, attackS3, attackS3LwS, attackS3Lw, attackHi3, attackLw3,
        attackS4Hi, attackS4HiS, attackS4, attackS4LwS, attackS4Lw, attackS42, attackHi4, attackLw4,
        airAttack, airAttackF, airAttackB, airAttackHi, airAttackLw
    };
    assignMeleeActionIndices(fighter);
    return fighter;
}

} // namespace pf
