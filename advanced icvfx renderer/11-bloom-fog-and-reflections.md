# 11 — Bloom, fog and reflections

**Track 3.** Depends on [04](04-compute-in-the-rhi.md) and
[05](05-padded-slice-render-and-crop.md). Three passes in increasing order of
cost and difficulty; take them in this order.

---

## A — Bloom

### Why

Advanced renders scene-linear `RGBA16F` and values above 1.0 survive the whole
compositor, which means the renderer already *has* the highlight energy — it just
has nowhere for it to go. A practical light or a sky through a window currently
clips to a flat bright shape. Bloom is what turns that into something that reads
as a real light source at real brightness, and on a wall it is one of the
strongest cues that the image is photographic rather than rendered.

Cheap, high impact, and it is the natural first consumer of the compute path.

### Design

Progressive downsample/upsample (the Jimenez/COD approach): a mip chain, a
13-tap downsample, a 9-tap tent upsample, additive combine with an authored
strength. Energy-conserving — no threshold, or a very soft knee, because a hard
threshold makes bloom pop as a light crosses it, which is worse than no bloom.

All constants authored, replicated, digested. The chain length is derived from
the padded extent and must be identical across nodes; derive it from the
**canvas** extent and the slice's proportion of it rather than from the local
target size, or a node with a differently sized slice takes a different number of
mips and blooms differently.

`SliceApronRequirements::bloom` already exists as a lane. Its value is the
effective radius of the full chain in canvas pixels — for a long chain that is
large, and it may hit `slice_apron_limit`. That refusal is correct behaviour and
must stay a refusal rather than a silently shortened chain.

Applied at the output boundary, before the display transform in
[10](10-display-transform-and-calibration.md), in linear light.

---

## B — Volumetric fog and atmosphere

### Why

Exteriors and any lit interior with atmosphere need it, and it is one of the
cheapest ways to make a set feel like it has depth and scale. It also does
something specifically useful on a volume: fog softens the far distance, which
hides the resolution limit of the background content exactly where a static
backdrop would otherwise betray itself.

### Design

Froxel volume — a view-frustum-aligned 3D grid (typically 160×90×64), compute:

1. Scatter/extinction per froxel from authored fog parameters plus light
   contributions, sampling the shadow cascades.
2. Integrate front-to-back into a scattering/transmittance volume.
3. Apply per pixel in the composite by sampling at the pixel's depth.

**Requires 3D textures in the RHI.** `TextureDesc` is 2D with array layers today.
[10](10-display-transform-and-calibration.md) wants a 3D LUT too, so add the
dimension once, for both.

**The multi-node problem is real here.** The froxel grid is aligned to the view
frustum, so each node's grid covers a different volume of world space with
different froxel boundaries. Two nodes would then quantise the same world point
differently and produce a visible discontinuity along the seam.

Fix: build the froxel grid from the **authored full-canvas camera frustum**, not
the node's slice frustum, and have each node evaluate only the froxels its slice
covers. Every node then uses the identical grid partition, and a shared world
point falls in the same froxel on both. This is the same discipline the shadow
cascades already use and it is not optional.

Temporal reprojection of the volume is standard practice for reducing noise, and
it must ride the existing `HistoryDecision` rather than a second temporal path.

---

## C — Reflections

### Why

With IBL ([07](07-image-based-lighting.md)) landed, smooth surfaces reflect the
environment but not each other — a car reflects the sky but not the ground it is
parked on, and a wet floor reflects nothing standing on it. That is the remaining
obvious tell.

### The problem that has no clean answer at a node boundary

**Screen-space reflections cannot be made seam-exact by an apron.** A reflection
ray from a pixel near the slice edge can travel arbitrarily far across the screen
— potentially entirely out of this node's slice and deep into its neighbour's.
No finite apron bounds it. `SliceApronRequirements::reflection` is set to 32 px
when ray tracing is requested, which bounds a *denoiser's* neighbourhood, not a
ray's reach.

Three honest options:

1. **SSR on single-producer surfaces only.** Enable it for a surface rendered by
   one node — the inner frustum under
   [01](01-camera-tracking-and-inner-outer-frustum.md) usually is, and it is also
   the region the camera actually photographs. The outer frustum falls back to
   IBL. This is the pragmatic answer and it maps well onto how a volume is
   actually shot.
2. **Render a wider region for reflection purposes** on every node. Expensive and
   still not exact.
3. **Wait for ray-traced reflections**, which read a full-scene TLAS and are
   therefore slice-independent by construction — the same reason the shadow
   cascades are.

Recommend option 1 now, option 3 as the real answer. Whichever is chosen, it must
be an explicit authored policy that fails closed, not a per-node decision.

### Ray-traced reflections

Needs, in order: acceleration structures and ray queries in the RHI
(`acceleration_structures` and `ray_queries` are honestly `false` today), BLAS
per immutable mesh with a cache, a TLAS built from stable instance order (the
`SceneBridge` already keys stable records by node and content identity, which is
exactly the input a deterministic TLAS needs), alpha-tested hit behaviour, and
then denoising with a history that rides the existing temporal decision.

That is a large body of work and it is the point where this project would start
genuinely resembling the donor engines' RT paths. It is correctly deferred until
tracks 1–3 are done.

---

## Tests

**Bloom**

