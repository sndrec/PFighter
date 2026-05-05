#if PFIGHTER_WITH_RAYLIB
#include "core/fighter_package.hpp"
#include "core/replay.hpp"
#include "core/simulation.hpp"
#include "editor/fighter_editor.hpp"

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

enum class AppMode {
    MainMenu,
    Gameplay,
    Editor,
};

static Vector3 toRay(pf::Vec3 v) {
    return {pf::fxToFloat(v.x), pf::fxToFloat(v.y), pf::fxToFloat(v.z)};
}

static Vector3 toRayGround(pf::Vec2 v) {
    return {pf::fxToFloat(v.x), pf::fxToFloat(v.y), 0.0f};
}

static Matrix toRayMatrix(const std::array<float, 16>& m) {
    return {
        m[0], m[1], m[2], m[3],
        m[4], m[5], m[6], m[7],
        m[8], m[9], m[10], m[11],
        m[12], m[13], m[14], m[15],
    };
}

static Matrix toRayMatrix(const std::array<pf::Fix, 16>& m) {
    return {
        pf::fxToFloat(m[0]), pf::fxToFloat(m[1]), pf::fxToFloat(m[2]), pf::fxToFloat(m[3]),
        pf::fxToFloat(m[4]), pf::fxToFloat(m[5]), pf::fxToFloat(m[6]), pf::fxToFloat(m[7]),
        pf::fxToFloat(m[8]), pf::fxToFloat(m[9]), pf::fxToFloat(m[10]), pf::fxToFloat(m[11]),
        pf::fxToFloat(m[12]), pf::fxToFloat(m[13]), pf::fxToFloat(m[14]), pf::fxToFloat(m[15]),
    };
}

static std::array<float, 16> multiplyRowMajor(const std::array<float, 16>& a, const std::array<float, 16>& b) {
    std::array<float, 16> result{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float value = 0.0f;
            for (int k = 0; k < 4; ++k) {
                value += a[static_cast<size_t>(row * 4 + k)] * b[static_cast<size_t>(k * 4 + col)];
            }
            result[static_cast<size_t>(row * 4 + col)] = value;
        }
    }
    return result;
}

static std::array<float, 16> toFloatMatrix(const pf::JointWorldTransform& transform) {
    std::array<float, 16> matrix{};
    for (size_t i = 0; i < matrix.size(); ++i) {
        matrix[i] = pf::fxToFloat(transform.matrix[i]);
    }
    return matrix;
}

static pf::Fix axisToFix(float value) {
    return pf::fxFromFloat(std::clamp(value, -1.0f, 1.0f));
}

static pf::Vec2 readGamepadStick(int gamepad, GamepadAxis xAxis, GamepadAxis yAxis) {
    constexpr float kDeadzone = 0.18f;
    float x = GetGamepadAxisMovement(gamepad, xAxis);
    float y = -GetGamepadAxisMovement(gamepad, yAxis);
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= kDeadzone) {
        return {};
    }
    const float scaled = std::min(1.0f, (magnitude - kDeadzone) / (1.0f - kDeadzone));
    const float normalX = x / magnitude;
    const float normalY = y / magnitude;
    return {axisToFix(normalX * scaled), axisToFix(normalY * scaled)};
}

static float readGamepadTrigger(int gamepad, GamepadAxis axis) {
    return std::clamp(GetGamepadAxisMovement(gamepad, axis), 0.0f, 1.0f);
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
        if (IsKeyPressed(KEY_RIGHT_ALT)) input.buttons |= pf::ButtonTaunt;
    } else {
        if (IsKeyDown(KEY_A)) input.move.x -= pf::fx(1);
        if (IsKeyDown(KEY_D)) input.move.x += pf::fx(1);
        if (IsKeyDown(KEY_W)) input.move.y += pf::fx(1);
        if (IsKeyDown(KEY_S)) input.move.y -= pf::fx(1);
        if (IsKeyDown(KEY_W)) input.buttons |= pf::ButtonJump;
        if (IsKeyPressed(KEY_F)) input.buttons |= pf::ButtonAttack;
        if (IsKeyDown(KEY_Q)) input.buttons |= pf::ButtonShield;
        if (IsKeyDown(KEY_Z)) input.buttons |= pf::ButtonGrab;
        if (IsKeyPressed(KEY_E)) input.buttons |= pf::ButtonTaunt;
    }
    return input;
}

static pf::InputFrame mergeInput(pf::InputFrame a, pf::InputFrame b) {
    pf::InputFrame merged;
    merged.move.x = std::clamp(a.move.x + b.move.x, -pf::fx(1), pf::fx(1));
    merged.move.y = std::clamp(a.move.y + b.move.y, -pf::fx(1), pf::fx(1));
    merged.cStick.x = std::clamp(a.cStick.x + b.cStick.x, -pf::fx(1), pf::fx(1));
    merged.cStick.y = std::clamp(a.cStick.y + b.cStick.y, -pf::fx(1), pf::fx(1));
    merged.shieldAnalog = std::max(a.shieldAnalog, b.shieldAnalog);
    merged.buttons = a.buttons | b.buttons;
    return merged;
}

static pf::InputFrame readGamepadInput(int gamepad) {
    pf::InputFrame input;
    if (!IsGamepadAvailable(gamepad)) {
        return input;
    }

    input.move = readGamepadStick(gamepad, GAMEPAD_AXIS_LEFT_X, GAMEPAD_AXIS_LEFT_Y);
    input.cStick = readGamepadStick(gamepad, GAMEPAD_AXIS_RIGHT_X, GAMEPAD_AXIS_RIGHT_Y);

    if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) {
        input.buttons |= pf::ButtonAttack;
    }
    if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT)) {
        input.buttons |= pf::ButtonSpecial;
    }
    if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT) ||
        IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
    {
        input.buttons |= pf::ButtonJump;
    }
    if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
    {
        input.buttons |= pf::ButtonShield;
        input.shieldAnalog = 0;
    }
    if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1)) {
        input.buttons |= pf::ButtonGrab;
    }
    if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_UP)) {
        input.buttons |= pf::ButtonTaunt;
    }

    const float leftTrigger = readGamepadTrigger(gamepad, GAMEPAD_AXIS_LEFT_TRIGGER);
    const float rightTrigger = readGamepadTrigger(gamepad, GAMEPAD_AXIS_RIGHT_TRIGGER);
    const float trigger = std::max(leftTrigger, rightTrigger);
    if (trigger > 0.05f) {
        input.buttons |= pf::ButtonShield;
        input.shieldAnalog = pf::fxFromFloat(trigger);
    }

    return input;
}

static pf::InputFrame readPlayerInput(int player, bool arrows) {
    return mergeInput(readKeyboardInput(arrows), readGamepadInput(player));
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

static void drawGroundRect(pf::Fix left, pf::Fix bottom, pf::Fix right, pf::Fix top, Color color) {
    const pf::Vec2 bl{left, bottom};
    const pf::Vec2 br{right, bottom};
    const pf::Vec2 tr{right, top};
    const pf::Vec2 tl{left, top};
    DrawLine3D(toRayGround(bl), toRayGround(br), color);
    DrawLine3D(toRayGround(br), toRayGround(tr), color);
    DrawLine3D(toRayGround(tr), toRayGround(tl), color);
    DrawLine3D(toRayGround(tl), toRayGround(bl), color);
}

static pf::Vec2 ecbJointProjection(const pf::FighterRuntime& fighter, pf::Vec3 joint) {
    return {fighter.position.x + fighter.facing * joint.z, joint.y};
}

static void drawNativeEcbSourceJoints(const pf::FighterDefinition& def, const pf::FighterRuntime& fighter) {
    if (fighter.jointWorldPositions.empty()) {
        return;
    }

    bool haveExtents = false;
    pf::Fix minHorizontal = 0;
    pf::Fix maxHorizontal = 0;
    if (fighter.jointWorldPositions.size() > 1) {
        const pf::Vec2 topN = ecbJointProjection(fighter, fighter.jointWorldPositions[1]);
        DrawSphere(toRayGround(topN), 0.08f, MAGENTA);
    }

    for (int bone : def.environmentCollisionBones) {
        if (bone < 0 || static_cast<size_t>(bone) >= fighter.jointWorldPositions.size()) {
            continue;
        }
        const pf::Vec3 source = fighter.jointWorldPositions[static_cast<size_t>(bone)];
        if (!haveExtents) {
            minHorizontal = source.z;
            maxHorizontal = source.z;
            haveExtents = true;
        } else {
            minHorizontal = std::min(minHorizontal, source.z);
            maxHorizontal = std::max(maxHorizontal, source.z);
        }
        DrawSphere(toRay(source), 0.055f, Fade(ORANGE, 0.75f));
        DrawSphere(toRayGround(ecbJointProjection(fighter, source)), 0.065f, GOLD);
    }

    if (haveExtents && fighter.jointWorldPositions.size() > 1) {
        const pf::Vec2 topN = ecbJointProjection(fighter, fighter.jointWorldPositions[1]);
        const pf::Fix halfWidth = pf::fxMul(pf::fxAbs(maxHorizontal - minHorizontal), pf::fxFromFloat(0.5f));
        const pf::Fix boxReach = halfWidth + def.properties.ledgeSnapX;
        const pf::Fix boxBottom = topN.y + def.properties.ledgeSnapY - pf::fxMul(def.properties.ledgeSnapHeight, pf::fxFromFloat(0.5f));
        const pf::Fix boxTop = topN.y + def.properties.ledgeSnapY + pf::fxMul(def.properties.ledgeSnapHeight, pf::fxFromFloat(0.5f));
        drawGroundRect(topN.x - boxReach, boxBottom, topN.x, boxTop, Fade(RED, 0.45f));
        drawGroundRect(topN.x, boxBottom, topN.x + boxReach, boxTop, Fade(BLUE, 0.45f));
    }
}

static void drawLedgeSnapSweep(const pf::FighterDefinition& def,
                               const pf::FighterRuntime& fighter,
                               const pf::StageLedge& ledge,
                               Color color) {
    const pf::FighterProperties& attr = def.properties;
    const pf::Fix halfHeight = pf::fxMul(attr.ledgeSnapHeight, pf::fxFromFloat(0.5f));
    const pf::Fix prevX = fighter.previousPosition.x;
    const pf::Fix curX = fighter.position.x;
    const pf::Fix prevY = fighter.previousPosition.y;
    const pf::Fix curY = fighter.position.y;
    const pf::Fix bottom = std::min(prevY, curY) + attr.ledgeSnapY - halfHeight;
    const pf::Fix top = std::max(prevY, curY) + attr.ledgeSnapY + halfHeight;

    pf::Fix left = 0;
    pf::Fix right = 0;
    if (ledge.direction < 0) {
        left = std::min(prevX, curX);
        right = attr.ledgeSnapX +
            (prevX < curX ? curX + fighter.ecb.points[2].x : prevX + fighter.ecb.points[2].x);
    } else {
        right = std::max(prevX, curX);
        left = -attr.ledgeSnapX +
            (prevX > curX ? curX + fighter.ecb.points[0].x : prevX + fighter.ecb.points[0].x);
    }
    drawGroundRect(left, bottom, right, top, color);
}

static const std::vector<pf::AnimationJoint>* animationSkeletonForDrawing(const pf::FighterDefinition& def) {
    return def.authoredSkeleton.empty() ? nullptr : &def.authoredSkeleton;
}

static void drawAnimationSkeleton(const pf::FighterDefinition& def, const pf::FighterRuntime& fighter, Color color) {
    const std::vector<pf::AnimationJoint>* skeleton = animationSkeletonForDrawing(def);
    if (!skeleton || fighter.jointWorldPositions.empty()) {
        return;
    }

    const size_t jointCount = std::min(skeleton->size(), fighter.jointWorldPositions.size());
    for (size_t i = 0; i < jointCount; ++i) {
        pf::Vec3 joint = fighter.jointWorldPositions[i];
        const int parent = (*skeleton)[i].parent;
        if (parent >= 0 && static_cast<size_t>(parent) < fighter.jointWorldPositions.size()) {
            pf::Vec3 parentJoint = fighter.jointWorldPositions[static_cast<size_t>(parent)];
            DrawLine3D(toRay(parentJoint), toRay(joint), color);
        }
        DrawSphere(toRay(joint), 0.04f, color);
    }

    const int head = def.fighterBones.head;
    if (head >= 0 && static_cast<size_t>(head) < fighter.jointWorldPositions.size()) {
        pf::Vec3 headJoint = fighter.jointWorldPositions[static_cast<size_t>(head)];
        DrawSphereWires(toRay(headJoint), 0.18f, 10, 6, color);
    } else if (fighter.jointWorldPositions.size() > 1) {
        pf::Vec3 tipJoint = fighter.jointWorldPositions.back();
        DrawSphereWires(toRay(tipJoint), 0.12f, 8, 4, color);
    }
}

namespace {

constexpr int kHsdShaderMaxBones = 200;
constexpr uint32_t kPobjCullBack = 1u << 14;
constexpr uint32_t kPobjCullFront = 1u << 15;
constexpr uint32_t kJObjSkeletonRoot = 1u << 1;

struct FighterMeshRenderBatch {
    unsigned int vao = 0;
    std::array<unsigned int, 8> vbo{};
    int vertexCount = 0;
    int parentBone = -1;
    int singleBindBone = -1;
    int dobjIndex = -1;
    int modelPartIndex = -1;
    int modelPartState = -1;
    bool hiddenByVisibilityTable = false;
    uint32_t parentFlags = 0;
    uint32_t polygonFlags = 0;
    bool hasEnvelopes = false;
    bool unknown2 = false;
    bool shapeSetAverage = false;
    int texture = -1;
    int textureColorOperation = 0;
    int textureAlphaOperation = 0;
    float textureBlend = 0.0f;
    std::array<uint8_t, 4> materialColor{255, 255, 255, 255};
};

struct FighterMeshRenderCache {
    Shader shader{};
    int locMvp = -1;
    int locParentMatrix = -1;
    int locTexture0 = -1;
    int locHasTexture = -1;
    int locTextureColorOperation = -1;
    int locTextureAlphaOperation = -1;
    int locTextureBlend = -1;
    int locHasEnvelopes = -1;
    int locUnknown2 = -1;
    int locShapeSetAverage = -1;
    int locParentIsSkeletonRoot = -1;
    int locMaterialColor = -1;
    unsigned int boneUniformBuffer = 0;
    std::vector<Texture2D> textures;
    std::vector<FighterMeshRenderBatch> batches;
};

#ifndef APIENTRY
#if defined(_WIN32)
#define APIENTRY __stdcall
#else
#define APIENTRY
#endif
#endif

using GlGenBuffersFn = void (APIENTRY*)(int, unsigned int*);
using GlBindBufferFn = void (APIENTRY*)(unsigned int, unsigned int);
using GlBufferDataFn = void (APIENTRY*)(unsigned int, ptrdiff_t, const void*, unsigned int);
using GlBufferSubDataFn = void (APIENTRY*)(unsigned int, ptrdiff_t, ptrdiff_t, const void*);
using GlBindBufferBaseFn = void (APIENTRY*)(unsigned int, unsigned int, unsigned int);
using GlGetUniformBlockIndexFn = unsigned int (APIENTRY*)(unsigned int, const char*);
using GlUniformBlockBindingFn = void (APIENTRY*)(unsigned int, unsigned int, unsigned int);

struct GlUniformBufferApi {
    GlGenBuffersFn genBuffers = nullptr;
    GlBindBufferFn bindBuffer = nullptr;
    GlBufferDataFn bufferData = nullptr;
    GlBufferSubDataFn bufferSubData = nullptr;
    GlBindBufferBaseFn bindBufferBase = nullptr;
    GlGetUniformBlockIndexFn getUniformBlockIndex = nullptr;
    GlUniformBlockBindingFn uniformBlockBinding = nullptr;
};

static GlUniformBufferApi& glUniformBufferApi() {
    static GlUniformBufferApi api{
        reinterpret_cast<GlGenBuffersFn>(rlGetProcAddress("glGenBuffers")),
        reinterpret_cast<GlBindBufferFn>(rlGetProcAddress("glBindBuffer")),
        reinterpret_cast<GlBufferDataFn>(rlGetProcAddress("glBufferData")),
        reinterpret_cast<GlBufferSubDataFn>(rlGetProcAddress("glBufferSubData")),
        reinterpret_cast<GlBindBufferBaseFn>(rlGetProcAddress("glBindBufferBase")),
        reinterpret_cast<GlGetUniformBlockIndexFn>(rlGetProcAddress("glGetUniformBlockIndex")),
        reinterpret_cast<GlUniformBlockBindingFn>(rlGetProcAddress("glUniformBlockBinding")),
    };
    return api;
}

static unsigned int createBoneUniformBuffer(Shader shader) {
    constexpr unsigned int kGlUniformBuffer = 0x8A11;
    constexpr unsigned int kGlDynamicDraw = 0x88E8;
    constexpr unsigned int kInvalidIndex = 0xFFFFFFFFu;
    GlUniformBufferApi& api = glUniformBufferApi();
    if (!api.genBuffers || !api.bindBuffer || !api.bufferData || !api.bindBufferBase ||
        !api.getUniformBlockIndex || !api.uniformBlockBinding) {
        return 0;
    }

    unsigned int buffer = 0;
    api.genBuffers(1, &buffer);
    api.bindBuffer(kGlUniformBuffer, buffer);
    api.bufferData(kGlUniformBuffer, sizeof(Matrix) * kHsdShaderMaxBones * 2, nullptr, kGlDynamicDraw);
    api.bindBuffer(kGlUniformBuffer, 0);

    unsigned int blockIndex = api.getUniformBlockIndex(shader.id, "BoneTransforms");
    if (blockIndex != kInvalidIndex) {
        api.uniformBlockBinding(shader.id, blockIndex, 0);
        api.bindBufferBase(kGlUniformBuffer, 0, buffer);
    }
    return buffer;
}

static void appendGpuMatrix(std::vector<float>& out, Matrix matrix) {
    out.push_back(matrix.m0);
    out.push_back(matrix.m1);
    out.push_back(matrix.m2);
    out.push_back(matrix.m3);
    out.push_back(matrix.m4);
    out.push_back(matrix.m5);
    out.push_back(matrix.m6);
    out.push_back(matrix.m7);
    out.push_back(matrix.m8);
    out.push_back(matrix.m9);
    out.push_back(matrix.m10);
    out.push_back(matrix.m11);
    out.push_back(matrix.m12);
    out.push_back(matrix.m13);
    out.push_back(matrix.m14);
    out.push_back(matrix.m15);
}

static std::vector<float> gpuMatrixBlock(const std::vector<Matrix>& boneMatrices, const std::vector<Matrix>& boneWorldMatrices) {
    std::vector<float> block;
    block.reserve(static_cast<size_t>(kHsdShaderMaxBones) * 2 * 16);
    for (Matrix matrix : boneMatrices) {
        appendGpuMatrix(block, matrix);
    }
    for (Matrix matrix : boneWorldMatrices) {
        appendGpuMatrix(block, matrix);
    }
    return block;
}

static void uploadBoneUniformBuffer(unsigned int buffer, const std::vector<Matrix>& boneMatrices, const std::vector<Matrix>& boneWorldMatrices) {
    constexpr unsigned int kGlUniformBuffer = 0x8A11;
    GlUniformBufferApi& api = glUniformBufferApi();
    if (buffer == 0 || !api.bindBuffer || !api.bufferSubData || !api.bindBufferBase) {
        return;
    }
    std::vector<float> block = gpuMatrixBlock(boneMatrices, boneWorldMatrices);
    api.bindBuffer(kGlUniformBuffer, buffer);
    api.bufferSubData(kGlUniformBuffer, 0, static_cast<ptrdiff_t>(block.size() * sizeof(float)), block.data());
    api.bindBufferBase(kGlUniformBuffer, 0, buffer);
    api.bindBuffer(kGlUniformBuffer, 0);
}

const char* fighterMeshVertexShader() {
    return R"glsl(
#version 330
layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) in vec3 vertexNormal;
layout(location = 3) in vec4 vertexColor;
layout(location = 6) in vec4 boneIndex0;
layout(location = 7) in vec4 boneWeight0;
layout(location = 8) in vec2 boneIndex1;
layout(location = 9) in vec2 boneWeight1;

uniform mat4 mvp;
layout(std140) uniform BoneTransforms
{
    mat4 boneMatrices[200];
    mat4 boneWorldMatrices[200];
};
uniform mat4 parentMatrix;
uniform int hasEnvelopes;
uniform int unknown2;
uniform int shapeSetAverage;
uniform int parentIsSkeletonRoot;

out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragNormal;

void addWeightedBone(inout vec4 position, inout vec3 normal, vec4 basePosition, vec3 baseNormal, float bone, float weight, int useBind)
{
    if (weight <= 0.0 || bone < 0.0) return;
    int index = clamp(int(bone + 0.5), 0, 199);
    mat4 transform = useBind == 1 ? boneMatrices[index] : boneWorldMatrices[index];
    position += (transform * basePosition) * weight;
    normal += mat3(transform) * baseNormal * weight;
}

void main()
{
    vec4 basePosition = vec4(vertexPosition, 1.0);
    vec3 baseNormal = vertexNormal;
    vec4 skinnedPosition = basePosition;
    vec3 skinnedNormal = baseNormal;

    if (hasEnvelopes == 1) {
        if (shapeSetAverage == 0 && parentIsSkeletonRoot == 0) {
            basePosition = parentMatrix * basePosition;
            baseNormal = mat3(parentMatrix) * baseNormal;
        }

        if (parentIsSkeletonRoot == 1 &&
            boneWeight0.x == 1.0 &&
            boneWeight0.y == 0.0 && boneWeight0.z == 0.0 && boneWeight0.w == 0.0 &&
            boneWeight1.x == 0.0 && boneWeight1.y == 0.0) {
            skinnedPosition = vec4(0.0);
            skinnedNormal = vec3(0.0);
            addWeightedBone(skinnedPosition, skinnedNormal, basePosition, baseNormal, boneIndex0.x, boneWeight0.x, 0);
        } else {
            skinnedPosition = vec4(0.0);
            skinnedNormal = vec3(0.0);
            addWeightedBone(skinnedPosition, skinnedNormal, basePosition, baseNormal, boneIndex0.x, boneWeight0.x, 1);
            addWeightedBone(skinnedPosition, skinnedNormal, basePosition, baseNormal, boneIndex0.y, boneWeight0.y, 1);
            addWeightedBone(skinnedPosition, skinnedNormal, basePosition, baseNormal, boneIndex0.z, boneWeight0.z, 1);
            addWeightedBone(skinnedPosition, skinnedNormal, basePosition, baseNormal, boneIndex0.w, boneWeight0.w, 1);
            addWeightedBone(skinnedPosition, skinnedNormal, basePosition, baseNormal, boneIndex1.x, boneWeight1.x, 1);
            addWeightedBone(skinnedPosition, skinnedNormal, basePosition, baseNormal, boneIndex1.y, boneWeight1.y, 1);
            if (skinnedPosition.w == 0.0) {
                skinnedPosition = basePosition;
                skinnedNormal = baseNormal;
            }
        }
    } else if (unknown2 == 1) {
        skinnedPosition = vec4(0.0);
        skinnedNormal = vec3(0.0);
        addWeightedBone(skinnedPosition, skinnedNormal, basePosition, baseNormal, boneIndex0.x, boneWeight0.x, 0);
        if (skinnedPosition.w == 0.0) {
            skinnedPosition = parentMatrix * basePosition;
            skinnedNormal = mat3(parentMatrix) * baseNormal;
        }
    } else if (shapeSetAverage == 0) {
        skinnedPosition = parentMatrix * basePosition;
        skinnedNormal = mat3(parentMatrix) * baseNormal;
    }

    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(skinnedNormal);
    gl_Position = mvp * skinnedPosition;
}
)glsl";
}

const char* fighterMeshFragmentShader() {
    return R"glsl(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;

uniform sampler2D texture0;
uniform int hasTexture;
uniform int textureColorOperation;
uniform int textureAlphaOperation;
uniform float textureBlend;
uniform vec4 materialColor;

out vec4 finalColor;

void main()
{
    vec4 texel = hasTexture == 1 ? texture(texture0, fragTexCoord) : vec4(1.0);
    vec4 surface = materialColor;
    if (hasTexture == 1) {
        if (textureColorOperation == 1 || textureColorOperation == 2) {
            surface.rgb = mix(surface.rgb, texel.rgb, texel.a);
        } else if (textureColorOperation == 3) {
            surface.rgb = mix(surface.rgb, texel.rgb, textureBlend);
        } else if (textureColorOperation == 4) {
            surface.rgb *= texel.rgb;
        } else if (textureColorOperation == 5) {
            surface.rgb = texel.rgb;
        } else if (textureColorOperation == 7) {
            surface.rgb += texel.rgb * texel.a;
        } else if (textureColorOperation == 8) {
            surface.rgb -= texel.rgb * texel.a;
        }

        if (textureAlphaOperation == 2) {
            surface.a = mix(surface.a, texel.a, textureBlend);
        } else if (textureAlphaOperation == 3) {
            surface.a *= texel.a;
        } else if (textureAlphaOperation == 4) {
            surface.a = texel.a;
        } else if (textureAlphaOperation == 6) {
            surface.a += texel.a;
        } else if (textureAlphaOperation == 7) {
            surface.a -= texel.a;
        }
    }

    vec3 lightDir = normalize(vec3(-0.35, 0.75, 0.55));
    float diffuse = max(dot(normalize(fragNormal), lightDir), 0.0);
    float light = 0.45 + diffuse * 0.75;
    finalColor = vec4(surface.rgb * fragColor.rgb * light, surface.a * fragColor.a);
}
)glsl";
}

static unsigned int uploadFloatAttribute(int location, int components, const std::vector<float>& data) {
    if (data.empty()) {
        return 0;
    }
    unsigned int vbo = rlLoadVertexBuffer(data.data(), static_cast<int>(data.size() * sizeof(float)), false);
    rlEnableVertexBuffer(vbo);
    rlSetVertexAttribute(static_cast<unsigned int>(location), components, RL_FLOAT, false, 0, 0);
    rlEnableVertexAttribute(static_cast<unsigned int>(location));
    return vbo;
}

static unsigned int uploadColorAttribute(const std::vector<unsigned char>& data) {
    if (data.empty()) {
        return 0;
    }
    unsigned int vbo = rlLoadVertexBuffer(data.data(), static_cast<int>(data.size() * sizeof(unsigned char)), false);
    rlEnableVertexBuffer(vbo);
    rlSetVertexAttribute(3, 4, RL_UNSIGNED_BYTE, true, 0, 0);
    rlEnableVertexAttribute(3);
    return vbo;
}

static Texture2D loadTextureFromRgba(const pf::FighterMeshTexture& texture) {
    Image image{};
    image.data = const_cast<uint8_t*>(texture.rgba.data());
    image.width = texture.width;
    image.height = texture.height;
    image.mipmaps = 1;
    image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    Texture2D loaded = LoadTextureFromImage(image);
    SetTextureFilter(loaded, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(loaded, TEXTURE_WRAP_REPEAT);
    return loaded;
}

static FighterMeshRenderCache createFighterMeshRenderCache(const pf::FighterMesh& mesh) {
    FighterMeshRenderCache cache;
    cache.shader = LoadShaderFromMemory(fighterMeshVertexShader(), fighterMeshFragmentShader());
    cache.locMvp = GetShaderLocation(cache.shader, "mvp");
    cache.locParentMatrix = GetShaderLocation(cache.shader, "parentMatrix");
    cache.locTexture0 = GetShaderLocation(cache.shader, "texture0");
    cache.locHasTexture = GetShaderLocation(cache.shader, "hasTexture");
    cache.locTextureColorOperation = GetShaderLocation(cache.shader, "textureColorOperation");
    cache.locTextureAlphaOperation = GetShaderLocation(cache.shader, "textureAlphaOperation");
    cache.locTextureBlend = GetShaderLocation(cache.shader, "textureBlend");
    cache.locHasEnvelopes = GetShaderLocation(cache.shader, "hasEnvelopes");
    cache.locUnknown2 = GetShaderLocation(cache.shader, "unknown2");
    cache.locShapeSetAverage = GetShaderLocation(cache.shader, "shapeSetAverage");
    cache.locParentIsSkeletonRoot = GetShaderLocation(cache.shader, "parentIsSkeletonRoot");
    cache.locMaterialColor = GetShaderLocation(cache.shader, "materialColor");
    cache.boneUniformBuffer = createBoneUniformBuffer(cache.shader);

    cache.textures.reserve(mesh.textures.size());
    for (const pf::FighterMeshTexture& texture : mesh.textures) {
        if (texture.width > 0 && texture.height > 0 && !texture.rgba.empty()) {
            cache.textures.push_back(loadTextureFromRgba(texture));
        }
    }

    cache.batches.reserve(mesh.batches.size());
    for (const pf::FighterMeshBatch& source : mesh.batches) {
        FighterMeshRenderBatch batch;
        batch.parentBone = source.parentBone;
        batch.singleBindBone = source.singleBindBone;
        batch.dobjIndex = source.dobjIndex;
        batch.modelPartIndex = source.modelPartIndex;
        batch.modelPartState = source.modelPartState;
        batch.hiddenByVisibilityTable = source.hiddenByVisibilityTable;
        batch.parentFlags = source.parentFlags;
        batch.polygonFlags = source.polygonFlags;
        batch.hasEnvelopes = source.hasEnvelopes;
        batch.unknown2 = source.unknown2;
        batch.shapeSetAverage = source.shapeSetAverage;
        batch.texture = source.texture;
        batch.textureColorOperation = source.textureColorOperation;
        batch.textureAlphaOperation = source.textureAlphaOperation;
        batch.textureBlend = source.textureBlend;
        batch.materialColor = source.materialColor;
        batch.vertexCount = static_cast<int>(source.vertices.size());

        std::vector<float> positions;
        std::vector<float> texcoords;
        std::vector<float> normals;
        std::vector<unsigned char> colors;
        std::vector<float> bone0;
        std::vector<float> weight0;
        std::vector<float> bone1;
        std::vector<float> weight1;
        positions.reserve(source.vertices.size() * 3);
        texcoords.reserve(source.vertices.size() * 2);
        normals.reserve(source.vertices.size() * 3);
        colors.reserve(source.vertices.size() * 4);
        bone0.reserve(source.vertices.size() * 4);
        weight0.reserve(source.vertices.size() * 4);
        bone1.reserve(source.vertices.size() * 2);
        weight1.reserve(source.vertices.size() * 2);

        for (const pf::FighterMeshVertex& vertex : source.vertices) {
            positions.push_back(pf::fxToFloat(vertex.position.x));
            positions.push_back(pf::fxToFloat(vertex.position.y));
            positions.push_back(pf::fxToFloat(vertex.position.z));
            texcoords.push_back(vertex.u);
            texcoords.push_back(vertex.v);
            normals.push_back(pf::fxToFloat(vertex.normal.x));
            normals.push_back(pf::fxToFloat(vertex.normal.y));
            normals.push_back(pf::fxToFloat(vertex.normal.z));
            colors.insert(colors.end(), vertex.color.begin(), vertex.color.end());
            for (int i = 0; i < 4; ++i) {
                bone0.push_back(static_cast<float>(vertex.influences[static_cast<size_t>(i)].bone));
                weight0.push_back(vertex.influences[static_cast<size_t>(i)].weight);
            }
            for (int i = 4; i < 6; ++i) {
                bone1.push_back(static_cast<float>(vertex.influences[static_cast<size_t>(i)].bone));
                weight1.push_back(vertex.influences[static_cast<size_t>(i)].weight);
            }
        }

        batch.vao = rlLoadVertexArray();
        rlEnableVertexArray(batch.vao);
        batch.vbo[0] = uploadFloatAttribute(0, 3, positions);
        batch.vbo[1] = uploadFloatAttribute(1, 2, texcoords);
        batch.vbo[2] = uploadFloatAttribute(2, 3, normals);
        batch.vbo[3] = uploadColorAttribute(colors);
        batch.vbo[4] = uploadFloatAttribute(6, 4, bone0);
        batch.vbo[5] = uploadFloatAttribute(7, 4, weight0);
        batch.vbo[6] = uploadFloatAttribute(8, 2, bone1);
        batch.vbo[7] = uploadFloatAttribute(9, 2, weight1);
        rlDisableVertexArray();
        rlDisableVertexBuffer();

        cache.batches.push_back(batch);
    }

    return cache;
}

static FighterMeshRenderCache& fighterMeshRenderCache(const pf::FighterMesh& mesh) {
    static std::unordered_map<const pf::FighterMesh*, std::unique_ptr<FighterMeshRenderCache>> caches;
    auto it = caches.find(&mesh);
    if (it == caches.end()) {
        it = caches.emplace(&mesh, std::make_unique<FighterMeshRenderCache>(createFighterMeshRenderCache(mesh))).first;
    }
    return *it->second;
}

static Matrix parentMatrixForBatch(const FighterMeshRenderBatch& batch,
                                   const std::vector<Matrix>& boneWorldMatrices) {
    int bone = batch.singleBindBone >= 0 ? batch.singleBindBone : batch.parentBone;
    if (bone >= 0 && static_cast<size_t>(bone) < boneWorldMatrices.size()) {
        return boneWorldMatrices[static_cast<size_t>(bone)];
    }
    return MatrixIdentity();
}

static bool isBatchVisible(const FighterMeshRenderBatch& batch, const pf::FighterRuntime& fighter) {
    if (!batch.hiddenByVisibilityTable) {
        return true;
    }
    if (batch.modelPartIndex < 0 || batch.modelPartState < 0) {
        return false;
    }
    if (static_cast<size_t>(batch.modelPartIndex) >= fighter.modelVisibilityStates.size()) {
        return batch.modelPartState == 0;
    }
    return fighter.modelVisibilityStates[static_cast<size_t>(batch.modelPartIndex)] == batch.modelPartState;
}

static void drawNativeFighterMesh(const pf::FighterDefinition& def, const pf::FighterRuntime& fighter) {
    const pf::FighterMesh& mesh = pf::authoredFighterMesh(def);
    if (mesh.batches.empty() || fighter.jointWorldTransforms.empty()) {
        return;
    }

    FighterMeshRenderCache& cache = fighterMeshRenderCache(mesh);
    constexpr int kMaxBones = kHsdShaderMaxBones;
    std::vector<Matrix> boneWorldMatrices(kMaxBones, MatrixIdentity());
    std::vector<Matrix> boneMatrices(kMaxBones, MatrixIdentity());
    const size_t boneCount = std::min({fighter.jointWorldTransforms.size(),
                                       mesh.inverseBindMatrices.size(),
                                       static_cast<size_t>(kMaxBones)});
    for (size_t i = 0; i < boneCount; ++i) {
        std::array<float, 16> world = toFloatMatrix(fighter.jointWorldTransforms[i]);
        boneWorldMatrices[i] = toRayMatrix(world);
        boneMatrices[i] = toRayMatrix(multiplyRowMajor(world, mesh.inverseBindMatrices[i]));
    }

    Matrix mvp = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    rlEnableShader(cache.shader.id);
    if (cache.locMvp >= 0) {
        SetShaderValueMatrix(cache.shader, cache.locMvp, mvp);
    }
    uploadBoneUniformBuffer(cache.boneUniformBuffer, boneMatrices, boneWorldMatrices);

    for (const FighterMeshRenderBatch& batch : cache.batches) {
        if (!isBatchVisible(batch, fighter)) {
            continue;
        }

        Matrix parent = parentMatrixForBatch(batch, boneWorldMatrices);
        const int hasEnvelopes = batch.hasEnvelopes ? 1 : 0;
        const int unknown2 = batch.unknown2 ? 1 : 0;
        const int shapeSetAverage = batch.shapeSetAverage ? 1 : 0;
        const int parentIsSkeletonRoot = (batch.parentFlags & kJObjSkeletonRoot) != 0 ? 1 : 0;
        const int hasTexture = batch.texture >= 0 && static_cast<size_t>(batch.texture) < cache.textures.size() ? 1 : 0;
        const float materialColor[4] = {
            batch.materialColor[0] / 255.0f,
            batch.materialColor[1] / 255.0f,
            batch.materialColor[2] / 255.0f,
            batch.materialColor[3] / 255.0f,
        };

        if (cache.locParentMatrix >= 0) SetShaderValueMatrix(cache.shader, cache.locParentMatrix, parent);
        if (cache.locHasEnvelopes >= 0) SetShaderValue(cache.shader, cache.locHasEnvelopes, &hasEnvelopes, SHADER_UNIFORM_INT);
        if (cache.locUnknown2 >= 0) SetShaderValue(cache.shader, cache.locUnknown2, &unknown2, SHADER_UNIFORM_INT);
        if (cache.locShapeSetAverage >= 0) SetShaderValue(cache.shader, cache.locShapeSetAverage, &shapeSetAverage, SHADER_UNIFORM_INT);
        if (cache.locParentIsSkeletonRoot >= 0) SetShaderValue(cache.shader, cache.locParentIsSkeletonRoot, &parentIsSkeletonRoot, SHADER_UNIFORM_INT);
        if (cache.locHasTexture >= 0) SetShaderValue(cache.shader, cache.locHasTexture, &hasTexture, SHADER_UNIFORM_INT);
        if (cache.locTextureColorOperation >= 0) SetShaderValue(cache.shader, cache.locTextureColorOperation, &batch.textureColorOperation, SHADER_UNIFORM_INT);
        if (cache.locTextureAlphaOperation >= 0) SetShaderValue(cache.shader, cache.locTextureAlphaOperation, &batch.textureAlphaOperation, SHADER_UNIFORM_INT);
        if (cache.locTextureBlend >= 0) SetShaderValue(cache.shader, cache.locTextureBlend, &batch.textureBlend, SHADER_UNIFORM_FLOAT);
        if (cache.locMaterialColor >= 0) SetShaderValue(cache.shader, cache.locMaterialColor, materialColor, SHADER_UNIFORM_VEC4);
        if (hasTexture != 0 && cache.locTexture0 >= 0) {
            const int textureUnit = 0;
            rlActiveTextureSlot(0);
            rlEnableTexture(cache.textures[static_cast<size_t>(batch.texture)].id);
            SetShaderValue(cache.shader, cache.locTexture0, &textureUnit, SHADER_UNIFORM_INT);
        } else {
            rlActiveTextureSlot(0);
            rlDisableTexture();
        }

        if ((batch.polygonFlags & kPobjCullFront) != 0) {
            rlEnableBackfaceCulling();
            rlSetCullFace(RL_CULL_FACE_FRONT);
        } else if ((batch.polygonFlags & kPobjCullBack) != 0) {
            rlEnableBackfaceCulling();
            rlSetCullFace(RL_CULL_FACE_BACK);
        } else {
            rlDisableBackfaceCulling();
        }

        if (rlEnableVertexArray(batch.vao)) {
            rlDrawVertexArray(0, batch.vertexCount);
            rlDisableVertexArray();
        }
    }

    rlDisableBackfaceCulling();
    rlDisableTexture();
    rlDisableShader();
}

} // namespace

static bool drawsShield(const pf::FighterState& state) {
    return state.name == "GuardOn" || state.name == "Guard" || state.name == "GuardSetOff" || state.name == "GuardReflect";
}

static const char* subactionTypeName(pf::SubactionType type) {
    switch (type) {
    case pf::SubactionType::SyncTimer: return "SyncTimer";
    case pf::SubactionType::AsyncTimer: return "AsyncTimer";
    case pf::SubactionType::SetLoop: return "SetLoop";
    case pf::SubactionType::ExecuteLoop: return "ExecuteLoop";
    case pf::SubactionType::CreateHitbox: return "CreateHitbox";
    case pf::SubactionType::RemoveHitbox: return "RemoveHitbox";
    case pf::SubactionType::AdjustHitboxDamage: return "AdjustHitboxDamage";
    case pf::SubactionType::AdjustHitboxSize: return "AdjustHitboxSize";
    case pf::SubactionType::SetHitboxInteraction: return "SetHitboxInteraction";
    case pf::SubactionType::CreateThrowHitbox: return "CreateThrowHitbox";
    case pf::SubactionType::ClearHitboxes: return "ClearHitboxes";
    case pf::SubactionType::SetHurtboxState: return "SetHurtboxState";
    case pf::SubactionType::SetBodyCollisionState: return "SetBodyCollisionState";
    case pf::SubactionType::SetInterruptible: return "SetInterruptible";
    case pf::SubactionType::SetFlag: return "SetFlag";
    case pf::SubactionType::SetThrowFlag: return "SetThrowFlag";
    case pf::SubactionType::SetThrowFlagLiteral: return "SetThrowFlagLiteral";
    case pf::SubactionType::EnableJabFollowup: return "EnableJabFollowup";
    case pf::SubactionType::SetJabRapid: return "SetJabRapid";
    case pf::SubactionType::SetJumpState: return "SetJumpState";
    case pf::SubactionType::ReverseDirection: return "ReverseDirection";
    case pf::SubactionType::StartSmashCharge: return "StartSmashCharge";
    case pf::SubactionType::SetModelVisibility: return "SetModelVisibility";
    case pf::SubactionType::RevertModelVisibility: return "RevertModelVisibility";
    case pf::SubactionType::RemoveModelVisibility: return "RemoveModelVisibility";
    case pf::SubactionType::SetFighterVisibility: return "SetFighterVisibility";
    case pf::SubactionType::SetModelPartAnimation: return "SetModelPartAnimation";
    case pf::SubactionType::SelfDamage: return "SelfDamage";
    case pf::SubactionType::SpawnObject: return "SpawnObject";
    case pf::SubactionType::SpawnProjectile: return "SpawnProjectile";
    case pf::SubactionType::CallScript: return "CallScript";
    }
    return "Unknown";
}

static Color subactionMarkerColor(const pf::Subaction& subaction) {
    switch (subaction.type) {
    case pf::SubactionType::CreateHitbox:
    case pf::SubactionType::CreateThrowHitbox:
        return RED;
    case pf::SubactionType::ClearHitboxes:
    case pf::SubactionType::RemoveHitbox:
        return MAROON;
    case pf::SubactionType::SetInterruptible:
        return GREEN;
    case pf::SubactionType::SpawnObject:
    case pf::SubactionType::SpawnProjectile:
    case pf::SubactionType::CallScript:
        return PURPLE;
    case pf::SubactionType::SetHurtboxState:
    case pf::SubactionType::SetBodyCollisionState:
        return BLUE;
    default:
        return ORANGE;
    }
}

static const char* hurtboxStateName(pf::HurtboxState state) {
    switch (state) {
    case pf::HurtboxState::Normal: return "Normal";
    case pf::HurtboxState::Invincible: return "Invincible";
    case pf::HurtboxState::Intangible: return "Intangible";
    }
    return "?";
}

static pf::HurtboxState nextHurtboxState(pf::HurtboxState state) {
    switch (state) {
    case pf::HurtboxState::Normal: return pf::HurtboxState::Invincible;
    case pf::HurtboxState::Invincible: return pf::HurtboxState::Intangible;
    case pf::HurtboxState::Intangible: return pf::HurtboxState::Normal;
    }
    return pf::HurtboxState::Normal;
}

static pf::BoneId nextEditorBone(pf::BoneId bone) {
    const int next = (static_cast<int>(bone) + 1) % pf::kBoneCount;
    return static_cast<pf::BoneId>(next);
}

struct EditorToolActionRects {
    Rectangle newState{};
    Rectangle cloneState{};
    Rectangle deleteState{};
    Rectangle test{};
    Rectangle play{};
    Rectangle boxes{};
    Rectangle side{};
};

static EditorToolActionRects editorToolActionRects() {
    const float y = 48.0f;
    const float gap = 8.0f;
    const std::array<float, 7> widths{74.0f, 74.0f, 74.0f, 74.0f, 76.0f, 76.0f, 76.0f};
    float totalWidth = 0.0f;
    for (float width : widths) {
        totalWidth += width;
    }
    totalWidth += gap * static_cast<float>(widths.size() - 1);
    float x = std::max(8.0f, static_cast<float>(GetScreenWidth()) - totalWidth - 12.0f);

    EditorToolActionRects rects;
    rects.newState = {x, y, widths[0], 26.0f};
    x += widths[0] + gap;
    rects.cloneState = {x, y, widths[1], 26.0f};
    x += widths[1] + gap;
    rects.deleteState = {x, y, widths[2], 26.0f};
    x += widths[2] + gap;
    rects.test = {x, y, widths[3], 26.0f};
    x += widths[3] + gap;
    rects.play = {x, y, widths[4], 26.0f};
    x += widths[4] + gap;
    rects.boxes = {x, y, widths[5], 26.0f};
    x += widths[5] + gap;
    rects.side = {x, y, widths[6], 26.0f};
    return rects;
}

static Rectangle editorTestButtonRect() {
    return editorToolActionRects().test;
}

static Rectangle editorNewStateButtonRect() {
    return editorToolActionRects().newState;
}

static Rectangle editorDeleteStateButtonRect() {
    return editorToolActionRects().deleteState;
}

static void duplicateEditorState(pf::World& world, pf::FighterEditor& editor);

static Rectangle mainMenuButtonRect(int index) {
    return {
        static_cast<float>(GetScreenWidth() / 2 - 150),
        static_cast<float>(260 + index * 54),
        300.0f,
        40.0f,
    };
}

static Rectangle topNavButtonRect(int index) {
    return {
        static_cast<float>(GetScreenWidth() - 330 + index * 104),
        12.0f,
        96.0f,
        28.0f,
    };
}

static Rectangle editorWorkspaceTabRect(int index) {
    return {
        12.0f + static_cast<float>(index) * 106.0f,
        286.0f,
        98.0f,
        28.0f,
    };
}

static bool uiButton(Rectangle rect, const std::string& label, bool active = false) {
    const Vector2 mouse = GetMousePosition();
    const bool hovered = CheckCollisionPointRec(mouse, rect);
    DrawRectangleRec(rect, active ? GREEN : (hovered ? Fade(ORANGE, 0.85f) : Fade(RAYWHITE, 0.82f)));
    DrawRectangleLinesEx(rect, 1.0f, DARKGRAY);
    const int textWidth = MeasureText(label.c_str(), 14);
    DrawText(label.c_str(), static_cast<int>(rect.x + (rect.width - static_cast<float>(textWidth)) * 0.5f), static_cast<int>(rect.y + 7.0f), 14, BLACK);
    return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static bool packageNameCharAllowed(int codepoint) {
    return std::isalnum(static_cast<unsigned char>(codepoint)) || codepoint == '_' || codepoint == '-' || codepoint == '.';
}

static bool uiTextField(
    Rectangle rect,
    const std::string& id,
    pf::FighterEditor& editor,
    const std::string& value,
    std::string& committed,
    size_t maxLength = 31)
{
    const Vector2 mouse = GetMousePosition();
    const bool hovered = CheckCollisionPointRec(mouse, rect);
    if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && editor.activeTextField != id) {
        editor.activeTextField = id;
        editor.textEditBuffer = value;
    }

    const bool active = editor.activeTextField == id;
    bool commit = false;
    if (active) {
        for (int codepoint = GetCharPressed(); codepoint > 0; codepoint = GetCharPressed()) {
            if (packageNameCharAllowed(codepoint) && editor.textEditBuffer.size() < maxLength) {
                editor.textEditBuffer.push_back(static_cast<char>(codepoint));
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !editor.textEditBuffer.empty()) {
            editor.textEditBuffer.pop_back();
        }
        commit = IsKeyPressed(KEY_ENTER) ||
            (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !hovered);
    }

    const std::string& shown = active ? editor.textEditBuffer : value;
    DrawRectangleRec(rect, active ? Fade(SKYBLUE, 0.55f) : (hovered ? Fade(RAYWHITE, 0.92f) : Fade(RAYWHITE, 0.72f)));
    DrawRectangleLinesEx(rect, 1.0f, active ? BLUE : DARKGRAY);
    std::string clipped = shown.empty() ? std::string{"name"} : shown;
    while (!clipped.empty() && MeasureText(clipped.c_str(), 12) > static_cast<int>(rect.width - 8.0f)) {
        clipped.pop_back();
    }
    DrawText(clipped.c_str(), static_cast<int>(rect.x + 4.0f), static_cast<int>(rect.y + 6.0f), 12, shown.empty() ? GRAY : BLACK);

    if (commit) {
        committed = editor.textEditBuffer;
        editor.activeTextField.clear();
        editor.textEditBuffer.clear();
        return true;
    }
    return false;
}

static const char* workspaceName(pf::EditorWorkspace workspace) {
    switch (workspace) {
    case pf::EditorWorkspace::Moveset: return "Moveset";
    case pf::EditorWorkspace::Logic: return "Logic";
    case pf::EditorWorkspace::Assets: return "Assets";
    case pf::EditorWorkspace::Animation: return "Animation";
    case pf::EditorWorkspace::TestLab: return "Test Lab";
    }
    return "Workspace";
}

static const char* packageScriptOpName(pf::PackageScriptOp op) {
    switch (op) {
    case pf::PackageScriptOp::Nop: return "Nop";
    case pf::PackageScriptOp::SetVarImmediate: return "SetVar";
    case pf::PackageScriptOp::SetVarFromVar: return "CopyVar";
    case pf::PackageScriptOp::AddVarImmediate: return "AddVar";
    case pf::PackageScriptOp::AddVar: return "AddVars";
    case pf::PackageScriptOp::ScaleVarFixed: return "ScaleVar";
    case pf::PackageScriptOp::SetVarRandom: return "Rand";
    case pf::PackageScriptOp::SetVarFrame: return "ReadFrame";
    case pf::PackageScriptOp::SetVarStateFrame: return "StateFrm";
    case pf::PackageScriptOp::SetVarStateIndex: return "StateIdx";
    case pf::PackageScriptOp::SetVarGrounded: return "ReadGround";
    case pf::PackageScriptOp::SetVarFacing: return "ReadFace";
    case pf::PackageScriptOp::SetVarFighterIndex: return "FighterId";
    case pf::PackageScriptOp::SetVarObjectIndex: return "ObjectId";
    case pf::PackageScriptOp::SetVarFighterStateFrame: return "FStateFrm";
    case pf::PackageScriptOp::SetVarFighterStateIndex: return "FStateIdx";
    case pf::PackageScriptOp::SetVarFighterGrounded: return "FGround";
    case pf::PackageScriptOp::SetVarFighterFacing: return "FFace";
    case pf::PackageScriptOp::SetVarFighterJumpsUsed: return "FJumps";
    case pf::PackageScriptOp::SetVarFighterJumpsRemaining: return "FJumpRem";
    case pf::PackageScriptOp::SetFighterJumpsUsed: return "SetJumps";
    case pf::PackageScriptOp::SetFighterJumpsUsedFromVar: return "JumpVar";
    case pf::PackageScriptOp::SetVarFighterCommandVar: return "CmdRead";
    case pf::PackageScriptOp::SetFighterCommandVarImmediate: return "CmdSet";
    case pf::PackageScriptOp::SetFighterCommandVarFromVar: return "CmdVar";
    case pf::PackageScriptOp::SetVarFighterThrowFlag: return "ThrRead";
    case pf::PackageScriptOp::SetFighterThrowFlagImmediate: return "ThrSet";
    case pf::PackageScriptOp::SetFighterThrowFlagFromVar: return "ThrVar";
    case pf::PackageScriptOp::SetVarFighterHeldObject: return "HeldObj";
    case pf::PackageScriptOp::SetVarFighterGrabbedFighter: return "Grabbed";
    case pf::PackageScriptOp::SetVarFighterGrabberFighter: return "Grabber";
    case pf::PackageScriptOp::SetVarFighterHitlag: return "Hitlag";
    case pf::PackageScriptOp::SetVarFighterHitstun: return "Hitstun";
    case pf::PackageScriptOp::SetVarFighterDamageHitboxOwner: return "DmgOwn";
    case pf::PackageScriptOp::SetVarFighterThrownHitboxOwner: return "ThrOwn";
    case pf::PackageScriptOp::SetVarFighterPercent: return "Pct";
    case pf::PackageScriptOp::SetVarFighterShield: return "ShieldHp";
    case pf::PackageScriptOp::SetVarFighterPositionX: return "PosX";
    case pf::PackageScriptOp::SetVarFighterPositionY: return "PosY";
    case pf::PackageScriptOp::SetVarFighterGroundVelocity: return "GVel";
    case pf::PackageScriptOp::SetVarFighterAirVelocityX: return "AirVX";
    case pf::PackageScriptOp::SetVarFighterAirVelocityY: return "AirVY";
    case pf::PackageScriptOp::SetVarFighterAnimationFrame: return "AnimF";
    case pf::PackageScriptOp::SetVarFighterAnimationRate: return "AnimR";
    case pf::PackageScriptOp::SetVarObjectOwner: return "ObjOwner";
    case pf::PackageScriptOp::SetVarObjectHeldBy: return "ObjHeld";
    case pf::PackageScriptOp::SetVarObjectGrabVictim: return "ObjGrab";
    case pf::PackageScriptOp::SetVarObjectLastFighter: return "ObjLastF";
    case pf::PackageScriptOp::SetVarObjectLastObject: return "ObjLastO";
    case pf::PackageScriptOp::SetVarObjectDamage: return "ObjDmg";
    case pf::PackageScriptOp::SetObjectDamage: return "ObjDmgSet";
    case pf::PackageScriptOp::SetObjectDamageFromVar: return "ObjDmgVar";
    case pf::PackageScriptOp::SetVarObjectHitlag: return "ObjHitlg";
    case pf::PackageScriptOp::SetObjectHitlag: return "ObjHitSet";
    case pf::PackageScriptOp::SetObjectHitlagFromVar: return "ObjHitVar";
    case pf::PackageScriptOp::SetVarObjectGroundSegment: return "ObjFloor";
    case pf::PackageScriptOp::SetVarObjectPositionX: return "ObjPosX";
    case pf::PackageScriptOp::SetVarObjectPositionY: return "ObjPosY";
    case pf::PackageScriptOp::SetVarObjectVelocityX: return "ObjVelX";
    case pf::PackageScriptOp::SetVarObjectVelocityY: return "ObjVelY";
    case pf::PackageScriptOp::SetVarObjectAnimationFrame: return "ObjAnimF";
    case pf::PackageScriptOp::SetVarObjectAnimationRate: return "ObjAnimR";
    case pf::PackageScriptOp::SetObjectOwner: return "ObjOwnSet";
    case pf::PackageScriptOp::SetObjectOwnerFromVar: return "ObjOwnVar";
    case pf::PackageScriptOp::SetVarOwnedObjectCount: return "OwnCnt";
    case pf::PackageScriptOp::SetVarOwnerFighterVar: return "OwnRead";
    case pf::PackageScriptOp::SetOwnerFighterVarImmediate: return "OwnSet";
    case pf::PackageScriptOp::SetOwnerFighterVarFromVar: return "OwnVar";
    case pf::PackageScriptOp::CallOwnerFighterScript: return "OwnCall";
    case pf::PackageScriptOp::SetVarButtonDown: return "BtnDown";
    case pf::PackageScriptOp::SetVarButtonPressed: return "BtnPress";
    case pf::PackageScriptOp::SetVarStickX: return "StickX";
    case pf::PackageScriptOp::SetVarStickY: return "StickY";
    case pf::PackageScriptOp::SetVarCStickX: return "CStickX";
    case pf::PackageScriptOp::SetVarCStickY: return "CStickY";
    case pf::PackageScriptOp::SetVarShield: return "ShieldA";
    case pf::PackageScriptOp::SetGroundVelocity: return "GroundVel";
    case pf::PackageScriptOp::SetAirVelocityX: return "AirVelX";
    case pf::PackageScriptOp::SetAirVelocityY: return "AirVelY";
    case pf::PackageScriptOp::SetPositionX: return "SetX";
    case pf::PackageScriptOp::SetPositionY: return "SetY";
    case pf::PackageScriptOp::SetAnimationRate: return "AnimRate";
    case pf::PackageScriptOp::SetAnimationFrame: return "AnimFrm";
    case pf::PackageScriptOp::SetFacing: return "Facing";
    case pf::PackageScriptOp::SetGroundVelocityFromVar: return "GVelVar";
    case pf::PackageScriptOp::SetAirVelocityXFromVar: return "AirXVar";
    case pf::PackageScriptOp::SetAirVelocityYFromVar: return "AirYVar";
    case pf::PackageScriptOp::SetPositionXFromVar: return "SetXVar";
    case pf::PackageScriptOp::SetPositionYFromVar: return "SetYVar";
    case pf::PackageScriptOp::SetAnimationRateFromVar: return "AnimVar";
    case pf::PackageScriptOp::SetAnimationFrameFromVar: return "FrmVar";
    case pf::PackageScriptOp::SetFacingFromVar: return "FaceVar";
    case pf::PackageScriptOp::ChangeState: return "State";
    case pf::PackageScriptOp::SpawnObject: return "Spawn";
    case pf::PackageScriptOp::SpawnObjectFromVars: return "SpawnV";
    case pf::PackageScriptOp::SpawnProjectile: return "Proj";
    case pf::PackageScriptOp::SpawnProjectileFromVars: return "ProjV";
    case pf::PackageScriptOp::SpawnObjectSetVar: return "SpawnSV";
    case pf::PackageScriptOp::SpawnProjectileSetVar: return "ProjSV";
    case pf::PackageScriptOp::SpawnObjectFromVarsSetVar: return "SpawnVSV";
    case pf::PackageScriptOp::SpawnProjectileFromVarsSetVar: return "ProjVSV";
    case pf::PackageScriptOp::DestroyObject: return "KillObj";
    case pf::PackageScriptOp::DestroyObjectFromVar: return "KillVar";
    case pf::PackageScriptOp::SetVarPickUpObjectFromVar: return "PickVar";
    case pf::PackageScriptOp::SetVarDropObjectFromVar: return "DropVar";
    case pf::PackageScriptOp::SetVarThrowObjectFromVar: return "ThrowVar";
    case pf::PackageScriptOp::SetVarReflectObjectFromVar: return "ReflVar";
    case pf::PackageScriptOp::SetVarAbsorbObjectFromVar: return "AbsVar";
    case pf::PackageScriptOp::SetVarShieldBounceObjectFromVar: return "BounceV";
    case pf::PackageScriptOp::SetVarInteractObjectFromVar: return "TouchV";
    case pf::PackageScriptOp::SetVarInteractObjectsFromVars: return "TouchOO";
    case pf::PackageScriptOp::DestroyOwnedObjects: return "KillOwn";
    case pf::PackageScriptOp::SkipIfVarLessThanImmediate: return "IfVarLt";
    case pf::PackageScriptOp::SkipIfVarLessThanVar: return "IfVarVar";
    case pf::PackageScriptOp::SkipIfVarEqualImmediate: return "IfVarEq";
    case pf::PackageScriptOp::SkipIfVarEqualVar: return "IfEqVar";
    case pf::PackageScriptOp::JumpRelative: return "Jump";
    case pf::PackageScriptOp::CallScript: return "Call";
    case pf::PackageScriptOp::SwitchFighterDefinition: return "Fighter";
    case pf::PackageScriptOp::SpawnFighter: return "SpawnF";
    case pf::PackageScriptOp::SpawnFighterSetVar: return "SpawnFV";
    case pf::PackageScriptOp::CallIndexedFighterScriptFromVar: return "IdxFCall";
    case pf::PackageScriptOp::SetVarIndexedFighterStateIndex: return "IdxFSt";
    case pf::PackageScriptOp::SetVarIndexedFighterPositionX: return "IdxFX";
    case pf::PackageScriptOp::SetVarIndexedFighterPositionY: return "IdxFY";
    case pf::PackageScriptOp::SetIndexedFighterStateFromVar: return "IdxFState";
    case pf::PackageScriptOp::SetIndexedFighterPositionFromVars: return "IdxFPos";
    case pf::PackageScriptOp::SetIndexedFighterFacingFromVar: return "IdxFFace";
    case pf::PackageScriptOp::SetVarIndexedFighterVar: return "IdxFRead";
    case pf::PackageScriptOp::SetIndexedFighterVarImmediate: return "IdxFSet";
    case pf::PackageScriptOp::SetIndexedFighterVarFromVar: return "IdxFVar";
    case pf::PackageScriptOp::SetVarIndexedObjectVar: return "IdxORead";
    case pf::PackageScriptOp::SetIndexedObjectVarImmediate: return "IdxOSet";
    case pf::PackageScriptOp::SetIndexedObjectVarFromVar: return "IdxOVar";
    case pf::PackageScriptOp::CallIndexedObjectScriptFromVar: return "IdxOCall";
    case pf::PackageScriptOp::SetVarLessThanImmediate: return "LtImm";
    case pf::PackageScriptOp::SetVarLessThanVar: return "LtVar";
    case pf::PackageScriptOp::SetVarEqualImmediate: return "EqImm";
    case pf::PackageScriptOp::SetVarEqualVar: return "EqVar";
    case pf::PackageScriptOp::SetVarNotEqualImmediate: return "NeImm";
    case pf::PackageScriptOp::SetVarNotEqualVar: return "NeVar";
    case pf::PackageScriptOp::SetVarGreaterThanImmediate: return "GtImm";
    case pf::PackageScriptOp::SetVarGreaterThanVar: return "GtVar";
    case pf::PackageScriptOp::SetVarNot: return "Not";
    case pf::PackageScriptOp::SetVarAnd: return "And";
    case pf::PackageScriptOp::SetVarOr: return "Or";
    }
    return "Op";
}

static const char* packageInputButtonName(int mask) {
    switch (mask) {
    case pf::ButtonJump: return "Jump";
    case pf::ButtonSpecial: return "Special";
    case pf::ButtonAttack: return "Attack";
    case pf::ButtonShield: return "Shield";
    case pf::ButtonGrab: return "Grab";
    case pf::ButtonTaunt: return "Taunt";
    case pf::ButtonPause: return "Pause";
    }
    return "Button";
}

static int nextPackageInputButton(int mask) {
    switch (mask) {
    case pf::ButtonJump: return pf::ButtonSpecial;
    case pf::ButtonSpecial: return pf::ButtonAttack;
    case pf::ButtonAttack: return pf::ButtonShield;
    case pf::ButtonShield: return pf::ButtonGrab;
    case pf::ButtonGrab: return pf::ButtonTaunt;
    case pf::ButtonTaunt: return pf::ButtonPause;
    default: return pf::ButtonJump;
    }
}

static std::string packageFighterTargetName(const pf::World& world, int fighterDef) {
    if (world.fighterDefs.empty()) {
        return {};
    }
    const int index = std::clamp(fighterDef, 0, static_cast<int>(world.fighterDefs.size()) - 1);
    return world.fighterDefs[static_cast<size_t>(index)].name;
}

static std::string packageObjectTargetName(const pf::World& world, int selectedObjectDef, pf::GameObjectKind kind) {
    if (world.objectDefs.empty()) {
        return {};
    }
    const int selected = std::clamp(selectedObjectDef, 0, static_cast<int>(world.objectDefs.size()) - 1);
    if (world.objectDefs[static_cast<size_t>(selected)].kind == kind) {
        return world.objectDefs[static_cast<size_t>(selected)].name;
    }
    const auto found = std::find_if(world.objectDefs.begin(), world.objectDefs.end(), [&](const pf::GameObjectDefinition& object) {
        return object.kind == kind;
    });
    return found == world.objectDefs.end() ? std::string{} : found->name;
}

static bool packageObjectHasKind(const pf::World& world, const std::string& objectName, pf::GameObjectKind kind) {
    const auto found = std::find_if(world.objectDefs.begin(), world.objectDefs.end(), [&](const pf::GameObjectDefinition& object) {
        return object.name == objectName;
    });
    return found != world.objectDefs.end() && found->kind == kind;
}

static bool packageObjectExists(const pf::World& world, const std::string& objectName) {
    return std::any_of(world.objectDefs.begin(), world.objectDefs.end(), [&](const pf::GameObjectDefinition& object) {
        return object.name == objectName;
    });
}

static bool packageObjectScriptExists(const pf::World& world, const std::string& scriptName) {
    return std::any_of(world.objectDefs.begin(), world.objectDefs.end(), [&](const pf::GameObjectDefinition& object) {
        return std::any_of(object.packageScripts.begin(), object.packageScripts.end(), [&](const pf::PackageScript& script) {
            return script.name == scriptName;
        });
    });
}

static std::string packageObjectScriptTargetName(const pf::World& world, int selectedObjectDef) {
    if (!world.objectDefs.empty()) {
        const int selected = std::clamp(selectedObjectDef, 0, static_cast<int>(world.objectDefs.size()) - 1);
        const pf::GameObjectDefinition& object = world.objectDefs[static_cast<size_t>(selected)];
        if (!object.packageScripts.empty()) {
            return object.packageScripts.front().name;
        }
    }
    for (const pf::GameObjectDefinition& object : world.objectDefs) {
        if (!object.packageScripts.empty()) {
            return object.packageScripts.front().name;
        }
    }
    return {};
}

static bool packageScriptOpIsSpawn(pf::PackageScriptOp op) {
    return op == pf::PackageScriptOp::SpawnObject ||
        op == pf::PackageScriptOp::SpawnObjectFromVars ||
        op == pf::PackageScriptOp::SpawnProjectile ||
        op == pf::PackageScriptOp::SpawnProjectileFromVars ||
        op == pf::PackageScriptOp::SpawnObjectSetVar ||
        op == pf::PackageScriptOp::SpawnProjectileSetVar ||
        op == pf::PackageScriptOp::SpawnObjectFromVarsSetVar ||
        op == pf::PackageScriptOp::SpawnProjectileFromVarsSetVar;
}

static bool packageScriptOpIsProjectileSpawn(pf::PackageScriptOp op) {
    return op == pf::PackageScriptOp::SpawnProjectile ||
        op == pf::PackageScriptOp::SpawnProjectileFromVars ||
        op == pf::PackageScriptOp::SpawnProjectileSetVar ||
        op == pf::PackageScriptOp::SpawnProjectileFromVarsSetVar;
}

static bool packageScriptOpTargetsAnyObject(pf::PackageScriptOp op) {
    return op == pf::PackageScriptOp::SetVarOwnedObjectCount ||
        op == pf::PackageScriptOp::DestroyOwnedObjects;
}

static void sanitizePackageInstructionForVariableCount(pf::PackageScriptInstruction& instruction, int variableCount) {
    if (variableCount > 0) {
        return;
    }

    switch (instruction.op) {
    case pf::PackageScriptOp::SetGroundVelocityFromVar:
        instruction.op = pf::PackageScriptOp::SetGroundVelocity;
        break;
    case pf::PackageScriptOp::SetAirVelocityXFromVar:
        instruction.op = pf::PackageScriptOp::SetAirVelocityX;
        break;
    case pf::PackageScriptOp::SetAirVelocityYFromVar:
        instruction.op = pf::PackageScriptOp::SetAirVelocityY;
        break;
    case pf::PackageScriptOp::SetPositionXFromVar:
        instruction.op = pf::PackageScriptOp::SetPositionX;
        break;
    case pf::PackageScriptOp::SetPositionYFromVar:
        instruction.op = pf::PackageScriptOp::SetPositionY;
        break;
    case pf::PackageScriptOp::SetAnimationRateFromVar:
        instruction.op = pf::PackageScriptOp::SetAnimationRate;
        break;
    case pf::PackageScriptOp::SetAnimationFrameFromVar:
        instruction.op = pf::PackageScriptOp::SetAnimationFrame;
        break;
    case pf::PackageScriptOp::SetFacingFromVar:
        instruction.op = pf::PackageScriptOp::SetFacing;
        break;
    case pf::PackageScriptOp::SetFighterJumpsUsedFromVar:
        instruction.op = pf::PackageScriptOp::SetFighterJumpsUsed;
        break;
    case pf::PackageScriptOp::SetFighterCommandVarFromVar:
        instruction.op = pf::PackageScriptOp::SetFighterCommandVarImmediate;
        break;
    case pf::PackageScriptOp::SetFighterThrowFlagFromVar:
        instruction.op = pf::PackageScriptOp::SetFighterThrowFlagImmediate;
        break;
    case pf::PackageScriptOp::SpawnFighterSetVar:
        instruction.op = pf::PackageScriptOp::SpawnFighter;
        break;
    case pf::PackageScriptOp::SetObjectDamageFromVar:
        instruction.op = pf::PackageScriptOp::SetObjectDamage;
        break;
    case pf::PackageScriptOp::SetObjectHitlagFromVar:
        instruction.op = pf::PackageScriptOp::SetObjectHitlag;
        break;
    case pf::PackageScriptOp::SetObjectOwnerFromVar:
        instruction.op = pf::PackageScriptOp::SetObjectOwner;
        break;
    case pf::PackageScriptOp::SpawnObjectFromVars:
        instruction.op = pf::PackageScriptOp::SpawnObject;
        break;
    case pf::PackageScriptOp::SpawnProjectileFromVars:
        instruction.op = pf::PackageScriptOp::SpawnProjectile;
        break;
    case pf::PackageScriptOp::SpawnObjectSetVar:
        instruction.op = pf::PackageScriptOp::SpawnObject;
        break;
    case pf::PackageScriptOp::SpawnProjectileSetVar:
        instruction.op = pf::PackageScriptOp::SpawnProjectile;
        break;
    case pf::PackageScriptOp::SpawnObjectFromVarsSetVar:
        instruction.op = pf::PackageScriptOp::SpawnObject;
        break;
    case pf::PackageScriptOp::SpawnProjectileFromVarsSetVar:
        instruction.op = pf::PackageScriptOp::SpawnProjectile;
        break;
    case pf::PackageScriptOp::DestroyObjectFromVar:
    case pf::PackageScriptOp::SetVarPickUpObjectFromVar:
    case pf::PackageScriptOp::SetVarDropObjectFromVar:
    case pf::PackageScriptOp::SetVarThrowObjectFromVar:
    case pf::PackageScriptOp::SetVarReflectObjectFromVar:
    case pf::PackageScriptOp::SetVarAbsorbObjectFromVar:
    case pf::PackageScriptOp::SetVarShieldBounceObjectFromVar:
    case pf::PackageScriptOp::SetVarInteractObjectFromVar:
    case pf::PackageScriptOp::SetVarInteractObjectsFromVars:
    case pf::PackageScriptOp::CallIndexedObjectScriptFromVar:
        instruction.op = pf::PackageScriptOp::Nop;
        break;
    case pf::PackageScriptOp::SetVarImmediate:
    case pf::PackageScriptOp::SetVarFromVar:
    case pf::PackageScriptOp::AddVarImmediate:
    case pf::PackageScriptOp::AddVar:
    case pf::PackageScriptOp::ScaleVarFixed:
    case pf::PackageScriptOp::SetVarRandom:
    case pf::PackageScriptOp::SetVarLessThanImmediate:
    case pf::PackageScriptOp::SetVarLessThanVar:
    case pf::PackageScriptOp::SetVarEqualImmediate:
    case pf::PackageScriptOp::SetVarEqualVar:
    case pf::PackageScriptOp::SetVarNotEqualImmediate:
    case pf::PackageScriptOp::SetVarNotEqualVar:
    case pf::PackageScriptOp::SetVarGreaterThanImmediate:
    case pf::PackageScriptOp::SetVarGreaterThanVar:
    case pf::PackageScriptOp::SetVarNot:
    case pf::PackageScriptOp::SetVarAnd:
    case pf::PackageScriptOp::SetVarOr:
    case pf::PackageScriptOp::SetVarFrame:
    case pf::PackageScriptOp::SetVarStateFrame:
    case pf::PackageScriptOp::SetVarStateIndex:
    case pf::PackageScriptOp::SetVarGrounded:
    case pf::PackageScriptOp::SetVarFacing:
    case pf::PackageScriptOp::SetVarFighterIndex:
    case pf::PackageScriptOp::SetVarObjectIndex:
    case pf::PackageScriptOp::SetVarFighterStateFrame:
    case pf::PackageScriptOp::SetVarFighterStateIndex:
    case pf::PackageScriptOp::SetVarFighterGrounded:
    case pf::PackageScriptOp::SetVarFighterFacing:
    case pf::PackageScriptOp::SetVarFighterJumpsUsed:
    case pf::PackageScriptOp::SetVarFighterJumpsRemaining:
    case pf::PackageScriptOp::SetVarFighterHeldObject:
    case pf::PackageScriptOp::SetVarFighterGrabbedFighter:
    case pf::PackageScriptOp::SetVarFighterGrabberFighter:
    case pf::PackageScriptOp::SetVarFighterHitlag:
    case pf::PackageScriptOp::SetVarFighterHitstun:
    case pf::PackageScriptOp::SetVarFighterDamageHitboxOwner:
    case pf::PackageScriptOp::SetVarFighterThrownHitboxOwner:
    case pf::PackageScriptOp::SetVarFighterPercent:
    case pf::PackageScriptOp::SetVarFighterShield:
    case pf::PackageScriptOp::SetVarFighterPositionX:
    case pf::PackageScriptOp::SetVarFighterPositionY:
    case pf::PackageScriptOp::SetVarFighterGroundVelocity:
    case pf::PackageScriptOp::SetVarFighterAirVelocityX:
    case pf::PackageScriptOp::SetVarFighterAirVelocityY:
    case pf::PackageScriptOp::SetVarFighterAnimationFrame:
    case pf::PackageScriptOp::SetVarFighterAnimationRate:
    case pf::PackageScriptOp::SetVarObjectOwner:
    case pf::PackageScriptOp::SetVarObjectHeldBy:
    case pf::PackageScriptOp::SetVarObjectGrabVictim:
    case pf::PackageScriptOp::SetVarObjectLastFighter:
    case pf::PackageScriptOp::SetVarObjectLastObject:
    case pf::PackageScriptOp::SetVarObjectDamage:
    case pf::PackageScriptOp::SetVarObjectHitlag:
    case pf::PackageScriptOp::SetVarObjectGroundSegment:
    case pf::PackageScriptOp::SetVarObjectPositionX:
    case pf::PackageScriptOp::SetVarObjectPositionY:
    case pf::PackageScriptOp::SetVarObjectVelocityX:
    case pf::PackageScriptOp::SetVarObjectVelocityY:
    case pf::PackageScriptOp::SetVarObjectAnimationFrame:
    case pf::PackageScriptOp::SetVarObjectAnimationRate:
    case pf::PackageScriptOp::SetVarOwnedObjectCount:
    case pf::PackageScriptOp::SetVarOwnerFighterVar:
    case pf::PackageScriptOp::SetOwnerFighterVarImmediate:
    case pf::PackageScriptOp::SetOwnerFighterVarFromVar:
    case pf::PackageScriptOp::CallOwnerFighterScript:
    case pf::PackageScriptOp::SetVarIndexedFighterVar:
    case pf::PackageScriptOp::CallIndexedFighterScriptFromVar:
    case pf::PackageScriptOp::SetVarIndexedFighterStateIndex:
    case pf::PackageScriptOp::SetVarIndexedFighterPositionX:
    case pf::PackageScriptOp::SetVarIndexedFighterPositionY:
    case pf::PackageScriptOp::SetIndexedFighterVarImmediate:
    case pf::PackageScriptOp::SetIndexedFighterVarFromVar:
    case pf::PackageScriptOp::SetIndexedFighterStateFromVar:
    case pf::PackageScriptOp::SetIndexedFighterPositionFromVars:
    case pf::PackageScriptOp::SetIndexedFighterFacingFromVar:
    case pf::PackageScriptOp::SetVarIndexedObjectVar:
    case pf::PackageScriptOp::SetIndexedObjectVarImmediate:
    case pf::PackageScriptOp::SetIndexedObjectVarFromVar:
    case pf::PackageScriptOp::SetVarButtonDown:
    case pf::PackageScriptOp::SetVarButtonPressed:
    case pf::PackageScriptOp::SetVarStickX:
    case pf::PackageScriptOp::SetVarStickY:
    case pf::PackageScriptOp::SetVarCStickX:
    case pf::PackageScriptOp::SetVarCStickY:
    case pf::PackageScriptOp::SetVarShield:
    case pf::PackageScriptOp::SkipIfVarLessThanImmediate:
    case pf::PackageScriptOp::SkipIfVarLessThanVar:
    case pf::PackageScriptOp::SkipIfVarEqualImmediate:
    case pf::PackageScriptOp::SkipIfVarEqualVar:
        instruction.op = pf::PackageScriptOp::Nop;
        break;
    default:
        break;
    }
}

static void normalizePackageObjectTargetInstruction(
    pf::PackageScriptInstruction& instruction,
    const pf::World& world,
    int selectedObjectDef)
{
    if (!packageScriptOpTargetsAnyObject(instruction.op)) {
        return;
    }
    if (world.objectDefs.empty()) {
        instruction.op = pf::PackageScriptOp::Nop;
        instruction.text.clear();
        return;
    }
    if (!packageObjectExists(world, instruction.text)) {
        const int objectIndex = std::clamp(selectedObjectDef, 0, static_cast<int>(world.objectDefs.size()) - 1);
        instruction.text = world.objectDefs[static_cast<size_t>(objectIndex)].name;
    }
}

static void normalizePackageSpawnInstructionTarget(
    pf::PackageScriptInstruction& instruction,
    const pf::World& world,
    int selectedObjectDef)
{
    if (!packageScriptOpIsSpawn(instruction.op)) {
        return;
    }
    if (world.objectDefs.empty()) {
        instruction.op = pf::PackageScriptOp::Nop;
        instruction.text.clear();
        return;
    }

    if (packageScriptOpIsProjectileSpawn(instruction.op)) {
        if (packageObjectHasKind(world, instruction.text, pf::GameObjectKind::Projectile)) {
            return;
        }

        const std::string projectileName = packageObjectTargetName(world, selectedObjectDef, pf::GameObjectKind::Projectile);
        if (!projectileName.empty()) {
            instruction.text = projectileName;
            return;
        }

        if (instruction.op == pf::PackageScriptOp::SpawnProjectileFromVars) {
            instruction.op = pf::PackageScriptOp::SpawnObjectFromVars;
        } else if (instruction.op == pf::PackageScriptOp::SpawnProjectileFromVarsSetVar) {
            instruction.op = pf::PackageScriptOp::SpawnObjectFromVarsSetVar;
        } else if (instruction.op == pf::PackageScriptOp::SpawnProjectileSetVar) {
            instruction.op = pf::PackageScriptOp::SpawnObjectSetVar;
        } else {
            instruction.op = pf::PackageScriptOp::SpawnObject;
        }
    }

    if (!packageObjectExists(world, instruction.text)) {
        const int objectIndex = std::clamp(selectedObjectDef, 0, static_cast<int>(world.objectDefs.size()) - 1);
        instruction.text = world.objectDefs[static_cast<size_t>(objectIndex)].name;
    }
}

static std::string nextObjectTargetName(
    const pf::World& world,
    const std::string& currentName,
    std::optional<pf::GameObjectKind> requiredKind)
{
    if (world.objectDefs.empty()) {
        return {};
    }
    int currentIndex = -1;
    for (int i = 0; i < static_cast<int>(world.objectDefs.size()); ++i) {
        if (world.objectDefs[static_cast<size_t>(i)].name == currentName) {
            currentIndex = i;
            break;
        }
    }
    for (int step = 1; step <= static_cast<int>(world.objectDefs.size()); ++step) {
        const int baseIndex = currentIndex < 0 ? -1 : currentIndex;
        const int index = (baseIndex + step) % static_cast<int>(world.objectDefs.size());
        const pf::GameObjectDefinition& object = world.objectDefs[static_cast<size_t>(index)];
        if (!requiredKind || object.kind == *requiredKind) {
            return object.name;
        }
    }
    return {};
}

static void cyclePackageFighterTarget(
    pf::PackageScriptInstruction& instruction,
    pf::PackageScriptOp op,
    const pf::World& world,
    int fallbackFighterDef)
{
    if (world.fighterDefs.empty()) {
        return;
    }
    if (instruction.op != op || instruction.text.empty()) {
        instruction.op = op;
        instruction.text = packageFighterTargetName(world, fallbackFighterDef);
        return;
    }

    auto found = std::find_if(world.fighterDefs.begin(), world.fighterDefs.end(), [&](const pf::FighterDefinition& def) {
        return def.name == instruction.text;
    });
    if (found == world.fighterDefs.end()) {
        instruction.text = packageFighterTargetName(world, fallbackFighterDef);
        return;
    }
    size_t index = static_cast<size_t>(std::distance(world.fighterDefs.begin(), found));
    index = (index + 1) % world.fighterDefs.size();
    instruction.text = world.fighterDefs[index].name;
}

static std::string packageScriptTargetName(const std::vector<pf::PackageScript>& scripts, int selectedScript) {
    if (scripts.empty()) {
        return {};
    }
    const int index = std::clamp(selectedScript, 0, static_cast<int>(scripts.size()) - 1);
    return scripts[static_cast<size_t>(index)].name;
}

static void cyclePackageScriptTarget(
    pf::PackageScriptInstruction& instruction,
    const std::vector<pf::PackageScript>& scripts,
    int fallbackScript)
{
    if (scripts.empty()) {
        instruction.text.clear();
        return;
    }
    if (instruction.op != pf::PackageScriptOp::CallScript || instruction.text.empty()) {
        instruction.op = pf::PackageScriptOp::CallScript;
        instruction.text = packageScriptTargetName(scripts, fallbackScript);
        return;
    }

    auto found = std::find_if(scripts.begin(), scripts.end(), [&](const pf::PackageScript& script) {
        return script.name == instruction.text;
    });
    if (found == scripts.end()) {
        instruction.text = packageScriptTargetName(scripts, fallbackScript);
        return;
    }
    size_t index = static_cast<size_t>(std::distance(scripts.begin(), found));
    index = (index + 1) % scripts.size();
    instruction.text = scripts[index].name;
}

static void cyclePackageScriptSubactionTarget(
    pf::Subaction& subaction,
    const std::vector<pf::PackageScript>& scripts,
    int fallbackScript)
{
    subaction.type = pf::SubactionType::CallScript;
    if (scripts.empty()) {
        subaction.objectName.clear();
        return;
    }
    if (subaction.objectName.empty()) {
        subaction.objectName = packageScriptTargetName(scripts, fallbackScript);
        return;
    }

    auto found = std::find_if(scripts.begin(), scripts.end(), [&](const pf::PackageScript& script) {
        return script.name == subaction.objectName;
    });
    if (found == scripts.end()) {
        subaction.objectName = packageScriptTargetName(scripts, fallbackScript);
        return;
    }
    size_t index = static_cast<size_t>(std::distance(scripts.begin(), found));
    index = (index + 1) % scripts.size();
    subaction.objectName = scripts[index].name;
}

static std::string packageInstructionLabel(const pf::PackageScriptInstruction& instruction) {
    std::string label = packageScriptOpName(instruction.op);
    switch (instruction.op) {
    case pf::PackageScriptOp::SetVarImmediate:
    case pf::PackageScriptOp::AddVarImmediate:
        label += " v" + std::to_string(instruction.dst) + " " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetFighterJumpsUsed:
        label += " " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetVarFromVar:
        label += " v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetVarFrame:
    case pf::PackageScriptOp::SetVarStateFrame:
    case pf::PackageScriptOp::SetVarStateIndex:
    case pf::PackageScriptOp::SetVarGrounded:
    case pf::PackageScriptOp::SetVarFacing:
    case pf::PackageScriptOp::SetVarFighterIndex:
    case pf::PackageScriptOp::SetVarObjectIndex:
    case pf::PackageScriptOp::SetVarFighterStateFrame:
    case pf::PackageScriptOp::SetVarFighterStateIndex:
    case pf::PackageScriptOp::SetVarFighterGrounded:
    case pf::PackageScriptOp::SetVarFighterFacing:
    case pf::PackageScriptOp::SetVarFighterJumpsUsed:
    case pf::PackageScriptOp::SetVarFighterJumpsRemaining:
    case pf::PackageScriptOp::SetVarFighterHeldObject:
    case pf::PackageScriptOp::SetVarFighterGrabbedFighter:
    case pf::PackageScriptOp::SetVarFighterGrabberFighter:
    case pf::PackageScriptOp::SetVarFighterHitlag:
    case pf::PackageScriptOp::SetVarFighterHitstun:
    case pf::PackageScriptOp::SetVarFighterDamageHitboxOwner:
    case pf::PackageScriptOp::SetVarFighterThrownHitboxOwner:
    case pf::PackageScriptOp::SetVarFighterPercent:
    case pf::PackageScriptOp::SetVarFighterShield:
    case pf::PackageScriptOp::SetVarFighterPositionX:
    case pf::PackageScriptOp::SetVarFighterPositionY:
    case pf::PackageScriptOp::SetVarFighterGroundVelocity:
    case pf::PackageScriptOp::SetVarFighterAirVelocityX:
    case pf::PackageScriptOp::SetVarFighterAirVelocityY:
    case pf::PackageScriptOp::SetVarFighterAnimationFrame:
    case pf::PackageScriptOp::SetVarFighterAnimationRate:
    case pf::PackageScriptOp::SetVarStickX:
    case pf::PackageScriptOp::SetVarStickY:
    case pf::PackageScriptOp::SetVarCStickX:
    case pf::PackageScriptOp::SetVarCStickY:
    case pf::PackageScriptOp::SetVarShield:
    case pf::PackageScriptOp::SetVarObjectOwner:
    case pf::PackageScriptOp::SetVarObjectHeldBy:
    case pf::PackageScriptOp::SetVarObjectGrabVictim:
    case pf::PackageScriptOp::SetVarObjectLastFighter:
    case pf::PackageScriptOp::SetVarObjectLastObject:
    case pf::PackageScriptOp::SetVarObjectDamage:
    case pf::PackageScriptOp::SetVarObjectHitlag:
    case pf::PackageScriptOp::SetVarObjectGroundSegment:
    case pf::PackageScriptOp::SetVarObjectPositionX:
    case pf::PackageScriptOp::SetVarObjectPositionY:
    case pf::PackageScriptOp::SetVarObjectVelocityX:
    case pf::PackageScriptOp::SetVarObjectVelocityY:
    case pf::PackageScriptOp::SetVarObjectAnimationFrame:
    case pf::PackageScriptOp::SetVarObjectAnimationRate:
        label += " v" + std::to_string(instruction.dst);
        break;
    case pf::PackageScriptOp::SetVarFighterCommandVar:
        label += " v" + std::to_string(instruction.dst) + " = cmd" + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetFighterCommandVarImmediate:
        label += " cmd" + std::to_string(instruction.dst) + " " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetFighterCommandVarFromVar:
        label += " cmd" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetVarFighterThrowFlag:
        label += " v" + std::to_string(instruction.dst) + " = throw" + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetFighterThrowFlagImmediate:
        label += " throw" + std::to_string(instruction.dst) + " " + std::to_string(instruction.intValue != 0 ? 1 : 0);
        break;
    case pf::PackageScriptOp::SetFighterThrowFlagFromVar:
        label += " throw" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetObjectDamage:
        label += " " + std::to_string(pf::fxToFloat(instruction.fixValue));
        break;
    case pf::PackageScriptOp::SetObjectDamageFromVar:
        label += " = v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetObjectHitlag:
        label += " " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetObjectHitlagFromVar:
        label += " = v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetObjectOwner:
        label += " " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetObjectOwnerFromVar:
        label += " = v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetVarOwnedObjectCount:
        label += " v" + std::to_string(instruction.dst) + " " + instruction.text;
        break;
    case pf::PackageScriptOp::SetVarOwnerFighterVar:
        label += " v" + std::to_string(instruction.dst) + " owner v" + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetOwnerFighterVarImmediate:
        label += " owner v" + std::to_string(instruction.dst) + " " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetOwnerFighterVarFromVar:
        label += " owner v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::CallOwnerFighterScript:
        label += " owner.call " + instruction.text;
        break;
    case pf::PackageScriptOp::SetVarButtonDown:
    case pf::PackageScriptOp::SetVarButtonPressed:
        label += " v" + std::to_string(instruction.dst) + " " + packageInputButtonName(instruction.intValue);
        break;
    case pf::PackageScriptOp::ScaleVarFixed:
        label += " v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA) + " * " + std::to_string(pf::fxToFloat(instruction.fixValue));
        break;
    case pf::PackageScriptOp::SetVarRandom:
        label += " v" + std::to_string(instruction.dst) + " rng < " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetVarLessThanImmediate:
        label += " v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA) + " < " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetVarEqualImmediate:
        label += " v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA) + " == " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetVarNotEqualImmediate:
        label += " v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA) + " != " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetVarGreaterThanImmediate:
        label += " v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA) + " > " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::AddVar:
        label += " v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA) + " + v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::SetVarLessThanVar:
        label += " v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA) + " < v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::SetVarEqualVar:
        label += " v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA) + " == v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::SetVarNotEqualVar:
        label += " v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA) + " != v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::SetVarGreaterThanVar:
        label += " v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA) + " > v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::SetVarNot:
        label += " v" + std::to_string(instruction.dst) + " = !v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetVarAnd:
        label += " v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA) + " && v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::SetVarOr:
        label += " v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA) + " || v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::SetGroundVelocityFromVar:
    case pf::PackageScriptOp::SetAirVelocityXFromVar:
    case pf::PackageScriptOp::SetAirVelocityYFromVar:
    case pf::PackageScriptOp::SetPositionXFromVar:
    case pf::PackageScriptOp::SetPositionYFromVar:
    case pf::PackageScriptOp::SetAnimationRateFromVar:
    case pf::PackageScriptOp::SetAnimationFrameFromVar:
    case pf::PackageScriptOp::SetFacingFromVar:
    case pf::PackageScriptOp::SetFighterJumpsUsedFromVar:
        label += " v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetGroundVelocity:
    case pf::PackageScriptOp::SetAirVelocityX:
    case pf::PackageScriptOp::SetAirVelocityY:
    case pf::PackageScriptOp::SetPositionX:
    case pf::PackageScriptOp::SetPositionY:
    case pf::PackageScriptOp::SetAnimationRate:
    case pf::PackageScriptOp::SetAnimationFrame:
        label += " " + std::to_string(pf::fxToFloat(instruction.fixValue));
        break;
    case pf::PackageScriptOp::SetFacing:
        label += instruction.intValue < 0 ? " left" : " right";
        break;
    case pf::PackageScriptOp::ChangeState:
        label += " " + instruction.text;
        break;
    case pf::PackageScriptOp::SpawnObject:
    case pf::PackageScriptOp::SpawnProjectile:
        label += " " + instruction.text;
        break;
    case pf::PackageScriptOp::SpawnObjectFromVars:
    case pf::PackageScriptOp::SpawnProjectileFromVars:
        label += " " + instruction.text + " v" + std::to_string(instruction.srcA) + "/v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::SpawnObjectSetVar:
    case pf::PackageScriptOp::SpawnProjectileSetVar:
        label += " v" + std::to_string(instruction.dst) + " " + instruction.text;
        break;
    case pf::PackageScriptOp::SpawnObjectFromVarsSetVar:
    case pf::PackageScriptOp::SpawnProjectileFromVarsSetVar:
        label += " v" + std::to_string(instruction.dst) + " " + instruction.text +
            " v" + std::to_string(instruction.srcA) + "/v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::DestroyObject:
        break;
    case pf::PackageScriptOp::DestroyObjectFromVar:
        label += " v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetVarPickUpObjectFromVar:
        label += " v" + std::to_string(instruction.dst) + " pick v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetVarDropObjectFromVar:
        label += " v" + std::to_string(instruction.dst) + " drop v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetVarThrowObjectFromVar:
        label += " v" + std::to_string(instruction.dst) + " throw v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetVarReflectObjectFromVar:
        label += " v" + std::to_string(instruction.dst) + " refl v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetVarAbsorbObjectFromVar:
        label += " v" + std::to_string(instruction.dst) + " absorb v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetVarShieldBounceObjectFromVar:
        label += " v" + std::to_string(instruction.dst) + " bounce v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetVarInteractObjectFromVar:
        label += " v" + std::to_string(instruction.dst) + " touch v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetVarInteractObjectsFromVars:
        label += " v" + std::to_string(instruction.dst) + " obj v" + std::to_string(instruction.srcA) + "/v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::DestroyOwnedObjects:
        label += " " + instruction.text;
        break;
    case pf::PackageScriptOp::SkipIfVarLessThanImmediate:
        label += " v" + std::to_string(instruction.dst) + " < " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SkipIfVarEqualImmediate:
        label += " v" + std::to_string(instruction.dst) + " == " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SkipIfVarLessThanVar:
        label += " v" + std::to_string(instruction.srcA) + " < v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::SkipIfVarEqualVar:
        label += " v" + std::to_string(instruction.srcA) + " == v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::JumpRelative:
        label += " " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::CallScript:
        label += " " + instruction.text;
        break;
    case pf::PackageScriptOp::SwitchFighterDefinition:
        label += " " + instruction.text;
        break;
    case pf::PackageScriptOp::SpawnFighter:
        label += " " + instruction.text;
        break;
    case pf::PackageScriptOp::SpawnFighterSetVar:
        label += " v" + std::to_string(instruction.dst) + " " + instruction.text;
        break;
    case pf::PackageScriptOp::CallIndexedFighterScriptFromVar:
        label += " fighter[v" + std::to_string(instruction.srcA) + "].call " + instruction.text;
        break;
    case pf::PackageScriptOp::SetVarIndexedFighterStateIndex:
        label += " v" + std::to_string(instruction.dst) + " = fighter[v" + std::to_string(instruction.srcA) + "].state";
        break;
    case pf::PackageScriptOp::SetVarIndexedFighterPositionX:
        label += " v" + std::to_string(instruction.dst) + " = fighter[v" + std::to_string(instruction.srcA) + "].x";
        break;
    case pf::PackageScriptOp::SetVarIndexedFighterPositionY:
        label += " v" + std::to_string(instruction.dst) + " = fighter[v" + std::to_string(instruction.srcA) + "].y";
        break;
    case pf::PackageScriptOp::SetIndexedFighterStateFromVar:
        label += " fighter[v" + std::to_string(instruction.dst) + "].state = v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetIndexedFighterPositionFromVars:
        label += " fighter[v" + std::to_string(instruction.dst) + "].pos = v" + std::to_string(instruction.srcA) + "/v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::SetIndexedFighterFacingFromVar:
        label += " fighter[v" + std::to_string(instruction.dst) + "].face = v" + std::to_string(instruction.srcA);
        break;
    case pf::PackageScriptOp::SetVarIndexedFighterVar:
        label += " v" + std::to_string(instruction.dst) + " = fighter[v" + std::to_string(instruction.srcA) + "].v" + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetIndexedFighterVarImmediate:
        label += " fighter[v" + std::to_string(instruction.srcA) + "].v" + std::to_string(instruction.dst) + " " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetIndexedFighterVarFromVar:
        label += " fighter[v" + std::to_string(instruction.srcA) + "].v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::SetVarIndexedObjectVar:
        label += " v" + std::to_string(instruction.dst) + " = object[v" + std::to_string(instruction.srcA) + "].v" + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetIndexedObjectVarImmediate:
        label += " object[v" + std::to_string(instruction.srcA) + "].v" + std::to_string(instruction.dst) + " " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::SetIndexedObjectVarFromVar:
        label += " object[v" + std::to_string(instruction.srcA) + "].v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::CallIndexedObjectScriptFromVar:
        label += " object[v" + std::to_string(instruction.srcA) + "].call " + instruction.text;
        break;
    case pf::PackageScriptOp::Nop:
        break;
    }
    return label;
}

static bool moveEditorPackageInstruction(pf::PackageScript& script, pf::FighterEditor& editor, int delta) {
    if (script.instructions.empty() || delta == 0) {
        return false;
    }
    editor.selectedPackageInstruction = std::clamp(
        editor.selectedPackageInstruction,
        0,
        static_cast<int>(script.instructions.size()) - 1);
    const int target = editor.selectedPackageInstruction + delta;
    if (target < 0 || target >= static_cast<int>(script.instructions.size())) {
        return false;
    }
    std::swap(script.instructions[static_cast<size_t>(editor.selectedPackageInstruction)],
              script.instructions[static_cast<size_t>(target)]);
    editor.selectedPackageInstruction = target;
    editor.status = delta < 0
        ? "Editor: moved selected script block earlier"
        : "Editor: moved selected script block later";
    return true;
}

static const char* groundRequirementName(pf::GroundRequirement ground) {
    switch (ground) {
    case pf::GroundRequirement::Any: return "Any";
    case pf::GroundRequirement::OnlyGrounded: return "Ground";
    case pf::GroundRequirement::OnlyAirborne: return "Air";
    }
    return "?";
}

static pf::GroundRequirement nextGroundRequirement(pf::GroundRequirement ground) {
    switch (ground) {
    case pf::GroundRequirement::Any: return pf::GroundRequirement::OnlyGrounded;
    case pf::GroundRequirement::OnlyGrounded: return pf::GroundRequirement::OnlyAirborne;
    case pf::GroundRequirement::OnlyAirborne: return pf::GroundRequirement::Any;
    }
    return pf::GroundRequirement::Any;
}

static const char* interruptConditionName(pf::InterruptCondition condition) {
    switch (condition) {
    case pf::InterruptCondition::WaitInput: return "Wait";
    case pf::InterruptCondition::BecameAirborne: return "Airborne";
    case pf::InterruptCondition::JumpPressed: return "Jump";
    case pf::InterruptCondition::RunJumpPressed: return "RunJump";
    case pf::InterruptCondition::AttackPressed: return "Attack";
    case pf::InterruptCondition::GrabPressed: return "Grab";
    case pf::InterruptCondition::ShieldHeld: return "Shield";
    case pf::InterruptCondition::SpecialNInput: return "SpecN";
    case pf::InterruptCondition::SpecialSInput: return "SpecS";
    case pf::InterruptCondition::SpecialHiInput: return "SpecHi";
    case pf::InterruptCondition::SpecialLwInput: return "SpecLw";
    case pf::InterruptCondition::DashInput: return "Dash";
    case pf::InterruptCondition::SquatInput: return "Squat";
    case pf::InterruptCondition::TurnInput: return "Turn";
    case pf::InterruptCondition::PackageVarAtLeast: return "PkgVar";
    default: return "Other";
    }
}

static pf::InterruptCondition nextCommonInterruptCondition(pf::InterruptCondition condition) {
    switch (condition) {
    case pf::InterruptCondition::WaitInput: return pf::InterruptCondition::BecameAirborne;
    case pf::InterruptCondition::BecameAirborne: return pf::InterruptCondition::JumpPressed;
    case pf::InterruptCondition::JumpPressed: return pf::InterruptCondition::RunJumpPressed;
    case pf::InterruptCondition::RunJumpPressed: return pf::InterruptCondition::AttackPressed;
    case pf::InterruptCondition::AttackPressed: return pf::InterruptCondition::GrabPressed;
    case pf::InterruptCondition::GrabPressed: return pf::InterruptCondition::ShieldHeld;
    case pf::InterruptCondition::ShieldHeld: return pf::InterruptCondition::SpecialNInput;
    case pf::InterruptCondition::SpecialNInput: return pf::InterruptCondition::SpecialSInput;
    case pf::InterruptCondition::SpecialSInput: return pf::InterruptCondition::SpecialHiInput;
    case pf::InterruptCondition::SpecialHiInput: return pf::InterruptCondition::SpecialLwInput;
    case pf::InterruptCondition::SpecialLwInput: return pf::InterruptCondition::DashInput;
    case pf::InterruptCondition::DashInput: return pf::InterruptCondition::SquatInput;
    case pf::InterruptCondition::SquatInput: return pf::InterruptCondition::TurnInput;
    case pf::InterruptCondition::TurnInput: return pf::InterruptCondition::WaitInput;
    case pf::InterruptCondition::PackageVarAtLeast: return pf::InterruptCondition::WaitInput;
    default: return pf::InterruptCondition::WaitInput;
    }
}

static pf::PackageScriptOp nextPackageScriptOp(pf::PackageScriptOp op) {
    switch (op) {
    case pf::PackageScriptOp::Nop: return pf::PackageScriptOp::SetVarImmediate;
    case pf::PackageScriptOp::SetVarImmediate: return pf::PackageScriptOp::SetVarFromVar;
    case pf::PackageScriptOp::SetVarFromVar: return pf::PackageScriptOp::AddVarImmediate;
    case pf::PackageScriptOp::AddVarImmediate: return pf::PackageScriptOp::AddVar;
    case pf::PackageScriptOp::AddVar: return pf::PackageScriptOp::ScaleVarFixed;
    case pf::PackageScriptOp::ScaleVarFixed: return pf::PackageScriptOp::SetVarRandom;
    case pf::PackageScriptOp::SetVarRandom: return pf::PackageScriptOp::SetVarLessThanImmediate;
    case pf::PackageScriptOp::SetVarLessThanImmediate: return pf::PackageScriptOp::SetVarLessThanVar;
    case pf::PackageScriptOp::SetVarLessThanVar: return pf::PackageScriptOp::SetVarEqualImmediate;
    case pf::PackageScriptOp::SetVarEqualImmediate: return pf::PackageScriptOp::SetVarEqualVar;
    case pf::PackageScriptOp::SetVarEqualVar: return pf::PackageScriptOp::SetVarNotEqualImmediate;
    case pf::PackageScriptOp::SetVarNotEqualImmediate: return pf::PackageScriptOp::SetVarNotEqualVar;
    case pf::PackageScriptOp::SetVarNotEqualVar: return pf::PackageScriptOp::SetVarGreaterThanImmediate;
    case pf::PackageScriptOp::SetVarGreaterThanImmediate: return pf::PackageScriptOp::SetVarGreaterThanVar;
    case pf::PackageScriptOp::SetVarGreaterThanVar: return pf::PackageScriptOp::SetVarNot;
    case pf::PackageScriptOp::SetVarNot: return pf::PackageScriptOp::SetVarAnd;
    case pf::PackageScriptOp::SetVarAnd: return pf::PackageScriptOp::SetVarOr;
    case pf::PackageScriptOp::SetVarOr: return pf::PackageScriptOp::SetVarFrame;
    case pf::PackageScriptOp::SetVarFrame: return pf::PackageScriptOp::SetVarStateFrame;
    case pf::PackageScriptOp::SetVarStateFrame: return pf::PackageScriptOp::SetVarStateIndex;
    case pf::PackageScriptOp::SetVarStateIndex: return pf::PackageScriptOp::SetVarGrounded;
    case pf::PackageScriptOp::SetVarGrounded: return pf::PackageScriptOp::SetVarFacing;
    case pf::PackageScriptOp::SetVarFacing: return pf::PackageScriptOp::SetVarFighterIndex;
    case pf::PackageScriptOp::SetVarFighterIndex: return pf::PackageScriptOp::SetVarFighterStateFrame;
    case pf::PackageScriptOp::SetVarFighterStateFrame: return pf::PackageScriptOp::SetVarFighterStateIndex;
    case pf::PackageScriptOp::SetVarFighterStateIndex: return pf::PackageScriptOp::SetVarFighterGrounded;
    case pf::PackageScriptOp::SetVarFighterGrounded: return pf::PackageScriptOp::SetVarFighterFacing;
    case pf::PackageScriptOp::SetVarFighterFacing: return pf::PackageScriptOp::SetVarFighterJumpsUsed;
    case pf::PackageScriptOp::SetVarFighterJumpsUsed: return pf::PackageScriptOp::SetVarFighterJumpsRemaining;
    case pf::PackageScriptOp::SetVarFighterJumpsRemaining: return pf::PackageScriptOp::SetFighterJumpsUsed;
    case pf::PackageScriptOp::SetFighterJumpsUsed: return pf::PackageScriptOp::SetFighterJumpsUsedFromVar;
    case pf::PackageScriptOp::SetFighterJumpsUsedFromVar: return pf::PackageScriptOp::SetVarFighterCommandVar;
    case pf::PackageScriptOp::SetVarFighterCommandVar: return pf::PackageScriptOp::SetFighterCommandVarImmediate;
    case pf::PackageScriptOp::SetFighterCommandVarImmediate: return pf::PackageScriptOp::SetFighterCommandVarFromVar;
    case pf::PackageScriptOp::SetFighterCommandVarFromVar: return pf::PackageScriptOp::SetVarFighterThrowFlag;
    case pf::PackageScriptOp::SetVarFighterThrowFlag: return pf::PackageScriptOp::SetFighterThrowFlagImmediate;
    case pf::PackageScriptOp::SetFighterThrowFlagImmediate: return pf::PackageScriptOp::SetFighterThrowFlagFromVar;
    case pf::PackageScriptOp::SetFighterThrowFlagFromVar: return pf::PackageScriptOp::SetVarFighterHeldObject;
    case pf::PackageScriptOp::SetVarFighterHeldObject: return pf::PackageScriptOp::SetVarFighterGrabbedFighter;
    case pf::PackageScriptOp::SetVarFighterGrabbedFighter: return pf::PackageScriptOp::SetVarFighterGrabberFighter;
    case pf::PackageScriptOp::SetVarFighterGrabberFighter: return pf::PackageScriptOp::SetVarFighterHitlag;
    case pf::PackageScriptOp::SetVarFighterHitlag: return pf::PackageScriptOp::SetVarFighterHitstun;
    case pf::PackageScriptOp::SetVarFighterHitstun: return pf::PackageScriptOp::SetVarFighterDamageHitboxOwner;
    case pf::PackageScriptOp::SetVarFighterDamageHitboxOwner: return pf::PackageScriptOp::SetVarFighterThrownHitboxOwner;
    case pf::PackageScriptOp::SetVarFighterThrownHitboxOwner: return pf::PackageScriptOp::SetVarFighterPercent;
    case pf::PackageScriptOp::SetVarFighterPercent: return pf::PackageScriptOp::SetVarFighterShield;
    case pf::PackageScriptOp::SetVarFighterShield: return pf::PackageScriptOp::SetVarFighterPositionX;
    case pf::PackageScriptOp::SetVarFighterPositionX: return pf::PackageScriptOp::SetVarFighterPositionY;
    case pf::PackageScriptOp::SetVarFighterPositionY: return pf::PackageScriptOp::SetVarFighterGroundVelocity;
    case pf::PackageScriptOp::SetVarFighterGroundVelocity: return pf::PackageScriptOp::SetVarFighterAirVelocityX;
    case pf::PackageScriptOp::SetVarFighterAirVelocityX: return pf::PackageScriptOp::SetVarFighterAirVelocityY;
    case pf::PackageScriptOp::SetVarFighterAirVelocityY: return pf::PackageScriptOp::SetVarFighterAnimationFrame;
    case pf::PackageScriptOp::SetVarFighterAnimationFrame: return pf::PackageScriptOp::SetVarFighterAnimationRate;
    case pf::PackageScriptOp::SetVarFighterAnimationRate: return pf::PackageScriptOp::SetVarOwnedObjectCount;
    case pf::PackageScriptOp::SetVarOwnedObjectCount: return pf::PackageScriptOp::SetVarButtonDown;
    case pf::PackageScriptOp::SetVarButtonDown: return pf::PackageScriptOp::SetVarButtonPressed;
    case pf::PackageScriptOp::SetVarButtonPressed: return pf::PackageScriptOp::SetVarStickX;
    case pf::PackageScriptOp::SetVarStickX: return pf::PackageScriptOp::SetVarStickY;
    case pf::PackageScriptOp::SetVarStickY: return pf::PackageScriptOp::SetVarCStickX;
    case pf::PackageScriptOp::SetVarCStickX: return pf::PackageScriptOp::SetVarCStickY;
    case pf::PackageScriptOp::SetVarCStickY: return pf::PackageScriptOp::SetVarShield;
    case pf::PackageScriptOp::SetVarShield: return pf::PackageScriptOp::SetGroundVelocity;
    case pf::PackageScriptOp::SetGroundVelocity: return pf::PackageScriptOp::SetAirVelocityX;
    case pf::PackageScriptOp::SetAirVelocityX: return pf::PackageScriptOp::SetAirVelocityY;
    case pf::PackageScriptOp::SetAirVelocityY: return pf::PackageScriptOp::SetPositionX;
    case pf::PackageScriptOp::SetPositionX: return pf::PackageScriptOp::SetPositionY;
    case pf::PackageScriptOp::SetPositionY: return pf::PackageScriptOp::SetAnimationRate;
    case pf::PackageScriptOp::SetAnimationRate: return pf::PackageScriptOp::SetAnimationRateFromVar;
    case pf::PackageScriptOp::SetAnimationRateFromVar: return pf::PackageScriptOp::SetAnimationFrame;
    case pf::PackageScriptOp::SetAnimationFrame: return pf::PackageScriptOp::SetAnimationFrameFromVar;
    case pf::PackageScriptOp::SetAnimationFrameFromVar: return pf::PackageScriptOp::SetFacing;
    case pf::PackageScriptOp::SetFacing: return pf::PackageScriptOp::SetGroundVelocityFromVar;
    case pf::PackageScriptOp::SetGroundVelocityFromVar: return pf::PackageScriptOp::SetAirVelocityXFromVar;
    case pf::PackageScriptOp::SetAirVelocityXFromVar: return pf::PackageScriptOp::SetAirVelocityYFromVar;
    case pf::PackageScriptOp::SetAirVelocityYFromVar: return pf::PackageScriptOp::SetPositionXFromVar;
    case pf::PackageScriptOp::SetPositionXFromVar: return pf::PackageScriptOp::SetPositionYFromVar;
    case pf::PackageScriptOp::SetPositionYFromVar: return pf::PackageScriptOp::SetFacingFromVar;
    case pf::PackageScriptOp::SetFacingFromVar: return pf::PackageScriptOp::ChangeState;
    case pf::PackageScriptOp::ChangeState: return pf::PackageScriptOp::SpawnObject;
    case pf::PackageScriptOp::SpawnObject: return pf::PackageScriptOp::SpawnObjectFromVars;
    case pf::PackageScriptOp::SpawnObjectFromVars: return pf::PackageScriptOp::SpawnProjectile;
    case pf::PackageScriptOp::SpawnProjectile: return pf::PackageScriptOp::SpawnProjectileFromVars;
    case pf::PackageScriptOp::SpawnProjectileFromVars: return pf::PackageScriptOp::SpawnObjectSetVar;
    case pf::PackageScriptOp::SpawnObjectSetVar: return pf::PackageScriptOp::SpawnProjectileSetVar;
    case pf::PackageScriptOp::SpawnProjectileSetVar: return pf::PackageScriptOp::SpawnObjectFromVarsSetVar;
    case pf::PackageScriptOp::SpawnObjectFromVarsSetVar: return pf::PackageScriptOp::SpawnProjectileFromVarsSetVar;
    case pf::PackageScriptOp::SpawnProjectileFromVarsSetVar: return pf::PackageScriptOp::DestroyObjectFromVar;
    case pf::PackageScriptOp::DestroyObject: return pf::PackageScriptOp::DestroyObjectFromVar;
    case pf::PackageScriptOp::DestroyObjectFromVar: return pf::PackageScriptOp::SetVarPickUpObjectFromVar;
    case pf::PackageScriptOp::SetVarPickUpObjectFromVar: return pf::PackageScriptOp::SetVarDropObjectFromVar;
    case pf::PackageScriptOp::SetVarDropObjectFromVar: return pf::PackageScriptOp::SetVarThrowObjectFromVar;
    case pf::PackageScriptOp::SetVarThrowObjectFromVar: return pf::PackageScriptOp::SetVarReflectObjectFromVar;
    case pf::PackageScriptOp::SetVarReflectObjectFromVar: return pf::PackageScriptOp::SetVarAbsorbObjectFromVar;
    case pf::PackageScriptOp::SetVarAbsorbObjectFromVar: return pf::PackageScriptOp::SetVarShieldBounceObjectFromVar;
    case pf::PackageScriptOp::SetVarShieldBounceObjectFromVar: return pf::PackageScriptOp::SetVarInteractObjectFromVar;
    case pf::PackageScriptOp::SetVarInteractObjectFromVar: return pf::PackageScriptOp::SetVarInteractObjectsFromVars;
    case pf::PackageScriptOp::SetVarInteractObjectsFromVars: return pf::PackageScriptOp::DestroyOwnedObjects;
    case pf::PackageScriptOp::DestroyOwnedObjects: return pf::PackageScriptOp::SkipIfVarLessThanImmediate;
    case pf::PackageScriptOp::SkipIfVarLessThanImmediate: return pf::PackageScriptOp::SkipIfVarLessThanVar;
    case pf::PackageScriptOp::SkipIfVarLessThanVar: return pf::PackageScriptOp::SkipIfVarEqualImmediate;
    case pf::PackageScriptOp::SkipIfVarEqualImmediate: return pf::PackageScriptOp::SkipIfVarEqualVar;
    case pf::PackageScriptOp::SkipIfVarEqualVar: return pf::PackageScriptOp::JumpRelative;
    case pf::PackageScriptOp::JumpRelative: return pf::PackageScriptOp::CallScript;
    case pf::PackageScriptOp::CallScript: return pf::PackageScriptOp::SwitchFighterDefinition;
    case pf::PackageScriptOp::SwitchFighterDefinition: return pf::PackageScriptOp::SpawnFighter;
    case pf::PackageScriptOp::SpawnFighter: return pf::PackageScriptOp::SpawnFighterSetVar;
    case pf::PackageScriptOp::SpawnFighterSetVar: return pf::PackageScriptOp::CallIndexedFighterScriptFromVar;
    case pf::PackageScriptOp::CallIndexedFighterScriptFromVar: return pf::PackageScriptOp::SetVarIndexedFighterStateIndex;
    case pf::PackageScriptOp::SetVarIndexedFighterStateIndex: return pf::PackageScriptOp::SetVarIndexedFighterPositionX;
    case pf::PackageScriptOp::SetVarIndexedFighterPositionX: return pf::PackageScriptOp::SetVarIndexedFighterPositionY;
    case pf::PackageScriptOp::SetVarIndexedFighterPositionY: return pf::PackageScriptOp::SetIndexedFighterStateFromVar;
    case pf::PackageScriptOp::SetIndexedFighterStateFromVar: return pf::PackageScriptOp::SetIndexedFighterPositionFromVars;
    case pf::PackageScriptOp::SetIndexedFighterPositionFromVars: return pf::PackageScriptOp::SetIndexedFighterFacingFromVar;
    case pf::PackageScriptOp::SetIndexedFighterFacingFromVar: return pf::PackageScriptOp::SetVarIndexedFighterVar;
    case pf::PackageScriptOp::SetVarIndexedFighterVar: return pf::PackageScriptOp::SetIndexedFighterVarImmediate;
    case pf::PackageScriptOp::SetIndexedFighterVarImmediate: return pf::PackageScriptOp::SetIndexedFighterVarFromVar;
    case pf::PackageScriptOp::SetIndexedFighterVarFromVar: return pf::PackageScriptOp::SetVarIndexedObjectVar;
    case pf::PackageScriptOp::SetVarIndexedObjectVar: return pf::PackageScriptOp::SetIndexedObjectVarImmediate;
    case pf::PackageScriptOp::SetIndexedObjectVarImmediate: return pf::PackageScriptOp::SetIndexedObjectVarFromVar;
    case pf::PackageScriptOp::SetIndexedObjectVarFromVar: return pf::PackageScriptOp::CallIndexedObjectScriptFromVar;
    case pf::PackageScriptOp::CallIndexedObjectScriptFromVar: return pf::PackageScriptOp::Nop;
    }
    return pf::PackageScriptOp::Nop;
}

static bool packageScriptOpAllowedForObject(pf::PackageScriptOp op) {
    return op != pf::PackageScriptOp::SwitchFighterDefinition &&
        op != pf::PackageScriptOp::SpawnFighter &&
        op != pf::PackageScriptOp::SpawnFighterSetVar &&
        op != pf::PackageScriptOp::CallIndexedFighterScriptFromVar &&
        op != pf::PackageScriptOp::SetVarIndexedFighterStateIndex &&
        op != pf::PackageScriptOp::SetVarIndexedFighterPositionX &&
        op != pf::PackageScriptOp::SetVarIndexedFighterPositionY &&
        op != pf::PackageScriptOp::SetIndexedFighterStateFromVar &&
        op != pf::PackageScriptOp::SetIndexedFighterPositionFromVars &&
        op != pf::PackageScriptOp::SetIndexedFighterFacingFromVar &&
        op != pf::PackageScriptOp::SetVarIndexedFighterVar &&
        op != pf::PackageScriptOp::SetIndexedFighterVarImmediate &&
        op != pf::PackageScriptOp::SetIndexedFighterVarFromVar &&
        op != pf::PackageScriptOp::SetVarButtonDown &&
        op != pf::PackageScriptOp::SetVarButtonPressed &&
        op != pf::PackageScriptOp::SetVarStickX &&
        op != pf::PackageScriptOp::SetVarStickY &&
        op != pf::PackageScriptOp::SetVarCStickX &&
        op != pf::PackageScriptOp::SetVarCStickY &&
        op != pf::PackageScriptOp::SetVarShield &&
        op != pf::PackageScriptOp::SetFighterCommandVarImmediate &&
        op != pf::PackageScriptOp::SetFighterCommandVarFromVar &&
        op != pf::PackageScriptOp::SetFighterThrowFlagImmediate &&
        op != pf::PackageScriptOp::SetFighterThrowFlagFromVar;
}

static pf::PackageScriptOp nextObjectPackageScriptOp(pf::PackageScriptOp op) {
    pf::PackageScriptOp next = nextPackageScriptOp(op);
    while (!packageScriptOpAllowedForObject(next)) {
        next = nextPackageScriptOp(next);
    }
    return next;
}

static pf::PackageScriptOp nextObjectContextReadOp(pf::PackageScriptOp op) {
    switch (op) {
    case pf::PackageScriptOp::SetVarObjectOwner: return pf::PackageScriptOp::SetObjectOwner;
    case pf::PackageScriptOp::SetObjectOwner: return pf::PackageScriptOp::SetObjectOwnerFromVar;
    case pf::PackageScriptOp::SetObjectOwnerFromVar: return pf::PackageScriptOp::SetVarObjectHeldBy;
    case pf::PackageScriptOp::SetVarObjectHeldBy: return pf::PackageScriptOp::SetVarObjectGrabVictim;
    case pf::PackageScriptOp::SetVarObjectGrabVictim: return pf::PackageScriptOp::SetVarObjectLastFighter;
    case pf::PackageScriptOp::SetVarObjectLastFighter: return pf::PackageScriptOp::SetVarObjectLastObject;
    case pf::PackageScriptOp::SetVarObjectLastObject: return pf::PackageScriptOp::SetVarObjectDamage;
    case pf::PackageScriptOp::SetVarObjectDamage: return pf::PackageScriptOp::SetObjectDamage;
    case pf::PackageScriptOp::SetObjectDamage: return pf::PackageScriptOp::SetObjectDamageFromVar;
    case pf::PackageScriptOp::SetObjectDamageFromVar: return pf::PackageScriptOp::SetVarObjectHitlag;
    case pf::PackageScriptOp::SetVarObjectHitlag: return pf::PackageScriptOp::SetObjectHitlag;
    case pf::PackageScriptOp::SetObjectHitlag: return pf::PackageScriptOp::SetObjectHitlagFromVar;
    case pf::PackageScriptOp::SetObjectHitlagFromVar: return pf::PackageScriptOp::SetVarObjectGroundSegment;
    case pf::PackageScriptOp::SetVarObjectGroundSegment: return pf::PackageScriptOp::SetVarObjectPositionX;
    case pf::PackageScriptOp::SetVarObjectPositionX: return pf::PackageScriptOp::SetVarObjectPositionY;
    case pf::PackageScriptOp::SetVarObjectPositionY: return pf::PackageScriptOp::SetVarObjectVelocityX;
    case pf::PackageScriptOp::SetVarObjectVelocityX: return pf::PackageScriptOp::SetVarObjectVelocityY;
    case pf::PackageScriptOp::SetVarObjectVelocityY: return pf::PackageScriptOp::SetVarObjectAnimationFrame;
    case pf::PackageScriptOp::SetVarObjectAnimationFrame: return pf::PackageScriptOp::SetVarObjectAnimationRate;
    case pf::PackageScriptOp::SetVarObjectAnimationRate: return pf::PackageScriptOp::SetVarOwnedObjectCount;
    case pf::PackageScriptOp::SetVarOwnedObjectCount: return pf::PackageScriptOp::SetVarOwnerFighterVar;
    case pf::PackageScriptOp::SetVarOwnerFighterVar: return pf::PackageScriptOp::SetOwnerFighterVarImmediate;
    case pf::PackageScriptOp::SetOwnerFighterVarImmediate: return pf::PackageScriptOp::SetOwnerFighterVarFromVar;
    case pf::PackageScriptOp::SetOwnerFighterVarFromVar: return pf::PackageScriptOp::CallOwnerFighterScript;
    case pf::PackageScriptOp::CallOwnerFighterScript: return pf::PackageScriptOp::SetVarObjectIndex;
    case pf::PackageScriptOp::SetVarObjectIndex: return pf::PackageScriptOp::SetVarObjectOwner;
    default: return pf::PackageScriptOp::SetVarObjectOwner;
    }
}

static bool packageScriptOpIsFighterContextRead(pf::PackageScriptOp op) {
    switch (op) {
    case pf::PackageScriptOp::SetVarFighterStateFrame:
    case pf::PackageScriptOp::SetVarFighterIndex:
    case pf::PackageScriptOp::SetVarFighterStateIndex:
    case pf::PackageScriptOp::SetVarFighterGrounded:
    case pf::PackageScriptOp::SetVarFighterFacing:
    case pf::PackageScriptOp::SetVarFighterJumpsUsed:
    case pf::PackageScriptOp::SetVarFighterJumpsRemaining:
    case pf::PackageScriptOp::SetVarFighterCommandVar:
    case pf::PackageScriptOp::SetVarFighterThrowFlag:
    case pf::PackageScriptOp::SetVarFighterHeldObject:
    case pf::PackageScriptOp::SetVarFighterGrabbedFighter:
    case pf::PackageScriptOp::SetVarFighterGrabberFighter:
    case pf::PackageScriptOp::SetVarFighterHitlag:
    case pf::PackageScriptOp::SetVarFighterHitstun:
    case pf::PackageScriptOp::SetVarFighterDamageHitboxOwner:
    case pf::PackageScriptOp::SetVarFighterThrownHitboxOwner:
    case pf::PackageScriptOp::SetVarFighterPercent:
    case pf::PackageScriptOp::SetVarFighterShield:
    case pf::PackageScriptOp::SetVarFighterPositionX:
    case pf::PackageScriptOp::SetVarFighterPositionY:
    case pf::PackageScriptOp::SetVarFighterGroundVelocity:
    case pf::PackageScriptOp::SetVarFighterAirVelocityX:
    case pf::PackageScriptOp::SetVarFighterAirVelocityY:
    case pf::PackageScriptOp::SetVarFighterAnimationFrame:
    case pf::PackageScriptOp::SetVarFighterAnimationRate:
        return true;
    default:
        return false;
    }
}

static pf::PackageScriptOp nextFighterContextReadOp(pf::PackageScriptOp op) {
    switch (op) {
    case pf::PackageScriptOp::SetVarFighterStateFrame: return pf::PackageScriptOp::SetVarFighterIndex;
    case pf::PackageScriptOp::SetVarFighterIndex: return pf::PackageScriptOp::SetVarFighterStateIndex;
    case pf::PackageScriptOp::SetVarFighterStateIndex: return pf::PackageScriptOp::SetVarFighterGrounded;
    case pf::PackageScriptOp::SetVarFighterGrounded: return pf::PackageScriptOp::SetVarFighterFacing;
    case pf::PackageScriptOp::SetVarFighterFacing: return pf::PackageScriptOp::SetVarFighterJumpsUsed;
    case pf::PackageScriptOp::SetVarFighterJumpsUsed: return pf::PackageScriptOp::SetVarFighterJumpsRemaining;
    case pf::PackageScriptOp::SetVarFighterJumpsRemaining: return pf::PackageScriptOp::SetVarFighterCommandVar;
    case pf::PackageScriptOp::SetVarFighterCommandVar: return pf::PackageScriptOp::SetVarFighterThrowFlag;
    case pf::PackageScriptOp::SetVarFighterThrowFlag: return pf::PackageScriptOp::SetVarFighterHeldObject;
    case pf::PackageScriptOp::SetVarFighterHeldObject: return pf::PackageScriptOp::SetVarFighterGrabbedFighter;
    case pf::PackageScriptOp::SetVarFighterGrabbedFighter: return pf::PackageScriptOp::SetVarFighterGrabberFighter;
    case pf::PackageScriptOp::SetVarFighterGrabberFighter: return pf::PackageScriptOp::SetVarFighterHitlag;
    case pf::PackageScriptOp::SetVarFighterHitlag: return pf::PackageScriptOp::SetVarFighterHitstun;
    case pf::PackageScriptOp::SetVarFighterHitstun: return pf::PackageScriptOp::SetVarFighterDamageHitboxOwner;
    case pf::PackageScriptOp::SetVarFighterDamageHitboxOwner: return pf::PackageScriptOp::SetVarFighterThrownHitboxOwner;
    case pf::PackageScriptOp::SetVarFighterThrownHitboxOwner: return pf::PackageScriptOp::SetVarFighterPercent;
    case pf::PackageScriptOp::SetVarFighterPercent: return pf::PackageScriptOp::SetVarFighterShield;
    case pf::PackageScriptOp::SetVarFighterShield: return pf::PackageScriptOp::SetVarFighterPositionX;
    case pf::PackageScriptOp::SetVarFighterPositionX: return pf::PackageScriptOp::SetVarFighterPositionY;
    case pf::PackageScriptOp::SetVarFighterPositionY: return pf::PackageScriptOp::SetVarFighterGroundVelocity;
    case pf::PackageScriptOp::SetVarFighterGroundVelocity: return pf::PackageScriptOp::SetVarFighterAirVelocityX;
    case pf::PackageScriptOp::SetVarFighterAirVelocityX: return pf::PackageScriptOp::SetVarFighterAirVelocityY;
    case pf::PackageScriptOp::SetVarFighterAirVelocityY: return pf::PackageScriptOp::SetVarFighterAnimationFrame;
    case pf::PackageScriptOp::SetVarFighterAnimationFrame: return pf::PackageScriptOp::SetVarFighterAnimationRate;
    case pf::PackageScriptOp::SetVarFighterAnimationRate: return pf::PackageScriptOp::SetVarFighterStateFrame;
    default: return pf::PackageScriptOp::SetVarFighterStateFrame;
    }
}

static void normalizePackageInstruction(
    pf::PackageScriptInstruction& instruction,
    const pf::FighterDefinition& def,
    const pf::World& world,
    int currentFighterDef,
    int selectedState,
    int selectedObjectDef)
{
    const int variableCount = static_cast<int>(def.packageVariables.size());
    if (variableCount > 0) {
        instruction.dst = std::clamp(instruction.dst < 0 ? 0 : instruction.dst, 0, variableCount - 1);
        instruction.srcA = std::clamp(instruction.srcA < 0 ? 0 : instruction.srcA, 0, variableCount - 1);
        instruction.srcB = std::clamp(instruction.srcB < 0 ? 0 : instruction.srcB, 0, variableCount - 1);
    } else {
        instruction.dst = -1;
        instruction.srcA = -1;
        instruction.srcB = -1;
    }
    sanitizePackageInstructionForVariableCount(instruction, variableCount);

    if (instruction.op == pf::PackageScriptOp::SetFacing && instruction.intValue == 0) {
        instruction.intValue = 1;
    }
    if (instruction.op == pf::PackageScriptOp::SetVarRandom && instruction.intValue <= 0) {
        instruction.intValue = 2;
    }
    if (instruction.op == pf::PackageScriptOp::SetVarFighterCommandVar) {
        instruction.intValue = std::clamp(instruction.intValue, 0, 3);
    }
    if (instruction.op == pf::PackageScriptOp::SetVarFighterThrowFlag) {
        instruction.intValue = std::clamp(instruction.intValue, 0, 31);
    }
    if ((instruction.op == pf::PackageScriptOp::CallIndexedFighterScriptFromVar ||
        instruction.op == pf::PackageScriptOp::CallOwnerFighterScript) &&
        !std::any_of(def.packageScripts.begin(), def.packageScripts.end(), [&](const pf::PackageScript& script) {
            return script.name == instruction.text;
        }))
    {
        instruction.text = packageScriptTargetName(def.packageScripts, 0);
    }
    if (instruction.op == pf::PackageScriptOp::CallIndexedObjectScriptFromVar &&
        !packageObjectScriptExists(world, instruction.text))
    {
        instruction.text = packageObjectScriptTargetName(world, selectedObjectDef);
        if (instruction.text.empty()) {
            instruction.op = pf::PackageScriptOp::Nop;
        }
    }
    if (instruction.op == pf::PackageScriptOp::SetFighterCommandVarImmediate ||
        instruction.op == pf::PackageScriptOp::SetFighterCommandVarFromVar)
    {
        instruction.dst = std::clamp(instruction.dst < 0 ? 0 : instruction.dst, 0, 3);
    }
    if (instruction.op == pf::PackageScriptOp::SetFighterThrowFlagImmediate ||
        instruction.op == pf::PackageScriptOp::SetFighterThrowFlagFromVar)
    {
        instruction.dst = std::clamp(instruction.dst < 0 ? 0 : instruction.dst, 0, 31);
    }
    if ((instruction.op == pf::PackageScriptOp::SetVarButtonDown ||
         instruction.op == pf::PackageScriptOp::SetVarButtonPressed) &&
        instruction.intValue == 0)
    {
        instruction.intValue = pf::ButtonJump;
    }
    if (instruction.op == pf::PackageScriptOp::ChangeState && instruction.text.empty() && !def.states.empty()) {
        const int stateIndex = std::clamp(selectedState, 0, static_cast<int>(def.states.size()) - 1);
        instruction.text = def.states[static_cast<size_t>(stateIndex)].name;
    }
    if (instruction.op == pf::PackageScriptOp::CallScript &&
        (instruction.text.empty() ||
         std::none_of(def.packageScripts.begin(), def.packageScripts.end(), [&](const pf::PackageScript& script) {
             return script.name == instruction.text;
         })))
    {
        instruction.text = packageScriptTargetName(def.packageScripts, 0);
    }
    if (instruction.op == pf::PackageScriptOp::CallIndexedObjectScriptFromVar &&
        !packageObjectScriptExists(world, instruction.text))
    {
        instruction.text = packageObjectScriptTargetName(world, selectedObjectDef);
        if (instruction.text.empty()) {
            instruction.op = pf::PackageScriptOp::Nop;
        }
    }
    normalizePackageSpawnInstructionTarget(instruction, world, selectedObjectDef);
    normalizePackageObjectTargetInstruction(instruction, world, selectedObjectDef);
    if (instruction.op == pf::PackageScriptOp::SwitchFighterDefinition && instruction.text.empty() && !world.fighterDefs.empty()) {
        instruction.text = packageFighterTargetName(world, currentFighterDef);
    }
    if ((instruction.op == pf::PackageScriptOp::SpawnFighter ||
         instruction.op == pf::PackageScriptOp::SpawnFighterSetVar) &&
        instruction.text.empty() &&
        !world.fighterDefs.empty())
    {
        instruction.text = packageFighterTargetName(world, currentFighterDef);
    }
}

static void normalizeObjectPackageInstruction(
    pf::PackageScriptInstruction& instruction,
    const pf::GameObjectDefinition& def,
    const pf::World& world,
    int selectedObjectState,
    int selectedObjectDef)
{
    const int variableCount = static_cast<int>(def.packageVariables.size());
    if (variableCount > 0) {
        instruction.dst = std::clamp(instruction.dst < 0 ? 0 : instruction.dst, 0, variableCount - 1);
        instruction.srcA = std::clamp(instruction.srcA < 0 ? 0 : instruction.srcA, 0, variableCount - 1);
        instruction.srcB = std::clamp(instruction.srcB < 0 ? 0 : instruction.srcB, 0, variableCount - 1);
    } else {
        instruction.dst = -1;
        instruction.srcA = -1;
        instruction.srcB = -1;
    }
    sanitizePackageInstructionForVariableCount(instruction, variableCount);

    if (instruction.op == pf::PackageScriptOp::SetFacing && instruction.intValue == 0) {
        instruction.intValue = 1;
    }
    if (instruction.op == pf::PackageScriptOp::SetVarRandom && instruction.intValue <= 0) {
        instruction.intValue = 2;
    }
    if (instruction.op == pf::PackageScriptOp::SetVarFighterCommandVar) {
        instruction.intValue = std::clamp(instruction.intValue, 0, 3);
    }
    if (instruction.op == pf::PackageScriptOp::SetVarFighterThrowFlag) {
        instruction.intValue = std::clamp(instruction.intValue, 0, 31);
    }
    if (instruction.op == pf::PackageScriptOp::ChangeState && instruction.text.empty() && !def.states.empty()) {
        const int stateIndex = std::clamp(selectedObjectState, 0, static_cast<int>(def.states.size()) - 1);
        instruction.text = def.states[static_cast<size_t>(stateIndex)].name;
    }
    if (instruction.op == pf::PackageScriptOp::CallScript &&
        (instruction.text.empty() ||
         std::none_of(def.packageScripts.begin(), def.packageScripts.end(), [&](const pf::PackageScript& script) {
             return script.name == instruction.text;
         })))
    {
        instruction.text = packageScriptTargetName(def.packageScripts, 0);
    }
    normalizePackageSpawnInstructionTarget(instruction, world, selectedObjectDef);
    normalizePackageObjectTargetInstruction(instruction, world, selectedObjectDef);
}

static void remapRemovedPackageVariableRef(int& ref, int removedIndex, int variableCountAfterRemove) {
    if (ref < 0) {
        return;
    }
    if (ref == removedIndex) {
        ref = variableCountAfterRemove > 0 ? std::min(removedIndex, variableCountAfterRemove - 1) : -1;
    } else if (ref > removedIndex) {
        --ref;
    }
}

static void remapRemovedPackageVariable(std::vector<pf::PackageScript>& scripts, int removedIndex, int variableCountAfterRemove) {
    for (pf::PackageScript& script : scripts) {
        for (pf::PackageScriptInstruction& instruction : script.instructions) {
            remapRemovedPackageVariableRef(instruction.dst, removedIndex, variableCountAfterRemove);
            remapRemovedPackageVariableRef(instruction.srcA, removedIndex, variableCountAfterRemove);
            remapRemovedPackageVariableRef(instruction.srcB, removedIndex, variableCountAfterRemove);
            sanitizePackageInstructionForVariableCount(instruction, variableCountAfterRemove);
        }
    }
}

static void normalizeInterruptPackageVariable(pf::InterruptRule& rule, int variableCount) {
    if (rule.condition != pf::InterruptCondition::PackageVarAtLeast) {
        return;
    }
    if (variableCount <= 0) {
        rule.condition = pf::InterruptCondition::WaitInput;
        rule.packageVariable = -1;
        return;
    }
    rule.packageVariable = std::clamp(rule.packageVariable < 0 ? 0 : rule.packageVariable, 0, variableCount - 1);
}

static void remapRemovedInterruptPackageVariable(std::vector<pf::FighterState>& states, int removedIndex, int variableCountAfterRemove) {
    for (pf::FighterState& state : states) {
        for (pf::InterruptRule& rule : state.interrupts) {
            if (rule.condition != pf::InterruptCondition::PackageVarAtLeast) {
                continue;
            }
            remapRemovedPackageVariableRef(rule.packageVariable, removedIndex, variableCountAfterRemove);
            normalizeInterruptPackageVariable(rule, variableCountAfterRemove);
        }
    }
}

static void remapRemovedObjectStateScriptTargets(
    pf::GameObjectDefinition& object,
    const std::string& removedStateName,
    const std::string& replacementStateName)
{
    for (pf::PackageScript& script : object.packageScripts) {
        for (pf::PackageScriptInstruction& instruction : script.instructions) {
            if (instruction.op == pf::PackageScriptOp::ChangeState && instruction.text == removedStateName) {
                instruction.text = replacementStateName;
            }
        }
    }
}

static bool objectStateNameAvailable(const pf::GameObjectDefinition& object, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < object.states.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && object.states[i].name == name) {
            return false;
        }
    }
    return true;
}

static bool fighterStateAliasLostByRemovingState(const pf::FighterDefinition& def, const std::string& target, const std::string& removedStateName) {
    return target == "AppealS" &&
        (removedStateName == "AppealSR" || removedStateName == "AppealSL") &&
        def.stateIndex("AppealSR") < 0 &&
        def.stateIndex("AppealSL") < 0;
}

static bool shouldRemapRemovedFighterStateTarget(const pf::FighterDefinition& def, const std::string& target, const std::string& removedStateName) {
    return target == removedStateName || fighterStateAliasLostByRemovingState(def, target, removedStateName);
}

static void remapRemovedFighterStateTargets(
    pf::FighterDefinition& def,
    const std::string& removedStateName,
    const std::string& replacementStateName)
{
    for (pf::FighterState& state : def.states) {
        if (shouldRemapRemovedFighterStateTarget(def, state.onAnimationFinishedState, removedStateName)) {
            state.onAnimationFinishedState = replacementStateName;
        }
        for (pf::InterruptRule& rule : state.interrupts) {
            if (shouldRemapRemovedFighterStateTarget(def, rule.targetState, removedStateName)) {
                rule.targetState = replacementStateName;
            }
        }
    }
    for (pf::PackageScript& script : def.packageScripts) {
        for (pf::PackageScriptInstruction& instruction : script.instructions) {
            if (instruction.op == pf::PackageScriptOp::ChangeState &&
                shouldRemapRemovedFighterStateTarget(def, instruction.text, removedStateName))
            {
                instruction.text = replacementStateName;
            }
        }
    }
}

static bool fighterStateNameAvailable(const pf::FighterDefinition& def, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < def.states.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && def.states[i].name == name) {
            return false;
        }
    }
    return true;
}

static void remapFighterStateTargets(
    pf::FighterDefinition& def,
    const std::string& oldStateName,
    const std::string& newStateName)
{
    for (pf::FighterState& state : def.states) {
        if (state.onAnimationFinishedState == oldStateName) {
            state.onAnimationFinishedState = newStateName;
        }
        for (pf::InterruptRule& rule : state.interrupts) {
            if (rule.targetState == oldStateName) {
                rule.targetState = newStateName;
            }
        }
    }
    for (pf::PackageScript& script : def.packageScripts) {
        for (pf::PackageScriptInstruction& instruction : script.instructions) {
            if (instruction.op == pf::PackageScriptOp::ChangeState && instruction.text == oldStateName) {
                instruction.text = newStateName;
            }
        }
    }
}

static void remapRemovedObjectScriptTarget(
    pf::PackageScriptInstruction& instruction,
    const std::string& removedObjectName,
    const std::string& replacementObjectName)
{
    if ((instruction.op != pf::PackageScriptOp::SpawnObject &&
         instruction.op != pf::PackageScriptOp::SpawnObjectFromVars &&
         instruction.op != pf::PackageScriptOp::SpawnProjectile &&
         instruction.op != pf::PackageScriptOp::SpawnProjectileFromVars &&
         !packageScriptOpTargetsAnyObject(instruction.op)) ||
        instruction.text != removedObjectName)
    {
        return;
    }
    if (replacementObjectName.empty()) {
        instruction.op = pf::PackageScriptOp::Nop;
        instruction.text.clear();
        return;
    }
    instruction.text = replacementObjectName;
}

static void remapRemovedPackageObjectTargets(
    pf::World& world,
    const std::string& removedObjectName,
    const std::string& replacementObjectName)
{
    for (pf::FighterDefinition& fighter : world.fighterDefs) {
        for (pf::FighterState& state : fighter.states) {
            for (pf::Subaction& subaction : state.action) {
                if ((subaction.type != pf::SubactionType::SpawnObject &&
                     subaction.type != pf::SubactionType::SpawnProjectile) ||
                    subaction.objectName != removedObjectName)
                {
                    continue;
                }
                if (replacementObjectName.empty()) {
                    subaction.type = pf::SubactionType::SyncTimer;
                    subaction.objectName.clear();
                    subaction.spawnVelocity = {};
                    subaction.spawnOffset = {};
                } else {
                    subaction.objectName = replacementObjectName;
                }
            }
        }
        for (pf::PackageScript& script : fighter.packageScripts) {
            for (pf::PackageScriptInstruction& instruction : script.instructions) {
                remapRemovedObjectScriptTarget(instruction, removedObjectName, replacementObjectName);
            }
        }
    }
    for (pf::GameObjectDefinition& object : world.objectDefs) {
        for (pf::PackageScript& script : object.packageScripts) {
            for (pf::PackageScriptInstruction& instruction : script.instructions) {
                remapRemovedObjectScriptTarget(instruction, removedObjectName, replacementObjectName);
            }
        }
    }
}

static bool objectNameAvailable(const pf::World& world, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < world.objectDefs.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && world.objectDefs[i].name == name) {
            return false;
        }
    }
    return true;
}

static void remapPackageFighterTargets(pf::World& world, const std::string& oldFighterName, const std::string& newFighterName) {
    auto remapInstruction = [&](pf::PackageScriptInstruction& instruction) {
        if ((instruction.op == pf::PackageScriptOp::SwitchFighterDefinition ||
             instruction.op == pf::PackageScriptOp::SpawnFighter ||
             instruction.op == pf::PackageScriptOp::SpawnFighterSetVar) &&
            instruction.text == oldFighterName)
        {
            instruction.text = newFighterName;
        }
    };
    for (pf::FighterDefinition& fighter : world.fighterDefs) {
        for (pf::PackageScript& script : fighter.packageScripts) {
            for (pf::PackageScriptInstruction& instruction : script.instructions) {
                remapInstruction(instruction);
            }
        }
    }
    for (pf::GameObjectDefinition& object : world.objectDefs) {
        for (pf::PackageScript& script : object.packageScripts) {
            for (pf::PackageScriptInstruction& instruction : script.instructions) {
                remapInstruction(instruction);
            }
        }
    }
}

static bool fighterNameAvailable(const pf::World& world, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < world.fighterDefs.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && world.fighterDefs[i].name == name) {
            return false;
        }
    }
    return true;
}

static int visibleListStart(int selected, int itemCount, int visibleRows) {
    if (itemCount <= visibleRows) {
        return 0;
    }
    return std::clamp(selected - visibleRows / 2, 0, itemCount - visibleRows);
}

static void drawPackageScriptBlockGraph(
    const pf::PackageScript& script,
    pf::FighterEditor& editor,
    Rectangle rect,
    pf::FighterEditorSession* session = nullptr)
{
    DrawRectangleRec(rect, Fade(LIGHTGRAY, 0.52f));
    DrawRectangleLinesEx(rect, 1.0f, Fade(DARKGRAY, 0.8f));
    DrawText("Block Graph", static_cast<int>(rect.x + 8.0f), static_cast<int>(rect.y + 7.0f), 13, DARKGRAY);
    if (script.instructions.empty()) {
        DrawText("No blocks", static_cast<int>(rect.x + 8.0f), static_cast<int>(rect.y + 34.0f), 13, GRAY);
        return;
    }

    const int visible = std::min(
        std::max(1, static_cast<int>((rect.width - 8.0f) / 50.0f)),
        static_cast<int>(script.instructions.size()));
    const int start = visibleListStart(
        editor.selectedPackageInstruction,
        static_cast<int>(script.instructions.size()),
        visible);
    for (int i = 0; i < visible; ++i) {
        const int instructionIndex = start + i;
        const float x = rect.x + 8.0f + static_cast<float>(i) * 50.0f;
        const Rectangle node{x, rect.y + 32.0f, 42.0f, 38.0f};
        const Vector2 mouse = GetMousePosition();
        const bool hovered = CheckCollisionPointRec(mouse, node);
        const bool selected = instructionIndex == editor.selectedPackageInstruction;
        DrawRectangleRec(node, selected ? Fade(GREEN, 0.72f) : (hovered ? Fade(ORANGE, 0.5f) : Fade(RAYWHITE, 0.75f)));
        DrawRectangleLinesEx(node, 1.0f, DARKGRAY);
        DrawText(packageScriptOpName(script.instructions[static_cast<size_t>(instructionIndex)].op), static_cast<int>(node.x + 4.0f), static_cast<int>(node.y + 6.0f), 10, BLACK);
        DrawText(std::to_string(instructionIndex).c_str(), static_cast<int>(node.x + 17.0f), static_cast<int>(node.y + 21.0f), 10, DARKGRAY);
        if (i + 1 < visible) {
            DrawLine(static_cast<int>(node.x + node.width), static_cast<int>(node.y + 19.0f), static_cast<int>(node.x + node.width + 8.0f), static_cast<int>(node.y + 19.0f), DARKGRAY);
        }
        if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            editor.selectedPackageInstruction = instructionIndex;
            editor.selectionKind = pf::FighterEditorSelectionKind::Instruction;
            if (session) {
                session->selectedPackageInstruction = instructionIndex;
            }
        }
    }
}

static const char* animationChannelName(pf::AnimationChannel channel) {
    switch (channel) {
    case pf::AnimationChannel::TranslateX: return "TX";
    case pf::AnimationChannel::TranslateY: return "TY";
    case pf::AnimationChannel::TranslateZ: return "TZ";
    case pf::AnimationChannel::RotateX: return "RX";
    case pf::AnimationChannel::RotateY: return "RY";
    case pf::AnimationChannel::RotateZ: return "RZ";
    case pf::AnimationChannel::ScaleX: return "SX";
    case pf::AnimationChannel::ScaleY: return "SY";
    case pf::AnimationChannel::ScaleZ: return "SZ";
    }
    return "??";
}

static pf::AnimationChannel nextAnimationChannel(pf::AnimationChannel channel) {
    switch (channel) {
    case pf::AnimationChannel::TranslateX: return pf::AnimationChannel::TranslateY;
    case pf::AnimationChannel::TranslateY: return pf::AnimationChannel::TranslateZ;
    case pf::AnimationChannel::TranslateZ: return pf::AnimationChannel::RotateX;
    case pf::AnimationChannel::RotateX: return pf::AnimationChannel::RotateY;
    case pf::AnimationChannel::RotateY: return pf::AnimationChannel::RotateZ;
    case pf::AnimationChannel::RotateZ: return pf::AnimationChannel::ScaleX;
    case pf::AnimationChannel::ScaleX: return pf::AnimationChannel::ScaleY;
    case pf::AnimationChannel::ScaleY: return pf::AnimationChannel::ScaleZ;
    case pf::AnimationChannel::ScaleZ: return pf::AnimationChannel::TranslateX;
    }
    return pf::AnimationChannel::TranslateX;
}

static std::string uniqueAuthoredClipName(const pf::FighterDefinition& def) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = "Clip" + std::to_string(index);
        const bool exists = std::any_of(def.authoredClips.begin(), def.authoredClips.end(), [&](const pf::AnimationClip& clip) {
            return clip.name == candidate;
        });
        if (!exists) {
            return candidate;
        }
    }
    return "ClipX";
}

static bool authoredClipNameAvailable(const pf::FighterDefinition& def, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < def.authoredClips.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && def.authoredClips[i].name == name) {
            return false;
        }
    }
    return true;
}

static int uniqueAuthoredClipActionIndex(const pf::FighterDefinition& def) {
    for (int index = 0; index < 10000; ++index) {
        const bool exists = std::any_of(def.authoredClips.begin(), def.authoredClips.end(), [&](const pf::AnimationClip& clip) {
            return clip.actionIndex == index;
        });
        if (!exists) {
            return index;
        }
    }
    return static_cast<int>(def.authoredClips.size());
}

static bool authoredClipActionIndexAvailable(const pf::FighterDefinition& def, int actionIndex, int ignoredIndex) {
    if (actionIndex < 0) {
        return false;
    }
    for (size_t i = 0; i < def.authoredClips.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && def.authoredClips[i].actionIndex == actionIndex) {
            return false;
        }
    }
    return true;
}

static int nextAuthoredClipActionIndex(const pf::FighterDefinition& def, int currentActionIndex, int direction, int ignoredIndex) {
    int candidate = std::max(0, currentActionIndex);
    for (int step = 0; step < 10000; ++step) {
        candidate = std::max(0, candidate + direction);
        if (authoredClipActionIndexAvailable(def, candidate, ignoredIndex)) {
            return candidate;
        }
    }
    return currentActionIndex;
}

static void ensureAuthoredRootJoint(pf::FighterDefinition& def) {
    if (!def.authoredSkeleton.empty()) {
        return;
    }
    def.authoredSkeleton.push_back({-1, "Root", 0, {}, {}, {pf::fx(1), pf::fx(1), pf::fx(1)}});
}

static bool authoredJointNameAvailable(const pf::FighterDefinition& def, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < def.authoredSkeleton.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && def.authoredSkeleton[i].name == name) {
            return false;
        }
    }
    return true;
}

static pf::FighterMesh makeEditorTriangleMesh() {
    pf::FighterMesh mesh;
    pf::FighterMeshBatch batch;
    batch.parentBone = 0;
    batch.singleBindBone = 0;
    batch.materialColor = {160, 220, 255, 255};

    auto vertex = [](pf::Vec3 position) {
        pf::FighterMeshVertex out;
        out.position = position;
        out.normal = {0, 0, pf::fx(1)};
        out.influences[0] = {0, 1.0f};
        return out;
    };
    batch.vertices = {
        vertex({pf::fxFromFloat(-0.35f), pf::fxFromFloat(0.2f), 0}),
        vertex({pf::fxFromFloat(0.35f), pf::fxFromFloat(0.2f), 0}),
        vertex({0, pf::fxFromFloat(1.0f), 0}),
    };
    mesh.batches.push_back(std::move(batch));
    return mesh;
}

static int authoredMeshVertexCount(const pf::FighterMesh& mesh) {
    int count = 0;
    for (const pf::FighterMeshBatch& batch : mesh.batches) {
        count += static_cast<int>(batch.vertices.size());
    }
    return count;
}

static pf::FighterMeshVertex* authoredMeshVertexAt(pf::FighterMesh& mesh, int vertexIndex) {
    int cursor = 0;
    for (pf::FighterMeshBatch& batch : mesh.batches) {
        const int next = cursor + static_cast<int>(batch.vertices.size());
        if (vertexIndex >= cursor && vertexIndex < next) {
            return &batch.vertices[static_cast<size_t>(vertexIndex - cursor)];
        }
        cursor = next;
    }
    return nullptr;
}

static std::string authoredMeshVertexInfluenceSummary(const pf::FighterMeshVertex& vertex) {
    std::string summary;
    int shown = 0;
    for (const pf::FighterMeshVertexInfluence& influence : vertex.influences) {
        if (influence.bone < 0 || influence.weight <= 0.0f) {
            continue;
        }
        if (!summary.empty()) {
            summary += " ";
        }
        summary += "j" + std::to_string(influence.bone) + ":" +
            std::to_string(static_cast<int>(std::lround(influence.weight * 100.0f)));
        ++shown;
        if (shown >= 3) {
            break;
        }
    }
    return summary.empty() ? "none" : summary;
}

static pf::Vec2 authoredMeshSize(const pf::FighterMesh& mesh) {
    bool haveVertex = false;
    pf::Fix minX = 0;
    pf::Fix maxX = 0;
    pf::Fix minY = 0;
    pf::Fix maxY = 0;
    for (const pf::FighterMeshBatch& batch : mesh.batches) {
        for (const pf::FighterMeshVertex& vertex : batch.vertices) {
            if (!haveVertex) {
                minX = maxX = vertex.position.x;
                minY = maxY = vertex.position.y;
                haveVertex = true;
            } else {
                minX = std::min(minX, vertex.position.x);
                maxX = std::max(maxX, vertex.position.x);
                minY = std::min(minY, vertex.position.y);
                maxY = std::max(maxY, vertex.position.y);
            }
        }
    }
    return haveVertex ? pf::Vec2{maxX - minX, maxY - minY} : pf::Vec2{};
}

static void scaleAuthoredMesh(pf::FighterMesh& mesh, pf::Fix scaleX, pf::Fix scaleY) {
    for (pf::FighterMeshBatch& batch : mesh.batches) {
        for (pf::FighterMeshVertex& vertex : batch.vertices) {
            vertex.position.x = pf::fxMul(vertex.position.x, scaleX);
            vertex.position.y = pf::fxMul(vertex.position.y, scaleY);
        }
    }
}

static void nudgeAuthoredMeshVertex(pf::FighterMesh& mesh, int vertexIndex, pf::Vec3 delta) {
    pf::FighterMeshVertex* vertex = authoredMeshVertexAt(mesh, vertexIndex);
    if (!vertex) {
        return;
    }
    vertex->position.x += delta.x;
    vertex->position.y += delta.y;
    vertex->position.z += delta.z;
}

static void normalizeAuthoredMeshVertexInfluences(pf::FighterMeshVertex& vertex, int fallbackJoint) {
    float sum = 0.0f;
    for (pf::FighterMeshVertexInfluence& influence : vertex.influences) {
        if (influence.bone < 0 || influence.weight <= 0.0f) {
            influence = {};
            influence.bone = -1;
            continue;
        }
        sum += influence.weight;
    }
    if (sum <= 0.0f) {
        vertex.influences = {};
        vertex.influences[0] = {fallbackJoint, 1.0f};
        return;
    }
    for (pf::FighterMeshVertexInfluence& influence : vertex.influences) {
        if (influence.bone >= 0 && influence.weight > 0.0f) {
            influence.weight /= sum;
        }
    }
}

static void bindAuthoredMeshVertexToJoint(pf::FighterMesh& mesh, int vertexIndex, int joint, int skeletonSize) {
    int cursor = 0;
    for (pf::FighterMeshBatch& batch : mesh.batches) {
        const int next = cursor + static_cast<int>(batch.vertices.size());
        if (vertexIndex >= cursor && vertexIndex < next) {
            pf::FighterMeshVertex& vertex = batch.vertices[static_cast<size_t>(vertexIndex - cursor)];
            vertex.influences = {};
            vertex.influences[0] = {joint, 1.0f};
            batch.parentBone = skeletonSize > 1 ? -1 : joint;
            batch.singleBindBone = skeletonSize > 1 ? -1 : joint;
            batch.hasEnvelopes = skeletonSize > 1;
            return;
        }
        cursor = next;
    }
}

static void blendAuthoredMeshVertexTowardJoint(
    pf::FighterMesh& mesh,
    int vertexIndex,
    int joint,
    int skeletonSize,
    float amount)
{
    int cursor = 0;
    for (pf::FighterMeshBatch& batch : mesh.batches) {
        const int next = cursor + static_cast<int>(batch.vertices.size());
        if (vertexIndex < cursor || vertexIndex >= next) {
            cursor = next;
            continue;
        }

        pf::FighterMeshVertex& vertex = batch.vertices[static_cast<size_t>(vertexIndex - cursor)];
        pf::FighterMeshVertexInfluence* target = nullptr;
        pf::FighterMeshVertexInfluence* empty = nullptr;
        for (pf::FighterMeshVertexInfluence& influence : vertex.influences) {
            if (influence.bone == joint) {
                target = &influence;
            }
            if (!empty && (influence.bone < 0 || influence.weight <= 0.0f)) {
                empty = &influence;
            }
            if (influence.weight > 0.0f) {
                influence.weight *= (1.0f - amount);
            }
        }
        if (!target) {
            target = empty ? empty : &vertex.influences.back();
            target->bone = joint;
            target->weight = 0.0f;
        }
        target->weight += amount;
        normalizeAuthoredMeshVertexInfluences(vertex, joint);
        batch.parentBone = skeletonSize > 1 ? -1 : joint;
        batch.singleBindBone = skeletonSize > 1 ? -1 : joint;
        batch.hasEnvelopes = skeletonSize > 1;
        return;
    }
}

static void bindAuthoredMeshToJoint(pf::FighterMesh& mesh, int joint) {
    for (pf::FighterMeshBatch& batch : mesh.batches) {
        batch.parentBone = joint;
        batch.singleBindBone = joint;
        for (pf::FighterMeshVertex& vertex : batch.vertices) {
            vertex.influences = {};
            vertex.influences[0] = {joint, 1.0f};
        }
    }
}

static int authoredMeshMaxInfluences(const pf::FighterMesh& mesh) {
    int maxInfluences = 0;
    for (const pf::FighterMeshBatch& batch : mesh.batches) {
        for (const pf::FighterMeshVertex& vertex : batch.vertices) {
            int vertexInfluences = 0;
            for (const pf::FighterMeshVertexInfluence& influence : vertex.influences) {
                if (influence.weight > 0.0f && influence.bone >= 0) {
                    ++vertexInfluences;
                }
            }
            maxInfluences = std::max(maxInfluences, vertexInfluences);
        }
    }
    return maxInfluences;
}

static float authoredMeshDistanceSquared(pf::Vec3 a, pf::Vec3 b) {
    const float dx = pf::fxToFloat(a.x - b.x);
    const float dy = pf::fxToFloat(a.y - b.y);
    const float dz = pf::fxToFloat(a.z - b.z);
    return dx * dx + dy * dy + dz * dz;
}

static std::vector<pf::Vec3> authoredSkeletonBindPositions(const std::vector<pf::AnimationJoint>& skeleton) {
    if (skeleton.empty()) {
        return {};
    }
    const pf::AnimationPose pose = pf::bindPose(skeleton);
    return pf::jointWorldTranslations(skeleton, pose);
}

static void autoWeightAuthoredMeshToSkeleton(
    pf::FighterMesh& mesh,
    const std::vector<pf::AnimationJoint>& skeleton)
{
    const std::vector<pf::Vec3> joints = authoredSkeletonBindPositions(skeleton);
    if (joints.empty()) {
        return;
    }

    for (pf::FighterMeshBatch& batch : mesh.batches) {
        batch.parentBone = joints.size() > 1 ? -1 : 0;
        batch.singleBindBone = joints.size() > 1 ? -1 : 0;
        batch.hasEnvelopes = joints.size() > 1;
        for (pf::FighterMeshVertex& vertex : batch.vertices) {
            int nearest = 0;
            int second = -1;
            float nearestDist = authoredMeshDistanceSquared(vertex.position, joints.front());
            float secondDist = 0.0f;
            for (size_t jointIndex = 1; jointIndex < joints.size(); ++jointIndex) {
                const float dist = authoredMeshDistanceSquared(vertex.position, joints[jointIndex]);
                if (dist < nearestDist) {
                    second = nearest;
                    secondDist = nearestDist;
                    nearest = static_cast<int>(jointIndex);
                    nearestDist = dist;
                } else if (second < 0 || dist < secondDist) {
                    second = static_cast<int>(jointIndex);
                    secondDist = dist;
                }
            }

            vertex.influences = {};
            if (second < 0) {
                vertex.influences[0] = {nearest, 1.0f};
                continue;
            }

            const float nearestLength = std::sqrt(nearestDist);
            const float secondLength = std::sqrt(secondDist);
            const float totalLength = nearestLength + secondLength;
            const float nearestWeight = totalLength > 0.0001f
                ? std::clamp(secondLength / totalLength, 0.0f, 1.0f)
                : 1.0f;
            vertex.influences[0] = {nearest, nearestWeight};
            vertex.influences[1] = {second, 1.0f - nearestWeight};
        }
    }
}

static int remapRemovedAuthoredBone(int bone, int removedIndex, int fallbackBone) {
    if (bone < 0) {
        return bone;
    }
    if (bone == removedIndex) {
        return fallbackBone;
    }
    return bone > removedIndex ? bone - 1 : bone;
}

static void removeAuthoredSkeletonJoint(pf::FighterDefinition& def, int jointIndex) {
    if (jointIndex < 0 || jointIndex >= static_cast<int>(def.authoredSkeleton.size()) ||
        def.authoredSkeleton.size() <= 1)
    {
        return;
    }
    const int removedParent = def.authoredSkeleton[static_cast<size_t>(jointIndex)].parent;
    def.authoredSkeleton.erase(def.authoredSkeleton.begin() + jointIndex);
    const int fallbackBone = def.authoredSkeleton.empty()
        ? -1
        : std::clamp(removedParent, 0, static_cast<int>(def.authoredSkeleton.size()) - 1);

    for (pf::AnimationJoint& joint : def.authoredSkeleton) {
        if (joint.parent == jointIndex) {
            joint.parent = removedParent;
        } else if (joint.parent > jointIndex) {
            --joint.parent;
        }
    }
    for (pf::AnimationClip& clip : def.authoredClips) {
        for (pf::AnimationTrack& track : clip.tracks) {
            track.joint = remapRemovedAuthoredBone(track.joint, jointIndex, fallbackBone);
        }
    }
    for (pf::FighterMeshBatch& batch : def.authoredMesh.batches) {
        batch.parentBone = remapRemovedAuthoredBone(batch.parentBone, jointIndex, fallbackBone);
        batch.singleBindBone = remapRemovedAuthoredBone(batch.singleBindBone, jointIndex, fallbackBone);
        for (pf::FighterMeshVertex& vertex : batch.vertices) {
            for (pf::FighterMeshVertexInfluence& influence : vertex.influences) {
                influence.bone = remapRemovedAuthoredBone(influence.bone, jointIndex, fallbackBone);
            }
        }
    }
}

static void scaleAuthoredJoint(pf::AnimationJoint& joint, pf::Vec3 delta) {
    const pf::Fix minScale = pf::fxFromFloat(0.05f);
    joint.scale.x = std::max(minScale, joint.scale.x + delta.x);
    joint.scale.y = std::max(minScale, joint.scale.y + delta.y);
    joint.scale.z = std::max(minScale, joint.scale.z + delta.z);
}

static void rotateAuthoredJoint(pf::AnimationJoint& joint, pf::Vec3 delta) {
    joint.rotation.x += delta.x;
    joint.rotation.y += delta.y;
    joint.rotation.z += delta.z;
}

static void sortAnimationKeys(std::vector<pf::AnimationKey>& keys) {
    std::sort(keys.begin(), keys.end(), [](const pf::AnimationKey& a, const pf::AnimationKey& b) {
        return a.frame < b.frame;
    });
}

static bool uiListRow(Rectangle rect, const std::string& label, bool active) {
    const Vector2 mouse = GetMousePosition();
    const bool hovered = CheckCollisionPointRec(mouse, rect);
    std::string fittedLabel = label;
    constexpr int fontSize = 13;
    const float maxTextWidth = std::max(0.0f, rect.width - 14.0f);
    if (MeasureText(fittedLabel.c_str(), fontSize) > maxTextWidth) {
        const std::string suffix = "...";
        while (!fittedLabel.empty() && MeasureText((fittedLabel + suffix).c_str(), fontSize) > maxTextWidth) {
            fittedLabel.pop_back();
        }
        fittedLabel += suffix;
    }
    DrawRectangleRec(rect, active ? Fade(GREEN, 0.72f) : (hovered ? Fade(ORANGE, 0.5f) : Fade(RAYWHITE, 0.62f)));
    DrawRectangleLinesEx(rect, 1.0f, Fade(DARKGRAY, 0.75f));
    DrawText(fittedLabel.c_str(), static_cast<int>(rect.x + 7.0f), static_cast<int>(rect.y + 5.0f), fontSize, BLACK);
    return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static std::string uniquePackageVariableName(const pf::FighterDefinition& def) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = "var" + std::to_string(index);
        const bool exists = std::any_of(def.packageVariables.begin(), def.packageVariables.end(), [&](const pf::PackageVariableDefinition& variable) {
            return variable.name == candidate;
        });
        if (!exists) {
            return candidate;
        }
    }
    return "varX";
}

static bool packageVariableNameAvailable(const pf::FighterDefinition& def, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < def.packageVariables.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && def.packageVariables[i].name == name) {
            return false;
        }
    }
    return true;
}

static std::string uniquePackageScriptName(const pf::FighterDefinition& def, const std::string& prefix) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        const bool exists = std::any_of(def.packageScripts.begin(), def.packageScripts.end(), [&](const pf::PackageScript& script) {
            return script.name == candidate;
        });
        if (!exists) {
            return candidate;
        }
    }
    return prefix + "X";
}

static std::string uniquePackageScriptName(const pf::FighterDefinition& def) {
    return uniquePackageScriptName(def, "Script");
}

static bool packageScriptNameAvailable(const pf::FighterDefinition& def, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < def.packageScripts.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && def.packageScripts[i].name == name) {
            return false;
        }
    }
    return true;
}

static std::string uniqueObjectPackageVariableName(const pf::GameObjectDefinition& def) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = "objVar" + std::to_string(index);
        const bool exists = std::any_of(def.packageVariables.begin(), def.packageVariables.end(), [&](const pf::PackageVariableDefinition& variable) {
            return variable.name == candidate;
        });
        if (!exists) {
            return candidate;
        }
    }
    return "objVarX";
}

static bool objectPackageVariableNameAvailable(const pf::GameObjectDefinition& def, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < def.packageVariables.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && def.packageVariables[i].name == name) {
            return false;
        }
    }
    return true;
}

static std::string uniqueObjectPackageScriptName(const pf::GameObjectDefinition& def, const std::string& prefix) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        const bool exists = std::any_of(def.packageScripts.begin(), def.packageScripts.end(), [&](const pf::PackageScript& script) {
            return script.name == candidate;
        });
        if (!exists) {
            return candidate;
        }
    }
    return prefix + "X";
}

static std::string uniqueObjectPackageScriptName(const pf::GameObjectDefinition& def) {
    return uniqueObjectPackageScriptName(def, "ObjectScript");
}

static bool objectPackageScriptNameAvailable(const pf::GameObjectDefinition& def, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < def.packageScripts.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && def.packageScripts[i].name == name) {
            return false;
        }
    }
    return true;
}

static std::string uniqueObjectName(const pf::World& world, const std::string& prefix) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        const bool exists = std::any_of(world.objectDefs.begin(), world.objectDefs.end(), [&](const pf::GameObjectDefinition& object) {
            return object.name == candidate;
        });
        if (!exists) {
            return candidate;
        }
    }
    return prefix + "X";
}

static std::string uniqueObjectStateName(const pf::GameObjectDefinition& object, const std::string& prefix) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        const bool exists = std::any_of(object.states.begin(), object.states.end(), [&](const pf::GameObjectStateDefinition& state) {
            return state.name == candidate;
        });
        if (!exists) {
            return candidate;
        }
    }
    return prefix + "X";
}

static std::string uniqueObjectStateName(const pf::GameObjectDefinition& object) {
    return uniqueObjectStateName(object, "State");
}

static std::string uniqueFighterName(const pf::World& world, const std::string& prefix) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        const bool exists = std::any_of(world.fighterDefs.begin(), world.fighterDefs.end(), [&](const pf::FighterDefinition& fighter) {
            return fighter.name == candidate;
        });
        if (!exists) {
            return candidate;
        }
    }
    return prefix + "X";
}

static pf::GameObjectDefinition makeEditorObjectDefinition(const std::string& name, pf::GameObjectKind kind) {
    pf::GameObjectDefinition object;
    object.name = name;
    object.kind = kind;
    object.initialState = 0;
    object.lifetimeFrames = kind == pf::GameObjectKind::Projectile ? 90 : 600;
    object.gravity = kind == pf::GameObjectKind::Projectile ? 0 : -pf::fxFromFloat(0.08f);
    object.terminalVelocity = pf::fxFromFloat(3.0f);
    object.destroyOnHit = kind == pf::GameObjectKind::Projectile;
    object.destroyOnShield = kind == pf::GameObjectKind::Projectile;
    object.states = {{"Idle", 1, true}};
    if (kind == pf::GameObjectKind::Projectile) {
        pf::HitboxDefinition hitbox;
        hitbox.radius = pf::fxFromFloat(0.45f);
        hitbox.damage = pf::fxFromFloat(3.0f);
        hitbox.knockbackAngleDegrees = pf::fx(45);
        hitbox.knockbackBase = pf::fx(20);
        hitbox.knockbackGrowth = pf::fx(60);
        object.hitboxes = {hitbox};
    } else {
        object.hurtboxes = {{{}, {}, pf::fxFromFloat(0.35f), pf::HurtboxState::Normal}};
        object.touchboxes = {{{}, {}, pf::fxFromFloat(0.45f), true, true}};
    }
    return object;
}

static pf::FighterDefinition makeEditorBlankFighterDefinition(const std::string& name, const pf::MeleeCommonData& common) {
    pf::FighterDefinition def;
    def.name = name;
    def.authoredEcb.enabled = true;
    def.properties.common = common;
    def.authoredSkeleton = {
        {-1, "Root", 0, {0, 0, 0}, {0, 0, 0}, {pf::fx(1), pf::fx(1), pf::fx(1)}},
    };
    pf::AnimationClip waitClip;
    waitClip.name = "Wait";
    waitClip.actionIndex = 0;
    waitClip.frameCount = pf::fx(60);
    waitClip.tracks = {
        {0, pf::AnimationChannel::TranslateY, {
            {0, 0, 0, pf::AnimationInterpolation::Linear},
            {pf::fx(60), 0, 0, pf::AnimationInterpolation::Linear},
        }},
    };
    pf::AnimationClip fallClip = waitClip;
    fallClip.name = "Fall";
    fallClip.actionIndex = 1;
    def.authoredClips = {waitClip, fallClip};
    def.hurtboxes = {
        pf::HurtboxDefinition{pf::BoneId::Hip, -1, "", {0, pf::fxFromFloat(-0.45f), 0}, {0, pf::fxFromFloat(0.55f), 0}, pf::fxFromFloat(0.45f), pf::HurtboxState::Normal, true},
        pf::HurtboxDefinition{pf::BoneId::Head, -1, "", {0, pf::fxFromFloat(-0.2f), 0}, {0, pf::fxFromFloat(0.2f), 0}, pf::fxFromFloat(0.32f), pf::HurtboxState::Normal, true},
    };

    pf::FighterState wait;
    wait.name = "Wait";
    wait.animation = "Wait";
    wait.animationActionIndex = 0;
    wait.animationLengthFrames = 60;
    wait.loopAnimation = true;
    wait.allowSlideoff = true;
    wait.allowLedgeGrab = true;
    wait.allowWallCollision = true;
    wait.allowCeilingCollision = true;
    wait.convertFloorCollisionToGround = true;
    wait.interrupts = {
        {"Fall", pf::InterruptCondition::BecameAirborne},
    };

    pf::FighterState fall;
    fall.name = "Fall";
    fall.animation = "Fall";
    fall.animationActionIndex = 1;
    fall.animationLengthFrames = 60;
    fall.loopAnimation = true;
    fall.allowSlideoff = true;
    fall.allowLedgeGrab = true;
    fall.allowWallCollision = true;
    fall.allowCeilingCollision = true;
    fall.convertFloorCollisionToGround = true;
    fall.onFrame = {{"process_airborne"}};
    fall.onLanding = {{"process_landing"}};

    def.states = {wait, fall};
    return def;
}

static void normalizeEditorAuthoredEcb(pf::FighterDefinition& def) {
    pf::FighterEcbDefinition& ecb = def.authoredEcb;
    ecb.points[0].x = std::min(ecb.points[0].x, -pf::fxFromFloat(0.1f));
    ecb.points[2].x = std::max(ecb.points[2].x, pf::fxFromFloat(0.1f));
    ecb.points[1].y = std::max(ecb.points[1].y, ecb.points[3].y + pf::fxFromFloat(0.5f));

    const pf::Fix minSide = ecb.points[3].y + pf::fxFromFloat(0.05f);
    const pf::Fix maxSide = ecb.points[1].y - pf::fxFromFloat(0.05f);
    const pf::Fix sideY = std::clamp(
        (ecb.points[0].y + ecb.points[2].y) / 2,
        minSide,
        maxSide);
    ecb.points[0].y = sideY;
    ecb.points[2].y = sideY;
}

static bool hasFunctionCall(const std::vector<pf::FunctionCall>& calls, const std::string& name) {
    return std::any_of(calls.begin(), calls.end(), [&](const pf::FunctionCall& call) {
        return call.name == name;
    });
}

static void bindPackageScriptCallback(
    std::vector<pf::FunctionCall>& calls,
    const std::string& scriptName,
    const char* label,
    pf::FighterEditor& editor)
{
    const std::string callback = "script:" + scriptName;
    if (!hasFunctionCall(calls, callback)) {
        calls.push_back({callback});
    }
    editor.status = std::string("Editor: bound ") + scriptName + " to selected state " + label;
}

static std::string callbackSummary(const std::vector<pf::FunctionCall>& calls) {
    if (calls.empty()) {
        return "-";
    }
    std::string summary = calls.front().name;
    if (calls.size() > 1) {
        summary += " +" + std::to_string(calls.size() - 1);
    }
    return summary;
}

static std::string animationBlendLabel(int blendFrames) {
    if (blendFrames == pf::kUseDefaultAnimationBlendFrames) {
        return "default";
    }
    if (blendFrames == pf::kDisableAnimationBlendFrames) {
        return "off";
    }
    return std::to_string(blendFrames);
}

static void removeLastCallback(
    std::vector<pf::FunctionCall>& calls,
    const char* label,
    pf::FighterEditor& editor)
{
    if (calls.empty()) {
        editor.status = std::string("Editor: no ") + label + " callback to remove";
        return;
    }
    const std::string removed = calls.back().name;
    calls.pop_back();
    editor.status = std::string("Editor: removed ") + label + " callback " + removed;
}

static int wrappedIndex(int index, int count) {
    return (index % count + count) % count;
}

static const char* objectStateCallbackLabel(int index) {
    switch (wrappedIndex(index, 4)) {
    case 0: return "state enter";
    case 1: return "state frame";
    case 2: return "state physics";
    case 3: return "state collision";
    }
    return "state enter";
}

static std::vector<pf::FunctionCall>& objectStateCallbacks(pf::GameObjectStateDefinition& state, int index) {
    switch (wrappedIndex(index, 4)) {
    case 0: return state.onEnter;
    case 1: return state.onFrame;
    case 2: return state.onPhysics;
    case 3: return state.onCollision;
    }
    return state.onEnter;
}

static const char* objectEventCallbackLabel(int index) {
    switch (wrappedIndex(index, 21)) {
    case 0: return "spawn";
    case 1: return "destroy";
    case 2: return "pickup";
    case 3: return "drop";
    case 4: return "throw";
    case 5: return "damage dealt";
    case 6: return "damage received";
    case 7: return "clank";
    case 8: return "reflect";
    case 9: return "absorb";
    case 10: return "shield bounce";
    case 11: return "hit shield";
    case 12: return "entered air";
    case 13: return "entered hitlag";
    case 14: return "exited hitlag";
    case 15: return "accessory";
    case 16: return "touch";
    case 17: return "jumped on";
    case 18: return "grab dealt";
    case 19: return "grab victim";
    case 20: return "interaction";
    }
    return "spawn";
}

static std::vector<pf::FunctionCall>& objectEventCallbacks(pf::GameObjectDefinition& object, int index) {
    switch (wrappedIndex(index, 21)) {
    case 0: return object.onSpawned;
    case 1: return object.onDestroyed;
    case 2: return object.onPickedUp;
    case 3: return object.onDropped;
    case 4: return object.onThrown;
    case 5: return object.onDamageDealt;
    case 6: return object.onDamageReceived;
    case 7: return object.onClanked;
    case 8: return object.onReflected;
    case 9: return object.onAbsorbed;
    case 10: return object.onShieldBounced;
    case 11: return object.onHitShield;
    case 12: return object.onEnteredAir;
    case 13: return object.onEnteredHitlag;
    case 14: return object.onExitedHitlag;
    case 15: return object.onAccessory;
    case 16: return object.onTouched;
    case 17: return object.onJumpedOn;
    case 18: return object.onGrabDealt;
    case 19: return object.onGrabbedForVictim;
    case 20: return object.onInteraction;
    }
    return object.onSpawned;
}

static void bindObjectPackageScriptCallback(
    std::vector<pf::FunctionCall>& calls,
    const std::string& scriptName,
    const char* label,
    pf::FighterEditor& editor)
{
    const std::string callback = "script:" + scriptName;
    if (!hasFunctionCall(calls, callback)) {
        calls.push_back({callback});
    }
    editor.status = std::string("Editor: bound ") + scriptName + " to selected object " + label;
}

static void bindObjectStatePackageScriptCallback(
    pf::GameObjectStateDefinition& state,
    const std::string& scriptName,
    pf::FighterEditor& editor)
{
    const int callback = wrappedIndex(editor.selectedObjectStateCallback, 4);
    bindObjectPackageScriptCallback(objectStateCallbacks(state, callback), scriptName, objectStateCallbackLabel(callback), editor);
    editor.selectedObjectStateCallback = callback + 1;
}

static void bindObjectEventPackageScriptCallback(
    pf::GameObjectDefinition& object,
    const std::string& scriptName,
    pf::FighterEditor& editor)
{
    const int callback = wrappedIndex(editor.selectedObjectEventCallback, 21);
    bindObjectPackageScriptCallback(objectEventCallbacks(object, callback), scriptName, objectEventCallbackLabel(callback), editor);
    editor.selectedObjectEventCallback = callback + 1;
}

static void removePackageScriptCallbackRefs(std::vector<pf::FunctionCall>& calls, const std::string& scriptName) {
    const std::string callback = "script:" + scriptName;
    calls.erase(
        std::remove_if(calls.begin(), calls.end(), [&](const pf::FunctionCall& call) {
            return call.name == callback;
        }),
        calls.end());
}

static void remapPackageScriptCallbackRefs(
    std::vector<pf::FunctionCall>& calls,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    const std::string oldCallback = "script:" + oldScriptName;
    const std::string newCallback = "script:" + newScriptName;
    for (pf::FunctionCall& call : calls) {
        if (call.name == oldCallback) {
            call.name = newCallback;
        }
    }
}

static bool packageInstructionCallsScriptName(const pf::PackageScriptInstruction& instruction, const std::string& scriptName) {
    return instruction.text == scriptName &&
        (instruction.op == pf::PackageScriptOp::CallScript ||
         instruction.op == pf::PackageScriptOp::CallIndexedFighterScriptFromVar ||
         instruction.op == pf::PackageScriptOp::CallOwnerFighterScript ||
         instruction.op == pf::PackageScriptOp::CallIndexedObjectScriptFromVar);
}

static void removePackageScriptInstructionRefs(std::vector<pf::PackageScript>& scripts, const std::string& scriptName) {
    for (pf::PackageScript& script : scripts) {
        for (pf::PackageScriptInstruction& instruction : script.instructions) {
            if (packageInstructionCallsScriptName(instruction, scriptName)) {
                instruction.op = pf::PackageScriptOp::Nop;
                instruction.text.clear();
            }
        }
    }
}

static void remapPackageScriptInstructionRefs(
    std::vector<pf::PackageScript>& scripts,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    for (pf::PackageScript& script : scripts) {
        for (pf::PackageScriptInstruction& instruction : script.instructions) {
            if (packageInstructionCallsScriptName(instruction, oldScriptName)) {
                instruction.text = newScriptName;
            }
        }
    }
}

static void removePackageScriptSubactionRefs(std::vector<pf::FighterState>& states, const std::string& scriptName) {
    for (pf::FighterState& state : states) {
        for (pf::Subaction& subaction : state.action) {
            if (subaction.type == pf::SubactionType::CallScript && subaction.objectName == scriptName) {
                subaction.type = pf::SubactionType::SyncTimer;
                subaction.objectName.clear();
                subaction.frames = std::max(1, subaction.frames);
            }
        }
    }
}

static void remapPackageScriptSubactionRefs(
    std::vector<pf::FighterState>& states,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    for (pf::FighterState& state : states) {
        for (pf::Subaction& subaction : state.action) {
            if (subaction.type == pf::SubactionType::CallScript && subaction.objectName == oldScriptName) {
                subaction.objectName = newScriptName;
            }
        }
    }
}

static void removeFighterPackageScriptRefs(pf::FighterDefinition& def, const std::string& scriptName) {
    removePackageScriptInstructionRefs(def.packageScripts, scriptName);
    removePackageScriptSubactionRefs(def.states, scriptName);
    for (pf::FighterState& state : def.states) {
        removePackageScriptCallbackRefs(state.onEnter, scriptName);
        removePackageScriptCallbackRefs(state.onFrame, scriptName);
        removePackageScriptCallbackRefs(state.onLanding, scriptName);
        removePackageScriptCallbackRefs(state.onAirborne, scriptName);
    }
}

static void remapFighterPackageScriptRefs(
    pf::FighterDefinition& def,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    remapPackageScriptInstructionRefs(def.packageScripts, oldScriptName, newScriptName);
    remapPackageScriptSubactionRefs(def.states, oldScriptName, newScriptName);
    for (pf::FighterState& state : def.states) {
        remapPackageScriptCallbackRefs(state.onEnter, oldScriptName, newScriptName);
        remapPackageScriptCallbackRefs(state.onFrame, oldScriptName, newScriptName);
        remapPackageScriptCallbackRefs(state.onLanding, oldScriptName, newScriptName);
        remapPackageScriptCallbackRefs(state.onAirborne, oldScriptName, newScriptName);
    }
}

static void removeObjectPackageScriptRefs(pf::GameObjectDefinition& object, const std::string& scriptName) {
    removePackageScriptInstructionRefs(object.packageScripts, scriptName);
    for (pf::GameObjectStateDefinition& state : object.states) {
        removePackageScriptCallbackRefs(state.onEnter, scriptName);
        removePackageScriptCallbackRefs(state.onFrame, scriptName);
        removePackageScriptCallbackRefs(state.onPhysics, scriptName);
        removePackageScriptCallbackRefs(state.onCollision, scriptName);
    }
    removePackageScriptCallbackRefs(object.onSpawned, scriptName);
    removePackageScriptCallbackRefs(object.onDestroyed, scriptName);
    removePackageScriptCallbackRefs(object.onPickedUp, scriptName);
    removePackageScriptCallbackRefs(object.onDropped, scriptName);
    removePackageScriptCallbackRefs(object.onThrown, scriptName);
    removePackageScriptCallbackRefs(object.onDamageDealt, scriptName);
    removePackageScriptCallbackRefs(object.onDamageReceived, scriptName);
    removePackageScriptCallbackRefs(object.onClanked, scriptName);
    removePackageScriptCallbackRefs(object.onReflected, scriptName);
    removePackageScriptCallbackRefs(object.onAbsorbed, scriptName);
    removePackageScriptCallbackRefs(object.onShieldBounced, scriptName);
    removePackageScriptCallbackRefs(object.onHitShield, scriptName);
    removePackageScriptCallbackRefs(object.onEnteredAir, scriptName);
    removePackageScriptCallbackRefs(object.onEnteredHitlag, scriptName);
    removePackageScriptCallbackRefs(object.onExitedHitlag, scriptName);
    removePackageScriptCallbackRefs(object.onAccessory, scriptName);
    removePackageScriptCallbackRefs(object.onTouched, scriptName);
    removePackageScriptCallbackRefs(object.onJumpedOn, scriptName);
    removePackageScriptCallbackRefs(object.onGrabDealt, scriptName);
    removePackageScriptCallbackRefs(object.onGrabbedForVictim, scriptName);
    removePackageScriptCallbackRefs(object.onInteraction, scriptName);
}

static void remapObjectPackageScriptRefs(
    pf::GameObjectDefinition& object,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    remapPackageScriptInstructionRefs(object.packageScripts, oldScriptName, newScriptName);
    for (pf::GameObjectStateDefinition& state : object.states) {
        remapPackageScriptCallbackRefs(state.onEnter, oldScriptName, newScriptName);
        remapPackageScriptCallbackRefs(state.onFrame, oldScriptName, newScriptName);
        remapPackageScriptCallbackRefs(state.onPhysics, oldScriptName, newScriptName);
        remapPackageScriptCallbackRefs(state.onCollision, oldScriptName, newScriptName);
    }
    remapPackageScriptCallbackRefs(object.onSpawned, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onDestroyed, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onPickedUp, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onDropped, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onThrown, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onDamageDealt, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onDamageReceived, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onClanked, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onReflected, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onAbsorbed, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onShieldBounced, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onHitShield, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onEnteredAir, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onEnteredHitlag, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onExitedHitlag, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onAccessory, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onTouched, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onJumpedOn, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onGrabDealt, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onGrabbedForVictim, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onInteraction, oldScriptName, newScriptName);
}

static void removeCrossFighterPackageScriptRefs(pf::World& world, const std::string& scriptName) {
    for (pf::FighterDefinition& fighter : world.fighterDefs) {
        for (pf::PackageScript& script : fighter.packageScripts) {
            for (pf::PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.op == pf::PackageScriptOp::CallIndexedFighterScriptFromVar &&
                    instruction.text == scriptName)
                {
                    instruction.op = pf::PackageScriptOp::Nop;
                    instruction.text.clear();
                }
            }
        }
    }
    for (pf::GameObjectDefinition& object : world.objectDefs) {
        for (pf::PackageScript& script : object.packageScripts) {
            for (pf::PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.op == pf::PackageScriptOp::CallOwnerFighterScript &&
                    instruction.text == scriptName)
                {
                    instruction.op = pf::PackageScriptOp::Nop;
                    instruction.text.clear();
                }
            }
        }
    }
}

static void remapCrossFighterPackageScriptRefs(
    pf::World& world,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    for (pf::FighterDefinition& fighter : world.fighterDefs) {
        for (pf::PackageScript& script : fighter.packageScripts) {
            for (pf::PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.op == pf::PackageScriptOp::CallIndexedFighterScriptFromVar &&
                    instruction.text == oldScriptName)
                {
                    instruction.text = newScriptName;
                }
            }
        }
    }
    for (pf::GameObjectDefinition& object : world.objectDefs) {
        for (pf::PackageScript& script : object.packageScripts) {
            for (pf::PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.op == pf::PackageScriptOp::CallOwnerFighterScript &&
                    instruction.text == oldScriptName)
                {
                    instruction.text = newScriptName;
                }
            }
        }
    }
}

static void removeCrossObjectPackageScriptRefs(pf::World& world, const std::string& scriptName) {
    for (pf::FighterDefinition& fighter : world.fighterDefs) {
        for (pf::PackageScript& script : fighter.packageScripts) {
            for (pf::PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.op == pf::PackageScriptOp::CallIndexedObjectScriptFromVar &&
                    instruction.text == scriptName)
                {
                    instruction.op = pf::PackageScriptOp::Nop;
                    instruction.text.clear();
                }
            }
        }
    }
    for (pf::GameObjectDefinition& object : world.objectDefs) {
        for (pf::PackageScript& script : object.packageScripts) {
            for (pf::PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.op == pf::PackageScriptOp::CallIndexedObjectScriptFromVar &&
                    instruction.text == scriptName)
                {
                    instruction.op = pf::PackageScriptOp::Nop;
                    instruction.text.clear();
                }
            }
        }
    }
}

static void remapCrossObjectPackageScriptRefs(
    pf::World& world,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    for (pf::FighterDefinition& fighter : world.fighterDefs) {
        for (pf::PackageScript& script : fighter.packageScripts) {
            for (pf::PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.op == pf::PackageScriptOp::CallIndexedObjectScriptFromVar &&
                    instruction.text == oldScriptName)
                {
                    instruction.text = newScriptName;
                }
            }
        }
    }
    for (pf::GameObjectDefinition& object : world.objectDefs) {
        for (pf::PackageScript& script : object.packageScripts) {
            for (pf::PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.op == pf::PackageScriptOp::CallIndexedObjectScriptFromVar &&
                    instruction.text == oldScriptName)
                {
                    instruction.text = newScriptName;
                }
            }
        }
    }
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

static pf::Vec3 authoredMeshVertexWorld(const pf::FighterRuntime& fighter, const pf::FighterMeshBatch& batch, const pf::FighterMeshVertex& vertex) {
    float blendedX = 0.0f;
    float blendedY = 0.0f;
    float blendedZ = 0.0f;
    float weightSum = 0.0f;
    for (const pf::FighterMeshVertexInfluence& influence : vertex.influences) {
        if (influence.weight <= 0.0f ||
            influence.bone < 0 ||
            static_cast<size_t>(influence.bone) >= fighter.jointWorldTransforms.size())
        {
            continue;
        }
        const pf::Vec3 weighted = pf::transformPoint(
            fighter.jointWorldTransforms[static_cast<size_t>(influence.bone)],
            vertex.position);
        blendedX += pf::fxToFloat(weighted.x) * influence.weight;
        blendedY += pf::fxToFloat(weighted.y) * influence.weight;
        blendedZ += pf::fxToFloat(weighted.z) * influence.weight;
        weightSum += influence.weight;
    }
    if (weightSum > 0.0f) {
        return {pf::fxFromFloat(blendedX), pf::fxFromFloat(blendedY), pf::fxFromFloat(blendedZ)};
    }

    const int bone = batch.singleBindBone >= 0 ? batch.singleBindBone : batch.parentBone;
    if (bone >= 0 && static_cast<size_t>(bone) < fighter.jointWorldTransforms.size()) {
        return pf::transformPoint(fighter.jointWorldTransforms[static_cast<size_t>(bone)], vertex.position);
    }
    return {fighter.position.x + vertex.position.x, fighter.position.y + vertex.position.y, vertex.position.z};
}

static void drawAuthoredMesh(const pf::FighterDefinition& def, const pf::FighterRuntime& fighter) {
    const pf::FighterMesh& mesh = pf::authoredFighterMesh(def);
    for (const pf::FighterMeshBatch& batch : mesh.batches) {
        const Color color{batch.materialColor[0], batch.materialColor[1], batch.materialColor[2], batch.materialColor[3]};
        for (size_t i = 0; i + 2 < batch.vertices.size(); i += 3) {
            const pf::Vec3 a = authoredMeshVertexWorld(fighter, batch, batch.vertices[i]);
            const pf::Vec3 b = authoredMeshVertexWorld(fighter, batch, batch.vertices[i + 1]);
            const pf::Vec3 c = authoredMeshVertexWorld(fighter, batch, batch.vertices[i + 2]);
            DrawTriangle3D(toRay(a), toRay(b), toRay(c), color);
            DrawLine3D(toRay(a), toRay(b), BLACK);
            DrawLine3D(toRay(b), toRay(c), BLACK);
            DrawLine3D(toRay(c), toRay(a), BLACK);
        }
    }
}

static bool authoredMeshVertexWorldAt(
    const pf::FighterDefinition& def,
    const pf::FighterRuntime& fighter,
    int vertexIndex,
    pf::Vec3& out)
{
    int cursor = 0;
    const pf::FighterMesh& mesh = pf::authoredFighterMesh(def);
    for (const pf::FighterMeshBatch& batch : mesh.batches) {
        const int next = cursor + static_cast<int>(batch.vertices.size());
        if (vertexIndex >= cursor && vertexIndex < next) {
            out = authoredMeshVertexWorld(fighter, batch, batch.vertices[static_cast<size_t>(vertexIndex - cursor)]);
            return true;
        }
        cursor = next;
    }
    return false;
}

static void drawEditorSelectedAuthoredMeshVertex(const pf::World& world, const pf::FighterEditor& editor) {
    if (editor.workspace != pf::EditorWorkspace::Assets ||
        editor.selectedFighter < 0 ||
        editor.selectedFighter >= static_cast<int>(world.fighters.size()))
    {
        return;
    }
    const pf::FighterRuntime& fighter = world.fighters[static_cast<size_t>(editor.selectedFighter)];
    if (fighter.fighterDef < 0 || fighter.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
        return;
    }
    const pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    pf::Vec3 selectedPosition;
    if (!authoredMeshVertexWorldAt(def, fighter, editor.selectedAuthoredMeshVertex, selectedPosition)) {
        return;
    }
    const Vector3 rayPosition = toRay(selectedPosition);
    DrawSphere(rayPosition, 0.075f, YELLOW);
    DrawSphereWires(rayPosition, 0.12f, 10, 6, BLACK);
}

static void drawEditorSelectedAuthoredJoint(const pf::World& world, const pf::FighterEditor& editor) {
    if (editor.workspace != pf::EditorWorkspace::Animation ||
        editor.selectedFighter < 0 ||
        editor.selectedFighter >= static_cast<int>(world.fighters.size()))
    {
        return;
    }
    const pf::FighterRuntime& fighter = world.fighters[static_cast<size_t>(editor.selectedFighter)];
    if (fighter.fighterDef < 0 || fighter.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
        return;
    }
    const pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    if (def.authoredSkeleton.empty() ||
        editor.selectedAnimationJoint < 0 ||
        editor.selectedAnimationJoint >= static_cast<int>(fighter.jointWorldPositions.size()) ||
        editor.selectedAnimationJoint >= static_cast<int>(def.authoredSkeleton.size()))
    {
        return;
    }
    const Vector3 jointPosition = toRay(fighter.jointWorldPositions[static_cast<size_t>(editor.selectedAnimationJoint)]);
    DrawSphere(jointPosition, 0.09f, YELLOW);
    DrawSphereWires(jointPosition, 0.16f, 12, 8, BLACK);
}

static void drawFighter(const pf::World& world, const pf::FighterRuntime& fighter, Color color, const pf::FighterEditor& editor) {
    const pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const Vector3 pos = toRayGround(fighter.position);
    const bool hasAnimationPose = !fighter.jointWorldPositions.empty() && animationSkeletonForDrawing(def) != nullptr;
    if (!editor.showModel) {
        // Keep debug volumes visible when the mesh/model layer is hidden.
    } else if (fighter.fighterInvisible) {
        // ftDrawCommon skips fighter model display when x221E_b5 is set.
    } else if (hasAnimationPose) {
        const pf::FighterMesh& mesh = pf::authoredFighterMesh(def);
        if (!mesh.batches.empty()) {
            drawNativeFighterMesh(def, fighter);
        } else {
            DrawCylinder(pos, 0.18f, 0.18f, 0.04f, 18, Fade(color, 0.45f));
            drawAnimationSkeleton(def, fighter, color);
        }
    } else {
        const pf::FighterMesh& mesh = pf::authoredFighterMesh(def);
        if (!mesh.batches.empty()) {
            drawAuthoredMesh(def, fighter);
        } else {
            DrawCube(pos, 0.55f, 1.1f, 0.35f, color);
            DrawCubeWires(pos, 0.55f, 1.1f, 0.35f, BLACK);
        }
    }

    const bool drawDebug = editor.showBoxes;
    if (!hasAnimationPose || (drawDebug && editor.showSkeleton)) {
        for (int i = 0; i < pf::kBoneCount; ++i) {
            pf::Vec3 bone = fighter.bones[static_cast<size_t>(i)].position;
            bone.x += fighter.position.x;
            bone.y += fighter.position.y;
            DrawSphere(toRay(bone), hasAnimationPose ? 0.04f : 0.07f, hasAnimationPose ? Fade(DARKBLUE, 0.35f) : DARKBLUE);
        }
    }

    if (drawsShield(pf::currentState(world, fighter)) && fighter.shieldHealth > 0) {
        pf::Vec3 center = fighter.bones[static_cast<size_t>(pf::BoneId::Hip)].position;
        center.x += fighter.position.x;
        center.y += fighter.position.y + pf::fxFromFloat(0.2f);
        const int shieldBone = def.fighterBones.shield;
        if (shieldBone >= 0 && static_cast<size_t>(shieldBone) < fighter.jointWorldTransforms.size()) {
            center = pf::transformPoint(fighter.jointWorldTransforms[static_cast<size_t>(shieldBone)], {});
        }
        DrawSphereWires(toRay(center), pf::fxToFloat(shieldRadius(def, fighter)), 18, 10, VIOLET);
    }

    if (!drawDebug) {
        return;
    }

    if (editor.showEcb) {
        drawEcb(fighter, YELLOW);
        drawNativeEcbSourceJoints(def, fighter);
    }
    if (editor.showHurtboxes && fighter.poseHurtboxCapsules.empty()) {
        for (const pf::HurtboxDefinition& hurt : def.hurtboxes) {
            pf::Vec3 base = fighter.bones[static_cast<size_t>(hurt.bone)].position;
            base.x += fighter.position.x;
            base.y += fighter.position.y;
            drawCapsule(base + hurt.startOffset, base + hurt.endOffset, hurt.radius, GREEN);
        }
    }
    if (editor.showHurtboxes) {
        for (const pf::Capsule& hurt : fighter.poseHurtboxCapsules) {
            drawCapsule(hurt.a, hurt.b, hurt.radius, GREEN);
        }
    }
    if (editor.showSkeleton) {
        drawAnimationSkeleton(def, fighter, DARKGREEN);
    }
    if (editor.showHitboxes) {
        for (const pf::ActiveHitbox& hit : fighter.activeHitboxes) {
            drawCapsule(hit.previous, hit.current, hit.def.radius, RED);
        }
    }
}

static void drawGameObjects(const pf::World& world, const pf::FighterEditor& editor) {
    for (const pf::GameObjectRuntime& object : world.objects) {
        if (!object.active || object.objectDef < 0 || object.objectDef >= static_cast<int>(world.objectDefs.size())) {
            continue;
        }
        const pf::GameObjectDefinition& def = world.objectDefs[static_cast<size_t>(object.objectDef)];
        const Color color = def.kind == pf::GameObjectKind::Projectile ? MAROON : BROWN;
        DrawSphere(toRayGround(object.position), def.kind == pf::GameObjectKind::Projectile ? 0.14f : 0.24f, color);
        DrawSphereWires(toRayGround(object.position), def.kind == pf::GameObjectKind::Projectile ? 0.16f : 0.27f, 10, 6, BLACK);
        if (!editor.showBoxes) {
            continue;
        }
        if (editor.showHitboxes) {
            for (const pf::ActiveHitbox& hit : object.activeHitboxes) {
                drawCapsule(hit.previous, hit.current, hit.def.radius, RED);
            }
        }
        if (editor.showHurtboxes) {
            for (const pf::GameObjectHurtboxDefinition& hurtbox : def.hurtboxes) {
                drawCapsule(
                    {object.position.x + hurtbox.startOffset.x, object.position.y + hurtbox.startOffset.y, hurtbox.startOffset.z},
                    {object.position.x + hurtbox.endOffset.x, object.position.y + hurtbox.endOffset.y, hurtbox.endOffset.z},
                    hurtbox.radius,
                    GREEN);
            }
        }
        if (editor.showHurtboxes) {
            for (const pf::GameObjectTouchboxDefinition& touchbox : def.touchboxes) {
                drawCapsule(
                    {object.position.x + touchbox.startOffset.x, object.position.y + touchbox.startOffset.y, touchbox.startOffset.z},
                    {object.position.x + touchbox.endOffset.x, object.position.y + touchbox.endOffset.y, touchbox.endOffset.z},
                    touchbox.radius,
                    SKYBLUE);
            }
        }
    }
}

static void drawMainMenu(AppMode& mode) {
    DrawText("PFighter", GetScreenWidth() / 2 - MeasureText("PFighter", 44) / 2, 150, 44, BLACK);
    DrawText("runtime fighter packages and Melee parity lab", GetScreenWidth() / 2 - MeasureText("runtime fighter packages and Melee parity lab", 16) / 2, 204, 16, DARKGRAY);
    if (uiButton(mainMenuButtonRect(0), "Regular Gameplay")) {
        mode = AppMode::Gameplay;
    }
    if (uiButton(mainMenuButtonRect(1), "Fighter Editor")) {
        mode = AppMode::Editor;
    }
}

static void drawTopNav(AppMode& mode) {
    if (uiButton(topNavButtonRect(0), "Menu", mode == AppMode::MainMenu)) {
        mode = AppMode::MainMenu;
    }
    if (uiButton(topNavButtonRect(1), "Play", mode == AppMode::Gameplay)) {
        mode = AppMode::Gameplay;
    }
    if (uiButton(topNavButtonRect(2), "Editor", mode == AppMode::Editor)) {
        mode = AppMode::Editor;
    }
}

static void drawEditorWorkspaceTabs(pf::FighterEditor& editor) {
    const std::array<pf::EditorWorkspace, 5> workspaces{
        pf::EditorWorkspace::Moveset,
        pf::EditorWorkspace::Logic,
        pf::EditorWorkspace::Assets,
        pf::EditorWorkspace::Animation,
        pf::EditorWorkspace::TestLab,
    };
    for (size_t i = 0; i < workspaces.size(); ++i) {
        const pf::EditorWorkspace workspace = workspaces[i];
        if (uiButton(editorWorkspaceTabRect(static_cast<int>(i)), workspaceName(workspace), editor.workspace == workspace)) {
            editor.workspace = workspace;
            editor.status = std::string("Editor workspace: ") + workspaceName(workspace);
        }
    }
}

static void drawEditorLogicWorkspace(pf::World& world, pf::FighterEditor& editor) {
    editor.clampToWorld(world);
    if (world.fighters.empty()) {
        return;
    }
    pf::FighterRuntime& fighter = world.fighters[static_cast<size_t>(editor.selectedFighter)];
    if (fighter.fighterDef < 0 || fighter.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
        return;
    }
    pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    pf::FighterState& state = def.states[static_cast<size_t>(editor.selectedState)];
    const Rectangle panel{12.0f, 324.0f, 530.0f, 390.0f};
    DrawRectangleRec(panel, Fade(RAYWHITE, 0.58f));
    DrawRectangleLinesEx(panel, 1.0f, DARKGRAY);
    DrawText("Package Logic", 24, 336, 16, BLACK);
    DrawText(("State hooks: enter " + std::to_string(state.onEnter.size()) +
              "  frame " + std::to_string(state.onFrame.size()) +
              "  land " + std::to_string(state.onLanding.size()) +
              "  air " + std::to_string(state.onAirborne.size())).c_str(), 24, 358, 13, DARKGRAY);
    DrawText(("Hooks: In " + callbackSummary(state.onEnter) +
              "  Fr " + callbackSummary(state.onFrame) +
              "  Ld " + callbackSummary(state.onLanding) +
              "  Air " + callbackSummary(state.onAirborne)).c_str(), 24, 376, 12, DARKGRAY);

    if (uiButton({338.0f, 334.0f, 58.0f, 24.0f}, "+ Var")) {
        def.packageVariables.push_back({uniquePackageVariableName(def), 0});
        editor.selectedPackageVariable = static_cast<int>(def.packageVariables.size()) - 1;
        for (pf::FighterRuntime& runtime : world.fighters) {
            if (runtime.fighterDef == fighter.fighterDef) {
                runtime.packageVars.push_back(def.packageVariables.back().initialValue);
            }
        }
        editor.status = "Editor: added package variable " + def.packageVariables.back().name;
    }
    if (uiButton({402.0f, 334.0f, 58.0f, 24.0f}, "- Var")) {
        if (!def.packageVariables.empty()) {
            const int removedIndex = std::clamp(editor.selectedPackageVariable, 0, static_cast<int>(def.packageVariables.size()) - 1);
            const std::string removed = def.packageVariables[static_cast<size_t>(removedIndex)].name;
            def.packageVariables.erase(def.packageVariables.begin() + removedIndex);
            remapRemovedPackageVariable(def.packageScripts, removedIndex, static_cast<int>(def.packageVariables.size()));
            remapRemovedInterruptPackageVariable(def.states, removedIndex, static_cast<int>(def.packageVariables.size()));
            for (pf::FighterRuntime& runtime : world.fighters) {
                if (runtime.fighterDef != fighter.fighterDef) {
                    continue;
                }
                if (removedIndex < static_cast<int>(runtime.packageVars.size())) {
                    runtime.packageVars.erase(runtime.packageVars.begin() + removedIndex);
                }
                runtime.packageVars.resize(def.packageVariables.size());
            }
            editor.selectedPackageVariable = std::clamp(editor.selectedPackageVariable, 0, std::max(0, static_cast<int>(def.packageVariables.size()) - 1));
            editor.status = "Editor: removed package variable " + removed;
        }
    }
    if (uiButton({338.0f, 364.0f, 58.0f, 24.0f}, "+ Script")) {
        pf::PackageScript script;
        script.name = uniquePackageScriptName(def);
        script.instructionBudget = 64;
        def.packageScripts.push_back(std::move(script));
        editor.selectedPackageScript = static_cast<int>(def.packageScripts.size()) - 1;
        editor.selectedPackageInstruction = 0;
        editor.status = "Editor: added package script " + def.packageScripts.back().name;
    }
    if (uiButton({402.0f, 364.0f, 58.0f, 24.0f}, "- Script")) {
        if (!def.packageScripts.empty()) {
            const std::string removed = def.packageScripts[static_cast<size_t>(editor.selectedPackageScript)].name;
            def.packageScripts.erase(def.packageScripts.begin() + editor.selectedPackageScript);
            removeFighterPackageScriptRefs(def, removed);
            removeCrossFighterPackageScriptRefs(world, removed);
            editor.selectedPackageScript = std::clamp(editor.selectedPackageScript, 0, std::max(0, static_cast<int>(def.packageScripts.size()) - 1));
            editor.selectedPackageInstruction = 0;
            editor.status = "Editor: removed package script " + removed;
        }
    }
    if (uiButton({584.0f, 364.0f, 58.0f, 24.0f}, "Clone")) {
        if (!def.packageScripts.empty()) {
            editor.selectedPackageScript = std::clamp(editor.selectedPackageScript, 0, static_cast<int>(def.packageScripts.size()) - 1);
            pf::PackageScript clone = def.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
            clone.name = uniquePackageScriptName(def, clone.name + "Copy");
            def.packageScripts.push_back(std::move(clone));
            editor.selectedPackageScript = static_cast<int>(def.packageScripts.size()) - 1;
            editor.selectedPackageInstruction = 0;
            editor.status = "Editor: cloned package script " + def.packageScripts.back().name;
        }
    }
    if (uiButton({466.0f, 334.0f, 50.0f, 24.0f}, "-In")) {
        removeLastCallback(state.onEnter, "enter", editor);
    }
    if (uiButton({522.0f, 334.0f, 50.0f, 24.0f}, "-Fr")) {
        removeLastCallback(state.onFrame, "frame", editor);
    }
    if (uiButton({466.0f, 364.0f, 50.0f, 24.0f}, "-Ld")) {
        removeLastCallback(state.onLanding, "landing", editor);
    }
    if (uiButton({522.0f, 364.0f, 50.0f, 24.0f}, "-Air")) {
        removeLastCallback(state.onAirborne, "airborne", editor);
    }

    DrawText("Vars", 24, 392, 13, DARKGRAY);
    if (!def.packageVariables.empty()) {
        editor.selectedPackageVariable = std::clamp(
            editor.selectedPackageVariable,
            0,
            static_cast<int>(def.packageVariables.size()) - 1);
        pf::PackageVariableDefinition& variable = def.packageVariables[static_cast<size_t>(editor.selectedPackageVariable)];
        if (uiButton({24.0f, 650.0f, 54.0f, 22.0f}, "Init-")) {
            --variable.initialValue;
            editor.status = "Editor: decreased initial value for " + variable.name;
        }
        if (uiButton({84.0f, 650.0f, 54.0f, 22.0f}, "Init+")) {
            ++variable.initialValue;
            editor.status = "Editor: increased initial value for " + variable.name;
        }
        std::string renamedVariable;
        if (uiTextField(
                {24.0f, 386.0f, 156.0f, 22.0f},
                "fighter-var-" + std::to_string(editor.selectedPackageVariable),
                editor,
                variable.name,
                renamedVariable))
        {
            if (!packageVariableNameAvailable(def, renamedVariable, editor.selectedPackageVariable)) {
                editor.status = "Editor: package variable name is empty or already used";
            } else {
                const std::string oldName = variable.name;
                variable.name = renamedVariable;
                editor.status = "Editor: renamed package variable " + oldName + " to " + variable.name;
            }
        }
    }
    const int visibleVars = std::min(5, static_cast<int>(def.packageVariables.size()));
    const int variableStart = visibleListStart(
        editor.selectedPackageVariable,
        static_cast<int>(def.packageVariables.size()),
        visibleVars);
    for (int row = 0; row < visibleVars; ++row) {
        const int variableIndex = variableStart + row;
        const pf::PackageVariableDefinition& variable = def.packageVariables[static_cast<size_t>(variableIndex)];
        const std::string label = variable.name + " = " + std::to_string(variable.initialValue);
        if (uiListRow({24.0f, 412.0f + 24.0f * row, 156.0f, 22.0f}, label, variableIndex == editor.selectedPackageVariable)) {
            editor.selectedPackageVariable = variableIndex;
        }
    }
    if (def.packageVariables.empty()) {
        DrawText("No variables", 31, 417, 13, GRAY);
    }

    DrawText("Scripts", 196, 392, 13, DARKGRAY);
    const int visibleScripts = std::min(5, static_cast<int>(def.packageScripts.size()));
    const int scriptStart = visibleListStart(
        editor.selectedPackageScript,
        static_cast<int>(def.packageScripts.size()),
        visibleScripts);
    for (int row = 0; row < visibleScripts; ++row) {
        const int scriptIndex = scriptStart + row;
        const pf::PackageScript& script = def.packageScripts[static_cast<size_t>(scriptIndex)];
        const std::string label = script.name + " (" + std::to_string(script.instructions.size()) + ")";
        if (uiListRow({196.0f, 412.0f + 24.0f * row, 156.0f, 22.0f}, label, scriptIndex == editor.selectedPackageScript)) {
            editor.selectedPackageScript = scriptIndex;
            editor.selectedPackageInstruction = 0;
        }
    }
    if (def.packageScripts.empty()) {
        DrawText("No scripts", 203, 417, 13, GRAY);
    }

    pf::PackageScript* script = nullptr;
    if (!def.packageScripts.empty()) {
        script = &def.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
        std::string renamedScript;
        if (uiTextField(
                {196.0f, 386.0f, 156.0f, 22.0f},
                "fighter-script-" + std::to_string(editor.selectedPackageScript),
                editor,
                script->name,
                renamedScript))
        {
            if (!packageScriptNameAvailable(def, renamedScript, editor.selectedPackageScript)) {
                editor.status = "Editor: package script name is empty or already used";
            } else {
                const std::string oldName = script->name;
                script->name = renamedScript;
                remapFighterPackageScriptRefs(def, oldName, script->name);
                remapCrossFighterPackageScriptRefs(world, oldName, script->name);
                editor.status = "Editor: renamed package script " + oldName + " to " + script->name;
            }
        }
    }
    DrawText("Instructions", 24, 506, 13, DARKGRAY);
    if (script) {
        const int visibleInstructions = std::min(2, static_cast<int>(script->instructions.size()));
        const int instructionStart = visibleListStart(
            editor.selectedPackageInstruction,
            static_cast<int>(script->instructions.size()),
            visibleInstructions);
        for (int row = 0; row < visibleInstructions; ++row) {
            const int instructionIndex = instructionStart + row;
            const pf::PackageScriptInstruction& instruction = script->instructions[static_cast<size_t>(instructionIndex)];
            if (uiListRow({106.0f, 500.0f + 24.0f * row, 246.0f, 22.0f}, packageInstructionLabel(instruction), instructionIndex == editor.selectedPackageInstruction)) {
                editor.selectedPackageInstruction = instructionIndex;
            }
        }
        if (script->instructions.empty()) {
            DrawText("No instructions", 113, 505, 13, GRAY);
        }
        drawPackageScriptBlockGraph(*script, editor, {24.0f, 536.0f, 328.0f, 108.0f});
    }

    if (uiButton({365.0f, 412.0f, 68.0f, 24.0f}, "+ AddVar")) {
        if (script && !def.packageVariables.empty()) {
            script->instructions.push_back({pf::PackageScriptOp::AddVarImmediate, editor.selectedPackageVariable, -1, -1, 1, 0, {}});
            editor.selectedPackageInstruction = static_cast<int>(script->instructions.size()) - 1;
            editor.status = "Editor: appended AddVar instruction to " + script->name;
        }
    }
    if (uiButton({440.0f, 412.0f, 68.0f, 24.0f}, "+ VelX")) {
        if (script) {
            script->instructions.push_back({pf::PackageScriptOp::SetAirVelocityX, -1, -1, -1, 0, pf::fxFromFloat(0.5f), {}});
            editor.selectedPackageInstruction = static_cast<int>(script->instructions.size()) - 1;
            editor.status = "Editor: appended AirVelX instruction to " + script->name;
        }
    }
    if (uiButton({515.0f, 412.0f, 68.0f, 24.0f}, "+ Fact")) {
        if (script && !def.packageVariables.empty()) {
            script->instructions.push_back({pf::PackageScriptOp::SetVarFrame, editor.selectedPackageVariable, -1, -1, 0, 0, {}});
            editor.selectedPackageInstruction = static_cast<int>(script->instructions.size()) - 1;
            editor.status = "Editor: appended runtime fact read to " + script->name;
        }
    }
    if (uiButton({590.0f, 412.0f, 58.0f, 24.0f}, "+ Ctx")) {
        if (script && !def.packageVariables.empty()) {
            script->instructions.push_back({pf::PackageScriptOp::SetVarFighterPercent, editor.selectedPackageVariable, -1, -1, 0, 0, {}});
            editor.selectedPackageInstruction = static_cast<int>(script->instructions.size()) - 1;
            editor.status = "Editor: appended fighter context read to " + script->name;
        }
    }
    if (uiButton({365.0f, 442.0f, 68.0f, 24.0f}, "- Instr")) {
        if (script && !script->instructions.empty()) {
            script->instructions.erase(script->instructions.begin() + editor.selectedPackageInstruction);
            editor.selectedPackageInstruction = std::clamp(editor.selectedPackageInstruction, 0, std::max(0, static_cast<int>(script->instructions.size()) - 1));
            editor.status = "Editor: removed package instruction";
        }
    }
    if (uiButton({440.0f, 442.0f, 68.0f, 24.0f}, "+ Spawn")) {
        if (script && !world.objectDefs.empty()) {
            editor.selectedObjectDef = std::clamp(editor.selectedObjectDef, 0, static_cast<int>(world.objectDefs.size()) - 1);
            const pf::GameObjectDefinition& object = world.objectDefs[static_cast<size_t>(editor.selectedObjectDef)];
            script->instructions.push_back({pf::PackageScriptOp::SpawnObject, -1, -1, -1, 0, pf::fxFromFloat(1.0f), object.name});
            editor.selectedPackageInstruction = static_cast<int>(script->instructions.size()) - 1;
            editor.status = "Editor: appended SpawnObject instruction for " + object.name;
        }
    }
    if (uiButton({590.0f, 442.0f, 58.0f, 24.0f}, "+ Proj")) {
        if (script) {
            const std::string projectileName = packageObjectTargetName(world, editor.selectedObjectDef, pf::GameObjectKind::Projectile);
            if (projectileName.empty()) {
                editor.status = "Editor: add a projectile object before authoring projectile spawns";
            } else {
                script->instructions.push_back({pf::PackageScriptOp::SpawnProjectile, -1, -1, -1, 0, pf::fxFromFloat(1.0f), projectileName});
                editor.selectedPackageInstruction = static_cast<int>(script->instructions.size()) - 1;
                editor.status = "Editor: appended projectile spawn for " + projectileName;
            }
        }
    }
    if (uiButton({515.0f, 442.0f, 68.0f, 24.0f}, "+ Btn")) {
        if (script && !def.packageVariables.empty()) {
            script->instructions.push_back({pf::PackageScriptOp::SetVarButtonPressed, editor.selectedPackageVariable, -1, -1, pf::ButtonAttack, 0, {}});
            editor.selectedPackageInstruction = static_cast<int>(script->instructions.size()) - 1;
            editor.status = "Editor: appended button input read to " + script->name;
        }
    }
    if (uiButton({515.0f, 472.0f, 68.0f, 24.0f}, "+ Axis")) {
        if (script && !def.packageVariables.empty()) {
            script->instructions.push_back({pf::PackageScriptOp::SetVarStickX, editor.selectedPackageVariable, -1, -1, 0, 0, {}});
            editor.selectedPackageInstruction = static_cast<int>(script->instructions.size()) - 1;
            editor.status = "Editor: appended analog input read to " + script->name;
        }
    }
    if (uiButton({365.0f, 682.0f, 68.0f, 24.0f}, "+ If")) {
        if (script && !def.packageVariables.empty()) {
            script->instructions.push_back({pf::PackageScriptOp::SkipIfVarLessThanImmediate, editor.selectedPackageVariable, -1, -1, 1, 0, {}});
            script->instructions.push_back({pf::PackageScriptOp::Nop, -1, -1, -1, 0, 0, {}});
            editor.selectedPackageInstruction = static_cast<int>(script->instructions.size()) - 2;
            editor.status = "Editor: appended branch condition to " + script->name;
        }
    }
    if (uiButton({440.0f, 682.0f, 68.0f, 24.0f}, "+ Jump")) {
        if (script) {
            script->instructions.push_back({pf::PackageScriptOp::JumpRelative, -1, -1, -1, 1, 0, {}});
            editor.selectedPackageInstruction = static_cast<int>(script->instructions.size()) - 1;
            editor.status = "Editor: appended jump to " + script->name;
        }
    }
    if (uiButton({515.0f, 682.0f, 68.0f, 24.0f}, "+ Call")) {
        if (script && !def.packageScripts.empty()) {
            script->instructions.push_back({
                pf::PackageScriptOp::CallScript,
                -1,
                -1,
                -1,
                0,
                0,
                packageScriptTargetName(def.packageScripts, editor.selectedPackageScript),
            });
            editor.selectedPackageInstruction = static_cast<int>(script->instructions.size()) - 1;
            editor.status = "Editor: appended script call to " + script->instructions.back().text;
        }
    }
    if (uiButton({290.0f, 682.0f, 68.0f, 24.0f}, "+ Fighter")) {
        if (script && !world.fighterDefs.empty()) {
            script->instructions.push_back({
                pf::PackageScriptOp::SwitchFighterDefinition,
                -1,
                -1,
                -1,
                0,
                0,
                packageFighterTargetName(world, fighter.fighterDef),
            });
            editor.selectedPackageInstruction = static_cast<int>(script->instructions.size()) - 1;
            editor.status = "Editor: appended fighter switch to " + script->name + " targeting " + script->instructions.back().text;
        }
    }
    if (uiButton({215.0f, 682.0f, 68.0f, 24.0f}, "+ Ally")) {
        if (script && !world.fighterDefs.empty()) {
            script->instructions.push_back({
                pf::PackageScriptOp::SpawnFighter,
                -1,
                -1,
                -1,
                0,
                pf::fxFromFloat(1.0f),
                packageFighterTargetName(world, fighter.fighterDef),
            });
            editor.selectedPackageInstruction = static_cast<int>(script->instructions.size()) - 1;
            editor.status = "Editor: appended companion spawn to " + script->name + " targeting " + script->instructions.back().text;
        }
    }
    if (uiButton({365.0f, 472.0f, 68.0f, 24.0f}, "Bind In")) {
        if (script) {
            bindPackageScriptCallback(state.onEnter, script->name, "enter", editor);
        }
    }
    if (uiButton({440.0f, 472.0f, 68.0f, 24.0f}, "Bind Fr")) {
        if (script) {
            bindPackageScriptCallback(state.onFrame, script->name, "frame", editor);
        }
    }
    if (uiButton({365.0f, 502.0f, 68.0f, 24.0f}, "Budget")) {
        if (script) {
            script->instructionBudget = script->instructionBudget == 64 ? 256 : 64;
            editor.status = "Editor: script budget set to " + std::to_string(script->instructionBudget);
        }
    }
    if (script && !script->instructions.empty()) {
        editor.selectedPackageInstruction = std::clamp(editor.selectedPackageInstruction, 0, static_cast<int>(script->instructions.size()) - 1);
        pf::PackageScriptInstruction& instruction = script->instructions[static_cast<size_t>(editor.selectedPackageInstruction)];
        normalizePackageInstruction(instruction, def, world, fighter.fighterDef, editor.selectedState, editor.selectedObjectDef);
        if (uiButton({440.0f, 502.0f, 68.0f, 24.0f}, "Op")) {
            instruction.op = nextPackageScriptOp(instruction.op);
            normalizePackageInstruction(instruction, def, world, fighter.fighterDef, editor.selectedState, editor.selectedObjectDef);
            editor.status = "Editor: cycled selected script block op";
        }
        if (uiButton({515.0f, 502.0f, 68.0f, 24.0f}, "Blk<")) {
            moveEditorPackageInstruction(*script, editor, -1);
        }
        if (uiButton({590.0f, 502.0f, 58.0f, 24.0f}, "Blk>")) {
            moveEditorPackageInstruction(*script, editor, 1);
        }
        if (uiButton({365.0f, 532.0f, 68.0f, 24.0f}, "Dst")) {
            if (!def.packageVariables.empty()) {
                instruction.dst = (std::max(0, instruction.dst) + 1) % static_cast<int>(def.packageVariables.size());
                editor.status = "Editor: cycled selected script destination variable";
            }
        }
        if (uiButton({440.0f, 532.0f, 68.0f, 24.0f}, "Src")) {
            if (!def.packageVariables.empty()) {
                instruction.srcA = (std::max(0, instruction.srcA) + 1) % static_cast<int>(def.packageVariables.size());
                instruction.srcB = (std::max(0, instruction.srcB) + 1) % static_cast<int>(def.packageVariables.size());
                editor.status = "Editor: cycled selected script source variables";
            }
        }
        if (uiButton({515.0f, 532.0f, 68.0f, 24.0f}, "Btn")) {
            if (instruction.op == pf::PackageScriptOp::SetVarButtonDown ||
                instruction.op == pf::PackageScriptOp::SetVarButtonPressed)
            {
                instruction.intValue = nextPackageInputButton(instruction.intValue);
                editor.status = "Editor: cycled selected script input button";
            }
        }
        if (uiButton({590.0f, 532.0f, 58.0f, 24.0f}, "CtxOp")) {
            instruction.op = nextFighterContextReadOp(instruction.op);
            normalizePackageInstruction(instruction, def, world, fighter.fighterDef, editor.selectedState, editor.selectedObjectDef);
            editor.status = "Editor: cycled selected fighter context read";
        }
        if (uiButton({365.0f, 562.0f, 68.0f, 24.0f}, "Val -")) {
            instruction.intValue -= 1;
            editor.status = "Editor: decreased selected script integer value";
        }
        if (uiButton({440.0f, 562.0f, 68.0f, 24.0f}, "Val +")) {
            instruction.intValue += 1;
            editor.status = "Editor: increased selected script integer value";
        }
        if (uiButton({365.0f, 592.0f, 68.0f, 24.0f}, "Fix -")) {
            instruction.fixValue -= pf::fxFromFloat(0.1f);
            editor.status = "Editor: decreased selected script fixed value";
        }
        if (uiButton({440.0f, 592.0f, 68.0f, 24.0f}, "Fix +")) {
            instruction.fixValue += pf::fxFromFloat(0.1f);
            editor.status = "Editor: increased selected script fixed value";
        }
        if (uiButton({365.0f, 622.0f, 68.0f, 24.0f}, "State")) {
            instruction.text = state.name;
            instruction.op = pf::PackageScriptOp::ChangeState;
            editor.status = "Editor: targeted selected state from script block";
        }
        if (uiButton({440.0f, 622.0f, 68.0f, 24.0f}, "Object")) {
            if (!world.objectDefs.empty()) {
                editor.selectedObjectDef = std::clamp(editor.selectedObjectDef, 0, static_cast<int>(world.objectDefs.size()) - 1);
                instruction.text = world.objectDefs[static_cast<size_t>(editor.selectedObjectDef)].name;
                instruction.op = pf::PackageScriptOp::SpawnObject;
                editor.status = "Editor: targeted selected object from script block";
            }
        }
        if (uiButton({515.0f, 622.0f, 68.0f, 24.0f}, "Proj")) {
            const std::string projectileName = packageObjectTargetName(world, editor.selectedObjectDef, pf::GameObjectKind::Projectile);
            if (projectileName.empty()) {
                editor.status = "Editor: add a projectile object before targeting a projectile";
            } else {
                instruction.text = projectileName;
                instruction.op = pf::PackageScriptOp::SpawnProjectile;
                editor.status = "Editor: targeted projectile " + instruction.text + " from script block";
            }
        }
        if (uiButton({590.0f, 622.0f, 58.0f, 24.0f}, "Call")) {
            cyclePackageScriptTarget(instruction, def.packageScripts, editor.selectedPackageScript);
            editor.status = "Editor: targeted script call " + instruction.text + " from script block";
        }
        if (uiButton({290.0f, 622.0f, 68.0f, 24.0f}, "Fighter")) {
            if (!world.fighterDefs.empty()) {
                cyclePackageFighterTarget(instruction, pf::PackageScriptOp::SwitchFighterDefinition, world, fighter.fighterDef);
                editor.status = "Editor: targeted fighter " + instruction.text + " from script block";
            }
        }
        if (uiButton({215.0f, 622.0f, 68.0f, 24.0f}, "Ally")) {
            if (!world.fighterDefs.empty()) {
                cyclePackageFighterTarget(instruction, pf::PackageScriptOp::SpawnFighter, world, fighter.fighterDef);
                editor.status = "Editor: targeted companion fighter " + instruction.text + " from script block";
            }
        }
        if (uiButton({365.0f, 652.0f, 68.0f, 24.0f}, "BindLd")) {
            bindPackageScriptCallback(state.onLanding, script->name, "landing", editor);
        }
        if (uiButton({440.0f, 652.0f, 68.0f, 24.0f}, "BindAir")) {
            bindPackageScriptCallback(state.onAirborne, script->name, "airborne", editor);
        }
    }
}

static void demoteProjectileSpawnsForObject(pf::World& world, const std::string& objectName) {
    for (pf::FighterDefinition& fighter : world.fighterDefs) {
        for (pf::FighterState& state : fighter.states) {
            for (pf::Subaction& subaction : state.action) {
                if (subaction.type == pf::SubactionType::SpawnProjectile &&
                    subaction.objectName == objectName)
                {
                    subaction.type = pf::SubactionType::SpawnObject;
                }
            }
        }
        for (pf::PackageScript& script : fighter.packageScripts) {
            for (pf::PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.text != objectName) {
                    continue;
                }
                if (instruction.op == pf::PackageScriptOp::SpawnProjectile) {
                    instruction.op = pf::PackageScriptOp::SpawnObject;
                } else if (instruction.op == pf::PackageScriptOp::SpawnProjectileFromVars) {
                    instruction.op = pf::PackageScriptOp::SpawnObjectFromVars;
                }
            }
        }
    }
    for (pf::GameObjectDefinition& object : world.objectDefs) {
        for (pf::PackageScript& script : object.packageScripts) {
            for (pf::PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.text != objectName) {
                    continue;
                }
                if (instruction.op == pf::PackageScriptOp::SpawnProjectile) {
                    instruction.op = pf::PackageScriptOp::SpawnObject;
                } else if (instruction.op == pf::PackageScriptOp::SpawnProjectileFromVars) {
                    instruction.op = pf::PackageScriptOp::SpawnObjectFromVars;
                }
            }
        }
    }
}

static void updateEditorPackageSummary(
    pf::FighterEditor& editor,
    const pf::FighterPackage& package,
    const std::vector<uint8_t>& bytes)
{
    editor.lastPackageName = package.name;
    editor.lastPackageBytes = bytes.size();
    editor.lastPackageChecksum = pf::fighterPackageChecksum(bytes);
    editor.lastPackageFighters = static_cast<int>(package.fighters.size());
    editor.lastPackageObjects = static_cast<int>(package.objects.size());
    editor.lastPackageAssets = 0;
    editor.lastPackageValid = true;
    editor.lastPackageMessage = "OK";
}

static void updateEditorPackageSummary(
    pf::FighterEditor& editor,
    const pf::FighterPackageDescriptor& descriptor)
{
    editor.lastPackageName = descriptor.name;
    editor.lastPackageBytes = descriptor.byteSize;
    editor.lastPackageChecksum = descriptor.checksum;
    editor.lastPackageFighters = static_cast<int>(descriptor.fighterNames.size());
    editor.lastPackageObjects = static_cast<int>(descriptor.objectNames.size());
    editor.lastPackageAssets = static_cast<int>(descriptor.legacyImportedAssetNames.size());
    editor.lastPackageValid = true;
    editor.lastPackageMessage = "OK";
}

static void updateEditorPackageFailure(pf::FighterEditor& editor, const std::string& message) {
    editor.lastPackageValid = false;
    editor.lastPackageMessage = message;
}

static bool materializeEditableNativeFighterData(pf::FighterDefinition& def) {
    bool materialized = false;
    if (def.authoredClips.empty() && def.authoredClipSource) {
        def.authoredClips = *def.authoredClipSource;
        def.authoredClipSource.reset();
        materialized = true;
    }
    if (def.authoredMesh.batches.empty() && def.authoredMesh.textures.empty() && def.authoredMeshSource) {
        def.authoredMesh = *def.authoredMeshSource;
        def.authoredMeshSource.reset();
        materialized = true;
    }
    if (def.modelPartAnimations.empty() && def.modelPartAnimationSource) {
        def.modelPartAnimations = *def.modelPartAnimationSource;
        def.modelPartAnimationSource.reset();
        materialized = true;
    }
    return materialized;
}

static void drawEditorAssetsWorkspace(pf::World& world, pf::FighterEditor& editor, int& selectedFighterDef) {
    editor.clampToWorld(world);
    if (world.fighters.empty()) {
        return;
    }
    pf::FighterRuntime& fighter = world.fighters[static_cast<size_t>(editor.selectedFighter)];
    if (fighter.fighterDef < 0 || fighter.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
        return;
    }
    pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    if (materializeEditableNativeFighterData(def)) {
        editor.status = "Editor: materialized native package asset data";
    }
    const Rectangle panel{12.0f, 324.0f, 530.0f, 330.0f};
    DrawRectangleRec(panel, Fade(RAYWHITE, 0.58f));
    DrawRectangleLinesEx(panel, 1.0f, DARKGRAY);
    DrawText("Package Assets", 24, 336, 16, BLACK);
    DrawText(("File: " + editor.packagePath).c_str(), 24, 360, 13, DARKGRAY);
    DrawText(("Native clips: " + std::to_string(def.authoredClips.size()) +
              " skeleton joints: " + std::to_string(def.authoredSkeleton.size())).c_str(), 24, 382, 13, DARKGRAY);
    DrawText(("Objects/articles in package: " + std::to_string(world.objectDefs.size())).c_str(), 24, 404, 13, DARKGRAY);
    DrawText(("Authored mesh batches: " + std::to_string(def.authoredMesh.batches.size())).c_str(), 24, 426, 13, DARKGRAY);
    if (editor.lastPackageBytes > 0) {
        DrawText(("Last package: " + editor.lastPackageName +
                  " bytes=" + std::to_string(editor.lastPackageBytes) +
                  " checksum=" + std::to_string(editor.lastPackageChecksum)).c_str(), 24, 448, 12, DARKGRAY);
        DrawText(("Contents: fighters=" + std::to_string(editor.lastPackageFighters) +
                  " objects=" + std::to_string(editor.lastPackageObjects) +
                  " legacyImportedAssets=" + std::to_string(editor.lastPackageAssets)).c_str(), 24, 466, 12, DARKGRAY);
    } else {
        DrawText("Last package: none saved or loaded", 24, 448, 12, GRAY);
    }
    if (!editor.lastPackageMessage.empty()) {
        std::string packageCheck = std::string("Check: ") +
            (editor.lastPackageValid ? "valid " : "invalid ") +
            editor.lastPackageMessage;
        while (!packageCheck.empty() && MeasureText(packageCheck.c_str(), 12) > 700) {
            packageCheck.pop_back();
        }
        DrawText(packageCheck.c_str(), 24, 484, 12, editor.lastPackageValid ? DARKGREEN : RED);
    }
    if (!world.objectDefs.empty()) {
        const int objectIndex = std::clamp(editor.selectedObjectDef, 0, static_cast<int>(world.objectDefs.size()) - 1);
        const pf::GameObjectDefinition& object = world.objectDefs[static_cast<size_t>(objectIndex)];
        if (object.packageVariables.empty()) {
            DrawText(("Obj life=" + std::to_string(object.lifetimeFrames) +
                      " grav=" + std::to_string(pf::fxToFloat(object.gravity)) +
                      " term=" + std::to_string(pf::fxToFloat(object.terminalVelocity)) +
                      " hp=" + std::to_string(pf::fxToFloat(object.maxDamage))).c_str(), 270, 402, 12, DARKGRAY);
        }
    }

    if (uiButton({338.0f, 338.0f, 82.0f, 26.0f}, "Save Pkg")) {
        pf::FighterPackage package = pf::makeEditorFighterPackage(world, fighter.fighterDef);
        std::string error;
        const std::vector<uint8_t> bytes = pf::writeFighterPackage(package, &error);
        if (!bytes.empty() && pf::saveFighterPackage(editor.packagePath, package, &error)) {
            updateEditorPackageSummary(editor, package, bytes);
            editor.status = "Editor: saved " + editor.packagePath +
                " bytes=" + std::to_string(bytes.size()) +
                " checksum=" + std::to_string(pf::fighterPackageChecksum(bytes));
        } else {
            updateEditorPackageFailure(editor, error);
            editor.status = "Editor package save failed: " + error;
        }
    }
    if (uiButton({522.0f, 338.0f, 82.0f, 26.0f}, "Check Pkg")) {
        pf::FighterPackage package = pf::makeEditorFighterPackage(world, fighter.fighterDef);
        std::string error;
        const std::vector<uint8_t> bytes = pf::writeFighterPackage(package, &error);
        if (!bytes.empty()) {
            updateEditorPackageSummary(editor, package, bytes);
            editor.status = "Editor: package validates bytes=" + std::to_string(bytes.size()) +
                " checksum=" + std::to_string(pf::fighterPackageChecksum(bytes));
        } else {
            updateEditorPackageFailure(editor, error);
            editor.status = "Editor package validation failed: " + error;
        }
    }
    if (uiButton({430.0f, 338.0f, 82.0f, 26.0f}, "Load Pkg")) {
        pf::FighterPackage package;
        std::string error;
        if (!pf::loadFighterPackage(editor.packagePath, package, &error)) {
            updateEditorPackageFailure(editor, error);
            editor.status = "Editor package load failed: " + error;
        } else if (package.fighters.empty()) {
            updateEditorPackageFailure(editor, "no fighters in package");
            editor.status = "Editor package load failed: no fighters in package";
        } else {
            std::string packageSummaryError;
            const std::vector<uint8_t> packageBytes = pf::writeFighterPackage(package, &packageSummaryError);
            if (!packageBytes.empty()) {
                updateEditorPackageSummary(editor, package, packageBytes);
            }
            const int fighterDef = fighter.fighterDef;
            const size_t fighterIndex = static_cast<size_t>(editor.selectedFighter);
            const pf::Vec2 position = fighter.position;
            const int facing = fighter.facing;
            world.fighterDefs[static_cast<size_t>(fighterDef)] = package.fighters.front();
            for (size_t packageFighterIndex = 1; packageFighterIndex < package.fighters.size(); ++packageFighterIndex) {
                const pf::FighterDefinition& packageFighter = package.fighters[packageFighterIndex];
                auto existing = std::find_if(world.fighterDefs.begin(), world.fighterDefs.end(), [&](const pf::FighterDefinition& candidate) {
                    return candidate.name == packageFighter.name;
                });
                if (existing == world.fighterDefs.end()) {
                    world.fighterDefs.push_back(packageFighter);
                } else {
                    *existing = packageFighter;
                }
            }
            world.objectDefs = package.objects;
            world.objects.clear();
            pf::resetTrainingFighter(world, fighterIndex, fighterDef, position, facing);
            selectedFighterDef = fighterDef;
            editor.selectedState = 0;
            editor.selectedSubaction = 0;
            editor.selectedPackageVariable = 0;
            editor.selectedPackageScript = 0;
            editor.selectedPackageInstruction = 0;
            editor.selectedObjectDef = 0;
            editor.selectedObjectState = 0;
            editor.selectedObjectHitbox = 0;
            editor.selectedObjectHurtbox = 0;
            editor.selectedObjectTouchbox = 0;
            editor.status = "Editor: loaded package fighter " + world.fighterDefs[static_cast<size_t>(fighterDef)].name +
                " objects=" + std::to_string(world.objectDefs.size());
        }
    }
    if (uiButton({338.0f, 370.0f, 82.0f, 26.0f}, "Blank")) {
        pf::FighterDefinition blank = makeEditorBlankFighterDefinition(
            uniqueFighterName(world, "BlankFighter"),
            def.properties.common);
        world.fighterDefs.push_back(std::move(blank));
        const int blankIndex = static_cast<int>(world.fighterDefs.size()) - 1;
        pf::resetTrainingFighter(world, static_cast<size_t>(editor.selectedFighter), blankIndex, fighter.position, fighter.facing);
        selectedFighterDef = blankIndex;
        editor.selectedState = 0;
        editor.selectedSubaction = 0;
        editor.selectedPackageVariable = 0;
        editor.selectedPackageScript = 0;
        editor.selectedPackageInstruction = 0;
        editor.status = "Editor: created blank runtime fighter " + world.fighterDefs.back().name;
    }
    if (uiButton({430.0f, 370.0f, 82.0f, 26.0f}, "Clone")) {
        pf::FighterDefinition clone = def;
        clone.name = uniqueFighterName(world, def.name + "Clone");
        world.fighterDefs.push_back(std::move(clone));
        const int cloneIndex = static_cast<int>(world.fighterDefs.size()) - 1;
        const pf::Vec2 position = fighter.position;
        const int facing = fighter.facing;
        pf::resetTrainingFighter(world, static_cast<size_t>(editor.selectedFighter), cloneIndex, position, facing);
        selectedFighterDef = cloneIndex;
        editor.selectedState = 0;
        editor.selectedSubaction = 0;
        editor.selectedPackageVariable = 0;
        editor.selectedPackageScript = 0;
        editor.selectedPackageInstruction = 0;
        editor.status = "Editor: cloned fighter into runtime slot " + world.fighterDefs.back().name;
    }
    if (uiButton({270.0f, 370.0f, 60.0f, 26.0f}, "TriMesh")) {
        ensureAuthoredRootJoint(def);
        def.authoredMesh = makeEditorTriangleMesh();
        editor.status = "Editor: seeded authored mesh triangle batch";
    }
    if (!def.authoredMesh.batches.empty()) {
        const pf::Vec2 meshSize = authoredMeshSize(def.authoredMesh);
        const int meshVerts = authoredMeshVertexCount(def.authoredMesh);
        editor.selectedAuthoredMeshVertex = std::clamp(
            editor.selectedAuthoredMeshVertex,
            0,
            std::max(0, meshVerts - 1));
        pf::FighterMeshVertex* selectedVertex = authoredMeshVertexAt(
            def.authoredMesh,
            editor.selectedAuthoredMeshVertex);
        const int meshBind = def.authoredMesh.batches.front().singleBindBone;
        const int meshParent = def.authoredMesh.batches.front().parentBone;
        DrawText(("Mesh verts=" + std::to_string(meshVerts) +
                  " v=" + std::to_string(editor.selectedAuthoredMeshVertex) +
                  " wt=" + (selectedVertex ? authoredMeshVertexInfluenceSummary(*selectedVertex) : std::string{"none"}) +
                  " inf=" + std::to_string(authoredMeshMaxInfluences(def.authoredMesh)) +
                  " size " + std::to_string(pf::fxToFloat(meshSize.x)) +
                  "," + std::to_string(pf::fxToFloat(meshSize.y)) +
                  " bind=" + std::to_string(meshBind) +
                  " parent=" + std::to_string(meshParent)).c_str(), 270, 428, 12, DARKGRAY);
        if (uiButton({270.0f, 398.0f, 54.0f, 22.0f}, "W+")) {
            scaleAuthoredMesh(def.authoredMesh, pf::fxFromFloat(1.1f), pf::fx(1));
            editor.status = "Editor: widened authored mesh";
        }
        if (uiButton({330.0f, 398.0f, 54.0f, 22.0f}, "W-")) {
            scaleAuthoredMesh(def.authoredMesh, pf::fxFromFloat(0.9f), pf::fx(1));
            editor.status = "Editor: narrowed authored mesh";
        }
        if (uiButton({390.0f, 398.0f, 54.0f, 22.0f}, "H+")) {
            scaleAuthoredMesh(def.authoredMesh, pf::fx(1), pf::fxFromFloat(1.1f));
            editor.status = "Editor: stretched authored mesh";
        }
        if (uiButton({450.0f, 398.0f, 54.0f, 22.0f}, "H-")) {
            scaleAuthoredMesh(def.authoredMesh, pf::fx(1), pf::fxFromFloat(0.9f));
            editor.status = "Editor: shortened authored mesh";
        }
        if (uiButton({510.0f, 398.0f, 54.0f, 22.0f}, "Bind")) {
            ensureAuthoredRootJoint(def);
            const int joint = std::clamp(
                editor.selectedAnimationJoint,
                0,
                std::max(0, static_cast<int>(def.authoredSkeleton.size()) - 1));
            bindAuthoredMeshToJoint(def.authoredMesh, joint);
            editor.status = "Editor: bound authored mesh weights to selected joint";
        }
        if (uiButton({570.0f, 398.0f, 64.0f, 22.0f}, "AutoWt")) {
            ensureAuthoredRootJoint(def);
            autoWeightAuthoredMeshToSkeleton(def.authoredMesh, def.authoredSkeleton);
            editor.status = "Editor: generated authored mesh skin weights from skeleton";
        }
        if (selectedVertex) {
            if (uiButton({642.0f, 398.0f, 34.0f, 22.0f}, "V-")) {
                editor.selectedAuthoredMeshVertex = wrappedIndex(editor.selectedAuthoredMeshVertex - 1, meshVerts);
                editor.status = "Editor: selected authored mesh vertex " + std::to_string(editor.selectedAuthoredMeshVertex);
            }
            if (uiButton({680.0f, 398.0f, 34.0f, 22.0f}, "V+")) {
                editor.selectedAuthoredMeshVertex = wrappedIndex(editor.selectedAuthoredMeshVertex + 1, meshVerts);
                editor.status = "Editor: selected authored mesh vertex " + std::to_string(editor.selectedAuthoredMeshVertex);
            }
            if (uiButton({718.0f, 398.0f, 34.0f, 22.0f}, "X-")) {
                nudgeAuthoredMeshVertex(def.authoredMesh, editor.selectedAuthoredMeshVertex, {-pf::fxFromFloat(0.05f), 0, 0});
                editor.status = "Editor: nudged authored mesh vertex left";
            }
            if (uiButton({756.0f, 398.0f, 34.0f, 22.0f}, "X+")) {
                nudgeAuthoredMeshVertex(def.authoredMesh, editor.selectedAuthoredMeshVertex, {pf::fxFromFloat(0.05f), 0, 0});
                editor.status = "Editor: nudged authored mesh vertex right";
            }
            if (uiButton({794.0f, 398.0f, 34.0f, 22.0f}, "Y-")) {
                nudgeAuthoredMeshVertex(def.authoredMesh, editor.selectedAuthoredMeshVertex, {0, -pf::fxFromFloat(0.05f), 0});
                editor.status = "Editor: lowered authored mesh vertex";
            }
            if (uiButton({832.0f, 398.0f, 34.0f, 22.0f}, "Y+")) {
                nudgeAuthoredMeshVertex(def.authoredMesh, editor.selectedAuthoredMeshVertex, {0, pf::fxFromFloat(0.05f), 0});
                editor.status = "Editor: raised authored mesh vertex";
            }
            if (uiButton({870.0f, 398.0f, 42.0f, 22.0f}, "Wt1")) {
                ensureAuthoredRootJoint(def);
                const int joint = std::clamp(
                    editor.selectedAnimationJoint,
                    0,
                    std::max(0, static_cast<int>(def.authoredSkeleton.size()) - 1));
                bindAuthoredMeshVertexToJoint(
                    def.authoredMesh,
                    editor.selectedAuthoredMeshVertex,
                    joint,
                    static_cast<int>(def.authoredSkeleton.size()));
                editor.status = "Editor: bound authored mesh vertex to selected joint";
            }
            if (uiButton({916.0f, 398.0f, 42.0f, 22.0f}, "Wt+")) {
                ensureAuthoredRootJoint(def);
                const int joint = std::clamp(
                    editor.selectedAnimationJoint,
                    0,
                    std::max(0, static_cast<int>(def.authoredSkeleton.size()) - 1));
                blendAuthoredMeshVertexTowardJoint(
                    def.authoredMesh,
                    editor.selectedAuthoredMeshVertex,
                    joint,
                    static_cast<int>(def.authoredSkeleton.size()),
                    0.25f);
                editor.status = "Editor: blended authored mesh vertex toward selected joint";
            }
        }
    }

    DrawText("Objects / Articles", 24, 436, 13, DARKGRAY);
    const int visibleObjects = std::min(3, static_cast<int>(world.objectDefs.size()));
    const int objectStart = std::clamp(
        editor.selectedObjectDef - visibleObjects / 2,
        0,
        std::max(0, static_cast<int>(world.objectDefs.size()) - visibleObjects));
    for (int row = 0; row < visibleObjects; ++row) {
        const int objectIndex = objectStart + row;
        const pf::GameObjectDefinition& object = world.objectDefs[static_cast<size_t>(objectIndex)];
        const std::string kind = object.kind == pf::GameObjectKind::Projectile ? "Proj" : "Item";
        const std::string label = kind + " " + object.name;
        if (uiListRow({24.0f, 456.0f + 24.0f * row, 230.0f, 22.0f}, label, objectIndex == editor.selectedObjectDef)) {
            editor.selectedObjectDef = objectIndex;
        }
    }
    if (world.objectDefs.empty()) {
        DrawText("No object definitions", 31, 461, 13, GRAY);
    }
    if (uiButton({270.0f, 456.0f, 76.0f, 24.0f}, "+ Item")) {
        world.objectDefs.push_back(makeEditorObjectDefinition(uniqueObjectName(world, "Item"), pf::GameObjectKind::Item));
        editor.selectedObjectDef = static_cast<int>(world.objectDefs.size()) - 1;
        editor.status = "Editor: added package item " + world.objectDefs.back().name;
    }
    if (uiButton({354.0f, 456.0f, 76.0f, 24.0f}, "+ Proj")) {
        world.objectDefs.push_back(makeEditorObjectDefinition(uniqueObjectName(world, "Projectile"), pf::GameObjectKind::Projectile));
        editor.selectedObjectDef = static_cast<int>(world.objectDefs.size()) - 1;
        editor.status = "Editor: added package projectile " + world.objectDefs.back().name;
    }
    if (uiButton({606.0f, 456.0f, 76.0f, 24.0f}, "Clone")) {
        if (!world.objectDefs.empty()) {
            const int sourceIndex = std::clamp(editor.selectedObjectDef, 0, static_cast<int>(world.objectDefs.size()) - 1);
            pf::GameObjectDefinition clone = world.objectDefs[static_cast<size_t>(sourceIndex)];
            clone.name = uniqueObjectName(world, clone.name + "Copy");
            world.objectDefs.push_back(std::move(clone));
            editor.selectedObjectDef = static_cast<int>(world.objectDefs.size()) - 1;
            editor.selectedObjectState = 0;
            editor.selectedObjectHitbox = 0;
            editor.selectedObjectHurtbox = 0;
            editor.selectedObjectTouchbox = 0;
            editor.selectedPackageVariable = 0;
            editor.selectedPackageScript = 0;
            editor.selectedPackageInstruction = 0;
            editor.status = "Editor: cloned package object " + world.objectDefs.back().name;
        }
    }
    if (uiButton({522.0f, 456.0f, 76.0f, 24.0f}, "- Obj")) {
        if (!world.objectDefs.empty()) {
            const int removeIndex = std::clamp(editor.selectedObjectDef, 0, static_cast<int>(world.objectDefs.size()) - 1);
            const std::string removedObjectName = world.objectDefs[static_cast<size_t>(removeIndex)].name;
            world.objectDefs.erase(world.objectDefs.begin() + removeIndex);
            if (world.objectDefs.empty()) {
                editor.selectedObjectDef = 0;
            } else {
                editor.selectedObjectDef = std::clamp(removeIndex, 0, static_cast<int>(world.objectDefs.size()) - 1);
            }
            const std::string replacementObjectName = world.objectDefs.empty()
                ? std::string{}
                : world.objectDefs[static_cast<size_t>(editor.selectedObjectDef)].name;
            remapRemovedPackageObjectTargets(world, removedObjectName, replacementObjectName);
            for (pf::GameObjectRuntime& runtime : world.objects) {
                if (runtime.objectDef == removeIndex) {
                    runtime.active = false;
                } else if (runtime.objectDef > removeIndex) {
                    --runtime.objectDef;
                }
            }
            editor.selectedObjectState = 0;
            editor.selectedPackageVariable = 0;
            editor.selectedPackageScript = 0;
            editor.selectedPackageInstruction = 0;
            editor.status = "Editor: removed package object " + removedObjectName;
            return;
        }
    }
    if (uiButton({438.0f, 456.0f, 76.0f, 24.0f}, "Spawn")) {
        if (!world.objectDefs.empty()) {
            editor.selectedObjectDef = std::clamp(editor.selectedObjectDef, 0, static_cast<int>(world.objectDefs.size()) - 1);
            const pf::GameObjectDefinition& object = world.objectDefs[static_cast<size_t>(editor.selectedObjectDef)];
            pf::Subaction spawn;
            spawn.type = object.kind == pf::GameObjectKind::Projectile
                ? pf::SubactionType::SpawnProjectile
                : pf::SubactionType::SpawnObject;
            spawn.frames = 1;
            spawn.objectName = object.name;
            spawn.spawnOffset = {pf::fxFromFloat(0.75f), pf::fxFromFloat(0.7f), 0};
            spawn.spawnVelocity = {pf::fxFromFloat(1.0f), pf::fxFromFloat(0.2f)};
            pf::FighterState& state = def.states[static_cast<size_t>(editor.selectedState)];
            state.action.push_back(std::move(spawn));
            editor.selectedSubaction = static_cast<int>(state.action.size()) - 1;
            editor.status = object.kind == pf::GameObjectKind::Projectile
                ? "Editor: added SpawnProjectile subaction for " + object.name
                : "Editor: added SpawnObject subaction for " + object.name;
        }
    }
    if (!world.objectDefs.empty()) {
        editor.selectedObjectDef = std::clamp(editor.selectedObjectDef, 0, static_cast<int>(world.objectDefs.size()) - 1);
        pf::GameObjectDefinition& object = world.objectDefs[static_cast<size_t>(editor.selectedObjectDef)];
        std::string renamedObject;
        if (uiTextField(
                {522.0f, 424.0f, 160.0f, 22.0f},
                "object-def-" + std::to_string(editor.selectedObjectDef),
                editor,
                object.name,
                renamedObject))
        {
            if (!objectNameAvailable(world, renamedObject, editor.selectedObjectDef)) {
                editor.status = "Editor: object name is empty or already used";
            } else {
                const std::string oldName = object.name;
                object.name = renamedObject;
                remapRemovedPackageObjectTargets(world, oldName, object.name);
                editor.status = "Editor: renamed package object " + oldName + " to " + object.name;
            }
        }
        editor.selectedPackageVariable = std::clamp(
            editor.selectedPackageVariable,
            0,
            std::max(0, static_cast<int>(object.packageVariables.size()) - 1));
        editor.selectedPackageScript = std::clamp(
            editor.selectedPackageScript,
            0,
            std::max(0, static_cast<int>(object.packageScripts.size()) - 1));
        editor.selectedObjectState = std::clamp(
            editor.selectedObjectState,
            0,
            std::max(0, static_cast<int>(object.states.size()) - 1));
        if (uiButton({270.0f, 424.0f, 36.0f, 22.0f}, "Lf-")) {
            object.lifetimeFrames = std::max(0, object.lifetimeFrames - 30);
            editor.status = "Editor: shortened object lifetime";
        }
        if (uiButton({310.0f, 424.0f, 36.0f, 22.0f}, "Lf+")) {
            object.lifetimeFrames += 30;
            editor.status = "Editor: lengthened object lifetime";
        }
        if (uiButton({350.0f, 424.0f, 36.0f, 22.0f}, "V-")) {
            object.terminalVelocity = std::max(pf::Fix{0}, object.terminalVelocity - pf::fxFromFloat(0.25f));
            editor.status = "Editor: decreased object terminal velocity";
        }
        if (uiButton({390.0f, 424.0f, 36.0f, 22.0f}, "V+")) {
            object.terminalVelocity += pf::fxFromFloat(0.25f);
            editor.status = "Editor: increased object terminal velocity";
        }
        if (uiButton({430.0f, 424.0f, 36.0f, 22.0f}, "HP-")) {
            object.maxDamage = std::max(pf::Fix{0}, object.maxDamage - pf::fx(1));
            editor.status = "Editor: decreased object max damage";
        }
        if (uiButton({470.0f, 424.0f, 36.0f, 22.0f}, "HP+")) {
            object.maxDamage += pf::fx(1);
            editor.status = "Editor: increased object max damage";
        }
        if (uiButton({510.0f, 424.0f, 36.0f, 22.0f}, "G-")) {
            object.gravity -= pf::fxFromFloat(0.02f);
            editor.status = "Editor: decreased object gravity";
        }
        if (uiButton({550.0f, 424.0f, 36.0f, 22.0f}, "G+")) {
            object.gravity += pf::fxFromFloat(0.02f);
            editor.status = "Editor: increased object gravity";
        }
        if (uiButton({590.0f, 424.0f, 44.0f, 22.0f}, "HitX", object.destroyOnHit)) {
            object.destroyOnHit = !object.destroyOnHit;
            editor.status = "Editor: toggled object destroy-on-hit";
        }
        if (uiButton({638.0f, 424.0f, 44.0f, 22.0f}, "ShdX", object.destroyOnShield)) {
            object.destroyOnShield = !object.destroyOnShield;
            editor.status = "Editor: toggled object destroy-on-shield";
        }
        if (uiButton({686.0f, 424.0f, 44.0f, 22.0f}, "Own", object.hitOwner)) {
            object.hitOwner = !object.hitOwner;
            editor.status = "Editor: toggled object owner hit permission";
        }
        if (!object.packageVariables.empty()) {
            pf::PackageVariableDefinition& variable = object.packageVariables[static_cast<size_t>(editor.selectedPackageVariable)];
            if (uiButton({270.0f, 398.0f, 58.0f, 22.0f}, "OInit-")) {
                --variable.initialValue;
                editor.status = "Editor: decreased initial value for object variable " + variable.name;
            }
            if (uiButton({332.0f, 398.0f, 58.0f, 22.0f}, "OInit+")) {
                ++variable.initialValue;
                editor.status = "Editor: increased initial value for object variable " + variable.name;
            }
            DrawText((variable.name + "=" + std::to_string(variable.initialValue)).c_str(), 398, 403, 12, DARKGRAY);
        }
        if (uiButton({522.0f, 486.0f, 58.0f, 24.0f}, "Logic", editor.objectPanel == pf::ObjectEditorPanel::Logic)) {
            editor.objectPanel = pf::ObjectEditorPanel::Logic;
            editor.status = "Editor: object logic panel";
        }
        if (uiButton({584.0f, 486.0f, 58.0f, 24.0f}, "Boxes", editor.objectPanel == pf::ObjectEditorPanel::Boxes)) {
            editor.objectPanel = pf::ObjectEditorPanel::Boxes;
            editor.status = "Editor: object box panel";
        }
        if (uiButton({646.0f, 486.0f, 58.0f, 24.0f}, "Kind")) {
            const std::string objectName = object.name;
            object.kind = object.kind == pf::GameObjectKind::Projectile
                ? pf::GameObjectKind::Item
                : pf::GameObjectKind::Projectile;
            if (object.kind == pf::GameObjectKind::Item) {
                demoteProjectileSpawnsForObject(world, objectName);
            }
            editor.status = object.kind == pf::GameObjectKind::Projectile
                ? "Editor: object is now a projectile"
                : "Editor: object is now an item";
        }

        if (!object.states.empty()) {
            pf::GameObjectStateDefinition& objectState = object.states[static_cast<size_t>(editor.selectedObjectState)];
            DrawText(("Object state: " + std::to_string(editor.selectedObjectState + 1) + "/" +
                      std::to_string(object.states.size()) + " " + objectState.name +
                      " len " + std::to_string(objectState.animationLengthFrames)).c_str(), 24, 532, 13, DARKGRAY);
            std::string renamedObjectState;
            if (uiTextField(
                    {24.0f, 546.0f, 160.0f, 24.0f},
                    "object-state-" + std::to_string(editor.selectedObjectDef) + "-" + std::to_string(editor.selectedObjectState),
                    editor,
                    objectState.name,
                    renamedObjectState))
            {
                if (!objectStateNameAvailable(object, renamedObjectState, editor.selectedObjectState)) {
                    editor.status = "Editor: object state name is empty or already used";
                } else {
                    const std::string oldName = objectState.name;
                    objectState.name = renamedObjectState;
                    remapRemovedObjectStateScriptTargets(object, oldName, objectState.name);
                    editor.status = "Editor: renamed object state " + oldName + " to " + objectState.name;
                }
            }
            if (uiButton({270.0f, 516.0f, 76.0f, 24.0f}, "+ State")) {
                pf::GameObjectStateDefinition state;
                state.name = uniqueObjectStateName(object);
                state.animationLengthFrames = 60;
                state.loopAnimation = true;
                state.onFrame = {{std::string{"object_lifetime"}}};
                object.states.push_back(std::move(state));
                editor.selectedObjectState = static_cast<int>(object.states.size()) - 1;
                editor.status = "Editor: added object state";
                return;
            }
            if (uiButton({354.0f, 516.0f, 76.0f, 24.0f}, "- State")) {
                if (object.states.size() > 1) {
                    const int removeIndex = std::clamp(editor.selectedObjectState, 0, static_cast<int>(object.states.size()) - 1);
                    const int oldInitialState = object.initialState;
                    const std::string removedStateName = object.states[static_cast<size_t>(removeIndex)].name;
                    object.states.erase(object.states.begin() + removeIndex);
                    editor.selectedObjectState = std::clamp(removeIndex, 0, static_cast<int>(object.states.size()) - 1);
                    if (oldInitialState == removeIndex) {
                        object.initialState = editor.selectedObjectState;
                    } else if (oldInitialState > removeIndex) {
                        object.initialState = oldInitialState - 1;
                    } else {
                        object.initialState = oldInitialState;
                    }
                    object.initialState = std::clamp(object.initialState, 0, static_cast<int>(object.states.size()) - 1);
                    const std::string& replacementStateName = object.states[static_cast<size_t>(editor.selectedObjectState)].name;
                    remapRemovedObjectStateScriptTargets(object, removedStateName, replacementStateName);
                    for (pf::GameObjectRuntime& runtime : world.objects) {
                        if (runtime.objectDef != editor.selectedObjectDef) {
                            continue;
                        }
                        if (runtime.state == removeIndex) {
                            runtime.state = editor.selectedObjectState;
                            runtime.lastStateChangeFrame = world.frame;
                            runtime.internalFrame = 0;
                            runtime.animationFrame = 0;
                        } else if (runtime.state > removeIndex) {
                            --runtime.state;
                        }
                    }
                    editor.status = "Editor: removed object state";
                    return;
                }
            }
            if (uiButton({438.0f, 516.0f, 76.0f, 24.0f}, "Initial")) {
                object.initialState = editor.selectedObjectState;
                editor.status = "Editor: selected object initial state";
            }
            if (uiButton({606.0f, 516.0f, 58.0f, 24.0f}, "Clone")) {
                const int sourceIndex = std::clamp(editor.selectedObjectState, 0, static_cast<int>(object.states.size()) - 1);
                pf::GameObjectStateDefinition clone = object.states[static_cast<size_t>(sourceIndex)];
                clone.name = uniqueObjectStateName(object, clone.name + "Copy");
                const int insertIndex = std::clamp(sourceIndex + 1, 0, static_cast<int>(object.states.size()));
                object.states.insert(object.states.begin() + insertIndex, std::move(clone));
                if (object.initialState >= insertIndex) {
                    ++object.initialState;
                }
                for (pf::GameObjectRuntime& runtime : world.objects) {
                    if (runtime.objectDef == editor.selectedObjectDef && runtime.state >= insertIndex) {
                        ++runtime.state;
                    }
                }
                editor.selectedObjectState = insertIndex;
                editor.status = "Editor: cloned object state " + object.states[static_cast<size_t>(insertIndex)].name;
                return;
            }
            if (uiButton({24.0f, 546.0f, 58.0f, 24.0f}, "Prev")) {
                editor.selectedObjectState = wrappedIndex(editor.selectedObjectState - 1, static_cast<int>(object.states.size()));
                editor.status = "Editor: selected previous object state";
            }
            if (uiButton({88.0f, 546.0f, 58.0f, 24.0f}, "Next")) {
                editor.selectedObjectState = wrappedIndex(editor.selectedObjectState + 1, static_cast<int>(object.states.size()));
                editor.status = "Editor: selected next object state";
            }
            if (uiButton({154.0f, 546.0f, 52.0f, 24.0f}, "Var>")) {
                if (!object.packageVariables.empty()) {
                    editor.selectedPackageVariable = wrappedIndex(
                        editor.selectedPackageVariable + 1,
                        static_cast<int>(object.packageVariables.size()));
                    editor.status = "Editor: selected next object package variable";
                }
            }
            if (uiButton({212.0f, 546.0f, 52.0f, 24.0f}, "Scr>")) {
                if (!object.packageScripts.empty()) {
                    editor.selectedPackageScript = wrappedIndex(
                        editor.selectedPackageScript + 1,
                        static_cast<int>(object.packageScripts.size()));
                    editor.selectedPackageInstruction = 0;
                    editor.status = "Editor: selected next object package script";
                }
            }
            if (uiButton({270.0f, 546.0f, 76.0f, 24.0f}, "Loop", objectState.loopAnimation)) {
                objectState.loopAnimation = !objectState.loopAnimation;
                editor.status = "Editor: toggled object state loop";
            }
            if (uiButton({354.0f, 546.0f, 76.0f, 24.0f}, "+ Len")) {
                ++objectState.animationLengthFrames;
                editor.status = "Editor: lengthened object state";
            }
            if (uiButton({438.0f, 546.0f, 76.0f, 24.0f}, "- Len")) {
                objectState.animationLengthFrames = std::max(1, objectState.animationLengthFrames - 1);
                editor.status = "Editor: shortened object state";
            }
            if (uiButton({522.0f, 516.0f, 58.0f, 24.0f}, "StCb>")) {
                editor.selectedObjectStateCallback = wrappedIndex(editor.selectedObjectStateCallback + 1, 4);
                editor.status = std::string("Editor: selected object ") + objectStateCallbackLabel(editor.selectedObjectStateCallback) + " callback";
            }
            if (uiButton({584.0f, 516.0f, 58.0f, 24.0f}, "-StCb")) {
                removeLastCallback(
                    objectStateCallbacks(objectState, editor.selectedObjectStateCallback),
                    objectStateCallbackLabel(editor.selectedObjectStateCallback),
                    editor);
            }
            if (uiButton({522.0f, 546.0f, 58.0f, 24.0f}, "Evt>")) {
                editor.selectedObjectEventCallback = wrappedIndex(editor.selectedObjectEventCallback + 1, 21);
                editor.status = std::string("Editor: selected object ") + objectEventCallbackLabel(editor.selectedObjectEventCallback) + " callback";
            }
            if (uiButton({584.0f, 546.0f, 58.0f, 24.0f}, "-Evt")) {
                removeLastCallback(
                    objectEventCallbacks(object, editor.selectedObjectEventCallback),
                    objectEventCallbackLabel(editor.selectedObjectEventCallback),
                    editor);
            }
        }

        if (editor.objectPanel == pf::ObjectEditorPanel::Logic) {
            if (!object.packageVariables.empty()) {
                editor.selectedPackageVariable = std::clamp(
                    editor.selectedPackageVariable,
                    0,
                    static_cast<int>(object.packageVariables.size()) - 1);
            }
            if (!object.packageScripts.empty()) {
                editor.selectedPackageScript = std::clamp(
                    editor.selectedPackageScript,
                    0,
                    static_cast<int>(object.packageScripts.size()) - 1);
            }
            DrawText(("Object logic: " + object.name + " vars " + std::to_string(object.packageVariables.size()) +
                      " scripts " + std::to_string(object.packageScripts.size())).c_str(), 24, 574, 13, DARKGRAY);
            if (!object.packageVariables.empty() || !object.packageScripts.empty()) {
                const std::string variableLabel = object.packageVariables.empty()
                    ? std::string{"var none"}
                    : "var " + object.packageVariables[static_cast<size_t>(editor.selectedPackageVariable)].name;
                const std::string scriptLabel = object.packageScripts.empty()
                    ? std::string{"script none"}
                    : "script " + object.packageScripts[static_cast<size_t>(editor.selectedPackageScript)].name;
                std::string logicSelection = variableLabel + "  " + scriptLabel;
                while (!logicSelection.empty() && MeasureText(logicSelection.c_str(), 12) > 430) {
                    logicSelection.pop_back();
                }
                DrawText(logicSelection.c_str(), 270, 574, 12, DARKGRAY);
            }
            if (!object.packageVariables.empty()) {
                pf::PackageVariableDefinition& variable = object.packageVariables[static_cast<size_t>(editor.selectedPackageVariable)];
                std::string renamedVariable;
                if (uiTextField(
                        {270.0f, 590.0f, 76.0f, 24.0f},
                        "object-var-" + std::to_string(editor.selectedObjectDef) + "-" + std::to_string(editor.selectedPackageVariable),
                        editor,
                        variable.name,
                        renamedVariable))
                {
                    if (!objectPackageVariableNameAvailable(object, renamedVariable, editor.selectedPackageVariable)) {
                        editor.status = "Editor: object package variable name is empty or already used";
                    } else {
                        const std::string oldName = variable.name;
                        variable.name = renamedVariable;
                        editor.status = "Editor: renamed object package variable " + oldName + " to " + variable.name;
                    }
                }
            }
            if (!object.packageScripts.empty()) {
                pf::PackageScript& script = object.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
                std::string renamedScript;
                if (uiTextField(
                        {270.0f, 620.0f, 76.0f, 24.0f},
                        "object-script-" + std::to_string(editor.selectedObjectDef) + "-" + std::to_string(editor.selectedPackageScript),
                        editor,
                        script.name,
                        renamedScript))
                {
                    if (!objectPackageScriptNameAvailable(object, renamedScript, editor.selectedPackageScript)) {
                        editor.status = "Editor: object package script name is empty or already used";
                    } else {
                        const std::string oldName = script.name;
                        script.name = renamedScript;
                        remapObjectPackageScriptRefs(object, oldName, script.name);
                        remapCrossObjectPackageScriptRefs(world, oldName, script.name);
                        editor.status = "Editor: renamed object package script " + oldName + " to " + script.name;
                    }
                }
            }
            if (!object.packageScripts.empty()) {
                pf::PackageScript& script = object.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
                editor.selectedPackageInstruction = std::clamp(
                    editor.selectedPackageInstruction,
                    0,
                    std::max(0, static_cast<int>(script.instructions.size()) - 1));
                drawPackageScriptBlockGraph(script, editor, {24.0f, 590.0f, 246.0f, 76.0f});
                if (uiButton({354.0f, 590.0f, 76.0f, 24.0f}, "+ Add")) {
                    if (!object.packageVariables.empty()) {
                        script.instructions.push_back({pf::PackageScriptOp::AddVarImmediate, editor.selectedPackageVariable, -1, -1, 1, 0, {}});
                        editor.selectedPackageInstruction = static_cast<int>(script.instructions.size()) - 1;
                        editor.status = "Editor: appended object AddVar instruction";
                    }
                }
                if (uiButton({438.0f, 590.0f, 76.0f, 24.0f}, "+ VelX")) {
                    script.instructions.push_back({pf::PackageScriptOp::SetAirVelocityX, -1, -1, -1, 0, pf::fxFromFloat(0.5f), {}});
                    editor.selectedPackageInstruction = static_cast<int>(script.instructions.size()) - 1;
                    editor.status = "Editor: appended object AirVelX instruction";
                }
                if (uiButton({354.0f, 620.0f, 76.0f, 24.0f}, "+ Spawn")) {
                    script.instructions.push_back({pf::PackageScriptOp::SpawnObject, -1, -1, -1, 0, pf::fxFromFloat(1.0f), object.name});
                    editor.selectedPackageInstruction = static_cast<int>(script.instructions.size()) - 1;
                    editor.status = "Editor: appended object SpawnObject instruction";
                }
                if (uiButton({606.0f, 560.0f, 58.0f, 24.0f}, "+Proj")) {
                    const std::string projectileName = packageObjectTargetName(world, editor.selectedObjectDef, pf::GameObjectKind::Projectile);
                    if (projectileName.empty()) {
                        editor.status = "Editor: add a projectile object before authoring object projectile spawns";
                    } else {
                        script.instructions.push_back({pf::PackageScriptOp::SpawnProjectile, -1, -1, -1, 0, pf::fxFromFloat(1.0f), projectileName});
                        editor.selectedPackageInstruction = static_cast<int>(script.instructions.size()) - 1;
                        editor.status = "Editor: appended object projectile spawn";
                    }
                }
                if (uiButton({666.0f, 560.0f, 58.0f, 24.0f}, "+Call")) {
                    script.instructions.push_back({
                        pf::PackageScriptOp::CallScript,
                        -1,
                        -1,
                        -1,
                        0,
                        0,
                        packageScriptTargetName(object.packageScripts, editor.selectedPackageScript),
                    });
                    editor.selectedPackageInstruction = static_cast<int>(script.instructions.size()) - 1;
                    editor.status = "Editor: appended object script call";
                }
                if (uiButton({606.0f, 620.0f, 58.0f, 24.0f}, "+ Kill")) {
                    script.instructions.push_back({pf::PackageScriptOp::DestroyObject, -1, -1, -1, 0, 0, {}});
                    editor.selectedPackageInstruction = static_cast<int>(script.instructions.size()) - 1;
                    editor.status = "Editor: appended object destroy instruction";
                }
                if (uiButton({666.0f, 620.0f, 58.0f, 24.0f}, "Blk<")) {
                    moveEditorPackageInstruction(script, editor, -1);
                }
                if (uiButton({438.0f, 620.0f, 76.0f, 24.0f}, "- Instr")) {
                    if (!script.instructions.empty()) {
                        script.instructions.erase(script.instructions.begin() + editor.selectedPackageInstruction);
                        editor.selectedPackageInstruction = std::clamp(editor.selectedPackageInstruction, 0, std::max(0, static_cast<int>(script.instructions.size()) - 1));
                        editor.status = "Editor: removed object script instruction";
                    }
                }
                if (uiButton({522.0f, 590.0f, 76.0f, 24.0f}, "+ If")) {
                    if (!object.packageVariables.empty()) {
                        script.instructions.push_back({pf::PackageScriptOp::SkipIfVarLessThanImmediate, editor.selectedPackageVariable, -1, -1, 1, 0, {}});
                        script.instructions.push_back({pf::PackageScriptOp::Nop, -1, -1, -1, 0, 0, {}});
                        editor.selectedPackageInstruction = static_cast<int>(script.instructions.size()) - 2;
                        editor.status = "Editor: appended object branch condition";
                    }
                }
                if (uiButton({522.0f, 620.0f, 76.0f, 24.0f}, "+ Jump")) {
                    script.instructions.push_back({pf::PackageScriptOp::JumpRelative, -1, -1, -1, 1, 0, {}});
                    editor.selectedPackageInstruction = static_cast<int>(script.instructions.size()) - 1;
                    editor.status = "Editor: appended object jump";
                }
                if (uiButton({522.0f, 650.0f, 76.0f, 24.0f}, "+ Fact")) {
                    if (!object.packageVariables.empty()) {
                        script.instructions.push_back({pf::PackageScriptOp::SetVarFrame, editor.selectedPackageVariable, -1, -1, 0, 0, {}});
                        editor.selectedPackageInstruction = static_cast<int>(script.instructions.size()) - 1;
                        editor.status = "Editor: appended object runtime fact read";
                    }
                }
                if (uiButton({606.0f, 590.0f, 58.0f, 24.0f}, "+ Ctx")) {
                    if (!object.packageVariables.empty()) {
                        script.instructions.push_back({pf::PackageScriptOp::SetVarObjectOwner, editor.selectedPackageVariable, -1, -1, 0, 0, {}});
                        editor.selectedPackageInstruction = static_cast<int>(script.instructions.size()) - 1;
                        editor.status = "Editor: appended object context read";
                    }
                }
                if (uiButton({666.0f, 590.0f, 58.0f, 24.0f}, "+ Own")) {
                    if (!object.packageVariables.empty()) {
                        script.instructions.push_back({pf::PackageScriptOp::SetVarFighterPercent, editor.selectedPackageVariable, -1, -1, 0, 0, {}});
                        editor.selectedPackageInstruction = static_cast<int>(script.instructions.size()) - 1;
                        editor.status = "Editor: appended owner fighter context read";
                    }
                }
                if (uiButton({354.0f, 650.0f, 76.0f, 24.0f}, "BindSp")) {
                    bindObjectPackageScriptCallback(object.onSpawned, script.name, "spawn", editor);
                }
                if (uiButton({438.0f, 650.0f, 76.0f, 24.0f}, "BindSt")) {
                    if (!object.states.empty()) {
                        bindObjectStatePackageScriptCallback(
                            object.states[static_cast<size_t>(editor.selectedObjectState)],
                            script.name,
                            editor);
                    }
                }
                if (uiButton({606.0f, 650.0f, 58.0f, 24.0f}, "BindEvt")) {
                    bindObjectEventPackageScriptCallback(object, script.name, editor);
                }
                if (uiButton({666.0f, 650.0f, 58.0f, 24.0f}, "Blk>")) {
                    moveEditorPackageInstruction(script, editor, 1);
                }
                if (!script.instructions.empty()) {
                    pf::PackageScriptInstruction& instruction = script.instructions[static_cast<size_t>(editor.selectedPackageInstruction)];
                    normalizeObjectPackageInstruction(instruction, object, world, editor.selectedObjectState, editor.selectedObjectDef);
                    if (uiButton({270.0f, 590.0f, 76.0f, 24.0f}, "Op")) {
                        instruction.op = nextObjectPackageScriptOp(instruction.op);
                        normalizeObjectPackageInstruction(instruction, object, world, editor.selectedObjectState, editor.selectedObjectDef);
                        editor.status = "Editor: cycled selected object script block op";
                    }
                    if (uiButton({606.0f, 680.0f, 58.0f, 22.0f}, "CtxOp")) {
                        instruction.op = packageScriptOpIsFighterContextRead(instruction.op)
                            ? nextFighterContextReadOp(instruction.op)
                            : nextObjectContextReadOp(instruction.op);
                        normalizeObjectPackageInstruction(instruction, object, world, editor.selectedObjectState, editor.selectedObjectDef);
                        editor.status = "Editor: cycled selected object context read";
                    }
                    if (uiButton({666.0f, 680.0f, 58.0f, 22.0f}, "OwnOp")) {
                        instruction.op = nextFighterContextReadOp(instruction.op);
                        normalizeObjectPackageInstruction(instruction, object, world, editor.selectedObjectState, editor.selectedObjectDef);
                        editor.status = "Editor: cycled selected owner fighter context read";
                    }
                    if (uiButton({270.0f, 620.0f, 76.0f, 24.0f}, "Dst")) {
                        if (!object.packageVariables.empty()) {
                            instruction.dst = (std::max(0, instruction.dst) + 1) % static_cast<int>(object.packageVariables.size());
                            editor.status = "Editor: cycled selected object script destination variable";
                        }
                    }
                    if (uiButton({270.0f, 650.0f, 76.0f, 24.0f}, "Val +")) {
                        ++instruction.intValue;
                        editor.status = "Editor: increased selected object script integer value";
                    }
                    if (uiButton({270.0f, 680.0f, 58.0f, 22.0f}, "Fix+")) {
                        instruction.fixValue += pf::fxFromFloat(0.1f);
                        editor.status = "Editor: increased selected object script fixed value";
                    }
                    if (uiButton({332.0f, 680.0f, 58.0f, 22.0f}, "State")) {
                        if (!object.states.empty()) {
                            instruction.text = object.states[static_cast<size_t>(editor.selectedObjectState)].name;
                            instruction.op = pf::PackageScriptOp::ChangeState;
                            editor.status = "Editor: targeted selected object state from script block";
                        }
                    }
                    if (uiButton({394.0f, 680.0f, 58.0f, 22.0f}, "Object")) {
                        instruction.text = object.name;
                        instruction.op = pf::PackageScriptOp::SpawnObject;
                        editor.status = "Editor: targeted selected object from object script block";
                    }
                    if (uiButton({518.0f, 680.0f, 58.0f, 22.0f}, "Proj")) {
                        const std::string projectileName = packageObjectTargetName(world, editor.selectedObjectDef, pf::GameObjectKind::Projectile);
                        if (projectileName.empty()) {
                            editor.status = "Editor: add a projectile object before targeting an object projectile";
                        } else {
                            instruction.text = projectileName;
                            instruction.op = pf::PackageScriptOp::SpawnProjectile;
                            editor.status = "Editor: targeted projectile from object script block";
                        }
                    }
                    if (uiButton({580.0f, 680.0f, 24.0f, 22.0f}, "Scr")) {
                        cyclePackageScriptTarget(instruction, object.packageScripts, editor.selectedPackageScript);
                        editor.status = "Editor: targeted object script call";
                    }
                    if (uiButton({456.0f, 680.0f, 58.0f, 22.0f}, "Val-")) {
                        --instruction.intValue;
                        editor.status = "Editor: decreased selected object script integer value";
                    }
                }
            } else {
                DrawText("No object script selected", 31, 590, 13, GRAY);
            }
            if (!object.states.empty()) {
                pf::GameObjectStateDefinition& objectState = object.states[static_cast<size_t>(editor.selectedObjectState)];
                DrawText(("State cb: " + std::string(objectStateCallbackLabel(editor.selectedObjectStateCallback)) +
                          " " + callbackSummary(objectStateCallbacks(objectState, editor.selectedObjectStateCallback))).c_str(), 24, 706, 12, DARKGRAY);
                DrawText(("Event cb: " + std::string(objectEventCallbackLabel(editor.selectedObjectEventCallback)) +
                          " " + callbackSummary(objectEventCallbacks(object, editor.selectedObjectEventCallback))).c_str(), 270, 706, 12, DARKGRAY);
            }
            if (uiButton({270.0f, 486.0f, 58.0f, 24.0f}, "+ OVar")) {
                object.packageVariables.push_back({uniqueObjectPackageVariableName(object), 0});
                editor.selectedPackageVariable = static_cast<int>(object.packageVariables.size()) - 1;
                for (pf::GameObjectRuntime& runtime : world.objects) {
                    if (runtime.objectDef == editor.selectedObjectDef) {
                        runtime.packageVars.push_back(object.packageVariables.back().initialValue);
                    }
                }
                editor.status = "Editor: added object package variable";
            }
            if (uiButton({332.0f, 486.0f, 58.0f, 24.0f}, "- OVar")) {
                if (!object.packageVariables.empty()) {
                    const int removedIndex = std::clamp(editor.selectedPackageVariable, 0, static_cast<int>(object.packageVariables.size()) - 1);
                    const std::string removed = object.packageVariables[static_cast<size_t>(removedIndex)].name;
                    object.packageVariables.erase(object.packageVariables.begin() + removedIndex);
                    remapRemovedPackageVariable(object.packageScripts, removedIndex, static_cast<int>(object.packageVariables.size()));
                    for (pf::GameObjectRuntime& runtime : world.objects) {
                        if (runtime.objectDef != editor.selectedObjectDef) {
                            continue;
                        }
                        if (removedIndex < static_cast<int>(runtime.packageVars.size())) {
                            runtime.packageVars.erase(runtime.packageVars.begin() + removedIndex);
                        }
                        runtime.packageVars.resize(object.packageVariables.size());
                    }
                    editor.selectedPackageVariable = std::clamp(editor.selectedPackageVariable, 0, std::max(0, static_cast<int>(object.packageVariables.size()) - 1));
                    editor.status = "Editor: removed object package variable " + removed;
                }
            }
            if (uiButton({394.0f, 486.0f, 58.0f, 24.0f}, "+ OScr")) {
                pf::PackageScript script;
                script.name = uniqueObjectPackageScriptName(object);
                script.instructionBudget = 64;
                object.packageScripts.push_back(std::move(script));
                editor.selectedPackageScript = static_cast<int>(object.packageScripts.size()) - 1;
                editor.selectedPackageInstruction = 0;
                editor.status = "Editor: added object package script";
            }
            if (uiButton({456.0f, 486.0f, 58.0f, 24.0f}, "- OScr")) {
                if (!object.packageScripts.empty()) {
                    const std::string removed = object.packageScripts[static_cast<size_t>(editor.selectedPackageScript)].name;
                    object.packageScripts.erase(object.packageScripts.begin() + editor.selectedPackageScript);
                    removeObjectPackageScriptRefs(object, removed);
                    removeCrossObjectPackageScriptRefs(world, removed);
                    editor.selectedPackageScript = std::clamp(editor.selectedPackageScript, 0, std::max(0, static_cast<int>(object.packageScripts.size()) - 1));
                    editor.selectedPackageInstruction = 0;
                    editor.status = "Editor: removed object package script " + removed;
                }
            }
            if (uiButton({708.0f, 486.0f, 58.0f, 24.0f}, "ClnS")) {
                if (!object.packageScripts.empty()) {
                    editor.selectedPackageScript = std::clamp(editor.selectedPackageScript, 0, static_cast<int>(object.packageScripts.size()) - 1);
                    pf::PackageScript clone = object.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
                    clone.name = uniqueObjectPackageScriptName(object, clone.name + "Copy");
                    object.packageScripts.push_back(std::move(clone));
                    editor.selectedPackageScript = static_cast<int>(object.packageScripts.size()) - 1;
                    editor.selectedPackageInstruction = 0;
                    editor.status = "Editor: cloned object package script " + object.packageScripts.back().name;
                }
            }
        }
        if (editor.objectPanel == pf::ObjectEditorPanel::Boxes) {
            editor.selectedObjectHitbox = std::clamp(editor.selectedObjectHitbox, 0, std::max(0, static_cast<int>(object.hitboxes.size()) - 1));
            editor.selectedObjectHurtbox = std::clamp(editor.selectedObjectHurtbox, 0, std::max(0, static_cast<int>(object.hurtboxes.size()) - 1));
            editor.selectedObjectTouchbox = std::clamp(editor.selectedObjectTouchbox, 0, std::max(0, static_cast<int>(object.touchboxes.size()) - 1));
            DrawText(("Object boxes: hit " + std::to_string(object.hitboxes.size()) +
                      " hurt " + std::to_string(object.hurtboxes.size()) +
                      " touch " + std::to_string(object.touchboxes.size())).c_str(), 24, 574, 13, DARKGRAY);
            if (uiButton({24.0f, 592.0f, 68.0f, 24.0f}, "+ Hit")) {
                pf::HitboxDefinition hitbox;
                hitbox.radius = pf::fxFromFloat(0.35f);
                hitbox.damage = pf::fxFromFloat(3.0f);
                hitbox.knockbackAngleDegrees = pf::fx(45);
                hitbox.knockbackBase = pf::fx(20);
                hitbox.knockbackGrowth = pf::fx(40);
                object.hitboxes.push_back(hitbox);
                editor.selectedObjectHitbox = static_cast<int>(object.hitboxes.size()) - 1;
                editor.status = "Editor: added object hitbox";
            }
            if (uiButton({98.0f, 592.0f, 68.0f, 24.0f}, "+ Hurt")) {
                object.hurtboxes.push_back({{}, {0, pf::fxFromFloat(0.7f), 0}, pf::fxFromFloat(0.35f), pf::HurtboxState::Normal});
                editor.selectedObjectHurtbox = static_cast<int>(object.hurtboxes.size()) - 1;
                editor.status = "Editor: added object hurtbox";
            }
            if (uiButton({172.0f, 592.0f, 68.0f, 24.0f}, "+ Touch")) {
                object.touchboxes.push_back({{}, {0, pf::fxFromFloat(0.8f), 0}, pf::fxFromFloat(0.45f), true, true});
                editor.selectedObjectTouchbox = static_cast<int>(object.touchboxes.size()) - 1;
                editor.status = "Editor: added object touchbox";
            }
            if (uiButton({24.0f, 622.0f, 68.0f, 24.0f}, "Hit>")) {
                if (!object.hitboxes.empty()) {
                    editor.selectedObjectHitbox = wrappedIndex(
                        editor.selectedObjectHitbox + 1,
                        static_cast<int>(object.hitboxes.size()));
                    editor.status = "Editor: selected next object hitbox";
                }
            }
            if (uiButton({98.0f, 622.0f, 68.0f, 24.0f}, "Hurt>")) {
                if (!object.hurtboxes.empty()) {
                    editor.selectedObjectHurtbox = wrappedIndex(
                        editor.selectedObjectHurtbox + 1,
                        static_cast<int>(object.hurtboxes.size()));
                    editor.status = "Editor: selected next object hurtbox";
                }
            }
            if (uiButton({172.0f, 622.0f, 68.0f, 24.0f}, "Touch>")) {
                if (!object.touchboxes.empty()) {
                    editor.selectedObjectTouchbox = wrappedIndex(
                        editor.selectedObjectTouchbox + 1,
                        static_cast<int>(object.touchboxes.size()));
                    editor.status = "Editor: selected next object touchbox";
                }
            }
            DrawText(("Selected: H" + std::to_string(editor.selectedObjectHitbox) +
                      " Hu" + std::to_string(editor.selectedObjectHurtbox) +
                      " T" + std::to_string(editor.selectedObjectTouchbox)).c_str(), 24, 650, 12, DARKGRAY);
            if (!object.hitboxes.empty()) {
                pf::HitboxDefinition& hitbox = object.hitboxes[static_cast<size_t>(editor.selectedObjectHitbox)];
                if (uiButton({270.0f, 592.0f, 76.0f, 24.0f}, "Dmg +")) {
                    hitbox.damage += pf::fx(1);
                    editor.status = "Editor: raised object hitbox damage";
                }
                if (uiButton({354.0f, 592.0f, 76.0f, 24.0f}, "Dmg -")) {
                    hitbox.damage = std::max(pf::Fix{0}, hitbox.damage - pf::fx(1));
                    editor.status = "Editor: lowered object hitbox damage";
                }
                if (uiButton({438.0f, 592.0f, 76.0f, 24.0f}, "- Hit")) {
                    object.hitboxes.erase(object.hitboxes.begin() + editor.selectedObjectHitbox);
                    editor.selectedObjectHitbox = std::clamp(editor.selectedObjectHitbox, 0, std::max(0, static_cast<int>(object.hitboxes.size()) - 1));
                    editor.status = "Editor: removed object hitbox";
                    return;
                }
                if (uiButton({270.0f, 622.0f, 76.0f, 24.0f}, "Hit +")) {
                    hitbox.radius += pf::fxFromFloat(0.05f);
                    editor.status = "Editor: enlarged object hitbox";
                }
                if (uiButton({354.0f, 622.0f, 76.0f, 24.0f}, "Hit -")) {
                    hitbox.radius = std::max(pf::fxFromFloat(0.05f), hitbox.radius - pf::fxFromFloat(0.05f));
                    editor.status = "Editor: shrank object hitbox";
                }
                if (uiButton({522.0f, 592.0f, 44.0f, 22.0f}, "A+")) {
                    hitbox.knockbackAngleDegrees += pf::fx(5);
                    editor.status = "Editor: raised object hitbox angle";
                }
                if (uiButton({572.0f, 592.0f, 44.0f, 22.0f}, "A-")) {
                    hitbox.knockbackAngleDegrees -= pf::fx(5);
                    editor.status = "Editor: lowered object hitbox angle";
                }
                if (uiButton({622.0f, 592.0f, 44.0f, 22.0f}, "B+")) {
                    hitbox.knockbackBase += pf::fx(5);
                    editor.status = "Editor: increased object hitbox base knockback";
                }
                if (uiButton({672.0f, 592.0f, 44.0f, 22.0f}, "K+")) {
                    hitbox.knockbackGrowth += pf::fx(5);
                    editor.status = "Editor: increased object hitbox growth";
                }
                if (uiButton({438.0f, 622.0f, 44.0f, 22.0f}, "X+")) {
                    hitbox.offset.x += pf::fxFromFloat(0.1f);
                    editor.status = "Editor: moved object hitbox forward";
                }
                if (uiButton({488.0f, 622.0f, 44.0f, 22.0f}, "X-")) {
                    hitbox.offset.x -= pf::fxFromFloat(0.1f);
                    editor.status = "Editor: moved object hitbox backward";
                }
                if (uiButton({538.0f, 622.0f, 44.0f, 22.0f}, "Y+")) {
                    hitbox.offset.y += pf::fxFromFloat(0.1f);
                    editor.status = "Editor: raised object hitbox";
                }
                if (uiButton({588.0f, 622.0f, 44.0f, 22.0f}, "Y-")) {
                    hitbox.offset.y -= pf::fxFromFloat(0.1f);
                    editor.status = "Editor: lowered object hitbox";
                }
                if (uiButton({638.0f, 622.0f, 36.0f, 22.0f}, "B-")) {
                    hitbox.knockbackBase = std::max(pf::Fix{0}, hitbox.knockbackBase - pf::fx(5));
                    editor.status = "Editor: decreased object hitbox base knockback";
                }
                if (uiButton({680.0f, 622.0f, 36.0f, 22.0f}, "K-")) {
                    hitbox.knockbackGrowth = std::max(pf::Fix{0}, hitbox.knockbackGrowth - pf::fx(5));
                    editor.status = "Editor: decreased object hitbox growth";
                }
                if (uiButton({522.0f, 652.0f, 44.0f, 22.0f}, "Gnd", hitbox.hitGrounded)) {
                    hitbox.hitGrounded = !hitbox.hitGrounded;
                    editor.status = "Editor: toggled object hitbox grounded target flag";
                }
                if (uiButton({572.0f, 652.0f, 44.0f, 22.0f}, "Air", hitbox.hitAirborne)) {
                    hitbox.hitAirborne = !hitbox.hitAirborne;
                    editor.status = "Editor: toggled object hitbox airborne target flag";
                }
            }
            if (!object.hurtboxes.empty()) {
                pf::GameObjectHurtboxDefinition& hurtbox = object.hurtboxes[static_cast<size_t>(editor.selectedObjectHurtbox)];
                if (uiButton({270.0f, 652.0f, 76.0f, 24.0f}, "Hurt +")) {
                    hurtbox.radius += pf::fxFromFloat(0.05f);
                    editor.status = "Editor: enlarged object hurtbox";
                }
                if (uiButton({354.0f, 652.0f, 76.0f, 24.0f}, "Hurt -")) {
                    hurtbox.radius = std::max(pf::fxFromFloat(0.05f), hurtbox.radius - pf::fxFromFloat(0.05f));
                    editor.status = "Editor: shrank object hurtbox";
                }
                if (uiButton({438.0f, 652.0f, 76.0f, 24.0f}, "- Hurt")) {
                    object.hurtboxes.erase(object.hurtboxes.begin() + editor.selectedObjectHurtbox);
                    editor.selectedObjectHurtbox = std::clamp(editor.selectedObjectHurtbox, 0, std::max(0, static_cast<int>(object.hurtboxes.size()) - 1));
                    editor.status = "Editor: removed object hurtbox";
                    return;
                }
                if (uiButton({24.0f, 672.0f, 58.0f, 22.0f}, "HState")) {
                    hurtbox.state = nextHurtboxState(hurtbox.state);
                    editor.status = std::string("Editor: set object hurtbox state ") + hurtboxStateName(hurtbox.state);
                }
                if (uiButton({86.0f, 672.0f, 36.0f, 22.0f}, "HX+")) {
                    hurtbox.startOffset.x += pf::fxFromFloat(0.1f);
                    hurtbox.endOffset.x += pf::fxFromFloat(0.1f);
                    editor.status = "Editor: moved object hurtbox forward";
                }
                if (uiButton({126.0f, 672.0f, 36.0f, 22.0f}, "HX-")) {
                    hurtbox.startOffset.x -= pf::fxFromFloat(0.1f);
                    hurtbox.endOffset.x -= pf::fxFromFloat(0.1f);
                    editor.status = "Editor: moved object hurtbox backward";
                }
                if (uiButton({166.0f, 672.0f, 36.0f, 22.0f}, "HY+")) {
                    hurtbox.startOffset.y += pf::fxFromFloat(0.1f);
                    hurtbox.endOffset.y += pf::fxFromFloat(0.1f);
                    editor.status = "Editor: raised object hurtbox";
                }
                if (uiButton({206.0f, 672.0f, 36.0f, 22.0f}, "HY-")) {
                    hurtbox.startOffset.y -= pf::fxFromFloat(0.1f);
                    hurtbox.endOffset.y -= pf::fxFromFloat(0.1f);
                    editor.status = "Editor: lowered object hurtbox";
                }
            }
            if (!object.touchboxes.empty()) {
                pf::GameObjectTouchboxDefinition& touchbox = object.touchboxes[static_cast<size_t>(editor.selectedObjectTouchbox)];
                if (uiButton({270.0f, 682.0f, 76.0f, 24.0f}, "Touch +")) {
                    touchbox.radius += pf::fxFromFloat(0.05f);
                    editor.status = "Editor: enlarged object touchbox";
                }
                if (uiButton({354.0f, 682.0f, 76.0f, 24.0f}, "Touch -")) {
                    touchbox.radius = std::max(pf::fxFromFloat(0.05f), touchbox.radius - pf::fxFromFloat(0.05f));
                    editor.status = "Editor: shrank object touchbox";
                }
                if (uiButton({438.0f, 682.0f, 76.0f, 24.0f}, "- Touch")) {
                    object.touchboxes.erase(object.touchboxes.begin() + editor.selectedObjectTouchbox);
                    editor.selectedObjectTouchbox = std::clamp(editor.selectedObjectTouchbox, 0, std::max(0, static_cast<int>(object.touchboxes.size()) - 1));
                    editor.status = "Editor: removed object touchbox";
                    return;
                }
                if (uiButton({522.0f, 682.0f, 44.0f, 22.0f}, "Ftr", touchbox.touchFighters)) {
                    touchbox.touchFighters = !touchbox.touchFighters;
                    editor.status = "Editor: toggled touchbox fighter contact";
                }
                if (uiButton({572.0f, 682.0f, 44.0f, 22.0f}, "Obj", touchbox.touchObjects)) {
                    touchbox.touchObjects = !touchbox.touchObjects;
                    editor.status = "Editor: toggled touchbox object contact";
                }
                if (uiButton({622.0f, 682.0f, 44.0f, 22.0f}, "TX+")) {
                    touchbox.startOffset.x += pf::fxFromFloat(0.1f);
                    touchbox.endOffset.x += pf::fxFromFloat(0.1f);
                    editor.status = "Editor: moved object touchbox forward";
                }
                if (uiButton({672.0f, 682.0f, 44.0f, 22.0f}, "TX-")) {
                    touchbox.startOffset.x -= pf::fxFromFloat(0.1f);
                    touchbox.endOffset.x -= pf::fxFromFloat(0.1f);
                    editor.status = "Editor: moved object touchbox backward";
                }
            }
        }
    }
}

static void drawEditorAnimationWorkspace(pf::World& world, pf::FighterEditor& editor) {
    editor.clampToWorld(world);
    if (world.fighters.empty()) {
        return;
    }
    pf::FighterRuntime& fighter = world.fighters[static_cast<size_t>(editor.selectedFighter)];
    if (fighter.fighterDef < 0 || fighter.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
        return;
    }
    pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const Rectangle panel{12.0f, 324.0f, 530.0f, 330.0f};
    DrawRectangleRec(panel, Fade(RAYWHITE, 0.58f));
    DrawRectangleLinesEx(panel, 1.0f, DARKGRAY);
    DrawText("Animation Preview", 24, 336, 16, BLACK);
    if (materializeEditableNativeFighterData(def)) {
        editor.selectedAnimationClip = 0;
        editor.status = "Editor: materialized native package animation data";
    }
    std::vector<pf::AnimationClip>* authoredClips = &def.authoredClips;
    const std::vector<pf::AnimationClip>* clips = authoredClips;
    const bool showingAuthoredClips = true;
    if (clips->empty()) {
        DrawText("No animation clips on this fighter", 24, 364, 13, GRAY);
        if (uiButton({352.0f, 362.0f, 72.0f, 24.0f}, "+ Clip")) {
            pf::AnimationClip clip;
            clip.name = "Wait";
            clip.actionIndex = 0;
            clip.frameCount = pf::fx(60);
            ensureAuthoredRootJoint(def);
            def.authoredClips.push_back(std::move(clip));
            editor.selectedAnimationClip = 0;
            editor.status = "Editor: created authored animation clip";
        }
        return;
    }

    editor.selectedAnimationClip = std::clamp(
        editor.selectedAnimationClip,
        0,
        static_cast<int>(clips->size()) - 1);
    pf::AnimationClip* selectedAuthoredClip = (!showingAuthoredClips || def.authoredClips.empty())
        ? nullptr
        : &def.authoredClips[static_cast<size_t>(std::clamp(editor.selectedAnimationClip, 0, static_cast<int>(def.authoredClips.size()) - 1))];
    const pf::AnimationClip& selectedClip = (*clips)[static_cast<size_t>(editor.selectedAnimationClip)];
    const int maxFrame = std::max(0, static_cast<int>(pf::fxToFloat(selectedClip.frameCount)) - 1);
    editor.animationScrubFrame = std::clamp(editor.animationScrubFrame, 0, maxFrame);

    DrawText(("Clips: " + std::to_string(clips->size()) + " authored").c_str(), 24, 362, 13, DARKGRAY);
    DrawText(("Selected: action " + std::to_string(selectedClip.actionIndex) +
              " frame " + std::to_string(editor.animationScrubFrame) + "/" + std::to_string(maxFrame)).c_str(), 24, 382, 13, DARKGRAY);
    if (showingAuthoredClips && selectedAuthoredClip) {
        std::string renamedClip;
        if (uiTextField(
                {224.0f, 378.0f, 110.0f, 22.0f},
                "anim-clip-" + std::to_string(editor.selectedAnimationClip),
                editor,
                selectedAuthoredClip->name,
                renamedClip))
        {
            if (!authoredClipNameAvailable(def, renamedClip, editor.selectedAnimationClip)) {
                editor.status = "Editor: authored clip name is empty or already used";
            } else {
                const std::string oldName = selectedAuthoredClip->name;
                selectedAuthoredClip->name = renamedClip;
                for (pf::FighterState& fighterState : def.states) {
                    if (fighterState.animation == oldName) {
                        fighterState.animation = selectedAuthoredClip->name;
                    }
                }
                editor.status = "Editor: renamed authored clip " + oldName + " to " + selectedAuthoredClip->name;
            }
        }
    }
    DrawText("Clips", 24, 396, 13, DARKGRAY);
    const int visibleClips = std::min(4, static_cast<int>(clips->size()));
    const int clipStart = visibleListStart(
        editor.selectedAnimationClip,
        static_cast<int>(clips->size()),
        visibleClips);
    for (int row = 0; row < visibleClips; ++row) {
        const int clipIndex = clipStart + row;
        const pf::AnimationClip& clip = (*clips)[static_cast<size_t>(clipIndex)];
        const std::string label = std::to_string(clip.actionIndex) + " " + clip.name;
        if (uiListRow({24.0f, 414.0f + 24.0f * row, 150.0f, 22.0f}, label, clipIndex == editor.selectedAnimationClip)) {
            editor.selectedAnimationClip = clipIndex;
            editor.animationScrubFrame = 0;
            editor.selectedAnimationTrack = 0;
            editor.selectedAnimationKey = 0;
            editor.animationPreviewActive = true;
            const bool ok = pf::previewFighterAnimation(world, static_cast<size_t>(editor.selectedFighter), clip.actionIndex, 0);
            editor.status = ok ? "Editor: previewing animation " + clip.name : "Editor: animation preview failed";
            editor.paused = true;
        }
    }

    if (showingAuthoredClips) {
        DrawText(("Joints: " + std::to_string(def.authoredSkeleton.size())).c_str(), 184, 396, 13, DARKGRAY);
        if (!def.authoredSkeleton.empty()) {
            const pf::AnimationJoint& joint = def.authoredSkeleton[static_cast<size_t>(std::clamp(
                editor.selectedAnimationJoint,
                0,
                static_cast<int>(def.authoredSkeleton.size()) - 1))];
            DrawText(("Joint scale " + std::to_string(pf::fxToFloat(joint.scale.x)) +
                      "," + std::to_string(pf::fxToFloat(joint.scale.y)) +
                      "," + std::to_string(pf::fxToFloat(joint.scale.z))).c_str(), 352, 396, 13, DARKGRAY);
            DrawText(("Joint rot " + std::to_string(pf::fxToFloat(joint.rotation.x)) +
                      "," + std::to_string(pf::fxToFloat(joint.rotation.y)) +
                      "," + std::to_string(pf::fxToFloat(joint.rotation.z))).c_str(), 352, 414, 13, DARKGRAY);
        }
        const int visibleJoints = std::min(4, static_cast<int>(def.authoredSkeleton.size()));
        const int jointStart = visibleListStart(
            editor.selectedAnimationJoint,
            static_cast<int>(def.authoredSkeleton.size()),
            visibleJoints);
        for (int row = 0; row < visibleJoints; ++row) {
            const int jointIndex = jointStart + row;
            const pf::AnimationJoint& joint = def.authoredSkeleton[static_cast<size_t>(jointIndex)];
            const std::string label = std::to_string(jointIndex) + " p" + std::to_string(joint.parent) + " " + joint.name;
            if (uiListRow({184.0f, 414.0f + 24.0f * row, 150.0f, 22.0f}, label, jointIndex == editor.selectedAnimationJoint)) {
                editor.selectedAnimationJoint = jointIndex;
                editor.status = "Editor: selected authored joint " + joint.name;
            }
        }
        if (!def.authoredSkeleton.empty()) {
            editor.selectedAnimationJoint = std::clamp(
                editor.selectedAnimationJoint,
                0,
                static_cast<int>(def.authoredSkeleton.size()) - 1);
            pf::AnimationJoint& selectedJoint = def.authoredSkeleton[static_cast<size_t>(editor.selectedAnimationJoint)];
            std::string renamedJoint;
            if (uiTextField(
                    {516.0f, 470.0f, 150.0f, 22.0f},
                    "anim-joint-" + std::to_string(editor.selectedAnimationJoint),
                    editor,
                    selectedJoint.name,
                    renamedJoint))
            {
                if (!authoredJointNameAvailable(def, renamedJoint, editor.selectedAnimationJoint)) {
                    editor.status = "Editor: authored joint name is empty or already used";
                } else {
                    const std::string oldName = selectedJoint.name;
                    selectedJoint.name = renamedJoint;
                    editor.status = "Editor: renamed authored joint " + oldName + " to " + selectedJoint.name;
                }
            }
        }
    } else {
        DrawText("Imported clips are read-only here", 184, 396, 13, GRAY);
    }

    if (uiButton({352.0f, 410.0f, 72.0f, 24.0f}, "- Frame")) {
        --editor.animationScrubFrame;
        editor.animationPreviewActive = true;
        editor.paused = true;
    }
    if (uiButton({434.0f, 410.0f, 72.0f, 24.0f}, "+ Frame")) {
        ++editor.animationScrubFrame;
        editor.animationPreviewActive = true;
        editor.paused = true;
    }
    if (uiButton({352.0f, 440.0f, 72.0f, 24.0f}, "Apply")) {
        editor.animationPreviewActive = true;
        editor.paused = true;
    }
    if (showingAuthoredClips && selectedAuthoredClip && !selectedAuthoredClip->tracks.empty() &&
        uiButton({352.0f, 470.0f, 72.0f, 24.0f}, "Trk +")) {
        editor.selectedAnimationTrack = (editor.selectedAnimationTrack + 1) % static_cast<int>(selectedAuthoredClip->tracks.size());
        editor.selectedAnimationKey = 0;
        editor.status = "Editor: selected next authored animation track";
    }
    if (showingAuthoredClips && selectedAuthoredClip && !selectedAuthoredClip->tracks.empty()) {
        const pf::AnimationTrack& track = selectedAuthoredClip->tracks[static_cast<size_t>(std::clamp(
            editor.selectedAnimationTrack,
            0,
            static_cast<int>(selectedAuthoredClip->tracks.size()) - 1))];
        if (!track.keys.empty() && uiButton({434.0f, 470.0f, 72.0f, 24.0f}, "Key +")) {
            editor.selectedAnimationKey = (editor.selectedAnimationKey + 1) % static_cast<int>(track.keys.size());
            editor.animationScrubFrame = static_cast<int>(pf::fxToFloat(track.keys[static_cast<size_t>(editor.selectedAnimationKey)].frame));
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: selected next authored animation key";
        }
    }
    if (showingAuthoredClips && selectedAuthoredClip && uiButton({24.0f, 486.0f, 72.0f, 24.0f}, "Del Clip")) {
        if (def.authoredClips.size() > 1) {
            const int removeIndex = std::clamp(editor.selectedAnimationClip, 0, static_cast<int>(def.authoredClips.size()) - 1);
            const std::string removedName = def.authoredClips[static_cast<size_t>(removeIndex)].name;
            const int removedAction = def.authoredClips[static_cast<size_t>(removeIndex)].actionIndex;
            def.authoredClips.erase(def.authoredClips.begin() + removeIndex);
            editor.selectedAnimationClip = std::clamp(removeIndex, 0, static_cast<int>(def.authoredClips.size()) - 1);
            const pf::AnimationClip& fallback = def.authoredClips[static_cast<size_t>(editor.selectedAnimationClip)];
            for (pf::FighterState& fighterState : def.states) {
                if (fighterState.animation == removedName || fighterState.animationActionIndex == removedAction) {
                    fighterState.animation = fallback.name;
                    fighterState.animationActionIndex = fallback.actionIndex;
                    fighterState.animationLengthFrames = std::max(1, static_cast<int>(pf::fxToFloat(fallback.frameCount)));
                }
            }
            editor.selectedAnimationTrack = 0;
            editor.selectedAnimationKey = 0;
            editor.animationScrubFrame = 0;
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: deleted authored animation clip " + removedName;
            return;
        }
        editor.status = "Editor: cannot delete the only authored clip";
    }
    if (showingAuthoredClips && uiButton({24.0f, 516.0f, 72.0f, 24.0f}, "+ Clip")) {
        pf::AnimationClip clip;
        clip.name = uniqueAuthoredClipName(def);
        clip.actionIndex = uniqueAuthoredClipActionIndex(def);
        clip.frameCount = pf::fx(60);
        def.authoredClips.push_back(std::move(clip));
        editor.selectedAnimationClip = static_cast<int>(def.authoredClips.size()) - 1;
        editor.selectedAnimationTrack = 0;
        editor.selectedAnimationKey = 0;
        editor.animationScrubFrame = 0;
        editor.status = "Editor: added authored animation clip";
        return;
    }
    if (showingAuthoredClips && uiButton({102.0f, 516.0f, 72.0f, 24.0f}, "+ Len")) {
        pf::AnimationClip& clip = *selectedAuthoredClip;
        clip.frameCount += pf::fx(5);
        editor.status = "Editor: lengthened authored animation clip";
    }
    if (showingAuthoredClips && uiButton({102.0f, 546.0f, 72.0f, 24.0f}, "- Len")) {
        pf::AnimationClip& clip = *selectedAuthoredClip;
        clip.frameCount = std::max(pf::fx(1), clip.frameCount - pf::fx(5));
        editor.animationScrubFrame = std::clamp(editor.animationScrubFrame, 0, std::max(0, static_cast<int>(pf::fxToFloat(clip.frameCount)) - 1));
        editor.status = "Editor: shortened authored animation clip";
    }
    if (showingAuthoredClips && selectedAuthoredClip && uiButton({102.0f, 576.0f, 72.0f, 24.0f}, "Act+")) {
        const int oldAction = selectedAuthoredClip->actionIndex;
        const int newAction = nextAuthoredClipActionIndex(def, oldAction, 1, editor.selectedAnimationClip);
        selectedAuthoredClip->actionIndex = newAction;
        for (pf::FighterState& fighterState : def.states) {
            if (fighterState.animationActionIndex == oldAction ||
                fighterState.animation == selectedAuthoredClip->name)
            {
                fighterState.animationActionIndex = newAction;
            }
        }
        editor.status = "Editor: raised authored clip action index to " + std::to_string(newAction);
    }
    if (showingAuthoredClips && selectedAuthoredClip && uiButton({184.0f, 576.0f, 72.0f, 24.0f}, "Act-")) {
        const int oldAction = selectedAuthoredClip->actionIndex;
        const int newAction = nextAuthoredClipActionIndex(def, oldAction, -1, editor.selectedAnimationClip);
        selectedAuthoredClip->actionIndex = newAction;
        for (pf::FighterState& fighterState : def.states) {
            if (fighterState.animationActionIndex == oldAction ||
                fighterState.animation == selectedAuthoredClip->name)
            {
                fighterState.animationActionIndex = newAction;
            }
        }
        editor.status = "Editor: lowered authored clip action index to " + std::to_string(newAction);
    }
    if (showingAuthoredClips && uiButton({184.0f, 516.0f, 72.0f, 24.0f}, "+ Joint")) {
        ensureAuthoredRootJoint(def);
        const int parent = def.authoredSkeleton.empty()
            ? -1
            : std::clamp(editor.selectedAnimationJoint, 0, static_cast<int>(def.authoredSkeleton.size()) - 1);
        pf::AnimationJoint joint;
        joint.name = "Joint" + std::to_string(def.authoredSkeleton.size());
        joint.parent = parent;
        joint.translation = {0, pf::fxFromFloat(0.5f), 0};
        def.authoredSkeleton.push_back(std::move(joint));
        editor.selectedAnimationJoint = static_cast<int>(def.authoredSkeleton.size()) - 1;
        editor.status = "Editor: added authored skeleton joint";
    }
    if (showingAuthoredClips && !def.authoredSkeleton.empty()) {
        pf::AnimationJoint& joint = def.authoredSkeleton[static_cast<size_t>(std::clamp(editor.selectedAnimationJoint, 0, static_cast<int>(def.authoredSkeleton.size()) - 1))];
        if (uiButton({516.0f, 500.0f, 72.0f, 24.0f}, "Jnt Lt")) {
            joint.translation.x -= pf::fxFromFloat(0.05f);
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: moved authored joint bind offset left";
        }
        if (uiButton({516.0f, 530.0f, 72.0f, 24.0f}, "Jnt Rt")) {
            joint.translation.x += pf::fxFromFloat(0.05f);
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: moved authored joint bind offset right";
        }
        if (uiButton({262.0f, 516.0f, 72.0f, 24.0f}, "Jnt Up")) {
            joint.translation.y += pf::fxFromFloat(0.05f);
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: raised authored joint bind offset";
        }
        if (uiButton({262.0f, 546.0f, 72.0f, 24.0f}, "Jnt Dn")) {
            joint.translation.y -= pf::fxFromFloat(0.05f);
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: lowered authored joint bind offset";
        }
        if (uiButton({516.0f, 560.0f, 72.0f, 24.0f}, "Parent")) {
            const int selectedJoint = std::clamp(
                editor.selectedAnimationJoint,
                0,
                static_cast<int>(def.authoredSkeleton.size()) - 1);
            if (selectedJoint <= 0) {
                joint.parent = -1;
                editor.status = "Editor: root joint parent is none";
            } else {
                joint.parent = (joint.parent + 2) % (selectedJoint + 1) - 1;
                editor.status = "Editor: cycled authored joint parent";
            }
            editor.animationPreviewActive = true;
            editor.paused = true;
        }
        if (uiButton({598.0f, 500.0f, 44.0f, 24.0f}, "Scl+")) {
            scaleAuthoredJoint(joint, {pf::fxFromFloat(0.05f), pf::fxFromFloat(0.05f), pf::fxFromFloat(0.05f)});
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: enlarged authored joint scale";
        }
        if (uiButton({648.0f, 500.0f, 44.0f, 24.0f}, "Scl-")) {
            scaleAuthoredJoint(joint, {-pf::fxFromFloat(0.05f), -pf::fxFromFloat(0.05f), -pf::fxFromFloat(0.05f)});
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: reduced authored joint scale";
        }
        if (uiButton({598.0f, 530.0f, 44.0f, 24.0f}, "SX+")) {
            scaleAuthoredJoint(joint, {pf::fxFromFloat(0.05f), 0, 0});
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: widened authored joint scale";
        }
        if (uiButton({648.0f, 530.0f, 44.0f, 24.0f}, "SX-")) {
            scaleAuthoredJoint(joint, {-pf::fxFromFloat(0.05f), 0, 0});
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: narrowed authored joint scale";
        }
        if (uiButton({598.0f, 560.0f, 44.0f, 24.0f}, "SY+")) {
            scaleAuthoredJoint(joint, {0, pf::fxFromFloat(0.05f), 0});
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: stretched authored joint scale";
        }
        if (uiButton({648.0f, 560.0f, 44.0f, 24.0f}, "SY-")) {
            scaleAuthoredJoint(joint, {0, -pf::fxFromFloat(0.05f), 0});
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: shortened authored joint scale";
        }
        if (uiButton({598.0f, 590.0f, 44.0f, 24.0f}, "RX-")) {
            rotateAuthoredJoint(joint, {-pf::fxFromFloat(0.05f), 0, 0});
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: rotated authored joint around X";
        }
        if (uiButton({648.0f, 590.0f, 44.0f, 24.0f}, "RX+")) {
            rotateAuthoredJoint(joint, {pf::fxFromFloat(0.05f), 0, 0});
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: rotated authored joint around X";
        }
        if (uiButton({598.0f, 620.0f, 44.0f, 24.0f}, "RY-")) {
            rotateAuthoredJoint(joint, {0, -pf::fxFromFloat(0.05f), 0});
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: rotated authored joint around Y";
        }
        if (uiButton({648.0f, 620.0f, 44.0f, 24.0f}, "RY+")) {
            rotateAuthoredJoint(joint, {0, pf::fxFromFloat(0.05f), 0});
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: rotated authored joint around Y";
        }
        if (uiButton({598.0f, 650.0f, 44.0f, 24.0f}, "RZ-")) {
            rotateAuthoredJoint(joint, {0, 0, -pf::fxFromFloat(0.05f)});
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: rotated authored joint around Z";
        }
        if (uiButton({648.0f, 650.0f, 44.0f, 24.0f}, "RZ+")) {
            rotateAuthoredJoint(joint, {0, 0, pf::fxFromFloat(0.05f)});
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: rotated authored joint around Z";
        }
        if (uiButton({516.0f, 590.0f, 72.0f, 24.0f}, "DelJnt")) {
            if (def.authoredSkeleton.size() <= 1) {
                editor.status = "Editor: cannot delete the only authored joint";
            } else {
                const int removeIndex = std::clamp(
                    editor.selectedAnimationJoint,
                    0,
                    static_cast<int>(def.authoredSkeleton.size()) - 1);
                const std::string removedName = def.authoredSkeleton[static_cast<size_t>(removeIndex)].name;
                removeAuthoredSkeletonJoint(def, removeIndex);
                editor.selectedAnimationJoint = std::clamp(
                    removeIndex,
                    0,
                    std::max(0, static_cast<int>(def.authoredSkeleton.size()) - 1));
                editor.selectedAnimationTrack = 0;
                editor.selectedAnimationKey = 0;
                editor.animationPreviewActive = true;
                editor.paused = true;
                editor.status = "Editor: deleted authored joint " + removedName;
                return;
            }
        }
    }

    if (showingAuthoredClips && selectedAuthoredClip && uiButton({352.0f, 500.0f, 72.0f, 24.0f}, "+ Track")) {
        ensureAuthoredRootJoint(def);
        pf::AnimationTrack track;
        track.joint = std::clamp(editor.selectedAnimationJoint, 0, std::max(0, static_cast<int>(def.authoredSkeleton.size()) - 1));
        track.channel = pf::AnimationChannel::TranslateY;
        track.keys = {
            {0, 0, 0, pf::AnimationInterpolation::Linear},
            {selectedAuthoredClip->frameCount, 0, 0, pf::AnimationInterpolation::Linear},
        };
        selectedAuthoredClip->tracks.push_back(std::move(track));
        editor.selectedAnimationTrack = static_cast<int>(selectedAuthoredClip->tracks.size()) - 1;
        editor.selectedAnimationKey = 0;
        editor.status = "Editor: added authored animation track";
    }
    if (showingAuthoredClips && selectedAuthoredClip && !selectedAuthoredClip->tracks.empty()) {
        editor.selectedAnimationTrack = std::clamp(editor.selectedAnimationTrack, 0, static_cast<int>(selectedAuthoredClip->tracks.size()) - 1);
        if (uiButton({352.0f, 626.0f, 72.0f, 24.0f}, "Del Tr")) {
            selectedAuthoredClip->tracks.erase(selectedAuthoredClip->tracks.begin() + editor.selectedAnimationTrack);
            editor.selectedAnimationTrack = std::clamp(editor.selectedAnimationTrack, 0, std::max(0, static_cast<int>(selectedAuthoredClip->tracks.size()) - 1));
            editor.selectedAnimationKey = 0;
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: deleted authored animation track";
            return;
        }
        pf::AnimationTrack& track = selectedAuthoredClip->tracks[static_cast<size_t>(editor.selectedAnimationTrack)];
        DrawText(("Track " + std::to_string(editor.selectedAnimationTrack) + " j" + std::to_string(track.joint) + " " + animationChannelName(track.channel)).c_str(), 352, 530, 13, DARKGRAY);
        if (!track.keys.empty()) {
            const Rectangle keyTimeline{24.0f, 584.0f, 310.0f, 28.0f};
            DrawRectangleRec(keyTimeline, Fade(LIGHTGRAY, 0.62f));
            DrawRectangleLinesEx(keyTimeline, 1.0f, DARKGRAY);
            const int clipFrameCount = std::max(1, static_cast<int>(pf::fxToFloat(selectedAuthoredClip->frameCount)) - 1);
            for (size_t keyIndex = 0; keyIndex < track.keys.size(); ++keyIndex) {
                const int keyFrame = static_cast<int>(pf::fxToFloat(track.keys[keyIndex].frame));
                const float t = static_cast<float>(std::clamp(keyFrame, 0, clipFrameCount)) / static_cast<float>(clipFrameCount);
                const float x = keyTimeline.x + 5.0f + (keyTimeline.width - 10.0f) * t;
                const bool selected = static_cast<int>(keyIndex) == editor.selectedAnimationKey;
                DrawRectangle(static_cast<int>(x - 2.0f), static_cast<int>(keyTimeline.y + 4.0f), 5, static_cast<int>(keyTimeline.height - 8.0f), selected ? BLUE : ORANGE);
            }
            const float scrubT = static_cast<float>(std::clamp(editor.animationScrubFrame, 0, clipFrameCount)) /
                static_cast<float>(clipFrameCount);
            const float scrubX = keyTimeline.x + 5.0f + (keyTimeline.width - 10.0f) * scrubT;
            DrawRectangle(static_cast<int>(scrubX), static_cast<int>(keyTimeline.y - 3.0f), 2, static_cast<int>(keyTimeline.height + 6.0f), BLACK);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), keyTimeline)) {
                const float clickT = std::clamp((GetMousePosition().x - keyTimeline.x - 5.0f) / (keyTimeline.width - 10.0f), 0.0f, 1.0f);
                const int clickedFrame = static_cast<int>(std::round(clickT * static_cast<float>(clipFrameCount)));
                int bestKey = 0;
                int bestDistance = 1000000;
                for (size_t keyIndex = 0; keyIndex < track.keys.size(); ++keyIndex) {
                    const int keyFrame = static_cast<int>(pf::fxToFloat(track.keys[keyIndex].frame));
                    const int distance = std::abs(keyFrame - clickedFrame);
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        bestKey = static_cast<int>(keyIndex);
                    }
                }
                editor.selectedAnimationKey = bestKey;
                editor.animationScrubFrame = static_cast<int>(pf::fxToFloat(track.keys[static_cast<size_t>(bestKey)].frame));
                editor.animationPreviewActive = true;
                editor.paused = true;
                editor.status = "Editor: selected authored key on timeline";
            }
        }
        if (uiButton({434.0f, 500.0f, 72.0f, 24.0f}, "Chan")) {
            track.channel = nextAnimationChannel(track.channel);
            editor.status = "Editor: cycled authored track channel";
        }
        if (uiButton({352.0f, 548.0f, 72.0f, 24.0f}, "UseJnt")) {
            track.joint = std::clamp(editor.selectedAnimationJoint, 0, std::max(0, static_cast<int>(def.authoredSkeleton.size()) - 1));
            editor.status = "Editor: assigned track to selected joint";
        }
        if (uiButton({434.0f, 548.0f, 72.0f, 24.0f}, "+ Key")) {
            const pf::Fix frame = pf::fx(editor.animationScrubFrame);
            const auto existing = std::find_if(track.keys.begin(), track.keys.end(), [&](const pf::AnimationKey& key) {
                return key.frame == frame;
            });
            if (existing != track.keys.end()) {
                editor.selectedAnimationKey = static_cast<int>(std::distance(track.keys.begin(), existing));
            } else {
                track.keys.push_back({frame, pf::sampleTrack(track, frame), 0, pf::AnimationInterpolation::Linear});
                sortAnimationKeys(track.keys);
                for (size_t i = 0; i < track.keys.size(); ++i) {
                    if (track.keys[i].frame == frame) {
                        editor.selectedAnimationKey = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (!track.keys.empty()) {
                const size_t keyIndex = static_cast<size_t>(std::clamp(editor.selectedAnimationKey, 0, static_cast<int>(track.keys.size()) - 1));
                track.keys[keyIndex].value = pf::sampleTrack(track, frame);
            }
            editor.animationPreviewActive = true;
            editor.paused = true;
            editor.status = "Editor: inserted authored animation key";
        }
        if (!track.keys.empty()) {
            editor.selectedAnimationKey = std::clamp(editor.selectedAnimationKey, 0, static_cast<int>(track.keys.size()) - 1);
            const pf::AnimationKey& key = track.keys[static_cast<size_t>(editor.selectedAnimationKey)];
            DrawText(("Key " + std::to_string(editor.selectedAnimationKey) + " f" + std::to_string(static_cast<int>(pf::fxToFloat(key.frame))) +
                      " v=" + std::to_string(pf::fxToFloat(key.value))).c_str(), 352, 578, 13, DARKGRAY);
            if (uiButton({434.0f, 626.0f, 72.0f, 24.0f}, "Del Key")) {
                track.keys.erase(track.keys.begin() + editor.selectedAnimationKey);
                editor.selectedAnimationKey = std::clamp(editor.selectedAnimationKey, 0, std::max(0, static_cast<int>(track.keys.size()) - 1));
                editor.animationPreviewActive = true;
                editor.paused = true;
                editor.status = "Editor: deleted authored animation key";
                return;
            }
            pf::AnimationKey& mutableKey = track.keys[static_cast<size_t>(editor.selectedAnimationKey)];
            if (uiButton({352.0f, 596.0f, 72.0f, 24.0f}, "Val -")) {
                mutableKey.value -= pf::fxFromFloat(0.05f);
                editor.animationPreviewActive = true;
                editor.paused = true;
                editor.status = "Editor: lowered authored key value";
            }
            if (uiButton({434.0f, 596.0f, 72.0f, 24.0f}, "Val +")) {
                mutableKey.value += pf::fxFromFloat(0.05f);
                editor.animationPreviewActive = true;
                editor.paused = true;
                editor.status = "Editor: raised authored key value";
            }
            auto moveSelectedKeyFrame = [&](int delta) {
                const int keyIndex = std::clamp(editor.selectedAnimationKey, 0, static_cast<int>(track.keys.size()) - 1);
                const int currentFrame = static_cast<int>(pf::fxToFloat(track.keys[static_cast<size_t>(keyIndex)].frame));
                const int maxClipFrame = std::max(0, static_cast<int>(pf::fxToFloat(selectedAuthoredClip->frameCount)) - 1);
                const int targetFrame = std::clamp(currentFrame + delta, 0, maxClipFrame);
                if (targetFrame == currentFrame) {
                    return;
                }
                const pf::Fix target = pf::fx(targetFrame);
                const auto duplicate = std::find_if(track.keys.begin(), track.keys.end(), [&](const pf::AnimationKey& candidate) {
                    return candidate.frame == target;
                });
                if (duplicate != track.keys.end() && static_cast<int>(std::distance(track.keys.begin(), duplicate)) != keyIndex) {
                    editor.status = "Editor: authored key frame already exists";
                    return;
                }
                track.keys[static_cast<size_t>(keyIndex)].frame = target;
                sortAnimationKeys(track.keys);
                for (size_t i = 0; i < track.keys.size(); ++i) {
                    if (track.keys[i].frame == target) {
                        editor.selectedAnimationKey = static_cast<int>(i);
                        break;
                    }
                }
                editor.animationScrubFrame = targetFrame;
                editor.animationPreviewActive = true;
                editor.paused = true;
                editor.status = "Editor: moved authored key frame";
            };
            if (uiButton({352.0f, 656.0f, 72.0f, 24.0f}, "Frame-")) {
                moveSelectedKeyFrame(-1);
            }
            if (uiButton({434.0f, 656.0f, 72.0f, 24.0f}, "Frame+")) {
                moveSelectedKeyFrame(1);
            }
        }
    } else if (showingAuthoredClips) {
        DrawText("No authored tracks", 352, 530, 13, GRAY);
    }
    if (showingAuthoredClips && selectedAuthoredClip && uiButton({24.0f, 546.0f, 72.0f, 24.0f}, "Sync St")) {
        pf::FighterState& state = def.states[static_cast<size_t>(editor.selectedState)];
        state.animation = selectedAuthoredClip->name;
        state.animationActionIndex = selectedAuthoredClip->actionIndex;
        state.animationLengthFrames = std::max(1, static_cast<int>(pf::fxToFloat(selectedAuthoredClip->frameCount)));
        editor.status = "Editor: assigned authored clip to selected state";
    }
    if (uiButton({434.0f, 440.0f, 72.0f, 24.0f}, "Clear")) {
        fighter.animationActionIndexOverride = -1;
        fighter.animationRate = pf::fx(1);
        editor.animationPreviewActive = false;
        editor.status = "Editor: cleared animation preview override";
        return;
    }
    editor.animationScrubFrame = std::clamp(editor.animationScrubFrame, 0, maxFrame);
    if (editor.animationPreviewActive) {
        const bool ok = pf::previewFighterAnimation(
            world,
            static_cast<size_t>(editor.selectedFighter),
            selectedClip.actionIndex,
            pf::fx(editor.animationScrubFrame));
        if (ok) {
            DrawText("Preview pose applied to selected fighter", 24, 626, 13, DARKGRAY);
        }
    }
}

static std::string editorClipActionName(const pf::AnimationClip& clip) {
    const std::string marker = "_ACTION_";
    const std::string suffix = "_figatree";
    const size_t markerPos = clip.name.find(marker);
    const size_t suffixPos = clip.name.rfind(suffix);
    if (markerPos != std::string::npos && suffixPos != std::string::npos && suffixPos > markerPos + marker.size()) {
        return clip.name.substr(markerPos + marker.size(), suffixPos - markerPos - marker.size());
    }
    return clip.name;
}

static const std::vector<pf::AnimationClip>* editorStateAnimationClips(const pf::FighterDefinition& def) {
    const std::vector<pf::AnimationClip>& clips = pf::authoredAnimationClips(def);
    return clips.empty() ? nullptr : &clips;
}

static void assignEditorStateAnimation(pf::FighterState& state, const pf::AnimationClip& clip) {
    state.animation = editorClipActionName(clip);
    state.animationActionIndex = clip.actionIndex;
    state.animationLengthFrames = std::max(1, static_cast<int>(pf::fxToFloat(clip.frameCount)));
}

static void cycleEditorStateAnimation(const pf::FighterDefinition& def, pf::FighterState& state, pf::FighterEditor& editor) {
    const std::vector<pf::AnimationClip>* clips = editorStateAnimationClips(def);
    if (!clips || clips->empty()) {
        editor.status = "Editor: no clips available for selected state";
        return;
    }
    int selectedClip = -1;
    for (size_t i = 0; i < clips->size(); ++i) {
        const pf::AnimationClip& clip = (*clips)[i];
        if ((state.animationActionIndex >= 0 && clip.actionIndex == state.animationActionIndex) ||
            (state.animationActionIndex < 0 && editorClipActionName(clip) == state.animation))
        {
            selectedClip = static_cast<int>(i);
            break;
        }
    }
    const int nextClip = wrappedIndex(selectedClip + 1, static_cast<int>(clips->size()));
    assignEditorStateAnimation(state, (*clips)[static_cast<size_t>(nextClip)]);
    editor.selectedAnimationClip = nextClip;
    editor.status = "Editor: assigned animation " + state.animation + " to " + state.name;
}

static bool moveEditorSubaction(pf::FighterState& state, pf::FighterEditor& editor, int delta) {
    if (state.action.empty() || delta == 0) {
        return false;
    }
    editor.selectedSubaction = std::clamp(editor.selectedSubaction, 0, static_cast<int>(state.action.size()) - 1);
    const int target = editor.selectedSubaction + delta;
    if (target < 0 || target >= static_cast<int>(state.action.size())) {
        return false;
    }
    std::swap(state.action[static_cast<size_t>(editor.selectedSubaction)],
              state.action[static_cast<size_t>(target)]);
    editor.selectedSubaction = target;
    editor.status = delta < 0
        ? "Editor: moved selected subaction earlier"
        : "Editor: moved selected subaction later";
    return true;
}

static void drawEditorMovesetWorkspace(pf::World& world, pf::FighterEditor& editor) {
    editor.clampToWorld(world);
    if (world.fighters.empty()) {
        return;
    }
    pf::FighterRuntime& fighter = world.fighters[static_cast<size_t>(editor.selectedFighter)];
    if (fighter.fighterDef < 0 || fighter.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
        return;
    }
    pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    pf::FighterState& state = def.states[static_cast<size_t>(editor.selectedState)];
    const Rectangle panel{12.0f, 324.0f, 530.0f, 260.0f};
    DrawRectangleRec(panel, Fade(RAYWHITE, 0.58f));
    DrawRectangleLinesEx(panel, 1.0f, DARKGRAY);
    DrawText("State Properties", 24, 336, 16, BLACK);
    DrawText(("Animation: " + state.animation + " action=" + std::to_string(state.animationActionIndex)).c_str(), 24, 360, 13, DARKGRAY);
    DrawText(("Length: " + std::to_string(state.animationLengthFrames) +
              "  IASA: " + std::to_string(state.initialInterruptibleFrame)).c_str(), 24, 382, 13, DARKGRAY);
    const std::string finishState = state.onAnimationFinishedState.empty() ? "none" : state.onAnimationFinishedState;
    DrawText(("Finish: " + finishState +
              "  FinBlend " + animationBlendLabel(state.onAnimationFinishedBlendFrames) +
              "  DefBlend " + animationBlendLabel(state.defaultAnimationBlendFrames)).c_str(), 516, 360, 12, DARKGRAY);

    if (uiButton({24.0f, 410.0f, 90.0f, 24.0f}, "Loop", state.loopAnimation)) {
        state.loopAnimation = !state.loopAnimation;
        editor.status = "Editor: toggled loop animation for " + state.name;
    }
    if (uiButton({122.0f, 410.0f, 90.0f, 24.0f}, "Anim Phys", state.useAnimPhysics)) {
        state.useAnimPhysics = !state.useAnimPhysics;
        editor.status = "Editor: toggled animation physics for " + state.name;
    }
    if (uiButton({220.0f, 410.0f, 90.0f, 24.0f}, "Slideoff", state.allowSlideoff)) {
        state.allowSlideoff = !state.allowSlideoff;
        editor.status = "Editor: toggled slideoff for " + state.name;
    }
    if (uiButton({318.0f, 410.0f, 90.0f, 24.0f}, "Ledge", state.allowLedgeGrab)) {
        state.allowLedgeGrab = !state.allowLedgeGrab;
        editor.status = "Editor: toggled ledge grab for " + state.name;
    }
    if (uiButton({416.0f, 410.0f, 90.0f, 24.0f}, "Floor", state.convertFloorCollisionToGround)) {
        state.convertFloorCollisionToGround = !state.convertFloorCollisionToGround;
        editor.status = "Editor: toggled floor collision for " + state.name;
    }
    if (uiButton({516.0f, 382.0f, 58.0f, 24.0f}, "Anim>")) {
        cycleEditorStateAnimation(def, state, editor);
    }
    if (uiButton({582.0f, 382.0f, 58.0f, 24.0f}, "Fin>")) {
        const int current = def.stateIndex(state.onAnimationFinishedState);
        const int next = wrappedIndex(std::max(0, current) + 1, static_cast<int>(def.states.size()));
        state.onAnimationFinishedState = def.states[static_cast<size_t>(next)].name;
        editor.status = "Editor: cycled animation-finished target for " + state.name;
    }
    if (uiButton({648.0f, 382.0f, 58.0f, 24.0f}, "FinX")) {
        state.onAnimationFinishedState.clear();
        editor.status = "Editor: cleared animation-finished target for " + state.name;
    }
    if (uiButton({714.0f, 382.0f, 58.0f, 24.0f}, "Blend-")) {
        state.defaultAnimationBlendFrames = std::max(0, state.defaultAnimationBlendFrames - 1);
        editor.status = "Editor: shortened default blend for " + state.name;
    }
    if (uiButton({780.0f, 382.0f, 58.0f, 24.0f}, "Blend+")) {
        ++state.defaultAnimationBlendFrames;
        editor.status = "Editor: lengthened default blend for " + state.name;
    }
    if (uiButton({846.0f, 382.0f, 58.0f, 24.0f}, "FinB-")) {
        if (state.onAnimationFinishedBlendFrames == pf::kDisableAnimationBlendFrames) {
            editor.status = "Editor: animation-finished blend is already disabled";
        } else {
            --state.onAnimationFinishedBlendFrames;
            editor.status = "Editor: shortened animation-finished blend for " + state.name;
        }
    }
    if (uiButton({912.0f, 382.0f, 58.0f, 24.0f}, "FinB+")) {
        ++state.onAnimationFinishedBlendFrames;
        editor.status = "Editor: lengthened animation-finished blend for " + state.name;
    }
    if (uiButton({516.0f, 410.0f, 90.0f, 24.0f}, "BackLed", state.allowBackwardsLedgeGrab)) {
        state.allowBackwardsLedgeGrab = !state.allowBackwardsLedgeGrab;
        editor.status = "Editor: toggled backwards ledge grab for " + state.name;
    }
    if (uiButton({614.0f, 410.0f, 90.0f, 24.0f}, "CloneSt")) {
        duplicateEditorState(world, editor);
        return;
    }
    if (uiButton({24.0f, 440.0f, 90.0f, 24.0f}, "Wall", state.allowWallCollision)) {
        state.allowWallCollision = !state.allowWallCollision;
        editor.status = "Editor: toggled wall collision for " + state.name;
    }
    if (uiButton({122.0f, 440.0f, 90.0f, 24.0f}, "Ceiling", state.allowCeilingCollision)) {
        state.allowCeilingCollision = !state.allowCeilingCollision;
        editor.status = "Editor: toggled ceiling collision for " + state.name;
    }
    if (uiButton({220.0f, 440.0f, 90.0f, 24.0f}, "- Len")) {
        state.animationLengthFrames = std::max(1, state.animationLengthFrames - 1);
        editor.status = "Editor: shortened " + state.name;
    }
    if (uiButton({318.0f, 440.0f, 90.0f, 24.0f}, "+ Len")) {
        ++state.animationLengthFrames;
        editor.status = "Editor: lengthened " + state.name;
    }
    if (uiButton({416.0f, 440.0f, 90.0f, 24.0f}, "+ IASA")) {
        ++state.initialInterruptibleFrame;
        editor.status = "Editor: moved IASA later for " + state.name;
    }
    if (uiButton({24.0f, 470.0f, 90.0f, 24.0f}, "- IASA")) {
        state.initialInterruptibleFrame = std::max(0, state.initialInterruptibleFrame - 1);
        editor.status = "Editor: moved IASA earlier for " + state.name;
    }
    if (uiButton({516.0f, 440.0f, 58.0f, 24.0f}, "+ Sync")) {
        pf::Subaction subaction;
        subaction.type = pf::SubactionType::SyncTimer;
        subaction.frames = 5;
        state.action.push_back(subaction);
        editor.selectedSubaction = static_cast<int>(state.action.size()) - 1;
        editor.status = "Editor: added sync timer subaction to " + state.name;
    }
    if (uiButton({582.0f, 440.0f, 58.0f, 24.0f}, "+ Async")) {
        pf::Subaction subaction;
        subaction.type = pf::SubactionType::AsyncTimer;
        subaction.frames = 5;
        state.action.push_back(subaction);
        editor.selectedSubaction = static_cast<int>(state.action.size()) - 1;
        editor.status = "Editor: added async timer subaction to " + state.name;
    }
    if (uiButton({648.0f, 440.0f, 58.0f, 24.0f}, "+ Intbl")) {
        pf::Subaction subaction;
        subaction.type = pf::SubactionType::SetInterruptible;
        subaction.frames = 1;
        subaction.interruptibleFrame = -1;
        state.action.push_back(subaction);
        editor.selectedSubaction = static_cast<int>(state.action.size()) - 1;
        editor.status = "Editor: added interruptible marker to " + state.name;
    }
    if (uiButton({714.0f, 440.0f, 58.0f, 24.0f}, "+ Rev")) {
        pf::Subaction subaction;
        subaction.type = pf::SubactionType::ReverseDirection;
        subaction.frames = 1;
        state.action.push_back(subaction);
        editor.selectedSubaction = static_cast<int>(state.action.size()) - 1;
        editor.status = "Editor: added reverse-direction subaction to " + state.name;
    }
    if (uiButton({780.0f, 440.0f, 58.0f, 24.0f}, "Sub<")) {
        moveEditorSubaction(state, editor, -1);
    }
    if (uiButton({846.0f, 440.0f, 58.0f, 24.0f}, "Sub>")) {
        moveEditorSubaction(state, editor, 1);
    }
    if (uiButton({122.0f, 470.0f, 90.0f, 24.0f}, "+ Hitbox")) {
        pf::Subaction subaction;
        subaction.type = pf::SubactionType::CreateHitbox;
        subaction.frames = 3;
        subaction.hitbox.hitboxId = static_cast<int>(state.action.size());
        subaction.hitbox.damage = pf::fxFromFloat(3.0f);
        subaction.hitbox.radius = pf::fxFromFloat(0.45f);
        subaction.hitbox.offset = {pf::fxFromFloat(0.85f), pf::fxFromFloat(1.2f), 0};
        subaction.hitbox.knockbackAngleDegrees = pf::fx(45);
        subaction.hitbox.knockbackBase = pf::fx(20);
        subaction.hitbox.knockbackGrowth = pf::fx(60);
        state.action.push_back(subaction);
        editor.selectedSubaction = static_cast<int>(state.action.size()) - 1;
        editor.status = "Editor: added hitbox subaction to " + state.name;
    }
    if (uiButton({220.0f, 470.0f, 90.0f, 24.0f}, "+ Clear")) {
        pf::Subaction subaction;
        subaction.type = pf::SubactionType::ClearHitboxes;
        subaction.frames = 1;
        state.action.push_back(subaction);
        editor.selectedSubaction = static_cast<int>(state.action.size()) - 1;
        editor.status = "Editor: added clear-hitboxes subaction to " + state.name;
    }
    if (uiButton({318.0f, 470.0f, 90.0f, 24.0f}, "+ Wait Int")) {
        pf::InterruptRule interrupt;
        interrupt.targetState = "Wait";
        interrupt.condition = pf::InterruptCondition::WaitInput;
        interrupt.enableFrame = state.initialInterruptibleFrame;
        interrupt.disableFrame = 0;
        state.interrupts.push_back(interrupt);
        editor.selectedInterrupt = static_cast<int>(state.interrupts.size()) - 1;
        editor.status = "Editor: added Wait interrupt to " + state.name;
    }
    if (uiButton({416.0f, 470.0f, 90.0f, 24.0f}, "- Int")) {
        if (!state.interrupts.empty()) {
            state.interrupts.erase(state.interrupts.begin() + editor.selectedInterrupt);
            editor.selectedInterrupt = std::clamp(editor.selectedInterrupt, 0, std::max(0, static_cast<int>(state.interrupts.size()) - 1));
            editor.status = "Editor: removed interrupt from " + state.name;
        }
    }
    if (uiButton({516.0f, 470.0f, 90.0f, 24.0f}, "+ Script")) {
        if (def.packageScripts.empty()) {
            editor.status = "Editor: add a package script before adding script subactions";
        } else {
            pf::Subaction subaction;
            subaction.type = pf::SubactionType::CallScript;
            subaction.frames = 1;
            subaction.objectName = packageScriptTargetName(def.packageScripts, editor.selectedPackageScript);
            state.action.push_back(subaction);
            editor.selectedSubaction = static_cast<int>(state.action.size()) - 1;
            editor.status = "Editor: added script subaction to " + state.name;
        }
    }
    if (!state.interrupts.empty()) {
        editor.selectedInterrupt = std::clamp(editor.selectedInterrupt, 0, static_cast<int>(state.interrupts.size()) - 1);
        pf::InterruptRule& interrupt = state.interrupts[static_cast<size_t>(editor.selectedInterrupt)];
        normalizeInterruptPackageVariable(interrupt, static_cast<int>(def.packageVariables.size()));
        DrawText(("Interrupt #" + std::to_string(editor.selectedInterrupt) + " -> " + interrupt.targetState +
                  " " + interruptConditionName(interrupt.condition) +
                  " " + groundRequirementName(interrupt.ground)).c_str(), 276, 506, 13, DARKGRAY);
        DrawText(("en " + std::to_string(interrupt.enableFrame) +
                  " dis " + std::to_string(interrupt.disableFrame) +
                  " lag " + std::to_string(interrupt.lagFrames) +
                  " blend " + animationBlendLabel(interrupt.blendFrames)).c_str(), 714, 506, 13, DARKGRAY);
        if (interrupt.condition == pf::InterruptCondition::PackageVarAtLeast) {
            DrawText(("v" + std::to_string(interrupt.packageVariable) + " >= " + std::to_string(interrupt.packageValue)).c_str(), 516, 506, 13, DARKGRAY);
        }
        if (uiButton({276.0f, 524.0f, 44.0f, 24.0f}, "Prev")) {
            --editor.selectedInterrupt;
        }
        if (uiButton({326.0f, 524.0f, 44.0f, 24.0f}, "Next")) {
            ++editor.selectedInterrupt;
        }
        if (uiButton({376.0f, 524.0f, 44.0f, 24.0f}, "Cond")) {
            interrupt.condition = nextCommonInterruptCondition(interrupt.condition);
            if (interrupt.condition == pf::InterruptCondition::PackageVarAtLeast && def.packageVariables.empty()) {
                interrupt.condition = pf::InterruptCondition::WaitInput;
                interrupt.packageVariable = -1;
                editor.status = "Editor: skipped package-var interrupt because no package variables exist";
            } else {
                normalizeInterruptPackageVariable(interrupt, static_cast<int>(def.packageVariables.size()));
                editor.status = "Editor: cycled interrupt condition";
            }
        }
        if (uiButton({426.0f, 524.0f, 44.0f, 24.0f}, "Gnd")) {
            interrupt.ground = nextGroundRequirement(interrupt.ground);
            editor.status = "Editor: cycled interrupt ground requirement";
        }
        if (uiButton({476.0f, 524.0f, 34.0f, 24.0f}, "Tgt")) {
            const int current = def.stateIndex(interrupt.targetState);
            const int next = def.states.empty() ? -1 : (std::max(0, current) + 1) % static_cast<int>(def.states.size());
            if (next >= 0) {
                interrupt.targetState = def.states[static_cast<size_t>(next)].name;
                editor.status = "Editor: cycled interrupt target state";
            }
        }
        if (uiButton({516.0f, 524.0f, 44.0f, 24.0f}, "Pkg")) {
            if (def.packageVariables.empty()) {
                editor.status = "Editor: add a package variable before using package-var interrupts";
            } else {
                interrupt.condition = pf::InterruptCondition::PackageVarAtLeast;
                interrupt.packageVariable = std::clamp(editor.selectedPackageVariable, 0, static_cast<int>(def.packageVariables.size()) - 1);
                interrupt.packageValue = std::max<int32_t>(1, interrupt.packageValue);
                editor.status = "Editor: set interrupt to package variable condition";
            }
        }
        if (uiButton({566.0f, 524.0f, 44.0f, 24.0f}, "Var")) {
            if (!def.packageVariables.empty()) {
                interrupt.condition = pf::InterruptCondition::PackageVarAtLeast;
                interrupt.packageVariable = (std::max(0, interrupt.packageVariable) + 1) % static_cast<int>(def.packageVariables.size());
                editor.status = "Editor: cycled interrupt package variable";
            }
        }
        if (uiButton({616.0f, 524.0f, 44.0f, 24.0f}, "Val+")) {
            if (def.packageVariables.empty()) {
                editor.status = "Editor: add a package variable before editing package-var thresholds";
            } else {
                interrupt.condition = pf::InterruptCondition::PackageVarAtLeast;
                interrupt.packageVariable = std::clamp(interrupt.packageVariable < 0 ? editor.selectedPackageVariable : interrupt.packageVariable, 0, static_cast<int>(def.packageVariables.size()) - 1);
                ++interrupt.packageValue;
                editor.status = "Editor: raised interrupt package variable threshold";
            }
        }
        if (uiButton({666.0f, 524.0f, 44.0f, 24.0f}, "Val-")) {
            if (def.packageVariables.empty()) {
                editor.status = "Editor: add a package variable before editing package-var thresholds";
            } else {
                interrupt.condition = pf::InterruptCondition::PackageVarAtLeast;
                interrupt.packageVariable = std::clamp(interrupt.packageVariable < 0 ? editor.selectedPackageVariable : interrupt.packageVariable, 0, static_cast<int>(def.packageVariables.size()) - 1);
                --interrupt.packageValue;
                editor.status = "Editor: lowered interrupt package variable threshold";
            }
        }
        if (uiButton({276.0f, 554.0f, 44.0f, 22.0f}, "En-")) {
            interrupt.enableFrame = std::max(0, interrupt.enableFrame - 1);
            editor.status = "Editor: moved interrupt enable frame earlier";
        }
        if (uiButton({326.0f, 554.0f, 44.0f, 22.0f}, "En+")) {
            ++interrupt.enableFrame;
            editor.status = "Editor: moved interrupt enable frame later";
        }
        if (uiButton({376.0f, 554.0f, 44.0f, 22.0f}, "Dis-")) {
            interrupt.disableFrame = std::max(0, interrupt.disableFrame - 1);
            editor.status = "Editor: moved interrupt disable frame earlier";
        }
        if (uiButton({426.0f, 554.0f, 44.0f, 22.0f}, "Dis+")) {
            ++interrupt.disableFrame;
            editor.status = "Editor: moved interrupt disable frame later";
        }
        if (uiButton({276.0f, 580.0f, 44.0f, 22.0f}, "Lag-")) {
            interrupt.lagFrames = std::max(0, interrupt.lagFrames - 1);
            editor.status = "Editor: shortened interrupt transition lag";
        }
        if (uiButton({326.0f, 580.0f, 44.0f, 22.0f}, "Lag+")) {
            ++interrupt.lagFrames;
            editor.status = "Editor: lengthened interrupt transition lag";
        }
        if (uiButton({376.0f, 580.0f, 44.0f, 22.0f}, "Bl-")) {
            if (interrupt.blendFrames == pf::kDisableAnimationBlendFrames) {
                editor.status = "Editor: interrupt blend is already disabled";
            } else {
                --interrupt.blendFrames;
                editor.status = "Editor: shortened interrupt transition blend";
            }
        }
        if (uiButton({426.0f, 580.0f, 44.0f, 22.0f}, "Bl+")) {
            ++interrupt.blendFrames;
            editor.status = "Editor: lengthened interrupt transition blend";
        }
        if (uiButton({276.0f, 606.0f, 54.0f, 22.0f}, "Start", interrupt.startActive)) {
            interrupt.startActive = !interrupt.startActive;
            editor.status = "Editor: toggled interrupt start-active flag";
        }
        if (uiButton({336.0f, 606.0f, 54.0f, 22.0f}, "Always", interrupt.alwaysActive)) {
            interrupt.alwaysActive = !interrupt.alwaysActive;
            editor.status = "Editor: toggled interrupt always-active flag";
        }
        if (uiButton({396.0f, 606.0f, 54.0f, 22.0f}, "NoHit", interrupt.requireNoHitstun)) {
            interrupt.requireNoHitstun = !interrupt.requireNoHitstun;
            editor.status = "Editor: toggled interrupt no-hitstun requirement";
        }
    }

    if (!state.action.empty()) {
        editor.selectedSubaction = std::clamp(editor.selectedSubaction, 0, static_cast<int>(state.action.size()) - 1);
        pf::Subaction& subaction = state.action[static_cast<size_t>(editor.selectedSubaction)];
        if (subaction.type == pf::SubactionType::SpawnObject ||
            subaction.type == pf::SubactionType::SpawnProjectile)
        {
            DrawText(("Spawn #" + std::to_string(editor.selectedSubaction) + " -> " + subaction.objectName).c_str(), 516, 554, 12, DARKGRAY);
            if (uiButton({516.0f, 570.0f, 44.0f, 22.0f}, "Tgt")) {
                const std::optional<pf::GameObjectKind> requiredKind = subaction.type == pf::SubactionType::SpawnProjectile
                    ? std::optional<pf::GameObjectKind>{pf::GameObjectKind::Projectile}
                    : std::optional<pf::GameObjectKind>{};
                const std::string target = nextObjectTargetName(world, subaction.objectName, requiredKind);
                if (!target.empty()) {
                    subaction.objectName = target;
                    editor.status = "Editor: cycled spawn target to " + target;
                }
            }
            if (uiButton({566.0f, 570.0f, 44.0f, 22.0f}, "Kind")) {
                if (subaction.type == pf::SubactionType::SpawnProjectile) {
                    subaction.type = pf::SubactionType::SpawnObject;
                    editor.status = "Editor: selected spawn now targets any object kind";
                } else {
                    const std::string target = nextObjectTargetName(
                        world,
                        subaction.objectName,
                        std::optional<pf::GameObjectKind>{pf::GameObjectKind::Projectile});
                    if (!target.empty()) {
                        subaction.type = pf::SubactionType::SpawnProjectile;
                        subaction.objectName = target;
                        editor.status = "Editor: selected spawn now requires projectile target";
                    } else {
                        editor.status = "Editor: no projectile object exists for selected spawn";
                    }
                }
            }
            if (uiButton({616.0f, 570.0f, 44.0f, 22.0f}, "VX+")) {
                subaction.spawnVelocity.x += pf::fxFromFloat(0.1f);
                editor.status = "Editor: increased spawn X velocity";
            }
            if (uiButton({666.0f, 570.0f, 44.0f, 22.0f}, "VX-")) {
                subaction.spawnVelocity.x -= pf::fxFromFloat(0.1f);
                editor.status = "Editor: decreased spawn X velocity";
            }
            DrawText(("vel " + std::to_string(pf::fxToFloat(subaction.spawnVelocity.x)) +
                      "," + std::to_string(pf::fxToFloat(subaction.spawnVelocity.y))).c_str(), 516, 596, 12, DARKGRAY);
            if (uiButton({616.0f, 592.0f, 44.0f, 22.0f}, "VY+")) {
                subaction.spawnVelocity.y += pf::fxFromFloat(0.1f);
                editor.status = "Editor: increased spawn Y velocity";
            }
            if (uiButton({666.0f, 592.0f, 44.0f, 22.0f}, "VY-")) {
                subaction.spawnVelocity.y -= pf::fxFromFloat(0.1f);
                editor.status = "Editor: decreased spawn Y velocity";
            }
            DrawText(("off " + std::to_string(pf::fxToFloat(subaction.spawnOffset.x)) +
                      "," + std::to_string(pf::fxToFloat(subaction.spawnOffset.y))).c_str(), 516, 620, 12, DARKGRAY);
            if (uiButton({616.0f, 616.0f, 44.0f, 22.0f}, "OX+")) {
                subaction.spawnOffset.x += pf::fxFromFloat(0.1f);
                editor.status = "Editor: moved spawn offset forward";
            }
            if (uiButton({666.0f, 616.0f, 44.0f, 22.0f}, "OY+")) {
                subaction.spawnOffset.y += pf::fxFromFloat(0.1f);
                editor.status = "Editor: raised spawn offset";
            }
            if (uiButton({616.0f, 638.0f, 44.0f, 22.0f}, "OX-")) {
                subaction.spawnOffset.x -= pf::fxFromFloat(0.1f);
                editor.status = "Editor: moved spawn offset backward";
            }
            if (uiButton({666.0f, 638.0f, 44.0f, 22.0f}, "OY-")) {
                subaction.spawnOffset.y -= pf::fxFromFloat(0.1f);
                editor.status = "Editor: lowered spawn offset";
            }
        } else if (subaction.type == pf::SubactionType::CallScript) {
            DrawText(("Script #" + std::to_string(editor.selectedSubaction) + " -> " + subaction.objectName +
                      " frames " + std::to_string(subaction.frames)).c_str(), 516, 554, 12, DARKGRAY);
            if (uiButton({516.0f, 570.0f, 44.0f, 22.0f}, "Tgt")) {
                if (def.packageScripts.empty()) {
                    editor.status = "Editor: add a package script before targeting script subactions";
                } else {
                    cyclePackageScriptSubactionTarget(subaction, def.packageScripts, editor.selectedPackageScript);
                    editor.status = "Editor: cycled script subaction target to " + subaction.objectName;
                }
            }
            if (uiButton({566.0f, 570.0f, 44.0f, 22.0f}, "Fr+")) {
                ++subaction.frames;
                editor.status = "Editor: lengthened selected script subaction";
            }
            if (uiButton({616.0f, 570.0f, 44.0f, 22.0f}, "Fr-")) {
                subaction.frames = std::max(0, subaction.frames - 1);
                editor.status = "Editor: shortened selected script subaction";
            }
        } else if (subaction.type == pf::SubactionType::CreateHitbox ||
                   subaction.type == pf::SubactionType::CreateThrowHitbox)
        {
            pf::HitboxDefinition& hitbox = subaction.hitbox;
            DrawText(("Hitbox #" + std::to_string(hitbox.hitboxId) +
                      " dmg " + std::to_string(pf::fxToFloat(hitbox.damage)) +
                      " r " + std::to_string(pf::fxToFloat(hitbox.radius))).c_str(), 516, 554, 12, DARKGRAY);
            if (uiButton({516.0f, 570.0f, 44.0f, 22.0f}, "D+")) {
                hitbox.damage += pf::fx(1);
                editor.status = "Editor: increased selected hitbox damage";
            }
            if (uiButton({566.0f, 570.0f, 44.0f, 22.0f}, "D-")) {
                hitbox.damage = std::max(pf::Fix{0}, hitbox.damage - pf::fx(1));
                editor.status = "Editor: decreased selected hitbox damage";
            }
            if (uiButton({616.0f, 570.0f, 44.0f, 22.0f}, "R+")) {
                hitbox.radius += pf::fxFromFloat(0.05f);
                editor.status = "Editor: increased selected hitbox radius";
            }
            if (uiButton({666.0f, 570.0f, 44.0f, 22.0f}, "R-")) {
                hitbox.radius = std::max(pf::fxFromFloat(0.05f), hitbox.radius - pf::fxFromFloat(0.05f));
                editor.status = "Editor: decreased selected hitbox radius";
            }
            DrawText(("ang " + std::to_string(pf::fxToFloat(hitbox.knockbackAngleDegrees)) +
                      " bkb " + std::to_string(pf::fxToFloat(hitbox.knockbackBase)) +
                      " kbg " + std::to_string(pf::fxToFloat(hitbox.knockbackGrowth))).c_str(), 516, 596, 12, DARKGRAY);
            if (uiButton({516.0f, 592.0f, 44.0f, 22.0f}, "A+")) {
                hitbox.knockbackAngleDegrees += pf::fx(5);
                editor.status = "Editor: raised selected hitbox angle";
            }
            if (uiButton({566.0f, 592.0f, 44.0f, 22.0f}, "A-")) {
                hitbox.knockbackAngleDegrees -= pf::fx(5);
                editor.status = "Editor: lowered selected hitbox angle";
            }
            if (uiButton({616.0f, 592.0f, 44.0f, 22.0f}, "BKB+")) {
                hitbox.knockbackBase += pf::fx(5);
                editor.status = "Editor: increased selected hitbox base knockback";
            }
            if (uiButton({666.0f, 592.0f, 44.0f, 22.0f}, "KBG+")) {
                hitbox.knockbackGrowth += pf::fx(5);
                editor.status = "Editor: increased selected hitbox growth";
            }
            DrawText(("off " + std::to_string(pf::fxToFloat(hitbox.offset.x)) +
                      "," + std::to_string(pf::fxToFloat(hitbox.offset.y))).c_str(), 516, 620, 12, DARKGRAY);
            if (uiButton({516.0f, 616.0f, 44.0f, 22.0f}, "X+")) {
                hitbox.offset.x += pf::fxFromFloat(0.1f);
                editor.status = "Editor: moved selected hitbox forward";
            }
            if (uiButton({566.0f, 616.0f, 44.0f, 22.0f}, "X-")) {
                hitbox.offset.x -= pf::fxFromFloat(0.1f);
                editor.status = "Editor: moved selected hitbox backward";
            }
            if (uiButton({616.0f, 616.0f, 44.0f, 22.0f}, "Y+")) {
                hitbox.offset.y += pf::fxFromFloat(0.1f);
                editor.status = "Editor: raised selected hitbox";
            }
            if (uiButton({666.0f, 616.0f, 44.0f, 22.0f}, "Y-")) {
                hitbox.offset.y -= pf::fxFromFloat(0.1f);
                editor.status = "Editor: lowered selected hitbox";
            }
            if (uiButton({516.0f, 638.0f, 44.0f, 22.0f}, "BKB-")) {
                hitbox.knockbackBase = std::max(pf::Fix{0}, hitbox.knockbackBase - pf::fx(5));
                editor.status = "Editor: decreased selected hitbox base knockback";
            }
            if (uiButton({566.0f, 638.0f, 44.0f, 22.0f}, "KBG-")) {
                hitbox.knockbackGrowth = std::max(pf::Fix{0}, hitbox.knockbackGrowth - pf::fx(5));
                editor.status = "Editor: decreased selected hitbox growth";
            }
            if (uiButton({616.0f, 638.0f, 44.0f, 22.0f}, "Gnd", hitbox.hitGrounded)) {
                hitbox.hitGrounded = !hitbox.hitGrounded;
                editor.status = "Editor: toggled selected hitbox grounded target flag";
            }
            if (uiButton({666.0f, 638.0f, 44.0f, 22.0f}, "Air", hitbox.hitAirborne)) {
                hitbox.hitAirborne = !hitbox.hitAirborne;
                editor.status = "Editor: toggled selected hitbox airborne target flag";
            }
            if (uiButton({616.0f, 660.0f, 44.0f, 22.0f}, "Grab", hitbox.isGrab)) {
                hitbox.isGrab = !hitbox.isGrab;
                editor.status = "Editor: toggled selected hitbox grab flag";
            }
        } else {
            DrawText(("Subaction #" + std::to_string(editor.selectedSubaction) + " " +
                      subactionTypeName(subaction.type) +
                      " frames " + std::to_string(subaction.frames)).c_str(), 516, 554, 12, DARKGRAY);
            if (uiButton({516.0f, 570.0f, 44.0f, 22.0f}, "Fr+")) {
                ++subaction.frames;
                editor.status = "Editor: lengthened selected subaction";
            }
            if (uiButton({566.0f, 570.0f, 44.0f, 22.0f}, "Fr-")) {
                subaction.frames = std::max(0, subaction.frames - 1);
                editor.status = "Editor: shortened selected subaction";
            }
            if (subaction.type == pf::SubactionType::SetInterruptible) {
                DrawText(("IASA marker " + std::to_string(subaction.interruptibleFrame)).c_str(), 516, 596, 12, DARKGRAY);
                if (uiButton({516.0f, 612.0f, 44.0f, 22.0f}, "Cur")) {
                    subaction.interruptibleFrame = -1;
                    editor.status = "Editor: selected current-frame interruptible marker";
                }
                if (uiButton({566.0f, 612.0f, 44.0f, 22.0f}, "IAS+")) {
                    subaction.interruptibleFrame = std::max(0, subaction.interruptibleFrame) + 1;
                    editor.status = "Editor: moved interruptible marker later";
                }
                if (uiButton({616.0f, 612.0f, 44.0f, 22.0f}, "IAS-")) {
                    subaction.interruptibleFrame = std::max(0, subaction.interruptibleFrame) - 1;
                    editor.status = "Editor: moved interruptible marker earlier";
                }
            }
        }
    }

    DrawText(("Hurtboxes: " + std::to_string(def.hurtboxes.size())).c_str(), 24, 506, 13, DARKGRAY);
    if (uiButton({122.0f, 500.0f, 72.0f, 24.0f}, "+ Hurt")) {
        pf::HurtboxDefinition hurtbox;
        hurtbox.bone = pf::BoneId::Hip;
        hurtbox.startOffset = {0, pf::fxFromFloat(-0.35f), 0};
        hurtbox.endOffset = {0, pf::fxFromFloat(0.45f), 0};
        hurtbox.radius = pf::fxFromFloat(0.35f);
        def.hurtboxes.push_back(hurtbox);
        editor.selectedHurtbox = static_cast<int>(def.hurtboxes.size()) - 1;
        editor.status = "Editor: added fighter hurtbox";
    }
    if (uiButton({202.0f, 500.0f, 72.0f, 24.0f}, "- Hurt")) {
        if (!def.hurtboxes.empty()) {
            def.hurtboxes.erase(def.hurtboxes.begin() + editor.selectedHurtbox);
            editor.selectedHurtbox = std::clamp(editor.selectedHurtbox, 0, std::max(0, static_cast<int>(def.hurtboxes.size()) - 1));
            editor.status = "Editor: removed fighter hurtbox";
        }
    }
    if (!def.hurtboxes.empty()) {
        editor.selectedHurtbox = std::clamp(editor.selectedHurtbox, 0, static_cast<int>(def.hurtboxes.size()) - 1);
        pf::HurtboxDefinition& hurtbox = def.hurtboxes[static_cast<size_t>(editor.selectedHurtbox)];
        const std::string hurtboxLabel = "#" + std::to_string(editor.selectedHurtbox) +
            " " + pf::boneName(hurtbox.bone) +
            " " + hurtboxStateName(hurtbox.state) +
            " r=" + std::to_string(pf::fxToFloat(hurtbox.radius)) +
            " x=(" + std::to_string(pf::fxToFloat(hurtbox.startOffset.x)) +
            "," + std::to_string(pf::fxToFloat(hurtbox.endOffset.x)) + ")" +
            " y=(" + std::to_string(pf::fxToFloat(hurtbox.startOffset.y)) +
            "," + std::to_string(pf::fxToFloat(hurtbox.endOffset.y)) + ")";
        DrawText(hurtboxLabel.c_str(), 24, 532, 13, DARKGRAY);
        if (uiButton({24.0f, 554.0f, 54.0f, 24.0f}, "Prev")) {
            --editor.selectedHurtbox;
        }
        if (uiButton({84.0f, 554.0f, 54.0f, 24.0f}, "Next")) {
            ++editor.selectedHurtbox;
        }
        if (uiButton({150.0f, 554.0f, 54.0f, 24.0f}, "+ Rad")) {
            hurtbox.radius += pf::fxFromFloat(0.05f);
            editor.status = "Editor: increased hurtbox radius";
        }
        if (uiButton({210.0f, 554.0f, 54.0f, 24.0f}, "- Rad")) {
            hurtbox.radius = std::max(pf::fxFromFloat(0.05f), hurtbox.radius - pf::fxFromFloat(0.05f));
            editor.status = "Editor: decreased hurtbox radius";
        }
        if (uiButton({276.0f, 554.0f, 54.0f, 24.0f}, "+ Tall")) {
            hurtbox.endOffset.y += pf::fxFromFloat(0.1f);
            editor.status = "Editor: stretched hurtbox upward";
        }
        if (uiButton({336.0f, 554.0f, 54.0f, 24.0f}, "- Tall")) {
            hurtbox.endOffset.y = std::max(hurtbox.startOffset.y + pf::fxFromFloat(0.1f), hurtbox.endOffset.y - pf::fxFromFloat(0.1f));
            editor.status = "Editor: shortened hurtbox";
        }
        if (uiButton({402.0f, 554.0f, 54.0f, 24.0f}, "Grab", hurtbox.grabbable)) {
            hurtbox.grabbable = !hurtbox.grabbable;
            editor.status = "Editor: toggled hurtbox grabbable";
        }
        if (uiButton({462.0f, 554.0f, 54.0f, 24.0f}, "Bone")) {
            hurtbox.bone = nextEditorBone(hurtbox.bone);
            editor.status = std::string("Editor: assigned hurtbox bone ") + pf::boneName(hurtbox.bone);
        }
        if (uiButton({522.0f, 554.0f, 54.0f, 24.0f}, "State")) {
            hurtbox.state = nextHurtboxState(hurtbox.state);
            editor.status = std::string("Editor: set hurtbox state ") + hurtboxStateName(hurtbox.state);
        }
        if (uiButton({582.0f, 554.0f, 54.0f, 24.0f}, "X+")) {
            hurtbox.startOffset.x += pf::fxFromFloat(0.1f);
            hurtbox.endOffset.x += pf::fxFromFloat(0.1f);
            editor.status = "Editor: moved hurtbox forward";
        }
        if (uiButton({642.0f, 554.0f, 54.0f, 24.0f}, "X-")) {
            hurtbox.startOffset.x -= pf::fxFromFloat(0.1f);
            hurtbox.endOffset.x -= pf::fxFromFloat(0.1f);
            editor.status = "Editor: moved hurtbox backward";
        }
    } else {
        DrawText("No authored hurtboxes", 24, 532, 13, GRAY);
    }

    DrawText(("Authored ECB: " + std::string(def.authoredEcb.enabled ? "on" : "off")).c_str(), 24, 590, 13, DARKGRAY);
    if (uiButton({122.0f, 584.0f, 72.0f, 24.0f}, "ECB", def.authoredEcb.enabled)) {
        def.authoredEcb.enabled = !def.authoredEcb.enabled;
        normalizeEditorAuthoredEcb(def);
        editor.status = def.authoredEcb.enabled ? "Editor: enabled authored ECB" : "Editor: disabled authored ECB";
        pf::calculateEcb(def, fighter, true);
    }
    if (def.authoredEcb.enabled) {
        normalizeEditorAuthoredEcb(def);
        const float halfWidth = pf::fxToFloat(def.authoredEcb.points[2].x);
        const float top = pf::fxToFloat(def.authoredEcb.points[1].y);
        const float side = pf::fxToFloat(def.authoredEcb.points[0].y);
        const float bottom = pf::fxToFloat(def.authoredEcb.points[3].y);
        DrawText(("w=" + std::to_string(halfWidth * 2.0f) +
                  " top=" + std::to_string(top) +
                  " side=" + std::to_string(side) +
                  " bot=" + std::to_string(bottom)).c_str(), 24, 620, 13, DARKGRAY);
        if (uiButton({202.0f, 584.0f, 54.0f, 24.0f}, "+ W")) {
            def.authoredEcb.points[0].x -= pf::fxFromFloat(0.05f);
            def.authoredEcb.points[2].x += pf::fxFromFloat(0.05f);
            normalizeEditorAuthoredEcb(def);
            pf::calculateEcb(def, fighter, true);
            editor.status = "Editor: widened authored ECB";
        }
        if (uiButton({262.0f, 584.0f, 54.0f, 24.0f}, "- W")) {
            def.authoredEcb.points[0].x = std::min(def.authoredEcb.points[0].x + pf::fxFromFloat(0.05f), -pf::fxFromFloat(0.1f));
            def.authoredEcb.points[2].x = std::max(def.authoredEcb.points[2].x - pf::fxFromFloat(0.05f), pf::fxFromFloat(0.1f));
            normalizeEditorAuthoredEcb(def);
            pf::calculateEcb(def, fighter, true);
            editor.status = "Editor: narrowed authored ECB";
        }
        if (uiButton({322.0f, 584.0f, 54.0f, 24.0f}, "+ Top")) {
            def.authoredEcb.points[1].y += pf::fxFromFloat(0.1f);
            normalizeEditorAuthoredEcb(def);
            pf::calculateEcb(def, fighter, true);
            editor.status = "Editor: raised authored ECB top";
        }
        if (uiButton({382.0f, 584.0f, 54.0f, 24.0f}, "- Top")) {
            def.authoredEcb.points[1].y = std::max(def.authoredEcb.points[3].y + pf::fxFromFloat(0.5f), def.authoredEcb.points[1].y - pf::fxFromFloat(0.1f));
            normalizeEditorAuthoredEcb(def);
            pf::calculateEcb(def, fighter, true);
            editor.status = "Editor: lowered authored ECB top";
        }
        if (uiButton({442.0f, 584.0f, 54.0f, 24.0f}, "Up")) {
            def.authoredEcb.points[3].y += pf::fxFromFloat(0.05f);
            normalizeEditorAuthoredEcb(def);
            pf::calculateEcb(def, fighter, true);
            editor.status = "Editor: raised authored ECB bottom";
        }
        if (uiButton({502.0f, 584.0f, 54.0f, 24.0f}, "Down")) {
            def.authoredEcb.points[3].y -= pf::fxFromFloat(0.05f);
            normalizeEditorAuthoredEcb(def);
            pf::calculateEcb(def, fighter, true);
            editor.status = "Editor: lowered authored ECB bottom";
        }
        if (uiButton({202.0f, 642.0f, 54.0f, 24.0f}, "+ Side")) {
            def.authoredEcb.points[0].y += pf::fxFromFloat(0.05f);
            def.authoredEcb.points[2].y += pf::fxFromFloat(0.05f);
            normalizeEditorAuthoredEcb(def);
            pf::calculateEcb(def, fighter, true);
            editor.status = "Editor: raised authored ECB side points";
        }
        if (uiButton({262.0f, 642.0f, 54.0f, 24.0f}, "- Side")) {
            def.authoredEcb.points[0].y -= pf::fxFromFloat(0.05f);
            def.authoredEcb.points[2].y -= pf::fxFromFloat(0.05f);
            normalizeEditorAuthoredEcb(def);
            pf::calculateEcb(def, fighter, true);
            editor.status = "Editor: lowered authored ECB side points";
        }
    }
}

static void drawEditorTestLabWorkspace(pf::World& world, pf::FighterEditor& editor) {
    editor.clampToWorld(world);
    if (world.fighters.size() < 2) {
        return;
    }
    pf::FighterRuntime& p1 = world.fighters[0];
    pf::FighterRuntime& p2 = world.fighters[1];
    const Rectangle panel{12.0f, 324.0f, 530.0f, 228.0f};
    DrawRectangleRec(panel, Fade(RAYWHITE, 0.58f));
    DrawRectangleLinesEx(panel, 1.0f, DARKGRAY);
    DrawText("Test Lab", 24, 336, 16, BLACK);
    DrawText((std::string("Mode: ") + (editor.testMode ? "live editor test" : "regular") +
              "  Objects: " + std::to_string(world.objects.size())).c_str(), 24, 360, 13, DARKGRAY);
    DrawText(("P1 pct " + std::to_string(pf::fxToFloat(p1.percent)) +
              "  P2 pct " + std::to_string(pf::fxToFloat(p2.percent))).c_str(), 24, 382, 13, DARKGRAY);

    if (uiButton({24.0f, 410.0f, 72.0f, 24.0f}, "Pause", editor.paused)) {
        editor.paused = !editor.paused;
        editor.status = editor.paused ? "Editor test: paused" : "Editor test: running";
    }
    if (uiButton({104.0f, 410.0f, 72.0f, 24.0f}, "Step")) {
        pf::tickWorld(world, {pf::InputFrame{}, pf::InputFrame{}});
        editor.paused = true;
        editor.status = "Editor test: stepped one simulation frame";
    }
    if (uiButton({184.0f, 410.0f, 72.0f, 24.0f}, "Reset")) {
        pf::resetTrainingFighter(world, 0, p1.fighterDef, {-pf::fx(2), 0}, 1);
        pf::resetTrainingFighter(world, 1, p2.fighterDef, {pf::fx(2), 0}, -1);
        world.objects.clear();
        editor.status = "Editor test: reset fighters and cleared objects";
        return;
    }
    if (uiButton({264.0f, 410.0f, 72.0f, 24.0f}, "P1 0%")) {
        p1.percent = 0;
        editor.status = "Editor test: reset P1 percent";
    }
    if (uiButton({344.0f, 410.0f, 72.0f, 24.0f}, "P2 0%")) {
        p2.percent = 0;
        editor.status = "Editor test: reset P2 percent";
    }
    if (uiButton({424.0f, 410.0f, 72.0f, 24.0f}, "P2 +10")) {
        p2.percent += pf::fx(10);
        editor.status = "Editor test: raised P2 percent";
    }

    auto moveFighter = [&](pf::FighterRuntime& fighter, pf::Fix dx, pf::Fix dy) {
        fighter.position.x += dx;
        fighter.position.y = std::max(pf::Fix{0}, fighter.position.y + dy);
        fighter.previousPosition = fighter.position;
        if (fighter.fighterDef >= 0 && fighter.fighterDef < static_cast<int>(world.fighterDefs.size())) {
            pf::calculateEcb(world.fighterDefs[static_cast<size_t>(fighter.fighterDef)], fighter, true);
        }
    };
    if (uiButton({24.0f, 442.0f, 72.0f, 24.0f}, "P2 Left")) {
        moveFighter(p2, -pf::fxFromFloat(0.5f), 0);
        editor.status = "Editor test: moved P2 left";
    }
    if (uiButton({104.0f, 442.0f, 72.0f, 24.0f}, "P2 Right")) {
        moveFighter(p2, pf::fxFromFloat(0.5f), 0);
        editor.status = "Editor test: moved P2 right";
    }
    if (uiButton({184.0f, 442.0f, 72.0f, 24.0f}, "P2 Up")) {
        moveFighter(p2, 0, pf::fxFromFloat(0.5f));
        editor.status = "Editor test: moved P2 upward";
    }
    if (uiButton({264.0f, 442.0f, 72.0f, 24.0f}, "P2 Down")) {
        moveFighter(p2, 0, -pf::fxFromFloat(0.5f));
        editor.status = "Editor test: moved P2 downward";
    }
    if (uiButton({344.0f, 442.0f, 72.0f, 24.0f}, "ClearObj")) {
        world.objects.clear();
        editor.status = "Editor test: cleared active objects";
    }

    if (!world.objectDefs.empty()) {
        editor.selectedObjectDef = std::clamp(editor.selectedObjectDef, 0, static_cast<int>(world.objectDefs.size()) - 1);
        const pf::GameObjectDefinition& object = world.objectDefs[static_cast<size_t>(editor.selectedObjectDef)];
        DrawText(("Selected object: " + object.name).c_str(), 24, 490, 13, DARKGRAY);
        if (uiButton({24.0f, 510.0f, 72.0f, 24.0f}, "Obj>")) {
            editor.selectedObjectDef = wrappedIndex(editor.selectedObjectDef + 1, static_cast<int>(world.objectDefs.size()));
            editor.status = "Editor test: selected next object";
        }
        if (uiButton({104.0f, 510.0f, 72.0f, 24.0f}, "Spawn")) {
            const pf::Vec2 position{
                p1.position.x + p1.facing * pf::fxFromFloat(1.1f),
                p1.position.y + pf::fxFromFloat(1.0f),
            };
            const pf::Vec2 velocity{p1.facing * pf::fxFromFloat(0.35f), pf::fxFromFloat(0.1f)};
            pf::spawnGameObject(world, object.name, 0, position, p1.facing, velocity);
            editor.status = "Editor test: spawned " + object.name;
        }
    } else {
        DrawText("Selected object: none", 24, 490, 13, GRAY);
    }
}

static void previewEditorSelectedState(pf::World& world, pf::FighterEditor& editor, const pf::FighterDefinition& def) {
    (void)world;
    if (def.states.empty()) {
        editor.status = "Editor: no state available to preview";
        return;
    }
    editor.selectedState = std::clamp(editor.selectedState, 0, static_cast<int>(def.states.size()) - 1);
    const pf::FighterState& selectedState = def.states[static_cast<size_t>(editor.selectedState)];
    editor.previewCacheFrame = 0;
    editor.previewCacheDirty = true;
    editor.paused = true;
    editor.status = "Editor: queued savestate preview for " + selectedState.name;
}

static void drawEditorStateBrowser(pf::World& world, const pf::FighterDefinition& def, pf::FighterEditor& editor) {
    constexpr int kVisibleRows = 5;
    constexpr float kPanelX = 560.0f;
    constexpr float kPanelY = 170.0f;
    constexpr float kPanelW = 286.0f;
    constexpr float kPanelH = 164.0f;
    const Rectangle panel{kPanelX, kPanelY, kPanelW, kPanelH};
    DrawRectangleRec(panel, Fade(RAYWHITE, 0.56f));
    DrawRectangleLinesEx(panel, 1.0f, DARKGRAY);
    DrawText("State Browser", static_cast<int>(kPanelX + 12.0f), static_cast<int>(kPanelY + 10.0f), 16, BLACK);

    const int stateCount = static_cast<int>(def.states.size());
    DrawText(
        (std::to_string(editor.selectedState + 1) + "/" + std::to_string(std::max(1, stateCount))).c_str(),
        static_cast<int>(kPanelX + kPanelW - 58.0f),
        static_cast<int>(kPanelY + 12.0f),
        13,
        DARKGRAY);
    if (stateCount <= 0) {
        DrawText("No states", static_cast<int>(kPanelX + 12.0f), static_cast<int>(kPanelY + 44.0f), 13, GRAY);
        return;
    }

    editor.selectedState = std::clamp(editor.selectedState, 0, stateCount - 1);
    const int maxStart = std::max(0, stateCount - kVisibleRows);
    const int start = std::clamp(editor.selectedState - kVisibleRows / 2, 0, maxStart);
    for (int row = 0; row < std::min(kVisibleRows, stateCount); ++row) {
        const int stateIndex = start + row;
        const pf::FighterState& state = def.states[static_cast<size_t>(stateIndex)];
        const std::string label = std::to_string(stateIndex) + " " + state.name +
            "  len " + std::to_string(state.animationLengthFrames) +
            "  sub " + std::to_string(state.action.size());
        if (uiListRow(
                {kPanelX + 12.0f, kPanelY + 38.0f + 24.0f * row, kPanelW - 24.0f, 22.0f},
                label,
                stateIndex == editor.selectedState))
        {
            editor.selectedState = stateIndex;
            editor.selectedSubaction = 0;
            editor.selectedInterrupt = 0;
            editor.status = "Editor: selected state " + state.name;
        }
    }

    if (uiButton({kPanelX + 12.0f, kPanelY + 136.0f, 74.0f, 22.0f}, "Prev")) {
        editor.selectedState = std::max(0, editor.selectedState - kVisibleRows);
        editor.selectedSubaction = 0;
        editor.selectedInterrupt = 0;
        editor.status = "Editor: paged state browser up";
    }
    if (uiButton({kPanelX + 92.0f, kPanelY + 136.0f, 74.0f, 22.0f}, "Next")) {
        editor.selectedState = std::min(stateCount - 1, editor.selectedState + kVisibleRows);
        editor.selectedSubaction = 0;
        editor.selectedInterrupt = 0;
        editor.status = "Editor: paged state browser down";
    }
    if (uiButton({kPanelX + 172.0f, kPanelY + 136.0f, 90.0f, 22.0f}, "Preview")) {
        previewEditorSelectedState(world, editor, def);
    }
}

static std::vector<int> editorSubactionFirstFrames(const std::vector<pf::Subaction>& action) {
    std::vector<int> frames(action.size(), -1);
    int currentFrame = 0;
    int loopCount = 0;
    size_t loopStart = 0;
    size_t index = 0;
    int safety = 0;
    while (index < action.size() && safety < 10000) {
        ++safety;
        const pf::Subaction& subaction = action[index];
        if (subaction.type == pf::SubactionType::SetLoop) {
            loopStart = index + 1;
            loopCount = std::max(0, subaction.loopCount - 1);
            ++index;
            continue;
        }
        if (subaction.type == pf::SubactionType::ExecuteLoop) {
            if (loopCount > 0) {
                index = loopStart;
                --loopCount;
            } else {
                ++index;
            }
            continue;
        }
        if (subaction.type == pf::SubactionType::SyncTimer) {
            currentFrame += subaction.frames;
            ++index;
            continue;
        }
        if (subaction.type == pf::SubactionType::AsyncTimer) {
            currentFrame = std::max(currentFrame, subaction.frames);
            ++index;
            continue;
        }
        if (frames[index] < 0) {
            frames[index] = currentFrame;
        }
        ++index;
    }
    return frames;
}

struct EditorWorkstationLayout {
    Rectangle toolStrip{};
    Rectangle leftBrowser{};
    Rectangle viewport{};
    Rectangle timeline{};
    Rectangle rightGraph{};
    Rectangle rightInspector{};
    Rectangle diagnostics{};
};

static std::string clippedText(std::string text, int fontSize, float maxWidth) {
    while (!text.empty() && MeasureText(text.c_str(), fontSize) > static_cast<int>(maxWidth)) {
        text.pop_back();
    }
    return text;
}

static std::string lowerAscii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

static bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    return lowerAscii(haystack).find(lowerAscii(needle)) != std::string::npos;
}

static const char* editorSelectionKindName(pf::FighterEditorSelectionKind kind) {
    switch (kind) {
    case pf::FighterEditorSelectionKind::State: return "State";
    case pf::FighterEditorSelectionKind::Subaction: return "Timeline subaction";
    case pf::FighterEditorSelectionKind::Interrupt: return "Interrupt window";
    case pf::FighterEditorSelectionKind::Callback: return "State callback";
    case pf::FighterEditorSelectionKind::ObjectCallback: return "Object callback";
    case pf::FighterEditorSelectionKind::Script: return "Script";
    case pf::FighterEditorSelectionKind::Instruction: return "Graph instruction";
    case pf::FighterEditorSelectionKind::Variable: return "Package variable";
    case pf::FighterEditorSelectionKind::Object: return "Object/article";
    case pf::FighterEditorSelectionKind::Animation: return "Animation";
    case pf::FighterEditorSelectionKind::Viewport: return "Viewport";
    }
    return "State";
}

static const char* packageScriptGraphNodeKindName(pf::PackageScriptGraphNodeKind kind) {
    switch (kind) {
    case pf::PackageScriptGraphNodeKind::Entry: return "Entry";
    case pf::PackageScriptGraphNodeKind::Instruction: return "Instruction";
    case pf::PackageScriptGraphNodeKind::Comment: return "Comment";
    }
    return "Node";
}

static const char* stateCallbackSlotName(pf::FighterEditorStateCallbackSlot slot) {
    switch (slot) {
    case pf::FighterEditorStateCallbackSlot::Enter: return "enter";
    case pf::FighterEditorStateCallbackSlot::Frame: return "frame";
    case pf::FighterEditorStateCallbackSlot::Landing: return "landing";
    case pf::FighterEditorStateCallbackSlot::Airborne: return "airborne";
    }
    return "callback";
}

static const std::vector<pf::FunctionCall>& stateCallbackCalls(
    const pf::FighterState& state,
    pf::FighterEditorStateCallbackSlot slot)
{
    switch (slot) {
    case pf::FighterEditorStateCallbackSlot::Enter: return state.onEnter;
    case pf::FighterEditorStateCallbackSlot::Frame: return state.onFrame;
    case pf::FighterEditorStateCallbackSlot::Landing: return state.onLanding;
    case pf::FighterEditorStateCallbackSlot::Airborne: return state.onAirborne;
    }
    return state.onFrame;
}

static std::vector<pf::FunctionCall> editedStateCallbackCalls(
    const pf::FighterState& state,
    pf::FighterEditorStateCallbackSlot slot)
{
    return stateCallbackCalls(state, slot);
}

static pf::FighterEditorStateCallbackSlot wrappedStateCallbackSlot(
    pf::FighterEditorStateCallbackSlot slot,
    int delta)
{
    constexpr int kSlotCount = 4;
    const int index = wrappedIndex(static_cast<int>(slot) + delta, kSlotCount);
    return static_cast<pf::FighterEditorStateCallbackSlot>(index);
}

static std::string packageScriptNameFromCallback(const pf::FunctionCall& call) {
    constexpr const char* prefix = "script:";
    constexpr size_t prefixLength = 7;
    if (call.name.rfind(prefix, 0) == 0) {
        return call.name.substr(prefixLength);
    }
    return {};
}

static const char* editorStateGroupFilterName(pf::FighterEditorStateGroupFilter filter) {
    switch (filter) {
    case pf::FighterEditorStateGroupFilter::All: return "All";
    case pf::FighterEditorStateGroupFilter::Ground: return "Ground";
    case pf::FighterEditorStateGroupFilter::Air: return "Air";
    case pf::FighterEditorStateGroupFilter::Attack: return "Attack";
    case pf::FighterEditorStateGroupFilter::Special: return "Special";
    case pf::FighterEditorStateGroupFilter::Throw: return "Throw";
    case pf::FighterEditorStateGroupFilter::Ledge: return "Ledge";
    case pf::FighterEditorStateGroupFilter::Damage: return "Damage";
    case pf::FighterEditorStateGroupFilter::Other: return "Other";
    }
    return "All";
}

static bool stateNameContainsAny(const std::string& lowerName, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (lowerName.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static pf::FighterEditorStateGroupFilter classifyEditorStateGroup(const pf::FighterState& state) {
    const std::string name = lowerAscii(state.name + " " + state.animation);
    if (stateNameContainsAny(name, {"damage", "down", "bury", "ice", "sleep", "capture", "rebound"})) {
        return pf::FighterEditorStateGroupFilter::Damage;
    }
    if (stateNameContainsAny(name, {"cliff", "ledge"})) {
        return pf::FighterEditorStateGroupFilter::Ledge;
    }
    if (stateNameContainsAny(name, {"throw", "catch", "capture"})) {
        return pf::FighterEditorStateGroupFilter::Throw;
    }
    if (stateNameContainsAny(name, {"special", "appeal"})) {
        return pf::FighterEditorStateGroupFilter::Special;
    }
    if (stateNameContainsAny(name, {"attack", "smash", "guardattack", "sword", "jab"})) {
        return pf::FighterEditorStateGroupFilter::Attack;
    }
    if (stateNameContainsAny(name, {"fall", "jump", "air", "escapeair", "pass"})) {
        return pf::FighterEditorStateGroupFilter::Air;
    }
    if (stateNameContainsAny(name, {"wait", "walk", "dash", "run", "turn", "squat", "landing", "guard", "escape", "ottotto", "missfoot"})) {
        return pf::FighterEditorStateGroupFilter::Ground;
    }
    return pf::FighterEditorStateGroupFilter::Other;
}

static EditorWorkstationLayout editorWorkstationLayout() {
    const float screenW = static_cast<float>(GetScreenWidth());
    const float screenH = static_cast<float>(GetScreenHeight());
    const float toolY = 44.0f;
    const float toolH = 38.0f;
    const float diagnosticsH = std::clamp(screenH * 0.16f, 116.0f, 168.0f);
    const float timelineH = std::clamp(screenH * 0.24f, 176.0f, 244.0f);
    float leftW = std::clamp(screenW * 0.18f, 210.0f, 320.0f);
    float rightW = std::clamp(screenW * 0.28f, 320.0f, 500.0f);
    if (leftW + rightW + 280.0f > screenW) {
        rightW = std::max(260.0f, screenW - leftW - 280.0f);
    }
    if (leftW + rightW + 280.0f > screenW) {
        leftW = std::max(180.0f, screenW - rightW - 280.0f);
    }
    const float contentY = toolY + toolH;
    const float contentBottom = screenH - diagnosticsH;
    const float centerW = std::max(240.0f, screenW - leftW - rightW);
    const float centerH = std::max(160.0f, contentBottom - contentY - timelineH);
    const float rightH = std::max(160.0f, contentBottom - contentY);
    const float minInspectorH = std::min(220.0f, std::max(120.0f, rightH - 150.0f));
    const float maxGraphH = std::max(150.0f, rightH - minInspectorH);
    const float graphH = std::clamp(rightH * 0.44f, 150.0f, maxGraphH);

    EditorWorkstationLayout out;
    out.toolStrip = {0.0f, toolY, screenW, toolH};
    out.leftBrowser = {0.0f, contentY, leftW, std::max(160.0f, contentBottom - contentY)};
    out.viewport = {leftW, contentY, centerW, centerH};
    out.timeline = {leftW, contentY + centerH, centerW, timelineH};
    out.rightGraph = {leftW + centerW, contentY, rightW, graphH};
    out.rightInspector = {leftW + centerW, contentY + graphH, rightW, std::max(120.0f, rightH - graphH)};
    out.diagnostics = {0.0f, screenH - diagnosticsH, screenW, diagnosticsH};
    return out;
}

static void drawPanelChrome(Rectangle rect, const std::string& title) {
    DrawRectangleRec(rect, {18, 23, 28, 226});
    DrawRectangleLinesEx(rect, 1.0f, {64, 75, 86, 255});
    DrawRectangleRec({rect.x, rect.y, rect.width, 24.0f}, {28, 35, 42, 242});
    DrawText(title.c_str(), static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 6.0f), 13, RAYWHITE);
}

static void drawWorkstationRow(Rectangle rect, const std::string& label, bool active, Color accent = BLUE) {
    const Vector2 mouse = GetMousePosition();
    const bool hovered = CheckCollisionPointRec(mouse, rect);
    DrawRectangleRec(rect, active ? Fade(accent, 0.46f) : (hovered ? Fade(RAYWHITE, 0.13f) : Fade(RAYWHITE, 0.05f)));
    DrawRectangleLinesEx(rect, 1.0f, active ? accent : Fade(DARKGRAY, 0.65f));
    DrawText(clippedText(label, 12, rect.width - 10.0f).c_str(), static_cast<int>(rect.x + 6.0f), static_cast<int>(rect.y + 7.0f), 12, RAYWHITE);
}

static pf::StageDefinition makeEditorPreviewStage(bool airborne) {
    pf::StageDefinition stage;
    stage.name = airborne ? "Editor Air Preview" : "Editor Flat Preview";
    stage.blastMin = {-pf::fx(250), -pf::fx(250)};
    stage.blastMax = {pf::fx(250), pf::fx(250)};
    if (!airborne) {
        pf::StageSegment floor;
        floor.start = {-pf::fx(120), 0};
        floor.end = {pf::fx(120), 0};
        floor.friction = pf::fx(1);
        floor.type = pf::SegmentType::Solid;
        floor.lineKind = pf::SegmentLineKind::Floor;
        stage.segments.push_back(floor);
    }
    return stage;
}

static bool editorPreviewStateStartsAirborne(const pf::FighterState& state) {
    const pf::FighterEditorStateGroupFilter group = classifyEditorStateGroup(state);
    if (group == pf::FighterEditorStateGroupFilter::Air || group == pf::FighterEditorStateGroupFilter::Ledge) {
        return true;
    }
    const std::string name = lowerAscii(state.name + " " + state.animation);
    return stateNameContainsAny(name, {"air", "fall", "jump", "cliff", "ledge"});
}

static int editorFindRuntimeFighterDef(const pf::World& world, const std::string& name, int fallback) {
    if (!name.empty()) {
        const auto found = std::find_if(world.fighterDefs.begin(), world.fighterDefs.end(), [&](const pf::FighterDefinition& def) {
            return def.name == name;
        });
        if (found != world.fighterDefs.end()) {
            return static_cast<int>(std::distance(world.fighterDefs.begin(), found));
        }
    }
    if (!world.fighterDefs.empty()) {
        return std::clamp(fallback, 0, static_cast<int>(world.fighterDefs.size()) - 1);
    }
    return -1;
}

static int editorPreviewFrameCount(
    const pf::FighterEditorSession& session,
    const pf::FighterState& state)
{
    pf::FighterEditorStateTimeline timeline;
    std::string timelineError;
    if (pf::buildEditorSessionStateTimeline(session, session.selectedState, timeline, &timelineError)) {
        return std::max(1, timeline.frameCount);
    }
    const pf::UnfoldedAction actionFrames = pf::unfoldAction(state.action);
    return std::max(1, std::max(state.animationLengthFrames, static_cast<int>(actionFrames.size())));
}

static void applyEditorPreviewFixture(pf::World& world, bool airborne) {
    if (world.fighters.empty()) {
        return;
    }
    pf::FighterRuntime& fighter = world.fighters[0];
    const pf::Vec2 position = airborne ? pf::Vec2{0, pf::fx(8)} : pf::Vec2{};
    fighter.position = position;
    fighter.previousPosition = position;
    fighter.grounded = !airborne;
    fighter.groundSegment = airborne || world.stage.segments.empty() ? -1 : 0;
    fighter.groundNormal = {0, pf::fx(1)};
    fighter.facing = 1;
    fighter.poseFacing = 1;
    fighter.fighterVelocity = {};
    fighter.knockbackVelocity = {};
    fighter.groundVelocity = {};
    fighter.groundKnockbackVelocity = {};
    fighter.ecb.floorIndex = fighter.groundSegment;
    fighter.previousEcb = fighter.ecb;
}

static bool restoreEditorPreviewFrame(pf::World& world, pf::FighterEditor& editor, int frame, std::string* error = nullptr) {
    if (!editor.previewCacheValid || editor.previewCacheFrames.empty()) {
        if (error) *error = "preview cache is empty";
        return false;
    }
    const int maxFrame = static_cast<int>(editor.previewCacheFrames.size()) - 1;
    editor.previewCacheFrame = std::clamp(frame, 0, maxFrame);
    world.stage = editor.previewCacheStage;
    pf::loadWorld(world, editor.previewCacheFrames[static_cast<size_t>(editor.previewCacheFrame)]);
    editor.selectedFighter = 0;
    editor.animationPreviewActive = true;
    return true;
}

static bool rebuildEditorPreviewCache(
    pf::World& world,
    pf::FighterEditor& editor,
    const pf::FighterEditorSession& session,
    int selectedFighterDef,
    std::string* error = nullptr)
{
    if (editor.testMode) {
        if (error) *error = "test mode owns the live world";
        return false;
    }
    if (world.fighters.empty()) {
        if (error) *error = "world has no preview fighter";
        return false;
    }
    if (session.package.fighters.empty()) {
        if (error) *error = "editor session has no package fighters";
        return false;
    }
    const int packageFighter = std::clamp(session.selectedFighter, 0, static_cast<int>(session.package.fighters.size()) - 1);
    const pf::FighterDefinition& def = session.package.fighters[static_cast<size_t>(packageFighter)];
    if (def.states.empty()) {
        if (error) *error = "selected package fighter has no states";
        return false;
    }
    const int selectedState = std::clamp(session.selectedState, 0, static_cast<int>(def.states.size()) - 1);
    const pf::FighterState& state = def.states[static_cast<size_t>(selectedState)];
    const int runtimeDef = editorFindRuntimeFighterDef(world, def.name, selectedFighterDef);
    if (runtimeDef < 0) {
        if (error) *error = "selected package fighter is not installed in the preview world";
        return false;
    }

    const bool airborne = editorPreviewStateStartsAirborne(state);
    pf::World previewWorld = world;
    previewWorld.stage = makeEditorPreviewStage(airborne);
    previewWorld.frame = 0;
    previewWorld.objects.clear();
    pf::resetTrainingFighter(previewWorld, 0, runtimeDef, airborne ? pf::Vec2{0, pf::fx(8)} : pf::Vec2{}, 1);
    if (previewWorld.fighters.size() > 1) {
        pf::resetTrainingFighter(previewWorld, 1, std::clamp(runtimeDef, 0, static_cast<int>(previewWorld.fighterDefs.size()) - 1), {pf::fx(40), airborne ? pf::fx(8) : 0}, -1);
    }
    applyEditorPreviewFixture(previewWorld, airborne);
    pf::changeFighterState(previewWorld, previewWorld.fighters[0], state.name, 0, pf::kDisableAnimationBlendFrames);
    applyEditorPreviewFixture(previewWorld, airborne);

    const int authoredFrameCount = editorPreviewFrameCount(session, state);
    constexpr int kMaxPreviewFrameCount = 600;
    const int frameCount = std::clamp(authoredFrameCount, 1, kMaxPreviewFrameCount);
    std::vector<pf::WorldSnapshot> frames;
    frames.reserve(static_cast<size_t>(frameCount + 1));
    std::vector<pf::InputFrame> inputs(previewWorld.fighters.size());
    for (int frame = 0; frame <= frameCount; ++frame) {
        pf::tickWorld(previewWorld, inputs);
        frames.push_back(pf::saveWorld(previewWorld));
    }

    editor.previewCacheStage = previewWorld.stage;
    editor.previewCacheFrames = std::move(frames);
    editor.previewCacheFighter = packageFighter;
    editor.previewCacheState = selectedState;
    editor.previewCacheFrameCount = frameCount;
    editor.previewCacheFrame = std::clamp(editor.previewCacheFrame, 0, frameCount);
    editor.previewCacheValid = true;
    editor.previewCacheDirty = false;
    editor.previewCacheMessage = std::string(airborne ? "air fixture" : "flat fixture") +
        ", " + std::to_string(frameCount + 1) + " cached frames" +
        (authoredFrameCount > kMaxPreviewFrameCount ? " (capped)" : "");
    if (!restoreEditorPreviewFrame(world, editor, editor.previewCacheFrame, error)) {
        return false;
    }
    editor.selectedState = selectedState;
    editor.status = "Editor: rebuilt savestate preview for " + state.name + " (" + editor.previewCacheMessage + ")";
    return true;
}

static void scrubEditorSelectedState(pf::World& world, pf::FighterEditor& editor, int frame) {
    editor.previewCacheFrame = std::max(0, frame);
    editor.paused = true;
    std::string error;
    if (restoreEditorPreviewFrame(world, editor, editor.previewCacheFrame, &error)) {
        editor.status = "Editor: restored preview frame " + std::to_string(editor.previewCacheFrame);
    } else {
        editor.previewCacheDirty = true;
        editor.status = "Editor: queued preview scrub to frame " + std::to_string(editor.previewCacheFrame);
    }
}

static void prepareEditorEditPreview(
    pf::World& world,
    pf::FighterEditor& editor,
    const pf::FighterEditorSession& session,
    int selectedFighterDef,
    bool advanceFrame)
{
    if (editor.testMode || world.fighters.empty()) {
        return;
    }
    const bool cacheTargetsSelection =
        editor.previewCacheValid &&
        editor.previewCacheFighter == session.selectedFighter &&
        editor.previewCacheState == session.selectedState;
    if (editor.previewCacheDirty || !cacheTargetsSelection) {
        std::string error;
        if (!rebuildEditorPreviewCache(world, editor, session, selectedFighterDef, &error)) {
            editor.previewCacheValid = false;
            editor.previewCacheMessage = error;
            editor.status = "Editor preview failed: " + error;
        }
        return;
    }
    if (advanceFrame && editor.previewCacheValid && !editor.previewCacheFrames.empty()) {
        int nextFrame = editor.previewCacheFrame + 1;
        if (nextFrame >= static_cast<int>(editor.previewCacheFrames.size())) {
            const int packageFighter = std::clamp(session.selectedFighter, 0, static_cast<int>(session.package.fighters.size()) - 1);
            const pf::FighterDefinition& def = session.package.fighters[static_cast<size_t>(packageFighter)];
            const int selectedState = std::clamp(session.selectedState, 0, std::max(0, static_cast<int>(def.states.size()) - 1));
            const bool loop = !def.states.empty() && def.states[static_cast<size_t>(selectedState)].loopAnimation;
            nextFrame = loop ? 0 : static_cast<int>(editor.previewCacheFrames.size()) - 1;
            if (!loop) {
                editor.paused = true;
            }
        }
        restoreEditorPreviewFrame(world, editor, nextFrame);
    } else {
        restoreEditorPreviewFrame(world, editor, editor.previewCacheFrame);
    }
}

static void drawWorldScene3D(
    const pf::World& world,
    const pf::FighterEditor& editor,
    bool editorEditMode)
{
    DrawGrid(40, 10.0f);
    if (!editorEditMode) {
        for (const pf::StageSegment& segment : world.stage.segments) {
            DrawLine3D(toRayGround(segment.start), toRayGround(segment.end), segment.type == pf::SegmentType::Solid ? BLACK : DARKGREEN);
        }
    }
    if (!editorEditMode && editor.showBoxes && editor.showLedgeBoxes && !world.fighters.empty()) {
        for (const pf::StageLedge& ledge : world.stage.ledges) {
            const Vector3 ledgePos = toRayGround(ledge.position);
            DrawSphere(ledgePos, 0.12f, PURPLE);
            DrawLine3D(ledgePos, {ledgePos.x + 0.45f * static_cast<float>(ledge.direction), ledgePos.y, ledgePos.z}, PURPLE);
            drawLedgeSnapSweep(world.fighterDefs[static_cast<size_t>(world.fighters[0].fighterDef)], world.fighters[0], ledge, Fade(ORANGE, 0.45f));
            if (world.fighters.size() > 1) {
                drawLedgeSnapSweep(world.fighterDefs[static_cast<size_t>(world.fighters[1].fighterDef)], world.fighters[1], ledge, Fade(SKYBLUE, 0.45f));
            }
        }
    }
    if (editorEditMode) {
        if (editor.selectedFighter >= 0 && editor.selectedFighter < static_cast<int>(world.fighters.size())) {
            drawFighter(world, world.fighters[static_cast<size_t>(editor.selectedFighter)], ORANGE, editor);
        }
    } else {
        if (!world.fighters.empty()) {
            drawFighter(world, world.fighters[0], ORANGE, editor);
        }
        if (world.fighters.size() > 1) {
            drawFighter(world, world.fighters[1], SKYBLUE, editor);
        }
        drawGameObjects(world, editor);
    }
    if (editorEditMode) {
        drawEditorSelectedAuthoredMeshVertex(world, editor);
        drawEditorSelectedAuthoredJoint(world, editor);
    }
}

static void syncEditorSelectionFromSession(pf::FighterEditor& editor, const pf::FighterEditorSession& session) {
    editor.selectedState = session.selectedState;
    editor.selectedSubaction = session.selectedSubaction;
    editor.selectedInterrupt = session.selectedInterrupt;
    editor.selectedPackageScript = session.selectedPackageScript;
    editor.selectedPackageInstruction = session.selectedPackageInstruction;
}

static void resetEditorSelectionForPackageFighter(pf::FighterEditor& editor, pf::FighterEditorSession& session) {
    session.selectedState = 0;
    session.selectedSubaction = 0;
    session.selectedInterrupt = 0;
    session.selectedPackageScript = 0;
    session.selectedPackageInstruction = 0;
    editor.selectedPackageVariable = 0;
    editor.selectedPackageGraphNode = 0;
    editor.selectedStateCallbackSlot = pf::FighterEditorStateCallbackSlot::Enter;
    editor.selectedStateCallback = 0;
    editor.selectedAnimationClip = 0;
    editor.selectedAnimationJoint = 0;
    editor.selectedAnimationTrack = 0;
    editor.selectedAnimationKey = 0;
    editor.animationScrubFrame = 0;
    editor.selectedAuthoredMeshVertex = 0;
    editor.selectedHurtbox = 0;
    editor.selectionKind = pf::FighterEditorSelectionKind::State;
    session.clamp();
    syncEditorSelectionFromSession(editor, session);
}

static pf::FighterDefinition* selectedEditorSessionFighter(pf::FighterEditorSession& session) {
    session.clamp();
    if (session.package.fighters.empty()) {
        return nullptr;
    }
    return &session.package.fighters[static_cast<size_t>(session.selectedFighter)];
}

static const pf::FighterDefinition* selectedEditorSessionFighter(const pf::FighterEditorSession& session) {
    if (session.package.fighters.empty()) {
        return nullptr;
    }
    const int fighterIndex = std::clamp(
        session.selectedFighter,
        0,
        static_cast<int>(session.package.fighters.size()) - 1);
    return &session.package.fighters[static_cast<size_t>(fighterIndex)];
}

static bool ensureEditorSessionFromWorld(
    const pf::World& world,
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    bool& sessionActive,
    int rootFighterDef,
    std::string* error = nullptr)
{
    if (sessionActive && session.rootFighter()) {
        syncEditorSelectionFromSession(editor, session);
        return true;
    }
    if (!pf::beginFighterEditorSessionFromWorld(world, rootFighterDef, session, error)) {
        sessionActive = false;
        return false;
    }
    session.selectedState = editor.selectedState;
    session.selectedSubaction = editor.selectedSubaction;
    session.selectedInterrupt = editor.selectedInterrupt;
    session.selectedPackageScript = editor.selectedPackageScript;
    session.selectedPackageInstruction = editor.selectedPackageInstruction;
    session.clamp();
    syncEditorSelectionFromSession(editor, session);
    sessionActive = true;
    return true;
}

static bool syncEditorSessionToWorld(
    pf::World& world,
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    int& selectedFighterDef,
    std::string* error = nullptr)
{
    const bool wasDirty = session.dirty;
    pf::FighterEditorPackageSnapshot snapshot;
    if (!pf::exportFighterEditorSessionPackage(session, snapshot, error)) {
        session.dirty = wasDirty;
        return false;
    }

    int installedRoot = -1;
    pf::FighterPackageDescriptor descriptor;
    if (!pf::installFighterPackageBytes(world, snapshot.bytes, &installedRoot, &descriptor, error)) {
        session.dirty = wasDirty;
        return false;
    }
    if (installedRoot < 0) {
        if (error) {
            *error = "editor session installed without a root fighter";
        }
        session.dirty = wasDirty;
        return false;
    }

    const pf::FighterDefinition* selectedSessionDef = selectedEditorSessionFighter(session);
    const std::string previewFighterName = selectedSessionDef ? selectedSessionDef->name : std::string{};
    int previewFighterDef = installedRoot;
    if (!previewFighterName.empty()) {
        for (int defIndex = 0; defIndex < static_cast<int>(world.fighterDefs.size()); ++defIndex) {
            if (world.fighterDefs[static_cast<size_t>(defIndex)].name == previewFighterName) {
                previewFighterDef = defIndex;
                break;
            }
        }
    }

    selectedFighterDef = installedRoot;
    editor.selectedFighter = std::clamp(editor.selectedFighter, 0, std::max(0, static_cast<int>(world.fighters.size()) - 1));
    if (!world.fighters.empty()) {
        pf::FighterRuntime& preview = world.fighters[static_cast<size_t>(editor.selectedFighter)];
        const pf::Vec2 position = preview.position;
        const int facing = preview.facing;
        pf::resetTrainingFighter(world, static_cast<size_t>(editor.selectedFighter), previewFighterDef, position, facing);
    }
    updateEditorPackageSummary(editor, descriptor);
    session.lastDescriptor = descriptor;
    session.lastBytes = snapshot.bytes;
    session.lastMessage = "OK";
    session.dirty = wasDirty;
    syncEditorSelectionFromSession(editor, session);
    return true;
}

static bool syncEditorSessionMutation(
    pf::World& world,
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    int& selectedFighterDef,
    const std::string& successMessage)
{
    std::string error;
    if (!syncEditorSessionToWorld(world, editor, session, selectedFighterDef, &error)) {
        updateEditorPackageFailure(editor, error);
        editor.status = "Editor session sync failed: " + error;
        return false;
    }
    editor.status = successMessage;
    editor.uiRefreshPending = true;
    editor.previewCacheDirty = true;
    return true;
}

static bool writeEditorPackageBytesToFile(
    const std::string& path,
    const std::vector<uint8_t>& bytes,
    std::string* error)
{
    if (path.empty()) {
        if (error) {
            *error = "editor package path is empty";
        }
        return false;
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        if (error) {
            *error = "failed to open editor package for writing";
        }
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        if (error) {
            *error = "failed to write editor package";
        }
        return false;
    }
    return true;
}

static bool readEditorPackageBytesFromFile(
    const std::string& path,
    std::vector<uint8_t>& bytes,
    std::string* error)
{
    if (path.empty()) {
        if (error) {
            *error = "editor package path is empty";
        }
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) {
            *error = "failed to open editor package for reading";
        }
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamsize size = file.tellg();
    if (size < 0) {
        if (error) {
            *error = "failed to measure editor package";
        }
        return false;
    }
    file.seekg(0, std::ios::beg);
    bytes.assign(static_cast<size_t>(size), uint8_t{0});
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!file) {
            if (error) {
                *error = "failed to read editor package";
            }
            return false;
        }
    }
    return true;
}

static void drawEditorToolStrip(
    pf::World& world,
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    int& selectedFighterDef,
    Rectangle rect)
{
    DrawRectangleRec(rect, {15, 19, 23, 246});
    DrawLine(0, static_cast<int>(rect.y + rect.height), GetScreenWidth(), static_cast<int>(rect.y + rect.height), {55, 66, 76, 255});
    DrawText("PFighter Workstation", 12, static_cast<int>(rect.y + 11.0f), 15, RAYWHITE);
    const EditorToolActionRects actions = editorToolActionRects();
    const std::array<pf::EditorWorkspace, 5> workspaces{
        pf::EditorWorkspace::Moveset,
        pf::EditorWorkspace::Logic,
        pf::EditorWorkspace::Assets,
        pf::EditorWorkspace::Animation,
        pf::EditorWorkspace::TestLab,
    };
    const float tabStart = 156.0f;
    const float tabGap = 6.0f;
    const float tabAvailable = actions.newState.x - tabStart - 12.0f;
    if (tabAvailable >= 264.0f) {
        const float tabWidth = std::clamp((tabAvailable - tabGap * 4.0f) / 5.0f, 48.0f, 90.0f);
        float x = tabStart;
        for (pf::EditorWorkspace workspace : workspaces) {
            const Rectangle tab{x, rect.y + 6.0f, tabWidth, 26.0f};
            if (uiButton(tab, workspaceName(workspace), editor.workspace == workspace)) {
                editor.workspace = workspace;
                editor.status = std::string("Editor workspace: ") + workspaceName(workspace);
            }
            x += tabWidth + tabGap;
        }
    } else {
        DrawText(workspaceName(editor.workspace), static_cast<int>(tabStart), static_cast<int>(rect.y + 11.0f), 13, Fade(RAYWHITE, 0.76f));
    }
    if (uiButton(actions.newState, "New")) {
        std::string error;
        int created = -1;
        if (pf::createEditorSessionState(session, {}, session.selectedState, &created, &error)) {
            const pf::FighterDefinition* edited = selectedEditorSessionFighter(session);
            const std::string stateName = edited && created >= 0 && created < static_cast<int>(edited->states.size())
                ? edited->states[static_cast<size_t>(created)].name
                : std::string{"state"};
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: created session state " + stateName);
        } else {
            editor.status = "Editor: create state failed: " + error;
        }
    }
    if (uiButton(actions.cloneState, "Clone")) {
        std::string error;
        int created = -1;
        if (pf::duplicateEditorSessionState(session, session.selectedState, &created, &error)) {
            const pf::FighterDefinition* edited = selectedEditorSessionFighter(session);
            const std::string stateName = edited && created >= 0 && created < static_cast<int>(edited->states.size())
                ? edited->states[static_cast<size_t>(created)].name
                : std::string{"state"};
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: cloned session state " + stateName);
        } else {
            editor.status = "Editor: clone state failed: " + error;
        }
    }
    if (uiButton(actions.deleteState, "Delete")) {
        std::string error;
        const pf::FighterDefinition* edited = selectedEditorSessionFighter(session);
        const std::string removed = edited && session.selectedState >= 0 && session.selectedState < static_cast<int>(edited->states.size())
            ? edited->states[static_cast<size_t>(session.selectedState)].name
            : std::string{"state"};
        if (pf::removeEditorSessionState(session, session.selectedState, {}, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed session state " + removed);
        } else {
            editor.status = "Editor: remove state failed: " + error;
        }
    }

    uiButton(actions.test, editor.testMode ? "Return" : "Test", editor.testMode);
    if (uiButton(actions.play, editor.paused ? "Play" : "Pause", !editor.paused)) {
        editor.paused = !editor.paused;
    }
    if (uiButton(actions.boxes, "Boxes", editor.showBoxes)) {
        editor.showBoxes = !editor.showBoxes;
    }
    if (uiButton(actions.side, "Side", editor.sideView)) {
        editor.sideView = !editor.sideView;
    }
}

static void drawEditorViewportOverlayControls(pf::FighterEditor& editor, Rectangle rect) {
    const float buttonW = 58.0f;
    const float buttonH = 22.0f;
    const float gap = 6.0f;
    float x = rect.x + 12.0f;
    float y = rect.y + 50.0f;
    const float right = rect.x + rect.width - 12.0f;
    auto toggleButton = [&](const char* label, bool& value) {
        if (x + buttonW > right) {
            x = rect.x + 12.0f;
            y += buttonH + gap;
        }
        if (uiButton({x, y, buttonW, buttonH}, label, value)) {
            value = !value;
            editor.selectionKind = pf::FighterEditorSelectionKind::Viewport;
        }
        x += buttonW + gap;
    };

    toggleButton("Debug", editor.showBoxes);
    toggleButton("Model", editor.showModel);
    toggleButton("Hit", editor.showHitboxes);
    toggleButton("Hurt", editor.showHurtboxes);
    toggleButton("ECB", editor.showEcb);
    toggleButton("Ledge", editor.showLedgeBoxes);
    toggleButton("Bones", editor.showSkeleton);
}

static bool packageInstructionTargetsFighterName(const pf::PackageScriptInstruction& instruction, const std::string& fighterName) {
    if (instruction.text != fighterName) {
        return false;
    }
    return instruction.op == pf::PackageScriptOp::SwitchFighterDefinition ||
        instruction.op == pf::PackageScriptOp::SpawnFighter ||
        instruction.op == pf::PackageScriptOp::SpawnFighterSetVar;
}

static const char* editorPackageFighterRoleName(const pf::FighterPackage& package, int fighterIndex) {
    if (fighterIndex <= 0 || fighterIndex >= static_cast<int>(package.fighters.size())) {
        return "root";
    }
    const std::string& fighterName = package.fighters[static_cast<size_t>(fighterIndex)].name;
    bool switchedTo = false;
    bool spawned = false;
    auto inspectScripts = [&](const std::vector<pf::PackageScript>& scripts) {
        for (const pf::PackageScript& script : scripts) {
            for (const pf::PackageScriptInstruction& instruction : script.instructions) {
                if (!packageInstructionTargetsFighterName(instruction, fighterName)) {
                    continue;
                }
                switchedTo = switchedTo || instruction.op == pf::PackageScriptOp::SwitchFighterDefinition;
                spawned = spawned ||
                    instruction.op == pf::PackageScriptOp::SpawnFighter ||
                    instruction.op == pf::PackageScriptOp::SpawnFighterSetVar;
            }
        }
    };
    for (const pf::FighterDefinition& fighter : package.fighters) {
        inspectScripts(fighter.packageScripts);
    }
    for (const pf::GameObjectDefinition& object : package.objects) {
        inspectScripts(object.packageScripts);
    }
    if (switchedTo) {
        return "transform";
    }
    if (spawned) {
        return "helper";
    }
    return "companion";
}

static void drawEditorStateBrowserWorkstation(
    pf::World& world,
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    int& selectedFighterDef,
    const pf::FighterDefinition& def,
    Rectangle rect)
{
    drawPanelChrome(rect, "States");
    const int packageFighterCount = static_cast<int>(session.package.fighters.size());
    const int packageFighterIndex = packageFighterCount <= 0
        ? 0
        : std::clamp(session.selectedFighter, 0, packageFighterCount - 1);
    DrawText(("Fighter " + std::to_string(packageFighterIndex + 1) + "/" + std::to_string(std::max(1, packageFighterCount))).c_str(),
        static_cast<int>(rect.x + 10.0f),
        static_cast<int>(rect.y + 34.0f),
        11,
        Fade(RAYWHITE, 0.68f));
    auto selectPackageFighter = [&](int fighterIndex) -> bool {
        if (session.package.fighters.empty()) {
            return false;
        }
        session.selectedFighter = wrappedIndex(fighterIndex, static_cast<int>(session.package.fighters.size()));
        resetEditorSelectionForPackageFighter(editor, session);
        std::string error;
        if (!syncEditorSessionToWorld(world, editor, session, selectedFighterDef, &error)) {
            updateEditorPackageFailure(editor, error);
            editor.status = "Editor: package fighter selection failed: " + error;
            return false;
        }
        editor.uiRefreshPending = true;
        editor.previewCacheDirty = true;
        const pf::FighterDefinition* selected = selectedEditorSessionFighter(session);
        editor.status = selected
            ? "Editor: selected package fighter " + selected->name
            : "Editor: selected package fighter";
        return true;
    };
    if (uiButton({rect.x + rect.width - 76.0f, rect.y + 30.0f, 30.0f, 22.0f}, "<")) {
        selectPackageFighter(packageFighterIndex - 1);
        return;
    }
    if (uiButton({rect.x + rect.width - 40.0f, rect.y + 30.0f, 30.0f, 22.0f}, ">")) {
        selectPackageFighter(packageFighterIndex + 1);
        return;
    }
    std::string renamedFighter;
    if (uiTextField({rect.x + 10.0f, rect.y + 56.0f, rect.width - 20.0f, 24.0f},
            "package-fighter-name",
            editor,
            def.name,
            renamedFighter,
            48))
    {
        std::string error;
        const std::string oldName = def.name;
        if (pf::renameEditorSessionPackageFighter(session, packageFighterIndex, renamedFighter, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: renamed package fighter " + oldName + " to " + renamedFighter);
            return;
        }
        editor.status = "Editor: rename package fighter failed: " + error;
    }
    const float fighterButtonY = rect.y + 86.0f;
    if (uiButton({rect.x + 10.0f, fighterButtonY, 58.0f, 22.0f}, "+Blank")) {
        std::string error;
        int added = -1;
        pf::FighterDefinition blank = pf::makeFighterEditorBlankDefinition(
            pf::uniqueEditorPackageFighterName(session.package, "BlankFighter"),
            def.properties.common);
        if (pf::addEditorSessionPackageFighter(session, blank, blank.name, &added, &error)) {
            resetEditorSelectionForPackageFighter(editor, session);
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added blank package fighter " + blank.name);
            return;
        }
        editor.status = "Editor: add package fighter failed: " + error;
    }
    if (uiButton({rect.x + 74.0f, fighterButtonY, 54.0f, 22.0f}, "Clone")) {
        std::string error;
        int added = -1;
        if (pf::duplicateEditorSessionPackageFighter(session, packageFighterIndex, {}, &added, &error)) {
            resetEditorSelectionForPackageFighter(editor, session);
            const pf::FighterDefinition* selected = selectedEditorSessionFighter(session);
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: cloned package fighter " + (selected ? selected->name : std::string{"fighter"}));
            return;
        }
        editor.status = "Editor: clone package fighter failed: " + error;
    }
    if (uiButton({rect.x + 134.0f, fighterButtonY, 54.0f, 22.0f}, "-Ftr")) {
        std::string error;
        const std::string removed = def.name;
        if (pf::removeEditorSessionPackageFighter(session, packageFighterIndex, {}, &error)) {
            resetEditorSelectionForPackageFighter(editor, session);
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed package fighter " + removed);
            return;
        }
        editor.status = "Editor: remove package fighter failed: " + error;
    }
    const float fighterListY = rect.y + 116.0f;
    DrawText("Package fighters", static_cast<int>(rect.x + 10.0f), static_cast<int>(fighterListY), 10, Fade(RAYWHITE, 0.62f));
    const int fighterRows = std::min(3, packageFighterCount);
    const int fighterStart = visibleListStart(packageFighterIndex, packageFighterCount, fighterRows);
    for (int row = 0; row < fighterRows; ++row) {
        const int fighterIndex = fighterStart + row;
        const pf::FighterDefinition& packageFighter = session.package.fighters[static_cast<size_t>(fighterIndex)];
        const Rectangle rowRect{
            rect.x + 10.0f,
            fighterListY + 16.0f + 25.0f * static_cast<float>(row),
            rect.width - 20.0f,
            22.0f,
        };
        const bool active = fighterIndex == packageFighterIndex;
        const std::string label = std::to_string(fighterIndex) + "  " + packageFighter.name + "  [" +
            editorPackageFighterRoleName(session.package, fighterIndex) + "]";
        drawWorkstationRow(rowRect, clippedText(label, 10, rowRect.width - 8.0f), active, SKYBLUE);
        if (CheckCollisionPointRec(GetMousePosition(), rowRect) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            selectPackageFighter(fighterIndex);
            return;
        }
    }
    const float searchY = fighterListY + 96.0f;
    std::string committedSearch;
    if (uiTextField({rect.x + 10.0f, searchY, rect.width - 20.0f, 24.0f}, "state-search", editor, editor.stateSearch, committedSearch, 48)) {
        editor.stateSearch = committedSearch;
    }
    if (editor.stateSearch.empty()) {
        DrawText("Search states", static_cast<int>(rect.x + 18.0f), static_cast<int>(searchY + 7.0f), 12, Fade(RAYWHITE, 0.55f));
    }
    const std::array<pf::FighterEditorStateGroupFilter, 9> groupFilters{
        pf::FighterEditorStateGroupFilter::All,
        pf::FighterEditorStateGroupFilter::Ground,
        pf::FighterEditorStateGroupFilter::Air,
        pf::FighterEditorStateGroupFilter::Attack,
        pf::FighterEditorStateGroupFilter::Special,
        pf::FighterEditorStateGroupFilter::Throw,
        pf::FighterEditorStateGroupFilter::Ledge,
        pf::FighterEditorStateGroupFilter::Damage,
        pf::FighterEditorStateGroupFilter::Other,
    };
    const auto currentGroupIt = std::find(groupFilters.begin(), groupFilters.end(), editor.stateGroupFilter);
    const int currentGroupIndex = currentGroupIt == groupFilters.end()
        ? 0
        : static_cast<int>(std::distance(groupFilters.begin(), currentGroupIt));
    const float groupY = searchY + 30.0f;
    if (uiButton({rect.x + 10.0f, groupY, 26.0f, 22.0f}, "<")) {
        editor.stateGroupFilter = groupFilters[static_cast<size_t>(wrappedIndex(currentGroupIndex - 1, static_cast<int>(groupFilters.size())))];
    }
    if (uiButton({rect.x + rect.width - 36.0f, groupY, 26.0f, 22.0f}, ">")) {
        editor.stateGroupFilter = groupFilters[static_cast<size_t>(wrappedIndex(currentGroupIndex + 1, static_cast<int>(groupFilters.size())))];
    }
    DrawText(("Group: " + std::string(editorStateGroupFilterName(editor.stateGroupFilter))).c_str(),
        static_cast<int>(rect.x + 44.0f),
        static_cast<int>(groupY + 6.0f),
        11,
        Fade(RAYWHITE, 0.68f));

    std::vector<int> visibleStates;
    for (int i = 0; i < static_cast<int>(def.states.size()); ++i) {
        const pf::FighterState& state = def.states[static_cast<size_t>(i)];
        const std::string searchText = state.name + " " + state.animation;
        const pf::FighterEditorStateGroupFilter group = classifyEditorStateGroup(state);
        if (containsCaseInsensitive(searchText, editor.stateSearch) &&
            (editor.stateGroupFilter == pf::FighterEditorStateGroupFilter::All || editor.stateGroupFilter == group))
        {
            visibleStates.push_back(i);
        }
    }

    const float stateListY = groupY + 30.0f;
    const int rowCount = std::max(1, static_cast<int>((rect.y + rect.height - stateListY - 34.0f) / 43.0f));
    int selectedVisible = 0;
    bool selectedStateVisible = false;
    for (int i = 0; i < static_cast<int>(visibleStates.size()); ++i) {
        if (visibleStates[static_cast<size_t>(i)] == editor.selectedState) {
            selectedVisible = i;
            selectedStateVisible = true;
            break;
        }
    }
    if (!visibleStates.empty() && !selectedStateVisible) {
        const pf::FighterState& firstState = def.states[static_cast<size_t>(visibleStates.front())];
        DrawText(clippedText("Current state hidden by filter", 10, rect.width - 100.0f).c_str(),
            static_cast<int>(rect.x + 12.0f),
            static_cast<int>(stateListY),
            10,
            Fade(YELLOW, 0.82f));
        if (uiButton({rect.x + rect.width - 88.0f, stateListY - 3.0f, 78.0f, 22.0f}, "Select")) {
            editor.selectedState = visibleStates.front();
            editor.selectedSubaction = 0;
            editor.selectedInterrupt = 0;
            editor.selectionKind = pf::FighterEditorSelectionKind::State;
            session.selectedState = editor.selectedState;
            session.selectedSubaction = 0;
            session.selectedInterrupt = 0;
            session.clamp();
            previewEditorSelectedState(world, editor, def);
            editor.status = "Editor: selected filtered state " + firstState.name;
            return;
        }
    }
    const int start = visibleListStart(selectedVisible, static_cast<int>(visibleStates.size()), rowCount);
    const float filteredOffsetY = (!visibleStates.empty() && !selectedStateVisible) ? 24.0f : 0.0f;
    for (int row = 0; row < std::min(rowCount, static_cast<int>(visibleStates.size())); ++row) {
        const int stateIndex = visibleStates[static_cast<size_t>(start + row)];
        const pf::FighterState& state = def.states[static_cast<size_t>(stateIndex)];
        const Rectangle rowRect{rect.x + 10.0f, stateListY + filteredOffsetY + 43.0f * static_cast<float>(row), rect.width - 20.0f, 36.0f};
        const bool active = stateIndex == editor.selectedState;
        drawWorkstationRow(rowRect, std::to_string(stateIndex) + "  " + state.name, active, SKYBLUE);
        DrawText((std::string(editorStateGroupFilterName(classifyEditorStateGroup(state))) + "  " +
                  std::to_string(state.animationLengthFrames) + "f  " + state.animation).c_str(),
            static_cast<int>(rowRect.x + 6.0f),
            static_cast<int>(rowRect.y + 21.0f),
            10,
            Fade(RAYWHITE, 0.68f));
        if (CheckCollisionPointRec(GetMousePosition(), rowRect) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            editor.selectedState = stateIndex;
            editor.selectedSubaction = 0;
            editor.selectedInterrupt = 0;
            editor.selectionKind = pf::FighterEditorSelectionKind::State;
            session.selectedState = stateIndex;
            session.selectedSubaction = 0;
            session.selectedInterrupt = 0;
            session.clamp();
            previewEditorSelectedState(world, editor, def);
            editor.status = "Editor: selected state " + state.name + " from browser";
        }
    }
    if (visibleStates.empty()) {
        DrawText("No matching states", static_cast<int>(rect.x + 12.0f), static_cast<int>(stateListY), 13, Fade(RAYWHITE, 0.65f));
    }
    DrawText((std::to_string(visibleStates.size()) + " shown / " + std::to_string(def.states.size()) + " total").c_str(),
        static_cast<int>(rect.x + 10.0f),
        static_cast<int>(rect.y + rect.height - 24.0f),
        11,
        Fade(RAYWHITE, 0.62f));
}

static int editorTimelineMarkerLane(const pf::FighterEditorTimelineMarker& marker) {
    switch (marker.kind) {
    case pf::FighterEditorTimelineMarkerKind::Hitbox:
    case pf::FighterEditorTimelineMarkerKind::ThrowHitbox:
        return 1;
    case pf::FighterEditorTimelineMarkerKind::Callback:
        return 3;
    case pf::FighterEditorTimelineMarkerKind::AnimationKey:
        return 4;
    case pf::FighterEditorTimelineMarkerKind::Interruptible:
    case pf::FighterEditorTimelineMarkerKind::InterruptEnable:
    case pf::FighterEditorTimelineMarkerKind::InterruptDisable:
        return 5;
    case pf::FighterEditorTimelineMarkerKind::Subaction:
        break;
    }
    if (marker.subactionType == pf::SubactionType::SetHurtboxState ||
        marker.subactionType == pf::SubactionType::SetBodyCollisionState)
    {
        return 2;
    }
    if (marker.subactionType == pf::SubactionType::CallScript) {
        return 3;
    }
    return 0;
}

static Color editorTimelineMarkerColor(const pf::FighterEditorTimelineMarker& marker) {
    switch (marker.kind) {
    case pf::FighterEditorTimelineMarkerKind::Hitbox:
        return RED;
    case pf::FighterEditorTimelineMarkerKind::ThrowHitbox:
        return PURPLE;
    case pf::FighterEditorTimelineMarkerKind::Callback:
        return VIOLET;
    case pf::FighterEditorTimelineMarkerKind::AnimationKey:
        return GOLD;
    case pf::FighterEditorTimelineMarkerKind::Interruptible:
        return LIME;
    case pf::FighterEditorTimelineMarkerKind::InterruptEnable:
        return GREEN;
    case pf::FighterEditorTimelineMarkerKind::InterruptDisable:
        return Fade(GREEN, 0.62f);
    case pf::FighterEditorTimelineMarkerKind::Subaction:
        break;
    }
    if (marker.subactionType == pf::SubactionType::SetHurtboxState ||
        marker.subactionType == pf::SubactionType::SetBodyCollisionState)
    {
        return SKYBLUE;
    }
    if (marker.subactionType == pf::SubactionType::CallScript) {
        return VIOLET;
    }
    return ORANGE;
}

static std::string editorTimelineMarkerLabel(const pf::FighterEditorTimelineMarker& marker) {
    switch (marker.kind) {
    case pf::FighterEditorTimelineMarkerKind::Hitbox:
        return "hitbox";
    case pf::FighterEditorTimelineMarkerKind::ThrowHitbox:
        return "throw hitbox";
    case pf::FighterEditorTimelineMarkerKind::Callback:
        return std::string(stateCallbackSlotName(marker.callbackSlot)) + " callback";
    case pf::FighterEditorTimelineMarkerKind::AnimationKey:
        return "animation key";
    case pf::FighterEditorTimelineMarkerKind::Interruptible:
        return "interruptible";
    case pf::FighterEditorTimelineMarkerKind::InterruptEnable:
        return std::string("interrupt enable ") + interruptConditionName(marker.interruptCondition);
    case pf::FighterEditorTimelineMarkerKind::InterruptDisable:
        return std::string("interrupt disable ") + interruptConditionName(marker.interruptCondition);
    case pf::FighterEditorTimelineMarkerKind::Subaction:
        break;
    }
    return subactionTypeName(marker.subactionType);
}

static void drawEditorTimelineWorkstation(
    pf::World& world,
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    int& selectedFighterDef,
    const pf::FighterRuntime& fighter,
    const pf::FighterDefinition& def,
    const pf::FighterState& state,
    Rectangle rect)
{
    drawPanelChrome(rect, "Timeline");
    pf::FighterEditorStateTimeline timeline;
    std::string timelineError;
    const bool hasTimeline = pf::buildEditorSessionStateTimeline(session, session.selectedState, timeline, &timelineError);
    const pf::UnfoldedAction actionFrames = pf::unfoldAction(state.action);
    const std::vector<int> fallbackSubactionFrames = editorSubactionFirstFrames(state.action);
    const std::vector<int>& subactionFrames = hasTimeline ? timeline.subactionFrames : fallbackSubactionFrames;
    const int frameCount = hasTimeline
        ? std::max(1, timeline.frameCount)
        : std::max(1, std::max(state.animationLengthFrames, static_cast<int>(actionFrames.size())));
    const int liveFrame = pf::currentState(world, fighter).name == state.name ? pf::frameInState(fighter) : 0;
    const int playheadFrame = editor.previewCacheValid && editor.previewCacheFighter == session.selectedFighter && editor.previewCacheState == session.selectedState
        ? editor.previewCacheFrame
        : liveFrame;
    const std::array<const char*, 6> lanes{"Subactions", "Hitboxes", "Hurtboxes", "Callbacks", "Anim Keys", "Interrupts"};
    const float headerY = rect.y + 32.0f;
    const float toolbarY = rect.y + 54.0f;
    const float rulerY = rect.y + 82.0f;
    const float laneTop = rect.y + 114.0f;
    const float footerY = rect.y + rect.height - 24.0f;
    const float availableLaneH = std::max(16.0f, footerY - laneTop - 4.0f);
    const float laneH = std::clamp(availableLaneH / static_cast<float>(lanes.size()), 18.0f, 24.0f);
    const float laneBarH = std::max(13.0f, laneH - 6.0f);
    const auto laneY = [&](int lane) {
        return laneTop + static_cast<float>(lane) * laneH;
    };
    const Rectangle ruler{rect.x + 136.0f, rulerY, rect.width - 156.0f, 24.0f};
    DrawText(("State: " + state.name).c_str(), static_cast<int>(rect.x + 10.0f), static_cast<int>(headerY), 13, RAYWHITE);
    DrawText(("Frame " + std::to_string(std::clamp(playheadFrame, 0, frameCount)) + " / " + std::to_string(frameCount)).c_str(),
        static_cast<int>(rect.x + 10.0f),
        static_cast<int>(headerY + 18.0f),
        11,
        Fade(RAYWHITE, 0.68f));
    if (!hasTimeline) {
        DrawText(("Timeline model error: " + timelineError).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(toolbarY + 4.0f),
            10,
            Fade(ORANGE, 0.88f));
    }
    auto addSubaction = [&](pf::Subaction subaction, const std::string& label) -> bool {
        std::string error;
        int added = -1;
        const int targetFrame = std::clamp(playheadFrame, 0, frameCount);
        if (pf::addEditorSessionSubactionAtFrame(session, session.selectedState, subaction, targetFrame, &added, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added " + label + " subaction at frame " + std::to_string(targetFrame));
            return true;
        }
        editor.status = "Editor: add subaction failed: " + error;
        return false;
    };
    const float actionX = std::max(rect.x + 236.0f, rect.x + rect.width - 450.0f);
    const float actionY = toolbarY;
    if (uiButton({actionX, actionY, 48.0f, 22.0f}, "+Sync")) {
        pf::Subaction subaction;
        subaction.type = pf::SubactionType::SyncTimer;
        subaction.frames = 5;
        if (addSubaction(subaction, "sync timer")) {
            editor.selectionKind = pf::FighterEditorSelectionKind::Subaction;
            return;
        }
    }
    if (uiButton({actionX + 54.0f, actionY, 42.0f, 22.0f}, "+Hit")) {
        pf::Subaction subaction;
        subaction.type = pf::SubactionType::CreateHitbox;
        subaction.frames = 3;
        subaction.hitbox.hitboxId = static_cast<int>(state.action.size());
        subaction.hitbox.damage = pf::fxFromFloat(3.0f);
        subaction.hitbox.radius = pf::fxFromFloat(0.45f);
        subaction.hitbox.offset = {pf::fxFromFloat(0.85f), pf::fxFromFloat(1.2f), 0};
        subaction.hitbox.knockbackAngleDegrees = pf::fx(45);
        subaction.hitbox.knockbackBase = pf::fx(20);
        subaction.hitbox.knockbackGrowth = pf::fx(60);
        if (addSubaction(subaction, "hitbox")) {
            editor.selectionKind = pf::FighterEditorSelectionKind::Subaction;
            return;
        }
    }
    if (uiButton({actionX + 102.0f, actionY, 48.0f, 22.0f}, "+Hurt")) {
        pf::Subaction subaction;
        subaction.type = pf::SubactionType::SetHurtboxState;
        subaction.frames = 1;
        subaction.hurtboxIndex = def.hurtboxes.empty()
            ? -1
            : std::clamp(editor.selectedHurtbox, 0, static_cast<int>(def.hurtboxes.size()) - 1);
        subaction.hurtboxState = pf::HurtboxState::Invincible;
        if (addSubaction(subaction, "hurtbox state")) {
            editor.selectionKind = pf::FighterEditorSelectionKind::Subaction;
            return;
        }
    }
    if (uiButton({actionX + 156.0f, actionY, 42.0f, 22.0f}, "+Clr")) {
        pf::Subaction subaction;
        subaction.type = pf::SubactionType::ClearHitboxes;
        subaction.frames = 1;
        if (addSubaction(subaction, "clear hitboxes")) {
            editor.selectionKind = pf::FighterEditorSelectionKind::Subaction;
            return;
        }
    }
    if (uiButton({actionX + 204.0f, actionY, 44.0f, 22.0f}, "+Call")) {
        if (def.packageScripts.empty()) {
            editor.status = "Editor: add a package script before adding script subactions";
        } else {
            pf::Subaction subaction;
            subaction.type = pf::SubactionType::CallScript;
            subaction.frames = 1;
            subaction.objectName = packageScriptTargetName(def.packageScripts, editor.selectedPackageScript);
            if (addSubaction(subaction, "script call")) {
                editor.selectionKind = pf::FighterEditorSelectionKind::Subaction;
                return;
            }
        }
    }
    if (uiButton({actionX + 254.0f, actionY, 44.0f, 22.0f}, "-Sub")) {
        std::string error;
        if (pf::removeEditorSessionSubaction(session, session.selectedState, editor.selectedSubaction, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed selected subaction");
            editor.selectionKind = state.action.empty() ? pf::FighterEditorSelectionKind::State : pf::FighterEditorSelectionKind::Subaction;
            return;
        }
        editor.status = "Editor: remove subaction failed: " + error;
    }
    if (uiButton({actionX + 304.0f, actionY, 40.0f, 22.0f}, "+Int")) {
        pf::InterruptRule interrupt;
        interrupt.targetState = "Wait";
        interrupt.condition = pf::InterruptCondition::WaitInput;
        interrupt.enableFrame = std::max(0, playheadFrame);
        interrupt.disableFrame = 0;
        std::string error;
        int added = -1;
        if (pf::addEditorSessionInterrupt(session, session.selectedState, interrupt, -1, &added, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added Wait interrupt");
            editor.selectionKind = pf::FighterEditorSelectionKind::Interrupt;
            return;
        }
        editor.status = "Editor: add interrupt failed: " + error;
    }
    if (uiButton({actionX + 350.0f, actionY, 40.0f, 22.0f}, "-Int")) {
        std::string error;
        if (pf::removeEditorSessionInterrupt(session, session.selectedState, editor.selectedInterrupt, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed selected interrupt");
            editor.selectionKind = state.interrupts.empty() ? pf::FighterEditorSelectionKind::State : pf::FighterEditorSelectionKind::Interrupt;
            return;
        }
        editor.status = "Editor: remove interrupt failed: " + error;
    }
    DrawRectangleRec(ruler, {26, 31, 36, 255});
    DrawRectangleLinesEx(ruler, 1.0f, {74, 86, 98, 255});
    const int stateLengthFrame = std::clamp(state.animationLengthFrames, 0, frameCount);
    const float stateLengthX = ruler.x + ruler.width * static_cast<float>(stateLengthFrame) / static_cast<float>(frameCount);
    DrawRectangleRec(
        {ruler.x, ruler.y + 26.0f, std::max(2.0f, stateLengthX - ruler.x), 6.0f},
        state.loopAnimation ? Fade(SKYBLUE, 0.72f) : Fade(RAYWHITE, 0.38f));
    if (state.animationLengthFrames < frameCount) {
        DrawRectangleRec(
            {stateLengthX, ruler.y + 26.0f, std::max(0.0f, ruler.x + ruler.width - stateLengthX), 6.0f},
            Fade(ORANGE, 0.28f));
    }
    DrawLine(static_cast<int>(stateLengthX), static_cast<int>(ruler.y + 22.0f), static_cast<int>(stateLengthX), static_cast<int>(footerY - 4.0f), state.loopAnimation ? SKYBLUE : Fade(RAYWHITE, 0.5f));
    DrawText((std::string(state.loopAnimation ? "loop " : "end ") + std::to_string(state.animationLengthFrames)).c_str(),
        static_cast<int>(std::min(ruler.x + ruler.width - 54.0f, stateLengthX + 4.0f)),
        static_cast<int>(ruler.y + 28.0f),
        9,
        state.loopAnimation ? SKYBLUE : Fade(RAYWHITE, 0.62f));
    for (int tick = 0; tick <= frameCount; tick += std::max(1, frameCount / 12)) {
        const float x = ruler.x + ruler.width * static_cast<float>(tick) / static_cast<float>(frameCount);
        DrawLine(static_cast<int>(x), static_cast<int>(ruler.y), static_cast<int>(x), static_cast<int>(footerY - 4.0f), Fade(RAYWHITE, 0.18f));
        DrawText(std::to_string(tick).c_str(), static_cast<int>(x + 3.0f), static_cast<int>(ruler.y + 6.0f), 9, Fade(RAYWHITE, 0.56f));
    }

    for (int lane = 0; lane < static_cast<int>(lanes.size()); ++lane) {
        const float y = laneY(lane);
        DrawText(lanes[static_cast<size_t>(lane)], static_cast<int>(rect.x + 10.0f), static_cast<int>(y + 5.0f), 11, Fade(RAYWHITE, 0.72f));
        DrawRectangleRec({ruler.x, y, ruler.width, laneBarH}, lane % 2 == 0 ? Fade(RAYWHITE, 0.06f) : Fade(RAYWHITE, 0.035f));
    }

    for (int frame = 0; frame < static_cast<int>(actionFrames.size()); ++frame) {
        for (const pf::Subaction& subaction : actionFrames[static_cast<size_t>(frame)]) {
            int lane = 0;
            if (subaction.type == pf::SubactionType::CreateHitbox || subaction.type == pf::SubactionType::CreateThrowHitbox) lane = 1;
            else if (subaction.type == pf::SubactionType::SetHurtboxState || subaction.type == pf::SubactionType::SetBodyCollisionState) lane = 2;
            else if (subaction.type == pf::SubactionType::CallScript) lane = 3;
            else if (subaction.type == pf::SubactionType::SetInterruptible) lane = 5;
            const float x = ruler.x + ruler.width * static_cast<float>(frame) / static_cast<float>(frameCount);
            const float y = laneY(lane);
            DrawRectangle(static_cast<int>(x), static_cast<int>(y + 2.0f), 3, static_cast<int>(std::max(10.0f, laneBarH - 4.0f)), subactionMarkerColor(subaction));
        }
    }
    int hoveredMarker = -1;
    const Vector2 mouse = GetMousePosition();
    for (int interruptIndex = 0; interruptIndex < static_cast<int>(state.interrupts.size()); ++interruptIndex) {
        const pf::InterruptRule& rule = state.interrupts[static_cast<size_t>(interruptIndex)];
        const int startFrame = std::clamp(rule.enableFrame, 0, frameCount);
        const int endFrame = rule.disableFrame > 0 ? std::clamp(rule.disableFrame, startFrame, frameCount) : frameCount;
        const float x = ruler.x + ruler.width * static_cast<float>(startFrame) / static_cast<float>(frameCount);
        const float w = std::max(3.0f, ruler.width * static_cast<float>(endFrame - startFrame) / static_cast<float>(frameCount));
        DrawRectangleRec({x, laneY(5) + 3.0f, w, std::max(10.0f, laneBarH - 6.0f)}, Fade(GREEN, interruptIndex == editor.selectedInterrupt ? 0.72f : 0.42f));
    }
    if (state.initialInterruptibleFrame > 0) {
        const float x = ruler.x + ruler.width * static_cast<float>(std::clamp(state.initialInterruptibleFrame, 0, frameCount)) / static_cast<float>(frameCount);
        DrawLine(static_cast<int>(x), static_cast<int>(ruler.y + 24.0f), static_cast<int>(x), static_cast<int>(footerY - 4.0f), GREEN);
    }
    if (!state.action.empty() && editor.selectedSubaction >= 0 && editor.selectedSubaction < static_cast<int>(subactionFrames.size())) {
        const int frame = subactionFrames[static_cast<size_t>(editor.selectedSubaction)];
        if (frame >= 0) {
            const float x = ruler.x + ruler.width * static_cast<float>(std::clamp(frame, 0, frameCount)) / static_cast<float>(frameCount);
            DrawRectangle(static_cast<int>(x - 2.0f), static_cast<int>(ruler.y + 23.0f), 5, static_cast<int>(std::max(18.0f, footerY - ruler.y - 28.0f)), SKYBLUE);
        }
    }
    if (hasTimeline) {
        int closestMarkerDistance = 1000000;
        for (int markerIndex = 0; markerIndex < static_cast<int>(timeline.markers.size()); ++markerIndex) {
            const pf::FighterEditorTimelineMarker& marker = timeline.markers[static_cast<size_t>(markerIndex)];
            const int lane = editorTimelineMarkerLane(marker);
            if (lane < 0 || lane >= static_cast<int>(lanes.size())) {
                continue;
            }
            const int markerFrame = std::clamp(marker.frame, 0, frameCount);
            const float x = ruler.x + ruler.width * static_cast<float>(markerFrame) / static_cast<float>(frameCount);
            const float y = laneY(lane);
            bool selectedCallbackScript = false;
            if (marker.kind == pf::FighterEditorTimelineMarkerKind::Callback &&
                editor.selectionKind == pf::FighterEditorSelectionKind::Callback)
            {
                selectedCallbackScript =
                    marker.callbackSlot == editor.selectedStateCallbackSlot &&
                    marker.sourceIndex == editor.selectedStateCallback;
            }
            const bool selectedAnimationKey =
                marker.kind == pf::FighterEditorTimelineMarkerKind::AnimationKey &&
                editor.selectionKind == pf::FighterEditorSelectionKind::Animation &&
                marker.animationClipIndex == editor.selectedAnimationClip &&
                marker.animationTrackIndex == editor.selectedAnimationTrack &&
                marker.animationKeyIndex == editor.selectedAnimationKey;
            const bool selected =
                ((marker.kind == pf::FighterEditorTimelineMarkerKind::InterruptEnable ||
                     marker.kind == pf::FighterEditorTimelineMarkerKind::InterruptDisable) &&
                    editor.selectionKind == pf::FighterEditorSelectionKind::Interrupt &&
                    marker.sourceIndex == editor.selectedInterrupt) ||
                selectedCallbackScript ||
                selectedAnimationKey ||
                ((marker.kind == pf::FighterEditorTimelineMarkerKind::Subaction ||
                     marker.kind == pf::FighterEditorTimelineMarkerKind::Hitbox ||
                     marker.kind == pf::FighterEditorTimelineMarkerKind::ThrowHitbox ||
                     (marker.kind == pf::FighterEditorTimelineMarkerKind::Interruptible && marker.sourceIndex >= 0)) &&
                    editor.selectionKind == pf::FighterEditorSelectionKind::Subaction &&
                    marker.sourceIndex == editor.selectedSubaction);
            const Rectangle markerRect{x - 4.0f, y + 1.0f, 8.0f, std::max(12.0f, laneBarH - 2.0f)};
            const bool hovered = CheckCollisionPointRec(mouse, markerRect);
            if (hovered) {
                const int distance = std::abs(markerFrame - playheadFrame);
                if (distance < closestMarkerDistance) {
                    hoveredMarker = markerIndex;
                    closestMarkerDistance = distance;
                }
            }
            const Color color = editorTimelineMarkerColor(marker);
            DrawRectangleRec(markerRect, Fade(color, selected ? 0.95f : 0.72f));
            DrawRectangleLinesEx(markerRect, selected ? 2.0f : 1.0f, selected ? RAYWHITE : Fade(RAYWHITE, 0.38f));
        }
    }
    const float playheadX = ruler.x + ruler.width * static_cast<float>(std::clamp(playheadFrame, 0, frameCount)) / static_cast<float>(frameCount);
    DrawRectangle(static_cast<int>(playheadX - 1.0f), static_cast<int>(ruler.y), 3, static_cast<int>(std::max(24.0f, footerY - ruler.y)), BLUE);
    if (hoveredMarker >= 0 && hoveredMarker < static_cast<int>(timeline.markers.size())) {
        const pf::FighterEditorTimelineMarker& marker = timeline.markers[static_cast<size_t>(hoveredMarker)];
        std::string markerText = editorTimelineMarkerLabel(marker) + " @ frame " + std::to_string(marker.frame);
        if (marker.kind == pf::FighterEditorTimelineMarkerKind::Callback) {
            const std::vector<pf::FunctionCall>& calls = stateCallbackCalls(state, marker.callbackSlot);
            if (marker.sourceIndex >= 0 && marker.sourceIndex < static_cast<int>(calls.size())) {
                markerText += " " + calls[static_cast<size_t>(marker.sourceIndex)].name;
            }
        } else if (marker.kind == pf::FighterEditorTimelineMarkerKind::AnimationKey) {
            markerText += " clip " + std::to_string(marker.animationClipIndex) +
                " track " + std::to_string(marker.animationTrackIndex) +
                " key " + std::to_string(marker.animationKeyIndex);
        }
        DrawText(markerText.c_str(),
            static_cast<int>(ruler.x),
            static_cast<int>(footerY),
            11,
            Fade(RAYWHITE, 0.78f));
    } else if (editor.selectionKind == pf::FighterEditorSelectionKind::Subaction &&
               editor.selectedSubaction >= 0 &&
               editor.selectedSubaction < static_cast<int>(state.action.size()) &&
               editor.selectedSubaction < static_cast<int>(subactionFrames.size()))
    {
        DrawText(("Selected subaction #" + std::to_string(editor.selectedSubaction) + " " +
                  subactionTypeName(state.action[static_cast<size_t>(editor.selectedSubaction)].type) +
                  " @ frame " + std::to_string(subactionFrames[static_cast<size_t>(editor.selectedSubaction)])).c_str(),
            static_cast<int>(ruler.x),
            static_cast<int>(footerY),
            11,
            Fade(RAYWHITE, 0.72f));
    } else if (editor.selectionKind == pf::FighterEditorSelectionKind::Interrupt &&
               editor.selectedInterrupt >= 0 &&
               editor.selectedInterrupt < static_cast<int>(state.interrupts.size()))
    {
        const pf::InterruptRule& interrupt = state.interrupts[static_cast<size_t>(editor.selectedInterrupt)];
        DrawText(("Selected interrupt #" + std::to_string(editor.selectedInterrupt) + " " +
                  interruptConditionName(interrupt.condition) + " -> " + interrupt.targetState).c_str(),
            static_cast<int>(ruler.x),
            static_cast<int>(footerY),
            11,
            Fade(RAYWHITE, 0.72f));
    }
    if (CheckCollisionPointRec(mouse, {ruler.x, ruler.y, ruler.width, std::max(24.0f, footerY - ruler.y)}) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        const float t = std::clamp((mouse.x - ruler.x) / ruler.width, 0.0f, 1.0f);
        const int clickedFrame = static_cast<int>(std::round(t * static_cast<float>(frameCount)));
        const float clickedY = mouse.y;
        const int clickedLane = static_cast<int>((clickedY - laneTop) / laneH);
        if (hasTimeline) {
            int bestMarker = -1;
            int bestDistance = 1000000;
            for (int markerIndex = 0; markerIndex < static_cast<int>(timeline.markers.size()); ++markerIndex) {
                const pf::FighterEditorTimelineMarker& marker = timeline.markers[static_cast<size_t>(markerIndex)];
                if (editorTimelineMarkerLane(marker) != clickedLane) {
                    continue;
                }
                const int markerFrame = std::clamp(marker.frame, 0, frameCount);
                const int distance = std::abs(markerFrame - clickedFrame);
                if (distance < bestDistance) {
                    bestMarker = markerIndex;
                    bestDistance = distance;
                }
            }
            const int markerPickTolerance = std::max(1, frameCount / 64);
            if (bestMarker >= 0 && bestDistance <= markerPickTolerance) {
                const pf::FighterEditorTimelineMarker& marker = timeline.markers[static_cast<size_t>(bestMarker)];
                if (marker.kind == pf::FighterEditorTimelineMarkerKind::InterruptEnable ||
                    marker.kind == pf::FighterEditorTimelineMarkerKind::InterruptDisable)
                {
                    if (marker.sourceIndex >= 0 && marker.sourceIndex < static_cast<int>(state.interrupts.size())) {
                        editor.selectedInterrupt = marker.sourceIndex;
                        session.selectedInterrupt = marker.sourceIndex;
                        editor.selectionKind = pf::FighterEditorSelectionKind::Interrupt;
                    }
                } else if (marker.kind == pf::FighterEditorTimelineMarkerKind::Callback) {
                    const std::vector<pf::FunctionCall>& calls = stateCallbackCalls(state, marker.callbackSlot);
                    if (marker.sourceIndex >= 0 && marker.sourceIndex < static_cast<int>(calls.size())) {
                        editor.selectedStateCallbackSlot = marker.callbackSlot;
                        editor.selectedStateCallback = marker.sourceIndex;
                        editor.selectionKind = pf::FighterEditorSelectionKind::Callback;
                        const std::string scriptName = packageScriptNameFromCallback(calls[static_cast<size_t>(marker.sourceIndex)]);
                        const auto scriptIt = std::find_if(def.packageScripts.begin(), def.packageScripts.end(), [&](const pf::PackageScript& script) {
                            return script.name == scriptName;
                        });
                        if (scriptIt != def.packageScripts.end()) {
                            editor.selectedPackageScript = static_cast<int>(std::distance(def.packageScripts.begin(), scriptIt));
                            editor.selectedPackageInstruction = 0;
                            editor.selectedPackageGraphNode = 0;
                            session.selectedPackageScript = editor.selectedPackageScript;
                            session.selectedPackageInstruction = 0;
                        } else {
                            editor.status = "Editor: callback marker references non-package callback " + calls[static_cast<size_t>(marker.sourceIndex)].name;
                        }
                    }
                } else if (marker.kind == pf::FighterEditorTimelineMarkerKind::AnimationKey) {
                    if (marker.animationClipIndex >= 0 &&
                        marker.animationClipIndex < static_cast<int>(def.authoredClips.size()))
                    {
                        editor.selectedAnimationClip = marker.animationClipIndex;
                        const pf::AnimationClip& clip = def.authoredClips[static_cast<size_t>(marker.animationClipIndex)];
                        if (marker.animationTrackIndex >= 0 &&
                            marker.animationTrackIndex < static_cast<int>(clip.tracks.size()))
                        {
                            editor.selectedAnimationTrack = marker.animationTrackIndex;
                            const pf::AnimationTrack& track = clip.tracks[static_cast<size_t>(marker.animationTrackIndex)];
                            if (marker.animationKeyIndex >= 0 &&
                                marker.animationKeyIndex < static_cast<int>(track.keys.size()))
                            {
                                editor.selectedAnimationKey = marker.animationKeyIndex;
                                editor.animationScrubFrame = static_cast<int>(std::round(pf::fxToFloat(track.keys[static_cast<size_t>(marker.animationKeyIndex)].frame)));
                            }
                        }
                        editor.selectionKind = pf::FighterEditorSelectionKind::Animation;
                    }
                } else if (marker.sourceIndex >= 0 && marker.sourceIndex < static_cast<int>(state.action.size())) {
                    editor.selectedSubaction = marker.sourceIndex;
                    session.selectedSubaction = marker.sourceIndex;
                    editor.selectionKind = pf::FighterEditorSelectionKind::Subaction;
                    const pf::Subaction& selected = state.action[static_cast<size_t>(marker.sourceIndex)];
                    if (selected.type == pf::SubactionType::SetHurtboxState &&
                        selected.hurtboxIndex >= 0 &&
                        selected.hurtboxIndex < static_cast<int>(def.hurtboxes.size()))
                    {
                        editor.selectedHurtbox = selected.hurtboxIndex;
                    }
                } else {
                    editor.selectionKind = pf::FighterEditorSelectionKind::State;
                }
                session.selectedState = editor.selectedState;
                session.clamp();
                scrubEditorSelectedState(world, editor, clickedFrame);
                return;
            }
        }
        if (clickedLane == 5) {
            int bestInterrupt = -1;
            int bestDistance = 1000000;
            for (int interruptIndex = 0; interruptIndex < static_cast<int>(state.interrupts.size()); ++interruptIndex) {
                const pf::InterruptRule& rule = state.interrupts[static_cast<size_t>(interruptIndex)];
                const int startFrame = std::clamp(rule.enableFrame, 0, frameCount);
                const int endFrame = rule.disableFrame > 0 ? std::clamp(rule.disableFrame, startFrame, frameCount) : frameCount;
                if (clickedFrame >= startFrame && clickedFrame <= endFrame) {
                    const int distance = std::min(std::abs(clickedFrame - startFrame), std::abs(clickedFrame - endFrame));
                    if (distance < bestDistance) {
                        bestInterrupt = interruptIndex;
                        bestDistance = distance;
                    }
                }
            }
            if (bestInterrupt >= 0) {
                editor.selectedInterrupt = bestInterrupt;
                session.selectedInterrupt = bestInterrupt;
                editor.selectionKind = pf::FighterEditorSelectionKind::Interrupt;
                session.selectedState = editor.selectedState;
                session.clamp();
                scrubEditorSelectedState(world, editor, clickedFrame);
                return;
            }
        }
        int best = -1;
        int bestDistance = 1000000;
        for (int i = 0; i < static_cast<int>(subactionFrames.size()); ++i) {
            if (subactionFrames[static_cast<size_t>(i)] < 0) continue;
            const int distance = std::abs(subactionFrames[static_cast<size_t>(i)] - clickedFrame);
            if (distance < bestDistance) {
                best = i;
                bestDistance = distance;
            }
        }
        if (best >= 0) {
            editor.selectedSubaction = best;
            session.selectedSubaction = best;
            editor.selectionKind = pf::FighterEditorSelectionKind::Subaction;
        } else {
            editor.selectionKind = pf::FighterEditorSelectionKind::State;
        }
        session.selectedState = editor.selectedState;
        session.clamp();
        scrubEditorSelectedState(world, editor, clickedFrame);
    }
}

static void drawEditorPackageVariableStrip(
    pf::World& world,
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    int& selectedFighterDef,
    pf::FighterDefinition& def,
    Rectangle rect)
{
    DrawText("Variables", static_cast<int>(rect.x), static_cast<int>(rect.y), 10, Fade(RAYWHITE, 0.62f));
    if (def.packageVariables.empty()) {
        DrawText("No package variables", static_cast<int>(rect.x), static_cast<int>(rect.y + 22.0f), 11, Fade(RAYWHITE, 0.55f));
        if (uiButton({rect.x + rect.width - 62.0f, rect.y + 16.0f, 58.0f, 22.0f}, "+Var")) {
            std::string error;
            int added = -1;
            if (pf::addEditorSessionPackageVariable(session, {}, 0, &added, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added package variable");
                editor.selectedPackageVariable = added;
                editor.selectionKind = pf::FighterEditorSelectionKind::Variable;
            } else {
                editor.status = "Editor: add variable failed: " + error;
            }
        }
        return;
    }

    editor.selectedPackageVariable = std::clamp(editor.selectedPackageVariable, 0, static_cast<int>(def.packageVariables.size()) - 1);
    pf::PackageVariableDefinition& variable = def.packageVariables[static_cast<size_t>(editor.selectedPackageVariable)];
    if (uiButton({rect.x, rect.y + 16.0f, 26.0f, 22.0f}, "<")) {
        editor.selectedPackageVariable = wrappedIndex(editor.selectedPackageVariable - 1, static_cast<int>(def.packageVariables.size()));
        editor.selectionKind = pf::FighterEditorSelectionKind::Variable;
    }
    if (uiButton({rect.x + 32.0f, rect.y + 16.0f, 26.0f, 22.0f}, ">")) {
        editor.selectedPackageVariable = wrappedIndex(editor.selectedPackageVariable + 1, static_cast<int>(def.packageVariables.size()));
        editor.selectionKind = pf::FighterEditorSelectionKind::Variable;
    }
    const float nameX = rect.x + 64.0f;
    const float nameW = std::max(94.0f, rect.width - 324.0f);
    std::string renamedVariable;
    if (uiTextField({nameX, rect.y + 15.0f, nameW, 24.0f},
            "workstation-package-variable-name",
            editor,
            variable.name,
            renamedVariable,
            48))
    {
        std::string error;
        const std::string oldName = variable.name;
        if (pf::renameEditorSessionPackageVariable(session, editor.selectedPackageVariable, renamedVariable, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: renamed package variable " + oldName + " to " + renamedVariable);
            editor.selectionKind = pf::FighterEditorSelectionKind::Variable;
            return;
        }
        editor.status = "Editor: rename variable failed: " + error;
    }
    DrawText(("init=" + std::to_string(variable.initialValue)).c_str(),
        static_cast<int>(nameX + nameW + 8.0f),
        static_cast<int>(rect.y + 21.0f),
        10,
        Fade(RAYWHITE, 0.68f));
    if (uiButton({rect.x + rect.width - 214.0f, rect.y + 16.0f, 42.0f, 22.0f}, "I-")) {
        std::string error;
        if (pf::setEditorSessionPackageVariableInitialValue(session, editor.selectedPackageVariable, variable.initialValue - 1, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: lowered package variable initial value");
            editor.selectionKind = pf::FighterEditorSelectionKind::Variable;
            return;
        }
        editor.status = "Editor: variable initial edit failed: " + error;
    }
    if (uiButton({rect.x + rect.width - 166.0f, rect.y + 16.0f, 42.0f, 22.0f}, "I+")) {
        std::string error;
        if (pf::setEditorSessionPackageVariableInitialValue(session, editor.selectedPackageVariable, variable.initialValue + 1, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: raised package variable initial value");
            editor.selectionKind = pf::FighterEditorSelectionKind::Variable;
            return;
        }
        editor.status = "Editor: variable initial edit failed: " + error;
    }
    if (uiButton({rect.x + rect.width - 118.0f, rect.y + 16.0f, 54.0f, 22.0f}, "+Var")) {
        std::string error;
        int added = -1;
        if (pf::addEditorSessionPackageVariable(session, {}, 0, &added, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added package variable");
            editor.selectedPackageVariable = added;
            editor.selectionKind = pf::FighterEditorSelectionKind::Variable;
            return;
        }
        editor.status = "Editor: add variable failed: " + error;
    }
    if (uiButton({rect.x + rect.width - 58.0f, rect.y + 16.0f, 54.0f, 22.0f}, "-Var")) {
        std::string error;
        const bool removingOnlyVariable = def.packageVariables.size() <= 1;
        if (pf::removeEditorSessionPackageVariable(session, editor.selectedPackageVariable, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed package variable");
            editor.selectionKind = removingOnlyVariable ? pf::FighterEditorSelectionKind::Script : pf::FighterEditorSelectionKind::Variable;
            return;
        }
        editor.status = "Editor: remove variable failed: " + error;
    }
}

static int countPackageVariableOperandUses(const pf::FighterDefinition& def, int variableIndex) {
    int uses = 0;
    for (const pf::PackageScript& script : def.packageScripts) {
        for (const pf::PackageScriptInstruction& instruction : script.instructions) {
            if (instruction.dst == variableIndex) {
                ++uses;
            }
            if (instruction.srcA == variableIndex) {
                ++uses;
            }
            if (instruction.srcB == variableIndex) {
                ++uses;
            }
        }
    }
    for (const pf::FighterState& state : def.states) {
        for (const pf::InterruptRule& rule : state.interrupts) {
            if (rule.packageVariable == variableIndex) {
                ++uses;
            }
        }
    }
    return uses;
}

static const char* gameObjectKindName(pf::GameObjectKind kind) {
    switch (kind) {
    case pf::GameObjectKind::Item: return "Item";
    case pf::GameObjectKind::Projectile: return "Projectile";
    }
    return "Object";
}

static pf::GameObjectKind nextGameObjectKind(pf::GameObjectKind kind) {
    return kind == pf::GameObjectKind::Projectile
        ? pf::GameObjectKind::Item
        : pf::GameObjectKind::Projectile;
}

static void drawEditorObjectBoxPanel(
    pf::World& world,
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    int& selectedFighterDef,
    int objectIndex,
    pf::GameObjectDefinition& object,
    Rectangle rect)
{
    BeginScissorMode(
        static_cast<int>(rect.x),
        static_cast<int>(rect.y),
        static_cast<int>(rect.width),
        static_cast<int>(rect.height));
    struct ScopedScissorEnd {
        ~ScopedScissorEnd() { EndScissorMode(); }
    } scissorEnd;

    DrawText(("Boxes H" + std::to_string(object.hitboxes.size()) +
              " Hu" + std::to_string(object.hurtboxes.size()) +
              " T" + std::to_string(object.touchboxes.size())).c_str(),
        static_cast<int>(rect.x),
        static_cast<int>(rect.y),
        11,
        Fade(RAYWHITE, 0.72f));
    constexpr float boxButtonW = 40.0f;
    constexpr float boxButtonGap = 4.0f;
    auto bx = [&](int col) {
        return rect.x + static_cast<float>(col) * (boxButtonW + boxButtonGap);
    };
    if (uiButton({rect.x, rect.y + 18.0f, 48.0f, 22.0f}, "+Hit")) {
        pf::HitboxDefinition hitbox;
        hitbox.radius = pf::fxFromFloat(0.35f);
        hitbox.damage = pf::fxFromFloat(3.0f);
        hitbox.knockbackAngleDegrees = pf::fx(45);
        hitbox.knockbackBase = pf::fx(20);
        hitbox.knockbackGrowth = pf::fx(40);
        std::string error;
        int added = -1;
        if (pf::addEditorSessionObjectHitbox(session, objectIndex, hitbox, -1, &added, &error)) {
            editor.selectedObjectHitbox = added;
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added object hitbox");
            return;
        }
        editor.status = "Editor: add object hitbox failed: " + error;
    }
    if (uiButton({rect.x + 52.0f, rect.y + 18.0f, 52.0f, 22.0f}, "+Hu")) {
        pf::GameObjectHurtboxDefinition hurtbox;
        hurtbox.endOffset = {0, pf::fxFromFloat(0.7f), 0};
        hurtbox.radius = pf::fxFromFloat(0.35f);
        std::string error;
        int added = -1;
        if (pf::addEditorSessionObjectHurtbox(session, objectIndex, hurtbox, -1, &added, &error)) {
            editor.selectedObjectHurtbox = added;
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added object hurtbox");
            return;
        }
        editor.status = "Editor: add object hurtbox failed: " + error;
    }
    if (uiButton({rect.x + 108.0f, rect.y + 18.0f, 54.0f, 22.0f}, "+T")) {
        pf::GameObjectTouchboxDefinition touchbox;
        touchbox.endOffset = {0, pf::fxFromFloat(0.8f), 0};
        touchbox.radius = pf::fxFromFloat(0.45f);
        std::string error;
        int added = -1;
        if (pf::addEditorSessionObjectTouchbox(session, objectIndex, touchbox, -1, &added, &error)) {
            editor.selectedObjectTouchbox = added;
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added object touchbox");
            return;
        }
        editor.status = "Editor: add object touchbox failed: " + error;
    }

    float y = rect.y + 48.0f;
    if (!object.hitboxes.empty()) {
        editor.selectedObjectHitbox = std::clamp(editor.selectedObjectHitbox, 0, static_cast<int>(object.hitboxes.size()) - 1);
        const pf::HitboxDefinition& hitbox = object.hitboxes[static_cast<size_t>(editor.selectedObjectHitbox)];
        DrawText(("#H" + std::to_string(editor.selectedObjectHitbox) +
                  " dmg=" + std::to_string(pf::fxToFloat(hitbox.damage)) +
                  " r=" + std::to_string(pf::fxToFloat(hitbox.radius)) +
                  " angle=" + std::to_string(pf::fxToFloat(hitbox.knockbackAngleDegrees))).c_str(),
            static_cast<int>(rect.x),
            static_cast<int>(y),
            10,
            RAYWHITE);
        auto commitHitbox = [&](pf::HitboxDefinition edited, const std::string& message) -> bool {
            std::string error;
            if (pf::setEditorSessionObjectHitbox(session, objectIndex, editor.selectedObjectHitbox, edited, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                return true;
            }
            editor.status = "Editor: object hitbox edit failed: " + error;
            return false;
        };
        pf::HitboxDefinition edited = hitbox;
        const float by = y + 18.0f;
        if (uiButton({bx(0), by, 34.0f, 22.0f}, "<")) {
            editor.selectedObjectHitbox = wrappedIndex(editor.selectedObjectHitbox - 1, static_cast<int>(object.hitboxes.size()));
            return;
        }
        if (uiButton({rect.x + 38.0f, by, 34.0f, 22.0f}, ">")) {
            editor.selectedObjectHitbox = wrappedIndex(editor.selectedObjectHitbox + 1, static_cast<int>(object.hitboxes.size()));
            return;
        }
        if (uiButton({bx(2), by, boxButtonW, 22.0f}, "D-")) {
            edited.damage = std::max(pf::Fix{0}, edited.damage - pf::fx(1));
            if (commitHitbox(edited, "Editor: lowered object hitbox damage")) return;
        }
        if (uiButton({bx(3), by, boxButtonW, 22.0f}, "D+")) {
            edited.damage += pf::fx(1);
            if (commitHitbox(edited, "Editor: raised object hitbox damage")) return;
        }
        if (uiButton({bx(0), by + 26.0f, boxButtonW, 22.0f}, "R-")) {
            edited.radius = std::max(pf::fxFromFloat(0.05f), edited.radius - pf::fxFromFloat(0.05f));
            if (commitHitbox(edited, "Editor: shrank object hitbox")) return;
        }
        if (uiButton({bx(1), by + 26.0f, boxButtonW, 22.0f}, "R+")) {
            edited.radius += pf::fxFromFloat(0.05f);
            if (commitHitbox(edited, "Editor: enlarged object hitbox")) return;
        }
        if (uiButton({bx(2), by + 26.0f, boxButtonW, 22.0f}, "X-")) {
            edited.offset.x -= pf::fxFromFloat(0.1f);
            if (commitHitbox(edited, "Editor: moved object hitbox backward")) return;
        }
        if (uiButton({bx(3), by + 26.0f, boxButtonW, 22.0f}, "X+")) {
            edited.offset.x += pf::fxFromFloat(0.1f);
            if (commitHitbox(edited, "Editor: moved object hitbox forward")) return;
        }
        if (uiButton({bx(0), by + 52.0f, boxButtonW, 22.0f}, "Y-")) {
            edited.offset.y -= pf::fxFromFloat(0.1f);
            if (commitHitbox(edited, "Editor: lowered object hitbox")) return;
        }
        if (uiButton({bx(1), by + 52.0f, boxButtonW, 22.0f}, "Y+")) {
            edited.offset.y += pf::fxFromFloat(0.1f);
            if (commitHitbox(edited, "Editor: raised object hitbox")) return;
        }
        if (uiButton({bx(2), by + 52.0f, boxButtonW, 22.0f}, "Gnd", hitbox.hitGrounded)) {
            edited.hitGrounded = !edited.hitGrounded;
            if (commitHitbox(edited, "Editor: toggled object hitbox grounded target")) return;
        }
        if (uiButton({bx(3), by + 52.0f, boxButtonW, 22.0f}, "Air", hitbox.hitAirborne)) {
            edited.hitAirborne = !edited.hitAirborne;
            if (commitHitbox(edited, "Editor: toggled object hitbox airborne target")) return;
        }
        if (uiButton({bx(0), by + 78.0f, 50.0f, 22.0f}, "-Hit")) {
            std::string error;
            if (pf::removeEditorSessionObjectHitbox(session, objectIndex, editor.selectedObjectHitbox, &error)) {
                editor.selectedObjectHitbox = 0;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed object hitbox");
                return;
            }
            editor.status = "Editor: remove object hitbox failed: " + error;
        }
        y += 126.0f;
    }
    if (!object.hurtboxes.empty()) {
        editor.selectedObjectHurtbox = std::clamp(editor.selectedObjectHurtbox, 0, static_cast<int>(object.hurtboxes.size()) - 1);
        const pf::GameObjectHurtboxDefinition& hurtbox = object.hurtboxes[static_cast<size_t>(editor.selectedObjectHurtbox)];
        DrawText(("#Hu" + std::to_string(editor.selectedObjectHurtbox) +
                  " " + hurtboxStateName(hurtbox.state) +
                  " r=" + std::to_string(pf::fxToFloat(hurtbox.radius))).c_str(),
            static_cast<int>(rect.x),
            static_cast<int>(y),
            10,
            RAYWHITE);
        auto commitHurtbox = [&](pf::GameObjectHurtboxDefinition edited, const std::string& message) -> bool {
            std::string error;
            if (pf::setEditorSessionObjectHurtbox(session, objectIndex, editor.selectedObjectHurtbox, edited, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                return true;
            }
            editor.status = "Editor: object hurtbox edit failed: " + error;
            return false;
        };
        pf::GameObjectHurtboxDefinition edited = hurtbox;
        const float by = y + 18.0f;
        if (uiButton({bx(0), by, 34.0f, 22.0f}, "<")) {
            editor.selectedObjectHurtbox = wrappedIndex(editor.selectedObjectHurtbox - 1, static_cast<int>(object.hurtboxes.size()));
            return;
        }
        if (uiButton({rect.x + 38.0f, by, 34.0f, 22.0f}, ">")) {
            editor.selectedObjectHurtbox = wrappedIndex(editor.selectedObjectHurtbox + 1, static_cast<int>(object.hurtboxes.size()));
            return;
        }
        if (uiButton({bx(2), by, boxButtonW, 22.0f}, "R-")) {
            edited.radius = std::max(pf::fxFromFloat(0.05f), edited.radius - pf::fxFromFloat(0.05f));
            if (commitHurtbox(edited, "Editor: shrank object hurtbox")) return;
        }
        if (uiButton({bx(3), by, boxButtonW, 22.0f}, "R+")) {
            edited.radius += pf::fxFromFloat(0.05f);
            if (commitHurtbox(edited, "Editor: enlarged object hurtbox")) return;
        }
        if (uiButton({bx(0), by + 26.0f, 54.0f, 22.0f}, "State")) {
            edited.state = nextHurtboxState(edited.state);
            if (commitHurtbox(edited, "Editor: changed object hurtbox state")) return;
        }
        if (uiButton({rect.x + 58.0f, by + 26.0f, 54.0f, 22.0f}, "-Hu")) {
            std::string error;
            if (pf::removeEditorSessionObjectHurtbox(session, objectIndex, editor.selectedObjectHurtbox, &error)) {
                editor.selectedObjectHurtbox = 0;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed object hurtbox");
                return;
            }
            editor.status = "Editor: remove object hurtbox failed: " + error;
        }
        if (uiButton({bx(0), by + 52.0f, boxButtonW, 22.0f}, "X-")) {
            edited.startOffset.x -= pf::fxFromFloat(0.1f);
            edited.endOffset.x -= pf::fxFromFloat(0.1f);
            if (commitHurtbox(edited, "Editor: moved object hurtbox backward")) return;
        }
        if (uiButton({bx(1), by + 52.0f, boxButtonW, 22.0f}, "X+")) {
            edited.startOffset.x += pf::fxFromFloat(0.1f);
            edited.endOffset.x += pf::fxFromFloat(0.1f);
            if (commitHurtbox(edited, "Editor: moved object hurtbox forward")) return;
        }
        if (uiButton({bx(2), by + 52.0f, boxButtonW, 22.0f}, "Y-")) {
            edited.startOffset.y -= pf::fxFromFloat(0.1f);
            edited.endOffset.y -= pf::fxFromFloat(0.1f);
            if (commitHurtbox(edited, "Editor: lowered object hurtbox")) return;
        }
        if (uiButton({bx(3), by + 52.0f, boxButtonW, 22.0f}, "Y+")) {
            edited.startOffset.y += pf::fxFromFloat(0.1f);
            edited.endOffset.y += pf::fxFromFloat(0.1f);
            if (commitHurtbox(edited, "Editor: raised object hurtbox")) return;
        }
        y += 100.0f;
    }
    if (!object.touchboxes.empty()) {
        editor.selectedObjectTouchbox = std::clamp(editor.selectedObjectTouchbox, 0, static_cast<int>(object.touchboxes.size()) - 1);
        const pf::GameObjectTouchboxDefinition& touchbox = object.touchboxes[static_cast<size_t>(editor.selectedObjectTouchbox)];
        DrawText(("#T" + std::to_string(editor.selectedObjectTouchbox) +
                  " r=" + std::to_string(pf::fxToFloat(touchbox.radius)) +
                  (touchbox.touchFighters ? " fighters" : "") +
                  (touchbox.touchObjects ? " objects" : "")).c_str(),
            static_cast<int>(rect.x),
            static_cast<int>(y),
            10,
            RAYWHITE);
        auto commitTouchbox = [&](pf::GameObjectTouchboxDefinition edited, const std::string& message) -> bool {
            std::string error;
            if (pf::setEditorSessionObjectTouchbox(session, objectIndex, editor.selectedObjectTouchbox, edited, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                return true;
            }
            editor.status = "Editor: object touchbox edit failed: " + error;
            return false;
        };
        pf::GameObjectTouchboxDefinition edited = touchbox;
        const float by = y + 18.0f;
        if (uiButton({bx(0), by, 34.0f, 22.0f}, "<")) {
            editor.selectedObjectTouchbox = wrappedIndex(editor.selectedObjectTouchbox - 1, static_cast<int>(object.touchboxes.size()));
            return;
        }
        if (uiButton({rect.x + 38.0f, by, 34.0f, 22.0f}, ">")) {
            editor.selectedObjectTouchbox = wrappedIndex(editor.selectedObjectTouchbox + 1, static_cast<int>(object.touchboxes.size()));
            return;
        }
        if (uiButton({bx(2), by, boxButtonW, 22.0f}, "R-")) {
            edited.radius = std::max(pf::fxFromFloat(0.05f), edited.radius - pf::fxFromFloat(0.05f));
            if (commitTouchbox(edited, "Editor: shrank object touchbox")) return;
        }
        if (uiButton({bx(3), by, boxButtonW, 22.0f}, "R+")) {
            edited.radius += pf::fxFromFloat(0.05f);
            if (commitTouchbox(edited, "Editor: enlarged object touchbox")) return;
        }
        if (uiButton({bx(0), by + 26.0f, boxButtonW, 22.0f}, "Ftr", touchbox.touchFighters)) {
            edited.touchFighters = !edited.touchFighters;
            if (commitTouchbox(edited, "Editor: toggled object touchbox fighter contact")) return;
        }
        if (uiButton({bx(1), by + 26.0f, boxButtonW, 22.0f}, "Obj", touchbox.touchObjects)) {
            edited.touchObjects = !edited.touchObjects;
            if (commitTouchbox(edited, "Editor: toggled object touchbox object contact")) return;
        }
        if (uiButton({bx(2), by + 26.0f, 54.0f, 22.0f}, "-T")) {
            std::string error;
            if (pf::removeEditorSessionObjectTouchbox(session, objectIndex, editor.selectedObjectTouchbox, &error)) {
                editor.selectedObjectTouchbox = 0;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed object touchbox");
                return;
            }
            editor.status = "Editor: remove object touchbox failed: " + error;
        }
        if (uiButton({bx(0), by + 52.0f, boxButtonW, 22.0f}, "X-")) {
            edited.startOffset.x -= pf::fxFromFloat(0.1f);
            edited.endOffset.x -= pf::fxFromFloat(0.1f);
            if (commitTouchbox(edited, "Editor: moved object touchbox backward")) return;
        }
        if (uiButton({bx(1), by + 52.0f, boxButtonW, 22.0f}, "X+")) {
            edited.startOffset.x += pf::fxFromFloat(0.1f);
            edited.endOffset.x += pf::fxFromFloat(0.1f);
            if (commitTouchbox(edited, "Editor: moved object touchbox forward")) return;
        }
        if (uiButton({bx(2), by + 52.0f, boxButtonW, 22.0f}, "Y-")) {
            edited.startOffset.y -= pf::fxFromFloat(0.1f);
            edited.endOffset.y -= pf::fxFromFloat(0.1f);
            if (commitTouchbox(edited, "Editor: lowered object touchbox")) return;
        }
        if (uiButton({bx(3), by + 52.0f, boxButtonW, 22.0f}, "Y+")) {
            edited.startOffset.y += pf::fxFromFloat(0.1f);
            edited.endOffset.y += pf::fxFromFloat(0.1f);
            if (commitTouchbox(edited, "Editor: raised object touchbox")) return;
        }
    }
}

static bool drawEditorObjectCallbackStrip(
    pf::World& world,
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    int& selectedFighterDef,
    int objectIndex,
    pf::GameObjectDefinition& object,
    int scriptIndex,
    Rectangle rect)
{
    if (object.packageScripts.empty() || scriptIndex < 0 || scriptIndex >= static_cast<int>(object.packageScripts.size())) {
        return false;
    }
    const pf::PackageScript& script = object.packageScripts[static_cast<size_t>(scriptIndex)];
    const int stateSlot = wrappedIndex(editor.selectedObjectStateCallback, 4);
    const int eventSlot = wrappedIndex(editor.selectedObjectEventCallback, 21);
    const pf::FighterEditorObjectStateCallbackSlot stateCallbackSlot =
        static_cast<pf::FighterEditorObjectStateCallbackSlot>(stateSlot);
    const pf::FighterEditorObjectEventCallbackSlot eventCallbackSlot =
        static_cast<pf::FighterEditorObjectEventCallbackSlot>(eventSlot);
    DrawText("Callbacks", static_cast<int>(rect.x), static_cast<int>(rect.y), 10, Fade(RAYWHITE, 0.62f));
    const std::string stateSummary = !object.states.empty() && editor.selectedObjectState >= 0 && editor.selectedObjectState < static_cast<int>(object.states.size())
        ? callbackSummary(objectStateCallbacks(object.states[static_cast<size_t>(editor.selectedObjectState)], stateSlot))
        : "none";
    const std::string eventSummary = callbackSummary(objectEventCallbacks(object, eventSlot));
    DrawText(clippedText(std::string("State ") + objectStateCallbackLabel(stateSlot) + ": " + stateSummary, 10, rect.width - 12.0f).c_str(),
        static_cast<int>(rect.x),
        static_cast<int>(rect.y + 18.0f),
        10,
        Fade(RAYWHITE, 0.72f));
    DrawText(clippedText(std::string("Event ") + objectEventCallbackLabel(eventSlot) + ": " + eventSummary, 10, rect.width - 12.0f).c_str(),
        static_cast<int>(rect.x),
        static_cast<int>(rect.y + 40.0f),
        10,
        Fade(RAYWHITE, 0.72f));

    const float stateButtonY = rect.y + 62.0f;
    const float eventButtonY = stateButtonY + 26.0f;
    if (uiButton({rect.x, stateButtonY, 24.0f, 22.0f}, "S<")) {
        editor.selectedObjectStateCallback = wrappedIndex(stateSlot - 1, 4);
        editor.selectedObjectCallback = 0;
        editor.selectedObjectCallbackEvent = false;
        editor.selectionKind = pf::FighterEditorSelectionKind::ObjectCallback;
        return true;
    }
    if (uiButton({rect.x + 28.0f, stateButtonY, 24.0f, 22.0f}, "S>")) {
        editor.selectedObjectStateCallback = wrappedIndex(stateSlot + 1, 4);
        editor.selectedObjectCallback = 0;
        editor.selectedObjectCallbackEvent = false;
        editor.selectionKind = pf::FighterEditorSelectionKind::ObjectCallback;
        return true;
    }
    if (uiButton({rect.x + 56.0f, stateButtonY, 42.0f, 22.0f}, "BindS")) {
        if (object.states.empty()) {
            editor.status = "Editor: add an object state before binding a state callback";
        } else {
            std::string error;
            if (pf::bindEditorSessionObjectPackageScriptStateCallback(
                    session,
                    objectIndex,
                    editor.selectedObjectState,
                    stateCallbackSlot,
                    script.name,
                    &error))
            {
                editor.selectedObjectCallback = 0;
                editor.selectedObjectCallbackEvent = false;
                editor.selectionKind = pf::FighterEditorSelectionKind::ObjectCallback;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, std::string("Editor: bound object ") + objectStateCallbackLabel(stateSlot) + " callback");
                return true;
            }
            editor.status = "Editor: object state callback bind failed: " + error;
        }
    }
    if (uiButton({rect.x + 102.0f, stateButtonY, 36.0f, 22.0f}, "UseS")) {
        editor.selectedObjectCallback = 0;
        editor.selectedObjectCallbackEvent = false;
        editor.selectionKind = pf::FighterEditorSelectionKind::ObjectCallback;
        editor.status = std::string("Editor: selected object ") + objectStateCallbackLabel(stateSlot) + " callback";
        return true;
    }
    if (uiButton({rect.x + 142.0f, stateButtonY, 28.0f, 22.0f}, "-S")) {
        if (object.states.empty()) {
            editor.status = "Editor: no object state callback to remove";
        } else {
            std::vector<pf::FunctionCall> calls = objectStateCallbacks(object.states[static_cast<size_t>(editor.selectedObjectState)], stateSlot);
            removePackageScriptCallbackRefs(calls, script.name);
            std::string error;
            if (pf::setEditorSessionObjectStateCallbacks(session, objectIndex, editor.selectedObjectState, stateCallbackSlot, calls, &error)) {
                editor.selectedObjectCallback = 0;
                editor.selectedObjectCallbackEvent = false;
                editor.selectionKind = pf::FighterEditorSelectionKind::ObjectCallback;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, std::string("Editor: removed object ") + objectStateCallbackLabel(stateSlot) + " callback");
                return true;
            }
            editor.status = "Editor: object state callback remove failed: " + error;
        }
    }
    if (uiButton({rect.x, eventButtonY, 24.0f, 22.0f}, "E<")) {
        editor.selectedObjectEventCallback = wrappedIndex(eventSlot - 1, 21);
        editor.selectedObjectCallback = 0;
        editor.selectedObjectCallbackEvent = true;
        editor.selectionKind = pf::FighterEditorSelectionKind::ObjectCallback;
        return true;
    }
    if (uiButton({rect.x + 28.0f, eventButtonY, 24.0f, 22.0f}, "E>")) {
        editor.selectedObjectEventCallback = wrappedIndex(eventSlot + 1, 21);
        editor.selectedObjectCallback = 0;
        editor.selectedObjectCallbackEvent = true;
        editor.selectionKind = pf::FighterEditorSelectionKind::ObjectCallback;
        return true;
    }
    if (uiButton({rect.x + 56.0f, eventButtonY, 42.0f, 22.0f}, "BindE")) {
        std::string error;
        if (pf::bindEditorSessionObjectPackageScriptEventCallback(
                session,
                objectIndex,
                eventCallbackSlot,
                script.name,
                &error))
        {
            editor.selectedObjectCallback = 0;
            editor.selectedObjectCallbackEvent = true;
            editor.selectionKind = pf::FighterEditorSelectionKind::ObjectCallback;
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, std::string("Editor: bound object ") + objectEventCallbackLabel(eventSlot) + " callback");
            return true;
        }
        editor.status = "Editor: object event callback bind failed: " + error;
    }
    if (uiButton({rect.x + 102.0f, eventButtonY, 36.0f, 22.0f}, "UseE")) {
        editor.selectedObjectCallback = 0;
        editor.selectedObjectCallbackEvent = true;
        editor.selectionKind = pf::FighterEditorSelectionKind::ObjectCallback;
        editor.status = std::string("Editor: selected object ") + objectEventCallbackLabel(eventSlot) + " callback";
        return true;
    }
    if (uiButton({rect.x + 142.0f, eventButtonY, 28.0f, 22.0f}, "-E")) {
        std::vector<pf::FunctionCall> calls = objectEventCallbacks(object, eventSlot);
        removePackageScriptCallbackRefs(calls, script.name);
        std::string error;
        if (pf::setEditorSessionObjectEventCallbacks(session, objectIndex, eventCallbackSlot, calls, &error)) {
            editor.selectedObjectCallback = 0;
            editor.selectedObjectCallbackEvent = true;
            editor.selectionKind = pf::FighterEditorSelectionKind::ObjectCallback;
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, std::string("Editor: removed object ") + objectEventCallbackLabel(eventSlot) + " callback");
            return true;
        }
        editor.status = "Editor: object event callback remove failed: " + error;
    }
    return false;
}

static void drawEditorObjectWorkstation(
    pf::World& world,
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    int& selectedFighterDef,
    Rectangle rect)
{
    drawPanelChrome(rect, "Objects / Articles");
    auto addObject = [&](pf::GameObjectKind kind) -> bool {
        std::string error;
        int added = -1;
        const std::string prefix = kind == pf::GameObjectKind::Projectile ? "Projectile" : "Item";
        if (pf::addEditorSessionObject(session, pf::uniqueEditorObjectName(session.package, prefix), kind, &added, &error)) {
            editor.selectedObjectDef = added;
            editor.selectedObjectState = 0;
            editor.selectedPackageScript = 0;
            editor.selectedPackageInstruction = 0;
            editor.selectionKind = pf::FighterEditorSelectionKind::Object;
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, std::string("Editor: added ") + gameObjectKindName(kind));
            return true;
        }
        editor.status = "Editor: add object failed: " + error;
        return false;
    };

    if (session.package.objects.empty()) {
        DrawText("No package-owned objects/articles", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 36.0f), 13, Fade(RAYWHITE, 0.68f));
        if (uiButton({rect.x + 10.0f, rect.y + 62.0f, 94.0f, 24.0f}, "+Projectile")) {
            addObject(pf::GameObjectKind::Projectile);
            return;
        }
        if (uiButton({rect.x + 112.0f, rect.y + 62.0f, 74.0f, 24.0f}, "+Item")) {
            addObject(pf::GameObjectKind::Item);
            return;
        }
        DrawText("Spawn instructions and object callbacks will target definitions created here.",
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(rect.y + 102.0f),
            11,
            Fade(RAYWHITE, 0.56f));
        return;
    }

    editor.selectedObjectDef = std::clamp(editor.selectedObjectDef, 0, static_cast<int>(session.package.objects.size()) - 1);
    pf::GameObjectDefinition& object = session.package.objects[static_cast<size_t>(editor.selectedObjectDef)];
    DrawText((std::string("Selected: ") + object.name + "  " + gameObjectKindName(object.kind)).c_str(),
        static_cast<int>(rect.x + 10.0f),
        static_cast<int>(rect.y + 34.0f),
        13,
        RAYWHITE);
    if (uiButton({rect.x + rect.width - 236.0f, rect.y + 29.0f, 84.0f, 22.0f}, "+Projectile")) {
        addObject(pf::GameObjectKind::Projectile);
        return;
    }
    if (uiButton({rect.x + rect.width - 146.0f, rect.y + 29.0f, 58.0f, 22.0f}, "+Item")) {
        addObject(pf::GameObjectKind::Item);
        return;
    }
    if (uiButton({rect.x + rect.width - 82.0f, rect.y + 29.0f, 66.0f, 22.0f}, "-Object")) {
        std::string error;
        if (pf::removeEditorSessionObject(session, editor.selectedObjectDef, {}, &error)) {
            editor.selectedObjectDef = std::clamp(editor.selectedObjectDef, 0, std::max(0, static_cast<int>(session.package.objects.size()) - 1));
            editor.selectedObjectState = 0;
            editor.selectionKind = pf::FighterEditorSelectionKind::Object;
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed object/article");
            return;
        }
        editor.status = "Editor: remove object failed: " + error;
    }

    const float listY = rect.y + 62.0f;
    const float objectListW = std::clamp(rect.width * 0.34f, 116.0f, 172.0f);
    DrawText("Definitions", static_cast<int>(rect.x + 10.0f), static_cast<int>(listY), 10, Fade(RAYWHITE, 0.62f));
    const int visibleObjects = std::min(5, static_cast<int>(session.package.objects.size()));
    const int objectStart = visibleListStart(editor.selectedObjectDef, static_cast<int>(session.package.objects.size()), visibleObjects);
    for (int row = 0; row < visibleObjects; ++row) {
        const int objectIndex = objectStart + row;
        const pf::GameObjectDefinition& rowObject = session.package.objects[static_cast<size_t>(objectIndex)];
        const std::string label = clippedText(rowObject.name + "  " + gameObjectKindName(rowObject.kind), 10, objectListW - 8.0f);
        if (uiListRow({rect.x + 10.0f, listY + 16.0f + 22.0f * row, objectListW, 20.0f}, label, objectIndex == editor.selectedObjectDef)) {
            editor.selectedObjectDef = objectIndex;
            editor.selectedObjectState = 0;
            editor.selectedPackageScript = 0;
            editor.selectedPackageInstruction = 0;
            editor.selectionKind = pf::FighterEditorSelectionKind::Object;
            return;
        }
    }

    const float detailX = rect.x + 20.0f + objectListW;
    const float detailW = std::max(180.0f, rect.width - objectListW - 30.0f);
    if (uiButton({detailX, listY - 3.0f, 58.0f, 22.0f}, "Logic", editor.objectPanel == pf::ObjectEditorPanel::Logic)) {
        editor.objectPanel = pf::ObjectEditorPanel::Logic;
        return;
    }
    if (uiButton({detailX + 64.0f, listY - 3.0f, 58.0f, 22.0f}, "Boxes", editor.objectPanel == pf::ObjectEditorPanel::Boxes)) {
        editor.objectPanel = pf::ObjectEditorPanel::Boxes;
        return;
    }
    if (editor.objectPanel == pf::ObjectEditorPanel::Boxes) {
        drawEditorObjectBoxPanel(
            world,
            editor,
            session,
            selectedFighterDef,
            editor.selectedObjectDef,
            object,
            {detailX, listY + 26.0f, detailW, std::max(90.0f, rect.y + rect.height - listY - 36.0f)});
        return;
    }
    DrawText(clippedText("States " + std::to_string(object.states.size()) +
              "  Vars " + std::to_string(object.packageVariables.size()) +
              "  Scripts " + std::to_string(object.packageScripts.size()),
              11,
              std::max(24.0f, detailW - 132.0f)).c_str(),
        static_cast<int>(detailX + 132.0f),
        static_cast<int>(listY),
        11,
        Fade(RAYWHITE, 0.72f));

    if (!object.states.empty()) {
        editor.selectedObjectState = std::clamp(editor.selectedObjectState, 0, static_cast<int>(object.states.size()) - 1);
        const int visibleStates = std::min(3, static_cast<int>(object.states.size()));
        const int stateStart = visibleListStart(editor.selectedObjectState, static_cast<int>(object.states.size()), visibleStates);
        for (int row = 0; row < visibleStates; ++row) {
            const int stateIndex = stateStart + row;
            const pf::GameObjectStateDefinition& objectState = object.states[static_cast<size_t>(stateIndex)];
            std::string label = objectState.name + " len=" + std::to_string(objectState.animationLengthFrames);
            if (stateIndex == object.initialState) {
                label += " initial";
            }
            if (uiListRow({detailX, listY + 16.0f + 22.0f * row, detailW, 20.0f}, clippedText(label, 10, detailW - 8.0f), stateIndex == editor.selectedObjectState)) {
                editor.selectedObjectState = stateIndex;
                editor.selectionKind = pf::FighterEditorSelectionKind::Object;
                return;
            }
        }
    } else {
        DrawText("No object states", static_cast<int>(detailX), static_cast<int>(listY + 22.0f), 11, Fade(RAYWHITE, 0.56f));
    }

    const float stateButtonY = listY + 90.0f;
    if (uiButton({detailX, stateButtonY, 62.0f, 22.0f}, "+State")) {
        std::string error;
        int created = -1;
        if (pf::createEditorSessionObjectState(session, editor.selectedObjectDef, {}, -1, &created, &error)) {
            editor.selectedObjectState = created;
            editor.selectionKind = pf::FighterEditorSelectionKind::Object;
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added object state");
            return;
        }
        editor.status = "Editor: add object state failed: " + error;
    }
    if (uiButton({detailX + 68.0f, stateButtonY, 58.0f, 22.0f}, "Clone")) {
        std::string error;
        int created = -1;
        if (pf::duplicateEditorSessionObjectState(session, editor.selectedObjectDef, editor.selectedObjectState, &created, &error)) {
            editor.selectedObjectState = created;
            editor.selectionKind = pf::FighterEditorSelectionKind::Object;
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: cloned object state");
            return;
        }
        editor.status = "Editor: clone object state failed: " + error;
    }
    if (uiButton({detailX + 132.0f, stateButtonY, 58.0f, 22.0f}, "-State")) {
        std::string error;
        if (pf::removeEditorSessionObjectState(session, editor.selectedObjectDef, editor.selectedObjectState, {}, &error)) {
            editor.selectedObjectState = std::clamp(editor.selectedObjectState, 0, std::max(0, static_cast<int>(session.package.objects[static_cast<size_t>(editor.selectedObjectDef)].states.size()) - 1));
            editor.selectionKind = pf::FighterEditorSelectionKind::Object;
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed object state");
            return;
        }
        editor.status = "Editor: remove object state failed: " + error;
    }
    if (uiButton({detailX + 196.0f, stateButtonY, 58.0f, 22.0f}, "Initial")) {
        if (!object.states.empty()) {
            const pf::GameObjectStateDefinition& objectState = object.states[static_cast<size_t>(editor.selectedObjectState)];
            std::string error;
            if (pf::setEditorSessionObjectStateTiming(session, editor.selectedObjectDef, editor.selectedObjectState, objectState.animationLengthFrames, objectState.loopAnimation, true, &error)) {
                editor.selectionKind = pf::FighterEditorSelectionKind::Object;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: set object initial state");
                return;
            }
            editor.status = "Editor: set object initial failed: " + error;
        }
    }

    const float scriptY = stateButtonY + 36.0f;
    DrawText("Object Script", static_cast<int>(detailX), static_cast<int>(scriptY), 10, Fade(RAYWHITE, 0.62f));
    if (object.packageScripts.empty()) {
        DrawText("No object scripts", static_cast<int>(detailX), static_cast<int>(scriptY + 20.0f), 11, Fade(RAYWHITE, 0.56f));
        if (uiButton({detailX, scriptY + 44.0f, 72.0f, 22.0f}, "+Script")) {
            std::string error;
            int added = -1;
            if (pf::addEditorSessionObjectPackageScript(session, editor.selectedObjectDef, {}, 64, &added, &error)) {
                editor.selectedPackageScript = added;
                editor.selectedPackageInstruction = 0;
                editor.selectionKind = pf::FighterEditorSelectionKind::Object;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added object script");
                return;
            }
            editor.status = "Editor: add object script failed: " + error;
        }
    } else {
        editor.selectedPackageScript = std::clamp(editor.selectedPackageScript, 0, static_cast<int>(object.packageScripts.size()) - 1);
        pf::PackageScript& script = object.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
        DrawText(clippedText(script.name + "  instr=" + std::to_string(script.instructions.size()), 11, detailW - 8.0f).c_str(),
            static_cast<int>(detailX),
            static_cast<int>(scriptY + 20.0f),
            11,
            RAYWHITE);
        if (uiButton({detailX, scriptY + 42.0f, 34.0f, 22.0f}, "<")) {
            editor.selectedPackageScript = wrappedIndex(editor.selectedPackageScript - 1, static_cast<int>(object.packageScripts.size()));
            editor.selectedPackageInstruction = 0;
            editor.selectionKind = pf::FighterEditorSelectionKind::Object;
            return;
        }
        if (uiButton({detailX + 40.0f, scriptY + 42.0f, 34.0f, 22.0f}, ">")) {
            editor.selectedPackageScript = wrappedIndex(editor.selectedPackageScript + 1, static_cast<int>(object.packageScripts.size()));
            editor.selectedPackageInstruction = 0;
            editor.selectionKind = pf::FighterEditorSelectionKind::Object;
            return;
        }
        if (uiButton({detailX + 80.0f, scriptY + 42.0f, 58.0f, 22.0f}, "+Script")) {
            std::string error;
            int added = -1;
            if (pf::addEditorSessionObjectPackageScript(session, editor.selectedObjectDef, {}, 64, &added, &error)) {
                editor.selectedPackageScript = added;
                editor.selectedPackageInstruction = 0;
                editor.selectionKind = pf::FighterEditorSelectionKind::Object;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added object script");
                return;
            }
            editor.status = "Editor: add object script failed: " + error;
        }
        if (uiButton({detailX + 144.0f, scriptY + 42.0f, 58.0f, 22.0f}, "Clone")) {
            std::string error;
            int added = -1;
            if (pf::duplicateEditorSessionObjectPackageScript(session, editor.selectedObjectDef, editor.selectedPackageScript, &added, &error)) {
                editor.selectedPackageScript = added;
                editor.selectedPackageInstruction = 0;
                editor.selectionKind = pf::FighterEditorSelectionKind::Object;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: cloned object script");
                return;
            }
            editor.status = "Editor: clone object script failed: " + error;
        }
        if (uiButton({detailX + 208.0f, scriptY + 42.0f, 58.0f, 22.0f}, "-Script")) {
            std::string error;
            if (pf::removeEditorSessionObjectPackageScript(session, editor.selectedObjectDef, editor.selectedPackageScript, &error)) {
                editor.selectedPackageScript = 0;
                editor.selectedPackageInstruction = 0;
                editor.selectionKind = pf::FighterEditorSelectionKind::Object;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed object script");
                return;
            }
            editor.status = "Editor: remove object script failed: " + error;
        }

        const float callbackY = scriptY + 72.0f;
        if (drawEditorObjectCallbackStrip(
                world,
                editor,
                session,
                selectedFighterDef,
                editor.selectedObjectDef,
                object,
                editor.selectedPackageScript,
                {detailX, callbackY, detailW, 114.0f}))
        {
            return;
        }

        const float instrY = callbackY + 122.0f;
        if (uiButton({detailX, instrY, 54.0f, 22.0f}, "+Nop")) {
            std::string error;
            int added = -1;
            if (pf::addEditorSessionObjectPackageInstruction(session, editor.selectedObjectDef, editor.selectedPackageScript, {pf::PackageScriptOp::Nop, -1, -1, -1, 0, 0, {}}, -1, &added, &error)) {
                editor.selectedPackageInstruction = added;
                editor.selectionKind = pf::FighterEditorSelectionKind::Instruction;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added object Nop instruction");
                return;
            }
            editor.status = "Editor: add object instruction failed: " + error;
        }
        if (uiButton({detailX + 60.0f, instrY, 58.0f, 22.0f}, "+State")) {
            pf::PackageScriptInstruction instruction{pf::PackageScriptOp::ChangeState, -1, -1, -1, 0, 0, {}};
            normalizeObjectPackageInstruction(instruction, object, world, editor.selectedObjectState, editor.selectedObjectDef);
            std::string error;
            int added = -1;
            if (pf::addEditorSessionObjectPackageInstruction(session, editor.selectedObjectDef, editor.selectedPackageScript, instruction, -1, &added, &error)) {
                editor.selectedPackageInstruction = added;
                editor.selectionKind = pf::FighterEditorSelectionKind::Instruction;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added object state-change instruction");
                return;
            }
            editor.status = "Editor: add object instruction failed: " + error;
        }
        if (uiButton({detailX + 124.0f, instrY, 56.0f, 22.0f}, "-Instr")) {
            std::string error;
            if (pf::removeEditorSessionObjectPackageInstruction(session, editor.selectedObjectDef, editor.selectedPackageScript, editor.selectedPackageInstruction, &error)) {
                editor.selectionKind = pf::FighterEditorSelectionKind::Instruction;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed object instruction");
                return;
            }
            editor.status = "Editor: remove object instruction failed: " + error;
        }
        if (uiButton({detailX + 186.0f, instrY, 54.0f, 22.0f}, "Linear")) {
            std::string error;
            if (pf::setEditorSessionObjectPackageScriptGraph(session, editor.selectedObjectDef, editor.selectedPackageScript, pf::makePackageScriptLinearGraph(script), &error)) {
                editor.selectionKind = pf::FighterEditorSelectionKind::Object;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: rebuilt object script graph");
                return;
            }
            editor.status = "Editor: object graph rebuild failed: " + error;
        }
        if (uiButton({detailX + 246.0f, instrY, 54.0f, 22.0f}, "Compile")) {
            std::string error;
            if (pf::compileEditorSessionObjectPackageScriptGraph(session, editor.selectedObjectDef, editor.selectedPackageScript, &error)) {
                editor.selectionKind = pf::FighterEditorSelectionKind::Object;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: compiled object script graph");
                return;
            }
            editor.status = "Editor: object graph compile failed: " + error;
        }

        Rectangle canvas{detailX, instrY + 30.0f, detailW, std::max(44.0f, rect.y + rect.height - instrY - 40.0f)};
        if (!script.graph.nodes.empty()) {
            auto selectedGraphNodeIt = std::find_if(script.graph.nodes.begin(), script.graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) {
                return node.id == editor.selectedPackageGraphNode;
            });
            if (selectedGraphNodeIt == script.graph.nodes.end()) {
                editor.selectedPackageGraphNode = script.graph.entryNode >= 0 ? script.graph.entryNode : script.graph.nodes.front().id;
                selectedGraphNodeIt = std::find_if(script.graph.nodes.begin(), script.graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) {
                    return node.id == editor.selectedPackageGraphNode;
                });
                if (selectedGraphNodeIt == script.graph.nodes.end()) {
                    selectedGraphNodeIt = script.graph.nodes.begin();
                    editor.selectedPackageGraphNode = selectedGraphNodeIt->id;
                }
            }
            auto selectedGraphNodeIndex = [&]() {
                const auto found = std::find_if(script.graph.nodes.begin(), script.graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) {
                    return node.id == editor.selectedPackageGraphNode;
                });
                return found == script.graph.nodes.end()
                    ? 0
                    : static_cast<int>(std::distance(script.graph.nodes.begin(), found));
            };
            auto selectObjectGraphNodeByIndex = [&](int index) {
                const int nodeIndex = wrappedIndex(index, static_cast<int>(script.graph.nodes.size()));
                const pf::PackageScriptGraphNode& node = script.graph.nodes[static_cast<size_t>(nodeIndex)];
                editor.selectedPackageGraphNode = node.id;
                if (node.kind == pf::PackageScriptGraphNodeKind::Instruction && node.instructionIndex >= 0) {
                    editor.selectedPackageInstruction = std::clamp(node.instructionIndex, 0, std::max(0, static_cast<int>(script.instructions.size()) - 1));
                    session.selectedPackageInstruction = editor.selectedPackageInstruction;
                    editor.selectionKind = pf::FighterEditorSelectionKind::Instruction;
                } else {
                    editor.selectionKind = pf::FighterEditorSelectionKind::Object;
                }
            };
            auto nudgeSelectedObjectGraphNode = [&](pf::Fix dx, pf::Fix dy, const std::string& label) -> bool {
                const auto found = std::find_if(script.graph.nodes.begin(), script.graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) {
                    return node.id == editor.selectedPackageGraphNode;
                });
                if (found == script.graph.nodes.end()) {
                    return false;
                }
                pf::PackageScriptGraphNode edited = *found;
                edited.position.x += dx;
                edited.position.y += dy;
                std::string error;
                if (pf::setEditorSessionObjectPackageScriptGraphNode(session, editor.selectedObjectDef, editor.selectedPackageScript, edited.id, edited, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: nudged object graph node " + label);
                    return true;
                }
                editor.status = "Editor: object graph node edit failed: " + error;
                return false;
            };

            const float graphControlY = canvas.y;
            DrawText(("Node #" + std::to_string(editor.selectedPackageGraphNode)).c_str(),
                static_cast<int>(detailX + 2.0f),
                static_cast<int>(graphControlY + 6.0f),
                10,
                Fade(RAYWHITE, 0.68f));
            const float navX = detailX + std::min(74.0f, std::max(54.0f, detailW - 226.0f));
            if (uiButton({navX, graphControlY, 30.0f, 22.0f}, "N<")) {
                selectObjectGraphNodeByIndex(selectedGraphNodeIndex() - 1);
                return;
            }
            if (uiButton({navX + 34.0f, graphControlY, 30.0f, 22.0f}, "N>")) {
                selectObjectGraphNodeByIndex(selectedGraphNodeIndex() + 1);
                return;
            }
            if (uiButton({navX + 72.0f, graphControlY, 30.0f, 22.0f}, "X-")) {
                if (nudgeSelectedObjectGraphNode(-pf::fx(16), 0, "left")) return;
            }
            if (uiButton({navX + 106.0f, graphControlY, 30.0f, 22.0f}, "X+")) {
                if (nudgeSelectedObjectGraphNode(pf::fx(16), 0, "right")) return;
            }
            if (uiButton({navX + 144.0f, graphControlY, 30.0f, 22.0f}, "Y-")) {
                if (nudgeSelectedObjectGraphNode(0, -pf::fx(16), "up")) return;
            }
            if (uiButton({navX + 178.0f, graphControlY, 30.0f, 22.0f}, "Y+")) {
                if (nudgeSelectedObjectGraphNode(0, pf::fx(16), "down")) return;
            }
            canvas.y += 28.0f;
            canvas.height = std::max(28.0f, canvas.height - 28.0f);
        }
        drawPackageScriptBlockGraph(script, editor, canvas);
    }
}

static void drawEditorAnimationWorkstation(
    pf::World& world,
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    int& selectedFighterDef,
    pf::FighterDefinition& def,
    Rectangle rect)
{
    drawPanelChrome(rect, "Animation");
    editor.selectionKind = pf::FighterEditorSelectionKind::Animation;
    if (def.authoredClips.empty()) {
        DrawText("No authored clips in this package", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 36.0f), 13, Fade(RAYWHITE, 0.68f));
        if (uiButton({rect.x + 10.0f, rect.y + 62.0f, 78.0f, 24.0f}, "+ Clip")) {
            std::string error;
            int created = -1;
            if (pf::createEditorSessionAuthoredClip(session, {}, -1, &created, &error)) {
                editor.selectedAnimationClip = created;
                editor.selectedAnimationTrack = 0;
                editor.selectedAnimationKey = 0;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: created authored animation clip");
                return;
            }
            editor.status = "Editor: create clip failed: " + error;
        }
        return;
    }

    editor.selectedAnimationClip = std::clamp(editor.selectedAnimationClip, 0, static_cast<int>(def.authoredClips.size()) - 1);
    pf::AnimationClip& clip = def.authoredClips[static_cast<size_t>(editor.selectedAnimationClip)];
    const int clipFrameCount = std::max(1, static_cast<int>(pf::fxToFloat(clip.frameCount)));
    editor.animationScrubFrame = std::clamp(editor.animationScrubFrame, 0, std::max(0, clipFrameCount - 1));

    DrawText(("Clip: " + clip.name + "  action=" + std::to_string(clip.actionIndex) +
              " frames=" + std::to_string(clipFrameCount)).c_str(),
        static_cast<int>(rect.x + 10.0f),
        static_cast<int>(rect.y + 34.0f),
        13,
        RAYWHITE);

    const float clipListW = std::clamp(rect.width * 0.30f, 112.0f, 164.0f);
    const float listY = rect.y + 62.0f;
    DrawText("Clips", static_cast<int>(rect.x + 10.0f), static_cast<int>(listY), 10, Fade(RAYWHITE, 0.62f));
    const int visibleClips = std::min(5, static_cast<int>(def.authoredClips.size()));
    const int clipStart = visibleListStart(editor.selectedAnimationClip, static_cast<int>(def.authoredClips.size()), visibleClips);
    for (int row = 0; row < visibleClips; ++row) {
        const int clipIndex = clipStart + row;
        const pf::AnimationClip& rowClip = def.authoredClips[static_cast<size_t>(clipIndex)];
        const std::string label = clippedText(rowClip.name + " a" + std::to_string(rowClip.actionIndex), 10, clipListW - 8.0f);
        if (uiListRow({rect.x + 10.0f, listY + 16.0f + 22.0f * row, clipListW, 20.0f}, label, clipIndex == editor.selectedAnimationClip)) {
            editor.selectedAnimationClip = clipIndex;
            editor.selectedAnimationTrack = 0;
            editor.selectedAnimationKey = 0;
            editor.animationScrubFrame = 0;
            editor.animationPreviewActive = true;
            editor.paused = true;
            return;
        }
    }

    if (uiButton({rect.x + 10.0f, listY + 134.0f, 58.0f, 22.0f}, "+Clip")) {
        std::string error;
        int created = -1;
        if (pf::createEditorSessionAuthoredClip(session, {}, -1, &created, &error)) {
            editor.selectedAnimationClip = created;
            editor.selectedAnimationTrack = 0;
            editor.selectedAnimationKey = 0;
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: created authored clip");
            return;
        }
        editor.status = "Editor: create clip failed: " + error;
    }
    if (uiButton({rect.x + 74.0f, listY + 134.0f, 58.0f, 22.0f}, "Clone")) {
        std::string error;
        int created = -1;
        if (pf::duplicateEditorSessionAuthoredClip(session, editor.selectedAnimationClip, &created, &error)) {
            editor.selectedAnimationClip = created;
            editor.selectedAnimationTrack = 0;
            editor.selectedAnimationKey = 0;
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: cloned authored clip");
            return;
        }
        editor.status = "Editor: clone clip failed: " + error;
    }
    if (uiButton({rect.x + 138.0f, listY + 134.0f, 58.0f, 22.0f}, "-Clip")) {
        std::string error;
        if (pf::removeEditorSessionAuthoredClip(session, editor.selectedAnimationClip, {}, &error)) {
            editor.selectedAnimationClip = std::clamp(editor.selectedAnimationClip, 0, std::max(0, static_cast<int>(session.package.fighters.front().authoredClips.size()) - 1));
            editor.selectedAnimationTrack = 0;
            editor.selectedAnimationKey = 0;
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed authored clip");
            return;
        }
        editor.status = "Editor: remove clip failed: " + error;
    }

    const float detailX = rect.x + clipListW + 22.0f;
    const float detailW = std::max(190.0f, rect.width - clipListW - 32.0f);
    const Rectangle scrub{detailX, rect.y + 62.0f, detailW, 34.0f};
    DrawRectangleRec(scrub, Fade(BLACK, 0.22f));
    DrawRectangleLinesEx(scrub, 1.0f, Fade(RAYWHITE, 0.35f));
    const float scrubT = static_cast<float>(editor.animationScrubFrame) / static_cast<float>(std::max(1, clipFrameCount - 1));
    const float playheadX = scrub.x + 6.0f + (scrub.width - 12.0f) * scrubT;
    DrawRectangle(static_cast<int>(playheadX), static_cast<int>(scrub.y - 3.0f), 2, static_cast<int>(scrub.height + 6.0f), ORANGE);
    DrawText(("Frame " + std::to_string(editor.animationScrubFrame) + "/" + std::to_string(std::max(0, clipFrameCount - 1))).c_str(),
        static_cast<int>(scrub.x + 8.0f),
        static_cast<int>(scrub.y + 10.0f),
        11,
        RAYWHITE);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), scrub)) {
        const float t = std::clamp((GetMousePosition().x - scrub.x - 6.0f) / (scrub.width - 12.0f), 0.0f, 1.0f);
        editor.animationScrubFrame = static_cast<int>(std::round(t * static_cast<float>(std::max(0, clipFrameCount - 1))));
        editor.animationPreviewActive = true;
        editor.paused = true;
    }
    if (uiButton({detailX, rect.y + 104.0f, 52.0f, 22.0f}, "Prev")) {
        editor.animationScrubFrame = std::max(0, editor.animationScrubFrame - 1);
        editor.animationPreviewActive = true;
        editor.paused = true;
    }
    if (uiButton({detailX + 58.0f, rect.y + 104.0f, 52.0f, 22.0f}, "Next")) {
        editor.animationScrubFrame = std::min(std::max(0, clipFrameCount - 1), editor.animationScrubFrame + 1);
        editor.animationPreviewActive = true;
        editor.paused = true;
    }
    if (uiButton({detailX + 116.0f, rect.y + 104.0f, 64.0f, 22.0f}, "Preview", editor.animationPreviewActive)) {
        editor.animationPreviewActive = !editor.animationPreviewActive;
        editor.paused = true;
    }

    const float trackY = rect.y + 140.0f;
    DrawText(("Tracks " + std::to_string(clip.tracks.size()) +
              "  Skeleton joints " + std::to_string(def.authoredSkeleton.size())).c_str(),
        static_cast<int>(detailX),
        static_cast<int>(trackY),
        11,
        Fade(RAYWHITE, 0.7f));
    if (uiButton({detailX, trackY + 18.0f, 64.0f, 22.0f}, "+Track")) {
        pf::AnimationTrack track;
        track.joint = std::clamp(editor.selectedAnimationJoint, 0, std::max(0, static_cast<int>(def.authoredSkeleton.size()) - 1));
        track.channel = pf::AnimationChannel::TranslateX;
        track.keys = {
            {0, 0, 0, pf::AnimationInterpolation::Linear},
            {clip.frameCount, 0, 0, pf::AnimationInterpolation::Linear},
        };
        std::string error;
        int added = -1;
        if (pf::addEditorSessionAuthoredTrack(session, editor.selectedAnimationClip, track, -1, &added, &error)) {
            editor.selectedAnimationTrack = added;
            editor.selectedAnimationKey = 0;
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added authored animation track");
            return;
        }
        editor.status = "Editor: add track failed: " + error;
    }

    if (!clip.tracks.empty()) {
        editor.selectedAnimationTrack = std::clamp(editor.selectedAnimationTrack, 0, static_cast<int>(clip.tracks.size()) - 1);
        pf::AnimationTrack& track = clip.tracks[static_cast<size_t>(editor.selectedAnimationTrack)];
        DrawText(("Track #" + std::to_string(editor.selectedAnimationTrack) +
                  " joint=" + std::to_string(track.joint) +
                  " " + animationChannelName(track.channel)).c_str(),
            static_cast<int>(detailX + 74.0f),
            static_cast<int>(trackY + 23.0f),
            11,
            RAYWHITE);
        if (uiButton({detailX, trackY + 48.0f, 48.0f, 22.0f}, "Tr<")) {
            editor.selectedAnimationTrack = wrappedIndex(editor.selectedAnimationTrack - 1, static_cast<int>(clip.tracks.size()));
            editor.selectedAnimationKey = 0;
            return;
        }
        if (uiButton({detailX + 54.0f, trackY + 48.0f, 48.0f, 22.0f}, "Tr>")) {
            editor.selectedAnimationTrack = wrappedIndex(editor.selectedAnimationTrack + 1, static_cast<int>(clip.tracks.size()));
            editor.selectedAnimationKey = 0;
            return;
        }
        if (uiButton({detailX + 108.0f, trackY + 48.0f, 54.0f, 22.0f}, "Chan")) {
            pf::AnimationTrack edited = track;
            edited.channel = nextAnimationChannel(edited.channel);
            std::string error;
            if (pf::setEditorSessionAuthoredTrack(session, editor.selectedAnimationClip, editor.selectedAnimationTrack, edited, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: changed animation track channel");
                return;
            }
            editor.status = "Editor: track edit failed: " + error;
        }
        if (uiButton({detailX + 168.0f, trackY + 48.0f, 62.0f, 22.0f}, "UseJnt")) {
            pf::AnimationTrack edited = track;
            edited.joint = std::clamp(editor.selectedAnimationJoint, 0, std::max(0, static_cast<int>(def.authoredSkeleton.size()) - 1));
            std::string error;
            if (pf::setEditorSessionAuthoredTrack(session, editor.selectedAnimationClip, editor.selectedAnimationTrack, edited, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: assigned track to selected joint");
                return;
            }
            editor.status = "Editor: track edit failed: " + error;
        }
        if (uiButton({detailX + 236.0f, trackY + 48.0f, 58.0f, 22.0f}, "-Track")) {
            std::string error;
            if (pf::removeEditorSessionAuthoredTrack(session, editor.selectedAnimationClip, editor.selectedAnimationTrack, &error)) {
                editor.selectedAnimationTrack = std::clamp(editor.selectedAnimationTrack, 0, std::max(0, static_cast<int>(clip.tracks.size()) - 1));
                editor.selectedAnimationKey = 0;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed authored animation track");
                return;
            }
            editor.status = "Editor: remove track failed: " + error;
        }

        const Rectangle keyLane{detailX, trackY + 82.0f, detailW, 32.0f};
        DrawRectangleRec(keyLane, Fade(BLACK, 0.18f));
        DrawRectangleLinesEx(keyLane, 1.0f, Fade(RAYWHITE, 0.28f));
        for (size_t i = 0; i < track.keys.size(); ++i) {
            const pf::AnimationKey& key = track.keys[i];
            const float t = std::clamp(pf::fxToFloat(key.frame) / static_cast<float>(std::max(1, clipFrameCount - 1)), 0.0f, 1.0f);
            const float x = keyLane.x + 6.0f + (keyLane.width - 12.0f) * t;
            DrawCircle(static_cast<int>(x), static_cast<int>(keyLane.y + keyLane.height * 0.5f), static_cast<int>(i) == editor.selectedAnimationKey ? 5.0f : 3.0f, static_cast<int>(i) == editor.selectedAnimationKey ? ORANGE : SKYBLUE);
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), keyLane) && !track.keys.empty()) {
            const float t = std::clamp((GetMousePosition().x - keyLane.x - 6.0f) / (keyLane.width - 12.0f), 0.0f, 1.0f);
            const int clickedFrame = static_cast<int>(std::round(t * static_cast<float>(std::max(0, clipFrameCount - 1))));
            int bestKey = 0;
            int bestDistance = 1000000;
            for (size_t i = 0; i < track.keys.size(); ++i) {
                const int keyFrame = static_cast<int>(pf::fxToFloat(track.keys[i].frame));
                const int distance = std::abs(keyFrame - clickedFrame);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestKey = static_cast<int>(i);
                }
            }
            editor.selectedAnimationKey = bestKey;
            editor.animationScrubFrame = static_cast<int>(pf::fxToFloat(track.keys[static_cast<size_t>(bestKey)].frame));
            editor.animationPreviewActive = true;
            editor.paused = true;
        }
        const float keyY = trackY + 122.0f;
        if (uiButton({detailX, keyY, 54.0f, 22.0f}, "+Key")) {
            pf::AnimationKey key;
            key.frame = pf::fx(editor.animationScrubFrame);
            key.value = pf::sampleTrack(track, key.frame);
            key.interpolation = pf::AnimationInterpolation::Linear;
            std::string error;
            int added = -1;
            if (pf::addEditorSessionAuthoredKey(session, editor.selectedAnimationClip, editor.selectedAnimationTrack, key, &added, &error)) {
                editor.selectedAnimationKey = added;
                editor.animationPreviewActive = true;
                editor.paused = true;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added authored animation key");
                return;
            }
            editor.status = "Editor: add key failed: " + error;
        }
        if (!track.keys.empty()) {
            editor.selectedAnimationKey = std::clamp(editor.selectedAnimationKey, 0, static_cast<int>(track.keys.size()) - 1);
            const pf::AnimationKey& key = track.keys[static_cast<size_t>(editor.selectedAnimationKey)];
            DrawText(("Key #" + std::to_string(editor.selectedAnimationKey) +
                      " f=" + std::to_string(static_cast<int>(pf::fxToFloat(key.frame))) +
                      " v=" + std::to_string(pf::fxToFloat(key.value))).c_str(),
                static_cast<int>(detailX + 64.0f),
                static_cast<int>(keyY + 5.0f),
                10,
                RAYWHITE);
            auto commitKey = [&](pf::AnimationKey edited, const std::string& message) -> bool {
                std::string error;
                if (pf::setEditorSessionAuthoredKey(session, editor.selectedAnimationClip, editor.selectedAnimationTrack, editor.selectedAnimationKey, edited, &error)) {
                    editor.animationPreviewActive = true;
                    editor.paused = true;
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                    return true;
                }
                editor.status = "Editor: key edit failed: " + error;
                return false;
            };
            pf::AnimationKey editedKey = key;
            if (uiButton({detailX, keyY + 28.0f, 44.0f, 22.0f}, "Val-")) {
                editedKey.value -= pf::fxFromFloat(0.05f);
                if (commitKey(editedKey, "Editor: lowered authored key value")) return;
            }
            if (uiButton({detailX + 50.0f, keyY + 28.0f, 44.0f, 22.0f}, "Val+")) {
                editedKey.value += pf::fxFromFloat(0.05f);
                if (commitKey(editedKey, "Editor: raised authored key value")) return;
            }
            if (uiButton({detailX + 100.0f, keyY + 28.0f, 44.0f, 22.0f}, "Fr-")) {
                editedKey.frame = pf::fx(std::max(0, static_cast<int>(pf::fxToFloat(editedKey.frame)) - 1));
                editor.animationScrubFrame = static_cast<int>(pf::fxToFloat(editedKey.frame));
                if (commitKey(editedKey, "Editor: moved authored key earlier")) return;
            }
            if (uiButton({detailX + 150.0f, keyY + 28.0f, 44.0f, 22.0f}, "Fr+")) {
                editedKey.frame = pf::fx(std::min(std::max(0, clipFrameCount - 1), static_cast<int>(pf::fxToFloat(editedKey.frame)) + 1));
                editor.animationScrubFrame = static_cast<int>(pf::fxToFloat(editedKey.frame));
                if (commitKey(editedKey, "Editor: moved authored key later")) return;
            }
            if (uiButton({detailX + 200.0f, keyY + 28.0f, 58.0f, 22.0f}, "-Key")) {
                std::string error;
                if (pf::removeEditorSessionAuthoredKey(session, editor.selectedAnimationClip, editor.selectedAnimationTrack, editor.selectedAnimationKey, &error)) {
                    editor.selectedAnimationKey = 0;
                    editor.animationPreviewActive = true;
                    editor.paused = true;
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed authored key");
                    return;
                }
                editor.status = "Editor: remove key failed: " + error;
            }
        }
    }

    if (editor.animationPreviewActive) {
        pf::previewFighterAnimation(world, static_cast<size_t>(editor.selectedFighter), clip.actionIndex, pf::fx(editor.animationScrubFrame));
    }
}

static void drawEditorLogicGraphWorkstation(
    pf::World& world,
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    int& selectedFighterDef,
    pf::FighterDefinition& def,
    Rectangle rect)
{
    drawPanelChrome(rect, "Logic Graph");
    if (def.packageScripts.empty()) {
        DrawText("No package scripts", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 36.0f), 13, Fade(RAYWHITE, 0.7f));
        if (uiButton({rect.x + 10.0f, rect.y + 58.0f, 90.0f, 24.0f}, "+ Script")) {
            std::string error;
            int added = -1;
            if (pf::addEditorSessionPackageScript(session, {}, 64, &added, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added package script");
                editor.selectionKind = pf::FighterEditorSelectionKind::Script;
            } else {
                editor.status = "Editor: add script failed: " + error;
            }
        }
        drawEditorPackageVariableStrip(world, editor, session, selectedFighterDef, def, {rect.x + 10.0f, rect.y + 92.0f, rect.width - 20.0f, 44.0f});
        return;
    }
    editor.selectedPackageScript = std::clamp(editor.selectedPackageScript, 0, static_cast<int>(def.packageScripts.size()) - 1);
    session.selectedPackageScript = editor.selectedPackageScript;
    editor.selectedPackageInstruction = std::clamp(editor.selectedPackageInstruction, 0, std::max(0, static_cast<int>(def.packageScripts[static_cast<size_t>(editor.selectedPackageScript)].instructions.size()) - 1));
    session.selectedPackageInstruction = editor.selectedPackageInstruction;
    pf::PackageScript& script = def.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
    DrawText(clippedText(script.name + "  budget " + std::to_string(script.instructionBudget), 13, rect.width - 296.0f).c_str(),
        static_cast<int>(rect.x + 10.0f),
        static_cast<int>(rect.y + 34.0f),
        13,
        RAYWHITE);
    if (uiButton({rect.x + rect.width - 288.0f, rect.y + 29.0f, 38.0f, 22.0f}, "<")) {
        editor.selectedPackageScript = wrappedIndex(editor.selectedPackageScript - 1, static_cast<int>(def.packageScripts.size()));
        editor.selectedPackageInstruction = 0;
        editor.selectedPackageGraphNode = 0;
        session.selectedPackageScript = editor.selectedPackageScript;
        session.selectedPackageInstruction = 0;
    }
    if (uiButton({rect.x + rect.width - 246.0f, rect.y + 29.0f, 38.0f, 22.0f}, ">")) {
        editor.selectedPackageScript = wrappedIndex(editor.selectedPackageScript + 1, static_cast<int>(def.packageScripts.size()));
        editor.selectedPackageInstruction = 0;
        editor.selectedPackageGraphNode = 0;
        session.selectedPackageScript = editor.selectedPackageScript;
        session.selectedPackageInstruction = 0;
    }
    if (uiButton({rect.x + rect.width - 202.0f, rect.y + 29.0f, 58.0f, 22.0f}, "+Script")) {
        std::string error;
        int added = -1;
        if (pf::addEditorSessionPackageScript(session, {}, 64, &added, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added package script");
            editor.selectionKind = pf::FighterEditorSelectionKind::Script;
            return;
        }
        editor.status = "Editor: add script failed: " + error;
    }
    if (uiButton({rect.x + rect.width - 138.0f, rect.y + 29.0f, 58.0f, 22.0f}, "Clone")) {
        std::string error;
        int added = -1;
        if (pf::duplicateEditorSessionPackageScript(session, editor.selectedPackageScript, &added, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: cloned package script");
            editor.selectionKind = pf::FighterEditorSelectionKind::Script;
            return;
        }
        editor.status = "Editor: clone script failed: " + error;
    }
    if (uiButton({rect.x + rect.width - 74.0f, rect.y + 29.0f, 58.0f, 22.0f}, "-Script")) {
        std::string error;
        if (pf::removeEditorSessionPackageScript(session, editor.selectedPackageScript, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed package script");
            return;
        }
        editor.status = "Editor: remove script failed: " + error;
    }

    drawEditorPackageVariableStrip(world, editor, session, selectedFighterDef, def, {rect.x + 10.0f, rect.y + 56.0f, rect.width - 20.0f, 44.0f});

    const float listY = rect.y + 108.0f;
    const float scriptListW = std::clamp(rect.width * 0.32f, 108.0f, 170.0f);
    const float instructionListX = rect.x + 18.0f + scriptListW;
    const float instructionListW = std::max(124.0f, rect.width - scriptListW - 28.0f);
    DrawText("Scripts", static_cast<int>(rect.x + 10.0f), static_cast<int>(listY), 10, Fade(RAYWHITE, 0.62f));
    DrawText("Instructions", static_cast<int>(instructionListX), static_cast<int>(listY), 10, Fade(RAYWHITE, 0.62f));
    const int visibleScripts = std::min(2, static_cast<int>(def.packageScripts.size()));
    const int scriptStart = visibleListStart(editor.selectedPackageScript, static_cast<int>(def.packageScripts.size()), visibleScripts);
    for (int row = 0; row < visibleScripts; ++row) {
        const int scriptIndex = scriptStart + row;
        const pf::PackageScript& rowScript = def.packageScripts[static_cast<size_t>(scriptIndex)];
        if (uiListRow({rect.x + 10.0f, listY + 16.0f + 22.0f * row, scriptListW, 20.0f},
                clippedText(rowScript.name + " (" + std::to_string(rowScript.instructions.size()) + ")", 10, scriptListW - 8.0f),
                scriptIndex == editor.selectedPackageScript))
        {
            editor.selectedPackageScript = scriptIndex;
            editor.selectedPackageInstruction = 0;
            editor.selectedPackageGraphNode = 0;
            editor.selectionKind = pf::FighterEditorSelectionKind::Script;
            session.selectedPackageScript = scriptIndex;
            session.selectedPackageInstruction = 0;
            return;
        }
    }
    const int visibleInstructions = std::min(2, static_cast<int>(script.instructions.size()));
    const int instructionStart = visibleListStart(editor.selectedPackageInstruction, static_cast<int>(script.instructions.size()), visibleInstructions);
    for (int row = 0; row < visibleInstructions; ++row) {
        const int instructionIndex = instructionStart + row;
        const pf::PackageScriptInstruction& instruction = script.instructions[static_cast<size_t>(instructionIndex)];
        if (uiListRow({instructionListX, listY + 16.0f + 22.0f * row, instructionListW, 20.0f},
                clippedText("#" + std::to_string(instructionIndex) + " " + packageInstructionLabel(instruction), 10, instructionListW - 8.0f),
                instructionIndex == editor.selectedPackageInstruction))
        {
            editor.selectedPackageInstruction = instructionIndex;
            editor.selectionKind = pf::FighterEditorSelectionKind::Instruction;
            session.selectedPackageInstruction = instructionIndex;
        }
    }
    if (script.instructions.empty()) {
        DrawText("No instructions", static_cast<int>(instructionListX + 6.0f), static_cast<int>(listY + 22.0f), 11, Fade(RAYWHITE, 0.5f));
    }

    const float actionY = listY + 66.0f;
    auto addInstruction = [&](pf::PackageScriptInstruction instruction, const std::string& label) -> bool {
        normalizePackageInstruction(instruction, def, world, selectedFighterDef, editor.selectedState, editor.selectedObjectDef);
        std::string error;
        int added = -1;
        if (pf::addEditorSessionPackageInstruction(session, editor.selectedPackageScript, instruction, -1, &added, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added " + label + " instruction");
            editor.selectionKind = pf::FighterEditorSelectionKind::Instruction;
            return true;
        }
        editor.status = "Editor: add instruction failed: " + error;
        return false;
    };
    if (uiButton({rect.x + 10.0f, actionY, 54.0f, 22.0f}, "+Nop")) {
        if (addInstruction({pf::PackageScriptOp::Nop, -1, -1, -1, 0, 0, {}}, "Nop")) return;
    }
    if (uiButton({rect.x + 70.0f, actionY, 54.0f, 22.0f}, "+Read")) {
        if (addInstruction({pf::PackageScriptOp::SetVarFrame, editor.selectedPackageVariable, -1, -1, 0, 0, {}}, "fact read")) return;
    }
    if (uiButton({rect.x + 130.0f, actionY, 54.0f, 22.0f}, "+Vel")) {
        if (addInstruction({pf::PackageScriptOp::SetAirVelocityX, -1, -1, -1, 0, pf::fxFromFloat(0.5f), {}}, "velocity")) return;
    }
    if (uiButton({rect.x + 190.0f, actionY, 54.0f, 22.0f}, "+State")) {
        if (addInstruction({pf::PackageScriptOp::ChangeState, -1, -1, -1, 0, 0, {}}, "state change")) return;
    }
    if (uiButton({rect.x + 250.0f, actionY, 54.0f, 22.0f}, "+Call")) {
        if (addInstruction({pf::PackageScriptOp::CallScript, -1, -1, -1, 0, 0, {}}, "script call")) return;
    }
    if (uiButton({rect.x + 310.0f, actionY, 54.0f, 22.0f}, "-Instr")) {
        std::string error;
        if (pf::removeEditorSessionPackageInstruction(session, editor.selectedPackageScript, editor.selectedPackageInstruction, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed package instruction");
            return;
        }
        editor.status = "Editor: remove instruction failed: " + error;
    }
    if (uiButton({rect.x + 370.0f, actionY, 42.0f, 22.0f}, "Up")) {
        std::string error;
        int moved = -1;
        if (pf::moveEditorSessionPackageInstruction(session, editor.selectedPackageScript, editor.selectedPackageInstruction, -1, &moved, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: moved package instruction up");
            return;
        }
        editor.status = "Editor: move instruction failed: " + error;
    }
    if (uiButton({rect.x + 418.0f, actionY, 42.0f, 22.0f}, "Down")) {
        std::string error;
        int moved = -1;
        if (pf::moveEditorSessionPackageInstruction(session, editor.selectedPackageScript, editor.selectedPackageInstruction, 1, &moved, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: moved package instruction down");
            return;
        }
        editor.status = "Editor: move instruction failed: " + error;
    }
    if (uiButton({rect.x + rect.width - 186.0f, actionY, 56.0f, 22.0f}, "Linear")) {
        std::string error;
        if (pf::setEditorSessionPackageScriptGraph(session, editor.selectedPackageScript, pf::makePackageScriptLinearGraph(script), &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: rebuilt linear graph metadata");
            return;
        }
        editor.status = "Editor: graph rebuild failed: " + error;
    }
    if (uiButton({rect.x + rect.width - 124.0f, actionY, 52.0f, 22.0f}, "Flow")) {
        std::string error;
        if (pf::setEditorSessionPackageScriptGraph(session, editor.selectedPackageScript, pf::makePackageScriptControlFlowGraph(script), &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: rebuilt control-flow graph metadata");
            return;
        }
        editor.status = "Editor: graph rebuild failed: " + error;
    }
    if (uiButton({rect.x + rect.width - 66.0f, actionY, 50.0f, 22.0f}, "Compile")) {
        std::string error;
        if (pf::compileEditorSessionPackageScriptGraph(session, editor.selectedPackageScript, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: compiled graph into package script");
            return;
        }
        editor.status = "Editor: graph compile failed: " + error;
    }

    const Rectangle canvas{rect.x + 10.0f, actionY + 30.0f, rect.width - 20.0f, std::max(52.0f, rect.y + rect.height - actionY - 40.0f)};
    DrawRectangleRec(canvas, {12, 16, 20, 255});
    DrawRectangleLinesEx(canvas, 1.0f, {54, 64, 76, 255});
    for (int gx = 0; gx < static_cast<int>(canvas.width); gx += 32) {
        DrawLine(static_cast<int>(canvas.x + gx), static_cast<int>(canvas.y), static_cast<int>(canvas.x + gx), static_cast<int>(canvas.y + canvas.height), Fade(RAYWHITE, 0.06f));
    }
    for (int gy = 0; gy < static_cast<int>(canvas.height); gy += 32) {
        DrawLine(static_cast<int>(canvas.x), static_cast<int>(canvas.y + gy), static_cast<int>(canvas.x + canvas.width), static_cast<int>(canvas.y + gy), Fade(RAYWHITE, 0.06f));
    }
    if (script.graph.nodes.empty()) {
        drawPackageScriptBlockGraph(script, editor, canvas, &session);
        return;
    }
    auto selectedGraphNodeIt = std::find_if(script.graph.nodes.begin(), script.graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) {
        return node.id == editor.selectedPackageGraphNode;
    });
    if (selectedGraphNodeIt == script.graph.nodes.end()) {
        editor.selectedPackageGraphNode = script.graph.entryNode;
        selectedGraphNodeIt = std::find_if(script.graph.nodes.begin(), script.graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) {
            return node.id == editor.selectedPackageGraphNode;
        });
        if (selectedGraphNodeIt == script.graph.nodes.end()) {
            editor.selectedPackageGraphNode = script.graph.nodes.front().id;
            selectedGraphNodeIt = script.graph.nodes.begin();
        }
    }
    auto selectedGraphNodeIndex = [&]() {
        const auto found = std::find_if(script.graph.nodes.begin(), script.graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) {
            return node.id == editor.selectedPackageGraphNode;
        });
        return found == script.graph.nodes.end()
            ? 0
            : static_cast<int>(std::distance(script.graph.nodes.begin(), found));
    };
    auto selectGraphNodeByIndex = [&](int index) {
        if (script.graph.nodes.empty()) {
            return;
        }
        const int nodeIndex = wrappedIndex(index, static_cast<int>(script.graph.nodes.size()));
        const pf::PackageScriptGraphNode& node = script.graph.nodes[static_cast<size_t>(nodeIndex)];
        editor.selectedPackageGraphNode = node.id;
        if (node.kind == pf::PackageScriptGraphNodeKind::Instruction && node.instructionIndex >= 0) {
            editor.selectedPackageInstruction = std::clamp(node.instructionIndex, 0, std::max(0, static_cast<int>(script.instructions.size()) - 1));
            session.selectedPackageInstruction = editor.selectedPackageInstruction;
            editor.selectionKind = pf::FighterEditorSelectionKind::Instruction;
        } else {
            editor.selectionKind = pf::FighterEditorSelectionKind::Script;
        }
    };
    auto nudgeSelectedGraphNode = [&](pf::Fix dx, pf::Fix dy, const std::string& label) -> bool {
        const auto found = std::find_if(script.graph.nodes.begin(), script.graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) {
            return node.id == editor.selectedPackageGraphNode;
        });
        if (found == script.graph.nodes.end()) {
            return false;
        }
        pf::PackageScriptGraphNode edited = *found;
        edited.position.x += dx;
        edited.position.y += dy;
        std::string error;
        if (pf::setEditorSessionPackageScriptGraphNode(session, editor.selectedPackageScript, edited.id, edited, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: nudged graph node " + label);
            return true;
        }
        editor.status = "Editor: graph node edit failed: " + error;
        return false;
    };
    const float graphControlY = actionY + 30.0f;
    DrawText(("Node #" + std::to_string(editor.selectedPackageGraphNode)).c_str(),
        static_cast<int>(rect.x + 12.0f),
        static_cast<int>(graphControlY + 6.0f),
        10,
        Fade(RAYWHITE, 0.68f));
    if (uiButton({rect.x + 86.0f, graphControlY, 36.0f, 22.0f}, "N<")) {
        selectGraphNodeByIndex(selectedGraphNodeIndex() - 1);
        return;
    }
    if (uiButton({rect.x + 128.0f, graphControlY, 36.0f, 22.0f}, "N>")) {
        selectGraphNodeByIndex(selectedGraphNodeIndex() + 1);
        return;
    }
    if (uiButton({rect.x + 174.0f, graphControlY, 34.0f, 22.0f}, "X-")) {
        if (nudgeSelectedGraphNode(-pf::fx(16), 0, "left")) return;
    }
    if (uiButton({rect.x + 214.0f, graphControlY, 34.0f, 22.0f}, "X+")) {
        if (nudgeSelectedGraphNode(pf::fx(16), 0, "right")) return;
    }
    if (uiButton({rect.x + 254.0f, graphControlY, 34.0f, 22.0f}, "Y-")) {
        if (nudgeSelectedGraphNode(0, -pf::fx(16), "up")) return;
    }
    if (uiButton({rect.x + 294.0f, graphControlY, 34.0f, 22.0f}, "Y+")) {
        if (nudgeSelectedGraphNode(0, pf::fx(16), "down")) return;
    }
    const Rectangle graphCanvas{canvas.x, canvas.y + 28.0f, canvas.width, std::max(28.0f, canvas.height - 28.0f)};
    const float scale = 0.72f;
    for (const pf::PackageScriptGraphLink& link : script.graph.links) {
        const auto from = std::find_if(script.graph.nodes.begin(), script.graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) { return node.id == link.fromNode; });
        const auto to = std::find_if(script.graph.nodes.begin(), script.graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) { return node.id == link.toNode; });
        if (from == script.graph.nodes.end() || to == script.graph.nodes.end()) {
            continue;
        }
        const Vector2 a{graphCanvas.x + 40.0f + pf::fxToFloat(from->position.x) * scale + 104.0f, graphCanvas.y + 28.0f + pf::fxToFloat(from->position.y) * scale + 22.0f};
        const Vector2 b{graphCanvas.x + 40.0f + pf::fxToFloat(to->position.x) * scale, graphCanvas.y + 28.0f + pf::fxToFloat(to->position.y) * scale + 22.0f};
        DrawLineBezier(a, b, 2.0f, Fade(SKYBLUE, 0.6f));
    }
    for (const pf::PackageScriptGraphNode& node : script.graph.nodes) {
        const Rectangle nodeRect{
            graphCanvas.x + 40.0f + pf::fxToFloat(node.position.x) * scale,
            graphCanvas.y + 28.0f + pf::fxToFloat(node.position.y) * scale,
            node.kind == pf::PackageScriptGraphNodeKind::Entry ? 86.0f : 112.0f,
            44.0f,
        };
        const bool selected = node.id == editor.selectedPackageGraphNode ||
            (node.instructionIndex == editor.selectedPackageInstruction && node.kind == pf::PackageScriptGraphNodeKind::Instruction);
        const Color fill = node.kind == pf::PackageScriptGraphNodeKind::Entry ? Fade(PURPLE, 0.74f) : (selected ? Fade(ORANGE, 0.78f) : Fade(BLUE, 0.56f));
        DrawRectangleRounded(nodeRect, 0.08f, 6, fill);
        DrawRectangleRoundedLines(nodeRect, 0.08f, 6, selected ? ORANGE : Fade(RAYWHITE, 0.5f));
        const std::string label = node.kind == pf::PackageScriptGraphNodeKind::Entry
            ? "Entry"
            : (node.instructionIndex >= 0 && node.instructionIndex < static_cast<int>(script.instructions.size())
                ? packageScriptOpName(script.instructions[static_cast<size_t>(node.instructionIndex)].op)
                : "Node");
        DrawText(clippedText(label, 11, nodeRect.width - 12.0f).c_str(), static_cast<int>(nodeRect.x + 7.0f), static_cast<int>(nodeRect.y + 9.0f), 11, RAYWHITE);
        DrawText(("#" + std::to_string(node.id)).c_str(), static_cast<int>(nodeRect.x + 7.0f), static_cast<int>(nodeRect.y + 25.0f), 10, Fade(RAYWHITE, 0.68f));
        if (CheckCollisionPointRec(GetMousePosition(), nodeRect) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            editor.selectedPackageGraphNode = node.id;
            if (node.kind == pf::PackageScriptGraphNodeKind::Instruction && node.instructionIndex >= 0) {
                editor.selectedPackageInstruction = node.instructionIndex;
                editor.selectionKind = pf::FighterEditorSelectionKind::Instruction;
                session.selectedPackageInstruction = node.instructionIndex;
            } else {
                editor.selectionKind = pf::FighterEditorSelectionKind::Script;
            }
        }
    }
}

static void drawEditorInspectorWorkstation(
    pf::World& world,
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    int& selectedFighterDef,
    pf::FighterDefinition& def,
    pf::FighterState& state,
    Rectangle rect)
{
    drawPanelChrome(rect, "Inspector");
    DrawText(("Context: " + std::string(editorSelectionKindName(editor.selectionKind))).c_str(),
        static_cast<int>(std::max(rect.x + 118.0f, rect.x + rect.width - 178.0f)),
        static_cast<int>(rect.y + 6.0f),
        11,
        Fade(RAYWHITE, 0.68f));
    if (editor.selectionKind == pf::FighterEditorSelectionKind::Instruction &&
        editor.workspace == pf::EditorWorkspace::Assets &&
        !session.package.objects.empty())
    {
        DrawText("Object Instruction", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 34.0f), 12, Fade(RAYWHITE, 0.7f));
        editor.selectedObjectDef = std::clamp(editor.selectedObjectDef, 0, static_cast<int>(session.package.objects.size()) - 1);
        pf::GameObjectDefinition& object = session.package.objects[static_cast<size_t>(editor.selectedObjectDef)];
        if (object.packageScripts.empty()) {
            DrawText("Selected object has no scripts", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 62.0f), 12, Fade(RAYWHITE, 0.62f));
            return;
        }
        editor.selectedPackageScript = std::clamp(editor.selectedPackageScript, 0, static_cast<int>(object.packageScripts.size()) - 1);
        pf::PackageScript& script = object.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
        DrawText(clippedText(object.name + "  " + script.name, 11, rect.width - 20.0f).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(rect.y + 62.0f),
            11,
            RAYWHITE);
        if (script.instructions.empty()) {
            DrawText("Selected object script has no instructions", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 86.0f), 11, Fade(RAYWHITE, 0.62f));
            return;
        }
        editor.selectedPackageInstruction = std::clamp(editor.selectedPackageInstruction, 0, static_cast<int>(script.instructions.size()) - 1);
        const pf::PackageScriptInstruction& selectedInstruction = script.instructions[static_cast<size_t>(editor.selectedPackageInstruction)];
        DrawText(clippedText("#" + std::to_string(editor.selectedPackageInstruction) + "  " + packageInstructionLabel(selectedInstruction), 11, rect.width - 20.0f).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(rect.y + 84.0f),
            11,
            Fade(RAYWHITE, 0.76f));
        auto commitObjectInstruction = [&](pf::PackageScriptInstruction instruction, const std::string& message) -> bool {
            normalizeObjectPackageInstruction(instruction, object, world, editor.selectedObjectState, editor.selectedObjectDef);
            std::string error;
            if (pf::setEditorSessionObjectPackageInstruction(
                    session,
                    editor.selectedObjectDef,
                    editor.selectedPackageScript,
                    editor.selectedPackageInstruction,
                    instruction,
                    &error))
            {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                return true;
            }
            editor.status = "Editor: object instruction edit failed: " + error;
            return false;
        };
        pf::PackageScriptInstruction edited = selectedInstruction;
        const float rowA = rect.y + 112.0f;
        if (uiButton({rect.x + 10.0f, rowA, 52.0f, 22.0f}, "Op>")) {
            edited.op = nextObjectPackageScriptOp(edited.op);
            if (commitObjectInstruction(edited, "Editor: cycled object instruction op")) return;
        }
        if (uiButton({rect.x + 68.0f, rowA, 44.0f, 22.0f}, "Dst-")) {
            --edited.dst;
            if (commitObjectInstruction(edited, "Editor: changed object instruction dst")) return;
        }
        if (uiButton({rect.x + 118.0f, rowA, 44.0f, 22.0f}, "Dst+")) {
            ++edited.dst;
            if (commitObjectInstruction(edited, "Editor: changed object instruction dst")) return;
        }
        if (uiButton({rect.x + 168.0f, rowA, 42.0f, 22.0f}, "A-")) {
            --edited.srcA;
            if (commitObjectInstruction(edited, "Editor: changed object instruction srcA")) return;
        }
        if (uiButton({rect.x + 216.0f, rowA, 42.0f, 22.0f}, "A+")) {
            ++edited.srcA;
            if (commitObjectInstruction(edited, "Editor: changed object instruction srcA")) return;
        }
        if (uiButton({rect.x + 264.0f, rowA, 42.0f, 22.0f}, "B-")) {
            --edited.srcB;
            if (commitObjectInstruction(edited, "Editor: changed object instruction srcB")) return;
        }
        if (uiButton({rect.x + 312.0f, rowA, 42.0f, 22.0f}, "B+")) {
            ++edited.srcB;
            if (commitObjectInstruction(edited, "Editor: changed object instruction srcB")) return;
        }
        const float rowB = rowA + 30.0f;
        DrawText(("dst=" + std::to_string(selectedInstruction.dst) +
                  " a=" + std::to_string(selectedInstruction.srcA) +
                  " b=" + std::to_string(selectedInstruction.srcB) +
                  " i=" + std::to_string(selectedInstruction.intValue) +
                  " f=" + std::to_string(pf::fxToFloat(selectedInstruction.fixValue))).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(rowB + 4.0f),
            10,
            Fade(RAYWHITE, 0.64f));
        if (uiButton({rect.x + rect.width - 178.0f, rowB, 42.0f, 22.0f}, "I-")) {
            --edited.intValue;
            if (commitObjectInstruction(edited, "Editor: changed object instruction integer")) return;
        }
        if (uiButton({rect.x + rect.width - 130.0f, rowB, 42.0f, 22.0f}, "I+")) {
            ++edited.intValue;
            if (commitObjectInstruction(edited, "Editor: changed object instruction integer")) return;
        }
        if (uiButton({rect.x + rect.width - 82.0f, rowB, 32.0f, 22.0f}, "F-")) {
            edited.fixValue -= pf::fxFromFloat(0.1f);
            if (commitObjectInstruction(edited, "Editor: changed object instruction fixed value")) return;
        }
        if (uiButton({rect.x + rect.width - 44.0f, rowB, 32.0f, 22.0f}, "F+")) {
            edited.fixValue += pf::fxFromFloat(0.1f);
            if (commitObjectInstruction(edited, "Editor: changed object instruction fixed value")) return;
        }
        std::string committedText;
        if (uiTextField({rect.x + 10.0f, rowB + 30.0f, rect.width - 20.0f, 22.0f},
                "workstation-object-instruction-text",
                editor,
                selectedInstruction.text,
                committedText,
                64))
        {
            edited.text = committedText;
            if (commitObjectInstruction(edited, "Editor: changed object instruction target text")) return;
        }
        return;
    }
    if (editor.selectionKind == pf::FighterEditorSelectionKind::ObjectCallback) {
        DrawText("Object Callback", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 34.0f), 12, Fade(RAYWHITE, 0.7f));
        if (session.package.objects.empty()) {
            DrawText("No object definitions in this package", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 62.0f), 12, Fade(RAYWHITE, 0.62f));
            return;
        }
        editor.selectedObjectDef = std::clamp(editor.selectedObjectDef, 0, static_cast<int>(session.package.objects.size()) - 1);
        pf::GameObjectDefinition& object = session.package.objects[static_cast<size_t>(editor.selectedObjectDef)];
        const int stateSlot = wrappedIndex(editor.selectedObjectStateCallback, 4);
        const int eventSlot = wrappedIndex(editor.selectedObjectEventCallback, 21);
        if (!editor.selectedObjectCallbackEvent && object.states.empty()) {
            DrawText("Selected object has no states for state callbacks", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 62.0f), 11, Fade(RAYWHITE, 0.62f));
            return;
        }
        if (!object.states.empty()) {
            editor.selectedObjectState = std::clamp(editor.selectedObjectState, 0, static_cast<int>(object.states.size()) - 1);
        }
        std::vector<pf::FunctionCall>& calls = editor.selectedObjectCallbackEvent
            ? objectEventCallbacks(object, eventSlot)
            : objectStateCallbacks(object.states[static_cast<size_t>(editor.selectedObjectState)], stateSlot);
        const std::string slotLabel = editor.selectedObjectCallbackEvent
            ? std::string("event ") + objectEventCallbackLabel(eventSlot)
            : std::string(objectStateCallbackLabel(stateSlot)) + " on " + object.states[static_cast<size_t>(editor.selectedObjectState)].name;
        DrawText(clippedText(object.name + "  " + slotLabel, 11, rect.width - 20.0f).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(rect.y + 62.0f),
            11,
            RAYWHITE);
        DrawText(("Callbacks " + std::to_string(calls.size())).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(rect.y + 84.0f),
            11,
            Fade(RAYWHITE, 0.7f));
        auto commitObjectCallbacks = [&](std::vector<pf::FunctionCall> edited, const std::string& message) -> bool {
            std::string error;
            if (editor.selectedObjectCallbackEvent) {
                const auto slot = static_cast<pf::FighterEditorObjectEventCallbackSlot>(eventSlot);
                if (pf::setEditorSessionObjectEventCallbacks(session, editor.selectedObjectDef, slot, edited, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                    return true;
                }
            } else {
                const auto slot = static_cast<pf::FighterEditorObjectStateCallbackSlot>(stateSlot);
                if (pf::setEditorSessionObjectStateCallbacks(session, editor.selectedObjectDef, editor.selectedObjectState, slot, edited, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                    return true;
                }
            }
            editor.status = "Editor: object callback edit failed: " + error;
            return false;
        };
        const float buttonY = rect.y + 108.0f;
        if (uiButton({rect.x + 10.0f, buttonY, 54.0f, 22.0f}, "Prev")) {
            if (!calls.empty()) {
                editor.selectedObjectCallback = wrappedIndex(editor.selectedObjectCallback - 1, static_cast<int>(calls.size()));
            }
            return;
        }
        if (uiButton({rect.x + 70.0f, buttonY, 54.0f, 22.0f}, "Next")) {
            if (!calls.empty()) {
                editor.selectedObjectCallback = wrappedIndex(editor.selectedObjectCallback + 1, static_cast<int>(calls.size()));
            }
            return;
        }
        if (uiButton({rect.x + 130.0f, buttonY, 58.0f, 22.0f}, editor.selectedObjectCallbackEvent ? "State" : "Event")) {
            editor.selectedObjectCallbackEvent = !editor.selectedObjectCallbackEvent;
            editor.selectedObjectCallback = 0;
            return;
        }
        if (uiButton({rect.x + 194.0f, buttonY, 58.0f, 22.0f}, "Slot<")) {
            if (editor.selectedObjectCallbackEvent) {
                editor.selectedObjectEventCallback = wrappedIndex(eventSlot - 1, 21);
            } else {
                editor.selectedObjectStateCallback = wrappedIndex(stateSlot - 1, 4);
            }
            editor.selectedObjectCallback = 0;
            return;
        }
        if (uiButton({rect.x + 258.0f, buttonY, 58.0f, 22.0f}, "Slot>")) {
            if (editor.selectedObjectCallbackEvent) {
                editor.selectedObjectEventCallback = wrappedIndex(eventSlot + 1, 21);
            } else {
                editor.selectedObjectStateCallback = wrappedIndex(stateSlot + 1, 4);
            }
            editor.selectedObjectCallback = 0;
            return;
        }
        if (calls.empty()) {
            DrawText("No callbacks in this slot", static_cast<int>(rect.x + 10.0f), static_cast<int>(buttonY + 34.0f), 11, Fade(RAYWHITE, 0.62f));
            if (uiButton({rect.x + 10.0f, buttonY + 58.0f, 86.0f, 22.0f}, "UseScript") &&
                editor.selectedPackageScript >= 0 &&
                editor.selectedPackageScript < static_cast<int>(object.packageScripts.size()))
            {
                std::vector<pf::FunctionCall> edited;
                edited.push_back({"script:" + object.packageScripts[static_cast<size_t>(editor.selectedPackageScript)].name});
                if (commitObjectCallbacks(edited, "Editor: assigned object callback script")) return;
            }
            return;
        }
        editor.selectedObjectCallback = std::clamp(editor.selectedObjectCallback, 0, static_cast<int>(calls.size()) - 1);
        const pf::FunctionCall& call = calls[static_cast<size_t>(editor.selectedObjectCallback)];
        DrawText(clippedText("#" + std::to_string(editor.selectedObjectCallback) + "  " + call.name, 12, rect.width - 20.0f).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(buttonY + 34.0f),
            12,
            Fade(RAYWHITE, 0.78f));
        const float editY = buttonY + 60.0f;
        if (uiButton({rect.x + 10.0f, editY, 76.0f, 22.0f}, "UseScript") &&
            editor.selectedPackageScript >= 0 &&
            editor.selectedPackageScript < static_cast<int>(object.packageScripts.size()))
        {
            std::vector<pf::FunctionCall> edited = calls;
            edited[static_cast<size_t>(editor.selectedObjectCallback)].name =
                "script:" + object.packageScripts[static_cast<size_t>(editor.selectedPackageScript)].name;
            if (commitObjectCallbacks(edited, "Editor: retargeted object callback")) return;
        }
        if (uiButton({rect.x + 92.0f, editY, 68.0f, 22.0f}, "Remove")) {
            std::vector<pf::FunctionCall> edited = calls;
            edited.erase(edited.begin() + editor.selectedObjectCallback);
            editor.selectedObjectCallback = std::clamp(editor.selectedObjectCallback, 0, std::max(0, static_cast<int>(edited.size()) - 1));
            if (commitObjectCallbacks(edited, "Editor: removed object callback")) return;
        }
        if (!object.packageScripts.empty()) {
            const int scriptIndex = std::clamp(editor.selectedPackageScript, 0, static_cast<int>(object.packageScripts.size()) - 1);
            DrawText(clippedText("Selected object script: " + object.packageScripts[static_cast<size_t>(scriptIndex)].name, 10, rect.width - 20.0f).c_str(),
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(editY + 32.0f),
                10,
                Fade(RAYWHITE, 0.64f));
        }
        return;
    }
    if (editor.workspace == pf::EditorWorkspace::Assets || editor.selectionKind == pf::FighterEditorSelectionKind::Object) {
        DrawText("Object / Article", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 34.0f), 12, Fade(RAYWHITE, 0.7f));
        if (session.package.objects.empty()) {
            DrawText("No object definitions in this package", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 62.0f), 12, Fade(RAYWHITE, 0.62f));
            if (uiButton({rect.x + 10.0f, rect.y + 86.0f, 94.0f, 24.0f}, "+Projectile")) {
                std::string error;
                int added = -1;
                if (pf::addEditorSessionObject(session, pf::uniqueEditorObjectName(session.package, "Projectile"), pf::GameObjectKind::Projectile, &added, &error)) {
                    editor.selectedObjectDef = added;
                    editor.selectionKind = pf::FighterEditorSelectionKind::Object;
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added projectile object");
                    return;
                }
                editor.status = "Editor: add object failed: " + error;
            }
            return;
        }

        editor.selectedObjectDef = std::clamp(editor.selectedObjectDef, 0, static_cast<int>(session.package.objects.size()) - 1);
        pf::GameObjectDefinition& object = session.package.objects[static_cast<size_t>(editor.selectedObjectDef)];
        std::string renamedObject;
        if (uiTextField({rect.x + 10.0f, rect.y + 58.0f, rect.width - 20.0f, 24.0f},
                "workstation-object-name",
                editor,
                object.name,
                renamedObject,
                48))
        {
            std::string error;
            if (pf::renameEditorSessionObject(session, editor.selectedObjectDef, renamedObject, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: renamed object/article");
                return;
            }
            editor.status = "Editor: rename object failed: " + error;
        }

        DrawText(("Kind " + std::string(gameObjectKindName(object.kind)) +
                  "  life=" + std::to_string(object.lifetimeFrames) +
                  "  g=" + std::to_string(pf::fxToFloat(object.gravity)) +
                  "  maxDmg=" + std::to_string(pf::fxToFloat(object.maxDamage))).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(rect.y + 90.0f),
            11,
            RAYWHITE);
        auto commitObjectProperties = [&](const pf::GameObjectDefinition& edited, const std::string& message) -> bool {
            std::string error;
            if (pf::setEditorSessionObjectProperties(
                    session,
                    editor.selectedObjectDef,
                    std::max(1, edited.lifetimeFrames),
                    edited.gravity,
                    edited.terminalVelocity,
                    edited.maxDamage,
                    edited.destroyOnHit,
                    edited.destroyOnShield,
                    edited.hitOwner,
                    &error))
            {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                return true;
            }
            editor.status = "Editor: object property edit failed: " + error;
            return false;
        };
        pf::GameObjectDefinition editedObject = object;
        const float propY = rect.y + 112.0f;
        if (uiButton({rect.x + 10.0f, propY, 58.0f, 22.0f}, "Kind")) {
            std::string error;
            if (pf::setEditorSessionObjectKind(session, editor.selectedObjectDef, nextGameObjectKind(object.kind), &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: changed object kind");
                return;
            }
            editor.status = "Editor: object kind edit failed: " + error;
        }
        if (uiButton({rect.x + 74.0f, propY, 50.0f, 22.0f}, "Life-")) {
            editedObject.lifetimeFrames = std::max(1, editedObject.lifetimeFrames - 15);
            if (commitObjectProperties(editedObject, "Editor: shortened object lifetime")) return;
        }
        if (uiButton({rect.x + 130.0f, propY, 50.0f, 22.0f}, "Life+")) {
            editedObject.lifetimeFrames += 15;
            if (commitObjectProperties(editedObject, "Editor: lengthened object lifetime")) return;
        }
        if (uiButton({rect.x + 186.0f, propY, 42.0f, 22.0f}, "G-")) {
            editedObject.gravity -= pf::fxFromFloat(0.01f);
            if (commitObjectProperties(editedObject, "Editor: lowered object gravity")) return;
        }
        if (uiButton({rect.x + 234.0f, propY, 42.0f, 22.0f}, "G+")) {
            editedObject.gravity += pf::fxFromFloat(0.01f);
            if (commitObjectProperties(editedObject, "Editor: raised object gravity")) return;
        }
        if (uiButton({rect.x + 282.0f, propY, 58.0f, 22.0f}, "Owner", object.hitOwner)) {
            editedObject.hitOwner = !editedObject.hitOwner;
            if (commitObjectProperties(editedObject, "Editor: toggled object owner hit flag")) return;
        }
        const float flagY = propY + 28.0f;
        if (uiButton({rect.x + 10.0f, flagY, 76.0f, 22.0f}, "HitKill", object.destroyOnHit)) {
            editedObject.destroyOnHit = !editedObject.destroyOnHit;
            if (commitObjectProperties(editedObject, "Editor: toggled destroy on hit")) return;
        }
        if (uiButton({rect.x + 92.0f, flagY, 82.0f, 22.0f}, "ShieldKill", object.destroyOnShield)) {
            editedObject.destroyOnShield = !editedObject.destroyOnShield;
            if (commitObjectProperties(editedObject, "Editor: toggled destroy on shield")) return;
        }
        if (uiButton({rect.x + 180.0f, flagY, 62.0f, 22.0f}, "Dmg-")) {
            editedObject.maxDamage = std::max(pf::Fix{0}, editedObject.maxDamage - pf::fx(1));
            if (commitObjectProperties(editedObject, "Editor: lowered object max damage")) return;
        }
        if (uiButton({rect.x + 248.0f, flagY, 62.0f, 22.0f}, "Dmg+")) {
            editedObject.maxDamage += pf::fx(1);
            if (commitObjectProperties(editedObject, "Editor: raised object max damage")) return;
        }

        const float stateY = flagY + 36.0f;
        if (!object.states.empty()) {
            editor.selectedObjectState = std::clamp(editor.selectedObjectState, 0, static_cast<int>(object.states.size()) - 1);
            const pf::GameObjectStateDefinition& objectState = object.states[static_cast<size_t>(editor.selectedObjectState)];
            DrawText(("State #" + std::to_string(editor.selectedObjectState) + " " + objectState.name +
                      " len=" + std::to_string(objectState.animationLengthFrames) +
                      (editor.selectedObjectState == object.initialState ? " initial" : "")).c_str(),
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(stateY),
                11,
                RAYWHITE);
            std::string renamedObjectState;
            if (uiTextField({rect.x + 10.0f, stateY + 18.0f, rect.width - 20.0f, 22.0f},
                    "workstation-object-state-name",
                    editor,
                    objectState.name,
                    renamedObjectState,
                    48))
            {
                std::string error;
                if (pf::renameEditorSessionObjectState(session, editor.selectedObjectDef, editor.selectedObjectState, renamedObjectState, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: renamed object state");
                    return;
                }
                editor.status = "Editor: rename object state failed: " + error;
            }
            const float objectStateButtonY = stateY + 48.0f;
            if (uiButton({rect.x + 10.0f, objectStateButtonY, 50.0f, 22.0f}, "Prev")) {
                editor.selectedObjectState = wrappedIndex(editor.selectedObjectState - 1, static_cast<int>(object.states.size()));
                editor.selectionKind = pf::FighterEditorSelectionKind::Object;
                return;
            }
            if (uiButton({rect.x + 66.0f, objectStateButtonY, 50.0f, 22.0f}, "Next")) {
                editor.selectedObjectState = wrappedIndex(editor.selectedObjectState + 1, static_cast<int>(object.states.size()));
                editor.selectionKind = pf::FighterEditorSelectionKind::Object;
                return;
            }
            if (uiButton({rect.x + 122.0f, objectStateButtonY, 48.0f, 22.0f}, "Len-")) {
                std::string error;
                if (pf::setEditorSessionObjectStateTiming(session, editor.selectedObjectDef, editor.selectedObjectState, std::max(1, objectState.animationLengthFrames - 1), objectState.loopAnimation, false, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: shortened object state");
                    return;
                }
                editor.status = "Editor: object state timing failed: " + error;
            }
            if (uiButton({rect.x + 176.0f, objectStateButtonY, 48.0f, 22.0f}, "Len+")) {
                std::string error;
                if (pf::setEditorSessionObjectStateTiming(session, editor.selectedObjectDef, editor.selectedObjectState, objectState.animationLengthFrames + 1, objectState.loopAnimation, false, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: lengthened object state");
                    return;
                }
                editor.status = "Editor: object state timing failed: " + error;
            }
            if (uiButton({rect.x + 230.0f, objectStateButtonY, 54.0f, 22.0f}, "Loop", objectState.loopAnimation)) {
                std::string error;
                if (pf::setEditorSessionObjectStateTiming(session, editor.selectedObjectDef, editor.selectedObjectState, objectState.animationLengthFrames, !objectState.loopAnimation, false, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: toggled object state loop");
                    return;
                }
                editor.status = "Editor: object state timing failed: " + error;
            }
            if (uiButton({rect.x + 290.0f, objectStateButtonY, 58.0f, 22.0f}, "Initial")) {
                std::string error;
                if (pf::setEditorSessionObjectStateTiming(session, editor.selectedObjectDef, editor.selectedObjectState, objectState.animationLengthFrames, objectState.loopAnimation, true, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: set object initial state");
                    return;
                }
                editor.status = "Editor: object initial state failed: " + error;
            }
        }

        const float objectVarY = stateY + 82.0f;
        DrawText(("Object vars " + std::to_string(object.packageVariables.size()) +
                  "  scripts " + std::to_string(object.packageScripts.size()) +
                  "  boxes H" + std::to_string(object.hitboxes.size()) +
                  " Hu" + std::to_string(object.hurtboxes.size()) +
                  " T" + std::to_string(object.touchboxes.size())).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(objectVarY),
            10,
            Fade(RAYWHITE, 0.68f));
        if (uiButton({rect.x + 10.0f, objectVarY + 18.0f, 54.0f, 22.0f}, "+Var")) {
            std::string error;
            int added = -1;
            if (pf::addEditorSessionObjectPackageVariable(session, editor.selectedObjectDef, {}, 0, &added, &error)) {
                editor.selectedPackageVariable = added;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added object variable");
                return;
            }
            editor.status = "Editor: add object variable failed: " + error;
        }
        if (!object.packageVariables.empty()) {
            editor.selectedPackageVariable = std::clamp(editor.selectedPackageVariable, 0, static_cast<int>(object.packageVariables.size()) - 1);
            const pf::PackageVariableDefinition& objectVariable = object.packageVariables[static_cast<size_t>(editor.selectedPackageVariable)];
            DrawText(clippedText("v" + std::to_string(editor.selectedPackageVariable) + " " + objectVariable.name + " init=" + std::to_string(objectVariable.initialValue), 10, rect.width - 146.0f).c_str(),
                static_cast<int>(rect.x + 72.0f),
                static_cast<int>(objectVarY + 23.0f),
                10,
                RAYWHITE);
            if (uiButton({rect.x + rect.width - 68.0f, objectVarY + 18.0f, 26.0f, 22.0f}, "-")) {
                std::string error;
                if (pf::setEditorSessionObjectPackageVariableInitialValue(session, editor.selectedObjectDef, editor.selectedPackageVariable, objectVariable.initialValue - 1, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: lowered object variable initial value");
                    return;
                }
                editor.status = "Editor: object variable edit failed: " + error;
            }
            if (uiButton({rect.x + rect.width - 36.0f, objectVarY + 18.0f, 26.0f, 22.0f}, "+")) {
                std::string error;
                if (pf::setEditorSessionObjectPackageVariableInitialValue(session, editor.selectedObjectDef, editor.selectedPackageVariable, objectVariable.initialValue + 1, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: raised object variable initial value");
                    return;
                }
                editor.status = "Editor: object variable edit failed: " + error;
            }
        }
        if (!object.packageScripts.empty()) {
            editor.selectedPackageScript = std::clamp(editor.selectedPackageScript, 0, static_cast<int>(object.packageScripts.size()) - 1);
            pf::PackageScript& script = object.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
            const float scriptInspectY = objectVarY + 54.0f;
            std::string renamedScript;
            DrawText("Object Script", static_cast<int>(rect.x + 10.0f), static_cast<int>(scriptInspectY), 11, Fade(RAYWHITE, 0.7f));
            if (uiTextField({rect.x + 90.0f, scriptInspectY - 5.0f, rect.width - 100.0f, 22.0f},
                    "workstation-object-script-name",
                    editor,
                    script.name,
                    renamedScript,
                    64))
            {
                std::string error;
                const std::string oldName = script.name;
                if (pf::renameEditorSessionObjectPackageScript(session, editor.selectedObjectDef, editor.selectedPackageScript, renamedScript, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: renamed object script " + oldName + " to " + renamedScript);
                    return;
                }
                editor.status = "Editor: rename object script failed: " + error;
            }
            DrawText(("Instructions " + std::to_string(script.instructions.size()) +
                      "  Graph nodes " + std::to_string(script.graph.nodes.size()) +
                      "  Links " + std::to_string(script.graph.links.size())).c_str(),
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(scriptInspectY + 26.0f),
                10,
                Fade(RAYWHITE, 0.66f));
            if (uiButton({rect.x + rect.width - 146.0f, scriptInspectY + 20.0f, 68.0f, 22.0f}, "Budget-")) {
                std::string error;
                if (pf::setEditorSessionObjectPackageScriptBudget(session, editor.selectedObjectDef, editor.selectedPackageScript, std::max(1, script.instructionBudget - 8), &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: lowered object script instruction budget");
                    return;
                }
                editor.status = "Editor: object script budget failed: " + error;
            }
            if (uiButton({rect.x + rect.width - 72.0f, scriptInspectY + 20.0f, 62.0f, 22.0f}, "Budget+")) {
                std::string error;
                if (pf::setEditorSessionObjectPackageScriptBudget(session, editor.selectedObjectDef, editor.selectedPackageScript, script.instructionBudget + 8, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: raised object script instruction budget");
                    return;
                }
                editor.status = "Editor: object script budget failed: " + error;
            }
            auto selectedGraphNodeIt = std::find_if(script.graph.nodes.begin(), script.graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) {
                return node.id == editor.selectedPackageGraphNode;
            });
            if (selectedGraphNodeIt == script.graph.nodes.end() && !script.graph.nodes.empty()) {
                editor.selectedPackageGraphNode = script.graph.entryNode >= 0 ? script.graph.entryNode : script.graph.nodes.front().id;
                selectedGraphNodeIt = std::find_if(script.graph.nodes.begin(), script.graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) {
                    return node.id == editor.selectedPackageGraphNode;
                });
            }
            const float graphNodeY = scriptInspectY + 54.0f;
            if (selectedGraphNodeIt != script.graph.nodes.end()) {
                const pf::PackageScriptGraphNode& graphNode = *selectedGraphNodeIt;
                const std::string graphNodeLabel = "Graph node #" + std::to_string(graphNode.id) + " " +
                    packageScriptGraphNodeKindName(graphNode.kind) +
                    (graphNode.kind == pf::PackageScriptGraphNodeKind::Instruction
                        ? " inst=" + std::to_string(graphNode.instructionIndex)
                        : "");
                DrawText(clippedText(graphNodeLabel, 10, rect.width - 20.0f).c_str(),
                    static_cast<int>(rect.x + 10.0f),
                    static_cast<int>(graphNodeY),
                    10,
                    RAYWHITE);
                DrawText(("pos=(" + std::to_string(pf::fxToFloat(graphNode.position.x)) +
                          ", " + std::to_string(pf::fxToFloat(graphNode.position.y)) + ")").c_str(),
                    static_cast<int>(rect.x + 10.0f),
                    static_cast<int>(graphNodeY + 16.0f),
                    10,
                    Fade(RAYWHITE, 0.62f));
                auto commitObjectGraphNode = [&](pf::PackageScriptGraphNode edited, const std::string& message) -> bool {
                    std::string error;
                    if (pf::setEditorSessionObjectPackageScriptGraphNode(session, editor.selectedObjectDef, editor.selectedPackageScript, edited.id, edited, &error)) {
                        syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                        return true;
                    }
                    editor.status = "Editor: object graph node edit failed: " + error;
                    return false;
                };
                pf::PackageScriptGraphNode editedGraphNode = graphNode;
                const float graphNodeButtonY = graphNodeY + 34.0f;
                if (uiButton({rect.x + 10.0f, graphNodeButtonY, 36.0f, 22.0f}, "X-")) {
                    editedGraphNode.position.x -= pf::fx(16);
                    if (commitObjectGraphNode(editedGraphNode, "Editor: moved object graph node left")) return;
                }
                if (uiButton({rect.x + 52.0f, graphNodeButtonY, 36.0f, 22.0f}, "X+")) {
                    editedGraphNode.position.x += pf::fx(16);
                    if (commitObjectGraphNode(editedGraphNode, "Editor: moved object graph node right")) return;
                }
                if (uiButton({rect.x + 94.0f, graphNodeButtonY, 36.0f, 22.0f}, "Y-")) {
                    editedGraphNode.position.y -= pf::fx(16);
                    if (commitObjectGraphNode(editedGraphNode, "Editor: moved object graph node up")) return;
                }
                if (uiButton({rect.x + 136.0f, graphNodeButtonY, 36.0f, 22.0f}, "Y+")) {
                    editedGraphNode.position.y += pf::fx(16);
                    if (commitObjectGraphNode(editedGraphNode, "Editor: moved object graph node down")) return;
                }
                if (graphNode.kind == pf::PackageScriptGraphNodeKind::Instruction &&
                    graphNode.instructionIndex >= 0 &&
                    graphNode.instructionIndex < static_cast<int>(script.instructions.size()) &&
                    uiButton({rect.x + 184.0f, graphNodeButtonY, 62.0f, 22.0f}, "UseInst"))
                {
                    editor.selectedPackageInstruction = graphNode.instructionIndex;
                    session.selectedPackageInstruction = graphNode.instructionIndex;
                    editor.selectionKind = pf::FighterEditorSelectionKind::Instruction;
                    return;
                }
            }
        }
        return;
    }
    if (editor.workspace == pf::EditorWorkspace::Animation || editor.selectionKind == pf::FighterEditorSelectionKind::Animation) {
        DrawText("Animation", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 34.0f), 12, Fade(RAYWHITE, 0.7f));
        if (def.authoredClips.empty()) {
            DrawText("No authored clips", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 62.0f), 12, Fade(RAYWHITE, 0.62f));
            return;
        }
        editor.selectedAnimationClip = std::clamp(editor.selectedAnimationClip, 0, static_cast<int>(def.authoredClips.size()) - 1);
        const pf::AnimationClip& clip = def.authoredClips[static_cast<size_t>(editor.selectedAnimationClip)];
        std::string renamedClip;
        if (uiTextField({rect.x + 10.0f, rect.y + 58.0f, rect.width - 20.0f, 24.0f},
                "workstation-animation-clip-name",
                editor,
                clip.name,
                renamedClip,
                48))
        {
            std::string error;
            if (pf::renameEditorSessionAuthoredClip(session, editor.selectedAnimationClip, renamedClip, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: renamed authored clip");
                return;
            }
            editor.status = "Editor: rename clip failed: " + error;
        }
        const int clipFrames = std::max(1, static_cast<int>(pf::fxToFloat(clip.frameCount)));
        DrawText(("action=" + std::to_string(clip.actionIndex) +
                  " frames=" + std::to_string(clipFrames) +
                  " blend=" + std::to_string(clip.defaultBlendFrames) +
                  " flags=" + std::to_string(clip.actionFlags)).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(rect.y + 90.0f),
            11,
            RAYWHITE);
        auto commitClip = [&](int actionIndex, int frameCount, int blendFrames, uint32_t actionFlags, const std::string& message) -> bool {
            std::string error;
            if (pf::setEditorSessionAuthoredClipProperties(
                    session,
                    editor.selectedAnimationClip,
                    actionIndex,
                    pf::fx(std::max(1, frameCount)),
                    std::max(0, blendFrames),
                    actionFlags,
                    &error))
            {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                return true;
            }
            editor.status = "Editor: clip property edit failed: " + error;
            return false;
        };
        const float clipY = rect.y + 112.0f;
        if (uiButton({rect.x + 10.0f, clipY, 54.0f, 22.0f}, "Len-")) {
            if (commitClip(clip.actionIndex, clipFrames - 1, clip.defaultBlendFrames, clip.actionFlags, "Editor: shortened authored clip")) return;
        }
        if (uiButton({rect.x + 70.0f, clipY, 54.0f, 22.0f}, "Len+")) {
            if (commitClip(clip.actionIndex, clipFrames + 1, clip.defaultBlendFrames, clip.actionFlags, "Editor: lengthened authored clip")) return;
        }
        if (uiButton({rect.x + 130.0f, clipY, 54.0f, 22.0f}, "Act-")) {
            if (commitClip(std::max(0, clip.actionIndex - 1), clipFrames, clip.defaultBlendFrames, clip.actionFlags, "Editor: lowered authored clip action index")) return;
        }
        if (uiButton({rect.x + 190.0f, clipY, 54.0f, 22.0f}, "Act+")) {
            if (commitClip(clip.actionIndex + 1, clipFrames, clip.defaultBlendFrames, clip.actionFlags, "Editor: raised authored clip action index")) return;
        }
        if (uiButton({rect.x + 250.0f, clipY, 62.0f, 22.0f}, "Blend-")) {
            if (commitClip(clip.actionIndex, clipFrames, clip.defaultBlendFrames - 1, clip.actionFlags, "Editor: lowered authored clip blend")) return;
        }
        if (uiButton({rect.x + 318.0f, clipY, 62.0f, 22.0f}, "Blend+")) {
            if (commitClip(clip.actionIndex, clipFrames, clip.defaultBlendFrames + 1, clip.actionFlags, "Editor: raised authored clip blend")) return;
        }
        if (uiButton({rect.x + 10.0f, clipY + 30.0f, 86.0f, 22.0f}, "UseOnState")) {
            std::string error;
            if (pf::setEditorSessionStateAnimation(session, session.selectedState, clip.name, clip.actionIndex, clipFrames, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: assigned authored clip to selected state");
                return;
            }
            editor.status = "Editor: assign clip failed: " + error;
        }

        const float jointY = clipY + 66.0f;
        DrawText(("Joints " + std::to_string(def.authoredSkeleton.size()) +
                  "  selected " + std::to_string(editor.selectedAnimationJoint)).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(jointY),
            11,
            Fade(RAYWHITE, 0.7f));
        if (uiButton({rect.x + 10.0f, jointY + 20.0f, 64.0f, 22.0f}, "+Joint")) {
            pf::AnimationJoint joint;
            joint.parent = def.authoredSkeleton.empty() ? -1 : std::clamp(editor.selectedAnimationJoint, 0, static_cast<int>(def.authoredSkeleton.size()) - 1);
            joint.name = pf::uniqueEditorAuthoredJointName(def);
            joint.scale = {pf::fx(1), pf::fx(1), pf::fx(1)};
            std::string error;
            int added = -1;
            if (pf::addEditorSessionAuthoredJoint(session, joint, -1, &added, &error)) {
                editor.selectedAnimationJoint = added;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added authored joint");
                return;
            }
            editor.status = "Editor: add joint failed: " + error;
        }
        if (!def.authoredSkeleton.empty()) {
            editor.selectedAnimationJoint = std::clamp(editor.selectedAnimationJoint, 0, static_cast<int>(def.authoredSkeleton.size()) - 1);
            const pf::AnimationJoint& joint = def.authoredSkeleton[static_cast<size_t>(editor.selectedAnimationJoint)];
            std::string renamedJoint;
            if (uiTextField({rect.x + 82.0f, jointY + 20.0f, rect.width - 92.0f, 22.0f},
                    "workstation-animation-joint-name",
                    editor,
                    joint.name,
                    renamedJoint,
                    48))
            {
                pf::AnimationJoint edited = joint;
                edited.name = renamedJoint;
                std::string error;
                if (pf::setEditorSessionAuthoredJoint(session, editor.selectedAnimationJoint, edited, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: renamed authored joint");
                    return;
                }
                editor.status = "Editor: joint rename failed: " + error;
            }
            const float jointButtonY = jointY + 50.0f;
            if (uiButton({rect.x + 10.0f, jointButtonY, 46.0f, 22.0f}, "Jnt<")) {
                editor.selectedAnimationJoint = wrappedIndex(editor.selectedAnimationJoint - 1, static_cast<int>(def.authoredSkeleton.size()));
                return;
            }
            if (uiButton({rect.x + 62.0f, jointButtonY, 46.0f, 22.0f}, "Jnt>")) {
                editor.selectedAnimationJoint = wrappedIndex(editor.selectedAnimationJoint + 1, static_cast<int>(def.authoredSkeleton.size()));
                return;
            }
            auto commitJoint = [&](pf::AnimationJoint edited, const std::string& message) -> bool {
                std::string error;
                if (pf::setEditorSessionAuthoredJoint(session, editor.selectedAnimationJoint, edited, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                    return true;
                }
                editor.status = "Editor: joint edit failed: " + error;
                return false;
            };
            pf::AnimationJoint editedJoint = joint;
            if (uiButton({rect.x + 114.0f, jointButtonY, 36.0f, 22.0f}, "X-")) {
                editedJoint.translation.x -= pf::fxFromFloat(0.05f);
                if (commitJoint(editedJoint, "Editor: moved joint left")) return;
            }
            if (uiButton({rect.x + 156.0f, jointButtonY, 36.0f, 22.0f}, "X+")) {
                editedJoint.translation.x += pf::fxFromFloat(0.05f);
                if (commitJoint(editedJoint, "Editor: moved joint right")) return;
            }
            if (uiButton({rect.x + 198.0f, jointButtonY, 36.0f, 22.0f}, "Y-")) {
                editedJoint.translation.y -= pf::fxFromFloat(0.05f);
                if (commitJoint(editedJoint, "Editor: lowered joint")) return;
            }
            if (uiButton({rect.x + 240.0f, jointButtonY, 36.0f, 22.0f}, "Y+")) {
                editedJoint.translation.y += pf::fxFromFloat(0.05f);
                if (commitJoint(editedJoint, "Editor: raised joint")) return;
            }
            if (uiButton({rect.x + 282.0f, jointButtonY, 58.0f, 22.0f}, "-Joint")) {
                std::string error;
                if (pf::removeEditorSessionAuthoredJoint(session, editor.selectedAnimationJoint, &error)) {
                    editor.selectedAnimationJoint = 0;
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed authored joint");
                    return;
                }
                editor.status = "Editor: remove joint failed: " + error;
            }
            DrawText(("pos=(" + std::to_string(pf::fxToFloat(joint.translation.x)) + ", " +
                      std::to_string(pf::fxToFloat(joint.translation.y)) + ") parent=" +
                      std::to_string(joint.parent)).c_str(),
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(jointButtonY + 30.0f),
                10,
                Fade(RAYWHITE, 0.66f));
        }
        return;
    }
    if (editor.selectionKind == pf::FighterEditorSelectionKind::Viewport) {
        DrawText("Viewport Volumes", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 34.0f), 12, Fade(RAYWHITE, 0.7f));
        DrawText((std::string("Overlays ") +
                  (editor.showModel ? "model " : "") +
                  (editor.showHitboxes ? "hit " : "") +
                  (editor.showHurtboxes ? "hurt " : "") +
                  (editor.showEcb ? "ecb " : "") +
                  (editor.showSkeleton ? "bones" : "")).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(rect.y + 62.0f),
            11,
            Fade(RAYWHITE, 0.7f));
        const float ecbY = rect.y + 86.0f;
        DrawText(("Authored ECB: " + std::string(def.authoredEcb.enabled ? "enabled" : "disabled")).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(ecbY),
            11,
            RAYWHITE);
        auto commitEcb = [&](pf::FighterEcbDefinition edited, const std::string& message) -> bool {
            std::string error;
            if (pf::setEditorSessionAuthoredEcb(session, edited, true, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                return true;
            }
            editor.status = "Editor: authored ECB edit failed: " + error;
            return false;
        };
        pf::FighterEcbDefinition editedEcb = def.authoredEcb;
        if (uiButton({rect.x + 10.0f, ecbY + 20.0f, 52.0f, 22.0f}, "ECB", def.authoredEcb.enabled)) {
            editedEcb.enabled = !editedEcb.enabled;
            if (commitEcb(editedEcb, editedEcb.enabled ? "Editor: enabled authored ECB" : "Editor: disabled authored ECB")) return;
        }
        if (def.authoredEcb.enabled) {
            DrawText(("w=" + std::to_string(pf::fxToFloat(def.authoredEcb.points[2].x) * 2.0f) +
                      " top=" + std::to_string(pf::fxToFloat(def.authoredEcb.points[1].y)) +
                      " side=" + std::to_string(pf::fxToFloat(def.authoredEcb.points[0].y)) +
                      " bot=" + std::to_string(pf::fxToFloat(def.authoredEcb.points[3].y))).c_str(),
                static_cast<int>(rect.x + 70.0f),
                static_cast<int>(ecbY + 25.0f),
                10,
                Fade(RAYWHITE, 0.68f));
            const float ecbButtonY = ecbY + 50.0f;
            if (uiButton({rect.x + 10.0f, ecbButtonY, 46.0f, 22.0f}, "W+")) {
                editedEcb.points[0].x -= pf::fxFromFloat(0.05f);
                editedEcb.points[2].x += pf::fxFromFloat(0.05f);
                if (commitEcb(editedEcb, "Editor: widened authored ECB")) return;
            }
            if (uiButton({rect.x + 62.0f, ecbButtonY, 46.0f, 22.0f}, "W-")) {
                editedEcb.points[0].x = std::min(editedEcb.points[0].x + pf::fxFromFloat(0.05f), -pf::fxFromFloat(0.1f));
                editedEcb.points[2].x = std::max(editedEcb.points[2].x - pf::fxFromFloat(0.05f), pf::fxFromFloat(0.1f));
                if (commitEcb(editedEcb, "Editor: narrowed authored ECB")) return;
            }
            if (uiButton({rect.x + 114.0f, ecbButtonY, 54.0f, 22.0f}, "Top+")) {
                editedEcb.points[1].y += pf::fxFromFloat(0.1f);
                if (commitEcb(editedEcb, "Editor: raised authored ECB top")) return;
            }
            if (uiButton({rect.x + 174.0f, ecbButtonY, 54.0f, 22.0f}, "Top-")) {
                editedEcb.points[1].y = std::max(editedEcb.points[3].y + pf::fxFromFloat(0.5f), editedEcb.points[1].y - pf::fxFromFloat(0.1f));
                if (commitEcb(editedEcb, "Editor: lowered authored ECB top")) return;
            }
            if (uiButton({rect.x + 234.0f, ecbButtonY, 54.0f, 22.0f}, "Bot+")) {
                editedEcb.points[3].y += pf::fxFromFloat(0.05f);
                if (commitEcb(editedEcb, "Editor: raised authored ECB bottom")) return;
            }
            if (uiButton({rect.x + 294.0f, ecbButtonY, 54.0f, 22.0f}, "Bot-")) {
                editedEcb.points[3].y -= pf::fxFromFloat(0.05f);
                if (commitEcb(editedEcb, "Editor: lowered authored ECB bottom")) return;
            }
            if (uiButton({rect.x + 10.0f, ecbButtonY + 28.0f, 58.0f, 22.0f}, "Side+")) {
                editedEcb.points[0].y += pf::fxFromFloat(0.05f);
                editedEcb.points[2].y += pf::fxFromFloat(0.05f);
                if (commitEcb(editedEcb, "Editor: raised authored ECB side points")) return;
            }
            if (uiButton({rect.x + 74.0f, ecbButtonY + 28.0f, 58.0f, 22.0f}, "Side-")) {
                editedEcb.points[0].y -= pf::fxFromFloat(0.05f);
                editedEcb.points[2].y -= pf::fxFromFloat(0.05f);
                if (commitEcb(editedEcb, "Editor: lowered authored ECB side points")) return;
            }
        }

        const float hurtY = ecbY + 116.0f;
        DrawText(("Fighter hurtboxes: " + std::to_string(def.hurtboxes.size())).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(hurtY),
            11,
            RAYWHITE);
        if (uiButton({rect.x + 140.0f, hurtY - 4.0f, 62.0f, 22.0f}, "+Hurt")) {
            pf::HurtboxDefinition hurtbox;
            hurtbox.bone = pf::BoneId::Hip;
            hurtbox.startOffset = {0, pf::fxFromFloat(-0.35f), 0};
            hurtbox.endOffset = {0, pf::fxFromFloat(0.45f), 0};
            hurtbox.radius = pf::fxFromFloat(0.35f);
            std::string error;
            int added = -1;
            if (pf::addEditorSessionHurtbox(session, hurtbox, -1, &added, &error)) {
                editor.selectedHurtbox = added;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added fighter hurtbox");
                return;
            }
            editor.status = "Editor: add hurtbox failed: " + error;
        }
        if (uiButton({rect.x + 208.0f, hurtY - 4.0f, 62.0f, 22.0f}, "-Hurt")) {
            std::string error;
            if (pf::removeEditorSessionHurtbox(session, editor.selectedHurtbox, &error)) {
                editor.selectedHurtbox = 0;
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed fighter hurtbox");
                return;
            }
            editor.status = "Editor: remove hurtbox failed: " + error;
        }
        if (!def.hurtboxes.empty()) {
            editor.selectedHurtbox = std::clamp(editor.selectedHurtbox, 0, static_cast<int>(def.hurtboxes.size()) - 1);
            const pf::HurtboxDefinition& hurtbox = def.hurtboxes[static_cast<size_t>(editor.selectedHurtbox)];
            DrawText(("#" + std::to_string(editor.selectedHurtbox) + " " + pf::boneName(hurtbox.bone) +
                      " " + hurtboxStateName(hurtbox.state) +
                      " r=" + std::to_string(pf::fxToFloat(hurtbox.radius)) +
                      (hurtbox.grabbable ? " grab" : " no-grab")).c_str(),
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(hurtY + 24.0f),
                11,
                Fade(RAYWHITE, 0.72f));
            auto commitHurtbox = [&](pf::HurtboxDefinition edited, const std::string& message) -> bool {
                std::string error;
                if (pf::setEditorSessionHurtbox(session, editor.selectedHurtbox, edited, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                    return true;
                }
                editor.status = "Editor: hurtbox edit failed: " + error;
                return false;
            };
            pf::HurtboxDefinition editedHurtbox = hurtbox;
            const float hurtButtonY = hurtY + 48.0f;
            if (uiButton({rect.x + 10.0f, hurtButtonY, 46.0f, 22.0f}, "Prev")) {
                editor.selectedHurtbox = wrappedIndex(editor.selectedHurtbox - 1, static_cast<int>(def.hurtboxes.size()));
                return;
            }
            if (uiButton({rect.x + 62.0f, hurtButtonY, 46.0f, 22.0f}, "Next")) {
                editor.selectedHurtbox = wrappedIndex(editor.selectedHurtbox + 1, static_cast<int>(def.hurtboxes.size()));
                return;
            }
            if (uiButton({rect.x + 114.0f, hurtButtonY, 42.0f, 22.0f}, "R-")) {
                editedHurtbox.radius = std::max(pf::fxFromFloat(0.05f), editedHurtbox.radius - pf::fxFromFloat(0.05f));
                if (commitHurtbox(editedHurtbox, "Editor: shrank fighter hurtbox")) return;
            }
            if (uiButton({rect.x + 162.0f, hurtButtonY, 42.0f, 22.0f}, "R+")) {
                editedHurtbox.radius += pf::fxFromFloat(0.05f);
                if (commitHurtbox(editedHurtbox, "Editor: enlarged fighter hurtbox")) return;
            }
            if (uiButton({rect.x + 210.0f, hurtButtonY, 52.0f, 22.0f}, "Bone")) {
                editedHurtbox.bone = nextEditorBone(editedHurtbox.bone);
                if (commitHurtbox(editedHurtbox, "Editor: changed fighter hurtbox bone")) return;
            }
            if (uiButton({rect.x + 268.0f, hurtButtonY, 54.0f, 22.0f}, "State")) {
                editedHurtbox.state = nextHurtboxState(editedHurtbox.state);
                if (commitHurtbox(editedHurtbox, "Editor: changed fighter hurtbox state")) return;
            }
            if (uiButton({rect.x + 328.0f, hurtButtonY, 54.0f, 22.0f}, "Grab", hurtbox.grabbable)) {
                editedHurtbox.grabbable = !editedHurtbox.grabbable;
                if (commitHurtbox(editedHurtbox, "Editor: toggled fighter hurtbox grabbable")) return;
            }
            const float moveY = hurtButtonY + 28.0f;
            if (uiButton({rect.x + 10.0f, moveY, 42.0f, 22.0f}, "X-")) {
                editedHurtbox.startOffset.x -= pf::fxFromFloat(0.1f);
                editedHurtbox.endOffset.x -= pf::fxFromFloat(0.1f);
                if (commitHurtbox(editedHurtbox, "Editor: moved fighter hurtbox backward")) return;
            }
            if (uiButton({rect.x + 58.0f, moveY, 42.0f, 22.0f}, "X+")) {
                editedHurtbox.startOffset.x += pf::fxFromFloat(0.1f);
                editedHurtbox.endOffset.x += pf::fxFromFloat(0.1f);
                if (commitHurtbox(editedHurtbox, "Editor: moved fighter hurtbox forward")) return;
            }
            if (uiButton({rect.x + 106.0f, moveY, 42.0f, 22.0f}, "Y-")) {
                editedHurtbox.startOffset.y -= pf::fxFromFloat(0.1f);
                editedHurtbox.endOffset.y -= pf::fxFromFloat(0.1f);
                if (commitHurtbox(editedHurtbox, "Editor: lowered fighter hurtbox")) return;
            }
            if (uiButton({rect.x + 154.0f, moveY, 42.0f, 22.0f}, "Y+")) {
                editedHurtbox.startOffset.y += pf::fxFromFloat(0.1f);
                editedHurtbox.endOffset.y += pf::fxFromFloat(0.1f);
                if (commitHurtbox(editedHurtbox, "Editor: raised fighter hurtbox")) return;
            }
            if (uiButton({rect.x + 202.0f, moveY, 54.0f, 22.0f}, "Tall+")) {
                editedHurtbox.endOffset.y += pf::fxFromFloat(0.1f);
                if (commitHurtbox(editedHurtbox, "Editor: stretched fighter hurtbox")) return;
            }
            if (uiButton({rect.x + 262.0f, moveY, 54.0f, 22.0f}, "Tall-")) {
                editedHurtbox.endOffset.y = std::max(editedHurtbox.startOffset.y + pf::fxFromFloat(0.1f), editedHurtbox.endOffset.y - pf::fxFromFloat(0.1f));
                if (commitHurtbox(editedHurtbox, "Editor: shortened fighter hurtbox")) return;
            }
        }
        return;
    }
    if (editor.selectionKind == pf::FighterEditorSelectionKind::Variable) {
        DrawText("Package Variable", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 34.0f), 12, Fade(RAYWHITE, 0.7f));
        if (def.packageVariables.empty()) {
            DrawText("No package variables", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 62.0f), 12, Fade(RAYWHITE, 0.62f));
            if (uiButton({rect.x + 10.0f, rect.y + 86.0f, 72.0f, 24.0f}, "+Var")) {
                std::string error;
                int added = -1;
                if (pf::addEditorSessionPackageVariable(session, {}, 0, &added, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added package variable");
                    editor.selectedPackageVariable = added;
                } else {
                    editor.status = "Editor: add variable failed: " + error;
                }
            }
            return;
        }

        editor.selectedPackageVariable = std::clamp(editor.selectedPackageVariable, 0, static_cast<int>(def.packageVariables.size()) - 1);
        const pf::PackageVariableDefinition& variable = def.packageVariables[static_cast<size_t>(editor.selectedPackageVariable)];
        DrawText(("#" + std::to_string(editor.selectedPackageVariable) +
                  "  initial=" + std::to_string(variable.initialValue) +
                  "  operand uses=" + std::to_string(countPackageVariableOperandUses(def, editor.selectedPackageVariable))).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(rect.y + 62.0f),
            12,
            RAYWHITE);
        std::string renamedVariable;
        if (uiTextField({rect.x + 10.0f, rect.y + 84.0f, rect.width - 20.0f, 24.0f},
                "workstation-inspector-variable-name",
                editor,
                variable.name,
                renamedVariable,
                48))
        {
            std::string error;
            if (pf::renameEditorSessionPackageVariable(session, editor.selectedPackageVariable, renamedVariable, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: renamed package variable");
                return;
            }
            editor.status = "Editor: rename variable failed: " + error;
        }
        if (uiButton({rect.x + 10.0f, rect.y + 118.0f, 62.0f, 22.0f}, "Prev")) {
            editor.selectedPackageVariable = wrappedIndex(editor.selectedPackageVariable - 1, static_cast<int>(def.packageVariables.size()));
        }
        if (uiButton({rect.x + 78.0f, rect.y + 118.0f, 62.0f, 22.0f}, "Next")) {
            editor.selectedPackageVariable = wrappedIndex(editor.selectedPackageVariable + 1, static_cast<int>(def.packageVariables.size()));
        }
        if (uiButton({rect.x + 146.0f, rect.y + 118.0f, 54.0f, 22.0f}, "Init-")) {
            std::string error;
            if (pf::setEditorSessionPackageVariableInitialValue(session, editor.selectedPackageVariable, variable.initialValue - 1, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: lowered package variable initial value");
                return;
            }
            editor.status = "Editor: variable initial edit failed: " + error;
        }
        if (uiButton({rect.x + 206.0f, rect.y + 118.0f, 54.0f, 22.0f}, "Init+")) {
            std::string error;
            if (pf::setEditorSessionPackageVariableInitialValue(session, editor.selectedPackageVariable, variable.initialValue + 1, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: raised package variable initial value");
                return;
            }
            editor.status = "Editor: variable initial edit failed: " + error;
        }
        if (uiButton({rect.x + 10.0f, rect.y + 150.0f, 62.0f, 22.0f}, "+Var")) {
            std::string error;
            int added = -1;
            if (pf::addEditorSessionPackageVariable(session, {}, 0, &added, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: added package variable");
                editor.selectedPackageVariable = added;
                return;
            }
            editor.status = "Editor: add variable failed: " + error;
        }
        if (uiButton({rect.x + 78.0f, rect.y + 150.0f, 62.0f, 22.0f}, "-Var")) {
            std::string error;
            if (pf::removeEditorSessionPackageVariable(session, editor.selectedPackageVariable, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: removed package variable");
                return;
            }
            editor.status = "Editor: remove variable failed: " + error;
        }
        DrawText("Instructions can use this as dst/src operands; interrupt rules can use it for variable conditions.",
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(rect.y + 186.0f),
            10,
            Fade(RAYWHITE, 0.62f));
        return;
    }
    if (editor.selectionKind == pf::FighterEditorSelectionKind::Callback) {
        DrawText("State Callback", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 34.0f), 12, Fade(RAYWHITE, 0.7f));
        const std::vector<pf::FunctionCall>& calls = stateCallbackCalls(state, editor.selectedStateCallbackSlot);
        DrawText(("Slot " + std::string(stateCallbackSlotName(editor.selectedStateCallbackSlot)) +
                  "  count=" + std::to_string(calls.size())).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(rect.y + 62.0f),
            12,
            RAYWHITE);
        auto commitCallbacks = [&](std::vector<pf::FunctionCall> edited, const std::string& message) -> bool {
            std::string error;
            if (pf::setEditorSessionStateCallbacks(session, session.selectedState, editor.selectedStateCallbackSlot, edited, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                return true;
            }
            editor.status = "Editor: callback edit failed: " + error;
            return false;
        };
        auto addSelectedScriptCallback = [&]() -> bool {
            if (def.packageScripts.empty()) {
                editor.status = "Editor: add a package script before assigning state callbacks";
                return false;
            }
            const int scriptIndex = std::clamp(editor.selectedPackageScript, 0, static_cast<int>(def.packageScripts.size()) - 1);
            std::vector<pf::FunctionCall> edited = editedStateCallbackCalls(state, editor.selectedStateCallbackSlot);
            edited.push_back({"script:" + def.packageScripts[static_cast<size_t>(scriptIndex)].name});
            editor.selectedStateCallback = static_cast<int>(edited.size()) - 1;
            return commitCallbacks(edited, "Editor: added state callback script");
        };
        const float callbackY = rect.y + 112.0f;
        if (uiButton({rect.x + 10.0f, callbackY, 54.0f, 22.0f}, "Slot<")) {
            editor.selectedStateCallbackSlot = wrappedStateCallbackSlot(editor.selectedStateCallbackSlot, -1);
            editor.selectedStateCallback = 0;
            return;
        }
        if (uiButton({rect.x + 70.0f, callbackY, 54.0f, 22.0f}, "Slot>")) {
            editor.selectedStateCallbackSlot = wrappedStateCallbackSlot(editor.selectedStateCallbackSlot, 1);
            editor.selectedStateCallback = 0;
            return;
        }
        if (uiButton({rect.x + 130.0f, callbackY, 72.0f, 22.0f}, "+Script")) {
            addSelectedScriptCallback();
            return;
        }
        if (!def.packageScripts.empty()) {
            const int scriptIndex = std::clamp(editor.selectedPackageScript, 0, static_cast<int>(def.packageScripts.size()) - 1);
            DrawText(clippedText("Selected script: " + def.packageScripts[static_cast<size_t>(scriptIndex)].name, 10, rect.width - 20.0f).c_str(),
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(callbackY + 32.0f),
                10,
                Fade(RAYWHITE, 0.64f));
        } else {
            DrawText("No package scripts available for this callback slot",
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(callbackY + 32.0f),
                10,
                Fade(ORANGE, 0.78f));
        }
        if (calls.empty()) {
            DrawText("No callbacks in this slot", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 88.0f), 11, Fade(RAYWHITE, 0.62f));
            return;
        }
        editor.selectedStateCallback = std::clamp(editor.selectedStateCallback, 0, static_cast<int>(calls.size()) - 1);
        const pf::FunctionCall& call = calls[static_cast<size_t>(editor.selectedStateCallback)];
        DrawText(clippedText("#" + std::to_string(editor.selectedStateCallback) + "  " + call.name, 12, rect.width - 20.0f).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(rect.y + 86.0f),
            12,
            Fade(RAYWHITE, 0.78f));
        const float editY = callbackY + 60.0f;
        if (uiButton({rect.x + 10.0f, editY, 54.0f, 22.0f}, "Prev")) {
            editor.selectedStateCallback = wrappedIndex(editor.selectedStateCallback - 1, static_cast<int>(calls.size()));
            return;
        }
        if (uiButton({rect.x + 70.0f, editY, 54.0f, 22.0f}, "Next")) {
            editor.selectedStateCallback = wrappedIndex(editor.selectedStateCallback + 1, static_cast<int>(calls.size()));
            return;
        }
        if (uiButton({rect.x + 130.0f, editY, 76.0f, 22.0f}, "UseScript") && !def.packageScripts.empty()) {
            std::vector<pf::FunctionCall> edited = editedStateCallbackCalls(state, editor.selectedStateCallbackSlot);
            edited[static_cast<size_t>(editor.selectedStateCallback)].name =
                "script:" + packageScriptTargetName(def.packageScripts, editor.selectedPackageScript);
            if (commitCallbacks(edited, "Editor: retargeted state callback")) return;
        }
        if (uiButton({rect.x + 212.0f, editY, 70.0f, 22.0f}, "Remove")) {
            std::vector<pf::FunctionCall> edited = editedStateCallbackCalls(state, editor.selectedStateCallbackSlot);
            edited.erase(edited.begin() + editor.selectedStateCallback);
            editor.selectedStateCallback = std::clamp(editor.selectedStateCallback, 0, std::max(0, static_cast<int>(edited.size()) - 1));
            if (commitCallbacks(edited, "Editor: removed state callback")) return;
        }
        return;
    }
    if (editor.workspace == pf::EditorWorkspace::Logic && !def.packageScripts.empty()) {
        editor.selectedPackageScript = std::clamp(editor.selectedPackageScript, 0, static_cast<int>(def.packageScripts.size()) - 1);
        session.selectedPackageScript = editor.selectedPackageScript;
        pf::PackageScript& script = def.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
        std::string renamedScript;
        DrawText("Script", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 34.0f), 12, Fade(RAYWHITE, 0.7f));
        if (uiTextField({rect.x + 58.0f, rect.y + 29.0f, rect.width - 68.0f, 24.0f},
                "workstation-script-name",
                editor,
                script.name,
                renamedScript,
                64))
        {
            std::string error;
            const std::string oldName = script.name;
            if (pf::renameEditorSessionPackageScript(session, editor.selectedPackageScript, renamedScript, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: renamed script " + oldName + " to " + renamedScript);
                return;
            }
            editor.status = "Editor: rename script failed: " + error;
        }
        DrawText(("Instructions " + std::to_string(script.instructions.size()) +
                  "  Graph nodes " + std::to_string(script.graph.nodes.size()) +
                  "  Links " + std::to_string(script.graph.links.size())).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(rect.y + 62.0f),
            11,
            Fade(RAYWHITE, 0.72f));
        if (uiButton({rect.x + 10.0f, rect.y + 84.0f, 72.0f, 22.0f}, "Budget-")) {
            std::string error;
            if (pf::setEditorSessionPackageScriptBudget(session, editor.selectedPackageScript, std::max(1, script.instructionBudget - 8), &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: lowered script instruction budget");
                return;
            }
            editor.status = "Editor: script budget failed: " + error;
        }
        if (uiButton({rect.x + 88.0f, rect.y + 84.0f, 72.0f, 22.0f}, "Budget+")) {
            std::string error;
            if (pf::setEditorSessionPackageScriptBudget(session, editor.selectedPackageScript, script.instructionBudget + 8, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: raised script instruction budget");
                return;
            }
            editor.status = "Editor: script budget failed: " + error;
        }
        auto selectedGraphNodeIt = std::find_if(script.graph.nodes.begin(), script.graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) {
            return node.id == editor.selectedPackageGraphNode;
        });
        if (selectedGraphNodeIt == script.graph.nodes.end() && !script.graph.nodes.empty()) {
            editor.selectedPackageGraphNode = script.graph.entryNode >= 0 ? script.graph.entryNode : script.graph.nodes.front().id;
            selectedGraphNodeIt = std::find_if(script.graph.nodes.begin(), script.graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) {
                return node.id == editor.selectedPackageGraphNode;
            });
        }
        const float graphNodeY = rect.y + 114.0f;
        if (selectedGraphNodeIt != script.graph.nodes.end()) {
            const pf::PackageScriptGraphNode& graphNode = *selectedGraphNodeIt;
            const std::string graphNodeLabel = "Graph node #" + std::to_string(graphNode.id) + " " +
                packageScriptGraphNodeKindName(graphNode.kind) +
                (graphNode.kind == pf::PackageScriptGraphNodeKind::Instruction
                    ? " inst=" + std::to_string(graphNode.instructionIndex)
                    : "");
            DrawText(clippedText(graphNodeLabel, 11, rect.width - 20.0f).c_str(),
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(graphNodeY),
                11,
                RAYWHITE);
            DrawText(("pos=(" + std::to_string(pf::fxToFloat(graphNode.position.x)) +
                      ", " + std::to_string(pf::fxToFloat(graphNode.position.y)) + ")").c_str(),
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(graphNodeY + 18.0f),
                10,
                Fade(RAYWHITE, 0.66f));
            auto commitGraphNode = [&](pf::PackageScriptGraphNode edited, const std::string& message) -> bool {
                std::string error;
                if (pf::setEditorSessionPackageScriptGraphNode(session, editor.selectedPackageScript, edited.id, edited, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                    return true;
                }
                editor.status = "Editor: graph node edit failed: " + error;
                return false;
            };
            pf::PackageScriptGraphNode editedGraphNode = graphNode;
            const float graphNodeButtonY = graphNodeY + 36.0f;
            if (uiButton({rect.x + 10.0f, graphNodeButtonY, 38.0f, 22.0f}, "X-")) {
                editedGraphNode.position.x -= pf::fx(16);
                if (commitGraphNode(editedGraphNode, "Editor: moved graph node left")) return;
            }
            if (uiButton({rect.x + 54.0f, graphNodeButtonY, 38.0f, 22.0f}, "X+")) {
                editedGraphNode.position.x += pf::fx(16);
                if (commitGraphNode(editedGraphNode, "Editor: moved graph node right")) return;
            }
            if (uiButton({rect.x + 98.0f, graphNodeButtonY, 38.0f, 22.0f}, "Y-")) {
                editedGraphNode.position.y -= pf::fx(16);
                if (commitGraphNode(editedGraphNode, "Editor: moved graph node up")) return;
            }
            if (uiButton({rect.x + 142.0f, graphNodeButtonY, 38.0f, 22.0f}, "Y+")) {
                editedGraphNode.position.y += pf::fx(16);
                if (commitGraphNode(editedGraphNode, "Editor: moved graph node down")) return;
            }
            if (graphNode.kind == pf::PackageScriptGraphNodeKind::Instruction &&
                graphNode.instructionIndex >= 0 &&
                graphNode.instructionIndex < static_cast<int>(script.instructions.size()) &&
                uiButton({rect.x + 188.0f, graphNodeButtonY, 62.0f, 22.0f}, "UseInst"))
            {
                editor.selectedPackageInstruction = graphNode.instructionIndex;
                session.selectedPackageInstruction = graphNode.instructionIndex;
                editor.selectionKind = pf::FighterEditorSelectionKind::Instruction;
                return;
            }
            std::string renamedGraphNode;
            if (uiTextField({rect.x + 256.0f, graphNodeButtonY, rect.width - 266.0f, 22.0f},
                    "workstation-graph-node-label",
                    editor,
                    graphNode.label,
                    renamedGraphNode,
                    48))
            {
                editedGraphNode.label = renamedGraphNode;
                if (commitGraphNode(editedGraphNode, "Editor: renamed graph node label")) return;
            }
        }
        auto bindSlot = [&](pf::FighterEditorStateCallbackSlot slot, const char* label) -> bool {
            std::string error;
            if (pf::bindEditorSessionPackageScriptCallback(session, session.selectedState, slot, script.name, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, std::string("Editor: bound script to ") + label + " callback");
                return true;
            }
            editor.status = "Editor: callback bind failed: " + error;
            return false;
        };
        const float bindY = graphNodeY + 82.0f;
        DrawText("Bind selected script to state callback", static_cast<int>(rect.x + 10.0f), static_cast<int>(bindY), 11, Fade(RAYWHITE, 0.66f));
        if (uiButton({rect.x + 10.0f, bindY + 20.0f, 58.0f, 22.0f}, "Enter")) {
            if (bindSlot(pf::FighterEditorStateCallbackSlot::Enter, "enter")) return;
        }
        if (uiButton({rect.x + 74.0f, bindY + 20.0f, 58.0f, 22.0f}, "Frame")) {
            if (bindSlot(pf::FighterEditorStateCallbackSlot::Frame, "frame")) return;
        }
        if (uiButton({rect.x + 138.0f, bindY + 20.0f, 58.0f, 22.0f}, "Land")) {
            if (bindSlot(pf::FighterEditorStateCallbackSlot::Landing, "landing")) return;
        }
        if (uiButton({rect.x + 202.0f, bindY + 20.0f, 58.0f, 22.0f}, "Air")) {
            if (bindSlot(pf::FighterEditorStateCallbackSlot::Airborne, "airborne")) return;
        }
        if (!script.instructions.empty()) {
            editor.selectedPackageInstruction = std::clamp(editor.selectedPackageInstruction, 0, static_cast<int>(script.instructions.size()) - 1);
            session.selectedPackageInstruction = editor.selectedPackageInstruction;
            const pf::PackageScriptInstruction& selectedInstruction = script.instructions[static_cast<size_t>(editor.selectedPackageInstruction)];
            const float instrY = bindY + 54.0f;
            DrawText(clippedText("Instruction #" + std::to_string(editor.selectedPackageInstruction) + "  " + packageInstructionLabel(selectedInstruction), 11, rect.width - 20.0f).c_str(),
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(instrY),
                11,
                RAYWHITE);
            auto commitInstruction = [&](pf::PackageScriptInstruction instruction, const std::string& message) -> bool {
                normalizePackageInstruction(instruction, def, world, selectedFighterDef, editor.selectedState, editor.selectedObjectDef);
                std::string error;
                if (pf::setEditorSessionPackageInstruction(session, editor.selectedPackageScript, editor.selectedPackageInstruction, instruction, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                    return true;
                }
                editor.status = "Editor: instruction edit failed: " + error;
                return false;
            };
            pf::PackageScriptInstruction edited = selectedInstruction;
            if (uiButton({rect.x + 10.0f, instrY + 22.0f, 52.0f, 22.0f}, "Op>")) {
                edited.op = nextPackageScriptOp(edited.op);
                if (commitInstruction(edited, "Editor: cycled script op")) return;
            }
            if (uiButton({rect.x + 68.0f, instrY + 22.0f, 44.0f, 22.0f}, "Dst-")) {
                --edited.dst;
                if (commitInstruction(edited, "Editor: changed instruction dst")) return;
            }
            if (uiButton({rect.x + 118.0f, instrY + 22.0f, 44.0f, 22.0f}, "Dst+")) {
                ++edited.dst;
                if (commitInstruction(edited, "Editor: changed instruction dst")) return;
            }
            if (uiButton({rect.x + 168.0f, instrY + 22.0f, 42.0f, 22.0f}, "A-")) {
                --edited.srcA;
                if (commitInstruction(edited, "Editor: changed instruction srcA")) return;
            }
            if (uiButton({rect.x + 216.0f, instrY + 22.0f, 42.0f, 22.0f}, "A+")) {
                ++edited.srcA;
                if (commitInstruction(edited, "Editor: changed instruction srcA")) return;
            }
            if (uiButton({rect.x + 264.0f, instrY + 22.0f, 42.0f, 22.0f}, "I-")) {
                --edited.intValue;
                if (commitInstruction(edited, "Editor: changed instruction integer")) return;
            }
            if (uiButton({rect.x + 312.0f, instrY + 22.0f, 42.0f, 22.0f}, "I+")) {
                ++edited.intValue;
                if (commitInstruction(edited, "Editor: changed instruction integer")) return;
            }
            DrawText(("dst=" + std::to_string(selectedInstruction.dst) +
                      " a=" + std::to_string(selectedInstruction.srcA) +
                      " b=" + std::to_string(selectedInstruction.srcB) +
                      " i=" + std::to_string(selectedInstruction.intValue) +
                      " f=" + std::to_string(pf::fxToFloat(selectedInstruction.fixValue))).c_str(),
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(instrY + 50.0f),
                10,
                Fade(RAYWHITE, 0.64f));
            std::string committedText;
            if (uiTextField({rect.x + 10.0f, instrY + 70.0f, rect.width - 20.0f, 22.0f},
                    "workstation-logic-instruction-text",
                    editor,
                    selectedInstruction.text,
                    committedText,
                    64))
            {
                edited.text = committedText;
                if (commitInstruction(edited, "Editor: changed instruction target text")) return;
            }
        } else {
            DrawText("Selected script has no instructions", static_cast<int>(rect.x + 10.0f), static_cast<int>(bindY + 56.0f), 11, Fade(RAYWHITE, 0.6f));
        }
        return;
    }
    std::string renamedState;
    const auto drawInspectorRule = [&](float y, const char* title) {
        DrawText(title, static_cast<int>(rect.x + 10.0f), static_cast<int>(y), 11, Fade(SKYBLUE, 0.82f));
        DrawLine(
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(y + 15.0f),
            static_cast<int>(rect.x + rect.width - 10.0f),
            static_cast<int>(y + 15.0f),
            Fade(SKYBLUE, 0.24f));
    };

    drawInspectorRule(rect.y + 32.0f, "State");
    DrawText("Name", static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 57.0f), 10, Fade(RAYWHITE, 0.68f));
    if (uiTextField({rect.x + 58.0f, rect.y + 51.0f, rect.width - 68.0f, 24.0f}, "workstation-state-name", editor, state.name, renamedState)) {
        std::string error;
        const std::string oldName = state.name;
        if (pf::renameEditorSessionState(session, session.selectedState, renamedState, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: renamed session state " + oldName + " to " + renamedState);
        } else {
            editor.status = "Editor: rename state failed: " + error;
        }
    }
    int currentClipIndex = -1;
    for (int clipIndex = 0; clipIndex < static_cast<int>(def.authoredClips.size()); ++clipIndex) {
        const pf::AnimationClip& clip = def.authoredClips[static_cast<size_t>(clipIndex)];
        if ((state.animationActionIndex >= 0 && clip.actionIndex == state.animationActionIndex) ||
            (state.animationActionIndex < 0 && clip.name == state.animation))
        {
            currentClipIndex = clipIndex;
            break;
        }
    }
    const std::string animationSummary = currentClipIndex >= 0
        ? ("Anim #" + std::to_string(currentClipIndex) + "  " + state.animation +
           "  action=" + std::to_string(state.animationActionIndex))
        : ("Anim  " + state.animation + "  action=" + std::to_string(state.animationActionIndex));
    DrawText(clippedText(animationSummary, 11, rect.width - 178.0f).c_str(), static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 82.0f), 11, Fade(RAYWHITE, 0.78f));
    auto assignStateClip = [&](int clipIndex) -> bool {
        if (def.authoredClips.empty()) {
            editor.status = "Editor: selected fighter has no authored clips";
            return false;
        }
        clipIndex = wrappedIndex(clipIndex, static_cast<int>(def.authoredClips.size()));
        const pf::AnimationClip& clip = def.authoredClips[static_cast<size_t>(clipIndex)];
        const int clipFrames = std::max(1, static_cast<int>(pf::fxToFloat(clip.frameCount) + 0.5f));
        std::string error;
        if (pf::setEditorSessionStateAnimation(session, session.selectedState, clip.name, clip.actionIndex, clipFrames, &error)) {
            editor.selectedAnimationClip = clipIndex;
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: assigned state animation " + clip.name);
            return true;
        }
        editor.status = "Editor: state animation failed: " + error;
        return false;
    };
    const float clipButtonY = rect.y + 78.0f;
    const float clipButtonX = rect.x + rect.width - 160.0f;
    if (uiButton({clipButtonX, clipButtonY, 46.0f, 22.0f}, "Clip-")) {
        assignStateClip((currentClipIndex >= 0 ? currentClipIndex : editor.selectedAnimationClip) - 1);
        return;
    }
    if (uiButton({clipButtonX + 52.0f, clipButtonY, 46.0f, 22.0f}, "Clip+")) {
        assignStateClip((currentClipIndex >= 0 ? currentClipIndex : editor.selectedAnimationClip) + 1);
        return;
    }
    if (uiButton({clipButtonX + 104.0f, clipButtonY, 56.0f, 22.0f}, "Open")) {
        editor.workspace = pf::EditorWorkspace::Animation;
        editor.selectionKind = pf::FighterEditorSelectionKind::Animation;
        editor.selectedAnimationClip = currentClipIndex >= 0 ? currentClipIndex : std::clamp(editor.selectedAnimationClip, 0, std::max(0, static_cast<int>(def.authoredClips.size()) - 1));
        editor.status = "Editor: opened selected state animation clip";
        return;
    }
    drawInspectorRule(rect.y + 104.0f, "Timing");
    const std::string finishState = state.onAnimationFinishedState.empty() ? "none" : state.onAnimationFinishedState;
    DrawText(("Length " + std::to_string(state.animationLengthFrames) +
              "    IASA " + std::to_string(state.initialInterruptibleFrame) +
              "    Blend " + std::to_string(state.defaultAnimationBlendFrames) +
              "    Finish " + finishState +
              " / " + animationBlendLabel(state.onAnimationFinishedBlendFrames)).c_str(),
        static_cast<int>(rect.x + 10.0f),
        static_cast<int>(rect.y + 128.0f),
        11,
        Fade(RAYWHITE, 0.82f));
    if (uiButton({rect.x + 10.0f, rect.y + 148.0f, 54.0f, 22.0f}, "Len-")) {
        std::string error;
        if (pf::setEditorSessionStateTiming(session, session.selectedState, std::max(1, state.animationLengthFrames - 1), state.initialInterruptibleFrame, state.defaultAnimationBlendFrames, state.onAnimationFinishedBlendFrames, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: shortened selected state");
        } else {
            editor.status = "Editor: state timing failed: " + error;
        }
    }
    if (uiButton({rect.x + 70.0f, rect.y + 148.0f, 54.0f, 22.0f}, "Len+")) {
        std::string error;
        if (pf::setEditorSessionStateTiming(session, session.selectedState, state.animationLengthFrames + 1, state.initialInterruptibleFrame, state.defaultAnimationBlendFrames, state.onAnimationFinishedBlendFrames, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: lengthened selected state");
        } else {
            editor.status = "Editor: state timing failed: " + error;
        }
    }
    if (uiButton({rect.x + 130.0f, rect.y + 148.0f, 54.0f, 22.0f}, "IASA-")) {
        std::string error;
        if (pf::setEditorSessionStateTiming(session, session.selectedState, state.animationLengthFrames, std::max(0, state.initialInterruptibleFrame - 1), state.defaultAnimationBlendFrames, state.onAnimationFinishedBlendFrames, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: moved IASA earlier");
        } else {
            editor.status = "Editor: state timing failed: " + error;
        }
    }
    if (uiButton({rect.x + 190.0f, rect.y + 148.0f, 54.0f, 22.0f}, "IASA+")) {
        std::string error;
        if (pf::setEditorSessionStateTiming(session, session.selectedState, state.animationLengthFrames, state.initialInterruptibleFrame + 1, state.defaultAnimationBlendFrames, state.onAnimationFinishedBlendFrames, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: moved IASA later");
        } else {
            editor.status = "Editor: state timing failed: " + error;
        }
    }
    auto setFinishedTarget = [&](const std::string& targetState, int blendFrames, const std::string& message) -> bool {
        std::string error;
        if (pf::setEditorSessionStateAnimationFinished(session, session.selectedState, targetState, blendFrames, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
            return true;
        }
        editor.status = "Editor: animation-finished target failed: " + error;
        return false;
    };
    const float finishButtonY = rect.y + 174.0f;
    if (uiButton({rect.x + 10.0f, finishButtonY, 46.0f, 22.0f}, "Fin-")) {
        const int current = def.stateIndex(state.onAnimationFinishedState);
        const int next = wrappedIndex((current >= 0 ? current : session.selectedState) - 1, static_cast<int>(def.states.size()));
        setFinishedTarget(def.states[static_cast<size_t>(next)].name, state.onAnimationFinishedBlendFrames, "Editor: changed animation-finished target");
        return;
    }
    if (uiButton({rect.x + 62.0f, finishButtonY, 46.0f, 22.0f}, "Fin+")) {
        const int current = def.stateIndex(state.onAnimationFinishedState);
        const int next = wrappedIndex((current >= 0 ? current : session.selectedState) + 1, static_cast<int>(def.states.size()));
        setFinishedTarget(def.states[static_cast<size_t>(next)].name, state.onAnimationFinishedBlendFrames, "Editor: changed animation-finished target");
        return;
    }
    if (uiButton({rect.x + 114.0f, finishButtonY, 54.0f, 22.0f}, "Clear")) {
        setFinishedTarget({}, state.onAnimationFinishedBlendFrames, "Editor: cleared animation-finished target");
        return;
    }
    if (uiButton({rect.x + 174.0f, finishButtonY, 46.0f, 22.0f}, "FBl-")) {
        const int blend = state.onAnimationFinishedBlendFrames == pf::kDisableAnimationBlendFrames
            ? pf::kDisableAnimationBlendFrames
            : std::max(pf::kDisableAnimationBlendFrames, state.onAnimationFinishedBlendFrames - 1);
        setFinishedTarget(state.onAnimationFinishedState, blend, "Editor: shortened animation-finished blend");
        return;
    }
    if (uiButton({rect.x + 226.0f, finishButtonY, 46.0f, 22.0f}, "FBl+")) {
        const int blend = state.onAnimationFinishedBlendFrames == pf::kDisableAnimationBlendFrames
            ? 0
            : state.onAnimationFinishedBlendFrames + 1;
        setFinishedTarget(state.onAnimationFinishedState, blend, "Editor: lengthened animation-finished blend");
        return;
    }
    drawInspectorRule(rect.y + 210.0f, "Collision Flags");
    const float y = rect.y + 234.0f;
    if (uiButton({rect.x + 10.0f, y, 76.0f, 24.0f}, "Loop", state.loopAnimation)) {
        std::string error;
        if (pf::setEditorSessionStateLoop(session, session.selectedState, !state.loopAnimation, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: toggled state loop flag");
        } else {
            editor.status = "Editor: state loop edit failed: " + error;
        }
    }
    if (uiButton({rect.x + 92.0f, y, 76.0f, 24.0f}, "AnimPhys", state.useAnimPhysics)) {
        std::string error;
        if (pf::setEditorSessionStateCollisionFlags(session, session.selectedState, !state.useAnimPhysics, state.allowSlideoff, state.allowLedgeGrab, state.allowBackwardsLedgeGrab, state.allowWallCollision, state.allowCeilingCollision, state.convertFloorCollisionToGround, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: toggled animation physics");
        } else {
            editor.status = "Editor: collision flags failed: " + error;
        }
    }
    if (uiButton({rect.x + 174.0f, y, 76.0f, 24.0f}, "Ledge", state.allowLedgeGrab)) {
        std::string error;
        if (pf::setEditorSessionStateCollisionFlags(session, session.selectedState, state.useAnimPhysics, state.allowSlideoff, !state.allowLedgeGrab, state.allowBackwardsLedgeGrab, state.allowWallCollision, state.allowCeilingCollision, state.convertFloorCollisionToGround, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: toggled ledge grab flag");
        } else {
            editor.status = "Editor: collision flags failed: " + error;
        }
    }
    if (uiButton({rect.x + 256.0f, y, 76.0f, 24.0f}, "Wall", state.allowWallCollision)) {
        std::string error;
        if (pf::setEditorSessionStateCollisionFlags(session, session.selectedState, state.useAnimPhysics, state.allowSlideoff, state.allowLedgeGrab, state.allowBackwardsLedgeGrab, !state.allowWallCollision, state.allowCeilingCollision, state.convertFloorCollisionToGround, &error)) {
            syncEditorSessionMutation(world, editor, session, selectedFighterDef, "Editor: toggled wall collision flag");
        } else {
            editor.status = "Editor: collision flags failed: " + error;
        }
    }
    drawInspectorRule(y + 36.0f, "References");
    DrawText(("Callbacks  enter=" + std::to_string(state.onEnter.size()) +
              " frame=" + std::to_string(state.onFrame.size()) +
              " land=" + std::to_string(state.onLanding.size()) +
              " air=" + std::to_string(state.onAirborne.size())).c_str(),
        static_cast<int>(rect.x + 10.0f),
        static_cast<int>(y + 60.0f),
        11,
        Fade(RAYWHITE, 0.72f));
    DrawText(("Subactions " + std::to_string(state.action.size()) +
              "  Interrupts " + std::to_string(state.interrupts.size()) +
              "  Hurtboxes " + std::to_string(def.hurtboxes.size())).c_str(),
        static_cast<int>(rect.x + 10.0f),
        static_cast<int>(y + 78.0f),
        11,
        Fade(RAYWHITE, 0.72f));
    if (editor.selectionKind == pf::FighterEditorSelectionKind::Subaction && !state.action.empty()) {
        editor.selectedSubaction = std::clamp(editor.selectedSubaction, 0, static_cast<int>(state.action.size()) - 1);
        const pf::Subaction& subaction = state.action[static_cast<size_t>(editor.selectedSubaction)];
        DrawText(("Selected subaction #" + std::to_string(editor.selectedSubaction) + "  " + subactionTypeName(subaction.type)).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(y + 84.0f),
            12,
            RAYWHITE);
        auto commitSubaction = [&](pf::Subaction edited, const std::string& message) -> bool {
            std::string error;
            if (pf::setEditorSessionSubaction(session, session.selectedState, editor.selectedSubaction, edited, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                return true;
            }
            editor.status = "Editor: subaction edit failed: " + error;
            return false;
        };
        pf::Subaction editedSubaction = subaction;
        const float subY = y + 106.0f;
        DrawText(("frames=" + std::to_string(subaction.frames)).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(subY + 5.0f),
            10,
            Fade(RAYWHITE, 0.68f));
        if (uiButton({rect.x + 82.0f, subY, 48.0f, 22.0f}, "Fr-")) {
            editedSubaction.frames = std::max(0, editedSubaction.frames - 1);
            if (commitSubaction(editedSubaction, "Editor: shortened selected subaction")) return;
        }
        if (uiButton({rect.x + 136.0f, subY, 48.0f, 22.0f}, "Fr+")) {
            ++editedSubaction.frames;
            if (commitSubaction(editedSubaction, "Editor: lengthened selected subaction")) return;
        }
        if (subaction.type == pf::SubactionType::CallScript && !def.packageScripts.empty()) {
            if (uiButton({rect.x + 190.0f, subY, 74.0f, 22.0f}, "UseScript")) {
                editedSubaction.objectName = packageScriptTargetName(def.packageScripts, editor.selectedPackageScript);
                if (commitSubaction(editedSubaction, "Editor: retargeted script subaction")) return;
            }
            DrawText(clippedText("call " + subaction.objectName, 10, rect.width - 282.0f).c_str(),
                static_cast<int>(rect.x + 272.0f),
                static_cast<int>(subY + 5.0f),
                10,
                Fade(RAYWHITE, 0.68f));
        }
        if (subaction.type == pf::SubactionType::CreateHitbox || subaction.type == pf::SubactionType::CreateThrowHitbox) {
            DrawText(("Hitbox dmg=" + std::to_string(pf::fxToFloat(subaction.hitbox.damage)) +
                      " r=" + std::to_string(pf::fxToFloat(subaction.hitbox.radius)) +
                      " angle=" + std::to_string(pf::fxToFloat(subaction.hitbox.knockbackAngleDegrees))).c_str(),
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(y + 132.0f),
                11,
                Fade(RAYWHITE, 0.72f));
            const float hitY = y + 154.0f;
            if (uiButton({rect.x + 10.0f, hitY, 46.0f, 22.0f}, "Dmg-")) {
                editedSubaction.hitbox.damage = std::max(pf::Fix{0}, editedSubaction.hitbox.damage - pf::fx(1));
                if (commitSubaction(editedSubaction, "Editor: lowered selected hitbox damage")) return;
            }
            if (uiButton({rect.x + 62.0f, hitY, 46.0f, 22.0f}, "Dmg+")) {
                editedSubaction.hitbox.damage += pf::fx(1);
                if (commitSubaction(editedSubaction, "Editor: raised selected hitbox damage")) return;
            }
            if (uiButton({rect.x + 114.0f, hitY, 42.0f, 22.0f}, "R-")) {
                editedSubaction.hitbox.radius = std::max(pf::fxFromFloat(0.05f), editedSubaction.hitbox.radius - pf::fxFromFloat(0.05f));
                if (commitSubaction(editedSubaction, "Editor: shrank selected hitbox")) return;
            }
            if (uiButton({rect.x + 162.0f, hitY, 42.0f, 22.0f}, "R+")) {
                editedSubaction.hitbox.radius += pf::fxFromFloat(0.05f);
                if (commitSubaction(editedSubaction, "Editor: enlarged selected hitbox")) return;
            }
            if (uiButton({rect.x + 210.0f, hitY, 44.0f, 22.0f}, "Ang-")) {
                editedSubaction.hitbox.knockbackAngleDegrees -= pf::fx(5);
                if (commitSubaction(editedSubaction, "Editor: lowered selected hitbox angle")) return;
            }
            if (uiButton({rect.x + 260.0f, hitY, 44.0f, 22.0f}, "Ang+")) {
                editedSubaction.hitbox.knockbackAngleDegrees += pf::fx(5);
                if (commitSubaction(editedSubaction, "Editor: raised selected hitbox angle")) return;
            }
            const float offY = hitY + 26.0f;
            if (uiButton({rect.x + 10.0f, offY, 42.0f, 22.0f}, "X-")) {
                editedSubaction.hitbox.offset.x -= pf::fxFromFloat(0.1f);
                if (commitSubaction(editedSubaction, "Editor: moved selected hitbox backward")) return;
            }
            if (uiButton({rect.x + 58.0f, offY, 42.0f, 22.0f}, "X+")) {
                editedSubaction.hitbox.offset.x += pf::fxFromFloat(0.1f);
                if (commitSubaction(editedSubaction, "Editor: moved selected hitbox forward")) return;
            }
            if (uiButton({rect.x + 106.0f, offY, 42.0f, 22.0f}, "Y-")) {
                editedSubaction.hitbox.offset.y -= pf::fxFromFloat(0.1f);
                if (commitSubaction(editedSubaction, "Editor: lowered selected hitbox")) return;
            }
            if (uiButton({rect.x + 154.0f, offY, 42.0f, 22.0f}, "Y+")) {
                editedSubaction.hitbox.offset.y += pf::fxFromFloat(0.1f);
                if (commitSubaction(editedSubaction, "Editor: raised selected hitbox")) return;
            }
        }
        if (subaction.type == pf::SubactionType::SetHurtboxState) {
            const int authoredHurtboxCount = static_cast<int>(def.hurtboxes.size());
            const bool targetsAll = subaction.hurtboxIndex < 0 && subaction.joint < 0;
            std::string targetLabel = targetsAll
                ? "all hurtboxes"
                : ("hurtbox #" + std::to_string(subaction.hurtboxIndex));
            if (subaction.hurtboxIndex >= 0 && subaction.hurtboxIndex < authoredHurtboxCount) {
                const pf::HurtboxDefinition& target = def.hurtboxes[static_cast<size_t>(subaction.hurtboxIndex)];
                targetLabel += " " + std::string(pf::boneName(target.bone));
            }
            if (subaction.joint >= 0) {
                targetLabel = "bone id " + std::to_string(subaction.joint);
            }
            DrawText(("Hurtbox timing: " + targetLabel + " -> " + hurtboxStateName(subaction.hurtboxState)).c_str(),
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(y + 132.0f),
                11,
                Fade(RAYWHITE, 0.72f));
            const float hurtTimingY = y + 154.0f;
            if (uiButton({rect.x + 10.0f, hurtTimingY, 48.0f, 22.0f}, "Prev") && authoredHurtboxCount > 0) {
                editedSubaction.joint = -1;
                editedSubaction.hurtboxIndex = wrappedIndex(
                    editedSubaction.hurtboxIndex < 0 ? authoredHurtboxCount - 1 : editedSubaction.hurtboxIndex - 1,
                    authoredHurtboxCount);
                editor.selectedHurtbox = editedSubaction.hurtboxIndex;
                if (commitSubaction(editedSubaction, "Editor: retargeted hurtbox timing")) return;
            }
            if (uiButton({rect.x + 64.0f, hurtTimingY, 48.0f, 22.0f}, "Next") && authoredHurtboxCount > 0) {
                editedSubaction.joint = -1;
                editedSubaction.hurtboxIndex = wrappedIndex(editedSubaction.hurtboxIndex + 1, authoredHurtboxCount);
                editor.selectedHurtbox = editedSubaction.hurtboxIndex;
                if (commitSubaction(editedSubaction, "Editor: retargeted hurtbox timing")) return;
            }
            if (uiButton({rect.x + 118.0f, hurtTimingY, 46.0f, 22.0f}, "All")) {
                editedSubaction.joint = -1;
                editedSubaction.hurtboxIndex = -1;
                if (commitSubaction(editedSubaction, "Editor: set hurtbox timing target to all")) return;
            }
            if (uiButton({rect.x + 170.0f, hurtTimingY, 54.0f, 22.0f}, "State")) {
                editedSubaction.hurtboxState = nextHurtboxState(editedSubaction.hurtboxState);
                if (commitSubaction(editedSubaction, "Editor: changed hurtbox timing state")) return;
            }
            DrawText("Timeline hurtbox markers select this subaction and its authored hurtbox target.",
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(hurtTimingY + 30.0f),
                10,
                Fade(RAYWHITE, 0.62f));
        } else if (subaction.type == pf::SubactionType::SetBodyCollisionState) {
            DrawText(("Body collision timing -> " + std::string(hurtboxStateName(subaction.hurtboxState))).c_str(),
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(y + 132.0f),
                11,
                Fade(RAYWHITE, 0.72f));
            if (uiButton({rect.x + 10.0f, y + 154.0f, 64.0f, 22.0f}, "State")) {
                editedSubaction.hurtboxState = nextHurtboxState(editedSubaction.hurtboxState);
                if (commitSubaction(editedSubaction, "Editor: changed body collision timing state")) return;
            }
        }
    }
    if (!state.interrupts.empty()) {
        editor.selectedInterrupt = std::clamp(editor.selectedInterrupt, 0, static_cast<int>(state.interrupts.size()) - 1);
        const pf::InterruptRule& interrupt = state.interrupts[static_cast<size_t>(editor.selectedInterrupt)];
        const float intY = y + 206.0f;
        DrawText(("Interrupt #" + std::to_string(editor.selectedInterrupt) +
                  " -> " + interrupt.targetState +
                  "  " + interruptConditionName(interrupt.condition) +
                  "  " + groundRequirementName(interrupt.ground)).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(intY),
            11,
            RAYWHITE);
        auto commitInterrupt = [&](pf::InterruptRule edited, const std::string& message) -> bool {
            std::string error;
            if (pf::setEditorSessionInterrupt(session, session.selectedState, editor.selectedInterrupt, edited, &error)) {
                syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                return true;
            }
            editor.status = "Editor: interrupt edit failed: " + error;
            return false;
        };
        pf::InterruptRule editedInterrupt = interrupt;
        const float intButtonsY = intY + 22.0f;
        if (uiButton({rect.x + 10.0f, intButtonsY, 50.0f, 22.0f}, "Cond")) {
            editedInterrupt.condition = nextCommonInterruptCondition(editedInterrupt.condition);
            if (commitInterrupt(editedInterrupt, "Editor: changed interrupt condition")) return;
        }
        if (uiButton({rect.x + 66.0f, intButtonsY, 50.0f, 22.0f}, "Ground")) {
            editedInterrupt.ground = nextGroundRequirement(editedInterrupt.ground);
            if (commitInterrupt(editedInterrupt, "Editor: changed interrupt ground requirement")) return;
        }
        if (uiButton({rect.x + 122.0f, intButtonsY, 42.0f, 22.0f}, "En-")) {
            editedInterrupt.enableFrame = std::max(0, editedInterrupt.enableFrame - 1);
            if (commitInterrupt(editedInterrupt, "Editor: moved interrupt start earlier")) return;
        }
        if (uiButton({rect.x + 170.0f, intButtonsY, 42.0f, 22.0f}, "En+")) {
            ++editedInterrupt.enableFrame;
            if (commitInterrupt(editedInterrupt, "Editor: moved interrupt start later")) return;
        }
        if (uiButton({rect.x + 218.0f, intButtonsY, 44.0f, 22.0f}, "Dis-")) {
            editedInterrupt.disableFrame = std::max(0, editedInterrupt.disableFrame - 1);
            if (commitInterrupt(editedInterrupt, "Editor: moved interrupt end earlier")) return;
        }
        if (uiButton({rect.x + 268.0f, intButtonsY, 44.0f, 22.0f}, "Dis+")) {
            ++editedInterrupt.disableFrame;
            if (commitInterrupt(editedInterrupt, "Editor: moved interrupt end later")) return;
        }
        DrawText(("enable=" + std::to_string(interrupt.enableFrame) +
                  " disable=" + std::to_string(interrupt.disableFrame) +
                  " lag=" + std::to_string(interrupt.lagFrames)).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(intButtonsY + 28.0f),
            10,
            Fade(RAYWHITE, 0.64f));
        std::string committedTarget;
        if (uiTextField({rect.x + 10.0f, intButtonsY + 48.0f, rect.width - 20.0f, 22.0f},
                "workstation-interrupt-target",
                editor,
                interrupt.targetState,
                committedTarget,
                64))
        {
            editedInterrupt.targetState = committedTarget;
            if (commitInterrupt(editedInterrupt, "Editor: changed interrupt target state")) return;
        }
    }
    const float scriptY = y + 304.0f;
    if (!def.packageScripts.empty() && scriptY + 112.0f < rect.y + rect.height) {
        editor.selectedPackageScript = std::clamp(editor.selectedPackageScript, 0, static_cast<int>(def.packageScripts.size()) - 1);
        session.selectedPackageScript = editor.selectedPackageScript;
        pf::PackageScript& script = def.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
        DrawText(clippedText("Script node: " + script.name, 12, rect.width - 20.0f).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(scriptY),
            12,
            RAYWHITE);
        if (!script.instructions.empty()) {
            editor.selectedPackageInstruction = std::clamp(editor.selectedPackageInstruction, 0, static_cast<int>(script.instructions.size()) - 1);
            session.selectedPackageInstruction = editor.selectedPackageInstruction;
            const pf::PackageScriptInstruction& selectedInstruction = script.instructions[static_cast<size_t>(editor.selectedPackageInstruction)];
            DrawText(clippedText("#" + std::to_string(editor.selectedPackageInstruction) + "  " + packageInstructionLabel(selectedInstruction), 11, rect.width - 20.0f).c_str(),
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(scriptY + 20.0f),
                11,
                Fade(RAYWHITE, 0.74f));

            auto commitInstruction = [&](pf::PackageScriptInstruction instruction, const std::string& message) -> bool {
                normalizePackageInstruction(instruction, def, world, selectedFighterDef, editor.selectedState, editor.selectedObjectDef);
                std::string error;
                if (pf::setEditorSessionPackageInstruction(session, editor.selectedPackageScript, editor.selectedPackageInstruction, instruction, &error)) {
                    syncEditorSessionMutation(world, editor, session, selectedFighterDef, message);
                    return true;
                }
                editor.status = "Editor: instruction edit failed: " + error;
                return false;
            };

            pf::PackageScriptInstruction edited = selectedInstruction;
            const float rowA = scriptY + 42.0f;
            if (uiButton({rect.x + 10.0f, rowA, 52.0f, 22.0f}, "Op>")) {
                edited.op = nextPackageScriptOp(edited.op);
                if (commitInstruction(edited, "Editor: cycled script op")) return;
            }
            if (uiButton({rect.x + 68.0f, rowA, 44.0f, 22.0f}, "Dst-")) {
                --edited.dst;
                if (commitInstruction(edited, "Editor: changed instruction dst")) return;
            }
            if (uiButton({rect.x + 118.0f, rowA, 44.0f, 22.0f}, "Dst+")) {
                ++edited.dst;
                if (commitInstruction(edited, "Editor: changed instruction dst")) return;
            }
            if (uiButton({rect.x + 168.0f, rowA, 42.0f, 22.0f}, "A-")) {
                --edited.srcA;
                if (commitInstruction(edited, "Editor: changed instruction srcA")) return;
            }
            if (uiButton({rect.x + 216.0f, rowA, 42.0f, 22.0f}, "A+")) {
                ++edited.srcA;
                if (commitInstruction(edited, "Editor: changed instruction srcA")) return;
            }
            if (uiButton({rect.x + 264.0f, rowA, 42.0f, 22.0f}, "B-")) {
                --edited.srcB;
                if (commitInstruction(edited, "Editor: changed instruction srcB")) return;
            }
            if (uiButton({rect.x + 312.0f, rowA, 42.0f, 22.0f}, "B+")) {
                ++edited.srcB;
                if (commitInstruction(edited, "Editor: changed instruction srcB")) return;
            }

            const float rowB = scriptY + 70.0f;
            DrawText(("dst=" + std::to_string(selectedInstruction.dst) +
                      " a=" + std::to_string(selectedInstruction.srcA) +
                      " b=" + std::to_string(selectedInstruction.srcB) +
                      " i=" + std::to_string(selectedInstruction.intValue) +
                      " f=" + std::to_string(pf::fxToFloat(selectedInstruction.fixValue))).c_str(),
                static_cast<int>(rect.x + 10.0f),
                static_cast<int>(rowB + 4.0f),
                10,
                Fade(RAYWHITE, 0.64f));
            if (uiButton({rect.x + rect.width - 178.0f, rowB, 42.0f, 22.0f}, "I-")) {
                --edited.intValue;
                if (commitInstruction(edited, "Editor: changed instruction integer")) return;
            }
            if (uiButton({rect.x + rect.width - 130.0f, rowB, 42.0f, 22.0f}, "I+")) {
                ++edited.intValue;
                if (commitInstruction(edited, "Editor: changed instruction integer")) return;
            }
            if (uiButton({rect.x + rect.width - 82.0f, rowB, 32.0f, 22.0f}, "F-")) {
                edited.fixValue -= pf::fxFromFloat(0.1f);
                if (commitInstruction(edited, "Editor: changed instruction fixed value")) return;
            }
            if (uiButton({rect.x + rect.width - 44.0f, rowB, 32.0f, 22.0f}, "F+")) {
                edited.fixValue += pf::fxFromFloat(0.1f);
                if (commitInstruction(edited, "Editor: changed instruction fixed value")) return;
            }
            std::string committedText;
            if (uiTextField({rect.x + 10.0f, scriptY + 96.0f, rect.width - 20.0f, 22.0f},
                    "workstation-instruction-text",
                    editor,
                    selectedInstruction.text,
                    committedText,
                    64))
            {
                edited.text = committedText;
                if (commitInstruction(edited, "Editor: changed instruction target text")) return;
            }
        } else {
            DrawText("Selected script has no instructions", static_cast<int>(rect.x + 10.0f), static_cast<int>(scriptY + 20.0f), 11, Fade(RAYWHITE, 0.6f));
        }
    }
    if (!world.objectDefs.empty()) {
        DrawText(("Objects/articles: " + std::to_string(world.objectDefs.size())).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(rect.y + rect.height - 28.0f),
            11,
            Fade(RAYWHITE, 0.62f));
    }
}

static std::string editorContextDetail(
    const pf::FighterEditor& editor,
    const pf::FighterEditorSession& session,
    const pf::FighterDefinition& def,
    const pf::FighterState& state)
{
    switch (editor.selectionKind) {
    case pf::FighterEditorSelectionKind::State:
        return "state " + state.name + " index=" + std::to_string(session.selectedState);
    case pf::FighterEditorSelectionKind::Subaction:
        if (editor.selectedSubaction >= 0 && editor.selectedSubaction < static_cast<int>(state.action.size())) {
            const pf::Subaction& subaction = state.action[static_cast<size_t>(editor.selectedSubaction)];
            return "subaction #" + std::to_string(editor.selectedSubaction) + " " + subactionTypeName(subaction.type) +
                " frames=" + std::to_string(subaction.frames);
        }
        return "subaction none";
    case pf::FighterEditorSelectionKind::Interrupt:
        if (editor.selectedInterrupt >= 0 && editor.selectedInterrupt < static_cast<int>(state.interrupts.size())) {
            const pf::InterruptRule& interrupt = state.interrupts[static_cast<size_t>(editor.selectedInterrupt)];
            return "interrupt #" + std::to_string(editor.selectedInterrupt) + " -> " + interrupt.targetState +
                " enable=" + std::to_string(interrupt.enableFrame) +
                " disable=" + std::to_string(interrupt.disableFrame);
        }
        return "interrupt none";
    case pf::FighterEditorSelectionKind::Callback:
        {
            const std::vector<pf::FunctionCall>& calls = stateCallbackCalls(state, editor.selectedStateCallbackSlot);
            if (editor.selectedStateCallback >= 0 && editor.selectedStateCallback < static_cast<int>(calls.size())) {
                return std::string("callback ") + stateCallbackSlotName(editor.selectedStateCallbackSlot) +
                    " #" + std::to_string(editor.selectedStateCallback) +
                    " " + calls[static_cast<size_t>(editor.selectedStateCallback)].name;
            }
        }
        return std::string("callback ") + stateCallbackSlotName(editor.selectedStateCallbackSlot) + " none";
    case pf::FighterEditorSelectionKind::ObjectCallback:
        if (editor.selectedObjectDef >= 0 && editor.selectedObjectDef < static_cast<int>(session.package.objects.size())) {
            const pf::GameObjectDefinition& object = session.package.objects[static_cast<size_t>(editor.selectedObjectDef)];
            const int stateSlot = wrappedIndex(editor.selectedObjectStateCallback, 4);
            const int eventSlot = wrappedIndex(editor.selectedObjectEventCallback, 21);
            if (editor.selectedObjectCallbackEvent) {
                pf::GameObjectDefinition objectCopy = object;
                const std::vector<pf::FunctionCall>& calls = objectEventCallbacks(objectCopy, eventSlot);
                std::string detail = "object callback event " + std::string(objectEventCallbackLabel(eventSlot)) +
                    " object=" + object.name + " count=" + std::to_string(calls.size());
                if (editor.selectedObjectCallback >= 0 && editor.selectedObjectCallback < static_cast<int>(calls.size())) {
                    detail += " #" + std::to_string(editor.selectedObjectCallback) +
                        " " + calls[static_cast<size_t>(editor.selectedObjectCallback)].name;
                }
                return detail;
            }
            if (editor.selectedObjectState >= 0 && editor.selectedObjectState < static_cast<int>(object.states.size())) {
                pf::GameObjectStateDefinition stateCopy = object.states[static_cast<size_t>(editor.selectedObjectState)];
                const std::vector<pf::FunctionCall>& calls = objectStateCallbacks(stateCopy, stateSlot);
                std::string detail = "object callback " + std::string(objectStateCallbackLabel(stateSlot)) +
                    " object=" + object.name +
                    " state=" + object.states[static_cast<size_t>(editor.selectedObjectState)].name +
                    " count=" + std::to_string(calls.size());
                if (editor.selectedObjectCallback >= 0 && editor.selectedObjectCallback < static_cast<int>(calls.size())) {
                    detail += " #" + std::to_string(editor.selectedObjectCallback) +
                        " " + calls[static_cast<size_t>(editor.selectedObjectCallback)].name;
                }
                return detail;
            }
        }
        return "object callback none";
    case pf::FighterEditorSelectionKind::Script:
        if (editor.selectedPackageScript >= 0 && editor.selectedPackageScript < static_cast<int>(def.packageScripts.size())) {
            const pf::PackageScript& script = def.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
            return "script #" + std::to_string(editor.selectedPackageScript) + " " + script.name +
                " instructions=" + std::to_string(script.instructions.size()) +
                " graphNodes=" + std::to_string(script.graph.nodes.size());
        }
        return "script none";
    case pf::FighterEditorSelectionKind::Instruction:
        if (editor.workspace == pf::EditorWorkspace::Assets &&
            editor.selectedObjectDef >= 0 &&
            editor.selectedObjectDef < static_cast<int>(session.package.objects.size()))
        {
            const pf::GameObjectDefinition& object = session.package.objects[static_cast<size_t>(editor.selectedObjectDef)];
            if (editor.selectedPackageScript >= 0 && editor.selectedPackageScript < static_cast<int>(object.packageScripts.size())) {
                const pf::PackageScript& script = object.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
                if (editor.selectedPackageInstruction >= 0 && editor.selectedPackageInstruction < static_cast<int>(script.instructions.size())) {
                    return "object instruction #" + std::to_string(editor.selectedPackageInstruction) + " " +
                        packageInstructionLabel(script.instructions[static_cast<size_t>(editor.selectedPackageInstruction)]) +
                        " object=" + object.name + " script=" + script.name;
                }
            }
            return "object instruction none";
        }
        if (editor.selectedPackageScript >= 0 && editor.selectedPackageScript < static_cast<int>(def.packageScripts.size())) {
            const pf::PackageScript& script = def.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
            if (editor.selectedPackageInstruction >= 0 && editor.selectedPackageInstruction < static_cast<int>(script.instructions.size())) {
                return "instruction #" + std::to_string(editor.selectedPackageInstruction) + " " +
                    packageInstructionLabel(script.instructions[static_cast<size_t>(editor.selectedPackageInstruction)]);
            }
        }
        return "instruction none";
    case pf::FighterEditorSelectionKind::Variable:
        if (editor.selectedPackageVariable >= 0 && editor.selectedPackageVariable < static_cast<int>(def.packageVariables.size())) {
            const pf::PackageVariableDefinition& variable = def.packageVariables[static_cast<size_t>(editor.selectedPackageVariable)];
            return "variable #" + std::to_string(editor.selectedPackageVariable) + " " + variable.name +
                " initial=" + std::to_string(variable.initialValue) +
                " uses=" + std::to_string(countPackageVariableOperandUses(def, editor.selectedPackageVariable));
        }
        return "variable none";
    case pf::FighterEditorSelectionKind::Object:
        if (editor.selectedObjectDef >= 0 && editor.selectedObjectDef < static_cast<int>(session.package.objects.size())) {
            const pf::GameObjectDefinition& object = session.package.objects[static_cast<size_t>(editor.selectedObjectDef)];
            return "object #" + std::to_string(editor.selectedObjectDef) + " " + object.name +
                " kind=" + gameObjectKindName(object.kind) +
                " states=" + std::to_string(object.states.size()) +
                " scripts=" + std::to_string(object.packageScripts.size()) +
                " boxes=" + std::to_string(object.hitboxes.size() + object.hurtboxes.size() + object.touchboxes.size());
        }
        return "object/article none";
    case pf::FighterEditorSelectionKind::Animation:
        return "animation clip=" + std::to_string(editor.selectedAnimationClip) +
            " joint=" + std::to_string(editor.selectedAnimationJoint) +
            " frame=" + std::to_string(editor.animationScrubFrame);
    case pf::FighterEditorSelectionKind::Viewport:
        return std::string("viewport overlays model=") + (editor.showModel ? "on" : "off") +
            " hit=" + (editor.showHitboxes ? "on" : "off") +
            " hurt=" + (editor.showHurtboxes ? "on" : "off");
    }
    return "state " + state.name;
}

static const pf::PackageScriptGraphNode* editorPackageGraphNodeById(
    const pf::PackageScriptGraph& graph,
    int nodeId)
{
    const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) {
        return node.id == nodeId;
    });
    return found == graph.nodes.end() ? nullptr : &*found;
}

static std::string editorPackageGraphNodeSummary(
    const pf::PackageScript& script,
    const pf::PackageScriptGraphNode& node)
{
    std::string summary = "node #" + std::to_string(node.id) + " " + packageScriptGraphNodeKindName(node.kind);
    if (node.kind == pf::PackageScriptGraphNodeKind::Instruction) {
        summary += " inst=" + std::to_string(node.instructionIndex);
        if (node.instructionIndex >= 0 && node.instructionIndex < static_cast<int>(script.instructions.size())) {
            summary += " " + std::string(packageScriptOpName(script.instructions[static_cast<size_t>(node.instructionIndex)].op));
        }
    }
    summary += " pos=(" + std::to_string(pf::fxToFloat(node.position.x)) +
        ", " + std::to_string(pf::fxToFloat(node.position.y)) + ")";
    return summary;
}

static std::vector<int> editorReachableGraphInstructionNodes(const pf::PackageScriptGraph& graph) {
    std::vector<int> reachable;
    std::vector<int> seen;
    int currentNode = graph.entryNode;
    while (currentNode >= 0) {
        if (std::find(seen.begin(), seen.end(), currentNode) != seen.end()) {
            break;
        }
        seen.push_back(currentNode);
        const pf::PackageScriptGraphNode* node = editorPackageGraphNodeById(graph, currentNode);
        if (!node) {
            break;
        }
        if (node->kind == pf::PackageScriptGraphNodeKind::Instruction) {
            reachable.push_back(node->id);
        }
        const auto link = std::find_if(graph.links.begin(), graph.links.end(), [&](const pf::PackageScriptGraphLink& candidate) {
            return candidate.fromNode == currentNode && candidate.fromSocket == 0;
        });
        if (link == graph.links.end()) {
            break;
        }
        currentNode = link->toNode;
    }
    return reachable;
}

static std::string editorPackageGraphIssueHint(
    const pf::FighterEditor& editor,
    const pf::PackageScript& script,
    const std::string& error)
{
    const pf::PackageScriptGraph& graph = script.graph;
    if (graph.nodes.empty()) {
        return "Graph issue: script has no graph metadata";
    }

    const std::string lowerError = lowerAscii(error);
    if (lowerError.find("entry is invalid") != std::string::npos) {
        int entryCount = 0;
        for (const pf::PackageScriptGraphNode& node : graph.nodes) {
            if (node.kind == pf::PackageScriptGraphNodeKind::Entry) {
                ++entryCount;
            }
        }
        return "Graph issue: entryNode=" + std::to_string(graph.entryNode) +
            " entryNodes=" + std::to_string(entryCount);
    }

    if (lowerError.find("multiple links from one socket") != std::string::npos) {
        for (int i = 0; i < static_cast<int>(graph.links.size()); ++i) {
            const pf::PackageScriptGraphLink& a = graph.links[static_cast<size_t>(i)];
            for (int j = i + 1; j < static_cast<int>(graph.links.size()); ++j) {
                const pf::PackageScriptGraphLink& b = graph.links[static_cast<size_t>(j)];
                if (a.fromNode == b.fromNode && a.fromSocket == b.fromSocket) {
                    return "Graph issue: duplicate outgoing links from node #" +
                        std::to_string(a.fromNode) + " socket " + std::to_string(a.fromSocket);
                }
            }
        }
    }

    if (lowerError.find("link target is invalid") != std::string::npos ||
        lowerError.find("target is invalid") != std::string::npos)
    {
        for (const pf::PackageScriptGraphLink& link : graph.links) {
            if (!editorPackageGraphNodeById(graph, link.fromNode) ||
                !editorPackageGraphNodeById(graph, link.toNode))
            {
                return "Graph issue: invalid link " + std::to_string(link.fromNode) +
                    ":" + std::to_string(link.fromSocket) + " -> " +
                    std::to_string(link.toNode) + ":" + std::to_string(link.toSocket);
            }
        }
    }

    if (lowerError.find("instruction is invalid") != std::string::npos) {
        for (const pf::PackageScriptGraphNode& node : graph.nodes) {
            if (node.kind == pf::PackageScriptGraphNodeKind::Instruction &&
                (node.instructionIndex < 0 || node.instructionIndex >= static_cast<int>(script.instructions.size())))
            {
                return "Graph issue: " + editorPackageGraphNodeSummary(script, node) +
                    " outside instruction count " + std::to_string(script.instructions.size());
            }
        }
    }

    if (lowerError.find("instruction is duplicate") != std::string::npos) {
        for (int i = 0; i < static_cast<int>(graph.nodes.size()); ++i) {
            const pf::PackageScriptGraphNode& a = graph.nodes[static_cast<size_t>(i)];
            if (a.kind != pf::PackageScriptGraphNodeKind::Instruction) {
                continue;
            }
            for (int j = i + 1; j < static_cast<int>(graph.nodes.size()); ++j) {
                const pf::PackageScriptGraphNode& b = graph.nodes[static_cast<size_t>(j)];
                if (b.kind == pf::PackageScriptGraphNodeKind::Instruction &&
                    a.instructionIndex == b.instructionIndex)
                {
                    return "Graph issue: nodes #" + std::to_string(a.id) + " and #" +
                        std::to_string(b.id) + " both use instruction " +
                        std::to_string(a.instructionIndex);
                }
            }
        }
    }

    if (lowerError.find("control cycle") != std::string::npos) {
        std::vector<int> seen;
        int currentNode = graph.entryNode;
        while (currentNode >= 0) {
            if (std::find(seen.begin(), seen.end(), currentNode) != seen.end()) {
                return "Graph issue: control cycle returns to node #" + std::to_string(currentNode);
            }
            seen.push_back(currentNode);
            const auto link = std::find_if(graph.links.begin(), graph.links.end(), [&](const pf::PackageScriptGraphLink& candidate) {
                return candidate.fromNode == currentNode && candidate.fromSocket == 0;
            });
            if (link == graph.links.end()) {
                break;
            }
            currentNode = link->toNode;
        }
    }

    if (lowerError.find("disconnected instruction nodes") != std::string::npos) {
        const std::vector<int> reachable = editorReachableGraphInstructionNodes(graph);
        for (const pf::PackageScriptGraphNode& node : graph.nodes) {
            if (node.kind == pf::PackageScriptGraphNodeKind::Instruction &&
                std::find(reachable.begin(), reachable.end(), node.id) == reachable.end())
            {
                return "Graph issue: disconnected " + editorPackageGraphNodeSummary(script, node);
            }
        }
    }

    if (lowerError.find("does not cover all bytecode instructions") != std::string::npos) {
        for (int instructionIndex = 0; instructionIndex < static_cast<int>(script.instructions.size()); ++instructionIndex) {
            const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const pf::PackageScriptGraphNode& node) {
                return node.kind == pf::PackageScriptGraphNodeKind::Instruction &&
                    node.instructionIndex == instructionIndex;
            });
            if (found == graph.nodes.end()) {
                return "Graph issue: missing graph node for instruction #" +
                    std::to_string(instructionIndex) + " " +
                    packageScriptOpName(script.instructions[static_cast<size_t>(instructionIndex)].op);
            }
        }
    }

    const pf::PackageScriptGraphNode* selected = editorPackageGraphNodeById(graph, editor.selectedPackageGraphNode);
    if (selected) {
        return "Graph focus: " + editorPackageGraphNodeSummary(script, *selected);
    }
    return "Graph focus: no selected graph node";
}

static std::string editorSelectedGraphDiagnostic(
    const pf::FighterEditor& editor,
    const pf::FighterEditorSession& session,
    const pf::FighterDefinition& def,
    Color& color)
{
    color = Fade(RAYWHITE, 0.62f);
    const bool objectGraphContext =
        (editor.workspace == pf::EditorWorkspace::Assets ||
         editor.selectionKind == pf::FighterEditorSelectionKind::Object ||
         editor.selectionKind == pf::FighterEditorSelectionKind::ObjectCallback) &&
        editor.selectedObjectDef >= 0 &&
        editor.selectedObjectDef < static_cast<int>(session.package.objects.size());
    if (objectGraphContext) {
        const pf::GameObjectDefinition& object = session.package.objects[static_cast<size_t>(editor.selectedObjectDef)];
        if (object.packageScripts.empty()) {
            return "Graph: selected object has no scripts";
        }
        const int scriptIndex = std::clamp(editor.selectedPackageScript, 0, static_cast<int>(object.packageScripts.size()) - 1);
        pf::PackageScript script = object.packageScripts[static_cast<size_t>(scriptIndex)];
        if (script.graph.nodes.empty()) {
            return "Graph: selected object script has no metadata";
        }
        std::string error;
        if (pf::compilePackageScriptGraph(script, &error)) {
            color = GREEN;
            const pf::PackageScriptGraphNode* selected = editorPackageGraphNodeById(script.graph, editor.selectedPackageGraphNode);
            return selected
                ? "Graph: selected object script compiles; " + editorPackageGraphNodeSummary(script, *selected)
                : "Graph: selected object script compiles";
        }
        color = RED;
        return "Graph: object " + error + "; " + editorPackageGraphIssueHint(editor, script, error);
    }
    if (def.packageScripts.empty()) {
        return "Graph: no package scripts";
    }
    const int scriptIndex = std::clamp(editor.selectedPackageScript, 0, static_cast<int>(def.packageScripts.size()) - 1);
    pf::PackageScript script = def.packageScripts[static_cast<size_t>(scriptIndex)];
    if (script.graph.nodes.empty()) {
        return "Graph: no metadata; Linear/Flow can rebuild it";
    }
    std::string error;
    if (pf::compilePackageScriptGraph(script, &error)) {
        color = GREEN;
        const pf::PackageScriptGraphNode* selected = editorPackageGraphNodeById(script.graph, editor.selectedPackageGraphNode);
        return selected
            ? "Graph: selected script compiles; " + editorPackageGraphNodeSummary(script, *selected)
            : "Graph: selected script compiles";
    }
    color = RED;
    return "Graph: " + error + "; " + editorPackageGraphIssueHint(editor, script, error);
}

enum class EditorDiagnosticTargetKind : uint8_t {
    None,
    Fighter,
    State,
    Script,
    Variable,
    Object,
    ObjectScript,
    Subaction,
    Interrupt,
};

struct EditorDiagnosticTarget {
    EditorDiagnosticTargetKind kind = EditorDiagnosticTargetKind::None;
    int fighterIndex = -1;
    int stateIndex = -1;
    int scriptIndex = -1;
    int variableIndex = -1;
    int objectIndex = -1;
    int subactionIndex = -1;
    int interruptIndex = -1;
    int instructionIndex = -1;
    std::string label;
};

static EditorDiagnosticTarget editorDiagnosticTarget(
    const pf::FighterEditor& editor,
    const pf::FighterEditorSession& session,
    const pf::FighterDefinition& def,
    const pf::FighterState& state,
    const std::string& message)
{
    EditorDiagnosticTarget target;
    if (message.empty() || message == "OK") {
        return target;
    }
    for (int fighterIndex = 0; fighterIndex < static_cast<int>(session.package.fighters.size()); ++fighterIndex) {
        const pf::FighterDefinition& fighter = session.package.fighters[static_cast<size_t>(fighterIndex)];
        if (!fighter.name.empty() && containsCaseInsensitive(message, fighter.name)) {
            target.kind = EditorDiagnosticTargetKind::Fighter;
            target.fighterIndex = fighterIndex;
            target.label = "fighter #" + std::to_string(fighterIndex) + " " + fighter.name;
            return target;
        }
    }
    for (int stateIndex = 0; stateIndex < static_cast<int>(def.states.size()); ++stateIndex) {
        const pf::FighterState& candidate = def.states[static_cast<size_t>(stateIndex)];
        if (!candidate.name.empty() && containsCaseInsensitive(message, candidate.name)) {
            target.kind = EditorDiagnosticTargetKind::State;
            target.stateIndex = stateIndex;
            target.label = "state #" + std::to_string(stateIndex) + " " + candidate.name;
            return target;
        }
    }
    for (int scriptIndex = 0; scriptIndex < static_cast<int>(def.packageScripts.size()); ++scriptIndex) {
        const pf::PackageScript& script = def.packageScripts[static_cast<size_t>(scriptIndex)];
        if (!script.name.empty() && containsCaseInsensitive(message, script.name)) {
            target.kind = EditorDiagnosticTargetKind::Script;
            target.scriptIndex = scriptIndex;
            target.label = "script #" + std::to_string(scriptIndex) + " " + script.name;
            return target;
        }
    }
    for (int variableIndex = 0; variableIndex < static_cast<int>(def.packageVariables.size()); ++variableIndex) {
        const pf::PackageVariableDefinition& variable = def.packageVariables[static_cast<size_t>(variableIndex)];
        if (!variable.name.empty() && containsCaseInsensitive(message, variable.name)) {
            target.kind = EditorDiagnosticTargetKind::Variable;
            target.variableIndex = variableIndex;
            target.label = "variable #" + std::to_string(variableIndex) + " " + variable.name;
            return target;
        }
    }
    for (int objectIndex = 0; objectIndex < static_cast<int>(session.package.objects.size()); ++objectIndex) {
        const pf::GameObjectDefinition& object = session.package.objects[static_cast<size_t>(objectIndex)];
        if (!object.name.empty() && containsCaseInsensitive(message, object.name)) {
            target.kind = EditorDiagnosticTargetKind::Object;
            target.objectIndex = objectIndex;
            target.label = "object #" + std::to_string(objectIndex) + " " + object.name;
            return target;
        }
        for (int scriptIndex = 0; scriptIndex < static_cast<int>(object.packageScripts.size()); ++scriptIndex) {
            const pf::PackageScript& script = object.packageScripts[static_cast<size_t>(scriptIndex)];
            if (!script.name.empty() && containsCaseInsensitive(message, script.name)) {
                target.kind = EditorDiagnosticTargetKind::ObjectScript;
                target.objectIndex = objectIndex;
                target.scriptIndex = scriptIndex;
                target.label = "object #" + std::to_string(objectIndex) + " " + object.name +
                    " script #" + std::to_string(scriptIndex) + " " + script.name;
                return target;
            }
        }
    }

    const std::string lowerMessage = lowerAscii(message);
    if ((lowerMessage.find("object script") != std::string::npos ||
         lowerMessage.find("object graph") != std::string::npos) &&
        editor.selectedObjectDef >= 0 &&
        editor.selectedObjectDef < static_cast<int>(session.package.objects.size()))
    {
        target.kind = EditorDiagnosticTargetKind::ObjectScript;
        target.objectIndex = editor.selectedObjectDef;
        const pf::GameObjectDefinition& object = session.package.objects[static_cast<size_t>(editor.selectedObjectDef)];
        if (editor.selectedPackageScript >= 0 && editor.selectedPackageScript < static_cast<int>(object.packageScripts.size())) {
            target.scriptIndex = editor.selectedPackageScript;
        }
        target.instructionIndex = editor.selectedPackageInstruction;
        target.label = "selected object #" + std::to_string(target.objectIndex) + " " + object.name;
        if (target.scriptIndex >= 0) {
            target.label += " script #" + std::to_string(target.scriptIndex) + " " +
                object.packageScripts[static_cast<size_t>(target.scriptIndex)].name;
        }
        return target;
    }
    if (lowerMessage.find("script") != std::string::npos &&
        editor.selectedPackageScript >= 0 &&
        editor.selectedPackageScript < static_cast<int>(def.packageScripts.size()))
    {
        const pf::PackageScript& script = def.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
        target.kind = EditorDiagnosticTargetKind::Script;
        target.scriptIndex = editor.selectedPackageScript;
        target.instructionIndex = editor.selectedPackageInstruction;
        target.label = "selected script #" + std::to_string(target.scriptIndex) + " " + script.name;
        return target;
    }
    if ((lowerMessage.find("subaction") != std::string::npos ||
         lowerMessage.find("hitbox") != std::string::npos ||
         lowerMessage.find("hurtbox") != std::string::npos) &&
        editor.selectedSubaction >= 0 &&
        editor.selectedSubaction < static_cast<int>(state.action.size()))
    {
        target.kind = EditorDiagnosticTargetKind::Subaction;
        target.subactionIndex = editor.selectedSubaction;
        target.label = "selected timeline subaction #" + std::to_string(target.subactionIndex) +
            " " + subactionTypeName(state.action[static_cast<size_t>(target.subactionIndex)].type);
        return target;
    }
    if (lowerMessage.find("interrupt") != std::string::npos &&
        editor.selectedInterrupt >= 0 &&
        editor.selectedInterrupt < static_cast<int>(state.interrupts.size()))
    {
        target.kind = EditorDiagnosticTargetKind::Interrupt;
        target.interruptIndex = editor.selectedInterrupt;
        const pf::InterruptRule& interrupt = state.interrupts[static_cast<size_t>(target.interruptIndex)];
        target.label = "selected interrupt #" + std::to_string(target.interruptIndex) + " " +
            interruptConditionName(interrupt.condition) + " -> " + interrupt.targetState;
        return target;
    }
    if (lowerMessage.find("state") != std::string::npos) {
        target.kind = EditorDiagnosticTargetKind::State;
        target.stateIndex = session.selectedState;
        target.label = "selected state #" + std::to_string(session.selectedState) + " " + state.name;
        return target;
    }
    return target;
}

static bool focusEditorDiagnosticTarget(
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    const EditorDiagnosticTarget& target)
{
    if (target.kind == EditorDiagnosticTargetKind::None) {
        return false;
    }
    switch (target.kind) {
    case EditorDiagnosticTargetKind::Fighter:
        session.selectedFighter = target.fighterIndex;
        resetEditorSelectionForPackageFighter(editor, session);
        editor.workspace = pf::EditorWorkspace::Moveset;
        break;
    case EditorDiagnosticTargetKind::State:
        session.selectedState = target.stateIndex;
        editor.selectionKind = pf::FighterEditorSelectionKind::State;
        editor.workspace = pf::EditorWorkspace::Moveset;
        break;
    case EditorDiagnosticTargetKind::Script:
        editor.selectedPackageScript = target.scriptIndex;
        editor.selectedPackageInstruction = std::max(0, target.instructionIndex);
        editor.selectionKind = pf::FighterEditorSelectionKind::Script;
        editor.workspace = pf::EditorWorkspace::Logic;
        session.selectedPackageScript = editor.selectedPackageScript;
        session.selectedPackageInstruction = editor.selectedPackageInstruction;
        break;
    case EditorDiagnosticTargetKind::Variable:
        editor.selectedPackageVariable = target.variableIndex;
        editor.selectionKind = pf::FighterEditorSelectionKind::Variable;
        editor.workspace = pf::EditorWorkspace::Logic;
        break;
    case EditorDiagnosticTargetKind::Object:
        editor.selectedObjectDef = target.objectIndex;
        editor.selectionKind = pf::FighterEditorSelectionKind::Object;
        editor.workspace = pf::EditorWorkspace::Assets;
        editor.objectPanel = pf::ObjectEditorPanel::Logic;
        break;
    case EditorDiagnosticTargetKind::ObjectScript:
        editor.selectedObjectDef = target.objectIndex;
        editor.selectedPackageScript = target.scriptIndex;
        editor.selectedPackageInstruction = std::max(0, target.instructionIndex);
        editor.selectionKind = pf::FighterEditorSelectionKind::Script;
        editor.workspace = pf::EditorWorkspace::Assets;
        editor.objectPanel = pf::ObjectEditorPanel::Logic;
        session.selectedPackageScript = editor.selectedPackageScript;
        session.selectedPackageInstruction = editor.selectedPackageInstruction;
        break;
    case EditorDiagnosticTargetKind::Subaction:
        editor.selectedSubaction = target.subactionIndex;
        session.selectedSubaction = target.subactionIndex;
        editor.selectionKind = pf::FighterEditorSelectionKind::Subaction;
        editor.workspace = pf::EditorWorkspace::Moveset;
        break;
    case EditorDiagnosticTargetKind::Interrupt:
        editor.selectedInterrupt = target.interruptIndex;
        session.selectedInterrupt = target.interruptIndex;
        editor.selectionKind = pf::FighterEditorSelectionKind::Interrupt;
        editor.workspace = pf::EditorWorkspace::Moveset;
        break;
    case EditorDiagnosticTargetKind::None:
        return false;
    }
    session.clamp();
    syncEditorSelectionFromSession(editor, session);
    if (target.kind == EditorDiagnosticTargetKind::ObjectScript) {
        editor.selectedObjectDef = target.objectIndex;
        editor.selectedPackageScript = target.scriptIndex;
        editor.selectedPackageInstruction = std::max(0, target.instructionIndex);
        editor.selectionKind = pf::FighterEditorSelectionKind::Script;
        editor.workspace = pf::EditorWorkspace::Assets;
        editor.objectPanel = pf::ObjectEditorPanel::Logic;
    } else if (target.kind == EditorDiagnosticTargetKind::Object) {
        editor.selectedObjectDef = target.objectIndex;
        editor.selectionKind = pf::FighterEditorSelectionKind::Object;
        editor.workspace = pf::EditorWorkspace::Assets;
        editor.objectPanel = pf::ObjectEditorPanel::Logic;
    } else if (target.kind == EditorDiagnosticTargetKind::Variable) {
        editor.selectedPackageVariable = target.variableIndex;
        editor.selectionKind = pf::FighterEditorSelectionKind::Variable;
        editor.workspace = pf::EditorWorkspace::Logic;
    }
    editor.uiRefreshPending = true;
    editor.previewCacheDirty = true;
    editor.status = "Editor: focused diagnostic " + target.label;
    return true;
}

static std::string editorDiagnosticContextHint(
    const pf::FighterEditor& editor,
    const pf::FighterEditorSession& session,
    const pf::FighterDefinition& def,
    const pf::FighterState& state,
    const std::string& message)
{
    if (message.empty() || message == "OK") {
        return {};
    }
    for (int fighterIndex = 0; fighterIndex < static_cast<int>(session.package.fighters.size()); ++fighterIndex) {
        const pf::FighterDefinition& fighter = session.package.fighters[static_cast<size_t>(fighterIndex)];
        if (!fighter.name.empty() && containsCaseInsensitive(message, fighter.name)) {
            return "Context hint: fighter #" + std::to_string(fighterIndex) + " " + fighter.name;
        }
    }
    for (int stateIndex = 0; stateIndex < static_cast<int>(def.states.size()); ++stateIndex) {
        const pf::FighterState& candidate = def.states[static_cast<size_t>(stateIndex)];
        if (!candidate.name.empty() && containsCaseInsensitive(message, candidate.name)) {
            return "Context hint: state #" + std::to_string(stateIndex) + " " + candidate.name;
        }
    }
    for (int scriptIndex = 0; scriptIndex < static_cast<int>(def.packageScripts.size()); ++scriptIndex) {
        const pf::PackageScript& script = def.packageScripts[static_cast<size_t>(scriptIndex)];
        if (!script.name.empty() && containsCaseInsensitive(message, script.name)) {
            return "Context hint: script #" + std::to_string(scriptIndex) + " " + script.name;
        }
    }
    for (int variableIndex = 0; variableIndex < static_cast<int>(def.packageVariables.size()); ++variableIndex) {
        const pf::PackageVariableDefinition& variable = def.packageVariables[static_cast<size_t>(variableIndex)];
        if (!variable.name.empty() && containsCaseInsensitive(message, variable.name)) {
            return "Context hint: variable #" + std::to_string(variableIndex) + " " + variable.name;
        }
    }
    for (int objectIndex = 0; objectIndex < static_cast<int>(session.package.objects.size()); ++objectIndex) {
        const pf::GameObjectDefinition& object = session.package.objects[static_cast<size_t>(objectIndex)];
        if (!object.name.empty() && containsCaseInsensitive(message, object.name)) {
            return "Context hint: object #" + std::to_string(objectIndex) + " " + object.name;
        }
        for (int scriptIndex = 0; scriptIndex < static_cast<int>(object.packageScripts.size()); ++scriptIndex) {
            const pf::PackageScript& script = object.packageScripts[static_cast<size_t>(scriptIndex)];
            if (!script.name.empty() && containsCaseInsensitive(message, script.name)) {
                return "Context hint: object #" + std::to_string(objectIndex) + " " + object.name +
                    " script #" + std::to_string(scriptIndex) + " " + script.name;
            }
        }
    }

    const std::string lowerMessage = lowerAscii(message);
    const bool selectedObjectValid =
        editor.selectedObjectDef >= 0 &&
        editor.selectedObjectDef < static_cast<int>(session.package.objects.size());
    if ((lowerMessage.find("object script") != std::string::npos ||
         lowerMessage.find("object graph") != std::string::npos) &&
        selectedObjectValid)
    {
        const pf::GameObjectDefinition& object = session.package.objects[static_cast<size_t>(editor.selectedObjectDef)];
        std::string hint = "Context hint: selected object #" + std::to_string(editor.selectedObjectDef) + " " + object.name;
        if (editor.selectedPackageScript >= 0 && editor.selectedPackageScript < static_cast<int>(object.packageScripts.size())) {
            const pf::PackageScript& script = object.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
            hint += " script #" + std::to_string(editor.selectedPackageScript) + " " + script.name +
                " nodes=" + std::to_string(script.graph.nodes.size());
            if (editor.selectedPackageInstruction >= 0 && editor.selectedPackageInstruction < static_cast<int>(script.instructions.size())) {
                hint += " instruction #" + std::to_string(editor.selectedPackageInstruction) + " " +
                    packageInstructionLabel(script.instructions[static_cast<size_t>(editor.selectedPackageInstruction)]);
            }
        }
        return hint;
    }
    if (lowerMessage.find("script graph") != std::string::npos &&
        editor.selectedPackageScript >= 0 &&
        editor.selectedPackageScript < static_cast<int>(def.packageScripts.size()))
    {
        const pf::PackageScript& script = def.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
        return "Context hint: selected script graph #" + std::to_string(editor.selectedPackageScript) +
            " " + script.name + " nodes=" + std::to_string(script.graph.nodes.size());
    }
    if (lowerMessage.find("script") != std::string::npos &&
        editor.selectedPackageScript >= 0 &&
        editor.selectedPackageScript < static_cast<int>(def.packageScripts.size()))
    {
        const pf::PackageScript& script = def.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
        std::string hint = "Context hint: selected script #" + std::to_string(editor.selectedPackageScript) + " " + script.name;
        if (editor.selectedPackageInstruction >= 0 && editor.selectedPackageInstruction < static_cast<int>(script.instructions.size())) {
            hint += " instruction #" + std::to_string(editor.selectedPackageInstruction) + " " +
                packageInstructionLabel(script.instructions[static_cast<size_t>(editor.selectedPackageInstruction)]);
        }
        return hint;
    }
    if ((lowerMessage.find("subaction") != std::string::npos ||
         lowerMessage.find("hitbox") != std::string::npos ||
         lowerMessage.find("hurtbox") != std::string::npos) &&
        editor.selectedSubaction >= 0 &&
        editor.selectedSubaction < static_cast<int>(state.action.size()))
    {
        const pf::Subaction& subaction = state.action[static_cast<size_t>(editor.selectedSubaction)];
        return "Context hint: selected timeline subaction #" + std::to_string(editor.selectedSubaction) +
            " " + subactionTypeName(subaction.type);
    }
    if (lowerMessage.find("interrupt") != std::string::npos &&
        editor.selectedInterrupt >= 0 &&
        editor.selectedInterrupt < static_cast<int>(state.interrupts.size()))
    {
        const pf::InterruptRule& interrupt = state.interrupts[static_cast<size_t>(editor.selectedInterrupt)];
        return "Context hint: selected interrupt #" + std::to_string(editor.selectedInterrupt) +
            " " + interruptConditionName(interrupt.condition) + " -> " + interrupt.targetState;
    }
    if (lowerMessage.find("state") != std::string::npos) {
        return "Context hint: selected state #" + std::to_string(session.selectedState) + " " + state.name;
    }
    if (lowerMessage.find("object") != std::string::npos &&
        editor.selectedObjectDef >= 0 &&
        editor.selectedObjectDef < static_cast<int>(session.package.objects.size()))
    {
        const pf::GameObjectDefinition& object = session.package.objects[static_cast<size_t>(editor.selectedObjectDef)];
        return "Context hint: selected object #" + std::to_string(editor.selectedObjectDef) + " " + object.name;
    }
    if (lowerMessage.find("variable") != std::string::npos &&
        editor.selectedPackageVariable >= 0 &&
        editor.selectedPackageVariable < static_cast<int>(def.packageVariables.size()))
    {
        const pf::PackageVariableDefinition& variable = def.packageVariables[static_cast<size_t>(editor.selectedPackageVariable)];
        return "Context hint: selected variable #" + std::to_string(editor.selectedPackageVariable) + " " + variable.name;
    }
    return "Context hint: " + editorContextDetail(editor, session, def, state);
}

static void drawEditorDiagnosticsWorkstation(
    pf::World& world,
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    int& selectedFighterDef,
    const pf::FighterRuntime& fighter,
    const pf::FighterDefinition& def,
    const pf::FighterState& state,
    Rectangle rect)
{
    drawPanelChrome(rect, "Diagnostics");
    const float y = rect.y + 34.0f;
    DrawText(clippedText(editor.status, 12, rect.width * 0.58f).c_str(), static_cast<int>(rect.x + 10.0f), static_cast<int>(y), 12, RAYWHITE);
    DrawText((std::string("Mode: ") + (editor.testMode ? "Test" : "Edit") +
              "  Session: " + (session.dirty ? "dirty" : "clean") +
              "  Context: " + editorSelectionKindName(editor.selectionKind) +
              "  FighterDef " + std::to_string(fighter.fighterDef)).c_str(),
        static_cast<int>(rect.x + 10.0f),
        static_cast<int>(y + 22.0f),
        11,
        Fade(RAYWHITE, 0.66f));
    std::string committedPackagePath;
    const float pathX = rect.x + rect.width * 0.58f;
    const float pathW = std::max(92.0f, rect.x + rect.width - pathX - 404.0f);
    if (pathW >= 92.0f &&
        uiTextField({pathX, y + 18.0f, pathW, 22.0f}, "workstation-package-path", editor, editor.packagePath, committedPackagePath, 96))
    {
        editor.packagePath = committedPackagePath;
    }
    DrawText(clippedText("Selected: " + editorContextDetail(editor, session, def, state), 11, rect.width * 0.58f).c_str(),
        static_cast<int>(rect.x + 10.0f),
        static_cast<int>(y + 44.0f),
        11,
        Fade(RAYWHITE, 0.72f));
    Color graphDiagnosticColor = Fade(RAYWHITE, 0.62f);
    const std::string graphDiagnostic = editorSelectedGraphDiagnostic(editor, session, def, graphDiagnosticColor);
    DrawText(clippedText(graphDiagnostic, 11, rect.width * 0.42f).c_str(),
        static_cast<int>(rect.x + rect.width * 0.58f),
        static_cast<int>(y + 44.0f),
        11,
        graphDiagnosticColor);
    if (editor.lastPackageBytes > 0) {
        DrawText(("Last package " + editor.lastPackageName +
                  " bytes=" + std::to_string(editor.lastPackageBytes) +
                  " checksum=" + std::to_string(editor.lastPackageChecksum) +
                  " fighters=" + std::to_string(editor.lastPackageFighters) +
                  " objects=" + std::to_string(editor.lastPackageObjects)).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(y + 66.0f),
            11,
            editor.lastPackageValid ? GREEN : RED);
    } else {
        DrawText("No package validation has run this session", static_cast<int>(rect.x + 10.0f), static_cast<int>(y + 66.0f), 11, Fade(RAYWHITE, 0.58f));
    }
    if (!editor.lastPackageMessage.empty()) {
        DrawText(clippedText("Validation: " + editor.lastPackageMessage, 11, rect.width * 0.46f).c_str(),
            static_cast<int>(rect.x + rect.width * 0.52f),
            static_cast<int>(y + 66.0f),
            11,
            editor.lastPackageValid ? GREEN : RED);
    }
    const std::string diagnosticMessage = editor.lastPackageValid ? graphDiagnostic : editor.lastPackageMessage;
    const EditorDiagnosticTarget diagnosticTarget = editorDiagnosticTarget(editor, session, def, state, diagnosticMessage);
    const std::string contextHint = editorDiagnosticContextHint(editor, session, def, state, diagnosticMessage);
    if (!contextHint.empty()) {
        DrawText(clippedText(contextHint, 11, rect.width - 20.0f).c_str(),
            static_cast<int>(rect.x + 10.0f),
            static_cast<int>(y + 88.0f),
            11,
            Fade(YELLOW, 0.82f));
    }
    if (diagnosticTarget.kind != EditorDiagnosticTargetKind::None) {
        const Rectangle focusButton{rect.x + rect.width - 486.0f, y - 4.0f, 86.0f, 24.0f};
        if (uiButton(focusButton, "Focus")) {
            focusEditorDiagnosticTarget(editor, session, diagnosticTarget);
            return;
        }
    }
    const Rectangle validateButton{rect.x + rect.width - 390.0f, y - 4.0f, 86.0f, 24.0f};
    if (uiButton(validateButton, "Validate")) {
        std::string error;
        const bool wasDirty = session.dirty;
        pf::FighterEditorPackageSnapshot snapshot;
        if (pf::exportFighterEditorSessionPackage(session, snapshot, &error)) {
            session.dirty = wasDirty;
            updateEditorPackageSummary(editor, snapshot.descriptor);
            editor.status = "Editor: session package validates bytes=" + std::to_string(snapshot.bytes.size());
        } else {
            session.dirty = wasDirty;
            updateEditorPackageFailure(editor, error);
            editor.status = "Editor session validation failed: " + error;
        }
    }
    if (uiButton({rect.x + rect.width - 294.0f, y - 4.0f, 86.0f, 24.0f}, "Save")) {
        std::string error;
        pf::FighterEditorPackageSnapshot snapshot;
        if (!pf::exportFighterEditorSessionPackage(session, snapshot, &error)) {
            updateEditorPackageFailure(editor, error);
            editor.status = "Editor package save failed: " + error;
        } else if (!writeEditorPackageBytesToFile(editor.packagePath, snapshot.bytes, &error)) {
            updateEditorPackageFailure(editor, error);
            editor.status = "Editor package save failed: " + error;
        } else {
            updateEditorPackageSummary(editor, snapshot.descriptor);
            editor.status = "Editor: saved " + editor.packagePath +
                " bytes=" + std::to_string(snapshot.bytes.size()) +
                " checksum=" + std::to_string(snapshot.descriptor.checksum);
        }
    }
    if (uiButton({rect.x + rect.width - 198.0f, y - 4.0f, 86.0f, 24.0f}, "Load")) {
        std::string error;
        std::vector<uint8_t> bytes;
        if (!readEditorPackageBytesFromFile(editor.packagePath, bytes, &error)) {
            updateEditorPackageFailure(editor, error);
            editor.status = "Editor package load failed: " + error;
        } else if (!pf::loadFighterEditorSessionPackage(bytes, session, &error)) {
            updateEditorPackageFailure(editor, error);
            editor.status = "Editor package load failed: " + error;
        } else if (!syncEditorSessionToWorld(world, editor, session, selectedFighterDef, &error)) {
            updateEditorPackageFailure(editor, error);
            editor.status = "Editor package load sync failed: " + error;
        } else {
            editor.uiRefreshPending = true;
            editor.previewCacheDirty = true;
            updateEditorPackageSummary(editor, session.lastDescriptor);
            editor.selectionKind = pf::FighterEditorSelectionKind::State;
            editor.status = "Editor: loaded " + editor.packagePath +
                " fighters=" + std::to_string(session.lastDescriptor.fighterNames.size()) +
                " objects=" + std::to_string(session.lastDescriptor.objectNames.size());
        }
    }
    if (uiButton({rect.x + rect.width - 102.0f, y - 4.0f, 86.0f, 24.0f}, "Reset Msg")) {
        editor.status = "Editor: ready";
    }
}

static void drawEditorWorkstation(
    pf::World& world,
    pf::FighterEditor& editor,
    pf::FighterEditorSession& session,
    bool& sessionActive,
    int& selectedFighterDef)
{
    editor.clampToWorld(world);
    if (world.fighters.empty()) {
        return;
    }
    pf::FighterRuntime& fighter = world.fighters[static_cast<size_t>(editor.selectedFighter)];
    if (fighter.fighterDef < 0 || fighter.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
        return;
    }
    std::string sessionError;
    if (!ensureEditorSessionFromWorld(world, editor, session, sessionActive, fighter.fighterDef, &sessionError)) {
        editor.status = "Editor session failed: " + sessionError;
        return;
    }
    pf::FighterDefinition* sessionDef = selectedEditorSessionFighter(session);
    if (!sessionDef) {
        editor.status = "Editor session has no selected fighter";
        return;
    }
    pf::FighterDefinition& def = *sessionDef;
    if (def.states.empty()) {
        return;
    }
    session.clamp();
    syncEditorSelectionFromSession(editor, session);
    pf::FighterState& state = def.states[static_cast<size_t>(session.selectedState)];
    const EditorWorkstationLayout layout = editorWorkstationLayout();
    editor.uiRefreshPending = false;

    DrawRectangleRec(layout.viewport, Fade(BLACK, 0.08f));
    DrawRectangleLinesEx(layout.viewport, 1.0f, Fade(SKYBLUE, 0.45f));
    DrawText(("Viewport: " + def.name + " / " + state.name).c_str(),
        static_cast<int>(layout.viewport.x + 12.0f),
        static_cast<int>(layout.viewport.y + 10.0f),
        13,
        RAYWHITE);
    DrawText((std::string("Overlays: ") + (editor.showBoxes ? "debug capsules, ECB, bones" : "model only") +
              "  Camera: " + (editor.sideView ? "side" : "perspective")).c_str(),
        static_cast<int>(layout.viewport.x + 12.0f),
        static_cast<int>(layout.viewport.y + 30.0f),
        11,
        Fade(RAYWHITE, 0.7f));
    drawEditorViewportOverlayControls(editor, layout.viewport);

    drawEditorToolStrip(world, editor, session, selectedFighterDef, layout.toolStrip);
    if (editor.uiRefreshPending) return;
    drawEditorStateBrowserWorkstation(world, editor, session, selectedFighterDef, def, layout.leftBrowser);
    if (editor.uiRefreshPending) return;
    drawEditorTimelineWorkstation(world, editor, session, selectedFighterDef, fighter, def, state, layout.timeline);
    if (editor.uiRefreshPending) return;
    if (editor.workspace == pf::EditorWorkspace::Assets) {
        drawEditorObjectWorkstation(world, editor, session, selectedFighterDef, layout.rightGraph);
    } else if (editor.workspace == pf::EditorWorkspace::Animation) {
        drawEditorAnimationWorkstation(world, editor, session, selectedFighterDef, def, layout.rightGraph);
    } else {
        drawEditorLogicGraphWorkstation(world, editor, session, selectedFighterDef, def, layout.rightGraph);
    }
    if (editor.uiRefreshPending) return;
    drawEditorInspectorWorkstation(world, editor, session, selectedFighterDef, def, state, layout.rightInspector);
    if (editor.uiRefreshPending) return;
    drawEditorDiagnosticsWorkstation(world, editor, session, selectedFighterDef, fighter, def, state, layout.diagnostics);
}

static void drawEditor(pf::World& world, pf::FighterEditor& editor, int& selectedFighterDef) {
    editor.clampToWorld(world);
    const pf::FighterRuntime& fighter = world.fighters[static_cast<size_t>(editor.selectedFighter)];
    pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    pf::FighterState& state = def.states[static_cast<size_t>(editor.selectedState)];
    const pf::UnfoldedAction actionFrames = pf::unfoldAction(state.action);
    const std::vector<int> subactionFrames = editorSubactionFirstFrames(state.action);
    const int timelineFrameCount = std::max(1, std::max(state.animationLengthFrames, static_cast<int>(actionFrames.size())));
    const int liveFrame = pf::currentState(world, fighter).name == state.name ? pf::frameInState(fighter) : 0;
    const float timelineX = 24.0f;
    const float timelineY = 202.0f;
    const float timelineWidth = 500.0f;
    const float timelineHeight = 34.0f;
    const int selectedSubaction = state.action.empty()
        ? -1
        : std::clamp(editor.selectedSubaction, 0, static_cast<int>(state.action.size()) - 1);

    DrawRectangle(12, 12, 530, 322, Fade(RAYWHITE, 0.5f));
    DrawRectangleLines(12, 12, 530, 322, DARKGRAY);
    DrawText("PFighter C++ prototype editor", 24, 24, 18, BLACK);
    const Rectangle newStateButton = editorNewStateButtonRect();
    DrawRectangleRec(newStateButton, SKYBLUE);
    DrawRectangleLinesEx(newStateButton, 1.0f, DARKGRAY);
    DrawText("New", static_cast<int>(newStateButton.x + 14.0f), static_cast<int>(newStateButton.y + 7.0f), 14, BLACK);
    const Rectangle deleteStateButton = editorDeleteStateButtonRect();
    DrawRectangleRec(deleteStateButton, Fade(RED, 0.75f));
    DrawRectangleLinesEx(deleteStateButton, 1.0f, DARKGRAY);
    DrawText("Del", static_cast<int>(deleteStateButton.x + 17.0f), static_cast<int>(deleteStateButton.y + 7.0f), 14, BLACK);
    const Rectangle testButton = editorTestButtonRect();
    DrawRectangleRec(testButton, editor.testMode ? GREEN : ORANGE);
    DrawRectangleLinesEx(testButton, 1.0f, DARKGRAY);
    DrawText("Test", static_cast<int>(testButton.x + 18.0f), static_cast<int>(testButton.y + 7.0f), 14, BLACK);
    DrawText("Fighter:", 24, 54, 16, DARKGRAY);
    std::string renamedFighter;
    if (uiTextField(
            {88.0f, 50.0f, 210.0f, 24.0f},
            "fighter-def-" + std::to_string(fighter.fighterDef),
            editor,
            def.name,
            renamedFighter))
    {
        if (!fighterNameAvailable(world, renamedFighter, fighter.fighterDef)) {
            editor.status = "Editor: fighter name is empty or already used";
        } else {
            const std::string oldName = def.name;
            def.name = renamedFighter;
            remapPackageFighterTargets(world, oldName, def.name);
            editor.status = "Editor: renamed fighter " + oldName + " to " + def.name;
        }
    }
    DrawText(("Live state: " + pf::currentState(world, fighter).name).c_str(), 24, 76, 16, DARKGRAY);
    DrawText("Selected state:", 24, 98, 16, DARKGRAY);
    std::string renamedState;
    if (uiTextField(
            {142.0f, 94.0f, 190.0f, 24.0f},
            "fighter-state-" + std::to_string(fighter.fighterDef) + "-" + std::to_string(editor.selectedState),
            editor,
            state.name,
            renamedState))
    {
        if (!fighterStateNameAvailable(def, renamedState, editor.selectedState)) {
            editor.status = "Editor: fighter state name is empty or already used";
        } else {
            const std::string oldName = state.name;
            state.name = renamedState;
            remapFighterStateTargets(def, oldName, state.name);
            editor.status = "Editor: renamed fighter state " + oldName + " to " + state.name;
        }
    }
    DrawText(("Frame in state: " + std::to_string(pf::frameInState(fighter))).c_str(), 24, 120, 16, DARKGRAY);
    DrawText(("Frames: " + std::to_string(state.animationLengthFrames) +
              "  Subactions: " + std::to_string(state.action.size()) +
              "  Interrupts: " + std::to_string(state.interrupts.size())).c_str(), 24, 142, 16, DARKGRAY);
    if (selectedSubaction >= 0) {
        const pf::Subaction& subaction = state.action[static_cast<size_t>(selectedSubaction)];
        DrawText(("Selected subaction: " + std::to_string(selectedSubaction) + " " +
                  subactionTypeName(subaction.type)).c_str(), 24, 164, 14, DARKGRAY);
    } else {
        DrawText("Selected subaction: none", 24, 164, 14, DARKGRAY);
    }
    DrawText(("Animation pose: " + std::to_string(fighter.jointWorldPositions.size()) + " joints, " +
              std::to_string(fighter.poseHurtboxCapsules.size()) + " hurtboxes").c_str(), 24, 182, 14, DARKGRAY);

    DrawRectangle(static_cast<int>(timelineX), static_cast<int>(timelineY), static_cast<int>(timelineWidth), static_cast<int>(timelineHeight), Fade(LIGHTGRAY, 0.65f));
    DrawRectangleLines(static_cast<int>(timelineX), static_cast<int>(timelineY), static_cast<int>(timelineWidth), static_cast<int>(timelineHeight), DARKGRAY);
    for (int frame = 0; frame < static_cast<int>(actionFrames.size()); ++frame) {
        const std::vector<pf::Subaction>& frameActions = actionFrames[static_cast<size_t>(frame)];
        if (frameActions.empty()) {
            continue;
        }
        const float x = timelineX + timelineWidth * static_cast<float>(frame) / static_cast<float>(timelineFrameCount);
        Color color = ORANGE;
        for (const pf::Subaction& subaction : frameActions) {
            color = subactionMarkerColor(subaction);
            if (subaction.type == pf::SubactionType::CreateHitbox || subaction.type == pf::SubactionType::CreateThrowHitbox) {
                break;
            }
        }
        DrawRectangle(static_cast<int>(x), static_cast<int>(timelineY + 4.0f), 2, static_cast<int>(timelineHeight - 8.0f), color);
    }
    if (state.initialInterruptibleFrame > 0 && state.initialInterruptibleFrame <= timelineFrameCount) {
        const float x = timelineX + timelineWidth * static_cast<float>(state.initialInterruptibleFrame) / static_cast<float>(timelineFrameCount);
        DrawRectangle(static_cast<int>(x), static_cast<int>(timelineY), 2, static_cast<int>(timelineHeight), GREEN);
    }
    if (selectedSubaction >= 0 && selectedSubaction < static_cast<int>(subactionFrames.size())) {
        const int subactionFrame = subactionFrames[static_cast<size_t>(selectedSubaction)];
        if (subactionFrame >= 0) {
            const float x = timelineX + timelineWidth * static_cast<float>(std::clamp(subactionFrame, 0, timelineFrameCount)) /
                static_cast<float>(timelineFrameCount);
            DrawRectangle(static_cast<int>(x - 2.0f), static_cast<int>(timelineY - 5.0f), 5, static_cast<int>(timelineHeight + 10.0f), BLUE);
        }
    }
    const float liveX = timelineX + timelineWidth * static_cast<float>(std::clamp(liveFrame, 0, timelineFrameCount)) / static_cast<float>(timelineFrameCount);
    DrawRectangle(static_cast<int>(liveX), static_cast<int>(timelineY - 3.0f), 3, static_cast<int>(timelineHeight + 6.0f), BLACK);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        CheckCollisionPointRec(GetMousePosition(), {timelineX, timelineY, timelineWidth, timelineHeight}) &&
        !state.action.empty())
    {
        const float clickT = std::clamp((GetMousePosition().x - timelineX) / timelineWidth, 0.0f, 1.0f);
        const int clickedFrame = static_cast<int>(std::round(clickT * static_cast<float>(timelineFrameCount)));
        int bestSubaction = -1;
        int bestDistance = 1000000;
        for (size_t i = 0; i < subactionFrames.size(); ++i) {
            const int frame = subactionFrames[i];
            if (frame < 0) {
                continue;
            }
            const int distance = std::abs(frame - clickedFrame);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestSubaction = static_cast<int>(i);
            }
        }
        if (bestSubaction >= 0) {
            editor.selectedSubaction = bestSubaction;
            editor.status = "Editor: selected timeline subaction " + std::to_string(bestSubaction);
        }
    }
    DrawText(editor.status.c_str(), 24, 240, 14, DARKGRAY);
    DrawText("N/New state  Del/remove  T/Test playtest  [/] state  ,/. subaction  Space pause  R reset", 24, 258, 14, GRAY);
    drawEditorStateBrowser(world, def, editor);
    drawEditorWorkspaceTabs(editor);
    if (editor.workspace == pf::EditorWorkspace::Moveset) {
        drawEditorMovesetWorkspace(world, editor);
    } else if (editor.workspace == pf::EditorWorkspace::Logic) {
        drawEditorLogicWorkspace(world, editor);
    } else if (editor.workspace == pf::EditorWorkspace::Assets) {
        drawEditorAssetsWorkspace(world, editor, selectedFighterDef);
    } else if (editor.workspace == pf::EditorWorkspace::Animation) {
        drawEditorAnimationWorkspace(world, editor);
    } else if (editor.workspace == pf::EditorWorkspace::TestLab) {
        drawEditorTestLabWorkspace(world, editor);
    } else if (editor.workspace != pf::EditorWorkspace::Moveset) {
        DrawText((std::string(workspaceName(editor.workspace)) + " workspace shell").c_str(), 24, 324, 14, DARKGRAY);
    }
}

struct ReplayHarness {
    std::string path = "replay_last.pfreplay";
    std::string status = "Replay: F5 record  F6 save  F7 load  F8 step  F9 play";
    pf::ReplayData recording;
    pf::ReplayData playback;
    size_t playbackFrame = 0;
    bool recordingActive = false;
    bool playbackLoaded = false;
    bool realtimePlayback = false;
};

struct TickrateControl {
    int ticksPerSecond = 60;
    float accumulator = 0.0f;
    bool dragging = false;
};

static pf::World makeReplayStartWorld(int p1FighterDef, int p2FighterDef) {
    return pf::makeTrainingWorld(p1FighterDef, p2FighterDef);
}

static int fighterDefByName(const pf::World& world, const std::string& name, int fallback) {
    for (size_t i = 0; i < world.fighterDefs.size(); ++i) {
        if (world.fighterDefs[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return fallback;
}

static void beginReplayRecording(ReplayHarness& replay, pf::World& world, int p1FighterDef, int p2FighterDef) {
    replay.recording = {};
    replay.recording.p1FighterDef = p1FighterDef;
    replay.recording.p2FighterDef = p2FighterDef;
    replay.recordingActive = true;
    replay.playbackLoaded = false;
    replay.realtimePlayback = false;
    replay.playbackFrame = 0;
    world = makeReplayStartWorld(p1FighterDef, p2FighterDef);
    replay.status = "Recording replay from starting gamestate";
}

static void saveReplayRecording(ReplayHarness& replay) {
    if (!replay.recordingActive && replay.recording.frames.empty()) {
        replay.status = "No replay frames to save";
        return;
    }
    std::string error;
    if (pf::saveReplay(replay.path, replay.recording, &error)) {
        replay.status = "Saved " + replay.path + " (" + std::to_string(replay.recording.frames.size()) + " frames)";
    } else {
        replay.status = error;
    }
    replay.recordingActive = false;
}

static void loadReplayPlayback(ReplayHarness& replay, pf::World& world, int& selectedFighterDef) {
    std::string error;
    pf::ReplayData loaded;
    if (!pf::loadReplay(replay.path, loaded, &error)) {
        replay.status = error;
        return;
    }
    replay.playback = std::move(loaded);
    replay.playbackFrame = 0;
    replay.playbackLoaded = true;
    replay.realtimePlayback = false;
    replay.recordingActive = false;
    selectedFighterDef = replay.playback.p1FighterDef;
    world = makeReplayStartWorld(replay.playback.p1FighterDef, replay.playback.p2FighterDef);
    replay.status = "Loaded " + replay.path + " (" + std::to_string(replay.playback.frames.size()) + " frames)";
}

static void stepReplayPlayback(ReplayHarness& replay, pf::World& world) {
    if (!replay.playbackLoaded) {
        replay.status = "No replay loaded";
        return;
    }
    if (replay.playbackFrame >= replay.playback.frames.size()) {
        replay.realtimePlayback = false;
        replay.status = "Replay finished";
        return;
    }
    const pf::ReplayFrame& frame = replay.playback.frames[replay.playbackFrame++];
    pf::tickWorld(world, {frame.inputs[0], frame.inputs[1]});
    replay.status = "Replay frame " + std::to_string(replay.playbackFrame) + "/" +
        std::to_string(replay.playback.frames.size());
}

static void drawReplayStatus(const ReplayHarness& replay) {
    const int panelWidth = 390;
    const int panelX = GetScreenWidth() - panelWidth - 12;
    DrawRectangle(panelX, 12, panelWidth, 72, Fade(RAYWHITE, 0.5f));
    DrawRectangleLines(panelX, 12, panelWidth, 72, DARKGRAY);
    DrawText(replay.status.c_str(), panelX + 12, 24, 14, DARKGRAY);
    const std::string mode = replay.recordingActive ? "Mode: recording" :
        (replay.playbackLoaded ? (replay.realtimePlayback ? "Mode: replay realtime" : "Mode: replay paused") : "Mode: live");
    DrawText(mode.c_str(), panelX + 12, 46, 14, DARKGRAY);
    DrawText(("File: " + replay.path).c_str(), panelX + 12, 66, 14, GRAY);
}

static void updateTickrateControl(TickrateControl& tickrate) {
    constexpr int kMinTickrate = 5;
    constexpr int kMaxTickrate = 60;
    const float panelX = static_cast<float>(GetScreenWidth() - 390 - 12);
    const Rectangle panel{panelX, 96.0f, 390.0f, 58.0f};
    const Rectangle track{panelX + 96.0f, 132.0f, 260.0f, 8.0f};
    const Vector2 mouse = GetMousePosition();

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, panel)) {
        tickrate.dragging = true;
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        tickrate.dragging = false;
    }
    if (tickrate.dragging) {
        const float t = std::clamp((mouse.x - track.x) / track.width, 0.0f, 1.0f);
        tickrate.ticksPerSecond = kMinTickrate + static_cast<int>(std::round(t * static_cast<float>(kMaxTickrate - kMinTickrate)));
    }
}

static void drawTickrateControl(const TickrateControl& tickrate) {
    constexpr int kMinTickrate = 5;
    constexpr int kMaxTickrate = 60;
    const float panelX = static_cast<float>(GetScreenWidth() - 390 - 12);
    const Rectangle panel{panelX, 96.0f, 390.0f, 58.0f};
    const Rectangle track{panelX + 96.0f, 132.0f, 260.0f, 8.0f};
    const float t = static_cast<float>(tickrate.ticksPerSecond - kMinTickrate) /
        static_cast<float>(kMaxTickrate - kMinTickrate);
    const float knobX = track.x + track.width * t;

    DrawRectangleRec(panel, Fade(RAYWHITE, 0.5f));
    DrawRectangleLinesEx(panel, 1.0f, DARKGRAY);
    DrawText(("Sim tickrate: " + std::to_string(tickrate.ticksPerSecond) + " Hz").c_str(), static_cast<int>(panel.x + 12), 108, 14, DARKGRAY);
    DrawText("5", static_cast<int>(panel.x + 12), 128, 14, GRAY);
    DrawText("60", static_cast<int>(panel.x + 360), 128, 14, GRAY);
    DrawRectangleRec(track, Fade(GRAY, 0.45f));
    DrawRectangle(track.x, track.y, knobX - track.x, track.height, ORANGE);
    DrawCircle(static_cast<int>(std::round(knobX)), static_cast<int>(track.y + track.height * 0.5f), 8.0f, ORANGE);
    DrawCircleLines(static_cast<int>(std::round(knobX)), static_cast<int>(track.y + track.height * 0.5f), 8.0f, BROWN);
}

static std::string uniqueStateName(const pf::FighterDefinition& def, const std::string& baseName) {
    auto hasName = [&](const std::string& name) {
        return std::any_of(def.states.begin(), def.states.end(), [&](const pf::FighterState& state) {
            return state.name == name;
        });
    };
    if (!hasName(baseName)) {
        return baseName;
    }
    for (int index = 1; index < 10000; ++index) {
        const std::string candidate = baseName + std::to_string(index);
        if (!hasName(candidate)) {
            return candidate;
        }
    }
    return baseName + "X";
}

static void createEditorState(pf::World& world, pf::FighterEditor& editor) {
    editor.clampToWorld(world);
    if (world.fighters.empty()) {
        editor.status = "Editor: cannot create a state without a fighter";
        return;
    }
    const pf::FighterRuntime& selectedRuntime = world.fighters[static_cast<size_t>(editor.selectedFighter)];
    if (selectedRuntime.fighterDef < 0 || selectedRuntime.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
        editor.status = "Editor: cannot create a state for an invalid fighter definition";
        return;
    }
    pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(selectedRuntime.fighterDef)];
    pf::FighterState state;
    state.name = uniqueStateName(def, "NewState");
    state.animation = "Wait";
    state.animationLengthFrames = 60;
    state.loopAnimation = true;
    if (editor.selectedState >= 0 && editor.selectedState < static_cast<int>(def.states.size())) {
        const pf::FighterState& source = def.states[static_cast<size_t>(editor.selectedState)];
        state.animation = source.animation;
        state.animationActionIndex = source.animationActionIndex;
        state.animationLengthFrames = source.animationLengthFrames;
        state.loopAnimation = source.loopAnimation;
        state.defaultAnimationBlendFrames = source.defaultAnimationBlendFrames;
    }
    const int insertIndex = std::clamp(editor.selectedState + 1, 0, static_cast<int>(def.states.size()));
    def.states.insert(def.states.begin() + insertIndex, std::move(state));
    for (pf::FighterRuntime& runtime : world.fighters) {
        if (runtime.fighterDef == selectedRuntime.fighterDef && runtime.state >= insertIndex) {
            ++runtime.state;
        }
    }
    editor.selectedState = insertIndex;
    editor.selectedSubaction = 0;
    editor.status = "Editor: created unsaved state " + def.states[static_cast<size_t>(insertIndex)].name;
}

static void duplicateEditorState(pf::World& world, pf::FighterEditor& editor) {
    editor.clampToWorld(world);
    if (world.fighters.empty()) {
        editor.status = "Editor: cannot clone a state without a fighter";
        return;
    }
    const pf::FighterRuntime& selectedRuntime = world.fighters[static_cast<size_t>(editor.selectedFighter)];
    if (selectedRuntime.fighterDef < 0 || selectedRuntime.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
        editor.status = "Editor: cannot clone a state for an invalid fighter definition";
        return;
    }
    pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(selectedRuntime.fighterDef)];
    if (def.states.empty()) {
        editor.status = "Editor: cannot clone a state from an empty fighter definition";
        return;
    }
    const int sourceIndex = std::clamp(editor.selectedState, 0, static_cast<int>(def.states.size()) - 1);
    pf::FighterState clone = def.states[static_cast<size_t>(sourceIndex)];
    clone.name = uniqueStateName(def, clone.name + "Copy");
    const int insertIndex = std::clamp(sourceIndex + 1, 0, static_cast<int>(def.states.size()));
    def.states.insert(def.states.begin() + insertIndex, std::move(clone));
    for (pf::FighterRuntime& runtime : world.fighters) {
        if (runtime.fighterDef == selectedRuntime.fighterDef && runtime.state >= insertIndex) {
            ++runtime.state;
        }
    }
    editor.selectedState = insertIndex;
    editor.selectedSubaction = 0;
    editor.selectedInterrupt = 0;
    editor.status = "Editor: cloned unsaved state " + def.states[static_cast<size_t>(insertIndex)].name;
}

static void removeEditorState(pf::World& world, pf::FighterEditor& editor) {
    editor.clampToWorld(world);
    if (world.fighters.empty()) {
        editor.status = "Editor: cannot remove a state without a fighter";
        return;
    }
    const pf::FighterRuntime& selectedRuntime = world.fighters[static_cast<size_t>(editor.selectedFighter)];
    if (selectedRuntime.fighterDef < 0 || selectedRuntime.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
        editor.status = "Editor: cannot remove a state from an invalid fighter definition";
        return;
    }
    pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(selectedRuntime.fighterDef)];
    if (def.states.size() <= 1) {
        editor.status = "Editor: cannot remove the only state";
        return;
    }
    const int removeIndex = std::clamp(editor.selectedState, 0, static_cast<int>(def.states.size()) - 1);
    const std::string removedName = def.states[static_cast<size_t>(removeIndex)].name;
    def.states.erase(def.states.begin() + removeIndex);
    const int fallbackState = std::max(0, def.stateIndex("Wait"));
    for (pf::FighterRuntime& runtime : world.fighters) {
        if (runtime.fighterDef != selectedRuntime.fighterDef) {
            continue;
        }
        if (runtime.state == removeIndex) {
            runtime.state = std::min(fallbackState, static_cast<int>(def.states.size()) - 1);
            runtime.lastStateChangeFrame = world.frame;
            runtime.internalFrame = 0;
            runtime.animationFrame = 0;
            runtime.lastActionFrameExecuted = -1;
        } else if (runtime.state > removeIndex) {
            --runtime.state;
        }
    }
    editor.selectedState = std::clamp(removeIndex, 0, static_cast<int>(def.states.size()) - 1);
    editor.selectedSubaction = 0;
    const std::string replacementName = def.states[static_cast<size_t>(std::min(fallbackState, static_cast<int>(def.states.size()) - 1))].name;
    remapRemovedFighterStateTargets(def, removedName, replacementName);
    editor.status = "Editor: removed unsaved state " + removedName;
}

static void launchEditorTestWorld(
    pf::World& world,
    pf::FighterEditor& editor,
    ReplayHarness& replay,
    int& selectedFighterDef)
{
    editor.clampToWorld(world);
    const pf::FighterRuntime& selectedRuntime = world.fighters[static_cast<size_t>(editor.selectedFighter)];
    if (selectedRuntime.fighterDef < 0 || selectedRuntime.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
        editor.status = "Editor test failed: selected fighter definition is invalid";
        return;
    }

    const int editedFighterDef = selectedRuntime.fighterDef;
    pf::FighterEditorSession session;
    std::string packageError;
    if (!pf::beginFighterEditorSessionFromWorld(world, editedFighterDef, session, &packageError)) {
        updateEditorPackageFailure(editor, packageError);
        editor.status = "Editor test failed: session setup failed: " + packageError;
        return;
    }
    int testFighterIndex = -1;
    pf::FighterPackageDescriptor testDescriptor;
    if (!pf::makeFighterEditorSessionTestWorld(session, world, &testFighterIndex, &testDescriptor, &packageError)) {
        updateEditorPackageFailure(editor, packageError);
        editor.status = "Editor test failed: package test world failed: " + packageError;
        return;
    }
    updateEditorPackageSummary(editor, testDescriptor);
    pf::resetTrainingFighter(world, 0, testFighterIndex, {-pf::fx(2), 0}, 1);
    selectedFighterDef = testFighterIndex;
    editor.selectedFighter = 0;
    editor.testMode = true;
    editor.animationPreviewActive = false;
    editor.paused = false;
    replay.playbackLoaded = false;
    replay.realtimePlayback = false;
    replay.recordingActive = false;
    replay.playbackFrame = 0;
    editor.status = world.fighterDefs[static_cast<size_t>(world.fighters[1].fighterDef)].name == "Sandbag"
        ? "Editor test: package round-trip loaded on Battlefield with Sandbag checksum=" + std::to_string(editor.lastPackageChecksum)
        : "Editor test: package round-trip loaded on Battlefield; player 2 is inert checksum=" + std::to_string(editor.lastPackageChecksum);
}

int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 720, "PFighter C++ raylib prototype");
    if (!IsWindowReady()) {
        return 1;
    }
    SetWindowMinSize(960, 640);
    SetTargetFPS(60);

    Camera3D camera{};
    camera.position = {0.0f, 55.0f, 145.0f};
    camera.target = {0.0f, 10.0f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;
    float editorCameraYaw = 0.0f;
    float editorCameraPitch = 0.30f;
    float editorCameraDistance = 152.0f;
    float editorCameraOrthoSize = 120.0f;
    bool editorCameraOrbitDragging = false;

    AppMode appMode = AppMode::MainMenu;
    int testFighterDef = 0;
    pf::World world = pf::makeTrainingWorld(testFighterDef, testFighterDef);
    pf::FighterEditor editor;
    pf::FighterEditorSession editorSession;
    bool editorSessionActive = false;
    ReplayHarness replay;
    TickrateControl tickrate;
    const std::array<int, 7> fighterKeys{
        KEY_ONE, KEY_TWO, KEY_THREE, KEY_FOUR, KEY_FIVE, KEY_SIX, KEY_SEVEN,
    };
    auto selectFighterDef = [&](int fighterDef) {
        if (world.fighterDefs.empty()) {
            return;
        }
        const int fighterDefCount = static_cast<int>(world.fighterDefs.size());
        testFighterDef = (fighterDef % fighterDefCount + fighterDefCount) % fighterDefCount;
        world = pf::makeTrainingWorld(testFighterDef, testFighterDef);
        replay.playbackLoaded = false;
        replay.realtimePlayback = false;
        replay.recordingActive = false;
        editorSessionActive = false;
        editor.testMode = false;
        editor.animationPreviewActive = false;
        editor.status = "Editor: selected " + world.fighterDefs[static_cast<size_t>(testFighterDef)].name;
        editor.selectedState = 0;
        editor.selectedSubaction = 0;
    };
    auto selectedEditorState = [&]() -> pf::FighterState* {
        if (world.fighters.empty()) {
            return nullptr;
        }
        editor.clampToWorld(world);
        pf::FighterRuntime& fighter = world.fighters[static_cast<size_t>(editor.selectedFighter)];
        if (fighter.fighterDef < 0 || fighter.fighterDef >= static_cast<int>(world.fighterDefs.size())) {
            return nullptr;
        }
        pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
        if (def.states.empty()) {
            return nullptr;
        }
        editor.selectedState = std::clamp(editor.selectedState, 0, static_cast<int>(def.states.size()) - 1);
        return &def.states[static_cast<size_t>(editor.selectedState)];
    };
    auto removeSelectedSubaction = [&]() {
        pf::FighterState* state = selectedEditorState();
        if (!state || state->action.empty()) {
            return;
        }
        editor.selectedSubaction = std::clamp(editor.selectedSubaction, 0, static_cast<int>(state->action.size()) - 1);
        state->action.erase(state->action.begin() + editor.selectedSubaction);
        editor.selectedSubaction = std::clamp(editor.selectedSubaction, 0, std::max(0, static_cast<int>(state->action.size()) - 1));
        editor.status = "Editor: removed selected subaction";
    };
    auto adjustSelectedSubactionFrames = [&](int delta) {
        pf::FighterState* state = selectedEditorState();
        if (!state || state->action.empty()) {
            return;
        }
        editor.selectedSubaction = std::clamp(editor.selectedSubaction, 0, static_cast<int>(state->action.size()) - 1);
        pf::Subaction& subaction = state->action[static_cast<size_t>(editor.selectedSubaction)];
        subaction.frames = std::max(0, subaction.frames + delta);
        editor.status = "Editor: adjusted selected subaction frame count";
    };
    auto bindSelectedObjectStateCallback = [&]() {
        if (world.objectDefs.empty()) {
            return;
        }
        editor.selectedObjectDef = std::clamp(editor.selectedObjectDef, 0, static_cast<int>(world.objectDefs.size()) - 1);
        pf::GameObjectDefinition& object = world.objectDefs[static_cast<size_t>(editor.selectedObjectDef)];
        if (object.states.empty() || object.packageScripts.empty()) {
            return;
        }
        editor.selectedObjectState = std::clamp(editor.selectedObjectState, 0, static_cast<int>(object.states.size()) - 1);
        editor.selectedPackageScript = std::clamp(editor.selectedPackageScript, 0, static_cast<int>(object.packageScripts.size()) - 1);
        bindObjectStatePackageScriptCallback(
            object.states[static_cast<size_t>(editor.selectedObjectState)],
            object.packageScripts[static_cast<size_t>(editor.selectedPackageScript)].name,
            editor);
    };
    auto ensureMainEditorSession = [&]() -> bool {
        std::string error;
        int rootFighterDef = testFighterDef;
        if (!world.fighters.empty()) {
            editor.clampToWorld(world);
            const pf::FighterRuntime& fighter = world.fighters[static_cast<size_t>(editor.selectedFighter)];
            rootFighterDef = fighter.fighterDef;
        }
        if (!ensureEditorSessionFromWorld(world, editor, editorSession, editorSessionActive, rootFighterDef, &error)) {
            editor.status = "Editor session failed: " + error;
            return false;
        }
        return true;
    };
    auto syncMainEditorSession = [&](const std::string& successMessage) -> bool {
        return syncEditorSessionMutation(world, editor, editorSession, testFighterDef, successMessage);
    };
    auto createEditorSessionStateFromUi = [&]() {
        if (!ensureMainEditorSession()) {
            return;
        }
        std::string error;
        int created = -1;
        if (pf::createEditorSessionState(editorSession, {}, editorSession.selectedState, &created, &error)) {
            const pf::FighterDefinition* edited = selectedEditorSessionFighter(editorSession);
            const std::string stateName = edited && created >= 0 && created < static_cast<int>(edited->states.size())
                ? edited->states[static_cast<size_t>(created)].name
                : std::string{"state"};
            syncMainEditorSession("Editor: created session state " + stateName);
        } else {
            editor.status = "Editor: create state failed: " + error;
        }
    };
    auto removeEditorSessionStateFromUi = [&]() {
        if (!ensureMainEditorSession()) {
            return;
        }
        std::string error;
        const pf::FighterDefinition* edited = selectedEditorSessionFighter(editorSession);
        const std::string removed = edited && editorSession.selectedState >= 0 && editorSession.selectedState < static_cast<int>(edited->states.size())
            ? edited->states[static_cast<size_t>(editorSession.selectedState)].name
            : std::string{"state"};
        if (pf::removeEditorSessionState(editorSession, editorSession.selectedState, {}, &error)) {
            syncMainEditorSession("Editor: removed session state " + removed);
        } else {
            editor.status = "Editor: remove state failed: " + error;
        }
    };
    auto launchEditorSessionTestWorld = [&]() {
        if (!ensureMainEditorSession()) {
            return;
        }
        std::string error;
        int testRoot = -1;
        pf::FighterPackageDescriptor descriptor;
        if (!pf::makeFighterEditorSessionTestWorld(editorSession, world, &testRoot, &descriptor, &error)) {
            updateEditorPackageFailure(editor, error);
            editor.status = "Editor test failed: " + error;
            return;
        }
        updateEditorPackageSummary(editor, descriptor);
        testFighterDef = testRoot;
        editor.selectedFighter = 0;
        editor.selectedState = editorSession.selectedState;
        editor.selectedSubaction = editorSession.selectedSubaction;
        editor.selectedInterrupt = editorSession.selectedInterrupt;
        editor.testMode = true;
        editor.animationPreviewActive = false;
        editor.previewCacheValid = false;
        editor.previewCacheDirty = true;
        editor.paused = false;
        replay.playbackLoaded = false;
        replay.realtimePlayback = false;
        replay.recordingActive = false;
        editor.status = "Editor test: session package loaded on Battlefield with Sandbag checksum=" + std::to_string(editor.lastPackageChecksum);
    };
    auto returnEditorSessionFromTestWorld = [&]() {
        if (!editor.testMode) {
            return;
        }
        if (!ensureMainEditorSession()) {
            return;
        }
        if (!syncMainEditorSession("Editor: returned from test with unsaved session intact")) {
            return;
        }
        editor.selectedFighter = 0;
        editor.selectedState = editorSession.selectedState;
        editor.selectedSubaction = editorSession.selectedSubaction;
        editor.selectedInterrupt = editorSession.selectedInterrupt;
        editor.testMode = false;
        editor.animationPreviewActive = true;
        editor.previewCacheValid = false;
        editor.previewCacheDirty = true;
        editor.paused = true;
        replay.playbackLoaded = false;
        replay.realtimePlayback = false;
        replay.recordingActive = false;
        editor.status = "Editor: returned from test without discarding unsaved edits";
    };

    while (!WindowShouldClose()) {
        const bool editorMode = appMode == AppMode::Editor;
        const bool editorEditMode = editorMode && !editor.testMode;
        if (appMode == AppMode::Gameplay) {
            updateTickrateControl(tickrate);
        }
        const bool editorTextEditing = editorMode && !editor.activeTextField.empty();
        const bool globalShortcutsEnabled = !editorTextEditing;
        if (editorEditMode && !editorTextEditing) {
            const Rectangle viewport = editorWorkstationLayout().viewport;
            const Vector2 mouse = GetMousePosition();
            const bool mouseInViewport = CheckCollisionPointRec(mouse, viewport);
            const float wheel = GetMouseWheelMove();
            if (mouseInViewport && wheel != 0.0f) {
                if (editor.sideView) {
                    editorCameraOrthoSize = std::clamp(editorCameraOrthoSize - wheel * 8.0f, 28.0f, 220.0f);
                } else {
                    editorCameraDistance = std::clamp(editorCameraDistance - wheel * 9.0f, 28.0f, 260.0f);
                }
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) && mouseInViewport) {
                editorCameraOrbitDragging = true;
                editor.sideView = false;
            }
            if (IsMouseButtonReleased(MOUSE_BUTTON_MIDDLE)) {
                editorCameraOrbitDragging = false;
            }
            if (editorCameraOrbitDragging) {
                const Vector2 delta = GetMouseDelta();
                editorCameraYaw -= delta.x * 0.008f;
                editorCameraPitch = std::clamp(editorCameraPitch - delta.y * 0.008f, -1.10f, 1.10f);
            }
        } else {
            editorCameraOrbitDragging = false;
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            if (editorTextEditing) {
                editor.activeTextField.clear();
                editor.textEditBuffer.clear();
            } else {
                appMode = AppMode::MainMenu;
                replay.realtimePlayback = false;
                editor.paused = true;
            }
        }
        const bool testClicked = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(GetMousePosition(), editorTestButtonRect());
        const bool newStateClicked = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(GetMousePosition(), editorNewStateButtonRect());
        const bool deleteStateClicked = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(GetMousePosition(), editorDeleteStateButtonRect());
        if (globalShortcutsEnabled && appMode == AppMode::Editor && (IsKeyPressed(KEY_N) || newStateClicked)) {
            createEditorSessionStateFromUi();
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && (IsKeyPressed(KEY_DELETE) || deleteStateClicked)) {
            removeEditorSessionStateFromUi();
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && (IsKeyPressed(KEY_T) || testClicked)) {
            if (editor.testMode) {
                returnEditorSessionFromTestWorld();
            } else {
                launchEditorSessionTestWorld();
            }
        }
        if (globalShortcutsEnabled && appMode != AppMode::MainMenu && IsKeyPressed(KEY_F1)) editor.showBoxes = !editor.showBoxes;
        if (globalShortcutsEnabled && appMode != AppMode::MainMenu && IsKeyPressed(KEY_F2)) editor.sideView = !editor.sideView;
        if (globalShortcutsEnabled && appMode != AppMode::MainMenu && IsKeyPressed(KEY_SPACE)) {
            editor.paused = !editor.paused;
            if (editor.paused) {
                replay.realtimePlayback = false;
            }
        }
        if (globalShortcutsEnabled && appMode != AppMode::MainMenu && IsKeyPressed(KEY_R)) {
            world = pf::makeTrainingWorld(testFighterDef, testFighterDef);
            replay.playbackFrame = 0;
            replay.playbackLoaded = false;
            replay.realtimePlayback = false;
            replay.recordingActive = false;
            editorSessionActive = false;
            editor.testMode = false;
            editor.animationPreviewActive = false;
            editor.previewCacheValid = false;
            editor.previewCacheDirty = true;
            editor.previewCacheFrames.clear();
            editor.status = "Editor: reset Battlefield from saved/base fighter data";
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && IsKeyPressed(KEY_LEFT_BRACKET)) {
            if (ensureMainEditorSession()) {
                editorSession.selectedState = std::max(0, editorSession.selectedState - 1);
                editorSession.selectedSubaction = 0;
                editorSession.selectedInterrupt = 0;
                editorSession.clamp();
                syncEditorSelectionFromSession(editor, editorSession);
                editor.selectionKind = pf::FighterEditorSelectionKind::State;
                if (pf::FighterDefinition* def = selectedEditorSessionFighter(editorSession)) {
                    previewEditorSelectedState(world, editor, *def);
                }
            }
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && IsKeyPressed(KEY_RIGHT_BRACKET)) {
            if (ensureMainEditorSession()) {
                ++editorSession.selectedState;
                editorSession.selectedSubaction = 0;
                editorSession.selectedInterrupt = 0;
                editorSession.clamp();
                syncEditorSelectionFromSession(editor, editorSession);
                editor.selectionKind = pf::FighterEditorSelectionKind::State;
                if (pf::FighterDefinition* def = selectedEditorSessionFighter(editorSession)) {
                    previewEditorSelectedState(world, editor, *def);
                }
            }
        }
        const bool shiftHeld = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (globalShortcutsEnabled && appMode == AppMode::Editor && IsKeyPressed(KEY_COMMA)) {
            if (shiftHeld && editor.workspace == pf::EditorWorkspace::Moveset) {
                if (ensureMainEditorSession()) {
                    std::string error;
                    int moved = -1;
                    if (pf::moveEditorSessionSubaction(editorSession, editorSession.selectedState, editor.selectedSubaction, -1, &moved, &error)) {
                        syncMainEditorSession("Editor: moved selected subaction earlier");
                        editor.selectionKind = pf::FighterEditorSelectionKind::Subaction;
                    } else {
                        editor.status = "Editor: move subaction failed: " + error;
                    }
                }
            } else {
                --editor.selectedSubaction;
                editor.selectionKind = pf::FighterEditorSelectionKind::Subaction;
            }
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && IsKeyPressed(KEY_PERIOD)) {
            if (shiftHeld && editor.workspace == pf::EditorWorkspace::Moveset) {
                if (ensureMainEditorSession()) {
                    std::string error;
                    int moved = -1;
                    if (pf::moveEditorSessionSubaction(editorSession, editorSession.selectedState, editor.selectedSubaction, 1, &moved, &error)) {
                        syncMainEditorSession("Editor: moved selected subaction later");
                        editor.selectionKind = pf::FighterEditorSelectionKind::Subaction;
                    } else {
                        editor.status = "Editor: move subaction failed: " + error;
                    }
                }
            } else {
                ++editor.selectedSubaction;
                editor.selectionKind = pf::FighterEditorSelectionKind::Subaction;
            }
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && editor.workspace == pf::EditorWorkspace::Moveset && IsKeyPressed(KEY_BACKSPACE)) {
            if (ensureMainEditorSession()) {
                std::string error;
                if (pf::removeEditorSessionSubaction(editorSession, editorSession.selectedState, editor.selectedSubaction, &error)) {
                    syncMainEditorSession("Editor: removed selected subaction");
                    editor.selectionKind = pf::FighterEditorSelectionKind::Subaction;
                } else {
                    editor.status = "Editor: remove subaction failed: " + error;
                }
            }
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && editor.workspace == pf::EditorWorkspace::Moveset && IsKeyPressed(KEY_MINUS)) {
            if (ensureMainEditorSession()) {
                pf::FighterDefinition* def = selectedEditorSessionFighter(editorSession);
                if (def && editorSession.selectedState >= 0 && editorSession.selectedState < static_cast<int>(def->states.size())) {
                    pf::FighterState& state = def->states[static_cast<size_t>(editorSession.selectedState)];
                    if (!state.action.empty()) {
                        editor.selectedSubaction = std::clamp(editor.selectedSubaction, 0, static_cast<int>(state.action.size()) - 1);
                        pf::Subaction edited = state.action[static_cast<size_t>(editor.selectedSubaction)];
                        edited.frames = std::max(0, edited.frames - 1);
                        std::string error;
                        if (pf::setEditorSessionSubaction(editorSession, editorSession.selectedState, editor.selectedSubaction, edited, &error)) {
                            syncMainEditorSession("Editor: shortened selected subaction");
                            editor.selectionKind = pf::FighterEditorSelectionKind::Subaction;
                        } else {
                            editor.status = "Editor: subaction edit failed: " + error;
                        }
                    }
                }
            }
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && editor.workspace == pf::EditorWorkspace::Moveset && IsKeyPressed(KEY_EQUAL)) {
            if (ensureMainEditorSession()) {
                pf::FighterDefinition* def = selectedEditorSessionFighter(editorSession);
                if (def && editorSession.selectedState >= 0 && editorSession.selectedState < static_cast<int>(def->states.size())) {
                    pf::FighterState& state = def->states[static_cast<size_t>(editorSession.selectedState)];
                    if (!state.action.empty()) {
                        editor.selectedSubaction = std::clamp(editor.selectedSubaction, 0, static_cast<int>(state.action.size()) - 1);
                        pf::Subaction edited = state.action[static_cast<size_t>(editor.selectedSubaction)];
                        ++edited.frames;
                        std::string error;
                        if (pf::setEditorSessionSubaction(editorSession, editorSession.selectedState, editor.selectedSubaction, edited, &error)) {
                            syncMainEditorSession("Editor: lengthened selected subaction");
                            editor.selectionKind = pf::FighterEditorSelectionKind::Subaction;
                        } else {
                            editor.status = "Editor: subaction edit failed: " + error;
                        }
                    }
                }
            }
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && editor.workspace == pf::EditorWorkspace::Assets && IsKeyPressed(KEY_U)) {
            bindSelectedObjectStateCallback();
        }
        if (globalShortcutsEnabled && appMode != AppMode::MainMenu && IsKeyPressed(KEY_LEFT)) selectFighterDef(testFighterDef - 1);
        if (globalShortcutsEnabled && appMode != AppMode::MainMenu && IsKeyPressed(KEY_RIGHT)) selectFighterDef(testFighterDef + 1);
        for (size_t i = 0; i < fighterKeys.size(); ++i) {
            if (globalShortcutsEnabled && appMode != AppMode::MainMenu && IsKeyPressed(fighterKeys[i]) && i < world.fighterDefs.size()) {
                selectFighterDef(static_cast<int>(i));
            }
        }

        if (globalShortcutsEnabled && appMode == AppMode::Gameplay && IsKeyPressed(KEY_F5)) {
            beginReplayRecording(replay, world, testFighterDef, testFighterDef);
            editor.paused = false;
        }
        if (globalShortcutsEnabled && appMode == AppMode::Gameplay && IsKeyPressed(KEY_F6)) {
            saveReplayRecording(replay);
        }
        if (globalShortcutsEnabled && appMode == AppMode::Gameplay && IsKeyPressed(KEY_F7)) {
            loadReplayPlayback(replay, world, testFighterDef);
            editor.paused = true;
        }
        if (globalShortcutsEnabled && appMode == AppMode::Gameplay && IsKeyPressed(KEY_F8)) {
            stepReplayPlayback(replay, world);
        }
        if (globalShortcutsEnabled && appMode == AppMode::Gameplay && IsKeyPressed(KEY_F9) && replay.playbackLoaded) {
            replay.realtimePlayback = !replay.realtimePlayback;
            editor.paused = !replay.realtimePlayback;
        }

        const pf::InputFrame p1Input = editorTextEditing || editorEditMode ? pf::InputFrame{} : readPlayerInput(0, false);
        const pf::InputFrame p2Input = editorTextEditing || editorMode ? pf::InputFrame{} : readPlayerInput(1, true);
        const bool simRunning = appMode == AppMode::Gameplay
            ? ((replay.playbackLoaded && replay.realtimePlayback) || !editor.paused)
            : (editorMode && editor.testMode && !editor.paused);
        if (simRunning) {
            tickrate.accumulator += GetFrameTime();
            const float tickStep = 1.0f / static_cast<float>(tickrate.ticksPerSecond);
            while (tickrate.accumulator >= tickStep) {
                if (replay.playbackLoaded && replay.realtimePlayback) {
                    stepReplayPlayback(replay, world);
                } else {
                    pf::tickWorld(world, {p1Input, p2Input});
                    if (replay.recordingActive) {
                        replay.recording.frames.push_back({{p1Input, p2Input}});
                        replay.status = "Recording frame " + std::to_string(replay.recording.frames.size());
                    }
                }
                tickrate.accumulator -= tickStep;
            }
        } else {
            tickrate.accumulator = 0.0f;
        }
        if (editorEditMode) {
            if (ensureMainEditorSession()) {
                prepareEditorEditPreview(world, editor, editorSession, testFighterDef, !editor.paused);
            }
        }

        if (editor.sideView) {
            camera.position = {0.0f, 12.0f, 145.0f};
            camera.target = {0.0f, 12.0f, 0.0f};
            camera.fovy = editorCameraOrthoSize;
            camera.projection = CAMERA_ORTHOGRAPHIC;
        } else {
            const Vector3 target{0.0f, 10.0f, 0.0f};
            const float cosPitch = std::cos(editorCameraPitch);
            camera.target = target;
            camera.position = {
                target.x + std::sin(editorCameraYaw) * cosPitch * editorCameraDistance,
                target.y + std::sin(editorCameraPitch) * editorCameraDistance,
                target.z + std::cos(editorCameraYaw) * cosPitch * editorCameraDistance,
            };
            camera.fovy = 45.0f;
            camera.projection = CAMERA_PERSPECTIVE;
        }

        BeginDrawing();
        ClearBackground({205, 214, 222, 255});
        if (appMode == AppMode::MainMenu) {
            drawMainMenu(appMode);
        } else {
            if (editorEditMode) {
                const EditorWorkstationLayout layout = editorWorkstationLayout();
                const Rectangle viewport = layout.viewport;
                BeginScissorMode(
                    static_cast<int>(viewport.x),
                    static_cast<int>(viewport.y),
                    static_cast<int>(viewport.width),
                    static_cast<int>(viewport.height));
                rlViewport(
                    static_cast<int>(viewport.x),
                    GetScreenHeight() - static_cast<int>(viewport.y + viewport.height),
                    static_cast<int>(viewport.width),
                    static_cast<int>(viewport.height));
                BeginMode3D(camera);
                drawWorldScene3D(world, editor, true);
                EndMode3D();
                rlViewport(0, 0, GetScreenWidth(), GetScreenHeight());
                EndScissorMode();
            } else {
                BeginMode3D(camera);
                drawWorldScene3D(world, editor, false);
                if (editorMode) {
                    drawEditorSelectedAuthoredMeshVertex(world, editor);
                    drawEditorSelectedAuthoredJoint(world, editor);
                }
                EndMode3D();
            }
            if (editorMode) {
                drawEditorWorkstation(world, editor, editorSession, editorSessionActive, testFighterDef);
            }
            if (appMode == AppMode::Gameplay) {
                drawReplayStatus(replay);
                drawTickrateControl(tickrate);
            }
        }
        drawTopNav(appMode);
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
#endif
