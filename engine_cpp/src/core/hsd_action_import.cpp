#include "core/hsd_action_import.hpp"

#include <algorithm>
#include <cstdint>

namespace pf {

namespace {

class HsdCommandBitReader {
public:
    explicit HsdCommandBitReader(const std::vector<uint8_t>& source) : bytes(source) {}

    uint32_t read(int bits) {
        uint32_t value = 0;
        for (int i = 0; i < bits; ++i) {
            if (bit >= static_cast<int>(bytes.size() * 8)) {
                const int remaining = bits - i;
                if (remaining < 32) {
                    value <<= remaining;
                } else {
                    value = 0;
                }
                bit += bits - i;
                break;
            }
            value <<= 1;
            value |= (bytes[static_cast<size_t>(bit / 8)] >> (7 - (bit % 8))) & 1u;
            ++bit;
        }
        return value;
    }

    int32_t readSigned(int bits) {
        const uint32_t raw = read(bits);
        const uint32_t sign = uint32_t{1} << (bits - 1);
        if ((raw & sign) == 0) {
            return static_cast<int32_t>(raw);
        }
        const uint32_t mask = bits >= 32 ? 0xFFFFFFFFu : ((uint32_t{1} << bits) - 1u);
        return -static_cast<int32_t>(((~raw) & mask) + 1u);
    }

