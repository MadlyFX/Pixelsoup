# 07 — Image-based lighting

**Track 3. The biggest single visual change for the least new infrastructure.**
Depends on [04](04-compute-in-the-rhi.md).

**Status: implemented.** An authored HDRI lights the scene diffusely and
specularly, metals reflect it, the sky draws it, and no map authored is
byte-identical to before. See **What was built** at the bottom, and its "Still
open" section for the two-node determinism gap.

## Why

Ambient today is a two-colour hemisphere: `sky_ambient` above, `ground_exposure`
below, blended by `normal.y`. The comment in `scene.frag.hlsl` is honest about
it — "enough that an unlit face reads as being in shadow rather than as a hole in
the frame, and cheap enough not to matter."

The consequence is that **every metal in the scene reflects nothing**. There is
no specular ambient term at all. A chrome bumper, a glass window, a wet road, a
polished floor — the things virtual production content is actually made of — have
no environment to reflect, so they read as flat grey. This is the single largest
gap between the current image and a photoreal one.

The authored slot is already there and already replicated:

```cpp
struct Environment final {
    Vec3 sky_color{0.05F, 0.07F, 0.10F};
    Vec3 ground_color{0.02F, 0.02F, 0.02F};
    float ambient{1.0F};
    // Optional equirectangular image identity. It is resolved by the explicit
    // asset-loading path, never by scene evaluation or the frame loop.
    std::string map_source;
    float map_intensity{1.0F};
    Turns map_rotation{};
};
```

`map_source`, `map_intensity` and `map_rotation` are read from the document,
carried on the evaluated scene, and mixed into `scene::digest`. **Nothing in
`engine/render` consumes any of them.** The schema, persistence, and replication
work is done; the renderer half is not started.

## Seams

- `engine/render/include/mediaserver/render/image_decode.hpp` — currently
  stb-backed RGBA8 only. Needs an HDR path: Radiance `.hdr` is trivial (stb can
  do it) and OpenEXR half-float is what stages actually ship. Returning a
  `DecodedFrame` with `Rgba16Float` fits the existing shape.
- **New** `engine/render/environment_cache.{hpp,cpp}` — residency for the
  environment map, following `ModelCache` exactly: process-wide worker, bounded
  budget, `try_lock` observation, sticky failure, generation-fenced refresh,
  transactional GPU adoption, publication epoch latched per command list.
- `engine/render/src/scene_renderer.cpp` — prefilter passes at adoption time,
  new bindings, new constants.
- `engine/shaders/scene.frag.hlsl` — replace the hemisphere with irradiance plus
  prefiltered specular; keep the hemisphere as the no-map fallback.
- **New shaders** — equirect→cube, irradiance convolution, specular prefilter,
  BRDF LUT, and a sky/background pass.
- `engine/rhi/include/mediaserver/rhi/device.hpp` — cubemap texture views.
  `TextureDesc` has `array_layers` and `mip_levels` already; a cube view over six
  layers is the addition, plus per-mip per-face storage views for the prefilter
  ([04](04-compute-in-the-rhi.md) already needs subresource storage views).

## The pipeline

Standard split-sum. Everything below runs **once per environment generation**, at
GPU adoption time, never per frame:

1. **Equirect → cubemap.** Compute, one thread per output texel, sampling the
   equirect with the authored `map_rotation` applied. Face size derived from the
   source height, clamped to a fixed maximum from `RendererLimits`.
2. **Irradiance.** Either a small (32²) convolved cubemap or 9 spherical-harmonic
   coefficients. SH is cheaper to evaluate and trivially deterministic; a
   cubemap is easier to debug. Recommend SH9 — the diffuse term is low-frequency
   and SH is exact enough that nobody will see the difference on a wall.
3. **Prefiltered specular.** Mip chain over the cubemap, each mip GGX-importance-
   sampled at a fixed roughness. The sample set must be a **fixed, deterministic
   sequence** — Hammersley with a fixed count, no time or frame input. This is
   the pass that gives metals something to reflect.
4. **BRDF integration LUT.** View-independent and environment-independent, so
   generate it once at renderer construction rather than per environment. A 256²
   RG16F texture.

Then in the lit shader:

```
diffuse_ibl  = irradiance(N) * albedo * (1 - metallic) * intensity
specular_ibl = prefiltered(R, roughness) * (F0 * brdf.x + brdf.y) * intensity
```

