#include "editor/fighter_editor.hpp"

#include "core/action.hpp"

#include <algorithm>
#include <utility>

namespace pf {

namespace {

void setEditorError(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
}

bool packageScriptInstructionTargetsFighter(const PackageScriptInstruction& instruction) {
    return instruction.op == PackageScriptOp::SwitchFighterDefinition ||
        instruction.op == PackageScriptOp::SpawnFighter ||
        instruction.op == PackageScriptOp::SpawnFighterSetVar;
}

const FighterDefinition* fighterDefinitionByName(const World& world, const std::string& name) {
    const auto found = std::find_if(world.fighterDefs.begin(), world.fighterDefs.end(), [&](const FighterDefinition& fighter) {
        return fighter.name == name;
    });
    return found == world.fighterDefs.end() ? nullptr : &*found;
}

bool hasPackagedFighter(const std::vector<FighterDefinition>& fighters, const std::string& name) {
    return std::any_of(fighters.begin(), fighters.end(), [&](const FighterDefinition& fighter) {
        return fighter.name == name;
    });
}

std::vector<FighterDefinition> collectEditorPackageFighters(const World& world, const FighterDefinition& root) {
    std::vector<FighterDefinition> fighters;
    fighters.push_back(root);
    for (size_t scan = 0; scan < fighters.size(); ++scan) {
        const FighterDefinition& fighter = fighters[scan];
        for (const PackageScript& script : fighter.packageScripts) {
            for (const PackageScriptInstruction& instruction : script.instructions) {
                if (!packageScriptInstructionTargetsFighter(instruction) ||
                    instruction.text.empty() ||
                    hasPackagedFighter(fighters, instruction.text))
                {
                    continue;
                }
                if (const FighterDefinition* dependency = fighterDefinitionByName(world, instruction.text)) {
                    fighters.push_back(*dependency);
                }
            }
        }
    }
    return fighters;
}

void collectEditorPackageAssets(const std::vector<FighterDefinition>& fighters, FighterPackage& package) {
    for (const FighterDefinition& fighter : fighters) {
        if (!fighter.hsdAsset) {
            continue;
        }
        if (std::find(package.hsdAssets.begin(), package.hsdAssets.end(), fighter.hsdAsset) == package.hsdAssets.end()) {
            package.hsdAssets.push_back(fighter.hsdAsset);
        }
    }
}

bool fighterStateAliasLostByRemovingState(const FighterDefinition& def, const std::string& target, const std::string& removedStateName) {
    return target == "AppealS" &&
        (removedStateName == "AppealSR" || removedStateName == "AppealSL") &&
        def.stateIndex("AppealSR") < 0 &&
        def.stateIndex("AppealSL") < 0;
}

bool shouldRemapRemovedFighterStateTarget(const FighterDefinition& def, const std::string& target, const std::string& removedStateName) {
    return target == removedStateName || fighterStateAliasLostByRemovingState(def, target, removedStateName);
}

FighterState makeDefaultEditorState(const FighterDefinition& def, const std::string& name, int sourceStateIndex) {
    FighterState state;
    state.name = name;
    state.animation = "Wait";
    state.animationLengthFrames = 60;
    state.loopAnimation = true;
    if (sourceStateIndex >= 0 && sourceStateIndex < static_cast<int>(def.states.size())) {
        const FighterState& source = def.states[static_cast<size_t>(sourceStateIndex)];
        state.animation = source.animation;
        state.animationActionIndex = source.animationActionIndex;
        state.animationLengthFrames = source.animationLengthFrames;
        state.loopAnimation = source.loopAnimation;
        state.defaultAnimationBlendFrames = source.defaultAnimationBlendFrames;
    }
    return state;
}

bool validRootFighter(const FighterEditorSession& session, std::string* error) {
    if (session.package.fighters.empty()) {
        setEditorError(error, "editor session has no fighter");
        return false;
    }
    return true;
}

bool validSessionState(
    const FighterEditorSession& session,
    int stateIndex,
    const FighterDefinition** fighter,
    const FighterState** state,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    const FighterDefinition& root = session.package.fighters.front();
    if (stateIndex < 0 || stateIndex >= static_cast<int>(root.states.size())) {
        setEditorError(error, "editor state index is invalid");
        return false;
    }
    if (fighter) {
        *fighter = &root;
    }
    if (state) {
        *state = &root.states[static_cast<size_t>(stateIndex)];
    }
    return true;
}

bool validSessionState(
    FighterEditorSession& session,
    int stateIndex,
    FighterDefinition** fighter,
    FighterState** state,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& root = session.package.fighters.front();
    if (stateIndex < 0 || stateIndex >= static_cast<int>(root.states.size())) {
        setEditorError(error, "editor state index is invalid");
        return false;
    }
    if (fighter) {
        *fighter = &root;
    }
    if (state) {
        *state = &root.states[static_cast<size_t>(stateIndex)];
    }
    return true;
}

bool validateEditorSessionAfterMutation(
    FighterEditorSession& session,
    FighterPackage&& previous,
    std::string* error)
{
    std::string validationError;
    if (!validateFighterPackage(session.package, &validationError)) {
        session.package = std::move(previous);
        session.clamp();
        setEditorError(error, validationError);
        session.lastMessage = validationError;
        return false;
    }
    session.dirty = true;
    session.lastMessage = "OK";
    session.clamp();
    return true;
}

std::vector<FunctionCall>* stateCallbacks(FighterState& state, FighterEditorStateCallbackSlot slot) {
    switch (slot) {
    case FighterEditorStateCallbackSlot::Enter:
        return &state.onEnter;
    case FighterEditorStateCallbackSlot::Frame:
        return &state.onFrame;
    case FighterEditorStateCallbackSlot::Landing:
        return &state.onLanding;
    case FighterEditorStateCallbackSlot::Airborne:
        return &state.onAirborne;
    }
    return nullptr;
}

std::vector<int> editorSubactionFirstFrames(const std::vector<Subaction>& action) {
    std::vector<int> frames(action.size(), -1);
    int currentFrame = 0;
    int loopCount = 0;
    size_t loopStart = 0;
    size_t index = 0;
    int safety = 0;
    while (index < action.size() && safety < 10000) {
        ++safety;
        const Subaction& subaction = action[index];
        if (subaction.type == SubactionType::SetLoop) {
            loopStart = index + 1;
            loopCount = std::max(0, subaction.loopCount - 1);
            ++index;
            continue;
        }
        if (subaction.type == SubactionType::ExecuteLoop) {
            if (loopCount > 0) {
                index = loopStart;
                --loopCount;
            } else {
                ++index;
            }
            continue;
        }
        if (subaction.type == SubactionType::SyncTimer) {
            currentFrame += subaction.frames;
            ++index;
            continue;
        }
        if (subaction.type == SubactionType::AsyncTimer) {
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

void appendTimelineMarker(
    FighterEditorStateTimeline& timeline,
    FighterEditorTimelineMarkerKind kind,
    int frame,
    int sourceIndex,
    SubactionType subactionType = SubactionType::SyncTimer,
    InterruptCondition interruptCondition = InterruptCondition::JumpPressed)
{
    FighterEditorTimelineMarker marker;
    marker.kind = kind;
    marker.frame = std::max(0, frame);
    marker.sourceIndex = sourceIndex;
    marker.subactionType = subactionType;
    marker.interruptCondition = interruptCondition;
    timeline.markers.push_back(marker);
}

bool packageInstructionCallsScriptName(const PackageScriptInstruction& instruction, const std::string& scriptName) {
    return instruction.text == scriptName &&
        (instruction.op == PackageScriptOp::CallScript ||
         instruction.op == PackageScriptOp::CallIndexedFighterScriptFromVar ||
         instruction.op == PackageScriptOp::CallOwnerFighterScript ||
         instruction.op == PackageScriptOp::CallIndexedObjectScriptFromVar);
}

void removePackageScriptCallbackRefs(std::vector<FunctionCall>& calls, const std::string& scriptName) {
    const std::string callback = "script:" + scriptName;
    calls.erase(
        std::remove_if(calls.begin(), calls.end(), [&](const FunctionCall& call) {
            return call.name == callback;
        }),
        calls.end());
}

void remapPackageScriptCallbackRefs(
    std::vector<FunctionCall>& calls,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    const std::string oldCallback = "script:" + oldScriptName;
    const std::string newCallback = "script:" + newScriptName;
    for (FunctionCall& call : calls) {
        if (call.name == oldCallback) {
            call.name = newCallback;
        }
    }
}

void removePackageScriptInstructionRefs(std::vector<PackageScript>& scripts, const std::string& scriptName) {
    for (PackageScript& script : scripts) {
        for (PackageScriptInstruction& instruction : script.instructions) {
            if (packageInstructionCallsScriptName(instruction, scriptName)) {
                instruction.op = PackageScriptOp::Nop;
                instruction.text.clear();
            }
        }
    }
}

void remapPackageScriptInstructionRefs(
    std::vector<PackageScript>& scripts,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    for (PackageScript& script : scripts) {
        for (PackageScriptInstruction& instruction : script.instructions) {
            if (packageInstructionCallsScriptName(instruction, oldScriptName)) {
                instruction.text = newScriptName;
            }
        }
    }
}

void removePackageScriptSubactionRefs(std::vector<FighterState>& states, const std::string& scriptName) {
    for (FighterState& state : states) {
        for (Subaction& subaction : state.action) {
            if (subaction.type == SubactionType::CallScript && subaction.objectName == scriptName) {
                subaction.type = SubactionType::SyncTimer;
                subaction.objectName.clear();
                subaction.frames = std::max(1, subaction.frames);
            }
        }
    }
}

void remapPackageScriptSubactionRefs(
    std::vector<FighterState>& states,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    for (FighterState& state : states) {
        for (Subaction& subaction : state.action) {
            if (subaction.type == SubactionType::CallScript && subaction.objectName == oldScriptName) {
                subaction.objectName = newScriptName;
            }
        }
    }
}

void removeFighterPackageScriptRefs(FighterDefinition& def, const std::string& scriptName) {
    removePackageScriptInstructionRefs(def.packageScripts, scriptName);
    removePackageScriptSubactionRefs(def.states, scriptName);
    for (FighterState& state : def.states) {
        removePackageScriptCallbackRefs(state.onEnter, scriptName);
        removePackageScriptCallbackRefs(state.onFrame, scriptName);
        removePackageScriptCallbackRefs(state.onLanding, scriptName);
        removePackageScriptCallbackRefs(state.onAirborne, scriptName);
    }
}

void remapFighterPackageScriptRefs(
    FighterDefinition& def,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    remapPackageScriptInstructionRefs(def.packageScripts, oldScriptName, newScriptName);
    remapPackageScriptSubactionRefs(def.states, oldScriptName, newScriptName);
    for (FighterState& state : def.states) {
        remapPackageScriptCallbackRefs(state.onEnter, oldScriptName, newScriptName);
        remapPackageScriptCallbackRefs(state.onFrame, oldScriptName, newScriptName);
        remapPackageScriptCallbackRefs(state.onLanding, oldScriptName, newScriptName);
        remapPackageScriptCallbackRefs(state.onAirborne, oldScriptName, newScriptName);
    }
}

void removeCrossFighterPackageScriptRefs(FighterPackage& package, const std::string& scriptName) {
    for (FighterDefinition& fighter : package.fighters) {
        for (PackageScript& script : fighter.packageScripts) {
            for (PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.op == PackageScriptOp::CallIndexedFighterScriptFromVar &&
                    instruction.text == scriptName)
                {
                    instruction.op = PackageScriptOp::Nop;
                    instruction.text.clear();
                }
            }
        }
    }
    for (GameObjectDefinition& object : package.objects) {
        for (PackageScript& script : object.packageScripts) {
            for (PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.op == PackageScriptOp::CallOwnerFighterScript &&
                    instruction.text == scriptName)
                {
                    instruction.op = PackageScriptOp::Nop;
                    instruction.text.clear();
                }
            }
        }
    }
}

void remapCrossFighterPackageScriptRefs(
    FighterPackage& package,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    for (FighterDefinition& fighter : package.fighters) {
        for (PackageScript& script : fighter.packageScripts) {
            for (PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.op == PackageScriptOp::CallIndexedFighterScriptFromVar &&
                    instruction.text == oldScriptName)
                {
                    instruction.text = newScriptName;
                }
            }
        }
    }
    for (GameObjectDefinition& object : package.objects) {
        for (PackageScript& script : object.packageScripts) {
            for (PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.op == PackageScriptOp::CallOwnerFighterScript &&
                    instruction.text == oldScriptName)
                {
                    instruction.text = newScriptName;
                }
            }
        }
    }
}

void remapRemovedPackageVariableRef(int& ref, int removedIndex, int variableCountAfterRemove) {
    if (ref < 0) {
        return;
    }
    if (ref == removedIndex) {
        ref = variableCountAfterRemove > 0 ? std::min(removedIndex, variableCountAfterRemove - 1) : -1;
    } else if (ref > removedIndex) {
        --ref;
    }
}

void sanitizeRemovedPackageVariableInstruction(PackageScriptInstruction& instruction, int variableCountAfterRemove) {
    if (variableCountAfterRemove > 0) {
        return;
    }
    if (instruction.dst < 0 && instruction.srcA < 0 && instruction.srcB < 0) {
        return;
    }
    instruction.op = PackageScriptOp::Nop;
    instruction.dst = -1;
    instruction.srcA = -1;
    instruction.srcB = -1;
    instruction.text.clear();
}

void remapRemovedPackageVariable(std::vector<PackageScript>& scripts, int removedIndex, int variableCountAfterRemove) {
    for (PackageScript& script : scripts) {
        for (PackageScriptInstruction& instruction : script.instructions) {
            remapRemovedPackageVariableRef(instruction.dst, removedIndex, variableCountAfterRemove);
            remapRemovedPackageVariableRef(instruction.srcA, removedIndex, variableCountAfterRemove);
            remapRemovedPackageVariableRef(instruction.srcB, removedIndex, variableCountAfterRemove);
            sanitizeRemovedPackageVariableInstruction(instruction, variableCountAfterRemove);
        }
    }
}

void normalizeInterruptPackageVariable(InterruptRule& rule, int variableCount) {
    if (rule.condition != InterruptCondition::PackageVarAtLeast) {
        return;
    }
    if (variableCount <= 0) {
        rule.condition = InterruptCondition::WaitInput;
        rule.packageVariable = -1;
        return;
    }
    rule.packageVariable = std::clamp(rule.packageVariable < 0 ? 0 : rule.packageVariable, 0, variableCount - 1);
}

void remapRemovedInterruptPackageVariable(std::vector<FighterState>& states, int removedIndex, int variableCountAfterRemove) {
    for (FighterState& state : states) {
        for (InterruptRule& rule : state.interrupts) {
            if (rule.condition != InterruptCondition::PackageVarAtLeast) {
                continue;
            }
            remapRemovedPackageVariableRef(rule.packageVariable, removedIndex, variableCountAfterRemove);
            normalizeInterruptPackageVariable(rule, variableCountAfterRemove);
        }
    }
}

bool validSessionScript(
    FighterEditorSession& session,
    int scriptIndex,
    FighterDefinition** fighter,
    PackageScript** script,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& root = session.package.fighters.front();
    if (scriptIndex < 0 || scriptIndex >= static_cast<int>(root.packageScripts.size())) {
        setEditorError(error, "editor package script index is invalid");
        return false;
    }
    if (fighter) {
        *fighter = &root;
    }
    if (script) {
        *script = &root.packageScripts[static_cast<size_t>(scriptIndex)];
    }
    return true;
}

bool hasFunctionCall(const std::vector<FunctionCall>& calls, const std::string& name) {
    return std::any_of(calls.begin(), calls.end(), [&](const FunctionCall& call) {
        return call.name == name;
    });
}

bool validSessionObject(
    FighterEditorSession& session,
    int objectIndex,
    GameObjectDefinition** object,
    std::string* error)
{
    if (objectIndex < 0 || objectIndex >= static_cast<int>(session.package.objects.size())) {
        setEditorError(error, "editor package object index is invalid");
        return false;
    }
    if (object) {
        *object = &session.package.objects[static_cast<size_t>(objectIndex)];
    }
    return true;
}

bool validSessionObjectState(
    FighterEditorSession& session,
    int objectIndex,
    int stateIndex,
    GameObjectDefinition** object,
    GameObjectStateDefinition** state,
    std::string* error)
{
    GameObjectDefinition* targetObject = nullptr;
    if (!validSessionObject(session, objectIndex, &targetObject, error)) {
        return false;
    }
    if (stateIndex < 0 || stateIndex >= static_cast<int>(targetObject->states.size())) {
        setEditorError(error, "editor package object state index is invalid");
        return false;
    }
    if (object) {
        *object = targetObject;
    }
    if (state) {
        *state = &targetObject->states[static_cast<size_t>(stateIndex)];
    }
    return true;
}

bool packageScriptOpTargetsObjectName(PackageScriptOp op) {
    switch (op) {
    case PackageScriptOp::SpawnObject:
    case PackageScriptOp::SpawnObjectFromVars:
    case PackageScriptOp::SpawnObjectSetVar:
    case PackageScriptOp::SpawnObjectFromVarsSetVar:
    case PackageScriptOp::SpawnProjectile:
    case PackageScriptOp::SpawnProjectileFromVars:
    case PackageScriptOp::SpawnProjectileSetVar:
    case PackageScriptOp::SpawnProjectileFromVarsSetVar:
    case PackageScriptOp::DestroyOwnedObjects:
    case PackageScriptOp::SetVarOwnedObjectCount:
        return true;
    default:
        return false;
    }
}

bool packageScriptOpIsProjectileSpawn(PackageScriptOp op) {
    return op == PackageScriptOp::SpawnProjectile ||
        op == PackageScriptOp::SpawnProjectileFromVars ||
        op == PackageScriptOp::SpawnProjectileSetVar ||
        op == PackageScriptOp::SpawnProjectileFromVarsSetVar;
}

void demoteProjectileInstruction(PackageScriptInstruction& instruction) {
    switch (instruction.op) {
    case PackageScriptOp::SpawnProjectile:
        instruction.op = PackageScriptOp::SpawnObject;
        break;
    case PackageScriptOp::SpawnProjectileFromVars:
        instruction.op = PackageScriptOp::SpawnObjectFromVars;
        break;
    case PackageScriptOp::SpawnProjectileSetVar:
        instruction.op = PackageScriptOp::SpawnObjectSetVar;
        break;
    case PackageScriptOp::SpawnProjectileFromVarsSetVar:
        instruction.op = PackageScriptOp::SpawnObjectFromVarsSetVar;
        break;
    default:
        break;
    }
}

void remapObjectInstructionTarget(
    PackageScriptInstruction& instruction,
    const std::string& oldName,
    const std::string& newName,
    GameObjectKind newKind)
{
    if (!packageScriptOpTargetsObjectName(instruction.op) || instruction.text != oldName) {
        return;
    }
    if (newName.empty()) {
        instruction.op = PackageScriptOp::Nop;
        instruction.text.clear();
        return;
    }
    instruction.text = newName;
    if (newKind != GameObjectKind::Projectile && packageScriptOpIsProjectileSpawn(instruction.op)) {
        demoteProjectileInstruction(instruction);
    }
}

void remapObjectSubactionTarget(
    Subaction& subaction,
    const std::string& oldName,
    const std::string& newName,
    GameObjectKind newKind)
{
    if ((subaction.type != SubactionType::SpawnObject &&
         subaction.type != SubactionType::SpawnProjectile) ||
        subaction.objectName != oldName)
    {
        return;
    }
    if (newName.empty()) {
        subaction.type = SubactionType::SyncTimer;
        subaction.objectName.clear();
        subaction.spawnVelocity = {};
        subaction.spawnOffset = {};
        subaction.frames = std::max(1, subaction.frames);
        return;
    }
    subaction.objectName = newName;
    if (newKind != GameObjectKind::Projectile && subaction.type == SubactionType::SpawnProjectile) {
        subaction.type = SubactionType::SpawnObject;
    }
}

void remapPackageObjectTargets(
    FighterPackage& package,
    const std::string& oldName,
    const std::string& newName,
    GameObjectKind newKind)
{
    for (FighterDefinition& fighter : package.fighters) {
        for (FighterState& state : fighter.states) {
            for (Subaction& subaction : state.action) {
                remapObjectSubactionTarget(subaction, oldName, newName, newKind);
            }
        }
        for (PackageScript& script : fighter.packageScripts) {
            for (PackageScriptInstruction& instruction : script.instructions) {
                remapObjectInstructionTarget(instruction, oldName, newName, newKind);
            }
        }
    }
    for (GameObjectDefinition& object : package.objects) {
        for (PackageScript& script : object.packageScripts) {
            for (PackageScriptInstruction& instruction : script.instructions) {
                remapObjectInstructionTarget(instruction, oldName, newName, newKind);
            }
        }
    }
}

void remapObjectStateScriptTargets(
    GameObjectDefinition& object,
    const std::string& oldStateName,
    const std::string& newStateName)
{
    for (PackageScript& script : object.packageScripts) {
        for (PackageScriptInstruction& instruction : script.instructions) {
            if (instruction.op == PackageScriptOp::ChangeState && instruction.text == oldStateName) {
                instruction.text = newStateName;
            }
        }
    }
}

std::vector<FunctionCall>* objectStateCallbacks(
    GameObjectStateDefinition& state,
    FighterEditorObjectStateCallbackSlot slot)
{
    switch (slot) {
    case FighterEditorObjectStateCallbackSlot::Enter:
        return &state.onEnter;
    case FighterEditorObjectStateCallbackSlot::Frame:
        return &state.onFrame;
    case FighterEditorObjectStateCallbackSlot::Physics:
        return &state.onPhysics;
    case FighterEditorObjectStateCallbackSlot::Collision:
        return &state.onCollision;
    }
    return nullptr;
}

std::vector<FunctionCall>* objectEventCallbacks(
    GameObjectDefinition& object,
    FighterEditorObjectEventCallbackSlot slot)
{
    switch (slot) {
    case FighterEditorObjectEventCallbackSlot::Spawned: return &object.onSpawned;
    case FighterEditorObjectEventCallbackSlot::Destroyed: return &object.onDestroyed;
    case FighterEditorObjectEventCallbackSlot::PickedUp: return &object.onPickedUp;
    case FighterEditorObjectEventCallbackSlot::Dropped: return &object.onDropped;
    case FighterEditorObjectEventCallbackSlot::Thrown: return &object.onThrown;
    case FighterEditorObjectEventCallbackSlot::DamageDealt: return &object.onDamageDealt;
    case FighterEditorObjectEventCallbackSlot::DamageReceived: return &object.onDamageReceived;
    case FighterEditorObjectEventCallbackSlot::Clanked: return &object.onClanked;
    case FighterEditorObjectEventCallbackSlot::Reflected: return &object.onReflected;
    case FighterEditorObjectEventCallbackSlot::Absorbed: return &object.onAbsorbed;
    case FighterEditorObjectEventCallbackSlot::ShieldBounced: return &object.onShieldBounced;
    case FighterEditorObjectEventCallbackSlot::HitShield: return &object.onHitShield;
    case FighterEditorObjectEventCallbackSlot::EnteredAir: return &object.onEnteredAir;
    case FighterEditorObjectEventCallbackSlot::EnteredHitlag: return &object.onEnteredHitlag;
    case FighterEditorObjectEventCallbackSlot::ExitedHitlag: return &object.onExitedHitlag;
    case FighterEditorObjectEventCallbackSlot::Accessory: return &object.onAccessory;
    case FighterEditorObjectEventCallbackSlot::Touched: return &object.onTouched;
    case FighterEditorObjectEventCallbackSlot::JumpedOn: return &object.onJumpedOn;
    case FighterEditorObjectEventCallbackSlot::GrabDealt: return &object.onGrabDealt;
    case FighterEditorObjectEventCallbackSlot::GrabbedForVictim: return &object.onGrabbedForVictim;
    case FighterEditorObjectEventCallbackSlot::Interaction: return &object.onInteraction;
    }
    return nullptr;
}

} // namespace

