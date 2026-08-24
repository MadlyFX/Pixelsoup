# Next steps — from the vertical slice to a usable media/2D track

This plan picks up where the "video on the wall, manipulated in 2D and 3D"
vertical slice left off. That slice proved the interaction model end to end; the
three tracks below turn the proof into something an operator can actually run a
plate through. It maps onto **Phase 1 — Media and 2D** of the architecture plan
(§6, §6.2, §8, §9).

Each track is independent enough to ship on its own branch and merge to `main`
when green. They are ordered by dependency and payoff, but only track 2's
"distribution" half is a hard prerequisite for anything downstream (a plate you
can't get to a node can't be shown on a real wall).

---

## Current baseline (what the slice already gives us)

So each track starts from the same known ground:

- **A media element in the document.** `scene.media` nodes carry `source` (a
  `#`-run sequence template), `sequence_start`/`sequence_count`, and the usual
  transform properties. `engine/scene/scene_model.cpp` parses them and
  `evaluate()` emits a `scene::MediaInstance` per frame, with
  `frame_index = sequence_start + (time.frame % sequence_count)` — a fixed loop.
- **Frame-exact, replicated, deterministic.** The frame index is derived from
  the show frame, so every node agrees; the media digest covers it, so
  checkpoint and stream converge. This is the same guarantee the rest of the
  scene already has.
