# Remaining work

The current product is described by the [README](README.md),
[architecture](media-server-architecture-plan.md), [operator guide](docs/operator-guide.md)
and [renderer capabilities](advanced-renderer-status.md). Development rules and
build requirements are in [AGENTS.md](AGENTS.md). This roadmap retains future
contracts; check current code before treating an individual item as missing.

## Renderer and stage workflow

- Light cards, outer-frustum color-correction windows and plate chromakey:
  replicate and digest authored regions, emissive cards and deterministic wall
  markers; verify feathering and placement across output-node seams.
- Bindless descriptors, writable storage buffers and indirect submission before
  GPU-driven culling or BLAS/TLAS/ray-query effects. Preserve deterministic
  instance ordering, honest capability gates and cohort conformance.
- Physical two-node genlock/present-barrier and camera latency qualification,
  recorded capture procedures and a pinned driver matrix; an editor-as-remote
  client role would let a stage machine own the Director clock.
- Optional USD composition/identity translation remains gated by a real studio
  workflow need; see the [adapter proposal](blender%20live%20scene%20link/13-optional-usd-adapter.md).

## Media and studio qualification

- Preserve high-bit-depth software video through decode, upload and color
  conversion. The current generic ProRes/DNxHR path converts to RGBA8; decoder
  availability alone does not establish a precision-preserving pipeline.
- Add native GPU compressed-texture HAP playback and evaluate a qualified
  NotchLC path against real production files. Retain frame identity, alpha,
  color metadata and bounded admission; do not advertise profiles without
  pixel and seek coverage.
- Qualify simultaneous production streams at their authored resolution and
  rate, with measured decode, upload and GPU times, residency pressure and
  held frames. Include the nine-plate LED Car Process and switching while
  uploads are pending.
- Qualify the physical 15360x1546 Mosaic desktop with four 3456x1536 viewports
  at 3840-pixel intervals. GPU readback and configuration tests establish
  placement, but cannot establish driver scanout, physical synchronization
  or sustainable stage throughput.
- Preserve source material names in imported model packages so the current
  numbered material-part texture selector can show meaningful slot names.
  The [Asset Viewer plan](asset-viewer-plan.md) phase 3b depends on this.

## Editorial and compositor depth


The following packages are specified enough to estimate and implement. They
are ordered by dependency: establish the data model and deterministic CPU
projection before exposing editor controls or adding a GPU processor.

The Asset Viewer - a source monitor for media, a look-dev inspector for
models, and asset-level material and interpretation settings - is planned in
three phases in the [Asset Viewer plan](asset-viewer-plan.md). The first two
are built; phase 3, asset-level settings, remains.

#### Effects, masks, mattes, and graph diagnostics

Every effect below follows `effect-rendering-requirements.md`: it has one
document representation, registry entry, identity predicate, execution domain,
depth/alpha/bounds policy, editor and keyframe route, save/load and undo/redo
coverage, and a real GPU result. Parameters are finite and clamped at the model
boundary. An identity node remains editable but adds no pass and changes no
pixel, statistic, bound, or digest beyond its authored graph state.

The dependency-ordered common colour controls are implemented; this table is
retained as their exact contract:

| Effect | Parameters and exact operation | Domain and identity |
| --- | --- | --- |
| Levels | Per master/R/G/B channel: input black/white, gamma, and output black/white. Evaluate `x = clamp((value-inBlack)/(inWhite-inBlack), 0, 1)`, then `outBlack + pow(x, 1/gamma) * (outWhite-outBlack)`. Reject equal/reversed input endpoints rather than dividing by zero. | Inline; input/output endpoints `0/1` and gamma `1`. Alpha is unchanged. |
| Lift/Gamma/Gain | RGB lift, gamma, and gain vectors. In the selected project colour-response domain, evaluate `pow(max(rgb + lift, 0), 1/gamma) * gain`; gamma components must be positive. | Inline; lift `0`, gamma `1`, gain `1`. Alpha is unchanged. |
| Contrast/Pivot | RGB result `(rgb - pivot) * contrast + pivot`, with contrast `0..4` and pivot `0..1`. Do not insert an intermediate clamp. | Inline; contrast `1`. Alpha is unchanged. |
| Colour Matrix / Channel Mixer | Replicated 3x3 matrix plus RGB offset, evaluated in binary32 in the shader from model-normalized finite coefficients. This also supplies a deterministic base for monochrome and simple colour transforms. | Inline; identity matrix and zero offset. Alpha is unchanged. |
| White Balance | Temperature in `2000..12000 K` and tint in `-1..1`, converted from D65 by one documented Bradford chromatic-adaptation implementation to a 3x3 matrix during model evaluation. | Inline; `6500 K`, tint `0`. Alpha is unchanged. |
| Curves | Master and optional R/G/B curves with strictly increasing `x` control points in `0..1`. Generate a fixed 1024-entry binary32 LUT with the same monotone-cubic algorithm and rounding on every node; upload the LUT as derived render data. | Inline; endpoints `(0,0)` and `(1,1)` with no interior points. Alpha is unchanged unless a separately declared Alpha curve is later added. |

Add spatial filters only as isolated-layer processors with truthful allocation
bounds:

| Effect | Parameters and pass contract | Identity / bounds |
| --- | --- | --- |
| Box Blur | Integer radius and pass count; separable horizontal/vertical box passes. | Radius `0`; expand by `radius * passes`. |
| Directional Blur | Length in canvas pixels and angle in turns; sample along one line with a project-wide bounded tap count. | Length `0`; expand by the absolute projected kernel extents. |
| Zoom/Radial Blur | Amount, sample count, and normalized centre; samples follow the segment from the pixel to the centre. | Amount `0`; bounds remain the source bounds unless the chosen sampling equation reaches outside them. |
| Defocus | Disk or hexagonal aperture radius, blade rotation, and bounded quality preset. Use a documented gather kernel; do not label Gaussian blur as defocus. | Radius `0`; expand by aperture radius. |
| Bilateral Blur | Spatial radius and colour sigma, operating on colour while preserving strong edges. It is a spatial denoise, not a temporal denoiser. | Radius `0`; expand by spatial radius. |
| Unsharp Mask | Gaussian radius, amount, and threshold; output `source + amount * (source - blurred)`. | Amount or radius `0`; bounds match the Gaussian dependency. |

Add keying and texture effects with these contracts:

- **Chroma Key** consumes a source layer and a key colour. Parameters are key
  colour, Rec.709 Cb/Cr-distance tolerance, softness, edge erosion/dilation,
  spill suppression, and key mix `0..1`. It outputs premultiplied colour plus
  coverage; it changes alpha and therefore executes as an isolated
  colour-plus-depth layer, not as an opaque inline shader. Spill suppression
  changes straight RGB only after the matte is calculated; final coverage is
  `sourceCoverage * lerp(1, keyedCoverage, keyMix)`, then RGB is premultiplied
  again. Morphology is gated by source coverage, so it cannot fabricate an
  opaque black fringe outside a transparent source. Identity is disabled or
  key mix `0`.
- **Luma Key** has low/high thresholds plus independent low/high softness and
  invert plus key mix `0..1`. Thresholds must be ordered. It has the same
  premultiplied coverage and depth contract as Chroma Key; key mix `0` is
  identity.
