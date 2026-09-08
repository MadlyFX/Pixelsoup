# 01 — Camera tracking and inner/outer frustum

**Track 1. Nothing else in this folder matters as much.**

## Why

Right now the renderer draws one authored camera's view and slices it across
nodes. A physical camera moving on a volume sees a backdrop with wrong parallax:
the geometry does not shift against itself as the camera dollies, so the wall
reads as a flat image of a set rather than as the set. No amount of IBL, GI, or
antialiasing fixes this. It is the defining mechanism of ICVFX and it does not
exist in the tree — `grep -r "inner_frustum\|camera_tracking\|FreeD"` over
`engine` and `apps` returns nothing.

The shot needs two regions on the wall:

- **Inner frustum** — the region the camera actually sees, rendered from the
  tracked camera's real viewpoint at full quality. This is the plate.
- **Outer frustum** — everything else, which the camera does not see directly
  but which lights the subject and appears in reflections on cars, glass, and
  eyes. Lower fidelity is fine; correct colour and level is not optional.

## The critical design decision

**Tracked pose is per-frame control data, not document state.**

If tracking is written into the document, every frame bumps `document_version`.
`HistoryKey::document_version` is a deliberately conservative temporal epoch, so
every frame would `Reset` temporal history — which kills TAA
([08](08-temporal-antialiasing.md)), and every future temporal pass, permanently.
It would also push a document edit through the whole replication path 24-60 times
a second.

So tracking rides a separate replicated per-frame channel, sampled and stamped by
the Director:

1. The tracker (FreeD / Mo-Sys / Vicon / OptiTrack) is read **on the Director
   only**, on its own thread, into a small lock-free slot.
2. At the frame boundary the Director samples that slot **once**, associates it
   with the exact 64-bit show frame, and replicates it.
3. Nodes never read a tracker. A node that sampled its own tracker feed would
   get a different pose from its neighbour and open a seam mid-wall.

`scene::Camera` keeps its authored role: the fallback/previz camera, and the
authored lens profile lives beside it ([02](02-lens-model.md)). The tracked pose
*overrides* eye/target/up for the inner frustum when a tracking source is live
and the project authored one.

## Seams

- **New module** `engine/track/` — tracker protocol parsers (FreeD is a small
  fixed-size UDP datagram; start there), a bounded pose slot, and a pure
  `TrackedPose` value. No renderer or document types cross into it.
- `engine/net/schema/session_protocol.fbs` — a `TrackedCameraPose` table on the
  per-frame path. It belongs with `FrameCommitPermit` / the frame stream rather
  than in `Heartbeat`, because it must be exactly aligned to a show frame, not
  "recent".
- `engine/scene/include/mediaserver/scene/scene.hpp` — a `TrackedCamera` value
  (pose + validity + the show frame it was sampled for) carried on the evaluated
  `scene::Scene` beside `camera`, plus authored `IcvfxSettings` in
  `RendererSettings` (inner-frustum enable, margin, resolution policy, latency
  compensation in frames).
- `engine/scene/include/mediaserver/scene/canvas.hpp` — the inner-frustum region
  derivation. This is the seam that must be bit-identical across nodes, so it
  belongs beside `window_for_slice` and `view_for_slice` where that property is
  already established and tested.
- `engine/render/include/mediaserver/render/scene_renderer.hpp` —
  `SceneRenderView` already carries an explicit view with `history_identity`.
  The inner frustum is another `SceneRenderView`; the plumbing exists.
- `engine/render/src/hybrid_scene_renderer.cpp` — two renders per output now,
  composited. The history-slot machinery (256 slots, keyed by owner) already
  supports independent histories for inner and outer.
- `engine/output/perspective_mapping.hpp` and
  `engine/render/include/mediaserver/render/mapped_scene.hpp` — the wall's
  physical geometry, which is what the camera frustum must be intersected
  against to find the inner-frustum region.

## Design

