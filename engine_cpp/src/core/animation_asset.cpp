#include "core/animation_asset.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace pf {
namespace {

constexpr int32_t kCurrentPfhaLayout = 9;

class BinaryReader {
public:
    explicit BinaryReader(std::vector<uint8_t> data) : data(std::move(data)) {}

    bool hasMagic(const char* magic) const {
        return data.size() >= 4 && std::memcmp(data.data(), magic, 4) == 0;
    }

    void skip(size_t count) {
        require(count);
        position += count;
    }

    uint8_t readU8() {
        require(1);
        return data[position++];
    }

    std::vector<uint8_t> readBytes(size_t count) {
        require(count);
        std::vector<uint8_t> value(data.begin() + static_cast<std::ptrdiff_t>(position),
                                   data.begin() + static_cast<std::ptrdiff_t>(position + count));
        position += count;
        return value;
    }

    bool readBool() {
        return readU8() != 0;
    }

    int32_t readI32() {
        require(4);
        int32_t value = 0;
        std::memcpy(&value, data.data() + position, 4);
        position += 4;
        return value;
    }

    uint32_t readU32() {
        require(4);
        uint32_t value = 0;
        std::memcpy(&value, data.data() + position, 4);
        position += 4;
        return value;
    }

    float readF32() {
        require(4);
        float value = 0.0f;
        std::memcpy(&value, data.data() + position, 4);
        position += 4;
        return value;
    }

    std::string readString() {
        const uint32_t size = readU32();
        require(size);
        std::string value(reinterpret_cast<const char*>(data.data() + position), size);
        position += size;
        return value;
    }

    Vec3 readVec3() {
        return {fxFromFloat(readF32()), fxFromFloat(readF32()), fxFromFloat(readF32())};
    }

private:
    std::vector<uint8_t> data;
    size_t position = 0;

    void require(size_t count) const {
        if (position + count > data.size()) {
            throw std::runtime_error("truncated binary fighter asset");
        }
    }
};

std::vector<uint8_t> readBinaryFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to open " + path);
    }
    file.seekg(0, std::ios::end);
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    return bytes;
}

AnimationChannel channelFromId(uint8_t value) {
    switch (value) {
    case 0: return AnimationChannel::TranslateX;
    case 1: return AnimationChannel::TranslateY;
    case 2: return AnimationChannel::TranslateZ;
    case 3: return AnimationChannel::RotateX;
    case 4: return AnimationChannel::RotateY;
    case 5: return AnimationChannel::RotateZ;
    case 6: return AnimationChannel::ScaleX;
    case 7: return AnimationChannel::ScaleY;
    case 8: return AnimationChannel::ScaleZ;
    default: throw std::runtime_error("unknown binary animation channel");
    }
}

AnimationInterpolation interpolationFromId(uint8_t value) {
    switch (value) {
    case 0: return AnimationInterpolation::Constant;
    case 1: return AnimationInterpolation::Linear;
    case 2: return AnimationInterpolation::Spline;
    default: return AnimationInterpolation::Linear;
    }
}

AnimationTrack readAnimationTrack(BinaryReader& reader) {
    AnimationTrack track;
    track.joint = reader.readI32();
    track.channel = channelFromId(reader.readU8());

    const int32_t keyCount = reader.readI32();
    track.keys.reserve(static_cast<size_t>(keyCount));
    for (int32_t keyIndex = 0; keyIndex < keyCount; ++keyIndex) {
        AnimationKey key;
        key.frame = fxFromFloat(reader.readF32());
        key.value = fxFromFloat(reader.readF32());
        key.tangent = fxFromFloat(reader.readF32());
        key.interpolation = interpolationFromId(reader.readU8());
        track.keys.push_back(key);
    }
    return track;
}

AnimationClip readAnimationClip(BinaryReader& reader) {
    AnimationClip clip;
    clip.name = reader.readString();
    clip.actionIndex = reader.readI32();
    clip.actionFlags = reader.readU32();
    clip.defaultBlendFrames = reader.readU8();
    clip.frameCount = fxFromFloat(reader.readF32());

    const int32_t trackCount = reader.readI32();
    clip.tracks.reserve(static_cast<size_t>(trackCount));
    for (int32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
        clip.tracks.push_back(readAnimationTrack(reader));
    }
    return clip;
}

} // namespace

