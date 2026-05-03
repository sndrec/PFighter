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
#include <cstddef>
#include <cmath>
#include <memory>
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
        return PURPLE;
    case pf::SubactionType::SetHurtboxState:
    case pf::SubactionType::SetBodyCollisionState:
        return BLUE;
    default:
        return ORANGE;
    }
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
    case pf::PackageScriptOp::AddVarImmediate: return "AddVar";
    case pf::PackageScriptOp::AddVar: return "AddVars";
    case pf::PackageScriptOp::SetGroundVelocity: return "GroundVel";
    case pf::PackageScriptOp::SetAirVelocityX: return "AirVelX";
    case pf::PackageScriptOp::SetAirVelocityY: return "AirVelY";
    case pf::PackageScriptOp::SetFacing: return "Facing";
    case pf::PackageScriptOp::ChangeState: return "State";
    case pf::PackageScriptOp::SpawnObject: return "Spawn";
    }
    return "Op";
}

static std::string packageInstructionLabel(const pf::PackageScriptInstruction& instruction) {
    std::string label = packageScriptOpName(instruction.op);
    switch (instruction.op) {
    case pf::PackageScriptOp::SetVarImmediate:
    case pf::PackageScriptOp::AddVarImmediate:
        label += " v" + std::to_string(instruction.dst) + " " + std::to_string(instruction.intValue);
        break;
    case pf::PackageScriptOp::AddVar:
        label += " v" + std::to_string(instruction.dst) + " = v" + std::to_string(instruction.srcA) + " + v" + std::to_string(instruction.srcB);
        break;
    case pf::PackageScriptOp::SetGroundVelocity:
    case pf::PackageScriptOp::SetAirVelocityX:
    case pf::PackageScriptOp::SetAirVelocityY:
        label += " " + std::to_string(pf::fxToFloat(instruction.fixValue));
        break;
    case pf::PackageScriptOp::SetFacing:
        label += instruction.intValue < 0 ? " left" : " right";
        break;
    case pf::PackageScriptOp::ChangeState:
        label += " " + instruction.text;
        break;
    case pf::PackageScriptOp::SpawnObject:
        label += " " + instruction.text;
        break;
    case pf::PackageScriptOp::Nop:
        break;
    }
    return label;
}

