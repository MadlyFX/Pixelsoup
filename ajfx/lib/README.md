# ajfx

Deterministic, seekable GPU particles and volumes for C++20 engines.

Most particle systems give a different result whenever the frame rate, thread
scheduling or playback path changes. ajfx is built around the opposite promise:
an effect's state is a pure function of its authored settings and the exact
simulation frame. Every machine in a render cluster sees the same particles,
a timeline seek lands on exactly what playing through would have shown, and
replays and lockstep simulations stay in agreement.

Effects are authored in [ajfx Studio](../studio/README.md), a block editor
built on this library, and exported as `.ajfx` packages: the emitters and
module stacks, their textures, meshes and fields, the parameters the effect
publishes to a host, and precompiled kernels. A host such as the media server
loads packages and plays them; it does not author them.

The library depends on no host. This page is its reference; the
[ajfx README](../README.md) gives the whole project.

## Status

The runtime, its first host, rendering depth and a first grid volume are
done; tuning, custom modules, a Vulkan backend and more of volumes are next.
- **The CPU reference and the GPU runtime.** Per-emitter fused kernels are
  generated from the module stacks. The simulation runs on a D3D12 reference
  backend with a runtime compiler, stable compaction and indirect dispatch.
- **Packages.** Precompiled kernels and textures travel in `.ajfx`
  packages.
- **Sample applications.** A viewer and a player.
- **A first host.** The media server runs the kernels on its own RHI and
  draws effects in its scene renderer.
