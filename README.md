# Media Server

This repository is the implementation of the system described in
[`media-server-architecture-plan.md`](media-server-architecture-plan.md) and
[`implementation-plan-cpp.md`](implementation-plan-cpp.md).

The implemented foundation establishes the non-graphics control plane:

- C++20 engine libraries with strict warnings and RTTI disabled;
- strongly typed IDs and a caller-owned frame arena;
- immutable document snapshots backed by a structurally shared radix trie;
- Director-owned command IDs, explicit base/result revisions, and frame seals;
- deterministic command fingerprints covering both forward and inverse payloads;
- node-side reordering, completeness validation, and pre-staged frame results;
- transaction history, context-aware conflict detection, and command-based undo;
- stream checkpoints for snapshot resync and stale-traffic rejection;
- strict, bounded FlatBuffers v1 codecs with frozen compatibility fixtures;
- a crash-recoverable sealed-frame WAL and typed replay into fresh nodes;
- tokenized Director prepare/commit with durable append before publication;
- bounded genesis WAL startup recovery into unpublished Director and local-node
  state;
- a pinned standalone-Asio transport with strict size-prefixed TCP framing;
- a separately versioned cluster-session codec with WAL cursor resume, bounded
  replay, staged full-checkpoint fallback, and durable live-frame delivery;
- immutable reconnect snapshots, transactional complete-frame node ingestion,
  node-incarnation lease fencing, and bounded/cancellable TCP I/O;
- an atomic captured-tail/live handoff through a bounded, nonblocking Director
  session hub, with explicit prepared/committed frame acknowledgements and no
  acknowledgement encoding or socket I/O on the node show-clock boundary;
- a fixed-worker Director TCP service with aggregate receive admission,
  reconnect/backlog draining, exact session fencing, and deterministic shutdown;
- a supervised outbound node connector with cancellable dialing, bounded
  reconnect backoff, explicit checkpoint-boundary activation, and one live
  reader plus one preallocated-control writer;
- steady-clock heartbeat supervision and bounded directional graceful shutdown,
  sharing the same exact sequence lineage as frame traffic;
- a CRC-protected release ledger that binds each contiguous release decision to
  the exact durable WAL entry and refuses unresolved WAL suffixes on restart;
- a read-only unresolved-release inventory that authenticates the released WAL
  prefix and identifies the first still-unreleased durable frame;
- a CRC-chained cohort intent journal plus a recovery-only coordinator that
  checkpoints nodes at the durable release frontier, accepts only the exact
  planned node incarnations, and advances one unresolved frame after exact
  Prepared quorum without ever issuing a commit permit;
- a preallocated frame barrier that snapshots exact live membership, persists a
  release only after the cohort is prepared, and emits cursor-bound commit
  permits before nodes may publish the frame at their show-clock boundary;
- an in-memory loopback transport for deterministic cluster fault injection;
- an exact rational show clock with play/pause/exact-seek/loop transport, no
  accumulated drift at fractional rates, and replicated transport state carried
  as ordinary document commands rather than a side channel;
- a bounded, allocation-free document-root handoff to playback, with a
  retirement ring so no trie is released on the frame boundary either;
- a frame-boundary driver that commits only Director-approved frames, holds
  rather than invents when approval has not arrived, adopts staged checkpoints
  at a boundary, and reports held/skipped/overflow/resync diagnostics;
- deterministic timecoded test frames rasterised into a caller-owned surface;
- an optional SDL3 output shell for the Director preview and node output
  windows, with a readback self-check that proves what reached the display is
  what was rasterised;
- a composable-role server bootstrap and fast unit tests.

The control plane now has both deterministic in-memory fault injection and
supervised inbound and outbound blocking-TCP lifecycles. Reconnects resume from
exact committed WAL boundaries using an immutable prefix while later appends
continue; the same transaction installs a bounded live queue, so frames
committed during replay cannot fall through a capture-to-live gap. Unknown
epochs/ranges and faulted nodes receive a staged checkpoint that is published
only at an explicit boundary. The bounded Director acceptor/session workers and
multi-node acknowledgement aggregation, durable release frontier,
commit-permit gate, heartbeats, node reconnect supervision, and graceful close
are integrated.
Production authoring must seal through the release-gated frame barrier. The raw
hub and durable-Director seal APIs remain available for lower-level tooling and
tests; bypassing the barrier leaves an unreleased suffix and is intentionally
not a publishable or restartable state.

