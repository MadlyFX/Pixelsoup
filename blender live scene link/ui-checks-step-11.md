# Step 11 — Animation and stage context checks

Qualified with Blender **5.2.0 LTS, fbe6228777e7**, add-on **0.6.0** and the
Advanced D3D12 renderer. See [the contract](animation-and-stage-v1.md) for the
supported node/skin cache, timing, assembly placement, stage axes and budgets.

## Director: offline animation and shot placement

Build the Debug and Release GUI presets, then run:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/run-blender-animation.ps1 -Build release
```

The launcher generates a disposable fixture and runs its acceptance checks. It
prints the output directory and Director PID, and uses isolated local settings.
`-NoLaunch` generates and checks the fixture without opening an editor. If your
checkout does not include the README's build helper scripts, use `cmake --build
--preset windows-msvc-debug-gui` and `cmake --build --preset windows-msvc-release-gui`
from a Visual Studio developer shell.

1. Click **PREVIEW**. The **Animation Preview** scene contains an offline linked
   collection and **Animated assembly**, plus an authored camera. Its two small
   triangular surfaces exercise rigid-node and skin bindings in one package.
2. Select **Animated assembly**. Inspector shows **Hold shot placement (world)**
   and shot position **X 3, Y 0, Z 0**. Scroll to **MODEL ANIMATION**. **Animate
   model** and **Loop playback** are enabled; the start frame is **10**. The
   assembly is identified as dynamic node/skin geometry.
3. Click **START AT FRAME ZERO**. The displayed start frame changes to **0**.
   Click **PLAY** in the Timeline. The internal parent moves while the assembly's
   outer X=3 shot placement remains fixed. Blender is not required. Pause and seek
   using the Timeline to compare poses. For start 10, show frames 10 and 34 are
   identical loop poses; frame 22 is halfway through the one-second clip.
4. Disable **Loop playback** and seek beyond the clip end: the last pose holds.
   **Animate model** controls the internal pose; Hold/Follow controls assembly
   placement. A held placement continues playing its internal animation.
5. Save/reopen the project. Its immutable package, effective start/loop settings
   and shot placement remain available. Independent Copy/Unlink also preserves
   animation settings. Animated assemblies stay outside static GI baking.
6. Use the [step 08 publication controls](ui-checks-step-08.md) to stage and
   publish Preview to Program. The animation package is part of the same complete
   resource cohort as the scene; each output samples its committed show frame.
   Changing a Blender clip afterward changes Preview, while Program keeps its
   frozen revision until the next explicit publication.

## Blender: publish a clip and inspect references

Build/install the ZIP using [the integration README](../integrations/blender/README.md),
or launch this disposable scene without saving add-on preferences:

```powershell
& 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe' `
  --factory-startup --python scripts/blender-livelink-demo.py -- --animation
```

1. Dismiss the startup card. Open the 3D View sidebar with **N** and select
   **Media Server**. The demo has a moving parent and an applied bevel mesh.
   **Animation and stage context** shows **Animated assembly**, source bake range
   **1–25**, **Start at show frame 10**, and **Loop clip**.
2. In a Director Preview, choose **File > Start Scene Link**, then **Scene Link
   Details > Copy details**. In Blender click **Paste Director Details** and
   **Connect & Publish**. The explicit bake runs on a separate Blender process.
   Wait for **Accepted in Preview** and an accepted revision. A collection is one
   assembly; its internal meshes and bones remain editable in Blender.
3. Edit a key or mesh and click **Publish Now**. The old accepted clip remains
   available while the new one exports. For a longer export, edit a source vertex
   before it finishes: the completion is discarded and the accepted revision
   remains unchanged. Click **Publish Now** to bake the current source again.
4. Select **Animation assembly placement** in Blender's Outliner to use the
   existing shot-layer controls. A Director shot adjustment returns on that
   managed empty; the keyed parent channels retain their source values. Changing
   Blender's frame does not send pose revisions to the Director.
5. Enable **Stage references**. A separate **Media Server · Stage references**
   collection holds the managed camera and physical-stage origin. The panel shows
   the selected Director show frame and rational rate. With an active physical
   camera it uses the topmost drawing camera; otherwise it uses the authored view.
6. Click **View Director Camera**. This viewport uses the reference camera and
   its sensor aspect. The action also sets Blender's render aspect to that gate;
   passive camera updates do not change render settings or the Blender playhead.
   Use **Show Source Placement** / **Show Shot Placement** to inspect alignment
   without double-applying it. The origin axes and camera are excluded from publication.
7. Disconnect. Media Server continues rendering the saved animation. The managed
   Blender objects retain their last reference pose. Lens distortion, wall slices,
   projection mapping and final calibrated output remain Media Server functions.

For a custom skinned assembly, keep its complete hierarchy inside the collection,
use metre unit scale, parent the mesh to its armature, use linear vertex-group
weights with at most four influences, and apply other mesh modifiers first.
Unsupported simulations, vertex caches, preserve-volume deformation and reflected
or sheared sampled transforms produce diagnostics.

## Reproducible qualification

Both full GUI builds and **21/21 targeted CTests in each configuration** cover
linked animation/stage fixtures, model animation CPU/GPU, source identity,
ingestion, Director protocol, publication, materials, environment and editor controls.
The installed Blender integration exercises node animation, an evaluated transform
constraint, skinning, returned assembly placement, no frame-by-frame source feedback,
stage/camera references, stale export rejection and offline persistence.

```powershell
python scripts/test-blender-livelink.py --build debug --animation --output out/my-step11-debug
python scripts/test-blender-livelink.py --build release --animation --output out/my-step11-release
out/build/windows-msvc-release-gui/engine/render/mediaserver_linked_animation_stage_tests.exe `
  out/my-step11-release/accepted.mstage out/my-step11-renderer --gpu
```

Each integration output directory must be new. The installed add-on records five
Blender-evaluated pose bounds. The renderer checks the same rigid/constraint/skin
poses within **0.0001 m**, compares independent readers at rational show frames
after seeks and restart, checks loop/clamp and held shot placement, verifies
publication resources through two readers, and renders the actual package in
Advanced D3D12. This is local process/resource qualification; it does not claim
physical two-machine scanout or tracker-hardware calibration.

Native UI checks exercised Director Preview selection, the linked Inspector start
control and offline Timeline playback. Blender's native sidebar completed
**Connect & Publish**, showed show frame **12** at **24/1**, created the separate
managed references, and **View Director Camera** displayed the expected reference
gate. Static regressions passed the existing shot, material/light and environment
Blender exercises.

Evidence is retained in `evidence/step11-*`: build/test logs, installed Blender
reports, renderer numeric/GPU reports, and
[Director playback](evidence/step11-director-animation-playback.png) /
[Blender reference camera](evidence/step11-blender-reference-camera.png) screenshots.
