# Step 02 UI walkthrough

Build the Debug and Release GUI presets from the repository README. Generate the
qualified step 01 model packages, build the linked project fixtures, and launch
the editor using isolated settings and a fresh WAL:

```powershell
.\scripts\run-blender-linked-scene.ps1 -Build debug
```

Use `-NoLaunch` to generate projects without opening an editor. The projects are
under `out/blender-scene-reference/linked`. The launcher chooses a startup project
using an isolated recent-project record, so your normal workspace/preferences and
recent project are preserved. Close the test editor before launching another
fixture on the same port, or specify a different `-DirectorPort`.

For the side-by-side Release build produced during implementation (the regular
Release executable is held open by an existing lifecycle service), use:

```powershell
.\scripts\run-blender-linked-scene.ps1 -Build release -EditorPath .\out\build\windows-msvc-release-gui\apps\server\media-server-linked-scene.exe
```

1. Select **Hero prop** in the Scene list. The Inspector's **BLENDER SCENE LINK**
   section shows **Hold shot placement (world)** and world position
   `(1.5, 1, -1)`. The source parent and Shared prop have their own persistent rows.
   This fixture omits the unqualified reflected drawable from step 01.
2. Edit **Shot position (world meters)** X to `2.0`. The prop moves in Stage and
   the local output. Undo restores X to `1.5`. Stage translation also uses the
   same affine placement planner. The matrix API supports complete affine edits;
   the step 02 Inspector exposes world translation and stage alignment. Full
   rotation/scale interaction and bidirectional placement remain steps 06–07.
3. Change **Link stage alignment** X from `0` to `1`. Held Hero remains at its
   world placement; Shared prop follows alignment. Undo the alignment edit.
4. Press **Follow source with offset**. Hero remains in place. Change stage X
   again: Hero now follows it. Undo, and switch back to **Hold shot placement**.
   **Reset placement to source** deliberately drops the shot offset.
5. Press **Duplicate this linked scene**. The new attachment has independent
   occurrence records. Select a duplicated prop and change its stage alignment
   or shot position to separate the attachments. The other attachment keeps its
   placement. Undo restores the duplication as one transaction.
6. Press **Hide this occurrence** or Delete. The linked row remains in the Scene
   list. Select it and use **Follow source visibility** to restore source
   visibility. Ordinary document undo restores the previous local decision.
7. Edit roughness in MATERIAL. It overrides roughness on the imported slots while
   keeping their colors and textures. **Reset material factors to source** clears
   the local factor. Save a copy with **File > Save Project As** and reopen it to
   check the chosen policy, position, visibility and material decisions.
8. Press PREVIEW and edit the rehearsal's Hero. Scene cloning creates fresh link
   instance/occurrence identities, so Program keeps its own shot decisions.
9. Close the test editor and launch
   `-Scene linked-r2`. The shared prototype has revision 2's sloped top and the
   source parent has changed, while held Hero remains at `(1.5,1,-1)`. These
   fixtures are produced using the source-update planner, not manual placement
   copies. A live Blender connection remains steps 03–05.
10. Launch `-Scene source-removed`. Hero is absent from rendering but its row reads
    **Hero prop (source removed)**, with **Source removed - shot retained** in the
    Inspector. **Discard retained shot record** is the deliberate recovery-record
    removal action. Launch `-Scene source-restored` to see Hero restored with the
    same document identity, policy and placement.

The linked-scene regression suite separately checks invalid hierarchy, singular
and nonaffine transforms, origin rebase rejection, animated-parent rejection,
save/reopen digests, property-level material application, source duplication,
delete/restore, Preview cloning and last-valid-revision retention. UI inspection
results and build/test evidence are recorded in the step 02 implementation page.
