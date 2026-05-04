#include "editor/fighter_editor.hpp"

#include "core/action.hpp"

#include <algorithm>
#include <cmath>
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

void remapPackageFighterTargetRefs(FighterPackage& package, const std::string& oldName, const std::string& newName) {
    for (FighterDefinition& fighter : package.fighters) {
        for (PackageScript& script : fighter.packageScripts) {
            for (PackageScriptInstruction& instruction : script.instructions) {
                if (packageScriptInstructionTargetsFighter(instruction) && instruction.text == oldName) {
                    instruction.text = newName;
                }
            }
        }
    }
    for (GameObjectDefinition& object : package.objects) {
        for (PackageScript& script : object.packageScripts) {
            for (PackageScriptInstruction& instruction : script.instructions) {
                if (packageScriptInstructionTargetsFighter(instruction) && instruction.text == oldName) {
                    instruction.text = newName;
                }
            }
        }
    }
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

bool validSessionPackageFighter(
    FighterEditorSession& session,
    int fighterIndex,
    FighterDefinition** fighter,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    if (fighterIndex < 0 || fighterIndex >= static_cast<int>(session.package.fighters.size())) {
        setEditorError(error, "editor package fighter index is invalid");
        return false;
    }
    if (fighter) {
        *fighter = &session.package.fighters[static_cast<size_t>(fighterIndex)];
    }
    return true;
}

bool validSessionPackageFighterState(
    FighterEditorSession& session,
    int fighterIndex,
    int stateIndex,
    FighterDefinition** fighter,
    FighterState** state,
    std::string* error)
{
    FighterDefinition* def = nullptr;
    if (!validSessionPackageFighter(session, fighterIndex, &def, error)) {
        return false;
    }
    if (stateIndex < 0 || stateIndex >= static_cast<int>(def->states.size())) {
        setEditorError(error, "editor state index is invalid");
        return false;
    }
    if (fighter) {
        *fighter = def;
    }
    if (state) {
        *state = &def->states[static_cast<size_t>(stateIndex)];
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

void ensurePackageScriptGraphs(FighterPackage& package) {
    for (FighterDefinition& fighter : package.fighters) {
        for (PackageScript& script : fighter.packageScripts) {
            if (script.graph.nodes.empty()) {
                script.graph = makePackageScriptControlFlowGraph(script);
            }
        }
    }
    for (GameObjectDefinition& object : package.objects) {
        for (PackageScript& script : object.packageScripts) {
            if (script.graph.nodes.empty()) {
                script.graph = makePackageScriptControlFlowGraph(script);
            }
        }
    }
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

void clearPackageVariableInstruction(PackageScriptInstruction& instruction) {
    instruction.op = PackageScriptOp::Nop;
    instruction.dst = -1;
    instruction.srcA = -1;
    instruction.srcB = -1;
    instruction.intValue = 0;
    instruction.fixValue = 0;
    instruction.text.clear();
}

bool remapRemovedPackageVariableRefs(
    PackageScriptInstruction& instruction,
    int removedIndex,
    int variableCountAfterRemove,
    bool usesDst,
    bool usesSrcA,
    bool usesSrcB)
{
    if (usesDst) {
        remapRemovedPackageVariableRef(instruction.dst, removedIndex, variableCountAfterRemove);
    }
    if (usesSrcA) {
        remapRemovedPackageVariableRef(instruction.srcA, removedIndex, variableCountAfterRemove);
    }
    if (usesSrcB) {
        remapRemovedPackageVariableRef(instruction.srcB, removedIndex, variableCountAfterRemove);
    }
    const bool stillValid = (!usesDst || instruction.dst >= 0) &&
        (!usesSrcA || instruction.srcA >= 0) &&
        (!usesSrcB || instruction.srcB >= 0);
    if (!stillValid) {
        clearPackageVariableInstruction(instruction);
    }
    return stillValid;
}

void remapRemovedPackageVariableInstruction(
    PackageScriptInstruction& instruction,
    int removedIndex,
    int variableCountAfterRemove)
{
    switch (instruction.op) {
    case PackageScriptOp::Nop:
    case PackageScriptOp::SetGroundVelocity:
    case PackageScriptOp::SetAirVelocityX:
    case PackageScriptOp::SetAirVelocityY:
    case PackageScriptOp::SetAnimationRate:
    case PackageScriptOp::SetAnimationFrame:
    case PackageScriptOp::SetPositionX:
    case PackageScriptOp::SetPositionY:
    case PackageScriptOp::SetFacing:
    case PackageScriptOp::ChangeState:
    case PackageScriptOp::SpawnObject:
    case PackageScriptOp::SpawnProjectile:
    case PackageScriptOp::DestroyObject:
    case PackageScriptOp::CallScript:
    case PackageScriptOp::SwitchFighterDefinition:
    case PackageScriptOp::SpawnFighter:
    case PackageScriptOp::SetFighterCommandVarImmediate:
    case PackageScriptOp::SetFighterThrowFlagImmediate:
    case PackageScriptOp::SetObjectDamage:
    case PackageScriptOp::SetObjectHitlag:
    case PackageScriptOp::SetObjectOwner:
    case PackageScriptOp::SetOwnerFighterVarImmediate:
    case PackageScriptOp::CallOwnerFighterScript:
    case PackageScriptOp::DestroyOwnedObjects:
        return;
    case PackageScriptOp::SetVarImmediate:
    case PackageScriptOp::AddVarImmediate:
    case PackageScriptOp::SetVarFrame:
    case PackageScriptOp::SetVarStateFrame:
    case PackageScriptOp::SetVarStateIndex:
    case PackageScriptOp::SetVarGrounded:
    case PackageScriptOp::SetVarFacing:
    case PackageScriptOp::SetVarFighterIndex:
    case PackageScriptOp::SetVarObjectIndex:
    case PackageScriptOp::SetVarFighterStateFrame:
    case PackageScriptOp::SetVarFighterStateIndex:
    case PackageScriptOp::SetVarFighterGrounded:
    case PackageScriptOp::SetVarFighterFacing:
    case PackageScriptOp::SetVarFighterJumpsUsed:
    case PackageScriptOp::SetVarFighterJumpsRemaining:
    case PackageScriptOp::SetVarFighterCommandVar:
    case PackageScriptOp::SetVarFighterThrowFlag:
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
    case PackageScriptOp::SetVarObjectHitlag:
    case PackageScriptOp::SetVarObjectGroundSegment:
    case PackageScriptOp::SetVarObjectPositionX:
    case PackageScriptOp::SetVarObjectPositionY:
    case PackageScriptOp::SetVarObjectVelocityX:
    case PackageScriptOp::SetVarObjectVelocityY:
    case PackageScriptOp::SetVarObjectAnimationFrame:
    case PackageScriptOp::SetVarObjectAnimationRate:
    case PackageScriptOp::SetVarOwnedObjectCount:
    case PackageScriptOp::SetVarOwnerFighterVar:
    case PackageScriptOp::SetVarIndexedFighterVar:
    case PackageScriptOp::SetVarIndexedFighterStateIndex:
    case PackageScriptOp::SetVarIndexedFighterPositionX:
    case PackageScriptOp::SetVarIndexedFighterPositionY:
    case PackageScriptOp::SetVarIndexedObjectVar:
    case PackageScriptOp::SetVarButtonDown:
    case PackageScriptOp::SetVarButtonPressed:
    case PackageScriptOp::SetVarStickX:
    case PackageScriptOp::SetVarStickY:
    case PackageScriptOp::SetVarCStickX:
    case PackageScriptOp::SetVarCStickY:
    case PackageScriptOp::SetVarShield:
    case PackageScriptOp::SetVarRandom:
    case PackageScriptOp::SpawnObjectSetVar:
    case PackageScriptOp::SpawnProjectileSetVar:
    case PackageScriptOp::SetVarPickUpObjectFromVar:
    case PackageScriptOp::SetVarDropObjectFromVar:
    case PackageScriptOp::SetVarThrowObjectFromVar:
    case PackageScriptOp::SetVarReflectObjectFromVar:
    case PackageScriptOp::SetVarAbsorbObjectFromVar:
    case PackageScriptOp::SetVarShieldBounceObjectFromVar:
    case PackageScriptOp::SetVarInteractObjectFromVar:
    case PackageScriptOp::SkipIfVarLessThanImmediate:
    case PackageScriptOp::SkipIfVarEqualImmediate:
    case PackageScriptOp::SpawnFighterSetVar:
        remapRemovedPackageVariableRefs(instruction, removedIndex, variableCountAfterRemove, true, false, false);
        return;
    case PackageScriptOp::SetVarFromVar:
    case PackageScriptOp::AddVar:
    case PackageScriptOp::SkipIfVarLessThanVar:
    case PackageScriptOp::SkipIfVarEqualVar:
    case PackageScriptOp::SetVarLessThanVar:
    case PackageScriptOp::SetVarEqualVar:
    case PackageScriptOp::SetVarNotEqualVar:
    case PackageScriptOp::SetVarGreaterThanVar:
    case PackageScriptOp::SetVarAnd:
    case PackageScriptOp::SetVarOr:
    case PackageScriptOp::SetVarInteractObjectsFromVars:
        remapRemovedPackageVariableRefs(instruction, removedIndex, variableCountAfterRemove, true, true, true);
        return;
    case PackageScriptOp::ScaleVarFixed:
    case PackageScriptOp::SetVarLessThanImmediate:
    case PackageScriptOp::SetVarEqualImmediate:
    case PackageScriptOp::SetVarNotEqualImmediate:
    case PackageScriptOp::SetVarGreaterThanImmediate:
    case PackageScriptOp::SetVarNot:
        remapRemovedPackageVariableRefs(instruction, removedIndex, variableCountAfterRemove, true, true, false);
        return;
    case PackageScriptOp::SetFighterJumpsUsedFromVar:
    case PackageScriptOp::SetFighterCommandVarFromVar:
    case PackageScriptOp::SetFighterThrowFlagFromVar:
    case PackageScriptOp::SetObjectDamageFromVar:
    case PackageScriptOp::SetObjectHitlagFromVar:
    case PackageScriptOp::SetObjectOwnerFromVar:
    case PackageScriptOp::SetOwnerFighterVarFromVar:
    case PackageScriptOp::SetGroundVelocityFromVar:
    case PackageScriptOp::SetAirVelocityXFromVar:
    case PackageScriptOp::SetAirVelocityYFromVar:
    case PackageScriptOp::SetAnimationRateFromVar:
    case PackageScriptOp::SetAnimationFrameFromVar:
    case PackageScriptOp::SetPositionXFromVar:
    case PackageScriptOp::SetPositionYFromVar:
    case PackageScriptOp::SetFacingFromVar:
    case PackageScriptOp::DestroyObjectFromVar:
        remapRemovedPackageVariableRefs(instruction, removedIndex, variableCountAfterRemove, false, true, false);
        return;
    case PackageScriptOp::SetIndexedFighterStateFromVar:
    case PackageScriptOp::SetIndexedFighterFacingFromVar:
        remapRemovedPackageVariableRefs(instruction, removedIndex, variableCountAfterRemove, true, true, false);
        return;
    case PackageScriptOp::SetIndexedFighterPositionFromVars:
    case PackageScriptOp::SpawnObjectFromVars:
    case PackageScriptOp::SpawnProjectileFromVars:
        remapRemovedPackageVariableRefs(instruction, removedIndex, variableCountAfterRemove, false, true, true);
        return;
    case PackageScriptOp::SetIndexedFighterVarImmediate:
    case PackageScriptOp::SetIndexedObjectVarImmediate:
    case PackageScriptOp::CallIndexedFighterScriptFromVar:
    case PackageScriptOp::CallIndexedObjectScriptFromVar:
        remapRemovedPackageVariableRefs(instruction, removedIndex, variableCountAfterRemove, false, true, false);
        return;
    case PackageScriptOp::SetIndexedFighterVarFromVar:
    case PackageScriptOp::SetIndexedObjectVarFromVar:
        remapRemovedPackageVariableRefs(instruction, removedIndex, variableCountAfterRemove, false, true, true);
        return;
    case PackageScriptOp::SpawnObjectFromVarsSetVar:
    case PackageScriptOp::SpawnProjectileFromVarsSetVar:
        remapRemovedPackageVariableRefs(instruction, removedIndex, variableCountAfterRemove, true, true, true);
        return;
    case PackageScriptOp::JumpRelative:
        return;
    }
}

void remapRemovedPackageVariable(std::vector<PackageScript>& scripts, int removedIndex, int variableCountAfterRemove) {
    for (PackageScript& script : scripts) {
        for (PackageScriptInstruction& instruction : script.instructions) {
            remapRemovedPackageVariableInstruction(instruction, removedIndex, variableCountAfterRemove);
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

bool graphHasNode(const PackageScriptGraph& graph, int nodeId) {
    return std::any_of(graph.nodes.begin(), graph.nodes.end(), [&](const PackageScriptGraphNode& node) {
        return node.id == nodeId;
    });
}

void eraseGraphLinksToMissingNodes(PackageScriptGraph& graph) {
    graph.links.erase(
        std::remove_if(graph.links.begin(), graph.links.end(), [&](const PackageScriptGraphLink& link) {
            return !graphHasNode(graph, link.fromNode) || !graphHasNode(graph, link.toNode);
        }),
        graph.links.end());
    if (graph.entryNode >= 0 && !graphHasNode(graph, graph.entryNode)) {
        graph.entryNode = -1;
    }
}

void remapGraphAfterInstructionInsert(PackageScriptGraph& graph, int instructionIndex) {
    for (PackageScriptGraphNode& node : graph.nodes) {
        if (node.kind == PackageScriptGraphNodeKind::Instruction && node.instructionIndex >= instructionIndex) {
            ++node.instructionIndex;
        }
    }
}

void remapGraphAfterInstructionRemove(PackageScriptGraph& graph, int instructionIndex) {
    graph.nodes.erase(
        std::remove_if(graph.nodes.begin(), graph.nodes.end(), [&](const PackageScriptGraphNode& node) {
            return node.kind == PackageScriptGraphNodeKind::Instruction && node.instructionIndex == instructionIndex;
        }),
        graph.nodes.end());
    for (PackageScriptGraphNode& node : graph.nodes) {
        if (node.kind == PackageScriptGraphNodeKind::Instruction && node.instructionIndex > instructionIndex) {
            --node.instructionIndex;
        }
    }
    eraseGraphLinksToMissingNodes(graph);
}

void remapGraphAfterInstructionSwap(PackageScriptGraph& graph, int firstIndex, int secondIndex) {
    for (PackageScriptGraphNode& node : graph.nodes) {
        if (node.kind != PackageScriptGraphNodeKind::Instruction) {
            continue;
        }
        if (node.instructionIndex == firstIndex) {
            node.instructionIndex = secondIndex;
        } else if (node.instructionIndex == secondIndex) {
            node.instructionIndex = firstIndex;
        }
    }
}

PackageScriptGraphNode* graphNodeById(PackageScriptGraph& graph, int nodeId) {
    const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const PackageScriptGraphNode& node) {
        return node.id == nodeId;
    });
    return found == graph.nodes.end() ? nullptr : &*found;
}

const PackageScriptGraphNode* graphNodeById(const PackageScriptGraph& graph, int nodeId) {
    const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const PackageScriptGraphNode& node) {
        return node.id == nodeId;
    });
    return found == graph.nodes.end() ? nullptr : &*found;
}

int nextGraphNodeId(const PackageScriptGraph& graph) {
    int next = 0;
    for (const PackageScriptGraphNode& node : graph.nodes) {
        next = std::max(next, node.id + 1);
    }
    return next;
}

bool addGraphNode(PackageScriptGraph& graph, PackageScriptGraphNode node, int* addedNodeId, std::string* error) {
    if (node.id < 0) {
        node.id = nextGraphNodeId(graph);
    }
    if (graphHasNode(graph, node.id)) {
        setEditorError(error, "editor package script graph node id is already used");
        return false;
    }
    if (node.kind == PackageScriptGraphNodeKind::Entry) {
        graph.entryNode = node.id;
    }
    graph.nodes.push_back(node);
    if (addedNodeId) {
        *addedNodeId = node.id;
    }
    return true;
}

bool setGraphNode(PackageScriptGraph& graph, int nodeId, PackageScriptGraphNode node, std::string* error) {
    PackageScriptGraphNode* target = graphNodeById(graph, nodeId);
    if (!target) {
        setEditorError(error, "editor package script graph node id is invalid");
        return false;
    }
    node.id = nodeId;
    *target = node;
    if (node.kind == PackageScriptGraphNodeKind::Entry) {
        graph.entryNode = nodeId;
    } else if (graph.entryNode == nodeId) {
        graph.entryNode = -1;
    }
    return true;
}

bool removeGraphNode(PackageScriptGraph& graph, int nodeId, std::string* error) {
    const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const PackageScriptGraphNode& node) {
        return node.id == nodeId;
    });
    if (found == graph.nodes.end()) {
        setEditorError(error, "editor package script graph node id is invalid");
        return false;
    }
    graph.nodes.erase(found);
    eraseGraphLinksToMissingNodes(graph);
    return true;
}

void setGraphLink(PackageScriptGraph& graph, const PackageScriptGraphLink& link) {
    const auto found = std::find_if(graph.links.begin(), graph.links.end(), [&](const PackageScriptGraphLink& existing) {
        return existing.fromNode == link.fromNode && existing.fromSocket == link.fromSocket;
    });
    if (found == graph.links.end()) {
        graph.links.push_back(link);
    } else {
        *found = link;
    }
}

bool removeGraphLink(PackageScriptGraph& graph, int fromNode, int fromSocket, std::string* error) {
    const auto found = std::find_if(graph.links.begin(), graph.links.end(), [&](const PackageScriptGraphLink& link) {
        return link.fromNode == fromNode && link.fromSocket == fromSocket;
    });
    if (found == graph.links.end()) {
        setEditorError(error, "editor package script graph link is invalid");
        return false;
    }
    graph.links.erase(found);
    return true;
}

bool graphNodeSeen(const std::vector<int>& seen, int nodeId) {
    return std::find(seen.begin(), seen.end(), nodeId) != seen.end();
}

bool packageInstructionIsSkipBranch(PackageScriptOp op) {
    return op == PackageScriptOp::SkipIfVarLessThanImmediate ||
        op == PackageScriptOp::SkipIfVarLessThanVar ||
        op == PackageScriptOp::SkipIfVarEqualImmediate ||
        op == PackageScriptOp::SkipIfVarEqualVar;
}

bool packageInstructionIsGraphRetargetable(PackageScriptOp op) {
    return packageInstructionIsSkipBranch(op) || op == PackageScriptOp::JumpRelative;
}

int graphInstructionNodeCount(const PackageScriptGraph& graph) {
    return static_cast<int>(std::count_if(graph.nodes.begin(), graph.nodes.end(), [](const PackageScriptGraphNode& node) {
        return node.kind == PackageScriptGraphNodeKind::Instruction;
    }));
}

bool graphSocketTarget(
    const PackageScriptGraph& graph,
    int currentNode,
    int socket,
    int& nextNode,
    bool& found,
    std::string* error)
{
    found = false;
    for (const PackageScriptGraphLink& link : graph.links) {
        if (link.fromNode != currentNode || link.fromSocket != socket) {
            continue;
        }
        if (found) {
            setEditorError(error, "editor package script graph has multiple links from one socket");
            return false;
        }
        found = true;
        nextNode = link.toNode;
    }
    if (!found) {
        nextNode = -1;
    }
    return true;
}

bool nextGraphControlNode(const PackageScriptGraph& graph, int currentNode, int& nextNode, std::string* error) {
    bool found = false;
    return graphSocketTarget(graph, currentNode, 0, nextNode, found, error);
}

bool compiledGraphNodeIndex(const std::vector<int>& compiledNodeIds, int nodeId, int& compiledIndex) {
    const auto found = std::find(compiledNodeIds.begin(), compiledNodeIds.end(), nodeId);
    if (found == compiledNodeIds.end()) {
        return false;
    }
    compiledIndex = static_cast<int>(found - compiledNodeIds.begin());
    return true;
}

bool resolveGraphTargetIndex(
    const PackageScriptGraph& graph,
    const std::vector<int>& compiledNodeIds,
    int targetNode,
    int instructionCount,
    int& targetIndex,
    std::string* error)
{
    if (targetNode < 0) {
        targetIndex = instructionCount;
        return true;
    }

    std::vector<int> seenNodes;
    int currentNode = targetNode;
    while (currentNode >= 0) {
        if (graphNodeSeen(seenNodes, currentNode)) {
            setEditorError(error, "editor package script graph target has a control cycle");
            return false;
        }
        seenNodes.push_back(currentNode);

        const PackageScriptGraphNode* node = graphNodeById(graph, currentNode);
        if (!node) {
            setEditorError(error, "editor package script graph target is invalid");
            return false;
        }
        if (node->kind == PackageScriptGraphNodeKind::Instruction) {
            if (!compiledGraphNodeIndex(compiledNodeIds, node->id, targetIndex)) {
                setEditorError(error, "editor package script graph target is disconnected");
                return false;
            }
            return true;
        }
        if (node->kind == PackageScriptGraphNodeKind::Entry) {
            setEditorError(error, "editor package script graph target is invalid");
            return false;
        }

        bool found = false;
        int nextNode = -1;
        if (!graphSocketTarget(graph, currentNode, 0, nextNode, found, error)) {
            return false;
        }
        if (!found) {
            targetIndex = instructionCount;
            return true;
        }
        currentNode = nextNode;
    }

    targetIndex = instructionCount;
    return true;
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

bool validSessionObjectScript(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    GameObjectDefinition** object,
    PackageScript** script,
    std::string* error)
{
    GameObjectDefinition* targetObject = nullptr;
    if (!validSessionObject(session, objectIndex, &targetObject, error)) {
        return false;
    }
    if (scriptIndex < 0 || scriptIndex >= static_cast<int>(targetObject->packageScripts.size())) {
        setEditorError(error, "editor object package script index is invalid");
        return false;
    }
    if (object) {
        *object = targetObject;
    }
    if (script) {
        *script = &targetObject->packageScripts[static_cast<size_t>(scriptIndex)];
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

bool packageInstructionCallsLocalScriptName(const PackageScriptInstruction& instruction, const std::string& scriptName) {
    return instruction.op == PackageScriptOp::CallScript && instruction.text == scriptName;
}

void removeLocalPackageScriptInstructionRefs(std::vector<PackageScript>& scripts, const std::string& scriptName) {
    for (PackageScript& script : scripts) {
        for (PackageScriptInstruction& instruction : script.instructions) {
            if (packageInstructionCallsLocalScriptName(instruction, scriptName)) {
                clearPackageVariableInstruction(instruction);
            }
        }
    }
}

void remapLocalPackageScriptInstructionRefs(
    std::vector<PackageScript>& scripts,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    for (PackageScript& script : scripts) {
        for (PackageScriptInstruction& instruction : script.instructions) {
            if (packageInstructionCallsLocalScriptName(instruction, oldScriptName)) {
                instruction.text = newScriptName;
            }
        }
    }
}

void removeObjectPackageScriptCallbackRefs(GameObjectDefinition& object, const std::string& scriptName) {
    for (GameObjectStateDefinition& state : object.states) {
        removePackageScriptCallbackRefs(state.onEnter, scriptName);
        removePackageScriptCallbackRefs(state.onFrame, scriptName);
        removePackageScriptCallbackRefs(state.onPhysics, scriptName);
        removePackageScriptCallbackRefs(state.onCollision, scriptName);
    }
    removePackageScriptCallbackRefs(object.onSpawned, scriptName);
    removePackageScriptCallbackRefs(object.onDestroyed, scriptName);
    removePackageScriptCallbackRefs(object.onPickedUp, scriptName);
    removePackageScriptCallbackRefs(object.onDropped, scriptName);
    removePackageScriptCallbackRefs(object.onThrown, scriptName);
    removePackageScriptCallbackRefs(object.onDamageDealt, scriptName);
    removePackageScriptCallbackRefs(object.onDamageReceived, scriptName);
    removePackageScriptCallbackRefs(object.onClanked, scriptName);
    removePackageScriptCallbackRefs(object.onReflected, scriptName);
    removePackageScriptCallbackRefs(object.onAbsorbed, scriptName);
    removePackageScriptCallbackRefs(object.onShieldBounced, scriptName);
    removePackageScriptCallbackRefs(object.onHitShield, scriptName);
    removePackageScriptCallbackRefs(object.onEnteredAir, scriptName);
    removePackageScriptCallbackRefs(object.onEnteredHitlag, scriptName);
    removePackageScriptCallbackRefs(object.onExitedHitlag, scriptName);
    removePackageScriptCallbackRefs(object.onAccessory, scriptName);
    removePackageScriptCallbackRefs(object.onTouched, scriptName);
    removePackageScriptCallbackRefs(object.onJumpedOn, scriptName);
    removePackageScriptCallbackRefs(object.onGrabDealt, scriptName);
    removePackageScriptCallbackRefs(object.onGrabbedForVictim, scriptName);
    removePackageScriptCallbackRefs(object.onInteraction, scriptName);
}

void remapObjectPackageScriptCallbackRefs(
    GameObjectDefinition& object,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    for (GameObjectStateDefinition& state : object.states) {
        remapPackageScriptCallbackRefs(state.onEnter, oldScriptName, newScriptName);
        remapPackageScriptCallbackRefs(state.onFrame, oldScriptName, newScriptName);
        remapPackageScriptCallbackRefs(state.onPhysics, oldScriptName, newScriptName);
        remapPackageScriptCallbackRefs(state.onCollision, oldScriptName, newScriptName);
    }
    remapPackageScriptCallbackRefs(object.onSpawned, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onDestroyed, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onPickedUp, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onDropped, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onThrown, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onDamageDealt, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onDamageReceived, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onClanked, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onReflected, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onAbsorbed, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onShieldBounced, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onHitShield, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onEnteredAir, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onEnteredHitlag, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onExitedHitlag, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onAccessory, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onTouched, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onJumpedOn, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onGrabDealt, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onGrabbedForVictim, oldScriptName, newScriptName);
    remapPackageScriptCallbackRefs(object.onInteraction, oldScriptName, newScriptName);
}

void removeObjectPackageScriptRefs(GameObjectDefinition& object, const std::string& scriptName) {
    removeLocalPackageScriptInstructionRefs(object.packageScripts, scriptName);
    removeObjectPackageScriptCallbackRefs(object, scriptName);
}

void remapObjectPackageScriptRefs(
    GameObjectDefinition& object,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    remapLocalPackageScriptInstructionRefs(object.packageScripts, oldScriptName, newScriptName);
    remapObjectPackageScriptCallbackRefs(object, oldScriptName, newScriptName);
}

void removeCrossObjectPackageScriptRefs(FighterPackage& package, const std::string& scriptName) {
    for (FighterDefinition& fighter : package.fighters) {
        for (PackageScript& script : fighter.packageScripts) {
            for (PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.op == PackageScriptOp::CallIndexedObjectScriptFromVar &&
                    instruction.text == scriptName)
                {
                    clearPackageVariableInstruction(instruction);
                }
            }
        }
    }
    for (GameObjectDefinition& object : package.objects) {
        for (PackageScript& script : object.packageScripts) {
            for (PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.op == PackageScriptOp::CallIndexedObjectScriptFromVar &&
                    instruction.text == scriptName)
                {
                    clearPackageVariableInstruction(instruction);
                }
            }
        }
    }
}

void remapCrossObjectPackageScriptRefs(
    FighterPackage& package,
    const std::string& oldScriptName,
    const std::string& newScriptName)
{
    for (FighterDefinition& fighter : package.fighters) {
        for (PackageScript& script : fighter.packageScripts) {
            for (PackageScriptInstruction& instruction : script.instructions) {
                if (instruction.op == PackageScriptOp::CallIndexedObjectScriptFromVar &&
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
                if (instruction.op == PackageScriptOp::CallIndexedObjectScriptFromVar &&
                    instruction.text == oldScriptName)
                {
                    instruction.text = newScriptName;
                }
            }
        }
    }
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

namespace {

bool validEditorAuthoredClip(
    FighterEditorSession& session,
    int clipIndex,
    FighterDefinition** fighter,
    AnimationClip** clip,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    if (clipIndex < 0 || clipIndex >= static_cast<int>(def.authoredClips.size())) {
        setEditorError(error, "editor authored clip index is invalid");
        return false;
    }
    if (fighter) {
        *fighter = &def;
    }
    if (clip) {
        *clip = &def.authoredClips[static_cast<size_t>(clipIndex)];
    }
    return true;
}

bool validEditorAuthoredTrack(
    FighterEditorSession& session,
    int clipIndex,
    int trackIndex,
    FighterDefinition** fighter,
    AnimationClip** clip,
    AnimationTrack** track,
    std::string* error)
{
    AnimationClip* targetClip = nullptr;
    FighterDefinition* def = nullptr;
    if (!validEditorAuthoredClip(session, clipIndex, &def, &targetClip, error)) {
        return false;
    }
    if (trackIndex < 0 || trackIndex >= static_cast<int>(targetClip->tracks.size())) {
        setEditorError(error, "editor authored track index is invalid");
        return false;
    }
    if (fighter) {
        *fighter = def;
    }
    if (clip) {
        *clip = targetClip;
    }
    if (track) {
        *track = &targetClip->tracks[static_cast<size_t>(trackIndex)];
    }
    return true;
}

int remapRemovedAuthoredBone(int bone, int removedIndex, int fallbackBone) {
    if (bone < 0) {
        return bone;
    }
    if (bone == removedIndex) {
        return fallbackBone;
    }
    return bone > removedIndex ? bone - 1 : bone;
}

void sortEditorAnimationKeys(std::vector<AnimationKey>& keys) {
    std::sort(keys.begin(), keys.end(), [](const AnimationKey& a, const AnimationKey& b) {
        return a.frame < b.frame;
    });
}

void sanitizeEditorAnimationTrackKeys(AnimationTrack& track, Fix frameCount) {
    sortEditorAnimationKeys(track.keys);
    Fix previousFrame = -1;
    bool hasPreviousFrame = false;
    track.keys.erase(
        std::remove_if(track.keys.begin(), track.keys.end(), [&](const AnimationKey& key) {
            const bool invalid = key.frame < 0 ||
                (frameCount > 0 && key.frame > frameCount) ||
                (hasPreviousFrame && key.frame <= previousFrame);
            if (!invalid) {
                previousFrame = key.frame;
                hasPreviousFrame = true;
            }
            return invalid;
        }),
        track.keys.end());
}

bool hasEditorAnimationKeyFrame(const std::vector<AnimationKey>& keys, Fix frame, int ignoredIndex = -1) {
    for (size_t i = 0; i < keys.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && keys[i].frame == frame) {
            return true;
        }
    }
    return false;
}

HsdMeshVertex* editorAuthoredMeshVertexAt(HsdFighterMesh& mesh, int vertexIndex) {
    int cursor = 0;
    for (HsdMeshBatch& batch : mesh.batches) {
        const int next = cursor + static_cast<int>(batch.vertices.size());
        if (vertexIndex >= cursor && vertexIndex < next) {
            return &batch.vertices[static_cast<size_t>(vertexIndex - cursor)];
        }
        cursor = next;
    }
    return nullptr;
}

float editorAuthoredMeshDistanceSquared(Vec3 a, Vec3 b) {
    const float dx = fxToFloat(a.x - b.x);
    const float dy = fxToFloat(a.y - b.y);
    const float dz = fxToFloat(a.z - b.z);
    return dx * dx + dy * dy + dz * dz;
}

std::vector<Vec3> editorAuthoredSkeletonBindPositions(const std::vector<AnimationJoint>& skeleton) {
    if (skeleton.empty()) {
        return {};
    }
    const AnimationPose pose = bindPose(skeleton);
    return jointWorldTranslations(skeleton, pose);
}

void normalizeEditorAuthoredMeshVertexInfluences(HsdMeshVertex& vertex, int fallbackJoint) {
    float sum = 0.0f;
    for (HsdMeshVertexInfluence& influence : vertex.influences) {
        if (influence.bone < 0 || influence.weight <= 0.0f) {
            influence = {};
            influence.bone = -1;
            continue;
        }
        sum += influence.weight;
    }
    if (sum <= 0.0f) {
        vertex.influences = {};
        vertex.influences[0] = {fallbackJoint, 1.0f};
        return;
    }
    for (HsdMeshVertexInfluence& influence : vertex.influences) {
        if (influence.bone >= 0 && influence.weight > 0.0f) {
            influence.weight /= sum;
        }
    }
}

void bindEditorAuthoredMeshVertexToJoint(HsdFighterMesh& mesh, int vertexIndex, int joint, int skeletonSize) {
    HsdMeshVertex* vertex = editorAuthoredMeshVertexAt(mesh, vertexIndex);
    if (!vertex) {
        return;
    }
    vertex->influences = {};
    vertex->influences[0] = {joint, 1.0f};
    for (HsdMeshBatch& batch : mesh.batches) {
        batch.parentBone = skeletonSize > 1 ? -1 : joint;
        batch.singleBindBone = skeletonSize > 1 ? -1 : joint;
        batch.hasEnvelopes = skeletonSize > 1;
    }
}

void blendEditorAuthoredMeshVertexTowardJoint(
    HsdFighterMesh& mesh,
    int vertexIndex,
    int joint,
    int skeletonSize,
    float amount)
{
    HsdMeshVertex* vertex = editorAuthoredMeshVertexAt(mesh, vertexIndex);
    if (!vertex) {
        return;
    }
    HsdMeshVertexInfluence* target = nullptr;
    HsdMeshVertexInfluence* empty = nullptr;
    for (HsdMeshVertexInfluence& influence : vertex->influences) {
        if (influence.bone == joint) {
            target = &influence;
        }
        if (!empty && (influence.bone < 0 || influence.weight <= 0.0f)) {
            empty = &influence;
        }
        if (influence.weight > 0.0f) {
            influence.weight *= (1.0f - amount);
        }
    }
    if (!target) {
        target = empty ? empty : &vertex->influences.back();
        target->bone = joint;
        target->weight = 0.0f;
    }
    target->weight += amount;
    normalizeEditorAuthoredMeshVertexInfluences(*vertex, joint);
    for (HsdMeshBatch& batch : mesh.batches) {
        batch.parentBone = skeletonSize > 1 ? -1 : joint;
        batch.singleBindBone = skeletonSize > 1 ? -1 : joint;
        batch.hasEnvelopes = skeletonSize > 1;
    }
}

void autoWeightEditorAuthoredMesh(HsdFighterMesh& mesh, const std::vector<AnimationJoint>& skeleton) {
    const std::vector<Vec3> joints = editorAuthoredSkeletonBindPositions(skeleton);
    if (joints.empty()) {
        return;
    }
    for (HsdMeshBatch& batch : mesh.batches) {
        batch.parentBone = joints.size() > 1 ? -1 : 0;
        batch.singleBindBone = joints.size() > 1 ? -1 : 0;
        batch.hasEnvelopes = joints.size() > 1;
        for (HsdMeshVertex& vertex : batch.vertices) {
            int nearest = 0;
            int second = -1;
            float nearestDist = editorAuthoredMeshDistanceSquared(vertex.position, joints.front());
            float secondDist = 0.0f;
            for (size_t jointIndex = 1; jointIndex < joints.size(); ++jointIndex) {
                const float dist = editorAuthoredMeshDistanceSquared(vertex.position, joints[jointIndex]);
                if (dist < nearestDist) {
                    second = nearest;
                    secondDist = nearestDist;
                    nearest = static_cast<int>(jointIndex);
                    nearestDist = dist;
                } else if (second < 0 || dist < secondDist) {
                    second = static_cast<int>(jointIndex);
                    secondDist = dist;
                }
            }
            vertex.influences = {};
            if (second < 0) {
                vertex.influences[0] = {nearest, 1.0f};
                continue;
            }
            const float nearestLength = std::sqrt(nearestDist);
            const float secondLength = std::sqrt(secondDist);
            const float totalLength = nearestLength + secondLength;
            const float nearestWeight = totalLength > 0.0001f
                ? std::clamp(secondLength / totalLength, 0.0f, 1.0f)
                : 1.0f;
            vertex.influences[0] = {nearest, nearestWeight};
            vertex.influences[1] = {second, 1.0f - nearestWeight};
        }
    }
}

} // namespace

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
    ensurePackageScriptGraphs(package);
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
    ensurePackageScriptGraphs(package);
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
    FighterPackage compiledPackage = session.package;
    if (!compileFighterPackageScriptGraphs(compiledPackage, error)) {
        session.lastMessage = error ? *error : "editor package graph compile failed";
        return false;
    }
    std::vector<uint8_t> bytes = writeFighterPackage(compiledPackage, error);
    if (bytes.empty()) {
        session.lastMessage = error ? *error : "editor package export failed";
        return false;
    }
    FighterPackageDescriptor descriptor;
    if (!describeFighterPackage(compiledPackage, descriptor, bytes, error)) {
        session.lastMessage = error ? *error : "editor package describe failed";
        return false;
    }
    snapshot.package = compiledPackage;
    snapshot.bytes = bytes;
    snapshot.descriptor = descriptor;
    session.package = std::move(compiledPackage);
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
    const bool wasDirty = session.dirty;
    FighterEditorPackageSnapshot snapshot;
    if (!exportFighterEditorSessionPackage(session, snapshot, error)) {
        return false;
    }
    FighterPackageDescriptor testDescriptor;
    if (!makePackageTestWorldFromBytes(world, snapshot.bytes, rootFighterDef, &testDescriptor, error)) {
        session.dirty = wasDirty;
        session.lastMessage = error ? *error : "editor package test world failed";
        return false;
    }
    if (descriptor) {
        *descriptor = testDescriptor;
    }
    session.lastDescriptor = testDescriptor;
    session.lastMessage = "OK";
    session.dirty = wasDirty;
    return true;
}

bool editorPackageFighterNameAvailable(const FighterPackage& package, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < package.fighters.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && package.fighters[i].name == name) {
            return false;
        }
    }
    return true;
}

