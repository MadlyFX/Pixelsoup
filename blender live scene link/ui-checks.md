# Step 01 UI walkthrough

Build both GUI presets as described in the repository README. From the repository
root, the following command validates the checked-in fixture, generates model
packages and `.mstage` projects, and opens revision 1:

```powershell
.\scripts\run-blender-scene-reference.ps1 -Revision 1
```

Use `-Build debug` for the Debug editor. The launcher uses its own machine-local
settings directory and a fresh WAL under `out/blender-scene-reference/qualified`.
The usual recent project, workspace and preferences are preserved. Close the
reference editor before launching another revision on the same port (or pass a
different `-DirectorPort`). `-NoLaunch` generates the artifacts without opening
an editor; `-Benchmark` also writes the explicitly requested D3D12 measurements.

1. The Scene panel should show **Blender reference**, **Hero prop**, **Shared prop**,
   **Mirrored prop**, **Camera** (the composition camera), **Floor**, **Reference key**, and
   **Placement parent**. The hidden occurrence must not draw. The top two visible
   positive-scale props have cyan top faces and copper sides.
2. Select **Hero prop**. Its local position is `(1,1,0)`, scale `(2,0.5,1)`, under
   the parent at `(1,0,0)` with yaw 90 degrees. Its world origin is `(1,1,-1)`.
   The Program Output floating panel shows the fixed composition camera; the
   main Stage view may start in Free Orbit.
3. Select **Renderer** and inspect the Advanced renderer selection. The fixture
   has a connected floor-to-Output compositor graph; other drawables are retained
   by the existing selective Output behavior.
4. Press **PREVIEW**. The editor creates a **Blender reference Rehearsal** scene.
   Move Hero prop using the Stage gizmo or Inspector, and confirm Program remains
   at its previous composition. Undo the Preview move. These are existing local
   editing controls; no connected Blender status is expected in step 01.
5. Close this test editor, then launch `-Revision 2`. The cyan top is now sloped
   because the mesh's local +X top edge rose 0.25 m. The named objects retain the
   same transforms. Inspect **Hero prop** again to confirm the source package
   changed while the position, parent and scale stayed the same.
6. For the automated moved-prop/save-reopen case, open
   `out/blender-scene-reference/qualified/placement-retained.mstage` with
   **File > Open Project**. Hero's local X is 1.5, its world origin is `(1,1,-1.5)`,
   and it references revision 2. The checker verifies this exact state through
   the document delta and real project save/load code.

Generate the editable Blender scenes with:

```powershell
.\scripts\run-blender-scene-reference.ps1 -GenerateBlender -NoLaunch
```

Open `out/blender-scene-reference/reference-r1.blend` or `reference-r2.blend` in
the pinned Blender 5.2.0 LTS. View through **Composition camera** (Numpad 0 with
the viewport focused). Select Hero prop and Shared prop in turn: both use the
same mesh datablock. Hero's **Object Properties > Custom Properties** contains
`ms_link_id`; the name has changed from **Prop before rename** without changing
that value. Inspect the Copper and Cyan material slots and the sloped top in
revision 2. The Blender light is only an inspection aid; lighting/display pixels
are not expected to match the Media Server directional-light view exactly.

## Recorded inspection and qualification gaps

Both revisions were generated/reopened in Blender 5.2.0 LTS. Revision 2 was also
visually inspected in Blender: the shared sloped geometry and material colors
were visible, including the mirrored prop's cyan top. The generated native
projects were launched in the Debug editor with the Advanced renderer; both
revisions were visibly rendered in the Stage and local Program panel. Revision 2
retained Hero's `(1,1,0)` local position and `(2,0.5,1)` scale. Preview created an
independent rehearsal scene; entering local X=1.5 moved the Preview prop while
Program stayed unchanged, and Ctrl+Z restored X=1.0. The reference node IDs avoid
the runtime transport/switcher IDs, guarded by compile-time assertions.

**Mirrored rendering is not qualified.** The mirrored prop's world positions,
normal vectors and material factors pass the importer oracle, but its cyan top
was absent/dark in the Advanced view. Expected appearance is the same outward
cyan top with reflected placement. This is a front-face/parity qualification
failure to resolve before enabling negative-scale linked occurrences; preserve
this fixture. The shader already computes instance determinant orientation for
tangents, while coverage uses its own facing/reflection state. Step 03/10 must
verify parity across color, depth, IDs, normals and shadow passes, including
reflections, before claiming support. Do not mask this failure by enabling
double-sided materials or changing the fixture's negative scale.

The Windows automation helper could open the native Open dialog but failed its
value-setting/accessibility coordinates; direct startup through the isolated
launcher was used for the visual check. The command-line checker separately
verifies persistence and placement retention. Manual File > Open and a complete
live reverse-placement workflow are not claimed as automated UI passes.

The first live connection, ownership controls, reverse Blender placement and
multi-node Program publication remain steps 02–08. A green step-01 CTest result
only qualifies the CPU fixture/import/document seams described here.
