# Advanced Renderer Integration Status

This document records implemented behavior through 2026-09-07. The
recovered proposal and the longer roadmap remain in
[advanced-renderer-integration-plan.md](advanced-renderer-integration-plan.md).
Items in that proposal are not capabilities unless they are described as
implemented here.

## Camera tracking and inner/outer frustum

The replicated spine of
[01](advanced%20icvfx%20renderer/01-camera-tracking-and-inner-outer-frustum.md)
exists. A tracked camera pose reaches every node on a per-frame channel, every
node derives the same inner-frustum rectangle from it, and a GPU pass composites
an inner-frustum render over an outer-frustum one with a feather that is
byte-identical across a node boundary.

- `engine/track` is a new module with **no I/O of any kind** - no socket, no
  file, no clock, no thread. That is the structural guarantee that a render node
  cannot read its own tracker: there is nothing in the library that could, and a
  node sampling its own feed would get a different pose from its neighbour and
  open a seam mid-wall. It contains a FreeD D1 parser (29-byte datagram, protocol
  checksum, refused whole on any fault), an OpenTrackIO JSON sample parser
  (validating - a truncated datagram fails at the truncation rather than being
  accepted because the interesting fields arrived first), a sequence-counted
  `PoseSlot` for the tracker-thread-to-frame-boundary handoff, and `PoseChannel`,
  which holds the last pose for a show frame with no pose and never extrapolates.
- `scene::TrackedCamera` rides the evaluated scene beside `camera`. It is
  per-frame control data, not document state: tracking written into the document
  would bump `document_version` every frame, and document version is the
  temporal-history epoch, so every frame would reset history and permanently
  defeat temporal antialiasing. It joins `scene::digest` only once a pose is
  live, so a project with no tracking digests exactly as it did before.
- `scene::IcvfxSettings` in `RendererSettings` carries the authored margin, soft
  edge, latency compensation in frames, and inner-render resolution scale. All of
  it is clamped once at the model boundary, persisted through the document, and
  folded into `renderer_settings_digest` - conditionally, so an existing profile
  digest is unchanged until the inner frustum is switched on.
- `scene::inner_frustum_rect` derives the rectangle with outward rounding, beside
  `window_for_slice` where the bit-exactness discipline already lives.
  `output::inner_frustum_region` intersects the tracked frustum with a flat LED
  surface, and a second overload bounds it conservatively against an arbitrary
  projection mesh. A frustum the wall does not bound returns the whole raster,
  marked clamped: too much inner frustum costs pixels, too little is a hard cut
  inside the plate.
- `TrackedCameraPose` rides the per-frame
  Director-to-node stream - not `Heartbeat`, because a pose must be aligned to an
  exact show frame rather than merely recent, and not a document command for the
  reason above. It consumes the Director outbound sequence so a node can prove no
  pose was dropped, and it is acknowledged by nothing: a session that misses one
  holds its previous pose, which every node agrees on, rather than receiving a
  resend a frame late. A v3 node is refused at the handshake rather than
  rendering the authored camera while its neighbours render an inner frustum.
- The Director is the only machine that reads a tracker, and it replicates what
  it reads. `apps/server/tracker_listener` binds each enabled feed's UDP port on
  its own thread and parses into a `track::PoseSlot`. The Director samples those
  slots **at its frame boundary**, in the same unconditional block that refreshes
  the scene and sync projections "even on a Director with no editor" - so a
  headless `--role=director` tracks exactly as one with an editor open does, and
  the frame a pose is stamped for is the frame it was sampled on, which is what
  makes the authored latency compensation a fixed offset rather than an
  approximate one. Sampling and replication happen *before* the poses are
  injected into the Director's own scene, which they have to: injecting first
  would draw and digest last frame's stack while the nodes drew this frame's.
  A pose is stamped against the **control frame**, not the show frame. The show
  frame stops advancing the moment an operator parks the transport - which is
  exactly when they are lining up a shot and most want the wall to track - and
  a channel that saw the same stamp every frame would report every sample as a
  repeat and freeze the plate while the datagram counters kept climbing. The
  control frame is the replication clock the frame barrier already holds nodes
  together on, so it advances whether or not the show is rolling. nDisplay
  reaches the same place by tracking per render tick rather than per timeline
  position.
- The session protocol is version **7**: one `TrackedCameraPose` carries every
  tracked camera's pose for one control frame, in ascending camera order. One
  message rather than one per camera because the stack has to land together - a
  node that applied camera A's new pose over camera B's previous one would
  composite two moments into a single frame while its neighbour split them the
  other way. `DirectorFrameBarrier::publish_tracked_pose` fans one sample
  out to every live session, and the session service drains and sends it, so a
  pose leaves the Director once and every node is given identical bits. The pose
  is injected into the Director's own scene before `authored_scene_digest` is
  taken, because the digest is the cross-node agreement check and a Director
  that digested a scene without the pose it just replicated would disagree with
  every node that received one. The status line reports the link, the
  accepted/received counts, and the held frames, because a headless stage
  machine has no panel to look at.
- `render::plan_inner_frustum` is the per-node derivation. The only per-node
  input is the slice, and it is used for exactly one thing: the rectangle
  intersection that decides whether this node renders the inner frustum at all.
  A node that misses it does the second render not at all and counts the skip.
- `render::tracked_stage_camera` is the one definition of where the physical
  camera is this frame. The presenter derives the rectangle the plate occupies
  on the wall and `IcvfxStage` renders what goes in it; both ask this, so a
  bound camera's own lens - whose entrance-pupil offset moves the eye - cannot
  move one without the other. `render::IcvfxStage` is what `sdl_presenter`
  records per mapped output: it plans, renders the inner frustum once at the
  authored scale, and composites it over the scene target, returning the scene
  target untouched whenever there is nothing to draw.
- `render::IcvfxCompositor` composites inner over outer. It reconstructs the
  absolute canvas coordinate from `SV_Position` rather than from the interpolated
  uv, because the uv round trip is exact only for power-of-two extents and a wall
  is not obliged to be laid out in powers of two. The inner frustum's
  `history_identity` is stable by construction, so a tracked move or zoom
  produces motion vectors instead of resetting temporal history every frame.
- **A camera is the thing an operator adds, and tracking is a section on it.**
  Cameras are in the Library beside Box and Plane and appear in the Scene list.
  A camera is placed and aimed - eye, target, and angle - rather than translated
  and rotated, because it reads none of the transform properties every other
  element uses and nine fields that move nothing is the failure a clip on a
  screen is already protected from.
  Its TRACKING section carries everything about its plate: which feed drives it,
  whether it draws an inner frustum, its overscan, its softness, its plate
  density, and its render order. Per camera and not per show, matching how
  nDisplay puts `bEnable`, `CustomFrustum`, `SoftEdge`, `BufferRatio`, and
  `RenderOrder` on `FDisplayClusterConfigurationICVFX_CameraSettings` - a wide
  master needs more overscan than a long lens, and a witness camera needs no
  plate at all.
- **A tracker feed is project state, not an object on the stage.** It has no
  transform and nothing to select; it is a socket and a lens, the way nDisplay
  names a LiveLink subject rather than putting the tracker in the level. Feeds
  are authored from the camera that wants one - "+ New feed" in TRACKING - and
  the bound feed's own settings (listening, protocol, port, camera id, sensor,
  fallback focal length) appear inline underneath, addressed to the feed's node
  while the selection stays the camera. `SceneModel::tracking_sources` is what
  the Director's `TrackerListenerPool` opens sockets from: one per enabled feed,
  in document order, so two machines open the same sockets in the same order.
- **Several cameras, several plates.** `Scene::tracked_cameras` carries every
  bound camera, sorted by render order and then node id, and that sequence *is*
  the composite order - bigger draws over, which is the convention every stage
  operator already knows. The order is digested as a sequence rather than a set,
  because two nodes compositing the same plates the other way round disagree
  wherever two overlap. `IcvfxStage` walks it bottom-first with two compositors
  alternating, so a stack of any depth is built without a compositor ever
  sampling the target it is writing.
- **Project Settings TRACKING holds what is genuinely show-wide**: the master
  switch that takes every plate off the wall at once, the latency compensation
  (which describes the wall and the wire, not one camera), and the values a
  newly added camera starts from. Those are defaults and not policy: once a
  camera exists it carries its own, and editing the project's never reaches back
  and changes one an operator already tuned. The settings node no longer carries
  a tracker at all; `SceneModel::tracking_source` is gone rather than left as a
  second place the same thing could be authored.
- **The operator can see it.** A docked **Live Data** panel reports the link
  state, the datagram and refusal counters by kind, the last pose in degrees
  and metres, the
  resolved field of view, the hold counters, and the plate rectangle with the
  fraction of wall it covers. The Stage viewport draws the tracked camera's
  frustum - dimmer while a pose is held - and both Stage and Program Output
  outline the plate and its feather. `scripts/send-freed-tracking.ps1` emits
  protocol-correct FreeD D1 datagrams, with an orbit that makes wrong parallax
  obvious and a `-Corrupt` switch that exercises the refusal counters, so all of
  this is drivable with no tracker on the desk.
