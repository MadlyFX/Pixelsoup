# 13 — Texture streaming

**Track 4.** Follows the residency contract already established by
`ModelCache`, `MediaCache` and `OutputProjectionCache`.

## Why

Photoreal sets are texture-bound before they are triangle-bound. A scanned
environment ships 4K–8K albedo, normal, roughness and AO maps per material, and a
set has hundreds of materials. Today a decoded model's textures are uploaded
whole at GPU adoption and retained, bounded by
`RendererLimits::max_model_texture_bytes` (512 MiB). Reaching that bound refuses
the newcomer and keeps the last complete answer — correct behaviour, but it means
a real set simply will not fit.

The renderer needs to hold the mips it is actually using and stream the rest.

## What the existing pattern gives you for free

The hard parts of this are already solved once and should be copied rather than
redesigned. From `ModelCache`:

- A process-shared, bounded CPU residency with one event-driven worker — no
  timer, no polling scan.
- Render observation through `try_lock` that performs no file, stat or codec
  work and returns pending / busy / sticky-failure / one immutable complete asset
  immediately.
- A publication epoch latched per command list, so a worker completion cannot
  become visible halfway through recording one output.
- Generation-fenced same-source refresh that retains the last complete asset
  until a replacement is complete, and discards late results.
- Residency that will not evict a Ready generation behind an outstanding command
  fence.
- Transactional, all-or-stale GPU adoption: candidate textures and the command
  receipt stay owned until the recording resolves, and only an accepted
  submission replaces the active asset.

Every one of those properties is needed here. The new work is mip-level
granularity and a residency *policy*, not new machinery.

## Scope: mip streaming, not virtual texturing

Full virtual texturing (page tables, feedback buffers, indirection textures) is a
large subsystem. Mip streaming gets most of the memory win for a fraction of the
work:

- Keep a **resident tail** — the lowest N mips of every referenced texture,
  always present. This is the guarantee that there is always *something* to
  sample, so a missing high mip is a slightly soft texture rather than a hole.
- Stream higher mips in and out against a budget.
- The sampler reads from whatever is resident; the shader does not know.

Do virtual texturing only if mip streaming provably is not enough.

## The determinism problem, and it is subtle

Each node sees a different slice of the canvas, so a screen-space "which mip do I
need" estimate gives a **different residency set per node**. Two nodes would then
sample different mips for the same wall pixel near a seam — one sharp, one soft.
It is a mild artifact compared with a lighting difference, but it is exactly the
class of fault this architecture exists to eliminate, and it would show up as a
soft band down the middle of a wall.

**Fix: compute the desired mip from the authored full-canvas projection.** Every
node then wants the identical set, budgets are identical (same build, same
limits), and eviction order is identical. Each node uploads the same mips even
though it only samples some of them — the memory cost is the same on every node,
which is also what makes capacity planning possible.

The corollary: streaming priority must never depend on what this node can see.
No "prioritise textures in my slice".

## Design

- **Desired mip** per texture per frame: a pure function of the full-canvas
  projection, the object's world bounds, and the texture's dimensions. Computed
  on the CPU during bridge diffing, where the bounds already are.
- **Budget** from `RendererLimits`, replacing or refining
  `max_model_texture_bytes`. Deterministic eviction: least-recently-desired,
  ties broken by stable asset identity — never by address or hash order.
- **Requests** go to the existing model/texture worker. Never a file read on the
  frame path; never a wait.
- **Uploads** ride the existing transactional adoption path. A partial mip upload
  must not become visible: same all-or-stale rule as models.
- **Format.** Streaming raw RGBA8 is wasteful; BC7/BC5 compressed source is what
  makes the budget work at all. That is an ingest concern
  ([14](14-material-model-and-ingest.md)) but the two should be designed
  together, since compressed mip sizes are what the budget is denominated in.

## Multi-node hazards

- Screen-space mip estimation from the local view. Covered above; this is the
  one.
- Eviction ordered by anything not replicated.
- A node whose upload was refused rendering a lower mip while its neighbour got
  the higher one. Because the desired set is identical and budgets are identical,
  this should only happen on genuine resource failure — and that should hold or
  report, matching how model integration refusal already behaves.

