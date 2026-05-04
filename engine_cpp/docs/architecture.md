# PFighter C++ Engine Architecture

This folder is the new raylib/C++ engine. The Godot project remains reference material.

The core rule is that gameplay simulation does not depend on rendering. `pfighter_core` owns fighter data, fixed-step input, state transitions, action/subaction unfolding, hitboxes, hurtboxes, skeletal pose data, and rollback snapshots.

Movement and collision should track Melee, not the Godot prototype. The Godot project is useful as conceptual reference for authoring modular fighters; Melee common action code is the reference for behavior.

## First slice

- `FighterDefinition` mirrors the Godot resource idea: properties, shield data, hurtboxes, and a list of named states.
- `FighterState` mirrors Godot states: animation name, animation finish transition, interrupts, and an `action` array.
- States run named reusable callbacks through the built-in function registry in `src/core/state_functions.cpp`. This is the C++ equivalent of Godot resources or Melee `MoveLogic` callback slots.
- `Subaction` preserves the current timer/action model: synchronous timers, asynchronous timers, loop markers, hitbox creation, hitbox clearing, and interrupt timing.
- Hurtbox state can also be driven by subactions, so invulnerability/intangibility windows live in fighter action data and are captured by rollback snapshots.
- `FighterRuntime` stores the live gameplay state, including current bone poses. Hitboxes and hurtboxes resolve against those poses so animation-relevant collision is part of the rollback state.
- Ground movement keeps a Melee-style `gr_vel` scalar in rollback state, queues ground acceleration callbacks, applies floor friction multipliers, then projects the resulting velocity along the current floor normal.
- Stick tilt timers are stored in rollback state and use the same `PlCo.dat` thresholds/windows that drive Melee dash, fastfall, and future smash/tap-jump checks.
- Dash, dash reverse, run, turn, buffered turn-to-dash, turn-run, run-brake, walk, jump, and fastfall callbacks are intentionally structured around the matching Melee common-action files so individual states can call reusable physics functions rather than hardcoding behavior inside the state table.
- RunDirect is represented as its own data state after TurnRun, preserving Melee's `PlCo.dat` x430 lockout before normal run turn/brake checks resume.
- Full hop, short hop, tap jump, aerial jump, fastfall, aerial drift, squat, platform pass-through, air dodge, wavedash landing, fall special, and wall jump use the same Mario/`PlCo.dat` movement constants and input-window ideas as Melee.
- GuardOn, Guard, GuardSetOff, GuardOff, and the first shield-break states are data states using reusable guard callbacks. Shield health, minimum hold time, drain, shield damage, setoff, defender pushback, attacker shield pushback, and regen come from the same `PlCo.dat` shield fields used by Melee and are included in rollback snapshots.
- EscapeN/spotdodge is a data state using `PlCo.dat` stick thresholds and hurtbox-state subactions for its intangibility window.
- Stage collision is segment-based and uses a Melee-style ECB diamond for floor sweeps, slope snapping, platform drop-through, ledge snap checks, run-off, and teeter transitions.
- Stage collision segments carry Melee-style floor/ceiling/right-wall/left-wall kinds. Authored segments use their explicit kind, while dynamic segments recompute kind from their current angle with Melee's tan30/tan60 thresholds.
- Ledge catch/wait are normal data states that call the reusable `maintain_ledge` callback, matching Melee's common cliff flow where collision detects the ledge and the cliff state owns the snap.
- The prototype fighter's baseline movement attributes are seeded from vanilla Mario's `PlMr.dat`, and common movement thresholds/cooldowns are seeded from vanilla `PlCo.dat`, both exported through HSDLib.
- `Pl*AJ.dat` action animation files are treated as concatenated HSD DAT chunks. Fighter action-table animation offsets point to those mini-DAT chunk starts, so the exporter slices each chunk and imports its real `HSD_FigaTree` instead of extracting bespoke root-motion data.
- HSD fighter assets are an importer/tool concern. The package converter can take a fighter DAT plus costume DAT, use the exporter as temporary scratch, and write a native `.pfpkg` that stores authored skeletons, clips, mesh/material/texture data, fighter bone lookup IDs, hurtboxes, model-part visibility, shield pose, ECB source bones, attributes, and converted action subactions directly.
- The training world loads generated native `.pfpkg` roster packages. Runtime pose sampling, model visibility, joint positions, and evaluated hurtbox capsules read native authored package data rather than interpreting HSD/DAT structures.
- `WorldSnapshot` is the rollback boundary. It captures integer gameplay state, input buffers, active hitboxes, and bone poses.

## Melee DAT Reference

HSDLib is available as a reference/import path for calibration data, not as a runtime dependency for the C++ simulation. Build it offline with:

```powershell
$env:DOTNET_CLI_HOME=(Resolve-Path '.').Path
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1'
$env:DOTNET_CLI_TELEMETRY_OPTOUT='1'
dotnet restore HSDLib-master\HSDRaw\HSDRaw.csproj --ignore-failed-sources /p:NetStandardImplicitPackageVersion=2.0.0
dotnet build HSDLib-master\HSDRaw\HSDRaw.csproj --no-restore /p:NetStandardImplicitPackageVersion=2.0.0
```

The preferred one-time fighter import writes a native authored package directly:

```powershell
engine_cpp\build-raylib\pfighter_package_converter.exe --fighter-dat vanillaroot\files\PlMr.dat --costume-dat vanillaroot\files\PlMrNr.dat --name Mario --out engine_cpp\data\packages\mario_native.pfpkg
```

The lower-level exporter can still write importer scratch assets for tooling/debugging:

```powershell
dotnet run --project engine_cpp\tools\hsd_exporter\PFighter.HsdExporter.csproj -- --asset-bin-out engine_cpp\data\fighters\mario_hsd.pfighter.bin vanillaroot\files\PlMr.dat vanillaroot\files\PlMrNr.dat
```

To export Melee Battlefield collision:

```powershell
dotnet run --project engine_cpp\tools\hsd_exporter\PFighter.HsdExporter.csproj -- --stage-bin-out engine_cpp\data\stages\battlefield_melee.pstage.bin vanillaroot\files\GrNBa.dat
```

## Custom Code

Built-in callbacks are named by data, so the same state format can later bind to custom native modules or a scripting VM. The likely long-term design is:

- engine-provided callbacks for Melee common behavior;
- optional fighter-provided callbacks for custom specials and unusual mechanics;
- action/subaction data for timing, hitboxes, flags, effects, and transitions;
- strict deterministic rules for anything that can run during rollback.

C# is pleasant for tooling, but for rollback gameplay code we should be careful. Native C++ modules or a small deterministic scripting language are safer first choices unless we sandbox runtime allocation, timing, and floating-point behavior tightly.

## Near-term next steps

1. Bind imported animation clips to fighter runtime states and replace the temporary procedural bone posing path.
2. Add editor-grade loader/saver support for the broader fighter and stage definitions.
3. Continue the Melee movement lab: pin moonwalk-sensitive dash/run/turn timings, deepen powershield/reflect behavior, add jump/aerial edge cases, and move ledge geometry toward the decomp's cliff checks.
4. Replace the starter raylib overlay with Dear ImGui editing controls.
5. Add a GGPO-style session layer on top of `WorldSnapshot`.
