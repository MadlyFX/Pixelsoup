# 11 — Animation and stage context

Status: proposed. Depends on: 06, 08.

## Outcome

Artists can work against stage/camera references while published animation remains
deterministic and independent of a running Blender session.

## Work

- Send physical-stage alignment, a reference camera, lens metadata, and selected
  show-time context from Media Server to managed Blender reference objects.
- Keep tracking samples separate from authored document edits and undo. Define
  bounded latest-sample delivery; tracking does not create persistent revisions
  at camera sample rate. Media Server remains authoritative for final ICVFX views.
- Specify camera axis, focal length, sensor fit, clipping, and which lens distortion
  or calibration effects Blender's reference viewport cannot reproduce.
- Publish supported node/skin animation through the existing model animation path.
  Map source time to rational show time with defined start, loop, and interpolation.
- Bake unsupported constraints and simulations into an explicitly supported cache
  or reject them. Add vertex-cache support only if required and separately budgeted.
- Keep shot offsets outside source animation. Define hold-placement behavior for
  animated roots and do not flatten animated transforms into static geometry.
- Advance animation and wind from the committed show frame on each render node,
  not from Blender's wall clock or arrival time. Reset motion history appropriately
  when topology or animation bindings change.

## Acceptance

Compare the same published pose across nodes at chosen show frames, after seeking,
and after restart. Disconnect Blender and continue playback. Animate a parent while
retaining a shot adjustment. Verify stage/camera alignment against known markers
and document differences between reference viewport and final calibrated output.
