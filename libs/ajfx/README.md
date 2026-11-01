# ajfx

Deterministic, seekable GPU particles and volumes for C++20 engines.

Most particle systems give a different result whenever the frame rate, thread
scheduling or playback path changes. ajfx is built around the opposite promise:
an effect's state is a pure function of its authored settings and the exact
simulation frame. Every machine in a render cluster sees the same particles,
a timeline seek lands on exactly what playing through would have shown, and
replays and lockstep simulations stay in agreement.

Effects are authored in ajfx Studio, a block editor built on this library
(`apps/ajfx-studio`), and exported as `.ajfx` packages: the emitters and module stacks,
their textures and meshes, the parameters the effect publishes to a host, and
precompiled kernels. A host such as the media server loads packages and plays
them; it does not author them.

It is developed inside the media server that first needs it, and has no
dependency on it so it can move to its own repository once the API settles.

## Status

Phases 1 to 4 are done and phase 5 (rendering depth) is under way.
- **The CPU reference and the GPU runtime.** Per-emitter fused kernels are
  generated from the module stacks. The simulation runs on a D3D12 reference
  backend with a runtime compiler, stable compaction and indirect dispatch.
- **Packages.** Precompiled kernels and textures travel in `.ajfx`
  packages.
- **Sample applications.** A viewer and a player.
- **The media server integration.**
- **Phase 5.** Done:
  - deterministic depth sorting;
  - textured sprites and flipbooks;
  - soft particles and ribbons;
  - mesh particles and particle lights;
  - sphere, box and distance-field colliders;
  - rigid particles for debris;
  - rigid physics emitters run by Jolt (`ajfx::rigid`), whose particles
    collide with each other and pile up.

The roadmap is the "Particles and volumes" section of the media server's
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
| Init | `Lifetime` (required), `ShapeLocation` (point, sphere, box, disc), `Velocity` (cone), `InitColor`, `InitSize`, `InitRotation`, `InitSpin`, `InitTumble` |
| Update: forces | `Gravity`, `Drag`, `CurlNoise`, `Vortex`, `Attractor`, `Converge` |
| Update: after integration | `RigidBody`, `PlaneCollision`, `SphereCollision`, `BoxCollision`, `SdfCollision`, `ColorOverLife`, `SizeOverLife` |
| Update: instead of all motion | `RigidPhysics` (needs a rigid solver, see below) |

Each step ages and retires particles, applies forces in stack order,
integrates position (and turns sprites by their `InitSpin` spin), applies the
after-integration modules in stack order, then spawns. Newborns are placed at their exact birth time within the step, so
emission is smooth at any simulation rate. Any float, vector or colour input
can be bound to a published parameter with `ajfx::bind`.

**Ranges.** Most values are picked for each particle between two ends:
- `ShapeLocation` gives each particle a radius between `radius_min` and
  `radius_max`: equal ends are a crisp edge, apart a soft one or a thick
  shell.
- `Velocity::spread_min` hollows the launch cone, as a fountain's crown.
- `InitColor` picks a brightness between `brightness_min` and
  `brightness_max`, apart from the colour it picks between `a` and `b`.
- Every force takes each particle by its own factor between `scale_min` and
  `scale_max`, kept for life: some sparks fall heavier, some smoke drags
  more. The default 1 to 1 is the force as given.

**Fire motion.** Four pieces give flames the way real fire moves:
- **`CurlNoise::drift`** moves the turbulence field at that velocity. Eddies
  then roll up with a plume rising about as fast, and the particles riding
  them curl, instead of passing through still swirls.
- **`over_life` curves** on `Gravity` and `CurlNoise` scale them over each
  particle's life. Buoyancy can be strongest while a flame is hot, and
  turbulence can be calm at the base and break up towards the tip.
- **`Converge`** pulls particles towards an axis, fading with height, as a
  fire draws in air at its base and gathers flames into tongues.
- **`InitSpin`** gives sprites a spin, either way round, that slows by its
  `drag`. Flame and smoke cells twist as they rise.

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

auto renderer = ajfx::gpu::ParticleRenderer::create(*device, kernels, ajfx::gpu::TextureFormat::Rgba8Unorm).value();
auto target = device->create_render_target(1280, 720, ajfx::gpu::TextureFormat::Rgba8Unorm, "frame").value();

gpu.advance_to(240);  // records the steps
renderer.render(gpu, ajfx::gpu::Camera::look_at(eye, target_point, up, fov, aspect, 0.1F, 100.0F), target);
device->flush();      // runs them
ajfx::gpu::EmitterBuffers buffers = gpu.emitter_buffers(0); // or draw it yourself
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
- `ParticleRenderer` draws each emitter with one indirect draw whose
  instance count the simulation wrote: camera-facing soft discs, or quads
  aligned to velocity and stretched by `SpriteRenderer::stretch` seconds of
  motion, blended alpha, additive or premultiplied. Alpha-blended emitters draw
  back to front (see Drawing).
