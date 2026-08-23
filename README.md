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
decoding, the 2D compositor, geometric warp, ST 2110 receive, and the retained
operator UI are the next product-facing layers. The longer-term direction is
recorded in [media-server-architecture-plan.md](media-server-architecture-plan.md).

There is no shipped format or protocol to preserve yet. Schemas and project
files can change while the product takes shape.

## Repository layout

```text
engine/core       shared types and allocation-free primitives
engine/doc        immutable project document
engine/cmd        commands, revisions, undo, and node command bus
engine/wire       current FlatBuffers project protocol
engine/persist    project WAL and release log
engine/show       show clock, transport, and frame presentation contracts
engine/net        Director/node TCP replication
engine/scene      the document's view of a 3D scene (headless, no GPU)
engine/output     the document's view of the output stage: the feed map
engine/rhi        backend-neutral GPU device seam
engine/render     draws a scene, and applies a feed map, onto a GPU target
engine/ui         SDL3 window and output presenters
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

The server is a development harness for the runtime foundation. Playback and
window options are useful for proving synchronized output, but they are not
the operator application yet.

## Product guardrails

- Keep the Director/node boundary small and explicit.
- Prefer project/edit persistence over frame-by-frame recovery machinery.
- Keep platform code behind the RHI and SDL presenter seams.
- Build the compositor and operator workflow before adding more distributed
  recovery features.
- Treat Windows, macOS, and Linux as first-class build targets from the start.
