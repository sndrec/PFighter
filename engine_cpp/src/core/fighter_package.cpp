#include "core/fighter_package.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace pf {
namespace {

constexpr uint32_t kPackageVersion = 1;
constexpr uint32_t kMaxFighters = 256;
constexpr uint32_t kMaxAssets = 1024;
constexpr uint32_t kMaxAssetBytes = 128 * 1024 * 1024;
constexpr uint32_t kMaxObjects = 4096;
constexpr uint32_t kMaxStates = 2048;
constexpr uint32_t kMaxCallbacks = 4096;
constexpr uint32_t kMaxInterrupts = 4096;
constexpr uint32_t kMaxSubactions = 65536;
constexpr uint32_t kMaxHitboxes = 4096;
constexpr uint32_t kMaxHurtboxes = 4096;
constexpr uint32_t kMaxTouchboxes = 4096;
constexpr uint32_t kMaxStringBytes = 1 << 20;

class PackageWriter {
public:
    void writeMagic(const char* magic) {
        bytes.insert(bytes.end(), magic, magic + 4);
    }

    template <typename T>
    void writePod(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* raw = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), raw, raw + sizeof(T));
    }

    void writeU8(uint8_t value) {
        bytes.push_back(value);
    }

    void writeBool(bool value) {
        writeU8(value ? 1 : 0);
    }

    void writeI32(int32_t value) {
        writePod(value);
    }

    void writeU32(uint32_t value) {
        writePod(value);
    }

    void writeString(const std::string& value) {
        if (value.size() > kMaxStringBytes) {
            throw std::runtime_error("package string is too large");
        }
        writeU32(static_cast<uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }

    void writeBytes(const std::vector<uint8_t>& value) {
        if (value.size() > kMaxAssetBytes) {
            throw std::runtime_error("package asset is too large");
        }
        writeU32(static_cast<uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }

    std::vector<uint8_t> bytes;
};

class PackageReader {
public:
    explicit PackageReader(const std::vector<uint8_t>& source) : bytes(source) {}

    void readMagic(const char* magic) {
        require(4);
        if (std::memcmp(bytes.data() + position, magic, 4) != 0) {
            throw std::runtime_error("invalid fighter package magic");
        }
        position += 4;
    }

    template <typename T>
    T readPod() {
        static_assert(std::is_trivially_copyable_v<T>);
        require(sizeof(T));
        T value{};
        std::memcpy(&value, bytes.data() + position, sizeof(T));
        position += sizeof(T);
        return value;
    }

    uint8_t readU8() {
        require(1);
        return bytes[position++];
    }

    bool readBool() {
        const uint8_t value = readU8();
        if (value > 1) {
            throw std::runtime_error("invalid package bool");
        }
        return value != 0;
    }

    int32_t readI32() {
        return readPod<int32_t>();
    }

    uint32_t readU32() {
        return readPod<uint32_t>();
    }

    std::string readString() {
        const uint32_t size = readCount(kMaxStringBytes, "string bytes");
        require(size);
        std::string value(reinterpret_cast<const char*>(bytes.data() + position), size);
        position += size;
        return value;
    }

    std::vector<uint8_t> readBytes(uint32_t maxBytes, const char* label) {
        const uint32_t size = readCount(maxBytes, label);
        require(size);
        std::vector<uint8_t> value(bytes.begin() + static_cast<std::ptrdiff_t>(position),
                                   bytes.begin() + static_cast<std::ptrdiff_t>(position + size));
        position += size;
        return value;
    }

    uint32_t readCount(uint32_t maxValue, const char* label) {
        const uint32_t count = readU32();
        if (count > maxValue) {
            throw std::runtime_error(std::string("fighter package ") + label + " count is too large");
        }
        return count;
    }

    void requireFinished() const {
        if (position != bytes.size()) {
            throw std::runtime_error("fighter package has trailing bytes");
        }
    }

private:
    const std::vector<uint8_t>& bytes;
    size_t position = 0;

    void require(size_t count) const {
        if (position + count > bytes.size()) {
            throw std::runtime_error("truncated fighter package");
        }
    }
};

template <typename T>
void writeNativeStruct(PackageWriter& writer, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    writer.writeU32(static_cast<uint32_t>(sizeof(T)));
    writer.writePod(value);
}

template <typename T>
T readNativeStruct(PackageReader& reader, const char* label) {
    static_assert(std::is_trivially_copyable_v<T>);
    const uint32_t size = reader.readU32();
    if (size != sizeof(T)) {
        throw std::runtime_error(std::string("fighter package ") + label + " native size mismatch");
    }
    return reader.readPod<T>();
}

template <typename T, typename WriteFn>
void writeVector(PackageWriter& writer, const std::vector<T>& values, WriteFn writeValue) {
    if (values.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("fighter package vector is too large");
    }
    writer.writeU32(static_cast<uint32_t>(values.size()));
    for (const T& value : values) {
        writeValue(value);
    }
}

template <typename T, typename ReadFn>
std::vector<T> readVector(PackageReader& reader, uint32_t maxCount, const char* label, ReadFn readValue) {
    const uint32_t count = reader.readCount(maxCount, label);
    std::vector<T> values;
    values.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        values.push_back(readValue());
    }
    return values;
}

bool validPackageScriptOp(PackageScriptOp op) {
    switch (op) {
    case PackageScriptOp::Nop:
    case PackageScriptOp::SetVarImmediate:
    case PackageScriptOp::AddVarImmediate:
    case PackageScriptOp::AddVar:
    case PackageScriptOp::SetGroundVelocity:
    case PackageScriptOp::SetAirVelocityX:
    case PackageScriptOp::SetAirVelocityY:
    case PackageScriptOp::SetFacing:
    case PackageScriptOp::ChangeState:
    case PackageScriptOp::SpawnObject:
        return true;
    }
    return false;
}

bool validSubactionType(SubactionType type) {
    switch (type) {
    case SubactionType::SyncTimer:
    case SubactionType::AsyncTimer:
    case SubactionType::SetLoop:
    case SubactionType::ExecuteLoop:
    case SubactionType::CreateHitbox:
    case SubactionType::RemoveHitbox:
    case SubactionType::AdjustHitboxDamage:
    case SubactionType::AdjustHitboxSize:
    case SubactionType::SetHitboxInteraction:
    case SubactionType::CreateThrowHitbox:
    case SubactionType::ClearHitboxes:
    case SubactionType::SetHurtboxState:
    case SubactionType::SetBodyCollisionState:
    case SubactionType::SetInterruptible:
    case SubactionType::SetFlag:
    case SubactionType::SetThrowFlag:
    case SubactionType::SetThrowFlagLiteral:
    case SubactionType::EnableJabFollowup:
    case SubactionType::SetJabRapid:
    case SubactionType::SetJumpState:
    case SubactionType::ReverseDirection:
    case SubactionType::StartSmashCharge:
    case SubactionType::SetModelVisibility:
    case SubactionType::RevertModelVisibility:
    case SubactionType::RemoveModelVisibility:
    case SubactionType::SetModelPartAnimation:
    case SubactionType::SetFighterVisibility:
    case SubactionType::SelfDamage:
    case SubactionType::SpawnObject:
        return true;
    }
    return false;
}

bool validHurtboxState(HurtboxState state) {
    switch (state) {
    case HurtboxState::Normal:
    case HurtboxState::Invincible:
    case HurtboxState::Intangible:
        return true;
    }
    return false;
}