with `intensity` folded from `map_intensity` and the camera exposure multiplier
that Advanced already applies.

**The sky.** Draw the environment as the background instead of the flat sky
clear, respecting rotation and exposure. This matters for the outer frustum
specifically: on a volume, what the wall shows *is* the environment lighting the
subject, so a correct sky is not decoration.

## Fallback behaviour

No `map_source` authored is not an error and must not read as one. Keep the
existing hemisphere path, byte-identical to today. A map that failed to load is
different — that is sticky failure, reported in diagnostics and surfaced to the
operator, still rendering the hemisphere rather than black.

Follow the house rule: name what is, not what is not. An unauthored environment
gets no warning banner.

## Multi-node hazards

- Prefiltering must depend only on the map bytes, the authored intensity and
  rotation, and fixed constants. Any per-node input — adapter, resolution,
  timing — and two nodes light the scene differently.
- The prefilter must run at a **generation boundary**, not lazily on first use,
  or two nodes could be one generation apart mid-show. This is the same
  replicated-asset-generation problem as [15](15-baked-global-illumination.md)
  and should reuse whatever commit mechanism that step builds.
- Importance-sample sequences must be fixed. A "random rotation per frame to hide
  banding" is exactly the kind of thing that seams across nodes.

## Tests

- HDR decode: a known `.hdr` and a known `.exr` decode to expected half-float
  values; malformed input returns an invalid frame, not a partial one.
- Residency: no file access on the frame path (reuse the blocked-decode teardown
  pattern from the media tests); sticky failure; generation-fenced refresh
  retaining the last complete map.
- Prefilter determinism: the same source produces byte-identical mips across two
  runs and two renderer instances.
- A constant-colour environment produces irradiance equal to that colour times
  π-normalisation — an analytic check, not a golden image.
- GPU: a mirror-metal sphere (roughness 0, metallic 1) reflects a recognisable
  pattern from the map; the same sphere with no map is unchanged from today.
- Slice determinism: two half-canvas nodes byte-exact against the full-canvas
  render with an environment authored.

## Definition of done

An authored HDRI lights the scene diffusely and specularly, metals reflect it,
the sky draws it, no map authored is byte-identical to today, and two nodes
produce identical pixels.

## Open questions

- **OpenEXR is a real dependency.** vcpkg has it, but it is not small. Radiance
  `.hdr` via stb costs nothing and covers a lot of stage HDRIs. Ship `.hdr`
  first?
- **SH9 versus irradiance cubemap** — recommend SH9; confirm against a reference
  render before committing, since it is awkward to change later.
- **Does the outer frustum's rendered content feed back as lighting?** Physically
  it should: the wall lights the subject. In practice that is a second-order
  effect handled on set with light cards. Defer, but note it.
- **Per-scene versus per-project environment.** `Environment` is per-scene today,
  which means a transition between scenes changes the prefiltered set. Bounded
  residency for more than one environment generation is needed, or a transition
  will stall.

---

## What was built

**Status: the whole path works.** An authored map is read on a worker,
prefiltered at a generation boundary, and consumed by the lit shader and the
sky; a scene with no map renders bit for bit as it did before. What is not
covered is stated under "Still open".

### HDR decode

`decode_radiance_bytes` reads Radiance `.hdr` into RGBA16F, separate from
`decode_image_bytes` rather than a format argument on it: an environment map is
radiance, values above one are the entire point, and a decoder that returned it
as RGBA8 would clip exactly the highlights that make a reflection look like
anything. A low-dynamic-range image handed to it is **refused** rather than
promoted - stb will happily float-convert a PNG, and the result would look like
it worked while lighting the scene with nothing above one, which is far harder
to diagnose than a refusal.

The binary32-to-binary16 conversion is written out rather than taken from an
intrinsic, because two nodes convert the same map independently and F16C would
make the result depend on what each machine implements. Values past the
half-float range saturate to infinity rather than wrapping a bright texel into a
dark one.

OpenEXR is not here. It is what a facility with a colour pipeline actually
ships, and it is a real dependency; `.hdr` covers a lot of stage HDRIs and costs
nothing, so it went first, per the open question.

### Cube views in the RHI