Masks and mattes need first-class structure rather than effect-specific hidden
fields:

- Add a scene mask node targeting one drawable or group. A mask is Rectangle,
  Ellipse, or closed Bezier. Drawable-mask points use normalized source-layer
  coordinates; because a group has no source plane, group-mask points use
  normalized authored-canvas coordinates and require one isolated group
  composite. Bezier points are child mask-point nodes with mask id, integer
  order, position, and in/out handles, avoiding an opaque array property.
  Authored mask properties are feather in canvas pixels,
  expansion/contraction, opacity, invert, order, and combine mode
  Add/Subtract/Intersect/Difference.
- Fold active masks strictly in `(order, node id)` order into one coverage
  field. The first active mask must be Add and seeds the field; later operators
  evaluate Add=`max(a,b)`, Subtract=`a*(1-b)`, Intersect=`min(a,b)`, and
  Difference=`abs(a-b)`. No active masks is identity. Rectangle/Ellipse with
  zero feather and opacity `1` may use an analytic hard-discard path that omits
  colour and depth outside the mask; Bezier, feathered, or partially opaque
  masks rasterize to a bounded single-channel offscreen target. Disabled masks
  are identities; empty or malformed paths fail visibly.
- Add a dedicated Masks panel for the active Scene/Stage target. Its ordered
  rows expose visibility/enabled state, Rectangle/Ellipse/Pen creation, combine
  mode, invert, opacity, feather, expansion, and direct rename/reorder/delete;
  malformed-path and target-space diagnostics appear on the row. The panel and
  Stage share one mask/point selection, while ordinary scene selection remains
  a separate typed domain.
- The Stage mask editor supports point selection, pen insertion, handle drag,
  whole-mask transform, feather/expansion overlays, and a toggle to view the
  coverage field. One pointer gesture is one transaction. Mask picking is
  editor-local; the mask path and ordering are document state.

Graph diagnostics become a stable operator contract:

- Give every diagnostic a stable code, Error/Warning severity, subject node,
  related node/edge, port, and deterministic sort key. Errors suppress the
  executable plan; warnings do not.
- Diagnose disconnected Output paths, dead/unreachable branches, missing or
  duplicate inputs, incompatible ports, cycles with the exact cycle members,
  unsupported effect-domain order, duplicate active Scene Layer sources,
  alpha/depth incompatibility, unbounded expansion, and pass/texture/tap budget
  overflow.
- Show the path from the offending node to Output, the execution domain and
  identity state of each effect, and estimated offscreen texture size/pass
  count. Resource estimates are derived from the authored canvas and project
  settings, never from one machine's available VRAM.
- Diagnostics may offer explicit actions such as Disconnect Edge, Move Before
  Blur, or Insert Matte. An action authors an ordinary undoable command; the
  validator never rewrites a graph by itself.

Acceptance requires CPU model tests for every normalization and diagnostic,
save/load and streamed-delta equivalence, editor/keyframe/undo tests, and GPU
pixel/depth tests for opaque, translucent, media, geometry, mapped output, and
identity cases.

#### Timeline lanes and clip editing

The landed lane model gives “subsequent” and “adjacent” exact meanings:

- Scene-owned timeline lane nodes have a stable lane id, integer order,
  name, authored mute, lock, and compositing mode. Media without an explicit
  lane evaluates in one implicit default lane; evaluation does not mutate the
  document.
- Each media placement references one lane and retains its existing `asset_id`,
  source, start, duration, source-in, asset-rate relationship, opacity ramps,
  transform, style, effects, and channels. Multiple placements may reference
  the same asset.
- Render lanes by `(lane order, lane id)` and clips within a lane by
  `(clip start, clip order, clip node id)`. Overlap is legal. Opacity ramps and
  explicit transitions determine a crossfade; otherwise the later clip order
  composites over the earlier clip using its authored blend mode.
- Lane create, rename, reorder, mute, lock, and delete operations are explicit.
  Deleting
  a non-empty lane requires an explicit choice to delete its clips or move them
  to a named destination lane. Solo is operator-local Preview state; mute is
  replicated show state.

Editing semantics:

All edit deltas below are integer show frames. Converting a show-frame delta to
source ordinals uses the asset/show rational-rate mapping and rounding already
used by playback, so trimming and rendering choose the same source frame.
Preserve the residual source-time phase when a trim or split falls between
native source frames. The current integer `source_in` can match the cut frame
but cannot preserve every subsequent sample at nonintegral source/show rate
ratios. A normalized, replicated phase representation must participate in
evaluation, digests, clipboard, undo, and checkpoint/delta tests before claiming
frame-identical fractional-rate cuts.

