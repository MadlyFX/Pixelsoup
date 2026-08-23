# Implementation status

## Implemented

Everything below exists and is covered by tests. Read it alongside **Frozen**
and **Cancelled**: the durable-log entries in this list are correct, finished,
and receiving no further work, and the recovery orchestration they were heading
towards has been cancelled outright. See
[`implementation-plan-cpp.md`](implementation-plan-cpp.md) §0.1 for why.

- Repository and CMake target structure for `engine/core`, `engine/doc`,
  `engine/cmd`, `engine/wire`, `engine/persist`, `engine/net`, `engine/show`,
  the optional `engine/ui`, and `apps/server`.
- C++20 compiler policy, strict warnings, no RTTI in engine libraries, and
  optional ASan/UBSan builds.
- Persistent document roots with O(1) snapshots, structural sharing on update,
  monotonic versions, and pointer-skipping diffs.
- Director/node role split: only `DirectorScheduler` authors command IDs,
  revisions, transaction history, and undo commands.
- Explicit base/result revision chains and one `FrameSeal` per frame, including
  empty frames.
- Deterministic full-command fingerprints covering metadata plus forward and
  inverse payload content.
- `NodeCommandBus` delivery reordering, exact seal completeness checks,
  identical/conflicting duplicate handling, stale/late classification, and
  latched resync faults.
- Pre-staged immutable frame roots; `commit_frame()` only publishes a prepared
  root and advances cursors after the entire sealed batch validates.
- Stream checkpoints containing the committed frame, revision, next command
  cursor, and immutable root.
- FlatBuffers 25.9.23#1 pinned through a fixed vcpkg baseline, with reproducible
  `flatc` code generation for the versioned control-protocol schema.
- Standalone Asio 1.32.0#0 pinned through the same baseline and linked through
  its canonical `asio::asio` package target on MSVC and clang-cl.
- Strict size-prefixed v1 codecs for commands, frame seals, cursors, and full
  checkpoints, including semantic validation, UTF-8/non-finite rejection,
  bounded verification/allocation, and historical snapshot revision restore.
- Frozen v1 command, seal, cursor, checkpoint, fingerprint, session-message,
  and complete-WAL byte fixtures, plus retained v1 schemas enforced by
  `flatc --conform` checks.
- An explicit little-endian, CRC32C-protected sealed-frame WAL with ordered
  body/commit persistence, durable receipts, strict cursor chains, fatal
  committed-corruption detection, and repair of incomplete final entries.
- A fixed-record release ledger with authenticated commit markers and exact WAL
  identity binding across before/after cursors, physical offset, serialized
  length, and stored entry CRC32C. Recovery uses bounded memory, repairs only an
  incomplete final decision, and never infers that an unreleased WAL suffix is
  safe merely because its bytes are durable.
- A bounded-memory unresolved-release inventory that authenticates the captured
  release-ledger prefix against the corresponding immutable WAL prefix, reports
  exact released/unresolved frame and byte boundaries, and retains the first
  unresolved WAL identity for an explicit recovery workflow without treating it
  as authorized.
- A separately bound, append-only cohort-recovery journal with durable
  Begin/Bind/Abort, explicit legacy adoption and reconfiguration, and
  generation-fenced recovery attempts. Its CRC-chained records are recovery
  intent only: every selected frame still has to match an immutable WAL
  snapshot before any node action or release.
- A capability-narrow recovery release authority that revalidates the captured
  inventory and exact physical WAL identity immediately before one contiguous
  release-ledger append, then verifies the complete post-append frontier. Any
  ambiguous persistence outcome permanently fences that authority instance.
- A recovery-only unresolved-cohort coordinator that reconstructs a checkpoint
  at the durable release frontier, durably records a fresh attempt before node
  admission, requires exact node/incarnation checkpoint activation and
  `Prepared` quorum for only the first unresolved frame, and releases at most
  that one frame. It exposes no commit-permit or authoring surface and requires
  a clean process restart after success.
- A typed command-log bridge that validates complete batches before append and
  fully decodes/cross-checks each replay frame before node delivery and commit.
- Exact-boundary typed WAL range visitation with complete preflight before the
  first callback, which lets a restarted Director stream historical batches
  without depending on its reset authoring indexes.
- Move-only immutable WAL-prefix snapshots captured under the same Director
  transaction lock as sealing. Sources are coordinator-bound capabilities;
  reconnect workers decode only their frozen prefix while later durable
  appends proceed independently.
- Tokenized two-phase Director sealing: prepare performs validation/allocation
  without publishing state, while exact-token commit is allocation-free and
  stale prepare callbacks cannot commit or abort a newer attempt.