bool validGroundRequirement(GroundRequirement ground) {
    switch (ground) {
    case GroundRequirement::Any:
    case GroundRequirement::OnlyGrounded:
    case GroundRequirement::OnlyAirborne:
        return true;
    }
    return false;
}

bool validInterruptCondition(InterruptCondition condition) {
    switch (condition) {
    case InterruptCondition::JumpPressed:
    case InterruptCondition::AerialJumpForwardPressed:
    case InterruptCondition::AerialJumpBackwardPressed:
    case InterruptCondition::AirDodgePressed:
    case InterruptCondition::WallJumpInput:
    case InterruptCondition::SquatInput:
    case InterruptCondition::SquatReleaseInput:
    case InterruptCondition::AttackPressed:
    case InterruptCondition::JabFollowupPressed:
    case InterruptCondition::RapidJabReady:
    case InterruptCondition::SpecialNInput:
    case InterruptCondition::SpecialSInput:
    case InterruptCondition::SpecialHiInput:
    case InterruptCondition::SpecialLwInput:
    case InterruptCondition::SpecialAirNInput:
    case InterruptCondition::SpecialAirSInput:
    case InterruptCondition::SpecialAirHiInput:
    case InterruptCondition::SpecialAirLwInput:
    case InterruptCondition::AttackDashPressed:
    case InterruptCondition::AttackDashGrabBuffer:
    case InterruptCondition::AttackS4HiPressed:
    case InterruptCondition::AttackS4HiSPressed:
    case InterruptCondition::AttackS4Pressed:
    case InterruptCondition::AttackS4LwSPressed:
    case InterruptCondition::AttackS4LwPressed:
    case InterruptCondition::AttackS42Pressed:
    case InterruptCondition::AttackHi4Pressed:
    case InterruptCondition::AttackHi4NoStickWindowPressed:
    case InterruptCondition::AttackLw4Pressed:
    case InterruptCondition::AttackS3HiPressed:
    case InterruptCondition::AttackS3HiSPressed:
    case InterruptCondition::AttackS3Pressed:
    case InterruptCondition::AttackS3LwSPressed:
    case InterruptCondition::AttackS3LwPressed:
    case InterruptCondition::AttackHi3Pressed:
    case InterruptCondition::AttackLw3Pressed:
    case InterruptCondition::AttackLw3Repeat:
    case InterruptCondition::AerialAttackNPressed:
    case InterruptCondition::AerialAttackFPressed:
    case InterruptCondition::AerialAttackBPressed:
    case InterruptCondition::AerialAttackHiPressed:
    case InterruptCondition::AerialAttackLwPressed:
    case InterruptCondition::DashInput:
    case InterruptCondition::ReverseDashInput:
    case InterruptCondition::RunInput:
    case InterruptCondition::RunJumpPressed:
    case InterruptCondition::TeeterWalkInput:
    case InterruptCondition::HorizontalWalkSlow:
    case InterruptCondition::HorizontalWalkMiddle:
    case InterruptCondition::HorizontalWalkFast:
    case InterruptCondition::TurnInput:
    case InterruptCondition::TurnRunInput:
    case InterruptCondition::RunBrakeTurnRunInput:
    case InterruptCondition::RunBrakeInput:
    case InterruptCondition::WaitInput:
    case InterruptCondition::BecameAirborne:
    case InterruptCondition::ShieldReflectInput:
    case InterruptCondition::ShieldPressed:
    case InterruptCondition::ShieldHeld:
    case InterruptCondition::ShieldJumpPressed:
    case InterruptCondition::GuardCatchDashPressed:
    case InterruptCondition::SpotDodgeInput:
    case InterruptCondition::RollForwardInput:
    case InterruptCondition::RollBackwardInput:
    case InterruptCondition::LedgeClimbInput:
    case InterruptCondition::LedgeDropInput:
    case InterruptCondition::LedgeAttackInput:
    case InterruptCondition::LedgeEscapeInput:
    case InterruptCondition::GrabPressed:
    case InterruptCondition::TauntPressed:
        return true;
    }
    return false;
}

bool validGameObjectKind(GameObjectKind kind) {
    switch (kind) {
    case GameObjectKind::Item:
    case GameObjectKind::Projectile:
        return true;
    }
    return false;
}

bool validBoneId(BoneId bone) {
    switch (bone) {
    case BoneId::Hip:
    case BoneId::Head:
    case BoneId::HandL:
    case BoneId::HandR:
    case BoneId::FootL:
    case BoneId::FootR:
    case BoneId::Extra:
        return true;
    case BoneId::Count:
        return false;
    }
    return false;
}

template <typename Enum, typename ValidFn>
void writeEnum(PackageWriter& writer, Enum value, ValidFn valid, const char* label) {
    if (!valid(value)) {
        throw std::runtime_error(std::string("invalid fighter package ") + label);
    }
    writer.writeU8(static_cast<uint8_t>(value));
}

template <typename Enum, typename ValidFn>
Enum readEnum(PackageReader& reader, ValidFn valid, const char* label) {
    const Enum value = static_cast<Enum>(reader.readU8());
    if (!valid(value)) {
        throw std::runtime_error(std::string("invalid fighter package ") + label);
    }
    return value;
}

void writeVec2(PackageWriter& writer, Vec2 value) {
    writer.writeI32(value.x);
    writer.writeI32(value.y);
}

Vec2 readVec2(PackageReader& reader) {
    Vec2 value;
    value.x = reader.readI32();
    value.y = reader.readI32();
    return value;
}

void writeVec3(PackageWriter& writer, Vec3 value) {
    writer.writeI32(value.x);
    writer.writeI32(value.y);
    writer.writeI32(value.z);
}

Vec3 readVec3(PackageReader& reader) {
    Vec3 value;
    value.x = reader.readI32();
    value.y = reader.readI32();
    value.z = reader.readI32();
    return value;
}

void writeFunctionCall(PackageWriter& writer, const FunctionCall& call) {
    writer.writeString(call.name);
    writer.writeBool(call.boolParam);
    writer.writeI32(call.intParam);
    writer.writeI32(call.fixParam);
}

FunctionCall readFunctionCall(PackageReader& reader) {
    FunctionCall call;
    call.name = reader.readString();
    call.boolParam = reader.readBool();
    call.intParam = reader.readI32();
    call.fixParam = reader.readI32();
    return call;
}

void writeFunctionCalls(PackageWriter& writer, const std::vector<FunctionCall>& calls) {
    writeVector(writer, calls, [&](const FunctionCall& call) {
        writeFunctionCall(writer, call);
    });
}

std::vector<FunctionCall> readFunctionCalls(PackageReader& reader) {
    return readVector<FunctionCall>(reader, kMaxCallbacks, "callback", [&]() {
        return readFunctionCall(reader);
    });
}

void writePackageVariable(PackageWriter& writer, const PackageVariableDefinition& variable) {
    writer.writeString(variable.name);
    writer.writeI32(variable.initialValue);
}

PackageVariableDefinition readPackageVariable(PackageReader& reader) {
    PackageVariableDefinition variable;
    variable.name = reader.readString();
    variable.initialValue = reader.readI32();
    return variable;
}

void writePackageScriptInstruction(PackageWriter& writer, const PackageScriptInstruction& instruction) {
    writeEnum(writer, instruction.op, validPackageScriptOp, "script op");
    writer.writeI32(instruction.dst);
    writer.writeI32(instruction.srcA);
    writer.writeI32(instruction.srcB);
    writer.writeI32(instruction.intValue);
    writer.writeI32(instruction.fixValue);
    writer.writeString(instruction.text);
}