std::string uniqueEditorPackageFighterName(const FighterPackage& package, const std::string& prefix) {
    if (editorPackageFighterNameAvailable(package, prefix)) {
        return prefix;
    }
    for (int index = 1; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        if (editorPackageFighterNameAvailable(package, candidate)) {
            return candidate;
        }
    }
    return prefix + "X";
}

bool addEditorSessionPackageFighter(
    FighterEditorSession& session,
    const FighterDefinition& fighter,
    const std::string& requestedName,
    int* addedFighterIndex,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition added = fighter;
    added.name = requestedName.empty()
        ? uniqueEditorPackageFighterName(session.package, added.name.empty() ? "Fighter" : added.name)
        : requestedName;
    if (!editorPackageFighterNameAvailable(session.package, added.name)) {
        setEditorError(error, "editor package fighter name is empty or already used");
        return false;
    }
    for (PackageScript& script : added.packageScripts) {
        if (script.graph.nodes.empty()) {
            script.graph = makePackageScriptControlFlowGraph(script);
        }
    }
    FighterPackage previous = session.package;
    session.package.fighters.push_back(std::move(added));
    collectEditorPackageAssets(session.package.fighters, session.package);
    session.selectedFighter = static_cast<int>(session.package.fighters.size()) - 1;
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedFighterIndex) {
        *addedFighterIndex = session.selectedFighter;
    }
    return true;
}

