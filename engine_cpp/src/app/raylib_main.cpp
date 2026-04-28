#if PFIGHTER_WITH_RAYLIB
#include "core/simulation.hpp"
#include "editor/fighter_editor.hpp"

#include <raylib.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

static Vector3 toRay(pf::Vec3 v) {
    return {pf::fxToFloat(v.x), pf::fxToFloat(v.y), pf::fxToFloat(v.z)};
}

static Vector3 toRayGround(pf::Vec2 v) {
    return {pf::fxToFloat(v.x), pf::fxToFloat(v.y), 0.0f};
}

static pf::InputFrame readKeyboardInput(bool arrows) {
    pf::InputFrame input;
    if (arrows) {
        if (IsKeyDown(KEY_LEFT)) input.move.x -= pf::fx(1);
        if (IsKeyDown(KEY_RIGHT)) input.move.x += pf::fx(1);
        if (IsKeyDown(KEY_UP)) input.move.y += pf::fx(1);
        if (IsKeyDown(KEY_DOWN)) input.move.y -= pf::fx(1);
        if (IsKeyDown(KEY_RIGHT_SHIFT)) input.buttons |= pf::ButtonJump;
        if (IsKeyPressed(KEY_ENTER)) input.buttons |= pf::ButtonAttack;
        if (IsKeyDown(KEY_RIGHT_CONTROL)) input.buttons |= pf::ButtonShield;
    } else {
        if (IsKeyDown(KEY_A)) input.move.x -= pf::fx(1);
        if (IsKeyDown(KEY_D)) input.move.x += pf::fx(1);
        if (IsKeyDown(KEY_W)) input.move.y += pf::fx(1);
        if (IsKeyDown(KEY_S)) input.move.y -= pf::fx(1);
        if (IsKeyDown(KEY_W)) input.buttons |= pf::ButtonJump;
        if (IsKeyPressed(KEY_F)) input.buttons |= pf::ButtonAttack;
        if (IsKeyDown(KEY_Q)) input.buttons |= pf::ButtonShield;
    }
    return input;
}

static void drawCapsule(pf::Vec3 a, pf::Vec3 b, pf::Fix radius, Color color) {
    DrawSphereWires(toRay(a), pf::fxToFloat(radius), 8, 8, color);
    DrawSphereWires(toRay(b), pf::fxToFloat(radius), 8, 8, color);
    DrawLine3D(toRay(a), toRay(b), color);
}

static void drawEcb(const pf::FighterRuntime& fighter, Color color) {
    for (size_t i = 0; i < fighter.ecb.points.size(); ++i) {
        const pf::Vec2 a = fighter.position + fighter.ecb.points[i];
        const pf::Vec2 b = fighter.position + fighter.ecb.points[(i + 1) % fighter.ecb.points.size()];
        DrawLine3D(toRayGround(a), toRayGround(b), color);
    }
}

static void drawImportedSkeleton(const pf::FighterDefinition& def, const pf::FighterRuntime& fighter, Color color) {
    if (!def.hsdAsset || fighter.hsdJointWorldPositions.empty()) {
        return;
    }

    const size_t jointCount = std::min(def.hsdAsset->skeleton.size(), fighter.hsdJointWorldPositions.size());
    for (size_t i = 0; i < jointCount; ++i) {
        pf::Vec3 joint = fighter.hsdJointWorldPositions[i];
        const int parent = def.hsdAsset->skeleton[i].parent;
        if (parent >= 0 && static_cast<size_t>(parent) < fighter.hsdJointWorldPositions.size()) {
            pf::Vec3 parentJoint = fighter.hsdJointWorldPositions[static_cast<size_t>(parent)];
            DrawLine3D(toRay(parentJoint), toRay(joint), color);
        }
        DrawSphere(toRay(joint), 0.04f, color);
    }

    const int head = def.hsdAsset->fighterBones.head;
    if (head >= 0 && static_cast<size_t>(head) < fighter.hsdJointWorldPositions.size()) {
        pf::Vec3 headJoint = fighter.hsdJointWorldPositions[static_cast<size_t>(head)];
        DrawSphereWires(toRay(headJoint), 0.18f, 10, 6, color);
    }
}

static bool drawsShield(const pf::FighterState& state) {
    return state.name == "GuardOn" || state.name == "Guard" || state.name == "GuardSetOff" || state.name == "GuardReflect";
}

