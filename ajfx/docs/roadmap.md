# ajfx roadmap

What ajfx is for, what is done and what remains. How the finished parts
work is in the [runtime reference](../lib/README.md) and the
[Studio guide](../studio/README.md); this page keeps only status, measured
results and open work.

## Why ajfx exists

There is no off-the-shelf GPU particle library whose simulation is
deterministic. ajfx's first host, the media server beside it in this
repository, needs exactly that: every render node must draw the same
particles at a given show frame, and a timeline seek must land on exactly
what playback would have shown. Replays and lockstep simulations elsewhere
need the same promise.

ajfx is deliberately a hybrid of the two ways an effect can be made. A
simulation is authored at whatever quality the shot wants, above realtime if
it has to be, and what it works out is then cached and played back at
realtime where it is displayed. Determinism is what makes that possible: a
bake is a pure function of the effect and the frame, so it can be taken once,
trusted, and replayed on every node. The same property serves the live path,
where a node resimulates rather than reading a cache. Caching is not built
yet -- [Caching and playback](#caching-and-playback) is the phase for it, and
today every host resimulates.

## What it delivers

Three pieces with one file format between them. A host plays effects; it
does not author them.

- **The runtime** (`lib/`): a GPU particle and volume library with no host
  dependencies. It loads effect packages, simulates and renders them. Effects
  are stacks of typed modules; custom modules are HLSL snippets fused into
  each emitter's kernel. A C++ builder API builds the same model in code, for
  tests, presets and engine programmers. The library defines a small GPU
  backend interface and can instead hand a host the passes to record into
  its own render graph. D3D12 and Vulkan reference backends ship with it.
- **The `.ajfx` package**: a versioned, self-contained file holding the
  emitters and module stacks, the textures, meshes and fields they use, the
  effect's published parameters (name, type, range, default) and kernels
  precompiled with the pinned DXC for each backend. Precompiled kernels keep
  shader compilation off a host's load and frame paths and give every node
  identical bytecode. A package is identified by content hash, so a host can
  treat it as an ordinary asset with a digest.
- **ajfx Studio** (`studio/`): a block editor built on the runtime. Effects
  are pages of layers and plain-language blocks, which a compiler turns into
  module stacks, merged curves and worked-out budgets. It has a scrubbable
  preview, budget and GPU cost readouts, raw modules for experts and export
  to `.ajfx`. The preview is drawn by the media server's renderer, so effects
  look as they will on the wall; the page model and compiler depend on ajfx
  alone.

Determinism rules: randomness is an integer hash of `(emitter seed, spawn
index, stream)`; a fixed simulation step; live lists compacted by prefix sum,
never atomic append; budgets fixed up front and refused when exceeded. Seeks
restore from a checkpoint ring and resimulate forward; emitters with no
feedback forces could evaluate any frame directly.

## Phases

0. **Skeleton, done.** Standalone CMake project, deterministic hash with an
   HLSL twin.
1. **Effect model, done.** Attributes, the built-in modules, builder API,
   presets, published parameters, the `.ajfx` package format (read and
   write, version checked, refused with a reason when unsupported) and a CPU
   reference simulator with checkpointed seeking. Tests: bit-identical
   reruns, seek equivalence, budget refusal, package round trip.
2. **GPU runtime on the D3D12 reference backend.**
   - **2a, done:** per-emitter fused update and spawn kernels generated from
     the module stacks, records holding only the attributes in use, a
     two-level prefix-sum compaction, indirect dispatch, checkpoints, a
     headless D3D12 device and a DXC compiler loaded at runtime. Spawning,
     deaths, ages and lifetimes match the CPU reference exactly and the rest
     within tolerance; reruns are bit-identical; clean under the D3D12 debug
     layer. About a million particles take 1.9 ms a step on an RTX 3090,
     untuned.
   - **2b, done:** graphics pipelines, render targets, indirect draws and
     window presentation in the backend; the sprite renderer; kernels
     precompiled into packages, keyed by a hash of their generated source so
     stale bytecode is refused; `ajfx_viewer` and `ajfx_player`. Rendering
     tests check pixels against hand-computed coverage.
   - **Open: tuning.** Candidates: structure-of-arrays records, writing
     survivors straight to the compacted buffer, a single-pass scan, and
     batching barriers across emitters. Measure with GPU timestamps first.
3. **A host's GPU, done.** Kernels take their constants at `b0, space1` and
   buffers at `u0`..`u7`, so packaged kernels run unchanged on a host whose
   compute layout matches. The media server runs them on its own RHI
   (`render::ParticleDevice`) and, on one GPU, leaves particle records
   byte-identical to ajfx's own D3D12 device.
4. **First host, done.** The media server imports packages as Library
   assets, places and animates them, and draws them in its scene renderer.
   What remains of it (two-node agreement on the cluster, catching
   up after a far seek) is in the media server's
   [`next-steps-plan.md`](../../next-steps-plan.md).
5. **Rendering depth.**
   - **5a, done:** deterministic depth sorting, and textured sprites with
     flipbooks.
     - **Cost of the sort on an RTX 3090:** about 0.2 ms up to 65,000
       particles, 0.5 ms at a million (1.9 ms before coalescing and the
       24-bit key), 1.9 ms at four million.
       - The floor is the 19 dispatches and their barriers. Fusing each
         pass's count into the previous scatter would lower it.
       - Eight-bit digits were tried and were slower: two splits per block
         cost more than the dispatches they saved.
       - Not yet measured on the RTX 2060.
   - **5b, done:** soft particles and ribbons (motion vectors are the
     host's, drawn from the same particles).
     - Not yet: several ribbons in one emitter, which needs a ribbon id per
       particle.
   - **5c, done:** sphere, box and distance-field colliders, mesh particles
     and particle lights.
     - The GPU matches the CPU reference particle for particle until
       contacts pile up. Where two colliders meet, a last-bit difference in
       the random spread can decide which a particle meets first, as with
       sparks on their floor.
     - Mesh particles are lit by a light at the camera until they take a
       host's scene lighting.
   - **Rigid particles, done:** tumbling debris (`InitTumble`, `RigidBody`).
     The GPU matches the reference particle for particle through contact and
     rest. Collisions between particles, stacking and contact by a mesh's
     own shape are left to rigid physics.
      - **Bodies as colliders, done (2026-11-01).** `BodyCollision` keeps an
     emitter's particles out of the bodies of a rigid physics emitter it
     names, so snow lands on the crates instead of falling through them. A
     body is met as the ball or box its size describes, turned as the body
     is. The emitter named must come first, so the bodies met are this
     step's, and its capacity is held to `max_body_colliders`, since every
     particle meets every body. The bodies travel in the module data the
     kernels already read each step, so the GPU needs no new binding and a
     host needs no new call. Measured: 340 flakes rest on one crate's lid
     and none reach the floor beneath it, where without the collider 321
     lie on that floor; the GPU and the reference put every flake in the
     same place (0 of 480 more than a thousandth of a unit apart).
     Studio authors it with the **Lands on pieces** block in a particle
     layer, which lands on the rigid layer above it on the page: the
     runtime wants the emitter it meets to come first, and reading down the
     page is that order, so no block names another layer and no page can
     name one that is not there. Its responses are the Ground block's
     (stick, slide, bounce, vanish), and sticking is what makes snow settle.
     The coach asks for a layer above where there is none, and refuses one
     holding more pieces than may be landed on. The **Crates in the snow**
     recipe is the worked shot: the crates breaking, with snow settling on
     what is left of them. Compiling a layer beside the layers already in,
     rather than alone, is what lets a block name an earlier emitter at all.
     - **Not yet:** particles are not carried by the body they rest on,
       which is accumulation below.
   - **Accumulation, not started.** Snow and dust that settle where they
     land and build up, while what they land on moves and breaks. Particles
     that stick deposit into a coverage buffer belonging to the body they
     hit, which drives a shell drawn over that body, so a crate's lid
     whitens and keeps its snow as it topples. Bodies as colliders are the
     half of it that lands the snow; what remains is a deposit target that
     follows a body, and a decision on how a host draws the shell. Doing it
     in the volume grid (V3 below) instead costs the detail of a thin layer
     on a lid. Today's nearest thing is a Ground block set to stick, which
     is friction 1 on one plane: flakes slide to a halt on the floor and
     nothing builds up.
   - **Excluded by design: depth-buffer collision.** It would make the
     simulation depend on one view's rendered depth, so nodes rendering
     different slices, or a seek, would disagree.
6. **Rigid physics.**
   - **Spike (2026-10-30): Box3D vs Jolt, one machine so far.**
     - **Scene:** 1000 and 4000 box chips flung from a lattice onto a
       floor, pillars and a turned wall, stepped at 60 Hz for 10 s. Each
       step hashes every chip's pose and velocities.
     - **Both are reproducible on one machine.** Every per-second hash
       matched across 1, 4 and 8 threads. Box3D also matched across its 4-
       and 8-wide SIMD paths, which it picks per CPU. Jolt's cross-platform
       build gives other bits than its default build, but each is
       consistent.
     - **Both seek exactly.** A restore taken mid-explosion (step 60) and
       stepped on to 600 matched the straight run at every step.
       - **Jolt:** restores into the same world, or a freshly built one as
         a late node would. 4000 chips snapshot to 610 KB in 1.8 ms and
         restore in 1.6 ms, through the public `SaveState` and
         `RestoreState`.
       - **Box3D:** seeks only through its recording player. Its world
         snapshot is internal, and each keyframe is about 9 MB for 4000
         chips.
     - **Cost on the Ryzen 5800X, 4000 chips, 8 threads:** Jolt 0.36 ms a
       step (0.38 cross-platform), Box3D 0.59 ms (4 substeps).
     - **Chosen: Jolt, in its cross-platform build.** It has a public,
       compact snapshot, restores into a fresh world, runs faster, and is
       mature. Box3D's determinism is just as good but it is at 0.x.
   - **Round 1, done (2026-11-01): Jolt in the library.** `RigidPhysics`
     emitters run by a host-supplied `RigidSolver`; `ajfx::rigid` provides
     one on Jolt v5.6.0 (`lib/third_party/jolt`), built cross-platform
     deterministic and SSE4.2-only so one binary runs on every node.
     Identical across reruns, thread counts, and Debug and Release; exact
     seeks and restores into a fresh simulation; the GPU runtime holds the
     reference's bits.
   - **Round 2, done (2026-11-01): hosting and piles.** `RigidRunner` steps
     rigid emitters off a host's frame path, and `SpawnPile` stacks bodies
     before step 0. Studio has Physics and Pile blocks and the Rock pile
     recipe. The media server hosts the runner in its particle pass.
   - **Round 3, in progress (2026-11-01): props.** `RigidPhysics::props`
     places authored objects in an emitter's world -- fixed, driven along one
     move, or loose. They collide as the model's own triangles (Jolt's
     `MeshShape`, exact and concave, and `fixed` only), its convex hull, or
     one of the generated shapes, of which `hollow_box`, `hollow_cylinder`
     and `pipe` are compounds of convex pieces so that a hollow prop can
     move. A `mesh` prop that moves is refused: a collision mesh has no
     volume to take mass from and no inside, and with assertions off Jolt
     would hand it an infinite inverse mass rather than complain. Prop poses
     travel in `EmitterState::props` and `RigidFrame`. Tests cover balls resting inside
     a hollow prop, a driven prop tipping what it holds, a loose prop
     settling, bit-identical reruns and thread counts, seek equivalence
     through a topple, and every refusal.
     Studio authors them with the **Prop** block (`Category::Physics`,
     beside Pile): a model, a pose, how it moves and what it collides as,
     with the model's outline and the collision shape drawn over the
     preview so the difference between them is visible. The
     **Bucket of stones** recipe is the worked shot, and the coach names what
     a prop cannot be with the fix that makes it possible.
     The media server draws them in its particle pass, through the same path
     as mesh particles, so a prop and the debris around it are lit alike.
     - **Not yet:** 3D gizmos for placing one (today's handles move its
       position across the floor and its radius, and the turn is typed);
       convex decomposition, so that a prop which moves could still collide
       as an arbitrary concave model rather than a generated shape.
   - **Round 4, done (2026-11-01): layer kinds.** A Studio layer is of one
     kind, `LayerKind::Particles` or `LayerKind::Rigid`, chosen when it is
     made. The kind, not the blocks a layer happens to hold, decides what
     moves it, and the catalog says per block, per control and per choice
     option which kinds have it (`BlockDef::layers`, `ControlDef::layers`,
     `ChoiceDef::layers`). The palette, the block picker, drag-and-drop and
     `move_block` all read that one answer, so what `check_rigid_physics`
     would refuse is never offered: Turbulence, Swirl, Attract, Draw in,
     Spin and Mesh collider are not in a rigid layer, and Pile and Prop are
     only there. `conform_block` clears what a kind hides, so a block never
     carries a setting no one can see. The Physics block is gone: those
     settings are the layer's own world (`Layer::world`), read as **Bodies**
     on the layer's heading, since a rigid layer has one world and no way to
     be without it. Changing kind is refused, naming the blocks in the way,
     rather than dropping anything authored. Older pages infer their kind
     from the Physics, Pile or Prop block that put them in a physics world,
     and the Physics block becomes the world; page format is now 2.
     Two silent losses were fixed with it: a rigid layer's Gravity, Rise,
     Wind and Air resistance put their strength in the force itself, since
     the engine reads neither a force's per-particle scale (so every slider
     was ignored, leaving exactly 1 g, 1 m/s^2 and a damping of 1) nor its
     curve over life.
   - **Round 5, in progress: breaking.** A crate that is hit hard enough
     comes apart into pieces. Four pieces in order; the first two are worth
     having on their own.
     - **Contacts out of the solver, done (2026-11-01).** `RigidWorld`
       reports the step's contacts and `Simulation::contacts` passes them
       on, a host asking for them through `SimulationOptions::contacts`.
       A `RigidContact` names its two sides (a body by its slot, a prop or a
       static by its index), where and which way they met, how fast they
       were closing when collision detection found them, and whether they
       met this step. The sides are put in a fixed order with the normal
       pointing from the first to the second, and the step's contacts are
       sorted by `contact_before`, so neither Jolt's internal order nor the
       thread count reaches them. `min_speed` is the bound rather than a
       filter after the fact -- a settled pile closes at nothing and reports
       nothing -- and `limit` caps a step with the rest counted, not quietly
       dropped. Jolt's own `SaveState` already carries the contact cache, so
       nothing new was needed for seeking: the list a step reports is a pure
       function of that step. Tests cover a block's one reported hit on the
       floor against its hand-computed arrival speed (5.07 units a second
       from 1.25 up) and its silence at rest, a settling pile's 1551
       contacts ordered and identical on 1 and 4 threads, 1297 of them
       matching step for step after a seek, the count beyond a limit of
       four, and a prop and another body each naming themselves as a side.
       - **Not yet:** the contact list stops at the CPU `Simulation`. A
         host reading contacts through `RigidRunner`, which is how the
         media server runs rigid physics, needs them on `RigidFrame`.
     - **Shape and material per body, done (2026-11-01).** Shape, density
       and friction, restitution and the hull's points are a
       `RigidWorldDesc::Body` rather than the world's, `add` names which kind
       a body is, and `RigidPhysics::bodies` authors the kinds past the
       emitter's own, which stays kind 0. A spawn module's `body` says what
       its newborns are, so a pile of crates and the ball dropped on them are
       one world's bodies: a bouncy kind came back 1.08 units above its
       lowest point where a dull one came back nothing, among crates that
       hardly bounce either way. A kind that collides as a hull wraps the
       model it names or the renderer's own, and `check_meshes` asks for it
       by the kind that wanted it. Refusals cover a spawn module naming a
       kind that does not exist, a kind named with no physics to make bodies
       in, a material out of range, too many kinds, and a hull with nothing
       to wrap. Package round trip, reruns, thread counts and seek
       equivalence are tested; damping and sleep stay the world's.
       - **Not yet, and what breaking needs on top:** a body is a sphere, a
         cube or a hull, scaled by one number, so a panel, a slab or a
         splinter cannot be a box at all. A box kind needs its own half
         extents, as a prop already has. (The one number is now the kind's
         own rather than the emitter's; see **A size per body kind** below.)
         And a kind decides what a body
         collides as, not what it looks like, since the renderer draws one
         mesh for the whole emitter. Pieces shaped like pieces therefore
         want both: extents on a box kind, or a shard model per kind, and a
         mesh per particle to draw them by, which is rendering work rather
         than physics. Fifteen kinds is also few for a shard library, so a
         piece set may need to carry its shapes rather than name kinds.
     - **A size per body kind, done (2026-11-01).** `RigidBodyKind::scale`
       multiplies the size the emitter gave at birth, so one world can hold
       a crate and the girder beside it. An emitter has one `InitSize` for
       every particle, so until this a kind could only be the size every
       other body was: an object that breaks has to be a body kind, and two
       breakable objects of different sizes could not be authored at all.
       Found while designing Studio's Object block, which wants exactly
       that.
       The scale is applied once, where the solver builds a body's shape, so
       a body made by a birth, by a break or by a restore agrees; and
       `body_colliders`, the pile lattice's spacing, the sample renderer and
       the media server's particle pass all take it, so what collides, what
       a `BodyCollision` meets and what is drawn cannot come apart. The
       media server groups a mesh draw by model *and* scale for that reason.
       A missing field keeps its default, so packages written before this
       read unchanged.
       Measured: a ball of size 0.4 rests with its middle at 0.2 and the
       same ball at half the size at 0.1, the radius each predicts; and one
       model drawn at both sizes covered 64 and 16 pixels of the test view,
       the 4:1 their sides predict. Tests also cover bit-identical reruns on
       1 and 4 threads, seek equivalence through a settle, the package round
       trip and the refusal of a scale that is zero, negative or not finite.
       - **Still not yet:** a box kind has no half extents of its own, so a
         panel or a slab is still a cube scaled by one number rather than a
         box. That is the other half of what pieces shaped like pieces want.
     - **The Break module, done (2026-11-01).** A `Break` module makes one
       body kind breakable: a body of that kind whose hardest contact in a
       step closed at `threshold` or faster comes apart at the end of it,
       into the `pieces` it lists, before the step's births. A piece is a
       body of its own kind at a share of the parent's size, at an offset
       and turn within the parent's pose, and it takes the parent's
       velocity, colour, age and lifetime with up to `scatter` outward and
       `spin` about an axis hashed from the parent and which piece it is.
       The emitter's init modules do not run on a piece: it is a piece of
       that crate, not a newborn. The pieces are authored rather than
       generated, so they are the panels and slivers a thing comes apart
       into; a crate's six panels are hulls of a shard model.
       Which kind breaks into what makes recursion the author's: a piece
       breaks again if its own kind has a `Break`, and a cycle of kinds is
       refused rather than counted by generations. A body breaks only if
       every piece fits the capacity; one that does not stays whole and its
       pieces are counted in `refused`, so nothing is half broken and no
       budget grows. Breaks are taken in birth order, pieces in listed
       order, and the world collects the contacts a break needs whether the
       host asked for contacts or not, down to the gentlest threshold.
       Measured: a crate hit by a dropped ball came apart at step 69 into
       six panels, which settled on the floor and lay still. Tests cover
       the break and its silence under a threshold nothing reaches, reruns
       and thread counts, six seeks across the break, the refusal with no
       room (and that one more place is all it needs), the package round
       trip, and every validation refusal including the cycle.
       - The body that breaks is itself solid: only a prop can be hollow (a
         compound of convex pieces), and a prop never dies, so a crate
         standing as a body is a closed lump until the moment it comes
         apart. A lid that caves in before it breaks is not this feature.
     - **A model per body kind, done (2026-11-01).** `RigidBodyKind::draw`
       names the model a kind's bodies are drawn as, so a crate and the
       panels it breaks into are each drawn as themselves. The media
       server's particle pass draws such an emitter model by model: one
       draw per model over an order buffer of the particles drawn with it,
       built on the CPU from `EmitterState::body_kind`, which the rigid
       frame now carries. The counts are the CPU's, so these draws are not
       indirect, and drawing model by model gives up the depth order
       between models -- the mesh pipeline writes depth, which is what
       settles it. Measured against hand-computed coverage: a cube and a
       half-size cube in one emitter covered 64 and 16 pixels of the test
       view, the 4:1 their sides predict.
       The sample renderer draws them too, and keeps the sort while doing
       it: every model an emitter draws goes into one pair of buffers with a
       table of each kind's index range, so one draw covers them all. Its
       vertex count is the largest model's and a particle drawn as a smaller
       one folds away the vertices past its own, which costs a little vertex
       work and buys the depth order -- grouping the draws by model would
       have composited them out of order, since this renderer has no depth
       buffer. The record carries each body's kind (`ParticleLayout`) only
       for an emitter that draws by kind. Tested with the near body drawn as
       one model and the far body as another, listed so that drawing model by
       model would hide the near one: the middle of the frame is the near
       body's colour, ringed by the far one's.
     - **Studio, done (2026-11-01).** Two blocks in a rigid layer, beside
       Pile and Prop (which an Object block later replaced; see **Studio
       authors things rather than arrangements** below, where a Break became
       one Object's rather than the layer's). **Break** says when the pieces
       come apart:
       how hard a hit it takes, how far the parts are thrown and how fast
       they tumble. **Part** is one of the parts a piece comes apart into:
       a model (the built-in shard, chip, rock or cube, or one of the
       artist's own), how big it is beside the whole piece, where it sat in
       it and how it is turned. Several Part blocks make the set.
       An artist never sees a body kind: the compiler makes one per Part,
       collides and draws it as that part's model, and gives it the layer's
       own material, since a part of a crate is made of what the crate is.
       Where the layer draws sprites a part still hits things as its own
       shape and the coach offers Appearance as the fix. Breaking is two
       halves and either alone does nothing, so the coach says which half
       is missing on the block that is there.
       The **Crates and a dropped ball** recipe is the worked shot: nine
       crates stacked, a loose bouncy ball dropped on them from two and a
       half metres, and four parts (a lid, two walls and a base). Measured
       in its test: nine crates become 33 bodies, so eight of the nine came
       apart.
       - **Not yet:** a Break block breaks the layer's own pieces, so a part
         cannot break again from Studio, though the runtime allows it; and
         a body kind of its own for a spawn block, so a bouncy ball among
         the crates is a loose prop rather than one of the layer's bodies.
     - **Studio authors things rather than arrangements, done (2026-11-01).**
       One **Object** block replaces Pile and Prop. An Object is one thing in
       the layer: its model, how many of it (just one, a heap or a stack),
       how big it is, how it moves, what it collides as, and the Break and
       Part blocks under it, which are its own. Breaking belongs to the
       Object above it, so a page of several things says which comes apart
       without any block naming another -- the page-order idiom that already
       decides which layer's pieces a particle lands on.
       This answers the complaint that started it: fracturing a mesh meant
       reaching for a Pile, and a second mesh to fracture it had to be a
       Prop, which cannot break at all. Now each is an Object, each at its
       own size (RigidBodyKind::scale, above), each with its own Break.
       The compiler decides what the engine makes of one: a body kind where
       the world moves it, a prop where it stays put, is driven, or is hollow
       so things sit inside it -- the artist meets neither word, and the
       coach speaks only where a combination is impossible. Every Object is a
       kind, every Part is a kind and every cell of a cut is a kind, so the
       compiler apportions `max_rigid_body_kinds` up front and refuses with
       what asked for them. It also sizes the layer for breaking, which
       `particle_bound` does not count, so a break is not refused mid-shot.
       Page format 3: a Pile or a Prop is converted as it loads, keeping its
       uid so module ids, seeds and therefore its bodies stay where they
       were, and a converted pile asks for the size its bodies were born at.
       Measured in its test: a crate cut into four and a boulder two and a
       half times its size break in one world, each Break naming its own
       kind; and the Crates recipe is now two Objects, where the ball was a
       prop that could never break.
       - **Not yet:** an Object cannot publish its size or pose for a host to
         animate, and a Part still cannot break again from Studio.
     - **Not yet after it:** snow settling on the pieces, which is
       accumulation, in phase 5 above.
   - **What breaking is, and what it is not (2026-11-01).** Round 5 is
     pre-fractured chunk substitution: a body is swapped for an authored set
     of pieces when it is hit hard enough. That is the realtime technique,
     and it is not what a fracture sim does. Measured against two references
     (a Blender Fracturing Tools wall, a concrete pipe struck by a metal
     one), what is missing is:
     - **Pieces that tile the thing they came from, done (2026-11-01).**
       `ajfx::fracture` (`lib/include/ajfx/fracture.hpp`) cuts a model into
       Voronoi cells that fill it, each a closed convex mesh with its middle
       and its volume, hashed from a seed and byte-identical for the same
       model and options. `focus` and `focus_bias` crowd the cells at an
       impact: 23 cells within a quarter unit against 2, averaging 24 times
       smaller. It cuts the model's convex hull, not its hollows -- an L
       beam of volume 0.75 fractures to its hull's 0.875 -- which is the
       limit a body already has, since one collides as its model's hull.
       Cutting is baking: a thousand-point model takes a fifth of a second
       and the hull is almost all of it, growing with the square of the
       points, so a scan rather than a crate would want quickhull.
       `cells_as_pieces` wires them to the Break module: it hands back a
       body kind a cell, collided and drawn as that cell, a piece putting
       each back where it was cut from, and the meshes to put in the
       package. The cells keep the model's own frame rather than each being
       fitted to a box of its own, which is what makes the pieces still
       tile; a piece is therefore size 1 at the cell's middle. Measured: a
       cube body hit by a dropped ball comes apart into exactly its 8
       cells, every one of them starting inside where the crate stood, and
       they settle no higher than 0.78 with all 8 at rest. Both budgets now
       hold a cut: 64 pieces and 64 body kinds.
       - **What 64 costs, and why it is the wrong number to cap
         (2026-11-01).** It was chosen rather than measured: 32 pieces and
         15 kinds blocked a cut into eight, so both went to 64. Measured
         since, in Debug, cutting a cube into N with a pile of 16 bodies: a
         seek costs about 0.37 ms for every body kind, because each kind is
         a shape and a seek rebuilds the world -- 2.2 ms at one kind, 5.7
         at 16, 25.4 at 64. Cutting itself is superlinear (0.5 ms to 59 ms
         over the same range) and does not matter, being bake time.
         The seek is the cost worth removing rather than capping. Those
         shapes depend only on the world's description, which does not
         change between rebuilds, so a solver that kept them would make a
         seek cost nothing for its kinds and the limit could then follow
         what a scene wants. Pieces are already bounded by the emitter's
         capacity, which refuses a break that will not fit, so the 64 there
         is belt and braces.
       Studio asks for one on the Break block: **Comes apart into** is
       either the Part blocks beside it or pieces cut from the model, with
       how many, how much they crowd at the middle, and a cut seed. The
       compiler only names the cut, since a model is not read until the
       package is built; `make_cuts` makes it once the models are in, adds
       a body kind and a mesh a cell and the Break that scatters them, and
       validates what it made before it goes anywhere. A cut that cannot be
       made leaves the layer playing unbroken and says why. The coach says
       when parts are placed beside a cut that ignores them.
       Measured: a page cutting a cube into six gives six kinds, six cells
       and six pieces, each at its own middle within the model and at the
       size it was cut; the same page cuts the same twice over, and another
       cut seed cuts it differently.
     - **Cracks that run, done (2026-11-01).** A `Glue` module holds the
       bodies born together where they touch, each join taking an impulse
       before it gives way, so a crack runs through the joins rather than a
       body breaking whole. Measured on a wall of 42 bricks under 67 joins,
       hit in the middle: all twelve bricks at the ends stay exactly where
       they were, where unglued only five of twelve do.
       Two things had to be the engine's rather than ajfx's. Jolt renumbers
       its constraints as they go and reads a checkpoint back by that
       numbering, so the join list is taken from the world in the world's
       order; and Jolt bakes a joint's rest from the world poses at the
       moment it is made, so a join carries its own rest and the bodies are
       put somewhere the join alone decides before it is made. Without the
       second, a seek landed 1.4e-04 from the step it should have.
       - **Not yet:** glued bodies are held by joints, not welded into one
         body, so a large network sags a little under its own weight (a
         wall of 42 settles 0.004) and costs a constraint a join. Islands
         welded into compound bodies would be stiffer and cheaper, and are
         what a sim does; they would also need island bookkeeping in the
         checkpoints.
     - **Debris at several scales.** The references carry big chunks, rubble
       and a carpet of gravel thrown metres out. `max_break_pieces` is 32
       and each piece is one authored model.
     - **An inside.** A broken face shows raw material, not the surface's.
       A body kind has one material and no notion of an interior.
     - **Dust the break drives.** Both references are mostly volumetric dust
       by the second after impact. That is the volumes work, below. Spawning
       particles where contacts are reported would be the cheap half of it,
       and the contacts are already there.
     - **Metal that dents.** The second reference deforms rather than
       breaking. Nothing in a rigid-body runtime does that.
   - **Cross-machine agreement is unverified.** Determinism is tested across
     reruns, thread counts, and Debug and Release on one machine; no second
     machine has printed the same `ajfx_rigid_bench` digest. Props widened
     what such a run would cover: mesh shapes, compound shapes, kinematic
     bodies and the first per-body rotation. The spike harness's
     `run_matrix.ps1` writes `results-<computer>` to diff.
   - **Later:** a loose prop that enters at a time of its own, or with a
     starting velocity, so a dropped object can be timed rather than aimed
     by the height it starts at; colliders that move with parameters; an
     emitter's props as another layer's colliders, as its bodies already are
     (phase 5's BodyCollision); distance-field and mesh colliders as Jolt
     mesh shapes for particles too; convex decomposition, so a loose prop
     can be any concave model rather than a generated hollow shape; an AVX2
     build once every node is known to have it.
7. **Custom HLSL modules and data interfaces.** Depth, distance fields,
   meshes, textures, curves and media that a host binds to an effect's named
   inputs; debug statistics and profiling markers.
8. **ajfx Studio.**
   - **First version, done:** the block editor (pages, layers, blocks, the
     "/" picker, drag to reorder, undo, saved `.ajfxstudio` pages); the
     compiler; recipes and ready-made layers; the coach and its fixes; raw
     Module blocks with Detach and Recombine; the preview with transport,
     framing, peak counts and GPU time; `.ajfx` export with kernels.
     `ajfx.studio` covers the page model and compiler headless.
   - **Done since:** ranges and per-block seeds, guides, handles and
     particle paths, the artist's own textures, debris models and mesh
     colliders, published parameters, the Object block and what it holds,
     and taking up where Studio was left.
   - **A rigid layer says what each block acts on, done (2026-11-01).** Such
     a layer holds blocks for four things at once -- its own pieces, what
     they break into, the props beside them and the world they share -- and
     a flat list said nothing about which, so Bodies set the pieces'
     material while a Prop three rows down carried its own and Break acted
     on neither. Its blocks now read under **The pieces**, **When they
     break**, **Props** and **The world**, with the layer's own Bodies row
     under the last of them. The grouping is the catalog's (`RigidScope`,
     `rigid_scope`), so one answer serves the page and anything that asks
     later. `Layer::blocks` stays one list in the order it was authored and
     nothing about the page format or the compiler moves: order is for
     reading, so drawing it grouped changes nothing else. A block's kind
     decides its heading, so a drag is offered only the gaps under its own
     and the drop line cannot promise a place the block will not stay --
     which `ajfx.studio_ui` covers by letting a palette block go at the
     wrong end of the page and reading where it landed. A particle layer
     has one scope and keeps its flat list.
     - **Not yet:** this says what a block acts on without changing what an
       artist has to build. Placing one object still means a Pile of one,
       a second object that breaks is still impossible (a prop has no body
       kind for Break to name), and the model of the thing that fractures
       still lives in Appearance.
   - **What it needed from the renderer:** `RendererLimits::particles`, so
     Studio can raise the particle step budget and an edited effect catches
     up in one frame; a raised `slice_apron_limit` for bloom on its single
     canvas. The renderer limits it found are in the media server's
     [renderer guide](../../docs/renderer.md).
   - **Not yet:** textures on imported debris models; per-block thumbnails;
     a timeline of parameter keys.
9. **Publication.** A Vulkan backend, documentation, examples, presets,
   Studio builds, a license and versioned releases.

## Caching and playback

A simulation authored above realtime, cached, and played back at realtime is
what ajfx is for. None of it is built: every host resimulates, so today an
effect's simulation has to fit a frame wherever it plays.

The ingredients are there and the path is not.

- `RigidRunner` steps a track's physics ahead of the playhead on its own
  thread, and the frame path picks up a finished tick or holds. So rigid
  physics races the show clock and has a policy for losing.
- `SimulationState` is the whole state, particles and solver state alike,
  and `Simulation::restore` rebuilds worlds from one. Nothing serializes it.
- The `.ajfx` package holds the effect, not what it works out: module
  stacks, textures, meshes, fields, published parameters and kernels.
- `CheckpointRing` is in memory and belongs to one timeline, cleared
  whenever the effect changes, because checkpoints from before describe a
  different timeline.

What a cache changes beyond its own cost: the caps phases 5 and 6 settled
become bake-time numbers rather than frame-time ones, physics leaves the
frame path, a far seek costs a read rather than a resimulate, and
cross-machine agreement becomes a matter of every node reading the same
bytes rather than each deriving them. It also moves the failure to watch
from disagreement to staleness, which is why B1 is identity before it is
bytes.

- **B1: state on disk.** A versioned serialization of `SimulationState`,
  refused with a reason on an unknown version exactly as the package format
  is. Jolt's `SaveState` and `RestoreState` already carry the solver half.
  A cache is identified by the effect package's content hash together with
  the bake-time parameter values it was taken with (B4), so a cache that
  does not belong to the effect in front of it is refused loudly rather
  than played. What proves it: a bake restored frame by frame equals the
  simulation it was taken from, bit for bit, including after a seek.
- **B2: a rigid cache.** Rigid physics first: it is the expensive half, it
  is what the frame path waits on, and its state is small. Each body needs
  its pose, its kind and a packed colour -- about 33 bytes, so 4,000 bodies
  are roughly 130 KB a frame and 8 MB a second at 60 Hz (arithmetic, not
  measured; Jolt's own full `SaveState` of 4,000 chips is 610 KB in 1.8 ms).
  A cached layer needs no runner, no lead and no hold.
- **B3: particles, per layer.** A million particles at 32 bytes a frame is
  32 MB a frame and about 1.9 GB a second at 60 Hz, against 1.9 ms to
  simulate the same step on an RTX 3090. So a cheap GPU layer is cheaper to
  resimulate than to stream, and caching is a per-layer choice rather than
  an effect-wide one. Keyframes at the checkpoint interval with quantised
  deltas between them, and a measured size and bandwidth per layer in
  Studio beside the budget it already shows.
- **B4: bake-time and playback-time parameters.** Open, and the decision
  everything else keys off. A cached simulation cannot honour a parameter
  the simulation reads, so published parameters have to divide:
  - *bake-time* -- rates, spawn radius, speeds, force strengths, break
    thresholds, cell counts. An operator's change invalidates the cache, so
    either the control is not published on a cached layer, or the cache is
    baked per value over the published range, or that layer stays live.
  - *playback-time* -- colour, brightness, opacity, overall size scale,
    time offset and the placement's own transform, which apply to cached
    state as it is drawn and cost nothing.
  `ControlDef::publishable` does not make this distinction today, and
  several publishable controls (spawn radius, speed, spread, turbulence
  strength) are squarely bake-time, so the media server's Inspector
  currently offers operators values a cached effect could not honour.
- **B5: Studio's bake.** Bake state per layer, the cached range on the
  transport, what a re-bake would cost before it is asked for, and a coach
  note when a layer's cache is stale because a bake-time setting changed
  under it. A layer is baked or live, and the page says which.

Open beyond B4: whether a cache travels inside the `.ajfx` package or
beside it as a sidecar keyed by the package hash (a package is already
content-addressed, which suits either); and how a cache is produced, since
a bake taken in Studio on one machine and a bake taken by a farm want the
same format but not the same trigger. The host half of this -- streaming
playback in place of simulation, and refusing a stale cache -- belongs in
the media server's [plan](../../next-steps-plan.md), as the rest of its
hosting does.

## Volumes

After phase 5, a companion module sharing the same backend, hashing, budgets
and checkpoints:

- V1: procedural volumes (noise and SDF shapes) injected into a host's
  froxel fog (the media server has one).
- V2: per-volume raymarching with self-shadowing, composited with sorted
  particles.
- V3: cellular flood-fill volumes through a voxelized scene.
- V4: sparse smoke and fire simulation, with particles depositing into the
  grid and advecting in its velocity.

V1 is the cheapest large gain in the plan, because a host's froxel fog is
lit by the scene's own lights through the same shadow lookup a surface
uses, so injected smoke is shadowed where a sprite plume is not. V4 wants
[caching](#caching-and-playback) under it: a grid stepped at authoring
quality is not a frame's work, and a cached grid is a streamed sequence.
Accumulation (phase 5) is the same shape.

Volumes are simulated once in world space and raymarched per node against
the full-canvas camera. Ray jitter is hashed from the show frame and canvas
coordinates; any temporal history resets with the host's temporal digest.
They need true 3D textures in the backend interface.

## Exit criteria

An effect authored in Studio and exported as `.ajfx` is reproducible bit for
bit on rerun and after a seek, stays within its declared budget or reports
why not, has a measured GPU cost, and plays in its host exactly as it
previewed. The host-side criteria (two-node slice agreement, loading without
stalling the control or frame path) are in the media server's
[plan](../../next-steps-plan.md).

Once [caching](#caching-and-playback) is in, one more: an effect baked in
Studio plays from its cache frame for frame as the simulation it was taken
from, within a declared size and bandwidth, and a cache that does not
belong to the effect in front of it is refused rather than played.