- A single bright pixel produces a symmetric, energy-conserving halo.
- Chain length is identical for a full-canvas render and for each of two slices.
- Two-node versus full-canvas byte-exact with bloom active.
- A radius past `slice_apron_limit` is refused, not shortened.

**Fog**

- Froxel boundaries are identical for two nodes with different slices — assert on
  the grid partition directly, not just on pixels.
- Uniform fog over a uniform background produces a uniform result across a node
  boundary.
- A repeated show frame is byte-identical with temporal reprojection active.

**Reflections**

- SSR is refused on a multi-producer surface rather than silently seaming.
- A mirror plane reflects a known object at the geometrically correct position.
- Fallback to IBL where the ray leaves the screen is continuous, not a hard edge.

## What was built — Part A

Bloom, end to end and authored. Parts B and C are untouched; what is missing is
under "Still open" rather than implied.

### The chain belongs to the canvas

`render/bloom_chain.hpp` is its own file for one reason, and it is the reason
this step was worth taking carefully. A mip level is a *partition* of the image
into blocks, and two nodes holding different rectangles of the same canvas will
partition it differently unless they are made not to. A slice starting at canvas
x=1280 puts its level-six boundaries at 1280, 1344, 1408 — which happen to be
the canvas's own, because 1280 divides by 64. Start it at 1300 and every
boundary in that node's chain falls where no other node has one, every level
averages a different set of pixels, and the wall carries a bloom seam that moves
whenever the slice does.

So the depth comes from the canvas and nothing else, and a rendered rectangle
off the coarsest level's grid is **refused** rather than quietly resampled. A
rectangle reaching the canvas edge counts as aligned there, because the short
last block is short for every node including one rendering the whole canvas.

This is the same discipline the shadow cascades follow and the froxel grid in
part B will have to. It is worth saying plainly here because part B's version of
the problem is harder and the answer is the same shape.

### The apron, and what exactness actually costs

`bloom_chain_apron` is `4 × (2^depth − 1)` canvas pixels rounded up to a whole
number of coarsest-level blocks — 252 becomes 256 for the six-level chain a
1080p or larger canvas takes.

The rounding is not tidiness, and it was found the only way it could be. The
rectangle that must sit on the canvas grid is the **padded** one, and a padded
rectangle is the slice displaced by the apron; an apron of 252 moves a
block-aligned slice four pixels off the grid, `plan_bloom_chain` refuses it, and
bloom refuses **every frame of `scripts/run-scene-demo.ps1`** — the 2560×720
wall split down the middle that is the closest thing here to a real deployment.
The two-node integration test did not catch it: its canvas takes a three-level
chain whose stride is 8, and occlusion's 32-pixel apron happens to divide by 8,
so the padded rectangle landed on the grid by accident. There is now a test for
the demo's own geometry, and the alignment is applied to the *maximum* across
lanes rather than folded into the bloom lane, because any lane can be the
largest and whichever wins is what displaces the rectangle.

An authored **slice** that does not itself start on the grid cannot be rescued
by any apron - the padded rectangle is the slice displaced by whole blocks, so
it is off the grid exactly when the slice is - and it is refused. A wall laid
out on any sane panel boundary satisfies this. That is a large number and it is the
honest price: for the authored region to hold the values a single node would
have produced, every texel the chain reads has to have been rendered, and the
dependency cone of one coarse texel is wide.

`max_bloom_levels` is six, and it is tied to the default `slice_apron_limit` of
256 on purpose — a chain whose apron the default budget cannot pay for would
refuse every frame, and the default ought to work. The cap is a constant, not a
budget: it is the same number on every node, so it does not reintroduce the
node-dependent chain length the file exists to prevent. Past the budget the
refusal comes from `padded_slice` and there is no frame to put a shortened chain
in, which is exactly what the plan asks for.

The number is deliberately conservative — it bounds the kernels rather than
measuring their falloff. The halo test measures the real span of a three-level
chain at thirteen pixels against an apron of twenty-eight, and that margin is
the point: the exactness claim should not rest on an off-by-one in a kernel
radius.

### Explicit taps, not the 13-tap kernel

The plan asks for the Jimenez/COD 13-tap downsample. It is not what was built,
and the reason is the same one that has decided every filtering question in this
renderer: the 13-tap is *defined* in terms of bilinear samples at half-texel
offsets — that is what lets thirteen fetches cover thirty-six texels — and a
bilinear tap is precisely what nothing here may depend on, because filtering
precision is not guaranteed identical between implementations and two machines
each render half of this wall.

What replaces it is the same idea written out: a separable `[1,3,3,1]/8` tent
over a 4×4 block on the way down, and a two-tap-per-axis tent on the way up
whose weights come from the destination texel's *parity* and are therefore
exactly 1/4 and 3/4. Both are wide, smooth, energy conserving by construction,
and identical on every machine.

### Energy, and why the upsample averages

The walk back up **averages** each level with what the downsample left there
rather than adding to it. Adding sums every scale in the chain and hands the
combine stage several times the light the frame emitted, which would make an
authored strength of one mean something different on a six-level canvas than on
a three-level one. Averaging weights the scales geometrically and leaves the
chain carrying the energy of the frame it came from — so the strength is a
property of the look rather than of the canvas size. The halo test asserts it
directly: light in, the same light out.

