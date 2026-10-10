# Qualification and support boundary v1

What the Blender live scene link is qualified to do, on what builds, and what it
does not do. Everything below was measured on the hardware named here; nothing
is inferred from a different scene size, machine or configuration.

## Delivered pair

| Part | Version | Notes |
| --- | --- | --- |
| Blender add-on | `mediaserver_livelink` **0.7.0** | `scripts/package-blender-livelink.py` writes the archive and its `manifest.json` |
| Authoring link protocol | **v1** with capability negotiation | Frame version stays 1; optional features are named, see [protocol v1](protocol-v1.md) |
| Blender | **5.2.0 LTS**, Windows build `fbe6228777e7` | `bl_info` requires 5.2.0; no other build is qualified |
| Media Server | this checkout, MSVC 14.44, Debug and Release GUI presets | Advanced D3D12 renderer for the GPU checks |

The add-on and the Director are **one delivery**. Install both from the same
checkout: the capability list below is how a mismatch is caught, not a promise
that mixed builds interoperate.

## Capability negotiation

Hello carries the pairing code on its first line, then optional declarations:

```text
<64-hex pairing code>
capabilities=base,stage,environment,materials,animation
client=mediaserver_livelink/0.10.0 blender/5.2.0
```

Welcome answers with this build's set:
`version=1; live_scene; one_inflight; idle_timeout_ms=30000; capabilities=base,stage,environment,materials,animation`.

| Capability | Covers | Refusal when absent |
| --- | --- | --- |
| `base` | Object/prototype/material records, snapshot and delta, resource transfer, writeback (ShotState, `absorb`) | Always negotiated; never refused |
| `stage` | Stage/camera reference samples (kind 14) | `capability_not_negotiated: stage` |
| `environment` | `batch` object suffix and `lod` prototype suffix (step 09) | `capability_not_negotiated: environment` |
| `materials` | `light` object suffix, native linked lights (step 10) | `capability_not_negotiated: materials` |
| `animation` | `animation` object suffix, node/skin assemblies (step 11) | `capability_not_negotiated: animation` |

Rules, enforced on both sides:

- An unknown capability name is refused at Hello: `unsupported_capability: <name>;
  supported=...`. The connection closes; no document is touched.
- A Hello with no `capabilities` line negotiates `base` alone. Its optional manifest
  suffixes are then refused by name instead of failing inside a record.
- A Welcome with no `capabilities` text is read by the add-on as `base`. Enabling **Animated assembly**, scatter groups,
  authored LODs or native lights against such a build is refused in Blender
  **before** any bake, extraction or upload, and the panel names the feature.
- A refusal is not a disconnect: the pairing, its retained resources and the last
  accepted revision survive, and the publisher can clear the feature and continue.

## Measured behaviour

Recorded on Windows 11 Pro 10.0.26200, AMD Ryzen 7 5800X, 64 GiB RAM, NVIDIA
GeForce RTX 3090 (driver 32.0.16.1656), local C: filesystem, loopback pairing.
Development machine with desktop activity present — not an isolated lab run.

Two independent measurements, never summed:

- **Director boundary** (`mediaserver_linked_qualification_tests`): first byte of
  a revision's transfer through the receipt the Director sends, broken into
  transfer, ingest/validation and Director admission. 32 accepted revisions per
  run; nearest-rank percentiles.
- **Publisher boundary** (installed add-on in factory Blender): first unpublished
  dependency event through the accepted receipt, split into the Blender share
  (coalescing, evaluation, extraction, GLB serialization) and the Director share.

| Measurement | Debug | Release | Scope |
| --- | ---: | ---: | --- |
| Director event-to-Preview p50 | 31 ms | 31 ms | 3 KB prop, 1-2 occurrences, loopback |
| Director event-to-Preview p95 | 143 ms | 128 ms | Includes the first cold ingest of each new resource |
| Ingest and validation p50 / p95 | 14 / 24 ms | 4 / 8 ms | Manifest merge, GLB qualification, immutable package, budget |
| Resource transfer p95 | 64 ms | 64 ms | 3 KB GLB, announce through verified completion, three acknowledged round trips |
| Director admission p95 / worst | 28 / 31 ms | 27 / 28 ms | Candidate offered to the command thread through receipt |
| Adoption stall p50 / worst | 6.3 / 9.1 ms | 0.7 / 1.5 ms | Plan + document transaction + scene evaluation on the control thread |
| Publisher event-to-Preview p50 / p95 | 313 / 471 ms | 308 / 409 ms | Installed add-on, textured cube, 9 samples, includes the 200 ms coalescing window |
| Publisher last revision, Blender / Director share | 211 / 61 ms | 211 / 42 ms | Coalescing, evaluation, extraction and GLB build against transfer, ingest and admission |
| Event-to-Program: stage / install | 23.2 / 4.8 ms | 6.3 / 0.4 ms | Freeze and verify the revision, then one Director transaction |
| Event-to-Program: GPU residency | 29.6 ms | 25.0 ms | Advanced D3D12, 2 submissions to a complete frame with no dropped instances |
| Peak pending bytes, one revision | 3,448 | 3,448 | Bytes staged for a revision before admission |
| Program reveal | 39 held frames, 1 revealed, 0 dropped | same | Two nodes, exact permit frame 40, show frame 7 |
| Retained per pairing | 6 resources / 18,440 bytes, 32 receipts | same | 32 accepted revisions in one pairing |

