# 10 — Display transform and calibration

**Track 3.** Corresponds to plan step 13's tail.

## Why

Everything else in this folder makes the render *correct*. This step is what
makes it *match on camera*, which is the only place correctness is judged.

The chain on a volume is: renderer → output transform → LED processor → panel →
lens → sensor → camera IDT → editorial. If any link is guessed at, the plate
comes back with a colour cast that the DP notices on the day and that costs the
production an hour. A photoreal render through a wrong display transform is not
photoreal; it is a well-lit wrong colour.

## Where it stands

The colour architecture underneath is already right, which is the hard part:

- Advanced scene colour is premultiplied scene-linear `RGBA16F`.
- `SceneMixer`, compositor effects, `ProjectionMeshWarp` and feed mapping all
  stay linear. Values above 1.0 survive the compositor.
- Tone mapping and output transfer happen only at the output boundary.

What sits at that boundary is a placeholder: Narkowicz's ACES *fit* — a curve
approximation, explicitly documented in `scene.frag.hlsl` as "a display transform
standing in for the managed pipeline, not colour management". `OutputTransform`
already reserves the seam for what replaces it:

> The pass is also the seam where LED colour output lands next — the
> per-panel/per-wall calibration LUT and dither before 10/12-bit quantisation,
> applied last on the node that owns the output.

**Update 2026-09-08:** [step 30](30-facility-colour-pipeline.md) implements the
reference OCIO transform, deeper output and per-wall camera compensation. It
extends the original scope below; individual panel uniformity remains downstream.

## Scope, given the LED-wall decision

Per the deployment decision recorded for this project: panels are LED walls, not
projectors, and **per-panel calibration and 3D-LUT correction are downstream on
the LED processor and deliberately deprioritised**. Known-gamma output is
sufficient today.

So this step is *not* "build a panel calibration system". It is three things:

1. **A real output transform** replacing the Narkowicz fit — an ACES-style
   RRT+ODT, or a configured OCIO display transform, baked to a 3D LUT.
2. **One authored show LUT** — the creative grade / show look, applied after the
   display transform, so an operator can match the wall to a reference monitor
   without touching the renderer.
3. **Dither before quantisation** — because 10-bit output of a smooth linear
   gradient bands visibly on a large wall, and banding on a wall is one of the
   easiest ways to make an expensive volume look cheap.

Per-panel correction stays out, and this document should be the reason nobody
re-adds it speculatively.

## Seams

- `engine/render/include/mediaserver/render/output_transform.hpp` — a LUT texture
  bound in the existing pass; the API already has `scene_linear_input` and the
  composite path, so this is an additional binding rather than a new stage.
- `engine/shaders/output_linear.frag.hlsl` — 3D LUT sample plus dither, replacing
  the inline tone map for the linear path.
- `engine/shaders/scene.frag.hlsl` — the inline `tonemap()` moves out of the
  scene shader entirely for Advanced. It is currently applied there, which is
  wrong for a scene-linear pipeline whose output boundary is elsewhere; check
  whether the linear variant already skips it and make it explicit either way.
- `engine/rhi/include/mediaserver/rhi/device.hpp` — 3D textures. `TextureDesc` is
  2D with array layers today; a 3D LUT needs a depth dimension. (Volumetric fog
  in [11](11-bloom-fog-and-reflections.md) needs the same addition, so do it
  once.) A 2D-atlas LUT is the alternative that avoids the RHI change, at the
  cost of manual trilinear filtering in the shader.
- `scene::RendererSettings` or a new `ColorSettings` — authored LUT source path,
  display transform selection, dither enable and bit depth. Replicated, clamped,
  digested, exposed in Project Settings.

## Design

**Bake, do not evaluate.** An OCIO config is a graph of transforms; evaluating it
per pixel is not frame-path work and pulling in the OCIO library on a render node
is a dependency nobody wants. Bake the config to a 3D LUT (33³ or 65³) at project
load, on a worker, following the residency contract — and then the frame path
samples one texture.

