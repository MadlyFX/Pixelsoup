# Media Server

A C++20 virtual-production media server with a native layer/timeline editor,
media playback, a Blender scene link, and synchronized render/output nodes.
The Director commits document edits and show frames; nodes render the same
authored state locally. Windows offers an Advanced D3D12 renderer and an SDL
GPU Basic fallback. Core document and cluster modules also build headless.

MediaServer plays the `.ajfx` particle packages exported by ajfx Studio.
The ajfx runtime and Studio live in the separate ajfx repository.

This is an unreleased prototype. Project and wire schemas may change;
breaking changes are acceptable without migration support.

## Build and run

Requirements: CMake 3.28+, Ninja, a C++20 compiler (MSVC 2022, Clang 18+,
or Apple Clang), and vcpkg in manifest mode. Set `VCPKG_ROOT` to the vcpkg
checkout. On Windows, use a Visual Studio developer shell with Ninja on PATH.
For GUI builds, place the ajfx checkout beside this checkout as `../ajfx`
or configure with `-DMEDIASERVER_AJFX_SOURCE_DIR=<path-to-ajfx>`.

```powershell
cmake --preset windows-msvc-debug-gui
cmake --build --preset windows-msvc-debug-gui
ctest --preset windows-msvc-debug-gui

cmake --preset windows-msvc-release-gui
cmake --build --preset windows-msvc-release-gui
ctest --preset windows-msvc-release-gui

.\scripts\run-editor-demo.ps1
```

The demo opens the editor and a supervised local output node. Close the editor
or press Enter in its console to stop it. Use `-Exe` to select a specific
executable. Reconfigure with `--fresh` when intentionally resetting a build.

For day-to-day work, build and test one area at a time
(`cmake --build --preset area-editor`, then `ctest --preset area-editor`), or
run `ctest --preset quick` to skip the slow tests. The full Debug and Release
suites run before packaging. [AGENTS.md](AGENTS.md#build-and-validation)
lists the areas.

Other presets: `windows-msvc-debug` and `linux-clang-debug` are headless;
`macos-clang-debug-gui` builds the SDL shell. GUI builds enable the `gui`
vcpkg feature for SDL3, FFmpeg, model/image import, OpenEXR, OCIO and FreeType.
Long soak tests require `MEDIASERVER_ENABLE_SOAK_TESTS=ON`.

For a split Director/node setup, use `scripts/run-editor.ps1` and
`scripts/run-output-node.ps1`; the editor launcher prints the node command
with its address and shared secret. Detailed setup is in the
[operator guide](docs/operator-guide.md#run-across-two-machines).

## Where to read

| Task | Start here |
| --- | --- |
| Find code, development rules and relevant tests | [AGENTS.md](AGENTS.md) |
| Editor, timeline, Program/Preview, assets and project files | [Operator guide](docs/operator-guide.md) |
| Document, replication and module architecture | [Architecture](media-server-architecture-plan.md) |
| Renderer behavior, materials, lights and color | [Renderer guide](docs/renderer.md) and [capabilities and limits](advanced-renderer-status.md) |
| Cluster recovery, deployment and sync policy | [Cluster operations](cluster-operations.md) |
| Add or change an effect | [Effect contracts](effect-rendering-requirements.md) |
| Blender workflow and verification | [Add-on README](integrations/blender/README.md) |
| Blender protocol and native data contracts | [Scene-link reference](blender%20live%20scene%20link/README.md) |
| Particle playback and render integration | [Renderer guide](docs/renderer.md#particle-effects) |
| Physical genlock / wall color validation | [Renderer runbooks](advanced%20icvfx%20renderer/README.md) |

## Layout

`engine/` contains the modules and their colocated tests; `apps/server/`
contains the Director, node and lifecycle process. `apps/bake/` provides the
offline lighting baker. `tests/unit/` covers the core command/document/wire
spine, `tests/blender/` the publisher, and `tests/fixtures/` the pinned
scene-link fixtures. `scripts/` contains supported launchers, packagers,
fixture generators and Blender integration checks. `third_party/` retains
required source, fitted tables, licenses and provenance. `Models/` contains
optional source models for manual rendering experiments.

Default ripgrep searches skip `Models/` to keep source inventories focused;
search that directory explicitly when working on the sample assets.

Generated output belongs in an `out/` directory and is ignored at every
depth. Python bytecode, scene-link caches and digest-named model-import
caches are also ignored. Keep source models, shaders and test fixtures in
Git; recreate their derived packages as needed. Historical implementation
plans and one-off validation captures remain available in Git history.

## Delivery and limits

```powershell
.\scripts\package-windows-release.ps1
python scripts/package-blender-livelink.py
```

Windows packaging first builds both GUI presets and runs their full test
suites, then packages the Release GUI build with app-local dependencies,
launchers and notices. `-SkipTests` packages the existing build untested. See
[package instructions](packaging/windows/README.txt).

SDI, NDI, ST 2110, a native Vulkan backend and external render/plugin
interfaces remain future work. NVAPI synchronization is opt-in and still
requires physical hardware qualification. GPU conformance checks and
loopback tests do not establish scanout, camera calibration or stage-network
performance; the [renderer limits](advanced-renderer-status.md) and
[cluster runbook](cluster-operations.md) describe those boundaries.
