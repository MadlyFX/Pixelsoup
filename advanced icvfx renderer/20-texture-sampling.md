# 20 — Texture sampling

Implemented 2026-09-07, following item 20 in
[the assessment](17-assessment-and-remaining-items.md). Tiled floors, walls,
and fabrics retain their authored addressing; grazing surfaces can use
anisotropic filtering. These settings apply to Basic and Advanced rendering.

## Authored behavior

Each of the fourteen supported glTF material texture views carries its own
`wrapS`, `wrapT`, minification filter, magnification filter, and mip filter.
Repeat, mirrored repeat, and clamp-to-edge are supported, including negative
UVs. Two views of the same image can use different samplers without duplicating
the image. All six glTF minification modes and both magnification modes are
preserved. An absent sampler uses repeat with linear/trilinear filtering.

Project Settings > Geometry > Texture Sampling exposes:

| Control | Values | Meaning |
| --- | --- | --- |
| Anisotropy | 1x, 2x, 4x, 8x, 16x; default 1x | Raises sampling quality on grazing surfaces with linear/trilinear samplers. Authored nearest or no-mip filters retain their meaning. |
| Sampling LOD bias | -4 to +4; default 0 | Negative values sharpen and positive values soften sampling within the resident mip chain. |

The existing Texture Residency > Mip bias controls which mip levels reside in
GPU memory. Sampling LOD bias is a separate signed value; it cannot recover a
mip level that is not resident. A no-mip texture view requires the original
base image. The residency planner refuses an insufficient budget instead of
substituting a lower-resolution base for that view.

Both new project fields are clamped, persisted, replicated, and included in
scene, renderer-settings, and temporal-profile digests. A sampling-contract
tag changes renderer capability evidence even at default values, preventing
an older clamp-only renderer from agreeing to the new profile.

## Implementation boundaries

- `rhi::SamplerDesc` supplies independent U/V/W addressing, min/mag/mip
  filtering, anisotropy, LOD bias and bounds, and optional comparison state.
  SDL GPU and D3D12 translate the same validated descriptor. The legacy
  `create_sampler(Filter)` overload retains its clamp behavior for media,
  shadows, and other existing callers.
- Each renderer constructs a fixed bank of 144 material samplers once.
  Frame selection is a bounded arithmetic lookup. D3D12 interns equal
  descriptors across renderers and reuses every complete sampler table in a
  recording, including alternating material batches. The shader-visible
  sampler heap remains bounded at 2048 descriptors; unique tables can still
  exhaust that bound and refuse work.
- The lit shader assigns samplers 0–13 to material views and shares two clamp
  samplers among Advanced's auxiliary textures. Opaque, instanced, composite,
  and transmitting model draws use the same binding helper. Reflective shadow
  maps use the base-colour view's sampler. Basic uploads the complete bounded
  mip chain so mip filtering and sampling bias also work there.
- The offline baker follows each base-colour, metallic, and emissive view's
  wrap modes and magnification filter. Its rays have no screen-space pixel
  footprint, so it samples the base image without the live camera's LOD bias
  or anisotropic footprint.
- Legacy specular/glossiness conversion preserves sampling when its inputs
  agree. Inputs requiring incompatible samplers are refused with a diagnostic
  rather than baked into one misleading texture view.

The `.msmodel` format is now **version 2**. Samplers and base-level residency
requirements participate in package bytes and content hashes. Version 1
packages are refused: reimport the original model to produce a version 2
package. This follows the repository's allowance for breaking development
formats rather than pretending an old package retained information it lost.

## Verification

`mediaserver.render_material_sampling` covers non-default project round trips
and digest changes, per-view glTF import, package identity and invalid-sampler
refusal, no-mip residency, and offline emission under all three address modes.

On both SDL GPU and D3D12, GPU readbacks verify negative/out-of-range UVs,
repeat/mirror/clamp, anisotropic sharpness at a grazing footprint, descriptor
LOD bias, repeated-frame equality, and invalid descriptor refusal. D3D12 also
records 2500 alternating draws and cancels/retries the recording, and creates
2500 identical sampler objects without exhausting the CPU descriptor heap.

An imported/package-backed quad with UVs from 0 to 4 renders four tiles through
Basic and Advanced, with separate base-colour and emissive samplers. Opaque
and composite images match adjacent independently rendered slices byte for
byte; held frames and biased sampling repeat exactly. Editor tests cover
selecting 8x, changing sampling bias, Apply, and Cancel.

These are independent slices on this machine's RTX 3090, not a two-machine
or cross-vendor certification. Anisotropic weights may differ across vendors
or driver versions. Item 23 remains responsible for cohort evidence.

Both full Windows GUI builds passed. The complete CTest suites passed
**145/145 in Debug (233.02 s)** and **145/145 in Release (198.45 s)**.

## Native editor walkthrough

1. Import a textured glTF with UVs extending beyond one tile through Library
   > Import Model, then place it in the scene. Its sampler modes are imported
   automatically; wrap modes remain asset authoring choices.
2. Open File > Project Settings > Geometry. Choose 8x or 16x anisotropy and
   Apply. Examine a tiled floor from a shallow camera angle.
3. Reopen Geometry and increase Sampling LOD bias to soften texture detail;
   compare with zero and a negative value. Keep the residency budget large
   enough to hold the detail being compared.
4. Change either control and Cancel to discard the draft. Apply the desired
   values, save the project, and reopen it to check persistence.

The native review used `out/step20-review.mstage`, a separate copy of the
lighting review project. A newly imported glTF floor repeated four times in
each direction in Stage and Program. Selecting 8x and increasing sampling bias
from approximately +2 to +4 visibly softened the floor; saving and restarting
restored the model and authored sampling values. The existing editor session
was preserved throughout.

## API references

- [glTF texture and sampler specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#textures)
- [SDL GPU sampler descriptor](https://wiki.libsdl.org/SDL3/SDL_GPUSamplerCreateInfo)
- [D3D12 sampler descriptor](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_sampler_desc)
