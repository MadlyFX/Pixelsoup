# 21 — Object motion blur at physical shutter

Implemented from item 21 in [the assessment](17-assessment-and-remaining-items.md).
Advanced rendering reconstructs object motion during the camera's shutter
interval. Camera motion contributes no rendered blur: the physical camera
already integrates that motion when it photographs the wall.

## Operator controls

File > Project Settings > Geometry > Object Motion Blur exposes the switch
and Maximum radius, in output pixels. The default is off with a 32-pixel
radius; the supported radius is 0–64 pixels. Zero disables reconstruction.
These fields persist and replicate through the ordinary project document and
participate in scene, renderer-profile and capability digests.

Select the camera and open Properties > Camera > Shutter s. The numeric
field displays six decimals and retains the precision of typed input.
For a tracked plate, the bound physical camera supplies this duration;
the unrelated scene camera cannot override it. Ordinary scene renders use
their scene camera's duration.

The exposed fraction is `clamp(shutter_seconds × show_fps, 0, 1)`, using the
same replicated rational frame rate as animation. At 24 fps, 1/48 second is
a half-frame exposure and 1/96 second is a quarter-frame exposure. An object
moving 16 pixels per show frame therefore contributes an 8- or 4-pixel
streak, respectively, in addition to its geometric width. Maximum radius
caps each side of this centred streak. Invalid/zero timing produces identity.

## Rendering contract

- A separate RGBA32F auxiliary replay projects the current and previous
  object transforms through the **current** canvas/view projection. It uses
  the existing geometry/material coverage path, including MASK coverage.
  The camera-and-object velocity target used by TAA remains separate.
- The reconstruction uses two bounded neighbourhood scans and exactly 33
  midpoint shutter samples. Integer texel offsets and deterministic tie
  order preserve the same weights on independently rendered slices. There
  is no frame-time-dependent sample count, radius, allocation or disk work.
- A depth-aware gather preserves static foreground occluders and processes
  premultiplied HDR colour. An unchanged pixel takes an exact identity path.
  The maximum-motion neighbourhood allows a visible moving silhouette to
  spread into its surrounding background.
- The direct renderer runs shutter reconstruction after antialiasing and
  before bloom. The compositor combines visible motion from its layers and
  applies reconstruction once, after the completed composite and fog, before
  bloom and final cropping. It never hands an individually blurred silhouette
  with unchanged hard depth to the layer compositor.
- The authored maximum radius adds an apron lane to the existing sequential
  filter support. Both ordinary canvas slices and guarded tracked plates
  render the required neighbours before cropping. Insufficient guard limits
  refuse the frame. Basic reports the Advanced-only pass as unsupported.
- A repeated show frame retains its previous object-transform pair. Pause
  and Play commands may advance the document revision without changing the
  picture: the evaluated scene carries a deterministic content digest that
  excludes only `show.transport`. Full model rebuilds and incremental updates
  produce the same identity. Actual authored edits, projection/camera identity changes,
  show-rate/shutter changes, and nonconsecutive seeks still cut history.

The pass publishes active/refused/unsupported status and refusal reasons.
Renderer diagnostics count direct passes and resource refusals. The renderer
settings panel includes object motion blur in its running-pass summary.

## Scope and limits

This is screen-space reconstruction from currently visible geometry, with
one dominant motion vector per neighbourhood. It cannot reveal hidden
surfaces or exactly integrate competing overlapping motions. Its fixed
33-point, nearest-texel reconstruction is quantized at pixel scale. A very
fast object saturates at the authored radius instead of silently increasing
cost or reading outside the guard.

Motion comes from object/model/media-surface transforms. Motion inside decoded
video pixels, skeletal deformation and vertex animation need their own motion
sources; this step does not infer them. Light and shadow animation is not a
separate velocity source. Shutter intervals longer than one show frame are
clamped to one frame.

Compositor layers with spatial filters, a legacy blur radius, masks, mattes,
or non-Normal blends are explicitly refused while object blur is requested.
Those processors change visible coverage after geometry and need a motion/depth
representation of their processed output before this combination can be honest.
The normal unfiltered scene-layer path, HDR, temporal AA, temporal AO, fog and
bloom use the existing supported composition order.

## Verification

`mediaserver.render_object_motion` covers authored non-default round trips,
clamping, rational cadence, physical-camera shutter selection, profile and
scene digests, and apron bounds. GPU readbacks run on SDL GPU and D3D12:

- A moving HDR bar has the predicted quarter-, half-, and full-shutter span,
  preserves integrated energy/premultiplication, respects its radius cap,
  and cannot smear over a nearer static occluder.
- Six camera-movement frames of static geometry are byte-identical with blur
  enabled and disabled. Camera velocity is nonzero while object velocity is
  zero. Object movement under that camera produces a streak.
- Repeated frames agree exactly; native-style transport document edits and
  model rebuilds preserve Hold/Advance, while a real authored edit and a seek
  reset history. An abandoned reconstruction recording can be retried.
- Twelve moving compositor frames match independently rendered 63/65-pixel
  node slices against a 128-pixel full render, byte for byte. Unsupported
  filtered-layer combinations refuse explicitly.

The existing temporal-stability suite also runs 32 moving tracked-plate
frames with object blur, temporal AA, temporal AO and bloom, checking exact
independent slices and held frames. A fourth unblurred plate verifies that
reconstruction visibly changes more than eight of those frames, so a zero
motion/identity path cannot satisfy the test. Editor tests cover the toggle, radius,
Apply/Cancel and precise shutter entry.

Both full Windows GUI builds pass. All 148 CTest entries have passed in Debug
and Release. The final full Release suite passed 148/148 in 203.06 seconds.
The post-fix Debug suite passed 147/148; the existing fog-digest test was then
updated to compare its visual field independently of authored-history metadata
and passed its rerun in 17.41 seconds. Final rebuilds and both focused GPU
suites passed in Debug (5.19 seconds) and Release (4.49 seconds).

This is local GPU evidence, not physical two-machine or cross-vendor
qualification. Cohort qualification remains item 23.

## Native walkthrough

1. Use Advanced rendering. Place a bright narrow box in front of a static
   background and animate its horizontal position on the timeline.
2. Set the timeline to 24 fps. Select the camera and enter `0.0208333333`
   in Properties > Camera > Shutter s.
3. Open File > Project Settings > Geometry. Scroll to Object Motion Blur,
   turn it on, set Maximum radius to 32 and Apply. Play the timeline and
   inspect Program. The moving box spreads horizontally; static geometry
   remains sharp.
4. Pause. The held image retains its streak. Resume, then seek to a distant
   frame: the first frame after the seek starts a fresh motion pair.
5. Enter `0.0104166667` for the shutter, resume playback and compare the
   shorter streak. Changing the camera position alone must not add blur.
6. Change the radius or toggle and Cancel to discard the draft. Apply the
   desired settings, save the project, reopen it and inspect the controls.

The review fixture is `out/step21-review.mstage`, with a moving gold bar and a
static blue bar at 24 fps. It uses an isolated application settings directory
and Director WAL under `out/`.

The native review confirmed playback blur, sharp static geometry, preserved
blur on Pause, the shorter 1/96-second shutter and Apply/Cancel. Applying a
16-pixel radius and saving/restarting restored the settings. The saved project
retained the full typed shutter value `0.0104166667`.