- `DurableDirector` coordination enforcing cursor alignment, Data/Full WAL
  durability, exact receipt validation, Director commit, then publication of a
  coordinator-bound committed batch. Ambiguous storage outcomes latch
  `RecoveryRequired` and prohibit retry or publication.
- Release-gated `DurableDirector` coordination that persists one contiguous
  release ordinal before minting a commit permit, blocks resume capture while
  the WAL tail is beyond the release frontier, and latches recovery on
  indeterminate ledger outcomes or post-release invariant failure.
- Genesis-only startup recovery that replays into unpublished temporary state,
  retires each verified frame so command-bus history stays bounded by one frame,
  returns fresh Director/local-node objects only after complete success, and
  reports the intentional reset of undo and historical indexes.
- Generation-bound, two-phase node checkpoint staging: decode and allocation
  leave the visible root/cursor/fault untouched, traffic is explicitly blocked
  while pending, and only exact-token activation publishes the replacement.
- A frozen, separately versioned `MSNP` v1 session codec for hello/welcome,
  complete frame batches, checkpoint transfer, sync completion, resume,
  prepared and committed acknowledgements, cursor-bound frame commit permits,
  heartbeats, heartbeat acknowledgements, directional graceful shutdown, and
  rejection, with strict semantic and resource validation.
- Blocking standalone-Asio TCP framing and Director/node resume state machines:
  exact epoch/cursor matches replay a captured WAL tail; old epochs, unavailable
  ranges, replay-window overflow, and faulted nodes stage a full checkpoint.
  Commands and seals are fully decoded before node delivery, checkpoints remain
  invisible until explicit activation, and only durable coordinator batches can
  enter the live send path.
- Explicit per-frame `Prepared` and `Committed` acknowledgements with one
  monotonic node-control sequence. The Director retains uncommitted frame/byte
  windows, validates exact batch/cursor identity and ordering, and releases
  capacity only after commit; malformed, duplicate, and out-of-order acks close
  the session deterministically.
- Node show-clock commit performs only the allocation-free command-bus publish
  plus enqueue into a negotiation-time fixed acknowledgement ring, and is
  rejected until the matching durable Director commit permit has arrived. A
  dedicated network-worker flush performs encoding/TCP writes and retires
  receive-window entries only after `Committed` is written; `Prepared` must be
  written before the corresponding permit and show-clock commit are accepted.
- A bounded Director session hub with atomic snapshot-tail registration,
  preallocated per-session rings, cursor-contiguous fan-out, and no socket I/O
  on the durable publication path. A slow or gapped subscriber alone becomes
  `ResyncRequired`; the durable seal outcome remains successful and available.
- Hub-issued resume sources feed the same connection state machine that drains
  subsequent published frames, closing the capture-to-live race. Foreign or
  moved-from sources are rejected by the receiving coordinator.
- A bounded Director TCP service with one acceptor and fixed worker capacity.
  Every connection receives a non-reused service-generation session ID, one
  shared aggregate receive-admission budget, an atomic hub subscription, and
  isolated publisher/acknowledgement loops. Replay commits drain before live
  registration; held frames retry after window release; overflow fences only
  that worker; stop wakes accept, resume, backlog, permit, and acknowledgement
  waits. Service start fails closed unless its observer coordinates durable
  commit permits, and the sole publisher drains permit intents before live
  frame traffic.
- End-to-end heartbeat supervision over the shared directional sequence spaces.
  The Director advertises only the durable release frontier, requires one exact
  response by a steady-clock deadline, and does not treat unrelated traffic as
  liveness. Node responses share the preallocated control FIFO with frame
  acknowledgements, while service shutdown sends one bounded, directionally
  correct `Goodbye` after membership departure and before transport close.
- A supervised outbound node connector with cancellable asynchronous dialing,
  capped reconnect backoff, stable node/incarnation identity, checkpoint-boundary
  activation, a sole live reader and sole control writer, terminal lease/protocol
  fault classification, and recovery from lost committed acknowledgements using
  the node's visible cursor. Node-only server startup can opt into this connector
  with a validated numeric unicast endpoint and explicit show/node/incarnation
  IDs; the actual show-clock boundary driver remains deliberately unwired.
- A fixed-capacity release-gated frame barrier whose membership cut spans cohort
  reservation, durable hub publication, and allocation-free finalization.
  Exact Prepared quorum advances releases contiguously, persists the decision
  outside the barrier lock, then queues one cursor- and batch-bound permit per
  eligible session. Session generations cannot repair an older lost obligation;
  a pre-release loss freezes that frame and every later prepared frame. Catch-up
  sessions are authorized only through an exact historical release lookup, and
  a raw-hub lineage bypass latches a tracking fault without hiding the already
  durable frame.
