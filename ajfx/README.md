# ajfx

Deterministic, seekable GPU particles and volumes, and the editor that
authors them.

An ajfx effect's state is a pure function of its authored settings and the
exact simulation frame. Every machine in a render cluster sees the same
particles, a seek lands on exactly what playing through would have shown, and
replays and lockstep simulations stay in agreement.

ajfx is its own project. It shares this repository with the media server, its
first host, which sits beside it at the repository root; the runtime depends
on nothing there.

## The pieces

| Piece | Where | What it is |
| --- | --- | --- |
| Runtime | [`lib/`](lib/README.md) | The C++20 library: effect model, CPU reference simulator, `.ajfx` packages, GPU runtime, D3D12 reference backend, rigid physics on Jolt, and the `ajfx_viewer` and `ajfx_player` samples |
| ajfx Studio | [`studio/`](studio/README.md) | A block editor: effects are pages of layers and plain-language blocks, compiled to module stacks and exported as `.ajfx` |
| Roadmap | [`docs/roadmap.md`](docs/roadmap.md) | Status, measured results and what remains |

A `.ajfx` package carries everything an effect needs: emitters and module
stacks, textures, meshes and distance fields, published parameters, and
precompiled kernels. A host loads and plays packages; it does not author
them.

## Building

Run from an MSVC developer shell with Ninja on PATH, in this directory:

```powershell
cmake --preset windows-msvc-debug
```

```powershell
cmake --build --preset windows-msvc-debug
```

```powershell
ctest --preset windows-msvc-debug
```

That builds the runtime, the D3D12 backend, the samples, Studio's headless
core and every `ajfx.*` test into `../out/ajfx/<preset>`. It needs no vcpkg
packages. `windows-msvc-release` is the optimized build, and
`linux-clang-debug` builds the CPU side.

The GPU suites need a D3D12 device with shader model 6.2 and
`dxcompiler.dll` with `dxil.dll` (from `AJFX_DXC_DIR`, by default the newest
Windows SDK that ships them); without either they report "skipped". Details
are under [Building](lib/README.md#building) in the runtime reference.

**The Studio application** previews through the media server's renderer and
draws with its UI toolkit, so it builds from the repository root with the
media server's GUI presets (which need `VCPKG_ROOT`):

```powershell
cmake --build --preset windows-msvc-debug-gui --target ajfx-studio
```

From the root, `cmake --build --preset area-particles` and
`ctest --preset area-particles` build and run every ajfx test.

| Option | Default | Effect |
| --- | --- | --- |
| `AJFX_WARNINGS_AS_ERRORS` | `OFF` (`ON` in the presets) | Warnings fail the build |
| `AJFX_BUILD_D3D12` | `ON` on Windows | The D3D12 backend and DXC compiler |
| `AJFX_RIGID` | `ON` | `ajfx::rigid`, rigid physics on the vendored Jolt |
| `AJFX_BUILD_EXAMPLES` | `ON` standalone | `ajfx_viewer` and `ajfx_player` |
| `AJFX_BUILD_STUDIO_APP` | `OFF` | The Studio application; a host providing the media server renderer turns it on |

## Using it from a host

Add this directory with `add_subdirectory(ajfx)` and link `ajfx::ajfx`, plus
`ajfx::d3d12` for the reference backend or `ajfx::rigid` for rigid physics.
A host with its own renderer implements `ajfx::gpu::Device`, or reads the
simulation's buffers and draws them itself; see
[Running on the GPU](lib/README.md#running-on-the-gpu). The media server's
integration (`engine/render/particle_*` at the repository root) is a worked
example.

## License

Not yet chosen; it will be decided before the first public release. Jolt
Physics, in `lib/third_party/jolt`, is MIT licensed.
