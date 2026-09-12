# Step 04 UI checks

Build both GUI presets, then generate the sample manifests:

```powershell
python scripts/generate-linked-scene-manifests.py
```

Launch the Director editor with your normal launcher or use the isolated reference
launcher from [step 03](ui-checks-step-03.md). The authoring service is available
only in the Director editor. A Blender add-on is not needed for these checks;
`scene-link-client.py` is the application-neutral reference publisher for step 04.

1. Select **PREVIEW**. Choose **File > Start Scene Link**, then **File > Scene Link
   Details...**. The panel shows the local port, pairing code and current status.
   **Copy details** copies the text. Keep the panel open: the control frame count
   continues advancing and incoming revisions can still be admitted.
2. In a shell, substitute the displayed port and code below:

   ```powershell
   python scripts/scene-link-client.py out/blender-scene-ingest/revision-1.mslink --port PORT --pairing-code CODE --duplicate
   ```

   Expect `received`, `validated`, then `accepted_in_preview;
   published_to_program=false`. The repeated transaction returns the same receipt.
   The panel reports **Accepted in Preview: revision-1**. Hero, Shared, Unchanged
   and Placement parent appear exactly once. The reference client disconnects after
   publishing, so the details also report that revision registration is needed on
   reconnect. The imported scene remains visible.
3. Close the details panel. Select **Hero prop** and click **Hold shot placement**
   in the Inspector. X=-2, Y=0.5, Z=0 for the provided sample.
4. Run the same command with `revision-2.mslink`, omitting `--duplicate`. The client
   reconnects, registers the source and uses the Director's current base revision.
   Hero and Shared change geometry; Unchanged retains its original resource. Hero
   remains held at the same shot position. Program remains unchanged.
5. Send `placement-only.mslink`. Following props shift in X; the held Hero remains
   in place. This exercises resource reuse without a new geometry revision.
6. With the Scene panel focused, choose **Edit > Undo** once. The most recent source
   revision is one undo action. Failed transmissions add no undo entry. Subsequent
   publishers must register the now-current revision; a sparse delta based on the
   undone revision is refused until a fresh snapshot establishes its baseline.
7. Reopen **Scene Link Details...** and send `revision-2.mslink` with `--base stale`.
   Expect `stale_revision: register_again`. The panel shows **Resync required** and
   the previously accepted scene stays intact. An incorrect pairing code or a
   malformed manifest also produces a specific rejection.
8. Click **Stop Scene Link** in the panel. It reports that the link stopped and
   Preview was retained. The old endpoint refuses further connections. Close the
   panel and start again: its code and session epoch change. Register and send a
   fresh snapshot with a new resulting revision to resume. Old deltas cannot cross
   the new pairing without a fresh baseline.
9. Save to a test `.mstage`, reopen and pair again. The saved scene and overrides
   remain available offline. Source registration reports the saved revision;
   source resource registration and transaction receipts require the new session.

Do not use SWAP to test step 04: synchronized Program publication is step 08.
An attachment used by Program is rejected so Preview updates cannot change it
indirectly. Stop revokes the pairing but does not remove accepted objects or caches.

## Recorded verification, 2026-09-09

Both GUI Debug and Release builds passed. Ten relevant CTest suites passed in each:
`net`, `net_director_session_service`, `scene_linked_scene`,
`director_link_service`, `linked_scene_ingest`, `blender_scene_reference`,
`render_model_loader`, `render_model_cache`, `gui_window`, and `editor_inspector`.
The new service suite uses real loopback TCP and the pinned GLB fixture. It covers
version and pairing failure, truncated/oversized frames, resource byte limits,
interrupted transfers, duplicate announcement/chunk/completion, receipt replay,
stale revisions, wrong scene/project, traversal, graph cycles, sparse coalescing,
restart, retained holds, ordered delete/restore and one-step Scene Undo.

Native UI verification observed initial pairing, snapshot acceptance and duplicate
receipt, held Hero placement across a geometry replacement, continued admission
with the details panel open, stale-base rejection and Stop refusing further
connections while retaining the scene. The initial native message box was replaced
with the live in-app panel after it was observed to block admission while open.
Menu pointer consumption was also fixed after a click reached an underlying scene
control. Save/reopen uses the existing linked-document persistence coverage;
manual checks 5, 6 and 9 are provided for operator follow-up rather than claimed as
observed in this UI run.

Evidence:

- [Held Hero after remote revision 2](evidence/step04-held-revision2.jpg)
- [Accepted revision with the live panel open](evidence/step04-live-panel.jpg)
- [Stale-revision resync with the scene retained](evidence/step04-stale-revision.jpg)
- [Stopped link with Preview retained](evidence/step04-stopped.jpg)
- Build and test logs are under `out/step04-debug-build.log`,
  `out/step04-release-build.log`, `out/step04-debug-tests.log` and
  `out/step04-release-tests.log`.
- [Debug test results](evidence/step04-debug-tests.txt) and
  [Release test results](evidence/step04-release-tests.txt) are retained here as well.
- Reference-client transcripts are under `out/step04-ui`. These are local test
  artifacts. The test pairing was revoked after verification.

The isolated editor's default scene already showed an unconnected compositor-input
diagnostic before the link was enabled. Link verification assessed scene membership,
geometry, held placement, receipts and unchanged Program output; it did not alter
that unrelated default-project configuration.
