# 12 — Qualification and delivery

Status: implemented. Depends on: 09, 10, 11.

## Outcome

A packaged feature with reproducible performance evidence and a declared support
boundary.

Delivered as add-on **0.7.0** with link-protocol capability negotiation, a
packaged archive carrying verifiable compatibility metadata, a scenario/fault
harness that records its own measurements, and a Scene Link card that shows the
negotiated capabilities and measured link. The
[qualification and support boundary](qualification-v1.md) publishes the measured
figures, declared limits and unsupported features. The
[operator guide](operator-guide.md) is the procedure, and the
[step 12 walkthrough](ui-checks-step-12.md) reproduces every check.

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

## Outcome as delivered

- `scripts/package-blender-livelink.py` writes the archive plus `manifest.json`
  (version, link protocol, capability list, pinned Blender build, install/upgrade/
  removal/recovery notes, per-file SHA-256), and refuses to package an add-on
  whose `bl_info` disagrees with its wire constants.
- Capability negotiation is part of Hello/Welcome. Both sides refuse the specific
  unsupported feature — before any bake, extraction or upload on the publisher,
  and before ingestion on the Director — without dropping the pairing.
- `mediaserver_linked_qualification_tests` covers 17 scenarios (18 with `--gpu`)
  and writes its own report; the installed add-on adds 13 checks against the real
  Director fixture, including in-place upgrade and complete removal.
- Latency is recorded at two independent boundaries and never summed. The
  measured figures, and the scheduler-quantum term that dominates them, are in
  [qualification v1](qualification-v1.md).
- `scripts/run-blender-qualification.ps1` produces the delivery bundle: add-on
  archive and manifest, qualification report, offline Preview and published
  Program projects, and a sample Blender collection.
- **Not claimed:** physical two-machine scanout, genlock, tracker-hardware
  calibration, any Blender build other than 5.2.0 LTS `fbe6228777e7`, LAN or TLS
  authoring transport, or a latency budget for scenes larger than the measured
  fixtures.
