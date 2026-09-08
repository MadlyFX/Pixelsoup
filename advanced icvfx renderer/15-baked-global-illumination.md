# 15 — Baked global illumination

**Implemented 2026-09-07: uniform irradiance probes, offline CPU baking, and replicated Program asset publication.** See the operator workflow and implementation notes below.

**Track 4, and the strategic centrepiece.** This is the step that decides what
"photoreal" means for this renderer.

## The argument

Unreal reaches photorealism on a volume through Lumen: dynamic, screen-and-world
space GI with hardware ray tracing, plus a decade of supporting infrastructure.
Reproducing that means acceleration structures, ray queries, radiance caching,
temporal reservoirs, and denoising — each of which is itself a project, and all
of which must then be made bit-identical across a cohort, which is a constraint
Unreal does not have to satisfy.

There is a shorter path to the same picture, and it fits this system better:

**Bake the light transport offline; render the result.**

- Most volume content is a **static set with a moving camera**. The lighting does
  not change during a take. That is precisely the case a bake solves exactly.
- Baking moves the expensive, non-deterministic part **off the frame path**,
  where none of this project's hard constraints apply. An offline path tracer can
  allocate freely, read files, take minutes, and use as many bounces as it likes.
- The result is an **asset with a content generation** — which is a problem this
  codebase already knows how to think about, and already knows it has not solved
  ([the outstanding replicated-asset-commit gap](../advanced-renderer-status.md)).
- On camera, through a real lens, with grain and a grade, **stable correct
  lighting beats noisy temporally-unstable dynamic lighting every time.** Lumen's
  characteristic artifacts — boiling in shadowed areas, lag as lighting changes —
  are things a camera integrates and a DP notices.

The cost is honest: relighting requires a rebake, so a lighting change is minutes
rather than instant. On a stage where the LED content is prepared before the
shoot day and the on-set changes are camera and talent, that is the right trade.
Where it is not — a shot that needs a practical to change during the take — a
small number of dynamic lights over the baked ambient covers it.

## What gets baked

Two options, and they are not exclusive:

**Irradiance probe volume.** A 3D grid of probes storing incoming radiance as
spherical harmonics or an octahedral map. Objects sample and interpolate. Handles
dynamic objects moving through the space, which lightmaps cannot. Lower spatial
frequency — soft, correct ambient, no sharp bounce detail.

**Lightmaps.** Per-surface baked irradiance in a UV2 chart. Much higher spatial
frequency, so contact darkening and coloured bounce land where they belong. Only
works for static geometry, and requires UV unwrapping in ingest.

Recommend **probes first**: no unwrap step, works for everything including
dynamic props, and it composes with the existing RSM bounce and with
[07](07-image-based-lighting.md)'s IBL as another ambient source. Add lightmaps
for hero static surfaces later if the probe frequency proves too low.

## Seams

- **New offline tool** — `apps/bake/` or a mode of the existing server binary.
  It consumes the evaluated scene plus the environment map and produces a probe
  asset. It is not frame-path code and should not pretend to be: no fixed
  allocation rules, no `try_lock`, no bounded workers.
- **New** probe asset format and loader, following the `ModelCache` residency
  contract on the render side.
- `engine/scene/include/mediaserver/scene/scene.hpp` — probe volume bounds,
  density, and the baked asset's identity as authored replicated state.
- `engine/render/src/scene_renderer.cpp` — probe sampling in the lit pass,
  replacing or supplementing the RSM term.
- `engine/net/` — the cohort commit. See below.

## The distributed problem this step forces

A bake is a large asset that arrives asynchronously. If node A adopts generation
7 on show frame 1000 and node B adopts it on 1003, the wall changes lighting in
two stages across three frames — visible, and on camera, permanent.

`advanced-renderer-status.md` already names this as the outstanding gap:

> asset-ready and refresh generations do not yet participate in Director cohort
> readiness, so nodes can first reveal a new still, video, model, or projection
> mesh on different show frames. A replicated generation plus explicit all-node
> commit is required before this is fully deterministic for live Program
> replacement.

Baked lighting makes solving it mandatory rather than merely correct. The
machinery to build on already exists: `DirectorFrameBarrier`, `FramePrepared` /
`FrameCommitted` / `FrameCommitPermit` in `session_protocol.fbs`, and the
existing readiness heartbeat that already carries an already-computed observation
rather than a probe.

