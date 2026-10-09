# Director authoring link protocol v1

Implemented by `net/authoring_link_protocol` and `render/director_link_service`.
The reference client is `scripts/scene-link-client.py`. Step 05 implements Blender
collection evaluation and publication in `integrations/blender/mediaserver_livelink`.

## Transport and pairing

The Director editor explicitly starts a loopback IPv4 TCP listener in Preview.
The OS selects a port. **File > Scene Link Details...** shows the endpoint and a
256-bit OS-random pairing code. Copy details copies them to the local clipboard.
Stop cancels I/O and ingestion and revokes the pairing; Start generates a new code,
epoch and scene token. Pairing is ephemeral, never saved in a project or sent to
output nodes. There is no LAN or TLS mode in v1. Local clients with the code are
authorized to submit Preview edits.

After reviewing the Director/node session and lifecycle seams, the existing
`TcpFramedStream` was selected for its bounded complete records, size-prefix
validation, deadlines and poisoned-stream behavior. Authoring uses a separate
listener and protocol; it cannot submit node replication commands or touch GPU
state. The new `interrupt()` wakes I/O without joining from the control loop.

One connection, one resource transfer and one scene transaction are processed at
a time. Clients wait for each resource acknowledgement and for the terminal scene
receipt before sending the next request. Bulk bytes use separate message types
and chunks on the same socket as controls. TCP flow control supplies backpressure;
there is no application backlog of scene transactions. An initial handshake and
each write have two-second deadlines; established reads have a 30-second deadline.
Clients can send Status as a keepalive. Stop also interrupts partial-frame reads.

## Frame layout

Integers are unsigned little-endian; native C++ structs are never serialized.

| Field | Encoding |
| --- | --- |
| Body length | `u32`, excludes its own four bytes |
| Magic | Four bytes, `MSL4` |
| Version, kind | Two `u16` values; version is exactly 1 |
| Epoch, scene, transaction, origin, source, base, revision, resource | Eight strings in this order; each `u16` length followed by printable ASCII, at most 128 bytes |
| Resource offset, total resource length | Two `u64` values |
| Payload | `u32` length followed by exactly that many bytes |

Total packet capacity is 1 MiB + 4096 bytes. Snapshot/delta payloads are at most
1 MiB, resource chunks at most 256 KiB, and other payloads at most 4096 bytes.
Trailing bytes, truncated fields, oversized strings and unknown kinds fail.
Control/manifest payloads contain text; resource chunks contain opaque bytes.
Unused strings are empty and unused numbers zero.

| Kind | ID | Meaning |
| --- | --- | --- |
| Hello | 1 | Origin identifies the client; payload is the pairing code |
| Welcome | 2 | Version 1 selected; returned epoch and scene token bind the session |
| Register | 3 | Source UUID and client's last known base revision; optional collection name payload |
| Snapshot | 4 | Complete `MSLINK 1` manifest and transaction envelope |
| Delta | 5 | Ordered sparse `MSDELTA 1` operations and transaction envelope |
| ResourceAnnounce | 6 | SHA-256 resource ID and total byte length |
| ResourceChunk | 7 | Resource ID, byte offset and binary payload |
| ResourceComplete | 8 | Final exact-length and SHA-256 validation |
| Ack | 9 | Registration/resource success or accepted-in-Preview receipt |
| Reject | 10 | Specific diagnostic; prior document remains intact |
| Resync | 11 | Revision, epoch, scene, origin or delta-baseline disagreement |
| Status | 12 | Admission progress, or client status/keepalive request |
| ShotState | 13 | Accepted shot state; conditional revision polling |
| ShotClaim | 14 | Fifteen-second ownership claim for placement or visibility |
| ShotCommit | 15 | Validated property edit against its claimed revision |
| ShotRelease | 16 | Explicit gesture release |

Step 06 adds the [shot payload and ownership contract](shot-placement-v1.md).
ShotState responses can contain up to 1 MiB; other new controls retain the
4096-byte limit. Incoming authoring uses a dedicated undo context, preserving
local Scene Undo when source geometry arrives after a local shot gesture.

## Step 12 capability negotiation

The frame version stays 1. Optional wire features are named instead, so a
publisher and Director from different deliveries refuse the specific operation
rather than failing inside a record one side cannot parse.

Hello's payload keeps the pairing code on its first line and may add `key=value`
declarations on the lines after it: `capabilities=` (a comma list) and `client=`
(at most 96 printable bytes, shown in the Scene Link card). Unknown keys are
ignored; an unknown capability name is refused with `unsupported_capability:
<name>; supported=base,shot,stage,environment,materials,animation`. Welcome
carries this build's set as `capabilities=...` in its existing text.

