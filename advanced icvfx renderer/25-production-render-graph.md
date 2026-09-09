# 25 — Production render graph migration

**Implemented, 2026-09-08.** The D3D12 Advanced scene and post-processing passes
now execute through the existing render graph executor. This completes the
production migration deferred by [step 06](06-render-graph-execution.md).
There is no operator switch: selecting Advanced uses the graph automatically.

## Shipping path

`FrameGraph` derives texture access declarations from the actual RHI bindings,
compiles them in declaration order, and runs the new pass through
`RenderGraphExecutor`. The migrated work includes:

- Scene sky, directional cascades, reflective-shadow attributes, local shadow
  atlas, depth/normals/identity/velocity/object-velocity/coverage replays,
  opaque lighting, media and product copies.
- AO and its history, temporal resolve, spatial AA, physical object motion blur
  and plate depth of field.
- Fog scatter, integration and composition; bloom downsample, upsample and
  composition.
- Scene composition, depth merge, crop, transition mix, projection warp and
  output transform. Compositor scene layers use the same migrated renderer.
- Environment compute preparation, including filtering and SH/BRDF work.

This is the Advanced scene pipeline migration. Independent compositor effect
implementations such as layer keying and generic Gaussian blur retain their
existing recorders; they are not claimed as graph-native by this step.

## Execution and allocation contract

Each scene renderer or post-process component owns its graph storage, allocated
once at construction. A pass body is a function/context pair borrowing live
stack uniforms. Each appended prefix is validated and only its newest suffix
executes synchronously. No body, uniform or callback is retained after that
call, and recording adds no growing containers or per-frame heap allocations.

The compiler retains the existing bounds: 128 resources, 128 passes, 768
accesses and 768 transitions per plan. When an already executed serial segment
fills its pass/access arrays, `discard_executed_passes()` imports its written
products and starts another segment with the same texture identities and
executor bindings. This permits existing dense frames with hundreds of authored
layers to continue without truncation or increasing graph storage. The resource
bound still applies to each graph instance. Capacity and execution refusals are
counted and named; they do not trigger quality reduction.

Repeated identical sampled bindings declare one read. Clear/load flags become
Write/ReadWrite accesses. Sampled/target feedback is refused, compute
read-modify-write targets are declared explicitly, and copies name both source
and destination states. Transitions use the existing RHI mutation journal, so a
discarded command list restores the previous state claims. Step 24's GPU
timestamps bracket the executed transitions and body.

The graph remains serial on the graphics queue. It does not reorder passes,
alias memory or introduce async compute. Requests for unsupported queues remain
explicit executor failures. Basic SDL_GPU continues through its original RHI
operations because it does not expose explicit barriers.

## Per-surface transient ownership

Bloom's fixed pyramid and fog's scatter, integrated and output textures now live
in component-owned `GraphTransientPool`s. Construction and resize adopt the
same descriptors and allocations used by the previous recorder, then seal the
pool. Recording releases logical acquisitions with `begin_frame()` and acquires
only exact matching textures. A sealed descriptor miss or exhausted pool
refuses without allocating. Components belonging to separate Program, Stage or
mapped views therefore have separate pools.

Persistent scene products, histories and caches keep their existing owners and
are bound into the graph. No two transient textures share memory. Pool texture
counts/refusals and the scene graph's executed pass/transition/segment counters
are available in `RendererDiagnostics`.

## Proof and operating limits

The existing render-conformance fixture now renders through both the graph and
the original RHI recorder. It compares all 21 product digests over sixteen
frames, covering shadows, auxiliary products, temporal history, AO, fog, bloom,
motion blur, depth of field, composition and output. The reference selector is
thread-local and test-only; there is no authored fallback switch.

The 240-layer integration fixture asserts that more than 960 graph passes ran,
multiple segments were used, and no graph refusal occurred. Existing graph
tests retain read-before-write, queue, capacity and binding checks; the GPU
timing test verifies cancellation and pool exhaustion. The complete render
integration suite also retains its seam, restart and image checks.

On 2026-09-08, both GUI configurations built successfully and the complete
CTest suites passed: **155/155 Debug** and **155/155 Release** on the local
NVIDIA GeForce RTX 3090. Both include the sixteen-frame, 21-product
graph/reference comparison and the dense 240-layer fixture. The final Output
Monitor layout adjustment also passed the focused Debug workspace test.

This establishes the serial execution seam and preserves existing rendered
bytes on the local D3D12 fixtures. It does not establish cross-adapter equality
or physical wall synchronization; the step 23 cohort check and step 03 hardware
procedure remain the gates for those claims.
