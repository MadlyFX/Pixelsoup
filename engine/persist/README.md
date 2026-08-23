# Sealed-frame write-ahead log

`SealedFrameLog` is a codec-agnostic, caller-serialized single-writer
append-only log. Native cross-process exclusive locking is not implemented, so
the owner must prevent concurrent opens. `CommandLog` adds in-process
synchronization between its typed append and immutable-prefix capture APIs. The
wire layer supplies one opaque record per command followed by one opaque frame
seal record. Persistence validates the cursor/revision chain but does not
interpret those records.

`CommandLog` is the typed bridge used by the command pipeline. Its `append()`
method validates a complete command/seal batch, canonically writes the command
messages followed by the seal, and only then calls the raw WAL. Its
`replay_into()` method begins at a `NodeCommandBus` cursor and fully decodes and
cross-checks each frame against the outer WAL metadata before delivering any of
that frame to the node. Prepared frames are committed immediately. Failures
retain distinct log, encode, decode, cross-record protocol, and node categories.
The default replay overload preserves committed duplicate-detection history for
live nodes. `ReplayRetention::RetireCommitted` is an explicit offline mode that
retires each frame immediately after verified commit, reports peak/final history
diagnostics, and surfaces retirement refusal as its own error alternative. A
refusal occurs after that frame commits, so callers must discard the offline
replay node; genesis recovery already does so by returning no runtime.

`CommandLog::capture_committed()` briefly serializes with append and returns a
move-only `CommandLogSnapshot`. The snapshot owns its path and frozen
base/tail/valid-byte boundary plus all decode limits. Replay sequentially
validates only that immutable committed prefix; the physical file may grow and
no live writer metadata or entry-offset vector is consulted. Snapshot visitors
therefore never hold the append mutex, including while blocked on network I/O.

`DurableDirector` is the single-owner coordinator for authoring. It requires a
Data or Full log whose tail exactly matches the Director checkpoint, snapshots
the frame commands, prepares the seal, appends the canonical batch, validates
the durable receipt, and only then commits with the exact transient preparation
token. A known encode, decode, or protocol rejection is proven pre-write and
aborts that exact preparation so the caller may correct the configuration and
retry. Any storage failure or post-append invariant failure enters
`RecoveryRequired`, leaves the preparation latched, and rejects both retry and
publication; the process must recover from the WAL instead.

Successful sealing returns a move-only `CommittedFrameBatch`. Batches are bound
to their creating, non-copyable coordinator; moved-from, foreign, non-durable,
or recovery-state batches cannot be broadcast. Publication queues canonical
commands followed by the seal through `LoopbackTransport`. An allocation
failure can leave an identical prefix queued and returns false; retrying the
same batch is safe because the node protocol recognizes identical commands and
seals as duplicates.

`DurableDirector` serializes the complete WAL-append/Director-commit transaction
with resume-source capture. `DurableResumeSource` value-owns both the immutable
WAL snapshot and its matching document checkpoint and is a move-only capability
bound to the creating coordinator. An optional bounded registration hook runs
under the same transaction lock after snapshot/checkpoint equality is proven;
the session hub uses it to install the exact captured tail before any later
seal can publish. A caller therefore cannot observe the durable
append-before-Director-commit window or a capture-to-live gap, and no
persistence lock survives into replay or network delivery.

`recover_genesis_runtime()` accepts only a `0/0/1` log base. It replays into a
temporary unpublished node using immediate committed-history retirement, so its
command-bus buffering stays bounded by one frame rather than total log history.
It returns fresh, value-owned Director and local node state only after the
complete log succeeds. Version 1 reconstructs the document and exact
frame/revision/next-command cursor, but intentionally resets Director-local
undo/transaction history and historical command/seal indexes; the returned
reset report makes both losses explicit.

All integers are unsigned little-endian. CRCs use CRC32C (Castagnoli), with an
initial value of `0xffffffff` and a final bitwise complement.

## Version 1 layout

The file begins with a 64-byte header:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `MSCMDLOG` |
| 8 | 2 | format version (`1`) |
| 10 | 2 | header size (`64`) |
| 12 | 4 | flags (`0`) |
| 16 | 8 | base through-frame |
| 24 | 8 | base revision |
| 32 | 8 | base next-command ID |
| 40 | 20 | reserved zero bytes |
| 60 | 4 | CRC32C of bytes `[0, 60)` |

