#include "core/simulation.hpp"
#include "core/animation.hpp"
#include "core/animation_asset.hpp"
#include "core/fighter_package.hpp"
#include "core/replay.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

static bool sameVec2(pf::Vec2 a, pf::Vec2 b) {
    return a.x == b.x && a.y == b.y;
}

static pf::HsdFighterMesh makeSmokeAuthoredMesh() {
    pf::HsdFighterMesh mesh;
    pf::HsdMeshBatch batch;
    batch.parentBone = 0;
    batch.singleBindBone = 0;
    auto vertex = [](pf::Vec3 position) {
        pf::HsdMeshVertex out;
        out.position = position;
        out.normal = {0, 0, pf::fx(1)};
        out.influences[0] = {0, 1.0f};
        return out;
    };
    batch.vertices = {
        vertex({pf::fxFromFloat(-0.25f), pf::fxFromFloat(0.25f), 0}),
        vertex({pf::fxFromFloat(0.25f), pf::fxFromFloat(0.25f), 0}),
        vertex({0, pf::fxFromFloat(0.75f), 0}),
    };
    mesh.batches.push_back(std::move(batch));
    return mesh;
}

static bool matchesI32(const std::vector<uint8_t>& bytes, size_t offset, int32_t expected) {
    if (offset + sizeof(expected) > bytes.size()) {
        return false;
    }
    int32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value == expected;
}

static bool corruptFirstSmokeScriptOp(std::vector<uint8_t>& bytes) {
    for (size_t i = 0; i + 25 <= bytes.size(); ++i) {
        if (bytes[i] == static_cast<uint8_t>(pf::PackageScriptOp::AddVarImmediate) &&
            matchesI32(bytes, i + 1, 0) &&
            matchesI32(bytes, i + 5, -1) &&
            matchesI32(bytes, i + 9, -1) &&
            matchesI32(bytes, i + 13, 3) &&
            matchesI32(bytes, i + 17, 0) &&
            matchesI32(bytes, i + 21, 0))
        {
            bytes[i] = 255;
            return true;
        }
    }
    return false;
}

static bool sameFighterRuntimeCore(const pf::World& aWorld, const pf::World& bWorld, size_t fighterIndex) {
    const pf::FighterRuntime& a = aWorld.fighters[fighterIndex];
    const pf::FighterRuntime& b = bWorld.fighters[fighterIndex];
    return pf::currentState(aWorld, a).name == pf::currentState(bWorld, b).name &&
        a.internalFrame == b.internalFrame &&
        a.interruptibleFrame == b.interruptibleFrame &&
        a.animationFrame == b.animationFrame &&
        a.animationRate == b.animationRate &&
        a.lastActionFrameExecuted == b.lastActionFrameExecuted &&
        a.facing == b.facing &&
        a.jumpsUsed == b.jumpsUsed &&
        a.grounded == b.grounded &&
        a.percent == b.percent &&
        a.hitlag == b.hitlag &&
        a.hitstun == b.hitstun &&
        sameVec2(a.position, b.position) &&
        sameVec2(a.fighterVelocity, b.fighterVelocity) &&
        sameVec2(a.knockbackVelocity, b.knockbackVelocity) &&
        a.activeHitboxes.size() == b.activeHitboxes.size();
}

static bool sameWorldGameplayCore(const pf::World& a, const pf::World& b) {
    if (a.frame != b.frame || a.fighters.size() != b.fighters.size() || a.objects.size() != b.objects.size()) {
        return false;
    }
    for (size_t i = 0; i < a.fighters.size(); ++i) {
        if (!sameFighterRuntimeCore(a, b, i)) {
            return false;
        }
    }
    return true;
}

static pf::FighterRuntime& setFighterOnLeftLedge(pf::World& world, size_t fighterIndex) {
    pf::FighterRuntime& fighter = world.fighters[fighterIndex];
    const pf::StageLedge& ledge = world.stage.ledges[0];
    const pf::FighterProperties& attr = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)].properties;
    fighter.grabbedLedge = 0;
    fighter.grounded = false;
    fighter.groundSegment = -1;
    fighter.facing = -ledge.direction;
    fighter.position.x = ledge.position.x + ledge.direction * attr.ledgeHangX;
    fighter.position.y = ledge.position.y + attr.ledgeHangY;
    fighter.previousPosition = fighter.position;
    fighter.fighterVelocity = {};
    fighter.knockbackVelocity = {};
    fighter.attackerShieldKnockback = {};
    fighter.groundAttackerShieldKnockbackVelocity = 0;
    pf::changeFighterState(world, fighter, "CliffWait");
    return fighter;
}

static pf::FighterRuntime& setOnLeftLedge(pf::World& world) {
    return setFighterOnLeftLedge(world, 0);
}

static void printReplayFrame(const pf::World& world, int replayFrame) {
    const pf::FighterRuntime& p1 = world.fighters[0];
    const pf::FighterRuntime& p2 = world.fighters[1];
    std::cout << "replay_frame=" << replayFrame
              << " world_frame=" << world.frame
              << " p1_state=" << pf::currentState(world, p1).name
              << " p1_fis=" << pf::frameInState(p1)
              << " p1_pos=" << pf::toString(p1.position)
              << " p1_vel=" << pf::toString(p1.fighterVelocity)
              << " p1_ground_vel=" << pf::fxToFloat(p1.groundVelocity)
              << " p1_facing=" << p1.facing
              << " p2_state=" << pf::currentState(world, p2).name
              << " p2_pos=" << pf::toString(p2.position)
              << "\n";
}

static int runReplay(const std::string& path, int maxFrames) {
    pf::ReplayData replay;
    std::string error;
    if (!pf::loadReplay(path, replay, &error)) {
        std::cerr << error << "\n";
        return 1;
    }

    pf::World world = pf::makeTrainingWorld(replay.p1FighterDef, replay.p2FighterDef);
    const int frameCount = maxFrames >= 0
        ? std::min(maxFrames, static_cast<int>(replay.frames.size()))
        : static_cast<int>(replay.frames.size());
    std::cout << "loaded_replay=" << path
              << " p1=" << replay.p1FighterDef
              << " p2=" << replay.p2FighterDef
              << " frames=" << replay.frames.size()
              << "\n";
    printReplayFrame(world, 0);
    for (int frame = 0; frame < frameCount; ++frame) {
        const pf::ReplayFrame& replayFrame = replay.frames[static_cast<size_t>(frame)];
        pf::tickWorld(world, {replayFrame.inputs[0], replayFrame.inputs[1]});
        printReplayFrame(world, frame + 1);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--replay") {
        const int maxFrames = argc >= 4 ? std::stoi(argv[3]) : -1;
        return runReplay(argv[2], maxFrames);
    }

    pf::World world = pf::makeTrainingWorld();
    world.fighters[0].position = {-pf::fx(1), 0};
    world.fighters[1].position = {pf::fx(1), 0};
    const auto rosterHas = [&](const std::string& name) {
        return std::any_of(world.fighterDefs.begin(), world.fighterDefs.end(), [&](const pf::FighterDefinition& def) {
            return def.name == name;
        });
    };
    std::cout << "fighter_roster_count=" << world.fighterDefs.size()
              << " roster_has_fox=" << rosterHas("Fox")
              << " roster_has_falco=" << rosterHas("Falco")
              << " roster_has_ice_climbers=" << rosterHas("Ice Climbers")
              << " roster_has_mewtwo=" << rosterHas("Mewtwo")
              << " roster_has_jigglypuff=" << rosterHas("Jigglypuff")
              << "\n";
    size_t rosterHsdReady = 0;
    size_t rosterAttributesReady = 0;
    size_t rosterActionScriptsReady = 0;
    for (const pf::FighterDefinition& def : world.fighterDefs) {
        if (def.hasHsdAsset && def.hsdAsset &&
            !def.hsdAsset->skeleton.empty() &&
            !def.hsdAsset->clips.empty() &&
            !def.hsdAsset->hurtboxes.empty())
        {
            ++rosterHsdReady;
        }
        if (def.hasHsdAsset && def.hsdAsset && def.hsdAsset->hasAttributes) {
            ++rosterAttributesReady;
        }
        if (def.hasHsdAsset && def.hsdAsset && !def.hsdAsset->actionScripts.empty()) {
            ++rosterActionScriptsReady;
        }
    }
    std::cout << "fighter_roster_assets_ready=" << rosterHsdReady
              << " fighter_roster_attrs_ready=" << rosterAttributesReady
              << " fighter_roster_scripts_ready=" << rosterActionScriptsReady
              << "\n";

    pf::WorldSnapshot rewindPoint;

    for (int frame = 0; frame < 60; ++frame) {
        std::vector<pf::InputFrame> inputs(2);
        if (frame == 5) {
            inputs[0].buttons |= pf::ButtonAttack;
        }
        if (frame == 20) {
            rewindPoint = pf::saveWorld(world);
        }
        pf::tickWorld(world, inputs);
    }

    const pf::FighterRuntime& p1 = world.fighters[0];
    const pf::FighterRuntime& p2 = world.fighters[1];
    std::cout << "frame=" << world.frame
              << " p1=" << pf::toString(p1.position)
              << " p2=" << pf::toString(p2.position)
              << " p2_percent=" << pf::fxToFloat(p2.percent)
              << "\n";
    std::cout << "runtime_hsd_pose_joints=" << p1.hsdPose.joints.size()
              << " runtime_hsd_world_joints=" << p1.hsdJointWorldPositions.size()
              << " runtime_hsd_hurtboxes=" << p1.hsdHurtboxCapsules.size()
              << "\n";

    pf::loadWorld(world, rewindPoint);
    std::cout << "rewound_to_frame=" << world.frame
              << " p1=" << pf::toString(world.fighters[0].position)
              << "\n";

    pf::World jumpLandWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& jumpLandTester = jumpLandWorld.fighters[0];
    pf::Vec2 jumpStart = jumpLandTester.position;
    pf::InputFrame jumpLandInput;
    jumpLandInput.buttons |= pf::ButtonJump;
    jumpLandInput.move.y = pf::fx(1);
    pf::tickWorld(jumpLandWorld, {jumpLandInput, pf::InputFrame{}});
    int framesAfterLanding = 0;
    for (int frame = 0; frame < 180; ++frame) {
        pf::tickWorld(jumpLandWorld, {pf::InputFrame{}, pf::InputFrame{}});
        if (jumpLandTester.grounded && pf::currentState(jumpLandWorld, jumpLandTester).name != "JumpSquat" &&
            pf::currentState(jumpLandWorld, jumpLandTester).name != "JumpF")
        {
            ++framesAfterLanding;
        }
        if (framesAfterLanding >= 20) {
            break;
        }
    }
    std::cout << "jump_land_state=" << pf::currentState(jumpLandWorld, jumpLandTester).name
              << " start=" << pf::toString(jumpStart)
              << " pos=" << pf::toString(jumpLandTester.position)
              << " grounded=" << jumpLandTester.grounded
              << " ground_segment=" << jumpLandTester.groundSegment
              << " ledge=" << jumpLandTester.grabbedLedge
              << "\n";

    pf::World ledgeWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& ledgeTester = ledgeWorld.fighters[0];
    ledgeTester.position = {-pf::fx(9), -pf::fxFromFloat(0.2f)};
    ledgeTester.previousPosition = ledgeTester.position;
    ledgeTester.fighterVelocity = {pf::fxFromFloat(0.45f), -pf::fxFromFloat(0.1f)};
    ledgeTester.grounded = false;
    ledgeTester.groundSegment = -1;
    pf::changeFighterState(ledgeWorld, ledgeTester, "Fall");
    pf::tickWorld(ledgeWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "ledge_state=" << pf::currentState(ledgeWorld, ledgeTester).name
              << " ledge=" << ledgeTester.grabbedLedge
              << " pos=" << pf::toString(ledgeTester.position)
              << "\n";

    pf::World occupiedLedgeWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& occupiedHolder = setOnLeftLedge(occupiedLedgeWorld);
    const pf::StageLedge& occupiedLedge = occupiedLedgeWorld.stage.ledges[static_cast<size_t>(occupiedHolder.grabbedLedge)];
    pf::FighterRuntime& occupiedTester = occupiedLedgeWorld.fighters[1];
    const pf::FighterProperties& occupiedAttr = occupiedLedgeWorld.fighterDefs[static_cast<size_t>(occupiedTester.fighterDef)].properties;
    occupiedTester.grounded = false;
    occupiedTester.groundSegment = -1;
    occupiedTester.facing = -occupiedLedge.direction;
    occupiedTester.position.x = occupiedLedge.position.x + occupiedLedge.direction * occupiedAttr.ledgeHangX;
    occupiedTester.position.y = occupiedLedge.position.y + occupiedAttr.ledgeHangY + pf::fxFromFloat(0.2f);
    occupiedTester.previousPosition = {occupiedTester.position.x, occupiedTester.position.y + pf::fxFromFloat(0.4f)};
    occupiedTester.fighterVelocity = {0, -pf::fxFromFloat(0.3f)};
    pf::changeFighterState(occupiedLedgeWorld, occupiedTester, "Fall");
    pf::tickWorld(occupiedLedgeWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "occupied_ledge_holder=" << occupiedHolder.grabbedLedge
              << " occupied_ledge_challenger=" << occupiedTester.grabbedLedge
              << " challenger_state=" << pf::currentState(occupiedLedgeWorld, occupiedTester).name
              << "\n";

    pf::World slopeWorld = pf::makeTrainingWorld();
    slopeWorld.stage.segments = {
        {{-pf::fx(4), 0}, {pf::fx(4), pf::fx(2)}, pf::fx(1), pf::SegmentType::Solid, true, true},
    };
    slopeWorld.stage.ledges = {
        {slopeWorld.stage.segments[0].start, -1, 0},
        {slopeWorld.stage.segments[0].end, 1, 0},
    };
    pf::FighterRuntime& slopeTester = slopeWorld.fighters[0];
    slopeTester.position = {-pf::fx(2), pf::fxFromFloat(0.5f)};
    slopeTester.previousPosition = slopeTester.position;
    slopeTester.grounded = true;
    slopeTester.groundSegment = 0;
    slopeTester.groundVelocity = pf::fx(1);
    pf::tickWorld(slopeWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "slope_state=" << pf::currentState(slopeWorld, slopeTester).name
              << " slope_pos=" << pf::toString(slopeTester.position)
              << " vel=" << pf::toString(slopeTester.fighterVelocity)
              << " ground_vel=" << pf::fxToFloat(slopeTester.groundVelocity)
              << "\n";

    pf::World slopeJumpWorld = pf::makeTrainingWorld();
    slopeJumpWorld.stage.segments = {
        {{-pf::fx(4), 0}, {pf::fx(4), pf::fx(2)}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    slopeJumpWorld.stage.ledges.clear();
    pf::FighterRuntime& slopeJumpTester = slopeJumpWorld.fighters[0];
    slopeJumpTester.position = {-pf::fx(2), pf::fxFromFloat(0.5f)};
    slopeJumpTester.previousPosition = slopeJumpTester.position;
    slopeJumpTester.grounded = true;
    slopeJumpTester.groundSegment = 0;
    slopeJumpTester.groundNormal = { -pf::fxFromFloat(0.243f), pf::fxFromFloat(0.970f) };
    slopeJumpTester.groundVelocity = pf::fx(1);
    pf::InputFrame slopeJumpInput;
    slopeJumpInput.buttons |= pf::ButtonJump;
    for (int frame = 0; frame < 6; ++frame) {
        pf::tickWorld(slopeJumpWorld, {slopeJumpInput, pf::InputFrame{}});
    }
    std::cout << "slope_jump_state=" << pf::currentState(slopeJumpWorld, slopeJumpTester).name
              << " vel=" << pf::toString(slopeJumpTester.fighterVelocity)
              << "\n";

    pf::World walkInputWorld = pf::makeTrainingWorld();
    pf::InputFrame lightWalkInput;
    lightWalkInput.move.x = pf::fxFromFloat(0.25f);
    pf::tickWorld(walkInputWorld, {lightWalkInput, pf::InputFrame{}});
    std::cout << "wait_light_walk_state=" << pf::currentState(walkInputWorld, walkInputWorld.fighters[0]).name
              << "\n";

    pf::World walkTypeWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& walkTypeTester = walkTypeWorld.fighters[0];
    pf::changeFighterState(walkTypeWorld, walkTypeTester, "WalkFast");
    walkTypeTester.groundVelocity = 0;
    pf::tickWorld(walkTypeWorld, {lightWalkInput, pf::InputFrame{}});
    std::cout << "walk_fast_to_light_state=" << pf::currentState(walkTypeWorld, walkTypeTester).name
              << "\n";

    pf::World dashWorld = pf::makeTrainingWorld();
    dashWorld.stage.segments = {
        {{-pf::fx(100), 0}, {pf::fx(100), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    dashWorld.stage.ledges.clear();
    dashWorld.fighters[0].position = {0, 0};
    dashWorld.fighters[0].previousPosition = dashWorld.fighters[0].position;
    dashWorld.fighters[0].grounded = true;
    dashWorld.fighters[0].groundSegment = 0;
    for (int frame = 0; frame < 60; ++frame) {
        pf::InputFrame input;
        input.move.x = pf::fx(1);
        pf::tickWorld(dashWorld, {input, pf::InputFrame{}});
    }
    const pf::FighterRuntime& dashTester = dashWorld.fighters[0];
    std::cout << "dash_state=" << pf::currentState(dashWorld, dashTester).name
              << " ground_vel=" << pf::fxToFloat(dashTester.groundVelocity)
              << "\n";

    pf::World dashFirstFrameWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& dashFirstFrameTester = dashFirstFrameWorld.fighters[0];
    pf::changeFighterState(dashFirstFrameWorld, dashFirstFrameTester, "Dash");
    pf::InputFrame dashFirstFrameInput;
    dashFirstFrameInput.move.x = pf::fx(1);
    pf::tickWorld(dashFirstFrameWorld, {dashFirstFrameInput, pf::InputFrame{}});
    std::cout << "dash_first_frame_vel=" << pf::fxToFloat(dashFirstFrameTester.groundVelocity)
              << " dash_initial_vel=" << pf::fxToFloat(dashFirstFrameWorld.fighterDefs[0].properties.dashInitialVelocity)
              << "\n";

    pf::InputFrame dashNeutralInput;
    pf::tickWorld(dashFirstFrameWorld, {dashNeutralInput, pf::InputFrame{}});
    std::cout << "dash_neutral_decay_vel=" << pf::fxToFloat(dashFirstFrameTester.groundVelocity)
              << "\n";

    pf::World dashShieldWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& dashShieldTester = dashShieldWorld.fighters[0];
    pf::changeFighterState(dashShieldWorld, dashShieldTester, "Dash");
    pf::InputFrame dashShieldInput;
    dashShieldInput.buttons = pf::ButtonShield;
    pf::tickWorld(dashShieldWorld, {dashShieldInput, pf::InputFrame{}});
    const std::string earlyDashShieldState = pf::currentState(dashShieldWorld, dashShieldTester).name;

    pf::World dashShieldJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& dashShieldJumpTester = dashShieldJumpWorld.fighters[0];
    pf::changeFighterState(dashShieldJumpWorld, dashShieldJumpTester, "Dash");
    pf::InputFrame dashShieldJumpInput;
    dashShieldJumpInput.buttons = pf::ButtonShield | pf::ButtonJump;
    pf::tickWorld(dashShieldJumpWorld, {dashShieldJumpInput, pf::InputFrame{}});

    pf::World lateDashShieldWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& lateDashShieldTester = lateDashShieldWorld.fighters[0];
    pf::changeFighterState(lateDashShieldWorld, lateDashShieldTester, "Dash");
    for (int frame = 0; frame < lateDashShieldWorld.fighterDefs[0].properties.common.dashItemThrowWindowX48 + 1; ++frame) {
        pf::tickWorld(lateDashShieldWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    pf::tickWorld(lateDashShieldWorld, {dashShieldInput, pf::InputFrame{}});
    std::cout << "dash_shield_early_state=" << earlyDashShieldState
              << " dash_shield_late_state=" << pf::currentState(lateDashShieldWorld, lateDashShieldTester).name
              << " dash_shield_jump_state=" << pf::currentState(dashShieldJumpWorld, dashShieldJumpTester).name
              << "\n";

    pf::World escapeBackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& escapeBackTester = escapeBackWorld.fighters[0];
    escapeBackTester.facing = 1;
    pf::changeFighterState(escapeBackWorld, escapeBackTester, "EscapeB");
    pf::setFighterThrowFlag(escapeBackTester, 3, true);
    pf::tickWorld(escapeBackWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "escape_flag_flip_facing=" << escapeBackTester.facing
              << " flag_consumed=" << pf::fighterThrowFlag(escapeBackTester, 3)
              << "\n";

    pf::World escapeRollFrictionWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& escapeRollFrictionTester = escapeRollFrictionWorld.fighters[0];
    pf::changeFighterState(escapeRollFrictionWorld, escapeRollFrictionTester, "EscapeF");
    const pf::Fix escapeRollStartVelocity =
        escapeRollFrictionWorld.fighterDefs[0].properties.walkMaxVel + pf::fxFromFloat(0.5f);
    escapeRollFrictionTester.groundVelocity = escapeRollStartVelocity;
    pf::tickWorld(escapeRollFrictionWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "escape_roll_friction_delta="
              << pf::fxToFloat(escapeRollStartVelocity - escapeRollFrictionTester.groundVelocity)
              << " escape_roll_expected_delta="
              << pf::fxToFloat(escapeRollFrictionWorld.fighterDefs[0].properties.grFriction)
              << "\n";

    pf::World falconDashWorld = pf::makeTrainingWorld(3, 3);
    for (int frame = 0; frame < 32; ++frame) {
        pf::InputFrame input;
        input.move.x = pf::fx(1);
        pf::tickWorld(falconDashWorld, {input, pf::InputFrame{}});
    }
    const pf::FighterRuntime& falconDashTester = falconDashWorld.fighters[0];
    std::cout << "falcon_dash_state=" << pf::currentState(falconDashWorld, falconDashTester).name
              << " ground_vel=" << pf::fxToFloat(falconDashTester.groundVelocity)
              << " pos=" << pf::toString(falconDashTester.position)
              << "\n";

    pf::World falconDashDanceWorld = pf::makeTrainingWorld(3, 3);
    falconDashDanceWorld.stage.segments = {
        {{-pf::fx(100), 0}, {pf::fx(100), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    falconDashDanceWorld.stage.ledges.clear();
    falconDashDanceWorld.fighters[0].position = {0, 0};
    falconDashDanceWorld.fighters[0].previousPosition = falconDashDanceWorld.fighters[0].position;
    falconDashDanceWorld.fighters[0].facing = 1;
    falconDashDanceWorld.fighters[0].grounded = true;
    falconDashDanceWorld.fighters[0].groundSegment = 0;
    for (int frame = 0; frame < 4; ++frame) {
        pf::InputFrame input;
        input.move.x = pf::fx(1);
        pf::tickWorld(falconDashDanceWorld, {input, pf::InputFrame{}});
    }
    for (int frame = 0; frame < 6; ++frame) {
        pf::InputFrame input;
        input.move.x = -pf::fx(1);
        pf::tickWorld(falconDashDanceWorld, {input, pf::InputFrame{}});
    }
    const pf::FighterRuntime& falconDashDanceTester = falconDashDanceWorld.fighters[0];
    std::cout << "falcon_dash_dance_state=" << pf::currentState(falconDashDanceWorld, falconDashDanceTester).name
              << " facing=" << falconDashDanceTester.facing
              << " ground_vel=" << pf::fxToFloat(falconDashDanceTester.groundVelocity)
              << " pos=" << pf::toString(falconDashDanceTester.position)
              << "\n";

    pf::World lowFrictionDashWorld = pf::makeTrainingWorld();
    lowFrictionDashWorld.stage.segments = {
        {{-pf::fx(100), 0}, {pf::fx(100), 0}, pf::fxFromFloat(0.5f), pf::SegmentType::Solid, false, false},
    };
    lowFrictionDashWorld.stage.ledges.clear();
    pf::FighterRuntime& lowFrictionDashMutableTester = lowFrictionDashWorld.fighters[0];
    lowFrictionDashMutableTester.position.y = 0;
    lowFrictionDashMutableTester.previousPosition = lowFrictionDashMutableTester.position;
    lowFrictionDashMutableTester.grounded = true;
    lowFrictionDashMutableTester.groundSegment = 0;
    pf::InputFrame lowFrictionDashInput;
    lowFrictionDashInput.move.x = pf::fx(1);
    pf::tickWorld(lowFrictionDashWorld, {lowFrictionDashInput, pf::InputFrame{}});
    const pf::FighterRuntime& lowFrictionDashTester = lowFrictionDashWorld.fighters[0];
    std::cout << "low_friction_dash_state=" << pf::currentState(lowFrictionDashWorld, lowFrictionDashTester).name
              << " ground_vel=" << pf::fxToFloat(lowFrictionDashTester.groundVelocity)
              << "\n";

    pf::World reverseDashWorld = pf::makeTrainingWorld();
    reverseDashWorld.stage.segments = {
        {{-pf::fx(100), 0}, {pf::fx(100), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    reverseDashWorld.stage.ledges.clear();
    reverseDashWorld.fighters[0].position = {0, 0};
    reverseDashWorld.fighters[0].previousPosition = reverseDashWorld.fighters[0].position;
    reverseDashWorld.fighters[0].facing = 1;
    reverseDashWorld.fighters[0].grounded = true;
    reverseDashWorld.fighters[0].groundSegment = 0;
    pf::InputFrame rightDash;
    rightDash.move.x = pf::fx(1);
    pf::tickWorld(reverseDashWorld, {rightDash, pf::InputFrame{}});
    pf::InputFrame leftDash;
    leftDash.move.x = -pf::fx(1);
    pf::tickWorld(reverseDashWorld, {leftDash, pf::InputFrame{}});
    const pf::FighterRuntime& reverseDashTester = reverseDashWorld.fighters[0];
    std::cout << "reverse_dash_state=" << pf::currentState(reverseDashWorld, reverseDashTester).name
              << " facing=" << reverseDashTester.facing
              << " ground_vel=" << pf::fxToFloat(reverseDashTester.groundVelocity)
              << "\n";

    pf::World smashTurnDashWorld = pf::makeTrainingWorld();
    smashTurnDashWorld.stage.segments = {
        {{-pf::fx(100), 0}, {pf::fx(100), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    smashTurnDashWorld.stage.ledges.clear();
    smashTurnDashWorld.fighters[0].position = {-pf::fx(2), 0};
    smashTurnDashWorld.fighters[0].previousPosition = smashTurnDashWorld.fighters[0].position;
    smashTurnDashWorld.fighters[0].grounded = true;
    smashTurnDashWorld.fighters[0].groundSegment = 0;
    pf::InputFrame smashTurnDashInput;
    smashTurnDashInput.move.x = -pf::fx(1);
    for (int frame = 0; frame < 3 && pf::currentState(smashTurnDashWorld, smashTurnDashWorld.fighters[0]).name != "Dash"; ++frame) {
        pf::tickWorld(smashTurnDashWorld, {smashTurnDashInput, pf::InputFrame{}});
    }
    const pf::FighterRuntime& smashTurnDashTester = smashTurnDashWorld.fighters[0];
    std::cout << "smash_turn_dash_state=" << pf::currentState(smashTurnDashWorld, smashTurnDashTester).name
              << " facing=" << smashTurnDashTester.facing
              << " ground_vel=" << pf::fxToFloat(smashTurnDashTester.groundVelocity)
              << "\n";

    pf::World bufferedTurnDashWorld = pf::makeTrainingWorld();
    bufferedTurnDashWorld.stage.segments = {
        {{-pf::fx(100), 0}, {pf::fx(100), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    bufferedTurnDashWorld.stage.ledges.clear();
    pf::InputFrame turnInput;
    turnInput.move.x = -pf::fxFromFloat(0.3f);
    pf::tickWorld(bufferedTurnDashWorld, {turnInput, pf::InputFrame{}});
    pf::InputFrame bufferedDashInput;
    bufferedDashInput.move.x = -pf::fx(1);
    for (int frame = 0; frame < 6 && pf::currentState(bufferedTurnDashWorld, bufferedTurnDashWorld.fighters[0]).name != "Dash"; ++frame) {
        pf::tickWorld(bufferedTurnDashWorld, {bufferedDashInput, pf::InputFrame{}});
    }
    const pf::FighterRuntime& bufferedTurnDashTester = bufferedTurnDashWorld.fighters[0];
    std::cout << "buffered_turn_dash_state=" << pf::currentState(bufferedTurnDashWorld, bufferedTurnDashTester).name
              << " facing=" << bufferedTurnDashTester.facing
              << " ground_vel=" << pf::fxToFloat(bufferedTurnDashTester.groundVelocity)
              << "\n";

    pf::World runBrakeWorld = pf::makeTrainingWorld();
    runBrakeWorld.stage.segments = {
        {{-pf::fx(100), 0}, {pf::fx(100), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    runBrakeWorld.stage.ledges.clear();
    for (int frame = 0; frame < 45; ++frame) {
        pf::InputFrame input;
        input.move.x = pf::fx(1);
        pf::tickWorld(runBrakeWorld, {input, pf::InputFrame{}});
    }
    pf::tickWorld(runBrakeWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const pf::FighterRuntime& runBrakeTester = runBrakeWorld.fighters[0];
    std::cout << "run_brake_state=" << pf::currentState(runBrakeWorld, runBrakeTester).name
              << " ground_vel=" << pf::fxToFloat(runBrakeTester.groundVelocity)
              << "\n";

    pf::World runBrakeSquatWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& runBrakeSquatTester = runBrakeSquatWorld.fighters[0];
    pf::changeFighterState(runBrakeSquatWorld, runBrakeSquatTester, "RunBrake");
    pf::InputFrame runBrakeSquatInput;
    runBrakeSquatInput.move.y = -pf::fx(1);
    pf::tickWorld(runBrakeSquatWorld, {runBrakeSquatInput, pf::InputFrame{}});
    std::cout << "run_brake_squat_state=" << pf::currentState(runBrakeSquatWorld, runBrakeSquatTester).name
              << "\n";

    pf::World runBrakeCmdTurnWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& runBrakeCmdTurnTester = runBrakeCmdTurnWorld.fighters[0];
    pf::changeFighterState(runBrakeCmdTurnWorld, runBrakeCmdTurnTester, "RunBrake");
    pf::setFighterCommandFlag(runBrakeCmdTurnTester, 0, true);
    pf::InputFrame runBrakeCmdTurnInput;
    runBrakeCmdTurnInput.move.x = -pf::fx(1);
    runBrakeCmdTurnInput.move.y = -pf::fx(1);
    pf::tickWorld(runBrakeCmdTurnWorld, {runBrakeCmdTurnInput, pf::InputFrame{}});
    std::cout << "run_brake_cmd_turn_state=" << pf::currentState(runBrakeCmdTurnWorld, runBrakeCmdTurnTester).name
              << "\n";

    pf::World runAttackJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& runAttackJumpTester = runAttackJumpWorld.fighters[0];
    pf::changeFighterState(runAttackJumpWorld, runAttackJumpTester, "Run");
    pf::InputFrame runAttackJumpInput;
    runAttackJumpInput.buttons = pf::ButtonAttack | pf::ButtonJump;
    pf::tickWorld(runAttackJumpWorld, {runAttackJumpInput, pf::InputFrame{}});

    pf::World runShieldJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& runShieldJumpTester = runShieldJumpWorld.fighters[0];
    pf::changeFighterState(runShieldJumpWorld, runShieldJumpTester, "Run");
    pf::InputFrame runShieldJumpInput;
    runShieldJumpInput.buttons = pf::ButtonShield | pf::ButtonJump;
    pf::tickWorld(runShieldJumpWorld, {runShieldJumpInput, pf::InputFrame{}});
    std::cout << "run_attack_jump_state=" << pf::currentState(runAttackJumpWorld, runAttackJumpTester).name
              << " run_shield_jump_state=" << pf::currentState(runShieldJumpWorld, runShieldJumpTester).name
              << "\n";

    pf::World attackDashGrabWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& attackDashGrabTester = attackDashGrabWorld.fighters[0];
    pf::changeFighterState(attackDashGrabWorld, attackDashGrabTester, "Dash");
    pf::InputFrame dashAttackInput;
    dashAttackInput.buttons = pf::ButtonAttack;
    pf::tickWorld(attackDashGrabWorld, {dashAttackInput, pf::InputFrame{}});
    const int attackDashGrabBufferStart = attackDashGrabTester.attackDashGrabBufferTimer;
    pf::InputFrame attackDashShieldInput;
    attackDashShieldInput.buttons = pf::ButtonShield;
    pf::tickWorld(attackDashGrabWorld, {attackDashShieldInput, pf::InputFrame{}});

    pf::World attackDashGrabExpiredWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& attackDashGrabExpiredTester = attackDashGrabExpiredWorld.fighters[0];
    pf::changeFighterState(attackDashGrabExpiredWorld, attackDashGrabExpiredTester, "Dash");
    pf::tickWorld(attackDashGrabExpiredWorld, {dashAttackInput, pf::InputFrame{}});
    const int attackDashGrabBufferFrames =
        attackDashGrabExpiredWorld.fighterDefs[0].properties.common.attackDashGrabBufferFramesX68;
    for (int frame = 0; frame < attackDashGrabBufferFrames + 1; ++frame) {
        pf::tickWorld(attackDashGrabExpiredWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    pf::tickWorld(attackDashGrabExpiredWorld, {attackDashShieldInput, pf::InputFrame{}});
    std::cout << "attack_dash_grab_buffer_start=" << attackDashGrabBufferStart
              << " attack_dash_grab_state=" << pf::currentState(attackDashGrabWorld, attackDashGrabTester).name
              << " attack_dash_grab_expired_state=" << pf::currentState(attackDashGrabExpiredWorld, attackDashGrabExpiredTester).name
              << "\n";

    auto attackDashIasaStateFor = [](pf::InputFrame input) {
        pf::World world = pf::makeTrainingWorld();
        pf::FighterRuntime& fighter = world.fighters[0];
        pf::changeFighterState(world, fighter, "AttackDash");
        fighter.attackDashGrabBufferTimer = 0;
        fighter.interruptibleFrame = 0;
        pf::tickWorld(world, {input, pf::InputFrame{}});
        return pf::currentState(world, fighter).name;
    };
    pf::InputFrame attackDashIasaGrabInput;
    attackDashIasaGrabInput.buttons = pf::ButtonGrab;
    pf::InputFrame attackDashIasaJumpInput;
    attackDashIasaJumpInput.buttons = pf::ButtonJump;
    pf::InputFrame attackDashIasaDashInput;
    attackDashIasaDashInput.move.x = pf::fx(1);
    pf::InputFrame attackDashIasaSquatInput;
    attackDashIasaSquatInput.move.y = -pf::fx(1);
    pf::InputFrame attackDashIasaTauntInput;
    attackDashIasaTauntInput.buttons = pf::ButtonTaunt;
    std::cout << "attack_dash_iasa_grab_state=" << attackDashIasaStateFor(attackDashIasaGrabInput)
              << " attack_dash_iasa_jump_state=" << attackDashIasaStateFor(attackDashIasaJumpInput)
              << " attack_dash_iasa_dash_state=" << attackDashIasaStateFor(attackDashIasaDashInput)
              << " attack_dash_iasa_squat_state=" << attackDashIasaStateFor(attackDashIasaSquatInput)
              << " attack_dash_iasa_taunt_state=" << attackDashIasaStateFor(attackDashIasaTauntInput)
              << "\n";

    pf::World jabFollowupWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& jabFollowupTester = jabFollowupWorld.fighters[0];
    pf::changeFighterState(jabFollowupWorld, jabFollowupTester, "Attack11");
    jabFollowupTester.interruptibleFrame = 1000000;
    jabFollowupTester.jabFollowupEnabled = true;
    pf::InputFrame jabFollowupInput;
    jabFollowupInput.buttons = pf::ButtonAttack;
    pf::tickWorld(jabFollowupWorld, {jabFollowupInput, pf::InputFrame{}});

    auto jabIasaStateFor = [](const std::string& stateName, pf::InputFrame input, int interruptibleFrame) {
        pf::World world = pf::makeTrainingWorld();
        pf::FighterRuntime& fighter = world.fighters[0];
        pf::changeFighterState(world, fighter, stateName);
        fighter.interruptibleFrame = interruptibleFrame;
        pf::tickWorld(world, {input, pf::InputFrame{}});
        return pf::currentState(world, fighter).name;
    };
    pf::InputFrame jabIasaJumpInput;
    jabIasaJumpInput.buttons = pf::ButtonJump;
    pf::InputFrame jabIasaGrabInput;
    jabIasaGrabInput.buttons = pf::ButtonGrab;
    std::cout << "jab_followup_pre_iasa_state=" << pf::currentState(jabFollowupWorld, jabFollowupTester).name
              << " jab_pre_iasa_jump_state=" << jabIasaStateFor("Attack11", jabIasaJumpInput, 1000000)
              << " jab_iasa_jump_state=" << jabIasaStateFor("Attack11", jabIasaJumpInput, 0)
              << " jab13_iasa_grab_state=" << jabIasaStateFor("Attack13", jabIasaGrabInput, 0)
              << "\n";

    auto rapidJabStateFor = [](int fighterDef) {
        pf::World world = pf::makeTrainingWorld(fighterDef, fighterDef);
        pf::FighterRuntime& fighter = world.fighters[0];
        pf::changeFighterState(world, fighter, "Attack12");
        fighter.interruptibleFrame = 1000000;
        fighter.rapidJabEnabled = true;
        fighter.attackRapidInputCount = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)].properties.rapidJabWindow - 1;
        pf::InputFrame input;
        input.buttons = pf::ButtonAttack;
        pf::tickWorld(world, {input, pf::InputFrame{}});
        return pf::currentState(world, fighter).name;
    };
    auto attack100LoopStateFor = [](bool continuePressed) {
        pf::World world = pf::makeTrainingWorld(2, 2);
        pf::FighterRuntime& fighter = world.fighters[0];
        pf::changeFighterState(world, fighter, "Attack100Loop");
        fighter.attack100CanEnd = true;
        pf::setFighterThrowFlag(fighter, 3, true);
        pf::InputFrame input;
        if (continuePressed) {
            input.buttons = pf::ButtonAttack;
        }
        pf::tickWorld(world, {input, pf::InputFrame{}});
        return pf::currentState(world, fighter).name;
    };
    pf::World rapidJabFlagWorld = pf::makeTrainingWorld(2, 2);
    pf::FighterRuntime& rapidJabFlagTester = rapidJabFlagWorld.fighters[0];
    pf::changeFighterState(rapidJabFlagWorld, rapidJabFlagTester, "Attack12");
    bool rapidJabFlagSet = false;
    for (int frame = 0; frame < 20 && !rapidJabFlagSet; ++frame) {
        pf::tickWorld(rapidJabFlagWorld, {pf::InputFrame{}, pf::InputFrame{}});
        rapidJabFlagSet = rapidJabFlagTester.rapidJabEnabled;
    }
    pf::World rapidJabClearWorld = pf::makeTrainingWorld(2, 2);
    pf::FighterRuntime& rapidJabClearTester = rapidJabClearWorld.fighters[0];
    pf::changeFighterState(rapidJabClearWorld, rapidJabClearTester, "Attack13");
    rapidJabClearTester.rapidJabEnabled = true;
    pf::tickWorld(rapidJabClearWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "link_rapid_jab_state=" << rapidJabStateFor(2)
              << " mario_rapid_jab_state=" << rapidJabStateFor(0)
              << " link_rapid_jab_flag_set=" << rapidJabFlagSet
              << " link_rapid_jab_flag_clear=" << rapidJabClearTester.rapidJabEnabled
              << " attack100_loop_release_state=" << attack100LoopStateFor(false)
              << " attack100_loop_continue_state=" << attack100LoopStateFor(true)
              << "\n";

    auto groundedSpecialStateFor = [](pf::InputFrame input) {
        pf::World world = pf::makeTrainingWorld();
        pf::FighterRuntime& fighter = world.fighters[0];
        pf::tickWorld(world, {input, pf::InputFrame{}});
        return pf::currentState(world, fighter).name;
    };
    auto groundedSpecialFacingFor = [](pf::InputFrame input) {
        pf::World world = pf::makeTrainingWorld();
        pf::FighterRuntime& fighter = world.fighters[0];
        fighter.facing = 1;
        pf::tickWorld(world, {input, pf::InputFrame{}});
        return fighter.facing;
    };
    auto airborneSpecialStateFor = [](pf::InputFrame input) {
        pf::World world = pf::makeTrainingWorld();
        pf::FighterRuntime& fighter = world.fighters[0];
        fighter.grounded = false;
        fighter.groundSegment = -1;
        fighter.position.y = pf::fx(5);
        fighter.previousPosition = fighter.position;
        pf::changeFighterState(world, fighter, "Fall");
        pf::tickWorld(world, {input, pf::InputFrame{}});
        return pf::currentState(world, fighter).name;
    };
    pf::InputFrame specialNeutralInput;
    specialNeutralInput.buttons = pf::ButtonSpecial;
    pf::InputFrame specialSideInput;
    specialSideInput.buttons = pf::ButtonSpecial;
    specialSideInput.move.x = pf::fx(1);
    pf::InputFrame specialSideReverseInput = specialSideInput;
    specialSideReverseInput.move.x = -pf::fx(1);
    pf::InputFrame specialUpInput;
    specialUpInput.buttons = pf::ButtonSpecial;
    specialUpInput.move.y = pf::fx(1);
    pf::InputFrame specialDownInput;
    specialDownInput.buttons = pf::ButtonSpecial;
    specialDownInput.move.y = -pf::fx(1);
    std::cout << "ground_special_states=" << groundedSpecialStateFor(specialSideInput)
              << "," << groundedSpecialStateFor(specialUpInput)
              << "," << groundedSpecialStateFor(specialNeutralInput)
              << "," << groundedSpecialStateFor(specialDownInput)
              << " ground_special_side_reverse_facing=" << groundedSpecialFacingFor(specialSideReverseInput)
              << " air_special_states=" << airborneSpecialStateFor(specialUpInput)
              << "," << airborneSpecialStateFor(specialDownInput)
              << "," << airborneSpecialStateFor(specialSideInput)
              << "," << airborneSpecialStateFor(specialNeutralInput)
              << "\n";

    auto groundedAttackIasaStateFor = [](const std::string& stateName, pf::InputFrame input) {
        pf::World world = pf::makeTrainingWorld();
        pf::FighterRuntime& fighter = world.fighters[0];
        pf::changeFighterState(world, fighter, stateName);
        fighter.interruptibleFrame = 0;
        pf::tickWorld(world, {input, pf::InputFrame{}});
        return pf::currentState(world, fighter).name;
    };
    pf::InputFrame groundedAttackIasaSpotInput;
    groundedAttackIasaSpotInput.buttons = pf::ButtonShield;
    groundedAttackIasaSpotInput.move.y = -pf::fx(1);
    pf::InputFrame groundedAttackIasaShieldInput;
    groundedAttackIasaShieldInput.buttons = pf::ButtonShield;
    pf::InputFrame groundedAttackIasaGrabInput;
    groundedAttackIasaGrabInput.buttons = pf::ButtonGrab;
    std::cout << "attack_s3_iasa_spot_state=" << groundedAttackIasaStateFor("AttackS3", groundedAttackIasaSpotInput)
              << " attack_s3_iasa_grab_state=" << groundedAttackIasaStateFor("AttackS3", groundedAttackIasaGrabInput)
              << " attack_s4_iasa_spot_state=" << groundedAttackIasaStateFor("AttackS4", groundedAttackIasaSpotInput)
              << " attack_lw3_iasa_shield_state=" << groundedAttackIasaStateFor("AttackLw3", groundedAttackIasaShieldInput)
              << "\n";

    pf::World attackLw3RepeatNowWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& attackLw3RepeatNowTester = attackLw3RepeatNowWorld.fighters[0];
    pf::changeFighterState(attackLw3RepeatNowWorld, attackLw3RepeatNowTester, "AttackLw3");
    attackLw3RepeatNowTester.interruptibleFrame = 1000000;
    pf::setFighterCommandFlag(attackLw3RepeatNowTester, 0, true);
    pf::InputFrame attackLw3RepeatInput;
    attackLw3RepeatInput.buttons = pf::ButtonAttack;
    pf::tickWorld(attackLw3RepeatNowWorld, {attackLw3RepeatInput, pf::InputFrame{}});

    pf::World attackLw3RepeatBufferedWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& attackLw3RepeatBufferedTester = attackLw3RepeatBufferedWorld.fighters[0];
    pf::changeFighterState(attackLw3RepeatBufferedWorld, attackLw3RepeatBufferedTester, "AttackLw3");
    attackLw3RepeatBufferedTester.interruptibleFrame = 1000000;
    pf::tickWorld(attackLw3RepeatBufferedWorld, {attackLw3RepeatInput, pf::InputFrame{}});
    const bool attackLw3RepeatQueued = attackLw3RepeatBufferedTester.attackLw3RepeatQueued;
    pf::setFighterCommandFlag(attackLw3RepeatBufferedTester, 0, true);
    pf::tickWorld(attackLw3RepeatBufferedWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "attack_lw3_repeat_now_state=" << pf::currentState(attackLw3RepeatNowWorld, attackLw3RepeatNowTester).name
              << " attack_lw3_repeat_now_frame=" << pf::frameInState(attackLw3RepeatNowTester)
              << " attack_lw3_repeat_buffered_queued=" << attackLw3RepeatQueued
              << " attack_lw3_repeat_buffered_frame=" << pf::frameInState(attackLw3RepeatBufferedTester)
              << "\n";

    auto sideSmashSecondStateFor = [](int fighterDef) {
        pf::World world = pf::makeTrainingWorld(fighterDef, fighterDef);
        pf::FighterRuntime& fighter = world.fighters[0];
        pf::changeFighterState(world, fighter, "AttackS4");
        fighter.interruptibleFrame = 1000000;
        pf::setFighterCommandFlag(fighter, 0, true);
        pf::InputFrame input;
        input.buttons = pf::ButtonAttack;
        pf::tickWorld(world, {input, pf::InputFrame{}});
        return pf::currentState(world, fighter).name;
    };
    pf::World attackS42IasaWorld = pf::makeTrainingWorld(2, 2);
    pf::FighterRuntime& attackS42IasaTester = attackS42IasaWorld.fighters[0];
    pf::changeFighterState(attackS42IasaWorld, attackS42IasaTester, "AttackS42");
    attackS42IasaTester.interruptibleFrame = 0;
    pf::InputFrame attackS42JumpInput;
    attackS42JumpInput.buttons = pf::ButtonJump;
    pf::tickWorld(attackS42IasaWorld, {attackS42JumpInput, pf::InputFrame{}});
    std::cout << "link_attack_s42_state=" << sideSmashSecondStateFor(2)
              << " young_link_attack_s42_state=" << sideSmashSecondStateFor(20)
              << " mario_attack_s42_state=" << sideSmashSecondStateFor(0)
              << " attack_s42_iasa_jump_state=" << pf::currentState(attackS42IasaWorld, attackS42IasaTester).name
              << "\n";

    pf::World turnRunWorld = pf::makeTrainingWorld();
    turnRunWorld.stage.segments = {
        {{-pf::fx(100), 0}, {pf::fx(100), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    turnRunWorld.stage.ledges.clear();
    for (int frame = 0; frame < 45; ++frame) {
        pf::InputFrame input;
        input.move.x = pf::fx(1);
        pf::tickWorld(turnRunWorld, {input, pf::InputFrame{}});
    }
    pf::InputFrame reverseRun;
    reverseRun.move.x = -pf::fx(1);
    pf::tickWorld(turnRunWorld, {reverseRun, pf::InputFrame{}});
    for (int frame = 0; frame < 45; ++frame) {
        pf::tickWorld(turnRunWorld, {reverseRun, pf::InputFrame{}});
    }
    const pf::FighterRuntime& turnRunTester = turnRunWorld.fighters[0];
    std::cout << "turn_run_state=" << pf::currentState(turnRunWorld, turnRunTester).name
              << " facing=" << turnRunTester.facing
              << " ground_vel=" << pf::fxToFloat(turnRunTester.groundVelocity)
              << "\n";

    pf::World turnRunWalkReverseWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& turnRunWalkReverseTester = turnRunWalkReverseWorld.fighters[0];
    turnRunWalkReverseTester.facing = 1;
    turnRunWalkReverseTester.grounded = true;
    turnRunWalkReverseTester.groundSegment = 0;
    turnRunWalkReverseTester.groundVelocity = 0;
    pf::changeFighterState(turnRunWalkReverseWorld, turnRunWalkReverseTester, "TurnRun");
    pf::InputFrame turnRunWalkReverseInput;
    turnRunWalkReverseInput.move.x = -pf::fxFromFloat(0.4f);
    pf::tickWorld(turnRunWalkReverseWorld, {turnRunWalkReverseInput, pf::InputFrame{}});
    std::cout << "turn_run_walk_reverse_vel=" << pf::fxToFloat(turnRunWalkReverseTester.groundVelocity)
              << "\n";

    pf::World turnRunRunoffWorld = pf::makeTrainingWorld();
    turnRunRunoffWorld.stage.segments = {
        {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    turnRunRunoffWorld.stage.ledges.clear();
    pf::FighterRuntime& turnRunRunoffTester = turnRunRunoffWorld.fighters[0];
    turnRunRunoffTester.position = {pf::fxFromFloat(0.98f), 0};
    turnRunRunoffTester.previousPosition = turnRunRunoffTester.position;
    turnRunRunoffTester.grounded = true;
    turnRunRunoffTester.groundSegment = 0;
    turnRunRunoffTester.groundVelocity = pf::fxFromFloat(0.2f);
    pf::changeFighterState(turnRunRunoffWorld, turnRunRunoffTester, "TurnRun");
    pf::tickWorld(turnRunRunoffWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "turn_run_runoff_state=" << pf::currentState(turnRunRunoffWorld, turnRunRunoffTester).name
              << " grounded=" << turnRunRunoffTester.grounded
              << "\n";

    pf::World turnRunEdgeStopWorld = pf::makeTrainingWorld();
    turnRunEdgeStopWorld.stage.segments = {
        {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    turnRunEdgeStopWorld.stage.ledges.clear();
    pf::FighterRuntime& turnRunEdgeStopTester = turnRunEdgeStopWorld.fighters[0];
    turnRunEdgeStopTester.position = {pf::fxFromFloat(0.999f), 0};
    turnRunEdgeStopTester.previousPosition = turnRunEdgeStopTester.position;
    turnRunEdgeStopTester.grounded = true;
    turnRunEdgeStopTester.groundSegment = 0;
    turnRunEdgeStopTester.groundVelocity = 0;
    turnRunEdgeStopTester.knockbackVelocity.x = pf::fxFromFloat(0.001f);
    pf::changeFighterState(turnRunEdgeStopWorld, turnRunEdgeStopTester, "TurnRun");
    pf::tickWorld(turnRunEdgeStopWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "turn_run_edge_stop_state=" << pf::currentState(turnRunEdgeStopWorld, turnRunEdgeStopTester).name
              << " turn_run_edge_stop_ground_vel=" << pf::fxToFloat(turnRunEdgeStopTester.groundVelocity)
              << " turn_run_edge_stop_kb=" << pf::toString(turnRunEdgeStopTester.knockbackVelocity)
              << "\n";

    pf::World runDirectWorld = pf::makeTrainingWorld();
    runDirectWorld.stage.segments = {
        {{-pf::fx(100), 0}, {pf::fx(100), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    runDirectWorld.stage.ledges.clear();
    for (int frame = 0; frame < 45; ++frame) {
        pf::InputFrame input;
        input.move.x = pf::fx(1);
        pf::tickWorld(runDirectWorld, {input, pf::InputFrame{}});
    }
    pf::InputFrame runDirectReverse;
    runDirectReverse.move.x = -pf::fx(1);
    for (int frame = 0; frame < 18; ++frame) {
        pf::tickWorld(runDirectWorld, {runDirectReverse, pf::InputFrame{}});
    }
    const pf::FighterRuntime& runDirectTester = runDirectWorld.fighters[0];
    std::cout << "run_direct_state=" << pf::currentState(runDirectWorld, runDirectTester).name
              << " facing=" << runDirectTester.facing
              << " timer=" << runDirectTester.runDirectTimer
              << "\n";

    auto runDirectExpiredWithInput = [](pf::Fix stickX) {
        pf::World world = pf::makeTrainingWorld();
        pf::FighterRuntime& fighter = world.fighters[0];
        fighter.facing = 1;
        fighter.grounded = true;
        fighter.groundSegment = 0;
        pf::changeFighterState(world, fighter, "RunDirect");
        fighter.runDirectTimer = 0;
        pf::InputFrame input;
        input.move.x = stickX;
        pf::tickWorld(world, {input, pf::InputFrame{}});
        return pf::currentState(world, fighter).name;
    };
    std::cout << "run_direct_expired_reverse_state=" << runDirectExpiredWithInput(-pf::fx(1))
              << " run_direct_expired_neutral_state=" << runDirectExpiredWithInput(0)
              << " run_direct_expired_walk_state=" << runDirectExpiredWithInput(pf::fxFromFloat(0.4f))
              << " run_direct_expired_run_state=" << runDirectExpiredWithInput(pf::fx(1))
              << "\n";

    pf::World turnAttackJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& turnAttackJumpTester = turnAttackJumpWorld.fighters[0];
    pf::changeFighterState(turnAttackJumpWorld, turnAttackJumpTester, "Turn");
    pf::InputFrame turnAttackJumpInput;
    turnAttackJumpInput.buttons = pf::ButtonAttack | pf::ButtonJump;
    pf::tickWorld(turnAttackJumpWorld, {turnAttackJumpInput, pf::InputFrame{}});
    std::cout << "turn_attack_jump_state=" << pf::currentState(turnAttackJumpWorld, turnAttackJumpTester).name
              << " turn_attack_jump_facing=" << turnAttackJumpTester.facing
              << "\n";

    pf::World turnBufferedAttackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& turnBufferedAttackTester = turnBufferedAttackWorld.fighters[0];
    turnBufferedAttackTester.facing = -1;
    pf::changeFighterState(turnBufferedAttackWorld, turnBufferedAttackTester, "Turn");
    turnBufferedAttackTester.turnFacingAfter = 1;
    turnBufferedAttackTester.turnHasTurned = true;
    turnBufferedAttackTester.turnJustTurned = true;
    turnBufferedAttackTester.turnBufferedButtons = pf::ButtonAttack;
    pf::tickWorld(turnBufferedAttackWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "turn_buffered_attack_state=" << pf::currentState(turnBufferedAttackWorld, turnBufferedAttackTester).name
              << " turn_buffered_attack_facing=" << turnBufferedAttackTester.facing
              << "\n";

    pf::World fastfallWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& fastfallTester = fastfallWorld.fighters[0];
    fastfallTester.position.y = pf::fx(5);
    fastfallTester.previousPosition = fastfallTester.position;
    fastfallTester.grounded = false;
    fastfallTester.groundSegment = -1;
    fastfallTester.fighterVelocity.y = -pf::fxFromFloat(0.1f);
    pf::changeFighterState(fastfallWorld, fastfallTester, "Fall");
    pf::InputFrame fastfallInput;
    fastfallInput.move.y = -pf::fx(1);
    pf::tickWorld(fastfallWorld, {fastfallInput, pf::InputFrame{}});
    std::cout << "fastfall_state=" << pf::currentState(fastfallWorld, fastfallTester).name
              << " vel=" << pf::toString(fastfallTester.fighterVelocity)
              << "\n";

    pf::World fastfallZeroWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& fastfallZeroTester = fastfallZeroWorld.fighters[0];
    fastfallZeroTester.position.y = pf::fx(5);
    fastfallZeroTester.previousPosition = fastfallZeroTester.position;
    fastfallZeroTester.grounded = false;
    fastfallZeroTester.groundSegment = -1;
    fastfallZeroTester.fighterVelocity.y = 0;
    pf::changeFighterState(fastfallZeroWorld, fastfallZeroTester, "Fall");
    pf::tickWorld(fastfallZeroWorld, {fastfallInput, pf::InputFrame{}});
    std::cout << "fastfall_zero_flag=" << pf::fighterFlag(fastfallZeroTester, 12)
              << " fastfall_zero_vel_y=" << pf::fxToFloat(fastfallZeroTester.fighterVelocity.y)
              << "\n";

    pf::World shortHopWorld = pf::makeTrainingWorld();
    pf::InputFrame shortHopPress;
    shortHopPress.buttons |= pf::ButtonJump;
    pf::tickWorld(shortHopWorld, {shortHopPress, pf::InputFrame{}});
    for (int frame = 0; frame < 5; ++frame) {
        pf::tickWorld(shortHopWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    const pf::FighterRuntime& shortHopTester = shortHopWorld.fighters[0];
    std::cout << "short_hop_state=" << pf::currentState(shortHopWorld, shortHopTester).name
              << " vel=" << pf::toString(shortHopTester.fighterVelocity)
              << "\n";

    pf::World fullHopWorld = pf::makeTrainingWorld();
    pf::InputFrame fullHopHold;
    fullHopHold.buttons |= pf::ButtonJump;
    for (int frame = 0; frame < 6; ++frame) {
        pf::tickWorld(fullHopWorld, {fullHopHold, pf::InputFrame{}});
    }
    const pf::FighterRuntime& fullHopTester = fullHopWorld.fighters[0];
    std::cout << "full_hop_state=" << pf::currentState(fullHopWorld, fullHopTester).name
              << " vel=" << pf::toString(fullHopTester.fighterVelocity)
              << "\n";

    pf::World groundJumpHoldWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& groundJumpHoldTester = groundJumpHoldWorld.fighters[0];
    groundJumpHoldTester.position.y = pf::fx(6);
    groundJumpHoldTester.previousPosition = groundJumpHoldTester.position;
    groundJumpHoldTester.grounded = false;
    groundJumpHoldTester.groundSegment = -1;
    groundJumpHoldTester.fighterVelocity.y = pf::fxFromFloat(2.3f);
    pf::changeFighterState(groundJumpHoldWorld, groundJumpHoldTester, "JumpF");
    pf::tickWorld(groundJumpHoldWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const pf::Fix groundJumpFirstVelY = groundJumpHoldTester.fighterVelocity.y;
    pf::tickWorld(groundJumpHoldWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "ground_jump_hold_vel_y=" << pf::fxToFloat(groundJumpFirstVelY)
              << " ground_jump_fall_vel_y=" << pf::fxToFloat(groundJumpHoldTester.fighterVelocity.y)
              << "\n";

    pf::World tapJumpWorld = pf::makeTrainingWorld();
    pf::InputFrame tapJumpInput;
    tapJumpInput.move.y = pf::fx(1);
    for (int frame = 0; frame < 6; ++frame) {
        pf::tickWorld(tapJumpWorld, {tapJumpInput, pf::InputFrame{}});
    }
    const pf::FighterRuntime& tapJumpTester = tapJumpWorld.fighters[0];
    std::cout << "tap_jump_state=" << pf::currentState(tapJumpWorld, tapJumpTester).name
              << " vel=" << pf::toString(tapJumpTester.fighterVelocity)
              << "\n";

    pf::World airJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& airJumpTester = airJumpWorld.fighters[0];
    airJumpTester.position.y = pf::fx(6);
    airJumpTester.previousPosition = airJumpTester.position;
    airJumpTester.grounded = false;
    airJumpTester.groundSegment = -1;
    airJumpTester.jumpsUsed = 1;
    pf::changeFighterState(airJumpWorld, airJumpTester, "Fall");
    pf::InputFrame airJumpInput;
    airJumpInput.buttons |= pf::ButtonJump;
    airJumpInput.move.x = -pf::fx(1);
    pf::tickWorld(airJumpWorld, {airJumpInput, pf::InputFrame{}});
    std::cout << "air_jump_state=" << pf::currentState(airJumpWorld, airJumpTester).name
              << " jumps=" << airJumpTester.jumpsUsed
              << " vel=" << pf::toString(airJumpTester.fighterVelocity)
              << "\n";

    pf::World fallAttackJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& fallAttackJumpTester = fallAttackJumpWorld.fighters[0];
    fallAttackJumpTester.position.y = pf::fx(6);
    fallAttackJumpTester.previousPosition = fallAttackJumpTester.position;
    fallAttackJumpTester.grounded = false;
    fallAttackJumpTester.groundSegment = -1;
    fallAttackJumpTester.jumpsUsed = 1;
    pf::changeFighterState(fallAttackJumpWorld, fallAttackJumpTester, "Fall");
    pf::InputFrame airAttackJumpInput;
    airAttackJumpInput.buttons = pf::ButtonAttack | pf::ButtonJump;
    pf::tickWorld(fallAttackJumpWorld, {airAttackJumpInput, pf::InputFrame{}});

    pf::World jumpAerialAttackJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& jumpAerialAttackJumpTester = jumpAerialAttackJumpWorld.fighters[0];
    jumpAerialAttackJumpTester.position.y = pf::fx(6);
    jumpAerialAttackJumpTester.previousPosition = jumpAerialAttackJumpTester.position;
    jumpAerialAttackJumpTester.grounded = false;
    jumpAerialAttackJumpTester.groundSegment = -1;
    jumpAerialAttackJumpTester.jumpsUsed = 1;
    pf::changeFighterState(jumpAerialAttackJumpWorld, jumpAerialAttackJumpTester, "JumpAerialF");
    pf::tickWorld(jumpAerialAttackJumpWorld, {airAttackJumpInput, pf::InputFrame{}});

    pf::World passAttackJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& passAttackJumpTester = passAttackJumpWorld.fighters[0];
    passAttackJumpTester.position.y = pf::fx(6);
    passAttackJumpTester.previousPosition = passAttackJumpTester.position;
    passAttackJumpTester.grounded = false;
    passAttackJumpTester.groundSegment = -1;
    passAttackJumpTester.jumpsUsed = 1;
    pf::changeFighterState(passAttackJumpWorld, passAttackJumpTester, "Pass");
    pf::tickWorld(passAttackJumpWorld, {airAttackJumpInput, pf::InputFrame{}});
    std::cout << "fall_attack_jump_state=" << pf::currentState(fallAttackJumpWorld, fallAttackJumpTester).name
              << " jump_aerial_attack_jump_state=" << pf::currentState(jumpAerialAttackJumpWorld, jumpAerialAttackJumpTester).name
              << " pass_attack_jump_state=" << pf::currentState(passAttackJumpWorld, passAttackJumpTester).name
              << "\n";

    pf::World damageFallAirDodgeWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& damageFallAirDodgeTester = damageFallAirDodgeWorld.fighters[0];
    damageFallAirDodgeTester.position.y = pf::fx(6);
    damageFallAirDodgeTester.previousPosition = damageFallAirDodgeTester.position;
    damageFallAirDodgeTester.grounded = false;
    damageFallAirDodgeTester.groundSegment = -1;
    pf::changeFighterState(damageFallAirDodgeWorld, damageFallAirDodgeTester, "DamageFall");
    pf::InputFrame damageFallAirDodgeInput;
    damageFallAirDodgeInput.buttons = pf::ButtonShield;
    pf::tickWorld(damageFallAirDodgeWorld, {damageFallAirDodgeInput, pf::InputFrame{}});

    pf::World damageFallAttackJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& damageFallAttackJumpTester = damageFallAttackJumpWorld.fighters[0];
    damageFallAttackJumpTester.position.y = pf::fx(6);
    damageFallAttackJumpTester.previousPosition = damageFallAttackJumpTester.position;
    damageFallAttackJumpTester.grounded = false;
    damageFallAttackJumpTester.groundSegment = -1;
    damageFallAttackJumpTester.jumpsUsed = 1;
    pf::changeFighterState(damageFallAttackJumpWorld, damageFallAttackJumpTester, "DamageFall");
    pf::tickWorld(damageFallAttackJumpWorld, {airAttackJumpInput, pf::InputFrame{}});

    pf::World damageFallFastfallWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& damageFallFastfallTester = damageFallFastfallWorld.fighters[0];
    damageFallFastfallTester.position.y = pf::fx(6);
    damageFallFastfallTester.previousPosition = damageFallFastfallTester.position;
    damageFallFastfallTester.grounded = false;
    damageFallFastfallTester.groundSegment = -1;
    damageFallFastfallTester.fighterVelocity.y = -pf::fxFromFloat(0.1f);
    pf::changeFighterState(damageFallFastfallWorld, damageFallFastfallTester, "DamageFall");
    pf::InputFrame damageFallFastfallInput;
    damageFallFastfallInput.move.x = pf::fxFromFloat(0.25f);
    damageFallFastfallInput.move.y = -pf::fx(1);
    pf::tickWorld(damageFallFastfallWorld, {damageFallFastfallInput, pf::InputFrame{}});

    std::cout << "damage_fall_air_dodge_state=" << pf::currentState(damageFallAirDodgeWorld, damageFallAirDodgeTester).name
              << " damage_fall_attack_jump_state=" << pf::currentState(damageFallAttackJumpWorld, damageFallAttackJumpTester).name
              << " damage_fall_fastfall_state=" << pf::currentState(damageFallFastfallWorld, damageFallFastfallTester).name
              << " damage_fall_fastfall_flag=" << pf::fighterFlag(damageFallFastfallTester, 12)
              << " damage_fall_drift_x=" << pf::fxToFloat(damageFallFastfallTester.fighterVelocity.x)
              << "\n";

    pf::World airDodgeWorld = pf::makeTrainingWorld();
    airDodgeWorld.stage.segments = {
        {{-pf::fx(100), -pf::fx(200)}, {pf::fx(100), -pf::fx(200)}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    airDodgeWorld.stage.ledges.clear();
    pf::FighterRuntime& airDodgeTester = airDodgeWorld.fighters[0];
    airDodgeTester.position.y = pf::fx(6);
    airDodgeTester.previousPosition = airDodgeTester.position;
    airDodgeTester.grounded = false;
    airDodgeTester.groundSegment = -1;
    pf::changeFighterState(airDodgeWorld, airDodgeTester, "Fall");
    pf::InputFrame airDodgeInput;
    airDodgeInput.buttons |= pf::ButtonShield;
    airDodgeInput.move.x = pf::fx(1);
    airDodgeInput.move.y = -pf::fx(1);
    pf::tickWorld(airDodgeWorld, {airDodgeInput, pf::InputFrame{}});
    std::cout << "air_dodge_state=" << pf::currentState(airDodgeWorld, airDodgeTester).name
              << " vel=" << pf::toString(airDodgeTester.fighterVelocity)
              << "\n";
    for (int frame = 0; frame < 55 && pf::currentState(airDodgeWorld, airDodgeTester).name != "FallSpecial"; ++frame) {
        pf::tickWorld(airDodgeWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "fall_special_state=" << pf::currentState(airDodgeWorld, airDodgeTester).name
              << " vel=" << pf::toString(airDodgeTester.fighterVelocity)
              << "\n";
    std::cout << "air_dodge_fall_special_params limit=" << airDodgeTester.fallSpecialLimitDrift
              << " drift_max=" << pf::fxToFloat(airDodgeTester.fallSpecialDriftMax)
              << " force_landing=" << airDodgeTester.fallSpecialForceLanding
              << " interruptible=" << airDodgeTester.fallSpecialLandingInterruptible
              << "\n";
    pf::InputFrame airDodgeFreefallJumpInput;
    airDodgeFreefallJumpInput.buttons = pf::ButtonJump;
    airDodgeFreefallJumpInput.move.x = -pf::fx(1);
    pf::tickWorld(airDodgeWorld, {airDodgeFreefallJumpInput, pf::InputFrame{}});
    std::cout << "air_dodge_freefall_jump_state=" << pf::currentState(airDodgeWorld, airDodgeTester).name
              << " jumps=" << airDodgeTester.jumpsUsed
              << "\n";

    pf::World fallSpecialJumpWorld = pf::makeTrainingWorld();
    fallSpecialJumpWorld.stage.segments = {
        {{-pf::fx(100), -pf::fx(20)}, {pf::fx(100), -pf::fx(20)}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    fallSpecialJumpWorld.stage.ledges.clear();
    pf::FighterRuntime& fallSpecialJumpTester = fallSpecialJumpWorld.fighters[0];
    fallSpecialJumpTester.position.y = pf::fx(6);
    fallSpecialJumpTester.previousPosition = fallSpecialJumpTester.position;
    fallSpecialJumpTester.grounded = false;
    fallSpecialJumpTester.groundSegment = -1;
    fallSpecialJumpTester.jumpsUsed = 1;
    pf::changeFighterState(fallSpecialJumpWorld, fallSpecialJumpTester, "FallSpecial");
    pf::InputFrame fallSpecialJumpInput;
    fallSpecialJumpInput.buttons = pf::ButtonJump;
    fallSpecialJumpInput.move.x = -pf::fx(1);
    pf::tickWorld(fallSpecialJumpWorld, {fallSpecialJumpInput, pf::InputFrame{}});
    std::cout << "fall_special_jump_state=" << pf::currentState(fallSpecialJumpWorld, fallSpecialJumpTester).name
              << "\n";

    pf::World fallSpecialAttackWorld = pf::makeTrainingWorld();
    fallSpecialAttackWorld.stage.segments = {
        {{-pf::fx(100), -pf::fx(20)}, {pf::fx(100), -pf::fx(20)}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    fallSpecialAttackWorld.stage.ledges.clear();
    pf::FighterRuntime& fallSpecialAttackTester = fallSpecialAttackWorld.fighters[0];
    fallSpecialAttackTester.position.y = pf::fx(6);
    fallSpecialAttackTester.previousPosition = fallSpecialAttackTester.position;
    fallSpecialAttackTester.grounded = false;
    fallSpecialAttackTester.groundSegment = -1;
    fallSpecialAttackTester.jumpsUsed = 1;
    pf::changeFighterState(fallSpecialAttackWorld, fallSpecialAttackTester, "FallSpecial");
    pf::InputFrame fallSpecialAttackInput;
    fallSpecialAttackInput.buttons = pf::ButtonAttack;
    pf::tickWorld(fallSpecialAttackWorld, {fallSpecialAttackInput, pf::InputFrame{}});
    std::cout << "fall_special_attack_state=" << pf::currentState(fallSpecialAttackWorld, fallSpecialAttackTester).name
              << "\n";

    pf::World fallSpecialPlatformWorld = pf::makeTrainingWorld();
    fallSpecialPlatformWorld.stage.segments = {
        {{-pf::fx(20), 0}, {pf::fx(20), 0}, pf::fx(1), pf::SegmentType::Semisolid, false, false},
    };
    fallSpecialPlatformWorld.stage.ledges.clear();
    pf::FighterRuntime& fallSpecialPlatformTester = fallSpecialPlatformWorld.fighters[0];
    fallSpecialPlatformTester.position.y = pf::fx(4);
    fallSpecialPlatformTester.previousPosition = fallSpecialPlatformTester.position;
    fallSpecialPlatformTester.grounded = false;
    fallSpecialPlatformTester.groundSegment = -1;
    fallSpecialPlatformTester.fighterVelocity.y = -pf::fxFromFloat(1.0f);
    pf::changeFighterState(fallSpecialPlatformWorld, fallSpecialPlatformTester, "FallSpecial");
    for (int frame = 0; frame < 12 && pf::currentState(fallSpecialPlatformWorld, fallSpecialPlatformTester).name != "LandingFallSpecial"; ++frame) {
        pf::tickWorld(fallSpecialPlatformWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }

    pf::World fallSpecialPlatformDropWorld = pf::makeTrainingWorld();
    fallSpecialPlatformDropWorld.stage.segments = fallSpecialPlatformWorld.stage.segments;
    fallSpecialPlatformDropWorld.stage.ledges.clear();
    pf::FighterRuntime& fallSpecialPlatformDropTester = fallSpecialPlatformDropWorld.fighters[0];
    fallSpecialPlatformDropTester.position.y = pf::fx(4);
    fallSpecialPlatformDropTester.previousPosition = fallSpecialPlatformDropTester.position;
    fallSpecialPlatformDropTester.grounded = false;
    fallSpecialPlatformDropTester.groundSegment = -1;
    fallSpecialPlatformDropTester.fighterVelocity.y = -pf::fxFromFloat(1.0f);
    pf::changeFighterState(fallSpecialPlatformDropWorld, fallSpecialPlatformDropTester, "FallSpecial");
    pf::InputFrame fallSpecialPlatformDropInput;
    fallSpecialPlatformDropInput.move.y = -pf::fx(1);
    for (int frame = 0; frame < 12; ++frame) {
        pf::tickWorld(fallSpecialPlatformDropWorld, {fallSpecialPlatformDropInput, pf::InputFrame{}});
    }
    std::cout << "fall_special_platform_neutral_state=" << pf::currentState(fallSpecialPlatformWorld, fallSpecialPlatformTester).name
              << " fall_special_platform_down_state=" << pf::currentState(fallSpecialPlatformDropWorld, fallSpecialPlatformDropTester).name
              << " down_y=" << pf::fxToFloat(fallSpecialPlatformDropTester.position.y)
              << " threshold=" << pf::fxToFloat(fallSpecialPlatformDropWorld.fighterDefs[0].properties.common.fallSpecialPlatformStickThresholdX25C)
              << "\n";

    auto fallSpecialDriftProbe = [](bool limitDrift, bool useFastFallTerminal) {
        pf::World world = pf::makeTrainingWorld();
        world.stage.segments = {
            {{-pf::fx(100), -pf::fx(200)}, {pf::fx(100), -pf::fx(200)}, pf::fx(1), pf::SegmentType::Solid, false, false},
        };
        world.stage.ledges.clear();
        pf::FighterRuntime& fighter = world.fighters[0];
        const pf::FighterProperties& attr = world.fighterDefs[0].properties;
        fighter.position.y = pf::fx(60);
        fighter.previousPosition = fighter.position;
        fighter.grounded = false;
        fighter.groundSegment = -1;
        fighter.pendingFallSpecialLimitDrift = limitDrift;
        fighter.pendingFallSpecialUseFastFallTerminal = useFastFallTerminal;
        fighter.pendingFallSpecialDriftMax = pf::fxMul(attr.airDriftMax, attr.common.fallSpecialDriftX340);
        pf::changeFighterState(world, fighter, "FallSpecial");
        pf::InputFrame input;
        input.move.x = pf::fx(1);
        for (int frame = 0; frame < 90; ++frame) {
            pf::tickWorld(world, {input, pf::InputFrame{}});
        }
        return fighter.fighterVelocity;
    };
    const pf::Vec2 fallSpecialNormalDrift = fallSpecialDriftProbe(false, false);
    const pf::Vec2 fallSpecialLimitedDrift = fallSpecialDriftProbe(true, false);
    const pf::Vec2 fallSpecialFastTerminal = fallSpecialDriftProbe(true, true);
    std::cout << "fall_special_drift_normal_x=" << pf::fxToFloat(fallSpecialNormalDrift.x)
              << " fall_special_drift_limited_x=" << pf::fxToFloat(fallSpecialLimitedDrift.x)
              << " fall_special_drift_limit=" << pf::fxToFloat(pf::fxMul(
                     fallSpecialPlatformWorld.fighterDefs[0].properties.airDriftMax,
                     fallSpecialPlatformWorld.fighterDefs[0].properties.common.fallSpecialDriftX340))
              << " fall_special_terminal_y=" << pf::fxToFloat(fallSpecialLimitedDrift.y)
              << " fall_special_fast_terminal_y=" << pf::fxToFloat(fallSpecialFastTerminal.y)
              << "\n";

    auto fallAnimationProbe = [](const std::string& stateName, pf::Fix velocityX) {
        pf::World world = pf::makeTrainingWorld();
        world.stage.segments = {
            {{-pf::fx(100), -pf::fx(20)}, {pf::fx(100), -pf::fx(20)}, pf::fx(1), pf::SegmentType::Solid, false, false},
        };
        world.stage.ledges.clear();
        pf::FighterRuntime& fighter = world.fighters[0];
        fighter.position.y = pf::fx(6);
        fighter.previousPosition = fighter.position;
        fighter.grounded = false;
        fighter.groundSegment = -1;
        fighter.fighterVelocity.x = velocityX;
        pf::changeFighterState(world, fighter, stateName);
        pf::tickWorld(world, {pf::InputFrame{}, pf::InputFrame{}});
        return fighter.animationActionIndexOverride;
    };
    const pf::Fix fallAnimVelocity = fallSpecialPlatformWorld.fighterDefs[0].properties.airDriftMax;
    std::cout << "fall_anim_actions="
              << fallAnimationProbe("Fall", 0) << ","
              << fallAnimationProbe("Fall", fallAnimVelocity) << ","
              << fallAnimationProbe("Fall", -fallAnimVelocity)
              << " fall_aerial_anim_actions="
              << fallAnimationProbe("FallAerial", 0) << ","
              << fallAnimationProbe("FallAerial", fallAnimVelocity) << ","
              << fallAnimationProbe("FallAerial", -fallAnimVelocity)
              << " fall_special_anim_actions="
              << fallAnimationProbe("FallSpecial", fallAnimVelocity) << ","
              << fallAnimationProbe("FallSpecial", -fallAnimVelocity)
              << " fall_anim_threshold="
              << pf::fxToFloat(fallSpecialPlatformWorld.fighterDefs[0].properties.common.fallAnimationDriftThresholdX444)
              << "\n";

    pf::World jumpAerialFinishWorld = pf::makeTrainingWorld();
    jumpAerialFinishWorld.stage.segments = {
        {{-pf::fx(100), -pf::fx(20)}, {pf::fx(100), -pf::fx(20)}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    jumpAerialFinishWorld.stage.ledges.clear();
    pf::FighterRuntime& jumpAerialFinishTester = jumpAerialFinishWorld.fighters[0];
    jumpAerialFinishTester.position.y = pf::fx(6);
    jumpAerialFinishTester.previousPosition = jumpAerialFinishTester.position;
    jumpAerialFinishTester.grounded = false;
    jumpAerialFinishTester.groundSegment = -1;
    pf::changeFighterState(jumpAerialFinishWorld, jumpAerialFinishTester, "JumpAerialF");
    const int jumpAerialFinishProbeFrames = pf::currentState(jumpAerialFinishWorld, jumpAerialFinishTester).animationLengthFrames + 5;
    for (int frame = 0; frame < jumpAerialFinishProbeFrames && pf::currentState(jumpAerialFinishWorld, jumpAerialFinishTester).name == "JumpAerialF"; ++frame) {
        pf::tickWorld(jumpAerialFinishWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "jump_aerial_finish_state=" << pf::currentState(jumpAerialFinishWorld, jumpAerialFinishTester).name
              << " action=" << jumpAerialFinishTester.animationActionIndexOverride
              << "\n";

    pf::World itemScrewWorld = pf::makeTrainingWorld();
    itemScrewWorld.fighters[1].position.x = pf::fx(100);
    pf::FighterRuntime& itemScrewTester = itemScrewWorld.fighters[0];
    itemScrewTester.fighterVelocity = {pf::fxFromFloat(0.5f), pf::fxFromFloat(0.25f)};
    itemScrewTester.input.frames[0].move.x = pf::fx(1);
    pf::changeFighterState(itemScrewWorld, itemScrewTester, "ItemScrew");
    const pf::Vec2 itemScrewEnterVelocity = itemScrewTester.fighterVelocity;
    pf::tickWorld(itemScrewWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const pf::Vec2 itemScrewFirstTickVelocity = itemScrewTester.fighterVelocity;
    pf::tickWorld(itemScrewWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const pf::Vec2 itemScrewSecondTickVelocity = itemScrewTester.fighterVelocity;
    pf::tickWorld(itemScrewWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const pf::Vec2 itemScrewThirdTickVelocity = itemScrewTester.fighterVelocity;

    pf::World itemScrewFinishWorld = pf::makeTrainingWorld();
    itemScrewFinishWorld.fighters[1].position.x = pf::fx(100);
    pf::FighterRuntime& itemScrewFinishTester = itemScrewFinishWorld.fighters[0];
    itemScrewFinishTester.position.y = pf::fx(40);
    itemScrewFinishTester.previousPosition = itemScrewFinishTester.position;
    itemScrewFinishTester.grounded = false;
    itemScrewFinishTester.groundSegment = -1;
    pf::changeFighterState(itemScrewFinishWorld, itemScrewFinishTester, "ItemScrew");
    itemScrewFinishTester.lastStateChangeFrame = itemScrewFinishTester.internalFrame - 1000;
    pf::tickWorld(itemScrewFinishWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World itemScrewAirFinishWorld = pf::makeTrainingWorld();
    itemScrewAirFinishWorld.fighters[1].position.x = pf::fx(100);
    pf::FighterRuntime& itemScrewAirFinishTester = itemScrewAirFinishWorld.fighters[0];
    itemScrewAirFinishTester.position.y = pf::fx(40);
    itemScrewAirFinishTester.previousPosition = itemScrewAirFinishTester.position;
    itemScrewAirFinishTester.grounded = false;
    itemScrewAirFinishTester.groundSegment = -1;
    pf::changeFighterState(itemScrewAirFinishWorld, itemScrewAirFinishTester, "ItemScrewAir");
    itemScrewAirFinishTester.lastStateChangeFrame = itemScrewAirFinishTester.internalFrame - 1000;
    pf::tickWorld(itemScrewAirFinishWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World itemScrewPlatformDropWorld = pf::makeTrainingWorld();
    itemScrewPlatformDropWorld.fighters[1].position.x = pf::fx(100);
    itemScrewPlatformDropWorld.stage.segments = fallSpecialPlatformWorld.stage.segments;
    itemScrewPlatformDropWorld.stage.ledges.clear();
    pf::FighterRuntime& itemScrewPlatformDropTester = itemScrewPlatformDropWorld.fighters[0];
    itemScrewPlatformDropTester.position.y = pf::fx(4);
    itemScrewPlatformDropTester.previousPosition = itemScrewPlatformDropTester.position;
    itemScrewPlatformDropTester.grounded = false;
    itemScrewPlatformDropTester.groundSegment = -1;
    itemScrewPlatformDropTester.fighterVelocity.y = -pf::fxFromFloat(1.0f);
    pf::changeFighterState(itemScrewPlatformDropWorld, itemScrewPlatformDropTester, "ItemScrewAir");
    pf::InputFrame itemScrewPlatformDropInput;
    itemScrewPlatformDropInput.move.y = -pf::fx(1);
    for (int frame = 0; frame < 12; ++frame) {
        pf::tickWorld(itemScrewPlatformDropWorld, {itemScrewPlatformDropInput, pf::InputFrame{}});
    }
    std::cout << "item_screw_state=" << pf::currentState(itemScrewWorld, itemScrewTester).name
              << " item_screw_grounded=" << itemScrewTester.grounded
              << " item_screw_enter_vel=" << pf::toString(itemScrewEnterVelocity)
              << " item_screw_first_tick_vel=" << pf::toString(itemScrewFirstTickVelocity)
              << " item_screw_second_tick_vel=" << pf::toString(itemScrewSecondTickVelocity)
              << " item_screw_third_tick_vel=" << pf::toString(itemScrewThirdTickVelocity)
              << " item_screw_frame=" << pf::frameInState(itemScrewTester)
              << " item_screw_grav=" << pf::fxToFloat(itemScrewWorld.fighterDefs[0].properties.grav)
              << " item_screw_mult=" << pf::fxToFloat(itemScrewWorld.fighterDefs[0].properties.common.itemScrewJumpMultiplierX800)
              << " item_screw_finish_state=" << pf::currentState(itemScrewFinishWorld, itemScrewFinishTester).name
              << " item_screw_air_finish_state=" << pf::currentState(itemScrewAirFinishWorld, itemScrewAirFinishTester).name
              << " item_screw_platform_down_state=" << pf::currentState(itemScrewPlatformDropWorld, itemScrewPlatformDropTester).name
              << "\n";

    pf::World wavedashWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& wavedashTester = wavedashWorld.fighters[0];
    wavedashTester.position.y = pf::fxFromFloat(0.4f);
    wavedashTester.previousPosition = wavedashTester.position;
    wavedashTester.grounded = false;
    wavedashTester.groundSegment = -1;
    pf::changeFighterState(wavedashWorld, wavedashTester, "Fall");
    pf::InputFrame wavedashInput;
    wavedashInput.buttons |= pf::ButtonShield;
    wavedashInput.move.x = pf::fx(1);
    wavedashInput.move.y = -pf::fx(1);
    for (int frame = 0; frame < 6 && pf::currentState(wavedashWorld, wavedashTester).name != "LandingFallSpecial"; ++frame) {
        pf::tickWorld(wavedashWorld, {frame == 0 ? wavedashInput : pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "wavedash_landing_state=" << pf::currentState(wavedashWorld, wavedashTester).name
              << " ground_vel=" << pf::fxToFloat(wavedashTester.groundVelocity)
              << " interruptible=" << wavedashTester.interruptibleFrame
              << "\n";

    pf::World airDriftWorld = pf::makeTrainingWorld();
    airDriftWorld.stage.segments = {
        {{-pf::fx(100), -pf::fx(20)}, {pf::fx(100), -pf::fx(20)}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    airDriftWorld.stage.ledges.clear();
    pf::FighterRuntime& airDriftTester = airDriftWorld.fighters[0];
    airDriftTester.position.y = pf::fx(20);
    airDriftTester.previousPosition = airDriftTester.position;
    airDriftTester.grounded = false;
    airDriftTester.groundSegment = -1;
    pf::changeFighterState(airDriftWorld, airDriftTester, "Fall");
    pf::InputFrame driftInput;
    driftInput.move.x = pf::fx(1);
    for (int frame = 0; frame < 20; ++frame) {
        pf::tickWorld(airDriftWorld, {driftInput, pf::InputFrame{}});
    }
    const pf::FighterRuntime& airDriftResult = airDriftWorld.fighters[0];
    std::cout << "air_drift_state=" << pf::currentState(airDriftWorld, airDriftResult).name
              << " vel=" << pf::toString(airDriftResult.fighterVelocity)
              << "\n";

    pf::World landingWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& landingTester = landingWorld.fighters[0];
    landingTester.position.y = pf::fx(4);
    landingTester.previousPosition = landingTester.position;
    landingTester.grounded = false;
    landingTester.groundSegment = -1;
    landingTester.fighterVelocity.y = -pf::fxFromFloat(1.0f);
    pf::changeFighterState(landingWorld, landingTester, "Fall");
    for (int frame = 0; frame < 20 && pf::currentState(landingWorld, landingTester).name != "Landing"; ++frame) {
        pf::tickWorld(landingWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "landing_state=" << pf::currentState(landingWorld, landingTester).name
              << " interruptible=" << landingTester.interruptibleFrame
              << " grounded=" << landingTester.grounded
              << "\n";
    for (int frame = 0; frame < 3; ++frame) {
        pf::tickWorld(landingWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    pf::InputFrame landingAttack;
    landingAttack.buttons |= pf::ButtonAttack;
    pf::tickWorld(landingWorld, {landingAttack, pf::InputFrame{}});
    std::cout << "landing_interrupt_state=" << pf::currentState(landingWorld, landingTester).name
              << "\n";

    pf::World landingGrabWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& landingGrabTester = landingGrabWorld.fighters[0];
    pf::changeFighterState(landingGrabWorld, landingGrabTester, "Landing");
    for (int frame = 0; frame < landingGrabWorld.fighterDefs[0].properties.normalLandingLag; ++frame) {
        pf::tickWorld(landingGrabWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    pf::InputFrame landingGrabInput;
    landingGrabInput.buttons = pf::ButtonGrab;
    pf::tickWorld(landingGrabWorld, {landingGrabInput, pf::InputFrame{}});
    std::cout << "landing_grab_state=" << pf::currentState(landingGrabWorld, landingGrabTester).name
              << "\n";

    pf::World landingUpSmashWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& landingUpSmashTester = landingUpSmashWorld.fighters[0];
    pf::changeFighterState(landingUpSmashWorld, landingUpSmashTester, "Landing");
    for (int frame = 0; frame < landingUpSmashWorld.fighterDefs[0].properties.normalLandingLag; ++frame) {
        pf::tickWorld(landingUpSmashWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    pf::InputFrame landingUpSmashInput;
    landingUpSmashInput.buttons = pf::ButtonAttack;
    landingUpSmashInput.move.y = pf::fx(1);
    pf::tickWorld(landingUpSmashWorld, {landingUpSmashInput, pf::InputFrame{}});
    std::cout << "landing_up_smash_state=" << pf::currentState(landingUpSmashWorld, landingUpSmashTester).name
              << "\n";

    pf::World landingSquatEarlyWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& landingSquatEarlyTester = landingSquatEarlyWorld.fighters[0];
    pf::changeFighterState(landingSquatEarlyWorld, landingSquatEarlyTester, "Landing");
    for (int frame = 0; frame < landingSquatEarlyWorld.fighterDefs[0].properties.normalLandingLag - 1; ++frame) {
        pf::tickWorld(landingSquatEarlyWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    pf::InputFrame landingSquatInput;
    landingSquatInput.move.y = -pf::fx(1);
    pf::tickWorld(landingSquatEarlyWorld, {landingSquatInput, pf::InputFrame{}});

    pf::World landingSquatLateWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& landingSquatLateTester = landingSquatLateWorld.fighters[0];
    pf::changeFighterState(landingSquatLateWorld, landingSquatLateTester, "Landing");
    for (int frame = 0; frame < landingSquatLateWorld.fighterDefs[0].properties.normalLandingLag + 2; ++frame) {
        pf::tickWorld(landingSquatLateWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    pf::tickWorld(landingSquatLateWorld, {landingSquatInput, pf::InputFrame{}});
    std::cout << "landing_squat_early_state=" << pf::currentState(landingSquatEarlyWorld, landingSquatEarlyTester).name
              << " landing_squat_late_state=" << pf::currentState(landingSquatLateWorld, landingSquatLateTester).name
              << "\n";

    pf::World landingDashSquatWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& landingDashSquatTester = landingDashSquatWorld.fighters[0];
    pf::changeFighterState(landingDashSquatWorld, landingDashSquatTester, "Landing");
    for (int frame = 0; frame < landingDashSquatWorld.fighterDefs[0].properties.normalLandingLag - 1; ++frame) {
        pf::tickWorld(landingDashSquatWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    pf::InputFrame landingDashSquatInput;
    landingDashSquatInput.move.x = pf::fx(1);
    landingDashSquatInput.move.y = -pf::fx(1);
    pf::tickWorld(landingDashSquatWorld, {landingDashSquatInput, pf::InputFrame{}});
    std::cout << "landing_dash_squat_state=" << pf::currentState(landingDashSquatWorld, landingDashSquatTester).name
              << "\n";

    pf::World landingShieldJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& landingShieldJumpTester = landingShieldJumpWorld.fighters[0];
    pf::changeFighterState(landingShieldJumpWorld, landingShieldJumpTester, "Landing");
    for (int frame = 0; frame < landingShieldJumpWorld.fighterDefs[0].properties.normalLandingLag; ++frame) {
        pf::tickWorld(landingShieldJumpWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    pf::InputFrame landingShieldJumpInput;
    landingShieldJumpInput.buttons = pf::ButtonShield | pf::ButtonJump;
    pf::tickWorld(landingShieldJumpWorld, {landingShieldJumpInput, pf::InputFrame{}});
    std::cout << "landing_shield_jump_state=" << pf::currentState(landingShieldJumpWorld, landingShieldJumpTester).name
              << "\n";

    pf::World landingWalkFastWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& landingWalkFastTester = landingWalkFastWorld.fighters[0];
    pf::changeFighterState(landingWalkFastWorld, landingWalkFastTester, "Landing");
    landingWalkFastTester.groundVelocity = pf::fxMul(
        landingWalkFastWorld.fighterDefs[0].properties.walkMaxVel,
        pf::fxFromFloat(3.0f));
    for (int frame = 0; frame < landingWalkFastWorld.fighterDefs[0].properties.normalLandingLag; ++frame) {
        pf::tickWorld(landingWalkFastWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    pf::tickWorld(landingWalkFastWorld, {lightWalkInput, pf::InputFrame{}});
    std::cout << "landing_fast_walk_state=" << pf::currentState(landingWalkFastWorld, landingWalkFastTester).name
              << "\n";

    pf::World noImpactLandingWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& noImpactLandingTester = noImpactLandingWorld.fighters[0];
    noImpactLandingTester.position.y = pf::fxFromFloat(0.1f);
    noImpactLandingTester.previousPosition = noImpactLandingTester.position;
    noImpactLandingTester.grounded = false;
    noImpactLandingTester.groundSegment = -1;
    noImpactLandingTester.fighterVelocity.y = -pf::fxFromFloat(0.1f);
    pf::changeFighterState(noImpactLandingWorld, noImpactLandingTester, "Fall");
    pf::tickWorld(noImpactLandingWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "no_impact_landing_state=" << pf::currentState(noImpactLandingWorld, noImpactLandingTester).name
              << " landing_vel_y=" << pf::fxToFloat(noImpactLandingTester.lastLandingVelocityY)
              << "\n";

    pf::World aerialLandingWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& aerialLandingTester = aerialLandingWorld.fighters[0];
    aerialLandingTester.position.y = pf::fx(4);
    aerialLandingTester.previousPosition = aerialLandingTester.position;
    aerialLandingTester.grounded = false;
    aerialLandingTester.groundSegment = -1;
    aerialLandingTester.fighterVelocity.y = -pf::fxFromFloat(1.0f);
    pf::changeFighterState(aerialLandingWorld, aerialLandingTester, "AirAttackN");
    for (int frame = 0; frame < 20 && pf::currentState(aerialLandingWorld, aerialLandingTester).name != "LandingAirN"; ++frame) {
        pf::tickWorld(aerialLandingWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "aerial_landing_state=" << pf::currentState(aerialLandingWorld, aerialLandingTester).name
              << " interruptible=" << aerialLandingTester.interruptibleFrame
              << "\n";

    pf::World sweptLandingWorld = pf::makeTrainingWorld();
    sweptLandingWorld.stage.segments = {
        {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, true, true},
    };
    sweptLandingWorld.stage.ledges.clear();
    pf::FighterRuntime& sweptLandingTester = sweptLandingWorld.fighters[0];
    sweptLandingTester.position = {-pf::fx(1), pf::fx(1)};
    sweptLandingTester.previousPosition = sweptLandingTester.position;
    sweptLandingTester.grounded = false;
    sweptLandingTester.groundSegment = -1;
    sweptLandingTester.fighterVelocity = {pf::fx(3), -pf::fx(3)};
    pf::changeFighterState(sweptLandingWorld, sweptLandingTester, "Fall");
    pf::tickWorld(sweptLandingWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "swept_landing_state=" << pf::currentState(sweptLandingWorld, sweptLandingTester).name
              << " pos=" << pf::toString(sweptLandingTester.position)
              << " grounded=" << sweptLandingTester.grounded
              << "\n";

    pf::World wallWorld = pf::makeTrainingWorld();
    wallWorld.stage.segments = {
        {{pf::fxFromFloat(1.5f), -pf::fx(2)}, {pf::fxFromFloat(1.5f), pf::fx(5)}, pf::fx(1), pf::SegmentType::Solid, false, false, pf::SegmentLineKind::LeftWall},
    };
    wallWorld.stage.ledges.clear();
    pf::FighterRuntime& wallTester = wallWorld.fighters[0];
    wallTester.position = {0, pf::fx(1)};
    wallTester.previousPosition = wallTester.position;
    wallTester.grounded = false;
    wallTester.groundSegment = -1;
    wallTester.fighterVelocity = {pf::fx(2), 0};
    pf::changeFighterState(wallWorld, wallTester, "Fall");
    pf::tickWorld(wallWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "wall_collision_state=" << pf::currentState(wallWorld, wallTester).name
              << " pos=" << pf::toString(wallTester.position)
              << " vel=" << pf::toString(wallTester.fighterVelocity)
              << "\n";

    pf::World stopWallWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& stopWallTester = stopWallWorld.fighters[0];
    const pf::Fix stopWallX = stopWallTester.position.x + stopWallTester.ecb.points[2].x + pf::fxFromFloat(0.5f);
    stopWallWorld.stage.segments = {
        {{-pf::fx(10), 0}, {pf::fx(10), 0}, pf::fx(1), pf::SegmentType::Solid, false, false, pf::SegmentLineKind::Floor},
        {{stopWallX, -pf::fx(2)}, {stopWallX, pf::fx(5)}, pf::fx(1), pf::SegmentType::Solid, false, false, pf::SegmentLineKind::LeftWall},
    };
    stopWallWorld.stage.ledges.clear();
    stopWallTester.grounded = true;
    stopWallTester.groundSegment = 0;
    stopWallTester.facing = 1;
    pf::changeFighterState(stopWallWorld, stopWallTester, "Run");
    stopWallTester.groundVelocity = stopWallWorld.fighterDefs[0].properties.walkMaxVel + pf::fxFromFloat(0.75f);
    pf::InputFrame stopWallInput;
    stopWallInput.move.x = pf::fx(1);
    pf::tickWorld(stopWallWorld, {stopWallInput, pf::InputFrame{}});

    pf::World stopWallWaitWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& stopWallWaitTester = stopWallWaitWorld.fighters[0];
    const pf::Fix stopWallWaitX = stopWallWaitTester.position.x + stopWallWaitTester.ecb.points[2].x + pf::fxFromFloat(0.5f);
    stopWallWaitWorld.stage.segments = {
        {{-pf::fx(10), 0}, {pf::fx(10), 0}, pf::fx(1), pf::SegmentType::Solid, false, false, pf::SegmentLineKind::Floor},
        {{stopWallWaitX, -pf::fx(2)}, {stopWallWaitX, pf::fx(5)}, pf::fx(1), pf::SegmentType::Solid, false, false, pf::SegmentLineKind::LeftWall},
    };
    stopWallWaitWorld.stage.ledges.clear();
    stopWallWaitTester.grounded = true;
    stopWallWaitTester.groundSegment = 0;
    stopWallWaitTester.facing = 1;
    pf::changeFighterState(stopWallWaitWorld, stopWallWaitTester, "Wait");
    stopWallWaitTester.groundVelocity = stopWallWaitWorld.fighterDefs[0].properties.walkMaxVel + pf::fxFromFloat(0.75f);
    pf::tickWorld(stopWallWaitWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "stop_wall_fast_state=" << pf::currentState(stopWallWorld, stopWallTester).name
              << " stop_wall_fast_ground_vel=" << pf::fxToFloat(stopWallTester.groundVelocity)
              << " stop_wall_wait_state=" << pf::currentState(stopWallWaitWorld, stopWallWaitTester).name
              << " stop_wall_wait_ground_vel=" << pf::fxToFloat(stopWallWaitTester.groundVelocity)
              << "\n";

    pf::World wallJumpWorld = pf::makeTrainingWorld();
    wallJumpWorld.stage.segments = {
        {{pf::fxFromFloat(1.5f), -pf::fx(5)}, {pf::fxFromFloat(1.5f), pf::fx(8)}, pf::fx(1), pf::SegmentType::Solid, false, false, pf::SegmentLineKind::LeftWall},
    };
    wallJumpWorld.stage.ledges.clear();
    pf::FighterRuntime& wallJumpTester = wallJumpWorld.fighters[0];
    wallJumpTester.position = {0, pf::fx(2)};
    wallJumpTester.previousPosition = wallJumpTester.position;
    wallJumpTester.grounded = false;
    wallJumpTester.groundSegment = -1;
    wallJumpTester.fighterVelocity = {pf::fx(2), 0};
    pf::changeFighterState(wallJumpWorld, wallJumpTester, "Fall");
    pf::tickWorld(wallJumpWorld, {pf::InputFrame{}, pf::InputFrame{}});
    pf::InputFrame wallJumpInput;
    wallJumpInput.move.x = -pf::fx(1);
    for (int frame = 0; frame < 7; ++frame) {
        pf::tickWorld(wallJumpWorld, {frame == 0 ? wallJumpInput : pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "wall_jump_state=" << pf::currentState(wallJumpWorld, wallJumpTester).name
              << " facing=" << wallJumpTester.facing
              << " wall_contact=" << wallJumpTester.wallContactSide
              << " jumps=" << wallJumpTester.wallJumpsUsed
              << " vel=" << pf::toString(wallJumpTester.fighterVelocity)
              << "\n";

    pf::World ceilingWorld = pf::makeTrainingWorld();
    ceilingWorld.stage.ledges.clear();
    pf::FighterRuntime& ceilingTester = ceilingWorld.fighters[0];
    ceilingTester.position = {0, pf::fxFromFloat(0.2f)};
    ceilingTester.previousPosition = ceilingTester.position;
    ceilingTester.grounded = false;
    ceilingTester.groundSegment = -1;
    const pf::Fix ceilingY = ceilingTester.position.y + ceilingTester.ecb.points[1].y + pf::fxFromFloat(0.5f);
    ceilingWorld.stage.segments = {
        {{-pf::fx(4), ceilingY}, {pf::fx(4), ceilingY}, pf::fx(1), pf::SegmentType::Solid, false, false, pf::SegmentLineKind::Ceiling},
    };
    ceilingTester.fighterVelocity = {0, pf::fx(2)};
    pf::changeFighterState(ceilingWorld, ceilingTester, "Fall");
    pf::tickWorld(ceilingWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "ceiling_collision_state=" << pf::currentState(ceilingWorld, ceilingTester).name
              << " pos=" << pf::toString(ceilingTester.position)
              << " vel=" << pf::toString(ceilingTester.fighterVelocity)
              << "\n";

    pf::World stopCeilJumpWorld = pf::makeTrainingWorld();
    stopCeilJumpWorld.stage.ledges.clear();
    pf::FighterRuntime& stopCeilJumpTester = stopCeilJumpWorld.fighters[0];
    stopCeilJumpTester.position = {0, pf::fxFromFloat(0.2f)};
    stopCeilJumpTester.previousPosition = stopCeilJumpTester.position;
    stopCeilJumpTester.grounded = false;
    stopCeilJumpTester.groundSegment = -1;
    pf::changeFighterState(stopCeilJumpWorld, stopCeilJumpTester, "JumpF");
    const pf::Fix stopCeilJumpY = stopCeilJumpTester.position.y + stopCeilJumpTester.ecb.points[1].y + pf::fxFromFloat(0.5f);
    stopCeilJumpWorld.stage.segments = {
        {{-pf::fx(4), stopCeilJumpY}, {pf::fx(4), stopCeilJumpY}, pf::fx(1), pf::SegmentType::Solid, false, false, pf::SegmentLineKind::Ceiling},
    };
    stopCeilJumpTester.fighterVelocity = {pf::fxFromFloat(0.25f), pf::fx(2)};
    pf::tickWorld(stopCeilJumpWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const std::string stopCeilJumpState = pf::currentState(stopCeilJumpWorld, stopCeilJumpTester).name;
    stopCeilJumpTester.lastStateChangeFrame = stopCeilJumpTester.internalFrame - 1000;
    pf::tickWorld(stopCeilJumpWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "stop_ceil_jump_state=" << stopCeilJumpState
              << " stop_ceil_jump_vel=" << pf::toString(stopCeilJumpTester.fighterVelocity)
              << " stop_ceil_finish_state=" << pf::currentState(stopCeilJumpWorld, stopCeilJumpTester).name
              << "\n";

    pf::World flyReflectWallWorld = pf::makeTrainingWorld();
    flyReflectWallWorld.stage.segments = {
        {{pf::fxFromFloat(1.5f), -pf::fx(2)}, {pf::fxFromFloat(1.5f), pf::fx(5)}, pf::fx(1), pf::SegmentType::Solid, false, false, pf::SegmentLineKind::LeftWall},
    };
    flyReflectWallWorld.stage.ledges.clear();
    pf::FighterRuntime& flyReflectWallTester = flyReflectWallWorld.fighters[0];
    flyReflectWallTester.position = {0, pf::fx(1)};
    flyReflectWallTester.previousPosition = flyReflectWallTester.position;
    flyReflectWallTester.grounded = false;
    flyReflectWallTester.groundSegment = -1;
    pf::changeFighterState(flyReflectWallWorld, flyReflectWallTester, "DamageFlyN");
    flyReflectWallTester.damageTumble = true;
    flyReflectWallTester.hitstun = 10;
    flyReflectWallTester.knockbackVelocity = {pf::fx(2), 0};
    pf::tickWorld(flyReflectWallWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World flyReflectCeilWorld = pf::makeTrainingWorld();
    flyReflectCeilWorld.stage.ledges.clear();
    pf::FighterRuntime& flyReflectCeilTester = flyReflectCeilWorld.fighters[0];
    flyReflectCeilTester.position = {0, pf::fxFromFloat(0.2f)};
    flyReflectCeilTester.previousPosition = flyReflectCeilTester.position;
    flyReflectCeilTester.grounded = false;
    flyReflectCeilTester.groundSegment = -1;
    pf::changeFighterState(flyReflectCeilWorld, flyReflectCeilTester, "DamageFlyN");
    const pf::Fix flyReflectCeilingY = flyReflectCeilTester.position.y + flyReflectCeilTester.ecb.points[1].y + pf::fxFromFloat(0.5f);
    flyReflectCeilWorld.stage.segments = {
        {{-pf::fx(4), flyReflectCeilingY}, {pf::fx(4), flyReflectCeilingY}, pf::fx(1), pf::SegmentType::Solid, false, false, pf::SegmentLineKind::Ceiling},
    };
    flyReflectCeilTester.damageTumble = true;
    flyReflectCeilTester.hitstun = 10;
    flyReflectCeilTester.knockbackVelocity = {0, pf::fx(2)};
    pf::tickWorld(flyReflectCeilWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World flyReflectFastfallWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& flyReflectFastfallTester = flyReflectFastfallWorld.fighters[0];
    flyReflectFastfallTester.position.y = pf::fx(6);
    flyReflectFastfallTester.previousPosition = flyReflectFastfallTester.position;
    flyReflectFastfallTester.grounded = false;
    flyReflectFastfallTester.groundSegment = -1;
    flyReflectFastfallTester.fighterVelocity.y = -pf::fxFromFloat(0.1f);
    flyReflectFastfallTester.hitstun = 10;
    pf::changeFighterState(flyReflectFastfallWorld, flyReflectFastfallTester, "FlyReflectWall");
    pf::InputFrame flyReflectFastfallInput;
    flyReflectFastfallInput.move.y = -pf::fx(1);
    pf::tickWorld(flyReflectFastfallWorld, {flyReflectFastfallInput, pf::InputFrame{}});
    std::cout << "fly_reflect_wall_state=" << pf::currentState(flyReflectWallWorld, flyReflectWallTester).name
              << " fly_reflect_wall_kb=" << pf::toString(flyReflectWallTester.knockbackVelocity)
              << " fly_reflect_ceil_state=" << pf::currentState(flyReflectCeilWorld, flyReflectCeilTester).name
              << " fly_reflect_ceil_kb=" << pf::toString(flyReflectCeilTester.knockbackVelocity)
              << " fly_reflect_fastfall_flag=" << pf::fighterFlag(flyReflectFastfallTester, 12)
              << " fly_reflect_fastfall_vel_y=" << pf::fxToFloat(flyReflectFastfallTester.fighterVelocity.y)
              << "\n";

    pf::World downReflectWallWorld = pf::makeTrainingWorld();
    downReflectWallWorld.stage.segments = {
        {{pf::fxFromFloat(1.5f), -pf::fx(2)}, {pf::fxFromFloat(1.5f), pf::fx(5)}, pf::fx(1), pf::SegmentType::Solid, false, false, pf::SegmentLineKind::LeftWall},
    };
    downReflectWallWorld.stage.ledges.clear();
    pf::FighterRuntime& downReflectWallTester = downReflectWallWorld.fighters[0];
    downReflectWallTester.position = {0, pf::fx(1)};
    downReflectWallTester.previousPosition = downReflectWallTester.position;
    downReflectWallTester.grounded = false;
    downReflectWallTester.groundSegment = -1;
    pf::changeFighterState(downReflectWallWorld, downReflectWallTester, "DownDamageU");
    downReflectWallTester.knockbackVelocity = {pf::fx(2), 0};
    pf::tickWorld(downReflectWallWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World downDamageWallTechWorld = pf::makeTrainingWorld();
    downDamageWallTechWorld.stage.segments = downReflectWallWorld.stage.segments;
    downDamageWallTechWorld.stage.ledges.clear();
    pf::FighterRuntime& downDamageWallTechTester = downDamageWallTechWorld.fighters[0];
    downDamageWallTechTester.position = {0, pf::fx(1)};
    downDamageWallTechTester.previousPosition = downDamageWallTechTester.position;
    downDamageWallTechTester.grounded = false;
    downDamageWallTechTester.groundSegment = -1;
    pf::changeFighterState(downDamageWallTechWorld, downDamageWallTechTester, "DownDamageU");
    downDamageWallTechTester.knockbackVelocity = {pf::fx(2), 0};
    pf::InputFrame downDamageWallTechInput;
    downDamageWallTechInput.buttons |= pf::ButtonShield;
    pf::tickWorld(downDamageWallTechWorld, {downDamageWallTechInput, pf::InputFrame{}});

    pf::World downReflectLandingWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downReflectLandingTester = downReflectLandingWorld.fighters[0];
    downReflectLandingTester.position.y = pf::fxFromFloat(2.0f);
    downReflectLandingTester.previousPosition = downReflectLandingTester.position;
    downReflectLandingTester.grounded = false;
    downReflectLandingTester.groundSegment = -1;
    downReflectLandingTester.fighterVelocity.y = -pf::fxFromFloat(4.0f);
    pf::changeFighterState(downReflectLandingWorld, downReflectLandingTester, "DownReflect");
    for (int frame = 0; frame < 10 && !downReflectLandingTester.grounded; ++frame) {
        pf::tickWorld(downReflectLandingWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "down_reflect_wall_state=" << pf::currentState(downReflectWallWorld, downReflectWallTester).name
              << " down_reflect_wall_kb=" << pf::toString(downReflectWallTester.knockbackVelocity)
              << " down_damage_wall_tech_state=" << pf::currentState(downDamageWallTechWorld, downDamageWallTechTester).name
              << " down_reflect_landing_state=" << pf::currentState(downReflectLandingWorld, downReflectLandingTester).name
              << "\n";

    pf::World dynamicFloorWorld = pf::makeTrainingWorld();
    dynamicFloorWorld.stage.segments = {
        {{-pf::fx(2), 0}, {pf::fx(2), pf::fx(1)}, pf::fx(1), pf::SegmentType::Solid, false, false, pf::SegmentLineKind::Floor, true},
    };
    dynamicFloorWorld.stage.ledges.clear();
    pf::FighterRuntime& dynamicFloorTester = dynamicFloorWorld.fighters[0];
    dynamicFloorTester.position = {0, pf::fx(3)};
    dynamicFloorTester.previousPosition = dynamicFloorTester.position;
    dynamicFloorTester.grounded = false;
    dynamicFloorTester.groundSegment = -1;
    dynamicFloorTester.fighterVelocity = {0, -pf::fx(3)};
    pf::changeFighterState(dynamicFloorWorld, dynamicFloorTester, "Fall");
    for (int frame = 0; frame < 5 && !dynamicFloorTester.grounded; ++frame) {
        pf::tickWorld(dynamicFloorWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "dynamic_floor_state=" << pf::currentState(dynamicFloorWorld, dynamicFloorTester).name
              << " grounded=" << dynamicFloorTester.grounded
              << " ground_segment=" << dynamicFloorTester.groundSegment
              << "\n";

    pf::World dynamicCeilingWorld = pf::makeTrainingWorld();
    dynamicCeilingWorld.stage.segments = {
        {{pf::fx(4), pf::fx(3)}, {-pf::fx(4), pf::fx(3)}, pf::fx(1), pf::SegmentType::Solid, false, false, pf::SegmentLineKind::Floor, true},
    };
    dynamicCeilingWorld.stage.ledges.clear();
    pf::FighterRuntime& dynamicCeilingTester = dynamicCeilingWorld.fighters[0];
    dynamicCeilingTester.position = {0, pf::fxFromFloat(0.2f)};
    dynamicCeilingTester.previousPosition = dynamicCeilingTester.position;
    dynamicCeilingTester.grounded = false;
    dynamicCeilingTester.groundSegment = -1;
    dynamicCeilingTester.fighterVelocity = {0, pf::fx(2)};
    pf::changeFighterState(dynamicCeilingWorld, dynamicCeilingTester, "Fall");
    pf::tickWorld(dynamicCeilingWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "dynamic_ceiling_state=" << pf::currentState(dynamicCeilingWorld, dynamicCeilingTester).name
              << " pos=" << pf::toString(dynamicCeilingTester.position)
              << " vel=" << pf::toString(dynamicCeilingTester.fighterVelocity)
              << "\n";

    pf::World platformDropWorld = pf::makeTrainingWorld();
    platformDropWorld.stage.segments = {
        {{-pf::fx(4), pf::fx(2)}, {pf::fx(4), pf::fx(2)}, pf::fx(1), pf::SegmentType::Semisolid, false, false},
    };
    platformDropWorld.stage.ledges.clear();
    pf::FighterRuntime& platformDropTester = platformDropWorld.fighters[0];
    platformDropTester.position = {0, pf::fx(2)};
    platformDropTester.previousPosition = platformDropTester.position;
    platformDropTester.grounded = true;
    platformDropTester.groundSegment = 0;
    pf::InputFrame platformDropInput;
    platformDropInput.move.y = -pf::fx(1);
    pf::tickWorld(platformDropWorld, {platformDropInput, pf::InputFrame{}});
    std::cout << "squat_state=" << pf::currentState(platformDropWorld, platformDropTester).name
              << " grounded=" << platformDropTester.grounded
              << "\n";
    for (int frame = 0; frame < 3 && platformDropTester.grounded; ++frame) {
        pf::tickWorld(platformDropWorld, {platformDropInput, pf::InputFrame{}});
    }
    std::cout << "platform_drop_state=" << pf::currentState(platformDropWorld, platformDropTester).name
              << " grounded=" << platformDropTester.grounded
              << " floor_skip=" << platformDropTester.floorSkipSegment
              << " vel=" << pf::toString(platformDropTester.fighterVelocity)
              << "\n";
    pf::tickWorld(platformDropWorld, {pf::InputFrame{}, pf::InputFrame{}});
    pf::tickWorld(platformDropWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "platform_drop_clear_state=" << pf::currentState(platformDropWorld, platformDropTester).name
              << " floor_skip=" << platformDropTester.floorSkipSegment
              << " pos=" << pf::toString(platformDropTester.position)
              << "\n";

    pf::World passFastfallWorld = pf::makeTrainingWorld();
    passFastfallWorld.stage.segments = {
        {{-pf::fx(20), -pf::fx(20)}, {pf::fx(20), -pf::fx(20)}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    passFastfallWorld.stage.ledges.clear();
    pf::FighterRuntime& passFastfallTester = passFastfallWorld.fighters[0];
    passFastfallTester.position.y = pf::fx(6);
    passFastfallTester.previousPosition = passFastfallTester.position;
    passFastfallTester.grounded = false;
    passFastfallTester.groundSegment = -1;
    passFastfallTester.fighterVelocity.y = -pf::fxFromFloat(0.1f);
    pf::changeFighterState(passFastfallWorld, passFastfallTester, "Pass");
    pf::InputFrame passFastfallInput;
    passFastfallInput.move.y = -pf::fx(1);
    pf::tickWorld(passFastfallWorld, {passFastfallInput, pf::InputFrame{}});
    std::cout << "pass_fastfall_flag=" << pf::fighterFlag(passFastfallTester, 12)
              << " pass_fastfall_vel_y=" << pf::fxToFloat(passFastfallTester.fighterVelocity.y)
              << "\n";

    pf::World squatWaitDropDashWorld = pf::makeTrainingWorld();
    squatWaitDropDashWorld.stage.segments = {
        {{-pf::fx(4), pf::fx(2)}, {pf::fx(4), pf::fx(2)}, pf::fx(1), pf::SegmentType::Semisolid, false, false},
    };
    squatWaitDropDashWorld.stage.ledges.clear();
    pf::FighterRuntime& squatWaitDropDashTester = squatWaitDropDashWorld.fighters[0];
    squatWaitDropDashTester.position = {0, pf::fx(2)};
    squatWaitDropDashTester.previousPosition = squatWaitDropDashTester.position;
    squatWaitDropDashTester.grounded = true;
    squatWaitDropDashTester.groundSegment = 0;
    pf::changeFighterState(squatWaitDropDashWorld, squatWaitDropDashTester, "SquatWait");
    pf::InputFrame squatWaitDropDashInput;
    squatWaitDropDashInput.move.x = pf::fx(1);
    squatWaitDropDashInput.move.y = -pf::fx(1);
    pf::tickWorld(squatWaitDropDashWorld, {squatWaitDropDashInput, pf::InputFrame{}});
    const std::string squatWaitDropDashStartState = pf::currentState(squatWaitDropDashWorld, squatWaitDropDashTester).name;
    for (int frame = 0; frame < 3 && squatWaitDropDashTester.grounded; ++frame) {
        pf::tickWorld(squatWaitDropDashWorld, {squatWaitDropDashInput, pf::InputFrame{}});
    }
    std::cout << "squat_wait_drop_dash_start_state=" << squatWaitDropDashStartState
              << " squat_wait_drop_dash_state=" << pf::currentState(squatWaitDropDashWorld, squatWaitDropDashTester).name
              << "\n";

    pf::World squatShieldJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& squatShieldJumpTester = squatShieldJumpWorld.fighters[0];
    pf::changeFighterState(squatShieldJumpWorld, squatShieldJumpTester, "Squat");
    pf::InputFrame squatShieldJumpInput;
    squatShieldJumpInput.buttons = pf::ButtonShield | pf::ButtonJump;
    pf::tickWorld(squatShieldJumpWorld, {squatShieldJumpInput, pf::InputFrame{}});

    pf::World squatWaitShieldDashWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& squatWaitShieldDashTester = squatWaitShieldDashWorld.fighters[0];
    pf::changeFighterState(squatWaitShieldDashWorld, squatWaitShieldDashTester, "SquatWait");
    pf::InputFrame squatWaitShieldDashInput;
    squatWaitShieldDashInput.buttons = pf::ButtonShield;
    squatWaitShieldDashInput.move.x = pf::fx(1);
    pf::tickWorld(squatWaitShieldDashWorld, {squatWaitShieldDashInput, pf::InputFrame{}});
    std::cout << "squat_shield_jump_state=" << pf::currentState(squatShieldJumpWorld, squatShieldJumpTester).name
              << " squat_wait_shield_dash_state=" << pf::currentState(squatWaitShieldDashWorld, squatWaitShieldDashTester).name
              << "\n";

    pf::InputFrame grabInput;
    grabInput.buttons = pf::ButtonGrab;
    pf::World squatWaitGrabWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& squatWaitGrabTester = squatWaitGrabWorld.fighters[0];
    pf::changeFighterState(squatWaitGrabWorld, squatWaitGrabTester, "SquatWait");
    pf::tickWorld(squatWaitGrabWorld, {grabInput, pf::InputFrame{}});

    pf::World squatRvGrabWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& squatRvGrabTester = squatRvGrabWorld.fighters[0];
    pf::changeFighterState(squatRvGrabWorld, squatRvGrabTester, "SquatRv");
    pf::tickWorld(squatRvGrabWorld, {grabInput, pf::InputFrame{}});

    pf::World runBrakeGrabWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& runBrakeGrabTester = runBrakeGrabWorld.fighters[0];
    pf::changeFighterState(runBrakeGrabWorld, runBrakeGrabTester, "RunBrake");
    pf::tickWorld(runBrakeGrabWorld, {grabInput, pf::InputFrame{}});

    pf::World turnRunGrabWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& turnRunGrabTester = turnRunGrabWorld.fighters[0];
    pf::changeFighterState(turnRunGrabWorld, turnRunGrabTester, "TurnRun");
    pf::tickWorld(turnRunGrabWorld, {grabInput, pf::InputFrame{}});
    std::cout << "no_catch_squat_wait_grab_state=" << pf::currentState(squatWaitGrabWorld, squatWaitGrabTester).name
              << " no_catch_squat_rv_grab_state=" << pf::currentState(squatRvGrabWorld, squatRvGrabTester).name
              << " no_catch_run_brake_grab_state=" << pf::currentState(runBrakeGrabWorld, runBrakeGrabTester).name
              << " no_catch_turn_run_grab_state=" << pf::currentState(turnRunGrabWorld, turnRunGrabTester).name
              << "\n";

    auto prepareCatchWaitProbe = [](pf::World& probeWorld) -> pf::FighterRuntime& {
        pf::FighterRuntime& grabber = probeWorld.fighters[0];
        pf::FighterRuntime& victim = probeWorld.fighters[1];
        pf::changeFighterState(probeWorld, grabber, "CatchWait");
        grabber.grabbedFighter = 1;
        victim.grabberFighter = 0;
        return grabber;
    };

    pf::World catchDiagonalForwardWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& catchDiagonalForwardTester = prepareCatchWaitProbe(catchDiagonalForwardWorld);
    pf::InputFrame catchDiagonalForwardInput;
    catchDiagonalForwardInput.move = {pf::fx(1), pf::fx(1)};
    pf::tickWorld(catchDiagonalForwardWorld, {catchDiagonalForwardInput, pf::InputFrame{}});

    pf::World catchDiagonalBackwardWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& catchDiagonalBackwardTester = prepareCatchWaitProbe(catchDiagonalBackwardWorld);
    pf::InputFrame catchDiagonalBackwardInput;
    catchDiagonalBackwardInput.move = {-pf::fx(1), pf::fx(1)};
    pf::tickWorld(catchDiagonalBackwardWorld, {catchDiagonalBackwardInput, pf::InputFrame{}});

    pf::World catchZPummelWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& catchZPummelTester = prepareCatchWaitProbe(catchZPummelWorld);
    pf::tickWorld(catchZPummelWorld, {grabInput, pf::InputFrame{}});

    pf::World catchCStickHighWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& catchCStickHighTester = prepareCatchWaitProbe(catchCStickHighWorld);
    pf::InputFrame catchCStickHighInput;
    catchCStickHighInput.cStick.y = pf::fx(1);
    pf::tickWorld(catchCStickHighWorld, {catchCStickHighInput, pf::InputFrame{}});

    pf::World catchCStickLowFreshWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& catchCStickLowFreshTester = prepareCatchWaitProbe(catchCStickLowFreshWorld);
    pf::InputFrame catchCStickLowInput;
    catchCStickLowInput.cStick.y = -pf::fx(1);
    pf::tickWorld(catchCStickLowFreshWorld, {catchCStickLowInput, pf::InputFrame{}});

    pf::World catchCStickLowHeldWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& catchCStickLowHeldTester = prepareCatchWaitProbe(catchCStickLowHeldWorld);
    pf::tickWorld(catchCStickLowHeldWorld, {catchCStickLowInput, pf::InputFrame{}});
    pf::tickWorld(catchCStickLowHeldWorld, {catchCStickLowInput, pf::InputFrame{}});

    std::cout << "catch_diagonal_forward_throw_state=" << pf::currentState(catchDiagonalForwardWorld, catchDiagonalForwardTester).name
              << " catch_diagonal_backward_throw_state=" << pf::currentState(catchDiagonalBackwardWorld, catchDiagonalBackwardTester).name
              << " catch_z_pummel_state=" << pf::currentState(catchZPummelWorld, catchZPummelTester).name
              << " catch_cstick_high_throw_state=" << pf::currentState(catchCStickHighWorld, catchCStickHighTester).name
              << " catch_cstick_low_fresh_state=" << pf::currentState(catchCStickLowFreshWorld, catchCStickLowFreshTester).name
              << " catch_cstick_low_held_state=" << pf::currentState(catchCStickLowHeldWorld, catchCStickLowHeldTester).name
              << "\n";

    pf::World catchWaitRunoffWorld = pf::makeTrainingWorld();
    catchWaitRunoffWorld.stage.segments = {
        {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    catchWaitRunoffWorld.stage.ledges.clear();
    pf::FighterRuntime& catchWaitRunoffGrabber = catchWaitRunoffWorld.fighters[0];
    pf::FighterRuntime& catchWaitRunoffVictim = catchWaitRunoffWorld.fighters[1];
    catchWaitRunoffGrabber.position = {pf::fxFromFloat(0.99f), 0};
    catchWaitRunoffGrabber.previousPosition = catchWaitRunoffGrabber.position;
    catchWaitRunoffGrabber.grounded = true;
    catchWaitRunoffGrabber.groundSegment = 0;
    catchWaitRunoffGrabber.groundVelocity = pf::fxFromFloat(0.2f);
    catchWaitRunoffVictim.position = {pf::fxFromFloat(0.8f), 0};
    catchWaitRunoffVictim.previousPosition = catchWaitRunoffVictim.position;
    catchWaitRunoffVictim.grounded = true;
    catchWaitRunoffVictim.groundSegment = 0;
    pf::changeFighterState(catchWaitRunoffWorld, catchWaitRunoffGrabber, "CatchWait");
    pf::changeFighterState(catchWaitRunoffWorld, catchWaitRunoffVictim, "CaptureWaitHi");
    catchWaitRunoffGrabber.grabbedFighter = 1;
    catchWaitRunoffVictim.grabberFighter = 0;
    pf::tickWorld(catchWaitRunoffWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "catch_runoff_grabber_state=" << pf::currentState(catchWaitRunoffWorld, catchWaitRunoffGrabber).name
              << " catch_runoff_grabber_grounded=" << catchWaitRunoffGrabber.grounded
              << " catch_runoff_grabber_jumps=" << catchWaitRunoffGrabber.jumpsUsed
              << " catch_runoff_grabber_ecb=" << catchWaitRunoffGrabber.ecbLockTimer
              << " catch_runoff_victim_state=" << pf::currentState(catchWaitRunoffWorld, catchWaitRunoffVictim).name
              << " catch_runoff_victim_grounded=" << catchWaitRunoffVictim.grounded
              << " catch_runoff_victim_jumps=" << catchWaitRunoffVictim.jumpsUsed
              << " catch_runoff_victim_ecb=" << catchWaitRunoffVictim.ecbLockTimer
              << "\n";

    auto prepareCaptureWaitProbe = [](pf::World& probeWorld) -> pf::FighterRuntime& {
        pf::FighterRuntime& grabber = probeWorld.fighters[0];
        pf::FighterRuntime& victim = probeWorld.fighters[1];
        pf::changeFighterState(probeWorld, grabber, "CatchWait");
        pf::changeFighterState(probeWorld, victim, "CaptureWaitHi");
        grabber.grabbedFighter = 1;
        victim.grabberFighter = 0;
        victim.grabTimer = pf::fx(20);
        return victim;
    };

    pf::World captureBMashWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& captureBMashTester = prepareCaptureWaitProbe(captureBMashWorld);
    pf::InputFrame captureBMashInput;
    captureBMashInput.buttons = pf::ButtonSpecial;
    pf::tickWorld(captureBMashWorld, {pf::InputFrame{}, captureBMashInput});

    pf::World captureLightStickWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& captureLightStickTester = prepareCaptureWaitProbe(captureLightStickWorld);
    pf::InputFrame captureLightStickInput;
    captureLightStickInput.move.x = pf::fxFromFloat(0.5f);
    pf::tickWorld(captureLightStickWorld, {pf::InputFrame{}, captureLightStickInput});

    pf::World captureHeldShieldBWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& captureHeldShieldBTester = prepareCaptureWaitProbe(captureHeldShieldBWorld);
    pf::InputFrame captureHeldShieldInput;
    captureHeldShieldInput.buttons = pf::ButtonShield;
    captureHeldShieldBTester.input.frames[0] = captureHeldShieldInput;
    pf::InputFrame captureHeldShieldBInput;
    captureHeldShieldBInput.buttons = pf::ButtonShield | pf::ButtonSpecial;
    pf::tickWorld(captureHeldShieldBWorld, {pf::InputFrame{}, captureHeldShieldBInput});

    std::cout << "capture_b_mash_timer=" << pf::fxToFloat(captureBMashTester.grabTimer)
              << " capture_light_stick_timer=" << pf::fxToFloat(captureLightStickTester.grabTimer)
              << " capture_held_shield_b_mash_timer=" << pf::fxToFloat(captureHeldShieldBTester.grabTimer)
              << " capture_mash_threshold=" << pf::fxToFloat(captureBMashWorld.fighterDefs[0].properties.common.grabMashStickThresholdX308)
              << "\n";

    auto runCaptureLowHighProbe = [](const std::string& stateName) {
        pf::World probeWorld = pf::makeTrainingWorld();
        const pf::MeleeCommonData common = probeWorld.fighterDefs[0].properties.common;
        probeWorld.fighterDefs[0] = pf::makeDebugRook();
        probeWorld.fighterDefs[0].properties.common = common;
        for (pf::FighterRuntime& fighter : probeWorld.fighters) {
            fighter.hsdPose.joints.clear();
            fighter.hsdBlendFromPose.joints.clear();
            fighter.hsdJointWorldTransforms.clear();
            fighter.hsdJointWorldPositions.clear();
            fighter.hsdHurtboxCapsules.clear();
            fighter.hsdModelVisibilityDefaultStates.clear();
            fighter.hsdModelVisibilityStates.clear();
            fighter.hsdModelPartAnimations.clear();
            fighter.state = probeWorld.fighterDefs[0].stateIndex("Wait");
        }
        pf::FighterRuntime& grabber = probeWorld.fighters[0];
        pf::FighterRuntime& victim = probeWorld.fighters[1];
        pf::calculateEcb(probeWorld.fighterDefs[0], grabber, true);
        pf::calculateEcb(probeWorld.fighterDefs[0], victim, true);
        const pf::Fix highOffset = probeWorld.fighterDefs[static_cast<size_t>(victim.fighterDef)].properties.common.captureHighThresholdX3C4 + pf::fx(1);
        grabber.previousPosition = grabber.position;
        victim.position = {grabber.position.x, grabber.position.y - highOffset};
        victim.previousPosition = victim.position;
        victim.grounded = true;
        victim.groundSegment = 0;
        victim.grabTimer = pf::fx(20);
        pf::changeFighterState(probeWorld, grabber, "CatchWait");
        pf::changeFighterState(probeWorld, victim, stateName);
        grabber.grabbedFighter = 1;
        victim.grabberFighter = 0;
        pf::tickWorld(probeWorld, {pf::InputFrame{}, pf::InputFrame{}});
        return std::tuple<std::string, bool, int, int>{
            pf::currentState(probeWorld, victim).name,
            victim.grounded,
            victim.jumpsUsed,
            victim.ecbLockTimer,
        };
    };
    const auto [capturePulledLowHighState, capturePulledLowHighGrounded, capturePulledLowHighJumps, capturePulledLowHighEcb] =
        runCaptureLowHighProbe("CapturePulledLw");
    const auto [captureWaitLowHighState, captureWaitLowHighGrounded, captureWaitLowHighJumps, captureWaitLowHighEcb] =
        runCaptureLowHighProbe("CaptureWaitLw");
    const auto [captureDamageLowHighState, captureDamageLowHighGrounded, captureDamageLowHighJumps, captureDamageLowHighEcb] =
        runCaptureLowHighProbe("CaptureDamageLw");
    std::cout << "capture_low_high_states="
              << capturePulledLowHighState << "," << captureWaitLowHighState << "," << captureDamageLowHighState
              << " capture_low_high_grounded="
              << capturePulledLowHighGrounded << "," << captureWaitLowHighGrounded << "," << captureDamageLowHighGrounded
              << " capture_low_high_jumps="
              << capturePulledLowHighJumps << "," << captureWaitLowHighJumps << "," << captureDamageLowHighJumps
              << " capture_low_high_ecb="
              << capturePulledLowHighEcb << "," << captureWaitLowHighEcb << "," << captureDamageLowHighEcb
              << "\n";

    auto runCaptureHighLowProbe = [](const std::string& stateName) {
        pf::World probeWorld = pf::makeTrainingWorld();
        const pf::MeleeCommonData common = probeWorld.fighterDefs[0].properties.common;
        probeWorld.fighterDefs[0] = pf::makeDebugRook();
        probeWorld.fighterDefs[0].properties.common = common;
        for (pf::FighterRuntime& fighter : probeWorld.fighters) {
            fighter.hsdPose.joints.clear();
            fighter.hsdBlendFromPose.joints.clear();
            fighter.hsdJointWorldTransforms.clear();
            fighter.hsdJointWorldPositions.clear();
            fighter.hsdHurtboxCapsules.clear();
            fighter.hsdModelVisibilityDefaultStates.clear();
            fighter.hsdModelVisibilityStates.clear();
            fighter.hsdModelPartAnimations.clear();
            fighter.state = probeWorld.fighterDefs[0].stateIndex("Wait");
        }
        pf::FighterRuntime& grabber = probeWorld.fighters[0];
        pf::FighterRuntime& victim = probeWorld.fighters[1];
        pf::calculateEcb(probeWorld.fighterDefs[0], grabber, true);
        pf::calculateEcb(probeWorld.fighterDefs[0], victim, true);
        const pf::Fix highOffset = probeWorld.fighterDefs[static_cast<size_t>(victim.fighterDef)].properties.common.captureHighThresholdX3C4 + pf::fx(1);
        grabber.previousPosition = grabber.position;
        victim.position = {grabber.position.x, grabber.position.y + highOffset};
        victim.previousPosition = victim.position;
        victim.grounded = false;
        victim.groundSegment = -1;
        victim.jumpsUsed = 1;
        victim.grabTimer = pf::fx(20);
        pf::changeFighterState(probeWorld, grabber, "CatchWait");
        pf::changeFighterState(probeWorld, victim, stateName);
        grabber.grabbedFighter = 1;
        victim.grabberFighter = 0;
        pf::tickWorld(probeWorld, {pf::InputFrame{}, pf::InputFrame{}});
        return std::tuple<std::string, bool, int, int>{
            pf::currentState(probeWorld, victim).name,
            victim.grounded,
            victim.jumpsUsed,
            victim.ecbLockTimer,
        };
    };
    const auto [capturePulledHighLowState, capturePulledHighLowGrounded, capturePulledHighLowJumps, capturePulledHighLowEcb] =
        runCaptureHighLowProbe("CapturePulledHi");
    const auto [captureWaitHighLowState, captureWaitHighLowGrounded, captureWaitHighLowJumps, captureWaitHighLowEcb] =
        runCaptureHighLowProbe("CaptureWaitHi");
    const auto [captureDamageHighLowState, captureDamageHighLowGrounded, captureDamageHighLowJumps, captureDamageHighLowEcb] =
        runCaptureHighLowProbe("CaptureDamageHi");
    std::cout << "capture_high_low_states="
              << capturePulledHighLowState << "," << captureWaitHighLowState << "," << captureDamageHighLowState
              << " capture_high_low_grounded="
              << capturePulledHighLowGrounded << "," << captureWaitHighLowGrounded << "," << captureDamageHighLowGrounded
              << " capture_high_low_jumps="
              << capturePulledHighLowJumps << "," << captureWaitHighLowJumps << "," << captureDamageHighLowJumps
              << " capture_high_low_ecb="
              << capturePulledHighLowEcb << "," << captureWaitHighLowEcb << "," << captureDamageHighLowEcb
              << "\n";

    pf::World captureJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& captureJumpGrabber = captureJumpWorld.fighters[0];
    pf::FighterRuntime& captureJumpTester = captureJumpWorld.fighters[1];
    pf::changeFighterState(captureJumpWorld, captureJumpGrabber, "CatchWait");
    pf::changeFighterState(captureJumpWorld, captureJumpTester, "CaptureWaitHi");
    captureJumpGrabber.grabbedFighter = 1;
    captureJumpTester.grabberFighter = 0;
    captureJumpTester.grabTimer = pf::fx(1);
    captureJumpTester.jumpsUsed = 0;
    pf::InputFrame captureJumpInput;
    captureJumpInput.buttons = pf::ButtonJump;
    pf::tickWorld(captureJumpWorld, {pf::InputFrame{}, captureJumpInput});
    std::cout << "capture_jump_state=" << pf::currentState(captureJumpWorld, captureJumpTester).name
              << " capture_jump_grounded=" << captureJumpTester.grounded
              << " capture_jump_jumps=" << captureJumpTester.jumpsUsed
              << " capture_jump_ecb=" << captureJumpTester.ecbLockTimer
              << " capture_jump_vel=" << pf::toString(captureJumpTester.fighterVelocity)
              << " capture_jump_links=" << captureJumpGrabber.grabbedFighter << "," << captureJumpTester.grabberFighter
              << "\n";

    pf::World captureJumpFastfallWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& captureJumpFastfallTester = captureJumpFastfallWorld.fighters[0];
    captureJumpFastfallTester.position.y += pf::fx(8);
    captureJumpFastfallTester.previousPosition = captureJumpFastfallTester.position;
    captureJumpFastfallTester.grounded = false;
    captureJumpFastfallTester.groundSegment = -1;
    captureJumpFastfallTester.fighterVelocity.y = -pf::fxFromFloat(0.1f);
    pf::changeFighterState(captureJumpFastfallWorld, captureJumpFastfallTester, "CaptureJump");
    captureJumpFastfallTester.fighterVelocity.y = -pf::fxFromFloat(0.1f);
    pf::InputFrame captureJumpFastfallInput;
    captureJumpFastfallInput.move.y = -pf::fx(1);
    pf::tickWorld(captureJumpFastfallWorld, {captureJumpFastfallInput, pf::InputFrame{}});
    std::cout << "capture_jump_fastfall_flag=" << pf::fighterFlag(captureJumpFastfallTester, 12)
              << " capture_jump_fastfall_vel_y=" << pf::fxToFloat(captureJumpFastfallTester.fighterVelocity.y)
              << "\n";

    const auto captureJumpAttackProbe = [](int idleFrames) {
        pf::World probeWorld = pf::makeTrainingWorld();
        pf::FighterRuntime& tester = probeWorld.fighters[0];
        tester.position.y += pf::fx(80);
        tester.previousPosition = tester.position;
        tester.grounded = false;
        tester.groundSegment = -1;
        pf::changeFighterState(probeWorld, tester, "CaptureJump");
        for (int frame = 0; frame < idleFrames; ++frame) {
            pf::tickWorld(probeWorld, {pf::InputFrame{}, pf::InputFrame{}});
        }
        pf::InputFrame attackInput;
        attackInput.buttons = pf::ButtonAttack;
        pf::tickWorld(probeWorld, {attackInput, pf::InputFrame{}});
        return pf::currentState(probeWorld, tester).name;
    };
    const int captureJumpGateEnableFrame =
        std::max(0, static_cast<int>(pf::fxToFloat(
            captureJumpWorld.fighterDefs[0].properties.common.captureJumpGravityThresholdX3B8) + 0.5f)) + 1;
    std::cout << "capture_jump_gate_threshold="
              << pf::fxToFloat(captureJumpWorld.fighterDefs[0].properties.common.captureJumpGravityThresholdX3B8)
              << " capture_jump_gate_enable_frame=" << captureJumpGateEnableFrame
              << " capture_jump_attack_early_state=" << captureJumpAttackProbe(0)
              << " capture_jump_attack_late_state=" << captureJumpAttackProbe(captureJumpGateEnableFrame)
              << "\n";

    pf::World captureJumpLandingWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& captureJumpLandingTester = captureJumpLandingWorld.fighters[0];
    captureJumpLandingTester.position.y += pf::fxFromFloat(0.05f);
    captureJumpLandingTester.previousPosition = captureJumpLandingTester.position;
    captureJumpLandingTester.grounded = false;
    captureJumpLandingTester.groundSegment = -1;
    captureJumpLandingTester.fighterVelocity.y = -pf::fxFromFloat(0.05f);
    pf::changeFighterState(captureJumpLandingWorld, captureJumpLandingTester, "CaptureJump");
    captureJumpLandingTester.fighterVelocity.y = -pf::fxFromFloat(0.05f);
    pf::tickWorld(captureJumpLandingWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "capture_jump_landing_state=" << pf::currentState(captureJumpLandingWorld, captureJumpLandingTester).name
              << " capture_jump_landing_grounded=" << captureJumpLandingTester.grounded
              << "\n";

    pf::World thrownDecayWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& thrownDecayGrabber = thrownDecayWorld.fighters[0];
    pf::FighterRuntime& thrownDecayTester = thrownDecayWorld.fighters[1];
    thrownDecayGrabber.grabbedFighter = 1;
    thrownDecayTester.grabberFighter = 0;
    thrownDecayTester.grabTimer = pf::fx(20);
    pf::changeFighterState(thrownDecayWorld, thrownDecayGrabber, "ThrowF");
    std::cout << "throw_entry_cleared_timer=" << pf::fxToFloat(thrownDecayTester.grabTimer)
              << " thrown_state=" << pf::currentState(thrownDecayWorld, thrownDecayTester).name
              << "\n";

    pf::World throwWeightScaleWorld = pf::makeTrainingWorld(0, 1);
    pf::FighterRuntime& throwWeightScaleGrabber = throwWeightScaleWorld.fighters[0];
    throwWeightScaleWorld.fighterDefs[0].properties.weightIndependentThrowsMask = 0;
    throwWeightScaleWorld.fighterDefs[1].properties.weight = pf::fx(200);
    throwWeightScaleGrabber.grabbedFighter = 1;
    throwWeightScaleWorld.fighters[1].grabberFighter = 0;
    pf::changeFighterState(throwWeightScaleWorld, throwWeightScaleGrabber, "ThrowF");

    pf::World throwWeightIndependentWorld = pf::makeTrainingWorld(0, 1);
    pf::FighterRuntime& throwWeightIndependentGrabber = throwWeightIndependentWorld.fighters[0];
    throwWeightIndependentWorld.fighterDefs[0].properties.weightIndependentThrowsMask = 1;
    throwWeightIndependentWorld.fighterDefs[1].properties.weight = pf::fx(200);
    throwWeightIndependentGrabber.grabbedFighter = 1;
    throwWeightIndependentWorld.fighters[1].grabberFighter = 0;
    pf::changeFighterState(throwWeightIndependentWorld, throwWeightIndependentGrabber, "ThrowF");
    std::cout << "throw_weight_scaled_rate=" << pf::fxToFloat(throwWeightScaleGrabber.animationRate)
              << " throw_weight_independent_rate=" << pf::fxToFloat(throwWeightIndependentGrabber.animationRate)
              << "\n";

    pf::World throwFinishGroundWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& throwFinishGroundTester = throwFinishGroundWorld.fighters[0];
    pf::changeFighterState(throwFinishGroundWorld, throwFinishGroundTester, "ThrowB");
    const int throwFinishGroundFrames = pf::currentState(throwFinishGroundWorld, throwFinishGroundTester).animationLengthFrames + 2;
    for (int frame = 0; frame < throwFinishGroundFrames; ++frame) {
        pf::tickWorld(throwFinishGroundWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }

    pf::World throwFinishAirWorld = pf::makeTrainingWorld();
    throwFinishAirWorld.stage.segments = {
        {{-pf::fx(20), -pf::fx(200)}, {pf::fx(20), -pf::fx(200)}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    throwFinishAirWorld.stage.ledges.clear();
    pf::FighterRuntime& throwFinishAirTester = throwFinishAirWorld.fighters[0];
    throwFinishAirTester.position.y = pf::fx(8);
    throwFinishAirTester.previousPosition = throwFinishAirTester.position;
    throwFinishAirTester.grounded = false;
    throwFinishAirTester.groundSegment = -1;
    pf::changeFighterState(throwFinishAirWorld, throwFinishAirTester, "ThrowB");
    const int throwFinishAirFrames = pf::currentState(throwFinishAirWorld, throwFinishAirTester).animationLengthFrames + 2;
    for (int frame = 0; frame < throwFinishAirFrames; ++frame) {
        pf::tickWorld(throwFinishAirWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "throw_finish_ground_state=" << pf::currentState(throwFinishGroundWorld, throwFinishGroundTester).name
              << " throw_finish_air_state=" << pf::currentState(throwFinishAirWorld, throwFinishAirTester).name
              << "\n";

    constexpr int kMarioRosterIndex = 0;
    constexpr int kBowserRosterIndex = 11;
    constexpr int kPeachRosterIndex = 12;
    constexpr int kZeldaRosterIndex = 13;

    pf::World bowserPeachThrowWorld = pf::makeTrainingWorld(kBowserRosterIndex, kPeachRosterIndex);
    pf::FighterRuntime& bowserThrowTester = bowserPeachThrowWorld.fighters[0];
    pf::FighterRuntime& peachThrownTester = bowserPeachThrowWorld.fighters[1];
    bowserThrowTester.grabbedFighter = 1;
    peachThrownTester.grabberFighter = 0;
    pf::changeFighterState(bowserPeachThrowWorld, bowserThrowTester, "ThrowLw");

    pf::World bowserMarioThrowWorld = pf::makeTrainingWorld(kBowserRosterIndex, kMarioRosterIndex);
    pf::FighterRuntime& bowserMarioThrowTester = bowserMarioThrowWorld.fighters[0];
    pf::FighterRuntime& marioThrownTester = bowserMarioThrowWorld.fighters[1];
    bowserMarioThrowTester.grabbedFighter = 1;
    marioThrownTester.grabberFighter = 0;
    pf::changeFighterState(bowserMarioThrowWorld, bowserMarioThrowTester, "ThrowLw");
    std::cout << "bowser_peach_down_throw_victim_state=" << pf::currentState(bowserPeachThrowWorld, peachThrownTester).name
              << " bowser_mario_down_throw_victim_state=" << pf::currentState(bowserMarioThrowWorld, marioThrownTester).name
              << "\n";

    pf::World bowserZeldaThrowWorld = pf::makeTrainingWorld(kBowserRosterIndex, kZeldaRosterIndex);
    pf::FighterRuntime& bowserZeldaThrowTester = bowserZeldaThrowWorld.fighters[0];
    pf::FighterRuntime& zeldaThrownTester = bowserZeldaThrowWorld.fighters[1];
    bowserZeldaThrowTester.grabbedFighter = 1;
    zeldaThrownTester.grabberFighter = 0;
    pf::changeFighterState(bowserZeldaThrowWorld, bowserZeldaThrowTester, "ThrowLw");

    pf::World gigaBowserPeachThrowWorld = pf::makeTrainingWorld(kBowserRosterIndex, kPeachRosterIndex);
    gigaBowserPeachThrowWorld.fighterDefs[kBowserRosterIndex].name = "Giga Bowser";
    pf::FighterRuntime& gigaBowserThrowTester = gigaBowserPeachThrowWorld.fighters[0];
    pf::FighterRuntime& gigaPeachThrownTester = gigaBowserPeachThrowWorld.fighters[1];
    gigaBowserThrowTester.grabbedFighter = 1;
    gigaPeachThrownTester.grabberFighter = 0;
    pf::changeFighterState(gigaBowserPeachThrowWorld, gigaBowserThrowTester, "ThrowLw");
    std::cout << "bowser_zelda_down_throw_victim_state=" << pf::currentState(bowserZeldaThrowWorld, zeldaThrownTester).name
              << " giga_bowser_peach_down_throw_victim_state=" << pf::currentState(gigaBowserPeachThrowWorld, gigaPeachThrownTester).name
              << "\n";

    pf::World throwLwReleaseAngleWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& throwLwReleaseGrabber = throwLwReleaseAngleWorld.fighters[0];
    pf::FighterRuntime& throwLwReleaseVictim = throwLwReleaseAngleWorld.fighters[1];
    throwLwReleaseGrabber.grabbedFighter = 1;
    throwLwReleaseVictim.grabberFighter = 0;
    pf::changeFighterState(throwLwReleaseAngleWorld, throwLwReleaseGrabber, "ThrowLw");
    throwLwReleaseGrabber.lastActionFrameExecuted = 0;
    pf::HitboxDefinition throwLwHitbox;
    throwLwHitbox.damage = pf::fx(5);
    throwLwHitbox.knockbackAngleDegrees = pf::fx(45);
    throwLwHitbox.knockbackBase = pf::fx(100);
    throwLwHitbox.knockbackGrowth = 0;
    throwLwReleaseGrabber.throwHitboxes[0] = throwLwHitbox;
    throwLwReleaseGrabber.throwHitboxActive[0] = true;
    pf::setFighterThrowFlag(throwLwReleaseGrabber, 3, true);
    pf::tickWorld(throwLwReleaseAngleWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "throw_lw_release_state=" << pf::currentState(throwLwReleaseAngleWorld, throwLwReleaseVictim).name
              << " throw_lw_release_angle=" << pf::fxToFloat(throwLwReleaseVictim.damageLaunchAngle)
              << "\n";

    auto prepareThrownHitboxClearProbe = [](pf::World& probeWorld, pf::Fix knockbackSpeed) -> pf::FighterRuntime& {
        pf::FighterRuntime& victim = probeWorld.fighters[1];
        probeWorld.fighters[0].position = {-pf::fx(20), 0};
        victim.position = {pf::fx(20), pf::fx(5)};
        victim.previousPosition = victim.position;
        victim.grounded = false;
        victim.groundSegment = -1;
        victim.hitstun = 5;
        victim.knockbackVelocity.x = knockbackSpeed;
        victim.thrownHitboxOwner = 0;
        pf::changeFighterState(probeWorld, victim, "DamageFlyN");
        victim.thrownHitboxOwner = 0;
        victim.hitstun = 5;
        victim.knockbackVelocity.x = knockbackSpeed;
        pf::ActiveHitbox active;
        active.def.requiresThrownHitboxOwner = true;
        active.def.damage = pf::fx(3);
        active.def.radius = pf::fx(1);
        victim.activeHitboxes.push_back(active);
        return victim;
    };
    pf::World thrownSlowHitboxWorld = pf::makeTrainingWorld();
    const pf::Fix thrownClearVelocity = thrownSlowHitboxWorld.fighterDefs[0].properties.common.thrownHitboxClearVelocityX1C8;
    pf::FighterRuntime& thrownSlowHitboxTester = prepareThrownHitboxClearProbe(thrownSlowHitboxWorld, pf::fxDiv(thrownClearVelocity, pf::fx(2)));
    pf::tickWorld(thrownSlowHitboxWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World thrownFastHitboxWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& thrownFastHitboxTester = prepareThrownHitboxClearProbe(thrownFastHitboxWorld, thrownClearVelocity + thrownClearVelocity);
    pf::tickWorld(thrownFastHitboxWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "thrown_slow_hitboxes=" << thrownSlowHitboxTester.activeHitboxes.size()
              << " thrown_fast_hitboxes=" << thrownFastHitboxTester.activeHitboxes.size()
              << " thrown_clear_velocity=" << pf::fxToFloat(thrownClearVelocity)
              << "\n";

    pf::World cargoThrownWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cargoThrownTester = cargoThrownWorld.fighters[1];
    cargoThrownTester.grabberFighter = 0;
    cargoThrownWorld.fighters[0].grabbedFighter = 1;
    pf::changeFighterState(cargoThrownWorld, cargoThrownTester, "ThrownFF");
    const std::string cargoThrownFState = pf::currentState(cargoThrownWorld, cargoThrownTester).name;
    pf::changeFighterState(cargoThrownWorld, cargoThrownTester, "ThrownFB");
    const std::string cargoThrownBState = pf::currentState(cargoThrownWorld, cargoThrownTester).name;
    pf::changeFighterState(cargoThrownWorld, cargoThrownTester, "ThrownFHi");
    const std::string cargoThrownHiState = pf::currentState(cargoThrownWorld, cargoThrownTester).name;
    pf::changeFighterState(cargoThrownWorld, cargoThrownTester, "ThrownFLw");
    const std::string cargoThrownLwState = pf::currentState(cargoThrownWorld, cargoThrownTester).name;
    pf::changeFighterState(cargoThrownWorld, cargoThrownTester, "Fall");
    std::cout << "cargo_thrown_states=" << cargoThrownFState
              << "," << cargoThrownBState
              << "," << cargoThrownHiState
              << "," << cargoThrownLwState
              << " cargo_cleanup_grabber_link=" << cargoThrownWorld.fighters[0].grabbedFighter
              << " cargo_cleanup_victim_link=" << cargoThrownTester.grabberFighter
              << "\n";

    pf::World mewtwoCaptureWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& mewtwoCaptureOwner = mewtwoCaptureWorld.fighters[0];
    pf::FighterRuntime& mewtwoThrownVictim = mewtwoCaptureWorld.fighters[1];
    mewtwoCaptureOwner.grabbedFighter = 1;
    mewtwoThrownVictim.grabberFighter = 0;
    mewtwoThrownVictim.grabTimer = pf::fx(7);
    pf::changeFighterState(mewtwoCaptureWorld, mewtwoCaptureOwner, "CaptureMewtwo");
    pf::changeFighterState(mewtwoCaptureWorld, mewtwoThrownVictim, "ThrownMewtwo");
    pf::tickWorld(mewtwoCaptureWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const std::string mewtwoThrownState = pf::currentState(mewtwoCaptureWorld, mewtwoThrownVictim).name;
    const float mewtwoThrownTimer = pf::fxToFloat(mewtwoThrownVictim.grabTimer);
    pf::changeFighterState(mewtwoCaptureWorld, mewtwoThrownVictim, "Fall");

    pf::World mewtwoAirCaptureWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& mewtwoAirCaptureOwner = mewtwoAirCaptureWorld.fighters[0];
    pf::FighterRuntime& mewtwoAirThrownVictim = mewtwoAirCaptureWorld.fighters[1];
    mewtwoAirCaptureOwner.grabbedFighter = 1;
    mewtwoAirThrownVictim.grabberFighter = 0;
    pf::changeFighterState(mewtwoAirCaptureWorld, mewtwoAirCaptureOwner, "CaptureMewtwoAir");
    pf::changeFighterState(mewtwoAirCaptureWorld, mewtwoAirThrownVictim, "ThrownMewtwoAir");
    pf::changeFighterState(mewtwoAirCaptureWorld, mewtwoAirCaptureOwner, "Fall");
    std::cout << "mewtwo_thrown_state=" << mewtwoThrownState
              << " mewtwo_thrown_timer=" << mewtwoThrownTimer
              << " mewtwo_cleanup_grabber_link=" << mewtwoCaptureOwner.grabbedFighter
              << " mewtwo_cleanup_victim_link=" << mewtwoThrownVictim.grabberFighter
              << " mewtwo_air_cleanup_grabber_link=" << mewtwoAirCaptureOwner.grabbedFighter
              << " mewtwo_air_cleanup_victim_link=" << mewtwoAirThrownVictim.grabberFighter
              << "\n";

    pf::World yoshiCaptureWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& yoshiCaptureOwner = yoshiCaptureWorld.fighters[0];
    pf::FighterRuntime& yoshiCaptureVictim = yoshiCaptureWorld.fighters[1];
    yoshiCaptureOwner.grabbedFighter = 1;
    yoshiCaptureVictim.grabberFighter = 0;
    yoshiCaptureVictim.grabTimer = pf::fx(9);
    pf::changeFighterState(yoshiCaptureWorld, yoshiCaptureVictim, "CaptureYoshi");
    pf::tickWorld(yoshiCaptureWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const std::string yoshiCaptureState = pf::currentState(yoshiCaptureWorld, yoshiCaptureVictim).name;
    const float yoshiCaptureTimer = pf::fxToFloat(yoshiCaptureVictim.grabTimer);
    pf::changeFighterState(yoshiCaptureWorld, yoshiCaptureVictim, "Fall");
    std::cout << "yoshi_capture_state=" << yoshiCaptureState
              << " yoshi_capture_timer=" << yoshiCaptureTimer
              << " yoshi_capture_cleanup_grabber_link=" << yoshiCaptureOwner.grabbedFighter
              << " yoshi_capture_cleanup_victim_link=" << yoshiCaptureVictim.grabberFighter
              << "\n";

    pf::World neckFootCaptureWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& neckFootCaptureOwner = neckFootCaptureWorld.fighters[0];
    pf::FighterRuntime& neckFootCaptureVictim = neckFootCaptureWorld.fighters[1];
    neckFootCaptureOwner.grabbedFighter = 1;
    neckFootCaptureVictim.grabberFighter = 0;
    pf::changeFighterState(neckFootCaptureWorld, neckFootCaptureVictim, "CaptureNeck");
    const std::string captureNeckState = pf::currentState(neckFootCaptureWorld, neckFootCaptureVictim).name;
    pf::changeFighterState(neckFootCaptureWorld, neckFootCaptureVictim, "CaptureFoot");
    const std::string captureFootState = pf::currentState(neckFootCaptureWorld, neckFootCaptureVictim).name;
    pf::changeFighterState(neckFootCaptureWorld, neckFootCaptureVictim, "Fall");
    std::cout << "capture_neck_foot_states=" << captureNeckState
              << "," << captureFootState
              << " capture_neck_foot_cleanup_grabber_link=" << neckFootCaptureOwner.grabbedFighter
              << " capture_neck_foot_cleanup_victim_link=" << neckFootCaptureVictim.grabberFighter
              << "\n";

    pf::World captainCaptureWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& captainCaptureOwner = captainCaptureWorld.fighters[0];
    pf::FighterRuntime& captainCaptureVictim = captainCaptureWorld.fighters[1];
    captainCaptureOwner.grabbedFighter = 1;
    captainCaptureVictim.grabberFighter = 0;
    pf::changeFighterState(captainCaptureWorld, captainCaptureOwner, "CaptureCaptain");
    const std::string captainCaptureState = pf::currentState(captainCaptureWorld, captainCaptureOwner).name;
    pf::changeFighterState(captainCaptureWorld, captainCaptureOwner, "Fall");
    std::cout << "captain_capture_state=" << captainCaptureState
              << " captain_capture_cleanup_grabber_link=" << captainCaptureOwner.grabbedFighter
              << " captain_capture_cleanup_victim_link=" << captainCaptureVictim.grabberFighter
              << "\n";

    pf::World koopaThrownWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& koopaThrownOwner = koopaThrownWorld.fighters[0];
    pf::FighterRuntime& koopaThrownVictim = koopaThrownWorld.fighters[1];
    koopaThrownOwner.grabbedFighter = 1;
    koopaThrownVictim.grabberFighter = 0;
    koopaThrownVictim.grabTimer = pf::fx(11);
    pf::changeFighterState(koopaThrownWorld, koopaThrownVictim, "ThrownKoopaF");
    const std::string koopaThrownFState = pf::currentState(koopaThrownWorld, koopaThrownVictim).name;
    pf::changeFighterState(koopaThrownWorld, koopaThrownVictim, "ThrownKoopaB");
    const std::string koopaThrownBState = pf::currentState(koopaThrownWorld, koopaThrownVictim).name;
    pf::changeFighterState(koopaThrownWorld, koopaThrownVictim, "ThrownKoopaAirF");
    const std::string koopaThrownAirFState = pf::currentState(koopaThrownWorld, koopaThrownVictim).name;
    pf::changeFighterState(koopaThrownWorld, koopaThrownVictim, "ThrownKoopaAirB");
    const std::string koopaThrownAirBState = pf::currentState(koopaThrownWorld, koopaThrownVictim).name;
    pf::tickWorld(koopaThrownWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const float koopaThrownTimer = pf::fxToFloat(koopaThrownVictim.grabTimer);
    pf::changeFighterState(koopaThrownWorld, koopaThrownVictim, "Fall");
    std::cout << "koopa_thrown_states=" << koopaThrownFState
              << "," << koopaThrownBState
              << "," << koopaThrownAirFState
              << "," << koopaThrownAirBState
              << " koopa_thrown_timer=" << koopaThrownTimer
              << " koopa_cleanup_grabber_link=" << koopaThrownOwner.grabbedFighter
              << " koopa_cleanup_victim_link=" << koopaThrownVictim.grabberFighter
              << "\n";

    pf::World koopaCaptureWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& koopaCaptureOwner = koopaCaptureWorld.fighters[0];
    pf::FighterRuntime& koopaCaptureVictim = koopaCaptureWorld.fighters[1];
    koopaCaptureOwner.grabbedFighter = 1;
    koopaCaptureVictim.grabberFighter = 0;
    pf::changeFighterState(koopaCaptureWorld, koopaCaptureOwner, "CaptureKoopa");
    const std::string koopaCaptureState = pf::currentState(koopaCaptureWorld, koopaCaptureOwner).name;
    pf::changeFighterState(koopaCaptureWorld, koopaCaptureOwner, "CaptureKoopaAir");
    const std::string koopaCaptureAirState = pf::currentState(koopaCaptureWorld, koopaCaptureOwner).name;
    pf::changeFighterState(koopaCaptureWorld, koopaCaptureOwner, "Fall");
    std::cout << "koopa_capture_states=" << koopaCaptureState
              << "," << koopaCaptureAirState
              << " koopa_capture_cleanup_grabber_link=" << koopaCaptureOwner.grabbedFighter
              << " koopa_capture_cleanup_victim_link=" << koopaCaptureVictim.grabberFighter
              << "\n";

    pf::World thrownWomenCleanupWorld = pf::makeTrainingWorld(2, 4);
    pf::FighterRuntime& thrownWomenCleanupGrabber = thrownWomenCleanupWorld.fighters[0];
    pf::FighterRuntime& thrownWomenCleanupVictim = thrownWomenCleanupWorld.fighters[1];
    thrownWomenCleanupGrabber.grabbedFighter = 1;
    thrownWomenCleanupVictim.grabberFighter = 0;
    pf::changeFighterState(thrownWomenCleanupWorld, thrownWomenCleanupGrabber, "ThrowLw");
    pf::changeFighterState(thrownWomenCleanupWorld, thrownWomenCleanupVictim, "Fall");
    std::cout << "thrown_lw_women_cleanup_grabber_link=" << thrownWomenCleanupGrabber.grabbedFighter
              << " thrown_lw_women_cleanup_victim_link=" << thrownWomenCleanupVictim.grabberFighter
              << "\n";

    pf::World thrownEscapeWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& thrownEscapeGrabber = thrownEscapeWorld.fighters[0];
    pf::FighterRuntime& thrownEscapeVictim = thrownEscapeWorld.fighters[1];
    thrownEscapeGrabber.grabbedFighter = 1;
    thrownEscapeVictim.grabberFighter = 0;
    thrownEscapeVictim.grabTimer = pf::fx(1);
    pf::changeFighterState(thrownEscapeWorld, thrownEscapeGrabber, "ThrowF");
    pf::tickWorld(thrownEscapeWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "throw_timer_escape_blocked_grabber_state=" << pf::currentState(thrownEscapeWorld, thrownEscapeGrabber).name
              << " throw_timer_escape_blocked_victim_state=" << pf::currentState(thrownEscapeWorld, thrownEscapeVictim).name
              << " throw_timer_escape_blocked_timer=" << pf::fxToFloat(thrownEscapeVictim.grabTimer)
              << "\n";

    pf::World kirbyThrowEscapeWorld = pf::makeTrainingWorld();
    kirbyThrowEscapeWorld.fighterDefs[0].name = "Kirby";
    pf::FighterRuntime& kirbyThrowEscapeGrabber = kirbyThrowEscapeWorld.fighters[0];
    pf::FighterRuntime& kirbyThrowEscapeVictim = kirbyThrowEscapeWorld.fighters[1];
    kirbyThrowEscapeGrabber.grabbedFighter = 1;
    kirbyThrowEscapeVictim.grabberFighter = 0;
    kirbyThrowEscapeVictim.grabTimer = pf::fx(1);
    pf::changeFighterState(kirbyThrowEscapeWorld, kirbyThrowEscapeGrabber, "ThrowF");
    pf::tickWorld(kirbyThrowEscapeWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "kirby_throw_timer_escape_grabber_state=" << pf::currentState(kirbyThrowEscapeWorld, kirbyThrowEscapeGrabber).name
              << " kirby_throw_timer_escape_victim_state=" << pf::currentState(kirbyThrowEscapeWorld, kirbyThrowEscapeVictim).name
              << "\n";

    pf::World catchCutFrictionWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& catchCutFrictionTester = catchCutFrictionWorld.fighters[0];
    pf::changeFighterState(catchCutFrictionWorld, catchCutFrictionTester, "CatchCut");
    catchCutFrictionTester.groundVelocity = pf::fx(1);
    pf::tickWorld(catchCutFrictionWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World captureCutFrictionWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& captureCutFrictionTester = captureCutFrictionWorld.fighters[1];
    pf::changeFighterState(captureCutFrictionWorld, captureCutFrictionTester, "CaptureCut");
    captureCutFrictionTester.groundVelocity = pf::fx(1);
    pf::tickWorld(captureCutFrictionWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World catchCutFastfallWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& catchCutFastfallTester = catchCutFastfallWorld.fighters[0];
    catchCutFastfallTester.position.y = pf::fx(6);
    catchCutFastfallTester.previousPosition = catchCutFastfallTester.position;
    catchCutFastfallTester.grounded = false;
    catchCutFastfallTester.groundSegment = -1;
    catchCutFastfallTester.fighterVelocity.y = -pf::fxFromFloat(0.1f);
    pf::changeFighterState(catchCutFastfallWorld, catchCutFastfallTester, "CatchCut");
    pf::InputFrame cutFastfallInput;
    cutFastfallInput.move.y = -pf::fx(1);
    pf::tickWorld(catchCutFastfallWorld, {cutFastfallInput, pf::InputFrame{}});

    pf::World captureCutFastfallWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& captureCutFastfallTester = captureCutFastfallWorld.fighters[1];
    captureCutFastfallTester.position.y = pf::fx(6);
    captureCutFastfallTester.previousPosition = captureCutFastfallTester.position;
    captureCutFastfallTester.grounded = false;
    captureCutFastfallTester.groundSegment = -1;
    captureCutFastfallTester.fighterVelocity.y = -pf::fxFromFloat(0.1f);
    pf::changeFighterState(captureCutFastfallWorld, captureCutFastfallTester, "CaptureCut");
    pf::tickWorld(captureCutFastfallWorld, {pf::InputFrame{}, cutFastfallInput});

    std::cout << "cut_friction catch=" << pf::fxToFloat(catchCutFrictionTester.groundVelocity)
              << " capture=" << pf::fxToFloat(captureCutFrictionTester.groundVelocity)
              << " catch_scale=" << pf::fxToFloat(catchCutFrictionWorld.fighterDefs[0].properties.common.catchCutFrictionScaleX64)
              << " capture_scale=" << pf::fxToFloat(captureCutFrictionWorld.fighterDefs[1].properties.common.captureCutFrictionScaleX36C)
              << " catch_fastfall=" << pf::fighterFlag(catchCutFastfallTester, 12)
              << " capture_fastfall=" << pf::fighterFlag(captureCutFastfallTester, 12)
              << "\n";

    pf::World catchCutRunoffWorld = pf::makeTrainingWorld();
    catchCutRunoffWorld.stage.segments = {
        {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    catchCutRunoffWorld.stage.ledges.clear();
    pf::FighterRuntime& catchCutRunoffTester = catchCutRunoffWorld.fighters[0];
    catchCutRunoffTester.position = {pf::fxFromFloat(0.99f), 0};
    catchCutRunoffTester.previousPosition = catchCutRunoffTester.position;
    catchCutRunoffTester.grounded = true;
    catchCutRunoffTester.groundSegment = 0;
    pf::changeFighterState(catchCutRunoffWorld, catchCutRunoffTester, "CatchCut");
    catchCutRunoffTester.groundVelocity = pf::fxFromFloat(0.2f);
    pf::tickWorld(catchCutRunoffWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World captureCutRunoffWorld = pf::makeTrainingWorld();
    captureCutRunoffWorld.stage.segments = {
        {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    captureCutRunoffWorld.stage.ledges.clear();
    pf::FighterRuntime& captureCutRunoffTester = captureCutRunoffWorld.fighters[1];
    captureCutRunoffTester.position = {pf::fxFromFloat(0.99f), 0};
    captureCutRunoffTester.previousPosition = captureCutRunoffTester.position;
    captureCutRunoffTester.grounded = true;
    captureCutRunoffTester.groundSegment = 0;
    pf::changeFighterState(captureCutRunoffWorld, captureCutRunoffTester, "CaptureCut");
    captureCutRunoffTester.groundVelocity = pf::fxFromFloat(0.2f);
    pf::tickWorld(captureCutRunoffWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "cut_runoff catch_state=" << pf::currentState(catchCutRunoffWorld, catchCutRunoffTester).name
              << " catch_grounded=" << catchCutRunoffTester.grounded
              << " capture_state=" << pf::currentState(captureCutRunoffWorld, captureCutRunoffTester).name
              << " capture_grounded=" << captureCutRunoffTester.grounded
              << " capture_ground_vel=" << pf::fxToFloat(captureCutRunoffTester.groundVelocity)
              << " capture_jumps=" << captureCutRunoffTester.jumpsUsed
              << " capture_ecb=" << captureCutRunoffTester.ecbLockTimer
              << "\n";

    pf::World cutFinishGroundWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cutFinishGroundTester = cutFinishGroundWorld.fighters[0];
    pf::changeFighterState(cutFinishGroundWorld, cutFinishGroundTester, "CatchCut");
    for (int frame = 0; frame < 32; ++frame) {
        pf::tickWorld(cutFinishGroundWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }

    pf::World cutFinishAirWorld = pf::makeTrainingWorld();
    cutFinishAirWorld.stage.segments = {
        {{-pf::fx(20), -pf::fx(200)}, {pf::fx(20), -pf::fx(200)}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    cutFinishAirWorld.stage.ledges.clear();
    pf::FighterRuntime& cutFinishAirTester = cutFinishAirWorld.fighters[1];
    cutFinishAirTester.position.y = pf::fx(8);
    cutFinishAirTester.previousPosition = cutFinishAirTester.position;
    cutFinishAirTester.grounded = false;
    cutFinishAirTester.groundSegment = -1;
    pf::changeFighterState(cutFinishAirWorld, cutFinishAirTester, "CaptureCut");
    for (int frame = 0; frame < 32; ++frame) {
        pf::tickWorld(cutFinishAirWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "cut_finish_ground_state=" << pf::currentState(cutFinishGroundWorld, cutFinishGroundTester).name
              << " cut_finish_air_state=" << pf::currentState(cutFinishAirWorld, cutFinishAirTester).name
              << "\n";

    auto addDamageBindHitbox = [](pf::FighterRuntime& attacker) {
        pf::ActiveHitbox active;
        active.def.element = 12;
        active.def.damage = pf::fx(5);
        active.def.radius = pf::fx(8);
        active.def.knockbackAngleDegrees = pf::fx(45);
        active.def.knockbackBase = pf::fx(30);
        active.def.knockbackGrowth = 0;
        attacker.activeHitboxes.push_back(active);
    };

    pf::World damageBindRouteWorld = pf::makeTrainingWorld();
    damageBindRouteWorld.fighters[0].position = {0, 0};
    damageBindRouteWorld.fighters[1].position = {0, 0};
    damageBindRouteWorld.fighters[1].percent = pf::fx(10);
    addDamageBindHitbox(damageBindRouteWorld.fighters[0]);
    pf::tickWorld(damageBindRouteWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const float damageBindRouteTimer = pf::fxToFloat(damageBindRouteWorld.fighters[1].grabTimer);

    pf::World damageBindAirRouteWorld = pf::makeTrainingWorld();
    damageBindAirRouteWorld.fighters[0].position = {0, pf::fx(5)};
    damageBindAirRouteWorld.fighters[1].position = {0, pf::fx(5)};
    damageBindAirRouteWorld.fighters[1].grounded = false;
    damageBindAirRouteWorld.fighters[1].groundSegment = -1;
    addDamageBindHitbox(damageBindAirRouteWorld.fighters[0]);
    pf::tickWorld(damageBindAirRouteWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World damageBindMashWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& damageBindMashTester = damageBindMashWorld.fighters[0];
    pf::changeFighterState(damageBindMashWorld, damageBindMashTester, "DamageBind");
    damageBindMashTester.grabTimer = pf::fx(20);
    pf::InputFrame damageBindMashInput;
    damageBindMashInput.buttons = pf::ButtonSpecial;
    pf::tickWorld(damageBindMashWorld, {damageBindMashInput, pf::InputFrame{}});

    pf::World damageBindReleaseWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& damageBindReleaseTester = damageBindReleaseWorld.fighters[0];
    pf::changeFighterState(damageBindReleaseWorld, damageBindReleaseTester, "DamageBind");
    damageBindReleaseTester.grabTimer = damageBindReleaseWorld.fighterDefs[0].properties.common.damageBindTimerDecrementX670;
    pf::tickWorld(damageBindReleaseWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World damageBindTeeterWorld = pf::makeTrainingWorld();
    damageBindTeeterWorld.stage.segments = {
        {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    damageBindTeeterWorld.stage.ledges.clear();
    pf::FighterRuntime& damageBindTeeterTester = damageBindTeeterWorld.fighters[0];
    damageBindTeeterTester.position = {pf::fxFromFloat(0.95f), 0};
    damageBindTeeterTester.previousPosition = damageBindTeeterTester.position;
    damageBindTeeterTester.grounded = true;
    damageBindTeeterTester.groundSegment = 0;
    damageBindTeeterTester.groundVelocity = pf::fxFromFloat(0.2f);
    pf::changeFighterState(damageBindTeeterWorld, damageBindTeeterTester, "DamageBind");
    damageBindTeeterTester.grabTimer = pf::fx(100);
    pf::tickWorld(damageBindTeeterWorld, {pf::InputFrame{}, pf::InputFrame{}});

    std::cout << "damage_bind_route_state=" << pf::currentState(damageBindRouteWorld, damageBindRouteWorld.fighters[1]).name
              << " damage_bind_route_timer=" << damageBindRouteTimer
              << " damage_bind_air_route_state=" << pf::currentState(damageBindAirRouteWorld, damageBindAirRouteWorld.fighters[1]).name
              << " damage_bind_mash_timer=" << pf::fxToFloat(damageBindMashTester.grabTimer)
              << " damage_bind_release_state=" << pf::currentState(damageBindReleaseWorld, damageBindReleaseTester).name
              << " damage_bind_teeter_state=" << pf::currentState(damageBindTeeterWorld, damageBindTeeterTester).name
              << "\n";

    auto addDamageScrewHitbox = [](pf::FighterRuntime& attacker) {
        pf::ActiveHitbox active;
        active.def.element = 14;
        active.def.damage = pf::fx(5);
        active.def.radius = pf::fx(8);
        active.def.knockbackAngleDegrees = pf::fx(80);
        active.def.knockbackBase = pf::fx(30);
        active.def.knockbackGrowth = 0;
        attacker.activeHitboxes.push_back(active);
    };

    pf::World damageScrewRouteWorld = pf::makeTrainingWorld();
    damageScrewRouteWorld.fighters[0].position = {0, 0};
    damageScrewRouteWorld.fighters[1].position = {0, 0};
    addDamageScrewHitbox(damageScrewRouteWorld.fighters[0]);
    pf::tickWorld(damageScrewRouteWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const std::string damageScrewRouteState = pf::currentState(damageScrewRouteWorld, damageScrewRouteWorld.fighters[1]).name;
    const float damageScrewRouteVelY = pf::fxToFloat(damageScrewRouteWorld.fighters[1].fighterVelocity.y);

    pf::World damageScrewAirRouteWorld = pf::makeTrainingWorld();
    damageScrewAirRouteWorld.fighters[0].position = {0, pf::fx(5)};
    damageScrewAirRouteWorld.fighters[1].position = {0, pf::fx(5)};
    damageScrewAirRouteWorld.fighters[1].grounded = false;
    damageScrewAirRouteWorld.fighters[1].groundSegment = -1;
    addDamageScrewHitbox(damageScrewAirRouteWorld.fighters[0]);
    pf::tickWorld(damageScrewAirRouteWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World damageScrewFinishWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& damageScrewFinishTester = damageScrewFinishWorld.fighters[0];
    damageScrewFinishTester.position.y = pf::fx(40);
    damageScrewFinishTester.previousPosition = damageScrewFinishTester.position;
    damageScrewFinishTester.grounded = false;
    damageScrewFinishTester.groundSegment = -1;
    pf::changeFighterState(damageScrewFinishWorld, damageScrewFinishTester, "DamageScrewAir");
    damageScrewFinishTester.lastStateChangeFrame = damageScrewFinishTester.internalFrame - 1000;
    pf::tickWorld(damageScrewFinishWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World damageScrewFloorWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& damageScrewFloorTester = damageScrewFloorWorld.fighters[0];
    damageScrewFloorTester.position.y = pf::fx(3);
    damageScrewFloorTester.previousPosition = damageScrewFloorTester.position;
    damageScrewFloorTester.grounded = false;
    damageScrewFloorTester.groundSegment = -1;
    damageScrewFloorTester.fighterVelocity.y = -pf::fxFromFloat(1.0f);
    pf::changeFighterState(damageScrewFloorWorld, damageScrewFloorTester, "DamageScrewAir");
    for (int frame = 0; frame < 12 && !damageScrewFloorTester.grounded; ++frame) {
        pf::tickWorld(damageScrewFloorWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }

    pf::World damageScrewLandingWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& damageScrewLandingTester = damageScrewLandingWorld.fighters[0];
    damageScrewLandingTester.position.y = pf::fx(3);
    damageScrewLandingTester.previousPosition = damageScrewLandingTester.position;
    damageScrewLandingTester.grounded = false;
    damageScrewLandingTester.groundSegment = -1;
    damageScrewLandingTester.fighterVelocity.y = -pf::fxFromFloat(1.0f);
    pf::changeFighterState(damageScrewLandingWorld, damageScrewLandingTester, "DamageScrewAir");
    damageScrewLandingTester.lastStateChangeFrame = damageScrewLandingTester.internalFrame - 1000;
    for (int frame = 0; frame < 12 && pf::currentState(damageScrewLandingWorld, damageScrewLandingTester).name.find("LandingFallSpecial") == std::string::npos; ++frame) {
        pf::tickWorld(damageScrewLandingWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    const std::string damageScrewLandingState = pf::currentState(damageScrewLandingWorld, damageScrewLandingTester).name;
    for (int frame = 0; frame < damageScrewLandingWorld.fighterDefs[0].properties.normalLandingLag; ++frame) {
        pf::tickWorld(damageScrewLandingWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    pf::InputFrame damageScrewLandingJumpInput;
    damageScrewLandingJumpInput.buttons = pf::ButtonJump;
    pf::tickWorld(damageScrewLandingWorld, {damageScrewLandingJumpInput, pf::InputFrame{}});

    pf::World damageScrewSoftLandingWorld = pf::makeTrainingWorld();
    damageScrewSoftLandingWorld.fighterDefs[0].properties.noImpactLandingVelocity = -pf::fx(1);
    pf::FighterRuntime& damageScrewSoftLandingTester = damageScrewSoftLandingWorld.fighters[0];
    damageScrewSoftLandingTester.position.y = pf::fxFromFloat(0.2f);
    damageScrewSoftLandingTester.previousPosition = damageScrewSoftLandingTester.position;
    damageScrewSoftLandingTester.grounded = false;
    damageScrewSoftLandingTester.groundSegment = -1;
    damageScrewSoftLandingTester.fighterVelocity.y = 0;
    const pf::Fix damageScrewSoftFloorY =
        damageScrewSoftLandingTester.position.y + damageScrewSoftLandingTester.ecb.points[3].y - pf::fxFromFloat(0.05f);
    damageScrewSoftLandingWorld.stage.segments = {
        {{-pf::fx(100), damageScrewSoftFloorY}, {pf::fx(100), damageScrewSoftFloorY}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    damageScrewSoftLandingWorld.stage.ledges.clear();
    damageScrewSoftLandingTester.pendingFallSpecialLandingLag = damageScrewSoftLandingWorld.fighterDefs[0].properties.normalLandingLag;
    damageScrewSoftLandingTester.pendingFallSpecialLandingInterruptible = true;
    damageScrewSoftLandingTester.pendingFallSpecialForceLanding = false;
    pf::changeFighterState(damageScrewSoftLandingWorld, damageScrewSoftLandingTester, "FallSpecial");
    for (int frame = 0; frame < 8 && pf::currentState(damageScrewSoftLandingWorld, damageScrewSoftLandingTester).name != "Wait"; ++frame) {
        pf::tickWorld(damageScrewSoftLandingWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "damage_screw_route_state=" << damageScrewRouteState
              << " damage_screw_route_vel_y=" << damageScrewRouteVelY
              << " damage_screw_air_route_state=" << pf::currentState(damageScrewAirRouteWorld, damageScrewAirRouteWorld.fighters[1]).name
              << " damage_screw_finish_state=" << pf::currentState(damageScrewFinishWorld, damageScrewFinishTester).name
              << " damage_screw_freefall_limit=" << damageScrewFinishTester.fallSpecialLimitDrift
              << " damage_screw_freefall_force=" << damageScrewFinishTester.fallSpecialForceLanding
              << " damage_screw_freefall_interruptible=" << damageScrewFinishTester.fallSpecialLandingInterruptible
              << " damage_screw_floor_state=" << pf::currentState(damageScrewFloorWorld, damageScrewFloorTester).name
              << " damage_screw_landing_state=" << damageScrewLandingState
              << " damage_screw_landing_jump_state=" << pf::currentState(damageScrewLandingWorld, damageScrewLandingTester).name
              << " damage_screw_soft_landing_state=" << pf::currentState(damageScrewSoftLandingWorld, damageScrewSoftLandingTester).name
              << " damage_screw_soft_landing_vel=" << pf::fxToFloat(damageScrewSoftLandingTester.lastLandingVelocityY)
              << " damage_screw_soft_no_impact=" << pf::fxToFloat(damageScrewSoftLandingWorld.fighterDefs[0].properties.noImpactLandingVelocity)
              << "\n";

    auto addDamageSongHitbox = [](pf::FighterRuntime& attacker, int element) {
        pf::ActiveHitbox active;
        active.def.element = element;
        active.def.damage = pf::fx(5);
        active.def.radius = pf::fx(8);
        active.def.knockbackAngleDegrees = pf::fx(45);
        active.def.knockbackBase = pf::fx(30);
        active.def.knockbackGrowth = 0;
        attacker.activeHitboxes.push_back(active);
    };

    pf::World damageSongRouteWorld = pf::makeTrainingWorld();
    damageSongRouteWorld.fighters[0].position = {0, 0};
    damageSongRouteWorld.fighters[1].position = {0, 0};
    damageSongRouteWorld.fighters[1].percent = pf::fx(10);
    addDamageSongHitbox(damageSongRouteWorld.fighters[0], 6);
    pf::tickWorld(damageSongRouteWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const float damageSongRouteTimer = pf::fxToFloat(damageSongRouteWorld.fighters[1].grabTimer);

    pf::World damageSongStrongWorld = pf::makeTrainingWorld();
    damageSongStrongWorld.fighters[0].position = {0, 0};
    damageSongStrongWorld.fighters[1].position = {0, 0};
    damageSongStrongWorld.fighters[1].percent = pf::fx(10);
    addDamageSongHitbox(damageSongStrongWorld.fighters[0], 7);
    pf::tickWorld(damageSongStrongWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const float damageSongStrongTimer = pf::fxToFloat(damageSongStrongWorld.fighters[1].grabTimer);

    pf::World damageSongMashWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& damageSongMashTester = damageSongMashWorld.fighters[0];
    pf::changeFighterState(damageSongMashWorld, damageSongMashTester, "DamageSongWait");
    damageSongMashTester.grabTimer = pf::fx(20);
    pf::InputFrame damageSongMashInput;
    damageSongMashInput.buttons = pf::ButtonSpecial;
    pf::tickWorld(damageSongMashWorld, {damageSongMashInput, pf::InputFrame{}});

    pf::World damageSongWaitWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& damageSongWaitTester = damageSongWaitWorld.fighters[0];
    pf::changeFighterState(damageSongWaitWorld, damageSongWaitTester, "DamageSong");
    damageSongWaitTester.grabTimer = pf::fx(100);
    damageSongWaitTester.lastStateChangeFrame = damageSongWaitTester.internalFrame - 1000;
    pf::tickWorld(damageSongWaitWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World damageSongReleaseWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& damageSongReleaseTester = damageSongReleaseWorld.fighters[0];
    pf::changeFighterState(damageSongReleaseWorld, damageSongReleaseTester, "DamageSongWait");
    damageSongReleaseTester.grabTimer = damageSongReleaseWorld.fighterDefs[0].properties.common.damageSongTimerDecrementX63C;
    pf::tickWorld(damageSongReleaseWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const std::string damageSongReleaseState = pf::currentState(damageSongReleaseWorld, damageSongReleaseTester).name;
    damageSongReleaseTester.lastStateChangeFrame = damageSongReleaseTester.internalFrame - 1000;
    pf::tickWorld(damageSongReleaseWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World damageSongTeeterWorld = pf::makeTrainingWorld();
    damageSongTeeterWorld.stage.segments = {
        {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    damageSongTeeterWorld.stage.ledges.clear();
    pf::FighterRuntime& damageSongTeeterTester = damageSongTeeterWorld.fighters[0];
    damageSongTeeterTester.position = {pf::fxFromFloat(0.95f), 0};
    damageSongTeeterTester.previousPosition = damageSongTeeterTester.position;
    damageSongTeeterTester.grounded = true;
    damageSongTeeterTester.groundSegment = 0;
    damageSongTeeterTester.groundVelocity = pf::fxFromFloat(0.2f);
    pf::changeFighterState(damageSongTeeterWorld, damageSongTeeterTester, "DamageSongWait");
    damageSongTeeterTester.grabTimer = pf::fx(100);
    pf::tickWorld(damageSongTeeterWorld, {pf::InputFrame{}, pf::InputFrame{}});

    std::cout << "damage_song_route_state=" << pf::currentState(damageSongRouteWorld, damageSongRouteWorld.fighters[1]).name
              << " damage_song_route_timer=" << damageSongRouteTimer
              << " damage_song_strong_timer=" << damageSongStrongTimer
              << " damage_song_mash_timer=" << pf::fxToFloat(damageSongMashTester.grabTimer)
              << " damage_song_wait_state=" << pf::currentState(damageSongWaitWorld, damageSongWaitTester).name
              << " damage_song_release_state=" << damageSongReleaseState
              << " damage_song_finish_state=" << pf::currentState(damageSongReleaseWorld, damageSongReleaseTester).name
              << " damage_song_teeter_state=" << pf::currentState(damageSongTeeterWorld, damageSongTeeterTester).name
              << "\n";

    auto addDamageIceHitbox = [](pf::FighterRuntime& attacker) {
        pf::ActiveHitbox active;
        active.def.element = 5;
        active.def.damage = pf::fx(5);
        active.def.radius = pf::fx(8);
        active.def.knockbackAngleDegrees = pf::fx(45);
        active.def.knockbackBase = pf::fx(30);
        active.def.knockbackGrowth = 0;
        attacker.activeHitboxes.push_back(active);
    };

    pf::World damageIceRouteWorld = pf::makeTrainingWorld();
    damageIceRouteWorld.fighters[0].position = {0, 0};
    damageIceRouteWorld.fighters[1].position = {0, 0};
    addDamageIceHitbox(damageIceRouteWorld.fighters[0]);
    pf::tickWorld(damageIceRouteWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const pf::FighterRuntime& damageIceRouteTester = damageIceRouteWorld.fighters[1];

    pf::World damageIceAirRouteWorld = pf::makeTrainingWorld();
    damageIceAirRouteWorld.fighters[0].position = {0, pf::fx(5)};
    pf::FighterRuntime& damageIceAirRouteTester = damageIceAirRouteWorld.fighters[1];
    damageIceAirRouteTester.position = {0, pf::fx(5)};
    damageIceAirRouteTester.previousPosition = damageIceAirRouteTester.position;
    damageIceAirRouteTester.grounded = false;
    damageIceAirRouteTester.groundSegment = -1;
    addDamageIceHitbox(damageIceAirRouteWorld.fighters[0]);
    pf::tickWorld(damageIceAirRouteWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World damageIceMashWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& damageIceMashTester = damageIceMashWorld.fighters[0];
    pf::changeFighterState(damageIceMashWorld, damageIceMashTester, "DamageIce");
    damageIceMashTester.grabTimer = pf::fx(20);
    pf::InputFrame damageIceMashInput;
    damageIceMashInput.buttons = pf::ButtonSpecial;
    pf::tickWorld(damageIceMashWorld, {damageIceMashInput, pf::InputFrame{}});

    pf::World damageIceReleaseWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& damageIceReleaseTester = damageIceReleaseWorld.fighters[0];
    pf::changeFighterState(damageIceReleaseWorld, damageIceReleaseTester, "DamageIce");
    damageIceReleaseTester.grabTimer = damageIceReleaseWorld.fighterDefs[0].properties.common.damageIceTimerDecrementX794;
    damageIceReleaseTester.input.frames[0].move.x = pf::fxFromFloat(0.5f);
    pf::tickWorld(damageIceReleaseWorld, {damageIceReleaseTester.input.frames[0], pf::InputFrame{}});
    const std::string damageIceReleaseState = pf::currentState(damageIceReleaseWorld, damageIceReleaseTester).name;

    pf::World damageIceJumpFinishWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& damageIceJumpFinishTester = damageIceJumpFinishWorld.fighters[0];
    damageIceJumpFinishTester.position.y = pf::fx(5);
    damageIceJumpFinishTester.previousPosition = damageIceJumpFinishTester.position;
    pf::changeFighterState(damageIceJumpFinishWorld, damageIceJumpFinishTester, "DamageIceJump");
    damageIceJumpFinishTester.grabTimer = pf::fx(1);
    pf::tickWorld(damageIceJumpFinishWorld, {pf::InputFrame{}, pf::InputFrame{}});

    std::cout << "damage_ice_route_state=" << pf::currentState(damageIceRouteWorld, damageIceRouteTester).name
              << " damage_ice_route_timer=" << pf::fxToFloat(damageIceRouteTester.grabTimer)
              << " damage_ice_hurtbox0=" << (damageIceRouteTester.hurtboxStates.empty() ? -1 : static_cast<int>(damageIceRouteTester.hurtboxStates[0]))
              << " damage_ice_air_route_state=" << pf::currentState(damageIceAirRouteWorld, damageIceAirRouteTester).name
              << " damage_ice_air_grounded=" << damageIceAirRouteTester.grounded
              << " damage_ice_mash_timer=" << pf::fxToFloat(damageIceMashTester.grabTimer)
              << " damage_ice_release_state=" << damageIceReleaseState
              << " damage_ice_release_vel=" << pf::toString(damageIceReleaseTester.fighterVelocity)
              << " damage_ice_jump_finish_state=" << pf::currentState(damageIceJumpFinishWorld, damageIceJumpFinishTester).name
              << "\n";

    auto addBuryHitbox = [](pf::FighterRuntime& attacker) {
        pf::ActiveHitbox active;
        active.def.element = 9;
        active.def.damage = pf::fx(5);
        active.def.radius = pf::fx(8);
        active.def.knockbackAngleDegrees = pf::fx(45);
        active.def.knockbackBase = pf::fx(30);
        active.def.knockbackGrowth = 0;
        attacker.activeHitboxes.push_back(active);
    };

    pf::World buryRouteWorld = pf::makeTrainingWorld();
    buryRouteWorld.fighters[0].position = {0, 0};
    buryRouteWorld.fighters[1].position = {0, 0};
    buryRouteWorld.fighters[1].percent = pf::fx(10);
    addBuryHitbox(buryRouteWorld.fighters[0]);
    pf::tickWorld(buryRouteWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const float buryRouteTimer = pf::fxToFloat(buryRouteWorld.fighters[1].grabTimer);
    const int burySubmergeTimer = buryRouteWorld.fighters[1].burySubmergeTimer;

    pf::World buryAirRouteWorld = pf::makeTrainingWorld();
    buryAirRouteWorld.fighters[0].position = {0, pf::fx(5)};
    buryAirRouteWorld.fighters[1].position = {0, pf::fx(5)};
    buryAirRouteWorld.fighters[1].grounded = false;
    buryAirRouteWorld.fighters[1].groundSegment = -1;
    addBuryHitbox(buryAirRouteWorld.fighters[0]);
    pf::tickWorld(buryAirRouteWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World buryWaitWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& buryWaitTester = buryWaitWorld.fighters[0];
    pf::changeFighterState(buryWaitWorld, buryWaitTester, "Bury");
    buryWaitTester.grabTimer = pf::fx(100);
    buryWaitTester.burySubmergeTimer = 1;
    pf::tickWorld(buryWaitWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World buryMashWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& buryMashTester = buryMashWorld.fighters[0];
    pf::changeFighterState(buryMashWorld, buryMashTester, "BuryWait");
    buryMashTester.grabTimer = pf::fx(20);
    pf::InputFrame buryMashInput;
    buryMashInput.buttons = pf::ButtonSpecial;
    pf::tickWorld(buryMashWorld, {buryMashInput, pf::InputFrame{}});

    pf::World buryReleaseWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& buryReleaseTester = buryReleaseWorld.fighters[0];
    pf::changeFighterState(buryReleaseWorld, buryReleaseTester, "BuryWait");
    buryReleaseTester.grabTimer = buryReleaseWorld.fighterDefs[0].properties.common.buryTimerDecrementX610;
    pf::tickWorld(buryReleaseWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const std::string buryReleaseState = pf::currentState(buryReleaseWorld, buryReleaseTester).name;
    const float buryReleaseVelY = pf::fxToFloat(buryReleaseTester.fighterVelocity.y);
    buryReleaseTester.position.y = pf::fx(40);
    buryReleaseTester.previousPosition = buryReleaseTester.position;
    buryReleaseTester.lastStateChangeFrame = buryReleaseTester.internalFrame - 1000;
    pf::tickWorld(buryReleaseWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World buryEdgeReleaseWorld = pf::makeTrainingWorld();
    buryEdgeReleaseWorld.stage.segments = {
        {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    buryEdgeReleaseWorld.stage.ledges.clear();
    pf::FighterRuntime& buryEdgeReleaseTester = buryEdgeReleaseWorld.fighters[0];
    buryEdgeReleaseTester.position = {pf::fxFromFloat(0.95f), 0};
    buryEdgeReleaseTester.previousPosition = buryEdgeReleaseTester.position;
    buryEdgeReleaseTester.grounded = true;
    buryEdgeReleaseTester.groundSegment = 0;
    buryEdgeReleaseTester.groundVelocity = pf::fxFromFloat(0.3f);
    pf::changeFighterState(buryEdgeReleaseWorld, buryEdgeReleaseTester, "BuryWait");
    buryEdgeReleaseTester.grabTimer = pf::fx(100);
    pf::tickWorld(buryEdgeReleaseWorld, {pf::InputFrame{}, pf::InputFrame{}});

    std::cout << "bury_route_state=" << pf::currentState(buryRouteWorld, buryRouteWorld.fighters[1]).name
              << " bury_route_timer=" << buryRouteTimer
              << " bury_submerge_timer=" << burySubmergeTimer
              << " bury_air_route_state=" << pf::currentState(buryAirRouteWorld, buryAirRouteWorld.fighters[1]).name
              << " bury_wait_state=" << pf::currentState(buryWaitWorld, buryWaitTester).name
              << " bury_mash_timer=" << pf::fxToFloat(buryMashTester.grabTimer)
              << " bury_release_state=" << buryReleaseState
              << " bury_release_vel_y=" << buryReleaseVelY
              << " bury_edge_release_state=" << pf::currentState(buryEdgeReleaseWorld, buryEdgeReleaseTester).name
              << " bury_edge_release_vel_y=" << pf::fxToFloat(buryEdgeReleaseTester.fighterVelocity.y)
              << " bury_finish_state=" << pf::currentState(buryReleaseWorld, buryReleaseTester).name
              << "\n";

    pf::World reboundWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& reboundLeft = reboundWorld.fighters[0];
    pf::FighterRuntime& reboundRight = reboundWorld.fighters[1];
    reboundLeft.position = {0, 0};
    reboundLeft.previousPosition = reboundLeft.position;
    reboundRight.position = {pf::fxFromFloat(0.5f), 0};
    reboundRight.previousPosition = reboundRight.position;
    pf::HitboxDefinition reboundDef;
    reboundDef.damage = pf::fx(8);
    reboundDef.radius = pf::fx(1);
    reboundDef.canClank = true;
    reboundDef.reboundsOnClank = true;
    pf::ActiveHitbox reboundHitbox;
    reboundHitbox.def = reboundDef;
    reboundLeft.activeHitboxes.push_back(reboundHitbox);
    reboundRight.activeHitboxes.push_back(reboundHitbox);
    pf::tickWorld(reboundWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const std::string reboundStopState = pf::currentState(reboundWorld, reboundLeft).name;
    pf::tickWorld(reboundWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "rebound_stop_state=" << reboundStopState
              << " rebound_state=" << pf::currentState(reboundWorld, reboundLeft).name
              << " rebound_ground_vel=" << pf::fxToFloat(reboundLeft.groundVelocity)
              << "\n";

    pf::World downFreshCStickWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downFreshCStickTester = downFreshCStickWorld.fighters[0];
    pf::changeFighterState(downFreshCStickWorld, downFreshCStickTester, "DownWaitU");
    pf::InputFrame downCStickRollInput;
    downCStickRollInput.cStick.x = pf::fx(1);
    pf::tickWorld(downFreshCStickWorld, {downCStickRollInput, pf::InputFrame{}});

    pf::World downHeldCStickWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downHeldCStickTester = downHeldCStickWorld.fighters[0];
    pf::changeFighterState(downHeldCStickWorld, downHeldCStickTester, "DownWaitU");
    downHeldCStickTester.input.frames[0].cStick.x = pf::fx(1);
    pf::tickWorld(downHeldCStickWorld, {downCStickRollInput, pf::InputFrame{}});

    pf::World downHighCStickWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downHighCStickTester = downHighCStickWorld.fighters[0];
    pf::changeFighterState(downHighCStickWorld, downHighCStickTester, "DownWaitU");
    pf::InputFrame downHighCStickInput;
    downHighCStickInput.cStick.x = downHighCStickWorld.fighterDefs[0].properties.common.downRollStickThresholdX248;
    downHighCStickInput.cStick.y = pf::fx(1);
    pf::tickWorld(downHighCStickWorld, {downHighCStickInput, pf::InputFrame{}});
    std::cout << "down_cstick_fresh_state=" << pf::currentState(downFreshCStickWorld, downFreshCStickTester).name
              << " down_cstick_held_state=" << pf::currentState(downHeldCStickWorld, downHeldCStickTester).name
              << " down_cstick_high_state=" << pf::currentState(downHighCStickWorld, downHighCStickTester).name
              << "\n";

    pf::World downFreshShieldWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downFreshShieldTester = downFreshShieldWorld.fighters[0];
    pf::changeFighterState(downFreshShieldWorld, downFreshShieldTester, "DownWaitU");
    pf::InputFrame downShieldInput;
    downShieldInput.shieldAnalog = pf::fx(1);
    pf::tickWorld(downFreshShieldWorld, {downShieldInput, pf::InputFrame{}});

    pf::World downHeldShieldWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downHeldShieldTester = downHeldShieldWorld.fighters[0];
    pf::changeFighterState(downHeldShieldWorld, downHeldShieldTester, "DownWaitU");
    downHeldShieldTester.input.frames[0].shieldAnalog = pf::fx(1);
    pf::tickWorld(downHeldShieldWorld, {downShieldInput, pf::InputFrame{}});
    std::cout << "down_shield_fresh_state=" << pf::currentState(downFreshShieldWorld, downFreshShieldTester).name
              << " down_shield_held_state=" << pf::currentState(downHeldShieldWorld, downHeldShieldTester).name
              << "\n";

    pf::World downGrabWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downGrabTester = downGrabWorld.fighters[0];
    pf::changeFighterState(downGrabWorld, downGrabTester, "DownWaitU");
    pf::tickWorld(downGrabWorld, {grabInput, pf::InputFrame{}});
    std::cout << "down_grab_state=" << pf::currentState(downGrabWorld, downGrabTester).name
              << "\n";

    pf::World downUpAngleWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downUpAngleTester = downUpAngleWorld.fighters[0];
    pf::changeFighterState(downUpAngleWorld, downUpAngleTester, "DownWaitU");
    pf::InputFrame downUpAngleInput;
    downUpAngleInput.move.y = downUpAngleWorld.fighterDefs[0].properties.common.downStandStickThresholdX244;
    pf::tickWorld(downUpAngleWorld, {downUpAngleInput, pf::InputFrame{}});

    pf::World downDiagonalUpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downDiagonalUpTester = downDiagonalUpWorld.fighters[0];
    pf::changeFighterState(downDiagonalUpWorld, downDiagonalUpTester, "DownWaitU");
    downDiagonalUpWorld.fighterDefs[0].properties.common.downRollStickThresholdX248 = pf::fx(2);
    downDiagonalUpWorld.fighterDefs[0].properties.common.aerialAttackAngleTanX20 = pf::fx(1);
    pf::InputFrame downDiagonalUpInput;
    downDiagonalUpInput.move.x = pf::fxFromFloat(0.8f);
    downDiagonalUpInput.move.y = downDiagonalUpWorld.fighterDefs[0].properties.common.downStandStickThresholdX244;
    pf::tickWorld(downDiagonalUpWorld, {downDiagonalUpInput, pf::InputFrame{}});
    std::cout << "down_up_angle_state=" << pf::currentState(downUpAngleWorld, downUpAngleTester).name
              << " down_diagonal_up_angle_state=" << pf::currentState(downDiagonalUpWorld, downDiagonalUpTester).name
              << "\n";

    pf::World downFreshCStickAttackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downFreshCStickAttackTester = downFreshCStickAttackWorld.fighters[0];
    pf::changeFighterState(downFreshCStickAttackWorld, downFreshCStickAttackTester, "DownWaitU");
    pf::InputFrame downCStickAttackInput;
    downCStickAttackInput.cStick.y = pf::fx(1);
    pf::tickWorld(downFreshCStickAttackWorld, {downCStickAttackInput, pf::InputFrame{}});

    pf::World downHeldCStickAttackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downHeldCStickAttackTester = downHeldCStickAttackWorld.fighters[0];
    pf::changeFighterState(downHeldCStickAttackWorld, downHeldCStickAttackTester, "DownWaitU");
    downHeldCStickAttackTester.input.frames[0].cStick.y = pf::fx(1);
    pf::tickWorld(downHeldCStickAttackWorld, {downCStickAttackInput, pf::InputFrame{}});
    std::cout << "down_cstick_attack_fresh_state=" << pf::currentState(downFreshCStickAttackWorld, downFreshCStickAttackTester).name
              << " down_cstick_attack_held_state=" << pf::currentState(downHeldCStickAttackWorld, downHeldCStickAttackTester).name
              << "\n";

    pf::InputFrame downAttackButtonInput;
    downAttackButtonInput.buttons = pf::ButtonAttack;
    pf::World downBoundBufferedAttackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downBoundBufferedAttackTester = downBoundBufferedAttackWorld.fighters[0];
    pf::changeFighterState(downBoundBufferedAttackWorld, downBoundBufferedAttackTester, "DownBoundU");
    const int downBoundBufferedLength = pf::currentState(downBoundBufferedAttackWorld, downBoundBufferedAttackTester).animationLengthFrames;
    for (int frame = 0; frame < std::max(0, downBoundBufferedLength - 5); ++frame) {
        pf::tickWorld(downBoundBufferedAttackWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    pf::tickWorld(downBoundBufferedAttackWorld, {downAttackButtonInput, pf::InputFrame{}});
    for (int frame = 0; frame < downBoundBufferedLength + 10 && pf::currentState(downBoundBufferedAttackWorld, downBoundBufferedAttackTester).name == "DownBoundU"; ++frame) {
        pf::tickWorld(downBoundBufferedAttackWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }

    pf::World downBoundNoAttackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downBoundNoAttackTester = downBoundNoAttackWorld.fighters[0];
    pf::changeFighterState(downBoundNoAttackWorld, downBoundNoAttackTester, "DownBoundU");
    const int downBoundNoAttackLength = pf::currentState(downBoundNoAttackWorld, downBoundNoAttackTester).animationLengthFrames;
    for (int frame = 0; frame < downBoundNoAttackLength + 10 && pf::currentState(downBoundNoAttackWorld, downBoundNoAttackTester).name == "DownBoundU"; ++frame) {
        pf::tickWorld(downBoundNoAttackWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "down_bound_attack_buffered_state=" << pf::currentState(downBoundBufferedAttackWorld, downBoundBufferedAttackTester).name
              << " down_bound_no_attack_state=" << pf::currentState(downBoundNoAttackWorld, downBoundNoAttackTester).name
              << "\n";

    pf::World downBoundGrabWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downBoundGrabTester = downBoundGrabWorld.fighters[0];
    pf::changeFighterState(downBoundGrabWorld, downBoundGrabTester, "DownBoundU");
    const int downBoundGrabLength = pf::currentState(downBoundGrabWorld, downBoundGrabTester).animationLengthFrames;
    for (int frame = 0; frame < std::max(0, downBoundGrabLength - 5); ++frame) {
        pf::tickWorld(downBoundGrabWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    pf::tickWorld(downBoundGrabWorld, {grabInput, pf::InputFrame{}});
    for (int frame = 0; frame < downBoundGrabLength + 10 &&
        pf::currentState(downBoundGrabWorld, downBoundGrabTester).name == "DownBoundU"; ++frame)
    {
        pf::tickWorld(downBoundGrabWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "down_bound_grab_state=" << pf::currentState(downBoundGrabWorld, downBoundGrabTester).name
              << "\n";

    pf::World downBoundPreEntryAttackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downBoundPreEntryAttackTester = downBoundPreEntryAttackWorld.fighters[0];
    downBoundPreEntryAttackTester.input.frames[0].buttons = pf::ButtonAttack;
    pf::changeFighterState(downBoundPreEntryAttackWorld, downBoundPreEntryAttackTester, "DownBoundU");
    const int downBoundPreEntryLength = pf::currentState(downBoundPreEntryAttackWorld, downBoundPreEntryAttackTester).animationLengthFrames;
    for (int frame = 0; frame < downBoundPreEntryLength + 10 &&
        pf::currentState(downBoundPreEntryAttackWorld, downBoundPreEntryAttackTester).name == "DownBoundU"; ++frame)
    {
        pf::tickWorld(downBoundPreEntryAttackWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "down_bound_pre_entry_attack_state=" << pf::currentState(downBoundPreEntryAttackWorld, downBoundPreEntryAttackTester).name
              << "\n";

    pf::World groundedDamageFrictionWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& groundedDamageFrictionTester = groundedDamageFrictionWorld.fighters[0];
    groundedDamageFrictionTester.grounded = true;
    groundedDamageFrictionTester.groundSegment = 0;
    groundedDamageFrictionTester.groundVelocity =
        groundedDamageFrictionWorld.fighterDefs[0].properties.walkMaxVel + pf::fx(1);
    const pf::Fix groundedDamageFrictionStart = groundedDamageFrictionTester.groundVelocity;
    pf::changeFighterState(groundedDamageFrictionWorld, groundedDamageFrictionTester, "DamageN1");
    groundedDamageFrictionTester.groundVelocity = groundedDamageFrictionStart;
    pf::tickWorld(groundedDamageFrictionWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "grounded_damage_friction_delta="
              << pf::fxToFloat(groundedDamageFrictionStart - groundedDamageFrictionTester.groundVelocity)
              << " grounded_damage_expected_delta="
              << pf::fxToFloat(pf::fxMul(
                  groundedDamageFrictionWorld.fighterDefs[0].properties.grFriction,
                  groundedDamageFrictionWorld.fighterDefs[0].properties.common.turnFrictionScaleAboveWalkMaxX6C))
              << "\n";

    pf::World downDamageExpiredWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downDamageExpiredTester = downDamageExpiredWorld.fighters[0];
    pf::changeFighterState(downDamageExpiredWorld, downDamageExpiredTester, "DownDamageU");
    downDamageExpiredTester.downWaitTimer = 1;
    const int downDamageExpiredLength = pf::currentState(downDamageExpiredWorld, downDamageExpiredTester).animationLengthFrames;
    for (int frame = 0; frame < downDamageExpiredLength + 5 &&
        pf::currentState(downDamageExpiredWorld, downDamageExpiredTester).name == "DownDamageU"; ++frame)
    {
        pf::tickWorld(downDamageExpiredWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }

    pf::World downDamageCarryWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downDamageCarryTester = downDamageCarryWorld.fighters[0];
    pf::changeFighterState(downDamageCarryWorld, downDamageCarryTester, "DownDamageU");
    const int downDamageCarryLength = pf::currentState(downDamageCarryWorld, downDamageCarryTester).animationLengthFrames;
    downDamageCarryTester.downWaitTimer = downDamageCarryLength + 5;
    for (int frame = 0; frame < downDamageCarryLength + 5 &&
        pf::currentState(downDamageCarryWorld, downDamageCarryTester).name == "DownDamageU"; ++frame)
    {
        pf::tickWorld(downDamageCarryWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "down_damage_expired_state=" << pf::currentState(downDamageExpiredWorld, downDamageExpiredTester).name
              << " down_damage_carry_state=" << pf::currentState(downDamageCarryWorld, downDamageCarryTester).name
              << " down_damage_carry_timer=" << downDamageCarryTester.downWaitTimer
              << "\n";

    pf::World downProneHitWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downProneHitAttacker = downProneHitWorld.fighters[0];
    pf::FighterRuntime& downProneHitVictim = downProneHitWorld.fighters[1];
    downProneHitAttacker.position = {-pf::fx(1), 0};
    downProneHitVictim.position = {pf::fx(1), 0};
    pf::changeFighterState(downProneHitWorld, downProneHitVictim, "DownWaitU");
    pf::HitboxDefinition downProneHitbox;
    downProneHitbox.damage = pf::fx(std::max(1, downProneHitWorld.fighterDefs[1].properties.common.downDamageThresholdX428 - 1));
    downProneHitbox.knockbackAngleDegrees = pf::fx(0);
    downProneHitbox.knockbackBase = pf::fx(10);
    downProneHitbox.knockbackGrowth = 0;
    downProneHitbox.radius = pf::fx(6);
    const pf::Vec3 downProneHitPoint{downProneHitVictim.position.x, downProneHitVictim.position.y, 0};
    downProneHitAttacker.activeHitboxes.push_back({downProneHitbox, false, downProneHitPoint, downProneHitPoint});
    pf::tickWorld(downProneHitWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "down_prone_low_hit_state=" << pf::currentState(downProneHitWorld, downProneHitVictim).name
              << " down_prone_low_hit_timer=" << downProneHitVictim.downWaitTimer
              << "\n";

    pf::World downBoundProneHitWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downBoundProneHitAttacker = downBoundProneHitWorld.fighters[0];
    pf::FighterRuntime& downBoundProneHitVictim = downBoundProneHitWorld.fighters[1];
    downBoundProneHitAttacker.position = {-pf::fx(1), 0};
    downBoundProneHitVictim.position = {pf::fx(1), 0};
    pf::changeFighterState(downBoundProneHitWorld, downBoundProneHitVictim, "DownBoundU");
    pf::HitboxDefinition downBoundProneHitbox = downProneHitbox;
    const pf::Vec3 downBoundProneHitPoint{downBoundProneHitVictim.position.x, downBoundProneHitVictim.position.y, 0};
    downBoundProneHitAttacker.activeHitboxes.push_back({downBoundProneHitbox, false, downBoundProneHitPoint, downBoundProneHitPoint});
    pf::tickWorld(downBoundProneHitWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "down_bound_prone_low_hit_state=" << pf::currentState(downBoundProneHitWorld, downBoundProneHitVictim).name
              << "\n";

    pf::World runoffWorld = pf::makeTrainingWorld();
    runoffWorld.stage.segments = {
        {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    runoffWorld.stage.ledges.clear();
    pf::FighterRuntime& runoffTester = runoffWorld.fighters[0];
    runoffTester.position = {pf::fxFromFloat(0.5f), 0};
    runoffTester.previousPosition = runoffTester.position;
    runoffTester.grounded = true;
    runoffTester.groundSegment = 0;
    runoffTester.groundVelocity = pf::fxFromFloat(1.5f);
    pf::changeFighterState(runoffWorld, runoffTester, "Run");
    pf::InputFrame runoffInput;
    runoffInput.move.x = pf::fx(1);
    pf::tickWorld(runoffWorld, {runoffInput, pf::InputFrame{}});
    std::cout << "runoff_state=" << pf::currentState(runoffWorld, runoffTester).name
              << " grounded=" << runoffTester.grounded
              << " pos=" << pf::toString(runoffTester.position)
              << "\n";

    pf::World backwardSlideWorld = pf::makeTrainingWorld();
    backwardSlideWorld.stage.segments = {
        {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    backwardSlideWorld.stage.ledges.clear();
    pf::FighterRuntime& backwardSlideTester = backwardSlideWorld.fighters[0];
    backwardSlideTester.position = {pf::fxFromFloat(0.99f), 0};
    backwardSlideTester.previousPosition = backwardSlideTester.position;
    backwardSlideTester.grounded = true;
    backwardSlideTester.groundSegment = 0;
    backwardSlideTester.facing = -1;
    backwardSlideTester.groundVelocity = pf::fxFromFloat(0.08f);
    pf::changeFighterState(backwardSlideWorld, backwardSlideTester, "Wait");
    pf::tickWorld(backwardSlideWorld, {pf::InputFrame{}, pf::InputFrame{}});

    auto prepareShiftedEcbRunoff = []() {
        pf::World world = pf::makeTrainingWorld();
        world.stage.segments = {
            {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
        };
        world.stage.ledges.clear();
        pf::FighterRuntime& fighter = world.fighters[0];
        fighter.position = {pf::fxFromFloat(1.01f), 0};
        fighter.previousPosition = fighter.position;
        fighter.grounded = true;
        fighter.groundSegment = 0;
        fighter.groundVelocity = 0;
        fighter.ecbLockTimer = 2;
        fighter.ecbLockBottom = {-pf::fxFromFloat(0.25f), 0};
        pf::changeFighterState(world, fighter, "Wait");
        return world;
    };
    pf::World shiftedEcbFallWorld = prepareShiftedEcbRunoff();
    pf::FighterRuntime& shiftedEcbFallTester = shiftedEcbFallWorld.fighters[0];
    shiftedEcbFallTester.facing = -1;
    pf::tickWorld(shiftedEcbFallWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World shiftedEcbTeeterWorld = prepareShiftedEcbRunoff();
    pf::FighterRuntime& shiftedEcbTeeterTester = shiftedEcbTeeterWorld.fighters[0];
    shiftedEcbTeeterTester.facing = 1;
    pf::tickWorld(shiftedEcbTeeterWorld, {pf::InputFrame{}, pf::InputFrame{}});

    std::cout << "backward_slide_runoff_state=" << pf::currentState(backwardSlideWorld, backwardSlideTester).name
              << " grounded=" << backwardSlideTester.grounded
              << " pos=" << pf::toString(backwardSlideTester.position)
              << " shifted_ecb_runoff_state=" << pf::currentState(shiftedEcbFallWorld, shiftedEcbFallTester).name
              << " shifted_ecb_teeter_state=" << pf::currentState(shiftedEcbTeeterWorld, shiftedEcbTeeterTester).name
              << "\n";

    auto noTeeterEdgeState = [](const std::string& stateName) {
        pf::World world = pf::makeTrainingWorld();
        world.stage.segments = {
            {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
        };
        world.stage.ledges.clear();
        pf::FighterRuntime& fighter = world.fighters[0];
        fighter.position = {pf::fxFromFloat(1.01f), 0};
        fighter.previousPosition = fighter.position;
        fighter.grounded = true;
        fighter.groundSegment = 0;
        fighter.facing = 1;
        fighter.groundVelocity = 0;
        pf::changeFighterState(world, fighter, stateName);
        pf::tickWorld(world, {pf::InputFrame{}, pf::InputFrame{}});
        return pf::currentState(world, fighter).name;
    };
    std::cout << "no_teeter_edge_turn_state=" << noTeeterEdgeState("Turn")
              << " no_teeter_edge_squat_state=" << noTeeterEdgeState("Squat")
              << " no_teeter_edge_squat_wait_state=" << noTeeterEdgeState("SquatWait")
              << " no_teeter_edge_squat_rv_state=" << noTeeterEdgeState("SquatRv")
              << " no_teeter_edge_escape_n_state=" << noTeeterEdgeState("EscapeN")
              << " no_teeter_edge_escape_f_state=" << noTeeterEdgeState("EscapeF")
              << " no_teeter_edge_escape_b_state=" << noTeeterEdgeState("EscapeB")
              << " no_teeter_edge_appeal_sr_state=" << noTeeterEdgeState("AppealSR")
              << " no_teeter_edge_appeal_sl_state=" << noTeeterEdgeState("AppealSL")
              << "\n";

    pf::InputFrame tauntInput;
    tauntInput.buttons |= pf::ButtonTaunt;
    pf::World appealRightWorld = pf::makeTrainingWorld();
    appealRightWorld.fighters[0].facing = 1;
    pf::tickWorld(appealRightWorld, {tauntInput, pf::InputFrame{}});
    pf::World appealLeftWorld = pf::makeTrainingWorld();
    appealLeftWorld.fighters[0].facing = -1;
    pf::tickWorld(appealLeftWorld, {tauntInput, pf::InputFrame{}});
    pf::World appealFinishWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& appealFinishTester = appealFinishWorld.fighters[0];
    pf::changeFighterState(appealFinishWorld, appealFinishTester, "AppealSR");
    appealFinishTester.lastStateChangeFrame = appealFinishTester.internalFrame - 1000;
    pf::tickWorld(appealFinishWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const pf::FighterDefinition& appealDef = appealRightWorld.fighterDefs[static_cast<size_t>(appealRightWorld.fighters[0].fighterDef)];
    const pf::FighterState& appealSRState = appealDef.states[static_cast<size_t>(appealDef.stateIndex("AppealSR"))];
    const pf::FighterState& appealSLState = appealDef.states[static_cast<size_t>(appealDef.stateIndex("AppealSL"))];
    const bool appealSRClipReady = appealDef.hsdAsset &&
        pf::findClipByActionIndex(*appealDef.hsdAsset, appealSRState.animationActionIndex) != nullptr;
    const bool appealSLClipReady = appealDef.hsdAsset &&
        pf::findClipByActionIndex(*appealDef.hsdAsset, appealSLState.animationActionIndex) != nullptr;
    std::cout << "appeal_right_input_state=" << pf::currentState(appealRightWorld, appealRightWorld.fighters[0]).name
              << " appeal_left_input_state=" << pf::currentState(appealLeftWorld, appealLeftWorld.fighters[0]).name
              << " appeal_finish_state=" << pf::currentState(appealFinishWorld, appealFinishTester).name
              << " appeal_sr_action=" << appealSRState.animationActionIndex
              << " appeal_sl_action=" << appealSLState.animationActionIndex
              << " appeal_sr_clip=" << appealSRClipReady
              << " appeal_sl_clip=" << appealSLClipReady
              << " appeal_sr_frames=" << appealSRState.animationLengthFrames
              << " appeal_sl_frames=" << appealSLState.animationLengthFrames
              << "\n";

    pf::World tauntDashPriorityWorld = pf::makeTrainingWorld();
    pf::InputFrame tauntDashPriorityInput;
    tauntDashPriorityInput.buttons = pf::ButtonTaunt;
    tauntDashPriorityInput.move.x = pf::fx(1);
    pf::tickWorld(tauntDashPriorityWorld, {tauntDashPriorityInput, pf::InputFrame{}});

    pf::World squatWaitTauntJumpPriorityWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& squatWaitTauntJumpPriorityTester = squatWaitTauntJumpPriorityWorld.fighters[0];
    pf::changeFighterState(squatWaitTauntJumpPriorityWorld, squatWaitTauntJumpPriorityTester, "SquatWait");
    pf::InputFrame squatWaitTauntJumpPriorityInput;
    squatWaitTauntJumpPriorityInput.buttons = pf::ButtonTaunt | pf::ButtonJump;
    pf::tickWorld(squatWaitTauntJumpPriorityWorld, {squatWaitTauntJumpPriorityInput, pf::InputFrame{}});
    std::cout << "taunt_dash_priority_state=" << pf::currentState(tauntDashPriorityWorld, tauntDashPriorityWorld.fighters[0]).name
              << " squat_wait_taunt_jump_priority_state=" << pf::currentState(squatWaitTauntJumpPriorityWorld, squatWaitTauntJumpPriorityTester).name
              << "\n";

    pf::World appealInterruptShieldWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& appealInterruptShieldTester = appealInterruptShieldWorld.fighters[0];
    pf::changeFighterState(appealInterruptShieldWorld, appealInterruptShieldTester, "AppealSR");
    appealInterruptShieldTester.interruptibleFrame = 0;
    pf::InputFrame appealInterruptShieldInput;
    appealInterruptShieldInput.buttons = pf::ButtonShield;
    pf::tickWorld(appealInterruptShieldWorld, {appealInterruptShieldInput, pf::InputFrame{}});

    pf::World appealInterruptJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& appealInterruptJumpTester = appealInterruptJumpWorld.fighters[0];
    pf::changeFighterState(appealInterruptJumpWorld, appealInterruptJumpTester, "AppealSR");
    appealInterruptJumpTester.interruptibleFrame = 0;
    pf::InputFrame appealInterruptJumpInput;
    appealInterruptJumpInput.buttons = pf::ButtonJump;
    pf::tickWorld(appealInterruptJumpWorld, {appealInterruptJumpInput, pf::InputFrame{}});
    std::cout << "appeal_interrupt_shield_state=" << pf::currentState(appealInterruptShieldWorld, appealInterruptShieldTester).name
              << " appeal_interrupt_jump_state=" << pf::currentState(appealInterruptJumpWorld, appealInterruptJumpTester).name
              << "\n";

    pf::World teeterWorld = pf::makeTrainingWorld();
    teeterWorld.stage.segments = {
        {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    teeterWorld.stage.ledges.clear();
    pf::FighterRuntime& teeterTester = teeterWorld.fighters[0];
    teeterTester.position = {pf::fxFromFloat(0.95f), 0};
    teeterTester.previousPosition = teeterTester.position;
    teeterTester.grounded = true;
    teeterTester.groundSegment = 0;
    teeterTester.groundVelocity = pf::fxFromFloat(0.2f);
    pf::tickWorld(teeterWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "teeter_state=" << pf::currentState(teeterWorld, teeterTester).name
              << " grounded=" << teeterTester.grounded
              << " pos=" << pf::toString(teeterTester.position)
              << "\n";

    auto makeTeeterInputWorld = [] {
        pf::World world = pf::makeTrainingWorld();
        world.stage.segments = {
            {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
        };
        world.stage.ledges.clear();
        pf::FighterRuntime& fighter = world.fighters[0];
        fighter.position = {pf::fx(1), 0};
        fighter.previousPosition = fighter.position;
        fighter.grounded = true;
        fighter.groundSegment = 0;
        fighter.facing = 1;
        pf::changeFighterState(world, fighter, "Ottotto");
        return world;
    };
    pf::World teeterGrabWorld = makeTeeterInputWorld();
    pf::InputFrame teeterGrabInput;
    teeterGrabInput.buttons = pf::ButtonGrab;
    pf::tickWorld(teeterGrabWorld, {teeterGrabInput, pf::InputFrame{}});

    pf::World teeterAttackWorld = makeTeeterInputWorld();
    pf::InputFrame teeterAttackInput;
    teeterAttackInput.buttons = pf::ButtonAttack;
    teeterAttackInput.move.x = pf::fxFromFloat(0.5f);
    pf::tickWorld(teeterAttackWorld, {teeterAttackInput, pf::InputFrame{}});

    pf::World teeterTauntWorld = makeTeeterInputWorld();
    pf::InputFrame teeterTauntInput;
    teeterTauntInput.buttons = pf::ButtonTaunt;
    pf::tickWorld(teeterTauntWorld, {teeterTauntInput, pf::InputFrame{}});
    std::cout << "teeter_grab_state=" << pf::currentState(teeterGrabWorld, teeterGrabWorld.fighters[0]).name
              << " teeter_attack_state=" << pf::currentState(teeterAttackWorld, teeterAttackWorld.fighters[0]).name
              << " teeter_taunt_state=" << pf::currentState(teeterTauntWorld, teeterTauntWorld.fighters[0]).name
              << "\n";

    pf::World teeterRunoffWorld = makeTeeterInputWorld();
    pf::FighterRuntime& teeterRunoffTester = teeterRunoffWorld.fighters[0];
    teeterRunoffTester.position = {pf::fxFromFloat(0.99f), 0};
    teeterRunoffTester.previousPosition = teeterRunoffTester.position;
    teeterRunoffTester.groundVelocity = pf::fxFromFloat(0.2f);
    pf::tickWorld(teeterRunoffWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "teeter_runoff_state=" << pf::currentState(teeterRunoffWorld, teeterRunoffTester).name
              << " teeter_runoff_grounded=" << teeterRunoffTester.grounded
              << " teeter_runoff_jumps=" << teeterRunoffTester.jumpsUsed
              << " teeter_runoff_ecb=" << teeterRunoffTester.ecbLockTimer
              << "\n";

    pf::World guardMissFootWorld = pf::makeTrainingWorld();
    guardMissFootWorld.stage.segments = {
        {{0, 0}, {pf::fx(1), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    guardMissFootWorld.stage.ledges.clear();
    pf::FighterRuntime& guardMissFootTester = guardMissFootWorld.fighters[0];
    guardMissFootTester.position = {pf::fxFromFloat(0.99f), 0};
    guardMissFootTester.previousPosition = guardMissFootTester.position;
    guardMissFootTester.grounded = true;
    guardMissFootTester.groundSegment = 0;
    guardMissFootTester.facing = -1;
    guardMissFootTester.groundVelocity = pf::fxFromFloat(0.08f);
    pf::changeFighterState(guardMissFootWorld, guardMissFootTester, "Guard");
    pf::tickWorld(guardMissFootWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World guardEdgeFallWorld = pf::makeTrainingWorld();
    guardEdgeFallWorld.stage.segments = guardMissFootWorld.stage.segments;
    guardEdgeFallWorld.stage.ledges.clear();
    pf::FighterRuntime& guardEdgeFallTester = guardEdgeFallWorld.fighters[0];
    guardEdgeFallTester.position = {pf::fxFromFloat(0.99f), 0};
    guardEdgeFallTester.previousPosition = guardEdgeFallTester.position;
    guardEdgeFallTester.grounded = true;
    guardEdgeFallTester.groundSegment = 0;
    guardEdgeFallTester.facing = 1;
    guardEdgeFallTester.groundVelocity = pf::fxFromFloat(0.08f);
    pf::changeFighterState(guardEdgeFallWorld, guardEdgeFallTester, "Guard");
    pf::tickWorld(guardEdgeFallWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World missFootFinishWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& missFootFinishTester = missFootFinishWorld.fighters[0];
    missFootFinishTester.position.y = pf::fx(40);
    missFootFinishTester.previousPosition = missFootFinishTester.position;
    missFootFinishTester.grounded = false;
    missFootFinishTester.groundSegment = -1;
    pf::changeFighterState(missFootFinishWorld, missFootFinishTester, "MissFoot");
    missFootFinishTester.lastStateChangeFrame = missFootFinishTester.internalFrame - 1000;
    pf::tickWorld(missFootFinishWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World missFootFastfallWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& missFootFastfallTester = missFootFastfallWorld.fighters[0];
    missFootFastfallTester.position.y = pf::fx(6);
    missFootFastfallTester.previousPosition = missFootFastfallTester.position;
    missFootFastfallTester.grounded = false;
    missFootFastfallTester.groundSegment = -1;
    missFootFastfallTester.fighterVelocity.y = -pf::fxFromFloat(0.1f);
    pf::changeFighterState(missFootFastfallWorld, missFootFastfallTester, "MissFoot");
    pf::InputFrame missFootFastfallInput;
    missFootFastfallInput.move.y = -pf::fx(1);
    pf::tickWorld(missFootFastfallWorld, {missFootFastfallInput, pf::InputFrame{}});

    std::cout << "guard_miss_foot_state=" << pf::currentState(guardMissFootWorld, guardMissFootTester).name
              << " guard_miss_foot_grounded=" << guardMissFootTester.grounded
              << " guard_miss_foot_jumps=" << guardMissFootTester.jumpsUsed
              << " guard_miss_foot_ecb=" << guardMissFootTester.ecbLockTimer
              << " guard_edge_fall_state=" << pf::currentState(guardEdgeFallWorld, guardEdgeFallTester).name
              << " miss_foot_finish_state=" << pf::currentState(missFootFinishWorld, missFootFinishTester).name
              << " miss_foot_fastfall_flag=" << pf::fighterFlag(missFootFastfallTester, 12)
              << " miss_foot_fastfall_vel_y=" << pf::fxToFloat(missFootFastfallTester.fighterVelocity.y)
              << "\n";

    auto setupDamageFlyLanding = [](pf::World& damageWorld) -> pf::FighterRuntime& {
        pf::FighterRuntime& tester = damageWorld.fighters[0];
        tester.position.y = pf::fxFromFloat(2.0f);
        tester.previousPosition = tester.position;
        tester.fighterVelocity.y = -pf::fxFromFloat(4.0f);
        tester.grounded = false;
        tester.groundSegment = -1;
        tester.damageTumble = true;
        pf::changeFighterState(damageWorld, tester, "DamageFlyN");
        return tester;
    };
    pf::World passiveRepeatNormalWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& passiveRepeatNormalTester = setupDamageFlyLanding(passiveRepeatNormalWorld);
    pf::InputFrame passiveRepeatInput;
    passiveRepeatInput.buttons |= pf::ButtonShield;
    for (int frame = 0; frame < 10 && !passiveRepeatNormalTester.grounded; ++frame) {
        pf::tickWorld(passiveRepeatNormalWorld, {frame == 0 ? passiveRepeatInput : pf::InputFrame{}, pf::InputFrame{}});
    }

    pf::World passiveRepeatRapidWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& passiveRepeatRapidTester = setupDamageFlyLanding(passiveRepeatRapidWorld);
    passiveRepeatRapidTester.input.frames[1].buttons |= pf::ButtonShield;
    for (int frame = 0; frame < 10 && !passiveRepeatRapidTester.grounded; ++frame) {
        pf::tickWorld(passiveRepeatRapidWorld, {frame == 0 ? passiveRepeatInput : pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "passive_repeat_normal_state=" << pf::currentState(passiveRepeatNormalWorld, passiveRepeatNormalTester).name
              << " passive_repeat_rapid_state=" << pf::currentState(passiveRepeatRapidWorld, passiveRepeatRapidTester).name
              << " passive_repeat_window=" << passiveRepeatRapidWorld.fighterDefs[0].properties.common.inputRepeatWindowX1C
              << "\n";

    pf::World passiveKnockbackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& passiveKnockbackTester = passiveKnockbackWorld.fighters[0];
    passiveKnockbackTester.grounded = true;
    passiveKnockbackTester.groundSegment = 0;
    passiveKnockbackTester.groundNormal = {0, pf::fx(1)};
    passiveKnockbackTester.knockbackVelocity = {pf::fx(10), 0};
    pf::changeFighterState(passiveKnockbackWorld, passiveKnockbackTester, "Passive");
    pf::World passiveStandKnockbackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& passiveStandKnockbackTester = passiveStandKnockbackWorld.fighters[0];
    passiveStandKnockbackTester.grounded = true;
    passiveStandKnockbackTester.groundSegment = 0;
    passiveStandKnockbackTester.groundNormal = {0, pf::fx(1)};
    passiveStandKnockbackTester.knockbackVelocity = {pf::fx(10), 0};
    pf::changeFighterState(passiveStandKnockbackWorld, passiveStandKnockbackTester, "PassiveStandF");
    std::cout << "passive_kb_flat=" << pf::fxToFloat(passiveKnockbackTester.groundKnockbackVelocity)
              << " passive_kb_clamp=" << pf::fxToFloat(passiveKnockbackWorld.fighterDefs[0].properties.common.damageGroundKnockbackClampX164)
              << " passive_stand_kb_x=" << pf::fxToFloat(passiveStandKnockbackTester.knockbackVelocity.x)
              << "\n";

    pf::World downBoundKnockbackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downBoundKnockbackTester = downBoundKnockbackWorld.fighters[0];
    downBoundKnockbackTester.grounded = true;
    downBoundKnockbackTester.groundSegment = 0;
    downBoundKnockbackTester.groundNormal = {0, pf::fx(1)};
    downBoundKnockbackTester.knockbackVelocity = {pf::fx(10), 0};
    pf::changeFighterState(downBoundKnockbackWorld, downBoundKnockbackTester, "DownBoundU");
    pf::World downWaitKnockbackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downWaitKnockbackTester = downWaitKnockbackWorld.fighters[0];
    downWaitKnockbackTester.grounded = true;
    downWaitKnockbackTester.groundSegment = 0;
    downWaitKnockbackTester.groundNormal = {0, pf::fx(1)};
    downWaitKnockbackTester.groundKnockbackVelocity = pf::fx(4);
    downWaitKnockbackTester.knockbackVelocity = {pf::fx(4), 0};
    pf::changeFighterState(downWaitKnockbackWorld, downWaitKnockbackTester, "DownWaitU");
    pf::World downRollKnockbackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& downRollKnockbackTester = downRollKnockbackWorld.fighters[0];
    downRollKnockbackTester.grounded = true;
    downRollKnockbackTester.groundSegment = 0;
    downRollKnockbackTester.groundNormal = {0, pf::fx(1)};
    downRollKnockbackTester.knockbackVelocity = {pf::fx(10), 0};
    pf::changeFighterState(downRollKnockbackWorld, downRollKnockbackTester, "DownForwardU");
    std::cout << "down_kb_bound=" << pf::fxToFloat(downBoundKnockbackTester.groundKnockbackVelocity)
              << " down_kb_wait=" << pf::fxToFloat(downWaitKnockbackTester.groundKnockbackVelocity)
              << " down_kb_roll=" << pf::fxToFloat(downRollKnockbackTester.groundKnockbackVelocity)
              << "\n";

    pf::World passiveWallWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& passiveWallTester = passiveWallWorld.fighters[0];
    passiveWallTester.position.y = pf::fx(8);
    passiveWallTester.previousPosition = passiveWallTester.position;
    passiveWallTester.grounded = false;
    passiveWallTester.groundSegment = -1;
    passiveWallTester.facing = -1;
    pf::changeFighterState(passiveWallWorld, passiveWallTester, "PassiveWall");
    pf::InputFrame passiveWallInput;
    passiveWallInput.move.x = pf::fx(1);
    pf::tickWorld(passiveWallWorld, {passiveWallInput, pf::InputFrame{}});
    const pf::Vec2 passiveWallHeldVelocity = passiveWallTester.fighterVelocity;
    for (int frame = 1; frame < passiveWallWorld.fighterDefs[0].properties.common.passiveWallTimerX760; ++frame) {
        pf::tickWorld(passiveWallWorld, {passiveWallInput, pf::InputFrame{}});
    }
    std::cout << "passive_wall_hold_vel=" << pf::toString(passiveWallHeldVelocity)
              << " passive_wall_kick_vel=" << pf::toString(passiveWallTester.fighterVelocity)
              << "\n";

    pf::World passiveWallJumpConvertWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& passiveWallJumpConvertTester = passiveWallJumpConvertWorld.fighters[0];
    passiveWallJumpConvertTester.position.y = pf::fx(8);
    passiveWallJumpConvertTester.previousPosition = passiveWallJumpConvertTester.position;
    passiveWallJumpConvertTester.grounded = false;
    passiveWallJumpConvertTester.groundSegment = -1;
    passiveWallJumpConvertTester.facing = -1;
    pf::changeFighterState(passiveWallJumpConvertWorld, passiveWallJumpConvertTester, "PassiveWall");
    pf::InputFrame passiveWallJumpConvertInput;
    passiveWallJumpConvertInput.buttons |= pf::ButtonJump;
    for (int frame = 0; frame < passiveWallJumpConvertWorld.fighterDefs[0].properties.common.passiveWallTimerX760; ++frame) {
        pf::tickWorld(passiveWallJumpConvertWorld, {passiveWallJumpConvertInput, pf::InputFrame{}});
    }
    std::cout << "passive_wall_jump_convert_state=" << pf::currentState(passiveWallJumpConvertWorld, passiveWallJumpConvertTester).name
              << " passive_wall_jump_convert_vel=" << pf::toString(passiveWallJumpConvertTester.fighterVelocity)
              << "\n";

    pf::World passiveCeilWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& passiveCeilTester = passiveCeilWorld.fighters[0];
    passiveCeilTester.position.y = pf::fx(8);
    passiveCeilTester.previousPosition = passiveCeilTester.position;
    passiveCeilTester.grounded = false;
    passiveCeilTester.groundSegment = -1;
    pf::changeFighterState(passiveCeilWorld, passiveCeilTester, "PassiveCeil");
    pf::InputFrame passiveCeilInput;
    passiveCeilInput.move.x = pf::fx(1);
    pf::tickWorld(passiveCeilWorld, {passiveCeilInput, pf::InputFrame{}});
    const pf::Vec2 passiveCeilNoFlagVelocity = passiveCeilTester.fighterVelocity;
    passiveCeilTester.fighterVelocity = {};
    passiveCeilTester.position.y = pf::fx(8);
    passiveCeilTester.previousPosition = passiveCeilTester.position;
    pf::setFighterThrowFlag(passiveCeilTester, 3, true);
    pf::tickWorld(passiveCeilWorld, {passiveCeilInput, pf::InputFrame{}});
    std::cout << "passive_ceil_no_flag_vel=" << pf::toString(passiveCeilNoFlagVelocity)
              << " passive_ceil_flag_vel=" << pf::toString(passiveCeilTester.fighterVelocity)
              << "\n";

    pf::World cliffJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffJumpTester = setOnLeftLedge(cliffJumpWorld);
    pf::InputFrame jumpInput;
    jumpInput.buttons |= pf::ButtonJump;
    pf::tickWorld(cliffJumpWorld, {jumpInput, pf::InputFrame{}});
    for (int frame = 0; frame < 60 && pf::currentState(cliffJumpWorld, cliffJumpTester).name == "CliffJumpQuick1"; ++frame) {
        pf::tickWorld(cliffJumpWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "cliff_jump_state=" << pf::currentState(cliffJumpWorld, cliffJumpTester).name
              << " vel=" << pf::toString(cliffJumpTester.fighterVelocity)
              << "\n";

    pf::World cliffJumpAirDelayWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffJumpAirDelayTester = setOnLeftLedge(cliffJumpAirDelayWorld);
    pf::changeFighterState(cliffJumpAirDelayWorld, cliffJumpAirDelayTester, "CliffJumpQuick2");
    const pf::Fix cliffJumpAirStartVelY = cliffJumpAirDelayTester.fighterVelocity.y;
    pf::tickWorld(cliffJumpAirDelayWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "cliff_jump_air_first_vel_y=" << pf::fxToFloat(cliffJumpAirDelayTester.fighterVelocity.y)
              << " cliff_jump_air_start_vel_y=" << pf::fxToFloat(cliffJumpAirStartVelY)
              << "\n";

    pf::World cliffClimbWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffClimbTester = setOnLeftLedge(cliffClimbWorld);
    cliffClimbTester.ledgeActionReady = true;
    cliffClimbTester.input.frames[0].move.y = pf::fx(1);
    cliffClimbTester.stickYTiltTimer = cliffClimbWorld.fighterDefs[0].properties.common.tapJumpWindowX74;
    pf::InputFrame climbInput;
    climbInput.move.y = pf::fx(1);
    pf::tickWorld(cliffClimbWorld, {climbInput, pf::InputFrame{}});
    for (int frame = 0; frame < 31; ++frame) {
        pf::tickWorld(cliffClimbWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "cliff_climb_state=" << pf::currentState(cliffClimbWorld, cliffClimbTester).name
              << " grounded=" << cliffClimbTester.grounded
              << " pos=" << pf::toString(cliffClimbTester.position)
              << "\n";

    pf::World cliffDropWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffDropTester = setOnLeftLedge(cliffDropWorld);
    pf::InputFrame dropInput;
    dropInput.move.y = -pf::fx(1);
    pf::tickWorld(cliffDropWorld, {dropInput, pf::InputFrame{}});
    const std::string gatedCliffDropState = pf::currentState(cliffDropWorld, cliffDropTester).name;
    pf::tickWorld(cliffDropWorld, {pf::InputFrame{}, pf::InputFrame{}});
    pf::tickWorld(cliffDropWorld, {dropInput, pf::InputFrame{}});
    std::cout << "cliff_drop_gated_state=" << gatedCliffDropState
              << " cliff_drop_state=" << pf::currentState(cliffDropWorld, cliffDropTester).name
              << " ledge=" << cliffDropTester.grabbedLedge
              << " vel=" << pf::toString(cliffDropTester.fighterVelocity)
              << "\n";

    pf::World cliffTimeoutWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffTimeoutTester = setOnLeftLedge(cliffTimeoutWorld);
    cliffTimeoutTester.ledgeWaitTimer = 1;
    pf::tickWorld(cliffTimeoutWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "cliff_timeout_state=" << pf::currentState(cliffTimeoutWorld, cliffTimeoutTester).name
              << " ledge=" << cliffTimeoutTester.grabbedLedge
              << " cooldown=" << cliffTimeoutTester.ledgeCooldown
              << "\n";

    pf::World cliffDamageWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffDamageVictim = setFighterOnLeftLedge(cliffDamageWorld, 1);
    pf::FighterRuntime& cliffDamageAttacker = cliffDamageWorld.fighters[0];
    pf::HitboxDefinition cliffDamageHitbox;
    cliffDamageHitbox.damage = pf::fx(5);
    cliffDamageHitbox.knockbackAngleDegrees = pf::fx(45);
    cliffDamageHitbox.knockbackBase = pf::fx(80);
    cliffDamageHitbox.knockbackGrowth = pf::fx(20);
    cliffDamageHitbox.radius = pf::fx(6);
    const pf::Vec3 cliffDamagePoint{cliffDamageVictim.position.x, cliffDamageVictim.position.y, 0};
    cliffDamageAttacker.activeHitboxes.push_back({cliffDamageHitbox, false, cliffDamagePoint, cliffDamagePoint});
    pf::tickWorld(cliffDamageWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "cliff_damage_state=" << pf::currentState(cliffDamageWorld, cliffDamageVictim).name
              << " ledge=" << cliffDamageVictim.grabbedLedge
              << " cooldown=" << cliffDamageVictim.ledgeCooldown
              << "\n";

    pf::World cliffAttackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffAttackTester = setOnLeftLedge(cliffAttackWorld);
    pf::InputFrame attackInput;
    attackInput.buttons |= pf::ButtonAttack;
    pf::tickWorld(cliffAttackWorld, {attackInput, pf::InputFrame{}});
    std::cout << "cliff_attack_state=" << pf::currentState(cliffAttackWorld, cliffAttackTester).name
              << "\n";

    pf::World cliffAttackAttachWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffAttackAttachTester = setOnLeftLedge(cliffAttackAttachWorld);
    pf::changeFighterState(cliffAttackAttachWorld, cliffAttackAttachTester, "CliffAttackQuick");
    for (int frame = 0; frame < pf::currentState(cliffAttackAttachWorld, cliffAttackAttachTester).animationLengthFrames - 2 &&
        pf::currentState(cliffAttackAttachWorld, cliffAttackAttachTester).name == "CliffAttackQuick" &&
        !cliffAttackAttachTester.grounded; ++frame)
    {
        pf::tickWorld(cliffAttackAttachWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "cliff_attack_attach_grounded=" << cliffAttackAttachTester.grounded
              << " cliff_attack_attach_ledge=" << cliffAttackAttachTester.grabbedLedge
              << "\n";

    auto finishLedgeActionProbe = [](const std::string& stateName) {
        pf::World finishWorld = pf::makeTrainingWorld();
        pf::FighterRuntime& tester = setOnLeftLedge(finishWorld);
        pf::changeFighterState(finishWorld, tester, stateName);
        const int finishFrames = pf::currentState(finishWorld, tester).animationLengthFrames + 2;
        for (int frame = 0; frame < finishFrames; ++frame) {
            pf::tickWorld(finishWorld, {pf::InputFrame{}, pf::InputFrame{}});
        }
        return pf::currentState(finishWorld, tester).name;
    };
    const std::string cliffClimbFinishState = finishLedgeActionProbe("CliffClimbQuick");
    const std::string cliffEscapeFinishState = finishLedgeActionProbe("CliffEscapeQuick");
    const std::string cliffAttackFinishState = finishLedgeActionProbe("CliffAttackQuick");
    std::cout << "cliff_finish_states=" << cliffClimbFinishState
              << "," << cliffEscapeFinishState
              << "," << cliffAttackFinishState
              << "\n";

    pf::World cliffSpecialAttackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffSpecialAttackTester = setOnLeftLedge(cliffSpecialAttackWorld);
    pf::InputFrame specialInput;
    specialInput.buttons |= pf::ButtonSpecial;
    pf::tickWorld(cliffSpecialAttackWorld, {specialInput, pf::InputFrame{}});
    std::cout << "cliff_special_attack_state=" << pf::currentState(cliffSpecialAttackWorld, cliffSpecialAttackTester).name
              << "\n";

    pf::World cliffCStickAttackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffCStickAttackTester = setOnLeftLedge(cliffCStickAttackWorld);
    pf::InputFrame cStickAttackInput;
    cStickAttackInput.cStick.y = pf::fx(1);
    pf::tickWorld(cliffCStickAttackWorld, {cStickAttackInput, pf::InputFrame{}});

    pf::World cliffCStickEscapeWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffCStickEscapeTester = setOnLeftLedge(cliffCStickEscapeWorld);
    pf::InputFrame cStickEscapeInput;
    cStickEscapeInput.cStick.x = cliffCStickEscapeTester.facing * pf::fx(1);
    pf::tickWorld(cliffCStickEscapeWorld, {cStickEscapeInput, pf::InputFrame{}});

    pf::World cliffCStickDropWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffCStickDropTester = setOnLeftLedge(cliffCStickDropWorld);
    cliffCStickDropTester.ledgeActionReady = true;
    pf::InputFrame cStickDropInput;
    cStickDropInput.cStick.y = -pf::fx(1);
    pf::tickWorld(cliffCStickDropWorld, {cStickDropInput, pf::InputFrame{}});
    std::cout << "cliff_cstick_attack_state=" << pf::currentState(cliffCStickAttackWorld, cliffCStickAttackTester).name
              << " cliff_cstick_escape_state=" << pf::currentState(cliffCStickEscapeWorld, cliffCStickEscapeTester).name
              << " cliff_cstick_drop_state=" << pf::currentState(cliffCStickDropWorld, cliffCStickDropTester).name
              << "\n";

    pf::World cliffSlowOptionsWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffSlowOptionsTester = setOnLeftLedge(cliffSlowOptionsWorld);
    cliffSlowOptionsTester.percent = pf::fx(120);
    pf::tickWorld(cliffSlowOptionsWorld, {attackInput, pf::InputFrame{}});
    pf::World cliffSlowJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffSlowJumpTester = setOnLeftLedge(cliffSlowJumpWorld);
    cliffSlowJumpTester.percent = pf::fx(120);
    pf::tickWorld(cliffSlowJumpWorld, {jumpInput, pf::InputFrame{}});
    std::cout << "cliff_slow_attack_state=" << pf::currentState(cliffSlowOptionsWorld, cliffSlowOptionsTester).name
              << " cliff_slow_jump_state=" << pf::currentState(cliffSlowJumpWorld, cliffSlowJumpTester).name
              << "\n";

    pf::World cliffReadyUpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffReadyUpTester = setOnLeftLedge(cliffReadyUpWorld);
    cliffReadyUpTester.ledgeActionReady = true;
    pf::InputFrame readyUpInput;
    readyUpInput.move.y = pf::fx(1);
    pf::tickWorld(cliffReadyUpWorld, {readyUpInput, pf::InputFrame{}});
    std::cout << "cliff_ready_up_state=" << pf::currentState(cliffReadyUpWorld, cliffReadyUpTester).name
              << "\n";

    pf::World guardWorld = pf::makeTrainingWorld();
    pf::InputFrame shieldInput;
    shieldInput.buttons |= pf::ButtonShield;
    pf::tickWorld(guardWorld, {shieldInput, pf::InputFrame{}});
    std::cout << "guard_reflect_state=" << pf::currentState(guardWorld, guardWorld.fighters[0]).name << "\n";
    for (int frame = 0; frame < 10; ++frame) {
        pf::tickWorld(guardWorld, {shieldInput, pf::InputFrame{}});
    }
    const pf::Fix guardHealth = guardWorld.fighters[0].shieldHealth;
    std::cout << "guard_hold_state=" << pf::currentState(guardWorld, guardWorld.fighters[0]).name
              << " shield=" << pf::fxToFloat(guardHealth)
              << "\n";
    for (int frame = 0; frame < 18 && pf::currentState(guardWorld, guardWorld.fighters[0]).name != "Wait"; ++frame) {
        pf::tickWorld(guardWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "guard_release_state=" << pf::currentState(guardWorld, guardWorld.fighters[0]).name
              << " shield_now=" << pf::fxToFloat(guardWorld.fighters[0].shieldHealth)
              << " shield_delta=" << pf::fxToFloat(guardWorld.fighterDefs[0].shield.maxHealth - guardHealth)
              << "\n";

    pf::World guardOnRepressWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& guardOnRepressTester = guardOnRepressWorld.fighters[0];
    pf::changeFighterState(guardOnRepressWorld, guardOnRepressTester, "GuardOn");
    pf::InputFrame guardRepressInput;
    guardRepressInput.buttons = pf::ButtonShield;
    pf::tickWorld(guardOnRepressWorld, {guardRepressInput, pf::InputFrame{}});

    pf::World guardRepressWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& guardRepressTester = guardRepressWorld.fighters[0];
    pf::changeFighterState(guardRepressWorld, guardRepressTester, "Guard");
    pf::tickWorld(guardRepressWorld, {guardRepressInput, pf::InputFrame{}});
    std::cout << "guard_on_repress_state=" << pf::currentState(guardOnRepressWorld, guardOnRepressTester).name
              << " guard_repress_state=" << pf::currentState(guardRepressWorld, guardRepressTester).name
              << "\n";

    pf::World guardGrabDownWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& guardGrabDownTester = guardGrabDownWorld.fighters[0];
    pf::changeFighterState(guardGrabDownWorld, guardGrabDownTester, "Guard");
    pf::InputFrame guardGrabDownInput;
    guardGrabDownInput.buttons = pf::ButtonGrab;
    guardGrabDownInput.move.y = -pf::fx(1);
    pf::tickWorld(guardGrabDownWorld, {guardGrabDownInput, pf::InputFrame{}});

    pf::World guardGrabSideWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& guardGrabSideTester = guardGrabSideWorld.fighters[0];
    pf::changeFighterState(guardGrabSideWorld, guardGrabSideTester, "Guard");
    pf::InputFrame guardGrabSideInput;
    guardGrabSideInput.buttons = pf::ButtonGrab;
    guardGrabSideInput.move.x = pf::fx(1);
    pf::tickWorld(guardGrabSideWorld, {guardGrabSideInput, pf::InputFrame{}});

    pf::World guardGrabWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& guardGrabTester = guardGrabWorld.fighters[0];
    pf::changeFighterState(guardGrabWorld, guardGrabTester, "Guard");
    pf::InputFrame guardGrabInput;
    guardGrabInput.buttons = pf::ButtonGrab;
    pf::tickWorld(guardGrabWorld, {guardGrabInput, pf::InputFrame{}});

    pf::World guardSetoffGrabWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& guardSetoffGrabTester = guardSetoffGrabWorld.fighters[0];
    pf::changeFighterState(guardSetoffGrabWorld, guardSetoffGrabTester, "GuardSetOff");
    pf::tickWorld(guardSetoffGrabWorld, {guardGrabInput, pf::InputFrame{}});
    std::cout << "guard_grab_down_state=" << pf::currentState(guardGrabDownWorld, guardGrabDownTester).name
              << " guard_grab_side_state=" << pf::currentState(guardGrabSideWorld, guardGrabSideTester).name
              << " guard_grab_state=" << pf::currentState(guardGrabWorld, guardGrabTester).name
              << " guard_setoff_grab_state=" << pf::currentState(guardSetoffGrabWorld, guardSetoffGrabTester).name
              << "\n";

    pf::World guardOffSpotWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& guardOffSpotTester = guardOffSpotWorld.fighters[0];
    pf::changeFighterState(guardOffSpotWorld, guardOffSpotTester, "GuardOff");
    pf::InputFrame guardOffSpotInput;
    guardOffSpotInput.buttons = pf::ButtonShield;
    guardOffSpotInput.move.y = -pf::fx(1);
    pf::tickWorld(guardOffSpotWorld, {guardOffSpotInput, pf::InputFrame{}});
    std::cout << "guard_off_spotdodge_state=" << pf::currentState(guardOffSpotWorld, guardOffSpotTester).name
              << "\n";

    pf::World guardOffGrabWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& guardOffGrabTester = guardOffGrabWorld.fighters[0];
    pf::changeFighterState(guardOffGrabWorld, guardOffGrabTester, "GuardOff");
    pf::tickWorld(guardOffGrabWorld, {guardGrabInput, pf::InputFrame{}});

    pf::World guardOffAttackJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& guardOffAttackJumpTester = guardOffAttackJumpWorld.fighters[0];
    pf::changeFighterState(guardOffAttackJumpWorld, guardOffAttackJumpTester, "GuardOff");
    pf::InputFrame guardOffAttackJumpInput;
    guardOffAttackJumpInput.buttons = pf::ButtonAttack | pf::ButtonJump;
    pf::tickWorld(guardOffAttackJumpWorld, {guardOffAttackJumpInput, pf::InputFrame{}});
    std::cout << "guard_off_grab_state=" << pf::currentState(guardOffGrabWorld, guardOffGrabTester).name
              << " guard_off_attack_jump_state=" << pf::currentState(guardOffAttackJumpWorld, guardOffAttackJumpTester).name
              << "\n";

    pf::World guardOffJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& guardOffJumpTester = guardOffJumpWorld.fighters[0];
    pf::changeFighterState(guardOffJumpWorld, guardOffJumpTester, "GuardOff");
    pf::InputFrame guardOffJumpInput;
    guardOffJumpInput.buttons = pf::ButtonJump;
    pf::tickWorld(guardOffJumpWorld, {guardOffJumpInput, pf::InputFrame{}});
    std::cout << "guard_off_jump_state=" << pf::currentState(guardOffJumpWorld, guardOffJumpTester).name
              << "\n";

    pf::World shieldHitWorld = pf::makeTrainingWorld();
    shieldHitWorld.fighters[0].position = {-pf::fx(1), 0};
    shieldHitWorld.fighters[1].position = {pf::fx(1), 0};
    pf::Fix p2ShieldStart = shieldHitWorld.fighters[1].shieldHealth;
    pf::Fix maxP1ShieldKb = 0;
    for (int frame = 0; frame < 20; ++frame) {
        pf::InputFrame p1Input;
        pf::InputFrame p2Input;
        p2Input.buttons |= pf::ButtonShield;
        if (frame == 5) {
            p1Input.buttons |= pf::ButtonAttack;
        }
        pf::tickWorld(shieldHitWorld, {p1Input, p2Input});
        maxP1ShieldKb = std::max(maxP1ShieldKb, pf::fxAbs(shieldHitWorld.fighters[0].attackerShieldKnockback.x));
    }
    std::cout << "shield_hit_state=" << pf::currentState(shieldHitWorld, shieldHitWorld.fighters[1]).name
              << " p2_percent=" << pf::fxToFloat(shieldHitWorld.fighters[1].percent)
              << " shield_loss=" << pf::fxToFloat(p2ShieldStart - shieldHitWorld.fighters[1].shieldHealth)
              << " p1_ground_vel=" << pf::fxToFloat(shieldHitWorld.fighters[0].groundVelocity)
              << " p1_shield_kb=" << pf::toString(shieldHitWorld.fighters[0].attackerShieldKnockback)
              << " p1_max_shield_kb=" << pf::fxToFloat(maxP1ShieldKb)
              << " p2_ground_vel=" << pf::fxToFloat(shieldHitWorld.fighters[1].groundVelocity)
              << "\n";

    pf::World shieldSdiWorld = pf::makeTrainingWorld();
    shieldSdiWorld.fighters[1].position = {pf::fx(1), 0};
    pf::changeFighterState(shieldSdiWorld, shieldSdiWorld.fighters[1], "GuardSetOff");
    shieldSdiWorld.fighters[1].hitlag = 1;
    const pf::Fix shieldSdiStartX = shieldSdiWorld.fighters[1].position.x;
    pf::InputFrame shieldSdiInput;
    shieldSdiInput.buttons |= pf::ButtonShield;
    shieldSdiInput.move.x = pf::fx(1);
    pf::tickWorld(shieldSdiWorld, {pf::InputFrame{}, shieldSdiInput});
    std::cout << "shield_sdi_delta=" << pf::fxToFloat(shieldSdiWorld.fighters[1].position.x - shieldSdiStartX)
              << " state=" << pf::currentState(shieldSdiWorld, shieldSdiWorld.fighters[1]).name
              << " hitlag=" << shieldSdiWorld.fighters[1].hitlag
              << "\n";

    pf::World shieldBreakWorld = pf::makeTrainingWorld();
    shieldBreakWorld.fighters[0].position = {-pf::fx(1), 0};
    shieldBreakWorld.fighters[1].position = {pf::fx(10), 0};
    shieldBreakWorld.fighters[1].shieldHealth = pf::fx(1);
    for (int frame = 0; frame < 20; ++frame) {
        pf::InputFrame p1Input;
        pf::InputFrame p2Input;
        p2Input.buttons |= pf::ButtonShield;
        if (frame == 5) {
            p1Input.buttons |= pf::ButtonAttack;
        }
        pf::tickWorld(shieldBreakWorld, {p1Input, p2Input});
    }
    std::cout << "shield_break_state=" << pf::currentState(shieldBreakWorld, shieldBreakWorld.fighters[1]).name
              << " shield=" << pf::fxToFloat(shieldBreakWorld.fighters[1].shieldHealth)
              << " vel=" << pf::toString(shieldBreakWorld.fighters[1].fighterVelocity)
              << "\n";

    pf::World shieldBreakLandingWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& shieldBreakLandingTester = shieldBreakLandingWorld.fighters[0];
    shieldBreakLandingTester.position.y = pf::fxFromFloat(0.2f);
    shieldBreakLandingTester.previousPosition = {shieldBreakLandingTester.position.x, pf::fx(2)};
    shieldBreakLandingTester.grounded = false;
    shieldBreakLandingTester.groundSegment = -1;
    shieldBreakLandingTester.fighterVelocity.y = -pf::fx(1);
    pf::changeFighterState(shieldBreakLandingWorld, shieldBreakLandingTester, "ShieldBreakFall");
    for (int frame = 0; frame < 20 &&
        pf::currentState(shieldBreakLandingWorld, shieldBreakLandingTester).name == "ShieldBreakFall"; ++frame)
    {
        pf::tickWorld(shieldBreakLandingWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }

    pf::World shieldBreakDownDWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& shieldBreakDownDTester = shieldBreakDownDWorld.fighters[0];
    pf::changeFighterState(shieldBreakDownDWorld, shieldBreakDownDTester, "ShieldBreakDownD");
    shieldBreakDownDTester.lastStateChangeFrame = shieldBreakDownDTester.internalFrame - 1000;
    pf::tickWorld(shieldBreakDownDWorld, {pf::InputFrame{}, pf::InputFrame{}});

    std::cout << "shield_break_landing_state=" << pf::currentState(shieldBreakLandingWorld, shieldBreakLandingTester).name
              << " shield_break_down_d_finish_state=" << pf::currentState(shieldBreakDownDWorld, shieldBreakDownDTester).name
              << "\n";

    pf::World shieldBreakFallClampWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& shieldBreakFallClampTester = shieldBreakFallClampWorld.fighters[0];
    shieldBreakFallClampTester.grounded = false;
    shieldBreakFallClampTester.groundSegment = -1;
    shieldBreakFallClampTester.position.y = pf::fx(8);
    shieldBreakFallClampTester.previousPosition = shieldBreakFallClampTester.position;
    shieldBreakFallClampTester.fighterVelocity.x =
        shieldBreakFallClampWorld.fighterDefs[0].properties.airDriftMax + pf::fx(2);
    pf::changeFighterState(shieldBreakFallClampWorld, shieldBreakFallClampTester, "ShieldBreakFall");
    std::cout << "shield_break_fall_clamp_vel_x=" << pf::fxToFloat(shieldBreakFallClampTester.fighterVelocity.x)
              << " shield_break_fall_clamp_max=" << pf::fxToFloat(shieldBreakFallClampWorld.fighterDefs[0].properties.airDriftMax)
              << "\n";

    pf::World shieldBreakNoDriftWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& shieldBreakNoDriftTester = shieldBreakNoDriftWorld.fighters[0];
    shieldBreakNoDriftTester.grounded = false;
    shieldBreakNoDriftTester.groundSegment = -1;
    shieldBreakNoDriftTester.position.y = pf::fx(8);
    shieldBreakNoDriftTester.previousPosition = shieldBreakNoDriftTester.position;
    shieldBreakNoDriftTester.fighterVelocity.x = 0;
    pf::changeFighterState(shieldBreakNoDriftWorld, shieldBreakNoDriftTester, "ShieldBreakFall");
    pf::InputFrame shieldBreakDriftInput;
    shieldBreakDriftInput.move.x = pf::fx(1);
    pf::tickWorld(shieldBreakNoDriftWorld, {shieldBreakDriftInput, pf::InputFrame{}});
    std::cout << "shield_break_no_drift_vel_x=" << pf::fxToFloat(shieldBreakNoDriftTester.fighterVelocity.x)
              << "\n";

    pf::World furafuraRouteWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& furafuraRouteTester = furafuraRouteWorld.fighters[0];
    pf::changeFighterState(furafuraRouteWorld, furafuraRouteTester, "ShieldBreakStandU");
    furafuraRouteTester.lastStateChangeFrame = furafuraRouteTester.internalFrame - 1000;
    pf::tickWorld(furafuraRouteWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World furafuraMashWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& furafuraMashTester = furafuraMashWorld.fighters[0];
    pf::changeFighterState(furafuraMashWorld, furafuraMashTester, "Furafura");
    furafuraMashTester.grabTimer = pf::fx(20);
    pf::InputFrame furafuraMashInput;
    furafuraMashInput.buttons = pf::ButtonSpecial;
    pf::tickWorld(furafuraMashWorld, {furafuraMashInput, pf::InputFrame{}});

    pf::World furafuraFinishWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& furafuraFinishTester = furafuraFinishWorld.fighters[0];
    pf::changeFighterState(furafuraFinishWorld, furafuraFinishTester, "Furafura");
    furafuraFinishTester.grabTimer = furafuraFinishWorld.fighterDefs[0].properties.common.furafuraTimerDecrementX300;
    pf::tickWorld(furafuraFinishWorld, {pf::InputFrame{}, pf::InputFrame{}});

    std::cout << "furafura_route_state=" << pf::currentState(furafuraRouteWorld, furafuraRouteTester).name
              << " furafura_shield=" << pf::fxToFloat(furafuraRouteTester.shieldHealth)
              << " furafura_mash_timer=" << pf::fxToFloat(furafuraMashTester.grabTimer)
              << " furafura_finish_state=" << pf::currentState(furafuraFinishWorld, furafuraFinishTester).name
              << "\n";

    pf::World spotDodgeWorld = pf::makeTrainingWorld();
    spotDodgeWorld.fighters[0].position = {-pf::fx(1), 0};
    spotDodgeWorld.fighters[1].position = {pf::fx(1), 0};
    for (int frame = 0; frame < 20; ++frame) {
        pf::InputFrame p1Input;
        pf::InputFrame p2Input;
        p2Input.buttons |= pf::ButtonShield;
        p2Input.move.y = -pf::fx(1);
        if (frame == 5) {
            p1Input.buttons |= pf::ButtonAttack;
        }
        pf::tickWorld(spotDodgeWorld, {p1Input, p2Input});
    }
    std::cout << "spot_dodge_state=" << pf::currentState(spotDodgeWorld, spotDodgeWorld.fighters[1]).name
              << " p2_percent=" << pf::fxToFloat(spotDodgeWorld.fighters[1].percent)
              << " hurtbox0=" << static_cast<int>(spotDodgeWorld.fighters[1].hurtboxStates[0])
              << "\n";

    pf::World invincibleWorld = pf::makeTrainingWorld();
    invincibleWorld.fighters[0].position = {-pf::fx(1), 0};
    invincibleWorld.fighters[1].position = {pf::fx(1), 0};
    invincibleWorld.fighters[1].hurtboxStates.assign(invincibleWorld.fighterDefs[invincibleWorld.fighters[1].fighterDef].hurtboxes.size(), pf::HurtboxState::Invincible);
    int maxInvincibleHitlag = 0;
    for (int frame = 0; frame < 20; ++frame) {
        pf::InputFrame p1Input;
        if (frame == 5) {
            p1Input.buttons |= pf::ButtonAttack;
        }
        pf::tickWorld(invincibleWorld, {p1Input, pf::InputFrame{}});
        maxInvincibleHitlag = std::max(maxInvincibleHitlag, invincibleWorld.fighters[0].hitlag);
    }
    std::cout << "invincible_hit_percent=" << pf::fxToFloat(invincibleWorld.fighters[1].percent)
              << " max_attacker_hitlag=" << maxInvincibleHitlag
              << " state=" << pf::currentState(invincibleWorld, invincibleWorld.fighters[1]).name
              << "\n";

    pf::World smashChargeWorld = pf::makeTrainingWorld();
    smashChargeWorld.fighters[1].position = {pf::fx(20), 0};
    for (int frame = 0; frame < 30; ++frame) {
        pf::InputFrame p1Input;
        p1Input.buttons |= pf::ButtonAttack;
        p1Input.move.x = pf::fx(1);
        pf::tickWorld(smashChargeWorld, {p1Input, pf::InputFrame{}});
    }
    pf::tickWorld(smashChargeWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "smash_charge_state=" << pf::currentState(smashChargeWorld, smashChargeWorld.fighters[0]).name
              << " charge_state=" << smashChargeWorld.fighters[0].smashChargeState
              << " charge_frames=" << pf::fxToFloat(smashChargeWorld.fighters[0].smashChargeFrames)
              << " anim_rate=" << pf::fxToFloat(smashChargeWorld.fighters[0].animationRate)
              << "\n";

    pf::World objectWorld = pf::makeTrainingWorld();
    objectWorld.fighters[0].position = {-pf::fx(1), 0};
    objectWorld.fighters[1].position = {0, 0};
    const int objectIndex = pf::spawnGameObject(
        objectWorld,
        "TrainingLaser",
        0,
        {-pf::fxFromFloat(0.5f), pf::fxFromFloat(1.3f)},
        1,
        {pf::fxFromFloat(0.25f), 0});
    pf::WorldSnapshot objectSnapshot = pf::saveWorld(objectWorld);
    for (int frame = 0; frame < 8; ++frame) {
        pf::tickWorld(objectWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "object_spawn_index=" << objectIndex
              << " object_snapshot_defs=" << objectSnapshot.objectDefs.size()
              << " object_snapshot_count=" << objectSnapshot.objects.size()
              << " object_after_count=" << objectWorld.objects.size()
              << " object_hit_percent=" << pf::fxToFloat(objectWorld.fighters[1].percent)
              << " object_hit_state=" << pf::currentState(objectWorld, objectWorld.fighters[1]).name
              << "\n";
    pf::loadWorld(objectWorld, objectSnapshot);
    std::cout << "object_restore_count=" << objectWorld.objects.size()
              << " object_restore_x=" << pf::fxToFloat(objectWorld.objects.empty() ? 0 : objectWorld.objects[0].position.x)
              << "\n";

    pf::World objectClankWorld = pf::makeTrainingWorld();
    objectClankWorld.fighters[0].position = {-pf::fx(20), 0};
    objectClankWorld.fighters[1].position = {pf::fx(20), 0};
    pf::spawnGameObject(
        objectClankWorld,
        "TrainingLaser",
        -1,
        {-pf::fxFromFloat(0.2f), pf::fxFromFloat(1.3f)},
        1,
        {pf::fxFromFloat(0.1f), 0});
    pf::spawnGameObject(
        objectClankWorld,
        "TrainingLaser",
        -1,
        {pf::fxFromFloat(0.2f), pf::fxFromFloat(1.3f)},
        -1,
        {-pf::fxFromFloat(0.1f), 0});
    for (int frame = 0; frame < 3; ++frame) {
        pf::tickWorld(objectClankWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "object_clank_count=" << objectClankWorld.objects.size()
              << "\n";

    pf::World objectDamageWorld = pf::makeTrainingWorld();
    objectDamageWorld.fighters[1].position = {pf::fx(20), 0};
    const pf::Vec3 hip = objectDamageWorld.fighters[0].bones[static_cast<size_t>(pf::BoneId::Hip)].position;
    pf::ActiveHitbox objectDamageHitbox;
    objectDamageHitbox.def.hitboxId = 0;
    objectDamageHitbox.def.bone = pf::BoneId::Hip;
    objectDamageHitbox.def.radius = pf::fxFromFloat(0.5f);
    objectDamageHitbox.def.damage = pf::fx(4);
    objectDamageWorld.fighters[0].activeHitboxes.push_back(objectDamageHitbox);
    pf::spawnGameObject(
        objectDamageWorld,
        "TrainingItem",
        -1,
        {objectDamageWorld.fighters[0].position.x + hip.x, objectDamageWorld.fighters[0].position.y + hip.y},
        1,
        {});
    pf::tickWorld(objectDamageWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "object_damage_count=" << objectDamageWorld.objects.size()
              << "\n";

    pf::World objectEnteredAirWorld = pf::makeTrainingWorld();
    objectEnteredAirWorld.fighters[1].position = {pf::fx(20), 0};
    const int enteredAirIndex = pf::spawnGameObject(
        objectEnteredAirWorld,
        "TrainingAirEventItem",
        -1,
        {0, 0},
        1,
        {0, pf::fxFromFloat(0.5f)});
    if (enteredAirIndex >= 0 && enteredAirIndex < static_cast<int>(objectEnteredAirWorld.objects.size())) {
        objectEnteredAirWorld.objects[static_cast<size_t>(enteredAirIndex)].grounded = true;
        objectEnteredAirWorld.objects[static_cast<size_t>(enteredAirIndex)].groundSegment = 0;
    }
    pf::tickWorld(objectEnteredAirWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "object_entered_air_count=" << objectEnteredAirWorld.objects.size()
              << "\n";

    pf::World objectStateEnterWorld = pf::makeTrainingWorld();
    objectStateEnterWorld.fighters[1].position = {pf::fx(20), 0};
    pf::spawnGameObject(
        objectStateEnterWorld,
        "TrainingStateEnterItem",
        -1,
        {0, pf::fx(3)},
        1,
        {});
    pf::tickWorld(objectStateEnterWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "object_state_enter_count=" << objectStateEnterWorld.objects.size()
              << "\n";

    pf::World objectHitlagEnterWorld = pf::makeTrainingWorld();
    objectHitlagEnterWorld.fighters[1].position = {pf::fx(20), 0};
    const pf::Vec3 hitlagHip = objectHitlagEnterWorld.fighters[0].bones[static_cast<size_t>(pf::BoneId::Hip)].position;
    pf::ActiveHitbox objectHitlagHitbox;
    objectHitlagHitbox.def.hitboxId = 0;
    objectHitlagHitbox.def.bone = pf::BoneId::Hip;
    objectHitlagHitbox.def.radius = pf::fxFromFloat(0.5f);
    objectHitlagHitbox.def.damage = pf::fx(3);
    objectHitlagEnterWorld.fighters[0].activeHitboxes.push_back(objectHitlagHitbox);
    pf::spawnGameObject(
        objectHitlagEnterWorld,
        "TrainingHitlagEnterItem",
        -1,
        {objectHitlagEnterWorld.fighters[0].position.x + hitlagHip.x, objectHitlagEnterWorld.fighters[0].position.y + hitlagHip.y},
        1,
        {});
    pf::tickWorld(objectHitlagEnterWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "object_hitlag_enter_count=" << objectHitlagEnterWorld.objects.size()
              << "\n";

    pf::World objectHitlagExitWorld = pf::makeTrainingWorld();
    objectHitlagExitWorld.fighters[1].position = {pf::fx(20), 0};
    const int hitlagExitIndex = pf::spawnGameObject(
        objectHitlagExitWorld,
        "TrainingHitlagExitItem",
        -1,
        {0, pf::fx(3)},
        1,
        {});
    if (hitlagExitIndex >= 0 && hitlagExitIndex < static_cast<int>(objectHitlagExitWorld.objects.size())) {
        objectHitlagExitWorld.objects[static_cast<size_t>(hitlagExitIndex)].hitlag = 1;
    }
    pf::tickWorld(objectHitlagExitWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "object_hitlag_exit_count=" << objectHitlagExitWorld.objects.size()
              << "\n";

    pf::World objectAccessoryWorld = pf::makeTrainingWorld();
    objectAccessoryWorld.fighters[1].position = {pf::fx(20), 0};
    pf::spawnGameObject(
        objectAccessoryWorld,
        "TrainingAccessoryItem",
        -1,
        {0, pf::fx(3)},
        1,
        {});
    pf::tickWorld(objectAccessoryWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "object_accessory_count=" << objectAccessoryWorld.objects.size()
              << "\n";

    pf::World objectTouchWorld = pf::makeTrainingWorld();
    objectTouchWorld.fighters[1].position = {pf::fx(20), 0};
    pf::spawnGameObject(
        objectTouchWorld,
        "TrainingTouchItem",
        -1,
        {objectTouchWorld.fighters[0].position.x, pf::fxFromFloat(1.3f)},
        1,
        {});
    pf::tickWorld(objectTouchWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "object_touch_count=" << objectTouchWorld.objects.size()
              << "\n";

    pf::World objectJumpedOnWorld = pf::makeTrainingWorld();
    objectJumpedOnWorld.fighters[1].position = {pf::fx(20), 0};
    objectJumpedOnWorld.fighters[0].grounded = false;
    objectJumpedOnWorld.fighters[0].position = {0, 0};
    objectJumpedOnWorld.fighters[0].previousPosition = {0, pf::fxFromFloat(3.2f)};
    objectJumpedOnWorld.fighters[0].hitlag = 1;
    pf::spawnGameObject(
        objectJumpedOnWorld,
        "TrainingJumpedOnItem",
        -1,
        {0, pf::fxFromFloat(1.3f)},
        1,
        {});
    pf::tickWorld(objectJumpedOnWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "object_jumped_on_count=" << objectJumpedOnWorld.objects.size()
              << " object_jumped_on_owner=" << (objectJumpedOnWorld.objects.empty() ? -2 : objectJumpedOnWorld.objects[0].ownerFighter)
              << "\n";

    pf::World objectGrabDealtWorld = pf::makeTrainingWorld();
    objectGrabDealtWorld.fighters[1].position = {pf::fx(20), 0};
    objectGrabDealtWorld.fighters[0].hsdHurtboxCapsules.clear();
    objectGrabDealtWorld.fighters[0].hurtboxStates.assign(
        objectGrabDealtWorld.fighterDefs[static_cast<size_t>(objectGrabDealtWorld.fighters[0].fighterDef)].hurtboxes.size(),
        pf::HurtboxState::Normal);
    const pf::Vec3 grabDealtHip = objectGrabDealtWorld.fighters[0].bones[static_cast<size_t>(pf::BoneId::Hip)].position;
    pf::spawnGameObject(
        objectGrabDealtWorld,
        "TrainingGrabDealtItem",
        -1,
        {objectGrabDealtWorld.fighters[0].position.x + grabDealtHip.x, objectGrabDealtWorld.fighters[0].position.y + grabDealtHip.y},
        1,
        {});
    pf::tickWorld(objectGrabDealtWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World objectGrabVictimWorld = pf::makeTrainingWorld();
    objectGrabVictimWorld.fighters[1].position = {pf::fx(20), 0};
    objectGrabVictimWorld.fighters[0].hsdHurtboxCapsules.clear();
    objectGrabVictimWorld.fighters[0].hurtboxStates.assign(
        objectGrabVictimWorld.fighterDefs[static_cast<size_t>(objectGrabVictimWorld.fighters[0].fighterDef)].hurtboxes.size(),
        pf::HurtboxState::Normal);
    const pf::Vec3 grabVictimHip = objectGrabVictimWorld.fighters[0].bones[static_cast<size_t>(pf::BoneId::Hip)].position;
    pf::spawnGameObject(
        objectGrabVictimWorld,
        "TrainingGrabVictimItem",
        -1,
        {objectGrabVictimWorld.fighters[0].position.x + grabVictimHip.x, objectGrabVictimWorld.fighters[0].position.y + grabVictimHip.y},
        1,
        {});
    pf::tickWorld(objectGrabVictimWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "object_grab_dealt_count=" << objectGrabDealtWorld.objects.size()
              << " object_grab_victim_count=" << objectGrabVictimWorld.objects.size()
              << "\n";

    pf::World objectInteractionWorld = pf::makeTrainingWorld();
    objectInteractionWorld.fighters[1].position = {pf::fx(20), 0};
    const int interactionIndex = pf::spawnGameObject(
        objectInteractionWorld,
        "TrainingInteractionItem",
        0,
        {0, pf::fx(3)},
        1,
        {});
    const bool interacted = pf::interactGameObjectWithFighter(objectInteractionWorld, interactionIndex, 0);
    pf::tickWorld(objectInteractionWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World objectObjectInteractionWorld = pf::makeTrainingWorld();
    objectObjectInteractionWorld.fighters[1].position = {pf::fx(20), 0};
    const int objectInteractionIndex = pf::spawnGameObject(
        objectObjectInteractionWorld,
        "TrainingInteractionItem",
        -1,
        {0, pf::fx(3)},
        1,
        {});
    const int referenceObjectIndex = pf::spawnGameObject(
        objectObjectInteractionWorld,
        "TrainingItem",
        -1,
        {pf::fx(3), pf::fx(3)},
        1,
        {});
    const bool objectInteracted = pf::interactGameObjects(objectObjectInteractionWorld, objectInteractionIndex, referenceObjectIndex);
    pf::tickWorld(objectObjectInteractionWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World objectClearRefWorld = pf::makeTrainingWorld();
    objectClearRefWorld.fighters[1].position = {pf::fx(20), 0};
    const int clearRefIndex = pf::spawnGameObject(
        objectClearRefWorld,
        "TrainingClearReferenceItem",
        0,
        {0, pf::fx(3)},
        1,
        {});
    if (clearRefIndex >= 0 && clearRefIndex < static_cast<int>(objectClearRefWorld.objects.size())) {
        objectClearRefWorld.objects[static_cast<size_t>(clearRefIndex)].grabVictimFighter = 0;
    }
    const bool clearRefInteracted = pf::interactGameObjectWithFighter(objectClearRefWorld, clearRefIndex, 0);
    std::cout << "object_interaction_ok=" << interacted
              << " object_interaction_count=" << objectInteractionWorld.objects.size()
              << " object_object_interaction_ok=" << objectInteracted
              << " object_object_interaction_count=" << objectObjectInteractionWorld.objects.size()
              << " object_clear_ref_ok=" << clearRefInteracted
              << " object_clear_ref_owner=" << (objectClearRefWorld.objects.empty() ? -2 : objectClearRefWorld.objects[0].ownerFighter)
              << " object_clear_ref_grab_victim=" << (objectClearRefWorld.objects.empty() ? -2 : objectClearRefWorld.objects[0].grabVictimFighter)
              << "\n";

    pf::World objectRuntimeRestoreWorld = pf::makeTrainingWorld();
    objectRuntimeRestoreWorld.fighters[1].position = {pf::fx(20), 0};
    const int runtimeRestoreIndex = pf::spawnGameObject(
        objectRuntimeRestoreWorld,
        "TrainingItem",
        0,
        {pf::fx(0), pf::fx(3)},
        1,
        {});
    if (runtimeRestoreIndex >= 0 && runtimeRestoreIndex < static_cast<int>(objectRuntimeRestoreWorld.objects.size())) {
        pf::GameObjectRuntime& runtimeObject = objectRuntimeRestoreWorld.objects[static_cast<size_t>(runtimeRestoreIndex)];
        runtimeObject.hitlag = 2;
        runtimeObject.grabVictimFighter = 0;
        runtimeObject.lastInteractionFighter = 1;
        runtimeObject.lastInteractionObject = runtimeRestoreIndex;
    }
    pf::WorldSnapshot objectRuntimeSnapshot = pf::saveWorld(objectRuntimeRestoreWorld);
    if (runtimeRestoreIndex >= 0 && runtimeRestoreIndex < static_cast<int>(objectRuntimeRestoreWorld.objects.size())) {
        pf::GameObjectRuntime& runtimeObject = objectRuntimeRestoreWorld.objects[static_cast<size_t>(runtimeRestoreIndex)];
        runtimeObject.hitlag = 0;
        runtimeObject.grabVictimFighter = -1;
        runtimeObject.lastInteractionFighter = -1;
        runtimeObject.lastInteractionObject = -1;
    }
    pf::loadWorld(objectRuntimeRestoreWorld, objectRuntimeSnapshot);
    const pf::GameObjectRuntime* restoredRuntimeObject = objectRuntimeRestoreWorld.objects.empty()
        ? nullptr
        : &objectRuntimeRestoreWorld.objects[0];
    std::cout << "object_runtime_restore_hitlag=" << (restoredRuntimeObject ? restoredRuntimeObject->hitlag : -1)
              << " object_runtime_restore_grab=" << (restoredRuntimeObject ? restoredRuntimeObject->grabVictimFighter : -1)
              << " object_runtime_restore_fighter=" << (restoredRuntimeObject ? restoredRuntimeObject->lastInteractionFighter : -1)
              << " object_runtime_restore_object=" << (restoredRuntimeObject ? restoredRuntimeObject->lastInteractionObject : -1)
              << "\n";

    pf::World objectHoldWorld = pf::makeTrainingWorld();
    objectHoldWorld.fighters[1].position = {pf::fx(20), 0};
    const int heldObjectIndex = pf::spawnGameObject(
        objectHoldWorld,
        "TrainingItem",
        -1,
        {pf::fx(0), pf::fx(3)},
        1,
        {});
    const bool pickedUp = pf::pickUpGameObject(objectHoldWorld, heldObjectIndex, 0);
    pf::tickWorld(objectHoldWorld, {pf::InputFrame{}, pf::InputFrame{}});
    pf::WorldSnapshot objectHoldSnapshot = pf::saveWorld(objectHoldWorld);
    const int heldOwner = objectHoldWorld.objects.empty() ? -2 : objectHoldWorld.objects[0].ownerFighter;
    const int heldBy = objectHoldWorld.objects.empty() ? -2 : objectHoldWorld.objects[0].heldByFighter;
    const int fighterHeld = objectHoldWorld.fighters[0].heldObject;
    const pf::Vec2 heldPosition = objectHoldWorld.objects.empty() ? pf::Vec2{} : objectHoldWorld.objects[0].position;
    const bool dropped = pf::dropGameObject(objectHoldWorld, heldObjectIndex, {pf::fxFromFloat(0.25f), pf::fxFromFloat(0.5f)});
    const bool thrown = pf::throwGameObject(objectHoldWorld, heldObjectIndex, 0, {pf::fxFromFloat(0.75f), pf::fxFromFloat(0.25f)});
    const int throwHeld = objectHoldWorld.objects.empty() ? -2 : objectHoldWorld.objects[0].heldByFighter;
    const pf::Vec2 throwVelocity = objectHoldWorld.objects.empty() ? pf::Vec2{} : objectHoldWorld.objects[0].velocity;
    pf::loadWorld(objectHoldWorld, objectHoldSnapshot);
    std::cout << "object_hold_picked=" << pickedUp
              << " object_hold_owner=" << heldOwner
              << " object_hold_by=" << heldBy
              << " object_fighter_held=" << fighterHeld
              << " object_hold_pos=" << pf::toString(heldPosition)
              << " object_drop_ok=" << dropped
              << " object_throw_ok=" << thrown
              << " object_throw_held=" << throwHeld
              << " object_throw_vel=" << pf::toString(throwVelocity)
              << " object_restore_fighter_held=" << objectHoldWorld.fighters[0].heldObject
              << " object_restore_held_by=" << (objectHoldWorld.objects.empty() ? -2 : objectHoldWorld.objects[0].heldByFighter)
              << "\n";

    pf::World objectEventWorld = pf::makeTrainingWorld();
    objectEventWorld.fighters[1].position = {pf::fx(20), 0};
    const int reflectIndex = pf::spawnGameObject(
        objectEventWorld,
        "TrainingLaser",
        0,
        {0, pf::fxFromFloat(1.3f)},
        1,
        {pf::fxFromFloat(0.5f), 0});
    const bool reflected = pf::reflectGameObject(objectEventWorld, reflectIndex, 1, {pf::fx(1), 0});
    const pf::Vec2 reflectedVelocity = objectEventWorld.objects.empty() ? pf::Vec2{} : objectEventWorld.objects[0].velocity;
    const int reflectedOwner = objectEventWorld.objects.empty() ? -2 : objectEventWorld.objects[0].ownerFighter;

    pf::World objectAbsorbWorld = pf::makeTrainingWorld();
    objectAbsorbWorld.fighters[1].position = {pf::fx(20), 0};
    const int absorbIndex = pf::spawnGameObject(
        objectAbsorbWorld,
        "TrainingLaser",
        0,
        {0, pf::fxFromFloat(1.3f)},
        1,
        {pf::fxFromFloat(0.5f), 0});
    const bool absorbed = pf::absorbGameObject(objectAbsorbWorld, absorbIndex, 1);
    pf::tickWorld(objectAbsorbWorld, {pf::InputFrame{}, pf::InputFrame{}});

    pf::World objectShieldBounceWorld = pf::makeTrainingWorld();
    objectShieldBounceWorld.fighters[1].position = {pf::fx(20), 0};
    const int bounceIndex = pf::spawnGameObject(
        objectShieldBounceWorld,
        "TrainingItem",
        0,
        {0, pf::fxFromFloat(1.3f)},
        1,
        {-pf::fxFromFloat(0.5f), 0});
    const bool shieldBounced = pf::shieldBounceGameObject(objectShieldBounceWorld, bounceIndex, 1, {pf::fx(1), 0});
    const pf::Vec2 shieldBounceVelocity = objectShieldBounceWorld.objects.empty() ? pf::Vec2{} : objectShieldBounceWorld.objects[0].velocity;
    std::cout << "object_reflect_ok=" << reflected
              << " object_reflect_owner=" << reflectedOwner
              << " object_reflect_vel=" << pf::toString(reflectedVelocity)
              << " object_absorb_ok=" << absorbed
              << " object_absorb_count=" << objectAbsorbWorld.objects.size()
              << " object_shield_bounce_ok=" << shieldBounced
              << " object_shield_bounce_vel=" << pf::toString(shieldBounceVelocity)
              << "\n";

    pf::World packageSourceWorld = pf::makeTrainingWorld();
    packageSourceWorld.fighterDefs[0].authoredSkeleton = {
        {-1, "Root", 0, {}, {}, {pf::fx(1), pf::fx(1), pf::fx(1)}},
    };
    packageSourceWorld.fighterDefs[0].authoredMesh = makeSmokeAuthoredMesh();
    packageSourceWorld.fighterDefs[0].packageVariables = {
        {"SmokeVar", 4},
        {"FrameVar", 0},
        {"StateFrameVar", 0},
        {"GroundedVar", 0},
        {"FacingVar", 0},
        {"ButtonDownVar", 0},
        {"ButtonPressedVar", 0},
        {"StickXVar", 0},
        {"StickYVar", 0},
        {"CStickXVar", 0},
        {"CStickYVar", 0},
        {"ShieldVar", 0},
        {"ScaledStickXVar", 0},
        {"PercentVar", 0},
        {"ShieldHealthVar", 0},
        {"PositionXVar", 0},
        {"PositionYVar", 0},
        {"GroundVelocityVar", 0},
        {"AirVelocityXVar", 0},
        {"AirVelocityYVar", 0},
    };
    packageSourceWorld.fighterDefs[0].packageScripts = {{
        "SmokeScript",
        8,
        {
            {pf::PackageScriptOp::AddVarImmediate, 0, -1, -1, 3, 0, {}},
            {pf::PackageScriptOp::SpawnObject, -1, -1, -1, 0, pf::fxFromFloat(0.25f), "TrainingItem"},
        },
    }, {
        "BranchScript",
        16,
        {
            {pf::PackageScriptOp::SetVarImmediate, 0, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarImmediate, 1, -1, -1, 3, 0, {}},
            {pf::PackageScriptOp::AddVarImmediate, 0, -1, -1, 1, 0, {}},
            {pf::PackageScriptOp::SkipIfVarLessThanVar, -1, 0, 1, 0, 0, {}},
            {pf::PackageScriptOp::JumpRelative, -1, -1, -1, 2, 0, {}},
            {pf::PackageScriptOp::JumpRelative, -1, -1, -1, -3, 0, {}},
        },
    }, {
        "SwitchScript",
        8,
        {
            {pf::PackageScriptOp::SwitchFighterDefinition, -1, -1, -1, 0, 0, "SmokeAlt"},
        },
    }, {
        "SpawnFighterScript",
        8,
        {
            {pf::PackageScriptOp::SpawnFighter, -1, -1, -1, 0, pf::fxFromFloat(1.0f), "SmokeAlt"},
        },
    }, {
        "FactScript",
        16,
        {
            {pf::PackageScriptOp::SetVarFrame, 1, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarStateFrame, 2, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarGrounded, 3, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarFacing, 4, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarFighterPercent, 13, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarFighterShield, 14, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarFighterPositionX, 15, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarFighterPositionY, 16, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarFighterGroundVelocity, 17, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarFighterAirVelocityX, 18, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarFighterAirVelocityY, 19, -1, -1, 0, 0, {}},
        },
    }, {
        "InputScript",
        24,
        {
            {pf::PackageScriptOp::SetVarButtonDown, 5, -1, -1, pf::ButtonPause, 0, {}},
            {pf::PackageScriptOp::SetVarButtonPressed, 6, -1, -1, pf::ButtonPause, 0, {}},
            {pf::PackageScriptOp::SetVarStickX, 7, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarStickY, 8, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarCStickX, 9, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarCStickY, 10, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarShield, 11, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::ScaleVarFixed, 12, 7, -1, 0, pf::fxFromFloat(2.0f), {}},
            {pf::PackageScriptOp::SetGroundVelocityFromVar, -1, 12, -1, 0, 0, {}},
            {pf::PackageScriptOp::SpawnObjectFromVars, -1, 12, 8, 0, pf::fxFromFloat(1.0f), "PackageVelocityObject"},
            {pf::PackageScriptOp::SetFacingFromVar, -1, 9, -1, 0, 0, {}},
        },
    }, {
        "InterruptScript",
        4,
        {
            {pf::PackageScriptOp::SetVarImmediate, 0, -1, -1, 5, 0, {}},
        },
    }, {
        "ProjectileScript",
        8,
        {
            {pf::PackageScriptOp::SetVarImmediate, 7, -1, -1, pf::fxFromFloat(0.6f), 0, {}},
            {pf::PackageScriptOp::SetVarImmediate, 8, -1, -1, pf::fxFromFloat(0.2f), 0, {}},
            {pf::PackageScriptOp::SpawnProjectile, -1, -1, -1, 0, pf::fxFromFloat(1.0f), "PackageProjectileObject"},
            {pf::PackageScriptOp::SpawnProjectileFromVars, -1, 7, 8, pf::fxFromFloat(0.4f), pf::fxFromFloat(1.0f), "PackageProjectileObject"},
        },
    }, {
        "CallTargetScript",
        4,
        {
            {pf::PackageScriptOp::AddVarImmediate, 0, -1, -1, 7, 0, {}},
        },
    }, {
        "CallScript",
        8,
        {
            {pf::PackageScriptOp::CallScript, -1, -1, -1, 0, 0, "CallTargetScript"},
            {pf::PackageScriptOp::AddVarImmediate, 0, -1, -1, 2, 0, {}},
        },
    }};
    pf::FighterDefinition packageAltFighter = packageSourceWorld.fighterDefs[0];
    packageAltFighter.name = "SmokeAlt";
    packageAltFighter.packageVariables = {{"AltSmokeVar", 42}};
    packageAltFighter.packageScripts.clear();
    packageSourceWorld.objectDefs[1].packageVariables = {
        {"ObjectSmokeVar", 2},
        {"ObjectFrameVar", 0},
        {"ObjectStateFrameVar", 0},
        {"ObjectGroundedVar", 0},
        {"ObjectFacingVar", 0},
        {"ObjectOwnerVar", 0},
        {"ObjectHeldByVar", 0},
        {"ObjectLastFighterVar", 0},
        {"ObjectLastObjectVar", 0},
        {"ObjectDamageVar", 0},
        {"ObjectPositionXVar", 0},
        {"ObjectPositionYVar", 0},
        {"ObjectVelocityXVar", 0},
        {"ObjectVelocityYVar", 0},
    };
    packageSourceWorld.objectDefs[1].packageScripts = {{
        "ObjectSmokeScript",
        24,
        {
            {pf::PackageScriptOp::AddVarImmediate, 0, -1, -1, 5, 0, {}},
            {pf::PackageScriptOp::SetVarFrame, 1, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarStateFrame, 2, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarGrounded, 3, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarFacing, 4, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarObjectOwner, 5, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarObjectHeldBy, 6, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarObjectLastFighter, 7, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarObjectLastObject, 8, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarObjectDamage, 9, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarObjectPositionX, 10, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarObjectPositionY, 11, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarObjectVelocityX, 12, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarObjectVelocityY, 13, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetAirVelocityX, -1, -1, -1, 0, pf::fxFromFloat(0.5f), {}},
            {pf::PackageScriptOp::SkipIfVarLessThanImmediate, 0, -1, -1, 10, 0, {}},
            {pf::PackageScriptOp::AddVarImmediate, 0, -1, -1, 100, 0, {}},
            {pf::PackageScriptOp::JumpRelative, -1, -1, -1, 2, 0, {}},
            {pf::PackageScriptOp::AddVarImmediate, 0, -1, -1, 1000, 0, {}},
        },
    }, {
        "ObjectCallTargetScript",
        4,
        {
            {pf::PackageScriptOp::AddVarImmediate, 0, -1, -1, 11, 0, {}},
        },
    }, {
        "ObjectCallScript",
        8,
        {
            {pf::PackageScriptOp::CallScript, -1, -1, -1, 0, 0, "ObjectCallTargetScript"},
            {pf::PackageScriptOp::AddVarImmediate, 0, -1, -1, 13, 0, {}},
        },
    }, {
        "ObjectSpawnScript",
        8,
        {
            {pf::PackageScriptOp::SetVarImmediate, 12, -1, -1, pf::fxFromFloat(0.6f), 0, {}},
            {pf::PackageScriptOp::SetVarImmediate, 13, -1, -1, pf::fxFromFloat(-0.2f), 0, {}},
            {pf::PackageScriptOp::SpawnProjectileFromVars, -1, 12, 13, pf::fxFromFloat(0.35f), pf::fxFromFloat(1.25f), "PackageProjectileObject"},
        },
    }};
    packageSourceWorld.objectDefs[1].onAccessory = {{std::string{"script:ObjectSmokeScript"}}};
    pf::GameObjectDefinition packageVelocityObject = packageSourceWorld.objectDefs[1];
    packageVelocityObject.name = "PackageVelocityObject";
    packageVelocityObject.states[0].onFrame = {{std::string{"object_lifetime"}}};
    packageVelocityObject.states[0].onPhysics.clear();
    packageVelocityObject.states[0].onCollision = {{std::string{"object_blast_destroy"}}};
    packageVelocityObject.onAccessory.clear();
    packageVelocityObject.hitboxes.clear();
    packageVelocityObject.hurtboxes.clear();
    packageVelocityObject.touchboxes.clear();
    packageVelocityObject.gravity = 0;
    packageSourceWorld.objectDefs.push_back(packageVelocityObject);
    pf::GameObjectDefinition packageProjectileObject = packageVelocityObject;
    packageProjectileObject.name = "PackageProjectileObject";
    packageProjectileObject.kind = pf::GameObjectKind::Projectile;
    packageProjectileObject.lifetimeFrames = 90;
    packageProjectileObject.destroyOnHit = true;
    packageProjectileObject.destroyOnShield = true;
    packageProjectileObject.states[0].onPhysics = {{std::string{"object_linear_physics"}}};
    packageProjectileObject.states[0].onCollision.clear();
    packageSourceWorld.objectDefs.push_back(packageProjectileObject);
    pf::GameObjectDefinition packageDestroyObject = packageVelocityObject;
    packageDestroyObject.name = "PackageDestroyObject";
    packageDestroyObject.packageScripts = {{
        "DestroyScript",
        4,
        {
            {pf::PackageScriptOp::DestroyObject, -1, -1, -1, 0, 0, {}},
        },
    }};
    packageDestroyObject.onSpawned = {{std::string{"script:DestroyScript"}}};
    packageSourceWorld.objectDefs.push_back(packageDestroyObject);
    pf::GameObjectDefinition packageOwnerContextObject = packageVelocityObject;
    packageOwnerContextObject.name = "PackageOwnerContextObject";
    packageOwnerContextObject.packageVariables = {
        {"OwnerPercent", 0},
        {"OwnerShield", 0},
        {"OwnerPositionX", 0},
        {"OwnerPositionY", 0},
        {"OwnerGroundVelocity", 0},
        {"OwnerAirVelocityX", 0},
        {"OwnerAirVelocityY", 0},
    };
    packageOwnerContextObject.packageScripts = {{
        "OwnerContextScript",
        16,
        {
            {pf::PackageScriptOp::SetVarFighterPercent, 0, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarFighterShield, 1, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarFighterPositionX, 2, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarFighterPositionY, 3, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarFighterGroundVelocity, 4, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarFighterAirVelocityX, 5, -1, -1, 0, 0, {}},
            {pf::PackageScriptOp::SetVarFighterAirVelocityY, 6, -1, -1, 0, 0, {}},
        },
    }};
    packageOwnerContextObject.onAccessory.clear();
    packageOwnerContextObject.onSpawned = {{std::string{"script:OwnerContextScript"}}};
    packageSourceWorld.objectDefs.push_back(packageOwnerContextObject);
    pf::FighterPackage sourcePackage;
    sourcePackage.name = "headless_smoke_package";
    sourcePackage.hsdAssets = {packageSourceWorld.fighterDefs[0].hsdAsset};
    sourcePackage.fighters = {packageSourceWorld.fighterDefs[0], packageAltFighter};
    sourcePackage.objects = packageSourceWorld.objectDefs;
    std::string packageError;
    pf::FighterPackage subactionProjectilePackage = sourcePackage;
    const int subactionProjectileStateIndex = subactionProjectilePackage.fighters[0].stateIndex("Wait");
    if (subactionProjectileStateIndex >= 0) {
        pf::Subaction subaction;
        subaction.type = pf::SubactionType::SpawnProjectile;
        subaction.frames = 1;
        subaction.objectName = "PackageProjectileObject";
        subaction.spawnOffset = {pf::fxFromFloat(0.75f), pf::fxFromFloat(0.7f), 0};
        subaction.spawnVelocity = {pf::fxFromFloat(0.4f), pf::fxFromFloat(0.1f)};
        subactionProjectilePackage.fighters[0].states[static_cast<size_t>(subactionProjectileStateIndex)].action.push_back(subaction);
    }
    const std::vector<uint8_t> subactionProjectilePackageBytes = subactionProjectileStateIndex >= 0
        ? pf::writeFighterPackage(subactionProjectilePackage, &packageError)
        : std::vector<uint8_t>{};
    pf::FighterPackage loadedSubactionProjectilePackage;
    const bool packageSubactionProjectileLoaded = !subactionProjectilePackageBytes.empty() &&
        pf::readFighterPackage(subactionProjectilePackageBytes, loadedSubactionProjectilePackage, &packageError);
    pf::FighterPackage subactionScriptPackage = sourcePackage;
    const int subactionScriptStateIndex = subactionScriptPackage.fighters[0].stateIndex("Wait");
    if (subactionScriptStateIndex >= 0) {
        pf::Subaction subaction;
        subaction.type = pf::SubactionType::CallScript;
        subaction.frames = 1;
        subaction.objectName = "CallScript";
        subactionScriptPackage.fighters[0].states[static_cast<size_t>(subactionScriptStateIndex)].action.push_back(subaction);
    }
    const std::vector<uint8_t> subactionScriptPackageBytes = subactionScriptStateIndex >= 0
        ? pf::writeFighterPackage(subactionScriptPackage, &packageError)
        : std::vector<uint8_t>{};
    pf::FighterPackage loadedSubactionScriptPackage;
    const bool packageSubactionScriptLoaded = !subactionScriptPackageBytes.empty() &&
        pf::readFighterPackage(subactionScriptPackageBytes, loadedSubactionScriptPackage, &packageError);
    const std::vector<uint8_t> packageBytes = pf::writeFighterPackage(sourcePackage, &packageError);
    pf::FighterPackage loadedPackage;
    const bool packageLoaded = pf::readFighterPackage(packageBytes, loadedPackage, &packageError);
    pf::FighterPackage invalidReadPackage;
    std::string invalidPackageError;
    std::vector<uint8_t> invalidPackageBytes = packageBytes;
    const bool invalidPackageMutated = corruptFirstSmokeScriptOp(invalidPackageBytes);
    const bool invalidPackageRejected = invalidPackageMutated &&
        !pf::readFighterPackage(invalidPackageBytes, invalidReadPackage, &invalidPackageError);
    pf::FighterPackage invalidWritePackage = sourcePackage;
    invalidWritePackage.fighters[0].packageScripts[0].instructions[0].op = static_cast<pf::PackageScriptOp>(255);
    const bool invalidPackageWriteRejected = pf::writeFighterPackage(invalidWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidVersionWritePackage = sourcePackage;
    invalidVersionWritePackage.version = 2;
    const bool invalidPackageVersionWriteRejected = pf::writeFighterPackage(invalidVersionWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidAnimationWritePackage = sourcePackage;
    invalidAnimationWritePackage.fighters[0].authoredSkeleton = {
        {-1, "Root", 0, {}, {}, {pf::fx(1), pf::fx(1), pf::fx(1)}},
    };
    pf::AnimationClip invalidAnimationClip;
    invalidAnimationClip.name = "InvalidAuthoredAnim";
    invalidAnimationClip.frameCount = pf::fx(10);
    invalidAnimationClip.tracks = {{
        7,
        pf::AnimationChannel::TranslateY,
        {{0, 0, 0, pf::AnimationInterpolation::Linear}},
    }};
    invalidAnimationWritePackage.fighters[0].authoredClips = {invalidAnimationClip};
    const bool invalidPackageAnimationWriteRejected = pf::writeFighterPackage(invalidAnimationWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidAnimationActionWritePackage = sourcePackage;
    pf::AnimationClip firstActionClip;
    firstActionClip.name = "FirstActionClip";
    firstActionClip.actionIndex = 0;
    firstActionClip.frameCount = pf::fx(10);
    pf::AnimationClip duplicateActionClip = firstActionClip;
    duplicateActionClip.name = "DuplicateActionIndex";
    invalidAnimationActionWritePackage.fighters[0].authoredClips = {firstActionClip, duplicateActionClip};
    const bool invalidPackageAnimationActionWriteRejected = pf::writeFighterPackage(invalidAnimationActionWritePackage, &invalidPackageError).empty();
    pf::FighterPackage authoredStateAnimationWritePackage = sourcePackage;
    authoredStateAnimationWritePackage.fighters[0].hasHsdAsset = false;
    authoredStateAnimationWritePackage.fighters[0].hsdAsset.reset();
    authoredStateAnimationWritePackage.fighters[0].authoredClips = {firstActionClip};
    pf::FighterState authoredAnimationState;
    authoredAnimationState.name = "Wait";
    authoredAnimationState.animation = firstActionClip.name;
    authoredAnimationState.animationActionIndex = firstActionClip.actionIndex;
    authoredAnimationState.animationLengthFrames = 10;
    authoredStateAnimationWritePackage.fighters[0].states = {authoredAnimationState};
    const bool packageAuthoredStateAnimationWriteOk = !pf::writeFighterPackage(authoredStateAnimationWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidAuthoredStateAnimationWritePackage = authoredStateAnimationWritePackage;
    invalidAuthoredStateAnimationWritePackage.fighters[0].states[0].animationActionIndex = 99;
    const bool invalidPackageAuthoredStateAnimationWriteRejected = pf::writeFighterPackage(invalidAuthoredStateAnimationWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidSkeletonNameWritePackage = authoredStateAnimationWritePackage;
    invalidSkeletonNameWritePackage.fighters[0].authoredSkeleton.push_back(
        invalidSkeletonNameWritePackage.fighters[0].authoredSkeleton[0]);
    invalidSkeletonNameWritePackage.fighters[0].authoredSkeleton.back().parent = 0;
    const bool invalidPackageSkeletonNameWriteRejected = pf::writeFighterPackage(invalidSkeletonNameWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidSkeletonScaleWritePackage = authoredStateAnimationWritePackage;
    invalidSkeletonScaleWritePackage.fighters[0].authoredSkeleton[0].scale.y = 0;
    const bool invalidPackageSkeletonScaleWriteRejected = pf::writeFighterPackage(invalidSkeletonScaleWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidStateTimingWritePackage = sourcePackage;
    invalidStateTimingWritePackage.fighters[0].states[0].animationLengthFrames = 0;
    const bool invalidPackageStateTimingWriteRejected = pf::writeFighterPackage(invalidStateTimingWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidDuplicateStateWritePackage = sourcePackage;
    invalidDuplicateStateWritePackage.fighters[0].states.push_back(invalidDuplicateStateWritePackage.fighters[0].states[0]);
    const bool invalidPackageDuplicateStateWriteRejected = pf::writeFighterPackage(invalidDuplicateStateWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidObjectStateTimingWritePackage = sourcePackage;
    invalidObjectStateTimingWritePackage.objects[1].states[0].animationLengthFrames = -1;
    const bool invalidPackageObjectStateTimingWriteRejected = pf::writeFighterPackage(invalidObjectStateTimingWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidDuplicateObjectStateWritePackage = sourcePackage;
    invalidDuplicateObjectStateWritePackage.objects[1].states.push_back(invalidDuplicateObjectStateWritePackage.objects[1].states[0]);
    const bool invalidPackageDuplicateObjectStateWriteRejected = pf::writeFighterPackage(invalidDuplicateObjectStateWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidReferenceWritePackage = sourcePackage;
    invalidReferenceWritePackage.fighters[0].packageScripts[0].instructions[1].text = "MissingObject";
    const bool invalidPackageReferenceWriteRejected = pf::writeFighterPackage(invalidReferenceWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidCallbackWritePackage = sourcePackage;
    invalidCallbackWritePackage.fighters[0].states[0].onFrame.push_back({std::string{"script:MissingScript"}});
    const bool invalidPackageCallbackWriteRejected = pf::writeFighterPackage(invalidCallbackWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidObjectCallbackWritePackage = sourcePackage;
    invalidObjectCallbackWritePackage.objects[1].onSpawned.push_back({std::string{"script:MissingObjectScript"}});
    const bool invalidPackageObjectCallbackWriteRejected = pf::writeFighterPackage(invalidObjectCallbackWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidGeometryWritePackage = sourcePackage;
    invalidGeometryWritePackage.objects[1].hitboxes[0].radius = 0;
    const bool invalidPackageGeometryWriteRejected = pf::writeFighterPackage(invalidGeometryWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidHitboxTargetWritePackage = sourcePackage;
    invalidHitboxTargetWritePackage.objects[1].hitboxes[0].hitGrounded = false;
    invalidHitboxTargetWritePackage.objects[1].hitboxes[0].hitAirborne = false;
    const bool invalidPackageHitboxTargetWriteRejected = pf::writeFighterPackage(invalidHitboxTargetWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidObjectPropertyWritePackage = sourcePackage;
    invalidObjectPropertyWritePackage.objects[1].terminalVelocity = -1;
    const bool invalidPackageObjectPropertyWriteRejected = pf::writeFighterPackage(invalidObjectPropertyWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidInterruptWritePackage = sourcePackage;
    invalidInterruptWritePackage.fighters[0].states[0].interrupts[0].enableFrame = -1;
    const bool invalidPackageInterruptWriteRejected = pf::writeFighterPackage(invalidInterruptWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidInterruptVarWritePackage = sourcePackage;
    pf::InterruptRule invalidInterruptVarRule;
    invalidInterruptVarRule.targetState = "Wait";
    invalidInterruptVarRule.condition = pf::InterruptCondition::PackageVarAtLeast;
    invalidInterruptVarRule.packageVariable = 999;
    invalidInterruptVarRule.packageValue = 1;
    invalidInterruptVarWritePackage.fighters[0].states[0].interrupts.push_back(invalidInterruptVarRule);
    const bool invalidPackageInterruptVarWriteRejected = pf::writeFighterPackage(invalidInterruptVarWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidProjectileTargetWritePackage = sourcePackage;
    invalidProjectileTargetWritePackage.fighters[0].packageScripts[0].instructions.push_back({
        pf::PackageScriptOp::SpawnProjectile,
        -1,
        -1,
        -1,
        0,
        pf::fxFromFloat(1.0f),
        "TrainingItem",
    });
    const bool invalidPackageProjectileTargetWriteRejected = pf::writeFighterPackage(invalidProjectileTargetWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidProjectileSubactionTargetWritePackage = sourcePackage;
    const int invalidProjectileSubactionState = invalidProjectileSubactionTargetWritePackage.fighters[0].stateIndex("Wait");
    if (invalidProjectileSubactionState >= 0) {
        pf::Subaction subaction;
        subaction.type = pf::SubactionType::SpawnProjectile;
        subaction.frames = 1;
        subaction.objectName = "TrainingItem";
        invalidProjectileSubactionTargetWritePackage.fighters[0].states[static_cast<size_t>(invalidProjectileSubactionState)].action.push_back(subaction);
    }
    const bool invalidPackageProjectileSubactionTargetWriteRejected = invalidProjectileSubactionState >= 0 &&
        pf::writeFighterPackage(invalidProjectileSubactionTargetWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidSubactionWritePackage = sourcePackage;
    const int invalidSubactionState = invalidSubactionWritePackage.fighters[0].stateIndex("Attack11");
    if (invalidSubactionState >= 0 && !invalidSubactionWritePackage.fighters[0].states[static_cast<size_t>(invalidSubactionState)].action.empty()) {
        invalidSubactionWritePackage.fighters[0].states[static_cast<size_t>(invalidSubactionState)].action[0].frames = -1;
    }
    const bool invalidPackageSubactionWriteRejected = invalidSubactionState >= 0 &&
        pf::writeFighterPackage(invalidSubactionWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidHurtboxRefWritePackage = sourcePackage;
    const int invalidHurtboxState = invalidHurtboxRefWritePackage.fighters[0].stateIndex("EscapeN");
    if (invalidHurtboxState >= 0 && invalidHurtboxRefWritePackage.fighters[0].states[static_cast<size_t>(invalidHurtboxState)].action.size() > 1) {
        invalidHurtboxRefWritePackage.fighters[0].states[static_cast<size_t>(invalidHurtboxState)].action[1].hurtboxIndex = 999;
    }
    const bool invalidPackageHurtboxRefWriteRejected = invalidHurtboxState >= 0 &&
        pf::writeFighterPackage(invalidHurtboxRefWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidMeshWritePackage = sourcePackage;
    invalidMeshWritePackage.fighters[0].authoredMesh.batches[0].vertices[0].influences[0].bone = 99;
    const bool invalidPackageMeshWriteRejected = pf::writeFighterPackage(invalidMeshWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidMeshWeightWritePackage = sourcePackage;
    invalidMeshWeightWritePackage.fighters[0].authoredMesh.batches[0].vertices[0].influences[0].weight = 0.0f;
    const bool invalidPackageMeshWeightWriteRejected = pf::writeFighterPackage(invalidMeshWeightWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidMeshTriangleWritePackage = sourcePackage;
    invalidMeshTriangleWritePackage.fighters[0].authoredMesh.batches[0].vertices.pop_back();
    const bool invalidPackageMeshTriangleWriteRejected = pf::writeFighterPackage(invalidMeshTriangleWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidNameWritePackage = sourcePackage;
    invalidNameWritePackage.objects[1].name = invalidNameWritePackage.objects[0].name;
    const bool invalidPackageNameWriteRejected = pf::writeFighterPackage(invalidNameWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidVariableNameWritePackage = sourcePackage;
    invalidVariableNameWritePackage.fighters[0].packageVariables.push_back(invalidVariableNameWritePackage.fighters[0].packageVariables[0]);
    const bool invalidPackageVariableNameWriteRejected = pf::writeFighterPackage(invalidVariableNameWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidObjectVariableNameWritePackage = sourcePackage;
    invalidObjectVariableNameWritePackage.objects[1].packageVariables.push_back(invalidObjectVariableNameWritePackage.objects[1].packageVariables[0]);
    const bool invalidPackageObjectVariableNameWriteRejected = pf::writeFighterPackage(invalidObjectVariableNameWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidScriptBranchWritePackage = sourcePackage;
    invalidScriptBranchWritePackage.fighters[0].packageScripts[1].instructions[4].intValue = 99;
    const bool invalidPackageBranchWriteRejected = pf::writeFighterPackage(invalidScriptBranchWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidVarBranchWritePackage = sourcePackage;
    invalidVarBranchWritePackage.fighters[0].packageScripts[1].instructions[3].srcB = 999;
    const bool invalidPackageVarBranchWriteRejected = pf::writeFighterPackage(invalidVarBranchWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidScriptDanglingBranchWritePackage = sourcePackage;
    invalidScriptDanglingBranchWritePackage.fighters[0].packageScripts[1].instructions.push_back({
        pf::PackageScriptOp::SkipIfVarLessThanImmediate,
        0,
        -1,
        -1,
        1,
        0,
        {},
    });
    const bool invalidPackageDanglingBranchWriteRejected = pf::writeFighterPackage(invalidScriptDanglingBranchWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidSwitchWritePackage = sourcePackage;
    invalidSwitchWritePackage.fighters[0].packageScripts[2].instructions[0].text = "MissingFighter";
    const bool invalidPackageSwitchWriteRejected = pf::writeFighterPackage(invalidSwitchWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidSpawnFighterWritePackage = sourcePackage;
    invalidSpawnFighterWritePackage.fighters[0].packageScripts[3].instructions[0].text = "MissingFighter";
    const bool invalidPackageSpawnFighterWriteRejected = pf::writeFighterPackage(invalidSpawnFighterWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidCallScriptWritePackage = sourcePackage;
    invalidCallScriptWritePackage.fighters[0].packageScripts.back().instructions[0].text = "MissingScript";
    const bool invalidPackageCallScriptWriteRejected = pf::writeFighterPackage(invalidCallScriptWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidSubactionCallScriptWritePackage = sourcePackage;
    const int invalidSubactionCallScriptState = invalidSubactionCallScriptWritePackage.fighters[0].stateIndex("Wait");
    if (invalidSubactionCallScriptState >= 0) {
        pf::Subaction subaction;
        subaction.type = pf::SubactionType::CallScript;
        subaction.frames = 1;
        subaction.objectName = "MissingScript";
        invalidSubactionCallScriptWritePackage.fighters[0].states[static_cast<size_t>(invalidSubactionCallScriptState)].action.push_back(subaction);
    }
    const bool invalidPackageSubactionCallScriptWriteRejected = invalidSubactionCallScriptState >= 0 &&
        pf::writeFighterPackage(invalidSubactionCallScriptWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidFighterDestroyWritePackage = sourcePackage;
    invalidFighterDestroyWritePackage.fighters[0].packageScripts[0].instructions.push_back({
        pf::PackageScriptOp::DestroyObject,
        -1,
        -1,
        -1,
        0,
        0,
        {},
    });
    const bool invalidPackageFighterDestroyWriteRejected = pf::writeFighterPackage(invalidFighterDestroyWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidFighterObjectContextWritePackage = sourcePackage;
    invalidFighterObjectContextWritePackage.fighters[0].packageScripts[0].instructions.push_back({
        pf::PackageScriptOp::SetVarObjectOwner,
        0,
        -1,
        -1,
        0,
        0,
        {},
    });
    const bool invalidPackageFighterObjectContextWriteRejected = pf::writeFighterPackage(invalidFighterObjectContextWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidFactWritePackage = sourcePackage;
    invalidFactWritePackage.fighters[0].packageScripts[4].instructions[0].dst = 999;
    const bool invalidPackageFactWriteRejected = pf::writeFighterPackage(invalidFactWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidInputWritePackage = sourcePackage;
    invalidInputWritePackage.fighters[0].packageScripts[5].instructions[0].intValue = 1 << 15;
    const bool invalidPackageInputWriteRejected = pf::writeFighterPackage(invalidInputWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidVarMotionWritePackage = sourcePackage;
    invalidVarMotionWritePackage.fighters[0].packageScripts[5].instructions[8].srcA = 999;
    const bool invalidPackageVarMotionWriteRejected = pf::writeFighterPackage(invalidVarMotionWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidSpawnObjectVarsWritePackage = sourcePackage;
    invalidSpawnObjectVarsWritePackage.fighters[0].packageScripts[5].instructions[9].srcB = 999;
    const bool invalidPackageSpawnObjectVarsWriteRejected = pf::writeFighterPackage(invalidSpawnObjectVarsWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidScaleWritePackage = sourcePackage;
    invalidScaleWritePackage.fighters[0].packageScripts[5].instructions[7].srcA = 999;
    const bool invalidPackageScaleWriteRejected = pf::writeFighterPackage(invalidScaleWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidObjectSwitchWritePackage = sourcePackage;
    invalidObjectSwitchWritePackage.objects[1].packageScripts[0].instructions.push_back({
        pf::PackageScriptOp::SwitchFighterDefinition,
        -1,
        -1,
        -1,
        0,
        0,
        "SmokeAlt",
    });
    const bool invalidPackageObjectSwitchWriteRejected = pf::writeFighterPackage(invalidObjectSwitchWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidObjectInputWritePackage = sourcePackage;
    invalidObjectInputWritePackage.objects[1].packageScripts[0].instructions.push_back({
        pf::PackageScriptOp::SetVarButtonDown,
        0,
        -1,
        -1,
        pf::ButtonAttack,
        0,
        {},
    });
    const bool invalidPackageObjectInputWriteRejected = pf::writeFighterPackage(invalidObjectInputWritePackage, &invalidPackageError).empty();
    pf::FighterPackage validObjectFighterContextWritePackage = sourcePackage;
    validObjectFighterContextWritePackage.objects[1].packageScripts[0].instructions.push_back({
        pf::PackageScriptOp::SetVarFighterPercent,
        0,
        -1,
        -1,
        0,
        0,
        {},
    });
    const bool packageObjectFighterContextWriteAccepted = !pf::writeFighterPackage(validObjectFighterContextWritePackage, &invalidPackageError).empty();
    pf::FighterPackage invalidObjectContextWritePackage = sourcePackage;
    invalidObjectContextWritePackage.objects[1].packageScripts[0].instructions[6].dst = 999;
    const bool invalidPackageObjectContextWriteRejected = pf::writeFighterPackage(invalidObjectContextWritePackage, &invalidPackageError).empty();
    pf::FighterPackage aliasStateWritePackage = sourcePackage;
    aliasStateWritePackage.fighters[0].packageScripts[0].instructions.push_back({
        pf::PackageScriptOp::ChangeState,
        -1,
        -1,
        -1,
        0,
        0,
        "AppealS",
    });
    const bool packageStateAliasWriteOk = !pf::writeFighterPackage(aliasStateWritePackage, &invalidPackageError).empty();
    pf::FighterPackage objectAliasStateWritePackage = sourcePackage;
    pf::GameObjectStateDefinition objectAppealState = objectAliasStateWritePackage.objects[1].states[0];
    objectAppealState.name = "AppealSR";
    objectAliasStateWritePackage.objects[1].states.push_back(objectAppealState);
    objectAppealState.name = "AppealSL";
    objectAliasStateWritePackage.objects[1].states.push_back(objectAppealState);
    objectAliasStateWritePackage.objects[1].packageScripts[0].instructions.push_back({
        pf::PackageScriptOp::ChangeState,
        -1,
        -1,
        -1,
        0,
        0,
        "AppealS",
    });
    const bool invalidPackageObjectStateAliasWriteRejected = pf::writeFighterPackage(objectAliasStateWritePackage, &invalidPackageError).empty();
    const bool packageShapeOk = packageLoaded &&
        loadedPackage.fighters.size() == 2 &&
        loadedPackage.objects.size() == packageSourceWorld.objectDefs.size() &&
        loadedPackage.fighters[0].states.size() == packageSourceWorld.fighterDefs[0].states.size() &&
        loadedPackage.fighters[0].hurtboxes.size() == packageSourceWorld.fighterDefs[0].hurtboxes.size() &&
        loadedPackage.fighters[0].authoredSkeleton.size() == 1 &&
        loadedPackage.fighters[0].authoredMesh.batches.size() == 1 &&
        loadedPackage.fighters[0].authoredMesh.batches[0].vertices.size() == 3 &&
        loadedPackage.fighters[0].packageVariables.size() == 20 &&
        loadedPackage.fighters[0].packageScripts.size() == 10 &&
        loadedPackage.fighters[1].name == "SmokeAlt" &&
        loadedPackage.objects.size() > 1 &&
        loadedPackage.objects[1].packageVariables.size() == 14 &&
        loadedPackage.objects[1].packageScripts.size() == 4;
    const bool packageAssetOk = packageShapeOk &&
        loadedPackage.fighters[0].hasHsdAsset &&
        loadedPackage.fighters[0].hsdAsset != nullptr &&
        loadedPackage.fighters[0].hsdAsset != packageSourceWorld.fighterDefs[0].hsdAsset &&
        loadedPackage.fighters[0].hsdAsset->name == packageSourceWorld.fighterDefs[0].hsdAsset->name &&
        loadedPackage.fighters[0].hsdAsset->sourceBytes.size() == packageSourceWorld.fighterDefs[0].hsdAsset->sourceBytes.size() &&
        loadedPackage.fighters[0].hsdAsset->skeleton.size() == packageSourceWorld.fighterDefs[0].hsdAsset->skeleton.size() &&
        loadedPackage.fighters[0].hsdAsset->clips.size() == packageSourceWorld.fighterDefs[0].hsdAsset->clips.size() &&
        loadedPackage.fighters[0].hsdAsset->mesh.batches.size() == packageSourceWorld.fighterDefs[0].hsdAsset->mesh.batches.size();
    const bool sandbagRosterOk = std::any_of(packageSourceWorld.fighterDefs.begin(), packageSourceWorld.fighterDefs.end(), [](const pf::FighterDefinition& def) {
        return def.name == "Sandbag" && def.hasHsdAsset && def.hsdAsset != nullptr;
    });

    pf::World packageBaselineWorld = pf::makeTrainingWorld();
    pf::World packageLoadedWorld = pf::makeTrainingWorld();
    if (packageShapeOk) {
        packageLoadedWorld.fighterDefs[0] = loadedPackage.fighters[0];
        packageLoadedWorld.fighterDefs.push_back(loadedPackage.fighters[1]);
        packageLoadedWorld.objectDefs = loadedPackage.objects;
    }
    bool packageParityOk = packageShapeOk;
    for (int frame = 0; frame < 90 && packageParityOk; ++frame) {
        pf::InputFrame p1Input;
        if (frame < 22) {
            p1Input.move.x = pf::fx(1);
        }
        if (frame == 28 || frame == 29) {
            p1Input.buttons |= pf::ButtonJump;
        }
        if (frame == 42) {
            p1Input.buttons |= pf::ButtonAttack;
        }
        const std::vector<pf::InputFrame> inputs{p1Input, pf::InputFrame{}};
        pf::tickWorld(packageBaselineWorld, inputs);
        pf::tickWorld(packageLoadedWorld, inputs);
        packageParityOk = sameWorldGameplayCore(packageBaselineWorld, packageLoadedWorld);
    }

    pf::World packageScriptWorld = pf::makeTrainingWorld();
    if (packageShapeOk) {
        packageScriptWorld.fighterDefs[0] = loadedPackage.fighters[0];
        packageScriptWorld.fighterDefs.push_back(loadedPackage.fighters[1]);
        const int waitIndex = packageScriptWorld.fighterDefs[0].stateIndex("Wait");
        if (waitIndex >= 0) {
            pf::FunctionCall scriptCall;
            scriptCall.name = "script:SmokeScript";
            packageScriptWorld.fighterDefs[0].states[static_cast<size_t>(waitIndex)].onFrame.push_back(scriptCall);
        }
        packageScriptWorld.fighters[0].packageVars.clear();
    }
    pf::tickWorld(packageScriptWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const int packageScriptSpawnCount = static_cast<int>(packageScriptWorld.objects.size());
    pf::WorldSnapshot packageScriptSnapshot = pf::saveWorld(packageScriptWorld);
    const int packageScriptVar = packageScriptWorld.fighters[0].packageVars.empty()
        ? -1
        : packageScriptWorld.fighters[0].packageVars[0];
    if (!packageScriptWorld.fighters[0].packageVars.empty()) {
        packageScriptWorld.fighters[0].packageVars[0] = 0;
    }
    pf::loadWorld(packageScriptWorld, packageScriptSnapshot);
    const int packageScriptRestoreVar = packageScriptWorld.fighters[0].packageVars.empty()
        ? -1
        : packageScriptWorld.fighters[0].packageVars[0];
    pf::World packageBranchScriptWorld = pf::makeTrainingWorld();
    if (packageShapeOk) {
        packageBranchScriptWorld.fighterDefs[0] = loadedPackage.fighters[0];
        packageBranchScriptWorld.fighterDefs.push_back(loadedPackage.fighters[1]);
        const int waitIndex = packageBranchScriptWorld.fighterDefs[0].stateIndex("Wait");
        if (waitIndex >= 0) {
            pf::FunctionCall scriptCall;
            scriptCall.name = "script:BranchScript";
            packageBranchScriptWorld.fighterDefs[0].states[static_cast<size_t>(waitIndex)].onFrame.push_back(scriptCall);
        }
        packageBranchScriptWorld.fighters[0].packageVars.clear();
    }
    pf::tickWorld(packageBranchScriptWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const int packageScriptBranchVar = packageBranchScriptWorld.fighters[0].packageVars.empty()
        ? -1
        : packageBranchScriptWorld.fighters[0].packageVars[0];
    pf::World packageCallScriptWorld = pf::makeTrainingWorld();
    if (packageShapeOk) {
        packageCallScriptWorld.fighterDefs[0] = loadedPackage.fighters[0];
        packageCallScriptWorld.fighterDefs.push_back(loadedPackage.fighters[1]);
        const int waitIndex = packageCallScriptWorld.fighterDefs[0].stateIndex("Wait");
        if (waitIndex >= 0) {
            pf::FunctionCall scriptCall;
            scriptCall.name = "script:CallScript";
            packageCallScriptWorld.fighterDefs[0].states[static_cast<size_t>(waitIndex)].onFrame.push_back(scriptCall);
        }
        packageCallScriptWorld.fighters[0].packageVars.clear();
    }
    pf::tickWorld(packageCallScriptWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const int packageScriptCallVar = packageCallScriptWorld.fighters[0].packageVars.empty()
        ? -1
        : packageCallScriptWorld.fighters[0].packageVars[0];
    pf::World packageFactScriptWorld = pf::makeTrainingWorld();
    if (packageShapeOk) {
        packageFactScriptWorld.fighterDefs[0] = loadedPackage.fighters[0];
        packageFactScriptWorld.fighterDefs.push_back(loadedPackage.fighters[1]);
        const int waitIndex = packageFactScriptWorld.fighterDefs[0].stateIndex("Wait");
        if (waitIndex >= 0) {
            pf::FunctionCall scriptCall;
            scriptCall.name = "script:FactScript";
            packageFactScriptWorld.fighterDefs[0].states[static_cast<size_t>(waitIndex)].onFrame.push_back(scriptCall);
        }
        packageFactScriptWorld.fighters[0].facing = -1;
        packageFactScriptWorld.fighters[0].percent = pf::fxFromFloat(37.0f);
        packageFactScriptWorld.fighters[0].shieldHealth = pf::fxFromFloat(42.0f);
        packageFactScriptWorld.fighters[0].position = {pf::fxFromFloat(-1.25f), pf::fxFromFloat(3.5f)};
        packageFactScriptWorld.fighters[0].previousPosition = packageFactScriptWorld.fighters[0].position;
        packageFactScriptWorld.fighters[0].groundVelocity = pf::fxFromFloat(0.75f);
        packageFactScriptWorld.fighters[0].fighterVelocity = {pf::fxFromFloat(-0.25f), pf::fxFromFloat(1.5f)};
        packageFactScriptWorld.fighters[0].packageVars.clear();
    }
    pf::tickWorld(packageFactScriptWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const bool packageFactScriptOk = packageShapeOk &&
        packageFactScriptWorld.fighters[0].packageVars.size() >= 20 &&
        packageFactScriptWorld.fighters[0].packageVars[1] == 1 &&
        packageFactScriptWorld.fighters[0].packageVars[2] == 1 &&
        packageFactScriptWorld.fighters[0].packageVars[3] == 1 &&
        packageFactScriptWorld.fighters[0].packageVars[4] == -1 &&
        packageFactScriptWorld.fighters[0].packageVars[13] == pf::fxFromFloat(37.0f) &&
        packageFactScriptWorld.fighters[0].packageVars[14] == pf::fxFromFloat(42.0f) &&
        packageFactScriptWorld.fighters[0].packageVars[15] == pf::fxFromFloat(-1.25f) &&
        packageFactScriptWorld.fighters[0].packageVars[16] == pf::fxFromFloat(3.5f) &&
        packageFactScriptWorld.fighters[0].packageVars[17] == pf::fxFromFloat(0.75f) &&
        packageFactScriptWorld.fighters[0].packageVars[18] == pf::fxFromFloat(-0.25f) &&
        packageFactScriptWorld.fighters[0].packageVars[19] == pf::fxFromFloat(1.5f);
    pf::World packageInputScriptWorld = pf::makeTrainingWorld();
    if (packageShapeOk) {
        packageInputScriptWorld.fighterDefs[0] = loadedPackage.fighters[0];
        packageInputScriptWorld.fighterDefs.push_back(loadedPackage.fighters[1]);
        packageInputScriptWorld.objectDefs = loadedPackage.objects;
        const int waitIndex = packageInputScriptWorld.fighterDefs[0].stateIndex("Wait");
        if (waitIndex >= 0) {
            pf::FunctionCall scriptCall;
            scriptCall.name = "script:InputScript";
            packageInputScriptWorld.fighterDefs[0].states[static_cast<size_t>(waitIndex)].onFrame.push_back(scriptCall);
            packageInputScriptWorld.fighterDefs[0].states[static_cast<size_t>(waitIndex)].interrupts.clear();
        }
        packageInputScriptWorld.fighters[0].packageVars.clear();
    }
    pf::InputFrame packagePauseInput{};
    packagePauseInput.buttons = pf::ButtonPause;
    packagePauseInput.move = {pf::fxFromFloat(0.25f), pf::fxFromFloat(-0.5f)};
    packagePauseInput.cStick = {pf::fxFromFloat(-0.75f), pf::fxFromFloat(1.0f)};
    packagePauseInput.shieldAnalog = pf::fxFromFloat(0.3f);
    pf::tickWorld(packageInputScriptWorld, {packagePauseInput, pf::InputFrame{}});
    pf::tickWorld(packageInputScriptWorld, {packagePauseInput, pf::InputFrame{}});
    const bool packageInputScriptOk = packageShapeOk &&
        packageInputScriptWorld.fighters[0].packageVars.size() >= 13 &&
        packageInputScriptWorld.fighters[0].packageVars[5] == 1 &&
        packageInputScriptWorld.fighters[0].packageVars[6] == 0 &&
        packageInputScriptWorld.fighters[0].packageVars[7] == pf::fxFromFloat(0.25f) &&
        packageInputScriptWorld.fighters[0].packageVars[8] == pf::fxFromFloat(-0.5f) &&
        packageInputScriptWorld.fighters[0].packageVars[9] == pf::fxFromFloat(-0.75f) &&
        packageInputScriptWorld.fighters[0].packageVars[10] == pf::fxFromFloat(1.0f) &&
        packageInputScriptWorld.fighters[0].packageVars[11] == pf::fxFromFloat(0.3f) &&
        packageInputScriptWorld.fighters[0].packageVars[12] == pf::fxFromFloat(0.5f) &&
        packageInputScriptWorld.fighters[0].groundVelocity == pf::fxFromFloat(0.5f) &&
        packageInputScriptWorld.fighters[0].facing == -1;
    bool packageVarSpawnObjectOk = false;
    for (const pf::GameObjectRuntime& object : packageInputScriptWorld.objects) {
        if (object.objectDef >= 0 &&
            object.objectDef < static_cast<int>(packageInputScriptWorld.objectDefs.size()) &&
            packageInputScriptWorld.objectDefs[static_cast<size_t>(object.objectDef)].name == "PackageVelocityObject" &&
            object.velocity.x == pf::fxFromFloat(0.5f) &&
            object.velocity.y == pf::fxFromFloat(-0.5f))
        {
            packageVarSpawnObjectOk = true;
        }
    }
    pf::World packageInterruptScriptWorld = pf::makeTrainingWorld();
    std::string packageInterruptTargetState = "SquatWait";
    if (packageShapeOk) {
        packageInterruptScriptWorld.fighterDefs[0] = loadedPackage.fighters[0];
        packageInterruptScriptWorld.fighterDefs.push_back(loadedPackage.fighters[1]);
        const int waitIndex = packageInterruptScriptWorld.fighterDefs[0].stateIndex("Wait");
        if (waitIndex >= 0) {
            pf::FunctionCall scriptCall;
            scriptCall.name = "script:InterruptScript";
            pf::FighterState& wait = packageInterruptScriptWorld.fighterDefs[0].states[static_cast<size_t>(waitIndex)];
            wait.onFrame.push_back(scriptCall);
            if (packageInterruptScriptWorld.fighterDefs[0].stateIndex(packageInterruptTargetState) < 0) {
                packageInterruptTargetState = "Fall";
            }
            pf::InterruptRule interrupt;
            interrupt.targetState = packageInterruptTargetState;
            interrupt.condition = pf::InterruptCondition::PackageVarAtLeast;
            interrupt.packageVariable = 0;
            interrupt.packageValue = 5;
            interrupt.alwaysActive = true;
            wait.interrupts.push_back(interrupt);
        }
        packageInterruptScriptWorld.fighters[0].packageVars.clear();
    }
    pf::tickWorld(packageInterruptScriptWorld, {pf::InputFrame{}, pf::InputFrame{}});
    pf::tickWorld(packageInterruptScriptWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const bool packageInterruptScriptOk = packageShapeOk &&
        pf::currentState(packageInterruptScriptWorld, packageInterruptScriptWorld.fighters[0]).name == packageInterruptTargetState &&
        !packageInterruptScriptWorld.fighters[0].packageVars.empty() &&
        packageInterruptScriptWorld.fighters[0].packageVars[0] == 5;
    pf::World packageProjectileScriptWorld = pf::makeTrainingWorld();
    if (packageShapeOk) {
        packageProjectileScriptWorld.fighterDefs[0] = loadedPackage.fighters[0];
        packageProjectileScriptWorld.fighterDefs.push_back(loadedPackage.fighters[1]);
        packageProjectileScriptWorld.objectDefs = loadedPackage.objects;
        const int waitIndex = packageProjectileScriptWorld.fighterDefs[0].stateIndex("Wait");
        if (waitIndex >= 0) {
            pf::FunctionCall scriptCall;
            scriptCall.name = "script:ProjectileScript";
            pf::FighterState& wait = packageProjectileScriptWorld.fighterDefs[0].states[static_cast<size_t>(waitIndex)];
            wait.onFrame.push_back(scriptCall);
            wait.interrupts.clear();
        }
        packageProjectileScriptWorld.fighters[0].packageVars.clear();
    }
    const pf::Vec2 packageProjectileSpawnBase = packageProjectileScriptWorld.fighters[0].position;
    pf::tickWorld(packageProjectileScriptWorld, {pf::InputFrame{}, pf::InputFrame{}});
    int packageProjectileSpawnCount = 0;
    bool packageProjectileVarSpawnOk = false;
    bool packageProjectileYOffsetOk = false;
    const bool packageProjectileScriptRan =
        packageProjectileScriptWorld.fighters[0].packageVars.size() > 8 &&
        packageProjectileScriptWorld.fighters[0].packageVars[7] == pf::fxFromFloat(0.6f) &&
        packageProjectileScriptWorld.fighters[0].packageVars[8] == pf::fxFromFloat(0.2f);
    const bool packageProjectileDefKindOk = packageShapeOk &&
        std::any_of(packageProjectileScriptWorld.objectDefs.begin(), packageProjectileScriptWorld.objectDefs.end(), [](const pf::GameObjectDefinition& object) {
            return object.name == "PackageProjectileObject" && object.kind == pf::GameObjectKind::Projectile;
        });
    for (const pf::GameObjectRuntime& object : packageProjectileScriptWorld.objects) {
        if (object.objectDef >= 0 &&
            object.objectDef < static_cast<int>(packageProjectileScriptWorld.objectDefs.size()) &&
            packageProjectileScriptWorld.objectDefs[static_cast<size_t>(object.objectDef)].kind == pf::GameObjectKind::Projectile &&
            packageProjectileScriptWorld.objectDefs[static_cast<size_t>(object.objectDef)].name == "PackageProjectileObject")
        {
            ++packageProjectileSpawnCount;
            if (object.velocity.x == pf::fxFromFloat(0.6f) && object.velocity.y == pf::fxFromFloat(0.2f)) {
                packageProjectileVarSpawnOk = true;
                packageProjectileYOffsetOk = object.previousPosition.y ==
                    packageProjectileSpawnBase.y + pf::fxFromFloat(0.7f) + pf::fxFromFloat(0.4f);
            }
        }
    }
    const bool packageProjectileScriptOk = packageShapeOk &&
        packageProjectileScriptRan &&
        packageProjectileDefKindOk &&
        packageProjectileSpawnCount >= 2 &&
        packageProjectileVarSpawnOk &&
        packageProjectileYOffsetOk;
    pf::World packageProjectileSubactionWorld = pf::makeTrainingWorld();
    if (packageSubactionProjectileLoaded) {
        packageProjectileSubactionWorld.fighterDefs[0] = loadedSubactionProjectilePackage.fighters[0];
        packageProjectileSubactionWorld.fighterDefs[0].hasHsdAsset = false;
        packageProjectileSubactionWorld.fighterDefs[0].hsdAsset.reset();
        packageProjectileSubactionWorld.fighterDefs.push_back(loadedSubactionProjectilePackage.fighters[1]);
        packageProjectileSubactionWorld.objectDefs = loadedSubactionProjectilePackage.objects;
        const int waitIndex = packageProjectileSubactionWorld.fighterDefs[0].stateIndex("Wait");
        if (waitIndex >= 0) {
            packageProjectileSubactionWorld.fighterDefs[0].states[static_cast<size_t>(waitIndex)].interrupts.clear();
            packageProjectileSubactionWorld.fighterDefs[0].states[static_cast<size_t>(waitIndex)].animationActionIndex = -1;
        }
    }
    pf::tickWorld(packageProjectileSubactionWorld, {pf::InputFrame{}, pf::InputFrame{}});
    int packageProjectileSubactionCount = 0;
    bool packageProjectileSubactionVelocityOk = false;
    for (const pf::GameObjectRuntime& object : packageProjectileSubactionWorld.objects) {
        if (object.objectDef >= 0 &&
            object.objectDef < static_cast<int>(packageProjectileSubactionWorld.objectDefs.size()) &&
            packageProjectileSubactionWorld.objectDefs[static_cast<size_t>(object.objectDef)].kind == pf::GameObjectKind::Projectile &&
            packageProjectileSubactionWorld.objectDefs[static_cast<size_t>(object.objectDef)].name == "PackageProjectileObject")
        {
            ++packageProjectileSubactionCount;
            packageProjectileSubactionVelocityOk =
                (object.velocity.x == pf::fxFromFloat(0.4f) || object.velocity.x == -pf::fxFromFloat(0.4f)) &&
                object.velocity.y == pf::fxFromFloat(0.1f);
        }
    }
    const bool packageProjectileSubactionOk = packageSubactionProjectileLoaded &&
        packageProjectileSubactionCount >= 1 &&
        packageProjectileSubactionVelocityOk;
    pf::World packageSubactionScriptWorld = pf::makeTrainingWorld();
    if (packageSubactionScriptLoaded) {
        packageSubactionScriptWorld.fighterDefs[0] = loadedSubactionScriptPackage.fighters[0];
        packageSubactionScriptWorld.fighterDefs[0].hasHsdAsset = false;
        packageSubactionScriptWorld.fighterDefs[0].hsdAsset.reset();
        packageSubactionScriptWorld.fighterDefs.push_back(loadedSubactionScriptPackage.fighters[1]);
        const int waitIndex = packageSubactionScriptWorld.fighterDefs[0].stateIndex("Wait");
        if (waitIndex >= 0) {
            packageSubactionScriptWorld.fighterDefs[0].states[static_cast<size_t>(waitIndex)].interrupts.clear();
            packageSubactionScriptWorld.fighterDefs[0].states[static_cast<size_t>(waitIndex)].animationActionIndex = -1;
        }
        packageSubactionScriptWorld.fighters[0].packageVars.clear();
    }
    pf::tickWorld(packageSubactionScriptWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const int packageSubactionScriptVar = packageSubactionScriptWorld.fighters[0].packageVars.empty()
        ? -1
        : packageSubactionScriptWorld.fighters[0].packageVars[0];
    pf::World packageSwitchScriptWorld = pf::makeTrainingWorld();
    if (packageShapeOk) {
        packageSwitchScriptWorld.fighterDefs[0] = loadedPackage.fighters[0];
        packageSwitchScriptWorld.fighterDefs.push_back(loadedPackage.fighters[1]);
        const int waitIndex = packageSwitchScriptWorld.fighterDefs[0].stateIndex("Wait");
        if (waitIndex >= 0) {
            pf::FunctionCall scriptCall;
            scriptCall.name = "script:SwitchScript";
            packageSwitchScriptWorld.fighterDefs[0].states[static_cast<size_t>(waitIndex)].onFrame.push_back(scriptCall);
        }
        packageSwitchScriptWorld.fighters[0].packageVars.clear();
    }
    pf::tickWorld(packageSwitchScriptWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const bool packageSwitchScriptOk = packageShapeOk &&
        packageSwitchScriptWorld.fighters[0].fighterDef >= 0 &&
        packageSwitchScriptWorld.fighters[0].fighterDef < static_cast<int>(packageSwitchScriptWorld.fighterDefs.size()) &&
        packageSwitchScriptWorld.fighterDefs[static_cast<size_t>(packageSwitchScriptWorld.fighters[0].fighterDef)].name == "SmokeAlt";
    const int packageSwitchScriptVar = packageSwitchScriptWorld.fighters[0].packageVars.empty()
        ? -1
        : packageSwitchScriptWorld.fighters[0].packageVars[0];
    pf::World packageSpawnFighterScriptWorld = pf::makeTrainingWorld();
    if (packageShapeOk) {
        packageSpawnFighterScriptWorld.fighterDefs[0] = loadedPackage.fighters[0];
        packageSpawnFighterScriptWorld.fighterDefs.push_back(loadedPackage.fighters[1]);
        pf::resetTrainingFighter(packageSpawnFighterScriptWorld, 1, 1, {pf::fx(2), 0}, -1);
        const int waitIndex = packageSpawnFighterScriptWorld.fighterDefs[0].stateIndex("Wait");
        if (waitIndex >= 0) {
            pf::FunctionCall scriptCall;
            scriptCall.name = "script:SpawnFighterScript";
            packageSpawnFighterScriptWorld.fighterDefs[0].states[static_cast<size_t>(waitIndex)].onFrame.push_back(scriptCall);
        }
    }
    pf::tickWorld(packageSpawnFighterScriptWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const bool packageSpawnFighterScriptOk = packageShapeOk &&
        packageSpawnFighterScriptWorld.fighters.size() == 3 &&
        packageSpawnFighterScriptWorld.fighters[2].fighterDef >= 0 &&
        packageSpawnFighterScriptWorld.fighters[2].fighterDef < static_cast<int>(packageSpawnFighterScriptWorld.fighterDefs.size()) &&
        packageSpawnFighterScriptWorld.fighterDefs[static_cast<size_t>(packageSpawnFighterScriptWorld.fighters[2].fighterDef)].name == "SmokeAlt";
    pf::World packageObjectScriptWorld = pf::makeTrainingWorld();
    if (packageShapeOk) {
        packageObjectScriptWorld.objectDefs = loadedPackage.objects;
    }
    const int packageObjectIndex = pf::spawnGameObject(
        packageObjectScriptWorld,
        "TrainingItem",
        0,
        {0, pf::fx(3)},
        1,
        {});
    if (packageObjectIndex >= 0 && packageObjectIndex < static_cast<int>(packageObjectScriptWorld.objects.size())) {
        pf::GameObjectRuntime& object = packageObjectScriptWorld.objects[static_cast<size_t>(packageObjectIndex)];
        object.lastInteractionFighter = 1;
        object.lastInteractionObject = packageObjectIndex;
        object.damageTaken = pf::fxFromFloat(2.0f);
        object.position = {pf::fxFromFloat(1.25f), pf::fxFromFloat(3.5f)};
        object.previousPosition = object.position;
        object.velocity = {pf::fxFromFloat(-0.25f), pf::fxFromFloat(0.75f)};
    }
    pf::tickWorld(packageObjectScriptWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const pf::GameObjectRuntime* packageObject = packageObjectIndex >= 0 && packageObjectIndex < static_cast<int>(packageObjectScriptWorld.objects.size())
        ? &packageObjectScriptWorld.objects[static_cast<size_t>(packageObjectIndex)]
        : nullptr;
    const int packageObjectScriptVar = packageObject && !packageObject->packageVars.empty() ? packageObject->packageVars[0] : -1;
    const pf::Fix packageObjectCtxPosX = packageObject && packageObject->packageVars.size() > 10 ? packageObject->packageVars[10] : pf::Fix{-1};
    const pf::Fix packageObjectCtxPosY = packageObject && packageObject->packageVars.size() > 11 ? packageObject->packageVars[11] : pf::Fix{-1};
    const pf::Fix packageObjectCtxVelX = packageObject && packageObject->packageVars.size() > 12 ? packageObject->packageVars[12] : pf::Fix{-1};
    const pf::Fix packageObjectCtxVelY = packageObject && packageObject->packageVars.size() > 13 ? packageObject->packageVars[13] : pf::Fix{-1};
    const bool packageObjectFactScriptOk = packageObject &&
        packageObject->packageVars.size() >= 14 &&
        packageObject->packageVars[1] == 1 &&
        packageObject->packageVars[2] == 1 &&
        packageObject->packageVars[3] == 0 &&
        packageObject->packageVars[4] == 1 &&
        packageObject->packageVars[5] == 0 &&
        packageObject->packageVars[6] == -1 &&
        packageObject->packageVars[7] == 1 &&
        packageObject->packageVars[8] == packageObjectIndex &&
        packageObject->packageVars[9] == pf::fxFromFloat(2.0f) &&
        packageObject->packageVars[10] == pf::fxFromFloat(1.0f) &&
        packageObject->packageVars[11] == pf::fxFromFloat(4.17f) &&
        packageObject->packageVars[12] == pf::fxFromFloat(-0.25f) &&
        packageObject->packageVars[13] == pf::fxFromFloat(0.67f);
    const pf::Fix packageObjectScriptVelX = packageObject ? packageObject->velocity.x : pf::Fix{-1};
    pf::World packageObjectStateScriptWorld = pf::makeTrainingWorld();
    if (packageShapeOk) {
        packageObjectStateScriptWorld.objectDefs = loadedPackage.objects;
        if (packageObjectStateScriptWorld.objectDefs.size() > 1 &&
            !packageObjectStateScriptWorld.objectDefs[1].states.empty() &&
            !packageObjectStateScriptWorld.objectDefs[1].packageScripts.empty())
        {
            packageObjectStateScriptWorld.objectDefs[1].onAccessory.clear();
            packageObjectStateScriptWorld.objectDefs[1].states[0].onFrame = {{std::string{"script:ObjectSmokeScript"}}};
        }
    }
    const int packageObjectStateIndex = pf::spawnGameObject(
        packageObjectStateScriptWorld,
        "TrainingItem",
        -1,
        {0, pf::fx(3)},
        1,
        {});
    pf::tickWorld(packageObjectStateScriptWorld, {pf::InputFrame{}, pf::InputFrame{}});
    const pf::GameObjectRuntime* packageObjectState = packageObjectStateIndex >= 0 &&
            packageObjectStateIndex < static_cast<int>(packageObjectStateScriptWorld.objects.size())
        ? &packageObjectStateScriptWorld.objects[static_cast<size_t>(packageObjectStateIndex)]
        : nullptr;
    const int packageObjectStateScriptVar = packageObjectState && !packageObjectState->packageVars.empty() ? packageObjectState->packageVars[0] : -1;
    pf::World packageObjectCallScriptWorld = pf::makeTrainingWorld();
    if (packageShapeOk) {
        packageObjectCallScriptWorld.objectDefs = loadedPackage.objects;
        packageObjectCallScriptWorld.objectDefs[1].onSpawned = {{std::string{"script:ObjectCallScript"}}};
    }
    const int packageObjectCallScriptIndex = pf::spawnGameObject(
        packageObjectCallScriptWorld,
        "TrainingItem",
        -1,
        {0, pf::fx(3)},
        1,
        {});
    const pf::GameObjectRuntime* packageObjectCallScript = packageObjectCallScriptIndex >= 0 &&
            packageObjectCallScriptIndex < static_cast<int>(packageObjectCallScriptWorld.objects.size())
        ? &packageObjectCallScriptWorld.objects[static_cast<size_t>(packageObjectCallScriptIndex)]
        : nullptr;
    const int packageObjectCallScriptVar = packageObjectCallScript && !packageObjectCallScript->packageVars.empty() ? packageObjectCallScript->packageVars[0] : -1;
    pf::World packageObjectDestroyScriptWorld = pf::makeTrainingWorld();
    if (packageShapeOk) {
        packageObjectDestroyScriptWorld.objectDefs = loadedPackage.objects;
    }
    const int packageObjectDestroyIndex = pf::spawnGameObject(
        packageObjectDestroyScriptWorld,
        "PackageDestroyObject",
        -1,
        {0, pf::fx(3)},
        1,
        {});
    const bool packageObjectDestroyScriptOk = packageObjectDestroyIndex >= 0 &&
        packageObjectDestroyIndex < static_cast<int>(packageObjectDestroyScriptWorld.objects.size()) &&
        !packageObjectDestroyScriptWorld.objects[static_cast<size_t>(packageObjectDestroyIndex)].active;
    pf::World packageObjectOwnerContextWorld = pf::makeTrainingWorld();
    if (packageShapeOk) {
        packageObjectOwnerContextWorld.objectDefs = loadedPackage.objects;
    }
    packageObjectOwnerContextWorld.fighters[0].percent = pf::fxFromFloat(33.0f);
    packageObjectOwnerContextWorld.fighters[0].shieldHealth = pf::fxFromFloat(44.0f);
    packageObjectOwnerContextWorld.fighters[0].position = {pf::fxFromFloat(-2.0f), pf::fxFromFloat(5.0f)};
    packageObjectOwnerContextWorld.fighters[0].groundVelocity = pf::fxFromFloat(0.75f);
    packageObjectOwnerContextWorld.fighters[0].fighterVelocity = {pf::fxFromFloat(-0.5f), pf::fxFromFloat(0.25f)};
    const int packageObjectOwnerContextIndex = pf::spawnGameObject(
        packageObjectOwnerContextWorld,
        "PackageOwnerContextObject",
        0,
        {0, pf::fx(3)},
        1,
        {});
    const pf::GameObjectRuntime* packageObjectOwnerContext = packageObjectOwnerContextIndex >= 0 &&
            packageObjectOwnerContextIndex < static_cast<int>(packageObjectOwnerContextWorld.objects.size())
        ? &packageObjectOwnerContextWorld.objects[static_cast<size_t>(packageObjectOwnerContextIndex)]
        : nullptr;
    const bool packageObjectOwnerContextOk = packageObjectOwnerContext &&
        packageObjectOwnerContext->packageVars.size() >= 7 &&
        packageObjectOwnerContext->packageVars[0] == pf::fxFromFloat(33.0f) &&
        packageObjectOwnerContext->packageVars[1] == pf::fxFromFloat(44.0f) &&
        packageObjectOwnerContext->packageVars[2] == pf::fxFromFloat(-2.0f) &&
        packageObjectOwnerContext->packageVars[3] == pf::fxFromFloat(5.0f) &&
        packageObjectOwnerContext->packageVars[4] == pf::fxFromFloat(0.75f) &&
        packageObjectOwnerContext->packageVars[5] == pf::fxFromFloat(-0.5f) &&
        packageObjectOwnerContext->packageVars[6] == pf::fxFromFloat(0.25f);
    pf::World packageObjectSpawnOffsetWorld = pf::makeTrainingWorld();
    if (packageShapeOk) {
        packageObjectSpawnOffsetWorld.objectDefs = loadedPackage.objects;
        for (pf::GameObjectDefinition& objectDef : packageObjectSpawnOffsetWorld.objectDefs) {
            if (objectDef.name == "PackageVelocityObject") {
                objectDef.onSpawned = {{std::string{"script:ObjectSpawnScript"}}};
                break;
            }
        }
    }
    const pf::Vec2 packageObjectSpawnOffsetBase{pf::fxFromFloat(2.0f), pf::fxFromFloat(3.0f)};
    pf::spawnGameObject(
        packageObjectSpawnOffsetWorld,
        "PackageVelocityObject",
        0,
        packageObjectSpawnOffsetBase,
        -1,
        {});
    bool packageObjectSpawnYOffsetOk = false;
    for (const pf::GameObjectRuntime& object : packageObjectSpawnOffsetWorld.objects) {
        if (object.objectDef >= 0 &&
            object.objectDef < static_cast<int>(packageObjectSpawnOffsetWorld.objectDefs.size()) &&
            packageObjectSpawnOffsetWorld.objectDefs[static_cast<size_t>(object.objectDef)].name == "PackageProjectileObject" &&
            object.velocity.x == -pf::fxFromFloat(0.6f) &&
            object.velocity.y == pf::fxFromFloat(-0.2f) &&
            object.previousPosition.x == packageObjectSpawnOffsetBase.x - pf::fxFromFloat(1.25f) &&
            object.previousPosition.y == packageObjectSpawnOffsetBase.y + pf::fxFromFloat(0.35f))
        {
            packageObjectSpawnYOffsetOk = true;
        }
    }
    std::cout << "fighter_package_bytes=" << packageBytes.size()
              << " fighter_package_checksum=" << pf::fighterPackageChecksum(packageBytes)
              << " fighter_package_loaded=" << packageLoaded
              << " fighter_package_shape_ok=" << packageShapeOk
              << " fighter_package_asset_ok=" << packageAssetOk
              << " fighter_package_parity_ok=" << packageParityOk
              << " fighter_package_script_var=" << packageScriptVar
              << " fighter_package_script_restore_var=" << packageScriptRestoreVar
              << " fighter_package_script_branch_var=" << packageScriptBranchVar
              << " fighter_package_script_call_var=" << packageScriptCallVar
              << " fighter_package_script_fact_ok=" << packageFactScriptOk
              << " fighter_package_script_input_ok=" << packageInputScriptOk
              << " fighter_package_script_spawn_vars_ok=" << packageVarSpawnObjectOk
              << " fighter_package_script_interrupt_ok=" << packageInterruptScriptOk
              << " fighter_package_script_projectile_ok=" << packageProjectileScriptOk
              << " fighter_package_script_projectile_count=" << packageProjectileSpawnCount
              << " fighter_package_script_projectile_var_spawn=" << packageProjectileVarSpawnOk
              << " fighter_package_script_projectile_y_offset=" << packageProjectileYOffsetOk
              << " fighter_package_script_projectile_ran=" << packageProjectileScriptRan
              << " fighter_package_script_projectile_def_kind=" << packageProjectileDefKindOk
              << " fighter_package_subaction_projectile_ok=" << packageProjectileSubactionOk
              << " fighter_package_subaction_projectile_count=" << packageProjectileSubactionCount
              << " fighter_package_subaction_script_var=" << packageSubactionScriptVar
              << " fighter_package_script_switch_ok=" << packageSwitchScriptOk
              << " fighter_package_script_switch_var=" << packageSwitchScriptVar
              << " fighter_package_script_spawn_fighter_ok=" << packageSpawnFighterScriptOk
              << " fighter_package_script_spawn_ok=" << (packageScriptSpawnCount > 0)
              << " fighter_package_script_spawn_count=" << packageScriptSpawnCount
              << " fighter_package_object_script_var=" << packageObjectScriptVar
              << " fighter_package_object_script_fact_ok=" << packageObjectFactScriptOk
              << " fighter_package_object_script_pos_x=" << pf::fxToFloat(packageObjectCtxPosX)
              << " fighter_package_object_script_pos_y=" << pf::fxToFloat(packageObjectCtxPosY)
              << " fighter_package_object_script_vel_x_read=" << pf::fxToFloat(packageObjectCtxVelX)
              << " fighter_package_object_script_vel_y_read=" << pf::fxToFloat(packageObjectCtxVelY)
              << " fighter_package_object_script_vel_x=" << pf::fxToFloat(packageObjectScriptVelX)
              << " fighter_package_object_state_script_var=" << packageObjectStateScriptVar
              << " fighter_package_object_call_script_var=" << packageObjectCallScriptVar
              << " fighter_package_object_destroy_script_ok=" << packageObjectDestroyScriptOk
              << " fighter_package_object_owner_context_ok=" << packageObjectOwnerContextOk
              << " fighter_package_object_spawn_y_offset_ok=" << packageObjectSpawnYOffsetOk
              << " fighter_package_invalid_read_rejected=" << invalidPackageRejected
              << " fighter_package_invalid_write_rejected=" << invalidPackageWriteRejected
              << " fighter_package_invalid_version_write_rejected=" << invalidPackageVersionWriteRejected
              << " fighter_package_invalid_animation_write_rejected=" << invalidPackageAnimationWriteRejected
              << " fighter_package_invalid_animation_action_write_rejected=" << invalidPackageAnimationActionWriteRejected
              << " fighter_package_authored_state_animation_write_ok=" << packageAuthoredStateAnimationWriteOk
              << " fighter_package_invalid_authored_state_animation_write_rejected=" << invalidPackageAuthoredStateAnimationWriteRejected
              << " fighter_package_invalid_skeleton_name_write_rejected=" << invalidPackageSkeletonNameWriteRejected
              << " fighter_package_invalid_skeleton_scale_write_rejected=" << invalidPackageSkeletonScaleWriteRejected
              << " fighter_package_invalid_state_timing_write_rejected=" << invalidPackageStateTimingWriteRejected
              << " fighter_package_invalid_duplicate_state_write_rejected=" << invalidPackageDuplicateStateWriteRejected
              << " fighter_package_invalid_object_state_timing_write_rejected=" << invalidPackageObjectStateTimingWriteRejected
              << " fighter_package_invalid_duplicate_object_state_write_rejected=" << invalidPackageDuplicateObjectStateWriteRejected
              << " fighter_package_invalid_reference_write_rejected=" << invalidPackageReferenceWriteRejected
              << " fighter_package_invalid_callback_write_rejected=" << invalidPackageCallbackWriteRejected
              << " fighter_package_invalid_object_callback_write_rejected=" << invalidPackageObjectCallbackWriteRejected
              << " fighter_package_invalid_geometry_write_rejected=" << invalidPackageGeometryWriteRejected
              << " fighter_package_invalid_hitbox_target_write_rejected=" << invalidPackageHitboxTargetWriteRejected
              << " fighter_package_invalid_object_property_write_rejected=" << invalidPackageObjectPropertyWriteRejected
              << " fighter_package_invalid_interrupt_write_rejected=" << invalidPackageInterruptWriteRejected
              << " fighter_package_invalid_interrupt_var_write_rejected=" << invalidPackageInterruptVarWriteRejected
              << " fighter_package_invalid_projectile_target_write_rejected=" << invalidPackageProjectileTargetWriteRejected
              << " fighter_package_invalid_projectile_subaction_target_write_rejected=" << invalidPackageProjectileSubactionTargetWriteRejected
              << " fighter_package_invalid_subaction_write_rejected=" << invalidPackageSubactionWriteRejected
              << " fighter_package_invalid_hurtbox_ref_write_rejected=" << invalidPackageHurtboxRefWriteRejected
              << " fighter_package_invalid_mesh_write_rejected=" << invalidPackageMeshWriteRejected
              << " fighter_package_invalid_mesh_weight_write_rejected=" << invalidPackageMeshWeightWriteRejected
              << " fighter_package_invalid_mesh_triangle_write_rejected=" << invalidPackageMeshTriangleWriteRejected
              << " fighter_package_invalid_name_write_rejected=" << invalidPackageNameWriteRejected
              << " fighter_package_invalid_variable_name_write_rejected=" << invalidPackageVariableNameWriteRejected
              << " fighter_package_invalid_object_variable_name_write_rejected=" << invalidPackageObjectVariableNameWriteRejected
              << " fighter_package_invalid_branch_write_rejected=" << invalidPackageBranchWriteRejected
              << " fighter_package_invalid_var_branch_write_rejected=" << invalidPackageVarBranchWriteRejected
              << " fighter_package_invalid_dangling_branch_write_rejected=" << invalidPackageDanglingBranchWriteRejected
              << " fighter_package_invalid_switch_write_rejected=" << invalidPackageSwitchWriteRejected
              << " fighter_package_invalid_spawn_fighter_write_rejected=" << invalidPackageSpawnFighterWriteRejected
              << " fighter_package_invalid_call_script_write_rejected=" << invalidPackageCallScriptWriteRejected
              << " fighter_package_invalid_subaction_call_script_write_rejected=" << invalidPackageSubactionCallScriptWriteRejected
              << " fighter_package_invalid_fighter_destroy_write_rejected=" << invalidPackageFighterDestroyWriteRejected
              << " fighter_package_invalid_fighter_object_context_write_rejected=" << invalidPackageFighterObjectContextWriteRejected
              << " fighter_package_invalid_fact_write_rejected=" << invalidPackageFactWriteRejected
              << " fighter_package_invalid_input_write_rejected=" << invalidPackageInputWriteRejected
              << " fighter_package_invalid_scale_write_rejected=" << invalidPackageScaleWriteRejected
              << " fighter_package_invalid_var_motion_write_rejected=" << invalidPackageVarMotionWriteRejected
              << " fighter_package_invalid_spawn_object_vars_write_rejected=" << invalidPackageSpawnObjectVarsWriteRejected
              << " fighter_package_invalid_object_switch_write_rejected=" << invalidPackageObjectSwitchWriteRejected
              << " fighter_package_invalid_object_input_write_rejected=" << invalidPackageObjectInputWriteRejected
              << " fighter_package_object_fighter_context_write_ok=" << packageObjectFighterContextWriteAccepted
              << " fighter_package_invalid_object_context_write_rejected=" << invalidPackageObjectContextWriteRejected
              << " fighter_package_state_alias_write_ok=" << packageStateAliasWriteOk
              << " fighter_package_invalid_object_state_alias_write_rejected=" << invalidPackageObjectStateAliasWriteRejected
              << " sandbag_roster_ok=" << sandbagRosterOk
              << "\n";

    pf::AnimationClip rootMotionClip;
    rootMotionClip.name = "smoke_root_motion";
    rootMotionClip.frameCount = pf::fx(10);
    rootMotionClip.tracks = {
        {0, pf::AnimationChannel::TranslateX, {
            {0, 0, 0, pf::AnimationInterpolation::Linear},
            {pf::fx(10), pf::fx(5), 0, pf::AnimationInterpolation::Linear},
        }},
        {0, pf::AnimationChannel::TranslateY, {
            {0, pf::fx(1), 0, pf::AnimationInterpolation::Constant},
            {pf::fx(10), pf::fx(3), 0, pf::AnimationInterpolation::Constant},
        }},
    };
    std::vector<pf::AnimationJoint> skeleton = {
        {-1, "TransN", {}, {}, {pf::fx(1), pf::fx(1), pf::fx(1)}},
    };
    pf::AnimationPose pose = pf::evaluateClip(skeleton, rootMotionClip, pf::fx(5));
    pf::Vec3 rootDelta = pf::rootTranslationDelta(rootMotionClip, 0, 0, pf::fx(5));
    std::cout << "animation_pose_root=(" << pf::fxToFloat(pose.joints[0].translation.x)
              << ", " << pf::fxToFloat(pose.joints[0].translation.y)
              << ") root_delta=" << pf::toString({rootDelta.x, rootDelta.y})
              << "\n";

    for (const pf::FighterDefinition& def : world.fighterDefs) {
        if (!def.hsdAsset) {
            continue;
        }
        const pf::HsdFighterAnimationAsset& asset = *def.hsdAsset;
        const pf::AnimationClip* dashClip = pf::findClipByActionIndex(asset, 12);
        std::cout << "hsd_asset_name=" << asset.name
                  << " fighter=" << def.name
                  << " joints=" << asset.skeleton.size()
                  << " hurtboxes=" << asset.hurtboxes.size()
                  << " head_bone=" << asset.fighterBones.head
                  << " shield_bone=" << asset.fighterBones.shield
                  << " shield_size=" << (asset.hasAttributes ? pf::fxToFloat(asset.attributes.shieldSize) : -1.0f)
                  << " clips=" << asset.clips.size()
                  << " action_scripts=" << asset.actionScripts.size()
                  << " dash_frames=" << (dashClip ? pf::fxToFloat(dashClip->frameCount) : -1.0f)
                  << "\n";
    }
    return 0;
}
