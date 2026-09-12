# 05 — Blender publisher

Status: implemented for the pinned static mesh subset. Depends on: 03, 04.

Implementation: [installable add-on](../integrations/blender/README.md),
[publisher design and limits](publisher-v1.md), and
[UI walkthrough and verification](ui-checks-step-05.md).

The Blender panel provides collection selection, clipboard pairing, initial
publish, live/pause, Publish Now, disconnect and resync. Edit/Sculpt changes publish
when returning to Object Mode. The evaluated-copy scheduler and separate Python
serializer process keep source access on Blender's main thread; placement drags
reuse existing geometry. Prototype, queue and pairing capacities remain explicit.

## Outcome

An installable add-on links a selected collection and publishes incremental
evaluated content while the artist continues working.

## Work

- Provide connection, collection selection, initial publish, live update, pause,
  and resync controls. Store persistent IDs and source metadata in the blend file
  or an explicit identity mapping for data that cannot carry editable properties.
- Observe dependency-graph updates and distinguish transforms, geometry, materials,
  and collection membership. Send tiny placement deltas without exporting meshes.
- Extract evaluated meshes with modifiers applied to the exported result, leaving
  the artist's original mesh, modifier stack, and procedural setup intact.
- Handle edit-mode completion, undo/redo, file loading, deletion, and add-on shutdown.
  Do not retain invalid Blender object references across undo or file changes.
- Access Blender data through its supported execution context. Copy evaluated data
  before delegating serialization/network work; do not access bpy from arbitrary
  background threads. Confirm the supported scheduling pattern on the pinned build.
- Bound extraction work and coalesce fast geometry edits. Publish the newest
  completed valid update, with explicit pending status for expensive evaluations.
- Cleanly unregister handlers and release temporary evaluated meshes.

## Acceptance

Move, sculpt, change a modifier, add/remove an object, undo, and reopen the file.
Media Server receives the intended evaluated result and stable identity. A transform
drag does not trigger mesh export. Rapid edits and disconnects do not leave Blender
unresponsive, leak temporary meshes, or grow an unlimited queue.