Each sealed frame has an 80-byte prefix, a length-prefixed record payload, and
a 24-byte commit trailer:

| Prefix offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `MSFRAME1` |
| 8 | 2 | format version (`1`) |
| 10 | 2 | prefix size (`80`) |
| 12 | 4 | flags (`0`) |
| 16 | 8 | total entry bytes |
| 24 | 8 | frame |
| 32 | 8 | base revision |
| 40 | 8 | result revision |
| 48 | 8 | first command ID |
| 56 | 8 | next command ID |
| 64 | 4 | record count |
| 68 | 4 | payload bytes |
| 72 | 4 | CRC32C of prefix bytes `[0, 72)` |
| 76 | 4 | reserved zero |

The payload repeats `u32 byte_count` followed by exactly that many opaque
bytes. The final record is always the seal, including for an empty frame.

| Trailer offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `MSCOMMIT` |
| 8 | 8 | total entry bytes |
| 16 | 4 | CRC32C of prefix, payload, and trailer bytes `[0, 16)` |
| 20 | 4 | trailer CRC32C of trailer bytes `[0, 20)` |

The trailer CRC authenticates the commit-marker structure, total length, and
stored entry CRC independently of the frame body. Recovery treats a partial or
invalid final commit marker as a torn append and truncates that entire entry
when repair is enabled. An invalid marker on an interior entry is corruption.
Once a marker is valid, a mismatched body/entry CRC is committed corruption and
is never silently discarded. If the next prefix itself is invalid, recovery
conservatively scans the remaining suffix with bounded memory. Without an
authenticated marker it is an uncommitted torn tail; any valid marker whose
length points to that entry or a later entry is commit evidence and makes the
prefix damage fatal. Scanning the whole suffix preserves committed frames even
when a newer torn append follows them. Other invalid layouts, limits, and cursor
chains are also fatal. Defaults cap records at 16 MiB, frame payloads at 64 MiB,
and frames at 65,536 records.

## Release-ledger version 1 layout

`ReleaseLedger` is a separate append-only durability authority for publication.
Its entries release a contiguous prefix of one immutable `CommandLogSnapshot`;
they do not infer release from the WAL tail. A missing or zero-length ledger may
be initialized only when the captured WAL tail equals its base. `SyncPolicy::None`
is rejected because no non-durable receipt may authorize a commit permit.
Release append is deliberately private: only `DurableDirector` can mint a
decision from its coordinator-bound, move-only committed-batch capability.
Callers cannot construct a plausible cursor/offset tuple and advance the live
frontier without that exact WAL append authority.

The ledger begins with a 64-byte header:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `MSRELLOG` |
| 8 | 2 | format version (`1`) |
| 10 | 2 | header size (`64`) |
| 12 | 4 | flags (`0`) |
| 16 | 8 | base through-frame |
| 24 | 8 | base revision |
| 32 | 8 | base next-command ID |
| 40 | 20 | reserved zero bytes |
| 60 | 4 | CRC32C of bytes `[0, 60)` |

Every release decision is exactly 128 bytes: a 104-byte prefix followed by the
same 24-byte authenticated commit-trailer shape used by the WAL.

| Prefix offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `MSRELES1` |
| 8 | 2 | format version (`1`) |
| 10 | 2 | prefix size (`104`) |
| 12 | 4 | flags (`0`) |
| 16 | 8 | total entry bytes (`128`) |
| 24 | 8 | one-based release ordinal |
| 32 | 8 | before through-frame |
| 40 | 8 | before revision |
| 48 | 8 | before next-command ID |
| 56 | 8 | after through-frame |
| 64 | 8 | after revision |
| 72 | 8 | after next-command ID |
| 80 | 8 | exact WAL entry offset |
| 88 | 8 | exact WAL entry byte count |
| 96 | 4 | stored WAL entry CRC32C |
| 100 | 4 | CRC32C of prefix bytes `[0, 100)` |

| Trailer offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `MSCOMMIT` |
| 8 | 8 | total entry bytes (`128`) |
| 16 | 4 | CRC32C of entry bytes `[0, 120)` |
| 20 | 4 | trailer CRC32C of trailer bytes `[0, 20)` |

