# Advanced ICVFX Renderer

Work items for taking the renderer from "competent forward raster with a
deterministic distribution spine" to "photoreal virtual background on an LED
volume". One file per step. Nothing here is implemented unless
[advanced-renderer-status.md](../advanced-renderer-status.md) says so; that
document is the record of what exists, these are proposals for what comes next.

The longer research proposal remains in
[advanced-renderer-integration-plan.md](../advanced-renderer-integration-plan.md).
Where a step here corresponds to a numbered item there, it says so.

## The target, stated plainly

This project will ultimately become an open source piece of software.

Photorealism on a volume is not primarily a renderer feature count. It is:

1. The wall showing the **correct perspective for the tracked camera**, or the
   shot reads as a big television no matter how good the pixels are.
2. **Content and baked lighting** that were already photoreal before the frame
   path saw them.
3. **Colour that survives the LED processor and the camera's IDT.**
4. **No temporal or spatial artifacts** — aliasing, seams, ghosting — because a
   camera pointed at a wall integrates them into the plate permanently.

The strategic choice this set of documents makes: **bake and light well rather
than chase dynamic Lumen-class GI.** Most volume content is a static set with a
moving camera, which is exactly the case an offline bake plus good IBL solves at
very high fidelity, and it moves the expensive light transport off the frame
path entirely. See [15](15-baked-global-illumination.md).

## Order

Tracks 1 and 2 can proceed in parallel. Track 3 depends on track 2. Track 4 is
what makes photoreal *content* usable at all and can start any time.

**Track 1 — whether you can shoot at all.** None of this is image quality.

- [01 — Camera tracking and inner/outer frustum](01-camera-tracking-and-inner-outer-frustum.md)
- [02 — Lens model](02-lens-model.md)
- [03 — Genlock and present barrier](03-genlock-and-present-barrier.md)
  - [Windows/NVIDIA two-node bring-up](03-genlock-and-present-barrier-bringup.md)

**Track 2 — foundations. Each blocks what follows.**

- [04 — Compute in the RHI](04-compute-in-the-rhi.md) — **done on D3D12**
- [05 — Padded slice render and crop](05-padded-slice-render-and-crop.md) — **done**
- [06 — Render graph execution](06-render-graph-execution.md) — **executor done, frame not migrated**

**Track 3 — the visual jump, in impact order.**

- [07 — Image-based lighting](07-image-based-lighting.md) — **done**
- [08 — Temporal antialiasing](08-temporal-antialiasing.md)
- [09 — Ambient occlusion and local-light shadows](09-ambient-occlusion-and-local-light-shadows.md)
- [10 — Display transform and calibration](10-display-transform-and-calibration.md) — **done**; the
  on-set procedure is [10 — Measuring a wall](10-wall-colour-procedure.md)
- [11 — Bloom, fog and reflections](11-bloom-fog-and-reflections.md) — **done**; reflections stop at the environment by decision

**Track 4 — scale and content.**

- [12 — Culling, instancing and LOD](12-culling-instancing-and-lod.md)
- [13 — Texture streaming](13-texture-streaming.md) — GPU mip residency implemented; BC7/BC5 ingest added in step 14
- [14 — Material model and ingest](14-material-model-and-ingest.md) — **implemented**; six glTF extensions, thin transmission, compressed packages and background imports
- [15 — Baked global illumination](15-baked-global-illumination.md) — **implemented**; offline diffuse probes, bake/rebake UI, and shared Program asset-generation commit

**Cheap and already half-authored.**

- [16 — Photometric units and spot lights](16-photometric-and-spot-lights.md) — **implemented**; calibrated lux/lumens/candela, colour temperature, and photometric spot cones

**Assessment follow-ups.**

