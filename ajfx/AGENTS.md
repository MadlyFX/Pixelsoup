# ajfx guide

ajfx is its own project: deterministic GPU particles (the runtime in `lib/`)
and ajfx Studio (`studio/`), the scene workspace that authors them. It shares
the repository with the media server at the root, its first host. Read this
guide and [README.md](README.md), then only the modules a change touches.
Search with `rg`; the `CMakeLists.txt` files locate tests.

## Code map

| Area | Location |
| --- | --- |
| Effect model, modules, builder, presets | `lib/include/ajfx/effect.hpp`, `modules.hpp`, `builder.hpp`, `lib/src/effect.cpp`, `presets.cpp` |
| CPU reference simulator, checkpoints, seeking | `lib/include/ajfx/simulation.hpp`, `lib/src/simulation.cpp` |
| Saved state, caches, bake-time and playback-time parameters | `lib/include/ajfx/cache.hpp`, `lib/src/cache.cpp` |
| `.ajfx` packages: textures, meshes, fields | `lib/src/package.cpp`, `texture.cpp`, `mesh.cpp`, `field.cpp` |
| GPU runtime: kernel generation, simulation, sort, renderer | `lib/src/gpu`, `lib/shaders` |
| D3D12 reference backend and DXC compiler | `lib/backends/d3d12` |
| Rigid physics: solver interface, Jolt solver, `RigidRunner` | `lib/include/ajfx/rigid*.hpp`, `lib/src/rigid`, `lib/src/rigid_runner.cpp`, `lib/third_party/jolt` |
| Samples | `lib/examples/viewer`, `lib/examples/player` |
| Studio core: scene lowering, block catalog and compiler (layer IR), bake | `studio/src` (`ajfx_studio_core`) |
| Scene ownership, metadata and persistence | `studio/src/scene.*`, `scene_properties.*`, `scene_io.*` |
| Scene tree and selected-item inspector | `studio/src/scene_view.*`, `docs/studio-scene-reference.md` |
| Studio application: window, preview, worker | `studio/src/studio_app.*`, `preview.*`, `package_builder.*`, `stage_overlay.*` |
| Compiler fixtures (old layer recipes) | `studio/tests/fixtures` |

`lib/` has no dependency outside the C++ standard library, the platform's
D3D12 and the vendored Jolt. Studio's core depends on `lib/` alone. Only the
Studio application links the media server's `MediaServer::Render`, `Gui` and
`Editor`, and that host code is the media server's, at the repository root.

## Contracts

- Determinism first. Randomness is `particle_hash(emitter seed, spawn index,
  stream)` with an HLSL twin; never seed from a thread, frame counter or
  clock. Fixed simulation steps, stable prefix-sum compaction, no atomic
  append, `/fp:precise` and `-ffp-contract=off` (set by `ajfx_set_options`).
- Budgets are fixed up front. Refuse and report an exceeded budget; never
  grow capacity or thin particles silently.
- Failures return `ajfx::Error` naming the emitter, module and field at
  fault. Nothing throws.
- Nothing in `lib/` or Studio's core names a host target, header or option.
  A host sets the `AJFX_*` options before `add_subdirectory(ajfx)`.
- The package format is versioned and refuses unknown fields and newer
  versions. ajfx is unreleased: breaking format changes are acceptable, with
  no migration layers unless a concrete need appears.
- Studio is a separate creative application with its own visual identity,
  workspace and interaction model. Follow
  [the Studio design](docs/studio-ui-architecture.md) and
  [delivery plan](next-steps-plan.md). The media server's `docs/ui-design.md`
  and shared-widget rules do not prescribe Studio's design or UI framework.
  Reuse host UI code only where it serves Studio's workflow. The current
  application still uses that toolkit, whose font atlas is ASCII-only;
  framework replacement is a planned decision, not an implemented change.
- Test observable behaviour with independent expectations: bit-identical
  reruns, seek equivalence, CPU and GPU agreement, pixels against
  hand-computed coverage.

## Build and validation

From an MSVC developer shell in this directory:

```powershell
cmake --preset windows-msvc-debug
```

```powershell
cmake --build --preset windows-msvc-debug
```

```powershell
ctest --preset windows-msvc-debug
```

Builds go to `../out/ajfx/<preset>`. Run `windows-msvc-release` too before
merging changes to the runtime.

A change to the Studio application, or to anything the media server's
particle integration uses, also builds from the repository root: the
`ajfx-studio` target of `windows-msvc-debug-gui`, `area-particles` (every
ajfx test) and `area-render` (the media server's particle pass).
The root's [AGENTS.md](../AGENTS.md) covers that build environment.

Write disposable builds, captures and exported packages
under `../out/`; keep artists' `.ajfxstudio` scenes out of commits.
