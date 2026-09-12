# Step 05 UI checks

Use **Blender 5.2.0 LTS, Windows build fbe6228777e7** and a GUI Media Server build.
The add-on deliberately refuses other builds until they have been qualified.

## Install and pair

1. From the repository root, run `python scripts/package-blender-livelink.py`.
   This produces `out/blender-livelink/mediaserver_livelink-0.1.0.zip`.
2. In Blender open **Edit > Preferences > Add-ons**. In the menu choose **Install
   from Disk**, select the ZIP and enable **Media Server LiveLink**. In the 3D View
   press **N** and select the **Media Server** sidebar tab.
3. In Media Server select **PREVIEW**, then **File > Start Scene Link** and
   **File > Scene Link Details...**. Click **Copy details**.
4. In Blender choose a collection containing meshes and transform parents. For a
   first test use an ordinary cube with a Bevel modifier and an untextured opaque
   Principled material. Cameras/lights, textured shaders and non-realized scatter
   should stay outside this initial collection. Click **Paste Director Details**,
   then **Connect & Publish**.
5. Expect **Accepted in Preview** with a revision and extraction counters. In Media
   Server the object appears once in the Scene panel and Preview viewport. Its
   Inspector exposes the existing linked-scene controls. The Director details
   report the same accepted revision. Program continues with its previous scene.

The selected collection, source metadata and UUID properties persist when the blend
file is saved. The code stays in memory and clears on file load. **Resync** reads
the Director's current revision and publishes a fresh snapshot; use it after an
interrupted connection. Restarting Scene Link in Media Server generates new details
that must be copied again.

## Exercise live updates

1. Drag the source object's X location in Blender. The last-update byte count
   should be small, and **Mesh extractions** and **GLB builds** should stay fixed.
   In the fixture below a placement update is 263 bytes. Media Server follows the
   placement unless its occurrence is held.
2. Change Bevel width or segments. The Preview geometry changes; the original cube
   vertices and modifier stack remain editable. Only the changed prototype is
   rebuilt. Rename the object and check it still appears once under its new name.
3. Enter **Edit Mode**, edit vertices, then return to **Object Mode**. The panel
   displays a pending message during editing and publishes the completed result.
   Repeat with an actual **Sculpt Mode** stroke; return to Object Mode to publish.
4. Duplicate an object, then delete the copy. The duplicate gets its own identity;
   the removed object becomes a Director recovery tombstone. Use Blender Undo and
   Redo and verify the same identity is restored rather than a second occurrence.
5. Make rapid modifier edits. The panel shows coalescing/pending work, then admits
   the latest completed source state. There is one submitted revision at a time.
   Blender's own evaluation of a very expensive modifier is not preemptible;
   reduce per-object complexity when the displayed evaluation/copy time is high.
6. Clear **Live updates**, make a placement edit and verify Preview stays still.
   **Publish Now** sends pending edits once while paused. Enable Live updates to
   resume. A transaction already submitted before Pause may finish.
7. Disconnect Blender. Preview remains available. Reconnect with **Connect &
   Publish**; after a network error use the same control or **Resync** while still
   connected. Stale-base and pairing errors remain visible until explicitly fixed.
8. Save the `.blend`, reopen it and reconnect using fresh copied details. Object and
   source IDs remain stable. Save/reopen the `.mstage` as well using the earlier
   linked-document persistence workflow. Playback does not need Blender running.
9. Disable the add-on. It removes its handlers, message-bus subscriptions and timer,
   closes sockets and terminates its serializer process. Accepted Preview content
   remains in Media Server.

All initial capacities and specific unsupported-content diagnostics are documented
in [publisher v1](publisher-v1.md). A pairing retains at most 64 distinct resource
hashes and 256 receipts, so a long editing session may require restarting Scene
Link even when its current collection is small. This is the existing step 04
capacity policy, not a large-environment qualification.

## Recorded verification, 2026-09-09

Both GUI Debug and Release builds passed. Eleven relevant CTest suites passed in
each configuration, including the new publisher core suite (nine Python checks).
The packaged ZIP was installed using Blender's add-on installer into an isolated
scripts directory and run in factory Blender against the actual C++ Director
service, immutable importer and command scheduler. Twenty integration
checks passed in each configuration:

- ZIP installation, evaluated initial publish and external transform-parent visibility.
- Fifty coalesced transform changes: **283-byte delta, zero additional mesh
  extractions, GLB builds or geometry ingestion**.
- Rename identity, modifier updates, material revisions and Edit Mode completion.
- Pause, Publish Now and resume.
- Separate copied object/mesh UUIDs, real Blender Undo/Redo, deletion and restoration.
- Rapid geometry edits, bounded pending state under backpressure and invalidation
  of an incomplete extraction batch.
- Save/reopen with no stored pairing code, source/object identity preservation,
  interrupted serializer recovery and full handler/timer/process cleanup.

The native GUI run used the disposable launcher in `scripts/blender-livelink-demo.py`.
It observed clipboard pairing, initial Preview rendering, a source-location drag
with extraction/GLB counts unchanged at one and a **263-byte** delta, an actual
Sculpt brush stroke with explicit pending status, and a **282-byte** prototype
delta after returning to Object Mode. Disconnect retained the accepted Preview.
The test pairing was revoked after verification. The native run exercised the
panel from repository source; the automated runs exercised the installed ZIP.

Evidence:

- [Blender placement delta with unchanged extraction counters](evidence/step05-blender-placement.jpg).
- [Director Preview and linked Inspector at the received placement](evidence/step05-director-preview.jpg).
- [Completed Sculpt Mode publication](evidence/step05-sculpt-complete.jpg).
- [Debug integration results](evidence/step05-blender-debug.json) and
  [Release integration results](evidence/step05-blender-release.json).
- [Debug CTest results](evidence/step05-debug-tests.txt) and
  [Release CTest results](evidence/step05-release-tests.txt).
- Local build logs: `out/step05-debug-build.log`, `out/step05-release-build.log`.
  Full integration transcripts, generated blend files and admitted resource
  packages are under `out/blender-livelink/<configuration>-<run>/`.

The isolated editor's default project already displayed an unrelated unconnected
compositor-input diagnostic before linking. Verification assessed the imported
Preview geometry, source placement, receipts and retained Program scene; it did
not change that default compositor configuration. Manual `.mstage` save/reopen is
provided as an operator check and retains the existing scene persistence coverage.
