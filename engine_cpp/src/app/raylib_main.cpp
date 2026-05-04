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

static pf::Vec2 hsdEcbProjection(const pf::FighterRuntime& fighter, pf::Vec3 joint) {
    return {fighter.position.x + fighter.facing * joint.z, joint.y};
}

static void drawImportedEcbSources(const pf::FighterDefinition& def, const pf::FighterRuntime& fighter) {
    if (!def.hsdAsset || !def.hsdAsset->hasEnvironmentCollision || fighter.hsdJointWorldPositions.empty()) {
        return;
    }

    bool haveExtents = false;
    pf::Fix minHorizontal = 0;
    pf::Fix maxHorizontal = 0;
    if (fighter.hsdJointWorldPositions.size() > 1) {
        const pf::Vec2 topN = hsdEcbProjection(fighter, fighter.hsdJointWorldPositions[1]);
        DrawSphere(toRayGround(topN), 0.08f, MAGENTA);
    }

    for (int bone : def.hsdAsset->environmentCollision.bones) {
        if (bone < 0 || static_cast<size_t>(bone) >= fighter.hsdJointWorldPositions.size()) {
            continue;
        }
        const pf::Vec3 source = fighter.hsdJointWorldPositions[static_cast<size_t>(bone)];
        if (!haveExtents) {
            minHorizontal = source.z;
            maxHorizontal = source.z;
            haveExtents = true;
        } else {
            minHorizontal = std::min(minHorizontal, source.z);
            maxHorizontal = std::max(maxHorizontal, source.z);
        }
        DrawSphere(toRay(source), 0.055f, Fade(ORANGE, 0.75f));
        DrawSphere(toRayGround(hsdEcbProjection(fighter, source)), 0.065f, GOLD);
    }

    if (haveExtents && fighter.hsdJointWorldPositions.size() > 1) {
        const pf::HsdEnvironmentCollision& source = def.hsdAsset->environmentCollision;
        const pf::Vec2 topN = hsdEcbProjection(fighter, fighter.hsdJointWorldPositions[1]);
        const pf::Fix halfWidth = pf::fxMul(pf::fxAbs(maxHorizontal - minHorizontal), pf::fxFromFloat(0.5f));
        const pf::Fix boxReach = halfWidth + source.ledgeGrabWidth;
        const pf::Fix boxBottom = topN.y + source.ledgeGrabYOffset - pf::fxMul(source.ledgeGrabHeight, pf::fxFromFloat(0.5f));
        const pf::Fix boxTop = topN.y + source.ledgeGrabYOffset + pf::fxMul(source.ledgeGrabHeight, pf::fxFromFloat(0.5f));
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
    if (def.hsdAsset) {
        return &def.hsdAsset->skeleton;
    }
    return def.authoredSkeleton.empty() ? nullptr : &def.authoredSkeleton;
}

static void drawAnimationSkeleton(const pf::FighterDefinition& def, const pf::FighterRuntime& fighter, Color color) {
    const std::vector<pf::AnimationJoint>* skeleton = animationSkeletonForDrawing(def);
    if (!skeleton || fighter.hsdJointWorldPositions.empty()) {
        return;
    }

    const size_t jointCount = std::min(skeleton->size(), fighter.hsdJointWorldPositions.size());
    for (size_t i = 0; i < jointCount; ++i) {
        pf::Vec3 joint = fighter.hsdJointWorldPositions[i];
        const int parent = (*skeleton)[i].parent;
        if (parent >= 0 && static_cast<size_t>(parent) < fighter.hsdJointWorldPositions.size()) {
            pf::Vec3 parentJoint = fighter.hsdJointWorldPositions[static_cast<size_t>(parent)];
            DrawLine3D(toRay(parentJoint), toRay(joint), color);
        }
        DrawSphere(toRay(joint), 0.04f, color);
    }

    const int head = def.hsdAsset ? def.hsdAsset->fighterBones.head : -1;
    if (head >= 0 && static_cast<size_t>(head) < fighter.hsdJointWorldPositions.size()) {
        pf::Vec3 headJoint = fighter.hsdJointWorldPositions[static_cast<size_t>(head)];
        DrawSphereWires(toRay(headJoint), 0.18f, 10, 6, color);
    } else if (!def.hsdAsset && fighter.hsdJointWorldPositions.size() > 1) {
        pf::Vec3 tipJoint = fighter.hsdJointWorldPositions.back();
        DrawSphereWires(toRay(tipJoint), 0.12f, 8, 4, color);
    }
}

namespace {

constexpr int kHsdShaderMaxBones = 200;
constexpr uint32_t kPobjCullBack = 1u << 14;
constexpr uint32_t kPobjCullFront = 1u << 15;
constexpr uint32_t kJObjSkeletonRoot = 1u << 1;

struct HsdRenderBatch {
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

struct HsdRenderCache {
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
    std::vector<HsdRenderBatch> batches;
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

const char* hsdMeshVertexShader() {
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

const char* hsdMeshFragmentShader() {
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

static Texture2D loadTextureFromRgba(const pf::HsdMeshTexture& texture) {
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

static HsdRenderCache createHsdRenderCache(const pf::HsdFighterAnimationAsset& asset) {
    HsdRenderCache cache;
    cache.shader = LoadShaderFromMemory(hsdMeshVertexShader(), hsdMeshFragmentShader());
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

    cache.textures.reserve(asset.mesh.textures.size());
    for (const pf::HsdMeshTexture& texture : asset.mesh.textures) {
        if (texture.width > 0 && texture.height > 0 && !texture.rgba.empty()) {
            cache.textures.push_back(loadTextureFromRgba(texture));
        }
    }

    cache.batches.reserve(asset.mesh.batches.size());
    for (const pf::HsdMeshBatch& source : asset.mesh.batches) {
        HsdRenderBatch batch;
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

        for (const pf::HsdMeshVertex& vertex : source.vertices) {
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

static HsdRenderCache& hsdRenderCache(const pf::HsdFighterAnimationAsset& asset) {
    static std::unordered_map<const pf::HsdFighterAnimationAsset*, std::unique_ptr<HsdRenderCache>> caches;
    auto it = caches.find(&asset);
    if (it == caches.end()) {
        it = caches.emplace(&asset, std::make_unique<HsdRenderCache>(createHsdRenderCache(asset))).first;
    }
    return *it->second;
}

static Matrix parentMatrixForBatch(const HsdRenderBatch& batch,
                                   const std::vector<Matrix>& boneWorldMatrices) {
    int bone = batch.singleBindBone >= 0 ? batch.singleBindBone : batch.parentBone;
    if (bone >= 0 && static_cast<size_t>(bone) < boneWorldMatrices.size()) {
        return boneWorldMatrices[static_cast<size_t>(bone)];
    }
    return MatrixIdentity();
}

static bool isBatchVisible(const HsdRenderBatch& batch, const pf::FighterRuntime& fighter) {
    if (!batch.hiddenByVisibilityTable) {
        return true;
    }
    if (batch.modelPartIndex < 0 || batch.modelPartState < 0) {
        return false;
    }
    if (static_cast<size_t>(batch.modelPartIndex) >= fighter.hsdModelVisibilityStates.size()) {
        return batch.modelPartState == 0;
    }
    return fighter.hsdModelVisibilityStates[static_cast<size_t>(batch.modelPartIndex)] == batch.modelPartState;
}

static void drawImportedMesh(const pf::FighterDefinition& def, const pf::FighterRuntime& fighter) {
    if (!def.hsdAsset || def.hsdAsset->mesh.batches.empty() || fighter.hsdJointWorldTransforms.empty()) {
        return;
    }

    HsdRenderCache& cache = hsdRenderCache(*def.hsdAsset);
    constexpr int kMaxBones = kHsdShaderMaxBones;
    std::vector<Matrix> boneWorldMatrices(kMaxBones, MatrixIdentity());
    std::vector<Matrix> boneMatrices(kMaxBones, MatrixIdentity());
    const size_t boneCount = std::min({fighter.hsdJointWorldTransforms.size(),
                                       def.hsdAsset->mesh.inverseBindMatrices.size(),
                                       static_cast<size_t>(kMaxBones)});
    for (size_t i = 0; i < boneCount; ++i) {
        std::array<float, 16> world = toFloatMatrix(fighter.hsdJointWorldTransforms[i]);
        boneWorldMatrices[i] = toRayMatrix(world);
        boneMatrices[i] = toRayMatrix(multiplyRowMajor(world, def.hsdAsset->mesh.inverseBindMatrices[i]));
    }

    Matrix mvp = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    rlEnableShader(cache.shader.id);
    if (cache.locMvp >= 0) {
        SetShaderValueMatrix(cache.shader, cache.locMvp, mvp);
    }
    uploadBoneUniformBuffer(cache.boneUniformBuffer, boneMatrices, boneWorldMatrices);

    for (const HsdRenderBatch& batch : cache.batches) {
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

static Rectangle editorTestButtonRect() {
    return {444.0f, 22.0f, 74.0f, 28.0f};
}

static Rectangle editorNewStateButtonRect() {
    return {318.0f, 22.0f, 54.0f, 28.0f};
}

static Rectangle editorDeleteStateButtonRect() {
    return {380.0f, 22.0f, 56.0f, 28.0f};
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
    case pf::PackageScriptOp::SetVarObjectLastFighter: return "ObjLastF";
    case pf::PackageScriptOp::SetVarObjectLastObject: return "ObjLastO";
    case pf::PackageScriptOp::SetVarObjectDamage: return "ObjDmg";
    case pf::PackageScriptOp::SetVarObjectPositionX: return "ObjPosX";
    case pf::PackageScriptOp::SetVarObjectPositionY: return "ObjPosY";
    case pf::PackageScriptOp::SetVarObjectVelocityX: return "ObjVelX";
    case pf::PackageScriptOp::SetVarObjectVelocityY: return "ObjVelY";
    case pf::PackageScriptOp::SetVarObjectAnimationFrame: return "ObjAnimF";
    case pf::PackageScriptOp::SetVarObjectAnimationRate: return "ObjAnimR";
    case pf::PackageScriptOp::SetVarOwnedObjectCount: return "OwnCnt";
    case pf::PackageScriptOp::SetVarOwnerFighterVar: return "OwnRead";
    case pf::PackageScriptOp::SetOwnerFighterVarImmediate: return "OwnSet";
    case pf::PackageScriptOp::SetOwnerFighterVarFromVar: return "OwnVar";
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
    case pf::PackageScriptOp::DestroyObject: return "KillObj";
    case pf::PackageScriptOp::DestroyOwnedObjects: return "KillOwn";
    case pf::PackageScriptOp::SkipIfVarLessThanImmediate: return "IfVarLt";
    case pf::PackageScriptOp::SkipIfVarLessThanVar: return "IfVarVar";
    case pf::PackageScriptOp::SkipIfVarEqualImmediate: return "IfVarEq";
    case pf::PackageScriptOp::SkipIfVarEqualVar: return "IfEqVar";
    case pf::PackageScriptOp::JumpRelative: return "Jump";
    case pf::PackageScriptOp::CallScript: return "Call";
    case pf::PackageScriptOp::SwitchFighterDefinition: return "Fighter";
    case pf::PackageScriptOp::SpawnFighter: return "SpawnF";
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

static bool packageScriptOpIsSpawn(pf::PackageScriptOp op) {
    return op == pf::PackageScriptOp::SpawnObject ||
        op == pf::PackageScriptOp::SpawnObjectFromVars ||
        op == pf::PackageScriptOp::SpawnProjectile ||
        op == pf::PackageScriptOp::SpawnProjectileFromVars;
}

static bool packageScriptOpIsProjectileSpawn(pf::PackageScriptOp op) {
    return op == pf::PackageScriptOp::SpawnProjectile ||
        op == pf::PackageScriptOp::SpawnProjectileFromVars;
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
    case pf::PackageScriptOp::SpawnObjectFromVars:
        instruction.op = pf::PackageScriptOp::SpawnObject;
        break;
    case pf::PackageScriptOp::SpawnProjectileFromVars:
        instruction.op = pf::PackageScriptOp::SpawnProjectile;
        break;
    case pf::PackageScriptOp::SetVarImmediate:
    case pf::PackageScriptOp::SetVarFromVar:
    case pf::PackageScriptOp::AddVarImmediate:
    case pf::PackageScriptOp::AddVar:
    case pf::PackageScriptOp::ScaleVarFixed:
    case pf::PackageScriptOp::SetVarRandom:
    case pf::PackageScriptOp::SetVarFrame:
    case pf::PackageScriptOp::SetVarStateFrame:
    case pf::PackageScriptOp::SetVarStateIndex:
    case pf::PackageScriptOp::SetVarGrounded:
    case pf::PackageScriptOp::SetVarFacing:
    case pf::PackageScriptOp::SetVarFighterStateFrame:
    case pf::PackageScriptOp::SetVarFighterStateIndex:
    case pf::PackageScriptOp::SetVarFighterGrounded:
    case pf::PackageScriptOp::SetVarFighterFacing:
    case pf::PackageScriptOp::SetVarFighterJumpsUsed:
    case pf::PackageScriptOp::SetVarFighterJumpsRemaining:
    case pf::PackageScriptOp::SetVarFighterCommandVar:
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
    case pf::PackageScriptOp::SetVarObjectLastFighter:
    case pf::PackageScriptOp::SetVarObjectLastObject:
    case pf::PackageScriptOp::SetVarObjectDamage:
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

        instruction.op = instruction.op == pf::PackageScriptOp::SpawnProjectileFromVars
            ? pf::PackageScriptOp::SpawnObjectFromVars
            : pf::PackageScriptOp::SpawnObject;
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
    case pf::PackageScriptOp::SetVarFighterStateFrame:
    case pf::PackageScriptOp::SetVarFighterStateIndex:
    case pf::PackageScriptOp::SetVarFighterGrounded:
    case pf::PackageScriptOp::SetVarFighterFacing:
    case pf::PackageScriptOp::SetVarFighterJumpsUsed:
    case pf::PackageScriptOp::SetVarFighterJumpsRemaining:
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
    case pf::PackageScriptOp::SetVarObjectLastFighter:
    case pf::PackageScriptOp::SetVarObjectLastObject:
    case pf::PackageScriptOp::SetVarObjectDamage:
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
    case pf::PackageScriptOp::AddVar:
        label += " v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA) + " + v" + std::to_string(instruction.srcB);
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
    case pf::PackageScriptOp::DestroyObject:
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
    case pf::PackageScriptOp::SetVarRandom: return pf::PackageScriptOp::SetVarFrame;
    case pf::PackageScriptOp::SetVarFrame: return pf::PackageScriptOp::SetVarStateFrame;
    case pf::PackageScriptOp::SetVarStateFrame: return pf::PackageScriptOp::SetVarStateIndex;
    case pf::PackageScriptOp::SetVarStateIndex: return pf::PackageScriptOp::SetVarGrounded;
    case pf::PackageScriptOp::SetVarGrounded: return pf::PackageScriptOp::SetVarFacing;
    case pf::PackageScriptOp::SetVarFacing: return pf::PackageScriptOp::SetVarFighterStateFrame;
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
    case pf::PackageScriptOp::SetFighterCommandVarFromVar: return pf::PackageScriptOp::SetVarFighterPercent;
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
    case pf::PackageScriptOp::SpawnProjectileFromVars: return pf::PackageScriptOp::DestroyOwnedObjects;
    case pf::PackageScriptOp::DestroyObject: return pf::PackageScriptOp::DestroyOwnedObjects;
    case pf::PackageScriptOp::DestroyOwnedObjects: return pf::PackageScriptOp::SkipIfVarLessThanImmediate;
    case pf::PackageScriptOp::SkipIfVarLessThanImmediate: return pf::PackageScriptOp::SkipIfVarLessThanVar;
    case pf::PackageScriptOp::SkipIfVarLessThanVar: return pf::PackageScriptOp::SkipIfVarEqualImmediate;
    case pf::PackageScriptOp::SkipIfVarEqualImmediate: return pf::PackageScriptOp::SkipIfVarEqualVar;
    case pf::PackageScriptOp::SkipIfVarEqualVar: return pf::PackageScriptOp::JumpRelative;
    case pf::PackageScriptOp::JumpRelative: return pf::PackageScriptOp::CallScript;
    case pf::PackageScriptOp::CallScript: return pf::PackageScriptOp::SwitchFighterDefinition;
    case pf::PackageScriptOp::SwitchFighterDefinition: return pf::PackageScriptOp::SpawnFighter;
    case pf::PackageScriptOp::SpawnFighter: return pf::PackageScriptOp::Nop;
    }
    return pf::PackageScriptOp::Nop;
}

static bool packageScriptOpAllowedForObject(pf::PackageScriptOp op) {
    return op != pf::PackageScriptOp::SwitchFighterDefinition &&
        op != pf::PackageScriptOp::SpawnFighter &&
        op != pf::PackageScriptOp::SetVarButtonDown &&
        op != pf::PackageScriptOp::SetVarButtonPressed &&
        op != pf::PackageScriptOp::SetVarStickX &&
        op != pf::PackageScriptOp::SetVarStickY &&
        op != pf::PackageScriptOp::SetVarCStickX &&
        op != pf::PackageScriptOp::SetVarCStickY &&
        op != pf::PackageScriptOp::SetVarShield &&
        op != pf::PackageScriptOp::SetFighterCommandVarImmediate &&
        op != pf::PackageScriptOp::SetFighterCommandVarFromVar;
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
    case pf::PackageScriptOp::SetVarObjectOwner: return pf::PackageScriptOp::SetVarObjectHeldBy;
    case pf::PackageScriptOp::SetVarObjectHeldBy: return pf::PackageScriptOp::SetVarObjectLastFighter;
    case pf::PackageScriptOp::SetVarObjectLastFighter: return pf::PackageScriptOp::SetVarObjectLastObject;
    case pf::PackageScriptOp::SetVarObjectLastObject: return pf::PackageScriptOp::SetVarObjectDamage;
    case pf::PackageScriptOp::SetVarObjectDamage: return pf::PackageScriptOp::SetVarObjectPositionX;
    case pf::PackageScriptOp::SetVarObjectPositionX: return pf::PackageScriptOp::SetVarObjectPositionY;
    case pf::PackageScriptOp::SetVarObjectPositionY: return pf::PackageScriptOp::SetVarObjectVelocityX;
    case pf::PackageScriptOp::SetVarObjectVelocityX: return pf::PackageScriptOp::SetVarObjectVelocityY;
    case pf::PackageScriptOp::SetVarObjectVelocityY: return pf::PackageScriptOp::SetVarObjectAnimationFrame;
    case pf::PackageScriptOp::SetVarObjectAnimationFrame: return pf::PackageScriptOp::SetVarObjectAnimationRate;
    case pf::PackageScriptOp::SetVarObjectAnimationRate: return pf::PackageScriptOp::SetVarOwnedObjectCount;
    case pf::PackageScriptOp::SetVarOwnedObjectCount: return pf::PackageScriptOp::SetVarOwnerFighterVar;
    case pf::PackageScriptOp::SetVarOwnerFighterVar: return pf::PackageScriptOp::SetOwnerFighterVarImmediate;
    case pf::PackageScriptOp::SetOwnerFighterVarImmediate: return pf::PackageScriptOp::SetOwnerFighterVarFromVar;
    case pf::PackageScriptOp::SetOwnerFighterVarFromVar: return pf::PackageScriptOp::SetVarObjectOwner;
    default: return pf::PackageScriptOp::SetVarObjectOwner;
    }
}

static bool packageScriptOpIsFighterContextRead(pf::PackageScriptOp op) {
    switch (op) {
    case pf::PackageScriptOp::SetVarFighterStateFrame:
    case pf::PackageScriptOp::SetVarFighterStateIndex:
    case pf::PackageScriptOp::SetVarFighterGrounded:
    case pf::PackageScriptOp::SetVarFighterFacing:
    case pf::PackageScriptOp::SetVarFighterJumpsUsed:
    case pf::PackageScriptOp::SetVarFighterJumpsRemaining:
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
    case pf::PackageScriptOp::SetVarFighterStateFrame: return pf::PackageScriptOp::SetVarFighterStateIndex;
    case pf::PackageScriptOp::SetVarFighterStateIndex: return pf::PackageScriptOp::SetVarFighterGrounded;
    case pf::PackageScriptOp::SetVarFighterGrounded: return pf::PackageScriptOp::SetVarFighterFacing;
    case pf::PackageScriptOp::SetVarFighterFacing: return pf::PackageScriptOp::SetVarFighterJumpsUsed;
    case pf::PackageScriptOp::SetVarFighterJumpsUsed: return pf::PackageScriptOp::SetVarFighterJumpsRemaining;
    case pf::PackageScriptOp::SetVarFighterJumpsRemaining: return pf::PackageScriptOp::SetVarFighterCommandVar;
    case pf::PackageScriptOp::SetVarFighterCommandVar: return pf::PackageScriptOp::SetVarFighterPercent;
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
    if (instruction.op == pf::PackageScriptOp::SetFighterCommandVarImmediate ||
        instruction.op == pf::PackageScriptOp::SetFighterCommandVarFromVar)
    {
        instruction.dst = std::clamp(instruction.dst < 0 ? 0 : instruction.dst, 0, 3);
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
    normalizePackageSpawnInstructionTarget(instruction, world, selectedObjectDef);
    normalizePackageObjectTargetInstruction(instruction, world, selectedObjectDef);
    if (instruction.op == pf::PackageScriptOp::SwitchFighterDefinition && instruction.text.empty() && !world.fighterDefs.empty()) {
        instruction.text = packageFighterTargetName(world, currentFighterDef);
    }
    if (instruction.op == pf::PackageScriptOp::SpawnFighter && instruction.text.empty() && !world.fighterDefs.empty()) {
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
             instruction.op == pf::PackageScriptOp::SpawnFighter) &&
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
    Rectangle rect)
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

static pf::HsdFighterMesh makeEditorTriangleMesh() {
    pf::HsdFighterMesh mesh;
    pf::HsdMeshBatch batch;
    batch.parentBone = 0;
    batch.singleBindBone = 0;
    batch.materialColor = {160, 220, 255, 255};

    auto vertex = [](pf::Vec3 position) {
        pf::HsdMeshVertex out;
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

static int authoredMeshVertexCount(const pf::HsdFighterMesh& mesh) {
    int count = 0;
    for (const pf::HsdMeshBatch& batch : mesh.batches) {
        count += static_cast<int>(batch.vertices.size());
    }
    return count;
}

static pf::HsdMeshVertex* authoredMeshVertexAt(pf::HsdFighterMesh& mesh, int vertexIndex) {
    int cursor = 0;
    for (pf::HsdMeshBatch& batch : mesh.batches) {
        const int next = cursor + static_cast<int>(batch.vertices.size());
        if (vertexIndex >= cursor && vertexIndex < next) {
            return &batch.vertices[static_cast<size_t>(vertexIndex - cursor)];
        }
        cursor = next;
    }
    return nullptr;
}

static std::string authoredMeshVertexInfluenceSummary(const pf::HsdMeshVertex& vertex) {
    std::string summary;
    int shown = 0;
    for (const pf::HsdMeshVertexInfluence& influence : vertex.influences) {
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

static pf::Vec2 authoredMeshSize(const pf::HsdFighterMesh& mesh) {
    bool haveVertex = false;
    pf::Fix minX = 0;
    pf::Fix maxX = 0;
    pf::Fix minY = 0;
    pf::Fix maxY = 0;
    for (const pf::HsdMeshBatch& batch : mesh.batches) {
        for (const pf::HsdMeshVertex& vertex : batch.vertices) {
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

static void scaleAuthoredMesh(pf::HsdFighterMesh& mesh, pf::Fix scaleX, pf::Fix scaleY) {
    for (pf::HsdMeshBatch& batch : mesh.batches) {
        for (pf::HsdMeshVertex& vertex : batch.vertices) {
            vertex.position.x = pf::fxMul(vertex.position.x, scaleX);
            vertex.position.y = pf::fxMul(vertex.position.y, scaleY);
        }
    }
}

static void nudgeAuthoredMeshVertex(pf::HsdFighterMesh& mesh, int vertexIndex, pf::Vec3 delta) {
    pf::HsdMeshVertex* vertex = authoredMeshVertexAt(mesh, vertexIndex);
    if (!vertex) {
        return;
    }
    vertex->position.x += delta.x;
    vertex->position.y += delta.y;
    vertex->position.z += delta.z;
}

static void normalizeAuthoredMeshVertexInfluences(pf::HsdMeshVertex& vertex, int fallbackJoint) {
    float sum = 0.0f;
    for (pf::HsdMeshVertexInfluence& influence : vertex.influences) {
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
    for (pf::HsdMeshVertexInfluence& influence : vertex.influences) {
        if (influence.bone >= 0 && influence.weight > 0.0f) {
            influence.weight /= sum;
        }
    }
}

static void bindAuthoredMeshVertexToJoint(pf::HsdFighterMesh& mesh, int vertexIndex, int joint, int skeletonSize) {
    int cursor = 0;
    for (pf::HsdMeshBatch& batch : mesh.batches) {
        const int next = cursor + static_cast<int>(batch.vertices.size());
        if (vertexIndex >= cursor && vertexIndex < next) {
            pf::HsdMeshVertex& vertex = batch.vertices[static_cast<size_t>(vertexIndex - cursor)];
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
    pf::HsdFighterMesh& mesh,
    int vertexIndex,
    int joint,
    int skeletonSize,
    float amount)
{
    int cursor = 0;
    for (pf::HsdMeshBatch& batch : mesh.batches) {
        const int next = cursor + static_cast<int>(batch.vertices.size());
        if (vertexIndex < cursor || vertexIndex >= next) {
            cursor = next;
            continue;
        }

        pf::HsdMeshVertex& vertex = batch.vertices[static_cast<size_t>(vertexIndex - cursor)];
        pf::HsdMeshVertexInfluence* target = nullptr;
        pf::HsdMeshVertexInfluence* empty = nullptr;
        for (pf::HsdMeshVertexInfluence& influence : vertex.influences) {
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

static void bindAuthoredMeshToJoint(pf::HsdFighterMesh& mesh, int joint) {
    for (pf::HsdMeshBatch& batch : mesh.batches) {
        batch.parentBone = joint;
        batch.singleBindBone = joint;
        for (pf::HsdMeshVertex& vertex : batch.vertices) {
            vertex.influences = {};
            vertex.influences[0] = {joint, 1.0f};
        }
    }
}

static int authoredMeshMaxInfluences(const pf::HsdFighterMesh& mesh) {
    int maxInfluences = 0;
    for (const pf::HsdMeshBatch& batch : mesh.batches) {
        for (const pf::HsdMeshVertex& vertex : batch.vertices) {
            int vertexInfluences = 0;
            for (const pf::HsdMeshVertexInfluence& influence : vertex.influences) {
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
    pf::HsdFighterMesh& mesh,
    const std::vector<pf::AnimationJoint>& skeleton)
{
    const std::vector<pf::Vec3> joints = authoredSkeletonBindPositions(skeleton);
    if (joints.empty()) {
        return;
    }

    for (pf::HsdMeshBatch& batch : mesh.batches) {
        batch.parentBone = joints.size() > 1 ? -1 : 0;
        batch.singleBindBone = joints.size() > 1 ? -1 : 0;
        batch.hasEnvelopes = joints.size() > 1;
        for (pf::HsdMeshVertex& vertex : batch.vertices) {
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
    for (pf::HsdMeshBatch& batch : def.authoredMesh.batches) {
        batch.parentBone = remapRemovedAuthoredBone(batch.parentBone, jointIndex, fallbackBone);
        batch.singleBindBone = remapRemovedAuthoredBone(batch.singleBindBone, jointIndex, fallbackBone);
        for (pf::HsdMeshVertex& vertex : batch.vertices) {
            for (pf::HsdMeshVertexInfluence& influence : vertex.influences) {
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
    def.hasHsdAsset = false;
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
        {pf::BoneId::Hip, {0, pf::fxFromFloat(-0.45f), 0}, {0, pf::fxFromFloat(0.55f), 0}, pf::fxFromFloat(0.45f), pf::HurtboxState::Normal, true},
        {pf::BoneId::Head, {0, pf::fxFromFloat(-0.2f), 0}, {0, pf::fxFromFloat(0.2f), 0}, pf::fxFromFloat(0.32f), pf::HurtboxState::Normal, true},
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

static void removePackageScriptInstructionRefs(std::vector<pf::PackageScript>& scripts, const std::string& scriptName) {
    for (pf::PackageScript& script : scripts) {
        for (pf::PackageScriptInstruction& instruction : script.instructions) {
            if (instruction.op == pf::PackageScriptOp::CallScript && instruction.text == scriptName) {
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
            if (instruction.op == pf::PackageScriptOp::CallScript && instruction.text == oldScriptName) {
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

static pf::Fix shieldRadius(const pf::FighterDefinition& def, const pf::FighterRuntime& fighter) {
    const pf::MeleeCommonData& common = def.properties.common;
    const pf::Fix healthRatio = def.shield.maxHealth > 0 ? pf::fxDiv(fighter.shieldHealth, def.shield.maxHealth) : 0;
    const pf::Fix light = fighter.input.down(pf::ButtonShield) ? std::clamp(fighter.input.frames[0].shieldAnalog, pf::Fix{0}, pf::fx(1)) : 0;
    const pf::Fix sizeScale = common.hardShieldSizeScaleX2D4 +
        pf::fxMul(light, common.lightShieldSizeScaleX2D8 - common.hardShieldSizeScaleX2D4);
    const pf::Fix scaledHealth = pf::fxMul(healthRatio, sizeScale);
    return pf::fxMul(def.shield.startSizeHardShield, common.minShieldScaleX264 + pf::fxMul(pf::fx(1) - common.minShieldScaleX264, scaledHealth));
}

static pf::Vec3 authoredMeshVertexWorld(const pf::FighterRuntime& fighter, const pf::HsdMeshBatch& batch, const pf::HsdMeshVertex& vertex) {
    float blendedX = 0.0f;
    float blendedY = 0.0f;
    float blendedZ = 0.0f;
    float weightSum = 0.0f;
    for (const pf::HsdMeshVertexInfluence& influence : vertex.influences) {
        if (influence.weight <= 0.0f ||
            influence.bone < 0 ||
            static_cast<size_t>(influence.bone) >= fighter.hsdJointWorldTransforms.size())
        {
            continue;
        }
        const pf::Vec3 weighted = pf::transformPoint(
            fighter.hsdJointWorldTransforms[static_cast<size_t>(influence.bone)],
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
    if (bone >= 0 && static_cast<size_t>(bone) < fighter.hsdJointWorldTransforms.size()) {
        return pf::transformPoint(fighter.hsdJointWorldTransforms[static_cast<size_t>(bone)], vertex.position);
    }
    return {fighter.position.x + vertex.position.x, fighter.position.y + vertex.position.y, vertex.position.z};
}

static void drawAuthoredMesh(const pf::FighterDefinition& def, const pf::FighterRuntime& fighter) {
    for (const pf::HsdMeshBatch& batch : def.authoredMesh.batches) {
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
    for (const pf::HsdMeshBatch& batch : def.authoredMesh.batches) {
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
        editor.selectedAnimationJoint >= static_cast<int>(fighter.hsdJointWorldPositions.size()) ||
        editor.selectedAnimationJoint >= static_cast<int>(def.authoredSkeleton.size()))
    {
        return;
    }
    const Vector3 jointPosition = toRay(fighter.hsdJointWorldPositions[static_cast<size_t>(editor.selectedAnimationJoint)]);
    DrawSphere(jointPosition, 0.09f, YELLOW);
    DrawSphereWires(jointPosition, 0.16f, 12, 8, BLACK);
}

static void drawFighter(const pf::World& world, const pf::FighterRuntime& fighter, Color color, bool boxes) {
    const pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const Vector3 pos = toRayGround(fighter.position);
    const bool hasAnimationPose = !fighter.hsdJointWorldPositions.empty() && animationSkeletonForDrawing(def) != nullptr;
    const bool hasImportedPose = def.hsdAsset && hasAnimationPose;
    if (fighter.fighterInvisible) {
        // ftDrawCommon skips fighter model display when x221E_b5 is set.
    } else if (hasImportedPose) {
        if (!def.hsdAsset->mesh.batches.empty()) {
            drawImportedMesh(def, fighter);
        } else {
            DrawCylinder(pos, 0.18f, 0.18f, 0.04f, 18, Fade(color, 0.45f));
            drawAnimationSkeleton(def, fighter, color);
        }
    } else if (hasAnimationPose) {
        if (!def.authoredMesh.batches.empty()) {
            drawAuthoredMesh(def, fighter);
        } else {
            DrawCylinder(pos, 0.18f, 0.18f, 0.04f, 18, Fade(color, 0.45f));
            drawAnimationSkeleton(def, fighter, color);
        }
    } else {
        if (!def.authoredMesh.batches.empty()) {
            drawAuthoredMesh(def, fighter);
        } else {
            DrawCube(pos, 0.55f, 1.1f, 0.35f, color);
            DrawCubeWires(pos, 0.55f, 1.1f, 0.35f, BLACK);
        }
    }

    if (!def.hasHsdAsset && (!hasAnimationPose || boxes)) {
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
    drawImportedEcbSources(def, fighter);
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
    drawAnimationSkeleton(def, fighter, DARKGREEN);
    for (const pf::ActiveHitbox& hit : fighter.activeHitboxes) {
        drawCapsule(hit.previous, hit.current, hit.def.radius, RED);
    }
}

static void drawGameObjects(const pf::World& world, bool boxes) {
    for (const pf::GameObjectRuntime& object : world.objects) {
        if (!object.active || object.objectDef < 0 || object.objectDef >= static_cast<int>(world.objectDefs.size())) {
            continue;
        }
        const pf::GameObjectDefinition& def = world.objectDefs[static_cast<size_t>(object.objectDef)];
        const Color color = def.kind == pf::GameObjectKind::Projectile ? MAROON : BROWN;
        DrawSphere(toRayGround(object.position), def.kind == pf::GameObjectKind::Projectile ? 0.14f : 0.24f, color);
        DrawSphereWires(toRayGround(object.position), def.kind == pf::GameObjectKind::Projectile ? 0.16f : 0.27f, 10, 6, BLACK);
        if (!boxes) {
            continue;
        }
        for (const pf::ActiveHitbox& hit : object.activeHitboxes) {
            drawCapsule(hit.previous, hit.current, hit.def.radius, RED);
        }
        for (const pf::GameObjectHurtboxDefinition& hurtbox : def.hurtboxes) {
            drawCapsule(
                {object.position.x + hurtbox.startOffset.x, object.position.y + hurtbox.startOffset.y, hurtbox.startOffset.z},
                {object.position.x + hurtbox.endOffset.x, object.position.y + hurtbox.endOffset.y, hurtbox.endOffset.z},
                hurtbox.radius,
                BLUE);
        }
        for (const pf::GameObjectTouchboxDefinition& touchbox : def.touchboxes) {
            drawCapsule(
                {object.position.x + touchbox.startOffset.x, object.position.y + touchbox.startOffset.y, touchbox.startOffset.z},
                {object.position.x + touchbox.endOffset.x, object.position.y + touchbox.endOffset.y, touchbox.endOffset.z},
                touchbox.radius,
                SKYBLUE);
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

static bool packageScriptInstructionTargetsFighter(const pf::PackageScriptInstruction& instruction) {
    return instruction.op == pf::PackageScriptOp::SwitchFighterDefinition ||
        instruction.op == pf::PackageScriptOp::SpawnFighter;
}

static const pf::FighterDefinition* fighterDefinitionByName(const pf::World& world, const std::string& name) {
    const auto found = std::find_if(world.fighterDefs.begin(), world.fighterDefs.end(), [&](const pf::FighterDefinition& candidate) {
        return candidate.name == name;
    });
    return found == world.fighterDefs.end() ? nullptr : &*found;
}

static bool hasPackagedFighter(const std::vector<pf::FighterDefinition>& fighters, const std::string& name) {
    return std::any_of(fighters.begin(), fighters.end(), [&](const pf::FighterDefinition& fighter) {
        return fighter.name == name;
    });
}

static std::vector<pf::FighterDefinition> collectEditorPackageFighters(const pf::World& world, const pf::FighterDefinition& root) {
    std::vector<pf::FighterDefinition> fighters;
    fighters.push_back(root);
    for (size_t scan = 0; scan < fighters.size(); ++scan) {
        const pf::FighterDefinition& fighter = fighters[scan];
        for (const pf::PackageScript& script : fighter.packageScripts) {
            for (const pf::PackageScriptInstruction& instruction : script.instructions) {
                if (!packageScriptInstructionTargetsFighter(instruction) ||
                    instruction.text.empty() ||
                    hasPackagedFighter(fighters, instruction.text))
                {
                    continue;
                }
                if (const pf::FighterDefinition* dependency = fighterDefinitionByName(world, instruction.text)) {
                    fighters.push_back(*dependency);
                }
            }
        }
    }
    return fighters;
}

static void collectEditorPackageAssets(const std::vector<pf::FighterDefinition>& fighters, pf::FighterPackage& package) {
    for (const pf::FighterDefinition& fighter : fighters) {
        if (!fighter.hsdAsset) {
            continue;
        }
        if (std::find(package.hsdAssets.begin(), package.hsdAssets.end(), fighter.hsdAsset) == package.hsdAssets.end()) {
            package.hsdAssets.push_back(fighter.hsdAsset);
        }
    }
}

static pf::FighterPackage makeEditorPackage(const pf::World& world, const pf::FighterDefinition& root) {
    pf::FighterPackage package;
    package.name = root.name + "_editor";
    package.fighters = collectEditorPackageFighters(world, root);
    collectEditorPackageAssets(package.fighters, package);
    package.objects = world.objectDefs;
    return package;
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
    editor.lastPackageAssets = static_cast<int>(package.hsdAssets.size());
    editor.lastPackageValid = true;
    editor.lastPackageMessage = "OK";
}

static void updateEditorPackageFailure(pf::FighterEditor& editor, const std::string& message) {
    editor.lastPackageValid = false;
    editor.lastPackageMessage = message;
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
    const Rectangle panel{12.0f, 324.0f, 530.0f, 330.0f};
    DrawRectangleRec(panel, Fade(RAYWHITE, 0.58f));
    DrawRectangleLinesEx(panel, 1.0f, DARKGRAY);
    DrawText("Package Assets", 24, 336, 16, BLACK);
    DrawText(("File: " + editor.packagePath).c_str(), 24, 360, 13, DARKGRAY);
    const size_t assetBytes = def.hsdAsset ? def.hsdAsset->sourceBytes.size() : 0;
    DrawText(("Embedded HSD bytes: " + std::to_string(assetBytes)).c_str(), 24, 382, 13, DARKGRAY);
    DrawText(("Objects/articles in package: " + std::to_string(world.objectDefs.size())).c_str(), 24, 404, 13, DARKGRAY);
    DrawText(("Authored mesh batches: " + std::to_string(def.authoredMesh.batches.size())).c_str(), 24, 426, 13, DARKGRAY);
    if (editor.lastPackageBytes > 0) {
        DrawText(("Last package: " + editor.lastPackageName +
                  " bytes=" + std::to_string(editor.lastPackageBytes) +
                  " checksum=" + std::to_string(editor.lastPackageChecksum)).c_str(), 24, 448, 12, DARKGRAY);
        DrawText(("Contents: fighters=" + std::to_string(editor.lastPackageFighters) +
                  " objects=" + std::to_string(editor.lastPackageObjects) +
                  " assets=" + std::to_string(editor.lastPackageAssets)).c_str(), 24, 466, 12, DARKGRAY);
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
        pf::FighterPackage package = makeEditorPackage(world, def);
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
        pf::FighterPackage package = makeEditorPackage(world, def);
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
        pf::HsdMeshVertex* selectedVertex = authoredMeshVertexAt(
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
    std::vector<pf::AnimationClip>* authoredClips = &def.authoredClips;
    const std::vector<pf::AnimationClip>* clips = def.hsdAsset && !def.hsdAsset->clips.empty()
        ? &def.hsdAsset->clips
        : authoredClips;
    const bool showingAuthoredClips = clips == authoredClips;
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

    DrawText(("Clips: " + std::to_string(clips->size()) + (clips == authoredClips ? " authored" : " imported")).c_str(), 24, 362, 13, DARKGRAY);
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
    if (def.hsdAsset && !def.hsdAsset->clips.empty()) {
        return &def.hsdAsset->clips;
    }
    return def.authoredClips.empty() ? nullptr : &def.authoredClips;
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
    if (world.fighters.empty() || def.states.empty()) {
        editor.status = "Editor: no state available to preview";
        return;
    }
    editor.selectedFighter = std::clamp(editor.selectedFighter, 0, static_cast<int>(world.fighters.size()) - 1);
    pf::FighterRuntime& fighter = world.fighters[static_cast<size_t>(editor.selectedFighter)];
    editor.selectedState = std::clamp(editor.selectedState, 0, static_cast<int>(def.states.size()) - 1);
    const pf::FighterState& selectedState = def.states[static_cast<size_t>(editor.selectedState)];
    pf::changeFighterState(world, fighter, selectedState.name, 0, pf::kDisableAnimationBlendFrames);
    editor.animationPreviewActive = false;
    editor.paused = true;
    editor.status = "Editor: previewing selected state " + selectedState.name;
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
    DrawText(("Animation pose: " + std::to_string(fighter.hsdJointWorldPositions.size()) + " joints, " +
              std::to_string(fighter.hsdHurtboxCapsules.size()) + " hurtboxes").c_str(), 24, 182, 14, DARKGRAY);

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
    const pf::FighterDefinition editedFighter = world.fighterDefs[static_cast<size_t>(editedFighterDef)];
    const std::vector<pf::GameObjectDefinition> editedObjects = world.objectDefs;
    const int sandbagFighterDef = fighterDefByName(world, "Sandbag", 0);
    world = pf::makeTrainingWorld(0, sandbagFighterDef);
    int testFighterIndex = editedFighterDef;
    if (testFighterIndex >= 0 && testFighterIndex < static_cast<int>(world.fighterDefs.size()) &&
        world.fighterDefs[static_cast<size_t>(testFighterIndex)].name == editedFighter.name)
    {
        world.fighterDefs[static_cast<size_t>(testFighterIndex)] = editedFighter;
    } else {
        world.fighterDefs.push_back(editedFighter);
        testFighterIndex = static_cast<int>(world.fighterDefs.size()) - 1;
    }
    world.objectDefs = editedObjects;
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
        ? "Editor test: live unsaved fighter data loaded on Battlefield with Sandbag"
        : "Editor test: live unsaved fighter data loaded on Battlefield; player 2 is inert";
}

int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, "PFighter C++ raylib prototype");
    if (!IsWindowReady()) {
        return 1;
    }
    SetTargetFPS(60);

    Camera3D camera{};
    camera.position = {0.0f, 55.0f, 145.0f};
    camera.target = {0.0f, 10.0f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    AppMode appMode = AppMode::MainMenu;
    int testFighterDef = 0;
    pf::World world = pf::makeTrainingWorld(testFighterDef, testFighterDef);
    pf::FighterEditor editor;
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

    while (!WindowShouldClose()) {
        if (appMode != AppMode::MainMenu) {
            updateTickrateControl(tickrate);
        }
        const bool editorTextEditing = appMode == AppMode::Editor && !editor.activeTextField.empty();
        const bool globalShortcutsEnabled = !editorTextEditing;
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
            createEditorState(world, editor);
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && (IsKeyPressed(KEY_DELETE) || deleteStateClicked)) {
            removeEditorState(world, editor);
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && (IsKeyPressed(KEY_T) || testClicked)) {
            launchEditorTestWorld(world, editor, replay, testFighterDef);
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
            editor.testMode = false;
            editor.animationPreviewActive = false;
            editor.status = "Editor: reset Battlefield from saved/base fighter data";
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && IsKeyPressed(KEY_LEFT_BRACKET)) --editor.selectedState;
        if (globalShortcutsEnabled && appMode == AppMode::Editor && IsKeyPressed(KEY_RIGHT_BRACKET)) ++editor.selectedState;
        const bool shiftHeld = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (globalShortcutsEnabled && appMode == AppMode::Editor && IsKeyPressed(KEY_COMMA)) {
            if (shiftHeld && editor.workspace == pf::EditorWorkspace::Moveset) {
                if (pf::FighterState* state = selectedEditorState()) {
                    moveEditorSubaction(*state, editor, -1);
                }
            } else {
                --editor.selectedSubaction;
            }
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && IsKeyPressed(KEY_PERIOD)) {
            if (shiftHeld && editor.workspace == pf::EditorWorkspace::Moveset) {
                if (pf::FighterState* state = selectedEditorState()) {
                    moveEditorSubaction(*state, editor, 1);
                }
            } else {
                ++editor.selectedSubaction;
            }
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && editor.workspace == pf::EditorWorkspace::Moveset && IsKeyPressed(KEY_BACKSPACE)) {
            removeSelectedSubaction();
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && editor.workspace == pf::EditorWorkspace::Moveset && IsKeyPressed(KEY_MINUS)) {
            adjustSelectedSubactionFrames(-1);
        }
        if (globalShortcutsEnabled && appMode == AppMode::Editor && editor.workspace == pf::EditorWorkspace::Moveset && IsKeyPressed(KEY_EQUAL)) {
            adjustSelectedSubactionFrames(1);
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

        if (globalShortcutsEnabled && appMode != AppMode::MainMenu && IsKeyPressed(KEY_F5)) {
            beginReplayRecording(replay, world, testFighterDef, testFighterDef);
            editor.paused = false;
        }
        if (globalShortcutsEnabled && appMode != AppMode::MainMenu && IsKeyPressed(KEY_F6)) {
            saveReplayRecording(replay);
        }
        if (globalShortcutsEnabled && appMode != AppMode::MainMenu && IsKeyPressed(KEY_F7)) {
            loadReplayPlayback(replay, world, testFighterDef);
            editor.paused = true;
        }
        if (globalShortcutsEnabled && appMode != AppMode::MainMenu && IsKeyPressed(KEY_F8)) {
            stepReplayPlayback(replay, world);
        }
        if (globalShortcutsEnabled && appMode != AppMode::MainMenu && IsKeyPressed(KEY_F9) && replay.playbackLoaded) {
            replay.realtimePlayback = !replay.realtimePlayback;
            editor.paused = !replay.realtimePlayback;
        }

        const pf::InputFrame p1Input = editorTextEditing ? pf::InputFrame{} : readPlayerInput(0, false);
        const pf::InputFrame p2Input = editorTextEditing || editor.testMode ? pf::InputFrame{} : readPlayerInput(1, true);
        const bool simRunning = appMode != AppMode::MainMenu && ((replay.playbackLoaded && replay.realtimePlayback) || !editor.paused);
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
        if (appMode == AppMode::MainMenu) {
            drawMainMenu(appMode);
        } else {
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
                    drawLedgeSnapSweep(world.fighterDefs[static_cast<size_t>(world.fighters[0].fighterDef)], world.fighters[0], ledge, Fade(ORANGE, 0.45f));
                    drawLedgeSnapSweep(world.fighterDefs[static_cast<size_t>(world.fighters[1].fighterDef)], world.fighters[1], ledge, Fade(SKYBLUE, 0.45f));
                }
            }
            drawFighter(world, world.fighters[0], ORANGE, editor.showBoxes);
            drawFighter(world, world.fighters[1], SKYBLUE, editor.showBoxes);
            drawGameObjects(world, editor.showBoxes);
            if (appMode == AppMode::Editor) {
                drawEditorSelectedAuthoredMeshVertex(world, editor);
                drawEditorSelectedAuthoredJoint(world, editor);
            }
            EndMode3D();
            if (appMode == AppMode::Editor) {
                drawEditor(world, editor, testFighterDef);
            }
            drawReplayStatus(replay);
            drawTickrateControl(tickrate);
        }
        drawTopNav(appMode);
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
#endif
