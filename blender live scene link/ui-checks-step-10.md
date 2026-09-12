# Step 10 — Materials and lighting checks

Qualified on Windows with Blender **5.2.0 LTS, fbe6228777e7**, add-on **0.5.0**
and the D3D12 Advanced renderer. The [translation contract](materials-and-lighting-v1.md)
defines supported graph patterns, color spaces, photometry, identities and limits.

## Try the Director reference scene

Build the Debug and Release GUI presets using the repository README. Then run:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/run-blender-look.ps1 -Build release
```

The launcher generates and qualifies an isolated `out/step10-*` fixture and prints
its directory and Director PID. It uses separate local settings. `-NoLaunch`
generates the fixture and acceptance evidence without opening the editor.

1. Click **PREVIEW**. **Look Preview** has a linked collection, a Material panels
   parent, Copper and Paint surface children, Key rectangle and Sun. The two
   triangular panels render in Advanced. The deliberately simple shapes make
   surface identity and bounds easy to inspect.
2. Select **Material panels / Copper** in the Scene tree. Its tight selection
   outline covers only Copper. **LINKED MATERIAL FACTORS** shows base RGB
   0.800 / 0.300 / 0.100, roughness **0.900**, metallic 0.700 and emissive RGB zero.
   The source roughness is 0.350; this fixture retains a 0.900 shot override through
   a texture edit and material slot reorder. Paint retains its own 0.350 roughness.
3. Select **Key rectangle**. The native **LIGHT** panel shows the 900-lumen shot
   intensity, two-metre width, one-metre height and 20-metre cutoff. Units and kind
   identify Blender ownership. Click **Reset light properties to source**:
   intensity returns to **1800.000**, and **Overrides: none** appears. Source pose
   is world (0, 3, 2), aimed downward. The Sun is a separate 400-lux native light.
4. The light reset affects indirect lighting, so **STALE LIGHTING** appears in the
   status bar. Select **Environment**, collapse Atmosphere, and expand **BAKED
   LIGHTING**. The warning is wrapped in the Inspector and tells the operator to
   rebake or disable GI/reflections before publishing. Existing derived lighting
   remains visible in Preview while stale.
5. Set **Bake path** to a disposable directory and click **Bake lighting**. The
   existing worker bakes GI and the enabled reflection capture. A successful
   completion authors a new resource and input revision; the stale warning clears.
   For this fixture, Copper's **Static geometry** checkbox is cleared because the
   offline integrator requires opaque static surfaces. Paint is the static contributor.
6. Select Paint and change **Shot position (world meters)**. Lighting becomes
   stale again. For the completion-race check, request a bake, then move Paint
   again before it completes; a completion for the previous input revision is
   discarded. Increasing the grid/sample count gives more time for the manual
   gesture. The automated worker check performs this sequence reproducibly.
7. Use the [step 08 publication controls](ui-checks-step-08.md) to publish. A stale
   candidate is refused and Program retains its previous complete revision.
   Rebake first, or explicitly disable both baked GI and every reflection capture.
   Matching geometry, images, lights and probe bytes stage as one resource cohort.
8. Save and reopen to check retained source values, shot overrides and currentness
   without Blender. Merely reopening or preparing identical assets must not mark
   lighting stale. `published.mstage` is the launcher's ready frozen snapshot;
   `stale.mstage` is a deliberately moved static-contributor fixture.

The launcher checks numeric material extension values and package identities before
opening the UI. It includes mask alpha, clearcoat and thin transmission on Copper;
these are source-authored properties, not additional shot controls. The two panels
are a translation reference rather than a Cycles/Eevee image comparison.

## Try Blender authoring

Package/install 0.5.0 using the [integration README](../integrations/blender/README.md),
or start this disposable scene without changing saved preferences:

```powershell
& 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe' `
  --factory-startup --python scripts/blender-livelink-demo.py
