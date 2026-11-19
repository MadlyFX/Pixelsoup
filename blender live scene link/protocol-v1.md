# Director authoring link protocol v1

Implemented by `net/authoring_link_protocol` and `render/director_link_service`.
The reference client is `scripts/scene-link-client.py`; the Blender add-on in
`integrations/blender/mediaserver_livelink` is the production client.

## Transport and pairing

The Director starts one loopback IPv4 TCP listener per linked scene when Blender
opens it. The OS selects a port, and each listener has a 256-bit OS-random pairing
code that the Director hands the add-on through same-user local discovery (below).
Closing the link cancels I/O and ingestion and revokes the pairing; opening it
again generates a new code, epoch and scene token. Pairing is ephemeral, never saved in a project or sent to
output nodes. There is no LAN or TLS mode in v1. Local clients with the code are
authorized to edit that scene.

The link uses `TcpFramedStream` for its bounded complete records, size-prefix
validation, deadlines and poisoned-stream behavior. Authoring uses a separate
listener and protocol; it cannot submit node replication commands or touch GPU
state. `interrupt()` wakes I/O without joining from the control loop.

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
| Ack | 9 | Registration/resource success or accepted receipt |
| Reject | 10 | Specific diagnostic; prior document remains intact |
| Resync | 11 | Revision, epoch, scene, origin or delta-baseline disagreement |
| Status | 12 | Admission progress, or client status/keepalive request |
| ShotState | 13 | Media Server's overrides on the source's occurrences (`MSSHOT 2`, see Writeback); conditional revision polling |
| StageContext | 14 | Latest camera/lens/show-time sample |

ShotState responses can contain up to 1 MiB; other controls retain the 4096-byte
limit. Incoming authoring uses a dedicated undo context, preserving local Scene Undo
when source geometry arrives after a local gesture.

## Capability negotiation

The frame version stays 1. Optional wire features are named instead, so a
publisher and Director from different deliveries refuse the specific operation
rather than failing inside a record one side cannot parse.

Hello's payload keeps the pairing code on its first line and may add `key=value`
declarations on the lines after it: `capabilities=` (a comma list) and `client=`
(at most 96 printable bytes, shown in the Scene Link card). Unknown keys are
ignored; an unknown capability name is refused with `unsupported_capability:
<name>; supported=base,stage,environment,materials,animation`. Welcome
carries this build's set as `capabilities=...` in its existing text.

`base` covers object/prototype/material records, snapshot, delta, resource
transfer and writeback (ShotState and `absorb` lines), and is always negotiated.
`stage` covers kind 14, `environment` the `batch` object suffix and `lod`
prototype suffix, `materials` the `light` object suffix, and `animation` the
`animation` object suffix. A Hello that declares nothing negotiates `base` alone,
so its optional suffixes are refused with `capability_not_negotiated: <name>`. A
Welcome without a capability list is read as `base` by the add-on, which then
refuses the named feature locally before any bake, extraction or upload. A
capability refusal preserves the connection, retained resources and last accepted
revision.

See [qualification v1](qualification-v1.md) for the delivered pair and the
support boundary.

Hello with another version returns `unsupported_version: supported=1`. A valid
Hello selects v1; subsequent messages must carry its epoch/scene and the same
origin. Register returns the Director's actual source revision in `revision`, empty
for a new source. A mismatched base receives Resync. Clients rebuild their candidate
against the returned revision before retrying. One source is registered per
connection; reconnect to change sources.

Register may carry the collection's display name as a UTF-8 text
payload of at most 256 bytes, without line breaks or tabs. An empty payload
preserves an existing name. Names affect the UI only; UUIDs still determine
identity. The accepted source stores the name with its next revision.

A saved `link_paused` attachment gates source resource announcements and scene
transactions, including a recheck immediately before Director admission. The
specific rejection is `source_paused: resume_the_collection_then_publish`.
It preserves the connection, retained complete resources and last accepted
revision. Status/keepalive and ShotState remain available. The publisher
retains its evaluated edit; when the operator resumes the collection the
Director's Status reports `source_resumed` and the publisher sends the retained
edit.

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
arbitrary resource dependencies are rejected. Ingestion then validates the complete
graph, transforms, materials and embedded static GLB contents before admission.

Limits are 4096 completed resource hashes, 64 MiB per resource and 16 GiB total
resource bytes per pairing; stored resources reported present count as if they had
been uploaded. Chunks stream to disk. The importer separately limits
manifests to 4096 records, prototypes to 1024 and candidate package/upload bytes to
16 GiB, with existing decoded image/geometry scratch limits per resource. These fit a
~30M-triangle scene, which arrives as a few hundred bounded geometry parts. One worker, one
candidate mailbox and one resolution slot bound pending work. At most 256 final
Director receipts are retained per pairing; reaching capacity requires re-pairing
and a fresh snapshot rather than silently evicting replay history.

Storage is `<project> Blender/Link Resources` beside the saved project; an
unsaved project uses `Documents/Media Server/Blender/Link Resources`. Every
pairing, including the concurrent links of different scenes,
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
stop new work above 64 GiB held by the store; ingest refuses a new package
generation at 65536 indexes. One already admitted import can add its bounded
packages before the next check. Earlier generations remain for undo and offline
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