- `deterministic::tan_half_turns` now applies its quadrant, and
  `deterministic::atan_turns` is its inverse. The tangent previously returned a
  negative value for any field of view at or past 90 degrees, which `valid()`
  refused - so no camera that ever rendered changes, and a short lens on a
  volume is usable instead of being rejected. The arctangent is what turns a
  sensor height and a focal length into an angle, deterministically, which is
  why it is here rather than reached for from the C library.
- `apps/server/tracked_camera_bridge` is the one place the tracker, the wire, and
  the scene meet, since each of the three libraries deliberately knows nothing
  about the other two. It carries the Director's latency compensation - authored
  in frames, applied once, never machine-local - and turns a channel answer into
  the `scene::TrackedCamera` the renderer evaluates.

Open decisions this settles, from that document's own list: `camera_history_digest`
needs no change, because tracked field of view lives in `TrackedCamera` and never
enters it, and the inner view's history identity is stable rather than
pose-derived. Inner-frustum resolution is a fixed authored rational, not derived
from angular coverage, because a derived resolution changes with the pose and
would reallocate a render target mid-show - so each camera authors its own
rational instead. Multiple simultaneous inner frusta are no longer deferred:
`Scene::tracked_cameras` carries up to `max_tracked_cameras` of them and the
stage composites the stack in render order.

A note on topology, since it is the obvious question. `Role` is a bitmask, so
`--role=director,render` is a render node that is also the replication
authority - which is what nDisplay calls the primary, and the same physical
arrangement. Whoever stamps a pose has to own the frame clock, and here that is
the Director; putting tracking anywhere else would mean moving the frame clock
too. What this codebase cannot express is the other half of that arrangement:
there is no editor-as-remote-client role, because `--edit` requires
`--role=director`. An operator machine is therefore always the authority, and
detaching the two is a larger piece of work than tracking.

What is **not** yet true, and must not be inferred:

- Only a flat surface gets a plate. `sdl_presenter` records the inner frustum
  for a mapped output whose surface is planar; a curved wall or an arbitrary
  projection mesh shows the outer frustum alone. That is honest rather than
  finished: the conservative mesh bound exists in `output::inner_frustum_region`
  and the per-node inner render does not yet use it.
- The tracker still runs on the Director alone, which is the design, but the
  Director is also always the operator machine: `--edit` requires
  `--role=director`, so there is no editor-as-remote-client. See the topology
  note above.

## Selection and ownership contract

`render::HybridSceneRenderer` is the scene-renderer selection boundary. It
accepts an evaluated `scene::Scene`, requested canvas slice, output extent,
render options, and a complete temporal-history identity. It returns the
selected backend, scene color, depth, velocity, object ID, coverage, temporal
disposition, and readiness result.

The host owns the RHI device, renderer, windows, swapchains, document, and
clock. The renderer is offscreen and never owns an application loop. Products
stay on the device which created them and reach a swapchain only through the
application presenter.

| Requested policy | Implemented behavior |
| --- | --- |
| Basic | Uses the regression/basic `SceneRenderer` on the host device. A scene-capable Windows presenter can run it on the preferred D3D12 device; SDL_GPU remains the fallback device path. |
| Advanced raster, optional | Uses the native D3D12 raster renderer, including its cascaded first-directional-light shadow and reflective-shadow-map diffuse-bounce passes, only when the complete current product set is constructible; otherwise reports an explicit Basic fallback. |
| Advanced raster, required | Returns Ready only for the complete current D3D12 raster, cascaded directional-shadow, and bounded diffuse-GI slice. Missing capabilities or construction failure returns `CapabilityUnsupported`; it never silently selects Basic. |
| Advanced plus RT, DDGI, ReSTIR, denoise, or TAAU | Not implemented. A required request reports `REQUESTED ADVANCED FEATURES UNAVAILABLE`; an optional request reports Basic fallback. The message identifies a software/product boundary rather than blaming the adapter. |

There is no path which records the Basic renderer while reporting Advanced as
selected.

## Deterministic scene and temporal state

- The bounded `SceneBridge` is keyed by document and asset identity. It stages
  a diff with `prepare()`, and the facade commits it only after the selected
  renderer has accepted the complete scene record. Failed recording does not
  commit; the next `prepare()` discards and replaces the abandoned staging
  while the committed bridge state remains unchanged. It never opens or hashes
  an asset's bytes.
- A temporal key includes scene, document version, surface and surface ID,
  authored canvas and slice, target extent, camera source, camera projection
  and exposure, stable mapped-view identity, and renderer profile. Camera pose
  is deliberately excluded so sequential camera animation produces motion
  vectors instead of resetting history.
- Sequential show frame `N + 1` advances. Repeating an unchanged frame `N`
  holds the same previous/current pair. A seek, camera cut, resize, mapping or
  profile change, document edit, topology change, or changed repeated frame
  resets. History decisions and reset counters contain no wall-clock input.
- The raster renderer retains current and previous object and camera
  transforms in bounded storage. Initialize/reset records zero velocity;
  Advance records motion; Hold reproduces the original frame's velocity rather
  than advancing it again.
- A renderer preallocates 256 small history slots by default. The hybrid
  boundary chooses an exact key first, then the same output owner, an empty
  slot, or deterministic least-recently-used eviction. Program, Preview, Stage,
  and mapped outputs can therefore accumulate independent camera histories in
  one renderer without per-frame allocation; live slot/eviction counters are
  exposed in diagnostics.
- Output-map views use the stable output-map node identity rather than hashing
  animated matrices. A tested counter-based sample helper accepts the exact
  64-bit show frame, stable identity, and absolute canvas coordinates; no
  temporal sampling pass consumes it yet.

Current temporal limitations:

- Per-view camera state is independent, while object transforms are shared as
  two complete evaluated-scene generations. This supports every mapped view at
  show frame N before the renderer advances to N+1, which is the intended
  lockstep schedule. Recording a node at N+2 before another view has consumed
  N+1 falls outside that two-generation window and resets/fails safely rather
  than inventing motion.
- Bridge and CPU history commit after successful command recording, before the
  outer compositor, presenter, and queue submission have all succeeded. An
  abandoned outer command list rolls back native D3D12 resource state, but it
  does not yet roll back these higher-level scene/history commits.
- Document version is a deliberately conservative temporal epoch. Any project
  edit resets history, even when the edit could eventually be proven visually
  irrelevant to that surface.

## Media residency on the frame path

Cold still and image-sequence decode no longer runs in `MediaCache::resolve()`.
The default `MediaSource` scheduling policy is `BackgroundRequired`; video
sources explicitly opt into their existing nonblocking `SourceManaged` path.

- One process-wide worker services a hard maximum of 256 pending and in-flight
  still requests. Urgent visible requests are FIFO ahead of speculative FIFO,
  can promote an already queued request, and evict the newest eligible
  speculative tail.
- Each cache retains its configured request, decoded-frame-count, and decoded-
  byte bounds. Invalid, failed, and oversized results become bounded completed
  failure markers instead of causing a retry on every frame.
- Render-facing admission and publication polling use `try_lock`. Contention,
  a cold result, or a full queue returns the placement's last ready GPU texture
  or null immediately. Cache destruction and move assignment never join an
  in-flight decode.
- A command recording freezes the exact held texture (including planar layout
  or an exact null) the first time a placement misses. A second mapped view on
  that same command therefore cannot observe a replacement published by a
  different command in between. The 16-recording by 64-snapshot table is
  allocated once during cache construction and never grows on the render path;
  referenced resident textures are protected from trim until the recording
  reaches a terminal submitted/canceled outcome.
- Video registry observation also uses `try_lock`, contains no wall-clock retry
  deadline, and keeps decoder-open failure sticky. Decoder destruction is
  deferred away from render observation. The explicit video refresh seam is
  source-scoped. Still refresh advances a cache-wide epoch, cancels or discards
  all pending work for that cache, and removes completed state for the named
  source; other visible sources are then readmitted. Late work from an older
  generation or epoch is discarded.
- Work is event-driven by a condition variable. No decode scan or maintenance
  task was added to a timer.

This path is nonblocking with respect to the worker and file/codec I/O, but is
not allocation-free hard real-time: request keys and bounded STL containers can
allocate after a render-side lock is acquired. One deliberately serialized
worker also means an indefinitely blocked operating-system read stalls later
still decodes globally, while frames continue with stale/null imagery. The
process-global worker joins at application shutdown. Asset-ready time is local
state, while step 15 now gates visible Program publication on all-node readiness
and an exact replicated activation frame. Library Rescan authors the shared
refresh generation; source refresh runs on the media worker.

## Model residency

- `ModelCache` replaces render-time `load_model()` calls with a process-shared,
  bounded CPU residency. One event-driven worker opens and parses glTF/OBJ,
  referenced images, tangents, and bounds. Render observation uses `try_lock`,
  performs no file/stat/codec work, and returns pending, busy, sticky failure,
  or one immutable complete asset immediately.
- Each command list latches the first process publication epoch it observes.
  Basic, Advanced, compositor-layer, and mapped-view renderers recorded into
  that list therefore cannot reveal a worker completion halfway through one
  output recording.
