# Step 06 UI checks

Use Blender **5.2.0 LTS / fbe6228777e7** and a current Debug or Release GUI build.
Package `python scripts/package-blender-livelink.py` and install
`out/blender-livelink/mediaserver_livelink-0.2.0.zip` using Blender's Add-ons menu.
The [step 05 walkthrough](ui-checks-step-05.md) covers pairing and initial source
publishing. Start with a cube, a Bevel modifier, and an Empty parent; keep rigs,
constraints, animation and unsupported modifiers outside this test collection.

## Director to Blender

1. Select **PREVIEW**, start Scene Link, copy details and **Connect & Publish** in
   Blender. Expect one linked object in Media Server and a `Shot layer · …` Empty
   above the original object in Blender's Outliner.
2. Select the linked prop in Media Server. In its Inspector, change **Shot world
   X**, or use its viewport gizmo. The prop moves in Blender while the original
   object's local transform fields and source mesh remain intact. The Blender
   mesh-extraction/GLB-build counters and source revision stay fixed.
3. Select the original mesh in Blender (expand the wrapper in the Outliner), enter
   Edit Mode, move a vertex, and return to Object Mode. Preview gets the revised
   geometry at the same shot placement. Repeat with a Bevel edit.
4. In the Director choose **Hold shot placement**, move the source parent in
   Blender, and verify the held world stays fixed. Choose **Follow source with
   offset**, move the parent again, and verify the prop follows by the parent
   delta while retaining its offset. Policy switching preserves the picture.
5. In Blender click **Show Source Placement**. Source parenting and transform
   edits now show their authored positions. Reparent the prop to another static
   Empty, publish, and click **Show Shot Placement**. Check the retained shot
   policy. Use source display when changing source viewport hiding.

## Blender shot gestures, conflicts and undo

1. Select the linked mesh or its wrapper. Click **Begin Shot Gesture**. The panel
   identifies the SHOT edit and selects the wrapper. Use Blender's ordinary Move,
   Rotate or Scale gizmo, then click **Commit Shot** within 15 seconds. Both apps
   converge on the new pose. The source object's transform channels stay intact.
2. Use **Hold Shot**, **Follow Source**, **Reset Shot Placement**, **Hide**, **Show**,
   and **Follow Source Visibility**. Hidden objects remain addressable through the
   wrapper in the Outliner. Source render visibility is unchanged; this workflow
   displays Director visibility in Blender's viewport.
3. Commit a Blender shot gesture, then edit vertices and publish. Click **Undo My
   Last Shot Edit**. Only that shot property returns to its earlier value; the
   revised vertices remain. Native Blender Undo/Redo targets source edits.
4. Move a prop in Media Server, publish a subsequent Blender mesh edit, and use
   Media Server's Scene Undo/Redo. The local placement is undone/redone while the
   latest mesh stays present. Incoming authoring has separate undo history.
5. Begin a Blender shot gesture. Before committing it, change the same prop's
   shot placement in Media Server. Commit in Blender and expect a conflict.
   **Accept Director** restores the current Director pose. Repeating the exercise
   and selecting **Retry My Shot** explicitly submits the retained proposed pose
   against the latest revision. No automatic overwrite or oscillation occurs.
6. Leave a claim open for over 15 seconds. Commit is disabled and expiry is
   visible. Accept Director to cancel, or retry/start another gesture. Disconnect
   during a gesture, then reconnect: the accepted pose is restored and the old
   claim cannot block a new gesture.

## Recovery and unsupported content

Save/reopen the `.blend` and reconnect using the active Director pairing. Source
IDs, source channels and saved shot wrappers survive; local transient shot undo
history and pairing credentials do not. Save/reopen the Media Server project and
restart Scene Link, then copy fresh pairing details. Accepted packages and shot
records use the existing linked-scene project persistence.

Add a constraint or keyed parent, then request reverse placement. Expect a specific
refusal and restoration of the source hierarchy. Rigs, singular placement,
library/override objects, unqualified modifiers and multiple Preview attachments
also require explicit correction. Full scope and transport limits are in the
[shot contract](shot-placement-v1.md).

## Recorded verification

Debug and Release GUI builds succeeded. Eight relevant CTest suites passed in
each configuration, including transport/admission, linked scene, ingestion,
scene model, Inspector and existing publisher checks. The Director suite adds
conditional state, claim overlap, geometry/placement independence, exact commit
replay, same-property conflicts, visibility, expiry, singular rejection, and
native Undo/Redo after remote source changes.

The installed ZIP passed the 20-check step 05 publisher integration suite and the
39-check step 06 suite in both configurations. The latter exercises real Blender
wrappers, Director-origin moves, zero source feedback, both placement policies,
vertex and parent edits, reparenting, Blender-origin shot gestures, guarded undo,
conflict/retry, expired uncommitted gesture retry, visibility reset, affine shear,
unsupported constraints/animation, save/reopen/reconnect, and root placement with
an identity anchor. Run with:

```powershell
python scripts/test-blender-livelink.py --build debug --shot
python scripts/test-blender-livelink.py --build release --shot
```

Evidence is retained under `evidence/step06-*`; complete build logs and disposable
fixtures remain under `out/step06-*` and `out/blender-livelink/`.

On 2026-09-10, a disposable Debug GUI session was paired through Blender's sidebar.
A Director Inspector edit moved the root prop to X = 0.400 m. Blender displayed
that returned placement while the source object's location remained X = 0 and its
mesh-extraction and GLB-build counters stayed at 1. The visible expired-claim
conflict was cancelled with **Accept Director**. Clicking **Hold Shot** in Blender
then changed the Director Inspector to **Hold shot placement (world)**, retaining
the pose and source revision. The source/shot labels and undo action were readable.
The broader geometry, parenting, undo and recovery cases above are exercised by
the installed-ZIP integration suite.

- [Director placement](evidence/step06-director-placement.jpg) and
  [Blender returned placement](evidence/step06-blender-return.jpg)
- [Blender Hold Shot acceptance](evidence/step06-blender-hold.jpg) and
  [Director policy after the Blender edit](evidence/step06-director-blender-hold.jpg)
- [Debug integration results](evidence/step06-blender-debug.json) and
  [Release integration results](evidence/step06-blender-release.json)