The shape:

1. Each node reports the asset generations it has **complete and resident** in
   its heartbeat — an already-known value, no scan, no stat.
2. The Director does not permit a generation to become visible until every node
   in the Program cohort reports it complete.
3. The permit names the exact show frame at which the new generation becomes
   visible. Every node switches on that frame.
4. A node that cannot reach the generation holds Program rather than revealing
   it early or late.

Build this once, here, and then still, video, model, projection-mesh, LUT
([10](10-display-transform-and-calibration.md)) and prefiltered-environment
([07](07-image-based-lighting.md)) generations all use it.

## Runtime design

- Sampling is a pure function of world position and the probe grid. No view
  dependence, so it is node-independent by construction — which is the same
  reason the cascade fits and the RSM field are already slice-independent.
- Interpolation across probes must use the identical expression everywhere;
  trilinear with the same clamping, no per-node fast path.
- Probes should carry visibility information (a depth/variance term per
  direction) or light leaks through walls, which is the classic probe-GI failure
  and is very visible on an interior set.
- The existing RSM bounce becomes either redundant or a complement for dynamic
  objects. Decide explicitly; running both without deciding double-counts bounce
  light.

## Tests

- Bake determinism: the same scene and environment produce a byte-identical probe
  asset across runs. Without this the cohort can never agree.
- Analytic: a probe inside a closed white box converges to the expected uniform
  irradiance; a probe in an open scene under a known environment matches the
  IBL irradiance.
- Light leak: a probe on the dark side of a wall is dark.
- Residency: no frame-path file access; sticky failure; generation fencing.
- **Cohort commit: two nodes reveal generation N+1 on the same show frame**, and
  a node that has not completed it holds Program. This is the test that makes the
  whole step trustworthy.
- Slice determinism with probes active.

## Definition of done

A baked set renders with correct soft indirect light including coloured bounce
and contact darkening; a rebake is an operator action with visible progress; every
node reveals a new bake on the same show frame; and a node that cannot reach the
bake holds rather than showing different lighting.

## Open questions

- **What does the bake use?** Writing a path tracer is a project. Options: a
  small CPU path tracer over the existing scene representation (slow but fully
  owned), a GPU path tracer once acceleration structures exist, or an external
  tool with an import. The first is the most tractable and the bake is not
  latency-sensitive.
- **Probe placement.** A uniform grid wastes probes in empty space and
  under-samples near geometry. Adaptive placement is better and harder to make
  deterministic — but the bake is offline, so determinism there only means "the
  same input gives the same output", which is achievable.
- **How do dynamic lights compose with the bake?** The standard answer is to bake
  only static lights and add dynamic ones at runtime, with the operator marking
  which is which. That marking is authored state and needs a schema.
- **Rebake time.** If a rebake takes twenty minutes, the workflow is "prepare
  before the day". If it takes twenty seconds, it becomes usable on set, which is
  a materially different product. Worth measuring early, because it changes what
  the feature is.


## Operator workflow

1. Select **Advanced** in **File > Project Settings > Renderer**.
2. In an object's or model's Properties, use **Bake Geometry: Static** for
   geometry that stays fixed. In a light's Properties, use **Indirect Light: Baked**
   for lights whose diffuse bounce should be included. Direct lighting remains
   live in both cases. Moving a static contributor or changing its lighting
   requires another bake.
3. Open **Project Settings > Environment > Baked Lighting**. Set the volume in
   metres, probe counts, sample count, bounce count, and output base path. Put
   probes in empty space around the set; probes inside walls cannot resolve the
   intended lighting. Default: 8 × 4 × 8 probes, 256 paths, four diffuse bounces.
4. Choose **Bake Lighting**. The evaluated Stage scene at the current frame is
   copied to a worker. The panel shows reading, probe progress, writing, and
   completion or a specific refusal. **Stop Bake** cancels the job. A completed
   result is discarded if its project, Stage, or static inputs changed meanwhile.
5. Completion enables the field and authors its source and content identity.
   **Rebake Lighting** produces another immutable file. Use **Strength** to
   adjust the diffuse contribution and the checkbox to return to live ambient.
6. Make the resulting `base.<content>.msgi` available at the authored path on
   every Program node. The existing shared-storage/asset staging workflow must
   provide those files; the session protocol transports identity and permits,
   not asset bytes. Output Monitor reports loading, commit pending, or a missed
   activation. Program retains its previous complete image until publication.