void FighterEditor::clampToWorld(const World& world) {
    if (world.fighters.empty()) {
        selectedFighter = 0;
        selectedState = 0;
        selectedSubaction = 0;
        return;
    }
    selectedFighter = std::clamp(selectedFighter, 0, static_cast<int>(world.fighters.size()) - 1);
    const FighterRuntime& fighter = world.fighters[static_cast<size_t>(selectedFighter)];
    const FighterDefinition& def = world.fighterDefs[static_cast<size_t>(fighter.fighterDef)];
    selectedState = std::clamp(selectedState, 0, static_cast<int>(def.states.size()) - 1);
    const FighterState& state = def.states[static_cast<size_t>(selectedState)];
    selectedSubaction = std::clamp(selectedSubaction, 0, std::max(0, static_cast<int>(state.action.size()) - 1));
    selectedInterrupt = std::clamp(
        selectedInterrupt,
        0,
        std::max(0, static_cast<int>(state.interrupts.size()) - 1));
    selectedPackageVariable = std::clamp(
        selectedPackageVariable,
        0,
        std::max(0, static_cast<int>(def.packageVariables.size()) - 1));
    selectedPackageScript = std::clamp(
        selectedPackageScript,
        0,
        std::max(0, static_cast<int>(def.packageScripts.size()) - 1));
    if (def.packageScripts.empty()) {
        selectedPackageInstruction = 0;
    } else {
        const PackageScript& script = def.packageScripts[static_cast<size_t>(selectedPackageScript)];
        selectedPackageInstruction = std::clamp(
            selectedPackageInstruction,
            0,
            std::max(0, static_cast<int>(script.instructions.size()) - 1));
    }
    selectedObjectDef = std::clamp(
        selectedObjectDef,
        0,
        std::max(0, static_cast<int>(world.objectDefs.size()) - 1));
    if (world.objectDefs.empty()) {
        selectedObjectState = 0;
    } else {
        const GameObjectDefinition& object = world.objectDefs[static_cast<size_t>(selectedObjectDef)];
        selectedObjectState = std::clamp(
            selectedObjectState,
            0,
            std::max(0, static_cast<int>(object.states.size()) - 1));
        selectedObjectHitbox = std::clamp(
            selectedObjectHitbox,
            0,
            std::max(0, static_cast<int>(object.hitboxes.size()) - 1));
        selectedObjectHurtbox = std::clamp(
            selectedObjectHurtbox,
            0,
            std::max(0, static_cast<int>(object.hurtboxes.size()) - 1));
        selectedObjectTouchbox = std::clamp(
            selectedObjectTouchbox,
            0,
            std::max(0, static_cast<int>(object.touchboxes.size()) - 1));
    }
    const bool useImportedClips = def.hsdAsset && !def.hsdAsset->clips.empty();
    const std::vector<AnimationClip>& clips = useImportedClips ? def.hsdAsset->clips : def.authoredClips;
    const int clipCount = static_cast<int>(clips.size());
    selectedAnimationClip = std::clamp(selectedAnimationClip, 0, std::max(0, clipCount - 1));
    selectedAnimationJoint = std::clamp(
        selectedAnimationJoint,
        0,
        std::max(0, static_cast<int>(def.authoredSkeleton.size()) - 1));
    if (clipCount > 0) {
        const AnimationClip& clip = clips[static_cast<size_t>(selectedAnimationClip)];
        selectedAnimationTrack = std::clamp(
            selectedAnimationTrack,
            0,
            std::max(0, static_cast<int>(clip.tracks.size()) - 1));
        if (!clip.tracks.empty()) {
            const AnimationTrack& track = clip.tracks[static_cast<size_t>(selectedAnimationTrack)];
            selectedAnimationKey = std::clamp(
                selectedAnimationKey,
                0,
                std::max(0, static_cast<int>(track.keys.size()) - 1));
        } else {
            selectedAnimationKey = 0;
        }
    } else {
        selectedAnimationTrack = 0;
        selectedAnimationKey = 0;
    }
    if (clipCount == 0) {
        animationScrubFrame = 0;
    }
    int authoredMeshVertices = 0;
    for (const HsdMeshBatch& batch : def.authoredMesh.batches) {
        authoredMeshVertices += static_cast<int>(batch.vertices.size());
    }
    selectedAuthoredMeshVertex = std::clamp(
        selectedAuthoredMeshVertex,
        0,
        std::max(0, authoredMeshVertices - 1));
    selectedHurtbox = std::clamp(
        selectedHurtbox,
        0,
        std::max(0, static_cast<int>(def.hurtboxes.size()) - 1));
}

