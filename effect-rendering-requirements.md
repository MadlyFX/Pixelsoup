# Effect Rendering Requirements

This document is the implementation and review contract for every new
first-class scene effect. It applies to values represented by
`scene::EffectKind` and folded into `scene::EffectStack`. Compositor structure
operators such as `RadialArray` are not scene effects, but must follow the same
depth, identity, determinism, and regression-testing principles in their own
planner path.

## Non-negotiable rendering invariants

1. **Ordinary geometry renders once with shared depth.** Enabled effects must
   never split normal, effect-free geometry into camera-dependent flat passes.
2. **Identity is indistinguishable from no effect.** A disabled effect, its
   documented identity value, and invalid values normalized to identity must
   not allocate a pass or alter pixels, depth, ordering, statistics, or bounds.
3. **Document identity is not z-order.** Node IDs may break exact depth ties for
   determinism, but must never decide which physically separated object is in
   front.
4. **Depth behavior is explicit.** Every active effect declares whether it
   shares scene depth, tests against it, produces replacement depth, or has no
   depth relationship.
5. **Coverage behavior is explicit.** An effect that changes alpha or expands
   its source bounds is not a crisp occluder unless its processor produces a
   matching hard-depth result.
6. **Unsupported combinations fail visibly.** The planner or executor must
   return `InvalidConfiguration`; it must not silently omit an effect, flatten
   it into a different order, or render it through an unrelated processor.
7. **All render nodes make the same decision.** Registry traits, identity
   normalization, pass construction, and processor execution must be
   deterministic from replicated project state.

## Compile-time registry

The registry is declared in
`engine/render/include/mediaserver/render/effect_execution.hpp`. Each entry is
indexed by `scene::EffectKind` and supplies:

- a stable display/debug name;
- an execution domain;
- a depth policy;
- an implemented processor route;
- the identity value and identity predicate;
- whether the effect changes alpha;
- whether it expands source bounds;
- whether it reads the already-composited destination;
- a resolver from the folded `EffectStack`;
- a reset function used when an isolated layer is normalized for rendering.

`EffectKind::Count` is the enforcement sentinel. New kinds must be inserted
before it. The registry array then grows at compile time, and its `static_assert`
fails until a complete, correctly indexed entry is supplied.

### Current contracts

| Effect | Domain | Depth | Processor | Identity |
| --- | --- | --- | --- | --- |
| Brightness | Inline | Shared scene depth | Material brightness | Disabled or `1` |
| Saturation | Inline | Shared scene depth | Material saturation | Disabled, non-finite, outside `0..4`, or `1` |
| Blur | Isolated layer | Tests against scene depth | Gaussian blur | Disabled, non-finite, or `<= 0` |
| Levels | Inline | Shared scene depth | Material levels | Every Master/R/G/B channel uses input/output `0/1` and gamma `1` |
| Lift/Gamma/Gain | Inline | Shared scene depth | Material lift/gamma/gain | RGB lift `0`, gamma `1`, and gain `1` |
| Contrast/Pivot | Inline | Shared scene depth | Material contrast/pivot | Contrast `1`; pivot remains authored but cannot affect pixels at identity |
| Colour Matrix / Channel Mixer | Inline | Shared scene depth | Material colour matrix | Row-major identity 3x3 and zero RGB offset |
| White Balance | Inline | Shared scene depth | Material white balance | D65 `6500 K`, tint `0`, producing a bit-exact identity matrix |
| Curves | Inline | Shared scene depth | Material curves LUT | Master identity endpoints and no active non-identity RGB channel |
| Film Grain / Noise | Inline | Shared scene depth | Material film grain | Amount `0` |
| Box Blur | Isolated layer | Tests against scene depth | Spatial box blur | Radius `0`; malformed radius/pass tuples fail closed |
| Directional Blur | Isolated layer | Tests against scene depth | Spatial directional blur | Length `0`; non-finite/out-of-range tuples fail closed |