bool duplicateEditorSessionPackageFighter(
    FighterEditorSession& session,
    int fighterIndex,
    const std::string& requestedName,
    int* addedFighterIndex,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    if (fighterIndex < 0 || fighterIndex >= static_cast<int>(session.package.fighters.size())) {
        setEditorError(error, "editor package fighter index is invalid");
        return false;
    }
    FighterDefinition copy = session.package.fighters[static_cast<size_t>(fighterIndex)];
    const std::string name = requestedName.empty()
        ? uniqueEditorPackageFighterName(session.package, copy.name + "Copy")
        : requestedName;
    return addEditorSessionPackageFighter(session, copy, name, addedFighterIndex, error);
}

bool renameEditorSessionPackageFighter(
    FighterEditorSession& session,
    int fighterIndex,
    const std::string& newName,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    if (fighterIndex < 0 || fighterIndex >= static_cast<int>(session.package.fighters.size())) {
        setEditorError(error, "editor package fighter index is invalid");
        return false;
    }
    if (!editorPackageFighterNameAvailable(session.package, newName, fighterIndex)) {
        setEditorError(error, "editor package fighter name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    const std::string oldName = session.package.fighters[static_cast<size_t>(fighterIndex)].name;
    session.package.fighters[static_cast<size_t>(fighterIndex)].name = newName;
    remapPackageFighterTargetRefs(session.package, oldName, newName);
    session.selectedFighter = fighterIndex;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionPackageFighter(
    FighterEditorSession& session,
    int fighterIndex,
    const std::string& replacementFighterName,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    if (fighterIndex <= 0 || fighterIndex >= static_cast<int>(session.package.fighters.size())) {
        setEditorError(error, "editor package fighter remove target is invalid");
        return false;
    }
    const std::string removedName = session.package.fighters[static_cast<size_t>(fighterIndex)].name;
    const std::string replacement = replacementFighterName.empty()
        ? session.package.fighters.front().name
        : replacementFighterName;
    const auto replacementIt = std::find_if(
        session.package.fighters.begin(),
        session.package.fighters.end(),
        [&](const FighterDefinition& fighter) {
            return fighter.name == replacement;
        });
    if (replacement == removedName ||
        replacementIt == session.package.fighters.end() ||
        static_cast<int>(std::distance(session.package.fighters.begin(), replacementIt)) == fighterIndex)
    {
        setEditorError(error, "editor package fighter replacement is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    remapPackageFighterTargetRefs(session.package, removedName, replacement);
    session.package.fighters.erase(session.package.fighters.begin() + fighterIndex);
    session.selectedFighter = std::clamp(fighterIndex, 0, static_cast<int>(session.package.fighters.size()) - 1);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool createEditorSessionState(
    FighterEditorSession& session,
    const std::string& requestedName,
    int sourceStateIndex,
    int* createdStateIndex,
    std::string* error)
{
    return createEditorSessionPackageFighterState(session, 0, requestedName, sourceStateIndex, createdStateIndex, error);
}

bool createEditorSessionPackageFighterState(
    FighterEditorSession& session,
    int fighterIndex,
    const std::string& requestedName,
    int sourceStateIndex,
    int* createdStateIndex,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition* fighter = nullptr;
    if (!validSessionPackageFighter(session, fighterIndex, &fighter, error)) {
        return false;
    }
    FighterDefinition& def = *fighter;
    const std::string name = requestedName.empty()
        ? uniqueEditorStateName(def, "NewState")
        : requestedName;
    if (!editorFighterStateNameAvailable(def, name)) {
        setEditorError(error, "editor state name is empty or already used");
        return false;
    }
    const int selectedState = fighterIndex == session.selectedFighter
        ? session.selectedState
        : static_cast<int>(def.states.size()) - 1;
    const int insertIndex = std::clamp(selectedState + 1, 0, static_cast<int>(def.states.size()));
    def.states.insert(def.states.begin() + insertIndex, makeDefaultEditorState(def, name, sourceStateIndex));
    session.selectedFighter = fighterIndex;
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
    return duplicateEditorSessionPackageFighterState(session, 0, sourceStateIndex, createdStateIndex, error);
}

bool duplicateEditorSessionPackageFighterState(
    FighterEditorSession& session,
    int fighterIndex,
    int sourceStateIndex,
    int* createdStateIndex,
    std::string* error)
{
    FighterDefinition* fighter = nullptr;
    if (!validSessionPackageFighter(session, fighterIndex, &fighter, error)) {
        return false;
    }
    FighterDefinition& def = *fighter;
    if (def.states.empty()) {
        setEditorError(error, "editor cannot clone a state from an empty fighter");
        return false;
    }
    const int sourceIndex = sourceStateIndex >= 0
        ? sourceStateIndex
        : (fighterIndex == session.selectedFighter
            ? std::clamp(session.selectedState, 0, static_cast<int>(def.states.size()) - 1)
            : 0);
    if (sourceIndex < 0 || sourceIndex >= static_cast<int>(def.states.size())) {
        setEditorError(error, "editor clone source state is invalid");
        return false;
    }
    FighterState clone = def.states[static_cast<size_t>(sourceIndex)];
    clone.name = uniqueEditorStateName(def, clone.name + "Copy");
    const int insertIndex = std::clamp(sourceIndex + 1, 0, static_cast<int>(def.states.size()));
    def.states.insert(def.states.begin() + insertIndex, std::move(clone));
    session.selectedFighter = fighterIndex;
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
    return renameEditorSessionPackageFighterState(session, 0, stateIndex, newName, error);
}

bool renameEditorSessionPackageFighterState(
    FighterEditorSession& session,
    int fighterIndex,
    int stateIndex,
    const std::string& newName,
    std::string* error)
{
    FighterDefinition* fighter = nullptr;
    if (!validSessionPackageFighter(session, fighterIndex, &fighter, error)) {
        return false;
    }
    FighterDefinition& def = *fighter;
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
    session.selectedFighter = fighterIndex;
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
    return removeEditorSessionPackageFighterState(session, 0, stateIndex, replacementStateName, error);
}

bool removeEditorSessionPackageFighterState(
    FighterEditorSession& session,
    int fighterIndex,
    int stateIndex,
    const std::string& replacementStateName,
    std::string* error)
{
    FighterDefinition* fighter = nullptr;
    if (!validSessionPackageFighter(session, fighterIndex, &fighter, error)) {
        return false;
    }
    FighterDefinition& def = *fighter;
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
    session.selectedFighter = fighterIndex;
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

PackageScriptGraph makePackageScriptLinearGraph(const PackageScript& script) {
    PackageScriptGraph graph;
    graph.entryNode = 0;
    graph.nodes.push_back({0, PackageScriptGraphNodeKind::Entry, -1, {0, 0}, "Entry"});
    for (int instructionIndex = 0; instructionIndex < static_cast<int>(script.instructions.size()); ++instructionIndex) {
        const int nodeId = instructionIndex + 1;
        graph.nodes.push_back({
            nodeId,
            PackageScriptGraphNodeKind::Instruction,
            instructionIndex,
            {fx(3), fx(instructionIndex * 2)},
            {},
        });
        graph.links.push_back({
            instructionIndex == 0 ? graph.entryNode : instructionIndex,
            0,
            nodeId,
            0,
        });
    }
    return graph;
}

PackageScriptGraph makePackageScriptControlFlowGraph(const PackageScript& script) {
    PackageScriptGraph graph = makePackageScriptLinearGraph(script);
    int endNode = -1;
    auto targetNodeForInstruction = [&](int targetIndex) -> int {
        if (targetIndex >= 0 && targetIndex < static_cast<int>(script.instructions.size())) {
            return targetIndex + 1;
        }
        if (targetIndex == static_cast<int>(script.instructions.size())) {
            if (endNode < 0) {
                endNode = static_cast<int>(graph.nodes.size());
                graph.nodes.push_back({
                    endNode,
                    PackageScriptGraphNodeKind::Comment,
                    -1,
                    {fx(6), fx(static_cast<int>(script.instructions.size()) * 2)},
                    "End",
                });
            }
            return endNode;
        }
        return -1;
    };

    for (int instructionIndex = 0; instructionIndex < static_cast<int>(script.instructions.size()); ++instructionIndex) {
        const PackageScriptInstruction& instruction = script.instructions[static_cast<size_t>(instructionIndex)];
        int targetIndex = -1;
        if (packageInstructionIsSkipBranch(instruction.op)) {
            targetIndex = instructionIndex + 2;
        } else if (instruction.op == PackageScriptOp::JumpRelative) {
            targetIndex = instructionIndex + instruction.intValue;
        } else {
            continue;
        }

        const int targetNode = targetNodeForInstruction(targetIndex);
        if (targetNode >= 0) {
            graph.links.push_back({
                instructionIndex + 1,
                1,
                targetNode,
                0,
            });
        }
    }

    return graph;
}

bool compilePackageScriptGraph(PackageScript& script, std::string* error) {
    PackageScriptGraph& graph = script.graph;
    if (graph.nodes.empty()) {
        setEditorError(error, "editor package script graph is empty");
        return false;
    }
    PackageScriptGraphNode* entry = graphNodeById(graph, graph.entryNode);
    if (!entry || entry->kind != PackageScriptGraphNodeKind::Entry) {
        setEditorError(error, "editor package script graph entry is invalid");
        return false;
    }
    std::vector<PackageScriptInstruction> compiled;
    std::vector<int> compiledNodeIds;
    std::vector<int> seenNodes;
    std::vector<int> seenInstructions;
    int currentNode = graph.entryNode;
    seenNodes.push_back(currentNode);
    while (true) {
        int nextNode = -1;
        if (!nextGraphControlNode(graph, currentNode, nextNode, error)) {
            return false;
        }
        if (nextNode < 0) {
            break;
        }
        if (graphNodeSeen(seenNodes, nextNode)) {
            setEditorError(error, "editor package script graph has a control cycle");
            return false;
        }
        PackageScriptGraphNode* node = graphNodeById(graph, nextNode);
        if (!node) {
            setEditorError(error, "editor package script graph link target is invalid");
            return false;
        }
        seenNodes.push_back(nextNode);
        if (node->kind == PackageScriptGraphNodeKind::Instruction) {
            if (node->instructionIndex < 0 || node->instructionIndex >= static_cast<int>(script.instructions.size())) {
                setEditorError(error, "editor package script graph instruction is invalid");
                return false;
            }
            if (std::find(seenInstructions.begin(), seenInstructions.end(), node->instructionIndex) != seenInstructions.end()) {
                setEditorError(error, "editor package script graph instruction is duplicate");
                return false;
            }
            seenInstructions.push_back(node->instructionIndex);
            compiled.push_back(script.instructions[static_cast<size_t>(node->instructionIndex)]);
            compiledNodeIds.push_back(node->id);
        } else if (node->kind == PackageScriptGraphNodeKind::Entry) {
            setEditorError(error, "editor package script graph control target is invalid");
            return false;
        }
        currentNode = nextNode;
    }

    if (static_cast<int>(compiled.size()) != graphInstructionNodeCount(graph)) {
        setEditorError(error, "editor package script graph has disconnected instruction nodes");
        return false;
    }
    if (compiled.size() != script.instructions.size()) {
        setEditorError(error, "editor package script graph does not cover all bytecode instructions");
        return false;
    }

    const int instructionCount = static_cast<int>(compiled.size());
    for (int compiledIndex = 0; compiledIndex < instructionCount; ++compiledIndex) {
        PackageScriptInstruction& instruction = compiled[static_cast<size_t>(compiledIndex)];
        if (!packageInstructionIsGraphRetargetable(instruction.op)) {
            continue;
        }

        const int nodeId = compiledNodeIds[static_cast<size_t>(compiledIndex)];
        if (instruction.op == PackageScriptOp::JumpRelative) {
            bool found = false;
            int targetNode = -1;
            if (!graphSocketTarget(graph, nodeId, 1, targetNode, found, error)) {
                return false;
            }
            if (!found && !graphSocketTarget(graph, nodeId, 0, targetNode, found, error)) {
                return false;
            }
            int targetIndex = instructionCount;
            if (!resolveGraphTargetIndex(graph, compiledNodeIds, found ? targetNode : -1, instructionCount, targetIndex, error)) {
                return false;
            }
            instruction.intValue = targetIndex - compiledIndex;
            continue;
        }

        if (compiledIndex + 2 > instructionCount) {
            setEditorError(error, "editor package script graph branch target is invalid");
            return false;
        }

        bool foundFallthrough = false;
        int fallthroughNode = -1;
        if (!graphSocketTarget(graph, nodeId, 0, fallthroughNode, foundFallthrough, error)) {
            return false;
        }
        int fallthroughIndex = instructionCount;
        if (!resolveGraphTargetIndex(
                graph,
                compiledNodeIds,
                foundFallthrough ? fallthroughNode : -1,
                instructionCount,
                fallthroughIndex,
                error))
        {
            return false;
        }
        if (fallthroughIndex != compiledIndex + 1) {
            setEditorError(error, "editor package script graph branch fallthrough is invalid");
            return false;
        }

        bool foundTaken = false;
        int takenNode = -1;
        if (!graphSocketTarget(graph, nodeId, 1, takenNode, foundTaken, error)) {
            return false;
        }
        if (foundTaken) {
            int takenIndex = instructionCount;
            if (!resolveGraphTargetIndex(graph, compiledNodeIds, takenNode, instructionCount, takenIndex, error)) {
                return false;
            }
            if (takenIndex != compiledIndex + 2) {
                setEditorError(error, "editor package script graph branch taken target is not bytecode-representable");
                return false;
            }
        }
    }

    script.instructions = std::move(compiled);
    for (int compiledIndex = 0; compiledIndex < static_cast<int>(compiledNodeIds.size()); ++compiledIndex) {
        PackageScriptGraphNode* node = graphNodeById(graph, compiledNodeIds[static_cast<size_t>(compiledIndex)]);
        if (node) {
            node->instructionIndex = compiledIndex;
        }
    }
    return true;
}

bool compileFighterPackageScriptGraphs(FighterPackage& package, std::string* error) {
    for (FighterDefinition& fighter : package.fighters) {
        for (PackageScript& script : fighter.packageScripts) {
            if (!script.graph.nodes.empty() && !compilePackageScriptGraph(script, error)) {
                return false;
            }
        }
    }
    for (GameObjectDefinition& object : package.objects) {
        for (PackageScript& script : object.packageScripts) {
            if (!script.graph.nodes.empty() && !compilePackageScriptGraph(script, error)) {
                return false;
            }
        }
    }
    return true;
}

bool setEditorSessionPackageScriptGraph(
    FighterEditorSession& session,
    int scriptIndex,
    const PackageScriptGraph& graph,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionScript(session, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    script->graph = graph;
    session.selectedPackageScript = scriptIndex;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool addEditorSessionPackageScriptGraphNode(
    FighterEditorSession& session,
    int scriptIndex,
    const PackageScriptGraphNode& node,
    int* addedNodeId,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionScript(session, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    int nodeId = -1;
    if (!addGraphNode(script->graph, node, &nodeId, error)) {
        return false;
    }
    session.selectedPackageScript = scriptIndex;
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedNodeId) {
        *addedNodeId = nodeId;
    }
    return true;
}

bool setEditorSessionPackageScriptGraphNode(
    FighterEditorSession& session,
    int scriptIndex,
    int nodeId,
    const PackageScriptGraphNode& node,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionScript(session, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    if (!setGraphNode(script->graph, nodeId, node, error)) {
        return false;
    }
    session.selectedPackageScript = scriptIndex;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionPackageScriptGraphNode(
    FighterEditorSession& session,
    int scriptIndex,
    int nodeId,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionScript(session, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    if (!removeGraphNode(script->graph, nodeId, error)) {
        return false;
    }
    session.selectedPackageScript = scriptIndex;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool setEditorSessionPackageScriptGraphLink(
    FighterEditorSession& session,
    int scriptIndex,
    const PackageScriptGraphLink& link,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionScript(session, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    setGraphLink(script->graph, link);
    session.selectedPackageScript = scriptIndex;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionPackageScriptGraphLink(
    FighterEditorSession& session,
    int scriptIndex,
    int fromNode,
    int fromSocket,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionScript(session, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    if (!removeGraphLink(script->graph, fromNode, fromSocket, error)) {
        return false;
    }
    session.selectedPackageScript = scriptIndex;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool compileEditorSessionPackageScriptGraph(
    FighterEditorSession& session,
    int scriptIndex,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionScript(session, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    if (!compilePackageScriptGraph(*script, error)) {
        return false;
    }
    session.selectedPackageScript = scriptIndex;
    session.selectedPackageInstruction = 0;
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
    remapGraphAfterInstructionInsert(script->graph, index);
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
    remapGraphAfterInstructionRemove(script->graph, instructionIndex);
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
    remapGraphAfterInstructionSwap(script->graph, instructionIndex, targetIndex);
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

std::string uniqueEditorAuthoredClipName(const FighterDefinition& def, const std::string& prefix) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        if (editorAuthoredClipNameAvailable(def, candidate)) {
            return candidate;
        }
    }
    return prefix + "X";
}

std::string uniqueEditorAuthoredJointName(const FighterDefinition& def, const std::string& prefix) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        if (editorAuthoredJointNameAvailable(def, candidate)) {
            return candidate;
        }
    }
    return prefix + "X";
}

int uniqueEditorAuthoredClipActionIndex(const FighterDefinition& def) {
    for (int index = 0; index < 10000; ++index) {
        if (editorAuthoredClipActionIndexAvailable(def, index)) {
            return index;
        }
    }
    return static_cast<int>(def.authoredClips.size());
}

bool editorAuthoredClipNameAvailable(const FighterDefinition& def, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < def.authoredClips.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && def.authoredClips[i].name == name) {
            return false;
        }
    }
    return true;
}

bool editorAuthoredJointNameAvailable(const FighterDefinition& def, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < def.authoredSkeleton.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && def.authoredSkeleton[i].name == name) {
            return false;
        }
    }
    return true;
}

bool editorAuthoredClipActionIndexAvailable(const FighterDefinition& def, int actionIndex, int ignoredIndex) {
    if (actionIndex < 0) {
        return false;
    }
    for (size_t i = 0; i < def.authoredClips.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && def.authoredClips[i].actionIndex == actionIndex) {
            return false;
        }
    }
    return true;
}

HsdFighterMesh makeFighterEditorTriangleMesh() {
    HsdFighterMesh mesh;
    HsdMeshBatch batch;
    batch.parentBone = 0;
    batch.singleBindBone = 0;
    batch.materialColor = {160, 220, 255, 255};
    auto vertex = [](Vec3 position) {
        HsdMeshVertex out;
        out.position = position;
        out.normal = {0, 0, fx(1)};
        out.influences[0] = {0, 1.0f};
        return out;
    };
    batch.vertices = {
        vertex({fxFromFloat(-0.35f), fxFromFloat(0.2f), 0}),
        vertex({fxFromFloat(0.35f), fxFromFloat(0.2f), 0}),
        vertex({0, fxFromFloat(1.0f), 0}),
    };
    mesh.batches.push_back(std::move(batch));
    return mesh;
}

int editorAuthoredMeshVertexCount(const HsdFighterMesh& mesh) {
    int count = 0;
    for (const HsdMeshBatch& batch : mesh.batches) {
        count += static_cast<int>(batch.vertices.size());
    }
    return count;
}

bool setEditorSessionAuthoredEcb(
    FighterEditorSession& session,
    const FighterEcbDefinition& ecb,
    bool normalize,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    FighterDefinition& def = session.package.fighters.front();
    def.authoredEcb = ecb;
    if (normalize) {
        normalizeFighterEditorAuthoredEcb(def);
    }
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool addEditorSessionHurtbox(
    FighterEditorSession& session,
    const HurtboxDefinition& hurtbox,
    int insertIndex,
    int* addedHurtboxIndex,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    FighterDefinition& def = session.package.fighters.front();
    const int index = insertIndex < 0
        ? static_cast<int>(def.hurtboxes.size())
        : std::clamp(insertIndex, 0, static_cast<int>(def.hurtboxes.size()));
    def.hurtboxes.insert(def.hurtboxes.begin() + index, hurtbox);
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedHurtboxIndex) {
        *addedHurtboxIndex = index;
    }
    return true;
}

bool setEditorSessionHurtbox(
    FighterEditorSession& session,
    int hurtboxIndex,
    const HurtboxDefinition& hurtbox,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    if (hurtboxIndex < 0 || hurtboxIndex >= static_cast<int>(def.hurtboxes.size())) {
        setEditorError(error, "editor hurtbox index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    def.hurtboxes[static_cast<size_t>(hurtboxIndex)] = hurtbox;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionHurtbox(
    FighterEditorSession& session,
    int hurtboxIndex,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    if (hurtboxIndex < 0 || hurtboxIndex >= static_cast<int>(def.hurtboxes.size())) {
        setEditorError(error, "editor hurtbox index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    def.hurtboxes.erase(def.hurtboxes.begin() + hurtboxIndex);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool addEditorSessionAuthoredJoint(
    FighterEditorSession& session,
    const AnimationJoint& joint,
    int insertIndex,
    int* addedJointIndex,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    FighterDefinition& def = session.package.fighters.front();
    AnimationJoint added = joint;
    if (added.name.empty()) {
        added.name = uniqueEditorAuthoredJointName(def);
    }
    const int index = insertIndex < 0
        ? static_cast<int>(def.authoredSkeleton.size())
        : std::clamp(insertIndex, 0, static_cast<int>(def.authoredSkeleton.size()));
    for (AnimationJoint& existing : def.authoredSkeleton) {
        if (existing.parent >= index) {
            ++existing.parent;
        }
    }
    for (AnimationClip& clip : def.authoredClips) {
        for (AnimationTrack& track : clip.tracks) {
            if (track.joint >= index) {
                ++track.joint;
            }
        }
    }
    for (HsdMeshBatch& batch : def.authoredMesh.batches) {
        if (batch.parentBone >= index) {
            ++batch.parentBone;
        }
        if (batch.singleBindBone >= index) {
            ++batch.singleBindBone;
        }
        for (HsdMeshVertex& vertex : batch.vertices) {
            for (HsdMeshVertexInfluence& influence : vertex.influences) {
                if (influence.bone >= index) {
                    ++influence.bone;
                }
            }
        }
    }
    def.authoredSkeleton.insert(def.authoredSkeleton.begin() + index, std::move(added));
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedJointIndex) {
        *addedJointIndex = index;
    }
    return true;
}

bool setEditorSessionAuthoredJoint(
    FighterEditorSession& session,
    int jointIndex,
    const AnimationJoint& joint,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    if (jointIndex < 0 || jointIndex >= static_cast<int>(def.authoredSkeleton.size())) {
        setEditorError(error, "editor authored joint index is invalid");
        return false;
    }
    if (!editorAuthoredJointNameAvailable(def, joint.name, jointIndex)) {
        setEditorError(error, "editor authored joint name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    AnimationJoint edited = joint;
    if (jointIndex == 0) {
        edited.parent = -1;
    }
    def.authoredSkeleton[static_cast<size_t>(jointIndex)] = edited;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionAuthoredJoint(
    FighterEditorSession& session,
    int jointIndex,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    if (jointIndex < 0 || jointIndex >= static_cast<int>(def.authoredSkeleton.size())) {
        setEditorError(error, "editor authored joint index is invalid");
        return false;
    }
    if (def.authoredSkeleton.size() <= 1) {
        setEditorError(error, "editor cannot remove the only authored joint");
        return false;
    }
    FighterPackage previous = session.package;
    const int removedParent = def.authoredSkeleton[static_cast<size_t>(jointIndex)].parent;
    def.authoredSkeleton.erase(def.authoredSkeleton.begin() + jointIndex);
    const int fallbackBone = def.authoredSkeleton.empty()
        ? -1
        : std::clamp(removedParent, 0, static_cast<int>(def.authoredSkeleton.size()) - 1);
    for (AnimationJoint& joint : def.authoredSkeleton) {
        if (joint.parent == jointIndex) {
            joint.parent = fallbackBone;
        } else if (joint.parent > jointIndex) {
            --joint.parent;
        }
    }
    for (AnimationClip& clip : def.authoredClips) {
        for (AnimationTrack& track : clip.tracks) {
            track.joint = remapRemovedAuthoredBone(track.joint, jointIndex, fallbackBone);
        }
    }
    for (HsdMeshBatch& batch : def.authoredMesh.batches) {
        batch.parentBone = remapRemovedAuthoredBone(batch.parentBone, jointIndex, fallbackBone);
        batch.singleBindBone = remapRemovedAuthoredBone(batch.singleBindBone, jointIndex, fallbackBone);
        for (HsdMeshVertex& vertex : batch.vertices) {
            for (HsdMeshVertexInfluence& influence : vertex.influences) {
                influence.bone = remapRemovedAuthoredBone(influence.bone, jointIndex, fallbackBone);
            }
        }
    }
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool createEditorSessionAuthoredClip(
    FighterEditorSession& session,
    const std::string& requestedName,
    int sourceClipIndex,
    int* createdClipIndex,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    const std::string name = requestedName.empty() ? uniqueEditorAuthoredClipName(def) : requestedName;
    if (!editorAuthoredClipNameAvailable(def, name)) {
        setEditorError(error, "editor authored clip name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    ensureFighterEditorAuthoredRootJoint(def);
    AnimationClip clip;
    clip.name = name;
    clip.actionIndex = uniqueEditorAuthoredClipActionIndex(def);
    clip.frameCount = fx(60);
    if (sourceClipIndex >= 0 && sourceClipIndex < static_cast<int>(def.authoredClips.size())) {
        clip = def.authoredClips[static_cast<size_t>(sourceClipIndex)];
        clip.name = name;
        clip.actionIndex = uniqueEditorAuthoredClipActionIndex(def);
    }
    def.authoredClips.push_back(std::move(clip));
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (createdClipIndex) {
        *createdClipIndex = static_cast<int>(session.package.fighters.front().authoredClips.size()) - 1;
    }
    return true;
}

bool duplicateEditorSessionAuthoredClip(
    FighterEditorSession& session,
    int sourceClipIndex,
    int* createdClipIndex,
    std::string* error)
{
    return createEditorSessionAuthoredClip(session, {}, sourceClipIndex, createdClipIndex, error);
}

bool renameEditorSessionAuthoredClip(
    FighterEditorSession& session,
    int clipIndex,
    const std::string& newName,
    std::string* error)
{
    FighterDefinition* def = nullptr;
    AnimationClip* clip = nullptr;
    if (!validEditorAuthoredClip(session, clipIndex, &def, &clip, error)) {
        return false;
    }
    if (!editorAuthoredClipNameAvailable(*def, newName, clipIndex)) {
        setEditorError(error, "editor authored clip name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    const std::string oldName = clip->name;
    clip->name = newName;
    for (FighterState& state : def->states) {
        if (state.animation == oldName) {
            state.animation = newName;
        }
    }
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool setEditorSessionAuthoredClipProperties(
    FighterEditorSession& session,
    int clipIndex,
    int actionIndex,
    Fix frameCount,
    int defaultBlendFrames,
    uint32_t actionFlags,
    std::string* error)
{
    FighterDefinition* def = nullptr;
    AnimationClip* clip = nullptr;
    if (!validEditorAuthoredClip(session, clipIndex, &def, &clip, error)) {
        return false;
    }
    if (!editorAuthoredClipActionIndexAvailable(*def, actionIndex, clipIndex)) {
        setEditorError(error, "editor authored clip action index is invalid or already used");
        return false;
    }
    FighterPackage previous = session.package;
    const int oldActionIndex = clip->actionIndex;
    clip->actionIndex = actionIndex;
    clip->frameCount = frameCount;
    clip->defaultBlendFrames = defaultBlendFrames;
    clip->actionFlags = actionFlags;
    for (AnimationTrack& track : clip->tracks) {
        sanitizeEditorAnimationTrackKeys(track, frameCount);
    }
    for (FighterState& state : def->states) {
        if (state.animation == clip->name || state.animationActionIndex == oldActionIndex) {
            state.animationActionIndex = actionIndex;
        }
    }
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionAuthoredClip(
    FighterEditorSession& session,
    int clipIndex,
    const std::string& replacementClipName,
    std::string* error)
{
    FighterDefinition* def = nullptr;
    AnimationClip* clip = nullptr;
    if (!validEditorAuthoredClip(session, clipIndex, &def, &clip, error)) {
        return false;
    }
    if (def->authoredClips.size() <= 1) {
        setEditorError(error, "editor cannot remove the only authored clip");
        return false;
    }
    FighterPackage previous = session.package;
    const std::string removedName = clip->name;
    const int removedAction = clip->actionIndex;
    def->authoredClips.erase(def->authoredClips.begin() + clipIndex);
    const AnimationClip* replacement = nullptr;
    if (!replacementClipName.empty()) {
        const auto found = std::find_if(def->authoredClips.begin(), def->authoredClips.end(), [&](const AnimationClip& candidate) {
            return candidate.name == replacementClipName;
        });
        replacement = found == def->authoredClips.end() ? nullptr : &*found;
    }
    if (!replacement) {
        replacement = &def->authoredClips[static_cast<size_t>(std::clamp(clipIndex, 0, static_cast<int>(def->authoredClips.size()) - 1))];
    }
    for (FighterState& state : def->states) {
        if (state.animation == removedName || state.animationActionIndex == removedAction) {
            state.animation = replacement->name;
            state.animationActionIndex = replacement->actionIndex;
            state.animationLengthFrames = std::max(1, static_cast<int>(fxToFloat(replacement->frameCount)));
        }
    }
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool addEditorSessionAuthoredTrack(
    FighterEditorSession& session,
    int clipIndex,
    const AnimationTrack& track,
    int insertIndex,
    int* addedTrackIndex,
    std::string* error)
{
    AnimationClip* clip = nullptr;
    if (!validEditorAuthoredClip(session, clipIndex, nullptr, &clip, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    const int index = insertIndex < 0
        ? static_cast<int>(clip->tracks.size())
        : std::clamp(insertIndex, 0, static_cast<int>(clip->tracks.size()));
    clip->tracks.insert(clip->tracks.begin() + index, track);
    sanitizeEditorAnimationTrackKeys(clip->tracks[static_cast<size_t>(index)], clip->frameCount);
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedTrackIndex) {
        *addedTrackIndex = index;
    }
    return true;
}

bool setEditorSessionAuthoredTrack(
    FighterEditorSession& session,
    int clipIndex,
    int trackIndex,
    const AnimationTrack& track,
    std::string* error)
{
    AnimationTrack* target = nullptr;
    if (!validEditorAuthoredTrack(session, clipIndex, trackIndex, nullptr, nullptr, &target, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    *target = track;
    sanitizeEditorAnimationTrackKeys(*target, session.package.fighters.front().authoredClips[static_cast<size_t>(clipIndex)].frameCount);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionAuthoredTrack(
    FighterEditorSession& session,
    int clipIndex,
    int trackIndex,
    std::string* error)
{
    AnimationClip* clip = nullptr;
    if (!validEditorAuthoredTrack(session, clipIndex, trackIndex, nullptr, &clip, nullptr, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    clip->tracks.erase(clip->tracks.begin() + trackIndex);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool addEditorSessionAuthoredKey(
    FighterEditorSession& session,
    int clipIndex,
    int trackIndex,
    const AnimationKey& key,
    int* addedKeyIndex,
    std::string* error)
{
    AnimationTrack* track = nullptr;
    if (!validEditorAuthoredTrack(session, clipIndex, trackIndex, nullptr, nullptr, &track, error)) {
        return false;
    }
    const AnimationClip& clip = session.package.fighters.front().authoredClips[static_cast<size_t>(clipIndex)];
    if (key.frame < 0 || (clip.frameCount > 0 && key.frame > clip.frameCount)) {
        setEditorError(error, "editor authored key frame is invalid");
        return false;
    }
    if (hasEditorAnimationKeyFrame(track->keys, key.frame)) {
        setEditorError(error, "editor authored key frame is already used");
        return false;
    }
    FighterPackage previous = session.package;
    track->keys.push_back(key);
    sortEditorAnimationKeys(track->keys);
    int addedIndex = 0;
    for (size_t i = 0; i < track->keys.size(); ++i) {
        if (track->keys[i].frame == key.frame) {
            addedIndex = static_cast<int>(i);
            break;
        }
    }
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedKeyIndex) {
        *addedKeyIndex = addedIndex;
    }
    return true;
}

bool setEditorSessionAuthoredKey(
    FighterEditorSession& session,
    int clipIndex,
    int trackIndex,
    int keyIndex,
    const AnimationKey& key,
    std::string* error)
{
    AnimationTrack* track = nullptr;
    if (!validEditorAuthoredTrack(session, clipIndex, trackIndex, nullptr, nullptr, &track, error)) {
        return false;
    }
    if (keyIndex < 0 || keyIndex >= static_cast<int>(track->keys.size())) {
        setEditorError(error, "editor authored key index is invalid");
        return false;
    }
    const AnimationClip& clip = session.package.fighters.front().authoredClips[static_cast<size_t>(clipIndex)];
    if (key.frame < 0 || (clip.frameCount > 0 && key.frame > clip.frameCount)) {
        setEditorError(error, "editor authored key frame is invalid");
        return false;
    }
    if (hasEditorAnimationKeyFrame(track->keys, key.frame, keyIndex)) {
        setEditorError(error, "editor authored key frame is already used");
        return false;
    }
    FighterPackage previous = session.package;
    track->keys[static_cast<size_t>(keyIndex)] = key;
    sortEditorAnimationKeys(track->keys);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionAuthoredKey(
    FighterEditorSession& session,
    int clipIndex,
    int trackIndex,
    int keyIndex,
    std::string* error)
{
    AnimationTrack* track = nullptr;
    if (!validEditorAuthoredTrack(session, clipIndex, trackIndex, nullptr, nullptr, &track, error)) {
        return false;
    }
    if (keyIndex < 0 || keyIndex >= static_cast<int>(track->keys.size())) {
        setEditorError(error, "editor authored key index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    track->keys.erase(track->keys.begin() + keyIndex);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool setEditorSessionAuthoredMesh(
    FighterEditorSession& session,
    const HsdFighterMesh& mesh,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    session.package.fighters.front().authoredMesh = mesh;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool scaleEditorSessionAuthoredMesh(
    FighterEditorSession& session,
    Fix scaleX,
    Fix scaleY,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    for (HsdMeshBatch& batch : session.package.fighters.front().authoredMesh.batches) {
        for (HsdMeshVertex& vertex : batch.vertices) {
            vertex.position.x = fxMul(vertex.position.x, scaleX);
            vertex.position.y = fxMul(vertex.position.y, scaleY);
        }
    }
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool nudgeEditorSessionAuthoredMeshVertex(
    FighterEditorSession& session,
    int vertexIndex,
    Vec3 delta,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    HsdMeshVertex* vertex = editorAuthoredMeshVertexAt(session.package.fighters.front().authoredMesh, vertexIndex);
    if (!vertex) {
        setEditorError(error, "editor authored mesh vertex index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    vertex->position.x += delta.x;
    vertex->position.y += delta.y;
    vertex->position.z += delta.z;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool bindEditorSessionAuthoredMeshToJoint(
    FighterEditorSession& session,
    int jointIndex,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    if (jointIndex < 0 || jointIndex >= static_cast<int>(def.authoredSkeleton.size())) {
        setEditorError(error, "editor authored mesh joint index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    for (HsdMeshBatch& batch : def.authoredMesh.batches) {
        batch.parentBone = jointIndex;
        batch.singleBindBone = jointIndex;
        batch.hasEnvelopes = false;
        for (HsdMeshVertex& vertex : batch.vertices) {
            vertex.influences = {};
            vertex.influences[0] = {jointIndex, 1.0f};
        }
    }
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool bindEditorSessionAuthoredMeshVertexToJoint(
    FighterEditorSession& session,
    int vertexIndex,
    int jointIndex,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    if (jointIndex < 0 || jointIndex >= static_cast<int>(def.authoredSkeleton.size())) {
        setEditorError(error, "editor authored mesh joint index is invalid");
        return false;
    }
    if (vertexIndex < 0 || vertexIndex >= editorAuthoredMeshVertexCount(def.authoredMesh)) {
        setEditorError(error, "editor authored mesh vertex index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    bindEditorAuthoredMeshVertexToJoint(def.authoredMesh, vertexIndex, jointIndex, static_cast<int>(def.authoredSkeleton.size()));
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool blendEditorSessionAuthoredMeshVertexTowardJoint(
    FighterEditorSession& session,
    int vertexIndex,
    int jointIndex,
    float amount,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    if (jointIndex < 0 || jointIndex >= static_cast<int>(def.authoredSkeleton.size())) {
        setEditorError(error, "editor authored mesh joint index is invalid");
        return false;
    }
    if (vertexIndex < 0 || vertexIndex >= editorAuthoredMeshVertexCount(def.authoredMesh)) {
        setEditorError(error, "editor authored mesh vertex index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    blendEditorAuthoredMeshVertexTowardJoint(
        def.authoredMesh,
        vertexIndex,
        jointIndex,
        static_cast<int>(def.authoredSkeleton.size()),
        std::clamp(amount, 0.0f, 1.0f));
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool autoWeightEditorSessionAuthoredMeshToSkeleton(
    FighterEditorSession& session,
    std::string* error)
{
    if (!validRootFighter(session, error)) {
        return false;
    }
    FighterDefinition& def = session.package.fighters.front();
    if (def.authoredSkeleton.empty()) {
        setEditorError(error, "editor authored skeleton is empty");
        return false;
    }
    FighterPackage previous = session.package;
    autoWeightEditorAuthoredMesh(def.authoredMesh, def.authoredSkeleton);
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

std::string uniqueEditorObjectPackageVariableName(const GameObjectDefinition& object, const std::string& prefix) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        if (editorObjectPackageVariableNameAvailable(object, candidate)) {
            return candidate;
        }
    }
    return prefix + "X";
}

std::string uniqueEditorObjectPackageScriptName(const GameObjectDefinition& object, const std::string& prefix) {
    for (int index = 0; index < 10000; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        if (editorObjectPackageScriptNameAvailable(object, candidate)) {
            return candidate;
        }
    }
    return prefix + "X";
}

bool editorObjectPackageVariableNameAvailable(const GameObjectDefinition& object, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < object.packageVariables.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && object.packageVariables[i].name == name) {
            return false;
        }
    }
    return true;
}

bool editorObjectPackageScriptNameAvailable(const GameObjectDefinition& object, const std::string& name, int ignoredIndex) {
    if (name.empty()) {
        return false;
    }
    for (size_t i = 0; i < object.packageScripts.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex && object.packageScripts[i].name == name) {
            return false;
        }
    }
    return true;
}

bool addEditorSessionObjectPackageVariable(
    FighterEditorSession& session,
    int objectIndex,
    const std::string& requestedName,
    int32_t initialValue,
    int* addedVariableIndex,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    const std::string name = requestedName.empty() ? uniqueEditorObjectPackageVariableName(*object) : requestedName;
    if (!editorObjectPackageVariableNameAvailable(*object, name)) {
        setEditorError(error, "editor object package variable name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    object->packageVariables.push_back({name, initialValue});
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedVariableIndex) {
        *addedVariableIndex = static_cast<int>(session.package.objects[static_cast<size_t>(objectIndex)].packageVariables.size()) - 1;
    }
    return true;
}

bool renameEditorSessionObjectPackageVariable(
    FighterEditorSession& session,
    int objectIndex,
    int variableIndex,
    const std::string& newName,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    if (variableIndex < 0 || variableIndex >= static_cast<int>(object->packageVariables.size())) {
        setEditorError(error, "editor object package variable index is invalid");
        return false;
    }
    if (!editorObjectPackageVariableNameAvailable(*object, newName, variableIndex)) {
        setEditorError(error, "editor object package variable name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    object->packageVariables[static_cast<size_t>(variableIndex)].name = newName;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionObjectPackageVariable(
    FighterEditorSession& session,
    int objectIndex,
    int variableIndex,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    if (variableIndex < 0 || variableIndex >= static_cast<int>(object->packageVariables.size())) {
        setEditorError(error, "editor object package variable index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    object->packageVariables.erase(object->packageVariables.begin() + variableIndex);
    const int variableCountAfterRemove = static_cast<int>(object->packageVariables.size());
    remapRemovedPackageVariable(object->packageScripts, variableIndex, variableCountAfterRemove);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool addEditorSessionObjectPackageScript(
    FighterEditorSession& session,
    int objectIndex,
    const std::string& requestedName,
    int instructionBudget,
    int* addedScriptIndex,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    const std::string name = requestedName.empty() ? uniqueEditorObjectPackageScriptName(*object) : requestedName;
    if (!editorObjectPackageScriptNameAvailable(*object, name)) {
        setEditorError(error, "editor object package script name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    PackageScript script;
    script.name = name;
    script.instructionBudget = instructionBudget;
    object->packageScripts.push_back(std::move(script));
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedScriptIndex) {
        *addedScriptIndex = static_cast<int>(session.package.objects[static_cast<size_t>(objectIndex)].packageScripts.size()) - 1;
    }
    return true;
}

bool duplicateEditorSessionObjectPackageScript(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int* addedScriptIndex,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    PackageScript* script = nullptr;
    if (!validSessionObjectScript(session, objectIndex, scriptIndex, &object, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    PackageScript clone = *script;
    clone.name = uniqueEditorObjectPackageScriptName(*object, clone.name + "Copy");
    object->packageScripts.push_back(std::move(clone));
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedScriptIndex) {
        *addedScriptIndex = static_cast<int>(session.package.objects[static_cast<size_t>(objectIndex)].packageScripts.size()) - 1;
    }
    return true;
}

bool renameEditorSessionObjectPackageScript(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    const std::string& newName,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    PackageScript* script = nullptr;
    if (!validSessionObjectScript(session, objectIndex, scriptIndex, &object, &script, error)) {
        return false;
    }
    if (!editorObjectPackageScriptNameAvailable(*object, newName, scriptIndex)) {
        setEditorError(error, "editor object package script name is empty or already used");
        return false;
    }
    FighterPackage previous = session.package;
    const std::string oldName = script->name;
    script->name = newName;
    remapObjectPackageScriptRefs(*object, oldName, newName);
    remapCrossObjectPackageScriptRefs(session.package, oldName, newName);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionObjectPackageScript(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    PackageScript* script = nullptr;
    if (!validSessionObjectScript(session, objectIndex, scriptIndex, &object, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    const std::string removed = script->name;
    object->packageScripts.erase(object->packageScripts.begin() + scriptIndex);
    removeObjectPackageScriptRefs(*object, removed);
    removeCrossObjectPackageScriptRefs(session.package, removed);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool setEditorSessionObjectPackageScriptBudget(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int instructionBudget,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionObjectScript(session, objectIndex, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    script->instructionBudget = instructionBudget;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool setEditorSessionObjectPackageScriptGraph(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    const PackageScriptGraph& graph,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionObjectScript(session, objectIndex, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    script->graph = graph;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool addEditorSessionObjectPackageScriptGraphNode(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    const PackageScriptGraphNode& node,
    int* addedNodeId,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionObjectScript(session, objectIndex, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    int nodeId = -1;
    if (!addGraphNode(script->graph, node, &nodeId, error)) {
        return false;
    }
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedNodeId) {
        *addedNodeId = nodeId;
    }
    return true;
}

bool setEditorSessionObjectPackageScriptGraphNode(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int nodeId,
    const PackageScriptGraphNode& node,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionObjectScript(session, objectIndex, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    if (!setGraphNode(script->graph, nodeId, node, error)) {
        return false;
    }
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionObjectPackageScriptGraphNode(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int nodeId,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionObjectScript(session, objectIndex, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    if (!removeGraphNode(script->graph, nodeId, error)) {
        return false;
    }
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool setEditorSessionObjectPackageScriptGraphLink(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    const PackageScriptGraphLink& link,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionObjectScript(session, objectIndex, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    setGraphLink(script->graph, link);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionObjectPackageScriptGraphLink(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int fromNode,
    int fromSocket,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionObjectScript(session, objectIndex, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    if (!removeGraphLink(script->graph, fromNode, fromSocket, error)) {
        return false;
    }
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool compileEditorSessionObjectPackageScriptGraph(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionObjectScript(session, objectIndex, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    if (!compilePackageScriptGraph(*script, error)) {
        return false;
    }
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool addEditorSessionObjectPackageInstruction(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    const PackageScriptInstruction& instruction,
    int insertIndex,
    int* addedInstructionIndex,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionObjectScript(session, objectIndex, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    FighterPackage previous = session.package;
    const int index = insertIndex < 0
        ? static_cast<int>(script->instructions.size())
        : std::clamp(insertIndex, 0, static_cast<int>(script->instructions.size()));
    script->instructions.insert(script->instructions.begin() + index, instruction);
    remapGraphAfterInstructionInsert(script->graph, index);
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (addedInstructionIndex) {
        *addedInstructionIndex = index;
    }
    return true;
}

bool setEditorSessionObjectPackageInstruction(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int instructionIndex,
    const PackageScriptInstruction& instruction,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionObjectScript(session, objectIndex, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    if (instructionIndex < 0 || instructionIndex >= static_cast<int>(script->instructions.size())) {
        setEditorError(error, "editor object package instruction index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    script->instructions[static_cast<size_t>(instructionIndex)] = instruction;
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool removeEditorSessionObjectPackageInstruction(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int instructionIndex,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionObjectScript(session, objectIndex, scriptIndex, nullptr, &script, error)) {
        return false;
    }
    if (instructionIndex < 0 || instructionIndex >= static_cast<int>(script->instructions.size())) {
        setEditorError(error, "editor object package instruction index is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    script->instructions.erase(script->instructions.begin() + instructionIndex);
    remapGraphAfterInstructionRemove(script->graph, instructionIndex);
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool moveEditorSessionObjectPackageInstruction(
    FighterEditorSession& session,
    int objectIndex,
    int scriptIndex,
    int instructionIndex,
    int delta,
    int* movedInstructionIndex,
    std::string* error)
{
    PackageScript* script = nullptr;
    if (!validSessionObjectScript(session, objectIndex, scriptIndex, nullptr, &script, error)) {
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
        setEditorError(error, "editor object package instruction move is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    std::swap(script->instructions[static_cast<size_t>(instructionIndex)], script->instructions[static_cast<size_t>(targetIndex)]);
    remapGraphAfterInstructionSwap(script->graph, instructionIndex, targetIndex);
    if (!validateEditorSessionAfterMutation(session, std::move(previous), error)) {
        return false;
    }
    if (movedInstructionIndex) {
        *movedInstructionIndex = targetIndex;
    }
    return true;
}

bool bindEditorSessionObjectPackageScriptStateCallback(
    FighterEditorSession& session,
    int objectIndex,
    int stateIndex,
    FighterEditorObjectStateCallbackSlot slot,
    const std::string& scriptName,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    GameObjectStateDefinition* state = nullptr;
    if (!validSessionObjectState(session, objectIndex, stateIndex, &object, &state, error)) {
        return false;
    }
    const bool scriptExists = std::any_of(object->packageScripts.begin(), object->packageScripts.end(), [&](const PackageScript& script) {
        return script.name == scriptName;
    });
    if (!scriptExists) {
        setEditorError(error, "editor object package script callback target is invalid");
        return false;
    }
    std::vector<FunctionCall>* calls = objectStateCallbacks(*state, slot);
    if (!calls) {
        setEditorError(error, "editor object state callback slot is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    const std::string callback = "script:" + scriptName;
    if (!hasFunctionCall(*calls, callback)) {
        calls->push_back({callback});
    }
    return validateEditorSessionAfterMutation(session, std::move(previous), error);
}

bool bindEditorSessionObjectPackageScriptEventCallback(
    FighterEditorSession& session,
    int objectIndex,
    FighterEditorObjectEventCallbackSlot slot,
    const std::string& scriptName,
    std::string* error)
{
    GameObjectDefinition* object = nullptr;
    if (!validSessionObject(session, objectIndex, &object, error)) {
        return false;
    }
    const bool scriptExists = std::any_of(object->packageScripts.begin(), object->packageScripts.end(), [&](const PackageScript& script) {
        return script.name == scriptName;
    });
    if (!scriptExists) {
        setEditorError(error, "editor object package script callback target is invalid");
        return false;
    }
    std::vector<FunctionCall>* calls = objectEventCallbacks(*object, slot);
    if (!calls) {
        setEditorError(error, "editor object event callback slot is invalid");
        return false;
    }
    FighterPackage previous = session.package;
    const std::string callback = "script:" + scriptName;
    if (!hasFunctionCall(*calls, callback)) {
        calls->push_back({callback});
    }
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
