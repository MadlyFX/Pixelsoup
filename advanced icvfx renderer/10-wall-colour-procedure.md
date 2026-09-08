# 10 — Measuring a wall and producing its look

The procedure that goes with [10 — Display transform and
calibration](10-display-transform-and-calibration.md). The code in that step is
only the delivery mechanism; this is what actually gets the colour right on the
day. Written to be followed cold by whoever is standing at the wall with a probe.

## What is being matched, and what is not

The chain on a volume is:

    renderer → output transform → LED processor → panel → lens → sensor → camera IDT → editorial

Media Server owns exactly one link: the **output transform**, applied last on
the node that owns the output. It is one baked table - the display transform
composed with the show look - plus an ordered dither to the output's bit
depth. Everything after it is the wall's and the camera's.

Per-panel and per-wall correction (uniformity, white point, primaries) is
**downstream on the LED processor** for this deployment and stays there. If
the wall does not measure as the processor says it should, fix the processor
first; a look baked here to compensate for a miscalibrated wall bakes that
wall's fault into the show and follows the project to the next stage.

The look produced by this procedure therefore answers one question only: *what
correction, applied to the renderer's display-referred output, makes the wall
as photographed match the reference monitor?* That is the residual after the
processor has done its job - the panel's spectral response as the camera sees
it, the lens, the sensor, the IDT - and it is a property of this wall with this
camera.

## Prerequisites

- The project running Advanced, and every render node reporting it. Colour is
  applied on the scene-linear path only; a project on Basic shows the COLOUR
  tab's status as *Ignored*, and nothing here reaches the wall.
- A spectroradiometer or colorimeter (CR-250/300, i1Display Pro or equivalent)
  and the software to read it. A colorimeter needs a matrix for the panel's
  primaries; a spectroradiometer does not.
- The show camera, at show settings: the lens, sensor mode, ISO, shutter and
  white balance the shoot will use. Record log, with no monitoring LUT.
- The reference monitor the DP and post have agreed on, calibrated, in the
  colour space post expects to receive.
- A grading tool that can read the camera's log through its IDT and export a
  3D LUT as a 33-point `.cube`. Resolve, Baselight and Nuke all can.
- One shared path visible from every node, at the **same absolute path** on
  each. Assets are paths here, never copies; a look at a path only one machine
  can see is a look one machine applies.

## The chart

A scene of flat emissive patches, so the value on the wall is a known
scene-linear number and nothing else:

1. New scene, Advanced. Environment: ambient 0, no map, sky black, fog off.
   Remove every light. Camera exposure at its defaults (f/2.8, 1/48 s, ISO
   100, 0 EV), which is the neutral multiplier - exposure would otherwise scale
   the patch values before the transform sees them.
2. Add planes facing the camera, each with a black base colour and an emissive
   colour at the value the patch should carry. Use a neutral ramp at least:
   0.02, 0.05, 0.10, 0.18, 0.36, 0.72, 1.0, 2.0, 4.0 - and the six primaries
   and secondaries at 0.18. The values above 1.0 are the point of the shaper;
   include them.
3. Patches large enough to fill the probe's aperture and to give the camera a
   flat field of at least 100 pixels on a side after the lens.

Save it. It is the chart for every subsequent check, and it should be part of
the show's project set so a recheck between setups takes minutes.

## Step 1 — Choose the transform, and verify it with the probe

Project Settings → COLOUR. Set the **display transform** before anything else
and do not change it afterwards: the look composes *after* it, so a look
measured under one transform is wrong under the other.

- **SRGB** is the reference: a straight encode, clipping at white. Choose it
  when the processor and camera pipeline are set up for display-referred
  BT.709 content and post wants the straight encode.
- **ACES APPROX** is the well-known fit of the ACES tone curve in AP1 with the
  sRGB encode. Choose it when the show wants a filmic shoulder on the wall.
  Note that it is the *approximate* fit, named so deliberately; if post
  requires a specific ACES version and output transform, that table is
  supplied as a measured bake rather than chosen here.

Leave the look empty and **dither on**. Apply. The status line under the Look
LUT field must read *Transform baked and live on the output*.

Now put the probe on the wall and read the neutral ramp. What you are checking
is the processor and panel, not this software:

- With SRGB, a correctly calibrated processor returns luminances proportional
  to the sRGB decode of the encoded value. Compute the expected encode with
  the reference table in `display_transform.cpp` - black, 0.18 and 1.0 are
  checked by the unit suite against hand-worked sRGB - or simply against the
  sRGB curve; the measured deviation is the processor's.