- **Ripple trim:** define the signed drag delta `d` in show frames. Tail ripple
  sets `duration += d` and shifts every other unlocked clip starting at or
  after the old tail by `d`. Head ripple sets `source_in += d` and
  `duration -= d`, keeps the selected clip's timeline start fixed, and shifts
  every later unlocked clip by `-d`. Thus removing source frames shortens the
  show and adding available handles lengthens it. Source-handle, positive-
  duration, and representable-frame limits clamp the whole gesture before any command is
  authored. Sync-locked, unlocked lanes take the same boundary and delta (see
  the operator guide's track switches); straddling and locked clips stay, and
  a collision with one clamps the trim.
- **Ripple delete:** the selection must contain whole clips whose union is one
  contiguous span, with no unselected clip intersecting that span on a targeted
  lane. Delete those clips, then shift clips starting at or after the old span
  end left by the exact span duration. Reject other selections rather than
  guessing which gaps or partial clips to remove.
- **Roll edit:** requires exactly two unlocked clips that touch at one cut on
  one lane. Move the shared cut by `d`: the left duration/source-out and right
  start/source-in change by opposite deltas, while the combined outer span and
  every later clip remain fixed. Clamp `d` to both assets' available handles.
  A transition centred on the cut moves with it and keeps its duration.
- **Slip edit:** keep a clip's timeline start/duration fixed while moving
  source-in by the source-rate equivalent of `d`. Looping playback wraps the
  source offset in either direction; a future explicit non-looping playback
  mode must clamp to available source handles. **Slide edit:** move the selected
  clip by `d` while rolling its two immediate neighbours so the three-clip
  outer span stays fixed. Slide requires both neighbours and no intervening
  gap.
- **Clip copy/paste:** Copy snapshots selected, unlocked placements at command
  time. The anchor is the minimum clip start. Include each clip's first-class
  effect nodes and channels targeting the clip or those effects. Paste into the
  active scene at the playhead, offsetting clip starts and key frames by
  `playhead - anchor`; preserve asset ids and source timing, allocate new node
  ids in ascending original-id order, rewrite every target/scene/lane reference,
  and schedule one transaction. Missing source lanes map by relative selected
  lane order, creating lanes only through an explicit Paste With New Lanes
  action. Clipboard data is Director-local snapshot data and clears when the
  project generation changes.
- **Insert/overwrite:** Insert shifts later clips on targeted unlocked lanes by
  the pasted duration. Overwrite lays the paste over the target lane in front
  of its clips, which stay whole underneath; nothing is trimmed, split or
  moved, so the lane keeps its length. Both land where the selection is, and
  outside every lane when no lane is chosen. Each mode previews its affected
  span before commit.

The Timeline adds lane headers, vertical reorder, per-lane lock/mute, adjustable
height, horizontal/vertical scrolling, track filters, and snap targets for
playhead, range endpoints, clip edges, transitions, and keys. Every completed
gesture is one transaction with focused tests for negative frames, unavailable
handles, locked lanes, overlaps, rational frame rates, undo/redo, and streamed
document evaluation.

#### Selection, groups, undo coalescing, and curves

Selection is editor-local; groups and authored edits are replicated:

- Click selects one item and establishes the primary selection. Ctrl-click
  toggles membership, Shift-click selects the contiguous visible row/lane
  range, and empty-canvas click clears. Marquee supports contain and intersect
  modes plus add/subtract modifiers. Store membership as a unique sorted node
  set plus a distinct primary id so command generation never depends on click
  order.
- Stage, Scene, Timeline, and Compositor maintain separate typed selection
  domains. Switching panels does not coerce node ids from one domain into
  another. Delete, copy, paste, and history route to the explicitly active
  domain.
- The Inspector shows a value when all selected compatible nodes agree and a
  Mixed marker otherwise. Typing a value is absolute for every compatible,
  unlocked target; dragging a numeric field applies one delta from each
  target's captured start. Incompatible or locked targets are listed and left
  untouched, not silently converted.
- Add scene group nodes with one transform. Each child stores one parent-group
  id and integer group order, which fits the existing primitive property model.
  Group creation reparents the selected top-level nodes while preserving world
  transforms; ungroup performs the inverse. Reject cycles, duplicate children,
  cross-scene parenting, or a child with two parents. Moving a group evaluates
  `group world * child local`; selection may enter the group or select it as one
  unit. Lock/visibility inheritance is explicit and model-tested.
- Vehicle process groups. The groundwork is in place: `scene.group` carries
  `group_kind` (`vehicle_process`; absent is a folder), the Scene panel creates
  one empty from its right-click menus and marks it with a vehicle icon, and
  the kind is parsed, replicated and part of the scene digest. It otherwise
  behaves exactly as a folder. The vehicle processing workflow itself - what
  such a group holds and what it adds to the Inspector and render path - is
  still to be specified; any field it adds that changes the picture needs
  normalisation, replication and digest coverage, and unsupported requests
  must fail readiness with a specific reason.
- Named Selection Sets use one selection-set node plus ordered member nodes
  carrying set id and target id. Recalling a set changes only editor-local
  selection. Missing members are reported and skipped. This avoids replicating
  every operator click while still letting a show carry useful named sets.
  A Selection Sets panel provides Save New, Replace Members, Recall, Rename,
  and Delete. It shows member and missing-member counts; double-click recalls,
  while replacing membership requires an explicit action and one undoable
  document transaction.

Undo coalescing uses semantic gesture boundaries, not elapsed wall time in the
replicated command stream:

- Pointer press through release, one text-field begin/edit/commit, one
  keyframe drag, one paste, one delete, and one group operation each allocate
  one transaction id. Live preview commands from a drag reuse that id.
- Repeated keyboard nudges may join only while focus, selection, property,
  direction, and modifier set are unchanged and no other command intervenes.
  The Director-local editor closes the group after 350 ms of inactivity;
  timestamps are never serialized and all emitted commands still have exact
  frame boundaries.
- Focus loss, pointer cancel, selection change, panel switch, project change,
  save/load, undo, and redo close the current coalescing group. A rejected edit
  neither creates an undo step nor clears redo.

Curve and dope-sheet work:

- Add box/lasso selection of keys, multi-channel move, duplicate-drag, delete,
  and rational time scale around an explicit first-key, last-key, playhead, or
  user marker anchor. Convert the result to integer show frames with
  round-to-nearest, ties away from zero; if multiple moved keys land on one
  frame, the key latest in `(original frame, node id)` order wins and the UI
  previews the collision.
- Copy/Paste Keys and Alt-duplicate carry authored handle weights unchanged.
  Where the destination interval is shorter than the handles across it the
  channel stops evaluating; refuse such a paste (or shorten the handles in
  proportion) and report it, as key moves, scales, handle drags and easing
  already do.
- Provide horizontal/vertical zoom, pan, fit selection, frame/value rulers,
  channel colour/visibility, snapping, value guides, and a playhead-linked
  numeric editor. Curves display evaluated pre/post-infinity as constant until
  explicit extrapolation modes are designed.
- Moving keys across properties or targets is not supported implicitly;
  Copy/Paste Keys performs an explicit compatibility check and reports every
  skipped channel. All multi-key changes use one transaction and restore exact
  tangents and selection on undo/redo.

#### Compositor graph editor

The Compositor panel should reach the fluency of a mature node editor such as
Unreal Blueprints. Node positions, wire waypoints, and comment boxes are
authored editor layout: they save, undo, and replicate with the graph, but stay
out of evaluation and the scene/settings digests. Every gesture below goes
through the existing intent and Director authoring path as one transaction.
Verify each step with `mediaserver_editor_ui_snapshot`'s `compositor`,
`compositor-far`, `compositor-near`, and `compositor-wire` views, at 100% and
150% display scale.

The foundations are in place: node layout measured from the font, text that
scales with zoom, curved wires picked along the curve, whole-row port targets
with snapping, no jump on release, type colours and pin shapes, and a
resizable, scrollable Parameters panel. `CompositorPanel::input_port_at()`
already answers which input a screen point is over, for drops from other
panels.

Wiring is in place too: drops from the Timeline, Effects, and Library onto
an input; the node search on Tab, right click, or a wire let go on empty
canvas; splicing a free node into a wire; Alt-click to break and Ctrl-drag to
move wires; reroute points stored as `point_N_x/y` on the wire's own node;
refusal reasons, flow highlighting, and dimmed dead branches. So is
selection and organization: Ctrl/Shift clicks and a contain-or-intersect
rubber band, group moves and coalesced arrow nudges, copy/paste/duplicate
through `plan_paste_graph` with a Director-local clipboard, F/Home framing
and a minimap, a right-click menu with align, distribute, Straighten
Connection and Arrange, `compositor.comment` boxes, and inline renaming.
Diagnostic badges sit on their subject node or wire with the messages on
hover; key values are edited on the node body (`inline_values()`); nodes
collapse to their title and wired ports through the layout-only `collapsed`
flag; a Scene Layer node and its Timeline row reveal each other without
sharing a selection; an Effects drop on an effect wire joins that input after
the wire's effect, renumbering the input's `order`s in the same step; and the
minimap flag is kept in workspace layout version 6. Bypass is the authored
`bypassed` flag on effect and Radial Array nodes (normalized away on any
other kind): `effect_node_is_identity()` treats a bypassed node as its
identity, so application, ordering, offscreen passes and metadata follow,
and the flag is digested. A Scene Layer's effects come in on two typed
inputs, EFFECTS (port 0, `PortValueType::Effect`) and FILTERS (port 2,
`PortValueType::Filter`), and LED Car Process has SETUP EFFECTS (9) and SETUP
FILTERS (10), so the executor's effects-then-filters order is part of the
wiring and `UnsupportedEffectOrder` is gone; wires on one input are
reordered by dragging their numbers or from the wire menu. Chroma Key, Luma
Key and Matte are edited in Parameters. What remains:

- **Library asset onto an image wire.** Effect nodes have no image input
  today, so splicing an image source into an image wire waits on an image-in,
  image-out effect node kind.

Richer nodes, last because each adds a model or render contract:

- **Output thumbnails.** Optional thumbnails of a node's output come from a
  bounded, asynchronous preview path, never from frame recording, and show a
  placeholder while unavailable.
- **Subgraphs and groups.** Bypass and comment boxes are in place, so
  these can be planned next.

Tests cover the pending-connect and pending-add hand-off, refused edits, the
port-drop precedence and every drop source, and layout-only fields that do not
change any digest. Each workflow is also exercised in the shipping editor.

#### Manifest-backed thumbnails, proxies, and asset derivatives

The persistent Library asset node remains the only asset identity. A thumbnail,
filmstrip, waveform, or proxy is a derivative of `(asset id, source content
fingerprint, derivative profile, generator version)`, never another Library
asset and never a replacement `asset_id` on a placement.

- Add expected byte length and SHA-256 fields to each persistent Library asset
  node; carry them into every required content-manifest entry for cross-node
  verification. Compute them during explicit import/ingest, cache the result by
  file metadata locally, and verify it after distribution. A changed file stays
  a visible mismatch on the same asset id until Relink or Accept New Revision
  updates the expected fingerprint through one undoable document edit.
- Define versioned derivative profiles. Start with `thumb-320x180-srgb`,
  `filmstrip-8x160x90-srgb`, `waveform-2048-peak-rms`, and
  `proxy-720p-cfr-h264`. Profile definitions include dimensions, pixel format,
  colour primaries/transfer/matrix, alpha policy, audio layout, codec settings,
  and generator version. Changing any field changes the cache key.
- Thumbnail frame selection is deterministic: stills use their only frame;
  video uses the first valid frame at or after 10% of source duration, falling
  back toward frame zero; image sequences use the corresponding exact ordinal.
  Filmstrip samples eight evenly spaced source ordinals including first and
  last. Respect source rotation and pixel aspect before fitting with transparent
  letterbox; never crop unless a profile says so.
- Proxy generation preserves source duration, rational frame-rate metadata,
  rotation, colour metadata, and a table mapping every proxy frame ordinal to
  source presentation time. Playback continues to resolve trims and show-frame
  sampling against the source timeline, then asks the proxy map for a frame;
  switching Source/Proxy cannot move a cut or keyframe by one frame.
- Derivatives live in a machine-local cache beneath a configured cache root,
  written to a temporary file and atomically renamed after validation. Cache
  files are disposable, excluded from project save and replication, and evicted
  by size-bounded LRU only when not in use. Cache paths are derived from the
  key; user filenames are never used as identity.
- The project may author a requested proxy profile and Source/Prefer
  Proxy/Require Proxy playback policy. Local generation progress, failures,
  cache size, and availability are runtime observations reported per node, not
  document state. Prefer falls back visibly to source; Require marks the node
  unready and never substitutes a different rendition.
- Library rows show asynchronous placeholder, queued, generating, ready,
  stale, or failed states. The editor requests only visible rows plus a bounded
  prefetch window, cancels work after scrolling away, and uploads completed
  thumbnails through the existing GPU cache seam. Timeline clips reuse the same
  thumbnail/filmstrip key rather than decoding independently.
- The Library's List/Grid view, column widths, hidden columns, sort and
  thumbnail sizes are editor-local and reset on launch. They belong in the
  workspace layout, next to the panel's dock state. Library folders are
  already authored document state.
- Assets imported before codec, start timecode, model vertex count and
  texture size were recorded show those columns blank until a reimport or
  an accepted change re-probes them. `RESCAN` should also offer to fill in
  missing metadata for unchanged files, as one undoable edit. Thumbnails
  exist for media only; models need a rendered turntable derivative under
  the same keying rules.
- Relink updates the existing asset node and invalidates its derivatives after
  one undoable confirmation. Delete Asset removes the document record only
  after reference analysis; cache cleanup is a separate recoverable maintenance
  action. Duplicate source imports resolve to the existing `(kind, exact
  source)` asset unless the operator explicitly creates a new source revision.

Tests cover manifest/checkpoint/delta equality, fingerprint mismatch, cache-key
versioning, atomic cancellation/restart, stale invalidation, exact source/proxy
frame mapping at fractional rates, alpha and colour metadata, missing-cache
fallback policy, LRU pinning, and the invariant that no derivative creates or
rewrites a document asset id.

#### Editor proxies: fast editing media without waiting for a transcode

Positioning, trimming and layout do not need full-resolution plates, but the
editor decodes every video and image sequence at source size. The goal is that
the Director's editor surfaces use a 720p rendition automatically, the operator
never waits for a transcode before working, and full resolution is one toggle
away for rotoscoping, tracking and fine colour work. Output nodes keep drawing
the source, or the authored higher-quality proxy, exactly as today.

**What already exists.** The `proxy-720p-cfr-h264` profile (1280x720, CFR at
the source rate, 8-bit 4:2:0 Rec.709, GOP 60, AAC stereo), the
`proxy-source-cfr-hevc-main10` profile, the FFmpeg backend that prefers NVENC
([ffmpeg_proxy_encoder.cpp](apps/server/ffmpeg_proxy_encoder.cpp)), exact
source-time frame maps, the keyed derivative cache with size-bounded LRU, the
authored Source/Prefer/Require policy, Library and Asset Viewer status chips,
and a single background generation worker
([playback_media.hpp](apps/server/playback_media.hpp)). The Director takes the
authored scene digest before proxy substitution, so a Director-local rendition
choice cannot disturb cohort agreement.

**What stands in the way today.**

- The Director already gives every video larger than 1920 x 1080 an
  editor-only 720p working proxy (`editor_playback_registry` in
  [playback_media.cpp](apps/server/playback_media.cpp)), generated on the
  Director when the asset enters the project and never on a node. Until
  phase 1 there was no way to see the source in the editor instead.
- A proxy is usable only after the whole file is encoded. The encode is
  about 98% of generation time; the source fingerprint is a size and
  modification-time signature and costs nothing, and verification, packaging
  and materialisation add about 2 s for a two-minute 4K plate (see the
  baseline below).
- Decode and scaling are CPU-only (software decode, `sws_scale`), one asset
  at a time on one worker. The default Background priority caps the decoder
  at two threads, which makes it 4.4x slower than Fast on 4K H.264: the CPU
  decode, not NVENC, sets the speed.
- Scrubbing and reverse play do not reach exact frames for the source or for
  a proxy (see the baseline), so a smaller rendition alone does not make
  J/K/L or a playhead drag responsive.
- The profile drops alpha and has no image-sequence path; the encoder opens
  only container files. An 8-bit Rec.709 rendition of a linear or HDR EXR
  plate looks wrong under grading or keying.
- The profile encodes AAC audio, although the program has no audio path.

**Phase 1: an editor-only rendition (done 2026-10-29).** Generation on
import and the source fallback already existed. Phase 1 added:

- A **Proxy / Full res** media menu on Stage and on Program Output, each
  view's own, kept in the workspace layout (flag bits, so older records read
  as Proxy). The Asset Viewer header has the same choice. Nothing is
  replicated or digested; the Director resolves Program and transitions,
  Stage and the Stage view, and the viewer through the authored registry for
  Full res and the working-proxy registry for Proxy. An authored Prefer or
  Require policy still applies in every view.
