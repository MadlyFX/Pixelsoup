# 08 — Temporal antialiasing

**Track 3. Second-biggest perceived jump, and unusually well set up already.**
Depends on [05](05-padded-slice-render-and-crop.md). Benefits from
[04](04-compute-in-the-rhi.md) but does not require it.
Corresponds to plan step 19's temporal contract.

## Why

There is no antialiasing of any kind — no MSAA, no post AA. On an LED wall the
content is displayed at or near 1:1 pixel, so every geometry edge is a hard
staircase, and a camera panning across it produces crawling edges that read as
digital artifact rather than as a set. This is usually the first thing anyone
notices about the current image after the missing reflections.

## Why this is easier here than usual

Almost every TAA prerequisite already exists, because the temporal contract was
designed before the temporal passes:

- **Velocity** — `RG16F` product, with the correct Initialize/Advance/Hold
  semantics: reset records zero, Advance records motion, Hold reproduces the
  original frame's velocity rather than advancing it again.
- **Object ID** — `R32_UINT`, for disocclusion rejection.
- **Coverage** and a **coverage-aware product depth**, so media edges are handled.
- **`TemporalHistory` / `HistoryKey` / `HistoryDecision`** — per-surface
  Initialize / Advance / Hold / Reset, keyed by scene, document version, surface,
  canvas, slice, extent, camera source, camera projection, view identity and
  renderer profile, with camera pose deliberately excluded so a moving camera
  produces motion vectors rather than a reset.
- **256 history slots** with deterministic LRU eviction, so Program, Preview,
  Stage and every mapped view already have independent history identities.
- Reset on seek, camera cut, resize, mapping change, profile change, document
  edit, topology change, and changed repeated frame — all already decided and
  tested.

What is missing: jitter, GPU history colour targets, and the resolve.

## Seams

- `engine/scene/include/mediaserver/scene/canvas.hpp` — jitter applied to the
  projection. It belongs here, beside `view_for_slice`, because the jitter offset
  must be expressed in **canvas** pixels so a slice's sub-pixel offset matches
  what the full-canvas render would have used.
- `engine/render/src/scene_renderer.cpp` — jitter into the view constants;
  history colour targets per surface.
- `engine/render/include/mediaserver/render/slice_apron.hpp` —
  `SliceApronRequirements::taau` already exists (currently 2 px when
  `settings.taau` is authored). Confirm the value against the chosen
  neighbourhood clamp radius.
- `engine/render/src/hybrid_scene_renderer.cpp` — the resolve pass and history
  lifetime, driven by the already-published `HistoryDecision`.
- **New shader** — the resolve. Compute if [04](04-compute-in-the-rhi.md) has
  landed, otherwise a fullscreen raster pass; the algorithm is identical.
- `scene::RendererSettings::taau` — the authored flag already exists and
  currently fails closed. This step is what makes it a capability rather than a
  refusal, so `evaluate_renderer_readiness` and the `USE RASTER` escape hatch in
  Project Settings both need updating when it lands.

## Design

**Jitter.** Halton(2, 3) with index `show_frame % sequence_length`, sequence
length authored (8 or 16). Applied as a sub-pixel offset to the projection's
`left/right/bottom/top` in canvas units. Derived from the exact 64-bit show frame
and nothing else — no frame counter, no wall clock, no per-node index. On a
`Hold` the jitter must be the **same as the held frame's**, not the next in
sequence, or a repeated frame is a new temporal sample and the whole
hold-is-not-a-sample contract breaks.

**History targets.** One colour history per surface, at the padded extent, in the
same `RGBA16F` scene-linear format. This is the new resource cost and it is per
surface, so it must be bounded and counted in `RendererDiagnostics` alongside the
existing history slot counters. A surface that cannot get a history target must
report, not silently run without TAA.

**Resolve.** Standard: reproject the previous frame through velocity, reject on
depth and object-ID mismatch, clamp to the neighbourhood colour box of the
current frame (variance or min/max over 3×3), blend with a fixed weight. Every
constant fixed and replicated; no adaptive blend weight based on local motion
magnitude unless the expression is identical everywhere.

**Disposition mapping.** The renderer already receives an explicit
`SceneHistoryAction`:

| Disposition | Resolve behaviour |
| --- | --- |
| Initialize | Output the current frame; write history. |
| Advance | Full reproject-and-blend. |
| Hold | Output the previous resolve unchanged. Do not accumulate. |
| Reset | Same as Initialize; discard history contents. |

This mapping is the whole reason the temporal contract exists. It should be a
table, not scattered conditionals.

## The risk worth stating plainly

TAA on a wall that a camera is filming is not the same trade as TAA in a game.
Ghosting and smearing under motion get integrated into the plate permanently and
cannot be graded out, whereas aliasing at least looks like a texture. A volume
DP will reject visible smear faster than visible aliasing.

So: implement a spatial fallback as well (SMAA or a good FXAA) and make the
choice authored, replicated and digested. Also keep the TAA blend conservative
and the rejection aggressive — err toward aliasing rather than smear.

## Multi-node hazards

