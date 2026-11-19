# Qualification and support boundary v1

What the Blender live scene link is qualified to do, on what builds, and what it
does not do. The workflow is in the [add-on README](../integrations/blender/README.md),
[operator guide](operator-guide.md) and [protocol](protocol-v1.md). Everything
below was measured on the hardware named here; nothing is inferred from a
different scene size, machine or configuration. The latency tables were recorded
with add-on 0.7.0 and its explicit freeze-and-publish path; rerun the checks under
[Reproduce](#reproduce) for a new build or hardware setup.

## Supported pair

| Part | Version | Notes |
| --- | --- | --- |
| Blender add-on | `mediaserver_livelink` (version in `bl_info`, currently **0.10.1**) | `scripts/package-blender-livelink.py` writes the archive and its `manifest.json` |
| Authoring link protocol | **v1** with capability negotiation | Frame version stays 1; optional features are named, see [protocol v1](protocol-v1.md) |
| Blender | **5.2.0 LTS**, Windows build `fbe6228777e7` | `bl_info` requires 5.2.0; no other build is qualified |
| Media Server | this checkout, MSVC 14.44, Debug and Release GUI presets | Advanced D3D12 renderer for the GPU checks |

The add-on and the Director are **one pair**. Use both from the same checkout:
the capability list below is how a mismatch is caught, not a promise that mixed
builds interoperate.

## Capability negotiation

Hello carries the pairing code on its first line, then optional declarations:

```text
<64-hex pairing code>
capabilities=base,stage,environment,materials,animation
client=mediaserver_livelink/<add-on version> blender/5.2.0
```

Welcome answers with this build's set:
`version=1; live_scene; one_inflight; idle_timeout_ms=30000; capabilities=base,stage,environment,materials,animation`.

| Capability | Covers | Refusal when absent |
| --- | --- | --- |
| `base` | Object/prototype/material records, snapshot and delta, resource transfer, writeback (ShotState, `absorb`) | Always negotiated; never refused |
| `stage` | Stage/camera reference samples (kind 14) | `capability_not_negotiated: stage` |
| `environment` | `batch` object suffix and `lod` prototype suffix | `capability_not_negotiated: environment` |
| `materials` | `light` object suffix, native linked lights | `capability_not_negotiated: materials` |
| `animation` | `animation` object suffix, node/skin assemblies | `capability_not_negotiated: animation` |

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
sleeping would lower the totals; the transport does neither.

Full reports (recorded with add-on 0.7.0): [Director Debug](evidence/director-debug-qualification.json),
[Director Release](evidence/director-release-qualification.json),
[Blender Debug](evidence/blender-addon-debug.json),
[Blender Release](evidence/blender-addon-release.json).

These are small-fixture, single-machine figures. The
[reference fixture measurements](reference-fixture-measurements.md) hold the export/upload/adoption numbers for the
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
all in view, 1920x1080, Advanced, D3D12, GPU timestamps per frame) streams the
29.8M-triangle set into the device arena in about 4 s and then uploads nothing per
frame. Each cell reads "before the show-rate work -> after"; "held" is a static camera, "moving"
orbits it every show frame:

| Scene | Cascades | Held camera | Moving camera |
| --- | --- | --- | --- |
| 29.8M triangles | 4 | 109 ms → **25.9 ms** | 112.9 ms → **30.0 ms** |
| 29.8M triangles | 2 (project default) | 90.1 ms → **21.2 ms** | 91.4 ms → **23.6 ms** |
| 29.8M triangles | none | 36.3 ms → **10.1 ms** | |
| 9.8M triangles | 4 | 51.9 ms → **22.3 ms** | |
| 2.9M triangles | 4 | 30.9 ms → **17.3 ms** | |

None of these changes alters a pixel. The benchmark's `digest` mode hashes the
colour, motion, identity, normal and cascade products over held, orbiting,
moving-part and relit sequences, with and without cascades, and every hash
matches the build before the work:

- **Indexed draws** (native D3D12). Batched opaque passes feed the arena's index
  stream to the input assembler, so a vertex shared by several triangles is shaded
  once. Every geometry pass roughly halved (109 → 56 ms on its own).
- **Shadow maps draw only what reaches them, and are kept.** Each cascade and
  spot tile skips casters wholly beyond its side planes, and a map is reused while
  a digest of everything its draw reads is unchanged. A held camera redraws no
  shadow map; a moving one redraws only the near cascades (3.7 ms at 29.8M), since
  the full-scene cascade and the reflective shadow map do not follow the camera.
  A moving caster redraws the maps it is in.
- **One auxiliary pass.** Identity, motion and normals are written together with
  three render targets: 10.9 → 5.1 ms after indexed draws (26.3 ms before
  either).
- **Lit stage.** The shadow kernel's rows and the bounce samples are loops rather
  than fully unrolled (the stage was too large to keep pixels in flight), and a
  point fully lit or fully shadowed in its nearest cascade skips the farther ones,
  which is exact for those values. 25.5 → 20.8 ms at four cascades after indexed
  draws (36.9 ms before either); a set-like layout (flat ground, 120 upright
  casters) gains a further 18% in this stage.

