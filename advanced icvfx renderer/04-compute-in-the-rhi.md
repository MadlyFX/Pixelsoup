# 04 — Compute in the RHI

**Track 2. The single highest-leverage infrastructure item.**
Corresponds to plan step 7.

**Status: implemented on D3D12.** `compute` and `storage_images` are true and
mean it. What landed, what was deliberately left out, and how the open questions
were answered are at the bottom under **What was built**.

## Why

`DeviceCapabilities::compute` is `false` on both backends today, and the comment
in `d3d12_backend.cpp` says exactly why: the adapter supports it, but this RHI
exposes no callable compute operation, and advertising raw hardware potential as
a renderer contract would be a lie. Same for `storage_images`, which currently
means "this format and resource can be constructed", not "you can bind and
dispatch against it".

Almost everything downstream wants compute:

| Wants compute | Step |
| --- | --- |
| Cubemap convolution, prefiltered specular, BRDF LUT | [07](07-image-based-lighting.md) |
| TAA resolve | [08](08-temporal-antialiasing.md) |
| SSAO, contact shadows | [09](09-ambient-occlusion-and-local-light-shadows.md) |
| Bloom downsample/upsample, froxel fog, SSR | [11](11-bloom-fog-and-reflections.md) |
| GPU culling, clustered lighting, indirect submission | [12](12-culling-instancing-and-lod.md) |
| Denoising, DDGI, ReSTIR | later |

Some of these could be fullscreen raster passes. Bloom and TAA genuinely can.
But group-shared memory and scattered writes make the difference between a
practical implementation and a pile of ping-pong targets, and every remaining
item after track 3 requires real compute regardless.

## What already exists

More than you would expect:

- `TextureDesc` already declares `storage_read`, `storage_write`, `array_layers`,
  and `mip_levels`.
- The D3D12 backend already creates a UAV for a `storage_write` texture
  (`d3d12_backend.cpp`, around the texture-creation path) and already tracks it
  in `D3d12Texture::uav`.
- `ShaderStage::Compute` already exists in the RHI enum.
- The resource-state journal (`TextureStateMutation`, `transition()`) and the
  command-recording ledger already handle speculative state and rollback, so a
  cancelled list with dispatches in it can restore correctly for free.
- Descriptor heaps per command context, with the sampler-table reuse added
  recently, already carry the binding machinery.

## Seams

- `engine/rhi/include/mediaserver/rhi/device.hpp` — `ComputePipeline`,
  `Device::create_compute_pipeline`, a `ComputeDispatch` description, and
  `CommandList::dispatch`.
- `engine/rhi/src/d3d12_backend.cpp` / `d3d12_backend.hpp` — compute root
  signature, PSO, descriptor binding, `Dispatch`, UAV barriers.
- `engine/rhi/src/sdl_gpu_device.cpp` — `SDL_CreateGPUComputePipeline`,
  `SDL_BeginGPUComputePass`, `SDL_DispatchGPUCompute`. Optional: Advanced is
  D3D12-gated anyway, so SDL may keep reporting unsupported until something
  needs it. Say which, and make the capability honest either way.
- `engine/shaders/CMakeLists.txt` — a compute target profile (`cs_6_x`) and
  reflection output for compute entry points, using the same pinned DXC
  (`v1.9.2607`) and the same warnings-as-errors and dependency-tracking rules.

## API shape

Mirror `PulledDraw`, which is the established idiom here — a gathered
description rather than ten arguments, spans borrowed for the call, uniform bytes
opaque to the backend:

```cpp
struct StorageTexture final {
    Texture* texture{};
    core::u32 mip_level{};
    // Whole-resource view when array_layers is 1.
    core::u32 array_layer{};
    bool write{};
};

struct ComputeDispatch final {
    const ComputePipeline* pipeline{};
    std::span<const StorageBuffer* const> storage_buffers;
    std::span<const SampledTexture> sampled_textures;
    std::span<const StorageTexture> storage_textures;
    std::span<const core::u8> uniform;
    core::u32 groups_x{};
    core::u32 groups_y{};
    core::u32 groups_z{};
};
```

`ShaderDesc` already carries `sampler_count`, `storage_buffer_count`, and
`uniform_buffer_count`; add a `storage_texture_count` so the root signature can be
built without inferring anything.

## Design notes