`base` covers object/prototype/material records, snapshot, delta and resource
transfer, and is implicit. `shot` covers kinds 13–16, `stage` kind 17,
`environment` the `batch` object suffix and `lod` prototype suffix, `materials`
the `light` object suffix, and `animation` the `animation` object suffix. A Hello
that declares nothing negotiates `base,shot,stage` — what a publisher written
before negotiation could already send — so its step 09–11 suffixes are refused
with `capability_not_negotiated: <name>`. A Welcome without a capability list is
read as the same baseline by the add-on, which then refuses the named feature
locally before any bake, extraction or upload. A capability refusal preserves the
connection, retained resources and last accepted revision.

See [qualification v1](qualification-v1.md) for the delivered pair and the
support boundary.

Hello with another version returns `unsupported_version: supported=1`. A valid
Hello selects v1; subsequent messages must carry its epoch/scene and the same
origin. Register returns the Director's actual source revision in `revision`, empty
for a new source. A mismatched base receives Resync. Clients rebuild their candidate
against the returned revision before retrying. One source is registered per
connection; reconnect to change sources.

Step 07 allows Register to carry the collection's display name as a UTF-8 text
payload of at most 256 bytes, without line breaks or tabs. An empty payload
preserves an existing name. Names affect the UI only; UUIDs still determine
identity. The accepted source stores the name with its next revision.

A saved `link_paused` attachment gates source resource announcements and scene
transactions, including a recheck immediately before Director admission. The
specific rejection is `source_paused: resume_the_collection_then_publish`.
It preserves the connection, retained complete resources and last accepted
revision. Status/keepalive and shot controls remain available. The publisher
retains its evaluated edit and retries only after the operator resumes the
collection and explicitly publishes. This requires no new message kind/version.

## Resources and capacities

Announce a lowercase 64-digit SHA-256 and byte length, then send chunks at the
acknowledged offset. Identical retransmissions of fully received chunks are
verified against staged bytes; overlapping or different repeats fail. Repeating
an active identical announcement reports the current offset. Complete checks
length and incremental SHA-256 before renaming the pairing's private staging file
to `<hash>.glb` in the shared store. Duplicate Complete and announcements of
retained resources acknowledge success. Announcing content the store already holds
returns `resource_present` without transfer, but only after the worker hashes the
stored file again; damaged stored bytes are transferred and replaced.

Disconnected partial transfers are deleted and restart at offset zero. Completed
resources survive reconnect, re-pairing and Director restart. Prototype paths must
be exactly `<hash>.glb` matching the declared hash, and that resource, and an
authored LOD's, must be complete in this pairing or verified in the store.
No client path is used as a write destination. Absolute paths, traversal and
arbitrary resource dependencies are rejected. Step 03 then validates the complete
graph, transforms, materials and embedded static GLB contents before admission.

Limits are 64 completed resource hashes, 64 MiB per resource and 256 MiB total
resource bytes per pairing; stored resources reported present count as if they had
been uploaded. Chunks stream to disk. The importer separately limits
manifests to 1024 records, prototypes to 64 and candidate package/upload bytes to
256 MiB, with existing decoded image/geometry scratch limits. One worker, one
candidate mailbox and one resolution slot bound pending work. At most 256 final
Director receipts are retained per pairing; reaching capacity requires re-pairing
and a fresh snapshot rather than silently evicting replay history.

Storage defaults to `out/scene-link-resources` under the application working
directory. Every pairing, including the concurrent links of different scenes,
shares one content-addressed store, `shared/`, which grows only with distinct
content. Completed resources are `<hash>.glb` and the store is also the ingest
package cache: a stored resource is its own frozen copy and one package index
serves every pairing, so re-pairing a scene neither copies nor re-ingests it.
Staging files are named by the pairing's 16-digit epoch prefix, so concurrent
pairings never share one. The first pairing to complete a hash publishes it by
rename; a later one keeps that copy once it verifies. Ingest into the store is
serialized within the process, and indexes, LOD companions and packages appear
only by rename, so another process never reads a partial file.

Worker-side admission checks at pairing, resource announcement and scene staging
stop new work above 1.5 GiB held by the store; ingest refuses a new package
generation at 4096 indexes. One already admitted import can add its bounded
packages before the next check. Pairing directories written by earlier builds
(`<16 hex digits>/`) are neither counted nor touched, and projects saved with them
keep rendering from them. Earlier generations remain for undo and offline
playback; there is no automatic garbage collection of anything a project may
reference. Only staging files more than an hour old are removed, at pairing. Local
cache locations never serve as persistent object IDs.