static bool uiListRow(Rectangle rect, const std::string& label, bool active) {
    const Vector2 mouse = GetMousePosition();
    const bool hovered = CheckCollisionPointRec(mouse, rect);
    DrawRectangleRec(rect, active ? Fade(GREEN, 0.72f) : (hovered ? Fade(ORANGE, 0.5f) : Fade(RAYWHITE, 0.62f)));
    DrawRectangleLinesEx(rect, 1.0f, Fade(DARKGRAY, 0.75f));
    DrawText(label.c_str(), static_cast<int>(rect.x + 7.0f), static_cast<int>(rect.y + 5.0f), 13, BLACK);
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

static std::string uniquePackageScriptName(const pf::FighterDefinition& def) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = "Script" + std::to_string(index);
        const bool exists = std::any_of(def.packageScripts.begin(), def.packageScripts.end(), [&](const pf::PackageScript& script) {
            return script.name == candidate;
        });
        if (!exists) {
            return candidate;
        }
    }
    return "ScriptX";
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
    def.hurtboxes = {
        {pf::BoneId::Hip, {0, pf::fxFromFloat(-0.45f), 0}, {0, pf::fxFromFloat(0.55f), 0}, pf::fxFromFloat(0.45f), pf::HurtboxState::Normal, true},
        {pf::BoneId::Head, {0, pf::fxFromFloat(-0.2f), 0}, {0, pf::fxFromFloat(0.2f), 0}, pf::fxFromFloat(0.32f), pf::HurtboxState::Normal, true},
    };

    pf::FighterState wait;
    wait.name = "Wait";
    wait.animation = "Wait";
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
    if (fighter.fighterInvisible) {
        // ftDrawCommon skips fighter model display when x221E_b5 is set.
    } else if (hasImportedPose) {
        if (!def.hsdAsset->mesh.batches.empty()) {
            drawImportedMesh(def, fighter);
        } else {
            DrawCylinder(pos, 0.18f, 0.18f, 0.04f, 18, Fade(color, 0.45f));
            drawImportedSkeleton(def, fighter, color);
        }
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
    drawImportedSkeleton(def, fighter, DARKGREEN);
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
    const Rectangle panel{12.0f, 324.0f, 530.0f, 330.0f};
    DrawRectangleRec(panel, Fade(RAYWHITE, 0.58f));
    DrawRectangleLinesEx(panel, 1.0f, DARKGRAY);
    DrawText("Package Logic", 24, 336, 16, BLACK);
    DrawText(("State hooks: enter " + std::to_string(state.onEnter.size()) +
              "  frame " + std::to_string(state.onFrame.size())).c_str(), 24, 358, 13, DARKGRAY);

    if (uiButton({338.0f, 334.0f, 58.0f, 24.0f}, "+ Var")) {
        def.packageVariables.push_back({uniquePackageVariableName(def), 0});
        editor.selectedPackageVariable = static_cast<int>(def.packageVariables.size()) - 1;
        editor.status = "Editor: added package variable " + def.packageVariables.back().name;
    }
    if (uiButton({402.0f, 334.0f, 58.0f, 24.0f}, "- Var")) {
        if (!def.packageVariables.empty()) {
            const std::string removed = def.packageVariables[static_cast<size_t>(editor.selectedPackageVariable)].name;
            def.packageVariables.erase(def.packageVariables.begin() + editor.selectedPackageVariable);
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
            editor.selectedPackageScript = std::clamp(editor.selectedPackageScript, 0, std::max(0, static_cast<int>(def.packageScripts.size()) - 1));
            editor.selectedPackageInstruction = 0;
            editor.status = "Editor: removed package script " + removed;
        }
    }

    DrawText("Vars", 24, 392, 13, DARKGRAY);
    const int visibleVars = std::min(5, static_cast<int>(def.packageVariables.size()));
    for (int row = 0; row < visibleVars; ++row) {
        const pf::PackageVariableDefinition& variable = def.packageVariables[static_cast<size_t>(row)];
        const std::string label = variable.name + " = " + std::to_string(variable.initialValue);
        if (uiListRow({24.0f, 412.0f + 24.0f * row, 156.0f, 22.0f}, label, row == editor.selectedPackageVariable)) {
            editor.selectedPackageVariable = row;
        }
    }
    if (def.packageVariables.empty()) {
        DrawText("No variables", 31, 417, 13, GRAY);
    }

    DrawText("Scripts", 196, 392, 13, DARKGRAY);
    const int visibleScripts = std::min(5, static_cast<int>(def.packageScripts.size()));
    for (int row = 0; row < visibleScripts; ++row) {
        const pf::PackageScript& script = def.packageScripts[static_cast<size_t>(row)];
        const std::string label = script.name + " (" + std::to_string(script.instructions.size()) + ")";
        if (uiListRow({196.0f, 412.0f + 24.0f * row, 156.0f, 22.0f}, label, row == editor.selectedPackageScript)) {
            editor.selectedPackageScript = row;
            editor.selectedPackageInstruction = 0;
        }
    }
    if (def.packageScripts.empty()) {
        DrawText("No scripts", 203, 417, 13, GRAY);
    }

    pf::PackageScript* script = nullptr;
    if (!def.packageScripts.empty()) {
        script = &def.packageScripts[static_cast<size_t>(editor.selectedPackageScript)];
    }
    DrawText("Instructions", 24, 506, 13, DARKGRAY);
    if (script) {
        const int visibleInstructions = std::min(2, static_cast<int>(script->instructions.size()));
        for (int row = 0; row < visibleInstructions; ++row) {
            const pf::PackageScriptInstruction& instruction = script->instructions[static_cast<size_t>(row)];
            if (uiListRow({106.0f, 500.0f + 24.0f * row, 246.0f, 22.0f}, packageInstructionLabel(instruction), row == editor.selectedPackageInstruction)) {
                editor.selectedPackageInstruction = row;
            }
        }
        if (script->instructions.empty()) {
            DrawText("No instructions", 113, 505, 13, GRAY);
        }
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
    const Rectangle panel{12.0f, 324.0f, 530.0f, 260.0f};
    DrawRectangleRec(panel, Fade(RAYWHITE, 0.58f));
    DrawRectangleLinesEx(panel, 1.0f, DARKGRAY);
    DrawText("Package Assets", 24, 336, 16, BLACK);
    DrawText(("File: " + editor.packagePath).c_str(), 24, 360, 13, DARKGRAY);
    const size_t assetBytes = def.hsdAsset ? def.hsdAsset->sourceBytes.size() : 0;
    DrawText(("Embedded HSD bytes: " + std::to_string(assetBytes)).c_str(), 24, 382, 13, DARKGRAY);
    DrawText(("Objects/articles in package: " + std::to_string(world.objectDefs.size())).c_str(), 24, 404, 13, DARKGRAY);

    if (uiButton({338.0f, 338.0f, 82.0f, 26.0f}, "Save Pkg")) {
        pf::FighterPackage package;
        package.name = def.name + "_editor";
        if (def.hsdAsset) {
            package.hsdAssets = {def.hsdAsset};
        }
        package.fighters = {def};
        package.objects = world.objectDefs;
        std::string error;
        if (pf::saveFighterPackage(editor.packagePath, package, &error)) {
            const std::vector<uint8_t> bytes = pf::writeFighterPackage(package, &error);
            editor.status = "Editor: saved " + editor.packagePath + " bytes=" + std::to_string(bytes.size());
        } else {
            editor.status = "Editor package save failed: " + error;
        }
    }
    if (uiButton({430.0f, 338.0f, 82.0f, 26.0f}, "Load Pkg")) {
        pf::FighterPackage package;
        std::string error;
        if (!pf::loadFighterPackage(editor.packagePath, package, &error)) {
            editor.status = "Editor package load failed: " + error;
        } else if (package.fighters.empty()) {
            editor.status = "Editor package load failed: no fighters in package";
        } else {
            const int fighterDef = fighter.fighterDef;
            world.fighterDefs[static_cast<size_t>(fighterDef)] = package.fighters.front();
            if (!package.objects.empty()) {
                world.objectDefs = package.objects;
            }
            editor.selectedState = 0;
            editor.selectedSubaction = 0;
            editor.selectedPackageVariable = 0;
            editor.selectedPackageScript = 0;
            editor.selectedPackageInstruction = 0;
            editor.status = "Editor: loaded package fighter " + world.fighterDefs[static_cast<size_t>(fighterDef)].name;
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

    DrawText("Objects / Articles", 24, 436, 13, DARKGRAY);
    const int visibleObjects = std::min(5, static_cast<int>(world.objectDefs.size()));
    for (int row = 0; row < visibleObjects; ++row) {
        const pf::GameObjectDefinition& object = world.objectDefs[static_cast<size_t>(row)];
        const std::string kind = object.kind == pf::GameObjectKind::Projectile ? "Proj" : "Item";
        const std::string label = kind + " " + object.name;
        if (uiListRow({24.0f, 456.0f + 24.0f * row, 230.0f, 22.0f}, label, row == editor.selectedObjectDef)) {
            editor.selectedObjectDef = row;
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
    if (uiButton({438.0f, 456.0f, 76.0f, 24.0f}, "Spawn")) {
        if (!world.objectDefs.empty()) {
            editor.selectedObjectDef = std::clamp(editor.selectedObjectDef, 0, static_cast<int>(world.objectDefs.size()) - 1);
            const pf::GameObjectDefinition& object = world.objectDefs[static_cast<size_t>(editor.selectedObjectDef)];
            pf::Subaction spawn;
            spawn.type = pf::SubactionType::SpawnObject;
            spawn.frames = 1;
            spawn.objectName = object.name;
            spawn.spawnOffset = {pf::fxFromFloat(0.75f), pf::fxFromFloat(0.7f), 0};
            spawn.spawnVelocity = {pf::fxFromFloat(1.0f), pf::fxFromFloat(0.2f)};
            pf::FighterState& state = def.states[static_cast<size_t>(editor.selectedState)];
            state.action.push_back(std::move(spawn));
            editor.selectedSubaction = static_cast<int>(state.action.size()) - 1;
            editor.status = "Editor: added SpawnObject subaction for " + object.name;
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
    const pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const Rectangle panel{12.0f, 324.0f, 530.0f, 260.0f};
    DrawRectangleRec(panel, Fade(RAYWHITE, 0.58f));
    DrawRectangleLinesEx(panel, 1.0f, DARKGRAY);
    DrawText("Animation Preview", 24, 336, 16, BLACK);
    if (!def.hsdAsset || def.hsdAsset->clips.empty()) {
        DrawText("No imported animation clips on this fighter", 24, 364, 13, GRAY);
        return;
    }

    editor.selectedAnimationClip = std::clamp(
        editor.selectedAnimationClip,
        0,
        static_cast<int>(def.hsdAsset->clips.size()) - 1);
    const pf::AnimationClip& selectedClip = def.hsdAsset->clips[static_cast<size_t>(editor.selectedAnimationClip)];
    const int maxFrame = std::max(0, static_cast<int>(pf::fxToFloat(selectedClip.frameCount)) - 1);
    editor.animationScrubFrame = std::clamp(editor.animationScrubFrame, 0, maxFrame);

    DrawText(("Clips: " + std::to_string(def.hsdAsset->clips.size())).c_str(), 24, 362, 13, DARKGRAY);
    DrawText(("Selected: action " + std::to_string(selectedClip.actionIndex) +
              " frame " + std::to_string(editor.animationScrubFrame) + "/" + std::to_string(maxFrame)).c_str(), 24, 382, 13, DARKGRAY);
    const int visibleClips = std::min(6, static_cast<int>(def.hsdAsset->clips.size()));
    for (int row = 0; row < visibleClips; ++row) {
        const pf::AnimationClip& clip = def.hsdAsset->clips[static_cast<size_t>(row)];
        const std::string label = std::to_string(clip.actionIndex) + " " + clip.name;
        if (uiListRow({24.0f, 410.0f + 24.0f * row, 310.0f, 22.0f}, label, row == editor.selectedAnimationClip)) {
            editor.selectedAnimationClip = row;
            editor.animationScrubFrame = 0;
            editor.animationPreviewActive = true;
            const bool ok = pf::previewFighterAnimation(world, static_cast<size_t>(editor.selectedFighter), clip.actionIndex, 0);
            editor.status = ok ? "Editor: previewing animation " + clip.name : "Editor: animation preview failed";
            editor.paused = true;
        }
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
            DrawText("Preview pose applied to selected fighter", 352, 476, 13, DARKGRAY);
        }
    }
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
        editor.status = "Editor: added Wait interrupt to " + state.name;
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
            " r=" + std::to_string(pf::fxToFloat(hurtbox.radius)) +
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
    } else {
        DrawText("No authored hurtboxes", 24, 532, 13, GRAY);
    }

    DrawText(("Authored ECB: " + std::string(def.authoredEcb.enabled ? "on" : "off")).c_str(), 24, 590, 13, DARKGRAY);
    if (uiButton({122.0f, 584.0f, 72.0f, 24.0f}, "ECB", def.authoredEcb.enabled)) {
        def.authoredEcb.enabled = !def.authoredEcb.enabled;
        editor.status = def.authoredEcb.enabled ? "Editor: enabled authored ECB" : "Editor: disabled authored ECB";
        pf::calculateEcb(def, fighter, true);
    }
    if (def.authoredEcb.enabled) {
        const float halfWidth = pf::fxToFloat(def.authoredEcb.points[2].x);
        const float top = pf::fxToFloat(def.authoredEcb.points[1].y);
        const float bottom = pf::fxToFloat(def.authoredEcb.points[3].y);
        DrawText(("w=" + std::to_string(halfWidth * 2.0f) +
                  " top=" + std::to_string(top) +
                  " bot=" + std::to_string(bottom)).c_str(), 24, 620, 13, DARKGRAY);
        if (uiButton({202.0f, 584.0f, 54.0f, 24.0f}, "+ W")) {
            def.authoredEcb.points[0].x -= pf::fxFromFloat(0.05f);
            def.authoredEcb.points[2].x += pf::fxFromFloat(0.05f);
            pf::calculateEcb(def, fighter, true);
            editor.status = "Editor: widened authored ECB";
        }
        if (uiButton({262.0f, 584.0f, 54.0f, 24.0f}, "- W")) {
            def.authoredEcb.points[0].x = std::min(def.authoredEcb.points[0].x + pf::fxFromFloat(0.05f), -pf::fxFromFloat(0.1f));
            def.authoredEcb.points[2].x = std::max(def.authoredEcb.points[2].x - pf::fxFromFloat(0.05f), pf::fxFromFloat(0.1f));
            pf::calculateEcb(def, fighter, true);
            editor.status = "Editor: narrowed authored ECB";
        }
        if (uiButton({322.0f, 584.0f, 54.0f, 24.0f}, "+ Top")) {
            def.authoredEcb.points[1].y += pf::fxFromFloat(0.1f);
            pf::calculateEcb(def, fighter, true);
            editor.status = "Editor: raised authored ECB top";
        }
        if (uiButton({382.0f, 584.0f, 54.0f, 24.0f}, "- Top")) {
            def.authoredEcb.points[1].y = std::max(def.authoredEcb.points[3].y + pf::fxFromFloat(0.5f), def.authoredEcb.points[1].y - pf::fxFromFloat(0.1f));
            pf::calculateEcb(def, fighter, true);
            editor.status = "Editor: lowered authored ECB top";
        }
        if (uiButton({442.0f, 584.0f, 54.0f, 24.0f}, "Up")) {
            def.authoredEcb.points[3].y += pf::fxFromFloat(0.05f);
            pf::calculateEcb(def, fighter, true);
            editor.status = "Editor: raised authored ECB bottom";
        }
    }
}

static void drawEditor(pf::World& world, pf::FighterEditor& editor, int& selectedFighterDef) {
    editor.clampToWorld(world);
    const pf::FighterRuntime& fighter = world.fighters[static_cast<size_t>(editor.selectedFighter)];
    const pf::FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    const pf::FighterState& state = def.states[static_cast<size_t>(editor.selectedState)];
    const pf::UnfoldedAction actionFrames = pf::unfoldAction(state.action);
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
    DrawText(("Fighter: " + def.name).c_str(), 24, 54, 16, DARKGRAY);
    DrawText(("Live state: " + pf::currentState(world, fighter).name).c_str(), 24, 76, 16, DARKGRAY);
    DrawText(("Selected state: " + state.name).c_str(), 24, 98, 16, DARKGRAY);
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
    DrawText(("HSD pose: " + std::to_string(fighter.hsdJointWorldPositions.size()) + " joints, " +
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
    const float liveX = timelineX + timelineWidth * static_cast<float>(std::clamp(liveFrame, 0, timelineFrameCount)) / static_cast<float>(timelineFrameCount);
    DrawRectangle(static_cast<int>(liveX), static_cast<int>(timelineY - 3.0f), 3, static_cast<int>(timelineHeight + 6.0f), BLACK);
    DrawText(editor.status.c_str(), 24, 240, 14, DARKGRAY);
    DrawText("N/New state  Del/remove  T/Test playtest  [/] state  ,/. subaction  Space pause  R reset", 24, 258, 14, GRAY);
    drawEditorWorkspaceTabs(editor);
    if (editor.workspace == pf::EditorWorkspace::Moveset) {
        drawEditorMovesetWorkspace(world, editor);
    } else if (editor.workspace == pf::EditorWorkspace::Logic) {
        drawEditorLogicWorkspace(world, editor);
    } else if (editor.workspace == pf::EditorWorkspace::Assets) {
        drawEditorAssetsWorkspace(world, editor, selectedFighterDef);
    } else if (editor.workspace == pf::EditorWorkspace::Animation) {
        drawEditorAnimationWorkspace(world, editor);
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

    while (!WindowShouldClose()) {
        if (appMode != AppMode::MainMenu) {
            updateTickrateControl(tickrate);
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            appMode = AppMode::MainMenu;
            replay.realtimePlayback = false;
            editor.paused = true;
        }
        const bool testClicked = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(GetMousePosition(), editorTestButtonRect());
        const bool newStateClicked = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(GetMousePosition(), editorNewStateButtonRect());
        const bool deleteStateClicked = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(GetMousePosition(), editorDeleteStateButtonRect());
        if (appMode == AppMode::Editor && (IsKeyPressed(KEY_N) || newStateClicked)) {
            createEditorState(world, editor);
        }
        if (appMode == AppMode::Editor && (IsKeyPressed(KEY_DELETE) || deleteStateClicked)) {
            removeEditorState(world, editor);
        }
        if (appMode == AppMode::Editor && (IsKeyPressed(KEY_T) || testClicked)) {
            launchEditorTestWorld(world, editor, replay, testFighterDef);
        }
        if (appMode != AppMode::MainMenu && IsKeyPressed(KEY_F1)) editor.showBoxes = !editor.showBoxes;
        if (appMode != AppMode::MainMenu && IsKeyPressed(KEY_F2)) editor.sideView = !editor.sideView;
        if (appMode != AppMode::MainMenu && IsKeyPressed(KEY_SPACE)) {
            editor.paused = !editor.paused;
            if (editor.paused) {
                replay.realtimePlayback = false;
            }
        }
        if (appMode != AppMode::MainMenu && IsKeyPressed(KEY_R)) {
            world = pf::makeTrainingWorld(testFighterDef, testFighterDef);
            replay.playbackFrame = 0;
            replay.playbackLoaded = false;
            replay.realtimePlayback = false;
            replay.recordingActive = false;
            editor.testMode = false;
            editor.animationPreviewActive = false;
            editor.status = "Editor: reset Battlefield from saved/base fighter data";
        }
        if (appMode == AppMode::Editor && IsKeyPressed(KEY_LEFT_BRACKET)) --editor.selectedState;
        if (appMode == AppMode::Editor && IsKeyPressed(KEY_RIGHT_BRACKET)) ++editor.selectedState;
        if (appMode == AppMode::Editor && IsKeyPressed(KEY_COMMA)) --editor.selectedSubaction;
        if (appMode == AppMode::Editor && IsKeyPressed(KEY_PERIOD)) ++editor.selectedSubaction;
        if (appMode != AppMode::MainMenu && IsKeyPressed(KEY_LEFT)) selectFighterDef(testFighterDef - 1);
        if (appMode != AppMode::MainMenu && IsKeyPressed(KEY_RIGHT)) selectFighterDef(testFighterDef + 1);
        for (size_t i = 0; i < fighterKeys.size(); ++i) {
            if (appMode != AppMode::MainMenu && IsKeyPressed(fighterKeys[i]) && i < world.fighterDefs.size()) {
                selectFighterDef(static_cast<int>(i));
            }
        }

        if (appMode != AppMode::MainMenu && IsKeyPressed(KEY_F5)) {
            beginReplayRecording(replay, world, testFighterDef, testFighterDef);
            editor.paused = false;
        }
        if (appMode != AppMode::MainMenu && IsKeyPressed(KEY_F6)) {
            saveReplayRecording(replay);
        }
        if (appMode != AppMode::MainMenu && IsKeyPressed(KEY_F7)) {
            loadReplayPlayback(replay, world, testFighterDef);
            editor.paused = true;
        }
        if (appMode != AppMode::MainMenu && IsKeyPressed(KEY_F8)) {
            stepReplayPlayback(replay, world);
        }
        if (appMode != AppMode::MainMenu && IsKeyPressed(KEY_F9) && replay.playbackLoaded) {
            replay.realtimePlayback = !replay.realtimePlayback;
            editor.paused = !replay.realtimePlayback;
        }

        const pf::InputFrame p1Input = readPlayerInput(0, false);
        const pf::InputFrame p2Input = editor.testMode ? pf::InputFrame{} : readPlayerInput(1, true);
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
