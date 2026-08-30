# Media Server

Media Server is an early-stage, cross-platform virtual-production media-server
prototype. It is building toward an operator workflow familiar from motion
graphics and compositing tools: author a project in a layer/timeline editor,
then run the same committed project on one machine or a synchronized set of
render/output nodes.

The repository is a working engineering baseline, not a production-ready media
server or a stable file/protocol format. The project, wire, and persistence
schemas may change while the product is being defined.

## What this optimizes for

This runs on a stage, during a shoot, with an operator standing at it. That
single fact decides most of the trade-offs below, and a feature that gets it
backwards will feel wrong no matter how correct it is.

**The show is rebuilt constantly, not deployed and left alone.** Plates are
recut between takes, surfaces move when the car does, a grade changes because
the sun did. Anything that assumes the project is settled - caches keyed on
"this will not change", work deferred because "it only happens once" - is
assuming the wrong thing.

**A stall is worse than a stale answer.** The operator is mid-gesture with a
crew waiting. Two seconds of frozen UI costs more than a readiness figure that
is a few seconds out of date, and far more than a check nobody asked for.
Correctness that arrives after the moment has passed is not correctness.

**The frame path never waits on a disk.** Nothing in the control loop reads a
file's bytes, hashes one, or blocks on I/O. Presence is a stat. Content is read
only when an operator asks for it, and then it says so while it works. This is
a hard rule, not a performance target: the loop that misses its boundary is the
loop that drops a frame on the wall.

**Nothing expensive runs on a timer.** Periodic work on the control thread is a
periodic stall, and it is the kind that survives review because each occurrence
looks small. If something must be rechecked, either make the recheck cost a
stat or give the operator a control and let them decide when.

**Media integrity is not the thing being protected.** The files are on a local
share that people are actively managing; a wrong file shows up as a wrong
picture on a wall someone is looking at. Byte-level verification exists and is
worth having, but it is an operator-invoked answer to a specific suspicion, not
a tax on every document edit.

**Direct manipulation before controls.** A clip moves between tracks by being
dragged there; a track reorders by being dragged. Reach for a button when there
is nothing to drag - creating an empty container, invoking a scan - and not
before. A row of two-letter buttons is what a UI looks like when nobody could
decide what the gesture was.

**Name what is, not what is not.** A clip in no track gets no header announcing
that; its position already says it. State the presence of a thing, never the
absence of one, and never invent a container to hold the exception.

## What works today

- An immutable document and frame-scheduled command system with per-context
  undo/redo, checkpoints, node replay, frame sealing, and Director/node TCP
  sessions.
- A Director-only, dockable native editor with Library, Scene, Stage, Inspector,
  Selection Sets, Color Correction, Texture, Timeline, Compositor, Hardware
  Map, Feed Map, and Output Monitor panels.
- Scene authoring for primitives, lights, imported glTF/GLB/OBJ models, media
  quads, materials, blend modes, ordered brightness/saturation/levels/lift-
  gamma-gain/contrast/channel-mixer/white-balance/curves/film-grain/Gaussian-
  blur/box-blur/directional-blur effects, and keyed component values. The
  timeline supports transport, exact Set In/Set Out and loop/range control,
  auto-keying, per-component keys, and key retiming.
- Clip editing on that same timeline: a media placement carries a start, a
  duration, a source trim, and head/tail opacity ramps. A scene-owned track is
  a screen - it owns the plane its clips are drawn on, with its own transform,
  raster and aspect lock, opacity, blend, grade, and deterministic
  order/mute/lock state - and tools cover move, standard/ripple
  trim, ripple delete, roll, slip, slide, split, Insert/Overwrite Clipboard,
  fades, and cross-dissolves. Every accepted operation is one document edit.
- Typed multi-selection across Stage, Scene, and Timeline, plus Director-local
  clip and key Copy/Paste, replicated transform groups, deterministic
  per-context redo, and semantic undo coalescing. A clip paste snapshots
  placements with their effects/channels, rewrites every target deterministically
  at the playhead, and undoes as one transaction.
- Named Selection Sets save ordered Stage membership in the document without
  replicating ordinary operator clicks. Recall remains Director-local, reports
  missing members, and the panel keeps Replace distinct from Recall.
- Imported assets carry expected byte length and SHA-256. The Library generates
  visible-row thumbnails asynchronously into a disposable, profile-versioned
  machine-local cache, while the Timeline requests exact eight-frame filmstrip
  atlases for visible clips plus a bounded prefetch window. Both upload only
  retained derivatives to separate bounded GPU texture caches, and trimmed
  clips sample the atlas with the same rational show-to-source mapping used by
  editing. Exact proxy profiles, source-time frame maps, Source/Prefer/
  Require playback resolution, and per-asset runtime status use that same
  derivative cache. The waveform profile has a versioned, validated 2,048-bin
  peak/RMS payload; the persistent Asset node remains the only asset identity.
- Project-wide effect settings in **File > Project Settings**: Gaussian kernel
  taps/sigma/pass count, Directional Blur taps, and whether inline colour
  response is scene-referred Exposure or display-referred Gain.
- GPU rendering through SDL GPU and a small RHI seam. The renderer supports
  PBR-style materials, image sequences and FFmpeg-decoded video, a typed
  compositor graph, and desktop-window presentation.
- A document-backed output stage. A process may select one feed by
  `node_index`; explicit Hardware output bindings let one supervised process
  drive multiple saved feed/display pairs. Mappings can be a canvas slice,
  planar physical surface, horizontal arc, or imported UV0 projection mesh.
- Checksummed, atomic `.mstage` project saves; WAL/release persistence and
  recent-project recovery.
- A Program/Preview switcher. Preview is a complete offline authoring surface
  with its own playhead, scene, and compositor graph; promoting it to Program
  is a cut or a short dissolve at an exact control frame.
- Authenticated lifecycle agents that can start, restart, stop, and report on
  fixed local render processes, including a transient machine/GPU/display
  inventory for the Output Monitor.
- Split control/render launch scripts for a real two-machine test, plus a
  portable Windows release package that carries them.

The current architecture and its intentional limits are described in
[media-server-architecture-plan.md](media-server-architecture-plan.md). The
near-term work is in [next-steps-plan.md](next-steps-plan.md). Renderer changes
that introduce effects must follow
[effect-rendering-requirements.md](effect-rendering-requirements.md).