No threshold, per the plan. A hard threshold makes a light pop the moment it
crosses one, which a moving light will find repeatedly during a take.

### Where it runs

After the antialiasing resolve — spill belongs on the resolved image rather than
on one frame's jittered sample of it — and before anything crops, because the
chain reads a neighbourhood and every pass that does lives inside the padded
region. In linear light, before the display transform from
[10](10-display-transform-and-calibration.md): adding spill to encoded values
would make a given number mean a different amount of light depending on how
bright the pixel already was.

It writes into the chain's own full-resolution level rather than over the frame,
so the colour the temporal resolve published — and stored as its history — is
untouched. **A history with bloom folded into it would bloom its own bloom, a
little more every frame.**

### Authoring

`bloom_strength` only, through the whole document path: struct, clamp, both
digests, schema key, parser, writer, and a control in Project Settings. The
chain's *shape* is deliberately not authored, and the omission is the design: a
per-node radius is the one knob that would make two halves of the wall disagree.

### Tests

- **The depth belongs to the canvas** — every slice of a 1920×1080 canvas,
  however narrow, reports the same depth; small canvases halve fewer times; a
  canvas too small to halve gets a chain of nothing rather than a refusal.
- **Two slices and the whole share one grid** — asserted on the partition
  itself rather than on pixels, because that is where the property lives. If
  the block boundaries agree the averages agree; if they do not, no comparison
  of output will explain why.
- **An unaligned rectangle is refused** — one pixel off the grid is off, and
  the case that would otherwise produce a perfectly plausible chain averaging a
  different set of canvas pixels from its neighbour.
- **The apron covers the whole chain**, and the default budget pays for exactly
  the deepest chain this file will build. The two constants are related on
  purpose; if either moves without the other, the test fails.
- **A bright block blooms into a symmetric halo** — on the GPU. Energy in
  equals energy out, the halo falls off with distance, and the result is
  symmetric under the half turn that maps the chain's grid onto itself. Radial
  symmetry is deliberately not asserted: a mip chain is block-structured by
  construction, and demanding a round halo would be demanding something the
  algorithm does not provide.
- **A chain past the apron budget is refused, not shortened** — and with bloom
  off the lane costs nothing.
- **Two nodes and one node agree** with bloom live, in the byte-exact
  comparison from steps 07–10. Bloom is byte-exact across nodes on its own.
  Combined with temporal antialiasing it multiplies that path's one-ulp
  residual to four — bloom adds `strength` times a sum over the chain, so it
  amplifies whatever difference reaches it — and, measured at eight frames and
  again at thirty-two, **the amplitude does not move**. The affected area
  spreads, from thirty-two byte lanes to sixty-eight, because spreading a
  difference is what a blur does. A floor, not a drift.

## Definition of done

Highlights bloom, atmosphere reads with depth, both are byte-exact across nodes,
and reflections are either correct or explicitly refused — never silently
different on two halves of a wall.

### The compositor was throwing all of it away

Bloom was authored at full strength on an Advanced project and the screen did
not change. The pass was not at fault, and neither was the document: the value
reached `RendererSettings`, the chain planned, the dispatches ran.

`CompositorSceneExecutor::execute_plan` took its layer colour from
`renderer.target()`. That is the **raw scene target** - what the geometry pass
drew - while the temporal resolve, the spatial filter and bloom each publish
into a texture of their own and leave that one untouched. The published result
is `last_products().color`, which the executor read for its readiness flags two
lines earlier and then ignored. Every path through the compositor funnels into
this function, so on every one of them the frame that reached the screen was the
one from before any of those three passes ran.

So this was never only a bloom bug. **Temporal and spatial antialiasing have
been silently discarded by the compositor for as long as they have existed** -
which is why nothing looked different when they were switched on either.

Two things let it survive this long. The renderer's own tests drive
`render_frame` and read `products.color`, so they were correct and passed; the
executor is the only thing that reads `target()`, and nothing tested what it
picked. And the bloom tests, until this was found, only ever asserted that two
halves of a frame *agreed* - which they do perfectly well when the frame has no
bloom in it. `authored_bloom_changes_the_published_frame` now asserts the thing
that was missing: that the published frame is different with the pass on.

### Reporting what actually ran

The compositor bug was invisible from the operator's seat, and that is the part
worth fixing permanently: an authored pass that never reaches the screen looks
exactly like a pass that does nothing. Project Settings had already solved this
once for the environment - `EnvironmentStatus` exists because a path that does
not resolve, a file that is not Radiance and a project still on Basic all
produce the same hemisphere - so the renderer passes now follow it.

`RendererPassStatus` is published on `RenderProducts` beside the readiness, and
carries the authored backend, the **selected** backend, and one `PassState` per
pass: `NotAuthored`, `Active`, `BackendUnsupported`, `Refused`. It is filled by
the renderer rather than re-derived by the panel, because only the renderer
knows whether a pass refused and a second implementation of that judgement would
eventually disagree with the first. `CompositorSceneExecutor` carries it out,
and the editor shell fills it in on the way to the dialog - the program renderer
is what knows, and it lives on that side of the boundary.

Project Settings reports it under the controls: the selected backend, one line
naming the passes that ran, and - only when there is one - a line in the warning
colour naming those that did not. A project that authored Advanced and was
handed Basic says so on the Backend row itself, where the decision is made,
rather than leaving an operator to infer it from six inert sliders.