PackageScriptInstruction readPackageScriptInstruction(PackageReader& reader) {
    PackageScriptInstruction instruction;
    instruction.op = readEnum<PackageScriptOp>(reader, validPackageScriptOp, "script op");
    instruction.dst = reader.readI32();
    instruction.srcA = reader.readI32();
    instruction.srcB = reader.readI32();
    instruction.intValue = reader.readI32();
    instruction.fixValue = reader.readI32();
    instruction.text = reader.readString();
    return instruction;
}

void writePackageScript(PackageWriter& writer, const PackageScript& script) {
    writer.writeString(script.name);
    writer.writeI32(script.instructionBudget);
    writeVector(writer, script.instructions, [&](const PackageScriptInstruction& instruction) {
        writePackageScriptInstruction(writer, instruction);
    });
}

PackageScript readPackageScript(PackageReader& reader) {
    PackageScript script;
    script.name = reader.readString();
    script.instructionBudget = reader.readI32();
    script.instructions = readVector<PackageScriptInstruction>(reader, kMaxSubactions, "script instruction", [&]() {
        return readPackageScriptInstruction(reader);
    });
    return script;
}

void writeHitbox(PackageWriter& writer, const HitboxDefinition& hitbox) {
    writeEnum(writer, hitbox.bone, validBoneId, "hitbox bone");
    writer.writeI32(hitbox.hsdBone);
    writer.writeI32(hitbox.hitboxId);
    writeVec3(writer, hitbox.offset);
    writer.writeI32(hitbox.radius);
    writer.writeI32(hitbox.damage);
    writer.writeI32(hitbox.damageShield);
    writer.writeI32(hitbox.knockbackAngleDegrees);
    writer.writeBool(hitbox.knockbackAngleFixed);
    writer.writeI32(hitbox.knockbackBase);
    writer.writeI32(hitbox.knockbackGrowth);
    writer.writeI32(hitbox.knockbackWeightSet);
    writer.writeI32(hitbox.element);
    writer.writeBool(hitbox.isGrab);
    writer.writeBool(hitbox.canClank);
    writer.writeBool(hitbox.reboundsOnClank);
    writer.writeBool(hitbox.hitFighters);
    writer.writeBool(hitbox.onlyHitGrabbed);
    writer.writeBool(hitbox.requiresThrownHitboxOwner);
    writer.writeBool(hitbox.hitGrounded);
    writer.writeBool(hitbox.hitAirborne);
}

HitboxDefinition readHitbox(PackageReader& reader) {
    HitboxDefinition hitbox;
    hitbox.bone = readEnum<BoneId>(reader, validBoneId, "hitbox bone");
    hitbox.hsdBone = reader.readI32();
    hitbox.hitboxId = reader.readI32();
    hitbox.offset = readVec3(reader);
    hitbox.radius = reader.readI32();
    hitbox.damage = reader.readI32();
    hitbox.damageShield = reader.readI32();
    hitbox.knockbackAngleDegrees = reader.readI32();
    hitbox.knockbackAngleFixed = reader.readBool();
    hitbox.knockbackBase = reader.readI32();
    hitbox.knockbackGrowth = reader.readI32();
    hitbox.knockbackWeightSet = reader.readI32();
    hitbox.element = reader.readI32();
    hitbox.isGrab = reader.readBool();
    hitbox.canClank = reader.readBool();
    hitbox.reboundsOnClank = reader.readBool();
    hitbox.hitFighters = reader.readBool();
    hitbox.onlyHitGrabbed = reader.readBool();
    hitbox.requiresThrownHitboxOwner = reader.readBool();
    hitbox.hitGrounded = reader.readBool();
    hitbox.hitAirborne = reader.readBool();
    return hitbox;
}

void writeHurtbox(PackageWriter& writer, const HurtboxDefinition& hurtbox) {
    writeEnum(writer, hurtbox.bone, validBoneId, "hurtbox bone");
    writeVec3(writer, hurtbox.startOffset);
    writeVec3(writer, hurtbox.endOffset);
    writer.writeI32(hurtbox.radius);
    writeEnum(writer, hurtbox.state, validHurtboxState, "hurtbox state");
    writer.writeBool(hurtbox.grabbable);
}

HurtboxDefinition readHurtbox(PackageReader& reader) {
    HurtboxDefinition hurtbox;
    hurtbox.bone = readEnum<BoneId>(reader, validBoneId, "hurtbox bone");
    hurtbox.startOffset = readVec3(reader);
    hurtbox.endOffset = readVec3(reader);
    hurtbox.radius = reader.readI32();
    hurtbox.state = readEnum<HurtboxState>(reader, validHurtboxState, "hurtbox state");
    hurtbox.grabbable = reader.readBool();
    return hurtbox;
}

void writeInterrupt(PackageWriter& writer, const InterruptRule& rule) {
    writer.writeString(rule.targetState);
    writeEnum(writer, rule.condition, validInterruptCondition, "interrupt condition");
    writeEnum(writer, rule.ground, validGroundRequirement, "interrupt ground requirement");
    writer.writeI32(rule.blendFrames);
    writer.writeI32(rule.lagFrames);
    writer.writeBool(rule.startActive);
    writer.writeBool(rule.alwaysActive);
    writer.writeBool(rule.requireNoHitstun);
    writer.writeI32(rule.enableFrame);
    writer.writeI32(rule.disableFrame);
}

InterruptRule readInterrupt(PackageReader& reader) {
    InterruptRule rule;
    rule.targetState = reader.readString();
    rule.condition = readEnum<InterruptCondition>(reader, validInterruptCondition, "interrupt condition");
    rule.ground = readEnum<GroundRequirement>(reader, validGroundRequirement, "interrupt ground requirement");
    rule.blendFrames = reader.readI32();
    rule.lagFrames = reader.readI32();
    rule.startActive = reader.readBool();
    rule.alwaysActive = reader.readBool();
    rule.requireNoHitstun = reader.readBool();
    rule.enableFrame = reader.readI32();
    rule.disableFrame = reader.readI32();
    return rule;
}

void writeSubaction(PackageWriter& writer, const Subaction& subaction) {
    writeEnum(writer, subaction.type, validSubactionType, "subaction type");
    writer.writeI32(subaction.frames);
    writer.writeI32(subaction.loopCount);
    writer.writeI32(subaction.interruptibleFrame);
    writer.writeI32(subaction.flag);
    writer.writeI32(subaction.hurtboxIndex);
    writer.writeI32(subaction.hsdBone);
    writeEnum(writer, subaction.hurtboxState, validHurtboxState, "hurtbox state");
    writer.writeU32(subaction.flagValue);
    writer.writeI32(subaction.smashChargeHoldFrames);
    writer.writeI32(subaction.smashChargeDamageMultiplier);
    writer.writeI32(subaction.selfDamage);
    writer.writeString(subaction.objectName);
    writeVec2(writer, subaction.spawnVelocity);
    writeVec3(writer, subaction.spawnOffset);
    writer.writeI32(subaction.modelPartIndex);
    writer.writeI32(subaction.modelPartState);
    writer.writeI32(subaction.modelPartAnimation);
    writeHitbox(writer, subaction.hitbox);
}

