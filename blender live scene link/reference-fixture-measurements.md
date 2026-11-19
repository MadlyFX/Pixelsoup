# Reference fixture measurements

Measured 2026-09-09 with the add-on of that date; later add-on versions are not re-measured here. Blender 5.2.0 LTS `fbe6228777e7`; Windows 11 Pro
10.0.26200; AMD Ryzen 7 5800X, 64 GiB installed RAM; NVIDIA GeForce RTX 3090,
Windows driver 32.0.16.1656. Pixelsoup MSVC 14.44, GUI Release build,
native D3D12 for the upload benchmark. Editor inspection used the Debug build's
Advanced renderer. Local C: filesystem, same host, no asset network transport;
the isolated editor Director listened on loopback. No remote render-node or
network-speed qualification is implied. Desktop activity was present and builds
had just completed; these are development-machine observations, not an isolated lab run.

## Datasets

| Dataset | Source bytes | Geometry after flattened import | Material runs | Images | Upload bytes |
| --- | ---: | --- | ---: | ---: | ---: |
| `reference-r2.glb` | 5,052 | 76 vertices / 38 triangles; 3 visible prop occurrences + floor | 7 | 0 | 2,888 |
| `workload-256.glb` | 17,564 | 6,144 vertices / 3,072 triangles; 256 occurrences of one mesh | 512 | 0 | 233,472 |

The first scene also has a transform parent, camera and hidden occurrence. Its
three unique materials become seven imported part/material runs. The grid has
two unique materials, expanded to 512 runs by the importer. GLB stores
the shared mesh once; the importer bakes and expands its occurrences.
The importer keeps prototypes separate from occurrence placement. The grid is a
small throughput probe, not a foliage test.

## Timing boundaries and results

20 samples per phase, milliseconds, nearest-rank percentiles. Blender's first
reference export is recorded separately at **569.83 ms**, including lazy
exporter initialization. The reference export percentile sample follows that
initialization. No samples are discarded from the C++ measurements; first upload
and first package creation can therefore influence their tails. Repeated file
reads use the OS cache; existing content-addressed package reuse is included.

| Phase | Reference p50 | Reference p95 | 256 grid p50 | 256 grid p95 |
| --- | ---: | ---: | ---: | ---: |
| Manual-export equivalent (Blender operator) | 11.983 | 15.260 | 64.897 | 102.827 |
| Ingest and immutable package | 2.974 | 3.661 | 19.026 | 20.317 |
| Geometry upload through completed GPU fence | 0.309 | 0.632 | 0.364 | 0.763 |
| Document delta + scene evaluation | 0.032 | 0.052 | 0.834 | 0.971 |

Export times surround `bpy.ops.export_scene.gltf`, including evaluated extraction,
serialization and writing GLB. They exclude human click time and launching
Blender. The generator invokes the same operator as a manual export, so the
percentiles are repeatable and are not a claim to have timed twenty human exports.

Ingest surrounds the actual `render::ingest_model` path: file loading, validation,
mesh/material preparation and immutable package generation/reuse. These images
are untextured; no texture compression or mip workload can be inferred.

Upload surrounds command acquisition, vertex/index storage-buffer uploads,
submit and fence completion, on the standalone benchmark thread. Buffer/device
creation and CPU loading occur before timing. This measures the RHI transfer
seam, **not** renderer model integration, a GPU timestamp, first rendered image,
texture upload or display scanout. No waits were added to production frame code.

Adoption surrounds `doc::diff`, `SceneModel::apply` and `evaluate` on a prepared
document. The source property changes for 3 or 256 visible model nodes; document
construction and resource work are outside the interval. It does not render 256
copies of the flattened grid, measure scheduler admission or wait for a Program
permit. The two datasets measure ingestion and document cardinality separately.

Raw samples: [reference CSV](evidence/reference-r2.glb.baseline.csv),
[grid CSV](evidence/workload-256.glb.baseline.csv), and Blender export timings,
build provenance and GLB SHA-256 values in
[reference.json](../tests/fixtures/blender-scene-link/reference.json).

## Scope

Do not sum percentile columns and label the result end-to-end p95: the samples
are not correlated. The tables cover export, ingest, upload and adoption on two
untextured datasets only. Textured assets, large unique meshes, scatter and
terrain, cold exporter start-up (the first export took ~570 ms), LAN transfer
and coordinated Program changes are covered by
[qualification](qualification-v1.md) and [environment scale](environment-scale-v1.md),
not by this table.

## Reproduce

Run from the repository root after a Release GUI build:

```powershell
& 'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe' `
  --background --factory-startup --python-exit-code 1 `
  --python scripts/blender-scene-reference.py -- `
  --output out/blender-scene-reference --samples 20

& out/build/windows-msvc-release-gui/engine/render/mediaserver_blender_scene_reference_tests.exe `
  out/blender-scene-reference out/blender-scene-reference/qualified --benchmark
```

The first command writes both `.blend` revisions, GLBs, evaluated corner oracles,
the grid dataset and a JSON run record. The second validates the reference,
generates ordinary `.mstage` UI projects and immutable local model packages,
then writes per-dataset CSV files. `--benchmark` explicitly requires D3D12 and
fails if unavailable; CPU CTest does not require a GPU or Blender installation.
Pass distinct scratch directories to concurrent tool runs.

The portable checked-in fixture can be tested without running Blender:

```powershell
ctest --preset windows-msvc-debug-gui -R blender_scene_reference --output-on-failure
ctest --preset windows-msvc-release-gui -R blender_scene_reference --output-on-failure
```

Checked-in GLBs are small regression artifacts, not a substitute for rerunning
Blender qualification on a different supported version. Regeneration deliberately
updates timing/provenance JSON; review fixture/oracle changes together.
