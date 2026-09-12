# Step 07 UI checks

Use the Debug or Release GUI build and Blender **5.2.0 LTS / fbe6228777e7**.
Build the add-on with `python scripts/package-blender-livelink.py`; install
`out/blender-livelink/mediaserver_livelink-0.3.0.zip` through Blender's Add-ons
Install from Disk action. The [step 05 walkthrough](ui-checks-step-05.md) covers
installation and pairing. Use a static collection with a mesh, Bevel modifier and
optional Empty parent. The repository's `scripts/blender-livelink-demo.py` can
prepare a disposable collection without changing saved Blender preferences.

## Link, select and edit

1. Open Media Server and select **PREVIEW**. Use an unlocked scene distinct from
   Program. Choose **File > Start Scene Link**, then **Scene Link Details... >
   Copy details**. In Blender choose the collection, **Paste Director Details**,
   then **Connect & Publish**.
2. Expand **Linked collections** in the Scene panel. The collection uses its
   Blender name. Expand its source hierarchy and select a prop; select through
   the viewport too. Try Ctrl/Shift selection, collapse a parent and filter for a
   child name. Selection should follow visible hierarchy order.
3. Select the collection. Its Inspector exposes **Link stage alignment**, status,
   revision and the nearby connection/update controls. Select a prop to see
   **Shot position**, Hold/Follow policy and its overridden properties instead.
4. Set Shot position X to **1.500 m**. Edit the mesh in Blender, return to Object
   Mode and allow publication. The geometry changes while shot placement remains.
   Use Scene Undo/Redo: the local position changes without undoing the incoming
   mesh revision. **Reset placement to source** removes the shot adjustment.
5. Try **Hide this occurrence**, **Show this occurrence** and **Follow source
   visibility**. Edit a material factor and reset it. Reset actions appear when
   the corresponding property is overridden. Lock the collection and confirm
   child edits are blocked; unlock it before continuing.

## Pause, update and reconnect

1. Select the collection or a prop and click **Pause source updates**. The tree
   and Inspector say paused and retain the accepted revision. Shot editing stays
   available.
2. In Blender enter Edit Mode, scale the selected mesh vertices on X, return to
   Object Mode and select **Publish Now**. Blender reports `source_paused` while
   staying connected. Media Server continues rendering the previous mesh.
3. Choose **Resume source updates** in Media Server, then **Publish Now** in
   Blender. The new geometry appears at the existing shot placement. Updating
   status lasts only while the candidate is pending. An unsupported resource
   instead produces an explicit refusal diagnostic and retains accepted content.
4. Use **Disconnect Scene Link**. The accepted scene stays visible and status
   becomes disconnected. **Start / reconnect Scene Link** starts a new pairing;
   open **Pairing / reconnect details**, copy the new details, paste them into
   Blender and **Connect & Publish**. Source identities and shot overrides persist.

## Copy, duplicate and unlink

1. Select a linked collection or one of its members and use **Ctrl+C, Ctrl+V**
   in Preview. The collection's **Copy independent snapshot** action invokes the
   same copy. Expect an ordinary group named `<collection> (independent)` and
   ordinary models with the current composed placement, visibility and appearance.
   Move the copied group aside; a subsequent source mesh update affects only the
   linked original. Undo the paste in one step.
2. Choose **Duplicate with shared source** in the collection Inspector. Both
   attachments receive source geometry, while each retains its own shot edits.
   **Project Scenes > Create Copy** also preserves shared source references with
   new attachment identities. Undo the extra attachment before testing Blender
   shot display: step 06 deliberately refuses ambiguous reverse placement when
   more than one attachment of a source exists in Preview.
3. Select the original collection and choose **Unlink collection - keep accepted
   scene**. It becomes an ordinary independent group/models with the same picture.
   Source-only Empty parents and removed-object recovery records are omitted;
   drawable worlds are flattened, including any affine shear. Pairing stops.
4. Use **Ctrl+Z** once. The complete link and overrides return; reconnect remains
   an explicit action. Ordinary Delete on a linked collection is suppressed so
   it cannot orphan children. Delete on a linked prop is a recoverable hide.