Subaction readSubaction(PackageReader& reader) {
    Subaction subaction;
    subaction.type = readEnum<SubactionType>(reader, validSubactionType, "subaction type");
    subaction.frames = reader.readI32();
    subaction.loopCount = reader.readI32();
    subaction.interruptibleFrame = reader.readI32();
    subaction.flag = reader.readI32();
    subaction.hurtboxIndex = reader.readI32();
    subaction.hsdBone = reader.readI32();
    subaction.hurtboxState = readEnum<HurtboxState>(reader, validHurtboxState, "hurtbox state");
    subaction.flagValue = reader.readU32();
    subaction.smashChargeHoldFrames = reader.readI32();
    subaction.smashChargeDamageMultiplier = reader.readI32();
    subaction.selfDamage = reader.readI32();
    subaction.objectName = reader.readString();
    subaction.spawnVelocity = readVec2(reader);
    subaction.spawnOffset = readVec3(reader);
    subaction.modelPartIndex = reader.readI32();
    subaction.modelPartState = reader.readI32();
    subaction.modelPartAnimation = reader.readI32();
    subaction.hitbox = readHitbox(reader);
    return subaction;
}

void writeFighterState(PackageWriter& writer, const FighterState& state) {
    writer.writeString(state.name);
    writer.writeString(state.animation);
    writer.writeI32(state.animationActionIndex);
    writer.writeBool(state.useAnimPhysics);
    writer.writeBool(state.allowSlideoff);
    writer.writeBool(state.allowLedgeGrab);
    writer.writeBool(state.allowBackwardsLedgeGrab);
    writer.writeBool(state.allowWallCollision);
    writer.writeBool(state.allowCeilingCollision);
    writer.writeBool(state.convertFloorCollisionToGround);
    writer.writeBool(state.loopAnimation);
    writer.writeString(state.onAnimationFinishedState);
    writer.writeI32(state.onAnimationFinishedBlendFrames);
    writer.writeI32(state.defaultAnimationBlendFrames);
    writer.writeI32(state.animationLengthFrames);
    writer.writeI32(state.initialInterruptibleFrame);
    writeFunctionCalls(writer, state.onEnter);
    writeFunctionCalls(writer, state.onFrame);
    writeFunctionCalls(writer, state.onLanding);
    writeFunctionCalls(writer, state.onAirborne);
    writeVector(writer, state.interrupts, [&](const InterruptRule& rule) {
        writeInterrupt(writer, rule);
    });
    writeVector(writer, state.action, [&](const Subaction& subaction) {
        writeSubaction(writer, subaction);
    });
}

FighterState readFighterState(PackageReader& reader) {
    FighterState state;
    state.name = reader.readString();
    state.animation = reader.readString();
    state.animationActionIndex = reader.readI32();
    state.useAnimPhysics = reader.readBool();
    state.allowSlideoff = reader.readBool();
    state.allowLedgeGrab = reader.readBool();
    state.allowBackwardsLedgeGrab = reader.readBool();
    state.allowWallCollision = reader.readBool();
    state.allowCeilingCollision = reader.readBool();
    state.convertFloorCollisionToGround = reader.readBool();
    state.loopAnimation = reader.readBool();
    state.onAnimationFinishedState = reader.readString();
    state.onAnimationFinishedBlendFrames = reader.readI32();
    state.defaultAnimationBlendFrames = reader.readI32();
    state.animationLengthFrames = reader.readI32();
    state.initialInterruptibleFrame = reader.readI32();
    state.onEnter = readFunctionCalls(reader);
    state.onFrame = readFunctionCalls(reader);
    state.onLanding = readFunctionCalls(reader);
    state.onAirborne = readFunctionCalls(reader);
    state.interrupts = readVector<InterruptRule>(reader, kMaxInterrupts, "interrupt", [&]() {
        return readInterrupt(reader);
    });
    state.action = readVector<Subaction>(reader, kMaxSubactions, "subaction", [&]() {
        return readSubaction(reader);
    });
    return state;
}

int32_t assetIndexFor(const FighterPackage& package, const std::shared_ptr<const HsdFighterAnimationAsset>& asset) {
    if (!asset) {
        return -1;
    }
    const auto found = std::find(package.hsdAssets.begin(), package.hsdAssets.end(), asset);
    if (found == package.hsdAssets.end()) {
        return -1;
    }
    return static_cast<int32_t>(std::distance(package.hsdAssets.begin(), found));
}

