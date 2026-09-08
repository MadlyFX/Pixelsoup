# 28 — Lens-driven depth of field on the plate

Implemented from item 28 in [the assessment](17-assessment-and-remaining-items.md).
Advanced renders additional lens blur into tracked plates, accounting for the
blur the physical camera will already impose on the LED wall. Ordinary canvas,
Stage and outer-frustum renders retain their existing image path.

## Operator controls

File > Project Settings > Geometry > Plate Depth of Field exposes the switch
and Plate maximum radius. The default is off, with a 32-pixel maximum; the
supported range is 0–64 **plate target pixels**. Zero disables the gather.
Changing plate density changes the conversion from physical blur to plate
pixels. The maximum caps the resulting ellipse's major radius.

Select the physical camera and open Properties > Camera:

- **Focus m** is an authored fallback for missing measured focus. Zero means
  unavailable, not infinity. Enter the physical lens's current focus distance.
- **Aperture f/** supplies the aperture fallback. It is also the camera's
  existing photographic exposure setting.
- The camera's Lens calibration supplies focal length when the tracker has
  none. Enable it and enter the lens's focal length and sensor dimensions.

OpenTrackIO's physical focal length, focus distance and f-stop take precedence
over these fallbacks for plate blur. FreeD encoder counts remain counts: this
step does not invent a conversion from an uncalibrated focus encoder to metres.
The existing lens table still resolves entrance-pupil position and calibration.
Measured iris drives blur independently of the existing authored exposure policy.

The physical wall must be authored in metres in Feed Mapping and the output
must reference that surface. Enable Inner frustums in project settings and on
the bound camera, and check that Live Data reports a received or held pose.
The effect applies on the mapped output's tracked plate; the editor's ordinary
Program/Stage view is not an optical plate preview.

## Optical contract

For focal length `f`, focus distance `s`, f-number `N`, and virtual axial depth
`z`, the thin-lens sensor radius is:

```
r(z) = f² / (2 N (s - f)) × abs(1 - s / z)
r_added = max(r(z_virtual) - r(z_wall), 0)
```

All lengths in this equation are metres. Depth is along the physical camera's
optical axis from the calibrated entrance pupil. Each pixel reconstructs its
virtual position and intersects the same ray with the replicated wall plane,
so a tilted wall has a spatially varying physical depth. Empty depth is treated
as infinity. A local projection Jacobian maps the sensor disk into a wall
ellipse; its major singular value is bounded by the authored maximum.

This uses the scalar radius subtraction requested by the assessment. It is an
approximation to the convolution of real optical point-spread functions, and
cannot undo blur already present in the photographed wall.

The original test sentence, "focus at the wall distance is byte-identical to
no depth of field", conflicts with that equation when virtual geometry lies
behind the wall. The implemented identity is **geometry at the wall distance**:
its virtual and physical circles cancel. Focusing on the wall makes the wall
sharp; virtual background behind it still needs additional blur. Tests cover
both cases, plus virtual geometry whose required blur is smaller than the
wall's existing blur.

The thin-lens basis and the limitations of a depth-based gather are described
in [NVIDIA GPU Gems 3, chapter 28](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-28-practical-post-process-depth-field).

## Rendering and replication

- The compositor merges visible reversed depth across its scene layers. It
  applies the gather once to the completed linear HDR composite, after fog and
  object motion reconstruction, before bloom and the final plate crop.
- The disk contains a fixed 113 taps. Integer texel offsets, fixed traversal
  order and the shader floating-point contract keep repeats deterministic.
  There is no random seed, wall clock, adaptive quality or per-frame allocation.
  Targets follow the existing construction/authored-resize resource policy.
- Premultiplied RGBA is filtered together. The implementation accumulates
  differences from the centre so constant colours retain their exact values.
  Sub-half-pixel support returns the centre unchanged.
- A separate `plate_dof` apron lane adds the authored radius to sequential
  filter support. It is reserved only for guarded lens views. Insufficient
  apron limits refuse the frame rather than shortening the kernel.
- Each participating node continues to render the same complete guarded
  plate, then composites its owned wall slice. No optical parameter is derived
  from the node's own output rectangle.
- The receiver's physical values now survive Director stamping, wire encoding,
  node reconstruction, the pose channel, held poses and evaluated scene state.
  Raw encoder controls travel separately. Session protocol **10** requires all
  cohort participants to upgrade together.
- Toggle, radius, authored focus and measured lens values participate in the
  relevant scene and renderer digests. The pass is stateless and follows live
  lens changes even when the show timeline is paused; TAA retains unblurred
  scene history upstream.
- Cohort conformance fixture **3** adds a twenty-first digest for this production
  gather with changing physical focus over sixteen frames. The independent
  conformance sample qualifies the shader; the plate integration suites verify
  composition order and seams.

Missing/invalid physical optics, unavailable targets, insufficient guard and
unsupported composition each publish a reason and refuse the plate frame.
Basic reports the plate request as unsupported. A non-plate view or zero radius
reports an explicit idle reason. Compositor statistics expose pass/refusal
counts; project settings include the pass in their renderer status summary.

## Scope

This is a single visible-depth reconstruction, not an optical path tracer. The
depth gate keeps nearer occluders out of a blurred background. It cannot reveal
surfaces hidden behind foreground silhouettes, reconstruct separate depths
inside transparency/refraction, or exactly convolve competing motions and blur.
Foreground silhouettes can remain hard. The fixed disk can show discrete taps
at very large radii; no node changes its kernel to hide that cost.

Spatially filtered layers, legacy layer blur, masks, mattes and non-Normal
layer blends are explicitly refused while plate DOF is requested: their
processed coverage does not match the available geometry depth. Normal scene
composition, temporal AA, temporal AO, fog, object blur and bloom are supported.
The current ICVFX context uses the existing planar physical-surface contract;
this is not a per-triangle optical calibration for curved volumes or a model of
anamorphic bokeh, cat-eye vignetting or diffraction.

## Verification

`mediaserver.render_plate_dof` verifies:

- Nondefault persistence, model-boundary radius clamping, scene/profile digests,
  measured controls and calibrated/authored fallbacks, and additive plate guards.
- Independent thin-lens numbers: 100 mm, f/2, focus 1.1 m, wall 2.2 m and virtual
  plane 4.4 m produce a 0.625 mm residual sensor radius. At the fixture density
  this becomes 8.8 pixels, verified by native GPU readback.
- Exact wall-depth and zero-radius identity; virtual background still blurs
  with focus at the wall; halving aperture diameter halves the radius.
- The predicted ellipse on a wall tilted by 30 degrees, maximum-radius bounds,
  HDR energy/premultiplication, sharp nearer occluders and canceled recordings.
- Eight changing camera/focus frames through independent full, 63-pixel and
  65-pixel output stages. Both node halves match the reference bytes, held frames
  repeat, every frame differs from a separately rendered disabled reference,
  and pixels outside the plate retain the outer image.
- Ordinary-view identity, explicit missing-data/guard/layer/backend refusals.

The existing 32-frame temporal stability test also enables this pass together
with motion blur, temporal AA, temporal AO and bloom. Editor tests exercise
toggle/radius Apply and Cancel, plus typed physical focus and aperture. Network
tests cover nondefault physical values, invalid values, held samples and the
updated frozen protocol bytes. Scene-model tests retain authored focus.

Both complete Windows GUI configurations built successfully. Full CTest runs
passed **154/154 in Debug** (177.58 seconds) and **154/154 in Release**
(143.01 seconds). After the native review's idle-status presentation fix, both
configurations were rebuilt and their inspector/workspace tests passed again.
The local Debug Director/output-node walkthrough exercised live and held
OpenTrackIO poses and toggled the pass through Apply: the gold background
softened while the blue wall-depth reference remained sharp, with no plate
refusals. The Release editor accepted typed focus 4.75 m and aperture f/1.8;
Save Project persisted both values with DOF on and radius 32, confirmed by
decoding the saved project. A radius draft of 59 was canceled without dirtying
the project. The ordinary preview's idle explanation was verified in neutral
text rather than the renderer fault colour.

## Native review fixture

From the repository root, run:

```powershell
python "advanced icvfx renderer/fixtures/step28-plate-dof.py"
```

This creates `out/step28-review.mstage` and an isolated recent-project profile
under `out/step28-profile`. Use that project in the editor. Its mapped wall is
three metres away; the blue reference is at wall depth, with five gold bars at
twelve metres. The local tracking source listens on UDP 40328.

1. Open the project in the Director, connect an output node at node index 0,
   and display its mapped 640×360 output. Press Play, then optionally Pause;
   the stopped transport deliberately presents black on an output node.
2. Run the fixture script with `--send --seconds 180`. Live Data should show
   OpenTrackIO reception. This sends pose/focal length while leaving physical
   focus and iris absent, so camera fallback edits are visible.
3. In File > Project Settings > Geometry, toggle Plate depth of field and Apply.
   The gold background changes; the blue wall-depth reference remains sharp.
   Change the maximum radius, then Cancel to verify draft edits are discarded.
4. Select the camera, open Properties and change Focus m or Aperture f/.
   The output follows these controls. Setting focus to three metres keeps the
   blue reference sharp and still blurs the background. Stopping down reduces
   the added blur. Aperture also changes authored photographic exposure.
5. Stop the first sender and run it with `--send --focus 2 --aperture 1.4` to
   verify measured optics take precedence over authored fallbacks for blur.
6. Stop sending. The held pose retains the last measured lens values. Save the
   project and reopen it to verify the authored toggle, radius and fallback.

Local readbacks and local Director/node checks are not physical camera/LED
qualification. Measure the deployed lens, surveyed wall geometry and processor
pipeline, and run the cohort check on the actual machines before a shoot.
