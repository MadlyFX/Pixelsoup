# Advanced Hybrid Renderer Integration Plan

## Recovered proposal

The renderer proposal was introduced in commit `686bffc` and was still present
in `dc34c3d`, immediately before commit `39c50c6` rewrote
`media-server-architecture-plan.md` as a description of the implemented system.
The historical text can be read with:

```powershell
git show dc34c3d:media-server-architecture-plan.md
```

That proposal did not describe two complete engines running beside each other.
It recommended one renderer owned by this project, assembled from three
permissively licensed sources:

- [SpartanEngine](https://github.com/PanosK92/SpartanEngine) as the primary
  renderer and native GPU foundation: bindless resources, GPU-driven geometry,
  universal HLSL, ReSTIR GI, ray-query reflections and shadows, TAAU, motion
  blur, HDR, profiling, and crash breadcrumbs.
- [Atlas-Engine](https://github.com/tippesi/Atlas-Engine) as the model for an
  embeddable/headless lifetime and as the source for DDGI concepts and shaders.
- [hybrid-rendering](https://github.com/diharaw/hybrid-rendering) as a reference
  for ray-traced shadows, AO, reflections, GI, SVGF temporal accumulation, and
  A-trous filtering in a deferred pipeline.

Research for this plan used Spartan commit
`4842e83cecaee6f2bb86cdc1aa183e881569a8dc`, Atlas tag `0.2.0` at
`76e4916e55706c0f6eac971d17d65bd8feacf70e`, and hybrid-rendering tag
`1.1.0` at `090360e0b231e6df38f0c0ce0b7465c8f8485b0e`. These are research
pins, not dependency declarations; implementation should pin reviewed commits
in a provenance manifest.

## Integration steps

1. **Freeze source provenance and licensing.** Create an internal Spartan fork,
   pin all three reviewed revisions, retain their MIT notices, inventory every
   copied source and shader, and add a provenance file recording the upstream
   path, commit, local destination, and material modifications. Do not import
   Atlas's differently licensed sample data or any demo assets whose production
   rights have not been verified.

2. **Build and capture the upstream references unchanged.** Run Spartan's
   D3D12 and Vulkan renderers, Atlas's headless renderer and DDGI sample, and
   hybrid-rendering's individual RT/denoise views on the intended GPUs. Save
   representative captures, GPU timings, feature/extension requirements, and
   reference images so later ports can be compared with known upstream output.

3. **Treat the projects as source donors, not product dependencies.** Do not
   link two engine runtimes or add their root builds to this repository.
   Spartan currently assumes engine-owned/static subsystems and a swapchain;
   Atlas's root build still pulls application/game dependencies even when its
   headless flags are used; hybrid-rendering is an application sample. Extract
   only the GPU abstractions, rendering code, and shaders that survive the
   contracts below. Keep Media Server's document model, command bus, app loop,
   native editor, asset identity, compositor, and output ownership.

4. **Define the host-facing renderer contract before porting code.** Add an
   interface above `SceneRenderer` that accepts a `scene::Scene`, exact
   `CanvasRect`, output extent, replicated show-frame ordinal, render profile,
   and explicit history key. Return scene-linear color plus the depth, velocity,
   object-ID, and coverage products required by compositing and picking. No
   upstream world, entity, window, clock, input, or swapchain type may cross
   this interface.

5. **Keep the existing renderer as an explicit backend.** Introduce renderer
   selection such as `Basic` and `Advanced`, leave the current SDL_GPU
   `SceneRenderer` available for unsupported machines and regression
   comparison, and make selection replicated project/deployment policy. If a
   project requires `Advanced`, missing capabilities must fail Program
   readiness; they must not silently select `Basic` or disable a pass.

6. **Turn `rhi::Device` into a real multi-backend seam.** Move the existing
   SDL_GPU implementation behind a backend implementation object, replace the
   assumption that `backend_handle()` is an `SDL_GPUTexture*` with typed native
   interop handles, and add backend selection/capability reporting. Preserve
   the current public operations while their callers migrate.

7. **Extend the RHI only from concrete renderer requirements.** Add scene-linear
   `RGBA16F` and optional `RGBA32F`, sampled depth, integer/object-ID targets,
   storage images, texture arrays and mips, typed/structured/indirect buffers,
   compute pipelines, push constants, bindless descriptors, timestamp queries,
   explicit resource states/barriers, graphics and compute queues, fences, and
   bottom/top-level acceleration structures. Add full ray-tracing-pipeline/SBT
   support only for a pass that cannot use Spartan's ray-query path.

8. **Land a native D3D12 backend first and retain a Vulkan-shaped contract.**
   Adapt Spartan's D3D12 device, queues, resources, descriptors, acceleration
   structures, command lists, and breadcrumbs into the Media Server RHI rather
   than exposing Spartan globals. D3D12 matches the current Windows/DXIL path,
   the planned NVAPI present barrier, and future D3D11 texture interop for
   Notch/Spout. Keep universal HLSL and backend-neutral synchronization so the
   Vulkan implementation can follow without changing `engine/render`.

9. **Replace the shader build with a reproducible universal-HLSL pipeline.**
   Pin DXC, compile the same source to DXIL and SPIR-V, support vertex,
   fragment, and compute entry points, generate resource-layout metadata, and
   make warnings fatal. Port selected Atlas GLSL and hybrid-rendering GLSL to
   the shared HLSL conventions instead of maintaining parallel material and
   lighting implementations. Record compiler version and flags in build output
   so every render node runs identical bytecode.

10. **Add a render graph owned by Media Server.** Represent raster, compute,
    copy, acceleration-structure, denoise, composite, warp, and output passes as
    declared resource reads/writes. Use it to derive transitions, transient
    allocations, queue synchronization, and async-compute overlap. The graph
    must accept imported media textures and export its final texture to the
    existing UI/presenter path without a CPU readback or cross-device copy.

11. **Build a deterministic scene bridge instead of a second scene graph.**
    Translate evaluated `scene::Scene` values into stable GPU records keyed by
    document node and content identity. Diff transforms, materials, lights, and
    assets between frames; never rebuild an upstream ECS. Maintain global
    geometry/index/material/instance buffers, create meshlets, build one BLAS
    per immutable mesh, and update/refit a TLAS from stable instance order.

12. **Complete the authored data needed by the advanced pipeline.** Extend the
    headless scene schema, persistence, hashing, wire behavior, editor, and tests
    for physical camera exposure and shutter, photometric light units and color
    temperature, spot/area/emissive lighting, environment maps, normal and
    occlusion maps, alpha mode/cutoff, double-sided materials, and the material
    properties actually consumed by the port. Add tangents and the remaining
    glTF texture views to `model_loader`; defer USD/MaterialX to its own ingest
    boundary rather than coupling it to renderer bring-up.

13. **Establish a scene-linear color contract before matching features.** Render
    and composite in premultiplied linear `RGBA16F`; decode input textures at
    their declared transfer, preserve values above 1.0 through the compositor,
    and perform tone mapping/output transfer only after scene mixing, spatial
    effects, projection warp, and feed mapping. Move `SceneMixer`,
    `CompositorSceneExecutor`, `ProjectionMeshWarp`, and `OutputTransform` to
    formats that honor this contract before enabling HDR or OCIO.

14. **Port the Spartan raster foundation.** Bring over vertex pulling, global
    geometry, material and instance buffers, meshlet generation/culling, depth
    prepass, G-buffer, clustered lighting, opaque/transparent handling, IBL,
    shadow maps, motion vectors, and the shared BSDF. Match Media Server's
    reversed-depth projection and exact sub-frustum math rather than importing
    Spartan's camera or swapchain conventions.

15. **Port ray tracing as an optional capability layer.** Add BLAS caching,
    deterministic TLAS instance order, alpha-tested hit behavior, ray-query
    shadows, reflections, and AO. Each pass must have an authored quality
    policy and a declared non-RT fallback. Capability negotiation belongs in
    readiness and diagnostics, not in per-frame heuristics.

16. **Port Atlas DDGI into the owned renderer.** Reimplement Atlas's irradiance
    volume/probe state, trace/update, border copy, relocation/classification,
    and sampling against the new RHI and HLSL conventions. Add replicated
    settings for probe bounds, counts/cascades, ray budget, update schedule,
    and a baked/snapshotted probe asset identity. For distributed outer-frustum
    rendering, update the full probe volume from full-scene state with
    show-frame-keyed seeds, or distribute one verified probe snapshot; never
    derive probe history from a node's local slice.

17. **Confine Spartan ReSTIR to render products with one history owner.** Use
    ReSTIR GI for a single-producer inner frustum or a single-GPU full surface.
    Do not enable independent temporal reservoirs on adjacent output slices.
    Key reservoirs and G-buffer history by scene, camera, output, extent, and
    profile, and reset them on seek, discontinuous/repeated show frame, project
    switch, camera cut, resize, relevant scene topology change, or quality
    change.

18. **Port denoising algorithms, not the hybrid-rendering framework.** Use its
    SVGF reprojection, luminance moments, variance estimation, tile
    classification, A-trous filters, hit-distance reprojection for reflections,
    and upsampling as reference implementations. Feed them the advanced
    renderer's depth, normals, velocity, object identity, and hit distance.
    Make history reset and rejection explicit and verify each pass in isolation
    against the captured reference outputs.

19. **Make all temporal work show-frame deterministic.** Derive jitter, sampling
    rotations, noise, and update schedules from the exact 64-bit show frame and
    stable scene/object identity, never wall clock, submission order, or
    slice-local pixel coordinates. Store previous transforms from the previous
    evaluated show frame. Treat a held/repeated frame as a hold, not as a new
    temporal sample. Apply camera-shutter motion blur only after that contract
    is testable.

20. **Render padded slices and crop at the output boundary.** Continue using
    `scene::view_for_slice`, but allocate a calculated apron for filters,
    reflections, TAAU, bloom, and denoising. Express random/sample positions in
    absolute canvas coordinates, render identical overlapping pixels on
    adjacent nodes, and crop only after neighborhood-dependent passes. Keep
    full-scene TLAS and DDGI inputs independent of the output slice.

21. **Refactor composition around advanced render products.** Stop
    `CompositorSceneExecutor` from invoking a complete temporal 3D pipeline once
    per isolated object. Render ordinary 3D geometry once with shared depth;
    carry existing inline per-object effects in material/object records; use
    object-ID/coverage and depth products for isolated spatial effects; and keep
    media, mattes, masks, layer blends, transitions, and projection/output
    passes in the Media Server compositor. Any effect that cannot preserve the
    invariants in `effect-rendering-requirements.md` must return
    `InvalidConfiguration` until its color-plus-depth behavior exists.

22. **Preserve independent Program, Preview, Stage, and transition histories.**
    Give every surface its own render targets, camera state, temporal resources,
    and graph instance while sharing immutable mesh, material, texture, and BLAS
    caches. Bound this residency, expose it in diagnostics, and destroy it in an
    order that cannot leave GPU resources referring to a dead device.

23. **Adopt Atlas's host-owned lifetime as an enforceable rule.** The app creates
    a device and renderer, supplies every frame request, owns every window and
    swapchain, and explicitly drains and destroys GPU work. Support offscreen
    creation with no window and no application main. Do not port SDL audio,
    input, ImGui/editor code, ECS/serialization, scripting, physics, Assimp, or
    either donor engine's application loop.

24. **Reconnect presentation and professional interop through the same native
    device.** Make editor viewports sample advanced targets directly, make node
    outputs pass through `SceneMixer`, warp, feed-map, and output transfer, and
    implement native swapchains behind the current presenter seam. Then connect
    the existing `PresentBarrierBackend` contract and later external-memory/
    fence handles for D3D11, Spout, video I/O, or ST 2110 without introducing a
    second render device.

25. **Add explicit resource, scheduling, and degradation policy.** Preallocate
    frame-critical targets and descriptor ranges, budget histories and BLAS/TLAS
    memory, upload assets asynchronously, and prohibit blocking file or shader
    work on the control/render loop. Define which optional passes may be
    disabled by an operator-selected profile; a runtime over-budget condition
    reports/holds according to sync policy instead of silently changing visual
    quality on one node.

26. **Add production diagnostics with the renderer.** Record CPU and GPU pass
    timings, queue overlap, transient and persistent memory, descriptor usage,
    AS build/update cost, history resets, shader/PSO hashes, capability choices,
    apron size, dropped work, and present timing. Wire Spartan-style breadcrumbs
    and device-loss reports into node health so a GPU fault produces a useful
    post-mortem rather than only a black output.

27. **Build conformance and image regression suites before switching Program.**
    Add RHI conformance tests per backend; headless/offscreen creation tests;
    scene-bridge and stable-order tests; shader math tests; GPU reference images
    for material, light, shadow, RT, DDGI, ReSTIR, denoise, TAAU, motion blur,
    alpha, and color; and current effect/depth invariant tests against both
    renderers. Compare a full-canvas render with two- and four-node padded
    slices at their shared pixels, repeat frame `N` twice, seek away and back,
    restart a node, and verify that every declared deterministic mode converges
    to the specified result.

28. **Gate activation through measured hardware validation.** Exercise the
    supported NVIDIA/AMD adapter and driver matrix, D3D12 first and Vulkan when
    implemented, at the actual canvas sizes, frame rates, media load, output
    count, and transition/effect load. Capture RenderDoc/PIX/Nsight traces and
    establish per-pass budgets. Enable the advanced backend in Preview/Stage
    first; permit Program only when capability, shader hash, asset readiness,
    sync state, and renderer self-checks all pass.

29. **Document ownership and the upstream update process.** Update the current
    architecture/status document when each seam is real, keep the historical
    proposal identified as research rather than present capability, publish the
    supported feature/backend matrix and color/temporal contracts, and require
    upstream updates to be re-audited and replayed through the renderer
    conformance suite instead of merging donor repositories wholesale.