`TextureDesc::cube` gives a six-layer array a cube SRV. It changes only how the
texture is read: a cube view lets the hardware filter across face edges, which
an array view cannot. That matters at high roughness, where the filter kernel
spans face boundaries - without seamless filtering the seams are visible as
lines on every reflective surface in the shot. Writing still addresses one face
at a time through an ordinary array storage view, which is what
[04](04-compute-in-the-rhi.md)'s per-subresource bindings already provide.

### The four passes

All compute, all at adoption time, none on the frame path. This is the first
production consumer of [04](04-compute-in-the-rhi.md).

1. **Equirect to cube**, per face and per level of the base chain. The authored
   rotation is applied here so every downstream pass sees one already-oriented
   environment and none of them can disagree about which way it faces. The mip
   chain is produced by re-projecting the source at each size rather than by
   downsampling the level above - exact where a box filter accumulates error,
   and free because this runs once per environment.
2. **Irradiance as SH9.** Nine coefficients into a 9x1 RGBA32F texture. Full
   float because nine numbers carry the entire diffuse environment and a
   half-float rounding here is a tint on every unlit surface in the shot.
3. **Specular prefilter**, one dispatch per face per mip, GGX importance
   sampling at a fixed roughness per level. This is the pass that gives metals
   something to reflect.
4. **BRDF table**, generated once at construction because it depends on nothing
   that changes. `prepare` is idempotent.

### Determinism, which was the hard part

Two nodes prefilter the same map independently and must produce the same bits,
or they light the same subject differently and the wall seams on whichever
surface is most reflective - usually the one the shot is about.

The reduction is where that goes wrong. Float addition is not associative, so an
irradiance projection accumulated with atomics gives a result that depends on
which thread group retired first. **The diffuse pass is therefore one thread
group**: 64 lanes each walk a fixed stride of the source in increasing order,
then a fixed-width tree reduction in groupshared memory. Same additions, same
order, same bits, every time and everywhere.

Every sample count is fixed and every sequence is a fixed Hammersley table. No
time, frame, adapter or resolution input reaches any of it.

### Tests

`engine/render/tests/environment_prefilter_tests.cpp`, plus three cases in the
image-decode tests:

- **a constant environment integrates to its own radiance** - the analytic check
  the plan asks for, not a golden image. For a uniform radiance over the sphere
  the zeroth SH coefficient is `L * Y0 * 4pi` and every higher band integrates
  to nothing. A golden image proves only that nothing changed; it cannot tell a
  correct convolution from a consistently wrong one. This is what catches a
  missing solid-angle weight, which over-counts the face corners by several
  percent and looks like a slightly bright environment rather than like a bug.
- **prefiltering is byte identical across instances** - two `EnvironmentPrefilter`
  instances, same source, same rotation, compared byte for byte, and the result
  checked to actually be the environment rather than an empty target.
- **an impossible prefilter configuration is refused** at construction, where a
  caller can still do something about it.
- **radiance bytes decode to exact half floats** - bits, not a tolerance,
  including saturation past the half range.
- **a low dynamic range image is not a radiance map.**

### The lit half

**`EnvironmentCache`** is residency for the authored map: a process-shared
worker reads and decodes, and `observe` never waits - a contended state returns
Busy for one frame rather than stalling. The contract is `ModelCache`'s, so a
renderer already knows the shape. What it deliberately omits is that cache's
budgeting: a byte cap, an eviction policy and a bounded source table are all
machinery guarding a set of size one.

Sticky failure keeps the previous complete map rather than going black - a scene
that had an environment and then got a bad path should keep lighting while an
operator reads the diagnostic. A decode that finishes after a refresh moved the
generation on is discarded and counted, because adopting it would replace the
map the operator just asked for with the one they replaced.

**Adoption** happens in `SceneRenderer::render`, at the top, once: observe,
decide, upload the equirect, record the whole prefilter chain into the same
command list. The maps a frame lights from are the ones its own recording
produced.

**The lit shader** gained three bindings and one constant. The diffuse term is
nine coefficients evaluated as a dot product - with the cosine-lobe convolution
factors, without which it returns radiance rather than irradiance and every
diffuse surface is off by a band-dependent factor that reads as a slightly wrong
colour rather than as an error. The specular term is one cubemap sample at a
level that inverts the prefilter's own roughness-per-mip mapping, which is
linear precisely so the inversion is exact.