What is left is mostly per-pixel lighting: at 29.8M the lit pass is ~21 ms with
four cascades and ~16 ms with two, of which geometry is ~5 ms and the bounce term
~6 ms; the auxiliary pass is ~5 ms. Show rate (50 fps, 20 ms) is reached at
2.9M triangles with four cascades and is within 6% at 29.8M with the default two;
four cascades at 29.8M run at ~39 fps held and ~33 fps moving. Fewer triangles in
view (authored LODs) or fewer cascades are the levers beyond this.

**Geometry memory is per device, not per view.** The 29.8M-triangle set occupies
a 1,154 MiB arena (1,031 MiB of meshes). Every renderer on one device with the same
arena limits shares one arena: the editor's Program, transition and stage
views, their Basic and Advanced halves and each planar-reflection child draw the
same resident meshes instead of each streaming its own copy. A mesh's slots are
reused only when no renderer holds it and no recording that read or wrote it is
pending.

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
python scripts/test-blender-livelink.py --build debug --qualification --output out/blender-qualification-debug
python scripts/test-blender-livelink.py --build release --qualification --output out/blender-qualification-release
# Detailed geometry: end to end through the Director, and the renderer alone.
$env:MS_HEAVY_SIDE = '3873'; python scripts/test-blender-livelink.py --build release --heavy
out/build/windows-msvc-release-gui/engine/render/mediaserver_render_geometry_benchmark.exe 300 223 4
# Moving camera, and the image hashes that must match across builds.
out/build/windows-msvc-release-gui/engine/render/mediaserver_render_geometry_benchmark.exe 300 223 4 orbit
out/build/windows-msvc-release-gui/engine/render/mediaserver_render_geometry_benchmark.exe 300 223 4 digest
```

Each Blender output directory must be new and its path short: immutable package
names must stay inside the Windows path limit.

`mediaserver_linked_qualification_tests` drives the real link service, ingest
planner, command scheduler and publication path through 17 scenarios (18 with
`--gpu`): first publication, rename, duplicate, reparent, origin change, delete,
undo, resync after undo, interrupted transfer, reconnect, malformed input, a
24-revision session with repeated resource replacement, cancellation, Director
restart, save and reopen, resource capacity, two-node publication with controlled
faults, and Advanced D3D12 rendering of the published revision. It writes
`qualification.json` with the counters and percentiles.

The installed add-on adds 20 recorded checks against the real Director fixture:
verified install metadata, capability negotiation, reparent, origin change, three
texture replacements, a 30-edit session in batches, a refused unrepresentable
name, explicit recovery, disconnect during an edit, save/reopen/reconnect,
in-place upgrade and complete removal. Its `results.json` records the measured
event-to-Preview percentiles and the Blender/Director split of the last revision.
The Python unit and Blender suites listed in the
[add-on README](../integrations/blender/README.md#upgrade-and-verification) cover
the add-on's day-to-day behavior.

### Delivery bundle

```powershell
powershell -ExecutionPolicy Bypass -File scripts/run-blender-qualification.ps1 -Build release -Gpu
```

One command packages the add-on, runs the qualification harness, writes the
sample Blender scene and two sample projects, prints the bundle, and starts a
Director editor on the sample Preview project with isolated local settings.
`-NoLaunch` builds the bundle only; `-NoBlender` skips the sample scene; `-Gpu`
also renders the published revision through the Advanced D3D12 renderer.

```text
mediaserver_livelink.zip                 installable add-on
mediaserver_livelink.manifest.json       version, protocol, capabilities, file hashes
sample-collection.blend                  sample collection ready to link
evidence/preview.mstage                  offline Preview with accepted linked content
evidence/published.mstage                the same content frozen to Program
evidence/qualification.json              the measured report for this machine
```

Open `evidence/preview.mstage` with **File > Open Project** and click **PREVIEW**:
the **Blender Preview** scene holds the two accepted occurrences the harness
published, **Renamed hero** (held placement, X 2) and **Duplicate**, with no
Blender process involved. Open `evidence/published.mstage` and click **PROGRAM**:
the frozen revision renders from its own immutable packages, so a take runs with
Blender absent. The **Freeze Blender** dialog (**File > Freeze Blender...**) shows
**NEGOTIATED CAPABILITIES AND MEASURED LINK**: the publisher build, negotiated and
served capabilities, accepted/refused/resync counts, event-to-Preview p50/p95, the
last revision's transfer/ingest/Director split and peak pending bytes. To see a
mismatch without a second build, publish with the reference client, which
declares only `base`:

```powershell
python scripts/scene-link-client.py <manifest.mslink> --port <port> --pairing-code <code>
```

The Blender sidebar's **Compatibility and evidence** box shows the add-on version,
protocol, Blender build, the capabilities the Director serves (and any it does not
serve, with an error icon) and the edit-to-Media-Server percentiles. A genuine
mismatch is refused by name before any bake or upload without dropping the
pairing; `tests/blender/test_publisher_core.py` asserts that wording and
`mediaserver_director_link_service_tests` covers the Director-side refusals. The
installed add-on folder contains `manifest.json`; compare it with
`mediaserver_livelink.manifest.json` from the bundle to prove which build is on a
machine. Every file carries a SHA-256.

This is local process, resource and GPU qualification on one machine. It does not
claim physical two-machine scanout, genlock, tracker-hardware calibration or lens
agreement between Blender's reference viewport and calibrated output.