- While a mask is selected or the eyedropper is armed every view draws the
  source and returns to its choice afterwards.
- The menu's tooltip reports what the view drew on the last frame: clips on
  proxies, clips on their source until their proxy is ready, and under Full
  res the clips whose asset asks for a proxy everywhere.

Still open from the phase 1 intent:

- Library row actions **Regenerate proxy** and **Use full res for this
  asset**, and cache eviction that prefers editor proxies of unused assets.
- The Asset Viewer's rendition chip is dropped when it does not fit beside a
  long name; the header needs a narrower layout for the long Source states.
- Working proxies cover UHD video only; image sequences wait for phase 3.

**Phase 2 findings, 2026-10-29** (before any phase 2 change):

- Keyframe interval hardly matters. Seek to an exact frame on a 720p proxy
  took 71, 63 and 60 ms at GOP 60, 12 and 4, against a 60 ms floor of
  request, decode hand-off and two renders; scrub and reverse results did not
  change. Keep GOP 60.
- Drag scrubbing works on proxies: a four-frame step holds the previous
  step's frame for one tick and never blanks, where four 4K sources blank on
  every tick. The decode registry clears queued read-ahead on each new
  current-frame request, so read-ahead never runs ahead of a drag.
- Reverse play fails on every rendition because each backward request seeks
  and decodes a whole GOP to keep one frame (`decode_until` in
  `engine/render/src/ffmpeg_source.cpp`). Retaining a backward-decoded GOP
  in the shared frame map is the fix; it lives in the decode registry that
  output nodes use too.
