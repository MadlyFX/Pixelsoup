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
   That work and what remains of it (two-node agreement on the cluster,
   catching up after a far seek) are tracked in the media server's
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
   - **Still to do before rigid effects go to a cluster:** a second machine
     (the Intel laptop) must print the same `ajfx_rigid_bench` digest as the
     Ryzen Director. This is also the spike's missing cross-machine run; the
     spike harness's `run_matrix.ps1` writes `results-<computer>` to diff.
   - **Later:** colliders that move with parameters; distance-field and mesh
     colliders as Jolt mesh shapes; an AVX2 build once every node is known to
     have it.
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
     colliders, published parameters, Physics and Pile blocks, and taking up
     where Studio was left.
   - **What it needed from the renderer:** `RendererLimits::particles`, so
     Studio can raise the particle step budget and an edited effect catches
     up in one frame; a raised `slice_apron_limit` for bloom on its single
     canvas. Renderer faults it found are recorded in the media server's
     plan.
   - **Not yet:** textures on imported debris models; per-block thumbnails;
     a timeline of parameter keys.
9. **Publication.** A Vulkan backend, documentation, examples, presets,
   Studio builds, a license and versioned releases.

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

Volumes are simulated once in world space and raymarched per node against
the full-canvas camera. Ray jitter is hashed from the show frame and canvas
coordinates; any temporal history resets with the host's temporal digest.
They need true 3D textures in the backend interface.

## Exit criteria

An effect authored in Studio and exported as `.ajfx` is reproducible bit for
bit on rerun and after a seek, stays within its declared budget or reports
why not, has a measured GPU cost, and plays in its host exactly as it
previewed. The host-side criteria (two-node slice agreement, loading without
stalling the control or frame path) are in the media server's plan.
