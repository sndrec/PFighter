#include "core/simulation.hpp"
#include "core/animation.hpp"
#include "core/animation_asset.hpp"
#include "core/replay.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

static pf::FighterRuntime& setOnLeftLedge(pf::World& world) {
    pf::FighterRuntime& fighter = world.fighters[0];
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

    pf::World dashWorld = pf::makeTrainingWorld();
    dashWorld.stage.segments = {
        {{-pf::fx(100), 0}, {pf::fx(100), 0}, pf::fx(1), pf::SegmentType::Solid, false, false},
    };
    dashWorld.stage.ledges.clear();
    dashWorld.fighters[0].position = {0, 0};
    dashWorld.fighters[0].previousPosition = dashWorld.fighters[0].position;
    for (int frame = 0; frame < 60; ++frame) {
        pf::InputFrame input;
        input.move.x = pf::fx(1);
        pf::tickWorld(dashWorld, {input, pf::InputFrame{}});
    }
    const pf::FighterRuntime& dashTester = dashWorld.fighters[0];
    std::cout << "dash_state=" << pf::currentState(dashWorld, dashTester).name
              << " ground_vel=" << pf::fxToFloat(dashTester.groundVelocity)
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
    pf::InputFrame smashTurnDashInput;
    smashTurnDashInput.move.x = -pf::fx(1);
    pf::tickWorld(smashTurnDashWorld, {smashTurnDashInput, pf::InputFrame{}});
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

    pf::World airDodgeWorld = pf::makeTrainingWorld();
    airDodgeWorld.stage.segments = {
        {{-pf::fx(100), -pf::fx(20)}, {pf::fx(100), -pf::fx(20)}, pf::fx(1), pf::SegmentType::Solid, false, false},
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
    ceilingWorld.stage.segments = {
        {{-pf::fx(4), pf::fx(3)}, {pf::fx(4), pf::fx(3)}, pf::fx(1), pf::SegmentType::Solid, false, false, pf::SegmentLineKind::Ceiling},
    };
    ceilingWorld.stage.ledges.clear();
    pf::FighterRuntime& ceilingTester = ceilingWorld.fighters[0];
    ceilingTester.position = {0, pf::fxFromFloat(0.2f)};
    ceilingTester.previousPosition = ceilingTester.position;
    ceilingTester.grounded = false;
    ceilingTester.groundSegment = -1;
    ceilingTester.fighterVelocity = {0, pf::fx(2)};
    pf::changeFighterState(ceilingWorld, ceilingTester, "Fall");
    pf::tickWorld(ceilingWorld, {pf::InputFrame{}, pf::InputFrame{}});
    std::cout << "ceiling_collision_state=" << pf::currentState(ceilingWorld, ceilingTester).name
              << " pos=" << pf::toString(ceilingTester.position)
              << " vel=" << pf::toString(ceilingTester.fighterVelocity)
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

    pf::World cliffJumpWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffJumpTester = setOnLeftLedge(cliffJumpWorld);
    pf::InputFrame jumpInput;
    jumpInput.buttons |= pf::ButtonJump;
    pf::tickWorld(cliffJumpWorld, {jumpInput, pf::InputFrame{}});
    for (int frame = 0; frame < 8; ++frame) {
        pf::tickWorld(cliffJumpWorld, {pf::InputFrame{}, pf::InputFrame{}});
    }
    std::cout << "cliff_jump_state=" << pf::currentState(cliffJumpWorld, cliffJumpTester).name
              << " vel=" << pf::toString(cliffJumpTester.fighterVelocity)
              << "\n";

    pf::World cliffClimbWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffClimbTester = setOnLeftLedge(cliffClimbWorld);
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
    std::cout << "cliff_drop_state=" << pf::currentState(cliffDropWorld, cliffDropTester).name
              << " ledge=" << cliffDropTester.grabbedLedge
              << " vel=" << pf::toString(cliffDropTester.fighterVelocity)
              << "\n";

    pf::World cliffAttackWorld = pf::makeTrainingWorld();
    pf::FighterRuntime& cliffAttackTester = setOnLeftLedge(cliffAttackWorld);
    pf::InputFrame attackInput;
    attackInput.buttons |= pf::ButtonAttack;
    pf::tickWorld(cliffAttackWorld, {attackInput, pf::InputFrame{}});
    std::cout << "cliff_attack_state=" << pf::currentState(cliffAttackWorld, cliffAttackTester).name
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