void FighterEditorSession::clamp() {
    selectedFighter = std::clamp(
        selectedFighter,
        0,
        std::max(0, static_cast<int>(package.fighters.size()) - 1));
    if (package.fighters.empty()) {
        selectedState = 0;
        selectedSubaction = 0;
        selectedInterrupt = 0;
        selectedPackageScript = 0;
        selectedPackageInstruction = 0;
        return;
    }
    const FighterDefinition& def = package.fighters[static_cast<size_t>(selectedFighter)];
    selectedState = std::clamp(selectedState, 0, std::max(0, static_cast<int>(def.states.size()) - 1));
    if (def.states.empty()) {
        selectedSubaction = 0;
        selectedInterrupt = 0;
        return;
    }
    const FighterState& state = def.states[static_cast<size_t>(selectedState)];
    selectedSubaction = std::clamp(selectedSubaction, 0, std::max(0, static_cast<int>(state.action.size()) - 1));
    selectedInterrupt = std::clamp(selectedInterrupt, 0, std::max(0, static_cast<int>(state.interrupts.size()) - 1));
    selectedPackageScript = std::clamp(selectedPackageScript, 0, std::max(0, static_cast<int>(def.packageScripts.size()) - 1));
    if (def.packageScripts.empty()) {
        selectedPackageInstruction = 0;
    } else {
        const PackageScript& script = def.packageScripts[static_cast<size_t>(selectedPackageScript)];
        selectedPackageInstruction = std::clamp(
            selectedPackageInstruction,
            0,
            std::max(0, static_cast<int>(script.instructions.size()) - 1));
    }
}