**Finding the inner-frustum region.** Project the tracked camera's frustum onto
the wall surface. The wall geometry is already available as a projection mesh
through `OutputProjectionCache`, and `perspective_mapping` already resolves
canvas→surface. Compute the axis-aligned canvas rectangle covering the projected
frustum corners, expand by the authored margin plus the lens distortion extent
([02](02-lens-model.md)), and clamp to the canvas.

That rectangle must be derived from replicated values only — tracked pose, lens
profile, wall geometry, canvas — with no per-node input, and rounded with the
same integer expression on every node. Round outward: a rectangle one pixel too
small shows outer-frustum content inside the camera's view, which is a hard cut
in the middle of the plate.

**Rendering it.** The inner frustum is an off-axis perspective render at the
tracked pose, into its own target at the authored inner resolution (usually
higher pixel density than the wall, since the camera is looking at a sub-region).
A node whose slice does not intersect the inner rectangle skips it entirely and
counts the skip — that is the whole performance argument for ICVFX, and it must
be a rectangle intersection test on replicated values, not a heuristic.

**Protocols**
The software should support FreeD and OpentrackIO protocols

**Compositing.** Outer frustum first (the existing scene render through
`view_for_slice`), inner frustum over it, with an authored soft edge. The soft
edge width is replicated; two nodes sharing the inner rectangle's boundary must
produce the same feather. Note that per the LED-wall-only decision there is no
node-to-node feathering — this is a feather *within* the canvas between two
render regions, which is a different thing and is required.

**Latency compensation.** Tracker → Director → wire → render → present is
several frames. The compensation is authored project state in frames, applied by
the Director when it stamps the pose, so every node uses the identical
compensated pose. Never machine-local, never adaptive.

## Multi-node hazards

- Any per-node tracker read. Covered above; make it structurally impossible by
  keeping `engine/track` out of the node binary's frame path.
- Inner-frustum rectangle rounding. Use the `window_for_slice` discipline: the
  same expression over the same integers, no accumulation, no per-node origin.
- A node that receives no pose for a show frame must **hold**, not extrapolate.
  Extrapolating locally means two nodes invent different poses.
- `camera_history_digest` currently includes camera projection and exposure. A
  tracked zoom changes projection every frame, which would `Reset` history every
  frame and defeat TAA. See open questions.

## Tests

- OpentrackIO datagram parsing: known bytes → known pose; malformed and truncated
  datagrams are refused, not partially applied.
- The pose channel: a node given the pose for show frame N renders N; a repeated
  N holds the same pose; a missing pose holds rather than extrapolating.
- Inner-frustum rectangle: two nodes with different slices derive byte-identical
  rectangles from the same replicated pose; the rectangle covers every projected
  frustum corner; rounding is outward.
- Slice intersection: a node whose slice misses the inner rectangle records no
  inner render and reports the skip.
- GPU: full-canvas inner+outer composite versus two-node slices, byte-exact at
  shared pixels, with the camera at a pose that puts the inner rectangle across
  the node boundary. This is the test that actually proves the feature.

## Definition of done

A tracked camera moving on the volume produces correct parallax on the wall;
every node in a Program cohort derives the same inner-frustum rectangle and the
same pose for the same show frame; a node with no pose holds rather than guessing;
and the two-node composite is byte-exact against the full-canvas render.

## Open questions

- **Does `camera_history_digest` need to stop including projection?** A live zoom
  otherwise resets temporal history every frame. Options: exclude projection
  (motion vectors then cover the zoom, which is what they are for), or quantise
  it. Needs a decision before [08](08-temporal-antialiasing.md).
- **Inner frustum resolution policy** — fixed, or derived from the camera's
  angular coverage? Derived is better quality per pixel but must be replicated
  and must not change per frame, or the render target reallocates mid-show.
- **More than one camera.** Multi-cam volumes need multiple inner frusta.
  The design above generalises, but the compositing order and the wall's
  lighting contribution do not; defer explicitly rather than half-supporting it.
