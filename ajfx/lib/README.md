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

The runtime, its first host and rendering depth are done; tuning, custom
modules, a Vulkan backend and volumes are next.
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
  - soft particles and ribbons;
  - mesh particles and particle lights;
  - sphere, box and distance-field colliders;
  - rigid particles for debris;
  - rigid physics emitters run by Jolt (`ajfx::rigid`), whose particles
    collide with each other and pile up;
  - piles that stand when an effect starts and collapse as it plays;
  - props an artist places in a rigid physics world, and the contacts its
    bodies report;
  - `RigidRunner`, which steps rigid physics off a host's frame path.

The [roadmap](../docs/roadmap.md) has what remains.

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
| Spawn | `SpawnRate`, `SpawnBurst`, `SpawnPile` (rigid physics only) |
| Init | `Lifetime` (required), `ShapeLocation` (point, sphere, box, disc), `Velocity` (cone), `InitColor`, `InitSize`, `InitRotation`, `InitSpin`, `InitTumble` |
| Update: forces | `Gravity`, `Drag`, `CurlNoise`, `Vortex`, `Attractor`, `Converge` |
| Update: after integration | `RigidBody`, `PlaneCollision`, `SphereCollision`, `BoxCollision`, `SdfCollision`, `ColorOverLife`, `SizeOverLife` |
| Update: instead of all motion | `RigidPhysics` (needs a rigid solver, see below) |
| Update: coming apart | `Break` (rigid physics only) |
| Update: hitting another emitter's bodies | `BodyCollision` |

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
| `examples/player/` | `ajfx_player`: a small media player for `.ajfx` files |
| `shaders/` | Module functions and shared kernels in HLSL, embedded in the library at configure time |
| `tests/` | Unit tests, registered with CTest as `ajfx.*` |

Planned additions:
- a Vulkan backend;
- `volume/`, raymarched voxel volumes.

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

**Another emitter's bodies.** `BodyCollision` keeps an emitter's particles
out of the bodies of a rigid physics emitter it names: snow that lands on
the crates rather than falling through them. A body is met as the ball or
box its size describes, turned as the body is -- a box for a body kind that
is a box, a ball for one that is a ball or a model's hull -- and the
response is the plane's, with restitution, friction and `kill`.
- **Order:** the named emitter must come before this one, so the bodies
  met are this step's. A later one is refused rather than leaving the
  answer to the order emitters happen to be in.
- **How many:** every particle is tested against every live body, so the
  named emitter's capacity is held to `max_body_colliders`. The cost is
  declared up front rather than discovered.
- **On the GPU:** the bodies travel in the module data the kernels already
  read each step, so a host binds nothing new. The kernel and the
  reference do the same arithmetic in the same order; snow on a crate
  agrees between them to within a thousandth of a unit.
- Particles are not carried along by a body they rest on: a body that
  moves out from under its snow leaves it behind. Snow that stays with what
  it settles on is accumulation, which the roadmap has.

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
  - **Again:** a piece breaks in its turn if its own kind has a `Break`. A
    cycle of kinds is refused, since breaking has to end.
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
  and `contacts_dropped(emitter)` how many would not fit. Each
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
    and `limit` caps what one step reports and counts the rest. A
    simulation that asks for nothing collects nothing: the engine is not
    even listened to.
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
- A rigid solver's world is built with its statics and then its props, in
  the order the desc lists them and before any body exists. It is then
  driven in a fixed order: removals oldest first, wakes in slot order, the
  driven props' new poses, one step, additions in birth order into the
  lowest free slots. Every node must build the same Jolt source with the
  same defines (`third_party/jolt`), and update together.
- The contacts a step reports are put in ajfx's own order
  (`contact_before`), with each pair's sides in a fixed order of their own.
  An engine finds them on whichever thread reaches them first, so neither
  that order nor the thread count may decide what a host does with them.

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