#define PF_PACKAGE_MELEE_COMMON_FIELDS(X) \
    X(stickXTiltThresholdX8) \
    X(stickYTiltThresholdXC) \
    X(inputRepeatWindowX1C) \
    X(aerialAttackAngleTanX20) \
    X(walkInputThresholdX24) \
    X(walkMiddleThresholdX28) \
    X(walkFastThresholdX2C) \
    X(walkAccelScaleX30) \
    X(turnInputThresholdX34) \
    X(turnRunInputThresholdX38) \
    X(dashInputThresholdX3C) \
    X(dashStickWindowX40) \
    X(dashEarlyInterruptWindowX44) \
    X(dashItemThrowWindowX48) \
    X(dashLateInterruptWindowX4C) \
    X(attackDashFrictionScaleX50) \
    X(dashDecayX54) \
    X(runInputThresholdX58) \
    X(runAccelScaleX5C) \
    X(groundFrictionScaleX60) \
    X(catchCutFrictionScaleX64) \
    X(attackDashGrabBufferFramesX68) \
    X(turnFrictionScaleAboveWalkMaxX6C) \
    X(tapJumpThresholdX70) \
    X(tapJumpWindowX74) \
    X(jumpBackwardThresholdX78) \
    X(tapJumpReleaseThresholdX7C) \
    X(aerialJumpStickThresholdX80) \
    X(fastfallStickThresholdX88) \
    X(fastfallStickWindowX8C) \
    X(squatStickThresholdX90) \
    X(squatReleaseThresholdX94) \
    X(attackS3StickThresholdX98) \
    X(attackS3HiAngleX9C) \
    X(attackS3HiSAngleXA0) \
    X(attackS3LwSAngleXA4) \
    X(attackS3LwAngleXA8) \
    X(attackHi3StickThresholdYxAC) \
    X(attackLw3StickThresholdYxB0) \
    X(attackS4HiAngleXB8) \
    X(attackS4HiSAngleXBC) \
    X(attackS4LwSAngleXC0) \
    X(attackS4LwAngleXC4) \
    X(attackHi4StickThresholdYxCC) \
    X(attackHi4StickWindowXD0) \
    X(attackLw4StickThresholdYxD4) \
    X(attackLw4StickWindowXD8) \
    X(lCancelInputWindowXE4) \
    X(lCancelLandingLagDivisorXE8) \
    X(knockbackWeightScaleXF4) \
    X(knockbackWeightDecayXF8) \
    X(damageVelocityScaleX100) \
    X(knockbackMaxX108) \
    X(throwKnockbackWeightX10C) \
    X(knockbackDamageBaseX110) \
    X(knockbackDamageScaleX114) \
    X(knockbackWeightSetScaleX118) \
    X(knockbackScaleX11C) \
    X(knockbackBaseX120) \
    X(damageSakuraiAngleAirX144) \
    X(damageSakuraiAngleScaleX148) \
    X(damageSakuraiAngleLowX14C) \
    X(damageSakuraiAngleHighX150) \
    X(hitstunMultiplierX154) \
    X(damageLevelThresholdX158) \
    X(damageLevelThresholdX15C) \
    X(damageLevelThresholdX160) \
    X(damageGroundKnockbackClampX164) \
    X(damageAirVelocityScaleX190) \
    X(damageWallBounceMinVelocityX1B0) \
    X(damageWallBounceDampingX1BC) \
    X(damageSurfaceLockoutX1C0) \
    X(thrownHitboxClearVelocityX1C8) \
    X(damageWallBounceMinVelocityX1E0) \
    X(damageLandingMinVelocityX1E4) \
    X(damageGroundBounceAngleX1E8) \
    X(damageGroundBounceDampingX1EC) \
    X(groundKnockbackFrictionScaleX200) \
    X(knockbackFrameDecayX204) \
    X(damageFallStickThresholdX210) \
    X(damageFallStickWindowX214) \
    X(specialSStickThresholdX218) \
    X(specialLwHiStickThresholdX21C) \
    X(specialSReverseThresholdX220) \
    X(specialNReverseFramesX224) \
    X(damageFlyTopAngleMinX234) \
    X(damageFlyTopAngleMaxX238) \
    X(damageFlyRollPercentX23C) \
    X(damageFlyRollChanceX240) \
    X(downStandStickThresholdX244) \
    X(downRollStickThresholdX248) \
    X(downAttackInputWindowX24C) \
    X(passiveInputWindowX250) \
    X(passiveStandStickThresholdX254) \
    X(downAttackCStickThresholdX7F4) \
    X(startShieldHealthX260) \
    X(minShieldScaleX264) \
    X(guardMinHoldFramesX268) \
    X(shieldDrainRateX278) \
    X(shieldRegenRateX27C) \
    X(shieldDamageScaleX284) \
    X(shieldDamageBaseX288) \
    X(shieldSetoffScaleX28C) \
    X(shieldSetoffBaseX290) \
    X(shieldPushbackScaleX294) \
    X(shieldPushbackMaxX298) \
    X(shieldReflectInputWindowX2A0) \
    X(guardHitReleaseLockoutX2B8) \
    X(hardShieldSizeScaleX2D4) \
    X(lightShieldSizeScaleX2D8) \
    X(hardShieldDamageScaleX2DC) \
    X(lightShieldDamageScaleX2E0) \
    X(hardShieldSetoffScaleX2E4) \
    X(lightShieldSetoffScaleX2E8) \
    X(hardShieldDrainScaleX2EC) \
    X(lightShieldDrainScaleX2F0) \
    X(shieldAlphaMinX2F4) \
    X(furafuraTimerBaseX2F8) \
    X(furafuraTimerMinX2FC) \
    X(furafuraTimerDecrementX300) \
    X(furafuraMashDecrementX304) \
    X(furafuraShieldHealthX280) \
    X(attackerShieldPushbackScaleX3E0) \
    X(attackerShieldPushbackBaseX3E4) \
    X(shieldKnockbackFrameDecayX3E8) \
    X(shieldGroundFrictionMultiplierX3EC) \
    X(grabMashStickThresholdX308) \
    X(grabTimerBaseX354) \
    X(grabTimerHandicapScaleX358) \
    X(grabTimerHandicapBaseX35C) \
    X(grabTimerPortScaleX360) \
    X(grabTimerPortBaseX364) \
    X(grabTimerPercentScaleX368) \
    X(captureCutFrictionScaleX36C) \
    X(captureCutGroundVelocityX370) \
    X(captureJumpVelocityX374) \
    X(captureJumpVelocityYx378) \
    X(throwWeightAnimationScaleX37C) \
    X(captureTimerDecrementX3A4) \
    X(captureMashDecrementX3A8) \
    X(captureJumpButtonWindowX3AC) \
    X(captureMashAnimHoldFramesX3B0) \
    X(captureMashAnimRateX3B4) \
    X(captureJumpGravityThresholdX3B8) \
    X(captureFloorSnapMaxX3BC) \
    X(captureHighThresholdX3C4) \
    X(thrownMashDecrementX3C8) \
    X(reboundDamageScaleX3D0) \
    X(reboundDamageBaseX3D4) \
    X(reboundAccelScaleX3D8) \
    X(reboundAccelBaseX3DC) \
    X(damageSongBaseX624) \
    X(damageSongHandicapScaleX628) \
    X(damageSongHandicapBaseX62C) \
    X(damageSongPortScaleX630) \
    X(damageSongPortBaseX634) \
    X(damageSongPercentScaleX638) \
    X(damageSongTimerDecrementX63C) \
    X(damageSongMashDecrementX640) \
    X(damageSongElement7TimerMultiplierX644) \
    X(damageBindBaseX658) \
    X(damageBindHandicapScaleX65C) \
    X(damageBindHandicapBaseX660) \
    X(damageBindPortScaleX664) \
    X(damageBindPortBaseX668) \
    X(damageBindPercentScaleX66C) \
    X(damageBindTimerDecrementX670) \
    X(damageBindMashDecrementX674) \
    X(burySubmergeFramesX5F4) \
    X(buryBaseX5F8) \
    X(buryHandicapScaleX5FC) \
    X(buryHandicapBaseX600) \
    X(buryPortScaleX604) \
    X(buryPortBaseX608) \
    X(buryPercentScaleX60C) \
    X(buryTimerDecrementX610) \
    X(buryMashDecrementX614) \
    X(buryJumpVelocityYx618) \
    X(buryJumpGravityThresholdX61C) \
    X(buryJumpCollisionFramesX620) \
    X(spotDodgeStickThresholdX314) \
    X(spotDodgeStickWindowX318) \
    X(rollStickThresholdX31C) \
    X(rollStickWindowX320) \
    X(rollFromGuardFlagX324) \
    X(escapeAirTimerX334) \
    X(escapeAirForceX338) \
    X(escapeAirDecayX33C) \
    X(fallSpecialDriftX340) \
    X(landingFallSpecialLagX344) \
    X(fallSpecialPlatformStickThresholdX25C) \
    X(itemScrewJumpMultiplierX800) \
    X(runStopTurnLagX410) \
    X(downWaitAutoStandFramesX424) \
    X(downDamageThresholdX428) \
    X(runBrakeAnimFreezeVelocityX42C) \
    X(runDirectFramesX430) \
    X(jumpMomentumYScaleX438) \
    X(animVelocityScaleX440) \
    X(fallAnimationDriftThresholdX444) \
    X(fallAnimationBlendRateX448) \
    X(shieldStickSmoothingX44C) \
    X(sdiMinStickMagX4B0) \
    X(sdiStickWindowX4B4) \
    X(sdiPosScaleX4B8) \
    X(shieldAsdiPosScaleX4BC) \
    X(shieldSdiDistanceX4C0) \
    X(platformDropStickThresholdX464) \
    X(platformDropStickWindowX468) \
    X(platformDropInitialVelocityX46C) \
    X(platformDropAnimationFramesX470) \
    X(teeterWalkInputThresholdX474) \
    X(teeterForwardDistanceX478) \
    X(teeterBackwardDistanceX47C) \
    X(ledgeNoGrabDownThresholdX480) \
    X(cliffActionPercentThresholdX488) \
    X(cliffWaitAutoReleaseFramesQuickX48C) \
    X(cliffWaitAutoReleaseFramesSlowX490) \
    X(cliffOptionStickThresholdX494) \
    X(cliffCStickAttackThresholdX7F8) \
    X(cliffCStickEscapeThresholdX7FC) \
    X(ledgeCooldownX498) \
    X(damageIceGravityMultiplierX77C) \
    X(damageIceTimerDamageScaleX790) \
    X(damageIceTimerDecrementX794) \
    X(damageIceMashDecrementX798) \
    X(damageIceHitDamageTimerReductionX79C) \
    X(damageIceJumpEscapeFramesX7A4) \
    X(passiveWallTimerX760) \
    X(passiveWallIntangibilityX764) \
    X(wallJumpInputWindowX768) \
    X(wallJumpStickThresholdX76C) \
    X(wallJumpStickWindowX770) \
    X(wallJumpStartupX774) \
    X(passiveWallVelYBaseX778) \
    X(aerialAttackDeadzoneXDC) \
    X(aerialAttackDeadzoneXE0)