Recovery first scans the ledger's ordinal, cursor, and physical-WAL byte chains
with fixed memory. It then visits the immutable typed WAL once and rereads the
corresponding fixed ledger record at `64 + index * 128`; every released record
must match the WAL's before/after cursors, offset, length, and stored entry CRC.
The WAL may contain a later unresolved suffix, but the ledger may never extend
beyond or disagree with it. Exact historical ordinal lookup authenticates one
candidate record from disk and exact-compares both cursors, retaining no
history-sized in-memory index.

`UnresolvedReleaseInventory::inspect()` is the read-only recovery bridge between
those two files. It captures the ledger's usable flag, counts, cursors, and
valid-byte boundary once, then streaming-merges that immutable decision prefix
with a typed WAL snapshot. Every released frame is exact-matched by physical
identity, the snapshot must contain the ledger-bound WAL tail at the recorded
frame count, and only a forward WAL extension is accepted. The result retains
the exact first unresolved identity, prefix/suffix counts and cursors, and byte
boundaries; inspection never repairs either file or exposes release authority.

A release append uses prefix write -> data sync -> commit-trailer write ->
configured Data or Full sync. Only then is a move-only durable receipt returned.
Final torn entries are repairable under the same authenticated-commit rules as
the WAL; committed or interior corruption is fatal. Every durable open also
syncs the parent directory on POSIX, including Data-policy creation and retries,
so a receipt cannot outlive the ledger's directory entry after power loss.

WAL append ordering is body write -> data sync -> commit-trailer write ->
configured final sync. `SyncPolicy::None` skips only that final commit sync (and
returns a non-durable receipt); it deliberately retains the body ordering
barrier so a crash cannot make an incomplete append look like committed
corruption. Data and Full receipts are durable. Durable opens also sync the
parent directory on POSIX so the WAL remains reachable by name after power
loss.

## Cohort-recovery journal version 1

`CohortJournal` is a separate append-only intent and fencing log for explicit
unresolved-frame recovery. It is deliberately not a release authority. A
`BindFrame` or `AdoptFrame` stores a claimed `WalFrameIdentity`, but a recovery
consumer must exact-match that identity (including its entry CRC32C) against an
immutable `CommandLogSnapshot` before it can drive a node or publish anything.
The journal itself never infers adoption, quorum, or release.

Automatic creation consumes an `UnresolvedReleaseInventory` and is permitted
only when the captured WAL tail equals the durable release frontier. This means
a missing journal cannot silently adopt an older nonempty uncovered WAL suffix;
that case requires a future explicit migration workflow. The 128-byte header
anchors a nonzero journal UUID and show ID to the WAL base cursor, release
cursor, released-WAL byte boundary, release decision count, and exact captured
release-ledger valid-byte boundary. Existing opens require the same identity and
base plus non-regressing WAL/release counts, cursors, and byte boundaries.

An open freezes those authoritative WAL and release cursors. Later external
appends or release decisions are not observed through the live object. The
caller must reopen after such progress before appending `AttemptStarted` or
`ReconfigureFrame`. `BindFrame` has one intentional exception: its protocol is
`BeginFrame` -> durable WAL append -> `BindFrame`, so it accepts only the exact
next contiguous WAL receipt after the captured boundary. `AdoptFrame` has no
such exception and must name a pre-existing identity inside the captured WAL.

The record state machine is strict:

- `BeginFrame` durably fixes a plan token unique within that exact frame
  lineage, expected before cursor, exact next frame, cut Director epoch,
  generation 1, deterministic plan fingerprint, and the full canonical cohort
  before a WAL frame exists.
- `BindFrame` closes that exact begin ordinal/token with the exact contiguous
  WAL identity and matching plan generation/token/fingerprint.
- `AbortFrame` closes an exact begin only before it is bound and records an
  explicit reason.
- `AdoptFrame` names a captured legacy-uncovered WAL identity, nonzero operator
  request ID, fixed `LegacyUncovered` provenance, generation-1 plan, and full
  replacement cohort.
- `ReconfigureFrame` applies only to the exact next unresolved frame. It carries
  a full compare-and-swap over the prior plan generation, token, fingerprint,
  journal ordinal, release frontier, and WAL tail, followed by a new generation,
  per-frame-unique token, operator request ID, reason, and full cohort.
