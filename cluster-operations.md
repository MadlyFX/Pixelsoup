# Cluster operations and supported envelope

This document defines what the current cluster control plane proves, how to
operate it safely, and what it deliberately does **not** claim about physical
display timing.

## Supported envelope

The release gate certifies this baseline:

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
one local boundary. These are safety limits, not permission to run
continuously at the limit.

## Lockstep and cluster lag

Outputs advance together, at the pace of the slowest render node. A node
presents a control frame only once the Director has released it, and the
Director releases a frame only when every live node has received it and no
more than two released frames are still waiting for the slowest node to
present them. A node that falls behind therefore holds every other output
within two frames of it rather than letting the wall tear. When the slowest
node recovers, the cohort catches up together: once every released frame has
been presented, everything the nodes have already received is released at
once and lands on one boundary, rather than one frame per boundary.

The Director does not wait for the cluster. It keeps sealing control frames
while the nodes are behind, so the operator's edits always apply in the
editor at once and reach the outputs when the cluster catches up. The cluster
may fall up to 1,024 control frames behind (about seventeen seconds at 60
fps). At 960 frames behind, the node holding everyone back is taken out of
lockstep: it stays connected and catches up on its own, the other outputs
continue without it, and it rejoins at a later cut. The Director console
prints `Director took node N out of lockstep`, the status bar shows `NODE N
FELL BEHIND`, and the Output Monitor incident count includes it.

An operator's transport change - stop, pause, seek or play - jumps the queue.
The Director sends it to every node ahead of the frames they are still owed,
and each node presents it at its next boundary: a stop or pause shows its own
frame immediately, however far behind the cluster is, and a play holds on its
first frame until the frames reach it. The same change is also in the sealed
frame, which every node still reaches in order and which takes over once it
arrives. Anchors the show authors itself - a cue's hold or loop, the end of
the range - are not sent ahead, because outputs play the frames before them
through. A priority message waits only behind a message already being written
on that connection, so a very large frame in flight can still delay it.

If the Director ever cannot seal at all, because the barrier has run out of
records, that is a fault rather than a slow node. A full barrier that lasts
more than a second is reported: the Director console prints `Director
stalled:` with the oldest frame and its prepared, committed and lost member
counts, the editor status bar shows `EDITS PAUSED`, and Output Monitor shows
`FAULT` and an **Edits paused** chip. Clearing prints the recovery and shows
`EDITS RESUMED`.

## Session transport round-trip latency

`TcpFramedStream` drives a non-blocking socket from a synchronous API, so a
transfer that finds the socket not ready has to wait somewhere. That wait used
to be `std::this_thread::sleep_for(1ms)`. At the default Windows timer
resolution a 1 ms sleep really takes about 15.6 ms, so every parked transfer
cost a whole scheduler quantum, and an exchange cost one quantum per park
whatever the payload was. The transfer loops now wait on socket readiness
(`WSAPoll` on Windows, `poll` elsewhere) and wake as soon as the socket is
usable.

Measured on the development machine by
`mediaserver_net_framed_stream_benchmark` (Release, loopback, a framed stream
at both ends), before and after that change:

| exchange | p50 before | p50 after | p95 before | p95 after |
| --- | --- | --- | --- | --- |
| one round trip, 256 B | 31.52 ms | 0.03 ms | 32.12 ms | 0.05 ms |
| three round trips, 3 KB payload | 94.45 ms | 0.09 ms | 96.03 ms | 0.18 ms |
| three round trips, 256 KB payload | 94.08 ms | 0.14 ms | 95.99 ms | 0.35 ms |

The two payload sizes costing the same before the change is the diagnosis: the
cost tracked the number of blocking exchanges, not the bytes moved. The same
run times `sleep_for(1ms)` directly and reports 15.6-15.7 ms per call.

Three things this deliberately does not do:

