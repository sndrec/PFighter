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
  however it is still treated as a separate imported asset type by runtime and
  packages.
- `FighterDefinition` already has native editable fields for properties, shield,
  ECB, authored skeleton, authored clips, authored mesh, package scripts,
  authored hurtboxes, states, interrupts, callbacks, and subactions.
- `FighterPackage` still has `hsdAssets`, and package serialization embeds
  `HsdFighterAnimationAsset::sourceBytes` for those assets.

## Runtime Gameplay Dependencies To Remove Or Convert

- Fighter roster construction in `simulation.cpp` loads `*_hsd.pfighter.bin`
  through `cachedHsdFighterAsset`, sets `FighterDefinition::hasHsdAsset`, then
  derives attributes, ledge snap values, animation lengths, and native
  subactions from that asset at runtime.
- Animation lookup and playback prefer `def.hsdAsset->clips` and
  `def.hsdAsset->skeleton` in simulation, state functions, headless checks, and
  raylib preview/rendering. This must become native `authoredClips` and
  `authoredSkeleton`.
- Hurtbox collision still uses `def.hsdAsset->hurtboxes` and live
  `hsdHurtboxCapsules` when imported data is present. Native packages need the
  converted capsules in `FighterDefinition::hurtboxes` and runtime hurtbox
  evaluation must not special-case imported HSD assets.
- Environment collision/ledge values are imported from
  `def.hsdAsset->environmentCollision` during fighter construction, but the
  debug ECB overlay still reads the imported collision bones directly.
- Common-bone lookup and special bones still come from
  `def.hsdAsset->commonBoneLookup` and `def.hsdAsset->fighterBones` for
  hitbox/subaction bone mapping, shield bone lookup, head/shield debug drawing,
  item hand references, and subtree checks.
- Model visibility and model-part animation state still size and resolve against
  `def.hsdAsset->modelPartAnimations`.
- Shield pose still reads `def.hsdAsset->shieldPose` and guard clip action 38
  directly.
- Some runtime fields and function names still use `hsd*` naming even when the
  value is now rollback-owned native pose state.

## Editor Dependencies To Remove Or Convert

- Editor package snapshots collect `fighter.hsdAsset` into `package.hsdAssets`.
- Clip lists prefer imported clips when `def.hsdAsset` exists, so converted
  Melee animations are not purely edited through `authoredClips`.
- Imported mesh status and preview paths read `def.hsdAsset->mesh` separately
  from `def.authoredMesh`.
- The timeline/subaction editor can show converted action subactions, but the
  conversion currently happens during runtime fighter construction, not as
  persisted package data.

## Package Format Dependencies To Remove

- `FighterPackage` stores `std::vector<std::shared_ptr<const HsdFighterAnimationAsset>> hsdAssets`.
- `writeFighterPackage` writes all `package.hsdAssets` and `writeHsdAsset`
  serializes `asset->sourceBytes` into the package. This violates the hard
  requirement for no embedded imported/raw asset blob.
- `readFighterPackage` can rebuild an imported asset from embedded bytes via
  `loadHsdFighterAnimationAssetFromBytes`, or resolve it from an external
  `hsdAssetPool`. Both are out-of-date package paths.
- `FighterDefinition` serialization writes `hasHsdAsset`, an asset index, and
  an asset name. Native packages should reject this legacy dependency rather
  than support it.
- Package validation skips authored animation reference validation whenever
  `fighter.hasHsdAsset` is set and counts imported hurtboxes for subaction
  validation.

## Native Data Already Present But Incomplete

- Native skeleton, animation clips, mesh textures/material-ish batches, and
  hurtboxes have storage and serialization.
- Native mesh storage is still named `HsdFighterMesh`/`HsdMesh*`; that should be
  renamed or wrapped as PFighter mesh data after behavior is migrated.
- Native metadata is missing for imported fighter bone roles, common-bone lookup,
  model-part visibility tables/animation sets, shield pose, and import
  provenance.
- Native action subactions exist, and HSD action scripts can be decoded into
  `FighterState::action`, but the decoded result is not yet the package truth.

## First Migration Targets

1. Add a conversion boundary that copies imported `PFHA` data into native
   `FighterDefinition` fields immediately after import.
2. Add native metadata fields for fighter bone roles, common-bone lookup,
   model-part animation sets, shield pose, and import provenance.
3. Switch package write/read/validation to reject `hasHsdAsset` and embedded
   imported bytes.
4. Switch runtime animation, hurtbox, bone lookup, shield pose, and render paths
   to read native fields only.
5. Keep `HSDRaw` and `HsdFighterAnimationAsset` parsing isolated to exporter or
   explicit conversion utilities until the names can be retired.
