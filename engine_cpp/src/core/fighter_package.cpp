#include "core/fighter_package.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
constexpr int32_t kPackageInputButtonMask =
    ButtonJump | ButtonSpecial | ButtonAttack | ButtonShield | ButtonGrab | ButtonPause | ButtonTaunt;
constexpr uint32_t kMaxTouchboxes = 4096;
constexpr uint32_t kMaxAnimationJoints = 4096;
constexpr uint32_t kMaxAnimationClips = 2048;
constexpr uint32_t kMaxAnimationTracks = 65536;
constexpr uint32_t kMaxAnimationKeys = 65536;
constexpr uint32_t kMaxMeshTextures = 4096;
constexpr uint32_t kMaxMeshBatches = 65536;
constexpr uint32_t kMaxMeshVertices = 1024 * 1024;
constexpr uint32_t kMaxMeshMatrices = 65536;
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
    case PackageScriptOp::SetVarFromVar:
    case PackageScriptOp::AddVarImmediate:
    case PackageScriptOp::AddVar:
    case PackageScriptOp::ScaleVarFixed:
    case PackageScriptOp::SetVarRandom:
    case PackageScriptOp::SetVarFrame:
    case PackageScriptOp::SetVarStateFrame:
    case PackageScriptOp::SetVarStateIndex:
    case PackageScriptOp::SetVarGrounded:
    case PackageScriptOp::SetVarFacing:
    case PackageScriptOp::SetVarFighterStateFrame:
    case PackageScriptOp::SetVarFighterStateIndex:
    case PackageScriptOp::SetVarFighterGrounded:
    case PackageScriptOp::SetVarFighterFacing:
    case PackageScriptOp::SetVarFighterJumpsUsed:
    case PackageScriptOp::SetVarFighterJumpsRemaining:
    case PackageScriptOp::SetFighterJumpsUsed:
    case PackageScriptOp::SetFighterJumpsUsedFromVar:
    case PackageScriptOp::SetVarFighterCommandVar:
    case PackageScriptOp::SetFighterCommandVarImmediate:
    case PackageScriptOp::SetFighterCommandVarFromVar:
    case PackageScriptOp::SetVarFighterThrowFlag:
    case PackageScriptOp::SetFighterThrowFlagImmediate:
    case PackageScriptOp::SetFighterThrowFlagFromVar:
    case PackageScriptOp::SetVarFighterHeldObject:
    case PackageScriptOp::SetVarFighterGrabbedFighter:
    case PackageScriptOp::SetVarFighterGrabberFighter:
    case PackageScriptOp::SetVarFighterHitlag:
    case PackageScriptOp::SetVarFighterHitstun:
    case PackageScriptOp::SetVarFighterDamageHitboxOwner:
    case PackageScriptOp::SetVarFighterThrownHitboxOwner:
    case PackageScriptOp::SetVarFighterPercent:
    case PackageScriptOp::SetVarFighterShield:
    case PackageScriptOp::SetVarFighterPositionX:
    case PackageScriptOp::SetVarFighterPositionY:
    case PackageScriptOp::SetVarFighterGroundVelocity:
    case PackageScriptOp::SetVarFighterAirVelocityX:
    case PackageScriptOp::SetVarFighterAirVelocityY:
    case PackageScriptOp::SetVarFighterAnimationFrame:
    case PackageScriptOp::SetVarFighterAnimationRate:
    case PackageScriptOp::SetVarObjectOwner:
    case PackageScriptOp::SetVarObjectHeldBy:
    case PackageScriptOp::SetVarObjectGrabVictim:
    case PackageScriptOp::SetVarObjectLastFighter:
    case PackageScriptOp::SetVarObjectLastObject:
    case PackageScriptOp::SetVarObjectDamage:
    case PackageScriptOp::SetObjectDamage:
    case PackageScriptOp::SetObjectDamageFromVar:
    case PackageScriptOp::SetVarObjectHitlag:
    case PackageScriptOp::SetObjectHitlag:
    case PackageScriptOp::SetObjectHitlagFromVar:
    case PackageScriptOp::SetVarObjectGroundSegment:
    case PackageScriptOp::SetVarObjectPositionX:
    case PackageScriptOp::SetVarObjectPositionY:
    case PackageScriptOp::SetVarObjectVelocityX:
    case PackageScriptOp::SetVarObjectVelocityY:
    case PackageScriptOp::SetVarObjectAnimationFrame:
    case PackageScriptOp::SetVarObjectAnimationRate:
    case PackageScriptOp::SetObjectOwner:
    case PackageScriptOp::SetObjectOwnerFromVar:
    case PackageScriptOp::SetVarOwnedObjectCount:
    case PackageScriptOp::SetVarOwnerFighterVar:
    case PackageScriptOp::SetOwnerFighterVarImmediate:
    case PackageScriptOp::SetOwnerFighterVarFromVar:
    case PackageScriptOp::SetVarButtonDown:
    case PackageScriptOp::SetVarButtonPressed:
    case PackageScriptOp::SetVarStickX:
    case PackageScriptOp::SetVarStickY:
    case PackageScriptOp::SetVarCStickX:
    case PackageScriptOp::SetVarCStickY:
    case PackageScriptOp::SetVarShield:
    case PackageScriptOp::SetGroundVelocity:
    case PackageScriptOp::SetAirVelocityX:
    case PackageScriptOp::SetAirVelocityY:
    case PackageScriptOp::SetPositionX:
    case PackageScriptOp::SetPositionY:
    case PackageScriptOp::SetFacing:
    case PackageScriptOp::SetGroundVelocityFromVar:
    case PackageScriptOp::SetAirVelocityXFromVar:
    case PackageScriptOp::SetAirVelocityYFromVar:
    case PackageScriptOp::SetPositionXFromVar:
    case PackageScriptOp::SetPositionYFromVar:
    case PackageScriptOp::SetFacingFromVar:
    case PackageScriptOp::ChangeState:
    case PackageScriptOp::SpawnObject:
    case PackageScriptOp::SpawnObjectFromVars:
    case PackageScriptOp::SpawnProjectile:
    case PackageScriptOp::SpawnProjectileFromVars:
    case PackageScriptOp::DestroyObject:
    case PackageScriptOp::DestroyOwnedObjects:
    case PackageScriptOp::SkipIfVarLessThanImmediate:
    case PackageScriptOp::SkipIfVarLessThanVar:
    case PackageScriptOp::SkipIfVarEqualImmediate:
    case PackageScriptOp::SkipIfVarEqualVar:
    case PackageScriptOp::JumpRelative:
    case PackageScriptOp::CallScript:
    case PackageScriptOp::SwitchFighterDefinition:
    case PackageScriptOp::SpawnFighter:
    case PackageScriptOp::SetAnimationRate:
    case PackageScriptOp::SetAnimationRateFromVar:
    case PackageScriptOp::SetAnimationFrame:
    case PackageScriptOp::SetAnimationFrameFromVar:
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
    case SubactionType::SpawnProjectile:
    case SubactionType::CallScript:
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
    case InterruptCondition::PackageVarAtLeast:
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

