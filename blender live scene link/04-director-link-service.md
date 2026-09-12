# 04 — Director link service

Status: implemented (2026-09-09). Depends on: 02; uses step 03 ingestion.

Use **File > Start Scene Link** in Preview, then **File > Scene Link Details...**.
See the [wire contract](protocol-v1.md) and [UI walkthrough](ui-checks-step-04.md).

## Outcome

A bounded application-neutral connection between authoring clients and the Director.

## Work

- Choose a framed transport after reviewing existing networking seams. Keep small
  control messages separate from bulk resource delivery even if they share a socket.
- Define handshake/version negotiation, source registration, snapshot, delta,
  resource announcement, resource completion, acknowledgement, rejection, and resync.
- Include session epoch, transaction ID, origin, base revision, and resulting
  revision. Replayed or duplicated messages must be idempotent.
- The Director orders accepted mutations and applies existing document commands.
  Render nodes consume Director-authorized state, not Blender messages.
- Require an explicit local pairing or authenticated connection. Validate message
  lengths, counts, paths, hierarchy cycles, and resource limits before admission.
- Separate received, validated, accepted-in-Preview, and published-to-Program status.
- Bound queues and apply backpressure. Coalesce replaceable intermediate updates
  without losing create/delete ordering or transaction boundaries.
- Reconnect using revision agreement or a fresh snapshot, then reapply retained
  shot overrides. Never assume a socket reconnect means identical scene state.

## Acceptance

Exercise duplicated messages, interrupted resource transfer, stale revisions,
unsupported protocol versions, restart, and reconnect to the wrong scene. Each
case either converges to an acknowledged revision or produces a clear rejection.
Network and source parsing work must not block the frame path.

## Implementation

The versioned protocol reuses `TcpFramedStream` on a separate, explicitly paired
loopback listener. One worker stages hash-addressed GLB resources and complete
snapshot/sparse-delta candidates. A bounded mailbox passes validated manifests to
the Director for one Preview transaction with retained shot overrides. Received,
validated and accepted-in-Preview statuses are separate; Program publication is
explicitly false until step 08 supplies that workflow.

Exact transaction replay and repeated resource messages are idempotent. Revision,
source, epoch and scene agreement gate reconnect. Stop cancels I/O and ingestion
without waiting on the frame path. Restart requires new pairing and a snapshot;
accepted state stays playable. The in-app details panel remains responsive while
admission continues, with Copy details, Stop and Close controls.

The loopback suite covers malformed framing, version/pairing rejection, resource
limits, interrupted transfers, duplicate resource messages, atomic snapshots and
sparse deltas, replay, stale/wrong-scene rejection, paths and cycles, restart,
retained shot overrides, ordered deletion/restoration and Scene Undo. Build/test
logs and native UI observations are recorded in the walkthrough.
