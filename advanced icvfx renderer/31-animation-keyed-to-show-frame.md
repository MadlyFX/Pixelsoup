# 31 — Animation keyed to the show frame

Implemented 2026-09-08. This supplies the node-animation, skinning, previous-pose
velocity and foliage-wind path proposed in [17](17-assessment-and-remaining-items.md).

## Operator workflow

Import a glTF/GLB through the existing model ingest workflow. The immutable
`.msmodel` now retains its node hierarchy, animation clips, skin weights and
inverse bind matrices. Existing version-2 packages remain readable; re-import
the original glTF to recover animation that an older package discarded.
An importable, self-contained [two-joint example](fixtures/show-frame-skin.gltf)
moves the upper edge of a card while its lower edge stays anchored.

Select the model and use **MODEL ANIMATION** in the Inspector:

- **Animation: On / Bind Pose** enables clip playback or the authored bind pose.
- **Clip (first = 0)** selects the source animation by its zero-based index.
  Clip zero plays by default. A missing selected clip refuses rendering and
  reports the reason, rather than choosing a different animation.
- **Playback: Loop / Clamp at End** chooses repeat or one-shot behavior.
- **Start at Current Show Frame** sets the model's start on the show timeline;
  **Start at Frame Zero** restores the default. The displayed start and the
  submitted edit retain integer precision, including beyond 2^53. Authored
  starts use the document's nonnegative signed 64-bit integer range.
- **Wind displacement** gives model-space XYZ displacement at full height.
  **Wind period** is an integer number of show frames. Zero displacement turns
  wind off. Wind is independent of the clip-enable switch, so an unskinned
  static foliage model can sway too. The minimum-Y edge stays anchored.

Mark animated or wind-driven models **Bake Geometry: Dynamic** before baking
lighting. The bake refuses them when marked Static, with a diagnostic naming
the model, so a rest-pose shadow cannot become permanent baked geometry.

All eight properties (`animation_enabled`, `animation_clip`, `animation_loop`,
`animation_start_frame`, `wind_x/y/z`, `wind_period_frames`) are document state,
carried by `ModelInstance`, included in scene identity and the scene bridge,
and replicated through the existing document channel.

## Clock and deformation contract

The evaluator consumes `Scene::show_frame` and the exact rational `show_rate`.
Looping phase is computed by bounded binary modular multiplication of the
64-bit ordinal. There is no conversion of absolute frame time to float or
double, no accumulated delta time and no wall clock. Nonlooping playback
saturates before conversion. Both methods retain frame identity through
`UINT64_MAX`; a seek evaluates the target directly.

glTF translation/rotation/scale channels support STEP, LINEAR and CUBICSPLINE.
Linear rotation uses shortest-arc quaternion slerp with the existing
deterministic trigonometry; cubic quaternion results are normalized. Cubic
tangents are scaled by the interval between source keys. Channels clamp to
their individual endpoint values. Hierarchy order and all interpolation
arithmetic are fixed. The render library now explicitly uses the same CPU
floating-point contract as scene evaluation.
Interpolation and inverse-bind conventions follow the
[Khronos glTF 2.0 specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html).

The importer retains the established baked vertex/tangent space and constructs
bindings that convert it back into joint bind space. Rigid animated nodes get
one binding; skinned nodes get their source joint palette. This preserves
translated mesh nodes, inverse binds and the existing material boundaries.
Each placement has its own current and previous palette in the pulled vertex
path, so instanced placements may select different starts or clips.

The shared vertex stage applies wind and skinning for color, depth, normals,
identity, shadows, reflected views and motion. Normals use the inverse
transpose of the blended affine basis; tangents follow the deformed basis,
including orientation changes. Fixed-height wind supplies its corresponding
normal/tangent shear. Conservative bounds include the current skin and wind
for visibility, shadow fitting and translucent sorting. Deforming placements
use geometry LOD zero.

Advancing a valid history evaluates the prior show frame. Hold evaluates the
same retained motion pair again. Initialize, reset and discontinuous seeks
use the current pose as the previous pose. Object velocity uses the current
camera at both endpoints, preserving item 21's physical-shutter contract.
Changing show cadence resets model history even when shutter blur is off.

The seam test exposed viewport-dependent hardware interpolation rounding when
subtracting the two endpoints of a deforming triangle. A dedicated motion
vertex variant supplies flat original triangle clip coordinates. The velocity
fragment solves homogeneous barycentrics at the shared canvas pixel in fixed
arithmetic. This makes deformation velocity byte-exact across the tested odd
slice boundary. Only motion replay carries the extra triangle evaluation and
flat payload; color and shadow passes use the ordinary vertex variant.

## Bounded resources and supported inputs

Per asset: 1,024 nodes, 4,096 pose bindings, 64 clips, at most three channels
per node per clip and one million keys total. Each vertex has four nonnegative
normalized `JOINTS_0`/`WEIGHTS_0` influences. Per renderer: 65,536 current/previous
binding pairs, an 8 MiB GPU palette buffer, a 32-byte skin stream per resident
vertex and construction-time CPU scratch. Multiple material parts reuse the
placement's palette. Asset parsing and validation remain on the model worker;
steady pose evaluation uses fixed scratch and does not allocate or re-upload
geometry. Pose and skin uploads contribute to existing upload-byte accounting,
and evaluation/replay costs are included in item 24's frame measurements.

The package writer emits version 3 with the existing content checksum and
content-addressed filename. The reader validates animation counts, references,
hierarchy order, finite values, key order, channel uniqueness and weight sums.
Animation payload memory is included in the model cache's residency budget.

Morph targets, extra influence sets and external animated LOD chains are
explicitly refused. Singular baked mesh-node transforms are also refused:
their original bind-space vertices cannot be recovered. Clip blending,
retargeting, animation events and simulation are outside this item.

## Verification

`mediaserver.model_animation` exercises source glTF skin/node import, translated
mesh nodes and inverse binds, package repeatability and version-2 compatibility,
corruption/refusal paths, interpolation equations, loop/clamp/start timing,
64-bit frame boundaries, scene/bridge identity and the static-bake guard.

`mediaserver.model_animation_gpu` checks 32 frames of color and object velocity
against independently rendered 63/65-pixel slices, repeated-frame color and
motion identity, seek and large-frame equivalence, independently timed
placements with batching enabled/disabled, wind and shadow deformation, invalid
clip refusal and no steady geometry upload. Motion is also checked against
independently projected per-frame displacement. The Inspector suite checks
that its animation controls emit the intended properties and preserve a start
frame above 2^53.

These are local GPU checks. Independent physical render nodes, actual wall
capture and the production asset's frame budget still require stage validation.
The compiled shader bundle changes with this implementation, so a deployed
cohort must run its existing conformance check again before Program promotion.

Verified on 2026-09-08:

- Complete `windows-msvc-release-gui` build, including the server, bake tool,
  tests and DXIL/SPIR-V shaders: passed with warnings treated as errors.
- Full Release CTest suite: **159/159 passed**, 201.40 seconds, after the final
  cadence/history change.
- Debug animation CPU/GPU, renderer integration and Inspector CTest suites:
  **4/4 passed**, 25.41 seconds. The latter two contain 34 renderer integration
  cases and 39 Inspector cases respectively.
- Advanced GPU checks ran on native D3D12. SDL's backend reports Advanced
  raster unavailable on this machine and is explicitly skipped for that test.
- `git diff --check`: passed.
