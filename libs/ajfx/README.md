# ajfx

Deterministic, seekable GPU particles and volumes for C++20 engines.

Most particle systems give a different result whenever the frame rate, thread
scheduling or playback path changes. ajfx is built around the opposite promise:
an effect's state is a pure function of its authored settings and the exact
simulation frame. Every machine in a render cluster sees the same particles,
a timeline seek lands on exactly what playing through would have shown, and
replays and lockstep simulations stay in agreement.

Effects are authored in ajfx Studio, a separate application built on this
library, and exported as `.ajfx` packages: the emitters and module stacks,
their textures and meshes, the parameters the effect publishes to a host, and
precompiled kernels. A host such as the media server loads packages and plays
them; it does not author them.

It is developed inside the media server that first needs it, and has no
dependency on it so it can move to its own repository once the API settles.

## Status

Phase 2a, the GPU simulation: per-emitter fused kernels generated from the
module stacks, a D3D12 reference backend and a runtime compiler, running the
CPU reference's simulation on the GPU with stable compaction and indirect
dispatch. Nothing draws yet; sprites and a sample application are next. The
roadmap is the "Particles and volumes" section of the media server's
[`next-steps-plan.md`](../../next-steps-plan.md).

## Getting started

Start from a preset, or build an effect in code:

```cpp
#include <ajfx/builder.hpp>
#include <ajfx/simulation.hpp>

ajfx::EffectBuilder builder{"embers"};
builder.float_parameter("rate", 200.0F, 0.0F, 1000.0F);
builder.emitter("sparks")
    .capacity(4096)
    .spawn(ajfx::SpawnRate{.rate = ajfx::bind("rate")})
    .init(ajfx::Lifetime{.min = 1.0F, .max = 2.0F})
    .init(ajfx::Velocity{.speed_min = 2.0F, .speed_max = 4.0F, .spread = 25.0F})
    .update(ajfx::Gravity{})
    .update(ajfx::CurlNoise{.strength = 2.0F});

ajfx::Result<ajfx::Effect> effect = builder.build(); // validated, budget checked
ajfx::Result<ajfx::Simulation> simulation = ajfx::Simulation::create(effect.value(), /*instance_seed=*/7);
// Step 60 times a second while the host plays at 24 frames a second.
simulation.value().advance_to(ajfx::tick_at(/*frame=*/240, 24, 1, effect.value().simulation_rate));
```

`write_package` and `read_package` save and load the same effect as an
`.ajfx` file. Nothing throws; failures come back as an `ajfx::Error` naming
the emitter, module and field at fault.

### Modules

| Stage | Modules |
| --- | --- |
| Spawn | `SpawnRate`, `SpawnBurst` |
| Init | `Lifetime` (required), `ShapeLocation` (point, sphere, box, disc), `Velocity` (cone), `InitColor`, `InitSize`, `InitRotation` |
| Update: forces | `Gravity`, `Drag`, `CurlNoise`, `Vortex`, `Attractor` |
| Update: after integration | `PlaneCollision`, `ColorOverLife`, `SizeOverLife` |

Each step ages and retires particles, applies forces in stack order,
integrates position, applies the after-integration modules in stack order,
then spawns. Newborns are placed at their exact birth time within the step, so
emission is smooth at any simulation rate. Any float, vector or colour input
can be bound to a published parameter with `ajfx::bind`.

### Parameters, budgets and seeking

- A host supplies parameter values per step through `ParameterProvider`.
  Values are clamped to each parameter's declared range, which lets validation
  prove up front that an emitter's worst case fits its capacity.
- Values must be a pure function of the step (derived from the host's
  timeline, never its clock). `CheckpointRing` then seeks to any step,
  forwards or backwards, and lands on exactly the state stepping would have
  produced.
- `Simulation::create` takes an instance seed so two placements of one effect
  differ while each stays reproducible.

## Running on the GPU

```cpp
#include <ajfx/d3d12.hpp>
#include <ajfx/gpu/simulation.hpp>

auto device = ajfx::d3d12::create_device().value();            // or {.warp = true}
auto compiler = ajfx::d3d12::create_shader_compiler().value(); // dxcompiler.dll + dxil.dll
ajfx::gpu::KernelSet kernels = ajfx::gpu::compile_kernels(effect, *compiler).value();
auto gpu = ajfx::gpu::GpuSimulation::create(*device, effect, kernels, /*instance_seed=*/7).value();

gpu.advance_to(240);  // records the steps
device->flush();      // runs them
ajfx::gpu::EmitterBuffers buffers = gpu.emitter_buffers(0); // particles, live count, layout
```

