# Native Package Transition Inventory

This inventory tracks the current gaps between imported Melee fighter data and
the goal of self-contained PFighter-native fighter packages. Import/conversion
code may continue to understand Melee DAT/HSD data, but runtime gameplay and
package load/save should operate on native authored data only.

## Current Data Boundary

- `engine_cpp/tools/hsd_exporter/Program.cs` is the only source path that parses
  HSDLib objects directly. It emits PFighter binary blobs with magic `PFHA`.
- `engine_cpp/src/core/animation_asset.*` reads those `PFHA` blobs into
  `HsdFighterAnimationAsset`. Despite the name, this is no longer raw DAT data;
  however it is still a separate imported asset type and it still lives in core.
  Long term, this loader should sit behind importer/converter tooling rather
  than normal gameplay construction.
- `engine_cpp/src/core/hsd_action_import.hpp` is the explicit import-facing API
  for converting HSD action scripts into native `Subaction` arrays. The general
  `core/action.hpp`/`action.cpp` pair now exposes only native action unfolding.
- `engine_cpp/src/core/melee_fighter_converter.hpp` is the explicit API used by
  `pfighter_package_converter.exe` for one-time Melee roster conversion. Normal
  simulation consumers no longer include these conversion entry points through
  `simulation.hpp`.
- `engine_cpp/src/core/melee_fighter_converter.cpp` owns the current PFHA/HSD
  import-to-native conversion path, including HSD action script decoding into
  native subactions. Runtime simulation no longer includes the HSD action import
  API.
- `FighterDefinition` already has native editable fields for properties, shield,
  ECB, authored skeleton, authored clips, authored mesh, package scripts,
  authored hurtboxes, states, interrupts, callbacks, and subactions.
- `FighterPackage` no longer has an in-memory imported asset pool. Package
  read/write/install paths do not serialize, deserialize, embed, or externally
  resolve imported HSD assets, and the reader still rejects old package bytes
  with nonzero imported asset counts.
- `FighterImportProvenance` now preserves source file name, source asset name,
  and importer warnings as debug metadata. Action indices are retained on native
  clips and state animation references, so provenance does not need to be
  gameplay truth.
- `engine_cpp/data/packages/` contains generated native packages for the full
  current Melee training roster. Runtime roster construction requires these
  packages by filename and fails loudly when one is missing.

## Runtime Gameplay Dependencies To Remove Or Convert

- Fighter roster construction in `simulation.cpp` first attempts to load
  `engine_cpp/data/packages/<fighter>_native.pfpkg`. Missing native packages now
  fail loudly instead of falling back to `*_hsd.pfighter.bin` during normal
  gameplay construction.
- The explicit package converter still reads `*_hsd.pfighter.bin` inside
  `melee_fighter_converter.cpp` and converts attributes, ledge snap values,
  animation lengths, hurtboxes, action scripts, skeleton, clips, mesh, model
  visibility, common bones, ECB source bones, and shield pose into native
  fields without storing imported assets on `FighterDefinition`.
- With the installed generated packages present, the headless roster smoke sees
  zero HSD-backed roster assets and native data for all 27 fighters.
- Normal animation clip lookup now goes through native authored clip accessors.
  Package-first roster loads may store large native clip, mesh, and model-part
  vectors behind shared `authored*Source` pointers for memory, but runtime reads
  them through native accessors instead of interpreting HSD structures.
- Imported hurtbox collision now populates `FighterDefinition::hurtboxes` with
  native joint/type/capsule data and runtime collision/grab/damage-region logic
  reads those native hurtbox definitions. The live pose-space capsule cache is
  named `poseHurtboxCapsules`.
- Environment collision/ledge values and imported ECB source bones are copied
  into native fighter fields during conversion; the debug ECB overlay reads the
  native metadata.
- Common-bone lookup and special fighter bones are copied into native fighter
  metadata and runtime shield centers, common-part lookup, capture anchors, and
  debug head/shield drawing read those native fields.
- Model visibility defaults now size from native `authoredMesh`, and model-part
  animation state resolves against native serialized model-part animation sets.
- Shield pose base data is copied into native fighter metadata and serialized in
  packages.
- Some runtime fields and function names still use `hsd*` naming even when the
  value is now rollback-owned native pose state.

## Editor Dependencies To Remove Or Convert

- Editor package snapshots require native fighters and package loads no longer
  accept an external HSD asset pool.
- Clip lists, asset summaries, mesh rendering, selected-vertex overlays, and
  animation previews now read native authored data. If package-first roster data
  is still parked in source-backed native vectors, entering the assets or
  animation workspace materializes those vectors into direct editable fields.
- Imported mesh editor controls now use the native `FighterMesh*` struct names.
- The timeline/subaction editor can show converted action subactions from
  persisted package data. `AttackHi3` has a headless package/editor gate that
  verifies native hitbox/subaction timeline markers and native clip references.

## Package Format Dependencies To Remove

- `FighterPackage` no longer stores imported assets. The writer emits an asset
  count of zero, and the reader rejects any nonzero imported asset count.
- `readFighterPackage` rejects nonzero imported asset counts and rejects legacy
  fighter records that set the serialized imported-HSD flag, asset index, or
  asset name. It no longer accepts an external imported asset pool.
- Package validation checks native authored animation references and native
  hurtbox-index references. Imported HSD-dependent packages are rejected by
  write/read/install/test-world paths.
- Current package format version is native-only and rejects packages that embed
  raw imported assets, set legacy imported-HSD fighter fields, use invalid old
  versions, or depend on HSD action/animation interpretation at package load
  time.
- Runtime/editor package export no longer has an HSD-backed fighter-definition
  path. The one-time converter owns imported-HSD conversion into native package
  data.

## Native Data Already Present But Incomplete

- Native skeleton, animation clips, mesh textures/material-ish batches, and
  hurtboxes have storage and serialization.
- Native mesh storage now uses `FighterMesh`/`FighterMesh*`, and package
  rendering now reads it through native authored mesh accessors.
- Native metadata now exists for imported fighter bone roles, common-bone
  lookup, ECB source bones, shield pose, model-part animation sets, and import
  provenance.
- Native action subactions exist, and HSD action scripts can be decoded into
  `FighterState::action`; converted packages persist those decoded native
  subactions as package truth.
- Provenance is still coarse. Per-joint/per-material source IDs and optional raw
  command/source offsets are not yet serialized except where equivalent IDs
  already survive in native records such as action indices, clip IDs, batch
  object indices, model-part indices, and texture/material fields.

## First Migration Targets

1. Move `animation_asset.*` and `hsd_action_import.*` out of the shared core
   library and behind explicit importer/converter targets.
2. Move the PFHA imported-asset structs in `animation_asset.*` behind a true
   converter/importer library boundary, leaving only renamed native authored
   package structs in shared runtime headers.
3. Rename remaining `hsd*` runtime pose/cache fields whose data is now native
   rollback state, keeping serialization compatibility in mind.
4. Expand import provenance for optional original joint/material/source-command
   IDs without letting those IDs drive gameplay behavior.
