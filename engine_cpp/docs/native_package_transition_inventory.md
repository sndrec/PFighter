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
- `FighterPackage` still has a legacy `hsdAssets` field in memory so old
  dependent package shapes can be rejected, but package read/write/install
  paths no longer serialize, deserialize, embed, or externally resolve imported
  HSD assets.

## Runtime Gameplay Dependencies To Remove Or Convert

- Fighter roster construction in `simulation.cpp` loads `*_hsd.pfighter.bin`
  through `cachedHsdFighterAsset`, sets `FighterDefinition::hasHsdAsset`, then
  derives attributes, ledge snap values, animation lengths, and native
  subactions from that asset at runtime.
- Animation lookup and playback prefer `def.hsdAsset->clips` and
  `def.hsdAsset->skeleton` in simulation, state functions, headless checks, and
  raylib preview/rendering. This must become native `authoredClips` and
  `authoredSkeleton`.
- Imported hurtbox collision now populates `FighterDefinition::hurtboxes` with
  native joint/type/capsule data and runtime collision/grab/damage-region logic
  reads those native hurtbox definitions. The live capsule cache is still named
  `hsdHurtboxCapsules` and should be renamed later.
- Environment collision/ledge values and imported ECB source bones are copied
  into native fighter fields during conversion; the debug ECB overlay reads the
  native metadata.
- Common-bone lookup and special fighter bones are copied into native fighter
  metadata and runtime shield centers, common-part lookup, capture anchors, and
  debug head/shield drawing read those native fields.
- Model visibility and model-part animation state still size and resolve against
  `def.hsdAsset->modelPartAnimations`.
- Shield pose base data is copied into native fighter metadata and serialized in
  packages. Guard-stick blending still looks up action 38 through imported
  animation clips until the animation payload migration is solved without
  duplicating roster memory.
- Some runtime fields and function names still use `hsd*` naming even when the
  value is now rollback-owned native pose state.

## Editor Dependencies To Remove Or Convert

- Editor package snapshots native-convert imported fighters and clear
  `package.hsdAssets`; package loads no longer accept an external HSD asset pool.
- Clip lists prefer imported clips when `def.hsdAsset` exists, so converted
  Melee animations are not purely edited through `authoredClips`.
- Imported mesh status and preview paths read `def.hsdAsset->mesh` separately
  from `def.authoredMesh`.
- The timeline/subaction editor can show converted action subactions, but the
  conversion currently happens during runtime fighter construction, not as
  persisted package data.

## Package Format Dependencies To Remove

- `FighterPackage` still stores
  `std::vector<std::shared_ptr<const HsdFighterAnimationAsset>> hsdAssets`, but
  validation rejects non-empty vectors and the writer emits an asset count of
  zero.
- `readFighterPackage` rejects nonzero imported asset counts and rejects fighter
  records that set `hasHsdAsset`, asset index, or asset name. It no longer
  accepts an external `hsdAssetPool`.
- Package validation checks native authored animation references and native
  hurtbox-index references. Imported HSD-dependent packages are rejected by
  write/read/install/test-world paths.

## Native Data Already Present But Incomplete

- Native skeleton, animation clips, mesh textures/material-ish batches, and
  hurtboxes have storage and serialization.
- Native mesh storage is still named `HsdFighterMesh`/`HsdMesh*`; that should be
  renamed or wrapped as PFighter mesh data after behavior is migrated.
- Native metadata now exists for imported fighter bone roles, common-bone
  lookup, ECB source bones, and shield pose. Native metadata is still missing
  for model-part visibility tables/animation sets and import provenance.
- Native action subactions exist, and HSD action scripts can be decoded into
  `FighterState::action`, but the decoded result is not yet the package truth.

## First Migration Targets

1. Add a conversion boundary that copies imported `PFHA` data into native
   `FighterDefinition` fields immediately after import.
2. Add native metadata fields for fighter bone roles, common-bone lookup,
   model-part animation sets, shield pose, and import provenance.
3. Continue moving imported roster construction behind an explicit conversion
   boundary without duplicating the full Melee animation payload in runtime
   memory.
4. Switch runtime animation, model-part visibility, and render paths to read
   native fields only.
5. Keep `HSDRaw` and `HsdFighterAnimationAsset` parsing isolated to exporter or
   explicit conversion utilities until the names can be retired.
