# 33 — Pose prediction and filtering

Implemented 2026-09-08. This completes item 33 in
[the assessment](17-assessment-and-remaining-items.md#professional-workflow).

## What it does

The Director can estimate where each tracked camera will be at its authored
latency-compensated control stamp. It fits constant velocity to recent position
and orientation measurements, optionally filters jitter, and replicates the
finished pose. Render nodes continue to hold and apply those samples through
the existing pose channel. They do not fit motion or extrapolate locally.

The project-wide controls live in **File → Project Settings → Tracking**:

| Control | Default | Range and effect |
| --- | --- | --- |
| Predict camera motion on the Director | Off | Enables constant-velocity extrapolation. |
| Latency | 2 frames | 0–16 control frames ahead; shared with the existing stamping policy. |
| Fit samples | 4 | 2–16 fresh measurements; larger windows smooth velocity but react more slowly to changes. |
| Jitter filter % | 0 | 0 keeps the measured present pose; 100 uses the fitted present pose. Intermediate values blend between them. |

Filtering also works with prediction off. With both off, the original pose bits
are preserved. The first fresh sample after a reset passes through unchanged;
two distinct control stamps are needed to fit velocity. These controls apply
to every bound tracked camera, including Stage inspection while the plate
master switch is off. The existing plate switches still determine what the
wall renders.

The timeline can remain paused: prediction uses the advancing control clock.
A repeated control stamp, unchanged source publication, duplicate tracker
sequence, or missing source sample produces no new pose. The nodes finish
applying already queued samples and then hold the last published pose exactly.
The Director never produces additional extrapolations from a stopped feed.

## Arithmetic and bounded state

`apps/server/camera_prediction` owns a fixed bank for four cameras. Each fit
holds at most sixteen measurements, one next-frame forecast, and seventeen
latency forecast slots. The service performs no allocation, I/O, or clock reads.
Each source slot is sampled once per Director iteration, so two cameras bound
to the same feed cannot observe different datagrams within that iteration.

For each of X, Y, Z, pan, tilt and roll, `scene::deterministic::fit_linear_motion`
fits a least-squares line in a fixed operation order. Time is the integer
control-frame difference from the newest stamp, converted to binary64 after
subtraction. This preserves small frame differences above 2^53. The reduction
also subtracts the newest position before fitting to reduce cancellation far
from the tracker origin. Both scene math and the predictor compile with the
deterministic floating-point options, including disabled contraction.

Angles unwrap along the shortest adjacent arc before fitting and normalize
again at publication. A +179° to −179° measurement therefore describes a 2°
move. The half-turn tie is −180°. The final pose is rounded to the existing
wire floats; measured lens encoders are carried unchanged. Field of view comes
from that same raw sample and its authored source settings.

The filter blends the latest raw value toward the fitted line at the current
stamp. Prediction then adds fitted velocity times the final target-frame
offset. The Director accepts that exact sample into its own channel and fans
it out through `DirectorFrameBarrier` in ascending camera order. No new wire
payload or node-side prediction path is required.

Changing prediction policy, source configuration, or control cadence restarts
the fit. Camera removal releases its fixed slot; stack reordering preserves
it; project replacement clears the bank. A tracker sequence rewind, protocol
or camera-id change, or gap longer than sixteen control frames also restarts
the fit. Tuning settings preserves the consumed-publication fence, and listener
reconfiguration fences old socket samples until a new datagram arrives.
Reducing latency holds publication until the control clock catches the previous
target, preserving strictly increasing pose stamps.

Invalid raw poses, lenses, stamp overflow, horizons beyond sixteen frames, and
predictions outside the existing replicated pose bounds are refused and counted.
Live Data names the refusal. A refusal clears the fit and leaves the last valid
published pose available to the channel. Newest-wins source coalescing is counted
from the publication difference at the Director boundary.

The three authored fields persist through the ordinary project model and
undo/redo path. They and latency always participate in
`renderer_settings_digest`, even with plates disabled, because they affect the
tracked pose used in Stage. The `POSEPRD1` digest marker identifies this changed
renderer contract.

## Reading Live Data

**Window → Live Data** shows the current prediction state and, while prediction
or filtering is enabled, the raw and sent poses side by side in stage metres
and degrees. Sample frame, target frame, and current fit size identify the
measurement behind the sent pose. The existing pose block continues to report
the channel's current resolved answer, which may be earlier than a newly sent
future sample.

**Next-frame error** compares a one-control-frame forecast against the raw
measurement when that exact next stamp arrives. **Latency error** separately
compares the actual sent forecast against the raw measurement at its target
stamp. Both report checked-sample counts, the last checked stamp, Euclidean
position error in millimetres, and the largest shortest-arc angle error among
the three axes in degrees. A skipped target is counted as unmatched; a held
socket value is never treated as ground truth. Resetting a fit also counts any
pending forecasts that can no longer be checked as unmatched.

Next-frame error exposes noise and changes in velocity. Latency error shows how
the model performs over the authored horizon. These compare control-frame
measurements at the Director; they do not measure physical sensor-to-photon
delay or automatically calibrate wall latency. A steady constant pan can have
zero model error at several horizons, so it cannot identify that physical
delay by itself.

## UI walkthrough

1. Run a Director with the editor. Add a Camera from the Library, select it,
   and use **Inspector → Tracking → + New feed** (or an existing feed). Enable
   listening, choose FreeD, and set a free UDP port, for example 40333.
2. Start the repository's simulator from PowerShell:

   ```powershell
   ./scripts/send-freed-tracking.ps1 -Port 40333 -Motion Pan -Rate 48 -Seconds 120
   ```

3. Open **Window → Live Data**. Check that received poses and the Stage camera
   frustum move while the timeline is paused. Enable the existing camera and
   project plate switches if testing a configured mapped wall output.
4. Open **File → Project Settings → Tracking**. Enable **Predict camera motion
   on the Director**, set Latency to 2, Fit samples to 8 and Jitter filter % to
   50, then click **Apply**.
5. In Live Data, observe **Raw > sent**, a target two control frames ahead,
   and advancing next-frame and latency checks. Scroll the panel if necessary.
   The sinusoidal simulator changes velocity, so its errors need not be zero.
6. Let the sender finish. Verify that the last sample, target and sent pose stop
   changing, prediction reports **Held: no fresh pose**, and Held frames grows.
7. Use **Edit → Undo** and **Edit → Redo** to verify the settings transaction.
   Use **File → Save Project**, close the editor, and reopen the project. The
   authored values must survive; runtime fit history starts empty.

## Verification performed

Both `windows-msvc-debug-gui` and `windows-msvc-release-gui` built successfully.
The full CTest suites passed **151/151 in Debug** (246.17 seconds) and
**151/151 in Release** (212.17 seconds). After the native walkthrough's final
diagnostic corrections, both configurations rebuilt and passed all six focused
CTest entries (scene ICVFX, editor workspace, Live Data, tracker listener,
tracked-camera bridge, and server bootstrap).

The tracked-camera bridge suite now has twelve cases. Its new coverage proves
exact constant-motion output across irregular stamps above 2^60, shortest-arc
angle wrapping on all axes, reduced alternating jitter, a known one-metre
acceleration error, matching versus missing forecast targets, immutable holds,
raw/lens preservation, invalid-output refusal, bounded slot reuse, source and
cadence resets, duplicate datagrams, and monotonic publication after latency
edits. Two registered Director sessions receive equal predicted frames through
their independent mailboxes and session codec, then resolve and hold equal
poses. This is a local mailbox/codec integration test; it does not claim a
physical two-machine run. Existing session transport regressions also pass in
the full suites.

Scene tests cover clamping, digest changes with plates on and off, and project
round-trip using nondefault authored values. Editor tests cover Apply/Cancel
and Live Data states/scrolling. Listener tests prove a reconfigured source
cannot return a sample from the previous socket configuration.

The native Debug walkthrough used an isolated local project and FreeD UDP feed:
prediction enablement, fit/filter edits, paused Stage tracking, raw/sent values,
forecast checks, sender-stop holds, undo/redo, and Save Project were exercised.
Release reopened that saved project with prediction enabled, fit size 8 and
filter 50, received another feed, and held sample 1352 / target 1354 unchanged
after it stopped while Held frames continued advancing. The walkthrough also
verified the fixes that preserve prediction status across ordinary editor
redraws and advance Director channel hold counters once per control frame.

This implements a constant-velocity model, not a constant-acceleration model.
Sudden reversals and discontinuous Euler representations can overshoot; lens
encoders are not predicted. Physical tracking, LED processor delay, camera
exposure timing and multi-machine wall qualification remain stage checks.