- Jitter derived from anything other than the show frame.
- The neighbourhood clamp reading outside the padded region and clamping to the
  edge. The apron must be at least the clamp radius plus one pixel for the jitter
  offset. This is why [05](05-padded-slice-render-and-crop.md) is a hard
  dependency.
- Two nodes on different `HistoryDecision`s for the same show frame. The decision
  is already derived from replicated state, so the risk is in the resolve reading
  local state instead of the published decision.
- A node whose history target allocation failed running without TAA while its
  neighbour runs with it. That is a visible seam; hold or refuse instead.

## Tests

- Jitter: `show_frame` N gives a known offset; the sequence repeats with period
  `sequence_length`; a held frame reuses the held offset.
- **Repeat frame N twice → byte-identical output.** This is the contract test.
- Seek away and back → `Reset`, and the first frame after has no contribution
  from before the seek (no ghost).
- A static scene over many frames converges and then stops changing.
- Two-node versus full-canvas byte-exact with TAA active over a multi-frame
  sequence, not just one frame — temporal state is where a per-node divergence
  compounds.
- A moving object over a static background: the background does not smear.
- History target exhaustion is reported, not silently skipped.

## Definition of done

Edges are stable, a repeated show frame is byte-identical, a seek resets cleanly,
two nodes agree over a sequence, and `scene::RendererSettings::taau` reports Ready
instead of failing closed.

## What was built

### Jitter

`scene::halton` and `scene::jitter_for_frame` live beside `view_for_slice`,
because the offset has to be expressed in **canvas** pixels. Everything that
converts it to projection units divides by the canvas extent, which every node
agrees on, and never by its own slice - the two are equal in exact arithmetic
and not equal in the last bit, which is the whole difference between two nodes
converging on one image and converging on two.

The hold contract comes out for free. `TemporalHistory` reports Hold exactly
when the show frame repeats, and the offset is a pure function of the show
frame, so a held frame reuses its offset by construction rather than by a
branch somebody could forget. That was worth more than it sounds: it is the one
place in this step where the obvious implementation and the correct one are the
same code.

Two paths reach a jittered projection. A canvas slice rebuilds its window
around the offset and adds the same number to both edges of an axis, so the
shared boundary between two slices stays the same expression over the same
integers and the seam cannot open. An output that has already resolved an
asymmetric view has only a matrix left, and `scene::jittered` shifts its
asymmetry terms instead. The scene math suite asserts the two agree, which is
what caught a sign error on y: reasoning from device-space handedness gave the
wrong answer, and matching the window form gave the right one.

### The resolve

`TemporalResolve` owns the colour history and the pass that folds one frame
into it. The **disposition table is CPU-side**, so the shader only ever runs
for an Advance - Initialize, Reset and Hold are a copy, a copy, and nothing.
That mapping is the entire reason the temporal contract exists, and a shader
that re-derived it would be a second place for it to be wrong.

Rejection is off-screen reprojection, an object-ID mismatch against the
previous frame's ids, and a 3x3 neighbourhood colour box over all four
channels. Alpha is in the box because an isolated compositor layer is
premultiplied and its coverage ghosts exactly as its colour does.

**The history is filtered by hand.** Bilinear through a sampler is not
guaranteed identical between implementations, and this value feeds back into
itself every frame - a last-bit difference between two nodes does not stay a
last-bit difference, it accumulates until the seam is visible. Four loads and a
fixed lerp order cost almost nothing and are exactly reproducible.

The blend weight is fixed and deliberately conservative. An adaptive weight
based on local motion is the standard trick and the wrong one here: two nodes
would have to evaluate it identically for every pixel of a shared edge.

### The spatial alternative

`SpatialAntialias` is an edge-directed blend in the FXAA family, holding no
state. It is not a degraded temporal pass, it is the other trade: it cannot
ghost because it cannot remember, and it resolves thin geometry far less well
for the same reason. On a wall a camera is filming, smear is integrated into
the plate permanently and cannot be graded out, so a DP may well take this one.

An absolute contrast floor keeps it off flat areas. Without one the whole wall
picks up a uniform softness that reads as a renderer fault rather than as
antialiasing.

### The authored surface

`taau` stops being a future-feature carrier - `requested_future_features` no
longer names it, and the `USE RASTER` escape hatch no longer clears it, because
that button exists to discard requests for passes that do not exist and this
one now does. `spatial_antialiasing` and `taa_sequence_length` join it in the
document, the digests and the wire.

Two authored booleans, one decision: `scene::antialiasing_for` is the only
place that resolves them, and the editor's buttons write them so that only one
can be set. A caller that tested them itself would eventually test them in a
different order from the renderer and put two nodes on different passes - which
is the duplicated-shape hazard this plan keeps recording, in its other form.

### What the work found

The apron was already right, and the reason is worth writing down: two pixels
is one for the 3x3 clamp radius and one for the sub-pixel jitter, which reaches
half a pixel and so can pull that neighbourhood a whole pixel further out once
rounded. A clamp reading outside the padded region would pin the colour box to
the edge, which is a bright line exactly where two nodes meet.