- **Rendering depth and physics.** Done:
  - deterministic depth sorting;
  - textured sprites and flipbooks;
  - lit particles: sprites and meshes lit by the scene through six
    directions, as rounded sprites or six-way lightmaps (see
    [Lighting](#drawing));
  - soft particles and ribbons;
  - mesh particles and particle lights;
  - sphere, box and distance-field colliders;
  - rigid particles for debris;
  - rigid physics emitters run by Jolt (`ajfx::rigid`), whose particles
    collide with each other and pile up;
  - piles that stand when an effect starts and collapse as it plays;
  - props an artist places in a rigid physics world, and the contacts its
    bodies report;
  - particles born where bodies hit (dust, sparks) and shed by bodies as
    they move (smoke, embers), a glow that cools, and init modules and
    curves scoped to one kind of thing in a mixed emitter;
  - `RigidRunner`, which steps rigid physics off a host's frame path.
- **Caching and playback.** Saved simulation state, and caches of rigid and
  particle emitters that play back in place of simulating them, beside live
  emitters that step exactly as they would beside the simulated ones (see
  [Caching](#caching)).
- **Grid volumes.** A fixed-step smoke grid that particles feed and are
  carried by, stepped the same to the bit on the CPU and the GPU, saved
  with the state, and raymarched by the reference renderer (see
  [Grid volumes](#grid-volumes)).

The [remaining work](../next-steps-plan.md) lists what is still to do.

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
| Spawn | `SpawnRate`, `SpawnBurst`, `SpawnPile` (rigid physics only), `SpawnFromContacts` and `SpawnFromBodies` (particles born of another emitter's bodies), `SpawnActions` (born of a host's actions, see below) |
| Init | `Lifetime` (required), `ShapeLocation` (point, sphere, box, disc), `Velocity` (cone), `InitColor`, `InitSize`, `InitRotation`, `InitSpin`, `InitTumble` |
| Update: appearance | `ColorOverLife`, `SizeOverLife`, `Heat` (a glow that cools) |
| Update: forces | `Gravity`, `Drag`, `CurlNoise`, `Vortex`, `Attractor`, `Converge`, `Shockwave` (a blast, see below), `VolumeAdvect` (carried by a grid volume) |
| Update: after integration | `RigidBody`, `PlaneCollision`, `SphereCollision`, `BoxCollision`, `SdfCollision`, `VolumeSource` (feeds a grid volume) |
| Update: instead of all motion | `RigidPhysics` (needs a rigid solver, see below) |
| Update: coming apart | `Break` (rigid physics only) |
| Update: hitting another emitter's bodies | `BodyCollision`, and `Wake` (stirred by them as they move) |

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

**Spawn indices.** Each spawn module counts its own births
(`EmitterState::spawn_counts`), and a particle's spawn index is
`spawn_index_of(source, count)`: the module's place in the spawn stack in
the top 8 bits, its own count in the low 24 (`spawn_source` reads the place
back). Every random draw is keyed by the spawn index, so a spawn module
added after the others leaves their particles bit for bit as they were. The
pieces broken bodies become are a source of their own
(`piece_spawn_source`). An emitter holds at most `max_spawn_modules` (255)
spawn modules. Within one module the index rises with birth; birth order
across modules is the particles' order in `EmitterState`, not their indices.

**Scopes.** One emitter can hold several kinds of thing that start and look
different: an init module, a colour or size over life, or a heat can be
scoped (`only`, a `Scope`) to some of its particles.
- `Scope::sources` names spawn modules by their place in the spawn stack
  (`spawn_source`), and `piece_spawn_source` for the pieces broken bodies
  become; `Scope::kinds` names body kinds of a rigid physics emitter.
- A particle is in scope when every list that is not empty names it
  (`in_scope`), so an empty scope is every particle: what a module without
  one has always done. The init modules `Lifetime`, `ShapeLocation`,
  `Velocity`, `InitColor`, `InitSize`, `InitRotation` and `InitTumble` have
  one, and so do `ColorOverLife`, `SizeOverLife` and `Heat`.
- So a rigid world's crates are brown and a unit across while its fragments
  are orange, a tenth the size and glowing, each kind with its own
  `InitColor`, `InitSize` and `Heat`, and a colour over life every body
  takes. Studio can merge every solid layer into one world this way.
- A spawn module's particles need a `Lifetime` in scope; validation refuses
  one that has none, naming it. A particle no other init module is in scope
  of keeps that module's default, as without the module.
- A scope must name what its emitter has: a spawn module it holds, the
  pieces only where it has rigid physics, kinds only where it has rigid
  physics and that many kinds.
- Scopes are data, not kernel source: the kernels read a bit per spawn
  source each step, so editing a scope needs no compile. A particle emitter
  has no body kinds on the GPU; kinds are for rigid physics emitters, which
  the CPU simulates.

**Heat.** `Heat` makes particles glow and cool: hot sparks, embers, a
fragment fresh from a blast.
- A particle's heat is `start` at birth and halves every `half_life`
  seconds of its age: `start * 2^(-age / half_life)`, worked out from the
  age every step rather than carried, so seeks, caches and the GPU agree
  and it falls monotonically.
- The glow is a black-body-like ramp of the heat -- black, red, orange,
  yellow, white -- times `glow`, added to red, green and blue. Above 1 the
  heat stays white; colours above 1 are emission a host draws as light.
- Every heat is added after every colour over life, wherever they stand in
  the stack, onto the colour worked out afresh from the base colour each
  step, so a glow never piles onto the last step's.
- Scoped by kind it lets hot fragments and cold crates be one rigid world's
  bodies. A piece a body breaks into takes its parent's age, so it is as
  hot as the parent; a fragment that should start hot is born hot, of its
  own spawn module or kind.

**Born of bodies.** Two spawn modules bear a particle emitter's particles
from a rigid physics emitter it names (`emitter`), which steps first:
- `SpawnFromContacts` bears them where its bodies hit something: dust where
  pieces land, sparks where a ball strikes a crate. Each contact of the
  step closing at `min_speed` or faster (with `new_only`, only those that
  began this step) bears `per_hit` plus `per_speed` for every unit a second
  of its speed, rounded up or down by a hash of the step and the contact's
  place in the list, at most `max_per_step` a step. A newborn starts at the
  contact's point at age 0 with `along_normal` of the speed along the
  normal turned to point up, and `along_surface` of it across, a random
  way, the two scattered within `spread` degrees.
- `SpawnFromBodies` sheds them off bodies as they move: smoke trailing a
  thrown rock, embers off a plank. Each body moving at `min_speed` or faster
  sheds `rate` plus `per_speed` for every unit a second of its speed, per
  second, rounded by a hash of the body and the step, at most `max_per_step`
  a step in all. They are spread along the step's stretch of the body's
  path at the ages they reached, so a trail is continuous at any speed,
  `surface` of the way from its middle to the ball its size spans, with
  `inherit` of its velocity.
- Both read only bodies in their `bodies` scope (that emitter's sources and
  kinds): sparks only where the ball hits, smoke only off the burning kind.
- The init modules set newborns up as any: a `ShapeLocation` is then an
  offset around where the contact or body put them, and a `Velocity` adds
  to what it gave. Scope them to the module to set its particles apart.
- Their count is bounded by `max_per_step`, which is what validation proves
  the capacity against. Each counts its own spawn indices, so adding one
  leaves every other module's particles bit for bit as they were.
- A rigid physics emitter cannot hold them (bodies are not born that way),
  and a live emitter cannot bear from a cached one: a cache keeps bodies,
  not their contacts or speeds. `default_cache_modes` caches such an emitter
  with its bodies as a particle layer, and `bake` refuses a live one.
- On the GPU the births, where they start and how fast are worked out on
  the CPU by the reference's own code and travel to the spawn kernel beside
  each newborn's age and spawn index.

**Blasts.** An effect lists its blasts (`Effect::blasts`, `EffectBuilder::blast`):
a `Blast` has a `name`, a `center`, a `power`, a `radius` and a `time`. It is
set once, and everything that answers it names it:
- `Shockwave{.blast = "boom"}` throws an emitter's particles straight out
  from the centre in the blast's step, each by its own factor between
  `scale_min` and `scale_max`, as every force takes one.
- In a rigid physics emitter the same module throws its bodies and its loose
  props (see Rigid physics).
- `Break::blast_threshold` shatters the bodies thrown that hard.

So a host or an editor moves one blast and the dust, the debris and what
breaks all follow it.
- **How hard:** `blast_push(blast, at)` is the push at a point: `power`
  units a second at the centre, fading linearly to nothing at `radius`,
  whatever the mass of what it throws. Nothing beyond the radius is touched.
- **When:** in one step, `blast_tick`: the first that starts at or after
  `time`. The decision is made on the CPU, so a seek finds it in that step.
- **Fixed:** a blast's values are authored, not bound to parameters; it is
  an event of the effect, as a prop is a thing in it. Each cached emitter's
  digest holds the blasts it answers.
- At most `max_blasts` (64) to an effect; names are unique.

### Grid volumes

An effect lists its grid volumes (`Effect::volumes`, `EffectBuilder::volume`):
a `Volume` is a dense box of cells, `nx` by `ny` by `nz` of `cell_size` from
`origin`, holding per cell a density, a temperature and a velocity, all
three at the cell's centre (collocated, no staggered faces). It is an
Eulerian smoke grid: particles feed it and ride it, and the grid carries the
smoke on after the particles that made it have died.
- **Feeding it:** `VolumeSource{.volume = "air"}` has every particle of its
  emitter inside the grid deposit, into the cell it is in, `density` and
  `temperature` per second and `velocity` of its own velocity per second
  as momentum, each times `over_life` at its age. It runs after
  integration, from where the step left the particle.
- **Riding it:** `VolumeAdvect{.volume = "air"}` is a force: each step a
  particle inside the grid takes `strength` times the step (at most all) of
  the way to the velocity the grid holds at it, sampled trilinearly between
  cell centres, times its own factor between `scale_min` and `scale_max`.
  It reads the grid as the step began.
- **The step:** every emitter steps first, then every grid, in the effect's
  order, each in this fixed order: inject the step's deposits; buoyancy,
  lifting by `buoyancy` times the temperature and sinking by `weight` times
  the density along Y; vorticity confinement of `vorticity` (0 is off);
  semi-Lagrangian advection of the velocity and both scalars; pressure
  projection by `pressure_iterations` Jacobi iterations from zero pressure,
  with still walls all round; then each field loses its
  `*_dissipation` of itself per second, as `Drag` loses velocity.
- **Exact:** the deposits are quantised to `volume_deposit_quantum`
  (2^-20) and summed as 64-bit integers, so a cell's sum is exact and the
  same whichever order its particles are taken in, on the CPU in particle
  order and on the GPU through integer atomics. Every other operation of the
  step is fixed and reads only the previous pass, so the GPU kernels run it
  operation for operation as the reference does: with the confinement off a
  grid stepped on either is the same grid to the bit, and reruns, seeks and
  restores land on the same grid. The confinement's square roots and
  divisions round as the GPU rounds them, so with it on the two agree within
  tolerance.
- **State:** the grids are part of `SimulationState` (`volumes`), so
  `write_state`, checkpoints, `CheckpointRing::seek` and GPU checkpoints
  carry them. A grid starts empty at step 0.
- **Budget:** at most `max_volumes` (16) to an effect, `max_volume_size`
  (256) cells along an axis, `max_volume_cells` (1,048,576) in a grid and
  `max_volume_pressure_iterations` (256); a larger grid is refused with the
  memory its state would need (20 bytes a cell; the GPU keeps 108 a cell
  for its working buffers). A module must name a volume the effect lists,
  and a rigid physics emitter can hold neither module.
- **Caching:** an emitter that feeds a grid cannot be cached (a cache keeps
  particles, not what they deposit); an emitter riding a grid depends, in
  its digest, on the grid and on every emitter feeding it.
- **On the GPU:** every grid lives in one buffer (`GpuSimulation::
  volume_buffers`), which the update kernel is handed at `u4` where it
  feeds or follows one; the step is seven compute kernels, the Jacobi one
  dispatched as many times as asked. `read_volume` reads a grid back and
  `write_volume` hands one in.
- `Simulation::volume(i)` is a grid's cells on the CPU.

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
- A parameter is **bake-time** (the default) or **playback-time**
  (`ParameterDesc::timing`, `EffectBuilder::playback`). The simulation reads
  bake-time parameters; only the renderer's draw inputs read playback-time
  ones: `SpriteRenderer::tint`, `brightness`, `opacity` and `size_scale`,
  which multiply each particle as it is drawn. `validate` holds the line both
  ways, so a cache can trust that a playback value never changed what it
  holds. `draw_adjustment` resolves an emitter's draw inputs against
  parameter values, and `adjust_for_drawing` applies them to a copy of CPU
  particles about to be drawn. The reference GPU renderer does not apply
  them yet; a host's renderer applies them per draw.

### Actions

A host can act on a running effect (`include/ajfx/actions.hpp`): place a
body before the first step, drop one mid-run, push it, steer it by hand,
take it away, paint particles in. Actions are inputs like parameter values:

- An emitter that takes them has one `SpawnActions` module, which declares
  only the bounds (`max_per_step`, and `max_alive` for a rigid physics
  emitter's bodies). The actions themselves come from an `ActionProvider`
  handed to `Simulation::step`, `CheckpointRing::seek`, `RigidRunner::track`
  and `GpuSimulation::step`, so acting never changes the effect or its
  package. `ActionList` is a provider kept by emitter and tick.
- Every action is stamped with a tick and applied in that tick's step: a
  body spawned at tick t stands at t + 1, exactly where and as the action
  says, of the body kind it names, asleep until its `release` if that is
  later. A rigid physics emitter's tick-0 spawns stand before the first
  step, as a pile does: that is a stage's initial state. A particle
  emitter's spawns start where the action says, offset and thrown by the
  init modules; the GPU runtime bears them too.
- `Push` adds a velocity and a spin to a body; `Steer` drives a body towards
  a pose, as a hand holding it that lets go when the steering stops; `Remove`
  ends a body's life. All three name their body by its spawn index, so a
  pile's bodies can be handled too. `Blast` names none: it throws every body
  within its `radius` of its `position` out from there, `power` m/s at the
  middle falling to nothing at the edge (`blast_push`, as an effect's Blast
  does), after breaking the bodies thrown at least as hard as their kind's
  `Break::blast_threshold`, and throws loose props too. They act on bodies
  only.
- Spawns take the action's own `serial` as their spawn count rather than a
  running one, so taking an action away or moving it leaves every other
  spawn's random draws as they were. A spawn past the bounds is refused and
  counted (`EmitterState::refused`), never squeezed in.
- A provider is a pure function of (emitter, tick). A host that appends
  while a simulation runs hands over a snapshot and says which tick changed:
  `CheckpointRing::clear_from` and `RigidTrack::set_actions` keep what stands
  before it and work the rest out again. With `RigidTrack::set_lead(0)` a
  playing host appends at ticks not yet stepped and nothing is thrown away.
- A cache is keyed by the actions its emitters were baked with
  (`BakeSettings::actions`, `CachedEmitter::actions`), as far as its steps
  read them: another performance is a stale cache, while an act after its
  last step is not part of it. The cache stores those actions, so it plays
  without them (`Cache::open_as_baked`, `Cache::performance`).

## Caching

A simulation can be worked out once and played back (`include/ajfx/cache.hpp`):
- **State on disk.** `write_state` and `read_state` save the whole
  `SimulationState`, rigid solver state and grid volumes included,
  versioned and checksummed as the package is. A restored state carries on
  bit for bit.
- **Baking.** `bake` runs the effect from step 0 and writes a cache. Per
  emitter (`BakeSettings::modes`) it holds nothing (`Live`), every step's
  bodies (`Rigid`) or quantised particles (`Particles`), plus whole-state
  checkpoints for the live emitters' far seeks.
  - A rigid frame is 36 bytes a body: spawn index, pose and age, exact. Kind,
    size, life and colour are kept once per body, and colour over life is
    worked out again from the age, to the bit. No velocities.
  - A particle layer keeps an exact keyframe every `keyframe_interval` steps.
    Between them a particle that lives on costs 6 bytes of position delta,
    quantised to `position_step` and never drifting, with velocity (where the
    renderer faces it), rotation and orientation at 16 bits as they are
    drawn. Newborns are exact. Age, colour and size are worked out again
    exactly.
  - Refused up front: a rigid layer over `max_cached_bodies` (the engine's
    65,536) or a particle layer over `max_cached_particles` (262,144, past
    which simulating is cheaper than streaming). Refused after measuring: a
    cache over `CacheBudget` (4 GB, 256 MB read in any second), naming the
    emitter that weighs most.
- **Identity.** A cache is keyed by what its cached emitters depend on
  (`CacheIdentity`): the instance seed, the simulation rate, each cached
  emitter's `emitter_digest` (its modules, the meshes and fields it names,
  the bodies it collides with, the bake-time parameters it reads), the
  actions it and those bodies were baked with (`actions_digest`) and those
  parameters' values. Textures, kernels, looks, draw inputs and live
  emitters are not in it, so editing them keeps a cache. `Cache::open`
  refuses one that does not belong (`StaleCache`) and says why: another
  seed, a changed or missing emitter, another value of a parameter it read.
- **As baked.** A cache (format 4) also stores the performance it was baked
  with, at the steps it holds. `Cache::open_as_baked` opens it with its own
  instance seed, performance and bake-time values, which is what a host that
  has none of them does: a delivered effect plays as it was performed
  wherever it is placed. It still refuses a cache whose cached emitters
  changed or whose rate differs.
- **Playback.** `SimulationOptions::cache` plays a `Cache`. Cached emitters
  are read where they would be stepped, so a cached rigid emitter needs no
  solver, and live emitters meet its bodies exactly where they met the
  simulated ones. The cached emitters' bake-time values are pinned; live
  emitters follow the provider. Past the last frame, cached emitters hold
  it (`beyond_cache`). `CheckpointRing::seek` restores the cache's
  checkpoints when they describe the playing effect.
- **The file.** A sidecar beside its package, `name.ajfxcache`, never inside
  it: it is taken per placement seed and per bake-time value, re-taken
  without re-exporting, and taken by Studio or a farm alike, none of which
  should change the package's hash.
- **On the GPU.** `GpuSimulation` does not read caches itself. Created with
  `SimulationOptions::cache` under `RigidHosting::Host`, it treats every
  emitter the cache holds as hosted: each step writes the frame's state of
  it instead of stepping it, and steps the live emitters beside it.
  `RigidRunner::track` given the cache makes those frames on the runner's
  thread by reading the cache (a frame at a time going forwards, from the
  nearest keyframe on a jump), so a seek is a read; every rigid emitter must
  be cached (`RigidTrack::error` names one that is not). The live emitters
  agree with the CPU reference playing the same cache
  (`a_cache_plays_on_the_gpu_from_a_cached_track`).

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
  particles appear at the same steps with the same ages and spawn indices:
  each newborn's age and spawn index travel to the spawn kernel in the
  module data buffer, after the modules' own floats.
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
| `src/` | Library implementation; `gpu/` holds kernel generation and the GPU simulation; `volume.cpp` the grid volumes' reference step |
| `backends/d3d12/` | The D3D12 device and the DXC compiler (target `ajfx::d3d12`, Windows) |
| `src/rigid/`, `third_party/jolt/` | The Jolt rigid solver (target `ajfx::rigid`) and Jolt Physics v5.6.0, MIT licensed |
| `examples/viewer/` | `ajfx_viewer`: plays, scrubs, captures and exports effects |
| `examples/player/` | `ajfx_player`: a small media player for `.ajfx` files |
| `shaders/` | Module functions, shared kernels, the grid volumes' kernels and raymarch, and the sprite shaders in HLSL, embedded in the library at configure time |
| `tests/` | Unit tests, registered with CTest as `ajfx.*` |

Planned additions:
- a Vulkan backend;
- more of volumes: a staggered grid, open boundaries, fuel and combustion,
  and shadowing the particles by the smoke.

ajfx Studio, the authoring application, is beside the library in
[`../studio`](../studio/README.md).

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

**Lighting.** By default a particle is unlit: its colour is what it shows,
which is right for sparks, glows and anything that emits. With
`SpriteRenderer::lighting` an emitter takes the scene's light instead, and
smoke, dust and debris sit in the scene rather than on it.
- **Six directions.** Light reaches a particle through six directions of
  its own frame: right, left, up, down, towards the camera and away from
  it. The vertex stage sums what every light gives each direction; the
  pixel stage weighs the six by a normal or by lightmaps. The same basis
  serves both kinds, so a lit sprite and a six-way one match in one effect.
- **`lit`:** a sprite takes a rounded normal, facing the camera at its
  centre and turning towards its edge by `curvature` (0 flat, 1 a full
  sphere); a mesh keeps its own normals. `wrap` carries light past the
  terminator (0 stops there, 1 reaches all the way round at half strength),
  which is what makes a thin puff glow at its rim when lit from behind. `ambient` scales the
  sky/ground hemisphere light it takes.
- **`six_way`:** for smoke rendered offline into lightmaps. `texture`
  holds in RGB how much light each texel takes from the right, from above
  and from the front, and `lightmap` the same from the left, from below and
  from behind; the texture's alpha is the coverage. Both are light, not
  colour: author them linear. They share the flipbook grid and must be the
  same size (`check_textures` refuses otherwise). Sprites only.
- **`shadowed`:** a lit particle takes the scene's directional shadows,
  where the host has them. Unlit particles, ribbons and the reference
  renderer ignore it.
- **The reference renderer** lights under a `gpu::SceneLight`: one
  directional light and a hemisphere, passed to `render`. A host lights
  from its scene: the media server sums its lights per particle and
  samples its shadow cascades per pixel.

**Grid volumes.** The reference renderer draws the effect's grid volumes
before its emitters, so the particles draw over the smoke.
- **How:** one full-screen pass per volume raymarches the density through
  the grid's box in 64 steps along each view ray. Each step absorbs by the
  density times `Volume::extinction`, and scatters `albedo` of what it
  absorbed towards the camera, in `color`, lit by the `SceneLight`'s
  directional light through 8 steps of its own back out of the box, plus
  the average of the sky and ground. The step counts are fixed, so a frame
  is the same frame on every run and node.
- **What it does not do:** the smoke casts no shadow on the particles, and
  the particles none on it; there is no depth, so a particle in front of or
  behind the smoke draws over it alike. A host marches `VolumeBuffers`
  itself.
- `Camera::orthographic` now runs its depth from z = 1000 to z = -1000, so
  the projection has an inverse for the rays; nothing between is clipped.

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
  wrongly where their centres' order differs from their surfaces'. A host
  with a depth buffer, such as the media server, draws meshes with depth.

**Lights.** Some of an emitter's particles become point lights in the
host's lit scene, at most `light_count` at once.
- **Which particles.** A particle carries a light for its whole life or not
  at all: those whose spawn index is a multiple of `light_spacing()`. That
  spacing shares the emitter's particle bound out over its lights, so a
  light never jumps from one particle to another as others are born and
  die. An emitter with several spawn modules picks carriers within each
  module's births, so it may light a few more than `light_count` at once,
  one at most for each module beyond the first.
- **Per particle.** Each light's intensity and reach are picked between
  `light_intensity_min`/`max` and `light_range_min`/`max` for its particle.
- **Over its life.** The colour is the particle's, or with
  `light_own_color` the `light_colors` keys (by their alpha). `light_curve`
  scales the intensity. Both follow the particle's life, with up to eight
  keys each, so a light can fade in and out instead of popping.

**Colliders.** `SphereCollision`, `BoxCollision` and `SdfCollision` keep
particles out of a shape, or with `inside` within it. The response is the
same as `PlaneCollision`'s: back onto the surface, with restitution,
friction or `kill`. A box may be turned (`turn`, degrees about Y, then X,
then Z, as a prop is), and a rigid physics emitter meets it turned too.
`SdfCollision` follows a distance field, a
`fields/<name>` package entry that `make_distance_field` bakes. The CPU
reference and the GPU kernels sample it identically.

**Another emitter's bodies.** `BodyCollision` keeps an emitter's particles
out of the bodies of a rigid physics emitter it names: snow that lands on
the crates rather than falling through them. A body is met as the ball or
box its size describes, turned as the body is -- a box for a body kind that
is a box, a ball for one that is a ball or a model's hull -- and the
response is the plane's, with restitution, friction and `kill`.
- **Order:** every rigid physics emitter steps before any other, so the
  bodies met are this step's wherever the named emitter is listed. An
  emitter may hold one of these per emitter it meets, so one layer of dust
  settles on everything solid in the effect; the bodies are gathered once a
  step and shared between them.
- **How many:** each named emitter's capacity is held to
  `max_body_colliders` (16384). Each step its bodies are sorted into a
  uniform grid -- cells about as wide as the largest body, at most 32768 of
  them, grown until the bodies fit; a counting sort, so by cell and then by
  body index -- and a particle searches only the cells around it.
- **Exactly as every body in turn:** a particle still meets bodies in body
  order. The next it meets is the lowest-numbered body whose bounding ball,
  grown by the particle's contact radius and 1%, reaches it where the last
  one left it; a body it is not touching does nothing. So the grid changes
  the cost, never a bit of the result. A reach wider than three cells
  either side, or a position that is not a number, meets every remaining
  body in turn. `SimulationOptions::body_broadphase = false` turns the grid
  off, which is how that is proved (`ajfx_body_grid_bench` times both).
- **On the GPU:** the bodies and their grid are written each step into the
  emitter's fields buffer, after its distance fields, so a host binds
  nothing new. The kernel walks the grid as the reference does; snow on a
  crate agrees between them to within a thousandth of a unit.
- Particles are not carried along by a body they rest on: a body that
  moves out from under its snow leaves it behind. Snow that stays with what
  it settles on is accumulation, which the roadmap has.

**Wakes.** `Wake` stirs an emitter's particles with another emitter's
moving bodies: smoke dragged along behind a rolling boulder, dust pulled
after a thrown crate.
- **Reach:** `radius` times a body's half size from its middle, fading
  linearly to nothing at the edge, for bodies moving at `min_speed` or
  faster: a body at rest leaves its smoke alone.
- **Pull:** each step a particle takes k / (1 + k) of the way to the
  body's velocity, k being `strength` times the step times the fade times
  the particle's own factor (`scale_min` to `scale_max`), so any strength is
  stable.
- **Which bodies:** `bodies` scopes them by the named emitter's spawn
  sources and kinds. Where wakes overlap the strongest wins, the earlier
  body on a tie: the result does not depend on the order bodies are looked
  at in, and a crowd stirs no harder than one.
- Found through the same grid as `BodyCollision`, on the CPU and the GPU.
  A cache of bodies keeps no speeds, so a stirred emitter is cached with
  the bodies or they stay live.

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

  `density`, `friction` and `restitution` set its material, and
  `angular_damping` and `allow_sleep` the whole world's.
- **More kinds of body:** `bodies` lists other kinds (`RigidBodyKind`), each
  with its own shape and material, and a spawn module's `body` says which
  kind its newborns are -- 0 being the emitter's own, which is every body's
  unless it is given another. So a wall of crates, the pieces they break
  into and the bouncy ball that broke them are bodies of one world, each
  answering a knock in its own way. A kind that collides as a `hull` wraps
  the model its `mesh` names, or the renderer's own when it names none. At
  most `max_rigid_body_kinds` beyond the emitter's.

  A kind may also name the model its bodies are **drawn** as (`draw`),
  where the emitter draws meshes: a crate and the panels it breaks into are
  then each drawn as themselves, without being emitters of their own.

  A kind carries its own **size** as well (`scale`), multiplying the size
  the emitter gave at birth. An emitter has one `InitSize` for every
  particle, so without it a kind could only be the size every other body
  is, and a crate and the girder beside it could not be one world's bodies.
  A solver applies it once, where it builds the body's shape, so a body
  made by a birth, by a break or by a restore is the same size; what a
  `BodyCollision` meets and what a renderer draws take it too, so the shape
  and the model cannot come apart. A piece a body breaks into takes
  `BreakPiece::size` of its parent on top, which is a share of that parent
  rather than of the kind.
  `models_by_kind` says an emitter does this, and its records then carry
  each body's kind. Size is the particle's, so whatever spawns a body
  decides how big it is.
  - **In this renderer:** every model an emitter draws goes into one pair of
    vertex and index buffers, with a table saying where each kind's model
    begins and how many indices it has. One draw covers them all -- its
    vertex count is the largest model's, and a particle drawn as a smaller
    one folds the vertices past its own away -- so the drawing order is
    still the depth sort's, which matters here because there is no depth
    buffer.
  - **In a host:** the media server's particle pass instead draws the
    emitter model by model, over an order buffer of the particles drawn with
    each. It has a depth buffer to settle the order between models, and its
    rigid particles come from the CPU, which therefore knows each group's
    count without asking the GPU.
- **What acts on them:**
  - `Gravity`, bound or not;
  - `Drag`, as linear damping;
  - `Shockwave`: in its blast's step, before the world steps, every body is
    given `blast_push`'s speed (times its own factor) by
    `RigidWorld::push`, in birth order, and then every loose prop, as the
    blast is (a prop has no factor of its own). Fixed and driven props are
    moved by nothing;
  - plane, sphere and box colliders, which become unmoving bodies with
    their own restitution and friction.

  Every other force is refused with the reason, and so are `SizeOverLife`,
  distance fields, `kill`, colliders bound to parameters, and keeping
  particles inside a sphere. Births, lifetimes and colour over life are
  every emitter's.
- **Determinism:** props enter the world first, in the order they are
  listed, then bodies enter and leave in birth order, in the lowest free
  slots, each as the kind its particle is (`EmitterState::body_kind`). `Simulation::state()` carries the engine's own state, so
  checkpoints and seeks land exactly. Results are identical across runs,
  thread counts, and Debug and Release builds.
- **On the GPU:** `GpuSimulation` runs rigid physics emitters on the CPU
  with the same code, and writes their particles into the emitter's
  buffers every step. Sorting, meshes, lights and drawing are unchanged.
- **Breaking:** a `Break` module makes one kind of the emitter's bodies
  breakable. A body of that kind whose hardest contact in a step closed at
  `threshold` or faster comes apart at the end of that step, into the
  pieces the module lists, before the step's births. The world reports the
  contacts a break needs whether the host asked for any or not.
  - **A piece** is a body of its own kind at a share of the parent's size,
    placed at an `offset` (a share of the parent's size, turned by the
    parent's turn) and a `turn` of its own. Pieces shaped like pieces are
    kinds that collide as shard models' hulls: a crate comes apart into
    panels and slivers, not into smaller crates.
  - **What a piece takes from its parent:** its pose, its velocity plus up
    to `scatter` units a second outward along where the piece sat, up to
    `spin` degrees a second about an axis of its own, and the parent's
    colour, age and lifetime -- so the pieces of a crate are pieces of that
    crate rather than newborns. The emitter's init modules do not run on
    them. The scatter and spin are hashed from the parent's spawn index and
    which piece it is, so a seek over a break finds the same pieces going
    the same way.
  - **Cut from the model:** `cells_as_pieces` (`include/ajfx/fracture.hpp`)
    cuts a model into cells and hands back the body kinds, the pieces and
    the meshes to put in the package, so a body comes apart into pieces
    that were it rather than into models placed beside one another. The
    cells keep the model's frame, so a piece is `size` 1 at the cell's own
    middle. Cutting belongs to baking: a thousand-point model is a fifth of
    a second, and the hull is almost all of it.
  - **Again:** a piece breaks in its turn if its own kind has a `Break`. A
    cycle of kinds is refused, since breaking has to end.
  - **Blasted:** with a `blast_threshold` above zero, a body of the kind
    that a `Shockwave` throws at that speed or harder comes apart at the
    start of the blast's step, before the blast throws anything, so each
    piece is thrown from where it sits. Bodies farther out are only pushed.
    Zero leaves blasts to push. A piece's spawn index is a birth of the
    pieces' own source.
  - **Room:** a body breaks only if every piece fits the emitter's
    capacity. One that does not stays whole and its pieces are counted in
    `EmitterState::refused`: a budget is refused and reported, and a body is
    never half broken.
  - **Order:** breaks are taken in birth order and each body's pieces in the
    order they are listed, so the slots they take are a pure function of the
    step.
  - A kind decides what a body collides as, not what it looks like, so every
    piece still draws as the emitter's one mesh. Pieces that look like
    pieces need a mesh per particle, which the roadmap has.
- **What they hit:** with `SimulationOptions::contacts`, every step reports
  the pairs it found touching: `Simulation::contacts(emitter)` gives them,
  and `contacts_dropped(emitter)` how many would not fit. A world also
  collects what the effect itself reads -- every `Break`'s threshold and
  every `SpawnFromContacts` naming it, down to the slowest (`contact_floor`),
  at least the hardest `max_rigid_contacts` (or the capacity) a step --
  whatever the host asked. Each
  `RigidContact` names its two sides -- one of the emitter's bodies by its
  slot, one of its props, or one of its colliders -- where and which way
  they met, how fast they were closing when collision detection found them,
  and whether they met this step or have lain together since an earlier
  one.
  - **Its sides** are in a fixed order, the lower of the two by (what it is,
    its index) first, and the normal points from that one to the other, so a
    pair reads the same however the engine happened to order it.
  - **Its order** is ajfx's own (`contact_before`): the engine's internal
    order, and the thread count it ran on, do not reach it. A seek that
    lands on a step reports what stepping through it would have.
- **How many:** `min_speed` is the bound. Only pairs closing at least that
    fast are reported, so a settled pile reports none however large it is,
    and `limit` caps what one step reports and counts the rest. Past the
    limit the softest go (`contact_harder`), so slower contacts a host asks
    for never crowd out a hard one the effect reads: what the effect reads
    is the same whatever the host asked, on the CPU, through a runner or on
    the GPU. A simulation that asks for nothing and reads nothing collects
    nothing: the engine is not even listened to.
  - **The speed is the arrival speed**, taken before the step resolved the
    contact, which is the only moment a hit's force is known. A fast pair is
    found a step before it strictly touches, so that it cannot pass through,
    and that is the contact carrying the speed.

**Props.** `RigidPhysics::props` are the objects an artist placed in the
emitter's world: a bucket for debris to spill out of, a wall to pile
against, a crate to knock over. A prop is authored rather than born. It
stands in the world from before the first step, in the pose given, and never
dies.
- **How it moves:** `fixed` stands still for ever; `driven` is moved by the
  effect, from its pose to `to_position`/`to_turn` between `move_from` and
  `move_from + move_over` seconds, shoving whatever is in its way and never
  pushed back, so it can tip a pile over; `loose` is an ordinary body that
  gravity and other bodies move.
- **What it collides as:**

  | `shape` | What it is |
  | --- | --- |
  | `mesh` | the model's own triangles, exactly, hollows and all; `fixed` only |
  | `hull` | the convex wrapping of the model |
  | `sphere`, `box`, `cylinder`, `cone` | generated from the measurements below |
  | `hollow_box`, `hollow_cylinder`, `pipe` | generated, open at the top, as a few convex pieces |

  `mesh` and `hull` read `mesh`, the name of a package mesh as a renderer's
  is; the generated shapes read `half_size`, `radius`, `height`, `wall` and
  `segments` and name no model. A model is held in the package with its
  longest side one unit long, so `size` is that side's length.
- **A collision mesh cannot move.** It has no volume to take mass from and
  no inside, so contact against a moving one is unreliable; Jolt says so
  through `Shape::MustBeStatic`, and with assertions off a moving mesh body
  would silently take an infinite inverse mass. So `shape = mesh` is
  `fixed` only, and a `mesh` prop that moves is refused, naming the
  generated hollow shapes instead.
- **Hollow props:** those generated shapes are compounds of convex pieces --
  a disc and a ring of staves, or a floor and four walls -- worked out from
  their measurements rather than decomposed from geometry, so every node
  builds the same pieces, and they move properly. A bucket that stands still
  and only has to hold is better `fixed` with `shape = mesh`, which is
  exact; one that tips has to be a `hollow_cylinder`.
- **Where they are:** `EmitterState::props` holds each prop's pose after
  every step, in the order `props` lists them, and `RigidFrame` carries them
  too, so a host can draw them. A prop is not a particle and takes none of
  the emitter's capacity; at most `max_rigid_props` stand in one world.
- **What is refused:** a `mesh` prop that is not `fixed`; a `mesh` or `hull`
  prop whose mesh the package does not hold (`MissingMesh`, naming the
  prop); and measurements that make no shape, such as a wall thicker than
  the radius it walls.
- **Cost on a Ryzen 5800X:**
  - `rubble`, 400 hulls: 0.75 ms a step on one thread, 0.32 ms on four.
  - 4,000 hulls in a dense heap: 9.7 ms on one thread, 3.4 ms on four.
  - A checkpoint is about 135 bytes a body.

  `ajfx_rigid_bench` measures it and prints a state digest for comparing
  builds and machines.

The `rubble` preset throws 400 chips that pile on the floor and each other.
When a body's life ends, everything touching it wakes, so nothing stays
asleep in the air where it rested.

**Piles.** `SpawnPile` places `count` bodies before anything moves: a pile
of rocks that collapses when the effect plays, or a wall that stands until
something knocks it down.
- **When:** the pile is part of step 0's state, so a paused effect shows it
  and every seek to the start finds it. Its bodies come first in birth
  order. `Simulation::reset(parameters)` sets it up with the parameters'
  values at step 0; if the first step is given other values, it sets the
  pile up again with those, so stepping and seeking always agree.
- **Where:** on a lattice spaced by the largest pile body's reach (the ball
  its shape turns within) times `spacing`, filled from the bottom up and
  resting on `center`:
  - `heap`: staggered layers, each body over a gap below, taken as nested
    cones of `slope` degrees reach them, so the heap keeps its slope at any
    count;
  - `box`: aligned rows filling `width` by `depth`, as high as it needs.

  `jitter` moves each body across by up to that share of the spacing.
- **How:** init modules set each body up as any newborn (life, colour,
  size, orientation from `InitTumble`), but position, velocity and spin are
  the pile's: it starts still.
- **Held:** until `release` seconds the bodies sleep in the engine
  (`RigidWorld::add` with `asleep`), so nothing moves them unless another
  body knocks into them. At `release` they wake together, in slot order.
- An emitter with a pile needs `RigidPhysics` and room for every body.

**Off the frame path.** A host recording frames cannot step a seek's worth
of engine steps, or save the engine's state for a checkpoint, while it
records. `RigidRunner` (`include/ajfx/rigid_runner.hpp`) steps the rigid
physics emitters of every `RigidTrack` on a thread of its own, with the
solver's threads as its budget:
- `RigidTrack::frame(tick)` returns that tick's particles if the runner has
  them; otherwise it returns none, and the runner works towards the tick.
  It works up to `lead` steps ahead of the latest tick asked, so playback
  finds each tick ready, and keeps its own checkpoints for seeks back.
- A tick the frame path waits on comes before filling any track's lead,
  and tracks take turns of `turn` steps.
- `set_parameters` drops what the runner holds and works it out again.
- `GpuSimulation::create(..., RigidHosting::Host)` leaves rigid physics
  emitters to the host: stepping skips them, and `write_rigid` writes a
  frame's particles into the buffers drawing reads.
- A `RigidFrame` carries each emitter's `contacts` (and `contacts_dropped`)
  from the step that reached its tick, as `Simulation::contacts` gives them.
  The runner never hands out a tick it restored straight onto -- it steps
  into it from an earlier checkpoint -- so a frame's contacts are the same
  after a seek. `RigidRunnerOptions::contacts` asks for more than the
  effect reads, as `SimulationOptions::contacts` does.
- `GpuSimulation::step(parameters, frame)`, given the runner's frame of the
  tick the step reaches, lets a hosted simulation's particle emitters meet
  those bodies and be born of those contacts and bodies exactly as the
  reference's are, and writes the frame for drawing. A hosted step without
  a frame meets no bodies and bears nothing from them.
- The runner computes exactly what `Simulation` does, whatever order ticks
  are asked in.

## Sample viewer

`ajfx_viewer` is built when ajfx is the top-level project
(`AJFX_BUILD_EXAMPLES`), as the standalone presets configure it.

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
- `effect.json` holds the effect (format 10: the `spawn_actions` module; 9
  BoxCollision's turn; 8 SpawnPile's single shape; 7 the effect's `volumes`
  and the `volume_source` and `volume_advect` modules; 6 the renderer's `lighting`,
  `lightmap`, `curvature`, `wrap`, `ambient` and `shadowed`; 5 added
  per-kind/prop `draw_tint` and
  SpawnPile initial velocity/turn; 4 added the `wake` module; 3 added the
  `spawn_from_contacts`, `spawn_from_bodies` and `heat` modules and the
  `only` scopes; 2 blasts, the `shockwave` module and
  `Break::blast_threshold`). An empty scope is left unsaid.
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
files with a reason. A parameter's `timing` and a renderer's draw inputs are
written only when they are not the defaults, so a package that uses neither
has the bytes it had before they existed.

A cache travels beside its package as `name.ajfxcache` (see
[Caching](#caching)): a header naming what it belongs to, an index of each
cached emitter's frames and the checkpoints, then the frames, with an FNV-1a
checksum.

## Determinism rules

- Every random value is `particle_hash(emitter_seed, spawn_index, stream)`
  (`include/ajfx/random.hpp`, HLSL twin `shaders/ajfx_random.hlsli`). Never seed
  from a thread index, frame counter or clock. The stream comes from the
  module's id, so adding or reordering modules leaves the others' random
  values unchanged, and a new id for one module (an editor's seed) deals
  that module alone new values. The spawn index comes from the particle's
  own spawn module's count, so a spawn module added after the others leaves
  their particles unchanged too; moving a spawn module within the stack
  moves the particles of those it passes.
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
- A rigid solver's world is built with its statics and then its props, in
  the order the desc lists them and before any body exists. It is then
  driven in a fixed order: removals oldest first (an action's removals among
  them), wakes in slot order, the actions' pushes and steers in the
  provider's order, a blast's breaks and pushes (bodies in birth order, then
  loose props), the driven props' new poses, one step, additions in birth
  order into the lowest free slots. Every node must build the same Jolt source with the
  same defines (`third_party/jolt`), and update together.
- The contacts a step reports are put in ajfx's own order
  (`contact_before`), with each pair's sides in a fixed order of their own.
  An engine finds them on whichever thread reaches them first, so neither
  that order nor the thread count may decide what a host does with them.
  What the effect reads of them is the slowest it asks for and the hardest
  that fit, in that order, so what a host asks for besides cannot change it.
- Particles born of contacts and bodies are counted by hashes of the step
  and the contact's place or the body's spawn index, and set off by draws
  keyed by their own spawn indices; their births, starts and speeds are
  worked out on the CPU for the GPU too. Particles born of actions take the
  action's serial as their count, so an action taken away or moved leaves
  the rest as they were.
- What particles deposit into a grid volume is quantised and summed as
  integers, so the sum is the same whichever order the particles come in;
  the grid's step reads only the previous pass and uses only products and
  reciprocals worked out once on the CPU, never a division or square root
  the GPU might round differently (vorticity confinement excepted).

## Building

The library builds with the rest of ajfx; see
[Building](../README.md#building) in the ajfx README.

The GPU suites need a D3D12 device with shader model 6.2 (hardware, or WARP
with `AJFX_TEST_WARP=1`) and `dxcompiler.dll` with `dxil.dll`. CMake copies
them beside the tests from `AJFX_DXC_DIR`, by default the newest Windows SDK
that ships them. Without either, those suites report "skipped". `ajfx.d3d12`
runs the backend under the D3D12 debug layer; `AJFX_TEST_DEBUG_LAYER=1` turns
it on for `ajfx.gpu` too.

## License

Not yet chosen; it will be decided before the first public release.