#define PF_PACKAGE_FIGHTER_PROPERTY_FIELDS(X) \
    X(walkInitVel) \
    X(walkAccel) \
    X(walkMaxVel) \
    X(slowWalkMax) \
    X(midWalkPoint) \
    X(fastWalkMin) \
    X(grFriction) \
    X(dashInitialVelocity) \
    X(dashRunAccelerationA) \
    X(dashRunAccelerationB) \
    X(dashRunTerminalVelocity) \
    X(groundMaxHorizontalVelocity) \
    X(runAnimationScaling) \
    X(maxRunBrakeFrames) \
    X(jumpHInitialVelocity) \
    X(jumpVInitialVelocity) \
    X(damageScrewVerticalVelocity) \
    X(hopVInitialVelocity) \
    X(groundToAirJumpMomentumMultiplier) \
    X(jumpHMaxVelocity) \
    X(airJumpVMultiplier) \
    X(airJumpHMultiplier) \
    X(grav) \
    X(terminalVel) \
    X(airDriftStickMul) \
    X(aerialDriftBase) \
    X(airFriction) \
    X(airDriftMax) \
    X(airMaxHorizontalVelocity) \
    X(ledgeSnapX) \
    X(ledgeSnapY) \
    X(ledgeSnapHeight) \
    X(ledgeHangX) \
    X(ledgeHangY) \
    X(ledgeClimbX) \
    X(ledgeEscapeX) \
    X(ledgeJumpHorizontalVelocity) \
    X(ledgeJumpVerticalVelocity) \
    X(ledgeDropHorizontalVelocity) \
    X(ledgeDropVerticalVelocity) \
    X(passiveWallHorizontalVelocity) \
    X(wallJumpHorizontalVelocity) \
    X(wallJumpVerticalVelocity) \
    X(passiveCeilHorizontalVelocity) \
    X(damageIceJumpVelocityY) \
    X(damageIceJumpVelocityXMultiplier) \
    X(initialShieldSize) \
    X(shieldBreakInitialVelocity) \
    X(gravity) \
    X(terminalVelocity) \
    X(fastFallTerminalVelocity) \
    X(noImpactLandingVelocity) \
    X(normalLandingLag) \
    X(nairLandingLag) \
    X(fairLandingLag) \
    X(bairLandingLag) \
    X(uairLandingLag) \
    X(dairLandingLag) \
    X(framesToChangeDirectionOnStandingTurn) \
    X(weight) \
    X(weightIndependentThrowsMask) \
    X(rapidJabWindow) \
    X(maxJumps) \
    X(jumpStartupLag) \
    X(modelScale) \
    X(initialWalkSpeed) \
    X(initialDashSpeed) \
    X(initialRunSpeed) \
    X(walkAcceleration) \
    X(runAcceleration) \
    X(maxWalkSpeed) \
    X(friction) \
    X(aerialAcceleration) \
    X(aerialFriction) \
    X(maxAerialHorizontalSpeed) \
    X(initialHorizontalJumpVelocity) \
    X(initialVerticalJumpVelocity) \
    X(maximumShorthopVerticalVelocity)

void writeMeleeCommonData(PackageWriter& writer, const MeleeCommonData& common) {
#define PF_WRITE_FIELD(field) writer.writeI32(common.field);
    PF_PACKAGE_MELEE_COMMON_FIELDS(PF_WRITE_FIELD)
#undef PF_WRITE_FIELD
    writeVec2(writer, common.escapeAirDeadzoneX32C);
}

MeleeCommonData readMeleeCommonData(PackageReader& reader) {
    MeleeCommonData common;
#define PF_READ_FIELD(field) common.field = reader.readI32();
    PF_PACKAGE_MELEE_COMMON_FIELDS(PF_READ_FIELD)
#undef PF_READ_FIELD
    common.escapeAirDeadzoneX32C = readVec2(reader);
    return common;
}

void writeFighterProperties(PackageWriter& writer, const FighterProperties& properties) {
#define PF_WRITE_FIELD(field) writer.writeI32(properties.field);
    PF_PACKAGE_FIGHTER_PROPERTY_FIELDS(PF_WRITE_FIELD)
#undef PF_WRITE_FIELD
    writer.writeBool(properties.shieldSizeScalesWithHealth);
    writer.writeBool(properties.canWallJump);
    writeMeleeCommonData(writer, properties.common);
}

FighterProperties readFighterProperties(PackageReader& reader) {
    FighterProperties properties;
#define PF_READ_FIELD(field) properties.field = reader.readI32();
    PF_PACKAGE_FIGHTER_PROPERTY_FIELDS(PF_READ_FIELD)
#undef PF_READ_FIELD
    properties.shieldSizeScalesWithHealth = reader.readBool();
    properties.canWallJump = reader.readBool();
    properties.common = readMeleeCommonData(reader);
    return properties;
}

void writeShieldDefinition(PackageWriter& writer, const ShieldDefinition& shield) {
    writer.writeI32(shield.startSizeHardShield);
    writer.writeI32(shield.endSizeHardShield);
    writer.writeI32(shield.startSizeLightShield);
    writer.writeI32(shield.endSizeLightShield);
    writer.writeI32(shield.maxHealth);
}

ShieldDefinition readShieldDefinition(PackageReader& reader) {
    ShieldDefinition shield;
    shield.startSizeHardShield = reader.readI32();
    shield.endSizeHardShield = reader.readI32();
    shield.startSizeLightShield = reader.readI32();
    shield.endSizeLightShield = reader.readI32();
    shield.maxHealth = reader.readI32();
    return shield;
}

void writeFighterDefinition(PackageWriter& writer, const FighterPackage& package, const FighterDefinition& fighter) {
    writer.writeString(fighter.name);
    writeFighterProperties(writer, fighter.properties);
    writeShieldDefinition(writer, fighter.shield);
    writer.writeBool(fighter.hasHsdAsset);
    const int32_t assetIndex = assetIndexFor(package, fighter.hsdAsset);
    if (fighter.hasHsdAsset && assetIndex < 0) {
        throw std::runtime_error("fighter package hsd asset reference is missing");
    }
    if (fighter.hasHsdAsset && (!fighter.hsdAsset || fighter.hsdAsset->sourceBytes.empty())) {
        throw std::runtime_error("fighter package hsd asset bytes are missing");
    }
    writer.writeI32(assetIndex);
    writer.writeString(fighter.hsdAsset ? fighter.hsdAsset->name : std::string{});
    writeVector(writer, fighter.packageVariables, [&](const PackageVariableDefinition& variable) {
        writePackageVariable(writer, variable);
    });
    writeVector(writer, fighter.packageScripts, [&](const PackageScript& script) {
        writePackageScript(writer, script);
    });
    writeVector(writer, fighter.hurtboxes, [&](const HurtboxDefinition& hurtbox) {
        writeHurtbox(writer, hurtbox);
    });
    writeVector(writer, fighter.states, [&](const FighterState& state) {
        writeFighterState(writer, state);
    });
}