    void skip(int bits) {
        bit += bits;
    }

private:
    const std::vector<uint8_t>& bytes;
    int bit = 0;
};

static Fix rawFixed256(int value) {
    return fxDiv(fx(value), fx(256));
}

static int mapCommonFighterPart(const HsdActionScript& script, int part) {
    if (part >= 0 && part < static_cast<int>(script.commonBoneLookup.size())) {
        const int mapped = script.commonBoneLookup[static_cast<size_t>(part)];
        if (mapped >= 0) {
            return mapped;
        }
    }
    return part;
}

static HurtboxState hurtboxStateFromMelee(int state) {
    if (state == 1) {
        return HurtboxState::Invincible;
    }
    if (state == 2) {
        return HurtboxState::Intangible;
    }
    return HurtboxState::Normal;
}

static bool buggedOnlyHitGrabbedFromNextCommand(const HsdActionCommand* nextCommand) {
    if (!nextCommand || nextCommand->bytes.size() < 2) {
        return false;
    }
    HsdCommandBitReader reader(nextCommand->bytes);
    reader.skip(12);
    return reader.read(1) != 0;
}

static Subaction decodeCreateHitbox(
    const HsdActionScript& script,
    const HsdActionCommand& command,
    const HsdActionCommand* nextCommand)
{
    HsdCommandBitReader reader(command.bytes);
    reader.skip(6);

    Subaction sub;
    sub.type = SubactionType::CreateHitbox;
    sub.hitbox.hitboxId = static_cast<int>(reader.read(3));
    reader.read(3); // hit group
    reader.read(1); // HSDLib labels this "Only Hit Grabbed Fighter", but ftAction_8007121C never reads it.
    const int bone = static_cast<int>(reader.read(8));
    const bool useCommonBoneIds = reader.read(1) != 0;
    sub.hitbox.damage = fx(static_cast<int>(reader.read(10)));
    sub.hitbox.radius = rawFixed256(static_cast<int>(reader.read(16)));
    const int zOffset = reader.readSigned(16);
    const int yOffset = reader.readSigned(16);
    const int xOffset = reader.readSigned(16);
    sub.hitbox.offset = {rawFixed256(zOffset), rawFixed256(yOffset), rawFixed256(xOffset)};
    sub.hitbox.knockbackAngleDegrees = fx(static_cast<int>(reader.read(9)));
    sub.hitbox.knockbackGrowth = fx(static_cast<int>(reader.read(9)));
    sub.hitbox.knockbackWeightSet = fx(static_cast<int>(reader.read(9)));
    reader.read(1); // item hit interaction
    sub.hitbox.requiresThrownHitboxOwner = reader.read(1) != 0;
    reader.read(1); // ignore fighter scale
    sub.hitbox.canClank = reader.read(1) != 0;
    sub.hitbox.reboundsOnClank = reader.read(1) != 0;
    sub.hitbox.knockbackBase = fx(static_cast<int>(reader.read(9)));
    sub.hitbox.element = static_cast<int>(reader.read(5));
    sub.hitbox.isGrab = sub.hitbox.element == 8;
    sub.hitbox.damageShield = fx(reader.readSigned(8));
    reader.read(3); // hit sfx severity
    reader.read(5); // hit sfx kind
    sub.hitbox.hitGrounded = reader.read(1) != 0;
    sub.hitbox.hitAirborne = reader.read(1) != 0;
    sub.hitbox.joint = useCommonBoneIds ? mapCommonFighterPart(script, bone) : bone;
    // Melee reads create_hitbox_5.x1_b4 after advancing past the five-word
    // hitbox command, so this flag comes from the following command word.
    sub.hitbox.onlyHitGrabbed = buggedOnlyHitGrabbedFromNextCommand(nextCommand);
    return sub;
}

static Subaction decodeThrowHitbox(const HsdActionCommand& command) {
    HsdCommandBitReader reader(command.bytes);
    reader.skip(6);

    Subaction sub;
    sub.type = SubactionType::CreateThrowHitbox;
    sub.hitbox.hitboxId = static_cast<int>(reader.read(3));
    sub.hitbox.damage = fx(static_cast<int>(reader.read(23)));
    sub.hitbox.knockbackAngleDegrees = fx(static_cast<int>(reader.read(9)));
    sub.hitbox.knockbackGrowth = fx(static_cast<int>(reader.read(9)));
    sub.hitbox.knockbackWeightSet = fx(static_cast<int>(reader.read(9)));
    reader.skip(5);
    sub.hitbox.knockbackBase = fx(static_cast<int>(reader.read(9)));
    sub.hitbox.element = static_cast<int>(reader.read(4));
    reader.read(3); // hit sfx severity
    reader.read(4); // hit sfx kind
    sub.hitbox.hitGrounded = true;
    sub.hitbox.hitAirborne = true;
    return sub;
}

} // namespace

std::vector<Subaction> decodeHsdActionScript(const HsdFighterAnimationAsset&, const HsdActionScript& script) {
    std::vector<Subaction> result;
    result.reserve(script.commands.size());
    for (size_t commandIndex = 0; commandIndex < script.commands.size(); ++commandIndex) {
        const HsdActionCommand& command = script.commands[commandIndex];
        const HsdActionCommand* nextCommand = commandIndex + 1 < script.commands.size() ? &script.commands[commandIndex + 1] : nullptr;
        HsdCommandBitReader reader(command.bytes);
        reader.skip(6);
        Subaction sub;
        switch (command.code) {
        case 0x00:
            return result;
        case 0x01:
            sub.type = SubactionType::SyncTimer;
            sub.frames = static_cast<int>(reader.read(26));
            result.push_back(sub);
            break;
        case 0x02:
            sub.type = SubactionType::AsyncTimer;
            sub.frames = static_cast<int>(reader.read(26));
            result.push_back(sub);
            break;
        case 0x03:
            sub.type = SubactionType::SetLoop;
            sub.loopCount = static_cast<int>(reader.read(26));
            result.push_back(sub);
            break;
        case 0x04:
            sub.type = SubactionType::ExecuteLoop;
            result.push_back(sub);
            break;
        case 0x0B:
            if (command.bytes.size() >= 20) {
                result.push_back(decodeCreateHitbox(script, command, nextCommand));
            }
            break;
        case 0x0C:
            sub.type = SubactionType::AdjustHitboxDamage;
            sub.hitbox.hitboxId = static_cast<int>(reader.read(3));
            sub.hitbox.damage = fx(static_cast<int>(reader.read(23)));
            result.push_back(sub);
            break;
        case 0x0D:
            sub.type = SubactionType::AdjustHitboxSize;
            sub.hitbox.hitboxId = static_cast<int>(reader.read(3));
            sub.hitbox.radius = rawFixed256(static_cast<int>(reader.read(23)));
            result.push_back(sub);
            break;
        case 0x0E:
            sub.type = SubactionType::SetHitboxInteraction;
            sub.hitbox.hitboxId = static_cast<int>(reader.read(24));
            sub.flag = static_cast<int>(reader.read(1));
            sub.flagValue = reader.read(1);
            result.push_back(sub);
            break;
        case 0x0F:
            sub.type = SubactionType::RemoveHitbox;
            sub.hitbox.hitboxId = static_cast<int>(reader.read(26));
            result.push_back(sub);
            break;
        case 0x10:
            sub.type = SubactionType::ClearHitboxes;
            result.push_back(sub);
            break;
        case 0x13:
            sub.type = SubactionType::SetFlag;
            sub.flag = static_cast<int>(reader.read(2));
            sub.flagValue = reader.read(24);
            result.push_back(sub);
            break;
        case 0x14:
            sub.type = SubactionType::SetThrowFlag;
            sub.flag = static_cast<int>(reader.read(26));
            sub.flagValue = true;
            result.push_back(sub);
            break;
        case 0x15:
            reader.read(26);
            sub.type = SubactionType::SetThrowFlagLiteral;
            sub.flag = 1;
            sub.flagValue = true;
            result.push_back(sub);
            break;
        case 0x16:
            reader.read(26);
            sub.type = SubactionType::SetThrowFlagLiteral;
            sub.flag = 2;
            sub.flagValue = true;
            result.push_back(sub);
            break;
        case 0x1E:
            sub.type = SubactionType::SetJabRapid;
            sub.flagValue = reader.read(26) != 0;
            result.push_back(sub);
            break;
        case 0x17:
            sub.type = SubactionType::SetInterruptible;
            sub.interruptibleFrame = -1;
            result.push_back(sub);
            break;
        case 0x18:
            reader.read(26);
            sub.type = SubactionType::SetThrowFlagLiteral;
            sub.flag = 0;
            sub.flagValue = true;
            result.push_back(sub);
            break;
        case 0x19:
            sub.type = SubactionType::SetJumpState;
            sub.flagValue = reader.read(26);
            result.push_back(sub);
            break;
        case 0x1A:
            sub.type = SubactionType::SetBodyCollisionState;
            sub.hurtboxState = hurtboxStateFromMelee(static_cast<int>(reader.read(26)));
            result.push_back(sub);
            break;
        case 0x1B:
            sub.type = SubactionType::SetHurtboxState;
            sub.hurtboxIndex = -1;
            sub.hurtboxState = hurtboxStateFromMelee(static_cast<int>(reader.read(26)));
            result.push_back(sub);
            break;
        case 0x1D:
            sub.type = SubactionType::EnableJabFollowup;
            sub.flagValue = reader.read(26);
            result.push_back(sub);
            break;
        case 0x1F:
            sub.type = SubactionType::SetModelVisibility;
            sub.modelPartIndex = static_cast<int>(reader.read(7));
            reader.read(1); // HSDLib's Change Model State splits this from the signed object id.
            sub.modelPartState = static_cast<int>(reader.readSigned(18));
            result.push_back(sub);
            break;
        case 0x20:
            reader.read(26);
            sub.type = SubactionType::RevertModelVisibility;
            result.push_back(sub);
            break;
        case 0x21:
            reader.read(26);
            sub.type = SubactionType::RemoveModelVisibility;
            result.push_back(sub);
            break;
        case 0x22:
            if (command.bytes.size() >= 12) {
                result.push_back(decodeThrowHitbox(command));
            }
            break;
        case 0x25:
            reader.read(25);
            sub.type = SubactionType::SetFighterVisibility;
            sub.flagValue = reader.read(1);
            result.push_back(sub);
            break;
        case 0x29:
            sub.type = SubactionType::SetModelPartAnimation;
            sub.modelPartIndex = static_cast<int>(reader.read(7));
            sub.modelPartAnimation = static_cast<int>(reader.read(7));
            reader.skip(12);
            result.push_back(sub);
            break;
        case 0x33:
            sub.type = SubactionType::SelfDamage;
            sub.selfDamage = fx(reader.readSigned(26));
            result.push_back(sub);
            break;
        case 0x1C:
            sub.type = SubactionType::SetHurtboxState;
            sub.joint = static_cast<int>(reader.read(8));
            sub.hurtboxIndex = -1;
            sub.hurtboxState = hurtboxStateFromMelee(static_cast<int>(reader.read(18)));
            result.push_back(sub);
            break;
        case 0x38:
            if (command.bytes.size() >= 8) {
                const int holdFramesRaw = static_cast<int>(reader.read(10));
                const int damageMultiplierRaw = static_cast<int>(reader.read(16));
                sub.type = SubactionType::StartSmashCharge;
                sub.smashChargeDamageMultiplier = rawFixed256(damageMultiplierRaw);
                sub.smashChargeHoldFrames = fx(holdFramesRaw);
                result.push_back(sub);
            }
            break;
        default:
            break;
        }
    }
    return result;
}

} // namespace pf
