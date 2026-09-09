# 32 — Area lights

Rectangle and sphere emitters now light surfaces in Basic and Advanced. The
published GGX linearly transformed cosine fit supplies finite specular highlights
and a clipped cosine integral supplies diffuse light. A softbox produces a
rectangular reflection whose size and softness follow the emitter and receiver.

## Authoring and units

Select a local light in Properties > Light. **Kind** offers **POINT**, **SPOT**,
**RECT**, and **SPHERE**. Rectangles expose **Width m**, **Height m**, **Roll turns**,
**Sidedness ONE/TWO**, and **Aim**; spheres expose **Radius m**. All local lights
retain position, colour, temperature, intensity, range and Bake indirect.

Aim is the direction a rectangle emits. Its width axis is world +X projected
onto the emission plane, with world +Z as fallback when Aim is nearly parallel
to X. Roll rotates that width axis toward `cross(Aim, width axis)`. This basis
depends only on authored light state. Sphere emission is outward and independent
of Aim or sidedness; receivers inside the source see no inward emission.

Dimensions clamp to 0.001–10000 metres; roll clamps to −1…1 turns. Shape, radius,
dimensions, roll and sidedness survive document commands, project save/load and
the wire checkpoint. They enter scene, visual and bake-input digests. Changing
kind is one undoable edit that clears incompatible IES and shadow requests and
resets an incompatible unit to Unitless.

| Units | Meaning for an area emitter |
| --- | --- |
| Unitless | Scene-linear emitted radiance |
| Nits | cd/m², multiplied by the project's photometric scale |
| Lumens | Total flux over all emitting faces; converted to radiance using `flux / (pi * emitting area)` |

Rectangle emitting area is width × height, doubled for two sides. Sphere
emitting area is `4*pi*radius²`. Thus two-sided lumen emission splits the same
flux between the two faces; two-sided nits retain the radiance of each face.
Candela and lux are not area-light units. Temperature uses the existing shared
step-16 conversion. The authored range applies the existing squared fade by
distance from the centre; integration already supplies geometric distance
falloff, so it receives no second inverse-square term.

## Editor light guides

The Stage **VIEW** viewport's **GUIDES** toggle includes light shapes. Sphere
lights have three solid great circles at their actual source radius. Rectangles
have a full width/height outline in the renderer's aim/roll basis and arrows on
each emitting face. Unselected spotlights have a short cone glyph; selecting a
spot expands its inner and outer cones to the authored range. Directional lights
show parallel arrows indicating light travel.

Selected local lights show a dim dashed range cutoff: three circles for point
and area lights, and a curved cap for spots. Range is the renderer's distance
from the light centre and is never capped to fit the view. Solid area outlines
describe the emitter size; the dashed boundary describes attenuation, not a
physical surface or a shadow prediction. A caption reports the primary light's
dimensions or half-angles and its range in metres. The guides update with
property edits and selection and never enter Program output.

To inspect them, enable **GUIDES**, select a light, and change Radius m, Width m,
Height m, Roll turns, Aim, Sidedness, cone angles or Range. Selection gives cyan
guides; unselected sources remain yellow. Disable **GUIDES** to hide the shapes
and captions. Fixed line tessellation and homogeneous clipping keep the guides
bounded as the camera crosses a light or a large range boundary.

## Fit asset and shading