- Proxy encodes still carry AAC audio, which nothing plays.

**Phase 2, done 2026-10-29: playback-aware priority, no audio.** Measured on
the Release Director (RTX 3090, 5800X) with the project's driving plates, on
an otherwise idle machine:

| Generation running | 10 proxies made in, segmented | Paused loop wake mean / late | Playback, 4 proxies | Playback, 4K source |
| --- | --- | --- | --- | --- |
| None | - | 2.2 ms / 64-67% | 712 / 720 exact | 720 / 720 exact |
| Background | 829 s (887 s one segment per encode) | 1.9 ms / 46% | 718 / 720 | 720 / 720 |
| Balanced | not run | - | 720 / 720 | 720 / 720 |
| Fast | 213 s (262 s one segment per encode) | 2.2 ms / 49% | 720 / 720 | 718 / 720 |

Playback is `mediaserver_render_media_video_benchmark --d3d12 --frames 360`
beside one encoder benchmark, one run per cell. Neither playback nor the
paused loop shows a cost from generation at any priority: the differences
are run-to-run noise, and the late share of the paused loop varies more
between idle runs than with generation. An earlier pass taken while a video
render ran elsewhere on the Director lost 4-6 frames at Balanced and Fast;
that was the render. Whole-file generation of the same ten proxies took
677 s at Background and 209 s at Fast during that render.

- Each job takes its CPU budget when it starts: the Preferences priority,
  now **Fast** by default, while nothing plays; Background while the show or
  the Preview playhead plays. A render node's generator always yields.
  A long Fast job begun just before play keeps Fast until it ends; segmented
  generation below shrinks that window to one segment.
- The 720p profile is version 2 with no audio; the encoder skips the
  source's audio when the profile has none, and both validators accept only
  an audio-free output for it.

**Phase 2, done 2026-10-29: segments, made by need, and a seamless switch.**

- The editor profile is made in segments of ten seconds of source
  (`proxy_segment_range`), each keyed by `derivative_segment_key` and
  published as its own cache envelope with a frame map in absolute source
  time. The worker makes the segment under each Program and Stage
  placement's requested frame first and carries on to the end of the file,
  then makes the start of the file, and joins the published segments into
  the whole-file proxy by remuxing them. Each such run of segments is one
  encode (`ProxyEncodeRequest::piece_frames`): the encoder starts a new file
  on a forced IDR at every segment boundary and the service publishes each
  segment as it is handed over. Between segments the worker looks again and
  leaves the run when a view needs a segment outside it or play starts or
  stops; what the run made is kept. A run reports segments the cache
  already holds at its start and encodes from the first missing one.
  The resolver plays a published segment until the whole proxy is ready, and
  prewarms the next segment within a second of the boundary.
- A placement that moves between files of the same asset keeps showing its
  last picture until the new file has a frame (`MediaCache::resolve`'s
  `rendition`), and the Stage loading banner counts only placements with
  nothing to show (`blank_requests`). Copying the old file's hold state into
  the new file's entry stalled the cache for good; the hand-over now borrows
  the old file's picture and keeps that file on air instead.
- Measured idle on a two-minute 4K H.264 plate at Fast, three interleaved
  pairs: the first segment is drawable after 1.8 s; one run makes all
  thirteen in 18.8-19.4 s against 18.3-18.8 s for a whole-file encode, and
  the join adds 1.6 s. Encoding one segment per encode took 24.2-24.5 s,
  because each segment reopened the source and encoder (about 0.2 s), seeked
  and refilled the decoder's threads (encoding at about 140 fps against
  176). The joined proxy decodes to the same pictures as a whole-file
  encode at every boundary (identical at the same index, 31-45 dB one frame
  off).
- Concurrent encodes hardly raise throughput at Fast: 18.2 s for one plate,
  15.1 s a plate for two and 14.7 s for four. At Background, which caps the
  encoder's threads, four concurrent encodes take 21.6 s a plate against
  75.7 s for one. CPU decode of the source is the ceiling at Fast, so there
  is no encoder pool.
- Live in the Release editor with an empty cache: a clip showed its proxy
  about 9 s after launch (including about 5 s of startup), and 18 s of
  playback across segment boundaries drew a picture in every capture, with
  no banner.

Still open from this phase:

- Segments stay in the cache after the join until its size limit trims them,
  and materialised segments stay in the session folder until the session
  ends; delete both once the joined proxy is materialised and nothing reads
  them.
- A backward jump onto another file of the same asset keeps the old picture
  until the new one decodes, which the same-file path deliberately does not.
- The Asset Viewer's requested frame does not yet steer the order.
- Joining reads every segment back from the cache to a work file before
  remuxing; about 1.6 s of the 20 s for a two-minute plate.
- Segments made at different priorities may carry different stream headers
  (encoder presets), which the join refuses; the worker then encodes the
  whole file again. A run that starts or stops play now ends at a segment
  boundary, so a playback toggle mid-proxy can trigger this. Not yet
  measured.