FighterDefinition readFighterDefinition(
    PackageReader& reader,
    const std::vector<std::shared_ptr<const HsdFighterAnimationAsset>>& hsdAssetPool)
{
    FighterDefinition fighter;
    fighter.name = reader.readString();
    fighter.properties = readFighterProperties(reader);
    fighter.shield = readShieldDefinition(reader);
    fighter.hasHsdAsset = reader.readBool();
    const int32_t assetIndex = reader.readI32();
    const std::string assetName = reader.readString();
    if (fighter.hasHsdAsset && (assetIndex < 0 || assetIndex >= static_cast<int32_t>(hsdAssetPool.size()) ||
        !hsdAssetPool[static_cast<size_t>(assetIndex)]))
    {
        throw std::runtime_error("fighter package hsd asset reference is invalid");
    }
    if (fighter.hasHsdAsset) {
        fighter.hsdAsset = hsdAssetPool[static_cast<size_t>(assetIndex)];
        if (!assetName.empty() && fighter.hsdAsset->name != assetName) {
            throw std::runtime_error("fighter package fighter asset name mismatch");
        }
    }
    fighter.packageVariables = readVector<PackageVariableDefinition>(reader, kMaxCallbacks, "package variable", [&]() {
        return readPackageVariable(reader);
    });
    fighter.packageScripts = readVector<PackageScript>(reader, kMaxCallbacks, "package script", [&]() {
        return readPackageScript(reader);
    });
    fighter.hurtboxes = readVector<HurtboxDefinition>(reader, kMaxHurtboxes, "hurtbox", [&]() {
        return readHurtbox(reader);
    });
    fighter.states = readVector<FighterState>(reader, kMaxStates, "state", [&]() {
        return readFighterState(reader);
    });
    return fighter;
}

void writeGameObjectState(PackageWriter& writer, const GameObjectStateDefinition& state) {
    writer.writeString(state.name);
    writer.writeI32(state.animationLengthFrames);
    writer.writeBool(state.loopAnimation);
    writeFunctionCalls(writer, state.onEnter);
    writeFunctionCalls(writer, state.onFrame);
    writeFunctionCalls(writer, state.onPhysics);
    writeFunctionCalls(writer, state.onCollision);
}

GameObjectStateDefinition readGameObjectState(PackageReader& reader) {
    GameObjectStateDefinition state;
    state.name = reader.readString();
    state.animationLengthFrames = reader.readI32();
    state.loopAnimation = reader.readBool();
    state.onEnter = readFunctionCalls(reader);
    state.onFrame = readFunctionCalls(reader);
    state.onPhysics = readFunctionCalls(reader);
    state.onCollision = readFunctionCalls(reader);
    return state;
}

void writeGameObjectHurtbox(PackageWriter& writer, const GameObjectHurtboxDefinition& hurtbox) {
    writeVec3(writer, hurtbox.startOffset);
    writeVec3(writer, hurtbox.endOffset);
    writer.writeI32(hurtbox.radius);
    writeEnum(writer, hurtbox.state, validHurtboxState, "object hurtbox state");
}

GameObjectHurtboxDefinition readGameObjectHurtbox(PackageReader& reader) {
    GameObjectHurtboxDefinition hurtbox;
    hurtbox.startOffset = readVec3(reader);
    hurtbox.endOffset = readVec3(reader);
    hurtbox.radius = reader.readI32();
    hurtbox.state = readEnum<HurtboxState>(reader, validHurtboxState, "object hurtbox state");
    return hurtbox;
}

void writeGameObjectTouchbox(PackageWriter& writer, const GameObjectTouchboxDefinition& touchbox) {
    writeVec3(writer, touchbox.startOffset);
    writeVec3(writer, touchbox.endOffset);
    writer.writeI32(touchbox.radius);
    writer.writeBool(touchbox.touchFighters);
    writer.writeBool(touchbox.touchObjects);
}

GameObjectTouchboxDefinition readGameObjectTouchbox(PackageReader& reader) {
    GameObjectTouchboxDefinition touchbox;
    touchbox.startOffset = readVec3(reader);
    touchbox.endOffset = readVec3(reader);
    touchbox.radius = reader.readI32();
    touchbox.touchFighters = reader.readBool();
    touchbox.touchObjects = reader.readBool();
    return touchbox;
}

void writeGameObjectDefinition(PackageWriter& writer, const GameObjectDefinition& object) {
    writer.writeString(object.name);
    writeEnum(writer, object.kind, validGameObjectKind, "object kind");
    writer.writeI32(object.initialState);
    writer.writeI32(object.lifetimeFrames);
    writer.writeI32(object.gravity);
    writer.writeI32(object.terminalVelocity);
    writer.writeI32(object.maxDamage);
    writer.writeBool(object.destroyOnHit);
    writer.writeBool(object.destroyOnShield);
    writer.writeBool(object.hitOwner);
    writeVector(writer, object.packageVariables, [&](const PackageVariableDefinition& variable) {
        writePackageVariable(writer, variable);
    });
    writeVector(writer, object.packageScripts, [&](const PackageScript& script) {
        writePackageScript(writer, script);
    });
    writeVector(writer, object.states, [&](const GameObjectStateDefinition& state) {
        writeGameObjectState(writer, state);
    });
    writeFunctionCalls(writer, object.onSpawned);
    writeFunctionCalls(writer, object.onDestroyed);
    writeFunctionCalls(writer, object.onPickedUp);
    writeFunctionCalls(writer, object.onDropped);
    writeFunctionCalls(writer, object.onThrown);
    writeFunctionCalls(writer, object.onDamageDealt);
    writeFunctionCalls(writer, object.onDamageReceived);
    writeFunctionCalls(writer, object.onClanked);
    writeFunctionCalls(writer, object.onReflected);
    writeFunctionCalls(writer, object.onAbsorbed);
    writeFunctionCalls(writer, object.onShieldBounced);
    writeFunctionCalls(writer, object.onHitShield);
    writeFunctionCalls(writer, object.onEnteredAir);
    writeFunctionCalls(writer, object.onEnteredHitlag);
    writeFunctionCalls(writer, object.onExitedHitlag);
    writeFunctionCalls(writer, object.onAccessory);
    writeFunctionCalls(writer, object.onTouched);
    writeFunctionCalls(writer, object.onJumpedOn);
    writeFunctionCalls(writer, object.onGrabDealt);
    writeFunctionCalls(writer, object.onGrabbedForVictim);
    writeFunctionCalls(writer, object.onInteraction);
    writeVector(writer, object.hitboxes, [&](const HitboxDefinition& hitbox) {
        writeHitbox(writer, hitbox);
    });
    writeVector(writer, object.hurtboxes, [&](const GameObjectHurtboxDefinition& hurtbox) {
        writeGameObjectHurtbox(writer, hurtbox);
    });
    writeVector(writer, object.touchboxes, [&](const GameObjectTouchboxDefinition& touchbox) {
        writeGameObjectTouchbox(writer, touchbox);
    });
}

