# 02 — Linked scene identity and overrides

Status: implemented, 2026-09-09. Depends on: 01.

## Delivered

- [Persistent schema and ownership contract](identity-and-overrides-v1.md), with
  exact matrix order, Blender identity/duplicate/undo rules, unsupported cases
  and source removal semantics.
- [Scene authoring and validation API](../engine/scene/include/mediaserver/scene/linked_scene.hpp),
  source transaction admission, independent link duplication, Preview scene
  cloning, last-valid-revision retention, generic persistence and scene digests.
- Native Inspector controls for follow/hold, shot world translation, stage
  alignment, local visibility, material reset, link duplication and retained
  source-deletion records. Stage translation and ordinary undo use the same
  affine authoring path.
- Per-property material factor application to imported model slots, preserving
  other imported factors/textures through geometry updates.
- [Regression suite](../engine/scene/tests/scene_linked_scene_tests.cpp), Inspector
  intent tests, [fixture launcher](../scripts/run-blender-linked-scene.ps1), and
  [UI walkthrough](ui-checks-step-02.md).

## Validation and UI inspection

The Debug GUI build passed. The Release libraries, tests and GUI executable
passed using the same configured Ninja graph with a separate executable output,
`out/build/windows-msvc-release-gui/apps/server/media-server-linked-scene.exe`.
The normal Release executable was locked by an existing background lifecycle
service; that process was left running. Only the generated, ignored Ninja graph's
executable output path was changed for this build. Source/CMake target definitions
retain the normal executable name. Once the service releases the original file,
the ordinary Release build can write it normally.

24 relevant CTest suites passed in each configuration: all 14 scene suites,
material ingest/sampling, scene rendering/mixing/compositing, mapped scene,
model loader/cache, Blender reference and Inspector. The focused linked-scene
and Inspector suites were rerun after the final admission/UI changes. Logs are
in `out/linked-scene-*-build.log`, `out/linked-scene-debug-regressions.log`,
`out/linked-scene-release-tests.log`, and `out/linked-scene-final-tests.log`.

The generated linked project was opened in Debug and visibly rendered through
the Advanced renderer. Hero showed held world `(1.5,1,-1)`. Editing stage alignment
moved following content while Hero stayed fixed. Hold-to-follow preserved its
world placement. Hiding removed the model from drawing while keeping the row;
Ctrl+Z restored it. Link duplication produced a new attachment (`100204` versus
`100200`), and moving that attachment separated its geometry from the original.
File > Save Project saved the changes. Reopening the saved project in the Release
editor retained the duplicate identity, follow policy, stage alignment and Hero
position `(1.5,1,0)` for that duplicate. The saved UI specimen is
`out/blender-scene-reference/linked/ui-saved-independent-links.mstage`.

The Release source-removal fixture visibly omitted Hero while retaining its
selectable **Hero prop (source removed)** row, held position `(1.5,1,-1)`, and
**Discard retained shot record** action. The discard command's behavior is covered
by the headless authoring test. Source deletion/restore, reparenting, geometry
revision changes, singular/invalid rollback and Preview identity remapping are
also covered by the regression suite.

Transport, manifest ingestion, Blender-side publication and reverse placement
remain later steps. Animated parents, origin rebasing and reflected linked
drawables fail with explicit diagnostics. The Inspector currently exposes
translation; complete affine matrices are supported by the authoring API, with
the richer rotation/scale workflow scheduled for steps 06–07.

## Outcome

A persisted linked-scene model that preserves shot decisions across source updates.

## Work

- Add a source-scene ID, link-instance ID, stable source-object ID, prototype ID,
  and material ID. Map each linked occurrence to a persistent document identity.
  Multiple links to the same Blender scene must have independent shot adjustments.
- Store source hierarchy and local transforms separately from stage alignment and
  per-occurrence shot overrides. Keep asset identity separate from content revision.
- Implement **Follow source with offset** and **Hold shot placement**. Specify
  local versus world-space behavior and exact matrix order in the schema contract.
  Holding world placement must compensate for upstream parent changes.
- Define behavior for reparenting, origin changes, singular transforms, animated
  parents, and deletion. Reject unsupported transform cases rather than drift.
- Store overrides by property; a geometry update must not replace placement,
  visibility, or a locally overridden material factor.
- Define source removal and local hiding separately. Preserve recoverable override
  records for deleted source objects and expose deliberate resolution when needed.
- Specify duplicate-ID repair in Blender, library-linked/read-only identity, and
  undo restoration. Names, memory addresses, and array positions are not identities.
- Extend schema validation, persistence, scene evaluation, and digests together.

## Acceptance

Rename, reorder, save/reopen, duplicate, delete/restore, and reparent the reference
objects. IDs and intended overrides survive. A changed source mesh updates every
linked occurrence while keeping their different placements. Invalid hierarchies
and singular hold-placement conversions leave the last valid revision intact.
