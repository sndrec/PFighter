#include "core/animation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace pf {

static Fix hermite(Fix t, Fix p0, Fix m0, Fix p1, Fix m1) {
    const Fix t2 = fxMul(t, t);
    const Fix t3 = fxMul(t2, t);
    const Fix h00 = fx(2) * t3 / kScale - fx(3) * t2 / kScale + fx(1);
    const Fix h10 = t3 - fx(2) * t2 / kScale + t;
    const Fix h01 = -fx(2) * t3 / kScale + fx(3) * t2 / kScale;
    const Fix h11 = t3 - t2;
    return fxMul(h00, p0) + fxMul(h10, m0) + fxMul(h01, p1) + fxMul(h11, m1);
}

static void applyChannel(JointPose& pose, AnimationChannel channel, Fix value) {
    switch (channel) {
    case AnimationChannel::TranslateX: pose.translation.x = value; break;
    case AnimationChannel::TranslateY: pose.translation.y = value; break;
    case AnimationChannel::TranslateZ: pose.translation.z = value; break;
    case AnimationChannel::RotateX: pose.rotation.x = value; pose.useQuaternion = false; break;
    case AnimationChannel::RotateY: pose.rotation.y = value; pose.useQuaternion = false; break;
    case AnimationChannel::RotateZ: pose.rotation.z = value; pose.useQuaternion = false; break;
    case AnimationChannel::ScaleX: pose.scale.x = value; break;
    case AnimationChannel::ScaleY: pose.scale.y = value; break;
    case AnimationChannel::ScaleZ: pose.scale.z = value; break;
    }
}

using Mat3 = std::array<Fix, 9>;
using Mat4 = std::array<Fix, 16>;

constexpr uint32_t kJObjClassicalScale = 1u << 3;

static Mat4 identity4() {
    return {
        fx(1), 0, 0, 0,
        0, fx(1), 0, 0,
        0, 0, fx(1), 0,
        0, 0, 0, fx(1),
    };
}

static Mat4 mul4(Mat4 a, Mat4 b) {
    Mat4 result{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            Fix value = 0;
            for (int k = 0; k < 4; ++k) {
                value += fxMul(a[static_cast<size_t>(row * 4 + k)], b[static_cast<size_t>(k * 4 + col)]);
            }
            result[static_cast<size_t>(row * 4 + col)] = value;
        }
    }
    return result;
}

static Vec3 transformColumnPoint(Mat4 matrix, Vec3 point) {
    return {
        fxMul(matrix[0], point.x) + fxMul(matrix[1], point.y) + fxMul(matrix[2], point.z) + matrix[3],
        fxMul(matrix[4], point.x) + fxMul(matrix[5], point.y) + fxMul(matrix[6], point.z) + matrix[7],
        fxMul(matrix[8], point.x) + fxMul(matrix[9], point.y) + fxMul(matrix[10], point.z) + matrix[11],
    };
}

Quaternion eulerToQuaternion(Vec3 euler) {
    const float cx = std::cos(0.5f * fxToFloat(euler.x));
    const float cy = std::cos(0.5f * fxToFloat(euler.y));
    const float cz = std::cos(0.5f * fxToFloat(euler.z));
    const float sx = std::sin(0.5f * fxToFloat(euler.x));
    const float sy = std::sin(0.5f * fxToFloat(euler.y));
    const float sz = std::sin(0.5f * fxToFloat(euler.z));
    const float ss = sy * sz;
    const float cc = cy * cz;
    return {
        fxFromFloat(sx * cc - cx * ss),
        fxFromFloat(cz * (cx * sy) + sz * (sx * cy)),
        fxFromFloat(sz * (cx * cy) - cz * (sx * sy)),
        fxFromFloat(cx * cc + sx * ss),
    };
}