- A host implements `gpu::Device` (buffers, compute and graphics pipelines,
  dispatch and draw, direct and indirect, copies, barriers, render targets) to
  run ajfx on its own renderer, or reads `EmitterBuffers` and draws itself.
  Kernels bind sixteen 32-bit constants at `b0, space1` and up to eight raw
  read-write buffers at `u0`..`u7` in space 0; a host whose compute layout
  matches runs packaged kernels unchanged. The media server's
  `render::ParticleDevice` is one such host, compute only.
- `write_package(effect, gpu::kernel_package_entries(effect, kernels))` stores
  the compiled kernels in the package, and `gpu::read_package_kernels` loads
  them, so a host plays effects without a compiler. Kernels are keyed by a
  hash of the generated source; a package from another ajfx version, or with
  an edited module stack, is refused with a reason rather than run with
  mismatched bytecode. Changing only values (a speed, a colour) keeps the
  kernels valid, and so does changing module ids: each module's data starts
  with its id, so an editor can give a module a new id (a seed) without a
  compile.

About a million live particles take 1.9 ms a step on an RTX 3090 (2.2 ms
with curl noise), measured as wall-clock time for recording and execution
before any tuning.

## Layout

| Path | Contents |
| --- | --- |
| `include/ajfx/` | Public C++ API; `gpu/` for the GPU runtime, `d3d12.hpp` for the backend |
| `src/` | Library implementation; `gpu/` holds kernel generation and the GPU simulation |
| `backends/d3d12/` | The D3D12 device and the DXC compiler (target `ajfx::d3d12`, Windows) |
| `src/rigid/`, `third_party/jolt/` | The Jolt rigid solver (target `ajfx::rigid`) and Jolt Physics v5.6.0, MIT licensed |
| `examples/viewer/` | `ajfx_viewer`: plays, scrubs, captures and exports effects |
| `shaders/` | Module functions and shared kernels in HLSL, embedded in the library at configure time |
| `tests/` | Unit tests, registered with CTest as `ajfx.*` |

Planned additions:
- a Vulkan backend;
- `volume/`, raymarched voxel volumes.

ajfx Studio, the authoring application, is in the media server tree at
[`apps/ajfx-studio`](../../apps/ajfx-studio/README.md). Its preview is drawn
by the media server's renderer, so an effect looks in Studio as it will on
the wall. Its page model and compiler use ajfx alone.

## Drawing

**Sorting.** Alpha-blended emitters draw back to front: `ParticleRenderer`
sorts them with a `gpu::ParticleSorter` before drawing.
- **Which emitters sort:** sprites and meshes with `SpriteRenderer::sort`
  set (the default) and a blend that depends on order. Additive emitters
  and ribbons keep birth order and cost nothing.
- **How it works:** a stable GPU radix sort of the view depth. It uses
  integer scans, with atomics only for counting. It sorts the top 24 bits of
  the depth (one part in 32,768 of the distance) in six passes, and sizes
  itself from the simulation's own live count, so nothing is read back.
- **Determinism:** equal depths, and depths closer than the sort resolves,
  keep birth order. Every run, seek and node
  that holds the same particles draws them in the same order.
- **Hosts with their own renderer** call `sort` themselves, with the axis in
  the effect's own space (`world` transposed times the camera's forward
  axis).
- **Cost on an RTX 3090:** about 0.2 ms up to 65,000 particles (mostly the
  fixed cost of its 19 dispatches), 0.5 ms for a million and 1.9 ms for four
  million.

**Textures.** A sprite with a `texture` samples it instead of drawing a soft
disc.
- **Flipbooks:** with `flipbook_columns` and `flipbook_rows`, a sprite plays
  the grid's cells over each particle's life, left to right and top to
  bottom. `flipbook_blend` cross-fades each cell into the next.
- **Where textures come from:** a package carries them, and
  `read_package_textures` reads and checks them. Presets generate theirs
  (`presets::textures()`).
- **What is refused:** an effect naming a texture it was not given, with
  `MissingTexture`; and a flipbook with more cells than texels.

**Soft particles.** With `soft_distance`, a sprite fades out over that
distance as it nears the scene behind it, instead of cutting a hard line
into a surface. It needs the host's scene depth. The reference renderer
draws no scene, so there it has nothing to fade against.