## Current limits

- Source assets are referenced by absolute path and are not distributed by this
  program. Every machine in a show must therefore receive the expected bytes
  through external media-sync tooling. Readiness verifies imported byte length
  and SHA-256, and exact-key Source/Prefer Proxy/Require Proxy resolution is
  wired through playback and visible in Library. The repository deliberately
  ships no pretend encoder: proxy publication has a validated injectable
  backend and the production default fails closed until a real encoder is
  integrated, so an unpopulated Prefer policy visibly falls back to source and
  Require remains unready.
- The output path is an SDL desktop window. SDI, NDI, ST 2110, tracked
  cameras, and external render/plugin interfaces are not implemented.
- Output synchronization is authored, derived, validated, and reported end to
  end, but this build carries no vendor sync integration and SDL_GPU
  swapchains cannot join a present barrier. A FRAMELOCK or EXTERNAL GENLOCK
  policy therefore reports `SYNC UNVERIFIED` / `SWAP BARRIER INCOMPLETE` and a
  STRICT policy refuses to promote Program. The two seams a vendor
  implementation plugs into are `net::ClusterSyncStatusProvider` (the sync
  device and per-display lock) and `rhi::PresentBarrierBackend` (the native
  D3D12/Vulkan presentation path that can join a swap group).
- Native display-target discovery is implemented on Windows. Other platforms
  report output inventory as unsupported and explicit physical bindings fail
  closed until a platform provider is added.
- Exclusive fullscreen bindings select and verify an exact SDL display mode.
  Borderless and windowed bindings apply their saved raster and placement but
  do not change the operating-system display mode.
- The output transfer is sRGB today. Per-panel calibration and 3D LUTs remain
  the responsibility of the downstream LED processor.

## Repository layout

```text
engine/core        shared types, IDs, results, and frame primitives
engine/doc         immutable document root and nodes
engine/cmd         commands, reducer, undo, Director scheduler, node bus
engine/wire        FlatBuffers codec for command replication
engine/persist     project files, WAL/release records, and recent-project state
engine/show        show clock, transport, switching, frame/presenter contracts
engine/net         TCP sessions, resume, heartbeats, barriers, lifecycle protocol
engine/scene       deterministic scene, animation, geometry, and manipulation
engine/assets      evaluated media/model asset registry
engine/compositor  typed compositor graph and deterministic pass plan
engine/output      feed map and physical/mesh projection models
engine/topology    authored hardware map and logical connections
engine/sync        sync policy, derived sync groups, and the physical-timing gate
engine/rhi         backend-neutral GPU-device seam
engine/render      scene, media, compositor, and output-transform rendering
engine/ui          SDL output presenter
engine/gui         native 2D UI, text, input, windows, and docking
engine/editor      operator-editor panels and orchestration
engine/shaders     HLSL shader sources and optional build-time bytecode
apps/server        `media-server` Director, node, and lifecycle-agent executable
scripts            local editor, scene, wall demos and Windows packaging
tests/unit         fast core/document/command/wire tests
```

All modules through `engine/compositor` are available in a headless build. The
RHI, renderer, SDL presentation, GUI, and editor are built when GUI support is
enabled.

## Build

Requirements:

- CMake 3.28 or newer;
- Ninja;
- a C++20 compiler: MSVC 2022, Clang 18+, or Apple Clang;
- vcpkg in manifest mode.

Set `VCPKG_ROOT` to a vcpkg checkout. The default Windows preset is headless:

```powershell
cmake --preset windows-msvc-debug --fresh
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

Use a GUI preset for the renderer, SDL windows, editor, FFmpeg, and import
dependencies:

```powershell
cmake --preset windows-msvc-debug-gui --fresh
cmake --build --preset windows-msvc-debug-gui
ctest --preset windows-msvc-debug-gui
```

The equivalent presets are `linux-clang-debug`, `macos-clang-debug-gui`, and
`windows-msvc-release-gui`. The release GUI preset is the input to
`scripts/package-windows-release.ps1`.

## Run the local editor demo

From the repository root, after a GUI build:

```powershell
.\scripts\run-editor-demo.ps1
```

The script starts a Director editor and a lifecycle-supervised local output
node. Dragging or editing content in the editor updates the replicated scene
shown by the node. It also creates a temporary lifecycle secret when needed.

Other Windows demonstrations are:

```powershell
.\scripts\run-scene-demo.ps1  # Director preview plus two scene slices
.\scripts\run-wall-demo.ps1   # Director preview plus two feed-mapped outputs
```

## Run across two machines

The demo above puts everything on one machine. To drive a render machine from a
separate control machine, use the two halves instead - one script per role,
sharing a lifecycle secret. On the control machine:

```powershell
.\scripts\run-editor.ps1
```

That opens the editor and a Director listener, and prints this machine's
address, the port, and the shared secret - together with the exact command to
run on the render machine. There, run:

```powershell
.\scripts\run-output-node.ps1 -DirectorHost 192.0.2.10 -NodeIndex 0 -Secret <secret>
```

`-NodeIndex` selects which slice of the wall that machine drives and must be
distinct per render machine; the 128-bit node identity is derived from it.
`-Fullscreen` drives a real wall, `-Windowed` is easier while setting up.

Both scripts take `-Build auto|packaged|release|debug`. `auto` prefers a
packaged `media-server.exe` beside the scripts directory, then the
`windows-msvc-release-gui` build, then `windows-msvc-debug-gui`, so the same
scripts work from the repository and from an extracted release package on a
machine with no build tree. `-DryRun` prints the exact command line without
starting anything.

By default the node script runs the machine-local lifecycle agent, which owns
the render process and starts it immediately. That is what lets Output Monitor
START, RESTART, and STOP the node, and what reports its sync device, house
reference, and display lock state. Finish the link in the editor: Hardware Map
> add a Render Server with that node index, the render machine's address, and
control port 47840. Pass `-Direct` to skip the agent and run the render process
alone - no remote control, but one fewer moving part when first proving
connectivity.

Two things are commonly in the way. Windows Firewall must allow the Director
port inbound on the control machine and the lifecycle port inbound on each
render machine; both scripts print the `netsh` command but deliberately do not
change firewall rules themselves. And every address here must be numeric -
`-DirectorHost` and the Hardware Map Host field take IPv4/IPv6 literals, not
host names.

To open only the editor, provide a Director listener and stable show identity:

```powershell
out/build/windows-msvc-debug-gui/apps/server/media-server.exe `
  --role=director --director-listen=127.0.0.1:45000 `
  --show-id=00000000000000010000000000000002 `
  --director-epoch=00000000000000030000000000000004 `
  --playback --edit --surface=1600x900 --wal=editor-test.wal
