#pragma once

#include "core/math.hpp"

#include <array>
#include <string>
#include <vector>

namespace pf {

enum class AnimationChannel {
    TranslateX,
    TranslateY,
    TranslateZ,
    RotateX,
    RotateY,
    RotateZ,
    ScaleX,
    ScaleY,
    ScaleZ,
};

enum class AnimationInterpolation {
    Constant,
    Linear,
    Spline,
};

struct AnimationKey {
    Fix frame = 0;
    Fix value = 0;
    Fix tangent = 0;
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
};

struct AnimationTrack {
    int joint = -1;
    AnimationChannel channel = AnimationChannel::TranslateX;
    std::vector<AnimationKey> keys;
};

struct AnimationJoint {
    int parent = -1;
    std::string name;
    uint32_t flags = 0;
    Vec3 translation{};
    Vec3 rotation{};
    Vec3 scale{fx(1), fx(1), fx(1)};
    int importSourceJointIndex = -1;
};

struct Quaternion {
    Fix x = 0;
    Fix y = 0;
    Fix z = 0;
    Fix w = fx(1);
};

struct AnimationClip {
    std::string name;
    int actionIndex = -1;
    uint32_t actionFlags = 0;
    int defaultBlendFrames = 0;
    Fix frameCount = 0;
    bool generatedFallback = false;
    std::vector<AnimationTrack> tracks;
    int importSourceClipIndex = -1;
};

struct JointPose {
    Vec3 translation{};
    Vec3 rotation{};
    Vec3 scale{fx(1), fx(1), fx(1)};
    Quaternion quaternion{};
    bool useQuaternion = false;
};

struct AnimationPose {
    std::vector<JointPose> joints;
};

struct JointWorldTransform {
    Vec3 translation{};
    std::array<Fix, 9> rotation{fx(1), 0, 0, 0, fx(1), 0, 0, 0, fx(1)};
    std::array<Fix, 16> matrix{
        fx(1), 0, 0, 0,
        0, fx(1), 0, 0,
        0, 0, fx(1), 0,
        0, 0, 0, fx(1),
    };
};

Fix sampleTrack(const AnimationTrack& track, Fix frame);
Quaternion eulerToQuaternion(Vec3 euler);
Quaternion slerpQuaternion(Quaternion from, Quaternion to, Fix t);
AnimationPose bindPose(const std::vector<AnimationJoint>& skeleton);
AnimationPose evaluateClip(const std::vector<AnimationJoint>& skeleton, const AnimationClip& clip, Fix frame);
std::vector<JointWorldTransform> jointWorldTransforms(const std::vector<AnimationJoint>& skeleton, const AnimationPose& pose);
std::vector<Vec3> jointWorldTranslations(const std::vector<AnimationJoint>& skeleton, const AnimationPose& pose);
Vec3 transformPoint(const JointWorldTransform& transform, Vec3 localPoint);
Vec3 rootTranslationDelta(const AnimationClip& clip, int joint, Fix fromFrame, Fix toFrame);

} // namespace pf
