# Media Server

Media Server is an early-stage, cross-platform virtual-production media-server
prototype. It is building toward an operator workflow familiar from motion
graphics and compositing tools: author a project in a layer/timeline editor,
then run the same committed project on one machine or a synchronized set of
render/output nodes.

The repository is a working engineering baseline, not a production-ready media
server or a stable file/protocol format. The project, wire, and persistence
schemas may change while the product is being defined.

## What works today

- An immutable document and frame-scheduled command system with undo contexts,
  checkpoints, node replay, frame sealing, and Director/node TCP sessions.
- A Director-only, dockable native editor with Library, Scene, Stage, Inspector,
  Timeline, Compositor, Hardware Map, Feed Map, and Output Monitor panels.
- Scene authoring for primitives, lights, imported glTF/GLB/OBJ models, media
  quads, materials, blend modes, brightness/blur effects, and keyed component
  values. The timeline supports transport, loop/range control, auto-keying,
  per-component keys, and key retiming.
- GPU rendering through SDL GPU and a small RHI seam. The renderer supports
  PBR-style materials, image sequences and FFmpeg-decoded video, a typed
  compositor graph, and desktop-window presentation.
- A document-backed output stage. Legacy projects select one feed by process
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
near-term work is in [next-steps-plan.md](next-steps-plan.md).

## Current limits

- Self-contained imported media, models, and calibration meshes are identified
  by SHA-256 and copied into a verified content-addressed store. Compound glTF,
  scene OBJ, and image-sequence packages still need dependency manifests before
  they can be treated as portable.
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

Projects saved before compositor graphs became scene-scoped carry graph objects
with no owner. Those are adopted into the Program scene, which is the scene
they were already being composited into, so an older project opens looking
exactly as it did.

## Portable project assets

Library imports are persistent project assets, separate from scene placement.
On import, the Director hashes the source and records its exact SHA-256, byte
size, probe metadata, and distribution policy in the project. Self-contained
files are installed under `--asset-store=PATH` in a content-addressed layout.
The store retains a canonical digest-only blob plus a verified,
extension-preserving alias used by the existing media/model loaders. Reimporting
the same exact source updates the existing asset record without changing its
stable project ID.

For a cluster, configure the Director and lifecycle agent on every render
machine with the same shared-store location, normally a mounted/UNC path or a
locally synchronized store. The default is an `assets` directory beside the
per-user recent-project settings. Asset source paths never cross the lifecycle
protocol: readiness requests contain only the digest and byte size, and each
agent re-hashes its local canonical blob before reporting `READY`, `MISSING`,
or `CORRUPT`.

Output Monitor displays the required/ready counts for each render node. START
and RESTART remain disabled until every required portable asset is verified in
that node's store; STOP remains available.

Asset readiness is reported, not enforced against the switcher. A disconnect,
restart, stale manifest, duplicate node index, pending Hardware Map change, or
unresolved local overlay shows on the Output Monitor, but it does not hold a
Program promotion: getting content onto render nodes is handled by external
sync tooling in this build, so the operator - not the readiness gate - decides
when a look goes on air. Unused Library assets are not counted at all.

Legacy unverified references and identified-but-external assets are shown as
unverified. They remain usable for a local one-machine project when their local
paths are usable, so opening an older project does not silently degrade merely
because it predates portable packaging.

Import hashing and SharedStore population run on one bounded background worker,
with at most 16 queued, active, or completed jobs. Loading another project
discards waiting work and cancels the hash or bounded copy that may already be
running; cancelled work never publishes a stale project edit. Media/model
metadata probing performed by the file chooser is still synchronous. Finish
exporting a source before importing it; if the file changes between that probe
and background hashing, reimport it so the stored probe metadata and verified
bytes agree.

Lifecycle readiness re-hashes canonical blobs rather than trusting file names
or timestamps, so very large manifests should live on fast local or
high-throughput shared storage. The lifecycle listener uses a fixed worker pool
and permits only one hash pass at a time; competing readiness requests fail
quickly instead of starving status/start/restart/stop. A readiness response has
its own configurable client budget (120 seconds by default, capped at 24 hours)
and a timeout remains fail-closed until a later poll succeeds. Agent and
Director shutdown cancel active readiness hashing and interrupt an outstanding
readiness receive before joining their bounded worker pools.

Playback keeps verified machine-local paths in a runtime-only overlay keyed by
asset role and placement identity. A manifest change installs a cheap
filesystem-free deny overlay immediately; first/full verification and later
retries all run on one persistent latest-wins resolver thread per playback
process. Missing, corrupt, or unreadable objects are retried every 30 seconds,
while already verified objects are not re-hashed. Request ID, manifest, project
generation, and resolution roots fence stale completions. Hash and alias-copy
loops check cancellation at 64 KiB checkpoints. Shutdown always joins this
worker so it cannot outlive the process; a pathological filesystem metadata,
open, or read operation that the operating system does not cancel can still
delay that join.

The current portable unit is one self-contained file. Image sequences that use
`#`, text `.gltf` files with external buffers/images, and scene `.obj` files
that may reference MTL/textures are deliberately classified as
`DEPENDENCIES REQUIRED`; the primary-file hash alone is not claimed to make
those packages ready. Binary `.glb`, ordinary single-file media, and projection
`.obj` meshes are supported by the shared store.

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
  --asset-store=Z:\MediaServerAssets `
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
to the public Internet. The lifecycle agent forwards its configured
`--asset-store` to the fixed child; do not add it as a lifecycle child argument.

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
Projects without output-binding nodes retain the legacy one-window behavior.

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
replication and undo. Effects can be applied to a selected element or connected
in that scene's compositor graph; the current node palette contains Scene
Layer, Brightness, Blur, Radial Array, Composite, and Output. Each scene owns
its own graph, which is what lets the Compositor panel be used in Preview
without touching what Program is rendering - see
[Program and Preview](#program-and-preview).

Hardware Map and Feed Map are setup panels. Feed mappings support flat,
horizontal-arc, and imported-mesh surfaces. For an imported projection mesh,
split the calibrated LED geometry externally, retain a shared model origin, and
import each feed mesh through the surface chooser. Its persistent projection
asset is hashed and resolved through the same verified store as media and
models. UV0 represents the full connector image; it is not material UV space.

Save and open complete projects through File > Save Project / Open Project.
`.mstage` saves are checksummed and atomically replaced. Recovered WAL state
takes precedence at startup; otherwise the last successfully opened/saved
project is offered through recent-project recovery. A damaged or inaccessible
project safely falls back to untitled startup.

## Engineering guardrails

- Keep Director/node state replication small, explicit, and bounded.
- Keep authored state separate from transient machine health and discovery.
- Treat deterministic document evaluation as the source of agreement between
  nodes; do not distribute full-wall pixels as a shortcut.
- Keep platform/GPU concerns behind the RHI and SDL seams.
- Prove behavior with headless model tests, protocol/session tests, GPU tests,
  and the local multi-process demos before broadening the product surface.
