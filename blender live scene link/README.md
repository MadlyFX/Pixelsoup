# Blender live scene link

Implementation roadmap for authoring complete environments in Blender and using
them in Media Server's Advanced ICVFX renderer, with fast updates and persistent
on-set adjustments. **All steps are proposed, not implemented by these files.**

Blender owns modeling, sculpting, foliage painting, procedural generation, and
rig authoring. Media Server owns scene assembly, shot adjustments, stage context,
and deterministic show playback. The connection exchanges evaluated content and
supported editable properties; it does not require a modeling application inside
Media Server or lossless interchange of arbitrary Blender features.

## Implementation sequence

| Step | Deliverable | Depends on |
| --- | --- | --- |
| [01 — Contract and reference scene](01-contract-and-reference-scene.md) | Scope, coordinate rules, fixtures, measured baseline | None |
| [02 — Linked scene identity and overrides](02-linked-scene-identity-and-overrides.md) | Persistent hierarchy, asset identity, placement policies | 01 |
| [03 — Scene ingestion and asset revisions](03-scene-ingestion-and-asset-revisions.md) | Manifest import and immutable per-asset updates | 02 |
| [04 — Director link service](04-director-link-service.md) | Versioned transport, transactions, reconnect | 02 |
| [05 — Blender publisher](05-blender-publisher.md) | Collection linking and incremental evaluated content | 03, 04 |
| [06 — Bidirectional shot placement](06-bidirectional-shot-placement.md) | Reverse updates, ownership, conflict handling | 05 |
| [07 — Preview and editing workflow](07-preview-and-editing-workflow.md) | Usable linked-scene controls and undo | 06 |
| [08 — Coordinated Program publication](08-coordinated-program-publication.md) | Asset staging and frame-aligned scene adoption | 07 |
| [09 — Foliage and terrain scale](09-foliage-and-terrain-scale.md) | Instance batches, terrain chunks, bounded work | 05, 08 |
| [10 — Materials and lighting](10-materials-and-lighting.md) | Defined look translation and bake invalidation | 05, 08 |
| [11 — Animation and stage context](11-animation-and-stage-context.md) | Show-time playback and camera references | 06, 08 |
| [12 — Qualification and delivery](12-qualification-and-delivery.md) | Compatibility matrix, performance evidence, packaging | 09–11 |
| [13 — Optional USD adapter](13-optional-usd-adapter.md) | Multi-application scene composition | 12; investigate earlier if needed |

Steps 03 and 04 have independent deliverables after the scene contract is settled.
Steps 09–11 extend the first usable static-scene workflow; their tests must feed
back into Program publication and capacity limits.

## Milestones

1. **First complete loop, steps 01–07:** link a collection, move a prop in Media
   Server, see that adjustment in Blender, edit its mesh, and retain the placement.
   Include save/reopen and disconnect/reconnect. This milestone targets Preview.
2. **Stage publication, step 08:** two output nodes reveal a complete revision on
   the same show frame, with asset distribution accounted for.
3. **Environment authoring, steps 09–12:** qualify foliage, terrain, materials,
   lighting, and animation on declared scene budgets and supported Blender builds.
4. **Broader interchange, step 13:** add USD when multi-application workflows justify it.

## Existing foundations and gaps

- [SceneModel](../engine/scene/include/mediaserver/scene/scene_model.hpp) separates
  document edits from deterministic frame evaluation.
- [ModelInstance](../engine/scene/include/mediaserver/scene/scene.hpp) already
  separates a model source from placement.
- [Model ingestion](../advanced%20icvfx%20renderer/14-material-model-and-ingest.md)
  builds immutable packages on workers.
- [Instancing and LOD](../advanced%20icvfx%20renderer/12-culling-instancing-and-lod.md)
  provide a starting point for repeated geometry, not a proven large-scatter budget.
- [Asset publication](../advanced%20icvfx%20renderer/15-baked-global-illumination.md)
  coordinates identities and permits. Its session protocol does **not** transport
  asset bytes; step 08 must provide or integrate staging.
- The [model loader](../engine/render/include/mediaserver/render/model_loader.hpp)
  bakes source mesh transforms into geometry and retains animation-specific node
  data. It is not a general editable linked-scene hierarchy.

## Rules across all steps

- Source data, shot overrides, and physical-stage alignment remain distinct.
- Persistent IDs survive renaming; content hashes identify revisions, not objects.
- The Director validates and commits authored changes. Blender never writes
  independently to render nodes or mutates GPU state.
- Frame recording performs no filesystem work, source parsing, compression,
  blocking waits, or capacity growth. Expensive work is event-driven and bounded.
- Preview follows authoring; Program publishes complete revisions through the
  existing frame authority. A disconnected Blender must not prevent show playback.
- Unsupported content and exceeded budgets produce specific diagnostics.
- Implementation changes require debug and release builds per the repository
  README, plus the acceptance checks in each step. This roadmap itself is documentation.

**Wired up through the UI** Simply passing a test does not make a feature usable.
These features should be usable in the software through the UI. New panels,
preferences tabs, etc. can be added to facilite this as needed. Provide a series of
instruction on how to test the features in the UI, and run the application to view
and test yourself before calling it done.

## Research references

- [Blender dependency graph](https://developer.blender.org/docs/features/core/depsgraph/)
- [OpenUSD composition and limitations](https://openusd.org/release/intro.html)
- [Blender USD support](https://docs.blender.org/manual/en/4.2/files/import_export/usd.html)
- [Blender 4.5 instancing changes](https://developer.blender.org/docs/release_notes/4.5/pipeline_assets_io/)
- [MeshSync DCC Plugins](https://github.com/Unity-Technologies/MeshSyncDCCPlugins)

These informed the design discussion. Pin and verify supported versions during
implementation; the links do not establish compatibility with future releases.