HsdFighterAnimationAsset loadHsdFighterAnimationAsset(const std::string& path) {
    BinaryReader reader(readBinaryFile(path));
    if (!reader.hasMagic("PFHA")) {
        throw std::runtime_error("invalid binary fighter asset magic");
    }
    reader.skip(4);
    const int32_t layout = reader.readI32();
    if (layout != kCurrentPfhaLayout) {
        throw std::runtime_error("unsupported binary fighter asset layout");
    }

    HsdFighterAnimationAsset asset;
    asset.name = reader.readString();

    const int32_t jointCount = reader.readI32();
    asset.skeleton.reserve(static_cast<size_t>(jointCount));
    for (int32_t i = 0; i < jointCount; ++i) {
        AnimationJoint joint;
        joint.parent = reader.readI32();
        joint.name = reader.readString();
        joint.flags = reader.readU32();
        joint.translation = reader.readVec3();
        joint.rotation = reader.readVec3();
        joint.scale = reader.readVec3();
        asset.skeleton.push_back(std::move(joint));
    }

    asset.fighterBones.head = reader.readI32();
    asset.fighterBones.rightArm = reader.readI32();
    asset.fighterBones.leftLeg = reader.readI32();
    asset.fighterBones.rightLeg = reader.readI32();
    asset.fighterBones.leftArm = reader.readI32();
    asset.fighterBones.itemHold = reader.readI32();
    asset.fighterBones.shield = reader.readI32();
    asset.fighterBones.topOfHead = reader.readI32();
    asset.fighterBones.leftFoot = reader.readI32();
    asset.fighterBones.rightFoot = reader.readI32();

    asset.hasAttributes = reader.readBool();
    if (asset.hasAttributes) {
        HsdFighterAttributes& attr = asset.attributes;
        attr.initialWalkSpeed = fxFromFloat(reader.readF32());
        attr.walkAcceleration = fxFromFloat(reader.readF32());
        attr.maxWalkSpeed = fxFromFloat(reader.readF32());
        attr.midWalkPoint = fxFromFloat(reader.readF32());
        attr.fastWalkSpeed = fxFromFloat(reader.readF32());
        attr.friction = fxFromFloat(reader.readF32());
        attr.initialDashSpeed = fxFromFloat(reader.readF32());
        attr.dashRunAccelerationA = fxFromFloat(reader.readF32());
        attr.dashRunAccelerationB = fxFromFloat(reader.readF32());
        attr.initialRunSpeed = fxFromFloat(reader.readF32());
        attr.runAnimationScale = fxFromFloat(reader.readF32());
        attr.maxRunBrakeFrames = fxFromFloat(reader.readF32());
        attr.groundMaxHorizontalVelocity = fxFromFloat(reader.readF32());
        attr.jumpStartupLag = fxFromFloat(reader.readF32());
        attr.initialHorizontalJumpVelocity = fxFromFloat(reader.readF32());
        attr.initialVerticalJumpVelocity = fxFromFloat(reader.readF32());
        attr.groundToAirJumpMomentumMultiplier = fxFromFloat(reader.readF32());
        attr.maximumShorthopHorizontalVelocity = fxFromFloat(reader.readF32());
        attr.maximumShorthopVerticalVelocity = fxFromFloat(reader.readF32());
        attr.verticalAirJumpMultiplier = fxFromFloat(reader.readF32());
        attr.horizontalAirJumpMultiplier = fxFromFloat(reader.readF32());
        attr.numberOfJumps = reader.readI32();
        attr.gravity = fxFromFloat(reader.readF32());
        attr.terminalVelocity = fxFromFloat(reader.readF32());
        attr.aerialSpeed = fxFromFloat(reader.readF32());
        attr.aerialFriction = fxFromFloat(reader.readF32());
        attr.maxAerialHorizontalSpeed = fxFromFloat(reader.readF32());
        attr.airFriction = fxFromFloat(reader.readF32());
        attr.fastFallTerminalVelocity = fxFromFloat(reader.readF32());
        attr.airMaxHorizontalVelocity = fxFromFloat(reader.readF32());
        attr.framesToChangeDirectionOnStandingTurn = fxFromFloat(reader.readF32());
        attr.weight = fxFromFloat(reader.readF32());
        attr.modelScale = fxFromFloat(reader.readF32());
        attr.shieldSize = fxFromFloat(reader.readF32());
        attr.shieldBreakInitialVelocity = fxFromFloat(reader.readF32());
        attr.normalLandingLag = fxFromFloat(reader.readF32());
        attr.nairLandingLag = fxFromFloat(reader.readF32());
        attr.fairLandingLag = fxFromFloat(reader.readF32());
        attr.bairLandingLag = fxFromFloat(reader.readF32());
        attr.uairLandingLag = fxFromFloat(reader.readF32());
        attr.dairLandingLag = fxFromFloat(reader.readF32());
        attr.wallJumpHorizontalVelocity = fxFromFloat(reader.readF32());
        attr.wallJumpVerticalVelocity = fxFromFloat(reader.readF32());
        attr.ledgeJumpHorizontalVelocity = fxFromFloat(reader.readF32());
        attr.ledgeJumpVerticalVelocity = fxFromFloat(reader.readF32());
    }

    asset.hasEnvironmentCollision = reader.readBool();
    for (int& bone : asset.environmentCollision.bones) {
        bone = reader.readI32();
    }
    asset.environmentCollision.multiplier = fxFromFloat(reader.readF32());
    asset.environmentCollision.ledgeGrabWidth = fxFromFloat(reader.readF32());
    asset.environmentCollision.ledgeGrabYOffset = fxFromFloat(reader.readF32());
    asset.environmentCollision.ledgeGrabHeight = fxFromFloat(reader.readF32());

    const int32_t hurtboxCount = reader.readI32();
    asset.hurtboxes.reserve(static_cast<size_t>(hurtboxCount));
    for (int32_t i = 0; i < hurtboxCount; ++i) {
        HsdHurtbox hurtbox;
        hurtbox.index = reader.readI32();
        hurtbox.bone = reader.readI32();
        hurtbox.type = reader.readString();
        hurtbox.grabbable = reader.readBool();
        hurtbox.start = reader.readVec3();
        hurtbox.end = reader.readVec3();
        hurtbox.radius = fxFromFloat(reader.readF32());
        asset.hurtboxes.push_back(std::move(hurtbox));
    }

    const int32_t modelPartCount = reader.readI32();
    asset.modelPartAnimations.reserve(static_cast<size_t>(modelPartCount));
    for (int32_t i = 0; i < modelPartCount; ++i) {
        HsdModelPartAnimationSet set;
        set.startingBone = reader.readI32();
        const int32_t entryCount = reader.readI32();
        set.entries.reserve(static_cast<size_t>(entryCount));
        for (int32_t entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
            set.entries.push_back(reader.readI32());
        }
        const int32_t animationCount = reader.readI32();
        set.animations.reserve(static_cast<size_t>(animationCount));
        for (int32_t animIndex = 0; animIndex < animationCount; ++animIndex) {
            set.animations.push_back(readAnimationClip(reader));
        }
        asset.modelPartAnimations.push_back(std::move(set));
    }

    const int32_t clipCount = reader.readI32();
    asset.clips.reserve(static_cast<size_t>(clipCount));
    for (int32_t i = 0; i < clipCount; ++i) {
        asset.clips.push_back(readAnimationClip(reader));
    }
    const int32_t scriptCount = reader.readI32();
    asset.actionScripts.reserve(static_cast<size_t>(scriptCount));
    for (int32_t scriptIndex = 0; scriptIndex < scriptCount; ++scriptIndex) {
        HsdActionScript script;
        script.name = reader.readString();
        script.actionIndex = reader.readI32();
        script.commonBoneLookup.fill(-1);
        const int32_t lookupCount = reader.readI32();
        for (int32_t i = 0; i < lookupCount; ++i) {
            const int32_t bone = reader.readI32();
            if (i >= 0 && i < static_cast<int32_t>(script.commonBoneLookup.size())) {
                script.commonBoneLookup[static_cast<size_t>(i)] = bone;
            }
        }
        const int32_t commandCount = reader.readI32();
        script.commands.reserve(static_cast<size_t>(commandCount));
        for (int32_t commandIndex = 0; commandIndex < commandCount; ++commandIndex) {
            HsdActionCommand command;
            command.code = reader.readU8();
            const int32_t byteCount = reader.readI32();
            command.bytes = reader.readBytes(static_cast<size_t>(byteCount));
            script.commands.push_back(std::move(command));
        }
        asset.actionScripts.push_back(std::move(script));
    }
    asset.hasShieldPose = reader.readBool();
    if (asset.hasShieldPose) {
        const int32_t poseJointCount = reader.readI32();
        asset.shieldPose.joints.reserve(static_cast<size_t>(poseJointCount));
        for (int32_t i = 0; i < poseJointCount; ++i) {
            JointPose pose;
            pose.translation = reader.readVec3();
            pose.rotation = reader.readVec3();
            pose.scale = reader.readVec3();
            asset.shieldPose.joints.push_back(pose);
        }
    }
    return asset;
}

const AnimationClip* findClipByActionIndex(const HsdFighterAnimationAsset& asset, int actionIndex) {
    for (const AnimationClip& clip : asset.clips) {
        if (clip.actionIndex == actionIndex) {
            return &clip;
        }
    }
    return nullptr;
}

const AnimationClip* findClipByName(const HsdFighterAnimationAsset& asset, const std::string& name) {
    for (const AnimationClip& clip : asset.clips) {
        if (clip.name == name) {
            return &clip;
        }
    }
    return nullptr;
}

const HsdActionScript* findActionScriptByActionIndex(const HsdFighterAnimationAsset& asset, int actionIndex) {
    for (const HsdActionScript& script : asset.actionScripts) {
        if (script.actionIndex == actionIndex) {
            return &script;
        }
    }
    return nullptr;
}

} // namespace pf
