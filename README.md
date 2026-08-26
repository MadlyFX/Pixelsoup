# Media Server

An early-stage virtual-production media server for Windows, macOS, and Linux.
The product direction is an After Effects-style operator surface: a layer and
timeline workflow backed by a real-time compositor, with the same project able
to drive one machine or a synchronized group of output nodes.

This repository is intentionally focused on the foundation needed for that
product:

- immutable project/document state and command-based editing;
- frame-scheduled transport and deterministic node replication;
- a small current wire protocol and TCP session layer;
- a project WAL and release log for auditability, not exact-frame crash resume;
- a backend-neutral RHI and SDL3 output shell;
- a single server executable with Director and node roles.

The renderer draws a replicated 3D scene, and the output stage feed-maps each
node's region of it so a synchronized group tiles one continuous image - the
scene and the feed map both authored into the document and derived identically
on every node. This targets LED walls, so adjacent regions abut pixel-exact
with no node-to-node blend, and output is a known transfer (sRGB today); the
per-panel calibration LUT is left to the downstream LED processor. Media
decoding, the 2D compositor, physical planar/arc projection, and UV-mesh warp
are present; ST 2110 receive and tracked-camera frustum workflows remain later
product-facing layers. The longer-term direction is recorded in
[media-server-architecture-plan.md](media-server-architecture-plan.md).

There is no shipped format or protocol to preserve yet. Schemas and project
files can change while the product takes shape.

## Repository layout

```text
engine/core       shared types and allocation-free primitives
engine/doc        immutable project document
engine/cmd        commands, revisions, undo, and node command bus
engine/wire       current FlatBuffers project protocol
engine/persist    atomic project files, project WAL, and release log
engine/show       show clock, transport, and frame presentation contracts
engine/net        Director/node TCP replication
engine/scene      the document's view of a 3D scene (headless, no GPU)
engine/output     the document's view of the output stage: the feed map
engine/rhi        backend-neutral GPU device seam
engine/render     draws a scene, and applies a feed map, onto a GPU target
engine/ui         SDL3 window and output presenters
engine/editor     bespoke production editor (Scene/Library, Stage, Inspector, timeline/nodes/output)
engine/shaders    optional build-time shader bytecode
apps/server       Director/node command-line application
tests/unit        fast core, document, command, and wire tests
```

## Build

Requirements:

- CMake 3.28 or newer;
- Ninja;
- a C++20 compiler (MSVC 2022, Clang 18+, or Apple Clang);
- vcpkg in manifest mode.

Set `VCPKG_ROOT` to a vcpkg checkout, then configure one of the presets:

```powershell
cmake --preset windows-msvc-debug --fresh
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

For an operator window, use the GUI preset. It installs SDL3 through the vcpkg
`gui` feature and builds the SDL/RHI output path:

```powershell
cmake --preset windows-msvc-debug-gui --fresh
cmake --build --preset windows-msvc-debug-gui
ctest --preset windows-msvc-debug-gui
```

The equivalent headless Linux and GUI macOS presets are
`linux-clang-debug` and `macos-clang-debug-gui`.

## Run

A headless node can be started without a display:

```text
out/build/windows-msvc-debug/apps/server/media-server.exe --role=output
```

Run a Director with its project log and listener:

```powershell
media-server.exe --role=director,output `
  --wal=project.wal `
  --director-listen=127.0.0.1:45000 `
  --show-id=00000000000000010000000000000002 `
  --director-epoch=00000000000000030000000000000004
```

The server is a development harness for the runtime foundation. With
`--playback --edit`, a Director opens the bespoke docked operator editor; the
ordinary playback and window options remain useful for proving synchronized
output independently.

For a local editor session from the repository root:

```powershell
out/build/windows-msvc-debug-gui/apps/server/media-server.exe `
  --role=director --director-listen=127.0.0.1:45000 `
  --show-id=00000000000000010000000000000002 `
  --director-epoch=00000000000000030000000000000004 `
  --playback --edit --surface=1600x900 `
  --wal=editor-test.wal
```