For a file changed at the same path, **Library > Rescan** authors a shared refresh
generation. That generation invalidates media, model, projection-mesh, LUT,
environment, and probe residency on workers. Repeating an observation of the
same generation does not reread the file. Legacy low-level local refresh APIs
remain available for diagnostics; production Program replacement uses the
authored Rescan generation or a new immutable source path. Media's existing
presence-only mode still assumes that nodes have the intended source bytes;
use the explicit asset verification workflow when that is in doubt.

The packaged offline tool also accepts:

```powershell
media-bake project.mstage shared/lighting/set [scene-id] [show-frame]
```

It uses the saved scene's probe settings and the project's rational frame rate,
prints the immutable asset path, content identity, and elapsed seconds, and
does not change the project. Omitted scene/frame select the first Stage and
frame zero. The editor workflow performs the authoring and cohort preparation.

## Implemented transport and sampling

The owned CPU tracer builds a stable BVH over static primitives and imported
model geometry. It integrates a hemisphere or Radiance HDR environment,
emission, directional/point/spot light visibility, and diffuse paths in a fixed
sample order. Base-colour, emissive, and metallic/roughness textures participate;
offline BC7 decoding supports the compressed model packages from step 14.
HDR transport uses an equal mixture of uniform directions and luminance weighted
by texel solid angle over the environment's bilinear footprint. This keeps tiny, bright
softboxes represented at the default 256 samples without letting unlit directions
or diffuse bounces disappear. Both distributions use the same mixture PDF. The
baker's environment rotation and filtering match the live IBL path.
Nine incoming-radiance SH coefficients and 8 × 8 octahedral distance moments
occupy 656 bytes per probe. Versioned, checksummed `.msgi` files are bounded to
4 MiB; the maximum 16 × 16 × 16 field is about 2.56 MiB.

The lit pass samples world position and world normal with identical trilinear
and visibility expressions for every view. Bilinear octahedral moment sampling
and a continuous visibility cutoff avoid hard shading patches at direction-cell
boundaries. Visibility variance rejects probes
through occluders. Baked diffuse replaces live environment/hemisphere diffuse;
environment specular and analytic direct lights remain live. RSM bounce is
disabled while a bake is enabled, preventing double counting.

The common publication gate covers the complete requested asset set, including
the replicated media/geometry manifest and explicit refresh generation. Worker
completion and GPU submission completion precede readiness. Heartbeats in
session protocol v7 carry the set digest, readiness, and visible commit. After
every required Program member agrees, the Director authors a permit through
the existing durable FramePrepared/FrameCommitPermit barrier. It names an exact
control frame and show frame, with 30 control frames of delivery lead. A member
that misses activation holds its previous pixels even after loading completes.
The Director can retry an activation only while no member has revealed it;
partial adoption never authorizes a later switch on the remaining members.
Playback preserves the activation snapshot for a renderer that was ready before
the deadline. Catch-up cannot drain past it, a busy GPU retries that same root,
and decode headroom cannot strand it behind an unpublished candidate. The
control producer continues advancing while the consumer retains the snapshot.
Readiness that arrives after the activation cannot rewind to it.
Saved projects omit these runtime activation ordinals; opening an older project
also removes them before replication. A new session must collect fresh readiness.
Program displays the preparation reason, waiting node, ready renderer count,
activation countdown, and elapsed wait over its retained image. Bake progress
continues in the main status bar when Project Settings is closed, and completion
distinguishes Stage readiness from lighting that is live in Program.

Probe, model and environment observations use publication fences; cancelled
GPU recordings retry their uploads. Cache failures are sticky until a changed
identity or authored refresh. Disk reads, decoding, tracing, checksumming, and
source refreshes stay off the render/control path. Bounded cache containers and
GPU adoption retain the existing nonblocking allocation contract; this is not
a claim of allocation-free hard real-time command recording.

## Limits