- Transactional complete-batch node ingestion: semantic conflicts and
  allocation failures return diagnostics without changing buffered traffic,
  the visible root/cursor, prepared state, or the resync latch.
- Negotiated replay and node receive windows, per-connection sequence/session
  fencing, required node-incarnation leases backed by a bounded Director
  registry, one-reader/one-writer stream safety, and reconnect atomicity for
  malformed or truncated session frames.
- Interruptible TCP close, steady-clock handshake/read/write deadlines, and a
  listener-shareable admission budget that rejects aggregate receive memory
  pressure before payload allocation.
- Role-aware server bootstrap: only the Director opens the WAL; node-only roles
  never touch it, the Director validates the release sidecar and refuses an
  unresolved durable suffix before constructing any publishable runtime, then
  constructs its session hub before readiness. The normal process remains alive
  until an interrupt or termination signal. The listener remains disabled by
  default; an explicit numeric bind endpoint plus show/epoch identity is bound
  and validated before readiness, and worker shutdown precedes hub shutdown.
  Node-only roles may independently supervise an outbound Director connection
  without ever opening Director persistence; validation mode never dials.
- Context-aware Director undo with postcondition conflict detection and fresh
  inverse commands sent through the normal sealed path.
- Deterministic in-memory loopback transport for multi-node permutation and
  recovery tests.
- An exact rational `ShowClock` covering play, pause, stop, exact seek, loop,
  and end-of-show hold. It reads no clock of its own, carries sub-frame phase as
  an integer remainder so a 60000/1001 show accrues no drift over any run
  length, reports the true number of boundaries a late caller crossed rather
  than hiding them, and refuses time regression and stale observations instead
  of synthesising elapsed frames.
- Replicated transport state: the show position is a well-known document node
  authored by the Director as an ordinary command inside a sealed frame, so
  every node derives the frame it presents from the same committed revision
  rather than from a local clock reading or a side channel.
- A bounded single-producer/single-consumer document-root handoff to playback.
  Slots are allocated once; publishing copies a shared_ptr and stores an index,
  consumption empties its slot, and a refused publish leaves the frame with its
  caller. A companion retirement ring parks superseded roots so no trie
  destructor runs on the boundary either. Overflow is reported, never resolved
  by discarding approved state.
- A frame-boundary driver that turns one wall-clock observation into the
  boundaries crossed, the control frames the Director has approved since the
  last boundary, and the immutable roots playback should present. It allocates
  nothing, takes no lock of its own, and performs no I/O; an unapproved frame is
  held rather than invented, a staged checkpoint is adopted only at a boundary,
  catch-up is bounded per tick, and a stalled presenter never stalls the control
  plane. An allocation-counting test guard enforces the boundary contract
  against both malloc and free.
- A commit seam with two implementations: a lock-free local command-bus sink
  used offline and in tests, and a node-session sink whose readiness questions
  are answered by a session-locked boundary query so the driver never reads the
  command bus the network worker is concurrently staging into.
- Deterministic timecoded test frames rasterised into a caller-owned surface:
  frame-cycling border, sweep bar, non-drop timecode, show/control frame
  readouts, per-node tint, and explicit held/resync markers. Rendering
  allocates nothing, clips to the declared surface, and never touches stride
  padding.
- A presentation seam with two implementations behind one interface: a
  headless rasteriser whose surface digest makes an automated run checkable,
  and an SDL3 window shell for the Director preview and node outputs. present()
  runs immediately after the commit boundary and never inside it, because a
  platform driver may allocate or block and no commit the cluster is waiting on
  may be delayed by drawing. The seam carries no document reference, so a
  presenter can never walk the trie.
- A presenter readback self-check. The SDL path reads the presented output back
  from the renderer and digests it, so pixel order, pitch, texture upload, and
  render are all verified against the rasterised source rather than assumed.
  Its tests run on the SDL dummy video driver and therefore need no display.
- Operator transport keys on the Director preview window, dispatched by window
  id off the shared SDL event queue so a second window cannot steal the first
  window's events. Node output windows are input-inert by construction.
- Role-aware playback wiring in the server binary. `--playback` makes the
  Director seal exactly one control frame per show frame through the
  release-gated barrier, and makes a node pace its own boundary, present only
  approved frames, and report presented show/control frame, revision, transport
  state, held boundaries, skipped frames, handoff overflows, and connection
  state. Transport options are refused without `--playback`, node playback
  without a Director connection, Director playback without the listener that
  releases its frames, and a window request in a build without the shell.
