# 05 — Padded slice render and crop

**Track 2. Hard prerequisite for every neighbourhood-dependent pass.**
Corresponds to plan step 20.

**Status: implemented.** A blurred wall split two and four ways is byte-exact
against the full-canvas render at every shared pixel. What landed, where it
departs from the plan below and why, and what is still open are at the bottom
under **What was built**.

## Why

A filter that reads neighbouring pixels — bloom, AO, TAA, SSR, any blur — cannot
produce the same answer at a node boundary as the full-canvas render would,
because the neighbours it needs are on the other node. The result is a visible
seam down the middle of the wall in exactly the content most likely to have one:
bright highlights, soft shadows, moving edges.

The mechanism to fix this already exists and is not wired up:
`render::padded_slice()` in `engine/render/src/slice_apron.cpp` computes and
validates a `PaddedSlice{render, crop, apron}`, `apron_requirements()` derives the
needed apron from the scene's effects and renderer settings, and
`HybridSceneRenderer::render_frame` calls both — then records
`request.slice` and publishes `padded` as metadata that nothing consumes.
`advanced-renderer-status.md` lists this under incomplete boundaries.

Until this lands, **no step in track 3 can be enabled on a multi-node wall.**

## What exists

- `SliceApronRequirements` with lanes for `spatial_filter`, `denoiser`,
  `reflection`, `taau`, `bloom`, and a `maximum()`.
- `padded_slice(canvas, slice, requirements, limit)` returning the expanded
  render rectangle, the crop rectangle of the authored slice inside it, and the
  apron actually used. It clamps against the canvas edge, so a node at the wall's
  edge gets a smaller apron on that side and the crop rectangle accounts for it.
- `scene::RendererSettings::slice_apron_limit`, replicated and digested, default
  256 canvas pixels, max `scene::max_slice_apron` (2048).
- A passing test, `slice apron expands clamps and preserves crop`, covering the
  metadata.

What is missing is that the renderer never renders the padded rectangle, and
nothing downstream crops.

## The allocation problem, and the recommendation

Render targets are allocated at the output extent at construction, and the
renderer allocates once and never again. A padded render needs more pixels than
the output extent.

Two routes:

**A — allocate padded targets.** Size the scene targets at
`output_extent + 2 * apron_in_output_pixels`, where the apron comes from
`slice_apron_limit` at construction. Simple, no RHI change, but it always pays
the memory and fill cost of the maximum apron even when the authored scene needs
none, and 256 canvas pixels on a small slice is a large proportional increase.

**B — allocate padded targets and add a viewport to the draw path.** Same
allocation, but `PulledDraw` gains a viewport rectangle so a frame that needs a
smaller apron rasterizes only the region it needs. The unused border keeps its
clear value.

**Recommend B.** The viewport addition is genuinely small — D3D12 already calls
`RSSetViewports`/`RSSetScissorRects` in `set_viewport()` and just needs the rect
passed through; SDL_GPU needs `SDL_SetGPUViewport` + `SDL_SetGPUScissor` after
`SDL_BeginGPURenderPass`. It also unlocks shadow atlases and any future
sub-rectangle rendering, and it means the cost of the apron scales with what the
scene actually authored rather than with the limit.

Note that clear semantics stay whole-target in both backends; document that,
because a viewport that does not clear its own region is a subtle trap.

## Seams

- `engine/rhi/include/mediaserver/rhi/device.hpp` — a viewport rectangle on
  `PulledDraw` (route B), plus validation that it fits the target.
- `engine/rhi/src/d3d12_backend.cpp`, `engine/rhi/src/sdl_gpu_device.cpp` —
  honour it; refuse a viewport larger than the target rather than clamping.
- `engine/render/src/hybrid_scene_renderer.cpp` — record `padded.render` instead
  of `request.slice`; publish the crop in **output pixels** on `RenderProducts`.