Quaternion slerpQuaternion(Quaternion from, Quaternion to, Fix t) {
    float fx0 = fxToFloat(from.x);
    float fy0 = fxToFloat(from.y);
    float fz0 = fxToFloat(from.z);
    float fw0 = fxToFloat(from.w);
    float tx = fxToFloat(to.x);
    float ty = fxToFloat(to.y);
    float tz = fxToFloat(to.z);
    float tw = fxToFloat(to.w);
    const float tt = fxToFloat(t);
    float cosom = fx0 * tx + fy0 * ty + fz0 * tz + fw0 * tw;
    if (cosom < 0.0f) {
        tx = -tx;
        ty = -ty;
        tz = -tz;
        tw = -tw;
        cosom = -cosom;
    }

    float fromScale = 1.0f - tt;
    float toScale = tt;
    if ((1.0f - cosom) > 1e-10f) {
        const float theta = std::acos(std::clamp(cosom, -1.0f, 1.0f));
        const float sinom = std::sin(theta);
        if (std::abs(sinom) > 1e-10f) {
            fromScale = std::sin((1.0f - tt) * theta) / sinom;
            toScale = std::sin(tt * theta) / sinom;
        }
    }

    return {
        fxFromFloat(fromScale * fx0 + toScale * tx),
        fxFromFloat(fromScale * fy0 + toScale * ty),
        fxFromFloat(fromScale * fz0 + toScale * tz),
        fxFromFloat(fromScale * fw0 + toScale * tw),
    };
}

static Mat3 rotationPart(Mat4 matrix) {
    return {
        matrix[0], matrix[1], matrix[2],
        matrix[4], matrix[5], matrix[6],
        matrix[8], matrix[9], matrix[10],
    };
}

static Fix scaledByParent(Fix value, Fix fromAxis, Fix toAxis) {
    if (fromAxis == 0) {
        return value;
    }
    return fxMul(fxMul(value, toAxis), fxDiv(fx(1), fromAxis));
}