## Tests

Mirror the existing model and media cache suites, which already establish the
shape:

- No file, stat or codec work in render-facing observation — reuse the
  blocked-decode teardown test.
- Budget exhaustion refuses the newcomer and keeps the last complete answer.
- A publication epoch latched by a command list is not disturbed by a worker
  completion mid-recording.
- Generation-fenced refresh retains the previous texture until the replacement is
  complete; late results are discarded.
- **Two renderers with different slices request the identical mip set** for the
  same scene and camera. This is the determinism test and it is the reason for
  the full-canvas rule.
- Eviction order is identical across two runs and two instances.
- A resident tail is always present: a texture whose high mips were evicted still
  samples, at lower detail, never as an error.

## Definition of done

A set whose full texture working set exceeds GPU memory renders correctly at
reduced detail; every node holds the same residency set; nothing on the frame
path reads a file; and refusal keeps the last complete answer rather than showing
a hole.

## Open questions

- **Does the budget replace `max_model_texture_bytes` or sit under it?** Two
  budgets that can disagree is worse than one.
- **Compressed formats in the RHI.** `TextureFormat` has no BC formats today.
  Adding them is small, but it commits the ingest path to producing them.
- **Streaming during a transition.** A cut to a different scene wants a different
  working set immediately. The existing project-transition reset on `ModelCache`
  is the model, but a hard cut may need pre-warming — which is a scheduling
  problem, not a residency one.
- **Is this needed before [15](15-baked-global-illumination.md)?** Baked lighting
  adds lightmaps, which are themselves large textures with the same streaming
  needs. Doing streaming first may avoid designing lightmap residency twice.


## Implemented workflow (2026-09-06)

The Advanced renderer streams RGBA8 mip chains from the process-shared CPU
model cache. Lowest three mips remain resident; the full-canvas projection,
world bounds and a replicated bias choose higher detail. Allocation and upload
failure never select a cheaper local plan. See the status document for current
CPU and GPU limits and the remaining cross-process publication boundary.

To exercise it in the editor:

1. Open a glTF/GLB model with material textures through Library and place it in
   the scene. Let its initial model decode and mip upload finish.
2. Open **File > Project Settings > Renderer**, choose **Advanced**, and Apply.
3. In **Geometry**, leave **Mip streaming** on and **Mip bias** at 0. The
   **Texture memory** value includes replacement headroom: at the 512 MiB
   default, 256 MiB is available for the settled texture working set.
4. Reduce Texture memory and Apply. In **Last render**, watch resident and
   allocated bytes settle; large updates upload over multiple recordings.
   Scroll the Geometry pane if its status lines extend below the card.
5. Increase Mip bias and Apply to force softer detail. Bring it back to 0 to
   restore projection-driven detail. Moving the authored camera closer requests
   finer mips; orbiting a Stage view or changing an output slice does not.
6. Disable Mip streaming to request all source levels. If they cannot fit,
   rendering reports a capacity refusal and retains the previous complete
   answer. Re-enable streaming or increase the budget to recover.
7. Compare two output slices after loading settles: requested mip sets and
   settled residency bytes must agree. Prewarm before shooting; synchronized
   first publication of a newly decoded asset is not yet a cluster protocol.

Memory counters measure texture payload bytes, excluding driver alignment and
other render targets. Geometry, media/video, environment maps and CPU backing
have their existing separate capacities. A lower budget temporarily retains the
old allocation envelope until its smaller replacement can be adopted safely.
Raw image codecs and mip filtering run only on the model worker; repeated mip
requests need no disk access. CPU pyramids currently remain whole in the bounded
2 GiB cache. [Step 14](14-material-model-and-ingest.md) adds BC7/BC5 source blocks,
block-aware budgets/uploads and immutable model packages. Disk-level per-mip
reads remain future work; neither step implements virtual texturing.

The dedicated `mediaserver.render_texture_streaming` suite checks the policy and
real GPU behavior. The model cache suite also publishes non-default texture
payloads through refresh, stale snapshots and command publication epochs.
Full Debug and Release builds pass all 141 registered CTest entries in each
configuration. The native editor's streaming toggle, budget and bias controls
were also exercised with the settings draft canceled afterward.