The restart counter exists because of a case the tests had to be rewritten to
reach. A surface whose show frame jumps is a Reset and starts over
legitimately; a *restart* is the other thing - the CPU decision said Advance
and the GPU had nothing to advance from, because the history target had been
evicted. Only a contiguous frame on an evicted surface gets there, and it is
the case worth counting, because it is the one where a node quietly differs
from a neighbour that kept its target.

### Tests

- **Jitter is a function of the show frame** - Halton against hand-worked
  values, the sequence repeats with its authored period, every offset stays
  inside the pixel, and a repeated frame gives a repeated offset.
- **A jittered slice agrees with the jittered canvas** - the window shifts
  rather than reshapes, two adjacent slices still share a bit-identical
  boundary under jitter, the matrix and window forms agree, and zero jitter
  reproduces the unjittered projection exactly.
- **Temporal antialiasing holds repeats and reports restarts** - on the GPU:
  the jitter moves the image, a repeated show frame returns the previous
  resolve byte for byte and twice, an evicted history is counted as a restart,
  and the pass costs nothing when unauthored (asserted byte for byte).
- **The spatial pass is a pure function of its frame** - two show frames of a
  static scene come back identical where the temporal pass made them differ,
  it changed something, and authoring both runs only the temporal one.
- **Antialiasing is one authored choice** - no click leaves both booleans set,
  and the jitter period is drawn only behind the pass that consumes it.

## Still open

- **Depth is not part of the rejection.** The plan asks for depth *and* object
  ID; only object ID is here, because the auxiliary depth product is created as
  a depth target and not as a sampled one, so the resolve cannot read it
  without an RHI change. Object ID catches disocclusion, which is the common
  case, and the neighbourhood clamp catches most of the rest; what is missed is
  the same object at a very different depth. Making the depth product
  sampleable is a small, separable change and is the next thing to do here.
- ~~**Two-node byte-exactness with the temporal pass active is not tested.**~~
  Written, and it found two real defects before it passed.
  `two_nodes_and_one_node_agree_byte_for_byte` in
  `engine/render/tests/renderer_integration_tests.cpp` renders a canvas whole
  and as two abutting slices and compares them, over eight frames and again
  over thirty-two.

  **Velocity was expressed in the render target's own UV space**, and a slice's
  target is not the canvas. One canvas pixel of motion was `1/128` of a
  whole-canvas target and `1/96` of a half-canvas one, stored through a
  binary16 velocity and multiplied back out by a different extent at the other
  end - exact for the power-of-two case, not for the other, and the history fed
  the difference back into itself every frame. Measured: **505 of 6144 pixels
  differing, worst case 0.11 of a unit of linear light**, and growing. The
  motion pair is now carried in *canvas-wide* clip space: `scene.vert.hlsl`
  projects it through a `canvas_view_projection` that no node derives from its
  own slice, and `taa_resolve.frag.hlsl` scales by the canvas extent rather
  than the target's. Both endpoints come from matrices every node holds
  identically, so the delta is identical too.

  **The jitter sequence landed on thirds.** With velocity fixed, three pixels
  still differed *on the first frame*, where the resolve is a pure copy - so it
  was not the resolve at all but the jittered projection underneath it. Halton
  in base two produces exact binary fractions and survives being added to a
  pixel index; base three produces thirds, which do not, so `x + jitter` rounds
  one way for one node's pixel index and another way for its neighbour's.
  `jitter_for_frame` now snaps to a 1/256 grid - far finer than the sub-pixel
  structure the sequence exists to sample - and the addition is exact for any
  canvas a wall will have. The first frame is byte-exact.

  What remains is **one unit in the last place of the target format, and it
  stays there**: the same bound after thirty-two frames as after eight. Two
  slices are two different frustums, and a shaded value is the same computation
  over inputs that round differently in their last bit; without a history that
  difference falls below the target format and the comparison really is exact,
  which is why the test asserts byte equality with the temporal pass off and a
  one-ulp bound with it on. The two runs are the measurement that it no longer
  compounds - which was the actual danger, and what the 0.11 figure above was.
- **A restart is not fenced across nodes.** A node whose history target was
  evicted starts accumulating again while a neighbour that kept its target does
  not, and for one frame the two differ along their shared edge. The counter
  makes it visible and a pool at least as large as the surfaces a node renders
  avoids it, but nothing currently *holds* Program when it happens.
- **The `camera_history_digest` question from
  [01](01-camera-tracking-and-inner-outer-frustum.md).** It currently includes
  camera projection, so a tracked zoom resets history every frame and TAA never
  accumulates. This must be resolved before TAA is useful on a tracked camera.
- **Upscaling (the "U" in TAAU).** Rendering the inner frustum below wall
  resolution and upscaling is a large performance win on a volume. Worth doing,
  but it is a second step — get temporal stability at 1:1 first.
- **Does the outer frustum need TAA?** It is only seen as spill and reflection.
  Possibly not, which halves the cost and the history memory.
- **Motion blur** shares all of this machinery and the camera shutter is already
  authored. It is the natural follow-on once the hold/advance contract is proven
  under a real temporal pass.