static Mat4 hsdMtxSrt(const JointPose& pose, const Vec3* parentScale) {
    Fix scaleXForX = pose.scale.x;
    Fix scaleYForX = pose.scale.y;
    Fix scaleZForX = pose.scale.z;
    Fix scaleXForY = pose.scale.x;
    Fix scaleYForY = pose.scale.y;
    Fix scaleZForY = pose.scale.z;
    Fix scaleXForZ = pose.scale.x;
    Fix scaleYForZ = pose.scale.y;
    Fix scaleZForZ = pose.scale.z;

    if (parentScale != nullptr) {
        scaleYForX = scaledByParent(scaleYForX, parentScale->x, parentScale->y);
        scaleZForX = scaledByParent(scaleZForX, parentScale->x, parentScale->z);
        scaleXForY = scaledByParent(scaleXForY, parentScale->y, parentScale->x);
        scaleZForY = scaledByParent(scaleZForY, parentScale->y, parentScale->z);
        scaleXForZ = scaledByParent(scaleXForZ, parentScale->z, parentScale->x);
        scaleYForZ = scaledByParent(scaleYForZ, parentScale->z, parentScale->y);
    }

    Mat4 matrix = identity4();
    if (pose.useQuaternion) {
        const float x = fxToFloat(pose.quaternion.x);
        const float y = fxToFloat(pose.quaternion.y);
        const float z = fxToFloat(pose.quaternion.z);
        const float w = fxToFloat(pose.quaternion.w);
        const Fix r00 = fxFromFloat(1.0f - 2.0f * y * y - 2.0f * z * z);
        const Fix r01 = fxFromFloat(2.0f * x * y - 2.0f * w * z);
        const Fix r02 = fxFromFloat(2.0f * x * z + 2.0f * w * y);
        const Fix r10 = fxFromFloat(2.0f * x * y + 2.0f * w * z);
        const Fix r11 = fxFromFloat(1.0f - 2.0f * x * x - 2.0f * z * z);
        const Fix r12 = fxFromFloat(2.0f * y * z - 2.0f * w * x);
        const Fix r20 = fxFromFloat(2.0f * x * z - 2.0f * w * y);
        const Fix r21 = fxFromFloat(2.0f * y * z + 2.0f * w * x);
        const Fix r22 = fxFromFloat(1.0f - 2.0f * x * x - 2.0f * y * y);

        matrix[0] = fxMul(scaleXForX, r00);
        matrix[4] = fxMul(scaleXForY, r10);
        matrix[8] = fxMul(scaleXForZ, r20);
        matrix[1] = fxMul(scaleYForX, r01);
        matrix[5] = fxMul(scaleYForY, r11);
        matrix[9] = fxMul(scaleYForZ, r21);
        matrix[2] = fxMul(scaleZForX, r02);
        matrix[6] = fxMul(scaleZForY, r12);
        matrix[10] = fxMul(scaleZForZ, r22);
    } else {
        const float rx = fxToFloat(pose.rotation.x);
        const float ry = fxToFloat(pose.rotation.y);
        const float rz = fxToFloat(pose.rotation.z);
        const Fix sinX = fxFromFloat(std::sin(rx));
        const Fix cosX = fxFromFloat(std::cos(rx));
        const Fix sinY = fxFromFloat(std::sin(ry));
        const Fix cosY = fxFromFloat(std::cos(ry));
        const Fix sinZ = fxFromFloat(std::sin(rz));
        const Fix cosZ = fxFromFloat(std::cos(rz));

        matrix[0] = fxMul(cosZ, fxMul(scaleXForX, cosY));
        matrix[4] = fxMul(sinZ, fxMul(scaleXForY, cosY));
        matrix[8] = -fxMul(scaleXForZ, sinY);

        matrix[1] = fxMul(scaleYForX, fxMul(cosZ, fxMul(sinX, sinY)) - fxMul(cosX, sinZ));
        matrix[5] = fxMul(scaleYForY, fxMul(sinZ, fxMul(sinX, sinY)) + fxMul(cosX, cosZ));
        matrix[9] = fxMul(cosY, fxMul(scaleYForZ, sinX));

        matrix[2] = fxMul(scaleZForX, fxMul(cosZ, fxMul(cosX, sinY)) + fxMul(sinX, sinZ));
        matrix[6] = fxMul(scaleZForY, fxMul(sinZ, fxMul(cosX, sinY)) - fxMul(sinX, cosZ));
        matrix[10] = fxMul(cosY, fxMul(scaleZForZ, cosX));
    }

    matrix[3] = pose.translation.x;
    matrix[7] = pose.translation.y;
    matrix[11] = pose.translation.z;
    return matrix;
}

Fix sampleTrack(const AnimationTrack& track, Fix frame) {
    if (track.keys.empty()) {
        return 0;
    }
    if (track.keys.size() == 1 || frame <= track.keys.front().frame) {
        return track.keys.front().value;
    }
    if (frame >= track.keys.back().frame) {
        return track.keys.back().value;
    }

    auto upper = std::upper_bound(track.keys.begin(), track.keys.end(), frame, [](Fix value, const AnimationKey& key) {
        return value < key.frame;
    });
    const AnimationKey& next = *upper;
    const AnimationKey& current = *(upper - 1);
    const Fix duration = next.frame - current.frame;
    if (duration <= 0 || current.interpolation == AnimationInterpolation::Constant) {
        return current.value;
    }

    const Fix t = fxDiv(frame - current.frame, duration);
    if (current.interpolation == AnimationInterpolation::Linear) {
        return current.value + fxMul(next.value - current.value, t);
    }

    const Fix scaledCurrentTangent = fxMul(current.tangent, duration);
    const Fix scaledNextTangent = fxMul(next.tangent, duration);
    return hermite(t, current.value, scaledCurrentTangent, next.value, scaledNextTangent);
}

AnimationPose bindPose(const std::vector<AnimationJoint>& skeleton) {
    AnimationPose pose;
    pose.joints.reserve(skeleton.size());
    for (const AnimationJoint& joint : skeleton) {
        pose.joints.push_back({
            joint.translation,
            joint.rotation,
            joint.scale,
        });
    }
    return pose;
}