- **Barriers.** A dispatch that writes a texture a later pass samples needs a
  transition to `UNORDERED_ACCESS` and back. Route it through the existing
  `transition()` helper so the journal records it and a cancelled list rolls it
  back. Add a UAV barrier for dispatch-to-dispatch on the same resource; that is
  a distinct barrier type and the journal does not currently model it.
- **No compute queue yet.** Everything on the direct queue. `GraphQueue::Compute`
  exists in the render graph model but async compute needs a second queue, a
  second fence, and cross-queue transitions; defer it and keep
  `async_compute = false`.
- **Refuse, never wait.** Descriptor or upload exhaustion is refused atomically,
  matching the existing rule that command acquisition polls and never blocks the
  frame path.
- **Capability honesty.** Only flip `compute = true` when a test dispatches and
  reads back a known pattern. Redefine `storage_images` at the same time to mean
  "bindable and dispatchable", and update the note in
  `advanced-renderer-status.md` that currently says it means format support only.
- **Determinism.** A dispatch's result must not depend on group scheduling order.
  No atomics-into-shared-accumulators where float non-associativity changes the
  answer, unless the result is provably order-independent. Two nodes running the
  same dispatch must get the same bits.

## Multi-node hazards

Compute makes it easy to write a reduction whose result depends on the order
groups happen to finish. Any pass whose output feeds pixels must be
order-independent, or must reduce in a fixed tree with a fixed group count.
This is the one new class of nondeterminism compute introduces.

## Tests

Extend `engine/rhi/tests/rhi_device_tests.cpp` alongside the existing native
D3D12 cases:

- A dispatch writes a known pattern to a storage texture; read back byte-exact.
- A dispatch writes a storage buffer; a second dispatch reads it after a UAV
  barrier and produces the dependent result.
- A dispatch on a cancelled command list restores texture state, matching the
  existing `native_d3d12_cancelled_lists_restore_texture_state` pattern.
- Descriptor exhaustion is refused rather than waited on.
- The same dispatch run twice produces byte-identical output.
- Capability flags match what is actually callable: a device reporting
  `compute == true` passes all of the above.

## Definition of done

`compute` and `storage_images` are `true` on D3D12 and mean it; a compute shader
compiles through the pinned DXC into the same reproducible build; a dispatch
binds sampled textures, storage textures, storage buffers and push constants;
cancellation rolls back; and nothing on the frame path waits.

## Open questions

- **SDL_GPU parity now or later?** Advanced is D3D12-only, so later is
  defensible — but then SDL's capability must keep saying `false`, and the Basic
  renderer can never use a compute pass.
- **Push constants versus a uniform buffer for dispatch parameters.** The
  existing draw path uses opaque uniform bytes at a root CBV; matching it is
  probably right for consistency even where root constants would be cheaper.
- **Do storage textures need full subresource views** (specific mip and array
  layer), or is whole-resource enough for now? IBL prefiltering needs per-mip
  per-face writes, so probably yes from the start.

---

## What was built

`Device::create_compute_pipeline` takes one `ShaderDesc` with
`ShaderStage::Compute` and builds a D3D12 compute root signature and PSO.
`CommandList::dispatch` takes a `ComputeDispatch` and records one dispatch on
the direct queue. `ShaderDesc` gained `storage_texture_count`, so the root
signature is built from what the shader declared rather than from what the first
dispatch happened to bind.

Two shaders compile through the pinned DXC at `cs_6_0`, with the same
warnings-as-errors, the same reflection output, and the same dependency tracking
as every raster stage — a compute entry point is not a second class of shader
with its own rules. `compute_fill.comp.hlsl` writes a value derived from each
thread's own coordinates; `compute_gather.comp.hlsl` reads a sampled texture, a
storage buffer, and a storage texture an earlier dispatch wrote, then writes a
second one. The second exists to prove all four binding kinds coexist in one
root signature at the right registers: a shader binding only one would pass with
three of them silently misplaced.

**A refused dispatch records nothing.** Every binding is resolved and validated
before the first barrier is written, so `false` leaves the command list exactly
as it was. This is stronger than the draw paths offer and it is not a nicety:
the first version validated per binding, after transitioning, and a storage
binding naming a texture created without `ALLOW_UNORDERED_ACCESS` recorded a
barrier into a state that resource cannot hold. The debug layer treats that as
fatal to the whole list, so the *next* legitimate dispatch failed at submit
instead. The refusal test found it on its first run.