- `engine/render/include/mediaserver/render/scene_renderer.hpp` — targets sized
  for the apron; `width()`/`height()` become the padded extent, so every caller
  that assumed target extent equals output extent must be found and fixed.
- Downstream consumers that must learn about the crop:
  `CompositorSceneExecutor`, `SceneMixer`, `ProjectionMeshWarp`,
  `OutputTransform`, `mapped_scene`, and both presenters.

## The conversion that must be exact

Apron is expressed in canvas pixels; targets are in output pixels. The
conversion is `apron * output_width / slice.width`, and it must be computed the
same way on both nodes sharing an edge, from replicated integers only, with no
floating-point accumulation. Round the apron **up** in output pixels: an apron
one pixel short is a seam, an apron one pixel long is wasted fill.

## Design notes

- **Crop at the output boundary, not before.** The whole point is that every
  neighbourhood pass runs on the padded image and only the final consumer crops.
  A pass that crops early hands the next pass a boundary again.
- **Sampling in absolute canvas coordinates.** Any noise, dither, or sample
  rotation must be positioned by absolute canvas coordinate and keyed by the
  exact show frame, so the apron region of node A and the interior of node B
  produce identical values for the same canvas pixel. The tested helper for this
  already exists (`deterministic samples use absolute canvas identity`).
- **Full-scene inputs stay full-scene.** Shadow cascades and any future
  probe/TLAS input are already independent of the slice and must remain so; the
  apron applies to screen-space neighbourhoods only.
- **Apron budget.** `apron_requirements()` already sums per-feature lanes and
  `padded_slice` refuses beyond `slice_apron_limit`. Keep the refusal — an
  over-budget apron must be an explicit `InvalidConfiguration`, not a silently
  reduced filter, because a reduced filter on one node is a seam.

## Multi-node hazards

This step *is* the multi-node hazard mitigation, so the risks are in getting it
subtly wrong:

- Different apron on two nodes because one clamped at the canvas edge and the
  other did not — this is correct and intended; the crop rectangle already
  accounts for it. Make sure downstream uses `crop`, not a recomputed offset.
- Conversion rounding differing by a pixel between nodes.
- A pass that samples outside the padded region and clamps, silently, producing a
  different value than the full-canvas render would.

## Tests

The decisive test compares a full-canvas render against 2-node and 4-node padded
slices at their shared pixels, **with a neighbourhood pass active**. Use the
existing Gaussian blur as the first consumer since it already exists and
`apron_requirements()` already derives its radius.

- Full canvas versus two padded halves: byte-exact at every shared pixel with a
  blur authored.
- Same at four nodes, including the interior corner where two aprons meet.
- A node at the canvas edge: apron clamps, crop compensates, output correct.
- Apron beyond `slice_apron_limit` is refused rather than reduced.
- No blur authored → apron zero → byte-identical to today's output, proving the
  change costs nothing when unused.
- RHI: a viewport draw writes only inside the viewport and leaves the border at
  the clear value; a viewport larger than the target is refused.

## Definition of done

`HybridSceneRenderer` renders `padded.render`, publishes a crop in output pixels,
the output boundary crops, and a blurred four-node wall is byte-exact against the
full-canvas render at every shared pixel.

## Open questions

- **Does the apron apply to the inner frustum too?** The inner frustum
  ([01](01-camera-tracking-and-inner-outer-frustum.md)) may be produced by a
  single node, in which case it needs no apron — but if it spans nodes it needs
  one, and its edges are inside the canvas rather than at a slice boundary.
- **Memory.** Padded targets at the default 256-pixel limit are a real increase
  across Program, Preview, Stage, and every mapped view. Worth deriving the
  construction-time allocation from the authored requirement instead of the
  limit, at the cost of reallocating when the requirement changes.

---

## What was built

