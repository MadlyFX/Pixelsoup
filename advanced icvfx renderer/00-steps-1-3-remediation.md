# 00 — Steps 01–03 remediation

**Work list. Read [01](01-camera-tracking-and-inner-outer-frustum.md),
[02](02-lens-model.md) and [03](03-genlock-and-present-barrier.md) first; this
says what those three steps actually delivered and what is left.**

Assessed 2026-09-01 against the tree at that date, with
`T:\UE\UnrealEngine\Engine\Plugins\Runtime\nDisplay` as the reference. Every
finding below was opened and read in source rather than inferred from the status
document. Where a claim was put to an adversarial check and did not survive, it
is recorded in [Not defects](#not-defects) rather than deleted, so nobody spends
a day re-finding it.

## Changes since assessment

**2026-09-02 — items 1.1, 2.1 and 2.2 are done.** The plate now reaches the wall
on the shipping path, and it is rendered as a real view of that wall.

- `SdlPresenter::present(frame, graph, maps)` and its transition sibling
  composite the plate, so the overload the node output loop actually calls no
  longer skips it. A transition plates each side over its own wall image before
  the mix, which needs a second `IcvfxStage` — added beside `scene_to` and
  `mesh_warp_to`, and lazily allocating its inner renderer exactly as the first
  one does. Both stages are now released before the device in the two teardown
  paths, which the first one was never doing.
- `inner_frustum_view` builds the plate with
  `output::perspective_view_for(surface, plan.inner_rect, tracked_eye, …)` — the
  same call the outer frustum uses — instead of narrowing the tracked camera's
  own frustum by the rectangle's fraction of the scene canvas. It returns
  `std::optional`, so a surface that refuses the eye drops that camera from the
  stack rather than rendering an unprojected plate, and the compiler forces
  every caller to handle it.
- The space contract is settled on **surface raster** throughout. Both headers
  say so, `plan_inner_frustum` takes the `PhysicalSurface` and gates containment
  against `surface.raster`, and `IcvfxStage::ensure_inner` sizes the inner target
  from the raster rather than `frame.canvas`.
- The lens no longer builds the plate's projection. It still moves the eye to
  the entrance pupil and still sizes the rectangle; the wall builds the frustum.

Two tests in `icvfx_tests.cpp` asserted the old construction against itself and
were replaced rather than patched, because their premise was the defect:

- `the plate registers with the wall from a displaced eye` — every wall pixel
  the plate covers must come back to itself through the plate's own view, with
  the tracked eye pushed well off the authored axis. Also covers the refusal
  when the eye is behind the wall.
- `a calibrated lens moves the inner view eye to the entrance pupil` — the eye
  moves along the optical axis by the calibrated offset and by nothing else, and
  the matrices do not change when the camera's own field of view does.
- `the compositor graph path composites the inner frustum` in
  `sdl_presenter_tests.cpp` — turning the show-wide switch on changes the
  presented pixels, and turning it off restores them.

Both replacements were verified to fail against the previous construction before
being kept. Both presets build clean and all 134 CTest entries pass. Note that
the presenter test needs a real window, so it executes under a normal run and
skips under ctest's `SDL_VIDEO_DRIVER=dummy`, like the other GPU-window tests in
that suite; it was run and passed on this machine's adapter in both configs.

### Live run, 2026-09-02

`run-editor-demo.ps1` with a Director editor and one lifecycle-supervised output
node, driven by `send-freed-tracking.ps1 -Port 40000 -Motion Orbit -Rate 60`
against a project already carrying a tracked camera, a FreeD feed and an LED
surface. What the run establishes:

- The tracker path is live and clean. Link went `WAITING` to `RECEIVING`;
  **4,741 received, 4,741 accepted, zero refusals** across four minutes of
  60 Hz datagrams, **0 held frames**. The generator's datagrams are accepted by
  the real parser for the real reasons.
- The Director replicates. **5,901 poses sent** against 4,741 datagrams —
  more sent than received, because a control frame that brings no new datagram
  re-stamps the held pose and the channel reports that as `Accepted`. That is
  item 3.2 visible from the outside.
- The plate is derived per frame from the live pose and tracks the orbit:
  `1920 x 1080 at 0,0` with **wall covered 100.0 %** at one end of the orbit,
  `89 x 73 at 1057,545` with **0.3 %** at the other. Pose, field of view and
  tracker sequence all read out live.
- Corroborates item 9.2. At 100 % the rectangle is the clamped whole-raster
  fallback, and the panel says only "Wall covered 100.0 %" — it never says
  *clamped*, so the operator cannot tell a camera that frames the wall from one
  that has swung off the end of it.

What the run does **not** establish: composited plate pixels. This demo's LED
surface faces empty scene, so its *outer* frustum renders black as well — inner
and outer are indistinguishable by eye whatever the compositor does. Two
attempts to add a primitive behind the wall from the Library (double-click and
drag-to-viewport) added nothing, which is worth its own look; making the wall
face existing geometry needs real scene authoring beyond the scope of this
check.

So item 1.1's "Done when" is met for the tracking and plate-derivation half and
still open for the pixel half. The pixel half is covered by
`the compositor graph path composites the inner frustum`, which runs against a
real GPU window and was shown to fail against the previous code — but it has not
been seen on a wall.

### Second live run, 2026-09-02, with scene geometry

Re-run against the same project after a box, a sphere, a sun and a lit floor
were added, so the wall finally had something to show. **The plate still does not
reach the wall, and the reason is neither of the two fixed above.**

Node output was pixel-identical across repeated samples while the Director's
readout swung from `1920 x 1080 at 0,0` / 100 % of wall to
`None: the camera sees no part of the canvas` and back. A 100 % plate would have
replaced the entire wall image with the tracked viewpoint; nothing changed.

Running the node directly with its stderr captured named the cause on the first
frame it declined:

```
inner frustum: no plate on this output - the tracked frustum meets no part of this wall
```

Everything upstream of that is working: the ICVFX stage was created, the node had
a valid pose for its tracked camera, the show switch was on, and the node's map
*was* bound to a planar surface. What fails is
`output::inner_frustum_region(map.resolved_surface->geometry, …)` returning no
region on the node while the Director reports a plate for the same pose on the
same frame.

- [x] **11.1 — `blocker` — Director and node disagree about the wall** *(fixed 2026-09-02)*

**Where.** `director_inner_frustum_rect` (`apps/server/playback.cpp:1828-1848`)
walks **every** authored `feed_map.surfaces` entry and reports the first planar
one that yields a valid region, using the *document* `surface.geometry`.
`record_inner_frustum` (`engine/ui/src/sdl_presenter.cpp`) instead uses **this
output's own** `map.resolved_surface->geometry` — the *evaluated* surface. The
two are different objects and can disagree, and in this project they do: the
Director reports a full-wall plate at the moment the node finds no intersection
at all.

**Why it matters.** The Live Data panel is the operator's only window onto the
plate, and it is reporting a plate that no wall is drawing. That is worse than
reporting nothing. It is also the confirmed, runtime form of the "Live Data's
plate row and the wall can disagree" item under section 9.

**Do.** Make the panel report what the node's own map resolves, or make both
derive from one function over one surface. Until then the readout is not
evidence.

**Note.** This is pre-existing, not introduced by items 1 or 2: the
`output::inner_frustum_region` call in `record_inner_frustum` is unchanged by
that work. Items 1, 2.1 and 2.2 are confirmed in place by this run — the stage
exists, the guards pass, the pose arrives.

### Diagnostics added while chasing this

`record_inner_frustum` now says, once per reason per process, why an output is
drawing no plate: no stage, show switch off, no tracked camera, not mapped to a
planar surface, no pose on this node, or the frustum missing the wall. Presenter
setup likewise reports a stage it could not build. Without these the failure is
indistinguishable from a show with no tracking, which is exactly what section 9
says an operator must not be left guessing at. They are what located 11.1.

One regression from the first pass was found and fixed here too: the two ICVFX
stages were created all-or-nothing, so a device that could not afford the second
(transition) stage would have lost **every** plate on **every** wall rather than
just its transition plates. They are now created independently.

### 11.1, and what it turned out to be

The diagnosis in 11.1 above was right about the symptom and wrong about the
cause. Both sides do use the same surface — instrumenting each end showed
`surfaces=1`, both choosing node `100028`, identical centre and raster, and
`resolved_surface->geometry` is a verbatim copy of `surface.geometry`
(`engine/output/src/feed_map_model.cpp:571`). They were never looking at
different walls.

The real fault was one level down, in `output::inner_frustum_region`. It had a
single conservative answer — the whole raster, marked clamped — for every case
where the frustum "is not bounded by the wall plane", and it took that path the
moment **any** corner ray failed to reach the plane going forward. That conflates
two unrelated situations:

- a frustum that **faces** the wall and overshoots its edges, where covering the
  whole raster is right and deliberately wasteful; and
- a camera that **cannot see the wall at all** — behind it, coplanar with it, or
  turned away — where the whole raster is not conservative but simply wrong. It
  puts a full-wall plate up in place of the outer frustum that is lighting the
  set, and it reports "Wall covered 100.0 %" to the operator at exactly the
  moment the camera is pointing somewhere else.

That second case is what the Live Data panel was showing, and it is why the
readout and the wall disagreed: the Director hit the clamp and called it a
plate, while the node's rectangle came back empty.

**Fixed** by separating the two. The eye must now be in front of the authored
face — the same test `perspective_view_for` already applies, and for the same
reason — or there is no plate. Corner rays that miss are counted rather than
short-circuiting: all four missing is a camera facing away and draws nothing,
while some missing is a genuine straddle and still clamps outward to the whole
raster. Four cases in
`engine/output/tests/perspective_mapping_tests.cpp` pin it: straddle clamps,
facing-away draws nothing, behind draws nothing, coplanar draws nothing.

This changes a deliberate behaviour — the old comment read "wasteful, never
wrong" — so it is worth being explicit that "never wrong" did not hold. A
full-wall plate for a camera that cannot see the wall replaces the outer
frustum every other camera and every crew member on the floor is lit by.

Confirmed live afterwards: the Director's readout now reports
`None: the camera sees no part of the canvas` where it previously claimed a
full-wall plate.

### Still open after 11.1

The plate has still not been seen on the wall, and the run that fixed 11.1
surfaced two separate reasons, both already on this list:

- **The node's pose stalls.** Under a sustained feed the node's pose froze —
  `seq=3138` identical at control frames 2830 and 2860, `held=1` — while the
  Director had moved on to frame 3831 with a completely different eye. The node
  kept rendering a stale pose whose frustum missed the wall. That is item 8.3
  (the per-session pose mailbox coalesces silently and uncounted) showing up as
  a stuck plate, and item 8.4 is why nothing reported it.
- **The tracker's orientation is not calibrated to the stage.** FreeD pan is
  applied directly as scene yaw, so the generator's "looking inward" orbit does
  not aim at the authored wall. Even the Director's own answer never became a
  bounded rectangle — only the clamped whole-raster one. That is item 5.1.

Neither is a defect in the inner-frustum path itself. Items 1, 2 and 11.1 are
confirmed in place: the stage exists, the guards pass, the pose arrives, and the
geometry answer is now honest.

## State at assessment

Both GUI presets build clean and all 134 CTest entries pass. Steps 01–03 amount
to roughly 4,350 lines of implementation against 3,670 lines of tests, and the
quality of the parts is high: the FreeD D1 parser is protocol-correct,
`engine/track` structurally cannot read a tracker, the lens model is a real
Brown–Conrady implementation with a bounded zoom/focus table, and the NVAPI
present-barrier backend is genuinely driver-backed.

None of it reaches a wall. `record_inner_frustum` has one call site and the node
output loop does not go through it. Everything else in this document is
therefore latent today — which is also the reason the order below matters more
than usual, because item 1 is what makes items 2 onward observable at all.

Two of the three "Definition of done" statements in the step documents are not
met. Step 03's is met apart from the Director's own clock.

## How to work through this

Items are ordered so each one makes the next verifiable on a wall rather than
only in a test. Each carries where the defect is, what to do, and what proves it
is done. Severity is `blocker` (stops a shoot), `major` (schedule it), or
`minor`.

---

## 1. Connect the plate to the graph-aware presenter

- [x] **1.1 — `blocker` — The inner frustum is never composited in production** *(fixed 2026-09-02)*

**Where.** `record_inner_frustum` is defined at
`engine/ui/src/sdl_presenter.cpp:225` and called from exactly one site,
`sdl_presenter.cpp:1380`, inside the graph-less overload
`SdlPresenter::present(frame, maps)` (`:1341`). The node output loop calls the
graph-aware overload `present(frame, graph, maps)` (`:1409`) at
`apps/server/playback.cpp:7493`, or its transition sibling at `:7488`. Neither
composites a plate. Because `SdlPresenter` overrides the graph+maps signature
directly (`sdl_presenter.hpp:143-146`), the base-class forwarding chain in
`presenter.hpp:377-382` that would otherwise have reached the plate path is
bypassed. The only callers of the plate-compositing overload in the whole tree
are `engine/ui/tests`.

**Do.** Give the graph-aware overload and its transition sibling the same
`record_inner_frustum` step the graph-less one has, between the scene record and
the display transform. Decide deliberately what a transition does with two
different tracked stacks rather than letting it fall out of the code.

**Prove.** A test that drives the graph-aware overload and asserts the returned
texture differs from the scene target when a tracked camera is drawing. The
existing `icvfx_tests.cpp` end-to-end case exercises the wrong entry point.

**Done when.** `scripts/send-freed-tracking.ps1` against a running node puts a
visible plate on the output, and the Live Data panel's plate rectangle matches
what is on screen.

---

## 2. Rebuild the inner view as an off-axis frustum through the wall

- [x] **2.1 — `blocker` — The plate is not a view of the wall** *(fixed 2026-09-02)*

**Where.** `engine/render/src/icvfx.cpp:194-197`. The inner view is built as
`scene::view_for_slice(tracked, scene.canvas, plan.inner_rect)`, which resolves
to the tracked camera's own lens frustum sub-windowed by the rectangle's
*fraction of the canvas* (`engine/scene/src/canvas.cpp:649-658`, and
`slice_of_window` at `:555-584`). The outer frustum for the same output is built
as a true off-axis perspective through the physical wall region:
`output::perspective_view_for(surface.geometry, map.source, eye, near, far)` at
`engine/render/src/mapped_scene.cpp:705-709`.

The inner view never touches `surface.geometry`. No caller anywhere passes a
tracked eye to `perspective_view_for` — grep it. The two constructions agree
only in the degenerate case where the tracked camera's frustum exactly covers
the whole wall.

**Why it matters.** The rectangle is found correctly, by projecting the tracked
frustum onto the wall. What is drawn into it is not. This is the parallax
mechanism ICVFX exists to produce, so connecting item 1 without this one puts a
visibly wrong plate on the wall.

**Do.** Build the inner view the same way the outer one is built, from the
tracked eye through the wall rectangle:
`perspective_view_for(surface.geometry, inner_rect, tracked_eye, near, far)`.
Keep `history_identity` stable as it already is.

**Prove.** A test with the tracked camera displaced from the authored camera,
asserting that a world point on the wall projects to the same canvas pixel in
the inner view and the outer view. That is the registration property, and no
current test asserts it — `icvfx_tests.cpp` asserts `inner_frustum_view ==
view_for_slice(...)`, which is the implementation restating itself.

- [x] **2.2 — `major` — Settle the canvas-versus-surface-raster contract** *(fixed 2026-09-02)*

**Where.** The headers disagree. `render/icvfx.hpp:43` documents `inner_rect` as
"absolute canvas pixels"; `render/icvfx_stage.hpp:68-73` says slice and regions
are "canvas pixels for a canvas-slice output, surface raster pixels for a mapped
one". `record_inner_frustum` supplies surface-raster values
(`output::inner_frustum_region` clamps to `surface.raster`,
`perspective_mapping.cpp:357` and `:417`; `map.source` is surface-raster per
`feed_map.hpp:107-110`), while `plan_inner_frustum` gates on
`scene::contains(scene.canvas, region.rect)` at `icvfx.cpp:133` and
`IcvfxStage::ensure_inner` sizes from `frame.canvas` at `icvfx_stage.cpp:41-45`.

The composite itself is self-consistent — `uniform_for` pairs `slice` with
`inner_rect` (`icvfx.cpp:72-101`) and `plan.covered` is their intersection
(`:146`) — so placement and the skip decision are correct. The activation gate
and the render view are not.

A surface raster is per surface, document-authored with its own 1920×1080
default (`feed_map_model.cpp:342-351`) and editable to 131072
(`mapping_panels.cpp:3840-3858`), against one scene canvas. With up to 256
surfaces at most one wall can match, so this bites on any multi-wall show.

**Do.** Pick one space, say so in both headers, and convert at the boundary.
Item 2.1 forces this decision anyway.

**Prove.** A test where surface raster and scene canvas differ. Every current
test sets them equal (`icvfx_tests.cpp:52-62`, `:349-363`).

---

## 3. Make latency compensation real

- [x] **3.1 — `blocker` — The compensated frame never reaches the wire** *(fixed 2026-09-02)*

**Where.** `server::stamped_tracker_sample` correctly returns
`sampled_control_frame + latency_frames`
(`apps/server/tracked_camera_bridge.cpp:18-28`). The Director then sets
`replicated.control_frame = input.control_frame.value()` — the raw,
uncompensated frame — at `apps/server/playback.cpp:10991`, and pushes only
`stamped->sample` at `:11013`, which carries no frame of its own. The
compensated value is used solely for the Director's own channel.

The wire field's own comment at
`engine/net/include/mediaserver/net/session_protocol.hpp:250-252` reads "The
control frame these poses are for, after latency compensation." The code
contradicts it.

**Consequence.** Compensation has zero effect on any render node. On
`--role=director,render` the Director labels the same pose `latency_frames`
ahead of its neighbours, so `TrackedCamera::control_frame` and therefore
`scene::digest` diverge — the exact cross-node agreement the design exists to
guarantee.

**Do.** Send `stamped->control_frame`.

- [x] **3.2 — `blocker` — A pose stamped forward is applied immediately** *(fixed 2026-09-02)*

**Where.** `engine/track/src/pose_channel.cpp:28-37`. `PoseChannel` holds one
`held_pose_`, and `peek(show_frame)` returns it unconditionally, marking `Exact`
only when the stamps happen to match. Nothing holds a pose until its frame
arrives, so stamping forward and reading at the current frame cancels exactly.

A second consequence follows from the default `latency_frames = 2`
(`scene.hpp:1129`): the stamp can never equal the frame asked for, so `peek`
never returns `Exact`, `held` is permanently true on a healthy feed, the hold
counters climb forever, and the viewport's "dimmer while a pose is held" frustum
is dim always.

**Do.** Give the channel a small ring — `max_icvfx_latency_frames` is 16, so a
17-slot fixed array — and have `peek` return the pose stamped for the frame
asked for, holding the newest older one when that slot is empty. Keep it
allocation-free.

**Prove.** Fixing 3.1 without 3.2, or the reverse, leaves compensation inert.
Test both together: a channel fed poses stamped N+2 must return the N−2 sample
at frame N, and two channels fed the same stream must agree at every frame.

- [ ] **3.3 — `major` — Mid-show compensation changes stall replication**

**Where.** `apps/server/playback.cpp:11013-11018`. Only samples whose local
`accept()` returned `Accepted` go on the wire. Lowering `latency_frames`, or a
Program swap between differently-authored scenes, makes the newly stamped frame
older than the held one, which `PoseChannel::accept` classifies as `Stale`
(`pose_channel.cpp:17-21`), so nothing is replicated until the show frame
catches up. `PoseChannel::reset()` exists for exactly this and is never called
in production.

**Do.** Reset the Director's channels on a compensation change or scene swap.

---

## 4. Stop polling the tracker socket

- [x] *(fixed 2026-09-02)* **4.1 — `blocker` — The receive loop sleeps 100 ms whenever the socket is
  empty**

**Where.** `apps/server/tracker_listener.cpp:32` defines
`receive_poll{100}` ms; the worker at `:207-231` uses a non-blocking
`receive_from` and, on `would_block`, sleeps for it.

At any real tracker rate the socket empties between datagrams, so the thread
sleeps, six or more datagrams queue in the kernel buffer, and the loop drains
them back-to-back before sleeping again. Pose updates reach the slot in ~100 ms
bursts — roughly 10 Hz effective against a 24–60 Hz shoot — and the Director
re-samples the same stale pose for most frames.

The resulting 0–100 ms jitter is not a fixed offset, so the authored
integer-frame compensation cannot correct it even once item 3 lands.

**Do.** Block on the socket with a short timeout, or use a cancellable wait, so
a datagram wakes the thread. Keep `stop()` bounded.

**Prove.** Feed the listener at 60 Hz and assert the slot's sequence advances
once per datagram within a frame period, rather than in bursts.

- [ ] **4.2 — `major` — `configure` joins the receive thread on the frame
  boundary**

**Where.** `TrackerListenerPool::configure(scene_model.tracking_sources())` runs
unconditionally on the Director's frame boundary
(`apps/server/playback.cpp:10985`) and joins the sleeping worker whenever any
authored field changes — including sensor height, which is an inspector drag
row. With 4.1 unfixed that join waits up to 100 ms on the frame boundary.

**Do.** Only reconfigure a feed whose socket-affecting fields actually changed;
sensor height and focal length do not need the socket touched at all.

- [ ] **4.3 — `major` — No live rate or jitter per feed**

The panel shows cumulative Received/Accepted only, so a 6 Hz feed and a 60 Hz
feed look identical after the first second. Nothing in the listener measures an
interval. Add a rate and a jitter figure — this is the readout that makes 4.1
self-evident on a stage.

---

## 5. Add the tracker-to-stage transform

- [x] **5.1 — `blocker` — There is no tracker-space to stage-space calibration** *(fixed 2026-09-02)*

**Where.** `engine/scene/src/scene.cpp:1008-1024`. `tracked_view_camera` assigns
`result.eye = tracked.position` and feeds the tracker's pan/tilt/roll straight
into the scene's own Euler order. `TrackingSourceSettings`
(`scene.hpp:956-977`) carries protocol, port, camera id, sensor height, aspect
and fallback focal length — and no origin offset, axis remap, handedness flip,
or scale.

FreeD hardware is commonly Z-up while the scene assumes Y-up. Without a remap
and an origin transform you cannot align a tracking volume to a stage origin,
which is a first-day bring-up task on every install. nDisplay gets this free
from the level's transform hierarchy.

**Do.** Add the transform to `TrackingSourceSettings`, clamped once at the model
boundary and digested like every other authored value. Apply it in
`tracked_view_camera` before the pupil offset.

- [ ] **5.2 — `major` — No per-camera frustum nudge**

nDisplay puts `FrustumRotation` and `FrustumOffset` on
`FDisplayClusterConfigurationICVFX_CameraSettings` precisely because tracking
always lands with a small systematic offset that gets dialled out at bring-up.
5.1 fixes the feed's frame of reference; this is the per-camera trim on top.

---

## 6. Close the lens loop

- [x] *(fixed 2026-09-02)* **6.1 — `major` — Two unrelated lenses decide the rectangle and the
  render**

**Where.** The rectangle uses `TrackedCamera::field_of_view`, which the Director
computes from the *feed's* single-point sensor height and fallback focal length
(`apps/server/tracker_listener.cpp:139-140` into
`scene::tracked_field_of_view`, `scene.cpp:1056-1071`). The render uses the
node's full `LensProfile` and encoders (`icvfx.cpp:194-197`). On FreeD the
feed's angle is frozen at the fallback focal length and does not follow the zoom
encoder at all.

`scene.hpp:965-969` states the Director's angle is deliberately authoritative
"rather than re-deriving it from a lens table each might hold differently" — so
the node-side path contradicts the stated design.

**Do.** Pick one and delete the other. If the Director's angle wins, it must be
computed from the camera's `LensProfile`, not the feed's two numbers.

- [x] *(fixed 2026-09-02)* **6.2 — `major` — Distortion-aware sizing is dead outside tests**

Both production call sites — `engine/ui/src/sdl_presenter.cpp:249` and
`apps/server/playback.cpp:1840` — use the non-lens `inner_frustum_region`
overload. The lens-aware overloads, which apply `distorted_lens_extent` exactly
as [02](02-lens-model.md) requires, are called only from `engine/output/tests`
and `engine/render/tests`. Authored Brown–Conrady coefficients therefore never
widen the plate, and a distorted lens can see outer-frustum content at the frame
edge.

- [ ] **6.3 — `major` — `lens.distortion` is discarded at the parser**

The OpenTrackIO parser reads `translation`, `rotation`, `focalLength`,
`focusDistance`, `entrancePupilOffset`, `fStop` and `sequenceNumber`. The entire
`lens.distortion` block, `distortionOverscan` and `projectionOffset` are
discarded — so a source streaming live distortion cannot drive the model that
already exists. `entrancePupilOffset` is parsed and then dropped too: it lands
in `OpenTrackIOSample` and nothing reads it.

- [ ] **6.4 — `major` — The calibration table has no authoring surface**

`LensProfile` carries sixteen zoom/focus calibration points. The LENS tab
renders a read-only "Imported table: N points" label, and there is no importer —
no ST map reader, no vendor calibration file, and the OpenTrackIO `static` lens
block is discarded. Nothing in the product can populate the table.
[02](02-lens-model.md) flagged this as an open question; it is now the thing
standing between a real lens and the model that would use it.

- [ ] **6.5 — `minor` — Live encoder values are never surfaced**

[02](02-lens-model.md) says encoder values "are status, not controls, and belong
wherever the tracking status reads out". `LensEncoders` appears nowhere in the
editor.

- [ ] **6.6 — `minor` — The LENS tab authors the wrong camera**

`ProjectSettingsState` is built from `stage_scene_frame.camera.lens_profile` and
`.camera_source` — the authored outer viewpoint, and whichever camera the scene
resolved first, not the selected one. The panel does not say which camera it is
editing.

---

## 7. Give the Director's clock a timing authority

- [x] **7.1 — `major` — The Director's show clock still free-runs**

**Where.** `run_director_show` creates a plain `show::ShowClock` pacer with no
timing authority (`apps/server/playback.cpp:7842-7846`), under every policy
including `ExternalGenlock`. `TimingAuthority::VideoReference` is implemented
and wired on the *node* frame-boundary driver (`:6936-7001`, `:7636-7642`).

The control frame is the replication clock the whole cohort is keyed to, so the
cohort's cadence is an OS clock while node presents are barrier-locked to house
reference. [03](03-genlock-and-present-barrier.md) calls this out by name as the
half people forget; it is half-done.

- [ ] **7.2 — `major` — No software sync policy**

`PresentBarrierState::Unsupported` feeds `barrier_incomplete` and fails
readiness (`engine/sync/src/sync_readiness.cpp:161-165`). There is exactly one
sync implementation and no software fallback, so a non-NVIDIA stage has no
locked path at all.

nDisplay ships five policies. `Ethernet` and `EthernetBarrier` align presents on
any GPU with a two-barrier plus VBlank-timestamp procedure and a computed sleep
— see
`DisplayCluster/Private/Render/Synchronization/DisplayClusterRenderSyncPolicyEthernet.h:33-42`
for the step sequence, and `DisplayClusterRenderSyncHelper.h` for the
`WaitForVBlank` / `SetMaximumFrameLatency` seam it needs. The RHI already calls
`SetMaximumFrameLatency` and `GetFrameLatencyWaitableObject`
(`engine/rhi/src/d3d12_backend.cpp:1242-1248`), so the primitives exist.

- [ ] **7.3 — `major` — No startup-ramp tolerance on the sync counters**

`presents_in_sync < presents` is compared against cumulative counters with zero
tolerance in three places, so a healthy join during ramp-up is indistinguishable
from a real loss. Compare deltas over a window instead.

- [ ] **7.4 — `minor` — `DXGI_PRESENT_ALLOW_TEARING` on the barrier present**

`engine/rhi/src/d3d12_backend.cpp:2374-2394`. Confirm this is intended when a
barrier is joined; tearing and a present barrier are contradictory requests.

---

## 8. Protocol and wire correctness

- [x] *(fixed 2026-09-02)* **8.1 — `blocker` — OpenTrackIO with a non-zero camera id drops
  everything**

The OpenTrackIO parser never populates `TrackedPose::camera_id` — the string
does not occur in `engine/track/src/opentrackio.cpp`, so the field keeps its
zero default (`tracked_pose.hpp:83`). The listener rejects any datagram whose
`camera_id` does not match the authored value
(`apps/server/tracker_listener.cpp:130-134`).

Authoring any camera id other than 0 on an OpenTrackIO feed gives a 100 %
refusal rate with the link showing as bound. The multi-camera-on-one-port demux
the FreeD path supports cannot work for OpenTrackIO at all.

**Do.** Parse `sourceId` / `sourceNumber` into `camera_id`, or refuse to author
a non-zero camera id on an OpenTrackIO feed. Do not leave it silently dropping.

- [ ] **8.2 — `major` — The transform chain is never composed**

`engine/track/src/opentrackio.cpp:701-717` selects a single transform entry and
ignores parents, so a multi-link chain (crane base → head → camera) silently
produces a wrong world pose. `opentrackio.hpp:29-35` says such samples "should
be refused upstream", but no upstream refusal was implemented. Either compose
the chain or actually refuse it.

- [x] **8.3 — `major` — The per-session pose mailbox coalesces silently** *(fixed 2026-09-02)*

`publish_tracked_pose` writes into a one-slot per-session mailbox and overwrites
whatever was pending, with no check and no counter
(`engine/net/src/director_frame_barrier.cpp:61-69`, `:1102-1114`). Session
workers drain independently and park on a 25 ms wait when their inflight window
is full (`director_session_service.cpp:943-953`) — longer than a 60 Hz control
period. The loss is undetectable on the node because the wire sequence is
assigned at send time and stays gap-free.

**Do.** At minimum count the overwrite and surface it. Better: let the mailbox
hold the small ring item 3.2 introduces.

- [x] **8.4 — `major` — A node reports nothing about tracking**

There is no tracking field in the node status line and no node→Director tracking
report, so the Director cannot tell whether a node is holding a pose, missed
one, or skipped its inner render. `render::IcvfxStatistics::inner_frames_skipped`
is incremented and never read outside `engine/render`.

- [ ] **8.5 — `minor` — Timing beyond `sequenceNumber` is discarded**

`timing.mode`, `sampleRate`, `sampleTimestamp`, `recordedTimestamp`, `timecode`
and `synchronization` are all dropped, so there is no timecode or PTP alignment
of tracking samples to the camera shutter. CBOR, the spec's other encoding, is
absent entirely.

---

## 9. Operator surface

- [x] *(fixed 2026-09-02)* **9.1 — `major` — The Live Data panel cannot scroll**

`live_data_panel.cpp` declares a `scroll_` member (`live_data_panel.hpp:186`)
that is never read; `draw()` clips to the body and walks *y* downward
unconditionally. A fully-populated camera occupies 404 px against roughly 628 px
of body at the default 1600×900 layout, so a second camera is clipped mid-way
and a third is never drawn. On a multi-camera volume the operator cannot reach
the later cameras' diagnostics at all.

- [x] *(fixed 2026-09-02)* **9.2 — `major` — The clamped flag is computed and dropped**

`InnerFrustumRegion::clamped` is carried onto `IcvfxPlan` and then read by
nothing. A frustum the wall does not bound returns the whole raster marked
clamped — the operator should be told, because it costs the entire wall's
pixels.

- [x] *(fixed 2026-09-02)* **9.3 — `major` — Barrier diagnosis collapses three faults into one**

An `Unsupported` barrier, a node that joined a different group, and a node that
left the barrier mid-show all surface the same way. `PresentBarrierStatus::Left`
exists precisely to be distinguishable.

- [ ] **9.4 — `major` — Feeds cannot be renamed or deleted**

Every feed is created with the literal name "Tracker", appears in no outliner,
and the list disambiguates only by port.

- [ ] **9.5 — `minor` — Plate authoring is numbers-only**

`viewport_panel.cpp:369-420` already draws the plate and its feather; the
inspector authors overscan and softness as numeric fields beside it. The repo
README's "direct manipulation before controls" points at dragging the plate
edge.

- [ ] **9.6 — `minor` — No pose record and playback**

Nothing captures the replicated pose stream or replays it, so the only offline
exercise is the synthetic PowerShell FreeD generator. A recorded take from a
real tracker is what makes items 2, 3 and 5 testable without a stage.

---

## 10. Correct the status document

- [ ] **10.1 — `minor`** — `advanced-renderer-status.md:574` reads "No tracker
  is read and no inner frustum reaches a wall." The tracker half is stale — the
  Director binds sockets (`tracker_listener`) and samples poses at its frame
  boundary. The inner-frustum half is accurate and should stay until item 1
  lands. Split the sentence.

- [ ] **10.2 — `minor`** — The camera tracking section claims a node "can prove
  no pose was dropped" and that compensation is "applied once, never
  machine-local". Items 3.1 and 8.3 contradict both. Correct them or mark them
  as intended-not-yet-true.

---

## Not defects

Each of these was suspected, investigated, and does **not** hold. Recorded so
the investigation is not repeated.

- **A reception fact in `scene::digest`.** `scene.cpp:589` digests
  `TrackedCamera::held`, but that bit is exactly
  `pose.control_frame != input.control_frame`, both of which are replicated. It
  is redundant, not divergence-inducing, and cross-node digest equality across a
  hold run is asserted at `tracked_camera_bridge_tests.cpp:226`.

- **No cross-node phase agreement on the video reference.** The reference
  counter supplies cadence only. `last_reference_frame_` is a rolling previous
  sample, not a latched epoch, so the initial value cancels out of every delta.
  The presented show frame is derived from the replicated transport anchor at
  the Director-approved control frame, so two nodes on the same control frame
  present the same show frame by construction.

- **NVAPI called with the wrong device type.** `NvAPI_D3D1x_QueryFrameCount`
  (`0x9152e055`) is declared by the SDK header for D3D12 devices as well as
  10/11; "D3D1x" names the API family, not a device restriction. The call at
  `nvapi_present_barrier.cpp:375-390` is correct.

- **The inner render target reallocating as the camera moves.**
  `inner_target_extent` (`icvfx_stage.cpp:11-45`) sizes from canvas × authored
  density and explicitly not from the moving rectangle. The comment says so and
  the code matches.

- **Poses resolved against a per-node frame breaking agreement.** Output nodes
  do resolve against the playback buffer's trailing cursor rather than the live
  control frame, but since `peek` returns the single held pose regardless of the
  frame asked for and the scene records `answer.stamped_frame`, nodes at
  different cursors still apply the same pose. Worth tidying after item 3.2
  lands; not a correctness defect today.

---

## Backlog — per-camera authoring, against nDisplay

Not required to shoot, but this is the gap between "a plate on a wall" and what
a stage expects. Source:
`DisplayClusterConfiguration/Public/DisplayClusterConfigurationTypes_ICVFX.h`.

- [ ] Asymmetric overscan — left/right/top/bottom independently, in percent or
      pixels, against the single `margin_px` here. A whip-pan needs overscan on
      the leading edge only.
- [ ] Overscan FOV multiplier — nDisplay's stated purpose is a buffer against
      tracking latency during camera movement.
- [ ] Soft edge split into horizontal, vertical and feather. A wall is wider
      than it is tall and the side edges travel further.
- [ ] Inner-frustum border — how a DP and gaffer line up a shot against the
      plate.
- [ ] Chromakey and chromakey markers.
- [ ] Light cards, over and under the plate stack, and UV light cards.
- [ ] Per-camera hide lists, and plates hidden from named viewports.
- [ ] Camera motion blur and depth of field on the plate. A pin-sharp plate
      behind a shallow-focus foreground reads immediately as a screen.
- [ ] Per-camera OCIO and per-node colour grading.
- [ ] An authored overlap policy for stacked plates. Two overlapping plates
      currently cross-fade into each other with no policy —
      `EDisplayClusterShaderParametersICVFX_CameraOverlappingRenderMode` is the
      reference.
- [ ] Curved and warp-mesh inner frustum. The conservative mesh bound already
      exists in `output::inner_frustum_region(mesh, ...)` with no caller;
      nDisplay's `DisplayClusterWarpBlendMath_Frustum` fits per-vertex
      projection angles with selectable strategies (`DynamicAABBCenter`,
      `StaticSurfaceNormal`, `StaticSurfacePlane`) and an auto-select.
- [ ] Report the resolved inner-frustum resolution and the pixel cost of
      overscan. The 16384 clamp in `inner_extent` is applied per axis and
      silently, so a large canvas at high density loses authored density and
      gains anisotropy with no counter.
- [ ] Other tracker ecosystems — Mo-Sys F4, Vicon, OptiTrack/NatNet, stYpe,
      Ncam — and transports beyond IPv4 unicast UDP. Multicast matters because
      it is OpenTrackIO's reference transport; serial matters because much
      installed FreeD hardware ships it.

---

## Definition of done for steps 01–03

A tracked camera moving on the volume produces correct parallax on the wall;
every node in a Program cohort derives the same inner-frustum rectangle *and the
same pose* for the same show frame; a node with no pose holds rather than
guessing; the plate's field of view matches the rectangle it lands in; the
cohort's clock as well as its presents are driven by the video reference; and
the two-node composite is byte-exact against the full-canvas render.

Items 1 through 7 are what stands between the tree and that sentence.


### 8.3, and the two silent drops behind it

Fixed, and the fix immediately found a real loss that nothing could previously
see.

The mailbox overwrite itself is the right behaviour and was left alone: a
session that fell behind wants the current pose, not a backlog of stale ones.
What was wrong is that the loss was invisible from both ends. The Director never
looked at what it was replacing, and the node could not tell either, because the
only ordinal on the wire is `sequence` — the Director's *outbound* stream, which
is assigned when a message is sent. A pose coalesced away before it was ever
sent consumes no outbound sequence, so the stream a node receives stays
perfectly gap-free however many were dropped.

- **`pose_sequence` on `TrackedCameraPose`.** A second ordinal, assigned once by
  the barrier when a pose is *published* to every session rather than when it is
  sent. A node comparing it against the last it saw can now tell that it never
  got one. Schema, codec, both session ends.
- **The Director counts the overwrite.** `publish_tracked_pose` increments
  `tracked_poses_coalesced` whenever it replaces an undrained pose, exposed on
  the barrier and reported in the Director status line as `poses-coalesced=`.
- **The node counts the gap.** It tracks the last `pose_sequence` and reports
  `pose-seq=` and `poses-missed=` in its status line — which is also the first
  thing a node has ever said about tracking at all, so it takes the edge off
  item 8.4.

`a coalesced pose is counted and leaves a visible gap` in
`engine/net/tests/director_frame_barrier_tests.cpp` pins it: three poses
published across one undrained session count two coalesced, the session receives
the newest, and the ordinal jumps 1 to 4 so the hole is visible. The frozen wire
golden in `session_codec_tests.cpp` was updated, with `pose_sequence` set
deliberately unequal to `sequence` so the two cannot be crossed unnoticed.

**Two drops were found wiring this up, both silent.** `pose_sequence` was
dropped on the way out in `DirectorResumeSession::send_tracked_camera_pose` and
again on the way in at the node's decode, because both rebuild the frame field
by field. The first live run showed `pose-seq=0` on a node the Director had sent
1,677 poses to, which is exactly the class of thing the whole item exists to
make visible — and is a warning about every other field on that path.

Live afterwards: node `pose-seq` advancing 788 → 848 → 908, Director
`poses-coalesced=0`, and **`poses-missed=1`** — one pose genuinely lost near
session start, counted rather than invisible. Note the node's miss is not
matched by a Director coalesce, so that pose went missing somewhere other than
the mailbox; worth a look, and now it can be looked at.

Both presets build clean and all 134 CTest entries pass.


## Why this took so long to find

Worth writing down, because the delay was not caused by the bug being subtle.
It was caused by three things that each said "this area is fine".

**The tests asserted the bug was correct.** Two cases in `icvfx_tests.cpp` were
of the form `assert inner_frustum_view(...) == view_for_slice(...)` - the
implementation compared against a restatement of itself. They could not fail
whatever the projection did, and they reported the area as covered. The
replacements state the property in wall terms and were checked to fail against
the old code before being kept. A grep for the pattern found only those two;
the one other candidate, `scene_slice_tests.cpp:428`, is a legitimate
equivalence between two call forms.

**A round-trip test was blind to the field it was round-tripping.**
`commit_permit_tests.cpp` already sent a pose Director-to-node and compared the
whole struct - and passed while `pose_sequence` was dropped in two places,
because the fixture left that field at its default and both ends compared zero
to zero. The fixture now gives every field a distinct non-default value, and
with the carry removed the test no longer passes. Note the failure mode: it
*hangs* rather than reporting, so in CI this class of regression appears as a
timeout with no message. Worth fixing separately.

**Nothing could say it was idle.** Six separate conditions in
`record_inner_frustum` are each a legitimate reason to draw no plate, and from
the wall they are one black rectangle. Adding a line that named the declining
condition turned hours of guessing into one run, which is why it is now a shared
primitive (`core/idle_report.hpp`) and an invariant in the folder README rather
than an ICVFX one-off. `report_idle_once` returns whether it actually spoke, so
the dedup is testable; `an idle reason is reported once and then stays quiet` in
`tests/unit/core_tests.cpp` covers it.

Three rules were added to the README's invariant list from this: honest idle
reporting, no test naming the function under test on both sides, and no
round-trip test leaving a field at its default. The status document's
contradictory entry was corrected with a note about *why* it misled, since the
failure was that it described intent where a reader needed reachable behaviour.

**Unrelated, found on the way:** `mediaserver.render_model_cache` is flaky under
the parallel suite - it failed once and passed standalone and on re-run. A suite
that fails intermittently is part of how a real defect survives, because it
trains everyone to re-run rather than look.


## 5.1, and the plate on the wall

**`TrackerCalibration` on `TrackingSourceSettings`.** Per feed, because it
describes the tracking installation and every camera on that system shares it:
an up-axis convention, a stage-space origin, a heading offset and a uniform
scale. Clamped once at the model boundary, persisted, replicated, and applied at
the single boundary where a tracker's numbers become the scene's
(`evaluated_tracked_camera`), so everything downstream - the plate rectangle,
the plate's view, the gizmo, the digest - deals only in stage coordinates and
knows nothing about how the tracker was surveyed.

Two decisions worth keeping:

- **Angles are not remapped, only offset.** Pan, tilt and roll are azimuth,
  elevation and bank about whatever the tracker calls up, and after the axis map
  that is the stage's up too *with the same sense of rotation* - so heading is
  the whole orientation correction, and it is the one an operator touches daily.
  A test pins the handedness: `cross(mapped +X, mapped +Y) == mapped +Z`.
- **Only right-handed conventions are offered.** A left-handed tracker needs a
  mirror, and a mirrored camera basis produces a plate that looks plausible and
  is reversed. Every system worth using can emit right-handed, and a documented
  gap beats a setting that is quietly wrong.

Authoring rows are in the camera's TRACKING section beside the feed's other
settings: Up axis, Heading, Origin X/Y/Z, Scale.

### The plate reached the wall

First confirmation, 2026-09-02, on the demo project with a real FreeD feed:

- Under `-Motion Pan` the node drew a plate. Its output differs from the same
  scene with no plate, and the node logged **no** decline past the startup
  message. The outer frustum is identical in both cases - it uses the authored
  camera, which the tracker does not touch - so the difference is the plate.
- Under `-Motion Orbit` the node logs `the tracked frustum meets no part of this
  wall` and the plate is absent, which is the aim problem 5.1 exists to correct
  and which this project has not yet been calibrated for.

A detail that confirms item 2.1 rather than contradicting it: under Pan the
plate's *pixels* are static while its *rectangle* moves. That is correct. After
2.1 the plate's projection is built from the tracked eye through the wall
rectangle, so rotating the camera changes which rectangle is chosen and nothing
inside it. Only translating the camera produces parallax. A plate that swam
while the camera merely panned would be the bug.

**Authoring rows confirmed by hand, 2026-09-02.** Up axis, Heading turns,
Origin X/Y/Z and Scale appear in the camera's TRACKING section under the bound
feed and read correctly. Verified by the operator rather than by this tooling:
synthetic clicks do not land in this UI - menus and list selections both
ignored them across several attempts - which is worth knowing for anyone
planning to automate against it.

The label says "turns" deliberately. Live Data reads pose angles out in degrees
because that is what an operator thinks in, and a heading field that took
degrees while the document stored turns would be wrong by a factor of 360 in
the one place someone is trying to line a plate up. The transform rows already
set this precedent with "Angle turns".

One more diagnostic was added on the way, and it is the one that made the Pan
result readable: `IcvfxStage::record` can decline after `record_inner_frustum`
has already approved everything it can see - a plan that never became active, or
a node whose slice misses every plate - and said nothing. It now reports both,
through the same `report_idle_once`. That was the last place a plate could go
missing silently.


## 3.1 and 3.2 — latency compensation now describes something

Fixed together, because neither works alone: sending a compensated frame to a
channel that applies it on receipt changes nothing, and a channel that waits for
a frame it is never told about waits forever.

- **3.1.** The Director now replicates the compensated frame. It previously sent
  the raw one while keeping the compensated value for its own channel, so every
  node applied each pose on arrival and the authored compensation described
  nothing. The wire field's own comment already said "after latency
  compensation".
- **3.2.** `PoseChannel` keeps a bounded window of poses stamped for frames that
  have not come round yet, and answers a frame with the newest pose stamped at
  or before it. A pose stamped forward now *waits*. Frames before any pose
  applies answer `None` rather than borrowing a future pose, which is honest:
  there is no pose for that frame yet, and inventing one is the thing two nodes
  must never do.

`a pose stamped forward waits for its frame` pins it - a pose stamped for 102
answers `None` at 100 and 101, `Exact` at 102, `Held` at 103 - and
`a full pending window holds rather than growing` pins the bounded degradation.
Both fail against the single-slot channel.

**Two follow-on problems this created, and how they are handled.**

Sending a compensated frame makes the replicated stamp move when the
compensation does, and *lowering* it mid-show would stamp the next pose earlier
than the last - which every node's channel refuses as stale, freezing the plate
at exactly the moment an operator is adjusting the setting meant to fix it. The
Director now clamps the replicated frame to never go backwards, so the change
costs a few frames of extra latency while the raw frame catches up and is then
exact again. That is most of item 3.3, arrived at from the other direction. The
Director also stamps its own channel with the frame that went on the wire rather
than the sample's own, so a `--role=director,render` machine cannot draw from a
differently-stamped pose than its neighbours were sent.

The clamp was wrong first time round, and the 8.3 counters caught it in one run:
`last + 1` forces the stamp upward every loop iteration, so a parked transport
made every iteration a new pose. The wire showed `pose-seq=2999` against 884
tracker samples, `poses-coalesced=232` and `poses-missed=738`. It is `last`, not
`last + 1` - the clamp must stop the frame going backwards, not force it
forwards, because a parked transport has to re-stamp the same frame and be
reported as a repeat. After the correction: `poses-coalesced=0`,
`poses-missed=0`, and the plate still drawing.

That is the observability work from earlier paying for itself: a counter added
an hour before turned a subtle wire regression into an obvious one.


## 4.1 — the receive wakes on a datagram, not on a timer

The loop is gone. The socket is armed with an asynchronous receive that re-arms
from its own completion, and the worker thread does nothing but service
completions on the `io_context` that was already sitting unused in the
listener's state. A datagram wakes the thread when it lands.

What it replaced: a non-blocking read that slept 100 ms whenever the socket was
empty. At any real tracker rate the socket empties between datagrams, so the
thread slept, a burst queued in the kernel buffer, the loop drained it back to
back, and slept again. Pose updates reached the slot at roughly 10 Hz against a
24-60 Hz shoot, and the resulting 0-100 ms of jitter is not a fixed offset, so
the authored latency compensation could not correct it however well items 3.1
and 3.2 now work.

`stop()` calls `io_context::stop()` before joining, so shutdown stays bounded
without a poll - the join now waits for a thread already on its way out rather
than for a sleep to expire. **That mostly settles 4.2 as a side effect**: the
`configure()` call on the Director's frame boundary still runs every frame, but
the join it performs on a settings change is now sub-millisecond instead of up
to 100 ms. Reconfiguring only when a socket-affecting field changed is still
worth doing; it is no longer a frame-boundary stall.

An oversized datagram is now counted rather than handled differently per
platform. Windows reports `message_size` and hands back truncated bytes while
Linux truncates silently, so the refusal counter is what makes the two agree -
and a sender this feed is not talking to is something an operator should see.

**Testing a latency fix without a flaky test.** `a datagram wakes the receive
rather than a timer` sends ten datagrams one at a time, each only after the
previous was observed, so a poll cannot amortise them into a single wake. An
event-driven receive costs microseconds per round; a 100 ms poll costs an
expected 50 ms, so ten rounds is half a second against a 500 ms budget. The
whole binary runs in about 30 ms across repeated runs - a hundredfold headroom
rather than a coin toss. The amplification is what makes the old behaviour fail
decisively instead of about half the time.

**Measured live:** 314 datagrams in ten seconds, about 31 Hz. That is the
*generator's* ceiling, not the listener's: `send-freed-tracking.ps1` paces with
`Start-Sleep -Milliseconds 16` and Windows' timer granularity is about 15.6 ms,
so a requested 60 Hz lands near 32 Hz. Worth knowing when reading any figure
from that script - it under-represents a real tracker, and the receiver is no
longer what limits it.


## 6.1 and 6.2 — one lens decides the plate

Both items had narrowed since 2.1. Once the plate's projection came from the
wall rather than from the camera's own frustum, the lens stopped deciding what
the plate *renders* and was left deciding only where the plate *is* - so 6.1 and
6.2 turned out to be the same fix in the same place.

**The rectangle is now sized by the camera's calibrated lens** when it has one,
at both production call sites: the node's `record_inner_frustum` and the
Director's Live Data readout. It previously came from a field of view the
Director derived from the *feed's* single sensor height and fallback focal
length - one point, which cannot follow a zoom encoder - while the camera
carried a calibrated profile that sized nothing. Two lenses deciding one plate
is a background that slides against the set piece it should be locked to during
a push-in.

The lens-aware form also widens the rectangle to the **distorted** field, which
is 6.2: it is what stops a wide lens photographing outer frustum at the corners
of its own plate. The render itself stays undistorted, as
[02](02-lens-model.md) requires - the physical lens will distort whatever the
wall shows, so pre-distorting would double it. Only the coverage is widened.

The feed's sensor and focal length remain the fallback for a camera with no
profile, and the comment on `TrackingSourceSettings::sensor_height_mm` now says
so. That comment previously justified the whole arrangement by claiming that
re-deriving from a lens table would make nodes disagree. It would not: a profile
is replicated document state like everything else, so every node evaluates the
same table with the same expression. A wrong reason in a comment is worse than
no comment, because it answers the question that would otherwise get asked.

**The duplication that made this necessary is gone.** The lens-aware overload
was a *copy* of the plain one, so the 11.1 fix - separating "faces the wall and
overshoots" from "cannot see the wall at all" - landed in one and left the other
behind. Switching the call sites to the lens form would have quietly restored
the old bug for exactly the calibrated cameras this work is for. Both now share
one `region_from_corners`, and the new test checks the refusals through the
lens-aware path specifically.

`a calibrated lens sizes the plate rather than the feed fallback` pins it: the
calibrated rectangle differs from the feed-derived one, a longer focal length
gives a smaller plate monotonically, and a camera behind or turned away from the
wall draws nothing through the lens path too.


## 8.1 — an OpenTrackIO feed can name its camera

The parser now reads `sourceNumber`, which is OpenTrackIO's analogue of the
camera id FreeD puts in every datagram, and the listener matches on it. A feed
authored for any camera but zero previously refused 100 % of what it received
while showing a bound link and climbing refusal counters - the parser never set
`camera_id`, so the listener compared an authored 3 against a hard zero.

**Absent is not zero.** FreeD always carries an id; OpenTrackIO makes it
optional, and collapsing "did not say" into "camera 0" is what created the bug
in the first place. `OpenTrackIOSample::identifies_camera` keeps the two apart,
and the listener uses it: a sample that names no camera is matched only by a
feed that asks for none, which is the ordinary single-camera-on-a-port case. A
feed authored for camera 3 receiving unidentified samples is a real mismatch and
is counted as foreign rather than quietly accepted - the operator set an id for
a reason.

An out-of-range `sourceNumber` refuses the sample whole rather than truncating
it into some other camera's id.

Two tests, because the bug lived in the seam between them:
`a source number names the camera a sample is for` covers the parser, including
that absent and zero stay distinguishable, and
`an opentrackio feed matches on the sample's source number` covers the listener
end to end - mine tracked, somebody else's counted, unidentified counted against
a numbered feed and accepted by an unnumbered one.


## 9.1 — the Live Data panel scrolls

The `scroll_` member that was declared and never read now does its job. Wheel
over the body scrolls, clamped at both ends, with a slim thumb down the right
edge so a clipped panel is distinguishable from a short one - without that an
operator has no reason to try the wheel.

**The scroll bound comes from what the last frame actually drew**, not from a
row count computed up front. A count would have to reproduce every conditional
in the body - which rows a held pose adds, what a clamped plate prints - and
would drift from the drawing code the first time either changed. That is the
same duplication that hid the 11.1 fix from the lens-aware overload and lost
`pose_sequence` in the wire copies, so it is worth not introducing a third
instance for a scrollbar. The cost is one frame of lag after the content
resizes, which on a diagnostics panel is invisible and self-correcting.

`content_height()` and `scroll_offset()` are exposed read-only. A diagnostics
panel reporting its own geometry is unobjectionable, and it lets the behaviour
be asserted without rendering pixels: content shorter than the body cannot
scroll at all, four cameras can, scrolling past either end clamps, and a pointer
outside the panel does not move it.

One thing the test found on the way, worth knowing for anyone writing another
panel test: the harness calls `input.new_frame()` at the top of its draw, which
clears the per-frame accumulators including the wheel. Input set before that
call is silently discarded. The harness now has `draw_with_wheel` which applies
it in the right order.


## 9.2 — a clamped plate says so

`director_inner_frustum_rect` returned only the rectangle, throwing the clamped
flag away at the one point the operator could have been told about it. It now
returns the whole `InnerFrustumRegion`, the observation carries
`inner_rect_clamped`, and the panel reports it.

The distinction matters because the two situations look identical in the number
that was being shown. A plate covering 100 % of the wall is either a camera
framing that wall exactly or a camera whose frustum runs off the end of it -
opposite things, and only the second is worth interrupting someone about. A
clamped plate is correct and expensive: it costs every pixel of the wall, so the
second render stops being the saving it exists to be.

`plate_coverage` is a pure function beside `inner_frustum_report`, for the
reason that one already gives in its own comment - it is the only way to assert
this without rendering text. That turned out to be load-bearing rather than
stylistic: the first version of the test compared vertex counts between a
clamped and a fitted panel and they were identical, because the test harness
builds a `FontAtlas::empty()` and text emits no glyphs. A vertex count cannot
see a string change in this harness, so any future panel assertion needs a pure
function too.

**Still unread:** `IcvfxPlan::clamped` on the node. That is now consistent
rather than dropped - both sides compute it from the same region, and the
operator surface reports it - but a node still says nothing about its own ICVFX
work. That is item 8.4 and 9.3 territory: `IcvfxStatistics` is incremented and
never read outside `engine/render`.


## 9.3 — the barrier says which fault it is

`SWAP BARRIER INCOMPLETE` covered three situations with three different fixes,
and `SWAP BARRIER LOST` covered two. Five distinct states now, because an
operator reading this line while the wall is wrong needs to know which thing to
go and do:

- **`SwapBarrierIncomplete`** - joined nothing yet. Ordinary during startup;
  wait, and worry only if it persists.
- **`SwapBarrierUnsupported`** - this build, driver or adapter has no present
  barrier at all. Nothing on the stage fixes it, so reporting "incomplete" sent
  someone looking for a cable that was never the problem.
- **`SwapBarrierMisconfigured`** - joined, but to a different group or barrier
  than the policy authorises, or with fewer outputs than expected. Fixed by
  editing the project. This had no test at all.
- **`SwapBarrierLost`** - a client was dropped after joining. It worked and now
  does not.
- **`SwapBarrierOutOfSync`** - still joined, and the driver's own in-sync count
  is falling behind its present count. Nothing has failed and the wall is coming
  apart anyway, which is the failure nobody notices until it is on camera.

Ordered by what to do first: something that broke, something that is breaking,
something authored wrongly, something this machine cannot do, and last the one
that may just need another moment.

**The gate is unchanged.** `verified` is still `physical == Ready` and
`physical_proven` still admits only `Ready` or `Unsynced`, so every one of these
refuses Program exactly as before. This changes what the operator is told, never
what the cohort is allowed to do - and the new test asserts that explicitly,
because a diagnostic split that quietly loosened a safety gate would be a much
worse bug than the one it fixed.

One mistake worth recording: the first pass at updating the existing assertions
used a pattern that matched neither of the two blocks it was aimed at, and the
suite failed on the one test that still expected the old code. Two tests in that
file differ only by a `!readiness.verified` line, and a search-and-replace with
a count of one is a coin toss between them. The fix was to read both and edit
each by hand.


## 8.4 — a node says what it did with the tracking

A node now reports seven counters to the Director on every heartbeat ack, and
they answer the question the Director could not previously ask: *is the plate on
that wall the one I sent?* Its own counters could never answer it - they say
what was **sent**, and every interesting failure happens after that.

`net::RuntimeTracking` rides the existing `HeartbeatAck`, the same path
`RuntimeOutputSync` and `RuntimeRendererReadiness` already take, and is held on
the service as well as the session so a reconnect replays the last report rather
than telling the Director this node has never tracked.

It carries the ordinal this node last received and how many it never saw; frames
answered exactly and frames answered by holding; plates drawn, skipped and
refused. The node status line prints all of it, the Live Data panel gets a row
per node, and the Director status line carries a one-token-per-node summary so a
headless stage - which is how a stage is usually run - can be read from the
console.

**Skipped is printed beside drawn, always.** On a wide wall most outputs miss
the plate rectangle, and that skip is the entire performance argument for a
second frustum. A panel that reported only "drawing" would make the number that
proves ICVFX is earning its cost invisible, and a zero there is itself worth
seeing.

`node_tracking_report` decides the row's verdict once, as a pure function, for
the same reason `inner_frustum_report` and `plate_coverage` are pure: six
situations - silent, no pose, no plate planned, planned and never intersecting,
drawing, drawing behind - look identical from the Director, which is a wall with
no plate on it, and they have six different fixes. Asserting that is a test;
reading it off a screenshot is not.

`show::PresenterPlateStats` is a new three-counter struct on the presenter
interface rather than a `render::IcvfxStats` reference, because `mediaserver_show`
does not depend on `mediaserver_render` and should not start. Both ICVFX stages
are summed, so a node's plate count does not stop dead at a graph transition it
actually rendered through.

### What it found on the first run

The point of the item was to make node-side tracking observable. It was, and
immediately:

```
director  poses-sent=1398 poses-coalesced=0   nodes-tracking=0:behind
node 0    pose-seq=1478 poses-missed=18 ...
```

The Director had coalesced nothing, so 18 poses had supposedly vanished on a
loopback TCP connection. They had not. `NodeResumeSession` held incoming poses in
a **one-deep slot that was overwritten and never consumed** - `tracked_camera_pose()`
was a plain read, so a pose that arrived while the render loop was busy was
silently replaced by the next one. The drop was real, correct policy, and
attributed to entirely the wrong machine: the node was blaming the Director for
its own backlog, and the node's `poses-missed` was the only number saying so.

Two changes:

- `take_tracked_camera_pose()` consumes the slot. The pose channels downstream
  are what retain a pose, so leaving it there bought nothing and cost the one
  thing that mattered - with the slot never emptied there is no way to tell an
  overwrite from an ordinary re-read, which is exactly why this went uncounted.
  It also stops the loop re-accepting the same pose every iteration.
- The overwrite is counted and reported as `poses_coalesced`, an eighth field,
  kept strictly apart from `poses_missed`.

**They are different faults on different machines.** A miss is a pose that never
reached this node - look at the link. A coalesce is one that did and was dropped
here - look at this machine. `node_tracking_report` reports the link first when
both are happening, because a node cannot drain what it never received.

The same run after the split:

```
node 0    pose-seq=1478 poses-missed=2 poses-coalesced=16 poses-applied=1418
          pose-holds=2 plates=5728 plates-skipped=0
```

Sixteen of the eighteen were the node's own, and two were genuinely lost. Every
one of them had been reported as lost before.

**This is the third instance of the pattern in this list.** A value written in
one place and read in another, where the read did not do what its name implied:
the lens overload that missed the 11.1 fix, the two wire structs that dropped
`pose_sequence` field-by-field, and now a slot called `tracked_camera_pose` that
was a peek. Each was invisible until a counter was put on both ends of it.

### Verified

Both presets build clean and 134/134 pass. Live on the two-process demo with the
FreeD generator driving `-Motion Pan` at 60 Hz: the node prints the full set, the
Director prints `nodes-tracking=0:behind` derived from what arrived over the ack,
and the counter split reproduces as above.

Tests: `a pose overwritten before it is drained is counted` drives three poses
with no drain between them and asserts two coalesces, newest-wins, an empty slot
after the take, and no hole in the ordinal - the whole point being that the
ordinal *cannot* show this and only the new counter can. `runtime tracking round
trip` covers all eight fields including that moving one does not move another.
`the node report distinguishes every reason` covers all six verdicts, including
that a missed pose on a node drawing nothing is not worth interrupting anyone
about, and that link-before-node ordering.

`mediaserver.render_model_cache` failed once under `-j 8` and passed standalone
and on re-run, as it has before. It touches nothing here.


## 7.1 — the Director's control frame follows house reference

The Director now paces its control frame from the video-reference counter when
the authored policy asks for an external reference, using the same modulo-2^32
counter differencing the node boundary driver already does. Before this it
created a plain `show::ShowClock` off `steady_clock` under every policy, so a
genlocked show had its presents locked to house reference and the clock those
presents derive their position from running on this machine's oscillator. The
two drift, and the whole wall hitches together.

`[03]` names this as the half people forget. It was half-done: `TimingAuthority`
and the reference sampling already existed and were wired on the node.

### It falls back rather than holding, and that is deliberate

A node without a reference holds - the previous frame stays on the wall, and
that is right for a node. A Director that held would seal no control frames at
all. The operator command that recovers from a reference fault - lowering the
policy, stopping the transport - **is itself a control frame**, so holding would
make the fault unrecoverable without restarting the show.

So the pacer keeps running on the system clock and reports that it is. The
existing comment on that clock already asserted the property this preserves:
"it free-runs regardless of transport state ... so pausing never stops the frame
stream that carries transport changes and keeps the cohort acknowledging."

### The gate, and a behaviour change worth knowing about

`SyncReadinessCode::DirectorClockUnreferenced` refuses Program when the policy
asks for an external reference and the Director is not using one. It is reported
**before** any node-level reference fault, because if the clock the cohort is
keyed to is unreferenced then a node's own reference is beside the point, and
naming the node first sends an operator to the wrong machine.

**This changes what promotes.** An `ExternalGenlock` show whose Director has no
video reference - no output window, or a card that is not in the house chain -
now refuses Program where it previously promoted. That is the fix rather than a
side effect: that configuration was drifting the whole time and every node
reported itself perfectly locked while it happened. It is called out here
because it will look like a regression to anyone who had one running.

`SyncReadinessInput::director_clock_referenced` defaults to true, so any caller
that has not wired it up reads exactly as this gate did before the field
existed.

### Reporting

`sync::DirectorClockStatus` carries whether a reference is wanted, what state it
is in, and how many control frames were paced each way. It rides on
`SyncStatusSnapshot` beside the node rows rather than as a node row, because
nothing about it comes off the wire.

The Director status line gained `clock=system|reference|unreferenced` and
`clock-unreferenced=`. An operator reading a wall of green node rows had no
other way to learn that the cadence they are all locked to is this machine's
system clock.

### Verified

Both presets build clean and 134/134 pass on each. Four new cases in
`engine/sync/tests/sync_topology_tests.cpp`:

- **an unreferenced director clock refuses a genlocked show** - the same cohort
  of fully locked nodes, satisfied with the Director referenced and refused
  without it. This is the fault every node reports as healthy.
- **the director clock is reported before a node reference** - with both wrong,
  the Director is named.
- **an unsynced show does not care about the director clock** - a gate that
  failed this would refuse every show that never asked for genlock.
- **the director clock status reports only real drift** - `drifting()` is false
  for an unsynced show whatever the reference reads, because a show that never
  asked for one is doing what it was told.

Live on the two-process demo, the Director reports `clock=system
clock-unreferenced=0` under the demo project's unsynced policy.

**Not verified on hardware.** This machine has no genlock - it reports
`sync=unsynced barrier=not-joined` - so the referenced path itself has never
run. What is proven is the policy plumbing, the fallback, the reporting and the
gate; what is not is that the counter differencing paces correctly against a
real house reference. That needs a genlocked box, and it is the same gap
[7.2](#) would close by giving a non-NVIDIA stage a locked path at all.

### Still open

- **A Director with no reference hardware cannot lock, at all.** Falling back
  and saying so is honest, but on a stage where the Director has no card in the
  house chain there is no way to key the cohort's cadence to house reference
  short of deriving it from the nodes - a different architecture, where the
  cohort's clock is a negotiated quantity rather than a local one. That is its
  own item, not a detail of this one.
- The policy is read one loop iteration behind, because it is evaluated further
  down the same loop from the document that iteration seals. That is a frame of
  latency on an operator changing the policy, not a source of drift.