Two lines rather than a row per pass, because the card has room for two and a
list of six mostly-fine states buries the one that is not. `Refused` is the only
state drawn as a fault; the rest are facts, and dressing a fact up as a warning
teaches people to ignore the colour.

The states are asserted in `the_renderer_reports_which_passes_ran`: the same
scene on Advanced and on Basic, with one pass deliberately left unauthored so
that `NotAuthored`, `Active` and `BackendUnsupported` have to be told apart
rather than merely counted.

## What was built — Part B

Fog, end to end: the grid, three compute stages over it, the authored
parameters, and the frame ordering. `render/froxel_grid.hpp` is the piece the
plan calls not optional, and it had to be right before a single froxel was
integrated, so it is described first.

### The grid belongs to the canvas

Same problem as the bloom chain and the same answer. A froxel is a *cell*, and
two nodes holding different rectangles of one canvas will cut the volume into
different cells unless they are made not to - so a world point would sit in one
node's cell and its neighbour's, be lit by two different integrations, and put a
discontinuity down the wall that moves whenever the camera does.

So the partition is the authored full-canvas frustum: columns and rows divide
the *canvas*, and the depth range is the camera's own near and far, which a
slice never narrows - a slice narrows the frustum's window. A node evaluates the
froxels its padded rectangle covers, widened outward to whole cells, and the
cell straddling a seam is integrated by both neighbours identically. Column
indices come from a numerator-first division, the same trick `window_for_slice`
uses for the frustum edge two nodes share.

### The depth warp is quadratic, not exponential

Every reference distributes froxel slices exponentially. This does not, and the
reason is the one that has decided the bloom kernels, the display transform and
the sub-pixel jitter before it: an exponential needs a transcendental, and a
transcendental is exactly the arithmetic this renderer does not let two machines
evaluate independently.

`near + (far - near) * t²` is one multiply, concentrates slices near the camera
where the eye is, and every node computes the same number from the same three
floats. Its inverse needs a square root - which is allowed precisely where a
`pow` is not, because IEEE 754 requires `sqrt` to be correctly rounded and it is
therefore exact to the last bit on every conforming implementation.

The test asserts the two are inverses **on the boundaries**, not merely in the
middle of a slice. A point on a boundary falling into one cell for one node and
the next for its neighbour is the whole failure, and a scene puts geometry on
round distances constantly.

### Storage: an atlas, not a 3D texture — and so no RHI change

The plan makes 3D textures a prerequisite for this step. They are not, for the
same reason [10](10-display-transform-and-calibration.md) reached about the
display LUT: the volume is sampled per pixel at that pixel's depth, the
filtering has to be written out by hand for two machines to agree, and once the
interpolation is manual a 3D texture buys nothing and costs an RHI change. The
froxel volume should be a 2D atlas of depth slices, addressed the way
`pack_display_lut_atlas` addresses the LUT.

That removes the prerequisite the plan puts in front of this step. The volume is
two atlases - scattering and the integrated result - each `160 x 90` per slice
laid left to right across 64 slices, addressed exactly the way
`pack_display_lut_atlas` addresses the display table. One convention for
volumes rather than two.

### Three stages

**Scatter** writes one `(in-scattered light, extinction)` per froxel. It
reconstructs the cell's world position by unprojecting the cell's *canvas* UV at
the near and far planes and walking along that ray, which keeps the
interpolation planar - a froxel boundary is a plane at constant view depth, not
a shell at constant distance from the eye. Light arrives through a
Henyey-Greenstein phase function; the anisotropy is what puts a halo around a
practical aimed at the camera, which is most of why volumetric fog is worth
rendering rather than a depth-faded tint.

Lights come from the **same storage buffer the lit draw reads**, evaluated with
the same falloff, the same cone and the same shadow lookup - directional and
local alike, told apart the way the fragment stage tells them apart. That is the
point rather than an economy: two implementations of one falloff would drift,
and the drift would read as fog that does not belong to the lamp it came from.
`SceneRenderer` publishes the light buffer, the shadow table and the row count
for exactly this.

The shadow lookup is `spot_shadow_visibility` with one difference: a froxel has
no normal, so the slope term has nothing to scale and the bias is its constant
part alone. **Sampling it is not optional.** An unshadowed practical lights the
air straight through the wall it is mounted on - and unlike the surface case,
where the wall at least stays dark, the fog glows *in front of* the wall and
gives the whole set a haze no lamp in the room can account for. That is worse
than fog which ignores local lights entirely, which is why this pass binds the
atlas rather than deferring it.

**Integrate** walks each ray front to back, one thread per column and row.
It uses the analytic integral of in-scattering across a slab of constant
density, `S * (1 - e^(-sigma d)) / sigma`, rather than a midpoint sample of it -
the difference is visible as banding at low slice counts, which is exactly where
a 64-slice grid lives. The running answer is written at *every* slice, not only
at the end, because the apply stage reads the volume at each pixel's own depth.

Beer-Lambert needs an exponential, and that is allowed. The rule this renderer
holds is about *filtering* precision - a hardware sampler is free to interpolate
differently, so every tap is a `Load` - and about transcendentals evaluated on
the CPU, where `deterministic::` exists. An HLSL `exp` is a defined operation on
the instruction set every node in a cohort runs; `gaussian_blur.frag.hlsl` has
used one since it was written. Getting this distinction wrong in the cautious
direction would have meant inventing a deterministic exponential for no reason.