FighterDefinition* FighterEditorSession::rootFighter() {
    return package.fighters.empty() ? nullptr : &package.fighters.front();
}

const FighterDefinition* FighterEditorSession::rootFighter() const {
    return package.fighters.empty() ? nullptr : &package.fighters.front();
}

std::string uniqueEditorFighterName(const World& world, const std::string& prefix) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        const bool exists = std::any_of(world.fighterDefs.begin(), world.fighterDefs.end(), [&](const FighterDefinition& fighter) {
            return fighter.name == candidate;
        });
        if (!exists) {
            return candidate;
        }
    }
    return prefix + "X";
}

std::string uniqueEditorStateName(const FighterDefinition& def, const std::string& prefix) {
    if (editorFighterStateNameAvailable(def, prefix)) {
        return prefix;
    }
    for (int index = 1; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        if (editorFighterStateNameAvailable(def, candidate)) {
            return candidate;
        }
    }
    return prefix + "X";
}

bool editorFighterStateNameAvailable(const FighterDefinition& def, const std::string& name, int ignoredIndex) {
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

void remapEditorFighterStateTargets(
    FighterDefinition& def,
    const std::string& oldStateName,
    const std::string& newStateName)
{
    for (FighterState& state : def.states) {
        if (state.onAnimationFinishedState == oldStateName) {
            state.onAnimationFinishedState = newStateName;
        }
        for (InterruptRule& rule : state.interrupts) {
            if (rule.targetState == oldStateName) {
                rule.targetState = newStateName;
            }
        }
    }
    for (PackageScript& script : def.packageScripts) {
        for (PackageScriptInstruction& instruction : script.instructions) {
            if (instruction.op == PackageScriptOp::ChangeState && instruction.text == oldStateName) {
                instruction.text = newStateName;
            }
        }
    }
}

void remapRemovedEditorFighterStateTargets(
    FighterDefinition& def,
    const std::string& removedStateName,
    const std::string& replacementStateName)
{
    for (FighterState& state : def.states) {
        if (shouldRemapRemovedFighterStateTarget(def, state.onAnimationFinishedState, removedStateName)) {
            state.onAnimationFinishedState = replacementStateName;
        }
        for (InterruptRule& rule : state.interrupts) {
            if (shouldRemapRemovedFighterStateTarget(def, rule.targetState, removedStateName)) {
                rule.targetState = replacementStateName;
            }
        }
    }
    for (PackageScript& script : def.packageScripts) {
        for (PackageScriptInstruction& instruction : script.instructions) {
            if (instruction.op == PackageScriptOp::ChangeState &&
                shouldRemapRemovedFighterStateTarget(def, instruction.text, removedStateName))
            {
                instruction.text = replacementStateName;
            }
        }
    }
}

void ensureFighterEditorAuthoredRootJoint(FighterDefinition& def) {
    if (!def.authoredSkeleton.empty()) {
        return;
    }
    def.authoredSkeleton.push_back({-1, "Root", 0, {}, {}, {fx(1), fx(1), fx(1)}});
}

void normalizeFighterEditorAuthoredEcb(FighterDefinition& def) {
    FighterEcbDefinition& ecb = def.authoredEcb;
    ecb.points[0].x = std::min(ecb.points[0].x, -fxFromFloat(0.1f));
    ecb.points[2].x = std::max(ecb.points[2].x, fxFromFloat(0.1f));
    ecb.points[1].y = std::max(ecb.points[1].y, ecb.points[3].y + fxFromFloat(0.5f));

    const Fix minSide = ecb.points[3].y + fxFromFloat(0.05f);
    const Fix maxSide = ecb.points[1].y - fxFromFloat(0.05f);
    const Fix sideY = std::clamp(
        (ecb.points[0].y + ecb.points[2].y) / 2,
        minSide,
        maxSide);
    ecb.points[0].y = sideY;
    ecb.points[2].y = sideY;
}

FighterDefinition makeFighterEditorBlankDefinition(const std::string& name, const MeleeCommonData& common) {
    FighterDefinition def;
    def.name = name;
    def.hasHsdAsset = false;
    def.authoredEcb.enabled = true;
    normalizeFighterEditorAuthoredEcb(def);
    def.properties.common = common;
    def.authoredSkeleton = {
        {-1, "Root", 0, {0, 0, 0}, {0, 0, 0}, {fx(1), fx(1), fx(1)}},
    };
    AnimationClip waitClip;
    waitClip.name = "Wait";
    waitClip.actionIndex = 0;
    waitClip.frameCount = fx(60);
    waitClip.tracks = {
        {0, AnimationChannel::TranslateY, {
            {0, 0, 0, AnimationInterpolation::Linear},
            {fx(60), 0, 0, AnimationInterpolation::Linear},
        }},
    };
    AnimationClip fallClip = waitClip;
    fallClip.name = "Fall";
    fallClip.actionIndex = 1;
    def.authoredClips = {waitClip, fallClip};
    def.hurtboxes = {
        {BoneId::Hip, {0, fxFromFloat(-0.45f), 0}, {0, fxFromFloat(0.55f), 0}, fxFromFloat(0.45f), HurtboxState::Normal, true},
        {BoneId::Head, {0, fxFromFloat(-0.2f), 0}, {0, fxFromFloat(0.2f), 0}, fxFromFloat(0.32f), HurtboxState::Normal, true},
    };

    FighterState wait;
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
        {"Fall", InterruptCondition::BecameAirborne},
    };

    FighterState fall;
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

GameObjectDefinition makeFighterEditorObjectDefinition(const std::string& name, GameObjectKind kind) {
    GameObjectDefinition object;
    object.name = name;
    object.kind = kind;
    object.initialState = 0;
    object.lifetimeFrames = kind == GameObjectKind::Projectile ? 90 : 600;
    object.gravity = kind == GameObjectKind::Projectile ? 0 : -fxFromFloat(0.08f);
    object.terminalVelocity = fxFromFloat(3.0f);
    object.destroyOnHit = kind == GameObjectKind::Projectile;
    object.destroyOnShield = kind == GameObjectKind::Projectile;
    object.states = {{"Idle", 1, true}};
    if (kind == GameObjectKind::Projectile) {
        HitboxDefinition hitbox;
        hitbox.radius = fxFromFloat(0.45f);
        hitbox.damage = fxFromFloat(3.0f);
        hitbox.knockbackAngleDegrees = fx(45);
        hitbox.knockbackBase = fx(20);
        hitbox.knockbackGrowth = fx(60);
        object.hitboxes = {hitbox};
    } else {
        object.hurtboxes = {{{}, {}, fxFromFloat(0.35f), HurtboxState::Normal}};
        object.touchboxes = {{{}, {}, fxFromFloat(0.45f), true, true}};
    }
    return object;
}

FighterPackage makeEditorFighterPackage(const World& world, int rootFighterDef, const std::string& packageName) {
    FighterPackage package;
    if (rootFighterDef < 0 || rootFighterDef >= static_cast<int>(world.fighterDefs.size())) {
        return package;
    }
    const FighterDefinition& root = world.fighterDefs[static_cast<size_t>(rootFighterDef)];
    package.name = packageName.empty() ? root.name + "_editor" : packageName;
    package.fighters = collectEditorPackageFighters(world, root);
    collectEditorPackageAssets(package.fighters, package);
    package.objects = world.objectDefs;
    return package;
}

bool beginFighterEditorSessionFromWorld(
    const World& world,
    int rootFighterDef,
    FighterEditorSession& session,
    std::string* error,
    const std::string& packageName)
{
    FighterPackage package = makeEditorFighterPackage(world, rootFighterDef, packageName);
    if (package.fighters.empty()) {
        setEditorError(error, "editor session root fighter is invalid");
        return false;
    }
    session = {};
    session.package = std::move(package);
    session.selectedFighter = 0;
    session.selectedState = 0;
    session.dirty = false;
    session.lastMessage = "OK";
    session.clamp();
    return true;
}

bool beginBlankFighterEditorSession(
    const std::string& fighterName,
    const MeleeCommonData& common,
    FighterEditorSession& session,
    std::string* error)
{
    if (fighterName.empty()) {
        setEditorError(error, "editor blank fighter name is invalid");
        return false;
    }
    session = {};
    session.package.name = fighterName + "_editor";
    session.package.fighters = {makeFighterEditorBlankDefinition(fighterName, common)};
    session.selectedFighter = 0;
    session.selectedState = 0;
    session.dirty = true;
    session.lastMessage = "OK";
    session.clamp();
    return true;
}

bool loadFighterEditorSessionPackage(
    const std::vector<uint8_t>& bytes,
    FighterEditorSession& session,
    std::string* error,
    const std::vector<std::shared_ptr<const HsdFighterAnimationAsset>>& hsdAssetPool)
{
    FighterPackage package;
    if (!readFighterPackage(bytes, package, error, hsdAssetPool)) {
        return false;
    }
    if (package.fighters.empty()) {
        setEditorError(error, "editor package has no fighters");
        return false;
    }
    session = {};
    session.package = std::move(package);
    session.lastBytes = bytes;
    describeFighterPackage(session.package, session.lastDescriptor, bytes, nullptr);
    session.lastMessage = "OK";
    session.clamp();
    return true;
}

bool exportFighterEditorSessionPackage(
    FighterEditorSession& session,
    FighterEditorPackageSnapshot& snapshot,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    std::vector<uint8_t> bytes = writeFighterPackage(session.package, error);
    if (bytes.empty()) {
        session.lastMessage = error ? *error : "editor package export failed";
        return false;
    }
    FighterPackageDescriptor descriptor;
    if (!describeFighterPackage(session.package, descriptor, bytes, error)) {
        session.lastMessage = error ? *error : "editor package describe failed";
        return false;
    }
    snapshot.package = session.package;
    snapshot.bytes = bytes;
    snapshot.descriptor = descriptor;
    session.lastBytes = bytes;
    session.lastDescriptor = descriptor;
    session.lastMessage = "OK";
    session.dirty = false;
    return true;
}

bool makeFighterEditorSessionTestWorld(
    FighterEditorSession& session,
    World& world,
    int* rootFighterDef,
    FighterPackageDescriptor* descriptor,
    std::string* error)
{
    FighterEditorPackageSnapshot snapshot;
    if (!exportFighterEditorSessionPackage(session, snapshot, error)) {
        return false;
    }
    FighterPackageDescriptor testDescriptor;
    if (!makePackageTestWorldFromBytes(world, snapshot.bytes, rootFighterDef, &testDescriptor, error)) {
        session.lastMessage = error ? *error : "editor package test world failed";
        return false;
    }
    if (descriptor) {
        *descriptor = testDescriptor;
    }
    session.lastDescriptor = testDescriptor;
    session.lastMessage = "OK";
    return true;
}

bool createEditorSessionState(
    FighterEditorSession& session,
    const std::string& requestedName,
    int sourceStateIndex,
    int* createdStateIndex,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    const std::string name = requestedName.empty()
        ? uniqueEditorStateName(def, "NewState")
        : requestedName;
    if (!editorFighterStateNameAvailable(def, name)) {
        setEditorError(error, "editor state name is empty or already used");
        return false;
    }
    const int insertIndex = std::clamp(session.selectedState + 1, 0, static_cast<int>(def.states.size()));
    def.states.insert(def.states.begin() + insertIndex, makeDefaultEditorState(def, name, sourceStateIndex));
    session.selectedState = insertIndex;
    session.selectedSubaction = 0;
    session.selectedInterrupt = 0;
    session.dirty = true;
    session.clamp();
    if (createdStateIndex) {
        *createdStateIndex = insertIndex;
    }
    return true;
}

bool duplicateEditorSessionState(
    FighterEditorSession& session,
    int sourceStateIndex,
    int* createdStateIndex,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    if (def.states.empty()) {
        setEditorError(error, "editor cannot clone a state from an empty fighter");
        return false;
    }
    const int sourceIndex = sourceStateIndex >= 0
        ? sourceStateIndex
        : std::clamp(session.selectedState, 0, static_cast<int>(def.states.size()) - 1);
    if (sourceIndex < 0 || sourceIndex >= static_cast<int>(def.states.size())) {
        setEditorError(error, "editor clone source state is invalid");
        return false;
    }
    FighterState clone = def.states[static_cast<size_t>(sourceIndex)];
    clone.name = uniqueEditorStateName(def, clone.name + "Copy");
    const int insertIndex = std::clamp(sourceIndex + 1, 0, static_cast<int>(def.states.size()));
    def.states.insert(def.states.begin() + insertIndex, std::move(clone));
    session.selectedState = insertIndex;
    session.selectedSubaction = 0;
    session.selectedInterrupt = 0;
    session.dirty = true;
    session.clamp();
    if (createdStateIndex) {
        *createdStateIndex = insertIndex;
    }
    return true;
}

bool renameEditorSessionState(
    FighterEditorSession& session,
    int stateIndex,
    const std::string& newName,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    if (stateIndex < 0 || stateIndex >= static_cast<int>(def.states.size())) {
        setEditorError(error, "editor rename state index is invalid");
        return false;
    }
    if (!editorFighterStateNameAvailable(def, newName, stateIndex)) {
        setEditorError(error, "editor state name is empty or already used");
        return false;
    }
    const std::string oldName = def.states[static_cast<size_t>(stateIndex)].name;
    def.states[static_cast<size_t>(stateIndex)].name = newName;
    remapEditorFighterStateTargets(def, oldName, newName);
    session.selectedState = stateIndex;
    session.dirty = true;
    session.clamp();
    return true;
}

bool removeEditorSessionState(
    FighterEditorSession& session,
    int stateIndex,
    const std::string& replacementStateName,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    if (def.states.size() <= 1) {
        setEditorError(error, "editor cannot remove the only fighter state");
        return false;
    }
    if (stateIndex < 0 || stateIndex >= static_cast<int>(def.states.size())) {
        setEditorError(error, "editor remove state index is invalid");
        return false;
    }
    const std::string removedName = def.states[static_cast<size_t>(stateIndex)].name;
    def.states.erase(def.states.begin() + stateIndex);
    std::string replacement = replacementStateName;
    if (replacement.empty() || def.stateIndex(replacement) < 0) {
        const int waitIndex = def.stateIndex("Wait");
        replacement = waitIndex >= 0 ? "Wait" : def.states.front().name;
    }
    remapRemovedEditorFighterStateTargets(def, removedName, replacement);
    session.selectedState = std::clamp(stateIndex, 0, static_cast<int>(def.states.size()) - 1);
    session.selectedSubaction = 0;
    session.selectedInterrupt = 0;
    session.dirty = true;
    session.clamp();
    return true;
}

bool addEditorSessionObject(
    FighterEditorSession& session,
    const std::string& requestedName,
    GameObjectKind kind,
    int* objectIndex,
    std::string* error)
{
    const std::string name = requestedName.empty() ? uniqueEditorObjectName(session.package) : requestedName;
    if (!editorObjectNameAvailable(session.package, name)) {
        setEditorError(error, "editor object name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    session.package.objects.push_back(makeFighterEditorObjectDefinition(name, kind));
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (objectIndex) {
        *objectIndex = static_cast<int>(session.package.objects.size()) - 1;
    }
    return true;
}

bool buildEditorSessionStateTimeline(
    const FighterEditorSession& session,
    int stateIndex,
    FighterEditorStateTimeline& timeline,
    std::string* error)
{
    const FighterState* state = nullptr;
    if (!validSessionState(session, stateIndex, nullptr, &state, error)) {
        return false;
    }
    timeline = {};
    timeline.animationLengthFrames = state->animationLengthFrames;
    timeline.initialInterruptibleFrame = state->initialInterruptibleFrame;
    const UnfoldedAction actionFrames = unfoldAction(state->action);
    timeline.actionLengthFrames = std::max(0, static_cast<int>(actionFrames.size()) - 1);
    timeline.frameCount = std::max(1, std::max(timeline.animationLengthFrames, timeline.actionLengthFrames));
    timeline.subactionFrames = editorSubactionFirstFrames(state->action);

    for (size_t i = 0; i < state->action.size(); ++i) {
        const Subaction& subaction = state->action[i];
        const int frame = i < timeline.subactionFrames.size() ? timeline.subactionFrames[i] : -1;
        if (frame < 0) {
            continue;
        }
        FighterEditorTimelineMarkerKind kind = FighterEditorTimelineMarkerKind::Subaction;
        if (subaction.type == SubactionType::CreateHitbox) {
            kind = FighterEditorTimelineMarkerKind::Hitbox;
        } else if (subaction.type == SubactionType::CreateThrowHitbox) {
            kind = FighterEditorTimelineMarkerKind::ThrowHitbox;
        } else if (subaction.type == SubactionType::SetInterruptible) {
            kind = FighterEditorTimelineMarkerKind::Interruptible;
        }
        appendTimelineMarker(timeline, kind, frame, static_cast<int>(i), subaction.type);
    }

    if (state->initialInterruptibleFrame > 0) {
        appendTimelineMarker(
            timeline,
            FighterEditorTimelineMarkerKind::Interruptible,
            state->initialInterruptibleFrame,
            -1);
    }
    for (size_t i = 0; i < state->interrupts.size(); ++i) {
        const InterruptRule& interrupt = state->interrupts[i];
        appendTimelineMarker(
            timeline,
            FighterEditorTimelineMarkerKind::InterruptEnable,
            interrupt.enableFrame,
            static_cast<int>(i),
            SubactionType::SyncTimer,
            interrupt.condition);
        if (interrupt.disableFrame > 0) {
            appendTimelineMarker(
                timeline,
                FighterEditorTimelineMarkerKind::InterruptDisable,
                interrupt.disableFrame,
                static_cast<int>(i),
                SubactionType::SyncTimer,
                interrupt.condition);
        }
    }

    std::sort(timeline.markers.begin(), timeline.markers.end(), [](const FighterEditorTimelineMarker& a, const FighterEditorTimelineMarker& b) {
        if (a.frame != b.frame) {
            return a.frame < b.frame;
        }
        if (a.kind != b.kind) {
            return static_cast<int>(a.kind) < static_cast<int>(b.kind);
        }
        return a.sourceIndex < b.sourceIndex;
    });
    return true;
}

bool setEditorSessionStateTiming(
    FighterEditorSession& session,
    int stateIndex,
    int animationLengthFrames,
    int initialInterruptibleFrame,
    int defaultAnimationBlendFrames,
    int onAnimationFinishedBlendFrames,
    std::string* error)
{
    FighterState* state = nullptr;
    if (!validSessionState(session, stateIndex, nullptr, &state, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    state->animationLengthFrames = animationLengthFrames;
    state->initialInterruptibleFrame = initialInterruptibleFrame;
    state->defaultAnimationBlendFrames = defaultAnimationBlendFrames;
    state->onAnimationFinishedBlendFrames = onAnimationFinishedBlendFrames;
    session.selectedState = stateIndex;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool setEditorSessionStateCollisionFlags(
    FighterEditorSession& session,
    int stateIndex,
    bool useAnimPhysics,
    bool allowSlideoff,
    bool allowLedgeGrab,
    bool allowBackwardsLedgeGrab,
    bool allowWallCollision,
    bool allowCeilingCollision,
    bool convertFloorCollisionToGround,
    std::string* error)
{
    FighterState* state = nullptr;
    if (!validSessionState(session, stateIndex, nullptr, &state, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    state->useAnimPhysics = useAnimPhysics;
    state->allowSlideoff = allowSlideoff;
    state->allowLedgeGrab = allowLedgeGrab;
    state->allowBackwardsLedgeGrab = allowBackwardsLedgeGrab;
    state->allowWallCollision = allowWallCollision;
    state->allowCeilingCollision = allowCeilingCollision;
    state->convertFloorCollisionToGround = convertFloorCollisionToGround;
    session.selectedState = stateIndex;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool setEditorSessionStateCallbacks(
    FighterEditorSession& session,
    int stateIndex,
    FighterEditorStateCallbackSlot slot,
    const std::vector<FunctionCall>& calls,
    std::string* error)
{
    FighterState* state = nullptr;
    if (!validSessionState(session, stateIndex, nullptr, &state, error)) {
        return false;
    }
    std::vector<FunctionCall>* target = stateCallbacks(*state, slot);
    if (!target) {
        setEditorError(error, "editor state callback slot is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    *target = calls;
    session.selectedState = stateIndex;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool addEditorSessionSubaction(
    FighterEditorSession& session,
    int stateIndex,
    const Subaction& subaction,
    int insertIndex,
    int* addedSubactionIndex,
    std::string* error)
{
    FighterState* state = nullptr;
    if (!validSessionState(session, stateIndex, nullptr, &state, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    const int index = insertIndex < 0
        ? static_cast<int>(state->action.size())
        : std::clamp(insertIndex, 0, static_cast<int>(state->action.size()));
    state->action.insert(state->action.begin() + index, subaction);
    session.selectedState = stateIndex;
    session.selectedSubaction = index;
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedSubactionIndex) {
        *addedSubactionIndex = index;
    }
    return true;
}

bool removeEditorSessionSubaction(
    FighterEditorSession& session,
    int stateIndex,
    int subactionIndex,
    std::string* error)
{
    FighterState* state = nullptr;
    if (!validSessionState(session, stateIndex, nullptr, &state, error)) {
        return false;
    }
    if (subactionIndex < 0 || subactionIndex >= static_cast<int>(state->action.size())) {
        setEditorError(error, "editor subaction index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    state->action.erase(state->action.begin() + subactionIndex);
    session.selectedState = stateIndex;
    session.selectedSubaction = std::clamp(subactionIndex, 0, std::max(0, static_cast<int>(state->action.size()) - 1));
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool moveEditorSessionSubaction(
    FighterEditorSession& session,
    int stateIndex,
    int subactionIndex,
    int delta,
    int* movedSubactionIndex,
    std::string* error)
{
    FighterState* state = nullptr;
    if (!validSessionState(session, stateIndex, nullptr, &state, error)) {
        return false;
    }
    if (delta == 0) {
        if (movedSubactionIndex) {
            *movedSubactionIndex = subactionIndex;
        }
        return true;
    }
    const int targetIndex = subactionIndex + delta;
    if (subactionIndex < 0 || subactionIndex >= static_cast<int>(state->action.size()) ||
        targetIndex < 0 || targetIndex >= static_cast<int>(state->action.size()))
    {
        setEditorError(error, "editor subaction move is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    std::swap(state->action[static_cast<size_t>(subactionIndex)], state->action[static_cast<size_t>(targetIndex)]);
    session.selectedState = stateIndex;
    session.selectedSubaction = targetIndex;
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (movedSubactionIndex) {
        *movedSubactionIndex = targetIndex;
    }
    return true;
}

bool addEditorSessionInterrupt(
    FighterEditorSession& session,
    int stateIndex,
    const InterruptRule& interrupt,
    int insertIndex,
    int* addedInterruptIndex,
    std::string* error)
{
    FighterState* state = nullptr;
    if (!validSessionState(session, stateIndex, nullptr, &state, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    const int index = insertIndex < 0
        ? static_cast<int>(state->interrupts.size())
        : std::clamp(insertIndex, 0, static_cast<int>(state->interrupts.size()));
    state->interrupts.insert(state->interrupts.begin() + index, interrupt);
    session.selectedState = stateIndex;
    session.selectedInterrupt = index;
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedInterruptIndex) {
        *addedInterruptIndex = index;
    }
    return true;
}

bool removeEditorSessionInterrupt(
    FighterEditorSession& session,
    int stateIndex,
    int interruptIndex,
    std::string* error)
{
    FighterState* state = nullptr;
    if (!validSessionState(session, stateIndex, nullptr, &state, error)) {
        return false;
    }
    if (interruptIndex < 0 || interruptIndex >= static_cast<int>(state->interrupts.size())) {
        setEditorError(error, "editor interrupt index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    state->interrupts.erase(state->interrupts.begin() + interruptIndex);
    session.selectedState = stateIndex;
    session.selectedInterrupt = std::clamp(interruptIndex, 0, std::max(0, static_cast<int>(state->interrupts.size()) - 1));
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

std::string uniqueEditorPackageVariableName(const FighterDefinition& def, const std::string& prefix) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        if (editorPackageVariableNameAvailable(def, candidate)) {
            return candidate;
        }
    }
    return prefix + "X";
}

std::string uniqueEditorPackageScriptName(const FighterDefinition& def, const std::string& prefix) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        if (editorPackageScriptNameAvailable(def, candidate)) {
            return candidate;
        }
    }
    return prefix + "X";
}

bool editorPackageVariableNameAvailable(const FighterDefinition& def, const std::string& name, int ignoredIndex) {
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

bool editorPackageScriptNameAvailable(const FighterDefinition& def, const std::string& name, int ignoredIndex) {
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

bool addEditorSessionPackageVariable(
    FighterEditorSession& session,
    const std::string& requestedName,
    int32_t initialValue,
    int* addedVariableIndex,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    const std::string name = requestedName.empty() ? uniqueEditorPackageVariableName(def) : requestedName;
    if (!editorPackageVariableNameAvailable(def, name)) {
        setEditorError(error, "editor package variable name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    def.packageVariables.push_back({name, initialValue});
    session.selectedFighter = 0;
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedVariableIndex) {
        *addedVariableIndex = static_cast<int>(def.packageVariables.size()) - 1;
    }
    return true;
}

bool renameEditorSessionPackageVariable(
    FighterEditorSession& session,
    int variableIndex,
    const std::string& newName,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    if (variableIndex < 0 || variableIndex >= static_cast<int>(def.packageVariables.size())) {
        setEditorError(error, "editor package variable index is invalid");
        return false;
    }
    if (!editorPackageVariableNameAvailable(def, newName, variableIndex)) {
        setEditorError(error, "editor package variable name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    def.packageVariables[static_cast<size_t>(variableIndex)].name = newName;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionPackageVariable(
    FighterEditorSession& session,
    int variableIndex,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    if (variableIndex < 0 || variableIndex >= static_cast<int>(def.packageVariables.size())) {
        setEditorError(error, "editor package variable index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    def.packageVariables.erase(def.packageVariables.begin() + variableIndex);
    const int variableCountAfterRemove = static_cast<int>(def.packageVariables.size());
    remapRemovedPackageVariable(def.packageScripts, variableIndex, variableCountAfterRemove);
    remapRemovedInterruptPackageVariable(def.states, variableIndex, variableCountAfterRemove);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool addEditorSessionPackageScript(
    FighterEditorSession& session,
    const std::string& requestedName,
    int instructionBudget,
    int* addedScriptIndex,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    const std::string name = requestedName.empty() ? uniqueEditorPackageScriptName(def) : requestedName;
    if (!editorPackageScriptNameAvailable(def, name)) {
        setEditorError(error, "editor package script name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    PackageScript script;
    script.name = name;
    script.instructionBudget = instructionBudget;
    def.packageScripts.push_back(std::move(script));
    session.selectedPackageScript = static_cast<int>(def.packageScripts.size()) - 1;
    session.selectedPackageInstruction = 0;
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedScriptIndex) {
        *addedScriptIndex = static_cast<int>(def.packageScripts.size()) - 1;
    }
    return true;
}

bool duplicateEditorSessionPackageScript(
    FighterEditorSession& session,
    int scriptIndex,
    int* addedScriptIndex,
    std::string* error)
{
    FighterDefinition* def = nullptr;
    PackageScript* script = nullptr;
    if (!validSessionScript(session, scriptIndex, &def, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    PackageScript clone = *script;
    clone.name = uniqueEditorPackageScriptName(*def, clone.name + "Copy");
    def->packageScripts.push_back(std::move(clone));
    session.selectedPackageScript = static_cast<int>(def->packageScripts.size()) - 1;
    session.selectedPackageInstruction = 0;
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedScriptIndex) {
        *addedScriptIndex = static_cast<int>(def->packageScripts.size()) - 1;
    }
    return true;
}

bool renameEditorSessionPackageScript(
    FighterEditorSession& session,
    int scriptIndex,
    const std::string& newName,
    std::string* error)
{
    FighterDefinition* def = nullptr;
    PackageScript* script = nullptr;
    if (!validSessionScript(session, scriptIndex, &def, &script, error)) {
        return false;
    }
    if (!editorPackageScriptNameAvailable(*def, newName, scriptIndex)) {
        setEditorError(error, "editor package script name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    const std::string oldName = script->name;
    script->name = newName;
    remapFighterPackageScriptRefs(*def, oldName, newName);
    remapCrossFighterPackageScriptRefs(session.package, oldName, newName);
    session.selectedPackageScript = scriptIndex;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionPackageScript(
    FighterEditorSession& session,
    int scriptIndex,
    std::string* error)
{
    FighterDefinition* def = nullptr;
    PackageScript* script = nullptr;
    if (!validSessionScript(session, scriptIndex, &def, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    const std::string removed = script->name;
    def->packageScripts.erase(def->packageScripts.begin() + scriptIndex);
    removeFighterPackageScriptRefs(*def, removed);
    removeCrossFighterPackageScriptRefs(session.package, removed);
    session.selectedPackageScript = std::clamp(scriptIndex, 0, std::max(0, static_cast<int>(def->packageScripts.size()) - 1));
    session.selectedPackageInstruction = 0;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool setEditorSessionPackageScriptBudget(
    FighterEditorSession& session,
    int scriptIndex,
    int instructionBudget,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionScript(session, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    script->instructionBudget = instructionBudget;
    session.selectedPackageScript = scriptIndex;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool addEditorSessionPackageInstruction(
    FighterEditorSession& session,
    int scriptIndex,
    const PackageScriptInstruction& instruction,
    int insertIndex,
    int* addedInstructionIndex,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionScript(session, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    const int index = insertIndex < 0
        ? static_cast<int>(script->instructions.size())
        : std::clamp(insertIndex, 0, static_cast<int>(script->instructions.size()));
    script->instructions.insert(script->instructions.begin() + index, instruction);
    session.selectedPackageScript = scriptIndex;
    session.selectedPackageInstruction = index;
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedInstructionIndex) {
        *addedInstructionIndex = index;
    }
    return true;
}

bool setEditorSessionPackageInstruction(
    FighterEditorSession& session,
    int scriptIndex,
    int instructionIndex,
    const PackageScriptInstruction& instruction,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionScript(session, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    if (instructionIndex < 0 || instructionIndex >= static_cast<int>(script->instructions.size())) {
        setEditorError(error, "editor package instruction index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    script->instructions[static_cast<size_t>(instructionIndex)] = instruction;
    session.selectedPackageScript = scriptIndex;
    session.selectedPackageInstruction = instructionIndex;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionPackageInstruction(
    FighterEditorSession& session,
    int scriptIndex,
    int instructionIndex,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionScript(session, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    if (instructionIndex < 0 || instructionIndex >= static_cast<int>(script->instructions.size())) {
        setEditorError(error, "editor package instruction index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    script->instructions.erase(script->instructions.begin() + instructionIndex);
    session.selectedPackageScript = scriptIndex;
    session.selectedPackageInstruction = std::clamp(instructionIndex, 0, std::max(0, static_cast<int>(script->instructions.size()) - 1));
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool moveEditorSessionPackageInstruction(
    FighterEditorSession& session,
    int scriptIndex,
    int instructionIndex,
    int delta,
    int* movedInstructionIndex,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionScript(session, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    if (delta == 0) {
        if (movedInstructionIndex) {
            *movedInstructionIndex = instructionIndex;
        }
        return true;
    }
    const int targetIndex = instructionIndex + delta;
    if (instructionIndex < 0 || instructionIndex >= static_cast<int>(script->instructions.size()) ||
        targetIndex < 0 || targetIndex >= static_cast<int>(script->instructions.size()))
    {
        setEditorError(error, "editor package instruction move is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    std::swap(script->instructions[static_cast<size_t>(instructionIndex)], script->instructions[static_cast<size_t>(targetIndex)]);
    session.selectedPackageScript = scriptIndex;
    session.selectedPackageInstruction = targetIndex;
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (movedInstructionIndex) {
        *movedInstructionIndex = targetIndex;
    }
    return true;
}

bool bindEditorSessionPackageScriptCallback(
    FighterEditorSession& session,
    int stateIndex,
    FighterEditorStateCallbackSlot slot,
    const std::string& scriptName,
    std::string* error)
{
    FighterDefinition* def = nullptr;
    FighterState* state = nullptr;
    if (!validSessionState(session, stateIndex, &def, &state, error)) {
        return false;
    }
    const bool scriptExists = std::any_of(def->packageScripts.begin(), def->packageScripts.end(), [&](const PackageScript& script) {
        return script.name == scriptName;
    });
    if (!scriptExists) {
        setEditorError(error, "editor package script callback target is invalid");
        return false;
    }
    std::vector<FunctionCall>* calls = stateCallbacks(*state, slot);
    if (!calls) {
        setEditorError(error, "editor state callback slot is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    const std::string callback = "script:" + scriptName;
    if (!hasFunctionCall(*calls, callback)) {
        calls->push_back({callback});
    }
    session.selectedState = stateIndex;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

std::string uniqueEditorObjectName(const FighterPackage& package, const std::string& prefix) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        if (editorObjectNameAvailable(package, candidate)) {
            return candidate;
        }
    }
    return prefix + "X";
}

std::string uniqueEditorObjectStateName(const GameObjectDefinition& object, const std::string& prefix) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        if (editorObjectStateNameAvailable(object, candidate)) {
            return candidate;
        }
    }
    return prefix + "X";
}

bool editorObjectNameAvailable(const FighterPackage& package, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < package.objects.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && package.objects[i].name == name) {
            return false;
        }
    }
    return true;
}

bool editorObjectStateNameAvailable(const GameObjectDefinition& object, const std::string& name, int ignoredIndex) {
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

bool renameEditorSessionObject(
    FighterEditorSession& session,
    int objectIndex,
    const std::string& newName,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    if (!editorObjectNameAvailable(session.package, newName, objectIndex)) {
        setEditorError(error, "editor object name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    const std::string oldName = object->name;
    object->name = newName;
    remapPackageObjectTargets(session.package, oldName, newName, object->kind);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionObject(
    FighterEditorSession& session,
    int objectIndex,
    const std::string& replacementObjectName,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    const std::string removedName = object->name;
    std::string replacementName = replacementObjectName;
    GameObjectKind replacementKind = GameObjectKind::Item;
    if (!replacementName.empty()) {
        if (replacementName == removedName) {
            setEditorError(error, "editor replacement object cannot be the removed object");
            return false;
        }
        const auto replacement = std::find_if(session.package.objects.begin(), session.package.objects.end(), [&](const GameObjectDefinition& candidate) {
            return candidate.name == replacementName;
        });
        if (replacement == session.package.objects.end()) {
            setEditorError(error, "editor replacement object is invalid");
            return false;
        }
        replacementKind = replacement->kind;
    }
    FighterPackage previous = session.package;
    remapPackageObjectTargets(session.package, removedName, replacementName, replacementKind);
    session.package.objects.erase(session.package.objects.begin() + objectIndex);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool setEditorSessionObjectKind(
    FighterEditorSession& session,
    int objectIndex,
    GameObjectKind kind,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    object->kind = kind;
    remapPackageObjectTargets(session.package, object->name, object->name, kind);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool setEditorSessionObjectProperties(
    FighterEditorSession& session,
    int objectIndex,
    int lifetimeFrames,
    Fix gravity,
    Fix terminalVelocity,
    Fix maxDamage,
    bool destroyOnHit,
    bool destroyOnShield,
    bool hitOwner,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    object->lifetimeFrames = lifetimeFrames;
    object->gravity = gravity;
    object->terminalVelocity = terminalVelocity;
    object->maxDamage = maxDamage;
    object->destroyOnHit = destroyOnHit;
    object->destroyOnShield = destroyOnShield;
    object->hitOwner = hitOwner;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool createEditorSessionObjectState(
    FighterEditorSession& session,
    int objectIndex,
    const std::string& requestedName,
    int sourceStateIndex,
    int* createdStateIndex,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    const std::string name = requestedName.empty() ? uniqueEditorObjectStateName(*object) : requestedName;
    if (!editorObjectStateNameAvailable(*object, name)) {
        setEditorError(error, "editor object state name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    GameObjectStateDefinition state;
    state.name = name;
    state.animationLengthFrames = 1;
    state.loopAnimation = true;
    if (sourceStateIndex >= 0 && sourceStateIndex < static_cast<int>(object->states.size())) {
        state = object->states[static_cast<size_t>(sourceStateIndex)];
        state.name = name;
    }
    object->states.push_back(std::move(state));
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (createdStateIndex) {
        *createdStateIndex = static_cast<int>(session.package.objects[static_cast<size_t>(objectIndex)].states.size()) - 1;
    }
    return true;
}

bool duplicateEditorSessionObjectState(
    FighterEditorSession& session,
    int objectIndex,
    int sourceStateIndex,
    int* createdStateIndex,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    if (object->states.empty()) {
        setEditorError(error, "editor cannot clone a state from an empty object");
        return false;
    }
    const int sourceIndex = sourceStateIndex >= 0
        ? sourceStateIndex
        : std::clamp(object->initialState, 0, static_cast<int>(object->states.size()) - 1);
    if (sourceIndex < 0 || sourceIndex >= static_cast<int>(object->states.size())) {
        setEditorError(error, "editor object clone source state is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    GameObjectStateDefinition clone = object->states[static_cast<size_t>(sourceIndex)];
    clone.name = uniqueEditorObjectStateName(*object, clone.name + "Copy");
    const int insertIndex = std::clamp(sourceIndex + 1, 0, static_cast<int>(object->states.size()));
    object->states.insert(object->states.begin() + insertIndex, std::move(clone));
    if (object->initialState >= insertIndex) {
        ++object->initialState;
    }
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (createdStateIndex) {
        *createdStateIndex = insertIndex;
    }
    return true;
}

bool renameEditorSessionObjectState(
    FighterEditorSession& session,
    int objectIndex,
    int stateIndex,
    const std::string& newName,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    GameObjectStateDefinition* state = nullptr;
    if (!validSessionObjectState(session, objectIndex, stateIndex, &object, &state, error)) {
        return false;
    }
    if (!editorObjectStateNameAvailable(*object, newName, stateIndex)) {
        setEditorError(error, "editor object state name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    const std::string oldName = state->name;
    state->name = newName;
    remapObjectStateScriptTargets(*object, oldName, newName);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionObjectState(
    FighterEditorSession& session,
    int objectIndex,
    int stateIndex,
    const std::string& replacementStateName,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    if (object->states.size() <= 1) {
        setEditorError(error, "editor cannot remove the only object state");
        return false;
    }
    if (stateIndex < 0 || stateIndex >= static_cast<int>(object->states.size())) {
        setEditorError(error, "editor remove object state index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    const std::string removedName = object->states[static_cast<size_t>(stateIndex)].name;
    object->states.erase(object->states.begin() + stateIndex);
    std::string replacement = replacementStateName;
    const auto replacementState = std::find_if(object->states.begin(), object->states.end(), [&](const GameObjectStateDefinition& state) {
        return state.name == replacement;
    });
    if (replacement.empty() || replacementState == object->states.end()) {
        const int replacementIndex = std::clamp(stateIndex, 0, static_cast<int>(object->states.size()) - 1);
        replacement = object->states[static_cast<size_t>(replacementIndex)].name;
    }
    if (object->initialState == stateIndex) {
        const auto initial = std::find_if(object->states.begin(), object->states.end(), [&](const GameObjectStateDefinition& state) {
            return state.name == replacement;
        });
        object->initialState = initial == object->states.end()
            ? 0
            : static_cast<int>(std::distance(object->states.begin(), initial));
    } else if (object->initialState > stateIndex) {
        --object->initialState;
    }
    remapObjectStateScriptTargets(*object, removedName, replacement);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool setEditorSessionObjectStateTiming(
    FighterEditorSession& session,
    int objectIndex,
    int stateIndex,
    int animationLengthFrames,
    bool loopAnimation,
    bool makeInitialState,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    GameObjectStateDefinition* state = nullptr;
    if (!validSessionObjectState(session, objectIndex, stateIndex, &object, &state, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    state->animationLengthFrames = animationLengthFrames;
    state->loopAnimation = loopAnimation;
    if (makeInitialState) {
        object->initialState = stateIndex;
    }
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool setEditorSessionObjectStateCallbacks(
    FighterEditorSession& session,
    int objectIndex,
    int stateIndex,
    FighterEditorObjectStateCallbackSlot slot,
    const std::vector<FunctionCall>& calls,
    std::string* error)
{
    GameObjectStateDefinition* state = nullptr;
    if (!validSessionObjectState(session, objectIndex, stateIndex, nullptr, &state, error)) {
        return false;
    }
    std::vector<FunctionCall>* target = objectStateCallbacks(*state, slot);
    if (!target) {
        setEditorError(error, "editor object state callback slot is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    *target = calls;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool setEditorSessionObjectEventCallbacks(
    FighterEditorSession& session,
    int objectIndex,
    FighterEditorObjectEventCallbackSlot slot,
    const std::vector<FunctionCall>& calls,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    std::vector<FunctionCall>* target = objectEventCallbacks(*object, slot);
    if (!target) {
        setEditorError(error, "editor object event callback slot is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    *target = calls;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool addEditorSessionObjectHitbox(
    FighterEditorSession& session,
    int objectIndex,
    const HitboxDefinition& hitbox,
    int insertIndex,
    int* addedHitboxIndex,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    const int index = insertIndex < 0
        ? static_cast<int>(object->hitboxes.size())
        : std::clamp(insertIndex, 0, static_cast<int>(object->hitboxes.size()));
    object->hitboxes.insert(object->hitboxes.begin() + index, hitbox);
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedHitboxIndex) {
        *addedHitboxIndex = index;
    }
    return true;
}

bool setEditorSessionObjectHitbox(
    FighterEditorSession& session,
    int objectIndex,
    int hitboxIndex,
    const HitboxDefinition& hitbox,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    if (hitboxIndex < 0 || hitboxIndex >= static_cast<int>(object->hitboxes.size())) {
        setEditorError(error, "editor object hitbox index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    object->hitboxes[static_cast<size_t>(hitboxIndex)] = hitbox;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionObjectHitbox(
    FighterEditorSession& session,
    int objectIndex,
    int hitboxIndex,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    if (hitboxIndex < 0 || hitboxIndex >= static_cast<int>(object->hitboxes.size())) {
        setEditorError(error, "editor object hitbox index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    object->hitboxes.erase(object->hitboxes.begin() + hitboxIndex);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool addEditorSessionObjectHurtbox(
    FighterEditorSession& session,
    int objectIndex,
    const GameObjectHurtboxDefinition& hurtbox,
    int insertIndex,
    int* addedHurtboxIndex,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    const int index = insertIndex < 0
        ? static_cast<int>(object->hurtboxes.size())
        : std::clamp(insertIndex, 0, static_cast<int>(object->hurtboxes.size()));
    object->hurtboxes.insert(object->hurtboxes.begin() + index, hurtbox);
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedHurtboxIndex) {
        *addedHurtboxIndex = index;
    }
    return true;
}

bool setEditorSessionObjectHurtbox(
    FighterEditorSession& session,
    int objectIndex,
    int hurtboxIndex,
    const GameObjectHurtboxDefinition& hurtbox,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    if (hurtboxIndex < 0 || hurtboxIndex >= static_cast<int>(object->hurtboxes.size())) {
        setEditorError(error, "editor object hurtbox index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    object->hurtboxes[static_cast<size_t>(hurtboxIndex)] = hurtbox;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionObjectHurtbox(
    FighterEditorSession& session,
    int objectIndex,
    int hurtboxIndex,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    if (hurtboxIndex < 0 || hurtboxIndex >= static_cast<int>(object->hurtboxes.size())) {
        setEditorError(error, "editor object hurtbox index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    object->hurtboxes.erase(object->hurtboxes.begin() + hurtboxIndex);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool addEditorSessionObjectTouchbox(
    FighterEditorSession& session,
    int objectIndex,
    const GameObjectTouchboxDefinition& touchbox,
    int insertIndex,
    int* addedTouchboxIndex,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    const int index = insertIndex < 0
        ? static_cast<int>(object->touchboxes.size())
        : std::clamp(insertIndex, 0, static_cast<int>(object->touchboxes.size()));
    object->touchboxes.insert(object->touchboxes.begin() + index, touchbox);
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedTouchboxIndex) {
        *addedTouchboxIndex = index;
    }
    return true;
}

bool setEditorSessionObjectTouchbox(
    FighterEditorSession& session,
    int objectIndex,
    int touchboxIndex,
    const GameObjectTouchboxDefinition& touchbox,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    if (touchboxIndex < 0 || touchboxIndex >= static_cast<int>(object->touchboxes.size())) {
        setEditorError(error, "editor object touchbox index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    object->touchboxes[static_cast<size_t>(touchboxIndex)] = touchbox;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionObjectTouchbox(
    FighterEditorSession& session,
    int objectIndex,
    int touchboxIndex,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    if (touchboxIndex < 0 || touchboxIndex >= static_cast<int>(object->touchboxes.size())) {
        setEditorError(error, "editor object touchbox index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    object->touchboxes.erase(object->touchboxes.begin() + touchboxIndex);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

} // namespace pf