**These latencies are dominated by scheduler quantum, not by work.** The framed
stream retries a non-blocking read after `sleep_for(1ms)`, the link service polls
its resolution mailbox every 2 ms, and the harness polls the candidate mailbox
every 1 ms; on this machine a 1 ms sleep actually takes **15.9 ms** (measured, 50
samples); in playback the candidate is instead collected once per Director frame.
A three-round-trip resource transfer therefore costs about three quanta whatever
the payload is, which is why the Debug and Release transfer figures agree while
ingest, which contains no sleep, falls from 14 ms to 4 ms. Read the columns that way: ingest and adoption measure work;
transfer, admission and the totals measure work plus a small number of sleep
quanta. Raising the process timer resolution or waiting on the socket instead of
sleeping is the lever on the totals; it is a transport change and is **not** part
of this step.

Full reports: [step 12 Debug](evidence/step12-debug-qualification.json),
[step 12 Release](evidence/step12-release-qualification.json),
[Blender Debug](evidence/step12-blender-debug.json),
[Blender Release](evidence/step12-blender-release.json).

These are small-fixture, single-machine figures. The step 01
[baseline](baseline-v1.md) holds the export/upload/adoption numbers for the
larger 256-occurrence dataset. **Do not** present either as a stage-network
end-to-end latency budget.

### Detailed geometry (~30M triangles)

Measured on the same machine (RTX 3090, Release, Blender 5.2), one dense
single-material grid synced through the real Director service
(`test-blender-livelink.py --heavy`, `MS_HEAVY_SIDE` quads a side):

| Scene | First sync | Blender evaluate + copy | Upload | Director import | Placement-only edit afterwards |
| --- | --- | --- | --- | --- | --- |
| 4.0M triangles, 41 resources | 12.9 s | 3.1 s | 1.6 s | not separated | 0.09 s |
| 30.0M triangles, 301 resources | 92.8 s | 25.0 s | 11.3 s | 32.7 s | 0.38 s |

The first sync is a one-time import; moving or hiding the object afterwards sends
no geometry. Blender is busy only during its evaluate-and-copy step. The renderer
side (`mediaserver_render_geometry_benchmark`, 300 distinct ~100k-triangle parts,
1920x1080, Advanced, D3D12) streams the 29.8M-triangle set into the device arena in
about 4 s and then uploads nothing per frame. Frame time is the raster passes:

| Scene | 4 shadow cascades | No directional shadows |
| --- | --- | --- |
| 29.8M triangles | ~106 ms | ~37 ms |
| 9.9M triangles | ~48 ms | |
| 3.0M triangles | ~21 ms | |

So the link and renderer now hold a ~30M-triangle scene, but show-rate playback
at that density needs fewer triangles in view (authored LODs, culling) or fewer
shadow passes; geometry throughput, not the old copy budget, is now the limit.

## Declared limits

| Limit | Value | Where it is refused |
| --- | --- | --- |
| Manifest records | 4096 | `record_capacity` |
| Delta operations | 4096 per transaction | `record_capacity` |
| Prototypes per scene | 1024 | `Prototype count exceeds 1024` |
| Instances per scene, including scatter | 4096 | publisher budget, then `measure_environment` |
| Resource bytes | 64 MiB per resource, 16 GiB per pairing | `invalid_resource_size_or_hash`, `resource_capacity` |
| Completed resources per pairing | 4096 | `resource_capacity` (measured: the 4097th announcement is refused) |
| Retained receipts per pairing | 256 | `receipt_capacity: pair_again_and_resync` |
| Resource cache | 64 GiB in the shared store, 65536 package generations (128 per standalone manifest directory) | `resource_cache_full`, `Linked resource cache is full` |
| Program storage | 64 GiB / 16384 files, 16 retained revisions | `Program storage capacity refused`, `Publication history capacity refused` |
| Copied scene bytes in Blender | 8 GiB evaluated arrays per scene, 4 GiB per mesh, split into 300k-corner resources | publisher `ValueError` before any transfer |
| Scene geometry | 32 M vertices, 96 M indices, 32 M triangles (renderer arena 48 M / 144 M) | `measure_environment` before admission |
| Node capacity | 48 M vertices, 144 M indices (old and new revision together), 16 GiB, 16384 nodes per publication | `capacity refused` before Program adoption |
| Shot claim | 15 s | `shot_claim_expired`, `shot_gesture_already_claimed` |
| Stage samples | 10 Hz ceiling, latest sample only | empty `StageContext` reply |