**Apply** resolves the volume at each pixel's depth and composites. The froxel
it reads is keyed to the pixel's **absolute canvas coordinate**, the same
discipline the occlusion kernel's rotation follows: the wall pixel decides the
cell, never the node that owns it. Interpolation is trilinear written out by
hand in a fixed order.

### Every node computes the whole volume

Not just the froxels its own slice covers. 160x90x64 is under a million cells;
computing the identical volume everywhere removes the coverage bookkeeping from
the frame path and leaves no arithmetic for two nodes to disagree about.
`plan_froxel_grid` still reports coverage and restricting the dispatch to it
remains available as a pure optimisation that cannot change the answer - which
is a better position to optimise from than one where correctness depends on the
restriction being right.

### Where it runs, and what that changes

Before the antialiasing resolve and before bloom. Atmosphere is part of the
image rather than a filter on it: it is light scattered toward the lens by the
air the set sits in, so it belongs in the frame the resolve then integrates and
the chain then spills. Applying it afterwards would antialias the set and then
lay haze over the result - a wash rather than depth - and would bloom the fog
rather than the lights inside it.

That required the temporal and spatial passes to take `last_products_.color`
rather than `selected.target()`, which is the same correction the compositor
needed and for the same reason. With fog off the two are the same texture, so
the change is inert until fog is authored.

The grid is deliberately built from the **unjittered** canvas view. The
partition is of the world, not of a sample pattern: jittering it would move
every cell boundary a fraction of a pixel each frame and the atmosphere would
crawl. The temporal resolve still antialiases the fogged frame afterwards, which
is where the jitter belongs.

### Authoring

`fog_density`, `fog_color` and `fog_anisotropy` on the **Environment**, not on
`RendererSettings` - it is content rather than renderer policy, and per scene
rather than per project, because a different scene is a different room and a
room's air is not a capability of the machine drawing it. Full round trip:
struct, digest, schema keys, parser with clamps, writer, and controls in Project
Settings under an ATMOSPHERE heading in the Environment category.

Clamped on the way in, like the map intensity beside it, and tested at both ends
- including the Henyey-Greenstein singularity at anisotropy +/-1, where the
phase function becomes a delta and the integral stops converging.

### Tests

- **A canvas pixel lands in one froxel** - the column and row are functions of
  the canvas and the grid, monotonic, and never run off either end.
- **The depth partition inverts** - and it is asserted *on the boundaries*, not
  merely in the middle of a slice. A point on a boundary falling into one cell
  for one node and the next for its neighbour is the whole failure, and a scene
  puts geometry on round distances constantly. Also that slices are denser near
  the camera, which is the entire reason for the warp.
- **Two slices cover the same grid** - the halves together cover every column
  and meet without a gap; they may overlap by the one froxel straddling the
  seam, which both integrate identically.
- **A degenerate grid is refused** - a zero depth, an inverted camera range, a
  rectangle outside the canvas.
- **Authoring atmosphere changes the published frame** - on the GPU, through
  the whole `HybridSceneRenderer` path, checking both that the pass reports
  `Active` and that it changed more than a quarter of the frame. This is the
  assertion bloom needed and did not have; without it a froxel volume could be
  built perfectly and thrown away, and every determinism test would still pass.
- **Two nodes and one node agree** with fog live - the byte-exact comparison
  now runs three legs: byte-exact without fog or temporal antialiasing, within
  one ulp with fog, within four with both. See below.
- **Atmosphere round-trips and clamps** - the document parse, and both ends of
  the anisotropy range including the Henyey-Greenstein singularity.
- **A practical lights the air, and a wall stops it** - the local-light claim,
  and both halves of it are held to a standard the earlier attempt at a fog
  test was not. Each assertion was checked against a deliberately broken build:
  the air measurement fails when the scatter pass skips positional lights, and
  the occlusion ratio fails when the shadow lookup is stubbed to fully lit.

  The first version of this test did *not* discriminate. It summed the lamp's
  contribution over the whole frame, which measures the lamp lighting the
  **floor** - something that happens whether or not the scatter pass reads a
  single local light. Restricting the measurement to rows above the horizon,
  where there is no geometry at all, is what makes it an assertion about the
  air. An assertion that cannot fail is not one.

### What fog costs the determinism claim

Byte equality survives everything else in this renderer; it does not survive
fog. Five of 24576 byte lanes differ between one node and two, at one unit in
the last place.

The volume itself is node-identical by construction - every node integrates the
whole grid from the canvas-wide matrix, and the apply stage keys each pixel's
lookup to its absolute position on the wall - so nothing in the fog arithmetic
differs. What differs is what reaches it: a pixel's depth is rasterised through
two different frustums, and a last-bit difference there moves the froxel lookup
a hair and the trilinear weight with it. It cannot accumulate, because fog keeps
no history, and the test measures at eight frames and again at thirty-two to say
so.

**The depth product as the cause is inferred**, from the magnitude and from the
fog arithmetic having no other node-dependent input. It has not been isolated
directly, and it is recorded here as inference rather than measurement.

### Atmosphere is applied per layer, and the background belongs to none of them

Reported from the editor as "fog only shows over geometry - the background
clips it", and the mechanism is worth stating in full because it is
architectural rather than a slip.

