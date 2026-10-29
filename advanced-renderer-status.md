# Advanced renderer capabilities and limits

Current implementation overview. For controls and appearance, see the
[renderer guide](docs/renderer.md); for data flow see the
[architecture](media-server-architecture-plan.md). The implementation and
colocated tests are authoritative when an older measurement describes a
different build.

## Implemented paths

- Basic uses SDL GPU; Advanced owns the Windows D3D12/DXGI raster path behind
  the common RHI. Selection and requested capabilities participate in project
  readiness. Scene-linear composition, crop, warp and output transform remain
  part of one document-controlled renderer.
- Director camera tracking, lens parameters, inner/outer frusta, deterministic
  pose prediction and optional filtering feed replicated show-frame poses.
  Nodes hold the supplied poses rather than independently predicting them.
- Fixed-capacity render-graph execution, explicit D3D12 texture transitions,
  direct-queue compute/storage images, bounded transient pools and pass timing
  support the production Advanced scene/post path.
- Raster cascades with bounded directional PCSS and fixed-filter spot shadows,
  temporal AA/AO, contact shadows, bloom,
  fog, object motion blur and physical lens depth of field use replicated
  settings and guarded image neighborhoods. Shader compilation pins DXC and
  strict floating-point inputs; the cohort self-check reports pass digests.
  Renderers agree when every pass digest, the shader bundle and the FP
  contract match. A different GPU model or driver that reproduces them is
  shown in the Output Monitor's Render check row, but does not hold Program.
- Image-based lighting, OpenEXR/HDR environments, offline diffuse GI and local
  reflection probes, one planar reflector, physical sky/sun, photometric
  point/spot/area lights and IES profiles have authored and cached paths.
- glTF/GLB/OBJ import, native material properties, compressed texture packages,
  authored samplers, mip residency, node/skin animation and show-frame wind
  are supported. Large geometry uses indexed submission, caster culling,
  retained shadow maps, combined auxiliary products and a shared per-device
  geometry arena. Source/package work remains outside frame recording.
- Output color uses a worker-baked 33-point transform, sRGB/ACES Approx/pinned
  OCIO ACES 1.3, optional show look and per-wall compensation. Native output
  formats include 8-bit, 10-bit and linear RGBA16F with format-aware dither.
- Asset generations, frame budgets, shader/history conformance and output sync
  evidence join Program readiness. A node reports or holds on a failure;
  it does not substitute a cheaper look or reveal an incomplete generation.

## Support boundaries

There is no native Vulkan backend, bindless material path, async-compute
execution, GPU-driven scene culling or submission, BLAS/TLAS/ray-query rendering,
DDGI/ReSTIR/SVGF pipeline or external-memory interop. The D3D12 RHI does
provide writable buffers and indirect dispatch and single-batch indirect pulled
draws (`writable_buffers`, `indirect_dispatch`, `indirect_draw`); ajfx particle
simulations run on them through `render::ParticleDevice`, but nothing in the
scene draws particles yet. Requested unsupported
features fail capability/readiness checks. Offline baked GI and probe/planar
reflections do not establish support for those features.

Physical synchronization uses the optional Windows NVAPI provider when built
with `MEDIASERVER_ENABLE_NVAPI=ON`. A software self-check or an Advanced Ready
result does not qualify genlock, present barriers, photon-level timing,
cross-vendor agreement or camera/lens calibration. Follow the
[two-node bring-up](advanced%20icvfx%20renderer/03-genlock-and-present-barrier-bringup.md)
and [wall color procedure](advanced%20icvfx%20renderer/10-wall-colour-procedure.md)
with recorded adapters, drivers and reference hardware. Other platforms lack
the Windows physical display-inventory provider and refuse unsupported bindings.

The renderer uses fixed capacity and approximation limits. Mip residency uses
CPU-backed model data rather than disk-paged virtual textures. Point/area-light
shadowing, arbitrary Blender procedural/vertex animation and unrestricted
material graphs are outside the supported subset. Physical multi-machine seam
and stage-network claims require measurements on the deployed cohort.

Rendering can still use isolated per-layer work; a complete render-once,
product-driven compositor is unfinished. Scaled and physical-surface/mesh
mapping need their own seam qualification rather than inheriting the exact
one-to-one canvas-slice apron guarantee.

## Relevant code and verification

| Concern | Code and tests |
| --- | --- |
| Backend capabilities, barriers, presentation | `engine/rhi`, `engine/ui` |
| Tracking, lens, deterministic scene/settings | `engine/track`, `engine/scene` |
| Renderer, caches, graph, conformance and timing | `engine/render`, `engine/shaders` |
| Frame permits, heartbeats and Program readiness | `engine/show`, `engine/net`, `engine/sync`, `apps/server` |
| Offline bake and fitted/reference data | `apps/bake`, `third_party` |

Use `ctest --preset windows-msvc-debug-gui -N` to inspect the current test list;
run affected entries in both GUI configurations. Blender integration and
qualification commands are in the [add-on README](integrations/blender/README.md)
and [scene-link reference](blender%20live%20scene%20link/README.md). Historical
test counts, hashes, captures and completed step plans are recoverable in Git.
Renderer source provenance remains in
[PROVENANCE.md](third_party/renderer/PROVENANCE.md).