`HybridSceneRenderer` records `padded.render` and publishes the crop in output
pixels on `RenderProducts`. `CompositorSceneExecutor` sizes every target that
carries the apron - the layer renderers, the blur and spatial-blur processors,
the keyer, the mask raster, the matte, and both composite accumulators - runs
the whole chain padded, and crops once into an output texture at the presented
extent. `target()` and `execute()` return that cropped texture, so `SceneMixer`,
`ProjectionMeshWarp`, `OutputTransform`, `mapped_scene` and both presenters are
unchanged and never see a padded texture.

In the RHI, `PulledDraw` gained a `ViewportRect` and `CommandList` gained
`copy_region` - an exact sub-rectangle copy with no filter, which is what the
crop needs. Both are tested on their own.

### Route A, not route B

The plan recommends B: allocate for the largest apron the project permits and
use a viewport to rasterize only what is needed. **B is not correct without more
work than it looks.** Every neighbourhood pass in the compositor clamps its
sampling at its texture edge. If the target is larger than the padded region,
that edge is not the padded edge, and a node whose slice touches the canvas
boundary clamps somewhere the full-canvas reference does not - so its edge
pixels diverge, which is precisely the class of error this step exists to
remove. Making B work means teaching every spatial processor a valid rectangle
and clamping to it in each of their shaders.

So the allocation is exact: the internals are resized to the padded extent when
it changes, and the texture edge is always the padded edge. The consequence is
that an authored change to a blur radius reallocates, where B would not - an
operator dragging a radius slider reallocates on the frames where the integer
apron changes. That is the trade this step takes, and the valid-rect work is
what would reverse it.

The viewport was still built, because it is small, independently correct, and
it is what a future valid-rect design needs; it also unlocks shadow atlases and
any other sub-rectangle rendering. It has no production caller yet, and its
tests say so plainly rather than implying one.

### The conversion, and why there isn't one

The plan expects an apron measured in canvas pixels to be converted into output
pixels, rounded up, identically on both sides of an edge. There is no
conversion here, because **the apron applies only at a one-to-one pitch** -
output extent equal to slice extent. That is the case the whole path exists for:
a wall node rendering its own rectangle of the canvas at the canvas's own
resolution.

At any other scale the render is a resample and the two grids do not line up.
Rounding the apron up in output pixels moves the interior by a fraction of a
pixel, because the projection maps the whole padded rectangle onto the whole
target - so the authored slice no longer lands on the integer boundary the crop
assumes. A fractional interior offset is a worse error than the seam an apron
prevents, and it would be present on every frame rather than at one edge. A
scaled render is a preview or a mapped view: it has no neighbour to seam
against, so it takes no apron and costs nothing.

The whole class of rounding hazards the plan warns about is therefore absent
rather than handled. If a scaled node ever needs an apron, the divisibility
treatment goes in then, with a test.

### Two defects found on the way

**`apron_requirements()` could not see the effects the compositor applies.** It
walked `scene.effects`, the older show-wide list. The first-class blur an
operator actually drops onto a layer lives in that layer's `LayerStyle` -
`blur_enabled`/`blur_radius` and `spatial_operations` - on screens, meshes,
media and models. None of it was counted, so the authored blur that most needs
an apron produced an apron of zero. It now walks all four instance lists.

**The apron was gated on the Advanced backend as a whole.** Only the denoiser,
reflection and TAAU lanes are Advanced-only; an authored blur is applied by the
compositor on either backend, so a Basic wall seamed exactly as an Advanced one
would. The gate now drops the three Advanced lanes and leaves the
spatial-filter lane alone.

### The circularity, and where the decision lives

Two attempts at "one decision, computed the same way in both places" were wrong
before the third worked, and both failures are worth recording because they look
like reasonable designs:

- **Deriving the padding from the renderer's own extent is circular.** The
  renderer's targets are already padded, so the one-to-one pitch test never
  passes and the apron switches itself off exactly where it was asked for.
  `RenderFrameRequest` already carried `output_width`/`output_height`; they now
  mean *what this render represents*, which is not the extent of the targets it
  draws into, and `render_frame` derives the target extent it requires from
  them rather than demanding they be equal.