AnimationPose evaluateClip(const std::vector<AnimationJoint>& skeleton, const AnimationClip& clip, Fix frame) {
    AnimationPose pose = bindPose(skeleton);
    if (clip.frameCount > 0) {
        frame = std::clamp(frame, Fix{0}, clip.frameCount);
    }
    for (const AnimationTrack& track : clip.tracks) {
        if (track.joint < 0 || static_cast<size_t>(track.joint) >= pose.joints.size()) {
            continue;
        }
        applyChannel(pose.joints[static_cast<size_t>(track.joint)], track.channel, sampleTrack(track, frame));
    }
    return pose;
}

std::vector<JointWorldTransform> jointWorldTransforms(const std::vector<AnimationJoint>& skeleton, const AnimationPose& pose) {
    std::vector<JointWorldTransform> transforms(skeleton.size());
    std::vector<Vec3> propagatedScales(skeleton.size(), {fx(1), fx(1), fx(1)});
    std::vector<bool> hasPropagatedScale(skeleton.size(), false);
    for (size_t i = 0; i < skeleton.size() && i < pose.joints.size(); ++i) {
        const int parent = skeleton[i].parent;
        const bool hasParent = parent >= 0 && static_cast<size_t>(parent) < i;
        const Vec3* parentScale = nullptr;
        if (hasParent && hasPropagatedScale[static_cast<size_t>(parent)]) {
            parentScale = &propagatedScales[static_cast<size_t>(parent)];
        }

        if ((skeleton[i].flags & kJObjClassicalScale) != 0) {
            if (parentScale != nullptr) {
                propagatedScales[i] = *parentScale;
                hasPropagatedScale[i] = true;
            }
        } else {
            hasPropagatedScale[i] = true;
            if (parentScale != nullptr) {
                propagatedScales[i] = {
                    fxMul(pose.joints[i].scale.x, parentScale->x),
                    fxMul(pose.joints[i].scale.y, parentScale->y),
                    fxMul(pose.joints[i].scale.z, parentScale->z),
                };
            } else {
                propagatedScales[i] = pose.joints[i].scale;
            }
        }

        const Mat4 local = hsdMtxSrt(pose.joints[i], parentScale);
        if (hasParent) {
            transforms[i].matrix = mul4(transforms[static_cast<size_t>(parent)].matrix, local);
        } else {
            transforms[i].matrix = local;
        }
        transforms[i].translation = {transforms[i].matrix[3], transforms[i].matrix[7], transforms[i].matrix[11]};
        transforms[i].rotation = rotationPart(transforms[i].matrix);
    }
    return transforms;
}

std::vector<Vec3> jointWorldTranslations(const std::vector<AnimationJoint>& skeleton, const AnimationPose& pose) {
    std::vector<JointWorldTransform> transforms = jointWorldTransforms(skeleton, pose);
    std::vector<Vec3> positions;
    positions.reserve(transforms.size());
    for (const JointWorldTransform& item : transforms) {
        positions.push_back(item.translation);
    }
    return positions;
}

Vec3 transformPoint(const JointWorldTransform& transformValue, Vec3 localPoint) {
    return transformColumnPoint(transformValue.matrix, localPoint);
}

Vec3 rootTranslationDelta(const AnimationClip& clip, int joint, Fix fromFrame, Fix toFrame) {
    Vec3 from{};
    Vec3 to{};
    for (const AnimationTrack& track : clip.tracks) {
        if (track.joint != joint) {
            continue;
        }
        const Fix fromValue = sampleTrack(track, fromFrame);
        const Fix toValue = sampleTrack(track, toFrame);
        switch (track.channel) {
        case AnimationChannel::TranslateX: from.x = fromValue; to.x = toValue; break;
        case AnimationChannel::TranslateY: from.y = fromValue; to.y = toValue; break;
        case AnimationChannel::TranslateZ: from.z = fromValue; to.z = toValue; break;
        default: break;
        }
    }
    return to - from;
}

} // namespace pf