static pf::Fix shieldRadius(const pf::FighterDefinition& def, const pf::FighterRuntime& fighter) {
    const pf::MeleeCommonData& common = def.properties.common;
    const pf::Fix healthRatio = def.shield.maxHealth > 0 ? pf::fxDiv(fighter.shieldHealth, def.shield.maxHealth) : 0;
    const pf::Fix light = fighter.input.down(pf::ButtonShield) ? std::clamp(fighter.input.frames[0].shieldAnalog, pf::Fix{0}, pf::fx(1)) : 0;
    const pf::Fix sizeScale = common.hardShieldSizeScaleX2D4 +
        pf::fxMul(light, common.lightShieldSizeScaleX2D8 - common.hardShieldSizeScaleX2D4);
    const pf::Fix scaledHealth = pf::fxMul(healthRatio, sizeScale);
    return pf::fxMul(def.shield.startSizeHardShield, common.minShieldScaleX264 + pf::fxMul(pf::fx(1) - common.minShieldScaleX264, scaledHealth));
}

static void drawFighter(const pf::World& world, const pf::FighterRuntime& fighter, Color color, bool boxes) {
    const pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const Vector3 pos = toRayGround(fighter.position);
    const bool hasImportedPose = def.hsdAsset && !fighter.hsdJointWorldPositions.empty();
    if (hasImportedPose) {
        DrawCylinder(pos, 0.18f, 0.18f, 0.04f, 18, Fade(color, 0.45f));
        drawImportedSkeleton(def, fighter, color);
    } else {
        DrawCube(pos, 0.55f, 1.1f, 0.35f, color);
        DrawCubeWires(pos, 0.55f, 1.1f, 0.35f, BLACK);
    }

    if (!def.hasHsdAsset && (!hasImportedPose || boxes)) {
        for (int i = 0; i < pf::kBoneCount; ++i) {
            pf::Vec3 bone = fighter.bones[static_cast<size_t>(i)].position;
            bone.x += fighter.position.x;
            bone.y += fighter.position.y;
            DrawSphere(toRay(bone), hasImportedPose ? 0.04f : 0.07f, hasImportedPose ? Fade(DARKBLUE, 0.35f) : DARKBLUE);
        }
    }

    if (drawsShield(pf::currentState(world, fighter)) && fighter.shieldHealth > 0) {
        pf::Vec3 center = fighter.bones[static_cast<size_t>(pf::BoneId::Hip)].position;
        center.x += fighter.position.x;
        center.y += fighter.position.y + pf::fxFromFloat(0.2f);
        if (def.hasHsdAsset && def.hsdAsset) {
            const int shieldBone = def.hsdAsset->fighterBones.shield;
            if (shieldBone >= 0 && static_cast<size_t>(shieldBone) < fighter.hsdJointWorldTransforms.size()) {
                center = pf::transformPoint(fighter.hsdJointWorldTransforms[static_cast<size_t>(shieldBone)], {});
            }
        }
        DrawSphereWires(toRay(center), pf::fxToFloat(shieldRadius(def, fighter)), 18, 10, VIOLET);
    }

    if (!boxes) {
        return;
    }

    drawEcb(fighter, YELLOW);
    if (!def.hasHsdAsset) {
        for (const pf::HurtboxDefinition& hurt : def.hurtboxes) {
            pf::Vec3 base = fighter.bones[static_cast<size_t>(hurt.bone)].position;
            base.x += fighter.position.x;
            base.y += fighter.position.y;
            drawCapsule(base + hurt.startOffset, base + hurt.endOffset, hurt.radius, BLUE);
        }
    }
    for (const pf::Capsule& hurt : fighter.hsdHurtboxCapsules) {
        drawCapsule(hurt.a, hurt.b, hurt.radius, GREEN);
    }
    drawImportedSkeleton(def, fighter, DARKGREEN);
    for (const pf::ActiveHitbox& hit : fighter.activeHitboxes) {
        drawCapsule(hit.previous, hit.current, hit.def.radius, RED);
    }
}

