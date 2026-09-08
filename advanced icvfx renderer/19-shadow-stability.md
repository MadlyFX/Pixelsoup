# 19 — Shadow stability

Implemented 2026-09-07 on the native D3D12 Advanced raster path. This is item
19 in [the assessment](17-assessment-and-remaining-items.md), not item 19 in
the older renderer integration plan.

## What changed

Directional cascades blend across a world-position-derived band at each map
edge. The same receiver position indexes both maps and determines the blend;
neither the output slice nor its camera-relative depth selects the band.
The last map remains the complete scene fit. Near cascade sizes use the
replicated caster-set diameter, capped by the authored camera far plane,
rather than recomputing their reach from camera-to-caster distance. Their
light basis is anchored to the scene and their centers snap to that basis's
texel grid. Camera translation therefore preserves texel scale and depth
range. Changes to scene bounds, light direction or camera lens can still
change a fit.

Directional and spot maps use a fixed 5 by 5 tent-weighted kernel with manual
bilinear depth comparison. Analytic receiver-plane depth correction works for
orthographic and perspective light projections, preventing a wider kernel
from darkening a bare sloping receiver. Reads outside a map or atlas tile
are lit; they never borrow a neighboring light's edge texel.

The first directional light's authored `angular_radius_turns` now controls
penumbra width. Nine fixed blocker samples estimate receiver/blocker separation;
the tangent of the angular radius converts that separation to a radius in
shadow texels. The radius is bounded between the authored filter radius and
16 texels. The filter keeps 25 taps at every radius. This is a bounded PCSS
approximation; it does not add adaptive searches or moment-shadow textures.

Contact shadows march twelve fixed world-space steps toward the first sun and
each spot that has an atlas tile. They read the existing auxiliary depth
product and attenuate those lights' direct contribution, including clearcoat.
World length, depth thickness and a 32-pixel screen bound limit the ray.
Coplanar hits are rejected. Missing depth and rays outside the image are lit.
There is no new target allocation or temporal history for this pass.

The contact-shadow apron is its own lane. Its support combines with AO by
maximum, then adds to subsequent TAA, transmission and bloom support. Ordinary
canvas slices and padded off-axis/ICVFX views share that calculation; an
insufficient authored guard budget refuses the padded render. Basic drops the
contact lane. Runtime stats count eligible contact lights, and disabled,
zero-thickness and missing-light conditions are reported once per process.

glTF MASK casters use the material's base-colour alpha, base opacity, UV,
sampler, sampling bias, cutoff and sidedness. The same coverage runs in
directional shadows, the spot atlas, reflective-shadow-map normal/flux,
camera depth/normal, object ID and velocity. A shared full shader varying
signature preserves the D3D12 vertex/pixel linkage for these late material
lanes. Blended/transmissive materials and video media remain outside this
cutout-caster contract.

## Authoring and replication

In **File > Project Settings > Renderer**, select Advanced and enable the
desired cascade count. **Shadows** contains the new controls:

| Control | Property | Default | Range |
| --- | --- | --- | --- |
| Cascade blend | `shadow_blend_band` | 0.1 | 0–0.25 of map extent |
| Shadow filter radius | `shadow_filter_radius` | 2 | 0–4 shadow texels |
| Contact shadow length | `contact_shadow_length` | 0 (off) | 0–10 world units |
| Contact thickness | `contact_shadow_thickness` | 0.02 | 0–1 world units |

Select a directional light and use **Properties > Light > Sun radius** for
the source's angular radius, in turns. The numeric field displays six decimal
places. Zero removes the variable penumbra while retaining the project filter.
The UI bounds radius below a quarter-turn; larger legacy-authored radii are
bounded at the shading conversion. The penumbra itself always stops at 16
texels.

The four renderer properties are clamped at the model boundary, written into
project settings, and included in scene and renderer-settings digests. The
temporal profile incorporates that renderer digest, so authored changes reset
the applicable history and capability evidence. Sun radius already belongs to
the replicated/digested light state.

## Verification

`mediaserver.render_shadow_stability` runs production shader code on analytic
GPU maps, then exercises real scene rendering. It verifies:

- Bare flat and sloping receivers stay fully lit at zero, two and four texels,
  with the sun-size path enabled.
- Wider kernels and sun size widen the partial-visibility region; repeats are
  byte identical.
- A moving cascade band transitions smoothly between lit and occluded maps;
  stationary receivers outside the band retain their value.
- A fully occluded neighboring atlas tile cannot darken an empty tile.
- A contact occluder affects the expected ray while a coplanar receiver does not.
- A textured MASK model opens holes in camera coverage and its spot shadow,
  and changes the illumination of the common receiver behind it.
- Changing the authored sun radius and contact length changes native pixels.
- Independent renderers for 63/65-pixel node slices reproduce the complete
  RGBA16F image byte for byte with all the new shadow options and MASK enabled.
- Camera dolly preserves cascade scale/depth basis and the full-scene map.
- Settings survive a non-default project round-trip, change digests, clamp
  invalid values, and refuse inadequate apron budgets.

Editor tests exercise the Shadows tab, Apply/reopen and exact sun-radius
authoring. Both complete Windows GUI builds passed, with **147/147 CTest
entries passing in Debug (102.36 s) and Release (75.27 s)**. GPU evidence is
from independent renderers on this machine; physical multi-node/wall and
cross-vendor qualification remain item 23.

## Native walkthrough

1. Open a scene with a floor, objects and a directional light; select Advanced
   and two or more cascades. A dedicated fixture is `out/step19-review.mstage`.
2. In Shadows, set blend to 0.15, filter radius to about 3, contact length to
   0.5 and thickness to 0.05. Apply and inspect the objects' floor contacts.
3. Select the sun. Scroll its Properties pane if needed, double-click Sun
   radius, enter `0.01`, and press Enter. Compare to `0.003` on a raised caster.
4. Move the camera through the shot and pause. The cascade transition should
   fade, and repeated paused frames should retain their shadows.
5. Import a glTF material with alpha mode MASK and a patterned alpha texture.
   Inspect both the card's visible holes and the shadow behind it.
6. Save and reopen the project. The Shadows controls and sun radius should
   retain their values.

The native review loaded the two-sphere/floor fixture with a sun and two spots,
showed Advanced ready, changed the filter from 2 to approximately 3, authored
sun radius 0.01, and saved the separate review project. Existing editor
sessions were preserved.