- Same-source refresh is generation-fenced and retains the last complete CPU
  and GPU model until a replacement is complete. Late results are discarded.
  Published CPU generations form bounded multi-version intervals, so a command
  which latched an older publication epoch can still observe that exact asset
  after a newer refresh completes.
- CPU residency does not evict a Ready generation behind an outstanding
  command fence. Default bounds are 32 pending requests, 512 source records,
  320 live/retired decoded generations, and 2 GiB including mip pyramids. Reaching a bound refuses
  the newcomer and keeps the last complete answer. An explicit project-
  transition reset is queued to the worker, where unreferenced generations and
  source records can be reclaimed without destroying a large last-owned asset
  on the caller; externally retained generations remain until a later reset.
- GPU adoption is transactional and all-or-stale. Geometry arenas are pre-
  reserved, texture and part vectors are reserved before mutation, and the
  decoded CPU asset, candidate textures, and command receipt remain owned until
  the recording resolves. Only an accepted submission replaces the active
  model. A canceled complete candidate reuses its existing textures and mesh
  ranges while re-recording copies; a partial attempt is retained and blocked
  for that decoded generation. The material-fallback and shared-geometry upload
  flags follow the same submit/cancel outcome instead of becoming true after
  recording alone.
- Basic GPU model texture residency is capped at 512 MiB and 64 retained refresh or
  failed-integration generations per renderer. Counters expose admissions,
  drops, failures, late results, CPU residency/retirement, integrations,
  refusals, and GPU texture bytes.
- Candidate-capacity and command-receipt pressure are retryable observations,
  not sticky generation failures. A decoded generation is superseded only by a
  newer Ready generation visible behind the same command publication fence;
  a newer live request alone cannot invalidate the older immutable candidate.

This closes model file and codec I/O on the frame path, but GPU resource
creation and upload still occur while recording the first command list that
adopts a completed decode. That work is bounded and failure is stale-safe, not
yet pre-staged on an upload thread. Publication epochs remain process-local; step 15
adds an authored generation above them and gates Program on a synchronized
commit. Library Rescan now drives that generation across every node. The safe CPU-residency reset is likewise an explicit API, not
yet a coordinated host project-transition action.

## Projection-mesh residency

- `OutputProjectionCache` performs no file stat, fingerprint, byte read, wait,
  or join in render-facing `resolve()`/`resolve_mesh()`. Imported geometry is
  loaded once into immutable file-local coordinates by a process-wide FIFO
  worker with at most 64 pending/in-flight jobs. Current calibration and scale
  are applied directly during view fitting, final warp-vertex generation, and
  editor project-through; placement, size, scale, map identity, and region
  animation never reparses the file or allocates a transformed mesh copy.
- A failed load is sticky. Explicit same-source refresh advances exactly one
  generation and retains exact-source stale geometry while loading or after
  failure; a source-path change can never borrow the former mesh. Status is a
  lock-free already-computed snapshot, and reset/destruction discard late
  publication without joining an in-flight read.
- Horizontal-arc meshes are cached and unchanged resolution reuses the same
  allocation. Viewport picking/wireframes and feed-mapping UV previews use the
  same nonblocking caches; they no longer load or stat an imported mesh during
  editor draw. Panel-local caches have deterministic LRU bounds and clear at a
  project-generation boundary.

Projection meshes now consume the step 15 authored refresh generation and
Program cohort commit. UV Mesh Browse produces an immutable package; Library
Rescan coordinates same-path replacement. Local picking caches retain their
independent diagnostic refresh seams.

## Camera exposure and color

- Advanced raster applies a deterministic relative camera exposure multiplier
  proportional to `shutter_seconds * ISO / aperture^2`, followed by exposure
  compensation in stops. The existing default of f/2.8, 1/48 second, ISO 100,
  and 0 EV is the neutral multiplier.
- Exposure affects lit and emissive 3D geometry and the environment/sky in
  Advanced mode. Basic remains the unchanged regression renderer.
- Timeline media remains a display-referred compositor plate and is not camera
  exposed. Plate levels therefore do not change when a virtual camera setting
  changes.
- Advanced scene color is premultiplied scene-linear `RGBA16F`. Scene mixing,
  compositor effects which support that format, projection warp, and feed
  mapping remain linear. Tone mapping and output transfer occur at the output
  boundary.

Step 16 now converts directional lux and point/spot lumens or candela through a
replicated project light scale. Its default, pi/1000, places an ideal 18%
Lambertian receiver under 1000 lux at scene-linear 0.18 at the neutral camera
exposure above. This establishes a scene photometry reference; physical LED
wall luminance still depends on output calibration. Unitless light intensity
keeps its existing scale. Enabled colour temperature now tints authored light
colour, with 6500 K exactly neutral.

## Display transform, show look and dither

The output boundary of
[10](advanced%20icvfx%20renderer/10-display-transform-and-calibration.md) is
authored, replicated and off the frame path.

- `scene::ColorSettings` rides `RendererSettings::color`: the display transform
  (`Srgb`, the hand-checkable reference, or `AcesApproximate`, the well-known
  fit in AP1 with the sRGB encode), the `.cube` show look's path, and the dither
  switch. Struct, clamp, schema keys, parser, writer and the Project Settings
  COLOUR tab all carry it; the round-trip test authors every field away from
  its default. It joins the profile digest and the scene digest **only once
  authored**, so an untouched project's digests are unchanged.
- The transform and look are one baked 33-point table, built by
  `DisplayLutCache` - a process-shared residency worker on `EnvironmentCache`'s
  contract - and adopted by `OutputTransform::adopt_color` once per frame before
  any viewport is recorded. The frame path samples one atlas with a manual
  trilinear and never opens the file. A look that cannot be read publishes the
  transform alone under `Failed`, which the COLOUR tab reports; the compiled
  default is baked inline at construction so an output works from frame one.
- Dither is an 8×8 ordered matrix keyed to the **absolute canvas pixel**: every
  `render`/`composite` takes the canvas region its viewport shows (the slice for
  a canvas output, `map.source` for a mapped one), and two nodes rendering one
  wall as halves produce the same bytes as one node rendering it whole. It
  quantises to the target's own depth - eight bits, the swapchain this build
  presents - and is on by default; the COLOUR tab and the procedure say so.
- [10 — Measuring a wall](advanced%20icvfx%20renderer/10-wall-colour-procedure.md)
  is the written procedure: the chart scene, probe verification of the
  processor, shooting the chart, building the look on display-referred values,
  authoring it, checking every node, verifying through the camera.

Step 15 now holds Program until every required member has the requested look
generation and shares its activation permit. A missing LUT cannot publish an
ungraded replacement. Output remains eight bits until the RHI presents a
deeper swapchain.

## Atmosphere and bloom on the composite

[11](advanced%20icvfx%20renderer/11-bloom-fog-and-reflections.md) parts A and B
now run where the picture is finished. `CompositorSceneExecutor` owns a
`VolumetricFog` and a `Bloom` sized with everything else that carries the apron;
every layer renders with `defer_scene_effects` and skips both, the nearest
surface across layers is merged into one depth by `depth_merge.comp.hlsl` as
each layer lands, and atmosphere is applied once to the whole composite - sky
and every layer - with bloom over that, before the one crop. A two-layer scene
fogs its background exactly as a one-layer scene does, asserted through the
executor. `fog_frame_for` and `bloom_plan_for` are the single derivation of
which matrix and raster a frame is keyed to, called by the renderer path and the
executor alike; a view-driven frame - a plate, a mapped surface - is keyed to
its own view and planned on its own raster, which closes a defect the earlier
pass had not found: bloom authored on any ICVFX plate or mapped output refused
every frame, because the chain was planned on the canvas and never described
the plate's targets. Fog has authored height falloff on the Environment. A
refused pass names its reason in `RendererPassStatus` and on the node's stderr,
and the Renderer card prints it under NOT RUNNING. Reflections stop at the
environment by decision; ray tracing is the answer and the RHI does not have it.

## Native D3D12 raster slice

- The host-owned backend selects an adapter and owns the D3D12 device, direct
  queue, fence, timestamp heap, descriptor heaps, command-context pool, and
  native flip-discard swapchains. Command acquisition polls and returns
  `Unavailable`; it never waits for an in-flight context. No compute queue is
  exposed or advertised yet.
- A fixed 1,024-slot, allocation-free command-recording ledger publishes
  `Submitted` as soon as D3D12 `ExecuteCommandLists` accepts a recording and
  `Canceled` only when execution was never accepted and backend rollback stops
  referencing captured resources. A later present or fence-signal failure can
  therefore report submit failure without falsely releasing provisional cache
  resources as canceled; a signal failure quarantines that context. Retained
  receipts survive command-list moves, release their slots after the final
  owner, and let caches keep provisional resources private until the outcome is
  known. Ledger exhaustion is latched for that command list, so later mapped
  views cannot acquire a newly freed receipt and reveal different asset state.
- Every command context pre-creates five persistently mapped 16 MiB upload
  pages. The exact budget is 80 MiB per context: 160 MiB at the default two
  contexts and 320 MiB at the maximum four. Texture copies split across pages
  on whole row boundaries. Two 4096x2160 RGBA8 plates consume 67.5 MiB; a third
  over-budget upload is rejected atomically without allocating or waiting.