```

Run `media-server.exe --help` for the complete option reference. `--playback`
starts the show clock; `--scene` opens an ordinary scene-output window; and
`--output` selects the feed-mapped output path. The editor is Director-only.

## Program and Preview

Program is what the wall is showing. Preview is where the next look is built.
The point of the pair is that an operator can do real work - scrub, play, move
elements, key them, rebuild the compositor graph - on a scene that is not on
air, and put it on air only when they decide it is ready.

The `PROGRAM` / `SWAP` / `PREVIEW` group at the top right is the whole control
surface. `PROGRAM` puts the editor back on the live scene; `PREVIEW` returns to
the scene being rehearsed; `SWAP` promotes Preview to Program. `PROJECT
SCENES`, at the top of the left-hand SCENE panel, is where a scene is chosen:
`PGM` marks the live one, `RHS` the rehearsed one, and clicking any other row
takes it into Preview. `+ NEW` creates an empty scene and `CREATE COPY`
duplicates the scene currently on the Stage, both straight into Preview. On a
project that has never used the switcher, the first `PREVIEW` press copies
Program into a new `<name> Rehearsal` scene and opens it, so there is always
something to work on.

**What Preview owns.** Entering Preview seeds a Director-local playhead from
wherever the show currently is and freezes it there. From that moment the
timeline - the playhead and its transport buttons, including step, jump, and
go-to-start - drives Preview alone. The show keeps running underneath at its
own frame, and the status bar reads `PREVIEW  PGM <frame>` so the live position
is always visible. The Program viewport and Output Monitor keep reporting the
live show, never the rehearsal. Preview's playhead is never replicated and
never written to the project: render nodes only ever hear about Program.

**Masking on the output.** A matte is judged against the picture it is cutting,
which is the one on the wall rather than the one in a perspective viewport. With
the Masks panel open on a target, the Program Output panel becomes a second
authoring surface for it: the same masks, the same selection, the same undo
gesture as the Stage, drawn over the connector image. Its overlay resolves
through the output's own projection, so a mask over a canvas slice registers
with the crop and one over a physical surface registers with the warp, and the
panel says `PROGRAM OUTPUT  /  MASKS` while it is doing so.

Program Output authors masks only while the Stage is on the Program scene. In
Preview the two panels are showing different scenes, and a mask dragged over a
picture that does not contain the layer it belongs to would be authored
somewhere nobody is looking; the panel stays an ordinary output view until the
Stage comes back. A running dissolve is excluded for the same reason - that
picture is a mix of two committed scenes and belongs to neither - and a locked
target still draws its overlay while refusing to author, exactly as on Stage.

Scenes own their content, so editing in Preview cannot reach Program. That
includes the compositor graph: graph objects carry the scene that owns them,
Program composites through its scene's graph, and the Compositor panel edits
the graph of whatever scene is on the Stage. Copying a scene copies its graph
with it, with scene-layer references repointed at the copied elements, so a
promoted copy looks exactly like the scene it came from before you change it.

**What is shared, deliberately.** Show length, loop, and frame rate are one
project-wide setting; Preview follows Program for those. So are the Hardware
Map and the Feed Map - the physical wall does not change because a different
scene is being rehearsed - and surface edits made while in Preview key against
the Program playhead, which is the frame the mapping panels draw.

**Promoting.** `SWAP` exchanges the two slots at an exact control frame, so
every node performs the same transition on the same frame. The default is a
30-control-frame dissolve. A dissolve renders both scenes and mixes two
finished pictures, which is only defined when the two scenes share a canvas;
when they do not, the button reads `CUT` and promotes on a single frame
instead. `FADING` means a transition is already running, during which scene,
graph, and switcher edits are all refused - both inputs of a dissolve are
committed pictures. After a swap, Preview holds the scene that just came off
air and its playhead re-seeds to the live frame.

`SWAP` is held by exactly one thing beyond the switcher's own preconditions: a
project that asks for physical output synchronization under a STRICT failure
policy, whose timing the cluster cannot prove. The button then reads `HELD`,
and Output Monitor's timing line names the unmet condition. Nothing else blocks
a promotion. In particular, whether render nodes hold the media a scene needs
is reported on the Output Monitor and never gates the switcher - distributing
content to nodes is an external concern in this build.

## Editing clips on the timeline

A media element is a *placement* of a source, not a copy of it. The document
carries where the clip starts on the show timeline, how many show frames it
occupies, which source frame its first show frame shows, and how long its head
and tail opacity ramps are. All of them are exact frame counts rather than
durations in seconds, for the same reason show time is an ordinal: every node
has to agree on the frame a cut lands on, and a seconds value would already
have been rounded.

### A track is a screen

Placements belong to scene-owned tracks, and a track owns the plane its clips
are drawn on. It is a screen: it has a place in the scene, a format, an
opacity, a blend mode, and a grade, and the clips filed under it are what is
playing on it. Nine driving plates for nine angles are nine screens, each
positioned where that part of the volume is and graded for it, and cutting a
plate is an edit to what plays on one screen rather than to where a picture
sits in the world.

Selecting a track header selects the track, and the Inspector shows the same
sections it shows for anything else - transform, compositing, effects - plus a
`SCREEN` section carrying the raster the screen drives and one aspect control.
With the lock closed the plane's height follows its width and the raster, so
media on the screen cannot be stretched by a scale gesture; the lock is applied
in evaluation rather than in the authoring path, because a gizmo drag, a
keyframe, and a typed field all have to obey it and only evaluation sees all
three. The raster is authored, never read from whatever clip happens to be up:
an operator sets a screen to the format of the wall, and a plate of the wrong
size should look wrong rather than quietly reshape the wall around itself.

A clip on a screen has no transform of its own to offer, so the Inspector says
which screen it is playing on instead of showing nine fields that move nothing.
Its transform properties are still in the document and untouched - they are
what it returns to when it is dragged out of the track - they are simply not
what it renders with. A track's grade runs after the clip's, because the look
an operator sets on a wall is meant to survive whichever plate is up.

Screens are outlined in the Stage viewport, so a track with nothing on it, a
muted one, and one between clips are all still findable and still have a gizmo.
They are carried in the evaluated scene rather than re-derived by each panel,
which is what makes the editor and the renderer agree about where a screen is.

A track holds any number of clips from any number of sources, ordered along
time by their starts. The list runs top-down through the compositing stack -
what is in front is at the top, for tracks and for the clips inside them -
which is the reverse of the ascending order the model reports them in. A track
header carries its own mute and lock toggles; a second click on the header
opens its name for editing, because the first one selects the track. The
toolbar keeps only the operations that need a confirmed destination. Track
order and each clip's explicit order are also the deterministic media
compositing order. That order stacks clips sharing one surface, where every
clip is at the same depth - which every clip on one screen is, by
construction; it does not reorder clips across surfaces at different depths,
which composite back-to-front from the camera like everything else in the
scene. Creating or deleting a track is an ordinary Scene edit. A non-empty
track never disappears on a bare delete: choose `DEL+CLIPS`, or choose a named
destination and `MOVE` its placements first.

A placement carrying no track belongs to none. It stands on its own transform
like any other scene element, and is drawn as an ordinary top-level row rather
than under a header of its own, because a clip outside every track is already
visibly outside every track, and inventing a container for it would be
inventing one the document does not have. Clips move between tracks the way
they move anywhere else: drag a clip up or down onto a track's band - its
header or any clip already in it - and it re-parents while the drag is still
held, in the same undo step as the movement that carried it. Dropping on any
row outside every band takes the clip out of its track again, and back onto its
own transform. A locked track refuses an arriving clip.

Track order is changed by dragging a header up or down the stack. Each frame of
the drag takes one step toward the track under the pointer rather than jumping
to an absolute position, so the planner keeps normalising the order the one way
it already did, and the whole drag is one undo step. A track whose name is open
for editing is grabbed by the strip left of its name field, because the rest of
the row is that field.

Compositing a track is still one draw per on-air clip at the screen's
transform, not a render of the screen to its own buffer. The two are the same
picture except where clips overlap - during a cross dissolve, a screen-level
opacity or a non-linear screen grade applies per clip rather than to the
blended result. That is a real difference and it is the reason this is written
down rather than assumed away.

The document keeps calling this a lane - `lane_id` on a placement, and
`timeline_lane` nodes - because those names are in saved projects and renaming
them would not open an older show.

The exact IN and OUT number fields remain available for typed or dragged range
entry. The adjacent Set In and Set Out controls take the current playhead in
one click and preserve Loop. If a local Preview playhead is outside the old
range, the other endpoint moves only as far as needed to keep the new range
valid.

On a media layer row the Timeline draws that placement as a bar:

- **Drag the bar** to move the clip. **Drag either end** to trim it; trimming
  the head advances the source trim by exactly the frames it removed, so the
  frames that survive still show what they showed.
- **SPLIT** cuts every selected clip at the playhead into two placements that
  together play what the one clip played. Nothing is copied or re-encoded, and
  a split that would leave an empty half is not offered.
- **The two handles on the bar's top edge** set the head and tail ramps, drawn
  as the diagonals an editor expects. A tail ramp over the scene's black
  surround is a fade to black; overlapping one clip's tail ramp with the next
  clip's head ramp is a cross dissolve.
- Drags snap to the playhead, the show range, and the ends of every other clip,
  so butt-joining two clips and cutting on the playhead do not need pixel
  accuracy. `SNAP` turns that off, and holding Alt suspends it for one gesture.

The `EDIT` picker selects context-sensitive advanced tools. Ripple Trim, Roll,
Slip, and Slide commit one signed-frame edit when the drag ends. Ripple Delete
requires one valid contiguous selected span. Insert Clipboard and Overwrite
Clipboard target the active unlocked track at the playhead, preview their exact
affected span, and run only after `GO`; Overwrite retains both uncovered split
remainders and their retargeted effects/key channels. Handle limits, adjacency,
locks, rational source rates, and stale selections are validated by the
Director, and every accepted operation is one undo step.

Navigating the timeline is the wheel and the middle button, on both axes.
Wheel zooms time about the frame under the pointer and Shift+wheel pans it; in
`CURVES` mode Alt+wheel zooms values about the value under the pointer and
Ctrl+wheel pans them, and a middle drag pans time with rows or, held with Ctrl,
values. Because all six of those are gestures, the `KEYS` panel carries no
stepper buttons for them - only the two things a wheel cannot do, `FIT SEL` and
`FIT ALL`, which put a selection or the whole animation back on screen. Track
mute and lock are the layer list's eye and padlock rather than a letter, so the
state of a stack reads from across a stage.

A ramp *scales* whatever opacity was authored or keyed rather than replacing
it, so a dissolve composes with an animated layer. A clip the playhead has
moved past keeps its row and its bar - it is marked `off air` rather than
disappearing, because a clip you cannot see is still a clip you have to be able
to drag. Every authored media item is an explicit placement with a positive
duration; an invalid placement is reported rather than reinterpreted as an
always-on layer.

## Project settings

**File > Project Settings** holds the numbers that decide *how* an effect is
computed, as opposed to how much of it an operator asked for. They live in the
project rather than in machine-local preferences because they change the image:
two nodes filtering with different kernels would render a visibly different
frame of the same show, which is exactly the divergence the scene digest exists
to catch. Applying them is an ordinary document edit, so it undoes, saves, and
replicates.

- **Blur** - Gaussian kernel taps per axis, sigma as a fraction of the authored
  radius, and how many times the separable pair runs. A wide radius stays smooth
  at a modest tap count by repeating passes rather than by widening the kernel.
  **Directional Blur taps** is a separate bounded odd gather count; Box Blur
  uses its exact per-effect integer radius and pass count.
- **Brightness** - whether the multiplier is a scene-referred *exposure*,
  applied before the tone map so highlights roll off, or a display-referred
  *gain* applied to the finished picture, which clips the way a vision mixer's
  gain does. A response gamma shapes the multiplier once, over the whole folded
  stack, so two brightness effects of 4 and 0.5 still mean one of 2.
- **Saturation** follows that same response choice: linear-light colourfulness
  in Exposure mode, encoded/display-referred colourfulness in Gain mode. The
  per-effect Amount remains in the Inspector; only its project-wide response
  domain is selected here.

None of this appears beside an individual effect in the Inspector. A kernel
width is set once per show and then left alone; five such controls on every
blur node would be five things an operator scrolls past forever.

## Preferences

The machine-local settings an operator would otherwise retype on every launch
live in **File > Preferences**: the Director listen endpoint, show ID, Director
epoch, lifecycle secret file, and write-ahead log path. They describe this
installation, not the show, so none of them is written into a `.mstage` or sent
to a render node.

Precedence is **command line > preferences > built-in default**. An argument
given this run always wins, which keeps scripted deployments and the bundled
demos exactly reproducible on a machine whose operator has saved settings; the
panel names the value actually in use beside any field an argument displaced,
so an argument-driven launch never reads as settings being ignored. An empty
field means "no preference" and restores the built-in default.

Saved settings take effect the next time the program starts. Nothing here is
applied live: the listener, the WAL, and the lifecycle credential are all bound
during start-up, and the panel says so rather than implying otherwise.

Settings are stored per user beside the recent-project record, written to an
exclusive sibling temporary file and atomically installed, so an interrupted
save leaves the previous settings intact. A damaged settings file is reported
at start-up and the built-in defaults are used; it is never silently ignored.

Render nodes still take `--role`, `--director-connect`, and their identity on
the command line, because they have no operator UI to configure first. A
lifecycle agent's own configuration is likewise local and immutable by design.

## Project assets

Library imports are persistent project assets, separate from scene placement.
An import records a reference: the Director probes the file for metadata
(dimensions, frame count, rate, triangle and vertex counts), streams it once to
record the expected byte length and SHA-256, and stores its **absolute path** in
the project. The source itself is not copied into the project. Reimporting the
same exact source updates the existing asset record without changing its stable
project ID; accepting changed bytes is therefore an explicit new revision of
that one identity, not a derivative asset.

The model this rests on is that a path means the same file everywhere: a clip
the operator opens from `D:\Media\clip.mov` is opened from `D:\Media\clip.mov`
by every render node. Use identical local layouts, a mounted share, or a UNC
path that resolves the same way on every machine. Sources must be absolute; a
relative or `..`-bearing path is refused at import, because it would otherwise
resolve against whichever directory each process happened to start in.

Getting source media onto those paths is external sync tooling's job, not this
program's. What the program does is distinguish a missing path, wrong byte
length, wrong hash, and a local verification failure instead of treating any
file with the right name as ready - when it is asked to. Routinely it asks the
cheaper question.

Every machine answers the same question about the paths the current document
actually needs — scene media, scene models, and imported projection surfaces.
Library assets nothing currently uses are not counted, so an operator can keep
a shelf of unused imports. A placement whose Library record was deleted is
still counted, because the renderer still opens that file.

Output Monitor displays required/ready counts per render node, including
missing, byte-mismatched, hash-mismatched, and verification-failed totals.
Readiness is reported over the authenticated lifecycle listener, so it is
available while the render child is stopped; the agent answers only the exact
manifest requirements and never returns a directory listing, rewritten path,
or file contents. START and RESTART remain disabled until every required path
matches on that node; STOP remains available.

Asset readiness is reported, not enforced against the switcher. A disconnect,
restart, stale manifest, duplicate node index, or pending Hardware Map change
shows on the Output Monitor, but it does not hold a Program promotion: the
operator, not the readiness gate, decides when a look goes on air.

**Presence is routine; content is asked for.** When the manifest revision
changes, every machine stats the required paths and reports present or missing.
That is one stat per asset, it reads nothing, and it cannot report a content
mismatch because it never looks at content.

`RESCAN`, in the Library, is the other question: it streams each required file
and reports wrong byte lengths, wrong hashes, and local verification failures.
It is the only thing in the program that reads a source's bytes for this
purpose, and the only place a mismatch can come from.

Nothing rescans on a timer. An earlier build re-verified every fingerprinted
source every five seconds for as long as anything was missing, which on a 40 MB
plate stalled the control thread for two and a half seconds at a time, forever,
because a missing file never stops being missing. A share that mounts late is
picked up by pressing `RESCAN` - which is a worse answer than automatic
recovery only if the automatic version is free, and that one was not.

Thumbnails are derivatives, not assets. Their key contains the persistent Asset
NodeId, expected fingerprint, frozen `thumb-320x180-srgb` profile, and generator
version. The Director chooses the first valid video frame at or after 10% of
duration (then follows the specified fallback order), corrects orientation and
pixel aspect, and writes a transparent-letterboxed PNG through an atomic,
validated machine-local cache envelope. Library requests only visible rows and
a bounded prefetch window; leaving that window cancels work. Ready PNGs are
decoded through the renderer and uploaded on the editor's own GPU device into a
64-entry retained texture cache. Cache files are disposable and never enter a
project save, command stream, placement, or content manifest.

## Render-node lifecycle control

The normal Director/node session exists only after a node process is running.
Lifecycle control is therefore a separate, per-machine listener that supervises
one fixed child configuration. The listener accepts only `status`, `start`,
`restart`, and `stop`; executable paths and child arguments remain local and
never cross the network.

Create one 32--256 byte shared secret, restrict it to the service accounts that
need it, and install the same value on the Director and every render machine.
For example, this agent supervises node 0:

```powershell
media-server.exe `
  --lifecycle-listen=10.20.0.21:47840 `
  --lifecycle-node-index=0 `
  --lifecycle-secret-file=C:\ProgramData\MediaServer\lifecycle.secret `
  --lifecycle-child=C:\MediaServer\media-server.exe `
  --lifecycle-autostart `
  --lifecycle-child-arg=--role=render,output `
  --lifecycle-child-arg=--director-connect=10.20.0.10:45000 `
  --lifecycle-child-arg=--show-id=00000000000000010000000000000002 `
  --lifecycle-child-arg=--node-id=00000000000000030000000000000004 `
  --lifecycle-child-arg=--node-incarnation=00000000000000050000000000000006 `
  --lifecycle-child-arg=--playback `
  --lifecycle-child-arg=--output
