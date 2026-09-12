# Step 03 UI checks

Build the GUI configuration, then generate the portable sample manifests:

```powershell
python scripts/generate-linked-scene-manifests.py
.\scripts\run-blender-scene-reference.ps1 -Build debug -DirectorPort 45173
```

The launcher uses isolated settings and a fresh WAL. It opens the step 01
reference project as visual context; your usual project settings are preserved.
The samples appear in `out/blender-scene-ingest`.

1. Click **PREVIEW** at the top right. A rehearsal scene is created. Program
   continues to display the original reference image.
2. Choose **File > Import Linked Scene...**, then `revision-1.mslink`.
   The scene list gains Placement parent, Hero prop, Shared prop and Unchanged
   prop. On a cold cache the status reports **1 prototypes ingested, 1 reused**:
   two different prototype identities initially share the same GLB content.
3. Select the newly imported Hero prop (the second Hero row in this reference
   project). Its Inspector shows **BLENDER SCENE LINK**, with X=-2, Y=0.5, Z=0.
   Click **Hold shot placement**. The policy changes to Hold.
4. Import `revision-2.mslink` using the same File command. Hero and Shared use
   revised geometry; Unchanged prop retains the old mesh. Hero stays at
   X=-2, Y=0.5, Z=0 and remains held. A cold revision reports **1 ingested, 1
   reused**. Reimporting a warm revision reports **0 ingested, 2 reused**.
5. Import `placement-only.mslink`. The status reports **0 prototypes ingested,
   2 reused**. The following props shift one metre in X; held Hero stays still.
   Program's reference image remains unchanged.
6. Import `missing-resource.mslink`. Expect **LINK IMPORT REJECTED**, with a
   missing-resource diagnostic. Object count, held policy, positions and geometry
   remain those of the last successful import. Repeat with `malformed.mslink`;
   unknown content is rejected without a partial hierarchy.
7. Use **Edit > Undo** once. Because failed imports add no history, this reverses
   the last successful placement-only revision as one action. The held policy
   remains. Redo restores that revision.
8. Save As a new test `.mstage` and reopen it. Linked identities and overrides are
   ordinary project properties; the cache packages keep it playable offline.
9. During a larger import, **File > Cancel Linked Scene Import** discards it.
   Selecting another manifest supersedes the pending request. The small samples
   usually finish too quickly for a meaningful manual cancellation race; the
   automated suite checks cancellation and latest-request acceptance.

Imports outside Preview are refused. A Preview attachment sharing a source with
Program is also refused; that case requires step 08's publication workflow.

## Recorded verification, 2026-09-09

The native Debug editor was launched and operated through the File menu. Checks
1–5 and the missing-resource portion of check 6 were observed. Revision replacement
after Hold initially exposed reused transaction IDs; the implementation was fixed,
rebuilt and the sequence retested successfully. The Director regression test now
covers import → shot edit → revision update → one-step undo.

| Evidence | Observed result |
| --- | --- |
| [Initial import](evidence/step03-import-r1.jpg) | Complete hierarchy, 1 ingested / 1 reused |
| [Revision 2 with held placement](evidence/step03-import-r2-held.jpg) | New geometry, same held position, Program unchanged |
| [Placement-only reuse](evidence/step03-placement-reuse.jpg) | 0 ingested / 2 reused, held Hero stationary |
| [Rejected resource](evidence/step03-rejected-resource.jpg) | Last complete hierarchy retained; screenshot predates the shortened missing-resource wording |

Both Debug and Release GUI builds passed. Seven relevant CTest suites passed in
each: `scene_linked_scene`, `linked_scene_ingest`, `render_material_ingest`,
`blender_scene_reference`, `render_model_loader`, `render_model_cache`, and
`editor_inspector`. Save/reopen, graph rejection, supersession, corrupt-cache
rejection and undo invariants have automated coverage; not every optional manual
walkthrough action above was exercised in this UI session.