- Decode on the GPU where the source codec allows it (D3D12VA or NVDEC for
  H.264, HEVC, AV1), scale on the GPU, and hand frames to NVENC without a CPU
  round trip. ProRes, DNx and other software codecs keep CPU decode but
  parallelise by segment instead of by one long stream.
- Use a short GOP for the editor profile, about 0.5 s or intra-only, chosen
  from measured scrub cost and file size, so random and backwards seeks decode
  at most a few frames.
- Make the editor cache scrub and play backwards. Today a drag at four frames
  per tick holds a previous frame on every render, and reverse play draws no
  exact frame at all, for 4K sources and 720p proxies alike. A scrub should
  show the nearest decoded frame at or before the request instead of holding
  the last one; reverse play needs GOP-wise backwards decoding with the
  decoded GOP retained. Proxies then make both cheap: a 720p GOP is about a
  ninth of a 4K one.

**Phase 3: image sequences and alpha.**

- Image sequences get a per-frame editor rendition rather than a video
  stream: each source frame scaled to fit 1280x720 and stored as an
  individually addressable frame, so it is usable the moment it is written and
  frames can be produced on all cores in any order. Scheduling follows the
  phase 2 rule, starting at the playhead.
- Store it in a GPU-ready format that keeps the source's range: BC6H (or
  half-float) for float and HDR sources such as EXR and HDR, BC7 for 8-bit
  sources with alpha, keeping the source's colour metadata. The editor then
  uploads compressed blocks with no decode.
- Sources with alpha (ProRes 4444, PNG and EXR sequences) need an editor
  rendition that keeps alpha. Use the per-frame path for sequences; for
  video, a second alpha plane or an alpha-capable codec chosen by measurement.
  Until then, an asset with alpha stays Source in the editor rather than
  compositing without its matte.
- Lift the "Video only" rule in [scene.hpp:1849](engine/scene/include/mediaserver/scene/scene.hpp:1849)
  for the editor rendition, not for nodes.

**Not in scope.** Nodes using a 720p rendition, and any automatic downgrade on
a late node: a node that cannot keep up reports and holds by policy. The
existing authored Require Proxy with `proxy-source-cfr-hevc-main10` remains the
route for a higher-quality node rendition (for example ProRes to GPU-decodable
10-bit HEVC); a node-side automatic source-resolution proxy is a separate
future decision.

**Measure first, and after each phase.** On the RTX 3090 Director and the RTX
2060 node, with representative 4K H.264, 4K HEVC 10-bit, 4K ProRes 422 HQ and
a 4K EXR sequence:

- time to the first proxy frame under the playhead after import, and time to
  a complete proxy;
- editor frame time, decode and upload time and held frames while scrubbing
  four 4K plates, Source versus Proxy;
- editor frame-loop impact while generation runs at each transcode priority;
- forward and backward scrub latency against GOP length.

**Baseline, 2026-10-28** (RTX 3090 Director, Ryzen 7 5800X, Release; the
2060 node and the frame-loop impact of generation are not yet measured).
Generation uses `mediaserver_proxy_encoder_benchmark SOURCE CACHE OUT
proxy-720p-cfr-h264 PRIORITY`, a fresh cache per run:

| Source (4K) | Length | Background | Fast |
| --- | --- | --- | --- |
| H.264 High 8-bit, 82 Mb/s | 122 s | 75.6 s (39 fps) | 18.1-18.4 s (171-177 fps) |
| HEVC Main10 | 20 s | 12.5 s (39 fps) | not run |
| ProRes 422 HQ 10-bit | 20 s | 16.2 s (30 fps) | 4.0 s (128 fps) |
| EXR half sequence | 4 s | no proxy path | no proxy path |

Editor decode uses `mediaserver_render_media_video_benchmark --d3d12`, which
records Program and Stage through one media cache as the editor does.
*Seek* is the time until both renders draw the exact frame after a jump
(`--seek-targets 20`); *play*, *scrub* (`--stride 4`) and *reverse* report the
share of renders at 24 fps that drew the exact frame:

| Clip | Seek mean / p95 | Play | Scrub x4 | Reverse | Peak VRAM, play |
| --- | --- | --- | --- | --- | --- |
| H.264 source | 149 / 220 ms | 100% | 0% (94% held) | 0% | 1.8 GB |
| H.264 proxy | 74 / 93 ms | 100% | 0% (all held) | 0% | 0.28 GB |
| ProRes source | 144 / 156 ms | 85% | 0% (none drawn) | 0% | 0.25 GB |
| ProRes proxy | 78 / 95 ms | 100% | 0% (all held) | 0% | 0.28 GB |
| HEVC Main10 source | never ready | 0% | 0% | 0% | - |
| HEVC Main10 proxy | 77 / 95 ms | 100% | 0% (all held) | 0% | 0.28 GB |
| EXR source | 196 / 218 ms | 0% | 17% | 0% | 0.06 GB |
| 4 x H.264 source | 559 / 825 ms | 88% | 0% (none drawn) | - | 2.6 GB |
| 4 x H.264 proxy | 287 / 406 ms | 100% | 0% (all held) | - | 0.35 GB |

What the baseline says:

- The proxy halves seek time, keeps four plates at full playback, and cuts
  playback VRAM by 5-7x (5.5 GB to 0.47 GB for four plates while
  scrubbing).
- Fast generation already outruns realtime by 5-7x; Background does not
  (1.2-1.6x). Thread count, not NVENC, is the limit.
- Scrub and reverse fail for every rendition; that is cache behaviour, not
  resolution (phase 2).
- 4K HEVC Main10 never decodes on the Advanced renderer's device: D3D12VA
  reports `Could not create the texture` for every frame, while the Basic
  renderer plays the same file at 98% and the proxy encoder reads it. This is
  a decode defect independent of proxies.
- A 4K EXR sequence cannot play at 24 fps from source (about 200 ms a frame),
  which makes phase 3 the largest editor win for EXR plates.

**Tests.** Rendition switching never moves a cut, keyframe or placement
(fractional rates included); nodes and their digests are unchanged by the
editor preference; an authored Prefer or Require still overrides it; segment
fallback draws source for unready segments and the proxy for ready ones on the
same clip; segment priority follows the playhead; a provisional key is
adopted only when the final fingerprint matches, and is discarded when the
file changed; alpha sources never draw without their matte; eviction never
removes a drawn rendition. Each workflow is exercised in the shipping editor.

#### External effect extensibility

- OFX and GLSL support remains deferred until plugin identity/versioning,
  parameter schemas, deterministic node distribution, shader sandboxing,
  resource bounds, offline/missing-plugin behavior, and render-node capability
  negotiation have an end-to-end design. Neither path may bypass the effect
  registry or turn an unsupported processor into a silent no-op.

### Exit criteria

Every new authoring operation has a deterministic document representation,
clear undo behavior, focused model tests, and an answer for how its assets and
render requirements reach every selected node.

## virtual-production and professional interfaces

### Problem

The current renderer provides a local scene/output workflow. It is not yet an
ICVFX system or a broadcast/live-event I/O platform.

### Deliverable

Add external interfaces only after their ownership, timing, media contract, and
failure behavior are designed end to end.

### Candidates

