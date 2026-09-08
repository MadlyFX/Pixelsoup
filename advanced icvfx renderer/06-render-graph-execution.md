# 06 — Render graph execution

**Track 2.** Corresponds to plan step 10.

**Status: partially implemented.** The execution layer - the RHI seam, the
executor, the transient pool - is complete and proven byte-identical against
hand-recorded passes, and the real Advanced frame's declaration compiles. The
frame itself has **not** been migrated. See **What was built** at the bottom,
and its "Not done" section.

## Why

The Advanced frame is already several passes: up to four shadow cascades, two
reflective-shadow-map attribute passes, the opaque lit pass, composited layers,
media, and four auxiliary product passes. Track 3 adds IBL prefiltering, TAA
resolve, AO, bloom, fog, and reflections. Somewhere around ten passes,
hand-written barrier and target management stops being reviewable — the failure
mode is a missing transition that works on one driver and corrupts on another,
which is the worst possible bug for a system whose entire premise is that two
machines produce identical pixels.

`render::RenderGraph` already exists and already does the hard analytical part.
It does not execute anything, and `advanced-renderer-status.md` correctly calls
it a scaffold.

## What exists

`engine/render/include/mediaserver/render/render_graph.hpp`:

- Fixed capacities (128 resources, 128 passes, 768 accesses, 768 transitions),
  no allocation, no graphics API, no clock, no filesystem.
- `GraphResourceDesc` with `imported` / `exported` flags.
- `GraphPassDesc` with kind (Raster, Compute, Copy, AccelerationStructure,
  Composite, Warp, Output), queue, and declared accesses.
- `GraphState` covering colour/depth targets, shader read, storage read/write,
  copy source/destination, acceleration structure, present.
- `compile()` producing a `RenderGraphPlan` of `CompiledPass` plus
  `GraphTransition` records including `queue_sync` and source/destination queue.
- Validation errors that matter: `ReadBeforeWrite`, `ExportNeverWritten`,
  `DuplicateAccess`, `CapacityExceeded`.
- A passing test, `render graph compiles hazards and queue handoffs`.

So the declaration and compilation layer is done. What is missing is an executor
that turns a plan into recorded commands.

## Design

**An executor, not a framework.** Keep the graph a value-level compiler and add a
thin executor that walks the compiled plan, applies transitions through the
existing backend helpers, and invokes a caller-supplied callback per pass. The
renderer keeps owning what a pass actually records.

```cpp
// Sketch. The callback records the pass body; the executor owns only ordering
// and state.
class RenderGraphExecutor final {
public:
    using PassBody = bool (*)(rhi::CommandList&, void* context);
    [[nodiscard]] bool execute(
        rhi::CommandList& commands,
        const RenderGraphPlan& plan,
        std::span<const PassBody> bodies,
        std::span<void* const> contexts) noexcept;
};
```

Fixed-capacity spans, no `std::function`, no heap — the frame path rules apply
here as everywhere.

**Transient resources.** Passes need targets that live only inside the frame.
Allocate a pool at construction sized from the renderer's limits and hand out
whole textures by descriptor match. Do **not** alias memory between transients
initially: aliasing means a region's undefined contents differ between nodes
unless every transient is fully written before it is read, and proving that for
every pass is exactly the kind of thing that is right until it is not. Add
aliasing later, behind a test that clears aliased memory to a known pattern and
asserts no pass reads it.

**Migration, not a big bang.** The riskiest possible version of this step is
"rewrite the frame in terms of the graph and hope". Instead:

1. Express the **existing** Advanced frame as a graph and execute it.
2. Assert the executed frame is byte-identical to the hand-recorded frame, on
   the same device, for the existing GPU fixtures. That is the whole safety net.
3. Delete the hand-recorded path only once that holds.
4. From then on, new passes are added only through the graph.

**Backend mapping.** `GraphState` → `D3D12_RESOURCE_STATES` must go through the
existing `transition()` helper so the `TextureStateMutation` journal records it
and a cancelled command list still rolls back. Do not open a second path to
resource state; that is how the journal stops being complete.

**Async compute stays deferred.** `GraphQueue::Compute` and `queue_sync` already
exist in the model, but the D3D12 backend exposes no compute queue and
`async_compute` is honestly `false`. Compiling a plan that names the compute
queue should be refused at execution rather than silently run on graphics —
running it on the wrong queue is the sort of thing that is invisible until a
profile.

