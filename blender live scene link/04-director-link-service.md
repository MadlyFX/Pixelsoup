# 04 — Director link service

Status: proposed. Depends on: 02.

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