- **Manipulation as document commands.** `engine/scene/manipulate.cpp`
  (`screen_drag_to_translation`, `pick_media`, `media_for`) resolves a drag to a
  world delta; `apps/server/playback.cpp::schedule_scene_edit` reads
  `projected_document().get(id)`, copies the node, sets `position_*`, and authors
  a `PutNode`/inverse pair on `UndoContextId{2}` (the scene stack, separate from
  transport's `1`). Edits ride the frame-scheduled bus and replicate on seal.
- **Two viewports over one scene.** The throwaway ImGui `engine/editor` shell
  renders the wall (show camera) and a free 3D view, with a working translate
  gizmo. It is explicitly disposable (§9: ImGui for internal tooling only).
- **A CPU image-sequence decode path.** `engine/render/media_cache.cpp` loads
  RGBA via stb_image and uploads per frame; the renderer composites the textured
  quad into the scene's colour+depth.

What's missing is everything that makes it a *tool*: motion that isn't a fixed
loop, real codecs and getting content to nodes, and edits that behave like edits
(one drag is one undo, several clips move together).

---

## Track 1 — Keyframe timeline

**Goal.** Replace fixed per-second rates and the hard-coded clip loop with
animatable parameter channels driven by a timeline, so a value can change over
show time and an operator can author that change. This is the track the slice
was designed to feed: edits are already document properties, precisely so a
keyframe channel can own them.

**Why now.** It's the natural generalization of what `evaluate()` already does
(map show frame → value) and it unblocks the timeline UI (§9's multi-track
timeline is 70%-bespoke work; get the *model* right first, behind the throwaway
shell). It has no new external dependencies, so it's the cleanest track to do
first and it exercises the document/undo model (§8) hard.

**Work.**

- **`engine/scene` — a keyframe channel type.** A channel is `(target node,
  property name) → ordered keys`, each key `(frame, value, interpolation)`.
  Interpolations: hold, linear, and a cubic (Bézier/Hermite) with tangent
  handles. Evaluation is pure and deterministic: binary-search the key interval,
  interpolate in `f64`, return `f32` — same discipline as `wrap_turns`
  (`math.hpp`) so ten-hour shows don't drift or step. Fold the existing
  per-second rates (object spin, the clip's sequence advance) into this: a
  "rate" becomes a two-key linear channel, so there is exactly one code path
  that turns show time into a value.
- **`scene_schema.hpp` / `scene_model.cpp`** — parse channel nodes; in
  `evaluate()`, apply channels over the base node values *before* building the
  `MeshInstance`/`MediaInstance` world matrix. Extend the digest to cover
  channel output (it already covers the derived transform, so this mostly falls
  out).
- **Clip playback becomes a channel.** `frame_index` is currently
  `time.frame % count`; make it the output of a `frame_index` channel (default:
  a looping ramp reproducing today's behaviour) so a clip can be retimed,
  held, or scrubbed independently of show time — the "per-layer transport"
  §6 asks for, in its simplest form.
- **`engine/editor`** — a timeline panel: a timecode ruler, one row per
  animated channel, draggable keys, and a playhead. Keep it minimal and ImGui;
  it's a stand-in for the real curve editor. Dragging a key authors a
  `PutNode` on the channel node through the same `schedule_scene_edit` rails.
- **`apps/server`** — nothing new structurally; channel edits are just more
  scene edits on `UndoContextId{2}`.

**Reuse.** `evaluate()`'s existing frame→value mapping; `schedule_scene_edit`;
the digest machinery; `deterministic::*` for any eased curves that need trig.

**Risks.** Cubic interpolation is where determinism bugs hide — pin the
evaluation order and test bit-exact across checkpoint-vs-stream, like the scene
model tests already do. Tangent representation (auto/broken/weighted) is a
rabbit hole; ship hold+linear+basic-cubic and defer the rest.

---

## Track 2 — Real video decode + content distribution to nodes

**Goal.** Two halves. (a) Decode real codecs, not just PNG sequences. (b) Get
the content to the nodes that need it, with a hard content-ready gate. Half (b)
is the load-bearing one — it's the difference between "plays on my machine" and
"plays on the wall" (§6.2).

**Why now.** The image-sequence path was always a stand-in. Until a node can
obtain and decode the plate, the editor only ever drives the Director's local
preview. This is the track that makes a second machine real.

**Work.**

- **Decode behind a seam (half a).** A `MediaSource` interface in
  `engine/render` (or a new `engine/media` module) that yields an RGBA/BCn frame
  for a given `frame_index`, with the current stb_image path as its first
  implementation. Add **HAP** next — it's the workhorse (§6): Snappy → BC1/BC7,
  cheap, and its BC upload path exercises compressed-texture upload we'll want
  anyway. FFmpeg (ProRes/DNxHR/H.26x via NVDEC) comes after, gated behind a vcpkg
  feature so a headless build never links it. Keep decode strictly behind the
  RHI/`MediaCache` seam so the renderer doesn't learn about codecs.
- **Frame-exact seek.** Whatever the codec, `frame_index → pixels` must be
  exact, never nearest-keyframe (§6). This is a decoder-implementation
  requirement, so bake it into the `MediaSource` contract and its conformance
  test.
- **Content-addressed store (half b).** A BLAKE3-hashed blob store; the document
  references content by hash, not path (the `#`-run template becomes an ingest
  concern, not a runtime one). The Director tracks which blobs each node holds;
  sync is delta-based, resumable, bandwidth-throttled — over the existing
  session/command transport where it fits, a side channel where it doesn't.
- **The content-ready gate.** A node reports ready for the content it holds; a
  scene referencing content no node-of-record has cannot go Live. Surface it in
  the editor as a hard, visible state — reuse the existing per-node
  status/digest reporting the Director already prints, promoted into the UI.
- **Defer ROI slicing.** §6.1's per-node ROI propagation and pre-slicing is the
  hardest part of the media pipeline and is *not* needed for LED-wall-tiled,
  effect-free content (the current output stage has no cross-node effects that
  would need an apron — see the LED-wall-only note). Full-decode-and-crop (§6.1
  mode 3) is the correct, robust fallback until an effect graph forces the
  issue. Note the deferral loudly rather than letting it look handled.

**Reuse.** `MediaCache` and the per-frame upload path; the command/session
transport and the Director's node tracking; the digest/status reporting that
already lets the operator confirm nodes agree.

**Risks.** FFmpeg licensing/linking and NVDEC driver coupling — the vcpkg
feature gate keeps them optional and off the headless path. Distribution is a
distributed-systems problem; keep it delta-based and resumable from day one,
because retrofitting resumability is painful.

---

## Track 3 — Multi-select and coalesced drag transactions

**Goal.** Make edits behave the way §8 requires: one continuous drag is **one**
transaction (one undo step), and several selected clips move **together** under
one command. Today each drag frame authors a fresh `PutNode`, so undo would step
back through hundreds of micro-edits, and only one node can be selected.

**Why now.** It's small, it's self-contained, and it removes the most obvious
rough edge in the editor. It also forces the transaction/coalescing model (§8's
`begin_txn`/`end_txn` with a coalescing window) to exist before the timeline
piles more continuous edits on top — so there's an argument for doing the
transaction half *before* Track 1's key-dragging.

**Work.**

- **Coalesce the drag (transactions).** A drag opens a transaction on
  mouse-down and closes it on release; intermediate frames update the *same*
  transaction rather than emitting new command pairs. The inverse captured at
  open is the one undo restores. This is the `begin_txn`/`end_txn` +
  coalescing-window model from §8; wire it through `cmd::DirectorScheduler` and
  `schedule_scene_edit` so the coalescing lives on the bus, not in the shell.
- **Multi-select.** The editor holds a *set* of selected `doc::NodeId`s
  (shift/ctrl-click to add, marquee later). `pick_media` already returns the
  front-most node; extend selection state and the Elements list to a set.
- **Group move.** One drag applies the same world delta to every selected clip
  and authors them under one transaction, so the group moves and undoes atomically.
  `screen_drag_to_translation` is per-object depth-locked; apply it per selected
  clip from each clip's own captured start position.
- **Editor plumbing.** Selection set, per-clip drag-start positions captured at
  mouse-down (mirroring the single-clip `drag_start_*` state already there), and
  the gizmo operating on the selection's shared origin.

**Reuse.** All the manipulation math (`screen_drag_to_translation`,
`translation_of`); the existing single-clip drag-capture pattern in
`editor_shell.cpp`; the `UndoContextId{2}` scene stack.

**Risks.** Coalescing-window semantics (when does a new gesture start a new
transaction?) need to be explicit and tested, or undo granularity gets
surprising. Keep the window tied to the mouse gesture (down→up) first; add
time-based coalescing for sliders later.

---

## Suggested ordering

1. **Track 3's transaction half** — smallest, and every later continuous edit
   (keys, group moves) wants it. Multi-select can follow immediately.
2. **Track 1 — keyframe timeline** — builds on transactions; unblocks the
   timeline UI and generalizes the animation model.
3. **Track 2 — decode + distribution** — largest and the one with external
   dependencies; the content-ready gate and a second real node are the payoff.
   Its decode half (HAP, then FFmpeg) and distribution half can proceed in
   parallel behind the `MediaSource` seam.

Rationale: 3-then-1 gets the document/undo model correct before the UI leans on
it; 2 is deliberately last because it's where the cost and the external risk
concentrate, and the other two are useful without it (against the Director's
local preview and a `--scene` node on the same machine, as the demo already
shows).

---

## Verification (per track, consistent with the current suite)

- **Headless model tests** for anything deterministic: channel evaluation
  bit-exact across checkpoint-vs-stream (Track 1); `MediaSource` frame-exact
  seek conformance (Track 2); transaction coalescing produces one inverse and
  group moves are atomic (Track 3). These join the existing `scene_model` /
  `scene_manipulate` suites and must stay green on the headless preset.
- **GPU tests** (skip with no device) for any new upload path — BCn/HAP upload
  mirrors the existing `render_media` readback test.
- **Live**, via the `run-editor-demo.ps1` pattern: a second node showing the
  same replicated scene, so a timeline scrub, a group drag, or a real plate is
  visible on the node's wall at the same frame — with the content-ready gate
  blocking Live until the node reports ready (Track 2).

---

## Deferred beyond these three (each its own later increment)

- ROI propagation / per-node pre-slicing (§6.1) — until a cross-node effect
  graph needs an apron.
- The bespoke §9 UI (retained-mode widgets over the RHI); ImGui stays throwaway.
- Colour management beyond known-gamma sRGB — calibration/3D-LUT stays
  downstream on the LED processor (see the LED-wall-only note).
- Lit/shadowed media and colour-correct exposure; curve-editor tangent modes
  (auto/broken/weighted); Notch/plugin-reflected animatable parameters (§7.1).
- Versions/branching, A/B compare, and presets (§8) — the document model
  supports them; they're a separate track once the timeline exists.
