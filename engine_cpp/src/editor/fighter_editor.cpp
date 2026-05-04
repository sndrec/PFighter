#include "editor/fighter_editor.hpp"

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
    if (requestedName.empty()) {
        setEditorError(error, "editor object name is invalid");
        return false;
    }
    const bool exists = std::any_of(session.package.objects.begin(), session.package.objects.end(), [&](const GameObjectDefinition& object) {
        return object.name == requestedName;
    });
    if (exists) {
        setEditorError(error, "editor object name is already used");
        return false;
    }
    session.package.objects.push_back(makeFighterEditorObjectDefinition(requestedName, kind));
    if (objectIndex) {
        *objectIndex = static_cast<int>(session.package.objects.size()) - 1;
    }
    session.dirty = true;
    return true;
}

} // namespace pf