static void drawEditor(const pf::World& world, pf::FighterEditor& editor) {
    editor.clampToWorld(world);
    const pf::FighterRuntime& fighter = world.fighters[static_cast<size_t>(editor.selectedFighter)];
    const pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const pf::FighterState& state = def.states[static_cast<size_t>(editor.selectedState)];

    DrawRectangle(12, 12, 390, 210, Fade(RAYWHITE, 0.92f));
    DrawRectangleLines(12, 12, 390, 210, DARKGRAY);
    DrawText("PFighter C++ prototype editor", 24, 24, 18, BLACK);
    DrawText(("Fighter: " + def.name).c_str(), 24, 54, 16, DARKGRAY);
    DrawText(("Live state: " + pf::currentState(world, fighter).name).c_str(), 24, 76, 16, DARKGRAY);
    DrawText(("Selected state: " + state.name).c_str(), 24, 98, 16, DARKGRAY);
    DrawText(("Frame in state: " + std::to_string(pf::frameInState(fighter))).c_str(), 24, 120, 16, DARKGRAY);
    DrawText(("Subactions: " + std::to_string(state.action.size())).c_str(), 24, 142, 16, DARKGRAY);
    DrawText(("HSD pose: " + std::to_string(fighter.hsdJointWorldPositions.size()) + " joints, " +
              std::to_string(fighter.hsdHurtboxCapsules.size()) + " hurtboxes").c_str(), 24, 164, 14, DARKGRAY);
    DrawText("F1 boxes  F2 side view  [/] state  Space pause  R reset  1-7 fighter", 24, 190, 14, GRAY);
}

int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, "PFighter C++ raylib prototype");
    SetTargetFPS(60);

    Camera3D camera{};
    camera.position = {0.0f, 55.0f, 145.0f};
    camera.target = {0.0f, 10.0f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    int testFighterDef = 0;
    pf::World world = pf::makeTrainingWorld(testFighterDef, testFighterDef);
    pf::FighterEditor editor;
    const std::array<int, 7> fighterKeys{
        KEY_ONE, KEY_TWO, KEY_THREE, KEY_FOUR, KEY_FIVE, KEY_SIX, KEY_SEVEN,
    };

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_F1)) editor.showBoxes = !editor.showBoxes;
        if (IsKeyPressed(KEY_F2)) editor.sideView = !editor.sideView;
        if (IsKeyPressed(KEY_SPACE)) editor.paused = !editor.paused;
        if (IsKeyPressed(KEY_R)) world = pf::makeTrainingWorld(testFighterDef, testFighterDef);
        if (IsKeyPressed(KEY_LEFT_BRACKET)) --editor.selectedState;
        if (IsKeyPressed(KEY_RIGHT_BRACKET)) ++editor.selectedState;
        for (size_t i = 0; i < fighterKeys.size(); ++i) {
            if (IsKeyPressed(fighterKeys[i]) && i < world.fighterDefs.size()) {
                testFighterDef = static_cast<int>(i);
                world = pf::makeTrainingWorld(testFighterDef, testFighterDef);
                editor.selectedState = 0;
                editor.selectedSubaction = 0;
            }
        }

        if (!editor.paused) {
            pf::tickWorld(world, {readKeyboardInput(false), readKeyboardInput(true)});
        }

        if (editor.sideView) {
            camera.position = {0.0f, 12.0f, 145.0f};
            camera.target = {0.0f, 12.0f, 0.0f};
            camera.fovy = 120.0f;
            camera.projection = CAMERA_ORTHOGRAPHIC;
        } else {
            camera.position = {0.0f, 55.0f, 145.0f};
            camera.target = {0.0f, 10.0f, 0.0f};
            camera.fovy = 45.0f;
            camera.projection = CAMERA_PERSPECTIVE;
        }

        BeginDrawing();
        ClearBackground({205, 214, 222, 255});
        BeginMode3D(camera);
        DrawGrid(40, 10.0f);
        for (const pf::StageSegment& segment : world.stage.segments) {
            DrawLine3D(toRayGround(segment.start), toRayGround(segment.end), segment.type == pf::SegmentType::Solid ? BLACK : DARKGREEN);
        }
        if (editor.showBoxes) {
            for (const pf::StageLedge& ledge : world.stage.ledges) {
                const Vector3 ledgePos = toRayGround(ledge.position);
                DrawSphere(ledgePos, 0.12f, PURPLE);
                DrawLine3D(ledgePos, {ledgePos.x + 0.45f * static_cast<float>(ledge.direction), ledgePos.y, ledgePos.z}, PURPLE);
            }
        }
        drawFighter(world, world.fighters[0], ORANGE, editor.showBoxes);
        drawFighter(world, world.fighters[1], SKYBLUE, editor.showBoxes);
        EndMode3D();
        drawEditor(world, editor);
        DrawText("P1: A/D move, W jump, F attack, Q shield    P2: arrows move, Right Shift jump, Enter attack, Right Ctrl shield", 24, 680, 16, DARKGRAY);
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
#endif