The two 64 × 64 tables come from the authors' published
[LTC repository](https://github.com/selfshadow/ltc_code/tree/31e5e96b54f98f33098f8503003119ba2231a1c6),
pinned to that commit. See [the data provenance](../third_party/ltc/README.md) and
[redistribution license](../third_party/ltc/LICENSE). The fit is an approximation
to height-correlated isotropic GGX; it is not a sampled point-light proxy.

The 128 KiB binary32 asset is compiled into the renderer. GPU storage is
allocated at construction. Adoption is keyed by the authored asset-generation
key and a command-recording token; canceled uploads are retried. A generation
change re-adopts the complete asset, with no partial matrix/amplitude update.
Because the immutable CPU asset exists before recording, it needs no worker,
filesystem lookup, timer, or publication race. There is no disk read, hash,
allocation or blocking lock on this path. Four explicit buffer reads and two
stages of interpolation implement each lookup without another texture sampler.
The renderer-profile digest includes the pinned LTC contract version.

Rectangle integrals use their four vertices and explicitly clip to the cosine
horizon. Spheres use the visible tangent circle represented by a fixed 32-sided
polygon. That is a bounded approximation and can reveal a faceted source outline
on very smooth receivers. Counts never change with distance, node, slice or
frame. Clearcoat uses a separate GGX integral with its own normal and roughness.
Anisotropic GGX and Charlie sheen keep their material BRDFs through fixed 8 × 8
emitter quadrature; narrow lobes can expose this coarser sampling.

Lighting uses the existing reconstructed full-canvas world position. There is
no screen-space neighbourhood and no new apron requirement. Geometry and fog
share the extended 128-byte light record. Fog integrates its phase function
over 64 fixed emitter samples. The offline baker uses the same surface samples
and radiance conversion, tracing visibility to each sample; area-light indirect
transport is now supported. Rectangle IES profiles remain unsupported.

## Shadows and diagnostics

Live area-light shadow maps remain deferred. The Inspector labels this limit;
it does not expose the spot-shadow controls for an area emitter. A document
that still requests area shadows reports the reason and fails asset readiness
so Program cannot silently accept unshadowed emission. Preview continues to
show the emitter. Renderer > Geometry reports area-light count, LTC readiness,
generation, and unsupported shadow requests. The baker does trace visibility.

The emitter is a light source, not visible scene geometry. Add ordinary emissive
geometry if the shot needs to see a physical luminaire directly.

## Operator walkthrough

1. Open a scene with a floor or wall and a local light. For HDR preview select
   **Advanced** in File > Project Settings > Renderer. Use a rough metallic
   receiver to see the source's reflected shape clearly.
2. Select the local light and choose **RECT** in Properties > Light > Kind.
   Set Width m to `3`, Height m to `0.8`, and Aim toward the receiver. Set
   **NITS** and adjust intensity for the project's light scale/exposure.
3. Change Roll turns from `0` to `0.25`: the long axis of the highlight rotates
   90 degrees. Increase receiver roughness: the highlight becomes softer.
4. Aim the rectangle away from the receiver. **ONE** emits nothing toward it;
   **TWO** restores emission. With LM, switching from one face to two halves
   each face's radiance; with NITS, it preserves face radiance.
5. Choose **SPHERE** and change Radius m from `0.4` to `0.8`. At fixed NITS the
   source grows and emits more flux; at fixed LM total flux is preserved.
6. Save, reopen, and use Undo on an edit to verify the controls persist. Enable
   fog to inspect emission into the air. Use Bake/Rebake for indirect light.
7. Output Monitor > **Check render cohort** exercises rectangle and sphere
   sources in fixture version 5, including geometry and fog digest lanes.

## Verification

The area-light suite compares diffuse shading with independent 256 × 256
surface quadrature and compares the GGX fit against independent BRDF integration.
Readbacks check highlight orientation, roughness, sidedness, source radius,
repeated bytes, exact adjacent-slice bytes, fog response, table-generation reuse
and canceled-upload retry. Scene tests use nondefault values through saved wire
checkpoints. Inspector tests exercise shape selection, numeric fields, nits and
sidedness. Bake tests check visible indirect emission, reversed faces, sphere
size and disabled indirect transport. The independent diffuse reference permits
1.2% error (including half-float output); the fitted GGX reference permits 15%
at the three tested view positions. These are bounded fixture checks, not an
error guarantee over every angle and roughness.

Both complete GUI builds succeeded. Debug passed **160/160 CTest entries** in
369.13 seconds; Release passed **160/160** in 238.76 seconds. Both configurations
compiled the updated shaders. The five focused suites also passed before the
full runs. `git diff --check` reported no whitespace errors.

Native editor review used a copy of the shadow-stability project. Spot-to-RECT
conversion, width `3`, NITS, roll `0.25`, TWO sidedness, SPHERE conversion,
radius `0.8`, Undo back to `0.5`, and File > Save Project were exercised with
visible lighting changes. The saved copy is `out/step32-review.mstage`, and
`out/step32-ui-review.png` records the editor view. The original project was
preserved. Automated project/wire tests cover reopening nondefault fields.

Local GPU equality verifies the local adapter/driver. Deployed multi-machine
qualification still uses cohort conformance on the actual stage hardware.

The viewport-guide follow-up rebuilt both complete Debug and Release GUI
configurations. All 16 editor CTest entries passed in each (8.46 s Debug,
5.86 s Release), including 41 viewport cases. New independent geometric checks
cover source radius versus a 19 m cutoff, rectangle aim/roll/face direction,
30/60-degree spot half-angles, wide cones, parallel sun rays, near/side-plane
clipping, selection, the GUIDES toggle and exclusion from Program.

Native review changed a sphere from 0.05 to 0.8 m, inspected a 3 x 1 m rectangle
at 0.25 turns, switched TWO to ONE and observed the reverse arrow disappear,
changed a spot's range from 12 to 4 m and toggled GUIDES off/on. A separate
`out/light-guides-review.mstage` preserves the rectangle/spot review state.
Screenshots are `out/light-guides-sphere.jpg`, `out/light-guides-rectangle.jpg`
and `out/light-guides-spot.jpg`. The previous review project remains intact.