- Two clocks on the Director, deliberately. A free-running pacer decides when a
  control frame is due and a transport clock decides only where the show is, so
  pausing never stops the frame stream that carries transport changes and keeps
  the cohort acknowledging. Unchanged transport seals an empty frame instead of
  re-authoring identical state, which makes a paused show nearly free: an
  observed 81-frame paused run authored exactly one command.
- SDL3 3.2.26 pinned through the same fixed vcpkg baseline and carried as an
  optional manifest feature, so a headless node never links a video subsystem
  and the default build needs no display stack.
- Focused C++ suites registered across twenty-four CTest targets covering arena
  behaviour, snapshot persistence, diff correctness, sealed
  ordering/completeness, corrupted/conflicting traffic,
  atomic failure, checkpoint resync, multi-node convergence, Director-owned
  undo, wire compatibility, WAL corruption/truncation, durable crash windows,
  startup isolation, exact restart continuation, TCP fragmentation, session
  compatibility, immutable concurrent reconnect capture, node-lease fencing,
  staged checkpoint activation, atomic snapshot/live handoff, queue overflow,
  acknowledgement ordering/rejection, window bounds, transactional rejection,
  timeout/cancellation, aggregate admission, bounded worker/session-ID
  capacity, replay-to-live catch-up, held-frame retry, membership-cut races,
  degraded cohorts, durable release crash windows and identity mismatches,
  cohort-journal truncation/corruption and stale-generation fencing,
  Prepared-only unresolved recovery and clean-restart continuation, permit
  ordering and fail-closed restart, resync/reconnect fencing, and malformed or
  truncated traffic, plus two schema-conformance checks and the role-ownership
  bootstrap smoke test.

## Product-focus checkpoint

Thirty focused CTest targets pass in the current strict clang-cl and MSVC lanes,
in both the headless and SDL3 configurations.

**V0 - playback plumbing - is complete.** A Director and two node processes run
the show clock end to end over TCP, each with its own output window: the
Director seals one control frame per show frame through the release-gated
barrier, and both nodes independently pace their own boundaries and present the
same numbered test frame. Play, pause, and exact seek all reach both nodes, a
node joining a running show adopts its checkpoint at a boundary, and the commit
boundary performs no allocation, no deallocation, and no network I/O.

Three automated checks carry that claim rather than asserting it: an in-process
convergence test over real loopback TCP, allocation-guarded boundary tests that
replace every global new/delete, and a presenter readback that compares what the
renderer actually displayed against what was rasterised. A three-process
windowed run reported zero present failures and identical show/control/revision
frames on both nodes.

**And V0 surfaced the problem that now sets the priority.** The durable seal
path caps the Director near 20-25 sealed frames per second, so the system as
built cannot reach 60 fps, let alone 120. See the strategy note below.

## Strategy - what changed after V0

The full reasoning and the revised gate table live in
[`implementation-plan-cpp.md`](implementation-plan-cpp.md) §0, and the recovery
model is recorded at architecture level in
[`media-server-architecture-plan.md`](media-server-architecture-plan.md) §10.1.
Section numbers below refer to the plan document. In short:

The control plane is built well beyond what any stated requirement asks of it,
while the parts that decide whether this ships - GPU rendering, feed mapping,
warp/blend, media decode - have no code. The correction is to return to the
ordering `media-server-architecture-plan.md` §11 already prescribes, whose Phase
0 (RHI, frame graph, colour pipeline, **and the thin output node**) is precisely
the part that was skipped.

Three decisions were taken, and they change what counts as in scope:

1. **The recovery model is restart and re-cue.** A show does not resume an exact
   frame after a Director failure; node failure is covered by hot-spare
   promotion. The durable log becomes a project artifact and an audit trail.
2. **Durability cadence is not frame cadence.** The per-frame seal, release and
   permit path stays in memory - it is what makes two nodes agree, and that
   guarantee is kept. Durability becomes edit-shaped. Separately, the transport
   becomes an anchor keyed to the **control-frame ordinal**, so a playing show
   makes no document edits at all and exact agreement needs no clock.
3. **The first RHI is SDL_GPU**, behind an explicit `engine/rhi` seam. The
   Spartan fork moves to the 3D phase where its value actually lives.

## Cancelled

Work that a previous version of this document listed as deferred, and that
plan decision §0.1 removes the requirement for. This is not a backlog; it is
closed.