Brightness factors multiply in binary64 across the first-class scene stack and
downstream compositor modifiers. The product is clamped to `0..16` and the
project response gamma is applied once, after the complete inline stack.

Saturation measures Rec.709 luminance and interpolates between that achromatic
value and the source colour. It does not change alpha, bounds, or destination
pixels. Its response domain follows the project Brightness Curve: linear light
for Exposure, encoded/display-referred values for Gain. Brightness is a scalar
and Saturation is a fixed-luminance matrix, so the two commute without an
intermediate clamp and may be folded in either authored order within that one
domain. Multiple Saturation nodes multiply in binary64 across the first-class
scene stack and downstream compositor modifiers, then clamp once to `0..4`;
`0` is monochrome and `1` is identity.

Levels, Lift/Gamma/Gain, Contrast/Pivot, Colour Matrix, White Balance, and
Curves are
structured, generally non-commutative operations. They remain in one bounded
authored-order inline program shared by lit geometry and media. Levels rejects
equal or reversed input endpoints; reversed output endpoints are a valid
inversion. Lift/Gamma/Gain evaluates
`pow(max(rgb + lift, 0), 1/gamma) * gain` component-wise. Contrast/Pivot
evaluates `(rgb - pivot) * contrast + pivot` without an intermediate clamp.
Colour Matrix evaluates `M * rgb + offset` from three binary32 rows and also
does not insert a clamp. White Balance derives a binary32 3x3 adaptation matrix
at model evaluation from the documented Planckian/CIE-1960/Bradford constants;
the GPU only applies that resolved matrix. Curves accepts a master curve and
optional R/G/B curves with exact `(0,0)` and `(1,1)` endpoints, strictly
increasing finite `x`, and at most 32 points per channel. Model evaluation uses
the documented monotone-cubic algorithm to derive exactly 1024 binary32 RGBA
LUT entries; render execution samples that derived buffer and never persists it
as a second authored curve. The project response domain places
that complete program before the tone map in Exposure mode and after it in Gain
mode.

Film Grain adds zero-mean monochrome or RGB noise from a uint32-only hash of
the replicated seed, exact 64-bit show frame, authored canvas-pixel cell, and
channel. `pixel_scale` groups canvas pixels into stable cells and
`chroma_ratio` blends the RGB samples away from the shared monochrome sample.
It never uses wall-clock time, slice-local coordinates, or driver randomness,
and it leaves alpha, depth, and bounds unchanged. Amount `0` is an inert
identity. Its Texture-panel edit writes the complete typed tuple atomically;
Amount alone also has the `film_grain_amount` keyframe route.

Box Blur is a separable horizontal/vertical gather with an integer canvas-pixel
radius `0..256` and `1..8` complete pass pairs. Its symmetric allocation extent
is exactly `radius * passes` on both axes. Directional Blur is a centred gather
line of length `0..512` canvas pixels, with angle measured in turns from canvas
`+X`; the project supplies one odd `3..65` tap count. Its allocation extent is
the absolute projected half-length on each axis. Both processors average the
premultiplied RGBA layer, so coverage expands with colour while tested-depth
composition remains explicit. They execute in authored spatial order and
bypass every pass/statistic when at identity.

The current fixed execution order is Geometry, Inline, Isolated Layer, then
Full Scene. Active inline effects therefore have to precede Blur. The scene
planner and compositor graph validate the combined scene-plus-graph sequence;
a reverse-domain order produces a visible diagnostic or
`InvalidConfiguration`. Identity nodes do not establish or violate an order.

### Execution domains

- **Inline:** computed in the drawable's normal material/media shader. It must
  remain in the shared-depth base pass.
- **Geometry:** changes instances or vertices before rasterization. Generated
  geometry must participate in normal depth testing and picking as applicable.
- **Isolated layer:** renders a source to an offscreen color target before a
  spatial or destination-dependent operation. Ordinary geometry still remains
  together in the base pass.