- Pages reset only after the context fence is complete and its submission
  ticket is released. A fixed texture-state journal restores speculative state
  when an unsubmitted command list is discarded. A second recording context
  touching the same texture is refused, close failure rolls back, and a signal
  failure after execution quarantines the context rather than reusing its
  upload memory unsafely.
- A pulled pass rebinds its fragment textures for every batch, but its
  samplers are the same objects each time. The command context therefore keeps
  the last fragment sampler table it built and reuses it whenever the incoming
  sampler set matches, abandoning the cached table before overwriting any of it
  so a refusal partway through cannot leave a table that compares equal to an
  unfinished one. D3D12 caps a shader-visible sampler heap at 2,048
  descriptors, and the lit pass now binds eleven per batch, so without this a
  few hundred layers would exhaust the heap and fail the frame.
- Ordinary storage-buffer uploads bind an immutable address in the command
  context's fixed pages, so multiple mapped views can upload and draw the same
  logical buffers on one command list without a later view changing an earlier
  draw. SDL_GPU cycles both transfer and destination backing for the equivalent
  case. A D3D12 payload larger than one 16 MiB page may use its persistent
  committed upload resource only when no recorded or in-flight draw captured
  that resource; a conflict is refused without waiting.
- The current raster pass uses stable GPU buffers and vertex pulling, reversed
  depth, metallic-roughness materials, local and environment lights, normal and
  occlusion maps, alpha modes, double-sided materials, and retained glTF
  tangent handedness. Missing tangents are generated during model decode.
- Advanced raster renders up to four fixed-resolution `R32F` reversed-depth
  cascade maps, sharing one `D32F` depth attachment, for the first directional
  light. The cascade count is replicated project data
  (`scene::RendererSettings::shadow_cascades`, 0-4); zero is an explicit "no
  directional shadows", not a degraded fallback. The last cascade is always the
  full-scene orthographic fit derived from the complete opaque/MASK caster set.
  Nearer cascades subdivide the authored full-canvas camera frustum - never the node's
  own output slice - with a blended logarithmic/uniform split whose far bound
  uses the caster-set diameter capped by the authored far plane. Each cascade
  fits a quantized bounding sphere so the map does not swim as the camera rotates,
  snaps its center to a scene-anchored texel grid, and takes its depth range from the
  whole scene so a caster in front of the cascade volume still rasterizes.
  Every cascade rasterizes the complete caster set; none is culled per cascade.
  A receiver blends adjacent cascades in an authored world-position-derived
  band beyond the filter's border guard. Directional and spot shadows use a
  fixed 5×5 tent kernel with manual bilinear comparison and analytic receiver-plane
  correction. The sun's angular radius adds a bounded penumbra estimated with
  nine fixed blocker samples. Optional contact shadows march twelve fixed steps
  through existing camera depth, with a separately budgeted 32-pixel apron.
  Opaque procedural meshes and opaque/MASK decoded models cast;
  opaque and composited geometry receive. Runtime stats report whether the pass
  ran, how many cascades were rendered, and how many caster instances were
  fitted. A cascade that cannot be fitted collapses the frame to the
  full-scene map alone rather than leaving a hole one node might not share.
- Per-cascade shadow resolution and the number of cascade maps allocated remain
  construction-time `RendererLimits`. They are a deployment memory bound rather
  than a look control: every node in a cohort runs the same build, so the bound
  is identical everywhere, and the authored cascade count is clamped to it
  deterministically. `RenderStats::directional_shadow_cascades` reports the
  count actually rendered, so a lowered bound is visible rather than silent.
- The same full-scene light fit - that is, the last cascade - also renders
  `RGBA16F` geometric-normal and diffuse-flux products. The scene shader
  reconstructs sixteen fixed virtual point lights from that cascade's depth and
  applies a bounded one-bounce reflective-shadow-map diffuse term. Its sample
  pattern contains no wall clock, frame-order, camera, or slice-local seed, so
  adjacent nodes use the same indirect field. The strength is replicated
  project data (`scene::RendererSettings::gi_strength`, 0-16); zero is an
  explicit conformance/off setting, never an adaptive downgrade.
- One Advanced record produces scene color `RGBA16F`, product depth `D32F`,
  motion/depth `RGBA32F` (UV motion, current Z, expected previous Z), object ID `R32_UINT`, and coverage `R32F`.
- Media auxiliary passes sample the same RGBA/P010 alpha, authored opacity, and
  horizontal edge coverage as the color pass. Zero-coverage texels do not claim
  object ID, velocity, or depth. Depth-only media uses the same composed 0.5
  coverage cutoff in color and auxiliary replays. Advanced exposes a separate
  coverage-aware product depth while retaining color-pass depth for compositor
  internals.
- The shader build pins DirectXShaderCompiler `v1.9.2607`, archive
  `dxc_2026_07_29.zip`, SHA-256
  `a1dfb116ba3eeae6a1582291b53a8e7bf65ad760676bd3194685c8f7367cd241`.
  It emits DXIL, SPIR-V with reflection annotations, and DXIL reflection
  artifacts. All local HLSL/include files participate in custom-command
  dependencies, so an included shader change cannot leave a stale linear or
  auxiliary variant in an incremental build.

Native-resource lifetime is still a caller contract rather than backend-
retained state: textures, buffers, pipelines, samplers, and readbacks referenced
by a recorded or submitted list must outlive that work. Storage payload bytes
are now snapshot-safe across draws, but destroying the logical resource before
submitted work retires is still invalid.

## Distributed readiness and presentation

- Renderer backend, required/optional policy, quality, RT feature requests,
  probe budget, slice-apron limit, directional shadow cascade count, and
  bounced-light strength are replicated project data. Every one of them
  participates in the renderer profile digest, so an edit to cascades or bounce
  strength invalidates capability evidence gathered before it and resets
  temporal history.
- Editor and output presenters publish the readiness result from their last
  real record attempt. Output nodes copy that already-computed observation into
  the existing session heartbeat; no capability probe is scheduled on a
  timer.
- Project Settings authors Shadow cascades and Bounced light beside the
  Advanced backend, availability, and profile rows, and only while Advanced is
  selected: they change what every node draws, so they belong with the
  replicated backend policy rather than in a machine-local preferences card.
- Project Settings exposes `USE RASTER` beside the Advanced profile only while
  one of the unimplemented RT, DDGI, ReSTIR, denoise, or TAAU
  requests is authored. This explicit operator action clears only those future
  requests and preserves backend, required/optional policy, quality, light
  limits, probe budget, and slice apron. The renderer gate never silently
  ignores an authored future request.
- The Director's Program promotion gate holds on missing or stale profiles,
  unavailable required backends, and mixed Basic/Advanced Program cohorts.
  Optional Advanced fallback is valid only when the complete Program cohort
  selected Basic.
- Scene-capable Windows presenters prefer the native D3D12 device and submit
  the output-transformed texture to a DXGI swapchain. If prior GPU work is not
  complete, they skip/hold rather than wait on the frame path. Basic and
  unsupported platforms retain the SDL GPU path.
- The editor records offscreen Program/Stage work and UI presentation as two
  submissions on one device. Its native D3D12 window owns four fixed command
  contexts and its SDL fallback owns three, leaving bounded headroom for the
  next nonblocking acquisition. A transient context miss preserves the last
  complete Program texture and reports `HOLD`; target, record, transform, and
  submit failures invalidate it and report their specific failure instead of
  presenting a generic black preview.
- Windows builds can opt into the NVIDIA D3D12 present-barrier path with
  `MEDIASERVER_ENABLE_NVAPI=ON`. The platform layer dynamically loads NVAPI,
  joins only native wall swapchains, owns the driver's present statistics, and
  releases its clients safely when a swapchain or backend goes away. An absent
  driver, unsupported adapter, or default build remains honestly
  `Unsupported`; it never falls back to an unsynchronised wall present.
- Under an external-genlock policy, the show frame boundary advances from the
  Quadro Sync video-reference frame counter rather than the OS clock. Lost
  reference, a left/failed barrier, or `presents_in_sync` lagging `presents`
  holds Program and is reported in node health. The editor Output Monitor also
  shows the barrier's own counters and show-clock reference state.

## Important incomplete boundaries

The following must not be inferred from the current Advanced Ready result:

- There is no native Vulkan backend.
- A tracker **is** read: the Director binds each enabled feed's socket and
  samples it at its frame boundary, and poses reach every node. An inner
  frustum now composites on the shipping output path as well, but has not yet
  been confirmed on a wall - see
  [00 — Steps 01–03 remediation](advanced%20icvfx%20renderer/00-steps-1-3-remediation.md)
  for what is proven, what is still open, and why.

  This entry previously read "no tracker is read and no inner frustum reaches a
  wall" while the camera-tracking section above described the feature in
  implemented-sounding prose. Both halves could not be true, and a reader
  believed the wrong one. **Describe reachable behaviour**: a thing that exists
  but is not called from the shipping binary is "implemented, not reachable",
  and saying so is the difference between a day of work and a month of it.
