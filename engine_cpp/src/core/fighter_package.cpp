#include "core/fighter_package.hpp"

#include <algorithm>
#include <cstring>
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
    writer.writeU8(static_cast<uint8_t>(instruction.op));
    writer.writeI32(instruction.dst);
    writer.writeI32(instruction.srcA);
    writer.writeI32(instruction.srcB);
    writer.writeI32(instruction.intValue);
    writer.writeI32(instruction.fixValue);
    writer.writeString(instruction.text);
}

PackageScriptInstruction readPackageScriptInstruction(PackageReader& reader) {
    PackageScriptInstruction instruction;
    instruction.op = static_cast<PackageScriptOp>(reader.readU8());
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
    writeNativeStruct(writer, hitbox);
}

HitboxDefinition readHitbox(PackageReader& reader) {
    return readNativeStruct<HitboxDefinition>(reader, "hitbox");
}

void writeHurtbox(PackageWriter& writer, const HurtboxDefinition& hurtbox) {
    writeNativeStruct(writer, hurtbox);
}

HurtboxDefinition readHurtbox(PackageReader& reader) {
    return readNativeStruct<HurtboxDefinition>(reader, "hurtbox");
}

void writeInterrupt(PackageWriter& writer, const InterruptRule& rule) {
    writer.writeString(rule.targetState);
    writer.writeU8(static_cast<uint8_t>(rule.condition));
    writer.writeU8(static_cast<uint8_t>(rule.ground));
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
    rule.condition = static_cast<InterruptCondition>(reader.readU8());
    rule.ground = static_cast<GroundRequirement>(reader.readU8());
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
    writer.writeU8(static_cast<uint8_t>(subaction.type));
    writer.writeI32(subaction.frames);
    writer.writeI32(subaction.loopCount);
    writer.writeI32(subaction.interruptibleFrame);
    writer.writeI32(subaction.flag);
    writer.writeI32(subaction.hurtboxIndex);
    writer.writeI32(subaction.hsdBone);
    writer.writeU8(static_cast<uint8_t>(subaction.hurtboxState));
    writer.writeU32(subaction.flagValue);
    writer.writeI32(subaction.smashChargeHoldFrames);
    writer.writeI32(subaction.smashChargeDamageMultiplier);
    writer.writeI32(subaction.selfDamage);
    writer.writeString(subaction.objectName);
    writeNativeStruct(writer, subaction.spawnVelocity);
    writeNativeStruct(writer, subaction.spawnOffset);
    writer.writeI32(subaction.modelPartIndex);
    writer.writeI32(subaction.modelPartState);
    writer.writeI32(subaction.modelPartAnimation);
    writeHitbox(writer, subaction.hitbox);
}

Subaction readSubaction(PackageReader& reader) {
    Subaction subaction;
    subaction.type = static_cast<SubactionType>(reader.readU8());
    subaction.frames = reader.readI32();
    subaction.loopCount = reader.readI32();
    subaction.interruptibleFrame = reader.readI32();
    subaction.flag = reader.readI32();
    subaction.hurtboxIndex = reader.readI32();
    subaction.hsdBone = reader.readI32();
    subaction.hurtboxState = static_cast<HurtboxState>(reader.readU8());
    subaction.flagValue = reader.readU32();
    subaction.smashChargeHoldFrames = reader.readI32();
    subaction.smashChargeDamageMultiplier = reader.readI32();
    subaction.selfDamage = reader.readI32();
    subaction.objectName = reader.readString();
    subaction.spawnVelocity = readNativeStruct<Vec2>(reader, "spawn velocity");
    subaction.spawnOffset = readNativeStruct<Vec3>(reader, "spawn offset");
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

void writeFighterDefinition(PackageWriter& writer, const FighterPackage& package, const FighterDefinition& fighter) {
    writer.writeString(fighter.name);
    writeNativeStruct(writer, fighter.properties);
    writeNativeStruct(writer, fighter.shield);
    writer.writeBool(fighter.hasHsdAsset);
    writer.writeI32(assetIndexFor(package, fighter.hsdAsset));
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
    fighter.properties = readNativeStruct<FighterProperties>(reader, "fighter properties");
    fighter.shield = readNativeStruct<ShieldDefinition>(reader, "shield");
    fighter.hasHsdAsset = reader.readBool();
    const int32_t assetIndex = reader.readI32();
    const std::string assetName = reader.readString();
    (void) assetName;
    if (fighter.hasHsdAsset && assetIndex >= 0 && assetIndex < static_cast<int32_t>(hsdAssetPool.size())) {
        fighter.hsdAsset = hsdAssetPool[static_cast<size_t>(assetIndex)];
    } else {
        fighter.hasHsdAsset = false;
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

void writeGameObjectDefinition(PackageWriter& writer, const GameObjectDefinition& object) {
    writer.writeString(object.name);
    writer.writeU8(static_cast<uint8_t>(object.kind));
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
        writeNativeStruct(writer, hurtbox);
    });
    writeVector(writer, object.touchboxes, [&](const GameObjectTouchboxDefinition& touchbox) {
        writeNativeStruct(writer, touchbox);
    });
}

GameObjectDefinition readGameObjectDefinition(PackageReader& reader) {
    GameObjectDefinition object;
    object.name = reader.readString();
    object.kind = static_cast<GameObjectKind>(reader.readU8());
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
        return readNativeStruct<GameObjectHurtboxDefinition>(reader, "object hurtbox");
    });
    object.touchboxes = readVector<GameObjectTouchboxDefinition>(reader, kMaxTouchboxes, "object touchbox", [&]() {
        return readNativeStruct<GameObjectTouchboxDefinition>(reader, "object touchbox");
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

uint32_t fighterPackageChecksum(const std::vector<uint8_t>& bytes) {
    uint32_t hash = 2166136261u;
    for (const uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

} // namespace pf