**The LUT is an asset with a generation.** Two nodes must apply the same LUT on
the same show frame, which is the replicated-asset-commit problem again. Reuse
whatever [15](15-baked-global-illumination.md) builds; do not invent a second
mechanism.

**Dither must be deterministic and position-based.** Blue noise or a fixed
ordered pattern indexed by **absolute canvas coordinates**, so two nodes produce
the same dither for the same wall pixel and the seam is invisible. A per-node
random dither is the sort of thing that looks fine on one machine and produces a
visible line down the middle of a wall.

Whether dither should vary per frame is a real question: temporally varying
dither hides banding better but a camera integrating over a shutter may or may
not average it the way a human eye does. Recommend static per-pixel dither to
start, and treat animated dither as an authored option that has to be tested on
camera before it ships.

**Bit depth.** Quantise to the panel's actual bit depth, authored, before the
swapchain format's own quantisation. Dithering to 8 bits when the wall takes 10
throws away a bit of precision for nothing.

## Multi-day, on-set reality

The thing that actually gets this right is not code: it is a documented procedure
for measuring the wall with a probe, generating the LUT, and verifying it through
the camera. Worth writing that procedure alongside the implementation, because
the code is only the delivery mechanism.

## Multi-node hazards

- Any per-node dither seed.
- Nodes on different LUT generations mid-show. Hold Program until the cohort
  agrees.
- The LUT being applied twice — once in the scene shader's leftover tone map and
  once at the output. Removing the inline tone map from the Advanced path is part
  of this step, not a follow-up.

## Tests

- A known linear input through a known LUT produces the expected output, to
  within the LUT's interpolation error.
- Identity LUT → byte-identical to the untransformed path.
- Dither is a pure function of absolute canvas coordinate: two nodes produce the
  same value for the same wall pixel; extend the existing absolute-canvas-identity
  test.
- A smooth linear ramp quantised to 10 bits with dither has no flat runs longer
  than a threshold (a banding check that does not need a golden image).
- LUT baking is deterministic: the same config produces byte-identical LUT data.
- A node with a stale LUT generation holds rather than presenting a different
  grade.

## What was built

In two passes. The first baked the transform and sampled it; the second, below
under "What was built — second pass", authored it, dithered it, moved the bake
off the frame path and wrote the procedure. What is still missing is under
"Still open" rather than implied.

### The table

`bake_display_lut` builds a cube of display values off the frame path, and the
output stage samples it. That is the architectural point of this step: the
transform stopped being five constants in a pixel shader and became data, so
replacing it with a measured one is supplying a different table rather than
editing and rebuilding a shader.

Two entries. `Srgb` is the reference - checkable by hand, and what to fall back
to when an authored transform fails to load. `AcesApproximate` is a well-known
fit in AP1 with the sRGB encode; it is named *approximate* deliberately,
because the ACES version and ODT a production expects is a facility decision
and the whole point of baking is that answering it later costs a table.

An authored `.cube` look composes *into* the same table, applied after the
display transform - which is where a grade belongs, on the display-referred
values a colourist actually saw. One lookup rather than two: two is two chances
for two nodes to disagree, and the frame path should do one.

### The shaper, and where its resolution goes

A table indexed by raw scene-linear would clip every highlight. The shaper is
`x / (x + k)`: monotonic over the whole positive range and exactly invertible,
so the table covers everything a scene can produce without a separate clamp.

`k` is the linear value that lands halfway along the table, and picking it is a
real decision rather than a constant. At four - the first value tried - four
fifths of the grid described highlights above diffuse white and the entire
range a shot is graded in shared the remaining fifth; the sRGB entry could not
even reproduce white to within six thousandths. At one, half the grid describes
everything below diffuse white, which is where the eye and the camera both are.
The failing test is what surfaced it.

### A 2D atlas, not a 3D texture

The plan left this open. It is an atlas, and the reason is determinism: the
filtering has to be done by hand anyway, because a hardware sampler's precision
is not guaranteed identical between implementations and this value feeds a wall
that two machines each render half of. With the interpolation manual, a 3D
texture buys nothing here and costs an RHI change - so the change is deferred
to whatever actually needs hardware 3D filtering.