- The fixed-capacity render graph now executes. `RenderGraphExecutor` walks a
  compiled plan, applies each pass's transitions through the same journal every
  implicit transition uses, and records the pass through a caller-supplied body;
  a two-pass frame executed from a plan is byte-identical to the same passes
  recorded by hand, and the real Advanced frame's declaration compiles with its
  hazards. `GraphTransientPool` supplies frame-lifetime targets by exact
  descriptor match, refusing rather than allocating past its bound, and aliases
  nothing.
  **The production renderer still records its frame by hand.** The executor and
  its byte-identical gate exist so that migration can proceed one pass at a
  time; none of it has moved yet.
- `DeviceCapabilities::explicit_barriers` is true on D3D12 and means it:
  `CommandList::transition` puts a texture into a named state through the
  existing state journal, so a cancelled list still rolls it back. SDL_GPU owns
  its barriers internally, reports false, and refuses. A plan naming
  `GraphQueue::Compute` is refused at execution rather than run on graphics,
  because `async_compute` remains honestly false.
- Advanced and future-feature capability fields describe callable, tested RHI
  behavior rather than raw adapter potential. Advanced raster has its own
  implementation gate; SDL_GPU never reports it. D3D12 reports async compute,
  bindless access, explicit barriers, indirect draw, acceleration structures,
  ray queries, and external memory as unavailable until those operations
  actually exist, so RT/DDGI/ReSTIR requests fail readiness honestly.
- The Director paces its control frame from the video-reference counter when the
  authored policy requires an external reference, rather than always from
  `steady_clock`. Without a reference it falls back and reports that, because a
  Director that held would seal no control frames and the operator command that
  recovers from a reference fault is itself a control frame.
  `SyncReadinessCode::DirectorClockUnreferenced` refuses Program in that state
  and is named ahead of any node-level reference fault. Note the consequence: an
  ExternalGenlock show whose Director has no video reference now refuses Program
  where it previously promoted - that configuration was drifting the whole time
  while every node reported itself locked. The referenced path has not been run
  against real genlock hardware.
- Image-based lighting is half wired. `EnvironmentPrefilter` turns an
  equirectangular radiance map into SH9 irradiance, a GGX-prefiltered specular
  cubemap and a BRDF table, all through compute at adoption time and all
  bit-reproducible across instances - the diffuse projection runs in a single
  thread group with a fixed reduction order precisely so it is. `.hdr` decodes
  to RGBA16F; OpenEXR does not yet. `TextureDesc::cube` gives the RHI seamless
  cube sampling.
  `EnvironmentCache` supplies residency off a process-shared worker without the
  frame path ever waiting, the lit shader consumes both terms, and the sky pass
  draws the environment as the background. A scene with no authored map renders
  byte-identical to before, asserted rather than assumed.
  Step 15 adds the common Program-generation gate above local decode and GPU
  prefilter readiness, so a newly ready environment cannot reveal early or
  late. Physical multi-machine environment capture remains part of the hardware
  validation procedure.
- D3D12 reports `compute` and `storage_images` as available, and both now mean
  something callable. `Device::create_compute_pipeline` builds a compute root
  signature and PSO from one `cs_6_0` shader; `CommandList::dispatch` binds
  sampled textures, per-subresource storage textures, storage buffers and
  uniform bytes, inserts the UAV barrier a dependent dispatch needs, and routes
  transitions through the same journal that rolls a cancelled list back.
  `storage_images` no longer means format/resource construction only - it means
  a storage texture can be bound to a dispatch and read or written through it,
  which is why SDL_GPU now reports it false. Everything runs on the direct
  queue: `async_compute` stays false, and writable storage *buffers* do not
  exist yet, so GPU-driven culling and indirect submission remain out of reach.
  SDL_GPU exposes no dispatch, so the Basic renderer cannot use a compute pass.
- Meshlets, clustered lighting, indirect GPU-driven submission, BLAS/TLAS
  construction, ray-query effects, DDGI, ReSTIR, SVGF/A-trous, TAAU, motion
  blur, and external-memory interop are not
  implemented. Shadowing is up to four camera-fitted raster cascades for the
  first directional light, plus a bounded spot atlas. Item 19 adds cascade
  blending, wider filtering, MASK casters and contact shadows. Point/area-light
  shadows, blended/transmissive or media casters, ray-query shadows and
  per-cascade caster culling remain unsupported. GI is the bounded, non-temporal, single-bounce
  RSM approximation above; it has no secondary visibility, probe persistence,
  multi-bounce transport, normal/emissive texture contribution in the flux
  pass, IBL, DDGI, or ReSTIR history.
- Per-cascade shadow-map resolution and the allocated cascade-map count are
  construction-time renderer limits rather than authored project controls. They
  are a memory bound, not a look control, and are identical across a cohort
  because they are build constants; the authored cascade count is clamped to
  them deterministically and the rendered count is reported in stats.
- Padded slice rendering is wired up. `HybridSceneRenderer` records the padded
  rectangle and publishes the crop in output pixels; `CompositorSceneExecutor`
  sizes every target that carries the apron, runs the whole layer/blur/composite
  chain padded, and crops once at its own boundary, so nothing downstream sees
  or needs to know about the apron. A blurred wall split two and four ways is
  byte-exact against the full-canvas render at every shared pixel, including the
  interior corner where two aprons meet.
  Two limits worth stating: the apron applies only at a one-to-one
  canvas-to-output pitch, because an apron in canvas pixels has no exact
  expression in output pixels at any other scale - a scaled render is a preview
  or a mapped view with no neighbour to seam against, so it takes none and costs
  nothing. And it covers `FeedProjection::CanvasSlice` maps only. A
  `PlanarSurface` map renders the whole scene through one asymmetric
  per-surface frustum instead, with no canvas rectangle to expand; that path -
  planar surfaces and mesh-warped ones alike - has the same seam exposure it had
  before, because `mesh_perspective_view_for` fits the frustum to the mesh with
  no margin.
- Ordinary shared-depth 3D rendering exists, but the compositor can still
  invoke isolated renderer instances for layer-scoped work. The complete
  render-once/product-driven compositor refactor is unfinished.
- No donor reference captures, cross-vendor image baselines, multi-node seam
  conformance suite, or qualified hardware/driver matrix exists yet.

## Remaining frame-path boundaries

Renderer selection, history decisions, scene-bridge diffing, still/video/model
result observation, projection resolution, command acquisition, and bounded
upload exhaustion do not read asset bytes or wait for I/O. A completed
decode/load failure does not use a frame-count or wall-clock retry; it is
retried only after an authored-key change or explicit operator refresh. A cold
request whose bounded queue admission lost a `try_lock` may attempt admission
again on a later observation, without probing the file.

The path is deliberately nonblocking rather than allocation-free hard real
time. Cold/authored-key changes can allocate bounded map/request entries;
procedural geometry and a changed horizontal-arc projection are built on the
calling thread; first GPU adoption of a decoded model creates textures, appends
pre-reserved arenas, and records uploads. Those operations need a future
prepare/publish stage if command recording itself must have a strict fixed CPU
budget.

Model Import, Add Model and UV Mesh Browse now validate and package the selected
file on a bounded ingest worker. Status-bar stages and specific failures remain
visible while the editor continues running. Results are fenced to their project
generation, and projection results also check that their calibration is still
current. See the step 14 entry below for the immutable package contract.

All background decoders/loaders are process-local. An indefinitely blocked OS
read can stall later work behind that worker while frames continue with
stale/null content. Asset-ready and authored refresh generations now
participate in Director cohort readiness through step 15. Program publishes a
replacement only on the shared permitted frame; a late member retains its
previous image. Asset distribution and presence-only media integrity retain the
existing shared-storage assumptions; see the step 15 limits.

Shader acquisition and compilation remain configure/build-time work. Byte-level
asset verification is not part of the frame loop.

## Donor ownership

Media Server owns the facade, temporal policy, render graph, scene bridge,
project schema, compositor, output stage, and application lifetime. Donor audit
and copied-file provenance are in
[third_party/renderer/PROVENANCE.md](third_party/renderer/PROVENANCE.md).
Reference-build attempts and capture gaps are in
[third_party/renderer/REFERENCE-AUDIT.md](third_party/renderer/REFERENCE-AUDIT.md).

## Validation

Focused Debug and Release suites cover temporal initialize/advance/hold/reset,
camera cuts and exposure, staged scene-bridge rollback, cold still scheduling,
bounded queue priority, exact held media across overlapping recordings, cache
teardown during blocked decode, large native D3D12 uploads and page reuse,
command cancellation state rollback, and native Advanced product readback.
The directional-light GPU test verifies that an opaque box darkens pixels whose
object ID remains the floor receiver, that a red floor contributes visible red
diffuse bounce compared with the explicit GI-off render, and that left/right
node-slice color is byte-exact against the corresponding crop of the full
Advanced render with both effects active.

The cascade GPU test renders a receding floor with three casters at zero, one,
and three cascades. It verifies that zero cascades runs no shadow pass and
reports none, that one and three both darken the floor against the
no-shadow render, that three cascades actually change the image rather than
reproducing the single whole-scene map, and that two half-canvas node slices
are byte-exact against the corresponding crops of the full-canvas render with
three cascades active. It also renders a floor with nothing above it at every
cascade count and requires that no floor pixel darkens, which is the guard on
the texel-referenced bias: the test fails outright with the bias set to zero.
A second GPU test drives 240 composited layers plus an
opaque caster through one recording with the complete cascade set bound, which
fails without the command context's fragment sampler-table reuse and passes
with it; it is the regression guard for D3D12's 2,048-descriptor
shader-visible sampler heap.