- Production orchestration for unresolved-cohort recovery: Begin-before-WAL
  cohort journalling, Bind-before-fanout, Begin-to-Bind startup reconciliation,
  and a prepare-only recovery listener/node mode. Under restart-and-re-cue there
  is no requirement for exact frame resumption, so none of this has a
  specification to satisfy.
- Atomic checkpoint-file replacement and a cryptographic binding between the
  checkpoint root and WAL generation, as a *resumption* mechanism. A project
  save file still wants atomic replacement; that is a different, smaller job.
- Content-addressed incremental snapshot/diff storage, as a means of bounding
  per-frame durable writes. Plan decision §0.2 removes per-frame durable
  writes instead.

The implemented storage engine stays in the tree. It is correct, it is tested,
and it is the audit trail. It simply stops growing.

## Frozen

Implemented and correct, receiving no further work. Startup keeps its
fail-closed refusal of an unresolved durable suffix; it costs nothing and is
correct under any recovery model.

- The sealed-frame WAL, release ledger, cohort journal, unresolved-release
  inventory, and recovery release authority.
- Native exclusive writer locking across those files. Callers serialize
  ownership today; short-write, torn-tail, sync-failure and corruption paths
  have deterministic coverage.
- Separately versioned Director authoring metadata for undo/transaction and
  historical command/seal indexes across restart.

## Still deferred

- Transport authentication, TLS, and a cryptographic binding between session
  identity, checkpoint roots, and WAL generation. Current IDs and fingerprints
  are fencing and corruption checks, not an authentication boundary.
- A cryptographic command/root digest. The deterministic 64-bit fingerprint is a
  corruption and compatibility guard.
- Generalized lock-free cross-thread document publication and a build-wide debug
  allocation hook. The playback path has a bounded root handoff, a retirement
  ring, and an allocation-counting test guard that enforces the frame-boundary
  contract.
- OCIO, ROI propagation, Rivermax, PTP, physical output, and
  hardware-in-the-loop validation.
- The Spartan import and baseline capture, the frame graph, and the Atlas /
  hybrid-rendering ports - moved to the 3D phase by plan decision §0.3, not
  abandoned.

## Next slice - V1, frame-rate headroom

Small, and it unblocks everything after it.

1. **Apply the recovery-model decision.** Stop writing a WAL entry and release
   decision per sealed frame. Keep the in-memory seal, release and permit path
   exactly as it is - that is the agreement guarantee. Journal operator edits
   instead.
2. **Re-key the transport to the control-frame ordinal.** Replace the
   per-frame `show.transport` republish with
   `(anchor_control_frame, anchor_show_frame, rate, direction, state)`, and
   derive the presented frame by integer arithmetic on the control frame. Nodes
   keep agreeing exactly, with no clock involved and no per-frame document edit.
3. **Retire Director authoring indexes as frames seal**, so a long show has flat
   memory. This is the one frozen-area item that V1 does need: the show clock is
   the first thing that exercises it continuously.
4. **Prove it.** Two nodes holding 120 fps for ten minutes, zero skipped show
   positions, identical show frames, flat memory. Extend
   `mediaserver.show_playback` with a sustained-rate case, and add a soak target.

Then V2 (GPU output path over SDL_GPU) and V3 (feed mapping, warp, blend,
calibration) per the gate table. Media and the operator surface follow at V4/V5.

### Known limits of the current playback path

- Durable seal throughput is the current rate ceiling: one WAL append plus one
  release decision per sealed frame, sustaining roughly 20-25 frames per second
  with `SyncPolicy::Full` on a development machine. Above that the show clock
  outruns its own seal stream - every node still presents an approved frame and
  all nodes still agree, but the show position advances in jumps. **This is what
  V1 removes.** The attribution to per-frame sync is inferred from the measured
  ceiling, not from a profile; confirm it before assuming the fix landed.
- Director authoring indexes grow per sealed frame. Transaction history, the
  per-frame command map, and the seal map are never retired, so a long show
  grows Director memory. V1 item 3.
- Presentation is a CPU surface uploaded to a streaming texture every frame -
  the intended fallback, and a full copy per frame per output. V2 removes it.
- The window shell has no compositor: it presents the timecode test pattern and
  nothing else. Layers, transforms and real media arrive at V4.
- `NodeSessionService::commit_prepared_frame` takes a session mutex. The
  boundary is allocation-free and performs no I/O, but it is not lock-free; the
  local command-bus sink is.
- The show position is republished every frame, which is the simplest
  construction that makes two nodes provably agree and is what the current tests
  assert. V1 item 2 replaces it with an anchor that preserves the same property
  at no per-frame cost.