**Ribbons.** With `geometry` set to `ribbon`, an emitter draws one strip
through its live particles in birth order (oldest first). Each end is as
wide as its particle and faces the camera across the trail. A texture runs
along the strip by each end's life. Ribbons are not depth-sorted, and they
refuse velocity facing, stretch and flipbooks.

**Meshes.** With `geometry` set to `mesh`, each particle draws the mesh
named by `mesh`, a package entry.
- **Placement:** the mesh is scaled by the particle's size. A rigid
  particle's mesh turns by its orientation. Otherwise its +Z points along
  the velocity with velocity facing, or it spins about Y by the particle's
  rotation.
- **Look:** a texture takes the mesh's own coordinates. Shading is a light
  at the camera.
- **Depth:** the reference renderer has no depth buffer. It culls back
  faces, so use convex meshes there, and it draws meshes back to front by
  their centres. Meshes that touch or interpenetrate can still overlap
  wrongly where their centres' order differs from their surfaces'. The media
  server draws meshes with depth.

**Lights.** Some of an emitter's particles become point lights in the
host's lit scene, at most `light_count` at once.
- **Which particles.** A particle carries a light for its whole life or not
  at all: those whose spawn index is a multiple of `light_spacing()`. That
  spacing shares the emitter's particle bound out over its lights, so a
  light never jumps from one particle to another as others are born and
  die.
- **Per particle.** Each light's intensity and reach are picked between
  `light_intensity_min`/`max` and `light_range_min`/`max` for its particle.
- **Over its life.** The colour is the particle's, or with
  `light_own_color` the `light_colors` keys (by their alpha). `light_curve`
  scales the intensity. Both follow the particle's life, with up to eight
  keys each, so a light can fade in and out instead of popping.

**Colliders.** `SphereCollision`, `BoxCollision` and `SdfCollision` keep
particles out of a shape, or with `inside` within it. The response is the
same as `PlaneCollision`'s: back onto the surface, with restitution,
friction or `kill`. `SdfCollision` follows a distance field, a
`fields/<name>` package entry that `make_distance_field` bakes. The CPU
reference and the GPU kernels sample it identically.

**Rigid particles.** For debris, `RigidBody` makes an emitter's particles
small solid bodies, and `InitTumble` sets them spinning.
- **Turning:** each particle has an orientation (a unit quaternion) and an
  angular velocity. `InitTumble` gives a uniformly random orientation and a
  spin of `speed_min` to `speed_max` degrees a second about a random axis.
  `RigidBody` turns the particle every step, with `angular_drag` slowing
  the spin as `Drag` slows velocity.
- **Contacts:** every collider meets a rigid particle as a ball
  `contact_scale` of its size in radius, not as a point.
  - A contact slower than `rest_speed` stops rather than bouncing, so a
    pile settles instead of buzzing.
  - The collider's friction grips the ball's surface, limited by the push
    as Coulomb friction is: friction `f` takes at most `f / (1 - f)` of the
    contact's push off the slip. A resting piece slides to a stop at that
    share of gravity, and friction 1 grips at once.
  - `roundness` says how readily a gripped piece rolls. At 1 it is a solid
    ball: sliding becomes rolling, and spin throws the ball along. At 0 it
    is a flat chip that only slides, and each contact takes its spin away.
- **Limits:** particles do not collide with each other, and a mesh turns
  by its orientation but collides as its ball. For stacking and resting
  piles, use rigid physics.

The `debris` preset throws chips of the `chip` mesh, which
`presets::meshes()` generates.

**Rigid physics.** `RigidPhysics` hands an emitter's particles to a
rigid-body engine. They collide with each other and with the emitter's
colliders, pile up, knock each other about and fall asleep at rest.
- **The engine:** core ajfx depends on none. It drives a `RigidSolver`
  (`include/ajfx/rigid_solver.hpp`) that the host passes in
  `SimulationOptions::rigid`. `ajfx::rigid::make_jolt_solver()`
  (`include/ajfx/rigid.hpp`, target `ajfx::rigid`, option `AJFX_RIGID`)
  provides one, built on Jolt Physics in its cross-platform deterministic
  build (`third_party/jolt`). Without a solver, an effect with rigid physics
  is refused with `RigidSolverMissing`.
- **Bodies:** each particle is a body of `shape` at its size from birth:
  - `hull`: the convex hull of the renderer's mesh;
  - `sphere`, or `box` (a cube);
  - `auto`: the hull for mesh geometry, a sphere otherwise.

  `density`, `friction`, `restitution`, `angular_damping` and
  `allow_sleep` set the material.