Model, editor, and lockstep suites cover the authored side: clamping and
document round-trip of the cascade count and bounce strength, their presence in
the evaluated scene digest, the Project Settings rows that author them, and the
renderer profile digest changing for either edit while two out-of-contract
edits that clamp to the same settings keep one digest.

The ICVFX suites cover both halves. Headless: FreeD and OpenTrackIO parsing
against known bytes with every truncation and every malformed form refused whole;
the pose slot under a concurrent writer, where every field is a fixed multiple of
one so a torn read is detectable rather than merely unlikely; the pose channel
holding rather than extrapolating, and two channels fed one stream agreeing on
every frame including the gaps; outward rounding of the inner rectangle; two
nodes with different slices deriving byte-identical rectangles; the wall
intersection clamping to the whole raster when the frustum is unbounded; and the
composite weight compared for **equality**, not proximity, across a node
boundary. On the GPU: a full-canvas inner-plus-outer composite against two node
composites with the rectangle straddling the node boundary and the feather
crossing it, byte-exact at every shared pixel, with a solid plate and a real ramp
present so the equality proves something. The wire suite adds the frozen
version-4 goldens, every unusable pose refused at the codec, and a live session
in which poses arrive in order, publish for the render loop, are answered by
nothing, and close the session when out of order or foreign.

Both `windows-msvc-debug-gui` and `windows-msvc-release-gui` were configured
with the new shaders, built in full, and passed all 132 registered CTest entries
with zero failures and zero skips. Native D3D12 tests ran on the available
adapter rather than taking a headless skip path, including multi-4K upload reuse,
cancelled-list state restoration, the complete Advanced product set, the
directional shadow/red-bounce and multi-node slice fixture, the cascade and
descriptor-budget fixtures, DXGI presentation, and the native scene presenter.
The GUI window regression also asserts the backend-specific fixed
command-context capacity required by the editor's two-submission frame. The
media and renderer-integration executables contain 32 and 11 passing cases
respectively in both configurations.

On 2026-09-06, after steps 10 and 11 landed, both presets were rebuilt and
passed all 139 registered CTest entries with zero failures. The new cases are:
the dither as a pure function of the wall pixel, the ramp banding check, the
display-table cache; two nodes dithering one wall byte for byte and an authored
look reaching the output on the GPU; the fog/bloom frame derivation, the
view-driven fog matrix and the fog height falloff through the renderer;
atmosphere applied once across layers, bloom on a view-driven frame and bloom
split across nodes through the compositor; the colour round trip and digest in
the scene model; and the COLOUR tab and its status text in the editor suite.


## Step 12: culling, instancing, and authored LOD (2026-09-06)

Basic and Advanced now share deterministic full-canvas CPU geometry culling,
instancing of repeated imported material parts, and authored model LOD chains.
Camera culling leaves the full opaque shadow caster set and cascade fits intact.
Per-node slices and render-view overrides never choose LOD, and no previous LOD
state survives a seek. Free Stage/mapped views and active tracked plates retain
all geometry conservatively because the scene renderer does not own the complete
replicated wall volumes.

**File > Project Settings > Geometry** exposes replicated culling, instancing,
and model-LOD switches and last-pass counters. Models load optional `.lod`
companions on the existing cache worker, as one bounded immutable generation.
An unconditional geometry profile marker prevents older builds from satisfying
this renderer's readiness evidence. Default capacity is 16,384 draw records,
4,096 model placements, 2,097,152 shared vertices, and 6,291,456 shared indices.

The implemented boundaries, asset format, capacity accounting, UI instructions,
and tests are in [step 12](advanced%20icvfx%20renderer/12-culling-instancing-and-lod.md).
Occlusion culling, GPU-driven submission, automatic simplification, and an editor
command to reload models across a cohort remain outside this implementation.

Both Windows GUI presets passed all 140 CTest entries with zero failures after
step 12. The native editor Geometry tab was also inspected and exercised.
The final Stage-camera and fog correction was rebuilt in both presets, followed
by 48 passing render, presenter, and editor checks in each, with zero failures.

The Stage viewport supplies its free camera as an explicit view while retaining
the authored camera/canvas for LOD. Its own clip planes now reach both fog paths,
so keeping the authored geometry policy does not change the preview atmosphere.
The native Release preview was verified with visible Stage geometry and fog;
Hybrid and compositor image comparisons cover differing camera depth ranges.


## Step 13: texture mip residency (2026-09-06)

Advanced model textures now use deterministic mip streaming. The existing
process-wide model worker prepares complete immutable pyramids, filtering
colour in linear light with alpha weighting, data linearly, and tangent normals
with renormalization. Images shared between incompatible material roles are
split on that worker. CPU residency, sticky decode failures, refresh generations,
publication epochs and blocked-decode teardown retain the ModelCache contract.

The authored full-canvas camera and world bounds determine requested detail.
Instances share the finest request for their source/image. Compositor layers
receive the complete scene for budgeting. Neither output slice nor explicit
view overrides affect selection. Every referenced image retains the lowest
three levels. When desired detail exceeds the budget, eviction is ordered by
replicated desired frame and stable source/image identity. Current references
share that frame; no local access history or wall clock influences the plan.

`RendererLimits::max_model_texture_bytes` is the total envelope; the authored
1-512 MiB limit may reduce it. Half holds the working set and half reserves
replacement headroom. All allocated, pending and fence-retired bytes are counted.
Reducing the budget stages the smaller set under the previously authorized
envelope, then releases that headroom. Mip changes replace textures without
consuming the model geometry's 64 refresh slots. Allocations are retained through
cancellation and D3D12 reclamation polls their last submission fence. The RHI
supports full-width row uploads to a specific mip/layer on SDL_GPU and D3D12;
streaming advances at most 16 MiB per recording and publishes only a complete
candidate set. Pending upgrades keep complete active model textures. The output presenter
submits upload progress while deferring its swapchain until all requested mips
are ready, including when media buffering is disabled. Resource refusal preserves
the active set and reports through renderer diagnostics.

File > Project Settings > Geometry exposes Mip streaming, Texture memory and
Mip bias when Advanced is selected. The same panel reports texture counts,
reduced-detail counts, resident/allocated bytes and loading/refusal reasons.
These settings persist in the document and join both scene and readiness digests.
The Geometry panel scrolls when the diagnostics exceed the available height.

Step 13 initially provided RGBA8 GPU mip residency; step 14 extends it with
BC7/BC5 ingest and block-aware mip uploads. Disk-backed per-mip reads and virtual
texturing are not implemented. CPU backing must still fit the 2 GiB ModelCache
limit. Residency budgets are per renderer, as the previous model budget was.
Program's cross-node asset publication handshake remains the existing incomplete
boundary: requests and settled residency agree, but worker completion and first
publication are not synchronized across processes. Prewarm a set before a take.

Validation: full Debug and Release builds, with all 141 registered CTest entries
passing in each configuration. Coverage includes CPU filtering, budget/ordering
and settings checks; GPU upload/cancellation, tail pixel, bounded progression and reclamation
checks on SDL_GPU and D3D12; a native raster test of a 1 MiB source in a 32 KiB
texture envelope, with two slices and a different view override selecting the
same mips. The native editor's toggle, memory and bias controls were exercised.
See [step 13](advanced%20icvfx%20renderer/13-texture-streaming.md) for operation.


## Step 14: material model and ingest (2026-09-06)

Basic and Advanced now shade glTF clearcoat, sheen, thin transmission, IOR,
specular colour/weight and anisotropy. Sixteen numeric components and nine new
map paths reach the document, model boundary, scene and SceneBridge digests,
Inspector and GPU records. All six corresponding `KHR_materials_*` extensions
import their factors and maps. Unsupported extensions, non-UV0/transform texture
views, missing referenced images and invalid required tangents produce explicit
import failures. A material version marker joins renderer readiness profiles.

Advanced combines the new direct-light lobes with environment lighting. Sheen
uses fixed hemisphere quadrature and conservative energy attenuation;
anisotropic environment reflection uses a bent-normal approximation. Transmission
reads one opaque geometry/background colour copy with a fixed nine-tap filter
and at most eight canvas pixels of support. Thickness, volume transport and
transmission through later composite media/transparent layers are outside this
implementation; `KHR_materials_volume` is refused. Alpha MASK/BLEND and sidedness
are applied by the surface shader. Sphere, cylinder and box X-face triangle
winding now agrees with outward normals under sidedness enforcement.

Ordinary slices retain a common full-canvas raster projection within native
viewport limits; larger canvases retain slice projections and small node targets.
Lighting
positions are reconstructed from absolute canvas pixels, depth and that shared
projection, avoiding the viewport-dependent final bits of interpolated world
attributes. Transmission support is reserved from authored state, including
model scenes before decode completion. Bloom and transmission add their support
to preceding filters; this also fixes occlusion-invalid pixels entering bloom
at a slice border. An insufficient authored apron limit refuses the pass.