- **Nagle is not the cause.** `TCP_NODELAY` was measured earlier and changed
  nothing, because the delay was a local wait rather than a queued segment. Do
  not re-add it on latency grounds. That measurement, and the benchmark's
  back-to-back rows (two and three small records sent before any reply, the
  frame stream's own shape), are loopback: acknowledgements there are
  immediate, so they cannot show a Nagle delay and do not rule one out
  between two machines. Settling it needs the same rows run across a real
  link.
- **The process timer resolution is left alone.** `timeBeginPeriod` would only
  shrink the quantum, applies to the whole process, and costs power everywhere
  else in it.
- **The readiness wait stays bounded**, at 10 ms per call. `close()` and
  `NodeResumeSession::interrupt_transport()` unwind an in-flight transfer by
  shutting the socket down and waiting for it to return, so that wait must
  never be indefinite. The bound is below the quantum the sleeping retry used
  to cost, so cancellation latency does not regress either.

The same sleeping retry sat in the lifecycle service's accept loop, so every
connection it accepted cost a quantum before the handshake even started. That
loop now waits on the listening socket. Its own suite,
`mediaserver.net_cluster_lifecycle` -- 23 tests that open roughly 60
connections -- went from a 1.65 s to a 0.72 s median over five runs each,
which is the same ~15.6 ms per connection seen from the other side.

The client half of connection setup was already correct and was left alone:
`connect_poll_interval` bounds an `io_context::run_for`, which is a real
reactor wait that returns the moment the connect handler fires, not a sleep.

`mediaserver.net` holds both halves in place: `round trips avoid the scheduler
quantum` fails if a sleeping retry returns to the transfer loops, and `close
wakes blocked read` bounds the unwind of a parked read.

## Health and diagnostics

Output Monitor reports three separate facts:

- `CONTROL HEALTHY`, `DEGRADED`, or `FAULT` describes current session
  membership, catch-up, release lag, pacer lag, and barrier health. A
  Director that has stopped sealing frames is always `FAULT`, with or without
  configured nodes, and the diagnostic record carries `control_stalled`.
- Node rows describe lifecycle/process state and content-store/runtime
  readiness.
- The timing line describes physical output synchronization: the authored
  policy, the exact readiness verdict, and how many group outputs are locked
  and joined to the present barrier. A green control state never changes it.
  See "Output synchronization" below for every verdict and what to check.

Timeouts, reconnects, presenter failures, acknowledgements, live/catching-up
membership, release lag, pacer lag, and barrier rejects are also written as
JSON Lines beside the Director WAL:

```text
<director-wal>.cluster.jsonl
```

The stream is flushed at the normal status cadence and at clean shutdown. It
rotates at 32 MiB; one `<director-wal>.cluster.jsonl.previous` file is retained.
The console status lines of the Director and of each node also report how
late that process's frame loop ran after the boundaries it waited for, over
the status interval: `wake-max-us`, `wake-mean-us`, and `wake-late=N/M` (N of
M boundaries over 2 ms). Loops wait on a high-resolution OS timer, so on an
unloaded machine the maximum stays near a millisecond. A sustained higher
figure means the loop's own work, or competing load on the machine, is
delaying frame sealing or rendering.

A node's frame pacer runs for the life of the process, whatever the transport
is doing and wherever the playhead is, so `boundaries=` on its status line
climbs by one status interval per line even while the show is paused, stopped
or disconnected. A node whose `boundaries=` stops climbing, or whose status
lines stop, has a stalled frame loop: it commits and acknowledges nothing, and
the Director's `release-lag` for it grows until it is demoted. The pacer takes
only its rate from the show; `--show-range` and `--show-loop` do not affect
it. If a node ever finds its pacer stopped it restarts it and prints `node
frame pacer stopped at its frame N; restarted it` once.

An external reference that is not supplying frames does not stall a node
either; it stops the picture and nothing else. Under `EXTERNAL GENLOCK` a node
draws on the frames its reference counter counts. While that counter is
absent, lost, or answering with a count that has not moved for four frame
periods (a quarter of a second at least), the node repeats its last complete
picture on every output and runs everything else on its own clock: it commits
and acknowledges frames, adopts checkpoints, follows the document and reports.
On its status line `boundaries=` and `control-frame=` keep climbing,
`presented=` stops, `reference=` names the state followed by `/holding`, and
each boundary is counted in `missed=` and `policy-held=`. It prints `node N is
holding its picture: video reference lost` when the hold starts, naming the
state it found, and `node N is drawing again: ...` when it ends, saying
whether the reference came back or the sync policy stopped asking for one.
Output Monitor shows the node as `SHOW CLOCK REFERENCE LOST` or `SHOW CLOCK
REFERENCE UNAVAILABLE`.
Because the node still follows the document, changing the sync mode in
Hardware Map, or redeploying its outputs, reaches it and ends the hold without
a restart.

Each node's status line also samples memory at the same interval, in MiB:
`gpu-local-mib=U/B` is the video memory the process uses (U) against the
budget the OS allows it (B); `gpu-system-mib` is system memory held
on the GPU's behalf, including resources demoted out of video memory once U
reaches B; `gpu-committed-mib` is what the renderer's own textures and geometry
buffers account for; and `private-mib` is the process's committed private
memory. The GPU fields are absent on an output with no D3D12 device. A leak
shows as `gpu-system-mib` and `private-mib` climbing while `gpu-local-mib`
sits at its budget, usually with `gpu-observed-us` stepping up as the GPU
starts reading demoted resources across the bus. If `gpu-committed-mib` stays
flat while the other figures climb, the growth is outside the renderer's
accounting: decoder surfaces, upload heaps or driver allocations.

On the Director those figures are the sealing thread's alone. It seals control
frames on a thread of its own and takes turns with the thread that evaluates,
draws the editor and presents (see "Director threads" in
`docs/architecture.md`), so the Director line carries four more
figures for the same interval: `seal-wait-max-us` and `seal-wait-mean-us`, how
long sealing waited for the other thread to finish its document work;
`seal-waited=N/M`, how many of the sealing thread's M wakes waited more than
2 ms; and `show-hold-max-us`, the longest the other thread kept the document to
itself. A slow editor frame, a slow present or a project being opened does not
appear in any of them. `show-hold-max-us` rising with a project's size is that
project's evaluation and authoring cost, and is the figure to watch when a
large show makes control frames late.

The log contains no lifecycle secret or asset source path. Copy the WAL,
release ledger, both diagnostic files, project, and relevant node logs before
restarting a failed show if post-show analysis is required.

### Process logs

Every media-server process tees what it prints to stdout and stderr - status
intervals, asset adoption, session and lockstep events, errors - into a
timestamped, rotating file, so a fullscreen render node with no console still
leaves a record:

| Process | File |
| --- | --- |
| Director / editor | `director.log` |
| Render node N | `node-N.log` |
| Lifecycle agent for node N | `agent-N.log` |
| Render-machine window | `render-machine.log` |

They live in `%LOCALAPPDATA%\MediaServer\logs` (`--log-dir=PATH` moves them;
a lifecycle agent passes its own folder to its render process). Each start
moves the previous run to `NAME.1.log`; a file rotates at 8 MiB and three
older files are kept. If another live process already writes a name, the new
one logs to `NAME-<pid>.log` instead. The first line of each run records the
command line, with any `--lifecycle-password` value masked.

The frame and control paths only write to a pipe. A reader thread copies the
bytes into a bounded 1 MiB queue and one writer thread owns the file; when the
queue is full the bytes are dropped and counted, and the writer records
`[log] dropped N bytes` rather than stalling a frame. Console forwarding has
its own bounded queue, so a console that stops reading no longer stalls the
process either.

**Reading a node's log from the editor.** Expand a render server's row in
Output Monitor and press **Show log**. The editor asks that machine's
lifecycle agent - over the same authenticated control port as Start and Stop -
for the last 400 lines of `node-N.log`, refreshes it every three seconds while
it stays open, and stops asking when it is closed. Because the agent reads the
file from disk, this works while the render process is hung, out of lockstep,
reconnecting, stopped, or after it crashed: when the current file is a fresh
run, the previous run's last lines are shown above a **Current run** divider.
A reply is bounded to 1,000 lines, 400 bytes per line and 96 KiB in all;
control characters and invalid UTF-8 are replaced before they cross the wire.
A node started with `run-output-node.ps1 -Direct` has no agent, so its log is
read on that machine only.

Cumulative incident counters remain in the diagnostic record after recovery;
the current control health can return to healthy once membership and progress
recover.

## Automated failure and soak gates

The default Release suite covers session loss/timeout, node restart with a new
incarnation, late checkpoint adoption, bounded historical replay, slow/catching
up sessions, a rejoining node going live within one second of show time after
backlogs of 20 to 3,000 frames, release barriers, WAL truncation/recovery, and release-ledger
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

Closing and reopening the editor leaves remote render machines supervised by
their own agents. Running nodes retry automatically against their configured
Director endpoint and show ID; Output Monitor shows CONNECTING, CATCHING UP,
then LIVE separately from machine/process status. Expanded rows show the
acknowledged and target control frames. A node that was away for more than a
few frames is handed the Director's current checkpoint rather than the frames
it missed, and it rejoins the release cohort as soon as it has prepared to
within two control frames of the newest seal, so CATCHING UP lasts a few
frames however long it was gone. A node that leaves never holds back the
nodes that stay: a frame it had not yet prepared is released once every
remaining node has prepared it, or at once when none remain. A node taken out
of lockstep for lagging also shows CATCHING UP until it rejoins. A node that stays in CATCHING UP is not
receiving or acknowledging frames; check its link rather than waiting. Replacement windows start black, and
disconnected outputs retain their last complete image. A restart tolerates
up to fifteen seconds of overlap with an old connection's lease; a persistent
duplicate becomes a process fault that can be restarted from Output Monitor.

## Bringing up a render machine

There are two supported ways to run a render machine's lifecycle agent. Both
run the same agent with the same render-process arguments, so Output Monitor
cannot tell them apart.

**The render-machine window** is for a machine an operator signs in to.
Launch `media-server.exe` with no arguments (a double-click, a shortcut, or
`media-server.exe --render-machine`):

1. The first launch opens **SET UP THIS RENDER MACHINE**. The control machine
   is discovered automatically. Leave the address, Node index and Show ID
   overrides empty, and enter the render node password from the editor's
   **File > Preferences** - or nothing if none is set there. Advanced: a
   control port other than 47840 + assigned node index, and an explicit show
   ID to filter several editors. The discovered editor supplies the show ID
   and saves a unique node index for this machine's saved identity. Simultaneous
   enrollments are serialized; conflicting manual indices are refused.
   The table is stored beside the Director WAL as `<wal>.nodes-<show-id>.bin`.
   Preserve it and the node's Preferences to retain assignments across restarts.
   The render process discovers the editor's current address and port on each
   start or restart and waits when none is found. Several editors for the same
   show require closing the extra editors or setting a manual address and port.
   A manual address also supports networks where discovery cannot reach the
   editor. Clear previously saved address, index and show overrides to enable
   their automatic values. A missing or unavailable editor leaves the window
   waiting without starting a render process with placeholder identity.
   **OPEN EDITOR**, on the form and in the status view, starts the operator
   editor on this machine as its Director instead (port 45060 on every
   interface, the standard show, unless Preferences stores a Director
   identity). From the form the window then closes; from the status view the
   render machine keeps running beside the editor.
2. **SAVE AND START** stores the settings in this machine's per-user
   Preferences (never a project, never sent anywhere), starts the agent on
   every interface after identity assignment, and starts the render process.
3. Later launches go straight to the status view with those settings. A
   stored value that can no longer be used - a damaged settings file, an
   address that does not parse - reopens the form saying which.
4. The status view shows the render process with local START, STOP and
   RESTART (Output Monitor's controls work alongside them), the control port,
   whether a password is set, accepted and refused control requests - a
   refused request had the wrong password - and this machine's addresses for
   entering it in Hardware Map by hand.
5. **Start this render machine when I sign in to Windows** adds a per-user
   Run entry for `media-server.exe --render-machine`; clearing it removes the
   entry. Nothing else is registered.
6. Closing the window asks first: it stops the agent and the render process,
   and Output Monitor cannot start the machine again until it is reopened.

Launched from Explorer, the window closes the console it was given and starts
the render process without one; started from a terminal it keeps the console,
and the render process logs to it as under the scripts. `media-server.exe
--render-machine --validate-startup` prints what a launch would start - node
index, Director, control port, password set or not - without opening a window,
a port, or a process.

**Windows Firewall.** The window looks for two inbound allow rules, named for
their ports: `Media Server Lifecycle (TCP <control port>)` and `Media Server
Discovery (UDP 47839)`. While either is missing it offers **ALLOW THROUGH
FIREWALL**, which runs one elevated `netsh` command - Windows asks for an
administrator first - that deletes any rule with exactly those names and adds
them again, so pressing it twice leaves one copy. It changes nothing else. The
equivalent by hand, from an elevated prompt:

```powershell
netsh advfirewall firewall add rule name="Media Server Lifecycle (TCP 47840)" `
  dir=in action=allow protocol=TCP localport=47840
netsh advfirewall firewall add rule name="Media Server Discovery (UDP 47839)" `
  dir=in action=allow protocol=UDP localport=47839
```

The rules allow any remote address. Where the control VLAN is shared, narrow
them with `remoteip=` to the control machine, as the next section recommends.
A rule found is not proof of reachability: Windows' own "allow access" prompt,
answered Cancel when the agent first listened, can leave a block rule for
`media-server.exe` that wins over any allow rule. If the window reports the
rules present but the editor still cannot find or reach the machine, look for
such a rule under Windows Defender Firewall > Advanced settings > Inbound
Rules.

**A service** is for a machine nobody signs in to, or one that must come up
before anyone does: run `media-server.exe --lifecycle-listen=... ` with the
options in the README's lifecycle section (or `scripts/run-output-node.ps1`)
under a service manager. The command-line agent reads no stored settings, so
a service's configuration is exactly its command line.

### Remote control for development builds

When render machines run `media-server.exe` from a shared build folder, a new
build cannot link while any machine still has that file open, and the
lifecycle agent is itself `media-server.exe`. Output Monitor's Stop ends only
the render process, so the agent keeps the file locked. For development
machines, `scripts/node-remote-control.py` owns `run-output-node.ps1` as a
child process and lets the control machine stop and start the whole tree - agent
and render process - and read the node's logs, without anyone at the render
machine. It uses only the Python standard library and runs on Windows.

On the render machine, from the repository's `scripts` folder; everything after
`--` goes to `run-output-node.ps1` unchanged:

```powershell
python node-remote-control.py --token <secret> --autostart -- `
  -DirectorHost 192.168.200.154 -Build release
```

It listens on TCP 47850 (`--port`, `--listen`) and refuses to start without a
token unless `--no-token` is passed deliberately; requests carry it in an
`X-Token` header or a `token=` query parameter. The node script opens in its
own console (`--hidden` suppresses it). Allow the port from an elevated
prompt, and narrow it with `remoteip=` on a shared VLAN:

```powershell
netsh advfirewall firewall add rule name="Media Server Remote Control" `
  dir=in action=allow protocol=TCP localport=47850
```

From the control machine:

```powershell
$h = @{ 'X-Token' = '<secret>' }
Invoke-RestMethod http://<render-ip>:47850/status -Headers $h
Invoke-RestMethod http://<render-ip>:47850/stop -Headers $h -Method Post
# ...build...
Invoke-RestMethod http://<render-ip>:47850/start -Headers $h -Method Post
Invoke-RestMethod "http://<render-ip>:47850/log?lines=200" -Headers $h
```

| Request | Effect |
| --- | --- |
| `GET /status` | Whether the node script is running, its uptime and command, and every `media-server.exe` on the machine. |
| `POST /start` | Starts the node script unless it is running. Refuses while a `media-server.exe` it did not start is running (a node started by hand or by an earlier run of the script); `?force=1` overrides. |
| `POST /stop` | Ends the node script's whole process tree with `taskkill /T /F`, then waits up to ten seconds for every `media-server.exe` to exit so a build started next can link. `?all=1` also ends processes it did not start. |
| `POST /restart` | Stop, then start. |
| `GET /log` | Plain text. `file=node` (default), `agent` or `control`; `previous=1` for the previous run's `.1.log`; `lines=N` (default 400, at most 20,000, `0` for the whole file up to its last 16 MiB); `grep=TEXT` keeps matching lines. |

Logs are read from the node script's `-LogDir`, otherwise
`%LOCALAPPDATA%\MediaServer\logs`, using its `-NodeIndex` (default 0);
`--log-dir` overrides. The script records its own requests in
`remote-control-<index>.log` beside them. Processes holding under 1 MB are
ignored by the start guard and the stop wait: Windows can leave an unreaped
`media-server.exe` of a few kilobytes behind that no longer holds the file.

Closing the script leaves a running node running, and a restarted script does
not track it. `--autostart` then reports the node already running instead of
starting a second one; `POST /stop?all=1` followed by `POST /start` hands the
node to the new script. Anyone who can reach the port and knows the
token can stop the wall: keep it on the control VLAN and off show machines
that do not need remote rebuilds.

## Network, service, and secret safety

- Put Director sessions and lifecycle listeners on a trusted, isolated control
  VLAN. Do not expose either listener to the public Internet.
- Allow inbound Director-session TCP only from configured render-node
  addresses. Allow each lifecycle control port only from the Director/operator
  address. Block lateral and Internet ingress by default.
- Discovery (UDP 47839 inbound on render machines and Directors) lists nodes under
  DETECTED ON NETWORK in Hardware Map. It discloses each machine's host name,
  node index, control port, and whether a password is set - nothing that
  grants control - and its queries are padded so an agent cannot amplify
  traffic. On a network where even that is too much, start agents with
  `--lifecycle-discovery-port=0` (`-DiscoveryPort 0`) or leave the port closed
  and enter machines by hand. Directors also advertise their show identity and
  TCP listener so automatically configured nodes can find them on the local
  IPv4 network. Nodes still validate the show session before rendering. When
  discovery is disabled, configure the Director address explicitly on nodes.
- Run each lifecycle listener as a machine service with automatic restart and
  a dedicated, non-administrator service identity. Grant that identity only
  read/execute access to the application, read access to its secret, read
  access to the media paths the show references, and the required output-device
  access.
- Set a render node password for any show network that is not fully
  isolated: without one an agent accepts any Media Server on the network, and
  Output Monitor shows `No password` to say so. The editor keeps it in
  Preferences as a PBKDF2-derived key; on a render machine prefer
  `--lifecycle-secret-file` (a file holding the password, ACL'd to the
  service identity) over `--lifecycle-password` on a service command line.
  It is HMAC authentication/integrity on a trusted LAN, not transport
  encryption.
- Change the password during a maintenance window: update every render
  machine's agent (CHANGE SETTINGS in its window, which restarts the agent;
  or restart a service's listener with the new password), then save the new
  password in the editor's Preferences - it applies immediately and every
  node is asked again.
  A node still on the old password reads `password rejected` until it is
  updated.
- Configure service recovery to restart the lifecycle listener, not an
  unmanaged duplicate render child. The listener's job/process-group ownership
  tears down the supervised child with it.

## Output synchronization

### What each layer prevents

| Layer | What it prevents | Current state |
| --- | --- | --- |
| Show-frame synchronization | Nodes rendering different timeline frames | Implemented (sealed frames, durable release, commit permits, lockstep release within two frames) |
| PTP / clock correlation | Clock and timestamp drift | Not implemented; owned by the operating system or a timing interface, not the document protocol |
| Genlock (external reference) | Scan-out timing drifting between machines | Modelled, authored, reported, and gated; NVIDIA Quadro Sync read through NVAPI in builds with `MEDIASERVER_ENABLE_NVAPI`, no other vendor |
| Frame lock | GPU output heads drifting relative to each other | Modelled, authored, reported, and gated; same NVIDIA provider, no other vendor |
| Swap group / present barrier | Genlocked outputs showing different frame numbers | Seam implemented (`rhi::PresentBarrierBackend`); NVAPI backend for the native D3D12 swapchain in builds with `MEDIASERVER_ENABLE_NVAPI`; SDL_GPU swapchains and other vendors report UNSUPPORTED |
| LED processor sync | Processor re-timing or tearing after the GPU | Not monitored; needs a device integration and measured validation |

Genlock and frame lock align raster boundaries. A present barrier is what makes
every joined output advance from image N to image N+1 on the same aligned
boundary. Both are required: without swap lock two perfectly genlocked outputs
can sit one frame apart indefinitely, and without genlock coordinated swaps
still scan out at different phases.

### Authoring the policy

Hardware Map, `SYNC POLICY` in the left rail:

- **MODE** - `UNSYNCED` makes no physical timing claim. `FRAMELOCK` requires
  every group output locked and every swapchain joined to the barrier.
  `EXTERNAL GENLOCK` additionally requires a house reference that is present
  *and actually in use* by the sync device.
- **Width / Height / Rate num / Rate den** - the exact timing every output in
  every group must present. The rate is rational on purpose: 60/1 and
  60000/1001 are different values and mixing them is precisely the
  configuration that drifts.
- **Swap group / Barrier** - the vendor identifiers. They are authored rather
  than derived so a show can share a barrier with equipment this application
  does not own.
- **ON FAILURE** - `STRICT` refuses to promote Program while any required
  condition is unproven. `DEGRADED` promotes but keeps the surface in a
  permanent warning state. Neither ever shows a generic green online state,
  and neither changes what a render node does without its reference under
  `EXTERNAL GENLOCK`: it holds its picture (see Health and diagnostics).

An output that cannot join a present barrier at all, because its adapter
offers none, is not stopped by `FRAMELOCK`: it goes on drawing as an ordinary
output and is reported as `SWAP BARRIER UNSUPPORTED`, which `STRICT` refuses
Program on. An output that could join and failed to, or joined and was
dropped, does hold its last picture, because showing it out of step beside
locked outputs is the fault the barrier exists to prevent.
- **LATE FRAME** - `HOLD GROUP` repeats the previous complete frame on every
  output a render process owns when one of them has no complete frame.
  `ADVANCE INDEPENDENTLY` lets each output advance on its own. Hold is the
  safe default for an LED wall: one repeated frame everywhere is far less
  visible than one segment advancing while its neighbour stands still.

The policy is an ordinary replicated document node, so it saves with the
project, replicates to nodes at a frame boundary, and undoes like any edit.

### How groups are derived

Groups come from the cabling already drawn in Hardware Map: the set of
render-server video outputs that reach a common downstream LED processor or
wall. Two outputs of one machine driving unrelated walls are two groups; two
machines feeding one processor are one group. The derivation is a pure
function of the authored map and policy, so every machine agrees exactly.

Validation reports, and a physical policy blocks on:

- `DISPLAY TIMING MISMATCH` - group members disagree about raster or rational
  refresh, two members name the same display target, or the group timing is
  not what the policy requires.
- `SYNC GROUP OUTPUTS UNBOUND` - a cabled video port has no display binding to
  lock, or a binding is not cabled to anything downstream.

Re-check both after any display configuration change. Discovery never rewrites
bindings, cables, or project files on its own.

### What is measured, and by whom

The two halves are deliberately separate because different processes can see
them:

- The machine-local **lifecycle agent** reports the sync device (detected /
  absent / unsupported), the reference signal (absent / present / in use /
  unstable), and each display target's lock state and exact timing. It answers
  over its own authenticated message kind and remains useful while the render
  child is stopped.
- The **render process** reports present-barrier membership, the joined output
  count against the expected one, and its presented, missed, late, held,
  repeated, and loss-of-sync counters. These travel on the already-validated
  node-session heartbeat. The same report carries its failed frames whatever
  the sync policy: how many, whether the latest attempt failed, the step and
  output map that failed, and the GPU's refusal (upload space full, or a
  buffer rewritten after an earlier draw in the same frame used it). A change
  of cause or of failing/drawing is sent at once; the count rides the status
  cadence. Output Monitor shows it as `FRAMES FAILING`.
- The render process also reports the **show clock reference**: the frame
  counter its picture is paced from under `EXTERNAL GENLOCK` (`reference=` on
  its status line). On NVIDIA it is the Quadro Sync swap-group frame counter.
  `locked` means that counter is counting. It does not say the board is
  following house sync - that is the lifecycle agent's reference reading
  above, and `EXTERNAL GENLOCK` needs both. A counter that answers but has
  stopped reads `lost`; an adapter with no swap groups, which includes every
  GeForce, reads `unsupported`. A counter that is reset, or jumps further than
  the elapsed time allows, is taken as a new starting point and never as that
  many frames; the Director reads its own counter by the same rule.

Anything unmeasured stays Unknown or Unsupported. There is no default that
means "probably locked", and an unanswered query is never treated as a lock.

### Reading Output Monitor

The timing line reports the policy, the exact verdict, and the proof:

```text
SYNC FRAMELOCK | SWAP BARRIER INCOMPLETE | LOCKED 2/2 | BARRIER 0/2 | GROUPS 1
```

Per-output rows add the same verdict for one supervised render process plus
its frame accounting. The verdicts, in the order the gate checks them:

| Verdict | Meaning | First thing to check |
| --- | --- | --- |
| `SYNC POLICY UNSYNCED` | No physical claim is being made | Nothing; this is a valid configuration |
| `DISPLAY TIMING MISMATCH` | Authored group timing disagrees | Output bindings and the policy raster/rate |
| `SYNC GROUP OUTPUTS UNBOUND` | A cabled output has no display binding | Hardware Map bindings and cables |
| `SYNC UNVERIFIED` | Nothing has reported, or this build cannot query | Lifecycle agent reachability; vendor integration |
| `SYNC DEVICE MISSING` | The device answered and there is no sync board | Physical card and driver |
| `REFERENCE LOST` | No usable house reference at the sync device | Reference cable, generator, termination |
| `REFERENCE NOT IN USE` | Reference present but not disciplined to | Sync device configuration |
| `DIRECTOR CLOCK UNREFERENCED` | The Director is sealing frames on its own clock, not the reference counter | The Director's own output and sync board |
| `SHOW CLOCK REFERENCE LOST` | A node's reference frame counter stopped or failed; that node is holding its picture | That node's sync board and reference cabling |
| `SHOW CLOCK REFERENCE UNAVAILABLE` | A node has no reference frame counter to read; that node is holding its picture | Whether that machine has sync hardware; otherwise choose another sync mode |
| `OUTPUTS UNLOCKED` | Group outputs are not all locked | Per-display lock state and timing |
| `SWAP BARRIER UNSUPPORTED` | A render machine's adapter or build offers no present barrier (every GeForce; any non-NVIDIA adapter). Its outputs keep drawing, unsynchronised | Use a card with swap groups for swap lock, or set the mode to `UNSYNCED` |
| `SWAP BARRIER INCOMPLETE` | Swapchains have not all joined the barrier | Presentation backend; barrier/group ids |
| `SYNC SIMULATED` | Every fact came from a simulated provider | Turn the simulation off before a show |
| `SYNC VERIFIED` | Every required condition is proven | - |

### Rehearsing without a genlock card

The lifecycle agent accepts `--sync-simulate=off|locked|reference-lost|
reference-idle|no-device`, and `scripts/run-editor-demo.ps1` exposes it as
`-SyncSimulate`. Every simulated sample is marked simulated all the way to the
operator surface and to the diagnostic stream, and can satisfy only a DEGRADED
policy - never a STRICT one. Simulated evidence is not evidence.

### Post-show records

Each observation in `<director-wal>.cluster.jsonl` carries `sync_mode`,
`sync_state` (the physical-timing verdict), `program_state` (the first unmet
Program condition overall, which may be an asset or membership hold rather
than a timing one), `sync_required`, `sync_satisfied`, `sync_degraded`,
`sync_simulated`, the group/output/barrier counts, the configuration-issue
count, and the aggregated missed/late/held/repeated/loss counters, so the exact
verdict can be read back without re-deriving it. `hardware_frame_lock_verified`
and `external_time_sync_verified` are set only by a non-simulated verdict that
satisfied a policy demanding it.

### What is still missing before a physical claim is credible

1. A sync-device provider (`net::ClusterSyncStatusProvider`) for vendors
   other than NVIDIA. The NVIDIA one reads Quadro Sync through the NVAPI GSync
   topology/status functions (`apps/server/nvapi_sync_device.cpp`) and is
   built only with `MEDIASERVER_ENABLE_NVAPI`.
2. A present barrier (`rhi::PresentBarrierBackend`) for anything but the
   native D3D12 swapchain on NVIDIA (`apps/server/nvapi_present_barrier.cpp`,
   same build option). SDL_GPU owns its own swapchains and exposes no native
   handle for one, which is why the barrier backend is installable from
   outside the RHI.
3. A platform time provider for PTP/IEEE 1588 clock correlation. PTP is useful
   for tracking timestamps, scheduled deadlines, and telemetry; it replaces
   neither genlock nor swap lock.
4. LED processor and camera-chain integration, plus a measured validation pass
   - a synchronized test pattern and a high-speed camera or measurement rig.
   Software-reported GPU lock does not prove that the emitted photons are
   aligned.

Control health, content readiness, display presence, and physical sync remain
four independent gates. Missing media is renderable using a grey placeholder
with an X, so it does not hold output or disable START/RESTART. Output Monitor
continues to report the source as missing. Missing models, calibration and
lighting assets retain their readiness checks.

