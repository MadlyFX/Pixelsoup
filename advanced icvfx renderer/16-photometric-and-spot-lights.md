# 16 — Photometric units and spot lights

**Implemented 2026-09-07.** The design below is retained as the original
proposal. Spots and local shadows had already landed in step 09, using
`PointLight` with `LocalLightKind::Spot` and the existing bounded local-light
array. This step completes their photometry and authoring controls; it does
not add a second spot container. See the implementation notes below and
[renderer status](../advanced-renderer-status.md#step-16-photometric-units-and-spot-lights-2026-09-07).

**Cheap, mostly authored already, and unblocks
[09](09-ambient-occlusion-and-local-light-shadows.md)'s local shadows.**
Corresponds to plan step 12's lighting half.

Two small pieces of work with an unusually good ratio of visible result to
effort. Neither needs compute, the render graph, or the apron.

---

## Part A — Photometric units and colour temperature

### Why

`scene::DirectionalLight` and `scene::PointLight` already carry:

```cpp
LightUnit unit{LightUnit::Unitless};
float color_temperature_kelvin{6'500.0F};
bool use_color_temperature{};
```

These are authored, replicated, persisted, and mixed into the `SceneBridge`
payload digest (`scene_bridge.cpp`, in both the directional and point light
paths). **They are never converted when packing `GpuLight`.** The status document
says so plainly:

> Authored light-unit and color-temperature carriers exist, but `LightUnit` is
> not yet converted while packing GPU lights, and the current material/light
> scale is not an absolute luminance calibration.

So an operator can author a 3200 K tungsten practical and a 5600 K daylight
window and the renderer will render both as the same neutral white. On a volume
that is not a subtlety — mixed colour temperature is how interiors read as
interiors, and the camera's white balance makes the difference obvious.

The same applies to intensity: `intensity` is a bare multiplier, so a lighting
artist cannot author "this practical is a 60 W bulb" and get a plausible result
relative to "this window is 10,000 lux". Advanced already applies a physically
meaningful camera exposure (`shutter × ISO / aperture²`), so the light side is
the half that is missing for the exposure to mean anything.

### Design

**Colour temperature → RGB.** Planckian locus to CIE xy, xy to linear sRGB /
Rec.709 primaries, normalised so that 6500 K is exactly neutral white (1,1,1) —
which keeps every existing authored scene byte-identical when
`use_color_temperature` is false, and makes the default temperature a no-op when
it is true. Apply at pack time in `scene_renderer.cpp`, multiplying the authored
`color`.

**Units → radiometric scale.** Convert each `LightUnit` to the renderer's
internal scale with the standard relationships:

- Point light in lumens → candela: `lm / (4π)`.
- Spot in lumens → candela over the cone solid angle.
- Directional in lux → irradiance directly.
- `Unitless` keeps today's behaviour exactly, so nothing that exists changes.

The absolute anchor — what internal value equals one candela — is a single
project-level constant, and it must be replicated and digested like everything
else. Choose it so that a default-exposed camera on a typical interior lands near
mid-grey, and document the choice, because every authored scene depends on it.

**Do the conversion once, at pack time, from replicated values.** Not in the
shader, not per pixel, and not cached across frames keyed on anything local.

---

## Part B — Spot lights

### Why

There is no spot light type. `GpuLight` distinguishes directional from point by
`vector_kind.w`, and that is all there is. A dressed interior is lit almost
entirely by spots and by lamps with shades — both of which are cones, and both of
which currently have to be faked as omnidirectional points that light through
their own housings.

Spots are also the cheap path to local-light shadows: one 2D projection per
light instead of a cube. [09](09-ambient-occlusion-and-local-light-shadows.md)
recommends doing spot shadows first for exactly this reason, which makes this
step its prerequisite.

### Design

- `scene::SpotLight` — position, direction, colour, intensity, range, inner and
  outer cone angles in turns (matching the project's `Turns` convention),
  plus the same `unit` / `color_temperature_kelvin` / `use_color_temperature`
  carriers as the other two.
- `scene::max_scene_spot_lights`, sized like the existing point-light cap.
- A light-kind field in `GpuLight` rather than more overloading of
  `vector_kind.w` — three kinds is where the encoding stops being readable.
- Shader: cone attenuation as `smoothstep` between the cosines of the outer and
  inner angles, multiplied into the existing windowed inverse-square falloff so
  that a spot still reaches exactly zero at its stated range. The existing point
  light already gets this right and the comment explains why: "A light that never
  quite ends cannot be culled and cannot be reasoned about by whoever placed it."
- Schema, model read with clamping, write, Inspector rows, digest — the ordinary
  authored-state path.

**Area lights are deliberately not in scope.** A correct rect area light needs
linearly-transformed cosines and a different specular path; it is a real feature
with real cost. Note it, defer it, and do not approximate it with a cluster of
points, which looks wrong and costs more.

---

## Multi-node hazards

Very few — this is authored state converted deterministically. The two to watch:

- A new field missing from `scene::digest` or the `SceneBridge` payload digest.
  The existing lights already mix `color_temperature_kelvin` and
  `use_color_temperature`; make sure the spot's cone angles and every unit field
  join them, or two nodes light differently while agreeing on the digest.
- The absolute luminance anchor being a build constant that could differ. Make it
  replicated project state.

## Tests

- 6500 K with `use_color_temperature` true is exactly neutral: byte-identical to
  the same scene with it false. This is the regression guard for every existing
  project.
- 3200 K reads warm and 8000 K reads cool, asserted on channel ratios rather than
  a golden image.
- Unit conversion: a known lumen value produces the expected candela; `Unitless`
  is byte-identical to today.
- Spot cone: a receiver inside the inner angle is fully lit, outside the outer
  angle is unlit, and the transition is monotonic between them.
- A spot reaches exactly zero at its authored range.
- Round-trip and digest coverage for every new field, one test per field.
- Slice determinism with spots active.

## Definition of done

Mixed colour temperature reads correctly, photometric intensities are meaningful
against the existing camera exposure, spot lights exist and cone correctly, every
existing scene renders byte-identically, and [09](09-ambient-occlusion-and-local-light-shadows.md)
has a light type worth shadowing.

## Open questions

- **What is the absolute anchor?** Picking it commits every future authored
  scene. Worth deriving from a reference setup — a known lux level, a known
  camera exposure, a known mid-grey result — rather than choosing a round number.
- **Should `Unitless` stay?** Keeping it means two authoring modes forever;
  removing it breaks existing projects, which the project explicitly permits.
  Probably keep it as the default and let photometric be opt-in per light.
- **IES profiles.** Real fixtures ship measured intensity distributions, and they
  are what makes a practical look like that specific practical. A natural
  follow-on once spots exist, and an asset with a generation like everything else.

## Implementation notes

`scene::light_emission` converts replicated light state once for GPU packing
and once per light for an offline bake. Geometry, reflective shadow maps, fog,
and baked diffuse probes consume the same colour and intensity. The GPU light
layout has an explicit integer kind, shared by geometry and fog shaders.

- Directional lights accept Unitless or Lux. Point and spot lights accept
  Unitless, Lumens, or Candela. Unsupported combinations, including nits and
  photometric rectangles, reject the render request instead of silently
  treating values as another unit. The old Unitless rectangle carrier keeps
  its existing point approximation; area emission and IES remain deferred.
- Point lumens become candela through `lm / (4*pi)`. For spots the divisor is
  `2*pi*(1 - (cos(inner) + cos(outer))/2)`, the integral of the inner cap and
  smoothstep penumbra over solid angle. Angles are cone half-angles in turns.
  The outer angle is clamped to [0.001, 0.249], and the inner to [0, outer].
  Equal angles produce a hard cone. Windowed inverse-square falloff still
  reaches zero at range. Unitless spots retain the old squared penumbra for
  existing pixel compatibility.
- `RendererSettings::photometric_scale` defaults to `pi/1000` and is clamped
  to [0.000001, 1]. A Lambertian 18% card at normal incidence under 1000 lux
  yields scene-linear 0.18 at f/2.8, 1/48 s, ISO 100, 0 EV before display
  transform. The full material model's Fresnel term yields approximately
  0.175 in the pixel regression. Camera changes retain the existing relative
  `shutter * ISO / aperture^2` response. This reference does not calibrate a
  physical wall's nits.
- Temperature uses a checked-in Planckian xy table derived from the CIE 1931
  2-degree observer at 1 nm, sampled every 100 K from 1000 to 40000 K. Runtime
  interpolation, linear Rec.709 conversion, 6500 K adaptation, gamut clipping,
  and unit-luminance normalization use deterministic arithmetic without I/O.
  Disabled temperature and enabled 6500 K preserve authored RGB exactly.
  The generator and data attribution are in `scripts/generate-light-temperature.py`
  and `third_party/cie/README.md`.
- Scale is saved, replicated, and included in scene, renderer-profile,
  photometric-light visual, and bake-input identities. Existing unit, colour
  temperature, aim, range, and cone fields continue through their authored
  paths. The profile capability tag changes even at default scale, so older
  renderers cannot claim a matching rendering contract.

## Operator walkthrough

1. In File > Project Settings > Renderer, select Advanced. In the Colour tab,
   Photometric Lighting > Light scale defaults to 0.003142 (pi/1000 internally).
2. Select a point or spot light and open Properties. Set Units to LM for
   lumens or CD for candela; directional lights offer Lux. Intensity is then
   interpreted in the selected unit.
3. Set Temperature to On and edit Kelvin, for example 3200 for a warm practical
   or 8000 for cool light. The colour swatch remains the authored multiplier.
4. For spots, edit Aim X/Y/Z, Inner cone and Outer cone in turns, and Range
   in metres. The existing shadow controls remain on the same light.
5. Save and reopen the project to retain units, temperature, cones and scale.
   Re-bake when photometric changes should be included in baked illumination.

## Verification

Both full Windows GUI builds pass. All 144 CTest entries pass in Debug and
Release. Added coverage includes exact 6500 K and disabled-temperature output,
warm/cool channel ratios, point-unit equivalence, numerical spot flux
integration, cone boundaries and range, known-lux exposure response, baked
transport, project save/load, per-field digests, and exact two-slice pixels
with a photometric spot. Inspector tests type units, Kelvin, aim and cone
values through the normal edit-intent path. The project scale uses a numeric
field with precise entry, covered by an additional workspace test and a native
edit/apply/save walkthrough. Both builds and the five affected editor/timeline
suites passed again after the numeric-field correction.
