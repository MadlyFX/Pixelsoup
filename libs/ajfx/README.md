# ajfx

Deterministic, seekable GPU particles and volumes for C++20 engines.

Most particle systems give a different result whenever the frame rate, thread
scheduling or playback path changes. ajfx is built around the opposite promise:
an effect's state is a pure function of its authored settings and the exact
simulation frame. Every machine in a render cluster sees the same particles,
a timeline seek lands on exactly what playing through would have shown, and
replays and lockstep simulations stay in agreement.

It is developed inside the media server that first needs it, and has no
dependency on it so it can move to its own repository once the API settles.

## Status

Phase 0: the project skeleton and the deterministic random primitive. Nothing
simulates or renders yet. The roadmap is the "Particles and volumes" section of
the media server's [`next-steps-plan.md`](../../next-steps-plan.md).

## Layout

| Path | Contents |
| --- | --- |
| `include/ajfx/` | Public C++ API |
| `src/` | Library implementation |
| `shaders/` | HLSL a host compiles into its kernels (`AJFX_SHADER_INCLUDE_DIR`) |
| `tests/` | Unit tests, registered with CTest as `ajfx.*` |

Planned additions: `src/core` (effect model, module compiler, CPU reference
simulator), `src/gpu` (runtime, backend interface), `backends/` (D3D12, Vulkan
reference backends), `renderers/` (sprite, ribbon, mesh, light passes) and
`volume/` (raymarched voxel volumes).

## Determinism rules

- Every random value is `particle_hash(emitter_seed, spawn_index, stream)`
  (`include/ajfx/random.hpp`, HLSL twin `shaders/ajfx_random.hlsli`). Never seed
  from a thread index, frame counter or clock.
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

## License

Not yet chosen; it will be decided before the first public release.
