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
  the pasted duration. Overwrite leaves later clips fixed and trims/splits any
  overlapped clips at the paste boundaries; it never silently deletes a
  partially covered remainder. Each mode previews its affected span before
  commit.

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

### Candidates

- Novastar Coex, Brompton, Megapixel and Colorlight LED processors
- Blackmagic Hyperdeck and Aja Ki Pro recorders
- Evertz and Brainstorm master clock generators
