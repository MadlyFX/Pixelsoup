# Cluster operations and supported envelope

This document defines what the current cluster control plane proves, how to
operate it safely, and what it deliberately does **not** claim about physical
display timing.

## Supported envelope

The release gate currently certifies this baseline:

- one Director and two render nodes connected over real TCP sessions;
- 120 control frames per second for ten minutes;
- at most one bounded document transaction per control frame, with the normal
  256-frame / 128 MiB Director replay window and edit-shaped WAL durability;
- a generated scene/feed workload, plus independent model/feed evaluation on
  each node;
- loss of one node while the other continues, then a new node incarnation
  replaying or adopting a checkpoint and converging within five seconds in the
  local Release test environment;
- 4K H.264 playback at its authored native rate on the measured development
  machine, as a separate renderer benchmark. This is not a two-node media
  throughput certification.

The executable admits up to 64 concurrent Director session workers, but 64
nodes are a capacity ceiling, not a certified performance envelope. More than
two output nodes, multiple simultaneous 4K/8K streams, high-bitrate image
sequences, shared-storage contention, non-local network latency, and vendor I/O
devices require a show-specific Release soak before use. Do not infer media or
GPU capacity from the control-plane rate test.

The node mailbox holds 16 published states and applies at most eight commits at
one local boundary. The Director retains 256 replay frames by default and
refuses to continue sealing when its bounded barrier backlog is exhausted.
These are safety limits, not permission to run continuously at the limit.

## Health and diagnostics

Output Monitor reports three separate facts:

- `CONTROL HEALTHY`, `DEGRADED`, or `FAULT` describes current session
  membership, catch-up, release lag, pacer lag, and barrier health.
- Node rows describe lifecycle/process state and content-store/runtime
  readiness.
- `PTP/GENLOCK/FRAME LOCK UNVERIFIED` describes physical timing. A green
  control state never changes this line.

Timeouts, reconnects, presenter failures, acknowledgements, live/catching-up
membership, release lag, pacer lag, and barrier rejects are also written as
JSON Lines beside the Director WAL:

```text
<director-wal>.cluster.jsonl
```

The stream is flushed at the normal status cadence and at clean shutdown. It
rotates at 32 MiB; one `<director-wal>.cluster.jsonl.previous` file is retained.
The log contains no lifecycle secret or asset source path. Copy the WAL,
release ledger, both diagnostic files, project, and relevant node logs before
restarting a failed show if post-show analysis is required.

Cumulative incident counters remain in the diagnostic record after recovery;
the current control health can return to healthy once membership and progress
recover.

## Automated failure and soak gates

The default Release suite covers session loss/timeout, node restart with a new
incarnation, late checkpoint adoption, bounded historical replay, slow/catching
up sessions, release barriers, WAL truncation/recovery, and release-ledger
recovery. The short 120 fps two-node rate case runs in the default suite.

Run the ten-minute gate explicitly:

```powershell
cmake --preset windows-msvc-release-gui --fresh `
  -DMEDIASERVER_ENABLE_SOAK_TESTS=ON
cmake --build --preset windows-msvc-release-gui
ctest --preset windows-msvc-release-gui -L soak --output-on-failure
```

Or run its executable directly for a different pre-show duration:

```powershell
out/build/windows-msvc-release-gui/engine/net/mediaserver_show_soak.exe `
  --seconds=1800 --rate=120 --report-seconds=30
```

A production acceptance run should use the actual project, media store,
network, GPUs, display modes, and intended node count. Pass criteria are no
skipped control positions, no unbounded retained/WAL growth, exact final node
convergence, no resync report, no presenter failure, and zero sustained release
or pacer lag. Archive the diagnostic logs with the result.

## Recovery procedure

1. Keep `STOP ALL` available; STOP is intentionally not blocked by asset or
   runtime readiness.
2. If one node fails, leave healthy nodes running while the lifecycle agent
   restarts that node. Output Monitor should show the session missing, then
   catching up, then live. Do not take Program until content and runtime
   adoption are ready and control health has recovered.
3. If the Director fails, stop outputs, preserve its WAL, `.release` ledger,
   diagnostic log, and project, then restart the Director against those same
   files. Confirm the recovered project and transport before starting nodes.
4. If recovery reports corruption, do not delete evidence or substitute an
   unrelated recent project. Start untitled/dirty or restore a known project
   copy, then re-stage content.
5. After any restart, verify every saved display binding and applied mode in
   Hardware Map. A control acknowledgement proves document adoption, not that
   a monitor, cable, LED processor, or LED cabinet is physically present.

Default Director heartbeats are sent every five seconds and a missing response
times out after ten seconds. Local restart convergence is tested within five
seconds after a new connection, but network outage detection plus service
restart is not a five-second end-to-end SLA.

## Network, service, and secret safety

- Put Director sessions and lifecycle listeners on a trusted, isolated control
  VLAN. Do not expose either listener to the public Internet.
- Allow inbound Director-session TCP only from configured render-node
  addresses. Allow each lifecycle control port only from the Director/operator
  address. Block lateral and Internet ingress by default.
- Run each lifecycle listener as a machine service with automatic restart and
  a dedicated, non-administrator service identity. Grant that identity only
  read/execute access to the application, read access to its secret, and the
  required asset-store/output-device access.
- Store the 32--256 byte lifecycle secret outside the project and repository.
  Apply an ACL limited to the Director and lifecycle service identities. It is
  HMAC authentication/integrity on a trusted LAN, not transport encryption.
- Rotate the secret during a maintenance window: STOP outputs, stop all
  lifecycle listeners, atomically replace the secret on every node and the
  Director, restart listeners, verify authenticated status for every node,
  then restart render children. Mixed secrets fail authentication; never fall
  back to an empty or project-supplied secret.
- Configure service recovery to restart the lifecycle listener, not an
  unmanaged duplicate render child. The listener's job/process-group ownership
  tears down the supervised child with it.

## Timing model and future synchronization boundary

Today the system provides deterministic control-frame ordering, durable
release, validated node commit acknowledgements, and software-paced local
presentation. It does not measure scan-out phase and does not implement PTP,
genlock, or GPU/display frame lock. Therefore it cannot claim that separate
physical displays scan the same image at the same instant.

A future physical-timing implementation must keep explicit boundaries:

1. A platform time provider reports the disciplined clock, offset, uncertainty,
   and last lock time. PTP/IEEE 1588 configuration belongs to the operating
   system or a supported timing interface, not the document protocol.
2. The presenter schedules against that clock and reports actual presentation
   timestamps/late frames.
3. A vendor-specific adapter/output provider reports genlock/frame-lock state
   for the exact GPU and connector. Unsupported hardware remains `UNVERIFIED`,
   never assumed locked.
4. Hardware validation measures LED processor/camera scan phase. Only a
   supported-device integration plus measured lock can turn the Output Monitor
   timing line green.

Control health, content readiness, display presence, and physical sync remain
four independent gates.