- Each emitter gets an update kernel that runs its whole update stack per
  particle in one pass, and a spawn kernel that runs its init stack per
  newborn. Their HLSL is generated from the module stacks
  (`generate_kernel_sources`) and is deterministic text. Shared kernels do a
  two-level prefix sum and scatter that keep survivors in birth order, and
  write the next step's indirect dispatch sizes. Nothing is read back while
  it runs.
- Particle records store only the attributes an emitter's modules use
  (`particle_layout`).
- Births are scheduled on the CPU by the same code the reference uses, so
  particles appear at the same steps with the same ages and spawn indices.
  Spawning, deaths, ages and lifetimes match the reference exactly; positions
  and colours match within floating-point tolerance, since GPU `sin`, `cos`,
  `sqrt` and division round differently. Reruns on one device are
  bit-identical, and `save`/`restore` checkpoints resume exactly.
- A host implements `gpu::Device` (buffers, compute pipelines, dispatch,
  indirect dispatch, copies, barriers) to run ajfx on its own renderer. Hosts
  that ship precompiled kernels need no `ShaderCompiler` at runtime.

About a million live particles take 1.9 ms a step on an RTX 3090 (2.2 ms
with curl noise), measured as wall-clock time for recording and execution
before any tuning.

## Layout

| Path | Contents |
| --- | --- |
| `include/ajfx/` | Public C++ API; `gpu/` for the GPU runtime, `d3d12.hpp` for the backend |
| `src/` | Library implementation; `gpu/` holds kernel generation and the GPU simulation |
| `backends/d3d12/` | The D3D12 device and the DXC compiler (target `ajfx::d3d12`, Windows) |
| `shaders/` | Module functions and shared kernels in HLSL, embedded in the library at configure time |
| `tests/` | Unit tests, registered with CTest as `ajfx.*` |

Planned additions: a Vulkan backend, `renderers/` (sprite, ribbon, mesh, light
passes), `volume/` (raymarched voxel volumes) and `studio/` (the authoring
application).

## The .ajfx package

A small container of named entries, sorted by name, with an FNV-1a checksum
(layout in `include/ajfx/package.hpp`). `effect.json` holds the effect;
textures, meshes and precompiled kernels join it as further entries. Writing is
deterministic, so a package's content hash identifies the effect. Readers
refuse unknown fields, newer format versions and damaged files with a reason.

## Determinism rules

- Every random value is `particle_hash(emitter_seed, spawn_index, stream)`
  (`include/ajfx/random.hpp`, HLSL twin `shaders/ajfx_random.hlsli`). Never seed
  from a thread index, frame counter or clock. The stream comes from the
  module's id, so adding or reordering modules leaves the others' random
  values unchanged.
- A fixed simulation step; hosts map their time onto steps with `tick_at`.
- The CPU reference is bit-identical between runs of the same build. Across
  compilers and against the GPU it agrees within tolerance, since standard
  library `sin`, `cos` and `cbrt` are not correctly rounded.
- Integer hashing only; floating-point code is compiled without contraction or
  reassociation (`/fp:precise`, `-ffp-contract=off`).
- Live-particle lists are built in a stable order (prefix sums), never by
  atomic append.
- Capacity is fixed up front. An exceeded budget is refused and reported, not
  grown or silently thinned.

## Building

Standalone:

```bash
cmake -S libs/ajfx -B out/ajfx
```

```bash
cmake --build out/ajfx
```

```bash
ctest --test-dir out/ajfx
```

Inside the media server it is part of every configuration; `area-particles`
builds and runs just its tests.

The GPU suites need a D3D12 device with shader model 6.2 (hardware, or WARP
with `AJFX_TEST_WARP=1`) and `dxcompiler.dll` with `dxil.dll`. CMake copies
them beside the tests from `AJFX_DXC_DIR`, by default the newest Windows SDK
that ships them. Without either, those suites report "skipped". `ajfx.d3d12`
runs the backend under the D3D12 debug layer; `AJFX_TEST_DEBUG_LAYER=1` turns
it on for `ajfx.gpu` too.

## License

Not yet chosen; it will be decided before the first public release.