```

`--lifecycle-autostart` is optional. It starts the configured child before the
listener accepts control requests; without it, Output Monitor starts the child
on demand. The controller authenticates requests with HMAC-SHA256 and a fresh
nonce, but it is designed for a trusted, isolated production-control LAN. Use
host firewalls, protect and rotate the secret, and do not expose the listener
to the public Internet.

In Hardware Map, set the project-wide control port and assign each Render
Server a unique numeric node index plus a numeric IPv4/IPv6 address (a blank
address means local loopback). Output Monitor derives the endpoint, rejects
duplicate node indices from lifecycle actions, and displays listener/process
state plus runtime inventory. Display targets are observations, not persistent
project state; on Windows they are active DisplayConfig targets, not a promise
of unused physical connectors.

For physical output deployment, start with the node graph after the Render
Server listener reports active displays. Drag a cable from the server's live
video-output pin to the corresponding video input. That one gesture creates
the cable, a persistent binding to the display's stable observed identity, and
a full-canvas Feed Mapping region. Repeat for each display driven by the same
render process. Select a binding row to tune its exact Feed index, Window mode,
Width, Height, Refresh numerator, and Rate divisor. Disconnecting the graph
cable removes its generated binding and region; a region shared by another
binding is retained. Feed indices refer to the numbered rows in Feed Mapping
and are never inferred from physical display order.

Fullscreen applies an exclusive native mode and refuses launch when the exact
raster and rational refresh are not advertised. Windowed and borderless modes
apply the saved raster at the detected target's desktop position. The Program
gate also requires authenticated inventory to contain every saved target; an
exclusive target must report the requested applied mode, and the live render
process must resolve every bound Feed index before acknowledging readiness. A
missing target stays
on its authored logical port and is labelled `MISSING` rather than compacting a
different connector into its place. Select that binding and use `REPAIR WITH
NEXT ACTIVE OUTPUT` to explicitly adopt a replacement. Discovery and hotplug
never rewrite bindings, cables, or project files on their own.

One lifecycle agent still supervises one fixed render-process identity. That
child now opens one presenter per saved output binding and selects each binding's
Feed index. Changing bindings is replicated as project state; Restart from
Output Monitor relaunches the same process, which reapplies the saved deployment.
Projects without output-binding nodes use the single-window output model.

## Output synchronization

Set the project policy in Hardware Map under `SYNC POLICY`: UNSYNCED,
FRAMELOCK, or EXTERNAL GENLOCK, the required raster and rational refresh, the
vendor swap-group and present-barrier identifiers, whether a failure is STRICT
or DEGRADED, and whether a late frame holds the whole group or lets outputs
advance independently. It is ordinary replicated document state and undoes like
any other edit.

Synchronization groups are derived from the cables already drawn in the same
panel: the render-server video outputs that reach a common LED processor or
wall are one group. Two outputs of one machine driving unrelated walls are two
groups. Group timing is validated exactly, so 60/1 and 60000/1001 are never
treated as the same rate.

Output Monitor reports what the cluster can actually prove. The machine-local
lifecycle agent answers for the sync device, the house reference, and each
display target's lock state; the render process answers for present-barrier
membership and its missed, late, held, and repeated frame counts. Until every
condition holds the surface names the specific unmet one - `SYNC UNVERIFIED`,
`REFERENCE LOST`, `SWAP BARRIER INCOMPLETE`, `DISPLAY TIMING MISMATCH` -
instead of a generic green state, and a STRICT policy refuses to promote
Program.

This build carries no vendor sync integration, and SDL_GPU swapchains cannot
join a present barrier, so a physical policy correctly reports
`SWAP BARRIER INCOMPLETE` on this presentation path. To rehearse the operator
procedures without a genlock card, pass `-SyncSimulate locked`,
`reference-lost`, `reference-idle`, or `no-device` to
`scripts/run-editor-demo.ps1`; simulated samples are marked as such all the
way to the surface and can satisfy only a DEGRADED policy.

Under HOLD GROUP a render process that cannot produce a complete frame for one
of its outputs repeats the previous complete frame on all of them: on an LED
wall one repeated frame everywhere is far less visible than one segment
advancing while its neighbour stands still.

## Cluster reliability and timing

Output Monitor now exposes current control-plane health, live and catching-up
session counts, release/pacer lag, cumulative incidents, and whether the
rotating post-show diagnostic log is recording. It reports the exact
synchronization verdict beside the control-plane one. A node acknowledgement
proves replicated state adoption, never simultaneous physical scan-out or even
physical cable/LED presence, so the two lines never merge into one green
online state.

The currently certified baseline is one Director plus two render nodes at 120
control frames per second for ten minutes with bounded replay and edit-shaped
durability. Node loss/restart, checkpoint/replay convergence, heartbeat
timeout, slow-node, WAL, and release-ledger recovery are automated Release
tests. Larger clusters and real project/media/device loads require their own
pre-show soak; the 64-session runtime ceiling is not a 64-node performance
claim.

See [cluster-operations.md](cluster-operations.md) for the exact supported
envelope, soak commands, JSONL diagnostic location, failure recovery procedure,
firewall/service/secret-rotation guidance, and the explicit future
PTP/genlock/frame-lock integration boundary.

## Authoring and project files

Use Library to import media or models, then drag them into Stage. The Inspector
and Timeline author ordinary document commands, so their changes participate in
replication and undo/redo. The adjacent history actions in the editor toolbar
also map to **Ctrl+Z** for undo and **Ctrl+Shift+Z** or **Ctrl+Y** for redo. A
new edit after undo starts a new branch and clears redo only for that authoring
context. Selecting a Scene/Stage item or a Compositor node/canvas chooses which
of those two histories the global actions navigate. A clip dropped into a
scene lands *placed* on the
timeline, with its own natural length in show frames, and is edited there - see
[Editing clips on the timeline](#editing-clips-on-the-timeline).

Effects can be applied to a selected element or connected in that scene's
compositor graph; the current node palette contains Scene Layer, Brightness,
Saturation, Levels, Lift/Gamma/Gain, Contrast/Pivot, Channel Mixer, White
Balance, Curves, Film Grain, Gaussian Blur, Box Blur, Directional Blur, Radial
Array, Composite, and Output. The Effects palette names the layer a drop would
land on, in its heading and on the drag itself, so a drag says what it will do
before it is let go.

Each effect in the Inspector carries the layer list's eye. Switching one off
authors the document's own enabled flag, so the effect, its parameters, and its
key channels all survive - which is what comparing a look against the one before
it needs, and what deleting the effect instead would throw away. A switched-off
effect keeps its row rather than disappearing from the panel that would switch
it back on. The Color Correction panel grades what is reaching the picture, so
it says when a layer's colour effects are off instead of offering controls that
would do nothing. That panel exposes the structured tonal controls without
flattening their authored order, and how those effects are computed is set once
per project under [Project settings](#project-settings). The Texture panel
exposes Film Grain's exact seed, pixel scale, chroma ratio, and monochrome/RGB
mode while Amount is also keyframeable in Inspector and Timeline. Each scene
owns its own graph, which is what lets the Compositor panel be used in Preview
without touching what Program is rendering - see
[Program and Preview](#program-and-preview).

Scene, Stage, and Timeline share one typed multi-selection for scene elements.
Ctrl toggles membership and Shift selects a visible range; the Inspector shows
`MIXED` for disagreeing compatible values and applies an accepted edit only to
unlocked compatible members. Select top-level elements and use `GROUP` in the
Scene footer to create a replicated transform group without moving them in
world space. Selecting that group enables `UNGROUP`, which preserves its direct
children and selects them. Rejected hierarchy or transform operations create no
history entry.

Hardware Map and Feed Map are setup panels. Feed mappings support flat,
horizontal-arc, and imported-mesh surfaces. For an imported projection mesh,
split the calibrated LED geometry externally, retain a shared model origin, and
import each feed mesh with `+ IMPORT UV MESH`. Its persistent projection asset
is referenced by absolute path and reported for readiness exactly like media
and models. UV0 represents the full connector image; it is not material UV
space.

A surface reaches an output by being dragged onto it. Drag a row out of
`PROJECTION SURFACES` and drop it on the output that drives that part of the
wall - its row in the list, or its rectangle in the mapping area, whichever the
operator is already looking at - and the drop switches that output to the
surface and takes its whole raster as the region. Every output that can accept
the drop outlines itself while one is in flight. The details rail names the
surface an output is on rather than offering a stepper to hunt through them:
cycling blindly past eight walls to find the ninth is what a control looks like
when the gesture was never decided.

Feed Mapping treats application windows and viewports as separate spaces. A
window's raster comes from its Hardware Map output binding and stays fixed;
the colored rectangle inside it is the feed's destination viewport. Moving or
resizing that rectangle changes only `viewport_x`, `viewport_y`,
`viewport_width`, and `viewport_height`, leaving the scene/surface source crop
unchanged. Pixels in the window outside the viewport are black. The panel lays
configured windows side by side. A four-node setup can therefore show four
3840x2160 windows across a 15360x2160 mapping area, each with a 3456x1356
viewport. An NVIDIA Mosaic setup instead authors one 15360x2160 window with
four independent 3456x1356 viewports at x=0, 3840, 7680, and 11520. Any number
of viewport rows may target the same logical window index; they are composited
into that window in stable document order. Every viewport explicitly authors
all four viewport properties.

Save and open complete projects through File > Save Project / Open Project.
`.mstage` saves are checksummed and atomically replaced. Recovered WAL state
takes precedence at startup; otherwise the last successfully opened/saved
project is offered through recent-project recovery. A damaged or inaccessible
project safely falls back to untitled startup.

## The 3D renderer

`engine/rhi` and `engine/render` are the two seams below the scene layer.
`engine/rhi` is a small, backend-neutral GPU device - textures, storage
buffers, pipelines, and three draw shapes (instanced quads, a depth-tested
"pulled" draw with no vertex/index buffer bindings, and a fullscreen textured
pass) - with SDL_GPU as the only backend today. Nothing above it names SDL,
which is what leaves room for a Vulkan-direct backend when bindless or
mesh-shader work needs it. `engine/render` is everything that turns a
`scene::Scene` value into pixels: it never reads the document, and the scene
layer never learns a GPU exists. A render node and the editor's Program/Stage
viewports share this exact path, so what an operator sees while authoring is
what the wall shows.

**One camera, many exact sub-frustums.** The camera is authored once, against
the project's shared `Canvas`, and a render node never adjusts it. Instead each
node narrows the camera to its own `CanvasRect` - an off-axis perspective
projection whose near-plane window is a sub-rectangle of the full one, with the
eye point and near/far planes untouched (`scene::view_for_slice`). Two adjacent
nodes therefore evaluate the identical floating-point expression for their
shared edge, so a four-node wall's seams are bit-exact rather than merely
close, and a four-node wall renders four quarter-resolution frames rather than
four full ones cropped afterward.

**`SceneRenderer`: one scene, one target.** Given a `Scene` and a slice, it
resolves every mesh key through an in-process cache - procedural primitives
generate on a miss, imported glTF/OBJ geometry loads and uploads once per
source file - then writes one GPU instance record per drawable. Opaque
instances stay batched by mesh for one instanced draw each; anything that needs
destination pixels (non-Normal blend modes, or any media clip, since a decoded
frame may carry alpha even at full layer opacity) is queued instead, sorted
back-to-front with document order and insertion order as tie-breakers, and
drawn as its own depth-tested pass with its own fixed-function blend equation.
Lighting is a small, deliberately bounded PBR core in `scene.frag.hlsl` -
Trowbridge-Reitz distribution, height-correlated Smith visibility, Schlick
Fresnel, Lambert diffuse, a two-colour hemisphere ambient term standing in for
image-based lighting, ACES tone mapping, and explicit sRGB encode/decode.
Shadows, normal maps, IBL, and any global illumination are named absences, not
oversights - each needs authored data (tangents, filtered cubemaps) or a
different pass this slice does not try to fake.

Brightness and Saturation are inline material/media operations, so active
values stay in the ordinary shared-depth draw. Saturation is colourfulness
around Rec.709 luminance, from monochrome at `0` through identity at `1` to a
bounded maximum of `4`; it runs in linear light for Exposure projects and on
encoded display values for Gain projects. Both lit geometry and decoded media
use the same contract. Brightness and Saturation factors retain one product
across first-class effects and graph modifiers, then apply their final clamp
and project response once. Blur remains a later isolated spatial operation. A
graph or first-class effect stack that authors an active inline operation after
Blur is rejected visibly instead of being silently reordered; identities are
inert. Because graph modifiers currently resolve back to authored scene nodes,
an Output-reachable graph also permits only one Scene Layer for a given source;
a duplicate is diagnosed rather than allowing two branches to leak effects
into each other.

Levels, Lift/Gamma/Gain, Contrast/Pivot, Channel Mixer, White Balance, and
Curves stay
as typed operations in that same bounded authored-order inline program; they
are not reduced to interchangeable scalars. Both lit geometry and media execute
the identical operation sequence. White Balance resolves its documented
D65/Bradford adaptation to a binary32 matrix in the model, while Channel Mixer
applies its authored row-major 3x3 matrix plus RGB offset without an intermediate
clamp. Curves validates strict master/optional RGB control points and derives
the same 1024-entry binary32 monotone-cubic LUT on every node; the GPU samples
that derived LUT without turning it into a second authored representation.
Identity operations remain visible for editing but add no shader op or render
pass.

Film Grain is another ordered inline operation. It adds deterministic
zero-mean monochrome or RGB texture without changing alpha, depth, or bounds.
Its integer hash uses the replicated seed, exact 64-bit show frame, authored
canvas-pixel coordinate, and channel; independently rendered adjacent wall
slices therefore produce byte-identical noise at their shared coordinates.
Amount `0` is identity and adds no shader operation.

**The compositor graph runs as isolated render passes, not shader tricks.**
`CompositorSceneExecutor` evaluates the typed graph (`engine/compositor`) into
an ordered stack of layers, renders each into its own transparent offscreen
`SceneRenderer` target, spatially blurs it if the layer's style calls for a
blur, and composites it over an accumulator texture with its blend mode - one
fullscreen pass per layer, ping-ponging between two textures so no layer reads
the target it is writing. A project with no authored graph gets the same
treatment implicitly: consecutive Normal/no-blur drawables stay one pass,
and only a layer that actually needs isolation (a blur, a destination-reading
blend) pays for one.

**Isolated layers keep the frame's depth.** Isolating a layer costs it the
shared depth buffer, and a flat premultiplied composite has no way to put that
back: adding a brightness or blur node to an object used to lift that object in
front of everything else in the scene. Each pass is therefore also handed the
drawables belonging to *other* passes that stand in front of it, and renders
them for their depth alone - they write depth and return a fully transparent
black, so the layer is occluded exactly as it would be in a single-pass render
and nothing of theirs reaches the colour target. A clip does this through an
alpha-tested depth pass, so it occludes exactly where its own pixels are
opaque. Only crisply opaque geometry qualifies: a translucent or additively
blended drawable does not hide what is behind it, and a blurred one has no hard
silhouette to occlude with, so neither writes depth on another layer's behalf.
Depth-only geometry is biased a few thousand float steps away from the camera,
so two coplanar layers are still ordered by the composite rather than
cancelling each other in a reversed-depth test neither can win. Program, Stage, and each side of a Preview dissolve
composite through their own scene's graph independently - see
[Program and Preview](#program-and-preview) - which is why rehearsing a
different composite in Preview never touches what Program is showing.

**Media and imported geometry share one decode contract.** `MediaSource` turns
a `(source, frame_index)` pair into pixels, frame-exact and pure - the same
pair always yields the same bytes on every node, with direct seek to any frame
rather than nearest-keyframe. Image sequences decode with stb_image; video
decodes through FFmpeg's demuxer/decoder registry - whatever codecs the linked
build carries - on an async worker so decode never blocks the frame path.
`MediaCache` is the GPU residency on top: four resident frames
or 256 MB, whichever binds first, evicted oldest-first, with current and
committed frames pinned across a frame so a reused slot is never overwritten
mid-draw. `model_loader` reads glTF/GLB and OBJ into the same vertex/index
arenas `SceneRenderer` already uses, preserving per-primitive material
boundaries and PBR textures so an imported asset shades through the identical
pipeline as a procedural one.

**Program transitions and the output stage.** `SceneMixer` blends two fully
rendered scene textures by one control-frame-driven scalar - never
object-level, so a dissolve is independent of layer count or effects and every
node performs the identical deterministic mix. `ProjectionMeshWarp` is the
nDisplay-style path for a curved or off-axis LED surface: the scene renders
once through a generalized asymmetric view, then this pass rasterizes the
physical surface in its own UV0 space and samples the scene render through
each vertex's projected clip position, so one warp handles a curved wall
instead of hand-aligning a camera per LED column. `OutputTransform` is the
final, deliberately trivial stage - a straight sample today - held as the seam
where a per-panel calibration LUT and dither land later, applied last, after
warp and after the switcher's dissolve.

**Deliberately absent, on purpose:** frustum culling (these scenes are small;
a wrong cull is worse than one slow frame), instancing beyond per-mesh
batching, shadows, and any form of global illumination. Each has a place in
[media-server-architecture-plan.md](media-server-architecture-plan.md); adding
one is additive to this seam rather than a rewrite of it.

## Engineering guardrails

- Keep Director/node state replication small, explicit, and bounded.
- Keep authored state separate from transient machine health and discovery.
- Treat deterministic document evaluation as the source of agreement between
  nodes; do not distribute full-wall pixels as a shortcut.
- Keep platform/GPU concerns behind the RHI and SDL seams.
- Keep file reads, hashing, and blocking I/O off the control loop. Presence is
  a stat; content is an operator-invoked action on a worker.
- Do not add periodic work to the control loop. A recheck that costs more than
  a stat needs a control, not a timer.
- Measure a stall before fixing it, and measure it again after. Frame timing
  and `pacer-lag` in the Director status line are the instruments; a plausible
  cause that has not been timed is a guess.
- Prove behavior with headless model tests, protocol/session tests, GPU tests,
  and the local multi-process demos before broadening the product surface.