```

1. Dismiss the startup card. Open the 3D View sidebar with **N** and select
   **Media Server**. The demo selects a UV-mapped mesh with one Principled material.
   **Materials and lighting** shows light efficacy, alpha handling and the explicit
   **Bake Base Color Copy** action.
2. Choose **Mask** in **Alpha handling**. The cutoff control appears. Opaque is the
   default; alpha below one or a linked alpha socket requires Mask or Blend.
   Transparent materials still use the existing Advanced transparency path.
3. Connect to a fresh Director Preview using the
   [pairing walkthrough](ui-checks-step-05.md). Add saved PNG/JPEG images with the
   exact graph patterns in the contract: sRGB base/emission, Non-Color tangent
   normal and packed green roughness / blue metallic, all on the render UV map.
4. Save a change to one image, then click **Publish Now**. This forces worker
   content re-reading. Only the surface packages that use the changed image are
   rebuilt. Reorder material slots while keeping the same material on each face:
   stable surface IDs and Director overrides follow the materials.
5. A procedural Base Color produces a specific baking diagnostic. On a mesh with
   one material and render UVs, click **Bake Base Color Copy**. The separate Blender
   worker bakes albedo, then assigns a supported material copy and retains the
   original material. Save its generated PNG with the project. Edits made while
   baking, even while disconnected, discard the stale result.
6. Add a Sun, Point, Spot or square/rectangle Area light. Set Point/Spot radius to
   zero and leave Normalize enabled. Set **Light efficacy (lm/W)** explicitly;
   Sun W/m2 becomes lux, local W becomes lumens. Select a light to set **Light
   cutoff (m)**. Its pose and supported dimensions appear in the Director's native
   Inspector. A power/dimension edit creates no mesh GLB.

Other shader graphs, unsupported image coordinates, packed/dirty images and
unsupported light semantics are diagnosed. The current translator does not promise
full Blender shader, shadow, volume or animation interchange.

## Recorded qualification — 2026-09-11

Both full GUI builds succeeded. **17/17 targeted tests passed in Debug and Release**:
material fixtures/unit tests, linked look CPU/GPU, baked lighting, linked identity,
manifest ingest, Director service, publication, environment CPU/GPU, publisher core,
Inspector, viewport and workspace checks. The Advanced look test requires ready
textures, native lights, GI and reflections with no failed, pending or dropped models.

The linked-look acceptance verifies independent texture ingestion, zero new packages
after slot reorder, source/shot light reset, transformed lights, independent copy/paste, native unlink,
persisted input hashes, a requested worker bake followed by another static edit,
stale publication refusal and complete geometry/lighting publication verified by
two cohort readers. This does not add a physical two-machine frame-timing claim.

Installed add-on integration runs against the real C++ Director passed **7 look
checks in each configuration**, including all four light types and actual external
Blender baking. The same run serializes three surface packages: two initial
surfaces and one changed image. Regressions passed 43 shot-sync checks and eight
environment checks in each configuration, plus 20 baseline publisher checks in Debug.

Native Release UI checks verified the Copper factor override and compact bounds,
source light reset, stale-lighting status, and a successful Inspector rebake to a
new destination directory. **Bake complete. Ready in Stage.** replaced the stale
warning. Bake failures now remain visible alongside stale status. Blender's native
panel was inspected, Mask exposed its cutoff, and **Bake Base Color Copy** completed
and reported assignment of the supported copy.

Evidence:

- [Debug build](evidence/step10-debug-build.log), [Release build](evidence/step10-release-build.log)
- [Debug tests](evidence/step10-debug-tests.log), [Release tests](evidence/step10-release-tests.log)
- [Debug Advanced GPU](evidence/step10-debug-gpu.json), [Release Advanced GPU](evidence/step10-release-gpu.json)
- [Debug Blender materials](evidence/step10-debug-blender-materials.json), [Release Blender materials](evidence/step10-release-blender-materials.json)
- [Debug shot regression](evidence/step10-debug-blender-shot.json), [Release shot regression](evidence/step10-release-blender-shot.json)
- [Debug environment regression](evidence/step10-debug-blender-environment.json), [Release environment regression](evidence/step10-release-blender-environment.json)
- [Baseline publisher regression](evidence/step10-debug-blender-publisher.json)
- [Material Inspector](evidence/step10-director-material.jpg), [Light reset](evidence/step10-director-light-reset.jpg), [Stale Inspector](evidence/step10-director-stale-lighting.jpg)
- [Completed native rebake](evidence/step10-director-rebake-complete.jpg), [Blender alpha controls](evidence/step10-blender-material-panel.jpg), [Blender baked copy](evidence/step10-blender-bake-copy.jpg)
