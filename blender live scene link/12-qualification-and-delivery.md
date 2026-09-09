# 12 — Qualification and delivery

Status: proposed. Depends on: 09, 10, 11.

## Outcome

A packaged feature with reproducible performance evidence and a declared support boundary.

## Work

- Package the Blender add-on with version/compatibility metadata, installation,
  pairing, upgrade, removal, and recovery instructions. Include protocol capability
  negotiation so mismatched builds refuse unsupported operations clearly.
- Run the fixtures through rename, duplicate, delete, undo, reparent, origin change,
  save/reopen, reconnect, and Director/node restart scenarios.
- Measure event-to-Preview and event-to-Program latency separately. Break down
  Blender evaluation/extraction, transfer, ingest, GPU upload, and frame scheduling.
- Record median and tail latency, peak pending bytes, retained revisions, UI stall
  duration, and dropped/held frames on specified hardware and scene sizes.
- Exercise long sculpting sessions, repeated texture replacement, cancellation,
  malformed input, cache eviction, insufficient residency, and interrupted transfer.
- Test two-node publication with controlled faults and verify revision/frame evidence.
- Build debug and release, run relevant scene/persistence/protocol/renderer tests,
  and execute the real Blender-to-Advanced-renderer workflow on supported hardware.
- Deliver a sample blend file and saved Media Server project plus a concise operator
  guide for linking, placement policies, publishing, disconnecting, and rebaking.

## Acceptance

Publish measured limits and known unsupported features. The first-loop scenario
works after a clean install and after restart; a take continues with Blender absent.
Do not declare production readiness from a single-machine happy-path demonstration.