## Where this is going

The first playable milestone (V0) is met: two node processes and a Director run
a show over TCP, each with an output window, agreeing on every frame through
play, pause and exact seek.

V0 also measured the problem that now sets the priority. The durable seal path
writes and syncs once per sealed frame, which caps the Director near 20-25
frames per second - so the system as built cannot reach the 60 fps the product
requires. The response is recorded as three decisions in
[`implementation-plan-cpp.md`](implementation-plan-cpp.md) §0:

1. **The recovery model is restart and re-cue.** A show does not resume an exact
   frame after a Director failure, and node failure is covered by hot-spare
   promotion. The durable log becomes a project artifact and an audit trail, and
   the unresolved-cohort recovery orchestration is cancelled rather than
   deferred. The implemented storage engine stays, correct and frozen.
2. **Durability cadence is not frame cadence.** The in-memory seal, release and
   permit path - the thing that makes two nodes agree - is unchanged. Durability
   becomes edit-shaped, and the transport becomes an anchor keyed to the
   control-frame ordinal so a playing show makes no document edits at all.
3. **The first RHI is SDL_GPU**, behind an explicit seam; the Spartan fork moves
   to the 3D phase.

The gates after that are frame-rate headroom, a GPU output path, feed mapping
and warp/blend, then media and the operator surface. Feed mapping comes before
media deliberately: it is the untouched risk and the actual differentiator,
while FFmpeg decode is well-trodden ground.

## Build

Windows builds require Git, CMake 3.28+, Ninja, and a Visual Studio 2022
developer shell. Linux builds require Git, CMake 3.28+, Clang 18+, and Ninja.
Both use vcpkg manifest mode to install the pinned FlatBuffers dependency.
The same manifest also installs pinned standalone Asio for the network module.

Set `VCPKG_ROOT` to a vcpkg checkout before configuring. For a fully pinned
tool and ports tree, check out the same baseline used by `vcpkg.json`:

```powershell
git clone https://github.com/microsoft/vcpkg.git "$env:USERPROFILE\vcpkg"
git -C "$env:USERPROFILE\vcpkg" checkout 3af1d1e60af2b2abf55760538cd607829029b07a
$env:VCPKG_ROOT = "$env:USERPROFILE\vcpkg"
& "$env:VCPKG_ROOT\bootstrap-vcpkg.bat" -disableMetrics
```

Then configure from the Visual Studio developer shell. Use `--fresh` when
migrating an existing build tree that was configured before vcpkg integration:

```powershell
cmake --preset windows-msvc-debug --fresh
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

The equivalent Linux bootstrap and build are:

```bash
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
git -C "$HOME/vcpkg" checkout 3af1d1e60af2b2abf55760538cd607829029b07a
export VCPKG_ROOT="$HOME/vcpkg"
"$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
cmake --preset linux-clang-debug --fresh
cmake --build --preset linux-clang-debug
ctest --preset linux-clang-debug
```

FlatBuffers and standalone Asio are resolved automatically from the locked
manifest and are not expected to be installed globally. The net target includes
the frozen session codec, deadline- and admission-bounded blocking framed
stream, reconnect/checkpoint state machines, bounded Director session hub,
fixed-worker listener, outbound node supervisor, heartbeat lifecycle, and
acknowledgement aggregator. The Director constructs
the hub before reporting readiness. Its TCP listener is opt-in and reports
`director-listener=disabled` unless an explicit endpoint and identity are
provided.

## Run

Node-only roles never create or open the Director WAL:

```powershell
out/build/windows-msvc-debug/apps/server/media-server.exe --role=output
```

To supervise a node's outbound session, provide the complete connector identity
quartet. The endpoint must be a numeric unicast address with a nonzero port; DNS,
unspecified, multicast, and broadcast addresses are rejected:

```powershell
out/build/windows-msvc-debug/apps/server/media-server.exe `
    --role=output `
    --director-connect=127.0.0.1:45000 `
    --show-id=00000000000000010000000000000002 `
    --node-id=00000000000000050000000000000006 `
    --node-incarnation=00000000000000070000000000000008
```

