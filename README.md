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
  recent-project recovery; Program/Rehearsal scene copies with a short dissolve.
- Authenticated lifecycle agents that can start, restart, stop, and report on
  fixed local render processes, including a transient machine/GPU/display
  inventory for the Output Monitor.

The current architecture and its intentional limits are described in
[media-server-architecture-plan.md](media-server-architecture-plan.md). The
near-term work is in [next-steps-plan.md](next-steps-plan.md).

## Current limits

- Self-contained imported media, models, and calibration meshes are identified
  by SHA-256 and copied into a verified content-addressed store. Compound glTF,
  scene OBJ, and image-sequence packages still need dependency manifests before
  they can be treated as portable.
- The output path is an SDL desktop window. SDI, NDI, ST 2110, PTP/genlock,
  tracked cameras, and external render/plugin interfaces are not implemented.
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
that node's store; STOP remains available. Program has the stricter gate: every
configured render process must also be live and must acknowledge, in a
validated heartbeat, that it installed the exact same manifest into its runtime
resolver. A disconnect, restart, stale manifest, duplicate node index, pending
Hardware Map change, or unresolved local overlay therefore blocks the
transition even when the files are already present. Unused Library assets do
not block Program.

Legacy unverified references and identified-but-external assets are shown as
unverified and fail closed when render nodes are configured. They remain usable
for a local one-machine project when their local paths are usable, so opening an
older project does not silently disable the Program/Preview workflow merely
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

## Cluster reliability and timing

Output Monitor now exposes current control-plane health, live and catching-up
session counts, release/pacer lag, cumulative incidents, and whether the
rotating post-show diagnostic log is recording. It deliberately reports
`PTP/GENLOCK/FRAME LOCK UNVERIFIED`: a node acknowledgement proves replicated
state adoption, not simultaneous physical scan-out or even physical cable/LED
presence.

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
in the shared graph; the current node palette contains Scene Layer,
Brightness, Blur, Radial Array, Composite, and Output.

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