`CompositorSceneExecutor::execute_plan` renders **every** layer with
`options.transparent_background = true`, including the first, and composites
them over an accumulator cleared to the sky colour. Fog runs inside
`render_frame`, so it runs once per layer - and a layer has alpha zero wherever
it drew nothing. Weighting the in-scattered term by that alpha, which is the
consistent-looking thing to do for premultiplied output, therefore produces zero
atmosphere everywhere no geometry was drawn. No layer owns the background, so
the sky reaches the composite untouched: fog visible only where it overlaps an
object.

That weighting was added earlier in this same step, from reasoning about an
isolated layer in isolation, and it traded a subtle over-brightening for a gross
missing effect. It is reverted: the in-scatter is added at full strength, which
fogs the background exactly once and correctly for a single-layer scene - the
common case - and over-fogs a shared background once per layer when there are
several. That is a real defect, recorded below.

**The honest fix is to apply atmosphere once to the composite** rather than
inside each layer's render, which is what it physically is: light scattered by
the air between the camera and whatever it can see, not a property of any one
layer. That needs a combined depth across layers, which the executor does not
have.

**No fog test would have caught this**, and that is the more useful finding.
Every one drives `HybridSceneRenderer::render_frame` directly; none goes through
`CompositorSceneExecutor`, which is the only path the editor and the wall
actually use. The same blind spot hid the compositor discarding bloom and
antialiasing earlier in this session. That test now exists -
`the_compositor_publishes_what_the_renderer_rendered` in
`compositor_scene_executor_tests.cpp` - and it is held to the standard the rest
of this step is: reintroducing the `renderer.target()` read makes it fail.

It also had to be rescued from a vacuous pass. It gated on
`device.capabilities().advanced_raster()` after opening a device through
`open_device()`, which leaves the backend at its default; the gate was never
satisfied, the test returned before executing a line, and it reported green. It
opens D3D12 explicitly now, and the comment at that line says why.

### What an adversarial review caught that the tests did not

Fog took its matrix from `scene::view_for_slice(scene.camera, canvas, whole)`
unconditionally, never consulting `request.options.view`. Two production paths
supply one: the **ICVFX inner frustum**, which is off-axis from the *tracked*
camera's eye, and **projection-mapped surfaces**. On those frames the depth was
written by that projection and unprojected with the authored camera's matrix, so
the recovered distance belonged to no camera at all.

The visible failure is precisely the class this path exists to prevent. The
outer frustum, which has no explicit view, would get geometrically correct fog;
the inner plate would get fog computed for a camera it was not rendered from;
and `IcvfxCompositor::render` feathers the two together - atmosphere stepping
across the feather, and the step moving whenever the camera did.

A second consequence came from the verification rather than the finding: for a
view-driven render the padded rectangle is the whole canvas, so the origin is
`(0,0)` while the target is the plate raster scaled by plate density. Keying the
froxel column to `scene.canvas` there treats a plate as though it were the
wall's top-left corner. Both are fixed: `record` now takes the matrix the frame
was rendered from *and* the raster that frustum fills, and the slice path passes
exactly what it passed before.

Refusing on a view-driven frame - the reflex this file uses everywhere else -
would have been wrong here, and the review said so: it would fail every ICVFX
frame that authors atmosphere.

**This fix has no regression test.** Two attempts failed to discriminate it: a
depth-ordering assertion, because fog still grows with distance under a
moderately wrong matrix, and a same-projection-two-ways comparison, which
produced byte-identical frames whether or not the fix was applied and therefore
proved nothing. A test that passes with and without a fix is worse than no test,
so neither was kept. The gap is real and is the first thing to close here.

## What was found — Part C

Nothing was built, and one thing was verified that had never been tested.

The plan requires that reflections "must be an explicit authored policy that
fails closed, not a per-node decision". That machinery already exists -
`ray_tracing` authored against a backend reporting no acceleration structures
marks the readiness unsatisfied - and `ray_traced_reflections_fail_closed` now
holds it to that. `acceleration_structures` and `ray_queries` are honestly
`false` on the D3D12 backend, and the test asserts that premise first so it
cannot quietly stop measuring anything.

The contract turned out to be **stronger than the plan assumes**. On
Availability REQUIRED it holds, as expected. On OPTIONAL it does concede - but
what it concedes to is *Basic*, not Advanced-without-reflections. That
distinction is the whole game: a node handed Advanced-minus-reflections would
produce a frame differing from its neighbour's by exactly the shiny surfaces,
which is subtle enough to reach a camera. Falling all the way to Basic and
setting `fallback` makes two nodes disagree about the entire renderer instead of
one term in it - visible immediately, in the place an operator is already
looking.

**Screen-space reflections are not implemented, and no authored control for them
was added.** That is deliberate, and on reflection it is the finished answer
rather than an unfinished one.

This step's definition of done says reflections must be "either correct or
explicitly refused - never silently different on two halves of a wall". Both
halves of that are satisfied today:

- **Correct**: environment reflections from [07](07-image-based-lighting.md) are
  node-identical. The prefilter is proven bit-identical across instances and the
  lit path is covered by the byte-exact two-node comparison.
- **Explicitly refused**: ray tracing holds the frame on REQUIRED and falls all
  the way back to Basic on OPTIONAL, tested above.