- `AttemptStarted` also applies only to the exact next unresolved frame. Its
  monotonically increasing generation and per-frame-unique attempt token are
  fenced by the Director epoch, session namespace, exact release/WAL cursors,
  and active plan generation/token/journal ordinal.

Cohorts are bounded and byte-canonical: members are sorted lexicographically by
exact 128-bit node ID then incarnation, every ID is nonzero, and a node ID may
appear only once. Empty cohorts require the strong
`EmptyCohortConfirmation::ExplicitlyEmpty` value; nonempty cohorts require
`None`. Plan fingerprints are deterministic FNV-1a-64 corruption/fencing
guards, not authentication. Reusing an operator request ID with byte-equivalent
`AdoptFrame` or `ReconfigureFrame` content returns the original durable receipt
before ordinal CAS; conflicting reuse is rejected.

All values are little-endian. The header layout is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `MSCOHORT` |
| 8 | 2 | format version (`1`) |
| 10 | 2 | header size (`128`) |
| 12 | 4 | flags (`0`) |
| 16 | 16 | journal UUID (high, low) |
| 32 | 16 | show ID (high, low) |
| 48 | 24 | WAL base cursor |
| 72 | 24 | anchor release cursor |
| 96 | 8 | anchor WAL byte offset |
| 104 | 8 | release decision count |
| 112 | 8 | release-ledger valid bytes |
| 120 | 4 | reserved zero |
| 124 | 4 | CRC32C of bytes `[0, 124)` |

Each variable-length record has a 64-byte common prefix, a kind-specific
canonical payload, and a 24-byte authenticated commit trailer:

| Prefix offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `MSCOHRT1` |
| 8 | 2 | format version (`1`) |
| 10 | 2 | prefix size (`64`) |
| 12 | 1 | record kind (`1` through `6`) |
| 13 | 1 | flags (`0`) |
| 14 | 2 | reserved zero |
| 16 | 8 | total record bytes |
| 24 | 8 | strict one-based journal ordinal |
| 32 | 4 | previous committed record CRC32C (`0` for ordinal 1) |
| 36 | 4 | payload bytes |
| 40 | 4 | CRC32C of prefix bytes `[0, 40)` |
| 44 | 20 | reserved zero |

The fixed payload portions are 96 bytes for `BeginFrame`, 128 for `BindFrame`,
32 for `AbortFrame`, 144 for `AdoptFrame`, 224 for `ReconfigureFrame`, and 208
for `AttemptStarted`. Begin, Adopt, and Reconfigure append exactly 32 bytes per
cohort member. The commit trailer is `MSCOMMIT`, total record bytes, a record
CRC32C over the prefix/payload/trailer bytes through the trailer length, and a
trailer CRC32C. The record CRC is chained into the next prefix.

Append ordering is body write -> Data sync -> commit-trailer write -> configured
Data or Full sync. `SyncPolicy::None` is forbidden, and durable opens sync the
parent directory on platforms that expose directory fsync. Only a physically
final record without authenticated commit evidence is repairable. Authenticated
body corruption, chain/state violations, interior damage, or a damaged prefix
that crosses any later authenticated commit marker is fatal. Counts, cohort and
record sizes, file bytes, offsets, and arithmetic are bounded before allocation
or I/O. The writer and its receipts are move-only; in-memory state and capacity
are fully prepared before the first body byte, leaving only no-throw publication
after final sync.

## Recovery release authority

`RecoveryReleaseAuthority` is the narrow persistence boundary used after an
explicit recovery cohort has prepared one unresolved frame. The production
`ReleaseLedgerRecoveryReleaseAuthority` accepts only an immutable
`CommandLogSnapshot`, its exact `UnresolvedReleaseInventory`, the inventory's
first unresolved `WalFrameIdentity`, and the next release ordinal. It exposes no
WAL append, adoption, normal publication, or commit-permit operation.

Before entering the private ledger append, the adapter decodes and exact-matches
the selected typed WAL frame, captures the exact following identity when one
exists, re-runs inventory authentication against the live ledger, and serializes
that whole sequence per adapter instance. After a durable receipt, it verifies
the exact ledger-report transition, refreshed inventory, and next unresolved
identity. A stale or malformed preflight remains retryable without touching the
ledger. Any append-phase failure or impossible postcondition permanently faults
the adapter instance, requiring the recovery process to reopen its durable
sources instead of guessing whether release occurred.