This step provides soft diffuse lighting at uniform-grid resolution. Lightmaps,
UV2 unwrapping, adaptive probe placement, glossy transport, refraction, and
normal-map transport are deferred. Static material alpha/transmissive geometry and
indirect-baked rectangle lights are explicitly refused; mark those contributors
dynamic/live. Direct rectangle lighting remains supported by the renderer.
Layer compositing, masks, and image effects do not modify the offline material
transport; use static opaque set geometry as bake contributors.
Visibility moments reduce wall leaks but cannot recover detail below the grid
spacing or fix probes placed inside solid geometry. Deterministic bake bytes
are tested for identical inputs on the same build; cross-architecture offline
floating-point equivalence is not asserted. Physical genlock validation still
requires the external multi-machine hardware procedure.

## Verification evidence

Initial full builds of `windows-msvc-debug-gui` and `windows-msvc-release-gui`
passed on 2026-09-07. Each preset passed all 143 CTest entries with zero
failures (178.00 seconds Debug; 157.36 seconds Release). The Windows packaging
script produced a portable archive containing the baker and its dependency
licenses; the packaged executable also completed the CLI smoke bake.

The CPU suite checks deterministic bytes, analytic constant-environment
irradiance, a closed diffuse emissive box, coloured bounce, rejection of light
leaks through a closed wall, static/dynamic contributor flags, schema
round trips, corruption rejection, sticky failure, stale worker completion,
cancellation, and snapshot fencing. GPU tests compare full-frame and split
renders byte for byte with probes active, verify cancelled upload recovery,
and verify that RSM bounce is disabled.

The native D3D12 presenter test prepares three independent outputs. All three
publish generation N on the permitted control/show frame. Two prepare N+1 and
publish identical changed pixels on its next permit; the third is unready on
activation and retains N's exact pixels afterward. A separate two-session
DirectorFrameBarrier test transports readiness through heartbeats and records
both generations through durable preparation and commit permits. These are
software cohort proofs on one workstation, not a physical multi-machine
genlock capture.

Model-cache tests causally hold a replacement after entry mutation but before
publication. The current recording still sees the previous generation as stale;
an older recording retains its original generation. This guards readiness
against a finishing refresh appearing current too soon. The cache suite passed
20 consecutive runs in each build. Media and projection tests also verify
authored same-path refresh, worker execution, changed pixels/geometry, and
idempotent reuse.

The native editor was exercised with a separate test project: bake settings,
completion, a larger rebake with visible probe progress, and Program held for
an absent configured node. A panel regression test covers Bake/Stop and retained
authored field settings. The packaged CLI successfully reproduced the asset.
The saved test project's default 256-probe smoke bake took 0.22 seconds in Debug
and 0.10–0.11 seconds in Release, including the CLI's 100 ms polling interval.
These small-scene numbers are not a production-set bake-time estimate. Debug,
repeated Release, and packaged runs produced 167,996-byte assets with SHA-256
`625dc3966a4e01c81746115911ebde7517788e1d718658b7df26e1bbb4e78014`.

The follow-up native UI review reproduced two defects in a saved HDR scene:
an old session's activation frame 909 delayed startup by about 38 seconds,
and uniform sampling missed most of the energy in its tiny HDR softboxes.
Fresh readiness now replaces saved activation permits. Importance sampling
recovers roughly 0.26–0.31 average probe radiance in the reviewed scene, compared
with roughly 0.06–0.07 before; the map itself averages roughly 0.25–0.30 after
intensity scaling. The GPU regression uses a rotated directional HDR and compares
baked and live diffuse lighting, and the CPU regression includes a 2 × 2 bright
patch in a 512 × 256 map at the default sample count. The closed-wall checks
continue to reject external light.

The editor and a separate output process on this workstation confirmed the same
activation for startup, two rebakes, and switching between live and baked
lighting during playback. The UI showed probe counts, retained Program pixels,
activation countdown, output confirmation, and completed publication. A separate
review project was saved with fresh baked data; the original project was retained.

Final follow-up validation passed full Debug and Release GUI builds and all
143 CTest entries in each configuration (271.31 s Debug, 179.86 s Release).
The 11-case baked-lighting CPU suite includes visibility continuity and the
32-case GPU scene suite reports a maximum live/baked linear HDR error of
0.00390625 in its directional-environment regression. The final packaged editor
and output both confirmed startup activation 139; native inspection confirmed
the hard direction-cell shading patches were removed. The packaged CLI reproduced
the editor's 256-probe, 4074-sample review bake in 0.55 s with identical SHA-256
`fd5dc00b36b176305a13914b35a579ebcd10407c66c077ff7c94a756038b3e56`.
These measurements concern the small review scene on this workstation.