- **Deriving it from the scene the renderer was handed is wrong for an isolated
  layer.** The compositor gives each layer renderer a scene whose effects have
  been lifted out into the pass that will apply them, so a renderer looking at
  what it was given computes zero for a layer that is about to be blurred. The
  padding now travels in the request: whoever can see the whole frame decides,
  and `RenderFrameRequest::padded` says so.

A renderer whose targets do not match the padding it was given refuses, rather
than drawing the padded rectangle squashed into an unpadded target - which would
look almost right and be wrong everywhere.

### Tests

Four new cases in `engine/render/tests/compositor_scene_executor_tests.cpp`:

- **a blurred wall splits across nodes byte exactly** - a full-canvas render
  against two halves and then four quadrants, compared byte for byte at every
  shared pixel with a blur authored across the seam. Not a tolerance: a
  tolerance passes with a one-pixel apron error, and one pixel is a seam. The
  four-way split covers the interior corner where two aprons meet, which a
  horizontal-only apron passes and a correct one has to get right.
- **a slice at the canvas edge clamps its apron and stays exact** - a strip that
  clamps on two sides and the whole canvas as one slice, which clamps on four.
  This is what breaks if a consumer recomputes the offset from the requested
  apron instead of reading the crop.
- **an unblurred wall pays nothing for the padded path** - identical pixels and,
  asserted separately, an allocation still at exactly the presented extent.
- **an apron beyond the budget is refused rather than reduced** - and raising
  the budget is all that was needed, so the refusal was the budget and not the
  geometry. Silently reducing the filter would leave one node blurring less than
  its neighbour while every node reported success.

Three in `engine/rhi/tests/rhi_device_tests.cpp`:

- **a viewport draw covers its rectangle and nothing else** - and the clear is
  still whole-target, which is the trap worth asserting rather than describing.
- **a viewport larger than its target is refused** - four ways, including one
  that fits but is offset past the edge.
- **a region copy moves exact pixels and refuses the rest** - a mismatched
  format, an empty rectangle, a rectangle past the source edge, and a
  destination too small.

`solid.vert.hlsl` / `solid.frag.hlsl` are a fullscreen triangle in one known
colour, built by the same pinned compiler and rules as every other shader. They
are the minimal draw the RHI tests need: a viewport test that broke because a
production shader gained a constant would be testing the wrong thing.

### Still open

- **The apron covers `FeedProjection::CanvasSlice` maps only.** That is the
  default and the one this step is about: a node renders its own rectangle of a
  shared canvas, and the apron is expressed in canvas pixels.

  A `PlanarSurface` map takes the other branch in `record_output_mapped_scene`:
  the whole scene through one asymmetric per-surface frustum, at the node output
  extent. It gets no apron, and could not use this one - there is no canvas
  rectangle to expand, and the pitch is not one-to-one. Both shapes of that
  branch are affected, not only the meshed one: a planar surface uses the render
  directly, an arc or imported mesh sends it through `ProjectionMeshWarp` first.

  The exposure there is real and pre-dates this step.
  `mesh_perspective_view_for` fits the frustum to the exact projected bounds of
  the mesh with no margin, so the mesh's extreme vertices sit on the source's
  outermost texels; a blur near a surface edge clamps where the adjoining
  surface's node has real content, and two adjacent segments disagree along the
  join. Separately, the warp samples with a linear clamped sampler, so the
  outermost vertices take a half-texel of edge replication with or without any
  authored effect.

  The fix is this step one space over: a margin in output pixels on
  `mesh_perspective_view_for`, a source sized for it, and the crop applied
  before the warp samples. `apron_requirements()` needs nothing new - only the
  conversion changes.
- **The inner frustum** ([01](01-camera-tracking-and-inner-outer-frustum.md))
  takes no apron. Its edges are inside the canvas rather than at a slice
  boundary, and a plate produced by a single node needs none; a plate spanning
  nodes would.
- **Reallocation on an authored apron change**, as above.
