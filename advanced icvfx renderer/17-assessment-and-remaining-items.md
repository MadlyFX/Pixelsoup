# 17 — Assessment, and the items that remain

**Written 2026-09-07 against the working tree, not against a commit.** Steps
01–16 are uncommitted: 139 tracked files changed and 146 new files, about
33,000 lines. Everything below was checked against the code rather than taken
from the step documents, and where the two disagree it says so.

A second agent session was building and editing this tree while this was
written (step 15 files, the playback buffer, and `frame_root_mailbox` changed
within the hour). The step 15 entries here describe the tree as it was read;
re-read that step before relying on them.

**Later update, 2026-09-07:** step 16 now consumes light units and colour
temperature in live geometry, fog, reflective shadow maps and baked lighting.
It adds the replicated project photometric scale and completes spot authoring.
The historical step-16 gaps below are superseded by the
[implementation notes](16-photometric-and-spot-lights.md#implementation-notes).
Area emission and IES remain follow-on work. The sun's angular radius is now
consumed for shadow penumbrae by item 19; its sky disc remains follow-on work.

**Texture sampling update, 2026-09-07:** item 20 is implemented. Per-view
wrap and filter modes survive glTF import and `.msmodel` packaging; Basic and
Advanced apply authored anisotropy and sampling LOD bias. Finding 1 and the
sampling gaps below describe the earlier tree and are superseded by
[20 — Texture sampling](20-texture-sampling.md).

**Temporal stability update, 2026-09-07:** item 18 is implemented. Padded
plates, depth-aware rejection, temporal AO, geometric specular AA, and
heartbeat restart agreement are covered by
[18 — Temporal stability](18-temporal-stability.md). The earlier finding 5
also misstated the plate decomposition: each participating node already drew
the complete plate; the missing border was outside that complete image.
The temporal gaps below describe the earlier tree.

**Shadow stability update, 2026-09-07:** item 19 is implemented. Cascade blend
bands, fixed 5×5 filtering, sun-size penumbrae, bounded contact shadows and
MASK coverage are covered by [19 — Shadow stability](19-shadow-stability.md).
Finding 6 and the shadow gaps below describe the earlier tree and are
superseded by those implementation and verification notes.

## What was examined

**Shader/cohort update:** items 22 and 23 are implemented. Findings 2 and 12
below describe the earlier tree. See [22 — Shader floating-point contract](22-shader-floating-point-contract.md)
and [23 — Cohort render-conformance](23-cohort-render-conformance.md) for the
compiler decision, measured GPU comparison, fixed fixture, heartbeat evidence,
Program gate and verification boundaries. Real multi-machine evidence is
gathered by running the new check on the deployed cohort.

- Every step document, the status document, and the integration plan.
- Behind each claim: the RHI capability and sampler surface, the DXC
  invocation, the lit / resolve / shadow shaders, the temporal history key,
  the tracked-pose channel, renderer diagnostics, the model loader, and the
  swapchain formats.
- Test evidence, on the tree as it stood at 10:20 today: the other session's
  `windows-msvc-release-gui` build linked with zero errors and its
  `windows-msvc-debug-gui` run passed all 143 CTest entries at 10:22; this
  session then ran the full `windows-msvc-release-gui` suite against those
  binaries and it passed 143 of 143 in 189 seconds. A build attempted from
  this session while the other was running collided on the object files and
  was abandoned rather than fought over.

## State by step

| Step | Built | Verified boundary |
| --- | --- | --- |
| 01 tracking, inner/outer frustum | Yes | Plate composites on the shipping path for planar surfaces only. Not yet confirmed on a wall. Poses are held, never predicted. |
| 02 lens model | Yes | Profile, Brown–Conrady, calibration points, encoders and entrance pupil exist in `scene/lens.hpp`; the tracked pose carries focus, iris and zoom. Distortion overscan not re-verified here. |
| 03 genlock, present barrier | Yes, unproven | NVAPI path is an opt-in build; the show clock follows the Quadro Sync counter. Never run against reference hardware. |
| 04 compute in the RHI | Yes | Direct queue only. No writable storage buffers, no async compute, no indirect draw, bindless false. |
| 05 padded slice and crop | Yes | `CanvasSlice` maps only. Planar-surface maps **and the inner frustum** take no apron. |
| 06 render graph | Executor only | Proven byte-identical; the production frame is still one hand-recorded function. |
| 07 image-based lighting | Yes | `.hdr` only. One environment resident at a time. |
| 08 temporal antialiasing | **Yes** (README says no) | No depth rejection. A restart on one node is counted, not held. |
| 09 occlusion, local shadows | **Yes** (README says no) | AO full-resolution, eight directions, undenoised. Spot shadows only, 3×3 PCF, no cascade blend, no contact shadows. |
| 10 display transform | Yes | 33-point atlas, ACES *approximate*, eight-bit swapchain, look per project. |
| 11 bloom, fog, reflections | A, B, D | Reflections stop at the environment by decision. |
| 12 culling, instancing, LOD | Yes | CPU only; no occlusion culling. |
| 13 texture streaming | Yes | Mip residency from a complete CPU pyramid; no disk paging. |
| 14 materials and ingest | Yes | Sixteen-sampler cap; bindless would lift it. |
| 15 baked GI | Yes, in flight | Being finished by the other session at the time of writing. |
| 16 photometric, spots | **Half** | Spots and their shadows landed under 09. Units, colour temperature and the sun's angular radius are authored, digested and **never consumed**. |

## Where the documents and the code disagree

- The README's "Order" list marks 08 and 09 as not done. Both are built and
  tested; the per-step documents are current, the README is not.
- 08's still-open bullet on `camera_history_digest` is stale. The digest mixes
  the *authored* camera's field of view; the tracked field of view lives in
  `TrackedCamera` and never enters it, and the inner view carries a stable
  `view_digest`. `icvfx_tests.cpp` asserts that a tracked move or zoom produces
  motion vectors rather than a reset.
- 15 says "direct rectangle lighting remains supported by the renderer". The
  lit shader distinguishes only directional from positional lights
  (`vector_kind.w`), the packer special-cases only `Spot`, and the bake
  refuses `Rectangle` outright. A rectangle's width, height and sidedness are
  authored and digested; on the GPU it is a point light.
- 16 has no "what was built" section although its Part B shipped inside 09.

## Findings the documents do not record

These came out of reading the code, and each one is something a photoreal
asset or a real camera will find on the first day.

1. **The sampler contract is clamp-only and trilinear at best.**
   `rhi::Filter` is `Nearest | Linear`; `create_sampler` takes nothing else,
   and the D3D12 backend hard-codes `CLAMP` on every axis with no anisotropy
   and no LOD bias. glTF sampler wrap modes are not imported. Any tiled
   material — every floor, wall and fabric in a scanned set — renders its last
   texel smeared past `[0,1]`, and every texture at a grazing angle shimmers
   under a moving camera. This is the single cheapest large gap.
2. **The GPU half of the floating-point contract is missing.** The CPU builds
   with `/fp:precise` and `-ffp-contract=off`, which is exactly right. DXC is
   invoked with `-WX` and nothing else: no `-Gis`, no `precise` anywhere in
   `engine/shaders`, so DXIL carries fast-math flags and the driver's back-end
   is free to contract and reassociate. Identical on one driver; not a
   contract across driver versions or vendors.
3. **Nothing measures the frame.** The D3D12 backend allocates a timestamp
   query heap and readback; no pass reads it. `RendererDiagnostics` has no
   GPU or CPU time, no present timing, no budget. "Nothing degrades
   adaptively" is an invariant with no instrument behind it.
4. **Four authored light fields reach the digest and stop.** `LightUnit`,
   `color_temperature_kelvin`, `use_color_temperature` and the directional
   `angular_radius` are parsed, clamped, persisted and mixed into the
   `SceneBridge` payload — and never packed. Two nodes agree exactly on a
   value neither uses. The sun is a hard-edged directional light everywhere.
5. **The plate has no apron.** `IcvfxStage` renders the intersection of the
   plate rectangle with the node's slice, unpadded. A plate that spans two
   nodes therefore seams inside the photographed region for every
   neighbourhood pass — TAA's 3×3 clamp, the 32-pixel occlusion search,
   bloom. The outer frustum, which is only ever spill, has the apron; the
   plate, which is the shot, does not.
6. **Cascade transitions are hard and the filter is fixed 3×3.** A receiver
   picks the first cascade whose border holds the kernel; nothing blends
   across the boundary, so a camera dolly moves a visible line across the
   floor. Penumbrae step at three texels.
7. **Contact shadows do not exist.** The word appears in two comments about
   bias; there is no screen-space march.
8. **No asset animates.** `model_loader` has no animation, skin or morph
   path. Every flag, tree, curtain and crowd member is a statue.
9. **Reflections are the environment only.** The status document is honest
   about it. A car on a wet road is the canonical volume shot and it cannot
   be lit correctly today.
10. **OpenEXR is absent.** `.hdr` covers stage HDRIs; plates, probes and
    graded environments from a facility arrive as `.exr`.
11. **Eight bits out.** The swapchain is `R8G8B8A8_UNORM`; the dither is
    correct for it. LED processors take ten and twelve.
12. **Determinism is proven on one adapter in one process.** Every two-node
    test renders both slices on this machine's RTX 3090. Nothing gathers
    evidence from a real cohort that two machines produced the same bytes.

## What separates this from the target

The README names four properties. Against each:

- **Correct perspective for the tracked camera.** The spine is there and
  tested to the byte. What is missing is *time*: a held pose plus a fixed
  frame offset cannot hide transport latency under a fast pan, and the
  result is parallax swim on the plate edge, integrated into the shot.
- **Photoreal content and baked lighting.** The bake exists. The lights it
  and the frame path consume are unitless, the sun is a point, there is no
  area light, no IES, no wrap sampling, no animation, no EXR.
- **Colour that survives the processor and the IDT.** An approximate ACES fit
  on an eight-bit surface, with the look per project rather than per wall.
- **No temporal or spatial artifacts.** TAA and the spatial pass are built and
  are the strongest part of the renderer. The artifacts that remain are the
  ones TAA cannot fix: shadow-boundary crawl, specular fireflies on rough
  photoreal materials, AO grain, texture shimmer without anisotropy.

## Proposed items

Numbered from 18 so each can become its own file when picked up. Grouped by
which of the four properties it buys. Each states the seams and the hazard
that would make it a seam on a wall.

### Temporal stability

**18 — Finish temporal antialiasing where the camera is looking.**
*Why:* the plate is the only region a camera integrates, and it is the one
region without an apron and without depth rejection.
*Design:* give `IcvfxStage` the same padded-render-and-crop the outer frustum
has, sized by `SliceApronRequirements::maximum()`; add the depth term to the
resolve's rejection now that the auxiliary depth is `R32_TYPELESS` and
sampleable; accumulate occlusion through the same `TemporalResolve` history
so the sparse kernel becomes affordable and the grain goes; add geometric
specular antialiasing (roughness widening from the per-quad normal derivative,
which is identical across nodes because the apron supplies the neighbour
quad); make `temporal_restarts` a cohort fact by carrying it in the
heartbeat and holding Program when members disagree for a frame.
*Hazards:* the plate apron changes the plate's target extent, which is part
of `HistoryKey`; a padded plate must key on the crop, not the render.
*Tests:* plate straddling a node boundary with TAA, AO and bloom on — byte
exact at every shared pixel over 32 frames; same-object depth discontinuity
rejected; occlusion converges and a repeated frame is byte identical.
*Later:* TAAU for the plate, since `inner_render_scale` already renders it
below wall resolution.

**19 — Shadow stability.**
*Why:* a moving camera drags a cascade boundary across the floor, and a
three-texel penumbra steps.
*Design:* a blend band at each cascade transition, evaluated from the same
world position both cascades are indexed by; a wider fixed kernel (5×5, or a
Gaussian-weighted set of the same nine taps) — or exponential/moment shadow
maps, which filter and can use the mip chain, provided the filtering is
manual as the history and LUT already are; consume `angular_radius` as a
penumbra width with a fixed tap count (PCSS-shaped, no adaptive search);
contact shadows as a short fixed-step march along the light direction in
the depth the occlusion pass already reads, with its own apron lane;
alpha-tested casters so foliage stops casting as a solid card.
*Hazards:* any search whose step count depends on distance to the camera.
*Tests:* a dolly through a cascade boundary produces no frame-to-frame
difference on a static receiver beyond the blend band; a floor with nothing
above it darkens nowhere at every kernel; two-node exact with every option on.

**20 — Texture sampling contract.**
*Why:* finding 1. A photoreal set cannot be loaded correctly today.
*Design:* `rhi::SamplerDesc` with per-axis address mode (wrap, mirror, clamp),
anisotropy, LOD bias and optional comparison; import glTF `wrapS/wrapT` into
the `.msmodel` package and the material record so a part binds the sampler it
authored; a replicated `texture_anisotropy` and `mip_bias` in
`RendererSettings` (they change what a node draws, so they digest); the
sampler table reuse in the command context extends to the new keys.
*Hazards:* anisotropic weights are implementation-defined between vendors.
That is acceptable on a same-GPU cohort and must be recorded by item 23.
*Tests:* a quad with UVs to 4.0 shows four tiles; a grazing plane is sharper
with anisotropy on and byte-exact across two nodes; a repeated frame is
byte identical with bias applied.

**21 — Object motion blur at the physical shutter.**
*Implemented:* [21 — Object motion blur](21-object-motion-blur.md), with
the supported composition limits and verification recorded there.
*Why:* the real camera blurs the wall itself, so camera motion must **not**
be rendered — but a virtual object crossing a static wall frame is sharp on
every frame the camera integrates, and reads as stop-motion.
*Design:* split the velocity product into object motion through the current
view only (previous object transform, current view-projection) and blur
that alone by the tracked camera's shutter fraction with a fixed sample
count and an authored maximum radius that sizes an apron lane. Hold
reproduces the held frame's blur.
*Tests:* a static scene under a moving camera is byte-identical with blur
on; a moving object's streak length equals shutter fraction times its
per-frame motion; two-node exact.

### Determinism as evidence

**22 — Shader floating-point contract.**
*Why:* finding 2. The invariant "two nodes must agree exactly" currently
rests on both nodes running the same driver on the same silicon, and nothing
says so.
*Design:* decide once between `-Gis` for every shader and `precise` on every
value that feeds a history, an integral, a dither or a LUT (TAA, fog,
bloom, occlusion, display transform); measure the cost; add `-denorm
preserve`; record the decision in `engine/shaders/CMakeLists.txt` where the
DXC pin already is.
*Tests:* the existing byte-exact suite under both settings, and a report of
what `-Gis` costs on the RTX 3090 so the decision is a number.

**23 — Cohort render-conformance self-check.**
*Why:* finding 12. Turn "two nodes agree" from a single-machine test into
evidence gathered on the machines that will be on the wall.
*Design:* a fixed conformance scene compiled into the binary that exercises
every pass — cascades, atlas, IBL, occlusion, fog, bloom, TAA over sixteen
frames, LUT, dither — rendered at a small fixed canvas on operator demand and
before Program promotion, never on the frame path. Each node publishes a
per-pass digest of the bytes, plus adapter, driver version and shader-bundle
hash, in the heartbeat. `evaluate_renderer_readiness` refuses a cohort whose
digests differ and names the first pass that did.
*Hazards:* the check itself must not depend on anything local (extent, slot
state, wall clock). Render it as a canvas of its own, whole, on every node.
*Tests:* the digest is stable across runs; a one-line shader change moves
it; a mismatched heartbeat holds Program and names the pass.

**24 — Frame budget instrumentation.**
*Why:* finding 3, and plan step 26.
*Design:* per-pass GPU timestamps through the heap the backend already owns,
CPU record time per surface, present timing from the barrier statistics,
upload bytes per recording — into `RendererDiagnostics`, the heartbeat and
the Output Monitor. An authored frame budget in `RendererSettings`; a node
over it reports and Program holds by sync policy. It never draws less.
*Tests:* a deliberately heavy pass is attributed to itself; times are
monotone with load; no per-frame allocation added.

**25 — Migrate the frame onto the render graph.**
*Why:* 06 says why and what it costs. Aliasing, async compute and the RT
layer are all behind it, and the byte-identical gate makes each pass
reviewable on its own.
*Design:* sky first, then the auxiliary replays, then occlusion, fog and
bloom; per-surface transient pools as 06 lists.

### Photorealism

**26 — Photometric lights, colour temperature, sun size, IES.**
*Why:* finding 4, and 16 Part A. Camera exposure means nothing until light
has units; a 3200 K practical against a 5600 K window is how an interior
reads as one.
*Design:* per 16, at pack time, with the absolute anchor replicated and
digested; `angular_radius` as the penumbra in item 19 and as the sun disc in
the sky; IES profiles as a bounded generation-fenced asset, sampled from a 2D
atlas with manual bilinear like the LUT.
*Tests:* per 16, plus an IES profile that darkens where its table says.

**27 — Reflections without ray tracing.**
*Why:* finding 9. The RHI has no acceleration structures and will not soon.
Two deterministic answers cover most of what a set needs.
*Design:* (a) local reflection probes baked by `media-bake` — box or sphere
proxy, GGX-prefiltered exactly as the environment is in 07, parallax
corrected, blended by authored volume and selected by document node id and
never by camera distance; (b) one authored planar reflector (wet floor,
mirror, window) rendered as the full-canvas camera mirrored through the
plane — a slice's mirrored window is exact, so it abuts exactly.
*Hazards:* the planar pass doubles geometry cost; cap at one and count the
refusal. Probe selection by anything view-dependent is a seam.
*Tests:* a chrome sphere beside a red wall reflects red from the probe and
not from the environment; the planar floor reflects a box at the mirrored
position; two-node exact; probe bytes deterministic across bakes.

**28 — Lens-driven depth of field on the plate.**
*Why:* the physical lens focuses on the wall. A background thirty metres
behind the talent is as sharp as the talent, which is the second thing a DP
notices after perspective.
*Design:* circle of confusion from the tracked focus distance and aperture
already in the pose, at the virtual depth, **minus** the circle the physical
lens will already impose on the wall plane (the surface's distance is
replicated state) — clamped at zero; a fixed-kernel gather with an apron lane
sized by the authored maximum radius; plate only. Depends on item 18's plate
apron.
*Tests:* focus at the wall distance is byte-identical to no depth of field;
a plane at depth blurs by the predicted radius; two-node exact across a
plate seam.

**29 — Physical sky and sun.**
*Why:* exteriors and car process cannot be re-timed from an HDRI.
*Design:* an analytic sky (Hosek–Wilkie) or precomputed scattering tables
built at adoption by the same deterministic compute path as the IBL
prefilter; a sun disc of `angular_radius`; the result feeds the SH and
specular cubemap exactly as a map does; time of day authored and keyable.
*Tests:* table bytes deterministic; elevation changes the SH; two-node exact.

**30 — Colour pipeline to facility grade.**
*Why:* the third target property, and findings 10 and 11.
*Design:* OpenEXR decode (half and float, ACEScg tag) on the existing image
worker; `R10G10B10A2` and `RGBA16F` swapchain formats with the dither
following the format as 10 already anticipates; an exact ACES 1.3 RRT+ODT —
or an OCIO config — baked into the same 33-point atlas off the frame path
so the frame path never changes; an authored wall-primaries and camera-IDT
inverse stage from the 10 procedure, per wall rather than per project.
*Tests:* an EXR round-trips to the half it contains; a ramp on the ten-bit
surface has no eight-bit steps; the ACES output matches the reference
implementation on the standard test values.

**31 — Animation keyed to the show frame.**
*Why:* finding 8.
*Design:* glTF node animation and skins in the package; pose evaluated from
the exact 64-bit show frame so Hold repeats it and a seek lands exactly;
joint matrices per instance on the vertex-pulling path; previous-frame pose
for velocity; a deterministic vertex wind for foliage keyed the same way.
*Tests:* frame N twice is byte identical; two-node exact through a skinned
mesh; velocity matches motion.

**32 — Area lights.**
*Why:* the rectangle light is authored and not shaded. Softboxes and
windows are area lights and nothing else looks like them.
*Design:* linearly transformed cosines with the fitted tables as a
generation-fenced asset; sphere and rectangle; the shadow question deferred
to item 19's penumbra.
*Tests:* a rectangle's reflection on a rough floor is a soft rectangle;
two-sided versus one-sided; two-node exact.

### Professional workflow

**33 — Pose prediction and filtering, on the Director, replicated.**
*Why:* the first target property, under motion.
*Design:* the Director alone predicts, before it replicates: a constant-
velocity or constant-acceleration fit over the last K control-frame stamps,
in `deterministic::` arithmetic, projected forward by the authored latency
compensation; an optional authored jitter filter. Nodes still never
extrapolate — they receive a pose that already includes the prediction, so
they agree by construction. The Live Data panel shows raw against predicted
and, one frame later, the prediction error — which is the operator's tuning
signal for the latency value.
*Tests:* a constant pan predicts exactly; a hold predicts nothing past the
last stamp; two sessions receive identical predicted streams; the error
counter reports.

**34 — Light cards, colour-correction windows, plate chromakey.**
*Why:* these are the three things an operator reaches for on every volume
day, and a renderer without them is not usable on set however it looks.
*Design:* light cards as authored emissive quads on the wall (colour,
temperature from item 26, softness, in front of or behind the plate);
colour-correction windows as authored regions of the outer frustum with a
local grade applied in the display transform; chromakey inside the plate
with tracking markers at deterministic wall positions. All replicated,
digested, drawn in the composite pass.
*Tests:* a card is byte-exact across a node seam; a window's edge feathers
identically on two nodes; markers land on the same wall pixel from either
node.

**35 — Bindless, GPU-driven submission, then ray tracing.**
*Why:* 14's sampler cap, 12's deferred occlusion culling, and every RT
request that fails readiness today.
*Design:* bindless descriptors first, since both the material count and the
RT layer need them; writable storage buffers and indirect draw; then
BLAS/TLAS with deterministic instance order and ray-query shadows, occlusion
and reflections as plan step 15 lays out. The long pole; do not start it
before items 18–23.

**36 — Hardware validation, and detaching the editor.**
*Why:* 03 has never met a Quadro Sync card; the Director is always the
operator machine.
*Design:* the two-machine bring-up in 03's companion document, a capture
procedure (PIX, RenderDoc), a pinned driver matrix that item 23 checks
against; then an editor-as-remote-client role so the frame clock can live on
a stage machine while the operator sits at a desk.

## Recommended order

1. **20** — the sampling contract. Cheapest, and nothing photoreal loads
   correctly without it.
2. **18** — the plate apron, depth rejection, occlusion through history,
   specular antialiasing. This is what a camera sees.
3. **19** — shadow stability.
4. **22 and 23** together — the GPU floating-point contract and the cohort
   self-check. Small, and they turn the project's central claim into evidence.
5. **26** — units, colour temperature, sun size. Makes exposure mean something.
6. **33** — pose prediction. The remaining perspective fault.
7. **27** — probes and a planar reflector.
8. **30** — EXR, ten bits, exact ACES.
9. **28** — depth of field on the plate.
10. **24** and **25** — instrumentation and the graph migration, before 35.

Items 21, 29, 31, 32 and 34 are independent of that order and each is a
week or less; they can be taken whenever a shot needs them.

## Housekeeping

- Update the README's "Order" list: 08 and 09 done, 16 half done.
- Strike 08's `camera_history_digest` bullet; it is settled and tested.
- Give 16 a "what was built" section pointing at 09's Part B.
- Correct 15's rectangle-light sentence.
- Commit steps 01–16. Thirty-three thousand uncommitted lines is a risk
  larger than any item above.