- **What acts on them:**
  - `Gravity`, bound or not;
  - `Drag`, as linear damping;
  - plane, sphere and box colliders, which become unmoving bodies with
    their own restitution and friction.

  Every other force is refused with the reason, and so are `SizeOverLife`,
  distance fields, `kill`, colliders bound to parameters, and keeping
  particles inside a sphere. Births, lifetimes and colour over life are
  every emitter's.
- **Determinism:** bodies enter and leave the world in birth order, in the
  lowest free slots. `Simulation::state()` carries the engine's own state,
  so checkpoints and seeks land exactly. Results are identical across runs,
  thread counts, and Debug and Release builds.
- **On the GPU:** `GpuSimulation` runs rigid physics emitters on the CPU
  with the same code, and writes their particles into the emitter's
  buffers every step. Sorting, meshes, lights and drawing are unchanged.
- **Cost on a Ryzen 5800X:**
  - `rubble`, 400 hulls: 0.75 ms a step on one thread, 0.32 ms on four.
  - 4,000 hulls in a dense heap: 9.7 ms on one thread, 3.4 ms on four.
  - A checkpoint is about 135 bytes a body.

  `ajfx_rigid_bench` measures it and prints a state digest for comparing
  builds and machines.

The `rubble` preset throws 400 chips that pile on the floor and each other.
When a body's life ends, everything touching it wakes, so nothing stays
asleep in the air where it rested.

## Sample viewer

`ajfx_viewer` is built when ajfx is the top-level project
(`AJFX_BUILD_EXAMPLES`).

```bash
ajfx_viewer --preset smoke
```

Drag to orbit, wheel to zoom, Space to pause, Left and Right to seek a second
(backwards through GPU checkpoints, landing exactly where playing would have),
Home to restart, 1 to 5 to switch preset (sparks, smoke, confetti,
debris, rubble).

```bash
ajfx_viewer --preset sparks --export sparks.ajfx
```

```bash
ajfx_viewer --package sparks.ajfx --seconds 2 --capture sparks.png
```

The first writes a package with precompiled kernels; the second plays it
without compiling and saves a frame.

## Player

`ajfx_player` is a small media player for `.ajfx` files, built alongside the
viewer. Open a file from the command line, File > Open, or by dropping it on
the window. It has play and pause, a scrub bar, a loop length, stepping one
simulation step at a time, and orbit and zoom with the view framed to
everything the effect reaches during the loop.

The status bar is for checking a package: live particles (per emitter when
there are several), the current step, the time a frame takes, and whether the
package's precompiled kernels were used or why they could not be. A file that
cannot be played shows the library's error, which names what is wrong.

File > Associate .ajfx files with this player (or `ajfx_player --register`)
makes double-clicking an `.ajfx` file open it, for the current Windows user
only; Remove the association (`--unregister`) undoes it. The association
points at the executable it was made from, so make it again after moving the
player.

| Key | Action |
| --- | --- |
| Space | Play or pause |
| Home | Restart |
| Left, Right | Back or forward a second |
| `,` `.` | Previous or next simulation step (pauses) |
| L | Loop on or off |
| F, or double-click the view | Frame the effect |
| Ctrl+O, F5 | Open, reload |

## The .ajfx package

A small container of named entries, sorted by name, with an FNV-1a checksum
(layout in `include/ajfx/package.hpp`).
- `effect.json` holds the effect.
- `kernels/<format>/...` holds precompiled kernels, keyed to their generated
  source.
- `textures/<name>` holds sprite textures.
  - Each is an RGBA8 mip chain built when the package is written, by
    `make_texture`. Readers need no image decoder, and every node samples
    the same texels.
  - The layout is in `include/ajfx/texture.hpp`.
- `meshes/<name>` holds particle meshes (layout in `include/ajfx/mesh.hpp`).
- `fields/<name>` holds distance fields for collision (layout in
  `include/ajfx/field.hpp`).

Writing is deterministic, so a package's content hash identifies the
effect. Readers refuse unknown fields, newer format versions and damaged
files with a reason.

## Determinism rules

- Every random value is `particle_hash(emitter_seed, spawn_index, stream)`
  (`include/ajfx/random.hpp`, HLSL twin `shaders/ajfx_random.hlsli`). Never seed
  from a thread index, frame counter or clock. The stream comes from the
  module's id, so adding or reordering modules leaves the others' random
  values unchanged, and a new id for one module (an editor's seed) deals
  that module alone new values.
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
- Drawing order and flipbook cells derive from particle state and the camera
  alone: a stable sort with birth order as the tie-break, and cells chosen
  from age.
- A rigid solver is driven in a fixed order: removals oldest first, one
  step, additions in birth order into the lowest free slots. Every node must
  build the same Jolt source with the same defines (`third_party/jolt`), and
  update together.

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