Library Import Model, Add Model and UV Mesh Browse now use one bounded ingest
worker with visible status stages and errors. Project generations and surface
calibration checks fence late results. Ingest writes immutable, versioned,
checksummed `source.<hash>.msmodel` siblings containing geometry, tangents,
materials, all map payloads, bounds and LODs. Colour/data mips use BC7 and normal
mips use BC5; non-block-aligned image bases remain RGBA8 without rescaling.
The package is the project's asset source, so render nodes consume the imported
result rather than reinterpret the original glTF. Both SDL_GPU and D3D12 upload
compressed base/lower mips and preserve candidates across command cancellation.
D3D12 supports the 26 sampled textures using 16 samplers; bindless remains false.

Packages remain wholly backed by the bounded CPU ModelCache. Per-mip disk
paging, virtual texturing, synchronized first publication, cohort reload, USD,
MaterialX and material graphs remain outside this work. The BC7 encoder is
vendored with pinned provenance and its MIT licence included in Windows packages.

The native editor was exercised on a separate test project: changing clearcoat
changed the highlight, a glTF containing all six extensions imported into the
Library and wrote its package, and an unsupported volume material produced a
visible refusal without adding an asset. The supported ranges, approximations,
package format and UI walkthrough are in
[step 14](advanced%20icvfx%20renderer/14-material-model-and-ingest.md).

Final validation: both `windows-msvc-debug-gui` and
`windows-msvc-release-gui` built in full and passed all 142 CTest entries with
zero failures (60.39 s Debug, 53.30 s Release). The scene renderer's 30 GPU
cases include a clearcoat/sheened/anisotropic/transmitting sphere, exact Basic
and Advanced HDR slice pixels with each new lobe active, and node slices on a
65,535-pixel canvas. The geometry suite's eight cases include outward winding
and the updated deterministic mesh digest. Compressed texture tests sample
BC7/BC5 base and tail mips, rectangular/unaligned mip bases and cancelled uploads
on SDL_GPU and native D3D12. Material ingest tests cover non-default document
values and clamps, both digests, frozen package payloads, corruption and worker
success/failure.


## Step 15: baked global illumination (2026-09-07)

Implemented the owned offline CPU diffuse tracer, checksummed immutable probe
assets, worker cache, world-space SH/visibility sampling, static contributor
flags, and the Environment > Baked Lighting operator workflow. `media-bake` is
built and packaged alongside `media-server`. Bakes replace environment diffuse
and RSM bounce; direct lights and specular IBL remain live.

Program now uses a common authored asset-set identity and refresh generation
across probe, model, media, projection-mesh, LUT, and environment caches. Nodes
report CPU/GPU readiness in v7 heartbeats; the Director schedules exact-frame
activation through the existing durable frame-permit barrier. Native outputs
and the editor retain the last approved Program texture while preparing the
next generation. Late adoption is refused. Library Rescan fans out the shared
generation, with source invalidation on workers.

For the operator procedure, format, publication contract and explicit limits,
see [step 15](advanced%20icvfx%20renderer/15-baked-global-illumination.md).

Validation: final full Debug and Release GUI builds passed all 143 CTest
entries with zero failures (178.00 s and 157.36 s). The model-cache suite also
passed 20 consecutive runs per configuration after a publication-fence fix:
an unpublished completion no longer hides an already-visible refresh. Native
D3D12 outputs demonstrated identical N+1 pixels on one permitted frame and
retained N pixels on an unready member afterward. Native editor bake/progress
and the packaged CLI were exercised; repeated CLI files had identical SHA-256
hashes. Physical multi-machine genlock capture remains a hardware validation
step, distinct from these software cohort tests.

Follow-up computer-use review reproduced the silent startup/refresh waits and
the dark HDR bake. Project save/load now strips session activation ordinals;
Program reports its preparation reason, waiting output, activation countdown,
and confirmation state. Bake progress remains in the main status bar and ends
only with an accurate Stage/Program completion message. Ready renderers retain
the exact activation snapshot across catch-up and GPU retries; playback's decode
headroom cannot deadlock publication.

The baker now matches live environment rotation/filtering and mixes uniform
directions with HDR importance sampling, preserving tiny bright softboxes at
the default sample count. Probe visibility uses bilinear moments and a smooth
cutoff to avoid abrupt patches. New regressions cover small HDR highlights,
live/baked directional HDR agreement, visibility continuity, saved permits,
readiness counts, mailbox activation retention, and playback headroom. The
native editor and a separate output process confirmed common activation frames
for initial load, rebakes, and lighting changes during playback.

Final follow-up verification: both full GUI builds passed, followed by 143/143
CTest entries in Debug (271.31 s) and Release (179.86 s). The packaged native
output also showed the corrected HDR energy and smooth visibility transitions;
the packaged CLI reproduced the editor bake byte for byte.

### HDRI fog distance follow-up (2026-09-07)

Project Settings > Environment > Sky and Fog now includes Background distance
in metres. Zero preserves the far-clip background; positive distances resolve
fog at a finite view depth without changing scene surface fog, environment
lighting, reflection mapping, or bake identities. The value is saved and
replicated, and the settings page scrolls to keep every fog control accessible.

Both full Debug and Release GUI builds pass. All eight targeted CTest entries
pass in each configuration: scene model, baked lighting, renderer integration,
compositor scene, froxel grid, editor workspace, Director frame barrier, and
renderer lockstep. GPU checks cover finite-depth transmission, near/far bounds,
unchanged geometry fog and lighting, and sliced-canvas agreement. Native editor
and separate output checks confirm the live change, saving, and reopening a
five-metre example at out/hdri-fog-review.mstage.

The native Save As walkthrough exposed a catch-up overflow after a long modal
pause. Director batches now respect the remaining 64-frame cohort capacity
and continue processing acknowledgements when full. With Save As held open for
50 seconds, the rebuilt editor stayed alive and returned to zero pacing lag.

## Step 16: photometric units and spot lights (2026-09-07)

Directional lux and point/spot lumens or candela now affect lighting. A common
CPU emission conversion supplies live geometry, fog, reflective shadow maps,
and offline baked probes. The replicated project scale defaults to pi/1000:
an ideal Lambertian 18% card under 1000 lux yields scene-linear 0.18 at f/2.8,
1/48 s, ISO 100, 0 EV. The material regression reads approximately 0.175 after
its Fresnel response. Physical wall nits still require output calibration.

Enabled temperature multiplies authored colour by a linear Rec.709 tint from
a deterministic Planckian xy table spanning 1000–40000 K. The CIE source,
licence, and regeneration procedure are recorded in `third_party/cie/README.md`.
The tint has unit luminance; disabled temperature and 6500 K are exact RGB
passthroughs. Unitless intensity retains its prior behavior and ignores the
project photometric scale.

The existing step-09 spots remain in the bounded local-light array. GPU lights
now have an explicit integer kind shared by geometry and fog shaders.
Photometric spots use smoothstep cones, with lumen-to-candela conversion
integrating the inner cap and penumbra. Equal cone angles form a hard edge;
windowed inverse-square falloff reaches zero at range. Unitless spots retain
their old squared penumbra. Unsupported unit/kind combinations reject rendering
and baking; area emission and IES remain deferred. Item 19 consumes the sun's
angular radius for shadow penumbrae; its sky disc remains deferred.

Properties exposes units, temperature enable and Kelvin beside intensity.
Spot aim and cone values now resolve through the numeric edit path. Project
Settings > Colour exposes Light scale. These controls author the ordinary
replicated document; scale participates in scene, renderer-profile,
photometric-light visual and bake-input digests. A renderer capability tag
prevents older builds from agreeing to the new photometric contract.

Both full Windows Debug and Release GUI builds pass. All 144 CTest entries
pass in each configuration. New CPU, GPU and editor coverage verifies
temperature, photometric conversion and flux, cone boundaries, exact range
cutoff, relative exposure, bake transport, project save/load, field digests,
Inspector edits and exact pixels across two slices with a photometric spot.
The native editor confirms warm/cool spots in Stage and Program, temperature
edits, neutral 6500 K, undo, and unit selection in the review project at
`out/step16-review.mstage`.

The native check also caught the slider's two-decimal display hiding the
small scale value. Light scale now uses the standard numeric field with six
display decimals and exact typed input. Both builds and the five affected
editor/timeline suites passed again after that correction. Typing 0.0062831853
(twice the default) visibly brightened both Stage and Program, and the native
Save command persisted the review project with that scale.


## Step 20: texture sampling (2026-09-07)

[Item 20](advanced%20icvfx%20renderer/20-texture-sampling.md) from the assessment
is implemented. All fourteen supported glTF material texture views preserve
repeat, mirror, or clamp addressing and their minification, magnification, and
mip filters independently, including views sharing one image. Project Settings
> Geometry exposes 1x–16x anisotropy and signed Sampling LOD bias. Basic and
Advanced consume these authored values; nearest/no-mip filters retain their
meaning. The fields persist and replicate through the normal document and
participate in scene, profile, and capability evidence.