## Qualified scenarios

Each of these runs in `mediaserver_linked_qualification_tests` (Debug and
Release) unless marked Blender, which runs the installed add-on against the real
Director fixture through `scripts/test-blender-livelink.py --qualification`.

Rename, duplicate, delete, undo, reparent, origin change, resync after undo,
interrupted transfer, reconnect, malformed input, a 24-revision session with
repeated resource replacement, cancellation, Director restart, save and reopen,
resource capacity, two-node publication with controlled faults, node restart
(the published document is saved without its runtime permit, reopened, and
re-verified from its own manifest), and Advanced D3D12 rendering of the published
revision (`--gpu`). Blender: verified install metadata, capability negotiation,
reparent, origin change, repeated texture replacement, a 30-edit session, refused
unrepresentable input, recovery, disconnect during an edit,
save/reopen/reconnect, in-place upgrade, complete removal.

Controlled faults in the publication path: a withheld manifest (a second node
refuses an incomplete transfer), a node below capacity
(`PublicationLimits` refusal before adoption), an incomplete cohort, and a
disconnected required node. Each holds the reveal; none shows a partial revision.

**Cache eviction is a refusal, not an eviction.** Nothing a project may reference
is deleted automatically: at 4096 completed resources, 16 GiB of pairing bytes,
64 GiB in the shared store or 65536 package generations, new work is refused by
name and the existing revisions stay usable for undo and offline playback. The
pairing limits recover by re-pairing, which reuses stored content without
transfer. The store grows only with distinct content; see `resource_cache_full`
in the [operator guide](operator-guide.md). The measured run confirms the 65th
resource announcement is refused with the pairing holding exactly 64.

## Not supported

- **Not qualified on any Blender build other than 5.2.0 LTS `fbe6228777e7`.**
  A different build may work; it has not been measured.
- No LAN or TLS transport, and no authoring across machines. The pairing is
  loopback-only and is never saved or sent to output nodes.
- No Program-side authoring: a source attached to Program is refused
  (`source_used_by_program_step08_required`). Publication is step 08's explicit,
  frame-aligned path.
- Mixed builds are refused by capability, not made to interoperate.
- Unsupported Blender content: vertex caches and shape keys, preserve-volume
  deformation, reflected or sheared sampled transforms, non-square/rectangle area
  lights, procedural base colour without an explicit bake, images not saved to
  disk, more than four vertex-group influences, simulations and constraints
  outside the evaluated sampled set. Each produces a specific diagnostic; see
  [materials](materials-and-lighting-v1.md) and [animation](animation-and-stage-v1.md).
- No physical two-machine scanout, genlock, tracker-hardware calibration or lens
  distortion agreement is claimed. The Blender reference viewport is a reference
  gate only; final calibrated output stays in Media Server.
- No automatic garbage collection of resource caches or Program storage. Retire
  directories by hand once no project references them.
- The measured figures cover the fixtures named above. Large unique meshes,
  large texture sets, foliage at stage scale and cold-start exporter costs are
  **not** covered by a latency claim.

## Reproduce

```powershell
cmake --build --preset windows-msvc-debug-gui
cmake --build --preset windows-msvc-release-gui
ctest --preset windows-msvc-debug-gui --output-on-failure -R "linked_qualification|director_link|linked_scene_ingest|linked_publication|blender_publisher_unit|scene_linked|linked_animation_stage|linked_look|linked_environment|editor"
ctest --preset windows-msvc-release-gui --output-on-failure -R "linked_qualification|director_link|linked_scene_ingest|linked_publication|blender_publisher_unit|scene_linked|linked_animation_stage|linked_look|linked_environment|editor"
powershell -ExecutionPolicy Bypass -File scripts/run-blender-qualification.ps1 -Build release -Gpu
python scripts/test-blender-livelink.py --build debug --qualification --output out/step12-blender-debug
python scripts/test-blender-livelink.py --build release --qualification --output out/step12-blender-release
# Detailed geometry: end to end through the Director, and the renderer alone.
$env:MS_HEAVY_SIDE = '3873'; python scripts/test-blender-livelink.py --build release --heavy
out/build/windows-msvc-release-gui/engine/render/mediaserver_render_geometry_benchmark.exe 300 223 4
```

`run-blender-qualification.ps1` writes the delivery bundle: the add-on archive
and manifest, `evidence/qualification.json`, the offline `preview.mstage` and
`published.mstage` projects, and `sample-collection.blend`. Keep the output
directory path short — immutable package names must stay inside the legacy
Windows path limit. See the [operator guide](operator-guide.md) and the
[step 12 walkthrough](ui-checks-step-12.md).