## Multi-node hazards

- **Pass ordering must be deterministic.** The compiler already walks passes in
  declaration order; keep it that way and never sort by anything derived from
  the local frame.
- **Transient contents must never be observable when undefined.** See aliasing
  above. If two nodes read uninitialised transient memory they read different
  garbage, which is a seam that appears only sometimes.
- **The graph must be built from replicated state.** Which passes exist is
  decided by the authored scene and renderer settings, not by what this node
  measured. A node that skipped bloom because it was behind is a node rendering a
  different image.

## Tests

- Plan equality: a known declaration compiles to a known transition list. Extend
  the existing graph test with the real Advanced frame's declaration.
- **Executed graph byte-identical to the hand-recorded frame** for every existing
  Advanced GPU fixture. This is the migration's proof.
- A cancelled command list containing graph-executed passes restores texture
  state, matching the existing cancellation test.
- A plan naming the compute queue is refused while `async_compute` is false.
- Transient pool exhaustion is refused and counted, not allocated around.
- Capacity limits: a graph past 128 passes reports `CapacityExceeded` rather
  than truncating.

## Definition of done

The current Advanced frame runs through the graph, produces byte-identical
output to the hand-recorded path on the existing fixtures, rolls back on
cancellation, and new passes in track 3 are added by declaring accesses rather
than by hand-writing barriers.

## Open questions

- **Where do the pass bodies live?** Free functions with a context pointer keeps
  it allocation-free but spreads the frame across many small functions. A
  fixed-size type-erased callable is the alternative. Pick one before there are
  twenty passes.
- **Does the graph own render targets, or does `SceneRenderer` keep owning
  them?** Imported-only is the smaller change and keeps ownership where it is;
  transient-owned is where the graph pays for itself. Probably imported for the
  existing products, transient for everything track 3 adds.
- **Per-surface graph instances.** Program, Preview, Stage and each mapped view
  need their own transients. Plan step 22 requires bounded, diagnosed residency
  for exactly this; the transient pool should be per-surface and counted.

---

## What was built

**Status: the execution layer is complete and proven; the production frame has
not been migrated.** Read the "Not done" section before treating this step as
finished.

### The RHI seam

`CommandList::transition(Texture&, ResourceState)` puts a texture into a named
state for the commands after it. It routes through the same `transition()`
helper every implicit transition already uses, so the `TextureStateMutation`
journal records it, a cancelled list restores it, and a second recording still
cannot touch a texture this one has claimed. There is no second path to
resource state - that is how the journal stops being complete.

`DeviceCapabilities::explicit_barriers` is now true on D3D12 and means it. SDL
stays false and refuses, gated on the capability rather than the backend kind so
the flag and the entry point cannot drift apart.

`ResourceState::ShaderRead` maps to both D3D12 read states at once. A planner
declares that a texture is read by a shader, not which stage reads it, and the
combined state is legal either way; a later implicit transition inside a draw
narrows it to the pixel state, which is one redundant barrier and no hazard.

### The executor

`RenderGraphExecutor` walks a compiled plan, applies each pass's transitions,
then records that pass through a body. Pass bodies are a function pointer and a
context pointer rather than `std::function` - the frame path allocates nothing,
and a fixed-size type-erased buffer is one more size to get wrong. The side
effect is that a body cannot capture, which is also what stops a pass quietly
using frame state it should have declared.

Everything it refuses is decided before a single barrier is recorded: a body
count that does not match the plan, a resource nothing bound, a state this RHI
has no equivalent for, a device with no explicit barrier, and any plan naming a
queue other than graphics. A plan that is going to be rejected must not leave
half its transitions on the list, or the caller holds a recording whose resource
states came from a frame that never happened.

`GraphQueue::Compute` is refused rather than run on graphics. The graph models
the compute queue and `async_compute` is honestly false; running those passes
somewhere else would be invisible until someone read a profile.

### The transient pool

`GraphTransientPool` hands out whole textures by **exact** descriptor match -
not "big enough", because a larger texture leaves a pass's viewport and its
target extent disagreeing and every sampling coordinate downstream comes from
the target extent. Exhaustion is refused and counted rather than allocated
around.

