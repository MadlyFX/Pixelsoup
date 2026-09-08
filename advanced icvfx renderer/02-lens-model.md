# 02 — Lens model

**Track 1. Pairs with [01](01-camera-tracking-and-inner-outer-frustum.md).**

## Why

A tracked pose without a lens is half the answer. The inner frustum has to match
the physical camera's actual field of view, entrance pupil position, and
distortion, or the wall content will not line up with the foreground when the
camera moves — the background will slide against the set piece it should be
locked to. Getting focal length wrong by a few percent is a parallax error that
grows with camera travel, which is exactly when it is most visible.

`scene::Camera` today has `field_of_view` as a scalar in turns applied to the
canvas height, plus near/far and the physical exposure controls. That is a
previz camera. A production lens needs sensor size, focal length from the zoom
encoder, entrance pupil offset, and a distortion model.

## The split that matters

Two kinds of data, and they must not be confused:

- **Lens profile** — authored, replicated document state. Sensor dimensions,
  distortion coefficients, entrance pupil offset, and the focal-length and focus
  breathing curves. This is *calibration*: it is measured once per lens on a
  chart, changes only when someone recalibrates, and belongs in the project.
- **Encoder values** — per-frame control data. Zoom, focus, iris positions from
  the lens. These change every frame and ride the same channel as the tracked
  pose ([01](01-camera-tracking-and-inner-outer-frustum.md)), sampled and stamped
  by the Director, for exactly the same reason: putting them in the document
  would reset temporal history every frame.

Every node evaluates `profile + encoders → effective projection` with the same
expression. No node interpolates its own calibration.

## Seams

- `engine/scene/include/mediaserver/scene/scene.hpp` — a `LensProfile` value and
  a `LensEncoders` value; `LensProfile` authored beside the camera, `LensEncoders`
  on the evaluated scene beside the tracked pose.
- `engine/scene/include/mediaserver/scene/scene_schema.hpp`,
  `engine/scene/src/scene_model.cpp`, `apps/server/playback.cpp` — the usual
  authored-state path: keys, read with clamping at the model boundary, write.
- `engine/scene/include/mediaserver/scene/canvas.hpp` /
  `engine/scene/src/canvas.cpp` — `window_for_slice` gains a lens-aware form.
  This is the single most important seam: two adjacent slices must still produce
  a bit-identical shared edge, which is the property `window_for_slice` was
  written to guarantee and is already tested for.
- `engine/editor/src/workspace_panels.cpp` — lens profile authoring lives in
  Project Settings beside the renderer policy; live encoder values are status,
  not controls, and belong wherever the tracking status reads out.

## Design

**Field of view from physics, not a turn scalar.** With sensor width `w`, sensor
height `h`, and focal length `f`:

```
half_width_at_near  = near * (w * 0.5) / f
half_height_at_near = near * (h * 0.5) / f
```

`FrustumWindow` already carries `left/right/bottom/top/near/far` explicitly and
already supports asymmetry. Derive those four from the sensor and focal length
instead of from `tan_half_turns(field_of_view)` applied to canvas height, then
slice exactly as today. Keep the existing turn-based path as the authored
fallback when no lens profile exists — an unauthored lens is not an error.

**Entrance pupil (nodal) offset.** The tracker reports the position of a marker
on the camera body; rays actually originate at the lens entrance pupil, which
moves along the optical axis as the lens zooms. Offset the eye by the profile's
pupil distance for the current focal length before building the view. This is
small — centimetres — and is the difference between a background that locks and
one that swims slightly during a push-in.

**Distortion.** The render must be **undistorted**: the physical lens will
distort whatever the wall shows, so pre-distorting would double it. What
distortion changes is *coverage* — a distorted lens sees further into the
corners than the undistorted frustum implies. So:

- Use the undistorted projection for rendering.
- Use the **distorted** field extent when sizing the inner-frustum rectangle in
  [01](01-camera-tracking-and-inner-outer-frustum.md), so nothing is missing at
  the edges of what the camera actually captures.

Support Brown–Conrady radial/tangential coefficients as the authored model, and
leave room for an ST Map (a texture of per-pixel displacement) as a later
alternative — ST Maps are how most vendors ship high-order lens calibration, and
they are an asset with a generation, so they follow the residency contract.

**Breathing and focus.** Focal length and distortion both vary with focus
distance on real lenses. Model the profile as a small table over
(zoom encoder, focus encoder) with deterministic interpolation. Table lookup and
interpolation must be pure and identical across nodes — no caching keyed on
"last frame's value".

## Multi-node hazards

- Interpolating the profile table with any per-node state. Make the lookup a
  pure function of (profile, encoders).
- Floating-point drift between nodes in the window derivation. Follow the
  existing `window_for_slice` rule: form each edge from the same expression over
  the same replicated integers, with no accumulation and no per-node origin.
- A missing encoder frame. Hold the last replicated value **that the Director
  stamped**, never a locally extrapolated one.

## Tests

- A known lens profile and focal length produce the expected `FrustumWindow`;
  compare against hand-computed values for a 35 mm sensor at several focal
  lengths.
- Adjacent slices of a lens-derived window share a bit-identical edge — extend
  the existing slice tests rather than writing a new harness.
- Entrance pupil offset moves the eye along the optical axis by the profile
  amount and by nothing else.
- Distortion: a point at the undistorted frustum edge maps inside the distorted
  extent; the inner-frustum rectangle sized with distortion is never smaller than
  the one sized without.
- No lens profile authored → byte-identical output to the current turn-based
  camera path.

## Definition of done

An authored lens profile plus live encoders produce a projection matching the
physical camera closely enough that background and foreground stay locked
through a dolly move; the inner-frustum rectangle covers the distorted field;
and adjacent nodes still share an exact edge.

## Open questions

- **Brown–Conrady versus ST Map first.** Coefficients are simpler and enough for
  most cine primes; ST Maps are what calibration tools actually output. Probably
  coefficients first, ST Map behind the same seam.
- **Where does calibration come from?** Authoring distortion coefficients by
  hand in Project Settings is not realistic. Either import a vendor calibration
  file, or accept that this arrives as an asset and design the ingest now.
- **Does the outer frustum need the lens at all?** It is not photographed
  directly, only as spill and reflection. Probably not — which would keep the
  lens strictly inside the inner-frustum path.