- [17 — Assessment and remaining items](17-assessment-and-remaining-items.md) — roadmap for items 18–36
- [18 — Temporal stability in the inner frustum](18-temporal-stability.md) — **implemented**; padded plates, depth rejection, temporal AO, geometric specular AA, and cohort restart evidence
- [19 — Shadow stability](19-shadow-stability.md) — **implemented**; cascade blending, fixed 5×5 filtering, sun-size penumbrae, bounded contact shadows, and MASK casters
- [20 — Texture sampling](20-texture-sampling.md) — **implemented**; per-view glTF samplers, anisotropy, sampling LOD bias, and bounded sampler reuse
- [21 — Object motion blur at physical shutter](21-object-motion-blur.md) — **implemented**; camera-excluded object velocity, fixed shutter reconstruction, guarded slices, and held-frame preservation
- [22 — Shader floating-point contract](22-shader-floating-point-contract.md) — **implemented**; all-shader IEEE strict arithmetic, denormal preservation, SM 6.2, compiled bundle identity and GPU comparison benchmark
- [23 — Cohort render-conformance self-check](23-cohort-render-conformance.md) — **implemented**; isolated sixteen-frame fixture, twenty-one pass digests (including step 28), authenticated heartbeat evidence, Program gate and Output Monitor controls
- [28 — Lens-driven depth of field](28-lens-driven-depth-of-field.md) — **implemented**; physical wall blur compensation, replicated lens controls, a fixed plate gather and guarded seams

## Invariants every step must hold

These are not style preferences. Each one is a rule the existing renderer
already keeps, and a step that breaks one produces a fault that only appears on
a wall, during a take, in front of a crew.

**The frame path never waits on a disk.** No file read, no stat, no hash, no
lock that can block. Asset residency follows the `ModelCache` /
`MediaCache` / `OutputProjectionCache` pattern: a process-wide worker, `try_lock`
observation, sticky failure, generation-fenced refresh, bounded budgets,
transactional GPU adoption. Never a timer.

**Fixed allocation.** Renderers allocate at construction. A scene past a bound
is refused and counted, not absorbed into an allocation while a command list is
being recorded.

**Two nodes must agree exactly.** Anything a node derives must come from
replicated state, evaluated by the same expression. The traps, in rough order of
how easy they are to fall into:

- Deriving anything from **this node's slice** rather than the authored full
  canvas. Cascade fits, culling, LOD selection, texture residency, jitter — all
  must use the full-canvas camera. See the note in each step.
- Deriving anything from **distance to the camera** when the camera differs per
  surface (shadow atlas slots, LOD, streaming priority).
- **Per-node random or wall-clock seeds.** Sampling positions go in absolute
  canvas coordinates and are keyed by the exact 64-bit show frame. There is a
  tested helper for this already.
- **Neighbourhood passes without an apron.** Any filter that reads a neighbour
  pixel seams at a node boundary unless [05](05-padded-slice-render-and-crop.md)
  lands first.

**Capability reporting is honest.** A flag in `rhi::DeviceCapabilities` means
"this RHI can actually do this and a test proves it", not "the adapter probably
supports it". Advanced features fail readiness rather than silently degrading.

**Nothing degrades quality adaptively.** A node that missed its budget reports
or holds according to sync policy. It never renders a cheaper frame than its
neighbour, because that is a visible seam that looks like a hardware fault.

**Authored state is replicated and digested.** Every new field that changes what
a node draws goes in `scene::RendererSettings` (or the scene), gets clamped once
at the model boundary, and joins `renderer_settings_digest` /
`scene::digest`. Otherwise a Director accepts capability evidence gathered for a
different look.

**A feature that is drawing nothing must say why.** Every reason a frame-path
feature declines is individually legitimate — no tracked camera, a switch off,
this output not bound to a wall, geometry that does not intersect — and from the
wall they are all the same black rectangle. A node that cannot tell the operator
which one it is has made a fault indistinguishable from a correct idle, and the
operator finds out on camera. Name the declining condition, once per reason, on
the node that declined it. This is the counterpart to honest capability
reporting: honest *idle* reporting.

**Overwriting is a policy, and a policy is counted.** Several places on the
tracking path deliberately keep one slot and let the newest value win: a pose
belongs to a frame, and a stack of late ones is worse than a fresh one. That is
correct. What is not correct is doing it silently. An uncounted overwrite is
indistinguishable downstream from a value that never arrived, and the two point
at different machines - the link, or the machine that could not drain it. Every
slot with newest-wins semantics counts what it displaced, on the side that
displaced it, and a reader that empties such a slot is named `take`, not `get`:
a peek that leaves the value behind makes an overwrite unobservable by
construction.

