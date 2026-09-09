# 27 — Reflections without ray tracing

Implemented on the Advanced D3D12 path. Basic reports that reflections require
Advanced. The feature has two independent controls in **File > Project Settings >
Environment > Baked Lighting**.

## Local probes

Up to four captures live in the scene's lighting asset. Each has an authored
document-node identity, world-space capture position, box half extents or sphere
radius, edge blend fraction and intensity. The node ID is a stable identifier;
the capture position is explicitly authored and does not follow object motion.
Captures are sorted by node ID. At each shaded point, influence comes only from
the authored volume. Overlapping weights normalize in that fixed order, with the
environment supplying the remaining weight. No camera-distance selection occurs.

`media-bake` and the editor's existing bake worker trace four fixed subpixel paths
per texel into a 256 × 128 scene-linear radiance panorama. They use the same static
geometry, lights, environment and diffuse transport as the existing lighting
baker. The immutable `.msgi` asset stores these captures with the diffuse field,
checks its complete payload and rejects duplicate IDs, malformed dimensions,
non-finite pixels, truncation and corruption. Diffuse-only version-1 files remain
readable; captures use version 2.

On adoption, the renderer uses `EnvironmentPrefilter` itself: the same GGX sample
sequence, six roughness mips, 256-pixel cube faces and BRDF integration as the
environment. All capture sources, filters and maps are allocated at renderer
construction. The existing shared probe worker, publication fence, sticky failure,
generation checks and Program asset commit govern loading. A canceled GPU
recording retries its uploads and BRDF preparation. A moved or missing capture
fails readiness and names the need to rebake.

Both box and sphere proxies correct the reflection direction by intersecting the
reflected ray with the proxy and looking from the capture position toward that
intersection. Base specular, clearcoat, sheen and the existing anisotropic lobe
consume the local radiance. Blend, extent, shape and intensity can be adjusted
without changing the capture; moving the capture or relighting its static
surroundings requires a new bake.

Undo/Redo preserves the current Program asset-activation metadata while reverting
authored settings. This prevents a completed activation from conflicting with a
probe edit's saved stage snapshot. Other authored-field conflicts remain strict,
and the generated commands carry the preserved metadata to every node.

## Planar reflection

One receiver object and one world-space plane are authored per scene. The extra
view uses the current resolved camera mirrored through that plane, including an
explicit tracked/physical-wall view. It excludes every part of the receiver and
clips triangles against the plane in hardware. A slice retains the common
full-canvas projection and changes only its raster window. On the receiver,
integer pixel loads preserve the identity between a point on the plane and its
mirrored projection.

The capture reads the complete scene before compositor isolation. Compositor
layers share that capture, so layer count does not add reflection passes. The
pass does not recurse. Exposure applies once at the final surface, and the
ordinary show-frame/held-scene contract also governs reflected objects. Missing
receivers, an unsupported backend, or construction limits are counted and named.

The receiver must be flat and opaque and must lie on the authored plane. Use a
reflective material: a metal or low-roughness dielectric. The current planar
capture is sharp; it does not convolve the reflected view for a rough surface or
simulate curved mirrors. Screen effects are applied to the main image, and
reflection-specific temporal motion, recursive mirrors and specular ray transport
inside the offline diffuse baker are outside this step.

## Operator checks

1. Select **Advanced** in Project Settings > Renderer.
2. For a floor, select its object in Stage. Open **Environment > Baked Lighting**,
   scroll to **Planar Reflection**, enable it and choose **Use Selected**. A floor
   at Y = 0 uses point `(0, 0, 0)` and normal `(0, 1, 0)`. Set its material metallic
   factor to 1 and roughness near 0.05, then Apply. A raised box should appear
   below the floor at its mirrored position.
3. For a local probe, select an object to give the capture a stable ID, enable a
   probe and choose **Use Selected**. Each enabled probe needs a distinct ID.
   Place the capture in empty space, set its half extents (or sphere radius in
   X), and select an output base path. **Bake Lighting** includes the captures.
   **Use baked diffuse lighting** controls the diffuse contribution independently.
4. Place a chrome sphere beside an emissive red wall. Mark the sphere Dynamic
   for baking if it is a prop, so the capture does not sit inside it. Bake again;
   the sphere should receive red from the local capture. Disable the probe and
   Apply to compare with environment-only specular.
5. Change the blend or proxy shape and Apply. Move the capture and Apply: the
   readiness message should request a rebake. **Rebake Lighting** publishes the
   complete replacement through the existing cohort activation path.

The command-line equivalent is:

```powershell
media-bake project.mstage output-base scene-id show-frame
```

## Verification

`mediaserver.render_reflections` covers non-default model fields and digests,
translated oblique-plane geometry, repeat-bake bytes independent of camera/frame,
local red-wall capture, corrupt/truncated payloads and duplicate-ID refusal.
Native GPU tests cover the mirrored box position, clipping below the plane,
receiver refusal, compositor-isolated geometry and capture reuse, canceled
uploads, red chrome, box/sphere proxies, moved-capture readiness, held images and
byte-exact full/adjacent-slice comparisons.

Both full Windows GUI builds and all 151 CTest entries passed in Debug and
Release. The native editor check used an isolated project with an emissive red
wall, chrome sphere, blue box and reflective floor. Apply toggles were compared
in Stage and Program; Undo/Redo after asset activation and Save were exercised.
The command-line bake produced and loaded the same
content-addressed lighting asset used by the editor.

The final editor shutdown fix keeps its device owner alive until all retained
GPU resources have been released. Both configurations were rebuilt afterward;
19 focused editor/reflection/unit/bootstrap tests passed in each, and native
editor runs completed asset activation and exited cleanly with code 0.

These GPU comparisons run on one local D3D12 adapter. A deployed multi-machine
wall remains a hardware verification boundary.