bool validAnimationChannel(AnimationChannel channel) {
    switch (channel) {
    case AnimationChannel::TranslateX:
    case AnimationChannel::TranslateY:
    case AnimationChannel::TranslateZ:
    case AnimationChannel::RotateX:
    case AnimationChannel::RotateY:
    case AnimationChannel::RotateZ:
    case AnimationChannel::ScaleX:
    case AnimationChannel::ScaleY:
    case AnimationChannel::ScaleZ:
        return true;
    }
    return false;
}

bool validAnimationInterpolation(AnimationInterpolation interpolation) {
    switch (interpolation) {
    case AnimationInterpolation::Constant:
    case AnimationInterpolation::Linear:
    case AnimationInterpolation::Spline:
        return true;
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
    writer.writeI32(rule.packageVariable);
    writer.writeI32(rule.packageValue);
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
    rule.packageVariable = reader.readI32();
    rule.packageValue = reader.readI32();
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

void writeFighterEcbDefinition(PackageWriter& writer, const FighterEcbDefinition& ecb) {
    writer.writeBool(ecb.enabled);
    for (const Vec2& point : ecb.points) {
        writeVec2(writer, point);
    }
}

FighterEcbDefinition readFighterEcbDefinition(PackageReader& reader) {
    FighterEcbDefinition ecb;
    ecb.enabled = reader.readBool();
    for (Vec2& point : ecb.points) {
        point = readVec2(reader);
    }
    return ecb;
}

void writeAnimationJoint(PackageWriter& writer, const AnimationJoint& joint) {
    writer.writeI32(joint.parent);
    writer.writeString(joint.name);
    writer.writeU32(joint.flags);
    writeVec3(writer, joint.translation);
    writeVec3(writer, joint.rotation);
    writeVec3(writer, joint.scale);
}

AnimationJoint readAnimationJoint(PackageReader& reader) {
    AnimationJoint joint;
    joint.parent = reader.readI32();
    joint.name = reader.readString();
    joint.flags = reader.readU32();
    joint.translation = readVec3(reader);
    joint.rotation = readVec3(reader);
    joint.scale = readVec3(reader);
    return joint;
}

void writeAnimationKey(PackageWriter& writer, const AnimationKey& key) {
    writer.writeI32(key.frame);
    writer.writeI32(key.value);
    writer.writeI32(key.tangent);
    writeEnum(writer, key.interpolation, validAnimationInterpolation, "animation interpolation");
}

AnimationKey readAnimationKey(PackageReader& reader) {
    AnimationKey key;
    key.frame = reader.readI32();
    key.value = reader.readI32();
    key.tangent = reader.readI32();
    key.interpolation = readEnum<AnimationInterpolation>(reader, validAnimationInterpolation, "animation interpolation");
    return key;
}

void writeAnimationTrack(PackageWriter& writer, const AnimationTrack& track) {
    writer.writeI32(track.joint);
    writeEnum(writer, track.channel, validAnimationChannel, "animation channel");
    writeVector(writer, track.keys, [&](const AnimationKey& key) {
        writeAnimationKey(writer, key);
    });
}

AnimationTrack readAnimationTrack(PackageReader& reader) {
    AnimationTrack track;
    track.joint = reader.readI32();
    track.channel = readEnum<AnimationChannel>(reader, validAnimationChannel, "animation channel");
    track.keys = readVector<AnimationKey>(reader, kMaxAnimationKeys, "animation key", [&]() {
        return readAnimationKey(reader);
    });
    return track;
}

void writeAnimationClip(PackageWriter& writer, const AnimationClip& clip) {
    writer.writeString(clip.name);
    writer.writeI32(clip.actionIndex);
    writer.writeU32(clip.actionFlags);
    writer.writeI32(clip.defaultBlendFrames);
    writer.writeI32(clip.frameCount);
    writeVector(writer, clip.tracks, [&](const AnimationTrack& track) {
        writeAnimationTrack(writer, track);
    });
}

AnimationClip readAnimationClip(PackageReader& reader) {
    AnimationClip clip;
    clip.name = reader.readString();
    clip.actionIndex = reader.readI32();
    clip.actionFlags = reader.readU32();
    clip.defaultBlendFrames = reader.readI32();
    clip.frameCount = reader.readI32();
    clip.tracks = readVector<AnimationTrack>(reader, kMaxAnimationTracks, "animation track", [&]() {
        return readAnimationTrack(reader);
    });
    return clip;
}

void writeFloatArray16(PackageWriter& writer, const std::array<float, 16>& values) {
    for (float value : values) {
        writer.writePod(value);
    }
}

std::array<float, 16> readFloatArray16(PackageReader& reader) {
    std::array<float, 16> values{};
    for (float& value : values) {
        value = reader.readPod<float>();
    }
    return values;
}

void writeByteArray4(PackageWriter& writer, const std::array<uint8_t, 4>& values) {
    for (uint8_t value : values) {
        writer.writeU8(value);
    }
}

std::array<uint8_t, 4> readByteArray4(PackageReader& reader) {
    std::array<uint8_t, 4> values{};
    for (uint8_t& value : values) {
        value = reader.readU8();
    }
    return values;
}

void writeMeshTexture(PackageWriter& writer, const HsdMeshTexture& texture) {
    writer.writeI32(texture.width);
    writer.writeI32(texture.height);
    writer.writeBytes(texture.rgba);
}

HsdMeshTexture readMeshTexture(PackageReader& reader) {
    HsdMeshTexture texture;
    texture.width = reader.readI32();
    texture.height = reader.readI32();
    texture.rgba = reader.readBytes(kMaxAssetBytes, "mesh texture bytes");
    return texture;
}

void writeMeshInfluence(PackageWriter& writer, const HsdMeshVertexInfluence& influence) {
    writer.writeI32(influence.bone);
    writer.writePod(influence.weight);
}

HsdMeshVertexInfluence readMeshInfluence(PackageReader& reader) {
    HsdMeshVertexInfluence influence;
    influence.bone = reader.readI32();
    influence.weight = reader.readPod<float>();
    return influence;
}

void writeMeshVertex(PackageWriter& writer, const HsdMeshVertex& vertex) {
    writeVec3(writer, vertex.position);
    writeVec3(writer, vertex.normal);
    writer.writePod(vertex.u);
    writer.writePod(vertex.v);
    writeByteArray4(writer, vertex.color);
    for (const HsdMeshVertexInfluence& influence : vertex.influences) {
        writeMeshInfluence(writer, influence);
    }
}

HsdMeshVertex readMeshVertex(PackageReader& reader) {
    HsdMeshVertex vertex;
    vertex.position = readVec3(reader);
    vertex.normal = readVec3(reader);
    vertex.u = reader.readPod<float>();
    vertex.v = reader.readPod<float>();
    vertex.color = readByteArray4(reader);
    for (HsdMeshVertexInfluence& influence : vertex.influences) {
        influence = readMeshInfluence(reader);
    }
    return vertex;
}

void writeMeshBatch(PackageWriter& writer, const HsdMeshBatch& batch) {
    writer.writeI32(batch.parentBone);
    writer.writeI32(batch.singleBindBone);
    writer.writeI32(batch.dobjIndex);
    writer.writeI32(batch.modelPartIndex);
    writer.writeI32(batch.modelPartState);
    writer.writeBool(batch.hiddenByVisibilityTable);
    writer.writeU32(batch.parentFlags);
    writer.writeU32(batch.polygonFlags);
    writer.writeBool(batch.hasEnvelopes);
    writer.writeBool(batch.unknown2);
    writer.writeBool(batch.shapeSetAverage);
    writer.writeI32(batch.texture);
    writer.writeI32(batch.textureColorOperation);
    writer.writeI32(batch.textureAlphaOperation);
    writer.writePod(batch.textureBlend);
    writeByteArray4(writer, batch.materialColor);
    writeVector(writer, batch.vertices, [&](const HsdMeshVertex& vertex) {
        writeMeshVertex(writer, vertex);
    });
}

HsdMeshBatch readMeshBatch(PackageReader& reader) {
    HsdMeshBatch batch;
    batch.parentBone = reader.readI32();
    batch.singleBindBone = reader.readI32();
    batch.dobjIndex = reader.readI32();
    batch.modelPartIndex = reader.readI32();
    batch.modelPartState = reader.readI32();
    batch.hiddenByVisibilityTable = reader.readBool();
    batch.parentFlags = reader.readU32();
    batch.polygonFlags = reader.readU32();
    batch.hasEnvelopes = reader.readBool();
    batch.unknown2 = reader.readBool();
    batch.shapeSetAverage = reader.readBool();
    batch.texture = reader.readI32();
    batch.textureColorOperation = reader.readI32();
    batch.textureAlphaOperation = reader.readI32();
    batch.textureBlend = reader.readPod<float>();
    batch.materialColor = readByteArray4(reader);
    batch.vertices = readVector<HsdMeshVertex>(reader, kMaxMeshVertices, "mesh vertex", [&]() {
        return readMeshVertex(reader);
    });
    return batch;
}

void writeFighterMesh(PackageWriter& writer, const HsdFighterMesh& mesh) {
    writeVector(writer, mesh.inverseBindMatrices, [&](const std::array<float, 16>& matrix) {
        writeFloatArray16(writer, matrix);
    });
    writeVector(writer, mesh.textures, [&](const HsdMeshTexture& texture) {
        writeMeshTexture(writer, texture);
    });
    writeVector(writer, mesh.batches, [&](const HsdMeshBatch& batch) {
        writeMeshBatch(writer, batch);
    });
}

HsdFighterMesh readFighterMesh(PackageReader& reader) {
    HsdFighterMesh mesh;
    mesh.inverseBindMatrices = readVector<std::array<float, 16>>(reader, kMaxMeshMatrices, "mesh inverse bind matrix", [&]() {
        return readFloatArray16(reader);
    });
    mesh.textures = readVector<HsdMeshTexture>(reader, kMaxMeshTextures, "mesh texture", [&]() {
        return readMeshTexture(reader);
    });
    mesh.batches = readVector<HsdMeshBatch>(reader, kMaxMeshBatches, "mesh batch", [&]() {
        return readMeshBatch(reader);
    });
    return mesh;
}

void validateAuthoredAnimationData(
    const std::vector<AnimationJoint>& skeleton,
    const std::vector<AnimationClip>& clips)
{
    for (size_t i = 0; i < skeleton.size(); ++i) {
        if (skeleton[i].name.empty()) {
            throw std::runtime_error("fighter package authored skeleton joint name is invalid");
        }
        const int parent = skeleton[i].parent;
        if (parent < -1 || parent >= static_cast<int>(i)) {
            throw std::runtime_error("fighter package authored skeleton parent is invalid");
        }
        if (skeleton[i].scale.x <= 0 || skeleton[i].scale.y <= 0 || skeleton[i].scale.z <= 0) {
            throw std::runtime_error("fighter package authored skeleton scale is invalid");
        }
        for (size_t otherIndex = 0; otherIndex < i; ++otherIndex) {
            if (skeleton[otherIndex].name == skeleton[i].name) {
                throw std::runtime_error("fighter package authored skeleton joint name is duplicate");
            }
        }
    }

    for (size_t clipIndex = 0; clipIndex < clips.size(); ++clipIndex) {
        const AnimationClip& clip = clips[clipIndex];
        if (clip.name.empty()) {
            throw std::runtime_error("fighter package authored animation clip name is invalid");
        }
        if (clip.actionIndex < 0) {
            throw std::runtime_error("fighter package authored animation action index is invalid");
        }
        for (size_t otherIndex = 0; otherIndex < clipIndex; ++otherIndex) {
            if (clips[otherIndex].name == clip.name) {
                throw std::runtime_error("fighter package authored animation clip name is duplicate");
            }
            if (clips[otherIndex].actionIndex == clip.actionIndex) {
                throw std::runtime_error("fighter package authored animation action index is duplicate");
            }
        }
        if (clip.frameCount < 0) {
            throw std::runtime_error("fighter package authored animation frame count is invalid");
        }
        for (const AnimationTrack& track : clip.tracks) {
            if (track.joint < 0 || track.joint >= static_cast<int>(skeleton.size())) {
                throw std::runtime_error("fighter package authored animation track joint is invalid");
            }
            Fix previousFrame = -1;
            bool hasPreviousFrame = false;
            for (const AnimationKey& key : track.keys) {
                if (key.frame < 0 || (clip.frameCount > 0 && key.frame > clip.frameCount)) {
                    throw std::runtime_error("fighter package authored animation key frame is invalid");
                }
                if (hasPreviousFrame && key.frame <= previousFrame) {
                    throw std::runtime_error("fighter package authored animation key frames are not strictly ordered");
                }
                previousFrame = key.frame;
                hasPreviousFrame = true;
            }
        }
    }
}

bool validAuthoredMeshBone(int bone, size_t skeletonSize) {
    return bone < 0 || static_cast<size_t>(bone) < skeletonSize;
}

void validateAuthoredMeshData(const HsdFighterMesh& mesh, const std::vector<AnimationJoint>& skeleton) {
    for (const HsdMeshTexture& texture : mesh.textures) {
        if (texture.width < 0 || texture.height < 0) {
            throw std::runtime_error("fighter package authored mesh texture size is invalid");
        }
        const uint64_t expectedBytes = static_cast<uint64_t>(texture.width) *
            static_cast<uint64_t>(texture.height) * 4u;
        if (expectedBytes > kMaxAssetBytes || texture.rgba.size() != expectedBytes) {
            throw std::runtime_error("fighter package authored mesh texture bytes are invalid");
        }
    }
    for (const HsdMeshBatch& batch : mesh.batches) {
        if (!validAuthoredMeshBone(batch.parentBone, skeleton.size()) ||
            !validAuthoredMeshBone(batch.singleBindBone, skeleton.size()))
        {
            throw std::runtime_error("fighter package authored mesh batch bone reference is invalid");
        }
        if (batch.texture < -1 || batch.texture >= static_cast<int>(mesh.textures.size())) {
            throw std::runtime_error("fighter package authored mesh texture reference is invalid");
        }
        if (batch.vertices.empty()) {
            throw std::runtime_error("fighter package authored mesh batch has no vertices");
        }
        if ((batch.vertices.size() % 3) != 0) {
            throw std::runtime_error("fighter package authored mesh batch triangle list is invalid");
        }
        for (const HsdMeshVertex& vertex : batch.vertices) {
            float weightSum = 0.0f;
            for (const HsdMeshVertexInfluence& influence : vertex.influences) {
                if (!std::isfinite(influence.weight) ||
                    !validAuthoredMeshBone(influence.bone, skeleton.size()) ||
                    influence.weight < 0.0f || influence.weight > 1.0f ||
                    (influence.bone < 0 && influence.weight > 0.0f))
                {
                    throw std::runtime_error("fighter package authored mesh vertex influence is invalid");
                }
                weightSum += influence.weight;
            }
            if (weightSum <= 0.0f || weightSum > 1.0001f) {
                throw std::runtime_error("fighter package authored mesh vertex influence weights are invalid");
            }
        }
    }
}

bool hasName(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool objectHasKind(const std::vector<GameObjectDefinition>& objects, const std::string& name, GameObjectKind kind) {
    const auto found = std::find_if(objects.begin(), objects.end(), [&](const GameObjectDefinition& object) {
        return object.name == name;
    });
    return found != objects.end() && found->kind == kind;
}

bool hasResolvableStateTarget(const std::vector<std::string>& names, const std::string& name) {
    if (hasName(names, name)) {
        return true;
    }
    return name == "AppealS" && (hasName(names, "AppealSR") || hasName(names, "AppealSL"));
}

void requireUniqueNonemptyNames(const std::vector<std::string>& names, const char* label) {
    std::vector<std::string> seenNames;
    seenNames.reserve(names.size());
    for (const std::string& name : names) {
        if (name.empty()) {
            throw std::runtime_error(std::string("fighter package ") + label + " name is invalid");
        }
        if (hasName(seenNames, name)) {
            throw std::runtime_error(std::string("fighter package ") + label + " name is duplicate");
        }
        seenNames.push_back(name);
    }
}

void requireNonemptyNames(const std::vector<std::string>& names, const char* label) {
    for (const std::string& name : names) {
        if (name.empty()) {
            throw std::runtime_error(std::string("fighter package ") + label + " name is invalid");
        }
    }
}

std::vector<std::string> fighterNames(const FighterPackage& package) {
    std::vector<std::string> names;
    names.reserve(package.fighters.size());
    for (const FighterDefinition& fighter : package.fighters) {
        names.push_back(fighter.name);
    }
    return names;
}

std::vector<std::string> fighterStateNames(const FighterDefinition& fighter) {
    std::vector<std::string> names;
    names.reserve(fighter.states.size());
    for (const FighterState& state : fighter.states) {
        names.push_back(state.name);
    }
    return names;
}

std::vector<std::string> objectStateNames(const GameObjectDefinition& object) {
    std::vector<std::string> names;
    names.reserve(object.states.size());
    for (const GameObjectStateDefinition& state : object.states) {
        names.push_back(state.name);
    }
    return names;
}

std::vector<std::string> objectNames(const FighterPackage& package) {
    std::vector<std::string> names;
    names.reserve(package.objects.size());
    for (const GameObjectDefinition& object : package.objects) {
        names.push_back(object.name);
    }
    return names;
}

std::vector<std::string> variableNames(const std::vector<PackageVariableDefinition>& variables) {
    std::vector<std::string> names;
    names.reserve(variables.size());
    for (const PackageVariableDefinition& variable : variables) {
        names.push_back(variable.name);
    }
    return names;
}

std::vector<std::string> scriptNames(const std::vector<PackageScript>& scripts) {
    std::vector<std::string> names;
    names.reserve(scripts.size());
    for (const PackageScript& script : scripts) {
        names.push_back(script.name);
    }
    return names;
}

void requireVariableIndex(int index, int variableCount, const char* label) {
    if (index < 0 || index >= variableCount) {
        throw std::runtime_error(std::string("fighter package script ") + label + " variable reference is invalid");
    }
}

void validatePackageScriptInstruction(
    const PackageScriptInstruction& instruction,
    int variableCount,
    const std::vector<std::string>& fighterNames,
    const std::vector<std::string>& stateNames,
    const std::vector<std::string>& scriptNames,
    const std::vector<std::string>& packageObjectNames,
    const std::vector<GameObjectDefinition>& packageObjects,
    bool allowResolvableStateTargets,
    bool allowFighterTargets,
    bool allowInputReads,
    bool allowFighterContextReads,
    bool allowObjectLifecycleOps,
    bool allowObjectContextReads,
    int instructionIndex,
    int instructionCount)
{
    switch (instruction.op) {
    case PackageScriptOp::Nop:
        break;
    case PackageScriptOp::SetVarImmediate:
    case PackageScriptOp::AddVarImmediate:
    case PackageScriptOp::SetVarFrame:
    case PackageScriptOp::SetVarStateFrame:
    case PackageScriptOp::SetVarStateIndex:
    case PackageScriptOp::SetVarGrounded:
    case PackageScriptOp::SetVarFacing:
        requireVariableIndex(instruction.dst, variableCount, "destination");
        break;
    case PackageScriptOp::SetVarFromVar:
        requireVariableIndex(instruction.dst, variableCount, "destination");
        requireVariableIndex(instruction.srcA, variableCount, "source");
        break;
    case PackageScriptOp::SetVarFighterStateFrame:
    case PackageScriptOp::SetVarFighterStateIndex:
    case PackageScriptOp::SetVarFighterGrounded:
    case PackageScriptOp::SetVarFighterFacing:
    case PackageScriptOp::SetVarFighterJumpsUsed:
    case PackageScriptOp::SetVarFighterJumpsRemaining:
    case PackageScriptOp::SetVarFighterPercent:
    case PackageScriptOp::SetVarFighterShield:
    case PackageScriptOp::SetVarFighterPositionX:
    case PackageScriptOp::SetVarFighterPositionY:
    case PackageScriptOp::SetVarFighterGroundVelocity:
    case PackageScriptOp::SetVarFighterAirVelocityX:
    case PackageScriptOp::SetVarFighterAirVelocityY:
    case PackageScriptOp::SetVarFighterAnimationFrame:
    case PackageScriptOp::SetVarFighterAnimationRate:
        requireVariableIndex(instruction.dst, variableCount, "destination");
        if (!allowFighterContextReads) {
            throw std::runtime_error("fighter package script fighter context read is invalid");
        }
        break;
    case PackageScriptOp::SetFighterJumpsUsed:
        if (!allowFighterContextReads || allowObjectContextReads || instruction.intValue < 0) {
            throw std::runtime_error("fighter package script fighter jump write is invalid");
        }
        break;
    case PackageScriptOp::SetFighterJumpsUsedFromVar:
        if (!allowFighterContextReads || allowObjectContextReads) {
            throw std::runtime_error("fighter package script fighter jump write is invalid");
        }
        requireVariableIndex(instruction.srcA, variableCount, "source");
        break;
    case PackageScriptOp::SetVarFighterCommandVar:
        requireVariableIndex(instruction.dst, variableCount, "destination");
        if (!allowFighterContextReads || instruction.intValue < 0 || instruction.intValue >= 4) {
            throw std::runtime_error("fighter package script fighter command variable read is invalid");
        }
        break;
    case PackageScriptOp::SetFighterCommandVarImmediate:
        if (!allowFighterContextReads || allowObjectContextReads || instruction.dst < 0 || instruction.dst >= 4) {
            throw std::runtime_error("fighter package script fighter command variable write is invalid");
        }
        break;
    case PackageScriptOp::SetFighterCommandVarFromVar:
        if (!allowFighterContextReads || allowObjectContextReads || instruction.dst < 0 || instruction.dst >= 4) {
            throw std::runtime_error("fighter package script fighter command variable write is invalid");
        }
        requireVariableIndex(instruction.srcA, variableCount, "source");
        break;
    case PackageScriptOp::SetVarFighterThrowFlag:
        requireVariableIndex(instruction.dst, variableCount, "destination");
        if (!allowFighterContextReads || instruction.intValue < 0 || instruction.intValue >= 32) {
            throw std::runtime_error("fighter package script fighter throw flag read is invalid");
        }
        break;
    case PackageScriptOp::SetFighterThrowFlagImmediate:
        if (!allowFighterContextReads || allowObjectContextReads || instruction.dst < 0 || instruction.dst >= 32) {
            throw std::runtime_error("fighter package script fighter throw flag write is invalid");
        }
        break;
    case PackageScriptOp::SetFighterThrowFlagFromVar:
        if (!allowFighterContextReads || allowObjectContextReads || instruction.dst < 0 || instruction.dst >= 32) {
            throw std::runtime_error("fighter package script fighter throw flag write is invalid");
        }
        requireVariableIndex(instruction.srcA, variableCount, "source");
        break;
    case PackageScriptOp::SetVarFighterHeldObject:
    case PackageScriptOp::SetVarFighterGrabbedFighter:
    case PackageScriptOp::SetVarFighterGrabberFighter:
    case PackageScriptOp::SetVarFighterHitlag:
    case PackageScriptOp::SetVarFighterHitstun:
    case PackageScriptOp::SetVarFighterDamageHitboxOwner:
    case PackageScriptOp::SetVarFighterThrownHitboxOwner:
        requireVariableIndex(instruction.dst, variableCount, "destination");
        if (!allowFighterContextReads) {
            throw std::runtime_error("fighter package script fighter interaction read is invalid");
        }
        break;
    case PackageScriptOp::SetVarObjectOwner:
    case PackageScriptOp::SetVarObjectHeldBy:
    case PackageScriptOp::SetVarObjectGrabVictim:
    case PackageScriptOp::SetVarObjectLastFighter:
    case PackageScriptOp::SetVarObjectLastObject:
    case PackageScriptOp::SetVarObjectDamage:
    case PackageScriptOp::SetVarObjectHitlag:
    case PackageScriptOp::SetVarObjectGroundSegment:
    case PackageScriptOp::SetVarObjectPositionX:
    case PackageScriptOp::SetVarObjectPositionY:
    case PackageScriptOp::SetVarObjectVelocityX:
    case PackageScriptOp::SetVarObjectVelocityY:
    case PackageScriptOp::SetVarObjectAnimationFrame:
    case PackageScriptOp::SetVarObjectAnimationRate:
        requireVariableIndex(instruction.dst, variableCount, "destination");
        if (!allowObjectContextReads) {
            throw std::runtime_error("fighter package script object context read is invalid");
        }
        break;
    case PackageScriptOp::SetObjectDamage:
        if (!allowObjectContextReads) {
            throw std::runtime_error("fighter package script object damage write is invalid");
        }
        break;
    case PackageScriptOp::SetObjectDamageFromVar:
        if (!allowObjectContextReads) {
            throw std::runtime_error("fighter package script object damage write is invalid");
        }
        requireVariableIndex(instruction.srcA, variableCount, "source");
        break;
    case PackageScriptOp::SetObjectHitlag:
        if (!allowObjectContextReads || instruction.intValue < 0) {
            throw std::runtime_error("fighter package script object hitlag write is invalid");
        }
        break;
    case PackageScriptOp::SetObjectHitlagFromVar:
        if (!allowObjectContextReads) {
            throw std::runtime_error("fighter package script object hitlag write is invalid");
        }
        requireVariableIndex(instruction.srcA, variableCount, "source");
        break;
    case PackageScriptOp::SetObjectOwner:
        if (!allowObjectContextReads || instruction.intValue < -1) {
            throw std::runtime_error("fighter package script object owner write is invalid");
        }
        break;
    case PackageScriptOp::SetObjectOwnerFromVar:
        if (!allowObjectContextReads) {
            throw std::runtime_error("fighter package script object owner write is invalid");
        }
        requireVariableIndex(instruction.srcA, variableCount, "source");
        break;
    case PackageScriptOp::SetVarOwnedObjectCount:
        requireVariableIndex(instruction.dst, variableCount, "destination");
        if (!hasName(packageObjectNames, instruction.text)) {
            throw std::runtime_error("fighter package script object target is invalid");
        }
        break;
    case PackageScriptOp::SetVarOwnerFighterVar:
        requireVariableIndex(instruction.dst, variableCount, "destination");
        if (instruction.intValue < 0 || !allowObjectContextReads) {
            throw std::runtime_error("fighter package script owner fighter variable read is invalid");
        }
        break;
    case PackageScriptOp::SetOwnerFighterVarImmediate:
        if (instruction.dst < 0 || !allowObjectContextReads) {
            throw std::runtime_error("fighter package script owner fighter variable write is invalid");
        }
        break;
    case PackageScriptOp::SetOwnerFighterVarFromVar:
        if (instruction.dst < 0 || !allowObjectContextReads) {
            throw std::runtime_error("fighter package script owner fighter variable write is invalid");
        }
        requireVariableIndex(instruction.srcA, variableCount, "source");
        break;
    case PackageScriptOp::SetVarButtonDown:
    case PackageScriptOp::SetVarButtonPressed:
        requireVariableIndex(instruction.dst, variableCount, "destination");
        if (!allowInputReads || instruction.intValue == 0 || (instruction.intValue & ~kPackageInputButtonMask) != 0) {
            throw std::runtime_error("fighter package script input read is invalid");
        }
        break;
    case PackageScriptOp::SetVarStickX:
    case PackageScriptOp::SetVarStickY:
    case PackageScriptOp::SetVarCStickX:
    case PackageScriptOp::SetVarCStickY:
    case PackageScriptOp::SetVarShield:
        requireVariableIndex(instruction.dst, variableCount, "destination");
        if (!allowInputReads) {
            throw std::runtime_error("fighter package script input read is invalid");
        }
        break;
    case PackageScriptOp::AddVar:
        requireVariableIndex(instruction.dst, variableCount, "destination");
        requireVariableIndex(instruction.srcA, variableCount, "source");
        requireVariableIndex(instruction.srcB, variableCount, "source");
        break;
    case PackageScriptOp::ScaleVarFixed:
        requireVariableIndex(instruction.dst, variableCount, "destination");
        requireVariableIndex(instruction.srcA, variableCount, "source");
        break;
    case PackageScriptOp::SetVarRandom:
        requireVariableIndex(instruction.dst, variableCount, "destination");
        if (instruction.intValue <= 0) {
            throw std::runtime_error("fighter package script random bound is invalid");
        }
        break;
    case PackageScriptOp::SetGroundVelocity:
    case PackageScriptOp::SetAirVelocityX:
    case PackageScriptOp::SetAirVelocityY:
    case PackageScriptOp::SetAnimationRate:
    case PackageScriptOp::SetAnimationFrame:
    case PackageScriptOp::SetPositionX:
    case PackageScriptOp::SetPositionY:
    case PackageScriptOp::SetFacing:
        break;
    case PackageScriptOp::SetGroundVelocityFromVar:
    case PackageScriptOp::SetAirVelocityXFromVar:
    case PackageScriptOp::SetAirVelocityYFromVar:
    case PackageScriptOp::SetAnimationRateFromVar:
    case PackageScriptOp::SetAnimationFrameFromVar:
    case PackageScriptOp::SetPositionXFromVar:
    case PackageScriptOp::SetPositionYFromVar:
    case PackageScriptOp::SetFacingFromVar:
        requireVariableIndex(instruction.srcA, variableCount, "source");
        break;
    case PackageScriptOp::ChangeState:
        if (!(allowResolvableStateTargets
            ? hasResolvableStateTarget(stateNames, instruction.text)
            : hasName(stateNames, instruction.text)))
        {
            throw std::runtime_error("fighter package script state target is invalid");
        }
        break;
    case PackageScriptOp::SpawnObject:
        if (!hasName(packageObjectNames, instruction.text)) {
            throw std::runtime_error("fighter package script object target is invalid");
        }
        break;
    case PackageScriptOp::SpawnObjectFromVars:
        requireVariableIndex(instruction.srcA, variableCount, "source");
        requireVariableIndex(instruction.srcB, variableCount, "source");
        if (!hasName(packageObjectNames, instruction.text)) {
            throw std::runtime_error("fighter package script object target is invalid");
        }
        break;
    case PackageScriptOp::SpawnProjectile:
        if (!objectHasKind(packageObjects, instruction.text, GameObjectKind::Projectile)) {
            throw std::runtime_error("fighter package script projectile target is invalid");
        }
        break;
    case PackageScriptOp::SpawnProjectileFromVars:
        requireVariableIndex(instruction.srcA, variableCount, "source");
        requireVariableIndex(instruction.srcB, variableCount, "source");
        if (!objectHasKind(packageObjects, instruction.text, GameObjectKind::Projectile)) {
            throw std::runtime_error("fighter package script projectile target is invalid");
        }
        break;
    case PackageScriptOp::DestroyObject:
        if (!allowObjectLifecycleOps) {
            throw std::runtime_error("fighter package script object lifecycle op is invalid");
        }
        break;
    case PackageScriptOp::DestroyOwnedObjects:
        if (!hasName(packageObjectNames, instruction.text)) {
            throw std::runtime_error("fighter package script object target is invalid");
        }
        break;
    case PackageScriptOp::SkipIfVarLessThanImmediate:
    case PackageScriptOp::SkipIfVarEqualImmediate:
        requireVariableIndex(instruction.dst, variableCount, "condition");
        if (instructionIndex + 2 > instructionCount) {
            throw std::runtime_error("fighter package script branch target is invalid");
        }
        break;
    case PackageScriptOp::SkipIfVarLessThanVar:
    case PackageScriptOp::SkipIfVarEqualVar:
        requireVariableIndex(instruction.srcA, variableCount, "source");
        requireVariableIndex(instruction.srcB, variableCount, "source");
        if (instructionIndex + 2 > instructionCount) {
            throw std::runtime_error("fighter package script branch target is invalid");
        }
        break;
    case PackageScriptOp::JumpRelative: {
        const int target = instructionIndex + instruction.intValue;
        if (target < 0 || target > instructionCount) {
            throw std::runtime_error("fighter package script jump target is invalid");
        }
        break;
    }
    case PackageScriptOp::CallScript:
        if (!hasName(scriptNames, instruction.text)) {
            throw std::runtime_error("fighter package script call target is invalid");
        }
        break;
    case PackageScriptOp::SwitchFighterDefinition:
        if (!allowFighterTargets || !hasName(fighterNames, instruction.text)) {
            throw std::runtime_error("fighter package script fighter target is invalid");
        }
        break;
    case PackageScriptOp::SpawnFighter:
        if (!allowFighterTargets || !hasName(fighterNames, instruction.text)) {
            throw std::runtime_error("fighter package script fighter target is invalid");
        }
        break;
    }
}

void validateFunctionCalls(
    const std::vector<FunctionCall>& calls,
    const std::vector<std::string>& availableScripts)
{
    for (const FunctionCall& call : calls) {
        constexpr const char* kScriptPrefix = "script:";
        constexpr size_t kScriptPrefixSize = 7;
        if (call.name.rfind(kScriptPrefix, 0) == 0 &&
            !hasName(availableScripts, call.name.substr(kScriptPrefixSize)))
        {
            throw std::runtime_error("fighter package script callback target is invalid");
        }
    }
}

void validatePackageScripts(
    const std::vector<PackageScript>& scripts,
    int variableCount,
    const std::vector<std::string>& fighterNames,
    const std::vector<std::string>& stateNames,
    const std::vector<std::string>& packageObjectNames,
    const std::vector<GameObjectDefinition>& packageObjects,
    bool allowResolvableStateTargets,
    bool allowFighterTargets,
    bool allowInputReads,
    bool allowFighterContextReads,
    bool allowObjectLifecycleOps,
    bool allowObjectContextReads)
{
    const std::vector<std::string> availableScriptNames = scriptNames(scripts);
    std::vector<std::string> seenNames;
    seenNames.reserve(scripts.size());
    for (const PackageScript& script : scripts) {
        if (script.name.empty() || hasName(seenNames, script.name)) {
            throw std::runtime_error("fighter package script name is invalid");
        }
        if (script.instructionBudget < 0 || script.instructionBudget > 1024) {
            throw std::runtime_error("fighter package script instruction budget is invalid");
        }
        const int instructionCount = static_cast<int>(script.instructions.size());
        for (int instructionIndex = 0; instructionIndex < instructionCount; ++instructionIndex) {
            validatePackageScriptInstruction(
                script.instructions[static_cast<size_t>(instructionIndex)],
                variableCount,
                fighterNames,
                stateNames,
                availableScriptNames,
                packageObjectNames,
                packageObjects,
                allowResolvableStateTargets,
                allowFighterTargets,
                allowInputReads,
                allowFighterContextReads,
                allowObjectLifecycleOps,
                allowObjectContextReads,
                instructionIndex,
                instructionCount);
        }
        seenNames.push_back(script.name);
    }
}

void validateHitboxGeometry(const HitboxDefinition& hitbox) {
    if (hitbox.radius <= 0) {
        throw std::runtime_error("fighter package hitbox radius is invalid");
    }
    if (hitbox.damage < 0 || hitbox.damageShield < 0 || hitbox.knockbackBase < 0 ||
        hitbox.knockbackGrowth < 0 || hitbox.knockbackWeightSet < 0)
    {
        throw std::runtime_error("fighter package hitbox numeric value is invalid");
    }
    if (hitbox.hitFighters && !hitbox.hitGrounded && !hitbox.hitAirborne) {
        throw std::runtime_error("fighter package hitbox target flags are invalid");
    }
}

void validateHurtboxGeometry(const HurtboxDefinition& hurtbox) {
    if (hurtbox.radius <= 0) {
        throw std::runtime_error("fighter package hurtbox radius is invalid");
    }
}

void validateObjectHurtboxGeometry(const GameObjectHurtboxDefinition& hurtbox) {
    if (hurtbox.radius <= 0) {
        throw std::runtime_error("fighter package object hurtbox radius is invalid");
    }
}

void validateObjectTouchboxGeometry(const GameObjectTouchboxDefinition& touchbox) {
    if (touchbox.radius <= 0) {
        throw std::runtime_error("fighter package object touchbox radius is invalid");
    }
}

void validateFighterEcbGeometry(const FighterEcbDefinition& ecb) {
    if (!ecb.enabled) {
        return;
    }
    const Vec2& left = ecb.points[0];
    const Vec2& top = ecb.points[1];
    const Vec2& right = ecb.points[2];
    const Vec2& bottom = ecb.points[3];
    if (static_cast<int64_t>(top.y) - static_cast<int64_t>(bottom.y) <= 0) {
        throw std::runtime_error("fighter package authored ECB height is invalid");
    }
    if (static_cast<int64_t>(right.x) - static_cast<int64_t>(left.x) <= 0) {
        throw std::runtime_error("fighter package authored ECB width is invalid");
    }
    if (left.y <= bottom.y || left.y >= top.y || right.y <= bottom.y || right.y >= top.y) {
        throw std::runtime_error("fighter package authored ECB side point is invalid");
    }
}

void validateObjectProperties(const GameObjectDefinition& object) {
    if (object.lifetimeFrames < 0 || object.terminalVelocity < 0 || object.maxDamage < 0) {
        throw std::runtime_error("fighter package object property is invalid");
    }
}

void validateInterruptRuleTiming(const InterruptRule& rule) {
    const bool validBlendFrames = rule.blendFrames >= 0 ||
        rule.blendFrames == kUseDefaultAnimationBlendFrames ||
        rule.blendFrames == kDisableAnimationBlendFrames;
    if (!validBlendFrames || rule.lagFrames < 0 ||
        rule.enableFrame < 0 || rule.disableFrame < 0)
    {
        throw std::runtime_error("fighter package interrupt timing is invalid");
    }
}

void validateInterruptRulePackageRefs(const InterruptRule& rule, int variableCount) {
    if (rule.condition != InterruptCondition::PackageVarAtLeast) {
        return;
    }
    if (rule.packageVariable < 0 || rule.packageVariable >= variableCount) {
        throw std::runtime_error("fighter package interrupt variable reference is invalid");
    }
}

bool validAnimationBlendFrames(int blendFrames) {
    return blendFrames >= 0 ||
        blendFrames == kUseDefaultAnimationBlendFrames ||
        blendFrames == kDisableAnimationBlendFrames;
}

void validateFighterStateTiming(const FighterState& state) {
    if (state.animationActionIndex < -1 || state.animationLengthFrames <= 0 || state.initialInterruptibleFrame < 0 ||
        state.defaultAnimationBlendFrames < 0 || !validAnimationBlendFrames(state.onAnimationFinishedBlendFrames))
    {
        throw std::runtime_error("fighter package state timing is invalid");
    }
}

bool hasAnimationClipActionIndex(const std::vector<AnimationClip>& clips, int actionIndex) {
    return std::any_of(clips.begin(), clips.end(), [&](const AnimationClip& clip) {
        return clip.actionIndex == actionIndex;
    });
}

bool hasAnimationClipName(const std::vector<AnimationClip>& clips, const std::string& name) {
    return std::any_of(clips.begin(), clips.end(), [&](const AnimationClip& clip) {
        return clip.name == name;
    });
}

void validateAuthoredStateAnimationReference(const FighterDefinition& fighter, const FighterState& state) {
    if (fighter.hasHsdAsset || fighter.authoredClips.empty()) {
        return;
    }
    if (state.animationActionIndex >= 0) {
        if (!hasAnimationClipActionIndex(fighter.authoredClips, state.animationActionIndex)) {
            throw std::runtime_error("fighter package authored state animation action is invalid");
        }
        return;
    }
    if (!state.animation.empty() && !hasAnimationClipName(fighter.authoredClips, state.animation)) {
        throw std::runtime_error("fighter package authored state animation name is invalid");
    }
}

void validateObjectStateTiming(const GameObjectStateDefinition& state) {
    if (state.animationLengthFrames < 0) {
        throw std::runtime_error("fighter package object state timing is invalid");
    }
}

void validateSubactionTiming(const Subaction& subaction) {
    if (subaction.frames < 0 || subaction.loopCount < 0) {
        throw std::runtime_error("fighter package subaction timing is invalid");
    }
}

size_t fighterHurtboxCount(const FighterDefinition& fighter) {
    if (fighter.hasHsdAsset && fighter.hsdAsset) {
        return fighter.hsdAsset->hurtboxes.size();
    }
    return fighter.hurtboxes.size();
}

void validateSubactionReferences(const FighterDefinition& fighter, const Subaction& subaction) {
    if (subaction.type != SubactionType::SetHurtboxState || subaction.hsdBone >= 0 || subaction.hurtboxIndex < 0) {
        return;
    }
    if (subaction.hurtboxIndex >= static_cast<int>(fighterHurtboxCount(fighter))) {
        throw std::runtime_error("fighter package subaction hurtbox reference is invalid");
    }
}

void validateFighterPackageReferences(const FighterPackage& package) {
    if (package.name.empty()) {
        throw std::runtime_error("fighter package name is invalid");
    }
    if (package.version != kPackageVersion) {
        throw std::runtime_error("fighter package version is invalid");
    }
    const std::vector<std::string> packageFighterNames = fighterNames(package);
    requireUniqueNonemptyNames(packageFighterNames, "fighter");
    const std::vector<std::string> packageObjectNames = objectNames(package);
    requireUniqueNonemptyNames(packageObjectNames, "object");
    for (const FighterDefinition& fighter : package.fighters) {
        if (fighter.states.empty()) {
            throw std::runtime_error("fighter package fighter states are missing");
        }
        const std::vector<std::string> states = fighterStateNames(fighter);
        const std::vector<std::string> scripts = scriptNames(fighter.packageScripts);
        requireUniqueNonemptyNames(states, "fighter state");
        requireUniqueNonemptyNames(variableNames(fighter.packageVariables), "fighter variable");
        validateFighterEcbGeometry(fighter.authoredEcb);
        validatePackageScripts(
            fighter.packageScripts,
            static_cast<int>(fighter.packageVariables.size()),
            packageFighterNames,
            states,
            packageObjectNames,
            package.objects,
            true,
            true,
            true,
            true,
            false,
            false);
        for (const HurtboxDefinition& hurtbox : fighter.hurtboxes) {
            validateHurtboxGeometry(hurtbox);
        }
        for (const FighterState& state : fighter.states) {
            validateFighterStateTiming(state);
            validateAuthoredStateAnimationReference(fighter, state);
            if (!state.onAnimationFinishedState.empty() && !hasResolvableStateTarget(states, state.onAnimationFinishedState)) {
                throw std::runtime_error("fighter package animation finished state target is invalid");
            }
            validateFunctionCalls(state.onEnter, scripts);
            validateFunctionCalls(state.onFrame, scripts);
            validateFunctionCalls(state.onLanding, scripts);
            validateFunctionCalls(state.onAirborne, scripts);
            for (const InterruptRule& rule : state.interrupts) {
                if (!hasResolvableStateTarget(states, rule.targetState)) {
                    throw std::runtime_error("fighter package interrupt state target is invalid: " + state.name + " -> " + rule.targetState);
                }
                validateInterruptRuleTiming(rule);
                validateInterruptRulePackageRefs(rule, static_cast<int>(fighter.packageVariables.size()));
            }
            for (const Subaction& subaction : state.action) {
                validateSubactionTiming(subaction);
                validateSubactionReferences(fighter, subaction);
                if (subaction.type == SubactionType::SpawnObject && !hasName(packageObjectNames, subaction.objectName)) {
                    throw std::runtime_error("fighter package subaction object target is invalid");
                }
                if (subaction.type == SubactionType::SpawnProjectile &&
                    !objectHasKind(package.objects, subaction.objectName, GameObjectKind::Projectile))
                {
                    throw std::runtime_error("fighter package subaction projectile target is invalid");
                }
                if (subaction.type == SubactionType::CallScript && !hasName(scripts, subaction.objectName)) {
                    throw std::runtime_error("fighter package subaction script target is invalid");
                }
                if (subaction.type == SubactionType::CreateHitbox ||
                    subaction.type == SubactionType::CreateThrowHitbox)
                {
                    validateHitboxGeometry(subaction.hitbox);
                }
            }
        }
    }

    for (const GameObjectDefinition& object : package.objects) {
        if (object.states.empty()) {
            throw std::runtime_error("fighter package object states are missing");
        }
        validateObjectProperties(object);
        const std::vector<std::string> states = objectStateNames(object);
        const std::vector<std::string> scripts = scriptNames(object.packageScripts);
        requireUniqueNonemptyNames(states, "object state");
        requireUniqueNonemptyNames(variableNames(object.packageVariables), "object variable");
        validatePackageScripts(
            object.packageScripts,
            static_cast<int>(object.packageVariables.size()),
            packageFighterNames,
            states,
            packageObjectNames,
            package.objects,
            false,
            false,
            false,
            true,
            true,
            true);
        if (object.initialState < 0 || object.initialState >= static_cast<int>(object.states.size())) {
            throw std::runtime_error("fighter package object initial state is invalid");
        }
        for (const GameObjectStateDefinition& state : object.states) {
            validateObjectStateTiming(state);
            validateFunctionCalls(state.onEnter, scripts);
            validateFunctionCalls(state.onFrame, scripts);
            validateFunctionCalls(state.onPhysics, scripts);
            validateFunctionCalls(state.onCollision, scripts);
        }
        validateFunctionCalls(object.onSpawned, scripts);
        validateFunctionCalls(object.onDestroyed, scripts);
        validateFunctionCalls(object.onPickedUp, scripts);
        validateFunctionCalls(object.onDropped, scripts);
        validateFunctionCalls(object.onThrown, scripts);
        validateFunctionCalls(object.onDamageDealt, scripts);
        validateFunctionCalls(object.onDamageReceived, scripts);
        validateFunctionCalls(object.onClanked, scripts);
        validateFunctionCalls(object.onReflected, scripts);
        validateFunctionCalls(object.onAbsorbed, scripts);
        validateFunctionCalls(object.onShieldBounced, scripts);
        validateFunctionCalls(object.onHitShield, scripts);
        validateFunctionCalls(object.onEnteredAir, scripts);
        validateFunctionCalls(object.onEnteredHitlag, scripts);
        validateFunctionCalls(object.onExitedHitlag, scripts);
        validateFunctionCalls(object.onAccessory, scripts);
        validateFunctionCalls(object.onTouched, scripts);
        validateFunctionCalls(object.onJumpedOn, scripts);
        validateFunctionCalls(object.onGrabDealt, scripts);
        validateFunctionCalls(object.onGrabbedForVictim, scripts);
        validateFunctionCalls(object.onInteraction, scripts);
        for (const HitboxDefinition& hitbox : object.hitboxes) {
            validateHitboxGeometry(hitbox);
        }
        for (const GameObjectHurtboxDefinition& hurtbox : object.hurtboxes) {
            validateObjectHurtboxGeometry(hurtbox);
        }
        for (const GameObjectTouchboxDefinition& touchbox : object.touchboxes) {
            validateObjectTouchboxGeometry(touchbox);
        }
    }
}

void writeFighterDefinition(PackageWriter& writer, const FighterPackage& package, const FighterDefinition& fighter) {
    validateAuthoredAnimationData(fighter.authoredSkeleton, fighter.authoredClips);
    validateAuthoredMeshData(fighter.authoredMesh, fighter.authoredSkeleton);
    writer.writeString(fighter.name);
    writeFighterProperties(writer, fighter.properties);
    writeShieldDefinition(writer, fighter.shield);
    writeFighterEcbDefinition(writer, fighter.authoredEcb);
    writeVector(writer, fighter.authoredSkeleton, [&](const AnimationJoint& joint) {
        writeAnimationJoint(writer, joint);
    });
    writeVector(writer, fighter.authoredClips, [&](const AnimationClip& clip) {
        writeAnimationClip(writer, clip);
    });
    writeFighterMesh(writer, fighter.authoredMesh);
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
    fighter.authoredEcb = readFighterEcbDefinition(reader);
    fighter.authoredSkeleton = readVector<AnimationJoint>(reader, kMaxAnimationJoints, "authored skeleton joint", [&]() {
        return readAnimationJoint(reader);
    });
    fighter.authoredClips = readVector<AnimationClip>(reader, kMaxAnimationClips, "authored animation clip", [&]() {
        return readAnimationClip(reader);
    });
    validateAuthoredAnimationData(fighter.authoredSkeleton, fighter.authoredClips);
    fighter.authoredMesh = readFighterMesh(reader);
    validateAuthoredMeshData(fighter.authoredMesh, fighter.authoredSkeleton);
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
        validateFighterPackageReferences(package);
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
        validateFighterPackageReferences(loaded);
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