The RHI has a validated sampler descriptor on SDL GPU and D3D12. A fixed bank
of material samplers and D3D12 descriptor/table reuse keep recording bounded.
Opaque, composite, instanced, and reflective-shadow-map paths bind the authored
views. Basic uploads complete mip chains. Streaming preserves the original
base image for no-mip views and refuses an inadequate budget. The offline
baker respects addressing and magnification filters for material transport.

Model packages are now version 2, with sampler state included in their content
hash. Reimport original model sources to replace version 1 packages. Live
sampling bias cannot restore evicted mip levels. Anisotropic agreement is
verified on independent slices of this RTX 3090; cross-machine and cross-vendor
cohort certification remains item 23.

Both full Windows GUI builds pass, with 145/145 CTest entries passing in Debug
(233.02 s) and Release (198.45 s). New GPU tests exercise SDL GPU and D3D12,
including four-tile materials, independent per-view samplers, anisotropic
sharpness, sampling bias, exact adjacent slices, held frames, and bounded
sampler reuse across 2500 alternating draws. CPU tests cover package and
project round trips, digest changes, no-mip budget refusal, and repeat/mirror/
clamp emission transport. Editor tests cover Apply and Cancel.

The native review imported and placed a tiled glTF floor, selected 8x
anisotropy, and increased sampling bias from about +2 to +4. Stage and Program
visibly softened the floor; saving and restarting restored the floor and
sampling settings in `out/step20-review.mstage`. The user's existing editor
session remained open throughout the isolated review.


## Item 18 — Temporal stability in the inner frustum

[Implementation and native walkthrough](advanced%20icvfx%20renderer/18-temporal-stability.md).
Participating nodes render a common padded plate, widen its projection, and
crop after all neighborhood passes. Filter supports add in execution order;
Geometry exposes the authored guard limit, with 512 pixels covering the
ordinary TAA/AO/bloom combination. Odd node boundaries preserve the same
derivative grid.

The RGBA32F motion/depth product carries expected previous depth from both
camera and model motion. Temporal color and R32F AO histories reject each
bilinear tap on depth and ID. Repeated frames preserve history; evictions reuse
bounded allocations and are reported. Advanced geometric specular filtering
broadens GGX and clearcoat roughness from fine normal derivatives.

Version-8 heartbeats report restart counts and their latest show frame. The
director holds Program if these differ across the renderer cohort, including
equal counts at different frames. Replicated document/profile edits restart
the history/reporting period; old transition snapshots cannot contaminate it.
The renderer profile advertises temporal contract version 2.

The new GPU suite checks 32 moving plate frames against independent node
slices, exact repeats, same-object disocclusion, valid depth motion, AO
variance reduction, odd-boundary glossy geometry, and bounded history reuse.
This is local GPU evidence; physical cohort qualification remains item 23.
Plate TAAU and complete CPU rollback after an abandoned outer recording remain
outside this increment.

## Item 19 — Shadow stability

[Implementation, controls and native walkthrough](advanced%20icvfx%20renderer/19-shadow-stability.md).
Directional cascades blend from the same canonical world position, retain
stable scale/depth under camera translation, and use a fixed 5×5 tent filter.
Manual bilinear comparisons and analytic receiver-plane correction also
apply to the spot atlas. Nine fixed blocker samples turn the sun's authored
angular radius into a penumbra bounded at 16 shadow texels.

Optional contact shadows take twelve fixed depth steps toward the first sun
and shadowed spots. Their 32-pixel apron combines with AO support before
subsequent filters. MASK materials share textured alpha/sidedness coverage
across shadow, RSM, depth/normal, object-ID and velocity passes. No new
per-frame allocation or disk I/O is introduced.

Project Settings > Shadows exposes cascade blend, filter radius, contact
length and thickness. Properties exposes the directional light's Sun radius
in turns with six decimal places. Settings persist in the replicated model
and participate in scene and renderer-profile digests; contact eligibility
is counted in stats and idle reasons are reported once.

Both full Windows GUI builds and all 147 CTest entries passed in Debug
(102.36 s) and Release (75.27 s). Final label/portability edits were followed
by successful rebuilds and all five focused renderer/editor checks in both
configurations. The GPU suite covers slope acne, filter/penumbra width,
cascade transitions, atlas isolation, contact hits, MASK holes, stable fits,
exact repeats and independent odd node slices. Physical multi-node/wall and
cross-vendor qualification remain item 23.

## Item 21 — Object motion blur at physical shutter

[Implementation, limits and native walkthrough](advanced%20icvfx%20renderer/21-object-motion-blur.md).
Advanced rendering has a separate object-motion/depth replay: both object
transforms use the current view projection, leaving camera motion out of
rendered shutter blur. A fixed 33-sample HDR reconstruction follows the
replicated rational show rate and the active camera's shutter duration.
Tracked plates use their bound physical camera's shutter.

Project Settings > Geometry exposes Object Motion Blur and a 0–64-pixel
Maximum radius. Camera Properties exposes precise Shutter s entry. The
authored radius contributes to the slice/plate apron and all new state
participates in the appropriate scene/profile/history digests. The compositor
combines visible layer motion and reconstructs the completed scene once,
before bloom and cropping. Pass status and explicit refusals are reported.

Pause/Play document edits preserve the motion pair through an evaluated
content identity excluding only transport controls. Real authored edits,
camera/view identity changes, cadence/shutter changes and nonconsecutive
seeks still reset history. Incremental adoption and full model rebuilds agree.

GPU tests cover camera-only identity, shutter-proportional streak width,
premultiplied HDR energy, foreground rejection, radius caps, Hold/Advance
through transport edits, real-edit/seek resets, recording cancellation and
exact 63/65-pixel independent slices. The tracked-plate temporal suite also
checks blur with TAA, temporal AO and bloom. Editor tests cover Apply/Cancel
and precise shutter entry.

This is visible screen-space reconstruction with one dominant neighbourhood
motion, not hidden-surface reconstruction or motion estimation inside media.
Compositor spatial filters, masks, mattes and non-Normal blends currently
refuse the combination because their processed coverage has no matching
motion/depth product. Physical two-machine/cross-vendor qualification remains
item 23.

Native review used the isolated `out/step21-review.mstage` fixture. The gold
bar blurred during playback while the static blue bar stayed sharp. Pause
retained the streak; a 1/96-second shutter shortened it. Applying a 16-pixel
radius, saving and restarting restored the authored settings, including the
full typed shutter value `0.0104166667` in the project file.

Both full Windows GUI builds pass, and all 148 CTest entries have passed in
each configuration. The final full Release run passed 148/148 in 203.06 s.
Debug's post-fix run needed one focused rerun of the existing fog-digest test
after separating its visual-field comparison from authored-history metadata;
that rerun passed. Final rebuilds and the object-motion/temporal-plate GPU
suites passed in both configurations. The plate test also compares against
an unblurred fourth render to prove the blur changes the image.

## Items 22 and 23 — Shader contract and cohort conformance

[Floating-point decision and measurements](advanced%20icvfx%20renderer/22-shader-floating-point-contract.md)
and [conformance implementation and operator workflow](advanced%20icvfx%20renderer/23-cohort-render-conformance.md).
Every production HLSL entry point now uses `-Gis -denorm preserve` for DXIL and
SPIR-V. Shader Model 6.2 is the checked D3D12 minimum. A generated SHA-256 identity
covers the actual compiled shader bundle and compiler contract. The experimental
relaxed build setting remains available for A/B measurements; strict is the
production default.

A private worker renders an embedded whole-canvas 128 × 128 scene for sixteen
fixed frames and reads twenty pass boundaries, including every environment face
and mip. The heartbeat carries its ordered digests, fixture/request versions,
adapter/driver and shader identity. Advanced Program requires complete current
matching evidence from the Director and every configured render node, independently
of the physical-sync policy. Pending, failed or divergent evidence holds Program;
the first failing node/pass remains the reported refusal.

Output Monitor runs the initial check automatically and exposes **Check render
cohort** and **Pass details**. Explicit requests invalidate older evidence and
propagate through the heartbeat. Success/failure stays cached until an explicit
request or device change. Protocol **v9** requires the Director and all nodes to
upgrade together.

Both full Windows GUI builds pass. Strict Debug and Release each passed all
149 CTest entries; the final full Release run took 174.23 seconds. Experimental
relaxed Release also passed 149/149. Focused post-review checks cover the actual
worker, heartbeat codec/service, Program gate, editor and server bootstrap.
A temporary fog-shader mutation changed the fog digest first and only its
downstream pass digests; the original shader was restored afterward.

Four alternating strict/relaxed benchmark pairs on the RTX 3090 measured a 2.8%
increase across the fixture's sixteen-frame GPU interval (about 0.010 ms/frame)
and 7.7% for environment upload/prefiltering. This small desktop fixture is not a
production-resolution frame-budget prediction. Physical multi-machine deployment
and cross-vendor qualification remain external checks; local GPU execution and
real loopback heartbeat transport are verified here.

Native Release UI verification used an isolated profile and the existing item-21
review fixture. The initial check passed, the operator button requested a fresh
generation, and the default Output Monitor dock exposed the complete adapter,
driver, two-line shader SHA-256 and all twenty pass digests. Post-review metadata
layout and scrolling fixes were rebuilt and checked in both configurations.