### Render-node lifecycle control

Output Monitor can start, restart, and stop one configured Render Server or the
whole valid cluster. This uses a separate per-machine agent because the normal
Director session cannot launch a render process that is not running yet. The
agent accepts only four fixed commands (`status`, `start`, `restart`, `stop`);
the child executable and every argument are local startup configuration and
never arrive over the network.

First create one strong shared secret and install the same contents on the
Director and every render machine. This Windows PowerShell 5.1-compatible
example creates the parent directory and writes 32 random bytes as 64
hexadecimal characters, avoiding shell quoting and binary-newline issues:

```powershell
$secretDirectory = "C:\ProgramData\MediaServer"
$secretPath = Join-Path $secretDirectory "lifecycle.secret"
[void](New-Item -ItemType Directory -Force -Path $secretDirectory)

$bytes = New-Object byte[] 32
$random = [Security.Cryptography.RandomNumberGenerator]::Create()
try {
  $random.GetBytes($bytes)
} finally {
  $random.Dispose()
}
$secretHex = [BitConverter]::ToString($bytes).Replace('-', '')
[IO.File]::WriteAllText($secretPath, $secretHex, [Text.Encoding]::ASCII)
```

The snippet does not set an ACL because the correct service identity is
deployment-specific. Before starting either process, restrict the directory
and file to the Director/render service accounts and the administrators who
manage them; do not rely on inherited `ProgramData` permissions.

Run one agent on each render machine. It owns exactly one node index and adds
the matching `--node-index` child argument itself:

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

`--lifecycle-autostart` starts the fixed local child during agent startup,
before the listener accepts control requests. This is convenient when the
listener service itself starts at machine boot. Omit it when the render process
should remain stopped until the Director uses **START** or **START ALL**. In
either case, later Output Monitor controls operate on the same supervised
process.

Start the Director editor with its local credential:

```powershell
media-server.exe --role=director --playback --edit `
  --director-listen=10.20.0.10:45000 `
  --show-id=00000000000000010000000000000002 `
  --director-epoch=00000000000000030000000000000004 `
  --lifecycle-secret-file=C:\ProgramData\MediaServer\lifecycle.secret `
  --wal=editor-test.wal