**A test may not name the function under test on both sides.** An assertion of
the form `f(x) == <restatement of f>(x)` cannot fail, whatever `f` does, and it
is worse than no test at all because it reports the area as covered. State the
property in the terms of the thing being built instead: not "the inner view
equals `view_for_slice`", but "a point on the wall projects to the same wall
pixel through the plate's view as through the outer one". Two tests of the wrong
shape let the inner frustum ship for months with a projection that never
consulted the wall, and both passed the whole time.

**A round-trip test proves nothing about a field left at its default.** Both
ends read zero and compare equal whether the value was carried or dropped on the
floor, so a fixture that default-initialises anything is blind to exactly the
field most likely to be forgotten. Give every field a distinct non-default value
that a dropped field could not produce — and when a struct crosses a wire,
remember that the drop is likelier in the hand-written field-by-field copies at
each end than in the codec.

**Wired up through the UI** Simply passing a test does not make a feature usable.
These features should be usable in the software through the UI. New panels,
preferences tabs, etc. can be added to facilite this as needed. Provide a series of
instruction on how to test the features in the UI, and run the application to view
and test yourself before calling it done.

## Shape of each document

Each file states why the step exists in shot terms, the exact seams it touches,
a design that fits the invariants above, the multi-node hazards specific to it,
the tests that would prove it, and the decisions still open. They are written to
be picked up cold.



# Media Server

Media Server is a cross-platform virtual-production media-server and render engine
built for nDisplay-type realtime ICVFX workflows, as well as video and media playback and editing. 
It is building toward an operator workflow familiar from motion
graphics and compositing tools: author a project in a layer/timeline editor,
then run the same committed project on one machine or a synchronized set of
render/output nodes.

The repository is a work in progress. The project, wire, and persistence
schemas may change while the product is being defined. Backward compatility or project migration is unimportant.
Breaking changes are okay.

All modifications should result in both a debug and release build

## What this optimizes for

This runs on a stage, during a shoot, with an operator standing at it. That
single fact decides most of the trade-offs below, and a feature that gets it
backwards will feel wrong no matter how correct it is.

**The show is rebuilt constantly, not deployed and left alone.** Plates are
recut between takes, surfaces move when the car does, a grade changes because
the sun did. Anything that assumes the project is settled - caches keyed on
"this will not change", work deferred because "it only happens once" - is
assuming the wrong thing.

**A stall is worse than a stale answer.** The operator is mid-gesture with a
crew waiting. Two seconds of frozen UI costs more than a readiness figure that
is a few seconds out of date, and far more than a check nobody asked for.
Correctness that arrives after the moment has passed is not correctness.

**The frame path never waits on a disk.** Nothing in the control loop reads a
file's bytes, hashes one, or blocks on I/O. Presence is a stat. Content is read
only when an operator asks for it, and then it says so while it works. This is
a hard rule, not a performance target: the loop that misses its boundary is the
loop that drops a frame on the wall.

**Nothing expensive runs on a timer.** Periodic work on the control thread is a
periodic stall, and it is the kind that survives review because each occurrence
looks small. If something must be rechecked, either make the recheck cost a
stat or give the operator a control and let them decide when.

**Media integrity is not the thing being protected.** The files are on a local
share that people are actively managing; a wrong file shows up as a wrong
picture on a wall someone is looking at. Byte-level verification exists and is
worth having, but it is an operator-invoked answer to a specific suspicion, not
a tax on every document edit.

**Direct manipulation before controls.** A clip moves between tracks by being
dragged there; tracks, clips, objects, models, lights, and groups reorder by being
dragged. Reach for a button when there is nothing to drag - creating an empty
container, invoking a scan - and not before. A row of two-letter buttons is
what a UI looks like when nobody could decide what the gesture was.

**Name what is, not what is not.** A clip in no track gets no header announcing
that; its position already says it. State the presence of a thing, never the
absence of one, and never invent a container to hold the exception.

**Everything in its place, so as not to overwhelm.** Information and actions are presented or made obvious when relevant.
The operator is busy, exhausted, and under stress, so unnessary information about status, or actions that are irrelevant to the
current software state should be hidden or minimized. Buttons or actions that are only pertinant to a specific UI/UX
feature should be placed next to that feature. Actions with broad usage such as copy/paste, undo, etc. can be put undet the
Edit menu just like other software.