`sample_display_lut` is the reference implementation of that interpolation,
shared so a test can assert the shader's arithmetic without a GPU and so the
two cannot drift.

### The inline tone map

Gone from the Advanced path, and it was worse than the plan suspected: the
scene shader computed the tone-mapped `display` value on the scene-linear path
and then **discarded it**. A tone map per pixel for nothing, and a standing
invitation to apply the transform twice. It is now inside the display-referred
branch where it belongs.

### Tests

- **The shaper covers the range and inverts** - monotonic across two hundred
  steps, round-trips over the range a show uses, clamps negatives rather than
  reflecting them, and its top grid point is finite: an infinity there would
  poison every interpolation touching it.
- **Baking is deterministic** - byte for byte, because two nodes bake
  independently and a table differing in its last bit grades two halves of a
  wall differently. An identity look composes transparently.
- **A known value lands where the transform says** - black, mid grey and white
  through the sRGB entry, checked against hand-worked sRGB; and the tone curve
  is asserted *darker* at white, which is what a shoulder is.
- **A `.cube` file parses and a broken one is refused** - red varying fastest,
  because reading it the other way round swaps a look's red and blue and looks
  deliberate. A 1D table is refused rather than half-read.
- **The atlas holds every entry where the shader looks** - an interior texel,
  not a corner, because a corner survives most indexing mistakes.
- **Scene-linear output goes through the baked table** - on the GPU, against
  the same manual trilinear the shader performs. Every other test in that file
  drives the display-referred path, so without this one the table could have
  been wired to nothing and the suite would still have passed.

## What was built — second pass

### Authored, replicated, digested

`scene::ColorSettings` rides `RendererSettings` as `color`: the display
transform (`Srgb` or `AcesApproximate`), the show look's `.cube` path, and the
dither switch. It goes through the whole document path - struct, clamp, schema
keys (`display_transform`, `show_lut`, `output_dither`), parser, writer - and
`every_renderer_setting_survives_the_document` now authors every colour field
away from its default, so a dropped one cannot hide behind a value it would
have had anyway.

The enumeration lives in the scene, not the renderer; `render::DisplayTransform`
is an alias of it. A second enum would have needed a mapping, and a mapping is
a place two nodes can disagree.

Colour joins both digests **conditionally**, the way the inner frustum does: a
project that never opened the tab reports exactly the profile digest it always
did, so a Director upgraded mid-show does not watch every node's capability
evidence go stale at once. Once anything about colour is authored, every field
is in - `colour_joins_the_profile_digest_only_when_authored` holds both halves.

The show look **belongs to the project**, said in the struct's comment rather
than defaulted into. Per-panel correction is downstream on the processor for
this deployment, so a per-wall look would be machinery guarding a case the
project does not have.

### The bake is off the frame path

`DisplayLutCache` is the residency worker, on `EnvironmentCache`'s contract:
Unseen / Pending / Ready / Failed / Busy, `try_lock` observation, a generation
that moves on refresh, one resident table because a show has one look. The
worker reads the `.cube`, parses it, composes it into the transform and
publishes the table; the frame path observes and adopts on a frame boundary.
`OutputTransform::adopt_color` is that adoption, called once per frame before
anything is recorded, so a table can never change between two viewports of
one window. The compiled default is still baked inline at construction - an
output works from its first frame with nothing authored, and the worker's
bake of the same request is bit-identical to it.

A look file that cannot be read publishes the **transform alone**, under
`Failed`. The alternative - keeping whatever look was resident before - shows a
wall that depends on history; the transform alone is a picture an operator can
predict while they read the reason. `the_display_table_cache_bakes_off_the_frame_path`
covers Pending-then-Ready, the composed bake byte for byte, the failed path's
fallback, a bake with no file at all, and refresh moving the generation on.

### Dither, keyed to the wall

An 8×8 ordered (Bayer) matrix, indexed by the **absolute canvas pixel**. The
output pass now knows where its viewport sits on the canvas: every `render` and
`composite` takes the canvas region the viewport shows, and the presenter passes
the slice for a canvas output and `map.source` for a mapped one - which for a
planar surface is the region of the *surface raster* this node owns, so two
nodes sharing one surface still key to the same pixel. A scaled preview has no
neighbour to seam against and simply gets a deterministic pattern.