## Snapshot, delta and Director admission

Transactions require a nonempty transaction ID, origin, source UUID and resulting
revision, plus the exact current base revision (empty for a new source). Resulting
revision must differ from base. Revisions are opaque source labels; publishers
should assign a fresh label for every distinct state. Payload source/revision
headers must match the envelope.

Snapshots use the [MSLINK 1 manifest](scene-manifest-v1.md), with hash filenames.
Sparse deltas use complete existing manifest records after `upsert`, or record kind
and UUID after `erase`:

```text
MSDELTA 1 "source-uuid" "new-revision"
upsert object "object-uuid" "Prop" "" "prototype-uuid" "" "origin-v1" 1 1 0 0 0 0 1 0 0 0 0 1 0 2 0 0 1
erase object "another-object-uuid"
```

Object, prototype and material records are supported. Up to 1024 operations apply
in order to the last accepted manifest, with at most 1024 resulting records.
Repeated replacements coalesce within the atomic delta. Erase remains an ordering
barrier; erasing an absent record fails. Omitted records remain. Deleted source
objects become existing recovery tombstones. The resulting full manifest is
validated and staged; unchanged resource packages are reused. No transaction is
coalesced across another transaction boundary.

Step 10 uses the existing material records and the manifest's optional typed
`light` object suffix. Material look and light-only changes therefore use the same
delta transaction with no new transport message; no mesh upload is needed for a
light edit. See [Materials and lighting v1](materials-and-lighting-v1.md).

The worker emits `Status(received)` then `Status(validated)`. The control thread
rechecks current project generation, Preview target, source revision, transitions,
attachment changes, removals and Program use. It plans against the current document,
preserving intervening shot placement/material/visibility overrides, and schedules
one existing Director document-replacement transaction in the incoming-authoring
undo context, separate from local Scene gestures.
Remote scheduler IDs use a separate high-bit namespace from local editor gestures.

Successful scheduling produces `Ack(accepted_in_preview;
published_to_program=false)`. This acknowledges the projected document, not GPU
readiness or frame-aligned Program adoption. Step 08 owns coordinated Program
publication. A source attached to Program is rejected to prevent indirect changes.
Network parsing, resource I/O, hashing and ingestion run only on the worker. The
control loop exchanges bounded pointers and applies ordinary document commands.

Final Director receipts are keyed by origin/source/transaction and stored before
sending. Exact packet replay returns the same receipt without mutation. Reusing
the identity with different bytes fails. Early validation rejections have no
document effect and can be retried after resources complete. Historic accepted
receipts stay historic after local Undo; Register/Status reports current revision.

## Reconnect and restart

Reconnect repeats Hello, obtains the epoch/scene, then registers source revision.
Revision agreement and an available matching manifest baseline permit deltas.
Stale revisions never overwrite implicitly. Local Undo or manual import can
invalidate the delta baseline and require a rebuilt snapshot.

Restart rotates the epoch and pairing and forgets receipts, resource registration
and delta baselines. The accepted document and shot overrides remain. Re-pair,
register, transfer needed resources and send a fresh snapshot with a new resulting
revision. Old-epoch messages fail. A changed project or target scene requires
re-pairing; opening a project stops the listener. Program mode disables admission.
Playback uses accepted immutable packages while the publisher is disconnected.

## Step 09 environment payloads

Snapshot and delta object rows may carry the explicit `batch` suffix, and prototype
rows may carry one explicit `lod` suffix, as defined in
[Scene manifest v1](scene-manifest-v1.md). Both base and low-detail GLBs must complete
the existing hashed resource transfer before admission. Density selection and
terrain clipping happen on the publisher; the wire carries final transforms and
chunk resources. The Director validates the complete expanded scene budget before
offering an atomic candidate to the command thread. No per-output regeneration or
new transport message type is introduced. Older implementations reject these
suffixes; a publisher must declare the `environment` capability, and the Director
and add-on ship as one delivery.

## Step 11 animation and transient references

Animation assemblies use the existing hashed GLB upload, with an explicit
`animation start_frame loop` suffix on their object row. Node/skin data remains
inside the immutable package. The new `StageContext` request/reply (kind 17)
delivers a bounded latest camera/lens/show-time sample without document commands
or revision changes. The [step 11 contract](animation-and-stage-v1.md) specifies
its fields, 10 Hz service ceiling, camera axes and reference limitations.