Screen-space reflections would *add* the case the definition forbids. The plan
says so itself - "screen-space reflections cannot be made seam-exact by an
apron", because a reflection ray from a pixel near the slice edge can travel
arbitrarily far across the screen and no finite apron bounds it. Option 1
constrains that to single-producer surfaces, which is a real answer, but it is a
policy with nothing to gate until the pass exists, and the pass would be a
second reflection path whose output differs from the first everywhere it
applies.

So the honest position is that part C's guarantee holds now, by refusal, and the
work that would improve it is option 3 - ray tracing, which is slice-independent
by construction for the same reason the shadow cascades are. That needs
acceleration structures and ray queries in the RHI, which the plan already
places after tracks 1-3, and building them to enable SSR in the meantime would
be the wrong order.

## What was built — Part D

The second attempt at applying atmosphere and bloom to the composite, and this
one is in. With it went the three defects the first pass left recorded, one it
had not found, and the diagnostics that would have found all of them sooner.

### Applied to the composite, once

`CompositorSceneExecutor` owns a `VolumetricFog` and a `Bloom` of its own, sized
with everything else that carries the apron. Every layer is rendered with
`RenderFrameRequest::defer_scene_effects` set and skips both passes; after the
last layer has landed, atmosphere is applied to the accumulator - sky and every
layer alike - and bloom over that, both at the padded extent, before the one
crop. The order and the reasons are the renderer's own: atmosphere is part of
the image the chain then spills, in linear light, before anything reads a
neighbour.

The nearest surface across layers is the piece the first attempt said it could
skip. It could not be skipped for long, and it turned out to be small:
`depth_merge.comp.hlsl` folds each layer's product depth into one storage
texture as the layer lands - a `max`, because depth is reversed and nothing
drawn is zero - and the atmosphere resolves against that. Reading only the
first layer's depth would have fogged an isolated foreground object as though
it were the wall behind it.

Why the first attempt failed is now plain, and it was the thing its own notes
pointed at: the effect targets have to follow the accumulator to the padded
extent *and* carry the accumulator's format, because the crop refuses a source
whose format differs from the output's. The executor's fog and bloom take the
working format for the frame they publish (the volume and the coarse levels
stay `RGBA16F`), so nothing between the last pass and the crop converts.

`atmosphere_is_applied_once_however_many_layers` is the test the defect
deserved: the same room as one layer and as two, the box lifted into a layer of
its own by a blur, and the sky rows - which no layer owns - required to agree.
Before this they were fogged twice. `bloom_splits_across_nodes_byte_exactly_through_the_compositor`
holds the executor path to the same standard the renderer's two-node test holds
the renderer: a bright emitter straddling the seam, two halves against the
whole, byte for byte.

### One derivation, for both

`fog_frame_for` and `bloom_plan_for` decide which matrix and which raster a
frame is keyed to. Both the renderer's own path and the executor call them, so
the two cannot drift - which matters because the executor is now the only path
a screen is reached by, while the renderer-level tests still drive
`render_frame` directly.

That is also where the defect the first pass had not found was. A frame rendered
from an **explicit view** - an ICVFX plate, a projection-mapped surface - was
having its bloom chain planned on the *canvas*, so the plan described targets the
plate never had, `Bloom::record` refused the mismatch, and every plate and every
mapped output that authored bloom **failed the frame**. The wall went black the
moment bloom was turned up. A view-driven frame is now planned on its own raster,
which is exact when one node renders all of it - the usual case for a plate - and
carries the same seam exposure a blur already has on a surface raster
partitioned across nodes, recorded in the status document.
`bloom_reaches_a_view_driven_frame` executes a plate-sized executor through an
explicit view with bloom authored and requires it to run and to change the
picture.

### The fog matrix, tested

The fix the adversarial review found - fog on a view-driven frame must use the
view's matrix, not the authored camera's - had no regression test because two
attempts could not discriminate it. It has two now.
`fog_and_bloom_are_keyed_to_the_frustum_they_were_rendered_from` states the
property directly on `fog_frame_for`: a slice takes the canvas-wide matrix and
the canvas, a view takes its own matrix and its own raster whatever the authored
camera says. `fog_on_a_view_driven_frame_uses_the_views_own_matrix` is the GPU
half, and it discriminates because the authored camera is put somewhere
*different* from the view - straight down from twelve metres up, while the plate
looks along the floor. The reference is the plate's camera authored and rendered
through the slice path; the plate rendered through the view must match it to a
last-bit difference, and it does. Through the authored camera's matrix the
disagreement is a large fraction of the fog itself. The earlier attempts
compared a view against an identical authored camera, which is exactly the case
in which the bug is invisible.

### Height falloff

`fog_height` and `fog_height_falloff` on the Environment, beside the density.
Extinction is full at and below the base height and falls off exponentially
above it, per froxel, so the slab integration still sees a constant within each
cell; zero falloff is the uniform air every scene had, which is the default.
Full round trip - schema keys, parser with clamps, writer in the settings edit,
digest (conditional, so an existing scene digests as it did), two rows in
Project Settings under ATMOSPHERE. `fog_settles_below_its_height` requires the
rows above the horizon to carry far less atmosphere with a falloff than without,
and the floor still to carry some.

### Indoor HDRI fog distance