The coefficient texture is fetched with `Load` rather than sampled: nine texels
carry the whole diffuse environment and a filter tap that strayed half a texel
would blend two unrelated basis coefficients.

**The sky** draws the environment as the background. It owns the clear when it
runs - clears colour and depth as any first pass does, fills every pixel, and
the lit pass then loads instead of clearing. A scene with no map skips it
entirely and the lit pass clears exactly as it always did, which is what keeps
the no-environment output unchanged.

`scene::invert` is new: a general 4x4 inverse by cofactor expansion in a fixed
operation order. `invert_affine` cannot do a projection - its bottom row is not
(0,0,0,1), which is the assumption that makes the affine version cheaper. A
pivoting solver would be more accurate on a badly conditioned matrix and would
let the pivot choice depend on values that differ in their last bit between two
machines, which is the one thing this must not do.

### What the work found

Three places bound the lit pipeline's fragment textures and only one of them was
obvious. The per-batch array and its reservation were easy to find; the
**composite layer draw** builds its own binding array against a separate
constant, and it silently used the old count until the draw refused. That is the
same duplicated-shape hazard recorded elsewhere in this plan - a count expressed
in three places, two of which were updated.

The reservation is worth naming on its own: a short `reserve` is not a wrong
image, it is a vector growth on the frame path, which is exactly what that arena
exists to prevent.

### Tests

- **an authored environment lights metal and absence changes nothing** - the
  end-to-end case. A mirror-metal instance rendered without a map, again without
  a map (asserted byte-identical, because "looks the same" is the claim that
  hides a one-code-value shift across a wall), then with a Radiance fixture
  written to a real file so the path goes through the cache, the worker and the
  disk. Asserts the frame changed, the metal got *brighter* - a darker result
  would mean the specular term was subtracting - and the background changed,
  which is the sky pass drawing.
- **a general inverse round trips a projection** - through an off-centre slice,
  because a centred frustum is the case a wrong inverse still passes. A singular
  matrix returns identity rather than infinities that would turn a sky ray into
  a NaN and every pixel it touches into black.

### Authoring, and what building it exposed

The map had no authoring surface at all. `scene_model.cpp` read
`environment_map`, `environment_map_intensity` and `environment_map_rotation`
from the stage node and they replicated and digested correctly, but nothing
anywhere wrote them - so the renderer half could be finished and still be
unreachable. Project Settings gained an **ENVIRONMENT** category, which is the
right home: the environment belongs to no selectable element, so there is
nothing in the viewport to click on to reach it.

Two defects surfaced immediately, both of the same kind - a feature that could
not be operated:

- **Apply ignored the environment.** The button's `changed` predicate compared
  only the effect settings, the renderer policy and the lens, so an operator who
  edited nothing but the map could not commit it.
- **No text field in the application could be pasted into.**
  `UiContext::text_field` explicitly passed `Key::Paste` through as a global
  shortcut. That is right for Undo and Redo - a field should not own document
  history - and wrong for paste, where a focused field should own the
  clipboard. The window host now reads the clipboard on the chord and pushes it
  through `add_text`, the same path a keystroke takes, so every field in the
  application became pasteable and the widget layer still knows nothing about a
  platform. An absolute path that can only be typed by hand is not an authoring
  surface.

  Copy and cut are still unhandled. The field has a caret and no selection
  model, so "copy" would have to mean the whole value, and inventing that
  semantic is worse than a stated gap.

**Every way the environment can fail was silent.** A path that does not resolve,
a file that is not Radiance, and a project still on the Basic backend all
produce exactly the same picture - the hemisphere - and nothing anywhere said
which. `EnvironmentStatus` now names all five states under the Map field, read
straight from the residency cache: default handles share one process-wide state,
so the dialog sees the same decode the renderer did without any plumbing back
out of the render path.

**The authored rotation never reached the renderer, and nothing re-recorded
when it changed.** Two independent defects sitting on top of each other, both
found by operating the dial rather than by reading the code:

- `adopt_environment` passed a literal `0.0F` where the authored rotation
  belonged. The shader took a rotation, `ibl_equirect_uv` applied it and
  `EnvironmentPrefilter::record` forwarded it - every layer below the call site
  was correct, and the one value that had to travel never left the scene.