- A wall that measures far from expectation, or that measures differently
  between panels, is a processor calibration problem. Stop and fix it there.
  The look built below would otherwise absorb it.

Values above 1.0 measure as the shoulder (ACES) or as clipped white (SRGB).
Both are correct; the difference between the two is the reason the transform
is chosen first.

## Step 2 — Shoot the chart

With the camera at show settings, at the distance and angle of the master
shot, record the chart for a few seconds. Also record the reference monitor
showing the same chart, if the pipeline allows it, or - better - display the
chart's *expected* values on the reference monitor from the grading tool
directly. The reference is what the patch *should* look like to post; the
recording of the wall is what it *does* look like.

Record with no monitoring LUT and no in-camera look. Everything in this step
is measured after the IDT in the grading tool.

## Step 3 — Build the look

In the grading tool:

1. Bring in the wall recording through the camera's IDT into the working
   space post uses.
2. For each patch, read the value and compare it with the reference. The
   difference is what the look must correct.
3. Build the correction as a transform on **display-referred** values - its
   input is the renderer's encoded output, 0 to 1 per channel, exactly as the
   wall receives it. Do not build a scene-referred (LMT-style) look; that is a
   different slot and this pipeline has no place for it.
4. Export as a 3D `.cube` at 33 points a side, red varying fastest (which is
   what every tool writes). 65 is accepted; there is no benefit at this stage.
   1D tables are refused.
5. Save it to the shared path, at the same absolute location on every node.

Keep the correction gentle and monotonic. A look that pushes the wall's
brightest patches past the encode range clips on every node identically, which
is at least consistent; a look with a crossed-over channel is a look that
inverts a gradient on camera.

## Step 4 — Author it

Project Settings → COLOUR → **Look LUT**: paste the path. Apply.

The status line reports what the output did with it:

- *Transform and look baked; live on the output.* — done.
- *Baking the table on the residency worker.* — wait one frame or two; the
  bake happens off the frame path and lands on the next boundary.
- *Look could not be read. 3D .cube only; the transform is live without it.* —
  the path is wrong, the file is not a 3D `.cube`, or it is malformed. The
  wall shows the transform alone meanwhile, which is a predictable picture,
  and stays that way until the path is fixed.
- *Ignored: colour is applied on the scene-linear path…* — the project is
  running Basic. Nothing here reaches the wall.

Then check **every node**, not only the Director. A render node reads the
file from its own disk; one that cannot prints `atmosphere`/`bloom`-style
idle reports do for those passes - a single line on stderr naming the
failure - and shows the transform alone. There is not yet a cohort-wide check
that every node holds the same look generation, so this is the operator's
check to make until there is. See "Still open" in step 10.

## Step 5 — Verify through the camera

Shoot the chart again with the look live and pull it through the IDT. The
patches should now sit on the reference within the tolerance the DP accepts;
a typical target is ΔE 2000 under 2 on the neutral ramp and under 4 on the
saturated patches.

If the neutrals are right and the saturated patches are not, the processor's
primaries are the likelier cause than the look; recheck step 1 with the probe.
If everything is off by a constant, the camera settings changed between
steps 2 and 5.

## Re-checking during the shoot

Colour drifts: panels warm up, the camera's white balance is changed, a lens
is swapped. Re-run steps 2 and 5 whenever any of those happen. The chart is
saved with the show, the look is a path, and a new `.cube` written to the same
path is picked up on the next frame boundary after the residency worker reads
it - no restart, no relink, no project reload.

## Dither, and what is deliberately not here

- **Dither is on by default** and should stay on. It is a fixed 8×8 ordered
  pattern keyed to the absolute wall pixel, half a code value in amplitude,
  identical on every node for the pixel they share, and far below any
  camera's noise floor. Turn it off only to demonstrate the banding it removes.
- **Bit depth** is the output's: eight bits per channel, because that is what
  this build presents. A ten-bit output needs a ten-bit swapchain in the RHI
  first, and dithering to a depth the output does not carry would only throw
  away precision.
- **White point, primaries, PQ/HLG and per-panel uniformity** belong to the
  processor and are not authored here.
- **A per-wall look** is not offered. The show look belongs to the project;
  a stage with mismatched panels is corrected at the processor.