## Save and reopen offline

1. Save the Blender file to retain its source IDs. Save the Media Server project
   with **File > Save Project As...** after a successful geometry update and a
   visible shot offset. Note its accepted revision and position.
2. Close Blender and Media Server. Reopen the saved `.mstage` and select its
   saved Preview scene. The accepted mesh, appearance and placement must render
   with disconnected status and no authoring process. Keep its accepted package
   directory: this version saves immutable resource paths, not a portable bundle.
3. Reopen the saved `.blend`, start Scene Link in Preview, copy fresh pairing
   details and connect. Edit its mesh again and publish. Verify the revision
   changes, geometry updates and the saved shot position remains intact.
4. Select **PROGRAM** and confirm it still shows its earlier scene. If the same
   linked source is already used by Program, the current guard refuses source
   publication. Coordinated Program adoption is step 08.

## Recorded verification

On 2026-09-10, Debug and Release GUI builds succeeded and all nine focused CTest
suites passed in each configuration: linked scene, scene model, linked ingestion,
Director service, Inspector, Scene workspace, publisher unit tests, model cache
and material ingest. Added checks cover exact-affine independent snapshots,
material masks, unlink/paste identity, source independence, save/reopen,
pause/resume, late pause admission, atomic undo/redo and visible hierarchy selection.

The installed ZIP passed **43 real Blender integration checks in each build**.
These include the prior shot placement, undo/conflict and source save/reopen
checks plus Director pause, retaining connection and geometry, resume and explicit
republish. Run with:

```powershell
ctest --preset windows-msvc-debug-gui -R 'mediaserver\.(scene_linked_scene|scene_model|linked_scene_ingest|director_link_service|editor_inspector|editor_workspace|blender_publisher_unit|render_model_cache|render_material_ingest)$' --output-on-failure
ctest --preset windows-msvc-release-gui -R 'mediaserver\.(scene_linked_scene|scene_model|linked_scene_ingest|director_link_service|editor_inspector|editor_workspace|blender_publisher_unit|render_model_cache|render_material_ingest)$' --output-on-failure
python scripts/test-blender-livelink.py --build debug --shot
python scripts/test-blender-livelink.py --build release --shot
```

The disposable native Debug UI session used **LiveLink Demo / LiveLink Hero**.
The Director shot position was X = 1.500 m, Y = 0.750 m, Z = 0.000 m. A paused
Blender Edit Mode X-scale left Preview unchanged; resume and explicit publication
accepted the wider mesh at that position. Ctrl+C/Ctrl+V created an independent
group, Scene Undo removed it, unlink retained appearance, and Scene Undo restored
the disconnected link. The project was saved as `out/step07-ui/accepted.mstage`.
With Blender closed, a fresh Director process loaded that project and rendered
the accepted wider mesh at the saved shot position.

The saved Blender file was then reopened, paired using fresh details from the
collection Inspector, and published successfully. A second Edit Mode mesh scale
on Z produced another accepted revision and a taller model. Shot position remained
X = 1.500 m, Y = 0.750 m, Z = 0.000 m. Switching to Program showed the unchanged
original scene; switching back restored the linked Preview.

The demo's existing unrelated compositor graph error and missing demo media are
visible in some captures; verification used the Stage Preview's linked model.
No test modified the user's saved Blender preferences or production project.

- [Blender paused](evidence/step07-blender-paused.jpg) and
  [Director paused](evidence/step07-director-paused.jpg)
- [Accepted geometry update](evidence/step07-preview-updated.jpg) and
  [independent unlink](evidence/step07-unlinked.jpg)
- [Project reopened with Blender closed](evidence/step07-offline-reopened.jpg)
- [Geometry update after fresh pairing](evidence/step07-reconnected.jpg) and
  [unchanged Program scene](evidence/step07-program-unchanged.jpg)
- [Debug integration results](evidence/step07-blender-debug.json) and
  [Release integration results](evidence/step07-blender-release.json)

Build/test logs and disposable fixtures remain under `out/step07-*`.