- A vendor sync-device provider behind `net::ClusterSyncStatusProvider`
  (NVIDIA G-Sync via the NVAPI GSync topology/status functions, and the AMD
  equivalent), and a native D3D12/Vulkan presentation path behind
  `rhi::PresentBarrierBackend` so swapchains can actually join a swap group.
  Both seams exist and are exercised; only the vendor implementations are
  missing, and until they land a physical sync policy correctly reports
  SWAP BARRIER INCOMPLETE.
- A platform PTP/IEEE 1588 time provider for clock correlation, scheduled
  deadlines, and telemetry. It replaces neither genlock nor swap lock.
- LED processor and camera-chain reference integration plus a measured
  validation pass: a synchronized test pattern and high-speed camera or
  measurement rig. Software-reported GPU lock does not prove that the emitted
  photons are aligned.
- Camera tracking, calibrated camera/frustum workflows, and inner-frustum
  composition.
- SDI, NDI, ST 2110, audio, and GPU/video-device paths with a well-defined
  clock and color contract.
- Managed color/HDR/calibration integration where it belongs in the output
  chain, including the boundary with the LED processor.
- Plugin or external-render texture interfaces with explicit device/API and
  synchronization ownership.

### Exit criteria

Each interface has a supported-device matrix, an integration test or hardware
test plan, a documented clock/color model, and a failure mode that cannot
silently put an incorrect image on Program.

## Specific hardware API integration and control

### Problem

The current Hardware Map provides some basic generic pieces of hardware, for example "led processor." The goal is to 
integrate the APIs and SDKs for specific hardware, allowing users to, for example drag a "Novastar Coex LED Processor"
onto the hardware map. The software would be able to provide a much richer information about the hardware status, 
which ports are active, etc. as well as flag any potential issues such as refresh rate or bit depth mismatches.

This would also allow for remote control of hardware, for example if such a bit depth mismatch is detected, a "resolve"
button could be clicked by the user to automatically fix the configuration. 

### Deliverable

Add hardware devices with their APIs or SDKs, make configuration as transparent to the user as possible

### Status

