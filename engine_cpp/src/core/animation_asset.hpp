#pragma once

#include "core/animation.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pf {

struct FighterBoneTable {
    int head = -1;
    int rightArm = -1;
    int leftLeg = -1;
    int rightLeg = -1;
    int leftArm = -1;
    int itemHold = -1;
    int shield = -1;
    int topOfHead = -1;
    int leftFoot = -1;
    int rightFoot = -1;
};

struct ModelPartAnimationSet {
    int startingBone = 0;
    std::vector<int> entries;
    std::vector<AnimationClip> animations;
};

struct FighterMeshTexture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    int importSourceTextureIndex = -1;
};

struct FighterMeshVertexInfluence {
    int bone = -1;
    float weight = 0.0f;
};

struct FighterMeshVertex {
    Vec3 position;
    Vec3 normal;
    float u = 0.0f;
    float v = 0.0f;
    std::array<uint8_t, 4> color{255, 255, 255, 255};
    std::array<FighterMeshVertexInfluence, 6> influences{};
};

struct FighterMeshBatch {
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
    std::vector<FighterMeshVertex> vertices;
    int importSourceMaterialIndex = -1;
};

struct FighterMesh {
    std::vector<std::array<float, 16>> inverseBindMatrices;
    std::vector<FighterMeshTexture> textures;
    std::vector<FighterMeshBatch> batches;
};

} // namespace pf