```

In Hardware Mapping, set the project-wide **Control port** once, then give each
Render Server its numeric **Node index** and a numeric IPv4 or IPv6 **Host**
address. DNS host names are not resolved. A blank Host means the local machine
(`127.0.0.1`). The listener target is derived automatically from Host, Control
port, and Node index and shown read-only; operators no longer type a redundant
`host:port` string on every server. The default range begins at 47840, matching
the bundled launch scripts. Duplicate node indices show `CONFIG ERROR` and are
excluded from all lifecycle actions. Output Monitor polls status on bounded
background workers and provides START ALL, RESTART ALL, and caution-styled STOP
ALL in addition to per-node controls.

Each Render Server also owns its output-window configuration in the project:
**Window** mode (`INHERIT`, `WINDOWED`, `BORDERLESS`, or `FULLSCREEN`), signed
**Desktop X/Y**, and **Width/Height**. A connected output node recreates its
presenter when those applied window settings change, so moving a feed to
another monitor does not require rebuilding lifecycle child arguments. The
authored **Refresh** and **Rate div** values are currently saved and displayed
as intended-mode metadata; the SDL presenter does not yet select or enforce an
operating-system display mode or refresh rate. `INHERIT` is the compatibility
mode and keeps the process's original launch settings. Negative desktop
coordinates are supported for displays arranged to the left or above the
primary display.

The same authenticated listener reports a cached, read-only machine inventory
even while its render child is stopped. Hardware Mapping uses that transient
report for listener/process state, host/OS, logical CPU count, RAM, GPU/VRAM,
and distinct active display-output pins. Inventory is never written into the
project: unplugging a display cannot dirty the show or erase a saved cable, and
a previously linked but missing output remains visible as a warning pin. An
unavailable probe is shown as `OUTPUTS UNKNOWN`/`UNSUPPORTED`, not as a false
zero-output result.

On Windows, display pins come from DisplayConfig targets and are therefore
labelled **active outputs**. MST branches, indirect/virtual displays, remote
sessions, and driver policy mean this is not guaranteed to equal the number of
unused physical sockets on a GPU; exact disconnected-connector inventory will
require optional vendor backends. Output slots 1-32 are stable logical project
ports, while the observed target order is runtime-only. At this stage a cable
documents the intended hardware chain; playback still launches one output feed
per listener `node_index`, so assigning several feeds to several connectors is
a separate multi-output binding milestone.

The lifecycle protocol uses a fresh OS-generated nonce and HMAC-SHA256, but it
is intended for a trusted, isolated production-control LAN, not the public
Internet. Restrict TCP 47840 with host/firewall rules to the Director address,
protect the secret with service-account file permissions, and rotate it as an
operational credential. The bundled listener processes one bounded request at
a time. Stop/restart is a forced process-tree teardown: a Windows Job Object or
POSIX process group prevents render helpers from being orphaned. On POSIX the
fixed child inherits the agent's working directory; on Windows it starts in the
child executable's directory.

The editor workflow is document-backed. Use File > Save Project or Open Project
to write and restore a complete `.mstage` project. Saves use a checksummed,
atomic replacement so an interrupted write does not destroy the previous file.
On startup, recovered session state takes precedence. The default WAL regains
the recent project's path only when its recovered content exactly matches that
saved project (apart from live transport); otherwise it remains untitled and
dirty and requires Save As. An explicitly named WAL never borrows the global
recent-project path. With no recovered WAL, the last project successfully
opened or saved is reopened. A missing, unreadable, or damaged recent project
falls back safely to normal untitled startup.

Import Media or Models in Library, then click a row or drag it into Stage;
select an element and click or drag a Brightness/Blur effect; use the diamonds
beside Inspector or Timeline properties to key the current frame. Effects can
also be dropped onto the selected object's Properties panel. Dragging an effect
onto Compositor creates a shared graph node. The graph flows Effect/Radial ->
Layer -> Composite -> Output; dropping a new connection onto an occupied single
input rewires it in one undoable edit.

Hardware Map and Feed Map are hidden from the everyday workspace. Click Setup
in the application bar to open them together in a floating dock. Drag a
hardware output port to a compatible input, middle-drag to pan, and use the
wheel to zoom; select a feed region and use DELETE REGION or Delete to remove
it. The Hardware palette owns the project-wide Control port. A Render Server's
details rail owns its Host, node identity, and output-window placement; walls,
processors, and operator machines intentionally have no process-control fields.
Close any panel with its tab X (or close a whole floating dock with its
title-bar X), then restore it from Window; its previous docking position is
remembered.

Feed Mapping supports flat surfaces, generated horizontal arcs, and one UV0
projection mesh per output. The render process must use the Output path
(`--output` in its fixed service configuration); raw Scene mode deliberately
bypasses Feed Mapping and shows the ordinary scene camera. For the mesh
workflow, split the full LED geometry
externally while retaining a shared model origin, select an output region, and
click IMPORT UV MESH. The import and feed assignment are committed together.
OBJ, glTF, and GLB are accepted; UV0 is the complete connector image, so its
region is locked and each split piece uses the same origin, yaw, and uniform
stage scale instead of being independently normalised. Mesh files are external
assets for now, so every render machine must resolve the saved path to the same
file until project asset packaging is added.

REHEARSE creates an isolated scene copy on first use, CREATE COPY duplicates
the displayed scene into Rehearsal, and SWAP promotes it with a short Program
dissolve; scene edits remain locked only while that dissolve is active.

## Product guardrails

- Keep the Director/node boundary small and explicit.
- Prefer project/edit persistence over frame-by-frame recovery machinery.
- Keep platform code behind the RHI and SDL presenter seams.
- Build the compositor and operator workflow before adding more distributed
  recovery features.
- Treat Windows, macOS, and Linux as first-class build targets from the start.