NovaStar COEX is in (`engine/devices`): a processor card speaks it from the
palette, a search finds controllers on the local subnets, the Director polls
each one's inputs, EDIDs and screens, cables into it are checked for EDID,
mode and signal, an input's EDID can be set to the output's mode, and Feed
Mapping builds a starting layout from what the processors report. See
[the operator guide](docs/operator-guide.md#hardware-map-and-led-processors).

Brompton Tessera is in too, through the IP control API of software 3.1
(port 80): the active input, panels, overrides, preset and model, mode and
signal checks, and search. Tessera's API has no EDID and no screen layout, so
a Brompton card offers no EDID fix and Build from hardware lays its windows
side by side. Its answers were built against the 3.1.0 beta 4 manual, whose
examples print keys unquoted; both forms are read. Verify against a real
processor, and map inputs other than the active one if a later API names
them.

Still to do for COEX: verify field meanings against real MX40 Pro firmware
(layer positions are taken as screen coordinates, cabinet positions as
canvas-relative, and an input without `sourceStatus` as live when it reports
a size); output bit depth and frame rate checks; finding controllers through
the ARP table (NovaStar OUI `54:b5:6c`) as well as by subnet search.

### Candidates

- Megapixel and Colorlight LED processors (a protocol each, beside
  `HardwareProcessorProtocol::NovastarCoex` and `BromptonTessera`)
- Blackmagic Hyperdeck and Aja Ki Pro recorders
- Evertz and Brainstorm master clock generators
- File storage servers and cameras on the map, each a device kind with its own
  sprite

## Particles and volumes (`libs/ajfx`)

### Problem

The renderer has no particle or volumetric effects beyond the froxel fog, and
there is no off-the-shelf GPU particle library whose simulation is
deterministic. Here that matters more than usual: every node must draw the same
particles at a given show frame, and a timeline seek must land on exactly what
playback would have shown.

### Deliverable

Three pieces with one file format between them. The media server plays
effects; it does not author them.

- **`libs/ajfx` runtime**: a GPU particle and volume library with no media
  server dependencies, published on its own once its API settles. It loads
  effect packages, simulates and renders them. Effects are stacks of typed
  modules; custom modules are HLSL snippets fused into each emitter's kernel.
  A C++ builder API builds the same model in code, for tests, presets and
  engine programmers. The library defines a small GPU backend interface and
  can instead hand a host the passes to record into its own render graph; the
  media server adapts its RHI that way. D3D12 and Vulkan reference backends
  ship with the library.
- **`.ajfx` effect package**: a versioned, self-contained file holding the
  emitters and module stacks, the textures and meshes they use, the effect's
  published parameters (name, type, range, default) and kernels precompiled
  with the pinned DXC for each backend, with HLSL source as the fallback.
  Precompiled kernels keep shader compilation off the media server's load and
  frame paths and give every node identical bytecode. A package is identified
  by content hash, so it is an ordinary asset with a digest.
- **ajfx Studio**: a separate authoring application built on the same runtime,
  with no media server dependency: module stack and parameter editing, a
  scrubbable preview that matches playback exactly because the simulation is
  deterministic, budget and GPU cost readouts, and export to `.ajfx`. A
  node-graph view can sit on the module model later.

In the media server an effect is a library asset. Operators place instances,
set and animate the effect's published parameters in the Inspector, and the
Director adopts a changed package on a background worker, so an artist can
iterate in Studio while the stage plays.

Determinism rules: randomness is an integer hash of `(emitter seed, spawn
index, stream)`; fixed simulation step; live lists compacted by prefix sum,
never atomic append; budgets fixed up front and refused when exceeded. Seeks
restore from a checkpoint ring and resimulate forward; emitters with no
feedback forces evaluate any frame directly.

### Phases

0. Skeleton: standalone CMake project, `area-particles` test area,
   deterministic hash with an HLSL twin. Done.
1. Effect model, attributes, fifteen built-in modules, builder API, presets,
   published parameters, the `.ajfx` package format (read and write, version
   checked, refused with a reason when unsupported) and a CPU reference
   simulator with checkpointed seeking. Tests: bit-identical reruns, seek
   equivalence, budget refusal, package round trip. Done.
2. GPU runtime on the D3D12 reference backend.
   - 2a, done: per-emitter fused update and spawn kernels generated from the
     module stacks, records holding only the attributes in use, a two-level
     prefix-sum compaction, indirect dispatch, checkpoints, a headless D3D12
     device and a DXC compiler loaded at runtime. Spawning, deaths, ages and
     lifetimes match the CPU reference exactly and the rest within
     tolerance; reruns are bit-identical; clean under the D3D12 debug layer.
     About a million particles take 1.9 ms a step on an RTX 3090, untuned.
   - 2b: sprite drawing (graphics pipelines, indirect draw, depth-sorted
     alpha), precompiled kernels carried in `.ajfx` packages, and a sample
     application. Tuning candidates: structure-of-arrays records, writing
     survivors straight to the compacted buffer, a single-pass scan, and
     batching barriers across emitters; measure with GPU timestamps first.
3. RHI: writable storage buffers in compute and indirect dispatch/draw (shared
   with the GPU-driven culling item above).
4. Integration: `.ajfx` import as a library asset (content-hash identity,
   parsed and its kernels created on a worker, never on the frame path), an
   RHI adapter pass in `engine/render`, and a `scene.particle_effect` node
   referencing the asset with its parameter values normalized, replicated and
   digested. Show-frame driving and seeking, placement by dragging from the
   Library, published parameters in the Inspector with keyframes, and
   adoption of a changed package. The Basic renderer fails readiness with a
   specific reason. Two-node agreement on the cluster.
5. GPU sort, ribbons, mesh particles, particle lights, soft particles, motion
   vectors, and depth and SDF collision.
6. Custom HLSL modules, data interfaces (depth, SDF, meshes, textures, curves,
   media) that a host binds to an effect's named inputs, debug statistics and
   profiling markers.
7. ajfx Studio: a standalone application on the D3D12 reference backend with
   module stack editing, preview with transport and scrubbing, published
   parameter editing, cost and budget readouts, and `.ajfx` export. A first
   usable version can start once phase 2 renders sprites; it grows with the
   runtime.
8. Publication: Vulkan backend, documentation, examples, presets, Studio
   builds, a license and versioned releases.

Volumes track (after phase 5), a companion module sharing the same backend,
hashing, budgets and checkpoints:

- V1: procedural volumes (noise and SDF shapes) injected into the froxel fog.
- V2: per-volume raymarching with self-shadowing, composited with sorted
  particles.
- V3: cellular flood-fill volumes through a voxelized scene (CS2-style smoke).
- V4: sparse smoke and fire simulation, with particles depositing into the grid
  and advecting in its velocity.

Volumes are simulated once in world space and raymarched per node against the
full-canvas camera, like the froxel grid. Ray jitter is hashed from the show
frame and canvas coordinates; any temporal history resets with the temporal
document digest. They need true 3D textures in the RHI.

### Exit criteria

An effect authored in ajfx Studio and exported as `.ajfx` plays in the media
server exactly as it previewed, is reproducible bit for bit on rerun and after
a seek, matches across a two-node slice boundary, stays within its declared
budget or reports why not, and has a measured GPU cost. Loading or replacing a
package never stalls the control or frame path.

## Vehicle process platesets

### Problem

The LED Car Process node arranges up to seven plates (Front, four
three-quarter/side, Back and Ceiling Reflection) around a vehicle, but each
plate is still an independent clip, layer and wire. An operator has to import
the camera angles, line them up by hand, keep every trim and move in step
across tracks, and wire each Scene Layer to the right socket. Changing to
another take repeats all of it. Import already records each clip's
`media_start_timecode`, but nothing uses it to align plates.

### Deliverable

A plateset: a library asset whose slots hold the camera-angle media for one
take, placed on the timeline as a single clip and wired to an LED Car Process
rig automatically.

- Slots use the LED Car Process socket roles, taken from one shared
  vocabulary. Rig templates (9, 5 and 3 camera) choose which slots exist.
  Dropping several media assets onto an empty plateset proposes slot
  assignments from file names and metadata (`FRONT`, `CAM_B`, reel names);
  the operator corrects them by dragging.
- Per-slot sync offsets belong to the plateset, not to a timeline placement,
  so every use of a take is in sync. They affect the image, so they are
  normalized, replicated and included in the scene digest.
- Readiness is checked when the plateset is built: mismatched frame rates or
  drop-frame modes, no common overlap, and an empty Front slot fail with a
  specific reason. The plateset estimates its decode load against the
  measured budget and offers derivative or proxy profiles when it is over,
  rather than failing on stage.
- On the timeline a plateset clip moves, trims, slips and splits every plate
  together. Expanding it shows a row per slot for local corrections (grade,
  mask, transform), which stay layer-local as LED Car Process slot transforms
  already do. Swap Take, or dropping another plateset onto the clip, replaces
  every plate in place and keeps trims, effects and wiring.
- Dropping a plateset into a scene with no LED Car Process creates one, wires
  each filled slot to its socket and connects it toward Output in one undo
  step through the Director's document authority, and highlights the new node
  in the Compositor. An existing rig with free sockets is reused; an occupied
  rig asks whether to replace its plates or add a second rig. Preferably the
  node takes the plateset clip as a single input, so swapping takes or adding
  a slot never rewires the graph.
- Sync by Timecode, on the plateset's context menu, aligns slots by their
  start timecode and trims to the span every plate covers. Mixed frame rates
  are refused with the reason rather than rounded. The expanded clip shows
  each slot's offset and the common span.
- Footage without timecode is synced from the plates themselves, without a
  model: every camera on the rig sees the same bumps, so frame-to-frame
  vertical motion correlates across slots to the frame; yaw from turns,
  brightness steps under bridges and tunnels, and scratch audio confirm it.
  Checking the start and end of a take reports drift between cameras that
  share timecode without genlock.
- The same motion signal proposes markers (turning left/right, stopped,
  accelerating, rough road, tunnel). Analysis runs on a bounded Director-side
  worker, never on the control or frame path, and is cached beside the asset
  by content hash. Results are suggestions; they become authored markers only
  when the operator accepts them.

### Status

Phases 1 to 4 are in place (see the operator guide's Vehicle process
platesets): the plateset asset with slots, rig templates, name-based
proposals, readiness and the decode budget; placement as a track and clip
per plate wired into an LED Car Process with the rig choice; take editing
and Swap Take; Sync by Timecode with offsets that placed plates follow;
background motion analysis with confidence, drift and suggested markers;
and the Timeline's take row, which folds a placement's plate tracks into one
collapsible row whose bars move, trim and cut each take, whose eye, solo and
lock switch the whole set, whose menu folds, cuts, ranges, opens and
deletes it, and which drags to reorder its tracks as a block.

Still open:

- Motion sync relies on shared shake. On the stabilised Chicago array in
  `Models/media/driving` it finds the plates essentially in sync but with
  low confidence; a stronger shared signal (optical-flow speed profiles
  that tolerate stabilisation) would help, measured against plates with a
  known offset.
- Phase 5, the optional vision model for semantic tags, is not started.
- Ripple, roll and slide on a plate act on that plate alone.

Follow-on ideas, each to be designed on its own:

- Interactive lighting: sample each plate's edges and average colour to drive
  emissive light cards or DMX fixtures, so passing headlights and tunnel
  lights reach the actors in sync. Shares the light-card work above.
- Motion curves from the detected bumps and turns to drive a motion base,
  camera shake or lighting flicker.
- A Back to One cue that returns every plate to the take's start mark.
- A loop finder for points where all plates match closely enough to loop a
  drive under a crossfade.
- Event search and, where cameras recorded GPS, a route map that scrubs the
  plateset.
- Flip Drive: swap left and right slots and flop them to fake the other side
  of the road or a right-hand-drive car.
- A Stage overlay showing plate seams while tuning Horizontal Blend and Array
  Scale.

### Exit criteria

A nine-camera take can be imported, assigned, synced and placed with its rig
wired in a handful of gestures through the shipping workflow, each one undoable
as one step. Sync offsets reach every node and the digest, two nodes agree on
the composed rig at the same show frame, an over-budget plateset reports why
before it goes live, and analysis never stalls the control thread.