**Nothing is aliased.** Two transients never share memory even when their
lifetimes provably do not overlap. Aliased memory is undefined until written,
and two nodes reading undefined memory read different garbage - a seam that
appears only sometimes. Proving every transient is fully written before it is
read is exactly the kind of argument that holds until someone adds a pass.

### What the declaration test found

The plan-equality test declares the **real** Advanced frame - four shadow
cascades sharing one depth buffer, both reflective-shadow-map attribute passes,
the opaque lit pass reading every shadow product it just wrote, the media
composite, and the three auxiliary products against their own depth - and
compiles it.

It did not compile on the first attempt, which is the point of writing it before
moving any code. The declaration claimed the shared shadow depth was `ReadWrite`
on the first cascade, and the compiler correctly refused it as a read of
something nothing had written.

**The access mode mirrors the pass's load flag.** A pass that clears its depth
is a `Write`; only a pass that loads it is a `ReadWrite`. In the current frame
exactly two passes set `load` - the depth-only occluders and the media
composites - and everything else clears. Getting that wrong in a hand-written
frame is invisible; getting it wrong in a declaration is a compile error. That
correspondence is the single most useful thing to carry into the migration, and
it was worth the test on its own.

With the declaration corrected, the compiler produces exactly one transition for
the shared shadow depth across the six passes that use it in the same state -
a barrier per pass would be correct and wasteful, none at all would be wrong -
and takes every shadow map colour-target-then-shader-read, in that order, once.

### Tests

Three in `engine/rhi/tests/rhi_device_tests.cpp`:

- **an explicit transition is journalled and rolls back** - and a second
  recording is refused the claimed texture, and the debug layer logs no error.
  It filters on severity rather than message count: the layer also emits
  performance advice about clearing to a colour the texture was not created
  with, and this test picks a distinctive colour on purpose so the readback
  proves the clear happened.
- **a redundant transition is a no-op** - three requests into the same state,
  then a clear that wants that state anyway. This is what lets a plan
  pre-transition a resource that the pass body then transitions again on its own
  account.
- **a backend without explicit barriers refuses.**

Six in `engine/render/tests/renderer_integration_tests.cpp`:

- **a graph executed frame matches the hand recorded one** - the migration's
  safety net. The same two passes, once recorded by hand exactly as the renderer
  does today and once declared as a graph and executed from the compiled plan,
  compared byte for byte. It also asserts the frame is not uniformly one value,
  which a comparison between two blank images would otherwise satisfy.
- **the advanced frame declaration compiles with its hazards** - above.
- **the graph executor refuses before it records** - every refusal, each
  asserted to have recorded nothing, plus a failing pass body stopping execution
  with its own transition already recorded, because a barrier has to precede the
  work it guards.
- **the transient pool reuses and refuses.**
- **graph states map to resource states** - a switch, asserted, because a silent
  numeric correspondence between two enumerations survives right up until
  someone inserts a value in the middle of one.
- **a graph past capacity is refused** - passes and resources both. A truncated
  plan is a frame missing passes, which draws something plausible.

## Not done

**The Advanced frame still records by hand.** Migrating it is the bulk of this
step and it has not started. `SceneRenderer::render` is a single function of
around three thousand lines in which the passes are interleaved with the local
state they consume - `shadow_view` rebuilt per cascade, `shadow_draw` mutated
between the reflective-shadow-map passes, `composite_draw` assembled per layer.
Turning each into a free function and a context struct means hoisting all of
that, and doing it quickly is precisely the "rewrite the frame in terms of the
graph and hope" this plan warns against.

What exists now is everything that migration needs and none of the risk: an
executor proven byte-identical against hand-recorded passes, a declaration of
the real frame proven to compile, and the load/access correspondence that first
declaration got wrong. The remaining work is mechanical and reviewable one pass
at a time, and the byte-identical test is the gate for each of them.

Also still open, and unchanged by this step:

- **Pass bodies as free functions** is the choice made here, per the plan's open
  question. It stays allocation-free and it spreads a frame across many small
  functions; that trade should be re-examined before there are twenty passes,
  not after.
- **The graph owns no render targets.** Everything is imported and bound by the
  caller, which keeps ownership where it already is. The transient pool exists
  for what track 3 adds rather than for the current products.
- **Per-surface transient pools.** Program, Preview, Stage and each mapped view
  need their own, counted. The pool is per-instance and bounded, so this is a
  matter of giving each surface one rather than of changing the pool.
- **Aliasing**, behind the test described above.