Static rather than animated, per the plan's recommendation: a camera integrating
over a shutter does not necessarily average a moving pattern the way an eye
does, and something that has to be tested on camera should be an authored
option rather than the default. Quantisation is to the **target's own** depth -
eight bits, the swapchain this build presents - because dithering to a depth the
output does not carry throws away precision for nothing; a deeper output needs a
deeper format in the RHI first, and that is recorded as the boundary rather
than dressed up as a control.

The matrix is mirrored in `display_transform.cpp` and `output.frag.hlsl`;
`two_nodes_dither_one_wall_identically` holds the copies together by comparing
GPU pixels against the C++ reference, and - the claim that matters - renders a
canvas whole and as two halves at their canvas origins and requires **byte
equality**. `a_ramp_quantised_with_dither_has_no_long_flat_runs` is the banding
check that needs no golden image: sixteen pixels per code value flat-lines under
rounding and breaks up under the pattern.

### Authored from the COLOUR tab, and reported there

Project Settings has a COLOUR category between IMAGE and ENVIRONMENT: the
transform as two buttons, the look path as a field, the dither as a switch. A
status line under the path says what the *applied* colour is doing - baking,
live with or without a look, a look that could not be read, or ignored because
the project is running Basic - because every one of those shows the same
picture from the operator's chair. The editor fills it from the Program
display's own `color_state`, and the renderer card's NOT RUNNING line now
carries a refused pass's *reason* as well as its name.

### The procedure

[10 — Measuring a wall](10-wall-colour-procedure.md): the chart scene, choosing
the transform first and verifying the processor with a probe, shooting the
chart, building the look on display-referred values, authoring it, checking
every node, verifying through the camera, and when to re-check. The code is
the delivery mechanism; that document is what gets the colour right.

## Definition of done

Scene-linear output goes through one baked display transform plus one authored
show LUT, dithers deterministically to the panel's bit depth, and two nodes
produce identical bytes; the inline tone map is gone from the Advanced path; and
there is a written procedure for producing the LUT for a given wall and camera.

**Met**, with one reading made explicit: "the panel's bit depth" is the depth
this build presents, eight bits, and a ten-bit wall is served at eight until the
RHI presents deeper. That is stated in the UI and in the procedure rather than
implied by a control that would do nothing.

## Still open

- **The look has no cohort commit.** A node whose disk lacks the `.cube` shows
  the transform alone and says so on its own stderr and in its own Project
  Settings; nothing yet holds Program until every node holds the same look
  generation. The plan is right that this is the replicated-asset commit
  mechanism shared with [15](15-baked-global-illumination.md), which does not
  exist yet. Until it does, the procedure makes checking every node the
  operator's step, and says so.
- **Eight bits.** The dither quantises to the target format's depth, and the
  target is `RGBA8` because the swapchain is. A ten-bit path needs a ten-bit
  texture format and swapchain in the RHI, at which point the dither follows
  the format with no further change here.
- **One table size.** Thirty-three points a side, the size a colourist's
  `.cube` almost always is. A 65-point look is accepted and composed into the
  33-point table; a table of a different size is refused by `set_display_lut`
  rather than reallocating the atlas mid-show.

## Open questions

- **ACES version and ODT.** ACES 1.3 versus 2.0 output transforms look
  meaningfully different. Whichever is chosen has to match what post expects, so
  this is a production decision, not an engineering one.
- ~~**3D texture in the RHI, or 2D atlas LUT?**~~ Settled: atlas, because
  determinism forces manual interpolation and that removes the 3D texture's
  only advantage here. Revisit when something needs hardware 3D filtering.
- **Does the show LUT belong to the project or to the output?** Per-project is
  simpler; per-wall is what a multi-wall stage with mismatched panels actually
  needs. Given per-panel correction is deprioritised, per-project is probably
  right for now — but say so explicitly rather than defaulting into it.