**UAV barriers** are emitted when a storage texture bound to this dispatch was
already used by an earlier one on the same recording. They are not routed
through the state journal, deliberately: a UAV barrier changes no state, so a
cancelled list has nothing to restore. Whether the prior use was a read or a
write is not tracked, so the barrier is emitted for any prior use — only
read-after-read does not need it, which is the cheapest of the four orderings
and the only one this costs anything on. Tracking it would be more state to keep
correct than the redundant barrier is worth.

Transitions do go through the journal, so a cancelled list restores every
texture a dispatch touched, and a second recording may not touch a texture the
first has claimed. Both are asserted.

### The open questions, answered

**SDL_GPU parity: later, and the capability says so.** SDL keeps
`compute = false`, and `storage_images` went from true to **false** there —
because the flag was redefined. It used to mean "this format and resource can be
constructed", which SDL can do; it now means "a storage texture can be bound to
a dispatch and read or written through it", which SDL cannot. Reporting the old
answer under the new definition would have been exactly the lie this seam exists
to avoid. The stated consequence: the Basic renderer cannot use a compute pass
until SDL's dispatch path lands.

**Push constants versus a uniform buffer: uniform bytes at a root CBV**, the
same as the draw path. Consistency was worth more than root constants would have
saved, and the existing upload helper already did it.

**Full subresource views: yes, from the start.** A `StorageTexture` names a mip
and an array layer, and the UAV is created into the recording's own descriptor
heap at dispatch time rather than taken from the resource-wide descriptor made
at creation. That descriptor covers the whole chain and cannot address one face
of one level, which is exactly what prefiltering an environment map needs.
`a dispatch addresses one mip of one layer` walks every subresource of a
3-layer, 3-level texture, each its own dispatch sized to that level.

### Deliberately not built

**Writable storage buffers.** The API sketch above binds
`std::span<const StorageBuffer* const>` — read-only — and that is what was
built. The test list asks for a dispatch that *writes* a storage buffer, and
that is not possible against today's `StorageBuffer`: it is an UPLOAD-heap,
CPU-mapped resource, and D3D12 has no UAV on one. Making buffers writable means
a DEFAULT-heap resource kind with staged uploads and its own readback — real
work, wanted by [12](12-culling-instancing-and-lod.md) for GPU culling and
indirect submission, and wanted by nothing in
[07](07-image-based-lighting.md), [08](08-temporal-antialiasing.md),
[09](09-ambient-occlusion-and-local-light-shadows.md) or
[11](11-bloom-fog-and-reflections.md), which all scatter into textures. It
belongs with indirect draw, not here.

The dispatch-to-dispatch dependency that bullet was really checking is proven
through a storage texture instead, which exercises the same UAV barrier.

**Async compute.** Everything is on the direct queue and `async_compute` stays
false. A second queue, a second fence, and cross-queue transitions are their own
piece of work, and keeping the two flags apart is what lets `compute = true` and
`async_compute = false` both be true statements.

### Tests

Six new cases in `engine/rhi/tests/rhi_device_tests.cpp`, all skipping cleanly
when no D3D12 device is available:

- **a dispatch writes a storage texture exactly** — a known pattern in, the same
  bytes out, in `R32Uint` so every expected value is exact rather than a
  tolerance. Runs the same dispatch twice and compares byte for byte, because
  two nodes running identical work have abutting walls.
- **a dependent dispatch reads what the first wrote** — two dispatches on one
  list, the second consuming the first's output alongside a sampled texture, a
  storage buffer and uniform bytes, with every output value predicted from the
  inputs. Without the barrier this reads whatever the first dispatch had
  reached, which would pass a test that only checked for non-zero.
- **a cancelled dispatch restores texture state** — and a second recording is
  refused the claimed texture. Asserts the debug layer logged nothing.
- **a malformed dispatch is refused rather than waited on** — no pipeline, zero
  groups, too few bindings, a mip and a layer past the end, a write to a texture
  that never asked to be written, and a refusal on the *second* of two bindings,
  which is what proves validation precedes all recording rather than just the
  offending binding. Then a well-formed dispatch on the same list still records
  and submits.
- **dispatch descriptor exhaustion is refused** — the recording heap runs out
  and the call refuses without blocking or growing; the list stays live and
  keeps refusing, and a fresh recording works normally.
- **a backend without compute refuses distinctly** — `OperationUnsupported`, a
  new `RhiError` meaning this backend exposes no such operation on any input.
  Distinct from `InvalidConfiguration`, where a corrected request would work.
  The capability flag and the entry point are asserted together, because a flag
  that drifted from what is callable is the failure this seam exists to prevent.
