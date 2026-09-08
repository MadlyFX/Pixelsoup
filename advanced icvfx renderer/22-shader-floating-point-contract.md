# 22 — Shader floating-point contract

The production build compiles **every HLSL entry point with `-Gis -denorm
preserve`**, for both DXIL and SPIR-V. The reviewed DXC pin remains in
`engine/shaders/CMakeLists.txt`. Shader Model 6.2 is the minimum because explicit
FP32 denormal behavior starts there; D3D12 device creation checks that floor.
See Microsoft's [denorm-mode specification](https://github.com/microsoft/DirectXShaderCompiler/wiki/Denorm-Mode).

This chooses one compiler contract over maintaining a list of `precise`
expressions. New shaders inherit it automatically. CPU floating-point options
remain unchanged. Strict arithmetic does not make texture filtering or GPU
transcendentals identical on arbitrary hardware: [item 23](23-cohort-render-conformance.md)
also requires matching adapter, driver, bundle, and measured pass bytes.

## Build identity and comparison mode

`MEDIASERVER_SHADER_STRICT_FLOAT` defaults to `ON`. Setting it to `OFF` omits
`-Gis` for an A/B experiment while retaining `-denorm preserve` and SM 6.2.
This isolates the cost of strict arithmetic. It is a build setting, never an
operator quality switch, and cannot change mid-show. Strict and relaxed nodes
cannot pass the same cohort gate.

The build emits actual `.dxil` and `.spv` files alongside the embedded headers.
`bundle_identity.cmake` SHA-256 hashes a sorted manifest of their relative names
and SHA-256 values, plus the contract. The resulting 32-byte identity is compiled
into the executable; runtime never reads or hashes shader files. Test probes
are excluded so `BUILD_TESTING` does not change deployment identity. The existing
`build_manifest.txt` also records the effective flags and DXC version.

## Verification and GPU cost

Measured on the NVIDIA GeForce RTX 3090, driver **616.56** (DXGI
**32.0.16.1656**), using the item-23 128 × 128 fixture: sixteen frames
with geometry, four cascades, a spot atlas, IBL, AO, fog, TAA, object motion blur,
bloom, LUT and dither. It uses D3D12 GPU timestamps, three warmups and ten measured
runs. Per-frame readbacks are disabled for timing. Environment upload/prefilter
time is reported separately from the sixteen frames. Device/pipeline construction
and CPU recording time are excluded.

Both Release settings passed all **149/149** CTest entries, including the
existing byte-exact suites. Strict Debug also passed **149/149**. The strict
TAA DXIL contains zero fast-math arithmetic operations versus 71 in the relaxed
TAA DXIL; both explicitly record `fp32-denorm-mode="preserve"`.

For the cost comparison, separately built strict and relaxed executables were
retained and run in four alternating pairs (AB, BA, AB, BA). Each invocation
discarded three warmups and measured ten runs. The values below are the median
of the four batch medians, with the full observed min–max range in parentheses.

| GPU workload | Relaxed | Strict | Strict cost |
| --- | ---: | ---: | ---: |
| Sixteen complete frames | 5.913 ms (5.857–16.518) | 6.078 ms (6.047–13.239) | +2.8% |
| Per frame, derived from the sixteen-frame interval | 0.370 ms | 0.380 ms | +0.010 ms |
| Environment upload and IBL prefilter | 0.691 ms (0.689–3.302) | 0.744 ms (0.741–2.862) | +7.7% |

Decision: keep `-Gis` globally. This measured cost is preferable to relying on
an expression-by-expression audit that a future shader can miss.

This is a small workload on the developer desktop, with ordinary desktop GPU
activity and variable clocks. Its distribution is evidence for this fixture,
not a forecast for a production wall resolution or a guaranteed frame budget.
Earlier widely separated samples varied substantially with desktop load and
clock state; those were not used to estimate the cost. Raw paired measurements
are in `out/step22-ab/results.json` and the adjacent per-run logs.

To reproduce from an MSVC developer shell with the repository dependencies:

```powershell
cmake --preset windows-msvc-release-gui -DMEDIASERVER_SHADER_STRICT_FLOAT=ON
cmake --build --preset windows-msvc-release-gui --parallel 4
ctest --preset windows-msvc-release-gui
& ./out/build/windows-msvc-release-gui/engine/render/mediaserver_render_conformance_tests.exe --benchmark

cmake --preset windows-msvc-release-gui -DMEDIASERVER_SHADER_STRICT_FLOAT=OFF
cmake --build --preset windows-msvc-release-gui --parallel 4
ctest --preset windows-msvc-release-gui
& ./out/build/windows-msvc-release-gui/engine/render/mediaserver_render_conformance_tests.exe --benchmark

# Restore the production contract and rebuild before running the show.
cmake --preset windows-msvc-release-gui -DMEDIASERVER_SHADER_STRICT_FLOAT=ON
cmake --build --preset windows-msvc-release-gui --parallel 4
```