**Project Settings > Environment > Sky and Fog > Background distance** sets
the background's fog depth in metres. Zero retains the far-clip behavior and
its 85% haze cap. A positive value, such as 5-10 metres for an indoor panorama,
resolves the integrated volume at that view depth with full optical coverage.
Distances are bounded by the view's near/far range. Scene surfaces keep their
measured depth, and the HDRI's projection, reflections, and lighting stay the
same. This is a fog distance control, not a geometry sphere with parallax.

`environment_fog_background_distance` is authored, replicated, saved, clamped
to 0-100,000 world units, and included in the scene digest only when nonzero.
It does not change asset or bake identities, so editing it requires no asset
reload or rebake. The Sky and Fog page scrolls while Apply and Cancel remain
visible. Finite background sampling interpolates between integrated slice
endpoints, including clear air before the first slice.

GPU regressions cover a five-metre backdrop's expected transmission, dense
finite fog without the legacy opacity cap, near/far limits, unchanged geometry
fog and lighting, and agreement between a complete canvas and two slices.

### Saying why

A refused pass now names its reason. `RendererPassStatus` carries
`bloom_refusal` and `fog_refusal` as literals, the renderer and the executor
fill them at every refusal - "slice does not start on the canvas's 64-pixel
bloom grid", "no combined depth for the atmosphere on this device" - and the
Renderer card's NOT RUNNING line is followed by one line per refused pass with
its reason. A refusal on a headless node reaches its stderr once through
`core::report_idle_once`, which is what the plan's honest-idle invariant asks
for. The executor also counts what it did (`fog_volumes`, `bloom_chains`) and
what it refused, on `CompositorSceneStats`.

### And the trap

`HybridSceneRenderer::resize` now resizes the temporal and spatial passes with
everything else. It was unreachable only because the renderer refuses a
mismatched extent; it is closed rather than left for the next person.

## Still open

- **Reflections stop at the environment.** That satisfies this step's
  definition of done - correct or explicitly refused - but a car still does not
  reflect the ground it is parked on. The answer is ray tracing, and it needs
  acceleration structures and ray queries the RHI does not have.
- **No temporal reprojection of the volume, by decision.** The plan asks for
  it, riding the existing `HistoryDecision`. Reprojection reduces the variance
  of a *stochastically sampled* volume, and this volume is not sampled that
  way: every cell is integrated analytically from the same inputs every frame,
  so a history would average identical answers and buy nothing while adding a
  temporal path that can ghost under a moving camera - on a wall a camera is
  filming, permanently. The under-sampling the plan is worried about shows as
  banding in a thick fog against a bright source, and the remedy for that is
  slice count or the analytic slab integral the integration stage already uses,
  not history. Revisit only if the scatter stage ever jitters its sample
  positions.
- **A partitioned surface raster blooms per node.** A view-driven frame is
  planned on its own raster, which is exact for a plate or a surface one node
  owns whole. A planar surface split across nodes gets a chain per node on
  each node's region, with the same seam exposure a blur already has there:
  the mapped path has no apron. The fix is the same one the status document
  records for blur on mapped surfaces.
- **The apron is expensive and the cap is low.** Six levels reach 252 pixels on
  every side. On a 1920-wide slice that is a rendered region a quarter wider
  than the slice itself, and a deeper chain — which a 4K canvas would otherwise
  want, for a bloom radius proportional to the frame — costs 508 and then 1020.
  The cap at six is what keeps the default budget workable, and it means a
  large canvas gets a proportionally tighter bloom than a small one. The
  alternatives are all real work: exchanging coarse levels between nodes, or
  computing the coarse end of the chain from a shared low-resolution image of
  the whole canvas. Neither is built.
- **Bloom is Advanced only.** Basic renders display-referred, where the
  highlight energy bloom exists to spread has already been clipped away, so
  there is nothing to spread. Worth stating rather than leaving as an accident
  of where the code sits.
- **The chain runs over the whole padded extent**, apron included, and the
  apron is then cropped away. That is correct and it is not free: the coarse
  levels are cheap but level one is three quarters of a full-resolution pass on
  a padded target. Half-resolution from level one down is the obvious saving and
  has the same half-pixel-offset hazard the plan already notes for occlusion.
- **The air is shadowed by the first layer's atlas.** The executor lights the
  atmosphere from the first layer's light buffer and shadow atlas, which every
  layer uploads identically - but an isolated layer's casters are not in the
  base layer's atlas, so a lamp behind a blurred object lights the air through
  it. The same limit layer isolation already places on surface shadows.

## Open questions

- ~~**3D textures in the RHI**~~ Settled: not needed. The LUT went to a 2D
  atlas in [10](10-display-transform-and-calibration.md) because determinism
  forces manual interpolation, and the froxel volume is the same case - it is
  sampled at one depth per pixel and filtered by hand for the same reason. The
  RHI change this step was said to require is not required. Revisit only if
  something needs hardware 3D filtering, which nothing in tracks 1-3 does.
- **Is fog worth it before baked GI ([15](15-baked-global-illumination.md))?**
  Atmospheric scattering interacts with the lighting solution; doing it after the
  bake may avoid redoing the integration.
- **Lens flare and glare** are a related family and are arguably *more* important
  on a volume, since the real lens will add its own — meaning rendered flare is
  usually wrong and should be left to the physical lens. Probably do not build it.