GameObjectDefinition readGameObjectDefinition(PackageReader& reader) {
    GameObjectDefinition object;
    object.name = reader.readString();
    object.kind = readEnum<GameObjectKind>(reader, validGameObjectKind, "object kind");
    object.initialState = reader.readI32();
    object.lifetimeFrames = reader.readI32();
    object.gravity = reader.readI32();
    object.terminalVelocity = reader.readI32();
    object.maxDamage = reader.readI32();
    object.destroyOnHit = reader.readBool();
    object.destroyOnShield = reader.readBool();
    object.hitOwner = reader.readBool();
    object.packageVariables = readVector<PackageVariableDefinition>(reader, kMaxCallbacks, "object package variable", [&]() {
        return readPackageVariable(reader);
    });
    object.packageScripts = readVector<PackageScript>(reader, kMaxCallbacks, "object package script", [&]() {
        return readPackageScript(reader);
    });
    object.states = readVector<GameObjectStateDefinition>(reader, kMaxStates, "object state", [&]() {
        return readGameObjectState(reader);
    });
    object.onSpawned = readFunctionCalls(reader);
    object.onDestroyed = readFunctionCalls(reader);
    object.onPickedUp = readFunctionCalls(reader);
    object.onDropped = readFunctionCalls(reader);
    object.onThrown = readFunctionCalls(reader);
    object.onDamageDealt = readFunctionCalls(reader);
    object.onDamageReceived = readFunctionCalls(reader);
    object.onClanked = readFunctionCalls(reader);
    object.onReflected = readFunctionCalls(reader);
    object.onAbsorbed = readFunctionCalls(reader);
    object.onShieldBounced = readFunctionCalls(reader);
    object.onHitShield = readFunctionCalls(reader);
    object.onEnteredAir = readFunctionCalls(reader);
    object.onEnteredHitlag = readFunctionCalls(reader);
    object.onExitedHitlag = readFunctionCalls(reader);
    object.onAccessory = readFunctionCalls(reader);
    object.onTouched = readFunctionCalls(reader);
    object.onJumpedOn = readFunctionCalls(reader);
    object.onGrabDealt = readFunctionCalls(reader);
    object.onGrabbedForVictim = readFunctionCalls(reader);
    object.onInteraction = readFunctionCalls(reader);
    object.hitboxes = readVector<HitboxDefinition>(reader, kMaxHitboxes, "object hitbox", [&]() {
        return readHitbox(reader);
    });
    object.hurtboxes = readVector<GameObjectHurtboxDefinition>(reader, kMaxHurtboxes, "object hurtbox", [&]() {
        return readGameObjectHurtbox(reader);
    });
    object.touchboxes = readVector<GameObjectTouchboxDefinition>(reader, kMaxTouchboxes, "object touchbox", [&]() {
        return readGameObjectTouchbox(reader);
    });
    return object;
}

bool fail(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
    return false;
}

void writeHsdAsset(PackageWriter& writer, const std::shared_ptr<const HsdFighterAnimationAsset>& asset) {
    writer.writeString(asset ? asset->name : std::string{});
    writer.writeBytes(asset ? asset->sourceBytes : std::vector<uint8_t>{});
}

std::shared_ptr<const HsdFighterAnimationAsset> readHsdAsset(
    PackageReader& reader,
    const std::vector<std::shared_ptr<const HsdFighterAnimationAsset>>& hsdAssetPool,
    size_t index)
{
    const std::string assetName = reader.readString();
    const std::vector<uint8_t> assetBytes = reader.readBytes(kMaxAssetBytes, "asset bytes");
    if (!assetBytes.empty()) {
        HsdFighterAnimationAsset asset = loadHsdFighterAnimationAssetFromBytes(assetBytes);
        if (!assetName.empty() && asset.name != assetName) {
            throw std::runtime_error("fighter package embedded asset name mismatch");
        }
        return std::make_shared<const HsdFighterAnimationAsset>(std::move(asset));
    }
    if (index < hsdAssetPool.size()) {
        if (!assetName.empty() && hsdAssetPool[index] && hsdAssetPool[index]->name != assetName) {
            throw std::runtime_error("fighter package pooled asset name mismatch");
        }
        return hsdAssetPool[index];
    }
    return nullptr;
}

} // namespace

std::vector<uint8_t> writeFighterPackage(const FighterPackage& package, std::string* error) {
    try {
        PackageWriter writer;
        writer.writeMagic("PFFP");
        writer.writeU32(kPackageVersion);
        writer.writeString(package.name);
        writer.writeU32(package.version);
        writeVector(writer, package.hsdAssets, [&](const std::shared_ptr<const HsdFighterAnimationAsset>& asset) {
            writeHsdAsset(writer, asset);
        });
        writeVector(writer, package.fighters, [&](const FighterDefinition& fighter) {
            writeFighterDefinition(writer, package, fighter);
        });
        writeVector(writer, package.objects, [&](const GameObjectDefinition& object) {
            writeGameObjectDefinition(writer, object);
        });
        return std::move(writer.bytes);
    } catch (const std::exception& ex) {
        if (error) {
            *error = ex.what();
        }
        return {};
    }
}

bool saveFighterPackage(const std::string& path, const FighterPackage& package, std::string* error) {
    const std::vector<uint8_t> bytes = writeFighterPackage(package, error);
    if (bytes.empty()) {
        return false;
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return fail(error, "failed to open fighter package for writing");
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        return fail(error, "failed to write fighter package");
    }
    return true;
}

bool readFighterPackage(
    const std::vector<uint8_t>& bytes,
    FighterPackage& package,
    std::string* error,
    const std::vector<std::shared_ptr<const HsdFighterAnimationAsset>>& hsdAssetPool)
{
    try {
        PackageReader reader(bytes);
        reader.readMagic("PFFP");
        const uint32_t formatVersion = reader.readU32();
        if (formatVersion != kPackageVersion) {
            return fail(error, "unsupported fighter package version");
        }

        FighterPackage loaded;
        loaded.name = reader.readString();
        loaded.version = reader.readU32();
        const uint32_t assetCount = reader.readCount(kMaxAssets, "asset");
        loaded.hsdAssets.reserve(assetCount);
        for (uint32_t i = 0; i < assetCount; ++i) {
            loaded.hsdAssets.push_back(readHsdAsset(reader, hsdAssetPool, i));
        }
        loaded.fighters = readVector<FighterDefinition>(reader, kMaxFighters, "fighter", [&]() {
            return readFighterDefinition(reader, loaded.hsdAssets);
        });
        loaded.objects = readVector<GameObjectDefinition>(reader, kMaxObjects, "object", [&]() {
            return readGameObjectDefinition(reader);
        });
        reader.requireFinished();
        package = std::move(loaded);
        return true;
    } catch (const std::exception& ex) {
        return fail(error, ex.what());
    }
}

bool loadFighterPackage(
    const std::string& path,
    FighterPackage& package,
    std::string* error,
    const std::vector<std::shared_ptr<const HsdFighterAnimationAsset>>& hsdAssetPool)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return fail(error, "failed to open fighter package for reading");
    }
    file.seekg(0, std::ios::end);
    const std::streamsize size = file.tellg();
    if (size < 0) {
        return fail(error, "failed to measure fighter package");
    }
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!file) {
            return fail(error, "failed to read fighter package");
        }
    }
    return readFighterPackage(bytes, package, error, hsdAssetPool);
}

uint32_t fighterPackageChecksum(const std::vector<uint8_t>& bytes) {
    uint32_t hash = 2166136261u;
    for (const uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

} // namespace pf