Without `--playback` the connector deliberately does not activate a checkpoint
or commit prepared frames: those calls belong to the show-clock boundary owner,
and startup reports `boundary-driver=unwired`. See [Playback](#playback) below
for wiring the boundary driver.

The Director owns a single durable WAL and completes validation/replay before
reporting readiness:

```powershell
out/build/windows-msvc-debug/apps/server/media-server.exe `
    --role=director,output `
    --wal=project.wal
```

The Director stores durable release decisions in `project.wal.release`. A new
sidecar is created only for an empty WAL. A nonempty WAL without a matching
ledger, or a WAL with frames beyond its durable release frontier, is rejected at
startup until an explicit recovery or migration workflow resolves it.

To admit node sessions, provide a numeric bind endpoint plus stable, nonzero
128-bit show and Director-epoch IDs as 32 hexadecimal digits:

```powershell
out/build/windows-msvc-debug/apps/server/media-server.exe `
    --role=director,output `
    --wal=project.wal `
    --director-listen=127.0.0.1:45000 `
    --show-id=00000000000000010000000000000002 `
    --director-epoch=00000000000000030000000000000004
```

### Playback

`--playback` wires the show clock. On the Director it seals exactly one control
frame per show frame, so the show position travels to every node as an ordinary
command inside a released, permit-gated frame. On a node it paces its own frame
boundary, publishes only frames the Director has approved, and presents the
position carried by the committed document:

```powershell
out/build/windows-msvc-debug/apps/server/media-server.exe `
    --role=director,output `
    --wal=project.wal `
    --director-listen=127.0.0.1:45000 `
    --show-id=00000000000000010000000000000002 `
    --director-epoch=00000000000000030000000000000004 `
    --playback --show-rate=50 --show-range=0:99999
```

```powershell
out/build/windows-msvc-debug/apps/server/media-server.exe `
    --role=output `
    --director-connect=127.0.0.1:45000 `
    --show-id=00000000000000010000000000000002 `
    --node-id=00000000000000050000000000000006 `
    --node-incarnation=00000000000000070000000000000008 `
    --playback --show-rate=50 --node-index=1
```

Each node reports its presented show frame, the control frame and revision it
came from, the transport state, held boundaries, skipped frames, handoff
overflows, and connection state. Two nodes on the same show print the same
show frame for the same control frame; a node that has not been approved for
the next frame holds the previous one and says so.

`--show-rate` takes an exact rational (`60000/1001`), `--show-range` bounds the
timeline, `--show-loop` wraps at the end, `--no-autoplay` starts paused, and
`--run-frames` exits cleanly after a fixed number of frames.

### Output windows

`--window` opens a real output window instead of rasterising to memory. It
requires a build with the SDL3 shell enabled, and asking for a window in a
build without it is refused rather than silently downgraded:

```powershell
cmake --preset windows-msvc-debug-gui
cmake --build --preset windows-msvc-debug-gui
```

Add `--window` to either role, with `--window-at=X,Y`, `--fullscreen`, or
`--borderless` for placement. The Director window is the operator preview and
accepts transport keys - space toggles play, left/right step one frame,
up/down jump one second, Home returns to the start, Escape closes. A node
output window is deliberately input-inert: what it shows is replicated state,
so keyboard transport there would be a lie.

Each frame is rasterised into a CPU surface and uploaded to a streaming
texture. That is the presentation fallback the plan calls for, and it leaves
the seam where Vulkan presentation replaces the upload without touching the
show clock or the frame-boundary driver. Vsync is explicitly off: the show
clock owns pacing, and blocking on the desktop refresh would beat against it.

The SDL3 dependency is an optional vcpkg manifest feature (`gui`), so a
headless node never links a video subsystem.

The current transport is not authenticated or encrypted. Bind it only to
loopback or a controlled production network until TLS and identity binding are
implemented. Changing the Director epoch intentionally forces nodes through
checkpoint recovery.

Both commands remain alive until interrupted. Add `--validate-startup` for a
one-shot open/recovery check suitable for deployment validation scripts.

Startup recovery is intentionally genesis-based in v1. Checkpoint compaction
will require an authenticated association between the checkpoint root and its
WAL generation. Director-local undo and historical indexes are explicitly
reset on restart until separately versioned authoring metadata is persisted.
The v1 release ledger is a crash-integrity and identity binding, not a
cryptographic authorization boundary.

See [`IMPLEMENTATION_STATUS.md`](IMPLEMENTATION_STATUS.md) for current scope and
the next engineering steps.
