#include "core/melee_fighter_converter.hpp"

#include "core/hsd_action_import.hpp"
#include "core/imported_fighter_asset.hpp"
#include "core/simulation.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace pf {

struct HsdFighterAssetSpec {
    const char* displayName;
    const char* fileName;
    bool shieldSizeScalesWithHealth = true;
};

static const std::array<HsdFighterAssetSpec, 27>& meleeTrainingRoster() {
    static const std::array<HsdFighterAssetSpec, 27> roster{{
        {"Mario", "mario_hsd.pfighter.bin"},
        {"Donkey Kong", "donkey_kong_hsd.pfighter.bin"},
        {"Link", "link_hsd.pfighter.bin"},
        {"Samus", "samus_hsd.pfighter.bin"},
        {"Yoshi", "yoshi_hsd.pfighter.bin", false},
        {"Kirby", "kirby_hsd.pfighter.bin"},
        {"Fox", "fox_hsd.pfighter.bin"},
        {"Pikachu", "pikachu_hsd.pfighter.bin"},
        {"Luigi", "luigi_hsd.pfighter.bin"},
        {"Captain Falcon", "captain_falcon_hsd.pfighter.bin"},
        {"Ness", "ness_hsd.pfighter.bin"},
        {"Bowser", "bowser_hsd.pfighter.bin"},
        {"Peach", "peach_hsd.pfighter.bin"},
        {"Zelda", "zelda_hsd.pfighter.bin"},
        {"Sheik", "sheik_hsd.pfighter.bin"},
        {"Ice Climbers", "ice_climbers_hsd.pfighter.bin"},
        {"Marth", "marth_hsd.pfighter.bin"},
        {"Game & Watch", "game_and_watch_hsd.pfighter.bin"},
        {"Falco", "falco_hsd.pfighter.bin"},
        {"Ganondorf", "ganondorf_hsd.pfighter.bin"},
        {"Young Link", "young_link_hsd.pfighter.bin"},
        {"Dr. Mario", "dr_mario_hsd.pfighter.bin"},
        {"Roy", "roy_hsd.pfighter.bin"},
        {"Pichu", "pichu_hsd.pfighter.bin"},
        {"Mewtwo", "mewtwo_hsd.pfighter.bin"},
        {"Jigglypuff", "jigglypuff_hsd.pfighter.bin"},
        {"Sandbag", "sandbag_hsd.pfighter.bin"},
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


static const HsdFighterAssetSpec* meleeTrainingRosterSpecByName(const std::string& fighterName) {
    for (const HsdFighterAssetSpec& spec : meleeTrainingRoster()) {
        if (fighterName == spec.displayName || fighterName == spec.fileName) {
            return &spec;
        }
    }
    return nullptr;
}

std::vector<std::string> meleeTrainingRosterFighterNames() {
    std::vector<std::string> names;
    names.reserve(meleeTrainingRoster().size());
    for (const HsdFighterAssetSpec& spec : meleeTrainingRoster()) {
        names.push_back(spec.displayName);
    }
    return names;
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


static int roundedFrames(Fix value) {
    return std::max(0, static_cast<int>(fxToFloat(value) + 0.5f));
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
    attr.damageScrewVerticalVelocity = source.damageScrewVerticalVelocity != 0
        ? source.damageScrewVerticalVelocity
        : source.initialVerticalJumpVelocity;
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
    attr.weightIndependentThrowsMask = source.weightIndependentThrowsMask;
    attr.rapidJabWindow = source.rapidJabWindow;
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
    attr.passiveWallHorizontalVelocity = source.passiveWallHorizontalVelocity;
    attr.wallJumpHorizontalVelocity = source.wallJumpHorizontalVelocity;
    attr.wallJumpVerticalVelocity = source.wallJumpVerticalVelocity;
    attr.passiveCeilHorizontalVelocity = source.passiveCeilHorizontalVelocity;
    attr.ledgeJumpHorizontalVelocity = source.ledgeJumpHorizontalVelocity;
    attr.ledgeJumpVerticalVelocity = source.ledgeJumpVerticalVelocity;
    attr.damageIceJumpVelocityY = source.damageIceJumpVelocityY;
    attr.damageIceJumpVelocityXMultiplier = source.damageIceJumpVelocityXMultiplier;
    def.shield.startSizeHardShield = source.shieldSize;
    def.shield.maxHealth = attr.common.startShieldHealthX260;
}

static void applyHsdAnimationLengths(FighterDefinition& def, const HsdFighterAnimationAsset& asset) {
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
        if (const AnimationClip* clip = findClipByActionIndex(asset, actionIndex)) {
            if (state.name != "JumpSquat") {
                state.animationLengthFrames = std::max(1, static_cast<int>(fxToFloat(clip->frameCount) + 0.5f));
            }
            state.defaultAnimationBlendFrames = std::max(0, clip->defaultBlendFrames);
            state.useAnimPhysics = usesGenericTransNPhysics(state, *clip);
            state.loopAnimation = state.loopAnimation || (clip->actionFlags & kMeleeActionFlagLoopAnimation) != 0;
        }
    }
}

static const HsdActionScript* hsdActionScriptForNativeImport(const HsdFighterAnimationAsset& asset, const FighterState& state) {
    const int actionIndex = state.animationActionIndex >= 0 ? state.animationActionIndex : fallbackActionIndex(state.animation);
    if (actionIndex >= 0) {
        if (const HsdActionScript* script = findActionScriptByActionIndex(asset, actionIndex)) {
            return script;
        }
    }
    const std::string suffix = "_ACTION_" + state.animation + "_figatree";
    for (const HsdActionScript& script : asset.actionScripts) {
        if (!state.animation.empty() &&
            script.name.size() >= suffix.size() &&
            script.name.compare(script.name.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            return &script;
        }
    }
    return nullptr;
}

static void importHsdActionScriptsAsNativeSubactions(FighterDefinition& def, const HsdFighterAnimationAsset& asset) {
    for (FighterState& state : def.states) {
        const HsdActionScript* script = hsdActionScriptForNativeImport(asset, state);
        if (!script) {
            continue;
        }
        state.action = decodeHsdActionScript(asset, *script);
        if (state.animationActionIndex < 0) {
            state.animationActionIndex = script->actionIndex;
        }
    }
}


static void applyCommonStateTimings(FighterDefinition& def) {
    const int buryJumpIndex = def.stateIndex("BuryJump");
    if (buryJumpIndex >= 0) {
        FighterState& buryJump = def.states[static_cast<size_t>(buryJumpIndex)];
        const int enableFrame = roundedFrames(def.properties.common.buryJumpGravityThresholdX61C) + 1;
        for (InterruptRule& rule : buryJump.interrupts) {
            rule.startActive = false;
            rule.enableFrame = enableFrame;
            rule.disableFrame = 0;
        }
    }

    const int captureJumpIndex = def.stateIndex("CaptureJump");
    if (captureJumpIndex >= 0) {
        FighterState& captureJump = def.states[static_cast<size_t>(captureJumpIndex)];
        const int enableFrame = roundedFrames(def.properties.common.captureJumpGravityThresholdX3B8) + 1;
        for (InterruptRule& rule : captureJump.interrupts) {
            rule.startActive = false;
            rule.enableFrame = enableFrame;
            rule.disableFrame = 0;
        }
    }
}

static HurtboxDefinition nativeHurtboxFromImported(const HsdHurtbox& source) {
    HurtboxDefinition out;
    out.bone = BoneId::Hip;
    out.joint = source.bone;
    out.type = source.type;
    out.startOffset = source.start;
    out.endOffset = source.end;
    out.radius = source.radius;
    out.grabbable = source.grabbable;
    return out;
}

static FighterDefinition makeImportedFighterDefinition(
    const HsdFighterAssetSpec& spec,
    const MeleeCommonData& common,
    const HsdFighterAnimationAsset& asset)
{
    FighterDefinition def = makeDebugRook();
    def.name = spec.displayName;
    def.importProvenance.sourceFileName = spec.fileName;
    def.importProvenance.sourceAssetName = asset.name;
    def.properties.common = common;
    def.shield.maxHealth = common.startShieldHealthX260;
    applyCommonStateTimings(def);
    def.authoredSkeleton = asset.skeleton;
    def.authoredClips = asset.clips;
    def.authoredMesh = asset.mesh;
    def.modelPartAnimations = asset.modelPartAnimations;
    def.fighterBones = asset.fighterBones;
    def.commonBoneLookup = asset.commonBoneLookup;
    def.hasShieldPose = asset.hasShieldPose;
    def.shieldPose = def.hasShieldPose ? asset.shieldPose : AnimationPose{};
    def.hurtboxes.clear();
    def.hurtboxes.reserve(asset.hurtboxes.size());
    for (const HsdHurtbox& hurtbox : asset.hurtboxes) {
        def.hurtboxes.push_back(nativeHurtboxFromImported(hurtbox));
    }
    if (asset.hasAttributes) {
        applyHsdFighterAttributes(def, asset.attributes);
    }
    def.properties.shieldSizeScalesWithHealth = spec.shieldSizeScalesWithHealth;
    if (asset.hasEnvironmentCollision) {
        def.environmentCollisionBones = asset.environmentCollision.bones;
        def.environmentCollisionMultiplier = asset.environmentCollision.multiplier;
        def.properties.ledgeSnapX = asset.environmentCollision.ledgeGrabWidth;
        def.properties.ledgeSnapY = asset.environmentCollision.ledgeGrabYOffset;
        def.properties.ledgeSnapHeight = asset.environmentCollision.ledgeGrabHeight;
    }
    applyHsdAnimationLengths(def, asset);
    importHsdActionScriptsAsNativeSubactions(def, asset);
    return def;
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

static void uniquifyAnimationClipNames(std::vector<AnimationClip>& clips) {
    std::vector<std::string> names;
    names.reserve(clips.size());
    for (AnimationClip& clip : clips) {
        std::string base = clip.name.empty() ? "Clip" : clip.name;
        std::string candidate = base;
        int suffix = 2;
        while (std::find(names.begin(), names.end(), candidate) != names.end()) {
            candidate = base + "_" + std::to_string(suffix++);
        }
        clip.name = candidate;
        names.push_back(candidate);
    }
}

static void uniquifyAnimationClipActionIndexes(std::vector<AnimationClip>& clips) {
    int nextActionIndex = 0;
    for (const AnimationClip& clip : clips) {
        nextActionIndex = std::max(nextActionIndex, clip.actionIndex + 1);
    }
    std::vector<int> actionIndexes;
    actionIndexes.reserve(clips.size());
    for (AnimationClip& clip : clips) {
        if (clip.actionIndex < 0 ||
            std::find(actionIndexes.begin(), actionIndexes.end(), clip.actionIndex) != actionIndexes.end())
        {
            while (std::find(actionIndexes.begin(), actionIndexes.end(), nextActionIndex) != actionIndexes.end()) {
                ++nextActionIndex;
            }
            clip.actionIndex = nextActionIndex++;
        }
        actionIndexes.push_back(clip.actionIndex);
    }
}


static bool failNativeConversion(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
    return false;
}

static bool validateImportedFighterConversionSource(
    const FighterDefinition& source,
    const HsdFighterAnimationAsset& asset,
    std::string* error)
{
    if (asset.hasShieldPose && asset.shieldPose.joints.size() != source.authoredSkeleton.size()) {
        std::ostringstream message;
        message << "imported fighter '" << source.name << "' has shield pose/skeleton joint count mismatch: "
            << "shieldPoseJoints=" << asset.shieldPose.joints.size()
            << " skeletonJoints=" << source.authoredSkeleton.size()
            << " sourceAsset=" << asset.name;
        return failNativeConversion(error, message.str());
    }
    return true;
}


static bool makeImportedNativePackageFighterDefinition(
    const FighterDefinition& source,
    const HsdFighterAnimationAsset& asset,
    FighterDefinition& out,
    std::string* error)
{
    out = source;
    if (!validateImportedFighterConversionSource(source, asset, error)) {
        return false;
    }

    if (out.authoredSkeleton.empty()) {
        out.authoredSkeleton = asset.skeleton;
    }
    if (out.authoredClips.empty()) {
        out.authoredClips = asset.clips;
    }
    removeOutOfRangeAnimationTracks(out.authoredClips, out.authoredSkeleton.size());
    if (out.modelPartAnimations.empty()) {
        out.modelPartAnimations = asset.modelPartAnimations;
    }
    if (out.authoredMesh.batches.empty() && out.authoredMesh.textures.empty()) {
        out.authoredMesh = asset.mesh;
    }
    out.fighterBones = asset.fighterBones;
    out.commonBoneLookup = asset.commonBoneLookup;
    out.hasShieldPose = asset.hasShieldPose;
    out.shieldPose = out.hasShieldPose ? asset.shieldPose : AnimationPose{};
    if (out.hurtboxes.empty()) {
        out.hurtboxes.reserve(asset.hurtboxes.size());
        for (const HsdHurtbox& hurtbox : asset.hurtboxes) {
            out.hurtboxes.push_back(nativeHurtboxFromImported(hurtbox));
        }
    }
    if (asset.hasEnvironmentCollision) {
        out.environmentCollisionBones = asset.environmentCollision.bones;
        out.environmentCollisionMultiplier = asset.environmentCollision.multiplier;
        out.authoredEcb.enabled = true;
    }
    int generatedFallbackClipCount = 0;
    for (FighterState& state : out.states) {
        int actionIndex = state.animationActionIndex;
        if (actionIndex < 0) {
            actionIndex = fallbackActionIndex(state.animation);
            if (actionIndex >= 0 && findClipByActionIndex(asset, actionIndex)) {
                state.animationActionIndex = actionIndex;
            }
        }
        const bool hasClip = actionIndex >= 0 &&
            std::any_of(out.authoredClips.begin(), out.authoredClips.end(), [&](const AnimationClip& clip) {
                return clip.actionIndex == actionIndex;
            });
        const bool hasNamedClip = !state.animation.empty() &&
            std::any_of(out.authoredClips.begin(), out.authoredClips.end(), [&](const AnimationClip& clip) {
                return clip.name == state.animation;
            });
        if ((actionIndex >= 0 && hasClip) || (actionIndex < 0 && (state.animation.empty() || hasNamedClip))) {
            continue;
        }

        AnimationClip clip;
        clip.name = state.animation.empty() ? state.name : state.animation;
        clip.actionIndex = actionIndex;
        clip.frameCount = fx(std::max(1, state.animationLengthFrames));
        clip.defaultBlendFrames = static_cast<int8_t>(std::clamp(state.defaultAnimationBlendFrames, 0, 127));
        clip.generatedFallback = true;
        out.authoredClips.push_back(std::move(clip));
        ++generatedFallbackClipCount;
    }
    if (out.importProvenance.sourceAssetName.empty()) {
        out.importProvenance.sourceAssetName = asset.name;
    }
    if (generatedFallbackClipCount > 0) {
        out.importProvenance.warnings.push_back(
            "generated " + std::to_string(generatedFallbackClipCount) +
            " native fallback animation clips for states without imported clips");
    }
    uniquifyAnimationClipNames(out.authoredClips);
    uniquifyAnimationClipActionIndexes(out.authoredClips);

    out.authoredClipSource.reset();
    out.authoredMeshSource.reset();
    out.modelPartAnimationSource.reset();
    return true;
}


bool makeConvertedMeleeFighterPackage(
    const std::string& fighterName,
    FighterPackage& package,
    std::string* error)
{
    const HsdFighterAssetSpec* spec = meleeTrainingRosterSpecByName(fighterName);
    if (!spec) {
        std::ostringstream message;
        message << "unknown Melee fighter '" << fighterName << "'. Known fighters:";
        for (const std::string& name : meleeTrainingRosterFighterNames()) {
            message << " " << name << ";";
        }
        return failNativeConversion(error, message.str());
    }

    const std::filesystem::path assetPath = findFighterAssetPath(spec->fileName);
    if (assetPath.empty()) {
        return failNativeConversion(error, "missing binary fighter asset: engine_cpp/data/fighters/" + std::string(spec->fileName));
    }

    try {
        const MeleeCommonData common = loadMeleeCommonData();
        HsdFighterAnimationAsset asset = loadHsdFighterAnimationAsset(assetPath.string());
        FighterDefinition imported = makeImportedFighterDefinition(*spec, common, asset);
        FighterDefinition native;
        if (!makeImportedNativePackageFighterDefinition(imported, asset, native, error)) {
            return false;
        }

        package = {};
        package.name = native.name + "_native";
        package.fighters.push_back(std::move(native));
        return validateFighterPackage(package, error);
    } catch (const std::exception& ex) {
        return failNativeConversion(error, ex.what());
    }
}

bool saveConvertedMeleeFighterPackage(
    const std::string& fighterName,
    const std::string& path,
    std::string* error)
{
    FighterPackage package;
    if (!makeConvertedMeleeFighterPackage(fighterName, package, error)) {
        return false;
    }
    return saveFighterPackage(path, package, error);
}


} // namespace pf