- **Full scene:** executes only after the complete 3D scene has resolved
  visibility. It must not be represented as a per-object layer.

## Required implementation steps

1. Add the new kind before `EffectKind::Count` and add its resolved parameters
   to `EffectStack` or an appropriate deterministic parameter carrier.
2. Update document schema/model evaluation, serialization, editor creation,
   Inspector controls, timeline/keyframing, and scene digesting.
3. Add one complete registry entry. Implement its resolver, identity predicate,
   and reset function. Non-finite and out-of-range inputs need a documented,
   deterministic result.
4. Implement or select a truthful `EffectProcessor`. A new processor requires
   an actual shader/render path and processor contract tests before an effect
   may reference it.
5. Route planning through registry traits. Do not add independent checks such
   as `if (new_effect_enabled)` to decide isolation, depth eligibility, or
   identity.
6. Preserve authored effect order. Folding is allowed only when the operations
   are mathematically associative and the folded result is deterministic.
7. Define bounds behavior. Spatial kernels, shadows, glows, displacement, and
   similar effects must expand allocation/culling bounds enough to retain every
   generated pixel.
8. Define depth and alpha behavior. If flat back-to-front compositing cannot
   represent an effect on intersecting geometry, implement color-plus-depth
   compositing rather than using average object depth as an approximation.
9. Add every test category below before exposing the effect in the editor.

## Mandatory tests for each effect

### Registry and identity tests

- The registry contains exactly one entry at the effect's enum index.
- Disabled and enabled-at-identity applications are identity.
- Identity produces the same render plan as no effect.
- Identity produces pixel-equivalent GPU output to no effect.
- Invalid/non-finite parameters normalize deterministically and do not create
  accidental passes.
- Resetting the effect's execution domain disables only effects in that domain.

### Depth and camera tests

- Effected object in front of ordinary geometry.
- Effected object behind ordinary geometry.
- The same two cases with document node IDs reversed.
- Camera viewed from both sides and orbited across the depth order.
- Object animated or dragged through the depth order.
- Multiple ordinary objects remain in one shared-depth base pass throughout.
- Coplanar placement has a deterministic, documented tie result.

### Coverage and composition tests

- Opaque, translucent, and alpha-bearing sources.
- Edges expanded by the effect are neither clipped nor promoted into hard
  occluders incorrectly.
- Interaction with every supported blend mode.
- Interaction with another instance of the same effect and with each other
  execution domain.
- Authored stack order changes output when the operations are non-commutative.
- Physical/mapped output views use the same visibility rules as Program and the
  free Stage camera.

### Editor and project tests

- Add, remove, enable, disable, and reorder survive save/load.
- Inspector edits update both the base value and a key at the playhead when the
  property is keyed.
- Timeline sampling updates the displayed property value.
- Undo/redo restores the full effect node and its ordering.

## Review checklist

Before merging a new effect, reviewers must be able to answer yes to all of the
following:

- Does it have a compile-time registry entry and a real processor?
- Is its identity value fully inert?
- Does ordinary geometry still render in one base pass?
- Are depth, alpha, bounds, and destination-read behavior truthful?
- Are all unsupported configurations rejected explicitly?
- Do reversed node IDs and moving cameras preserve physical visibility?
- Is the result deterministic across render nodes and output mappings?
- Are model, editor, keyframe, save/load, planner, and GPU tests present?

## Verification commands

Run the focused contracts first:

```powershell
cmake --build out/build/windows-msvc-debug-gui --config Debug --target `
  mediaserver_render_effect_execution_tests `
  mediaserver_render_compositor_plan_tests `
  mediaserver_render_compositor_scene_tests

ctest --test-dir out/build/windows-msvc-debug-gui --output-on-failure `
  -R "render_(effect_execution|compositor_plan|compositor_scene)"
```

Then run the complete Debug suite and rebuild the Release `media-server` target
used by `scripts/run-editor.ps1`.