- The adoption test compared the cache generation and the map source and
  stopped there. Rotation is baked into the cube by the equirect pass, so that
  it is deliberately not a per-frame constant the way intensity is - which
  means a change to it *has* to re-record the chain, and the test that decides
  whether to re-record did not look at it.

Either one alone is enough to make the dial inert, which is why fixing one and
looking would have shown nothing. The test renders two frames from **one**
renderer at different rotations: a fresh renderer per frame would pick the new
rotation up through ordinary first-time adoption and prove only the first half.
It also turns back to where it started and asserts the frame returns byte for
byte, because a renderer that re-recorded only on an increasing value would
pass a one-way check.

This is the same duplicated-shape hazard this plan keeps recording, in its
other form: not one value expressed in several places, but one value that had
to be carried through several and was dropped at the last hop.

### The compositor's background

`SceneRenderer` drew the sky correctly and almost nobody could see it. It only
draws one when it owns the background, and every isolated compositor layer is
transparent by contract - filling one makes it opaque and the composite shows
only whichever layer happened to be last. So on the surface an operator
actually watches, the environment lit the objects and never appeared behind
them. That is not a cosmetic gap: it is why the two rotation defects above
survived being looked at, because the one thing that would have made them
obvious was the part that was missing.

`CompositorSceneExecutor` already claimed the job in a comment - "the
compositor owns the visible environment behind them" - and then cleared to a
flat colour. It now asks a layer renderer to record the environment into the
accumulator before any layer composites over it.

**Through a layer renderer, not beside one.** The background has to agree with
the geometry drawn on top of it to the pixel, and the compositor renders a
padded slice: a second derivation of the viewpoint that agreed on the day it
was written would disagree by the apron the first time either changed.
`resolve_render_view` is now the single derivation both use, and the compositor
hands the renderer the same options the layers get, so the background is not
computed from the same inputs - it is computed by the same code.

Two guards. The sky is withheld when the accumulator is meant to carry
transparency, because it is opaque by construction and would quietly turn a
transparent output opaque. And `record_background` reports whether it drew, so
a scene with no environment keeps exactly the clear it always had rather than
having a fallback painted under it.

The environment it draws is the one a layer renderer adopted on an **earlier**
frame, since none of them has rendered yet when the background is recorded. A
decode that lands this frame therefore lights the objects one frame before it
appears behind them - the same lag the whole environment path already has
against authoring, and invisible against an async decode that takes many
frames.

**Tested** by rendering a compositor scene with and without a map: the
background pixel changes, the accumulator stays opaque, and a scene with no
environment is asserted byte for byte identical across two runs. The pixel
assertion is deliberately checked before the counter, because a counter can be
incremented by code that draws nothing.

### Still open

- ~~**Two-node byte-exactness with an environment authored** is not tested.~~
  Partly closed. `two_nodes_and_one_node_agree_byte_for_byte` in
  `engine/render/tests/renderer_integration_tests.cpp` renders a canvas whole
  and as two abutting slices over eight frames and compares every byte, and the
  environment path is live in it: the hemisphere ambient, the sky and the
  prefiltered lookups all agree exactly. What that test does *not* yet cover is
  a resident authored **map**, because environment adoption is asynchronous and
  the two renderers would have to be fenced to the same generation first - which
  is the very skew described in the next bullet.
- **Cross-node adoption skew is not fenced.** Each node admits and decodes on
  its own worker, so two nodes may adopt the same environment on different
  frames and light differently in that window. The plan names this as the
  replicated-asset-generation problem shared with
  [15](15-baked-global-illumination.md), and the commit mechanism it should
  reuse does not exist yet. The existing asset-adoption gate that holds Program
  for models is the natural home for it.
- **The sky is a hand-recorded pass**, not one declared on the render graph. It
  is the obvious first candidate for that migration, which needs the executor
  plumbed into `SceneRenderer` - a larger change than the pass itself, and not
  worth blocking a visible result on.
- **OpenEXR**, still. `.hdr` covers a lot of stage HDRIs; a facility with a
  colour pipeline ships `.exr`.
- **Per-scene environments and transitions.** One environment is resident at a
  time, so a switcher transition between scenes with different maps will
  prefilter on the frame it lands rather than ahead of it.