Material look and lights use the material records and the manifest's optional typed
`light` object suffix. Material look and light-only changes therefore use the same
delta transaction with no new transport message; no mesh upload is needed for a
light edit. See [Materials and lighting v1](materials-and-lighting-v1.md).

The worker emits `Status(received)` then `Status(validated)`. The control thread
rechecks current project generation, target scene, source revision and attachment
changes, and holds the revision while its scene is in a running dissolve. It plans
against the current document, preserving intervening shot placement/material/
visibility overrides, and schedules one existing Director document-replacement
transaction in the incoming-authoring undo context, separate from local Scene
gestures. Remote scheduler IDs use a separate high-bit namespace from local editor
gestures and are numbered by the Director across every link.

The target may be on Program: a link edits its scene wherever it is. A frozen
published revision is a separate, unlinked scene (see
[Program publication](program-publication-v1.md)). Successful scheduling
produces `Ack(accepted; live)`. This acknowledges the projected document, not GPU
readiness: an asset change still reaches Program through the existing asset cohort
permit, revealed by every node on one frame. Network parsing, resource I/O, hashing
and ingestion run only on the worker; an immutable file already hashed in the process,
unchanged in size and time, is not hashed again, so placement-only revisions read no
geometry. The control loop exchanges bounded pointers and applies ordinary document
commands.

## Writeback

Media Server's edits to linked occurrences remain overrides on the occurrence. The
publisher shows them in its own objects and keeps sending its own source values
underneath:

- `ShotState` returns `MSSHOT 2`, then one line per occurrence of the source in the
  target scene (at most 512): node, attachment, object uuid, held, visibility override
  (-1 none, 0 hidden, 1 shown), effective visibility, removed (by the source), deleted
  (in Media Server, itself or through an ancestor), the offset, held-world,
  attachment-alignment and world matrices (16 column-major values each), then three
  quoted fields - light overrides and material overrides as `key=value,...`, and the
  uuid of the object's source parent. The reply's `revision` hashes the text; a request
  whose `base` equals it gets an empty payload. A mesh split into parts by material is
  one Blender object; its single part's overrides apply to it.
- A deleted object is unlinked from the Blender scene's collections but kept, so the
  next revision removes the source object and the Director keeps both records. If
  Media Server's Undo clears the deletion the add-on links the object back; if the
  artist brings it back (Blender's Undo) the revision absorbs `deleted`.
- A snapshot or delta may carry
  `absorb "<object uuid>" <placement|visibility|light|material|deleted>` lines. They
  name objects whose Media Server override the artist has just replaced in Blender.
  They are not retained rows: the Director plans the revision, then returns that
  property of the object's occurrence in the target scene to the source, in the same
  transaction. An object with two attachments in the scene is not absorbed.

Objects removed by the source or deleted in Media Server are not listed in the editor.
Their records stay in the document so Undo on either side restores them with every
Media Server override.

## One Blender, many scenes (discovery)

The add-on holds a project's scenes in one `.blend` and tags each Blender scene with
its Media Server scene's source identity (`ms_scene_source`). Its instance record,
`<id>.instance` in `%LOCALAPPDATA%/MediaServer/blender`, is `MSBLENDER 2` followed by
ten lines: label, the scene shown, status, `ready` or `resync`, the open file, the
launch token Open in Blender gave the process, its process id, the source of the scene
it shows, a comma-separated list of every tagged scene in the file, and an optional
`<source>\t<name>` asking Media Server to create a scene for an untagged Blender scene.
Media Server answers with:

- `<id>.links` - `MSLINKS 1` then `<source>\t<port>\t<pairing code>\t<name>` per linked
  scene, replacing the previous list (empty unlinks everything). The add-on pairs with
  the scene it shows and switches pairing when Blender switches scenes.
- `<id>.activate` - `MSACTIVATE 1`, a source, a name and `show` or `adopt`: show that
  scene, adopting an untagged or orphaned scene of that name, else creating one.
  `adopt` (File > Connect to Blender...) tags the scene Blender shows now instead.

Both files are consumed.

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
re-pairing; opening a project stops the listener. A frozen published revision
refuses admission.
Playback uses accepted immutable packages while the publisher is disconnected.

## Environment payloads

Snapshot and delta object rows may carry the explicit `batch` suffix, and prototype
rows may carry one explicit `lod` suffix, as defined in
[Scene manifest v1](scene-manifest-v1.md). Both base and low-detail GLBs must complete
the existing hashed resource transfer before admission. Density selection and
terrain clipping happen on the publisher; the wire carries final transforms and
chunk resources. The Director validates the complete expanded scene budget before
offering an atomic candidate to the command thread. No per-output regeneration or
new transport message type is introduced. A publisher must declare the `environment` capability to use these suffixes,
and the Director and add-on ship as one delivery.

## Animation and transient references

Animation assemblies use the existing hashed GLB upload, with an explicit
`animation start_frame loop` suffix on their object row. Node/skin data remains
inside the immutable package. The `StageContext` request/reply (kind 14)
delivers a bounded latest camera/lens/show-time sample without document commands
or revision changes. The [animation contract](animation-and-stage-v1.md) specifies
its fields, 10 Hz service ceiling, camera axes and reference limitations.
