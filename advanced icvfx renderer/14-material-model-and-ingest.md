# 14 — Material model and ingest

**Track 4.** Corresponds to plan step 12's material half.

## Implemented (2026-09-06)

The renderer and model importer support `KHR_materials_clearcoat`,
`KHR_materials_sheen`, `KHR_materials_transmission`, `KHR_materials_ior`,
`KHR_materials_specular`, and `KHR_materials_anisotropy`, alongside the existing
metallic-roughness core and `KHR_materials_emissive_strength`. The remaining
sections below preserve the original proposal; this section describes the
implemented contract and its limits.

### Material path

Sixteen new numeric components and nine additional map paths reach the document,
SceneModel, Inspector, scene digest, SceneBridge digest, model package and GPU
instance record. The components include base alpha, clearcoat and its roughness
and normal scale, RGB sheen and its roughness, transmission, IOR, specular weight
and RGB colour, and anisotropy strength and rotation in radians. SceneModel
clamps unit factors to 0–1, coat normal scale to 0–4, IOR to 1–3 and anisotropy
rotation to ±2π. Ingest diagnoses unsupported factor ranges instead of silently
changing the asset; imported anisotropy rotations may be any finite radians.
Neutral defaults retain the original core shading path.
An unconditional material version marker joins renderer readiness profiles.

Basic and Advanced share anisotropic GGX direct lighting, a GGX clearcoat layer
with an independent normal, Charlie/Neubelt sheen, and dielectric Fresnel driven
by specular colour, weight and IOR. Advanced also evaluates these layers under
the environment. Anisotropic environment reflection uses a bent-normal
approximation; sheen uses a fixed 32-sample hemisphere quadrature and conservative
energy attenuation. These are bounded realtime approximations, not a reference
path tracer or a material graph. Alpha MASK/BLEND and double-sided materials are
honoured by the surface shader. Sphere, cylinder and box X-face winding now
agrees with their outward normals so these primitives remain visible with
sidedness enforced.

Transmission is the glTF thin-surface case: a fixed nine-tap filter reads one
opaque geometry/background colour copy, with roughness/IOR controlling a radius of at most
eight authored canvas pixels. Fresnel reflection remains on the surface.
There is no thickness, volume absorption, thick refraction, dispersion, or
transmission through later composite media or other transparent layers. Basic reconstructs approximate
linear colour from its display target; Advanced uses its linear HDR target.
`KHR_materials_volume` is refused, as are other unsupported declared extensions.
Only UV0 without texture transforms is supported. Missing referenced images,
unsupported texture coordinates and unusable required tangents fail import with
a diagnostic; tangent generation includes coat-normal and anisotropy maps.

The texture budget is explicit: Basic binds 15 textures/samplers; Advanced binds
26 textures and 16 samplers by sharing the existing linear sampler for additional
material maps. D3D12 now separates sampled-texture and sampler counts, without
claiming bindless support. BC5 normal maps reconstruct Z in the shader.

Ordinary geometry retains the full-canvas projection and an offset raster
viewport for each slice within the native viewport limit. Larger canvases retain
their slice projection, including small node targets on a 65,535-pixel canvas.
Lighting reconstructs positions from absolute canvas
pixels, raster depth and the shared inverse projection, avoiding slice-relative
world-attribute interpolation. Texture coordinates, normals, and shadow queries
retain their original attributes. Transmission reserves an eight-pixel apron
from authored state for model scenes, independently of worker completion.
Sequential filter support adds transmission and bloom to the preceding filter
radius, fixing bloom sampling occlusion-invalid border pixels. A configured
apron limit that cannot hold this sum refuses the render; it never shortens a
filter on one node. Pixel comparisons establish agreement on the tested adapter,
not bit-identical floating-point output across arbitrary GPU architectures.

### Ingest and texture residency

Library **Import Model**, **Add Model**, and **UV Mesh Browse** use a shared
background ingest worker. The status bar shows queued, reading, filtering/
compression, writing, complete or a specific failure. Admission and result
observation do not wait for a worker lock. One job runs with at most 15 queued;
the editor retains at most 16 handles. Closing/changing a project drops handles
without joining a worker. Project generations fence late results; a projection
result also refuses to overwrite changed or deleted surface calibration.

Ingest prepares role-correct mip pyramids, splits images used in incompatible
roles, encodes colour/data as BC7 and tangent normals as BC5, and writes a
versioned immutable sibling named `source.<content-hash>.msmodel`. Texture bases
whose dimensions are not multiples of four remain RGBA8, preserving authored
UVs and dimensions. BC resources retain block-aligned base levels; smaller or
unaligned levels remain valid lower mips inside that resource. GPU residency
accounts for the actual block payload and supports partial-mip upload and
cancellation on SDL_GPU and native D3D12.

The package contains geometry, tangents, complete factors, all 14 map bindings,
compressed mip payloads, bounds and authored LODs. Nodes load this result without
parsing the original glTF or rebuilding material mips. Source files are unchanged;
a changed import produces a new package. The little-endian format validates
counts, ranges, finite numbers, complete mip chains, version and checksum, with
a 2 GiB file bound. The checksum detects corruption; it is not an authentication
mechanism. Existing valid packages are reused. BC7 encoder provenance and its
MIT licence are in `third_party/bc7enc`, and the Windows package includes that
licence.

Packages are still loaded wholly into the bounded CPU ModelCache. Disk paging
of individual mips and virtual texturing remain future work. Distribute the
immutable package through the existing asset workflow and prewarm each node;
this step does not add synchronized first publication or a cohort reload
protocol. Legacy raw glTF/GLB/OBJ paths remain readable, but new editor imports
reference the package. USD, MaterialX and material graphs stay outside this
ingest boundary.

### Try it in the editor

1. Select **Advanced** in **File > Project Settings > Renderer**, and light an
   object with a directional/point light or an environment map.
2. Select a procedural object, open **Properties**, scroll below **Effects**,
   and expand **Surface Layers**. Increase **Clearcoat** and lower **Coat
   roughness** for a narrow varnish highlight. Adjust sheen RGB/roughness for
   cloth, or anisotropy strength/rotation under an off-axis light for brushed
   metal. Numeric changes use the ordinary document edits and undo path.
3. In the Library's **Models** section choose **Import Model** and select a
   glTF/GLB/OBJ. Watch the status bar, then add the completed asset to the scene.
   glTF maps and factors come from the imported materials. Inspect the sibling
   `.msmodel` file; that is the asset the project now references.
4. For thin glass, import a material with transmission enabled, white base
   colour, IOR 1.5 and an opaque coloured object behind it. Roughness broadens
   the transmitted background. Two stacked transparent surfaces are outside
   the supported approximation.
5. Import a glTF declaring `KHR_materials_volume`, a missing material image or
   a non-UV0 texture. The status bar must report the refusal and no partial
   model should appear. **UV Mesh Browse** uses the same job flow and validates
   the projection before changing its surface.
6. Under **Project Settings > Geometry**, exercise the texture budget/mip bias
   on a large imported model. Let uploads settle before comparing output nodes.

Validation covers every new field's non-default value and clamps, both digests,
supported/unsupported glTF extensions, immutable package round trips and
corruption, worker success/failure, native lobe and transmission pixels, BC7/BC5
sampling of base and lower mips on both RHIs, upload cancellation, and slice
comparisons. The native editor was also exercised: changing clearcoat altered
the highlight, a six-extension model produced a package/library asset, and
`KHR_materials_volume` produced a visible refusal without adding an asset.
Complete build/test results are recorded in
[advanced-renderer-status.md](../advanced-renderer-status.md#step-14-material-model-and-ingest-2026-09-06).

## Why

`scene::Material` today is a clean metallic-roughness core:

```cpp
Vec3 base_color; float roughness; float metallic;
Vec3 emissive; float emissive_strength;
float normal_scale; float occlusion_strength;
MaterialAlphaMode alpha_mode; float alpha_cutoff; bool double_sided;
std::string base_color_texture, metallic_roughness_texture,
            normal_texture, occlusion_texture, emissive_texture;
```

That covers a great deal — it is the glTF 2.0 core, and the shader implements a
correct Trowbridge-Reitz / Smith / Schlick / Lambert response over it. What it
does not cover is most of what makes specific real surfaces read as real:

- **Car paint, varnished wood, laminate floors** — clearcoat. A second specular
  lobe over the base, with its own normal.
- **Fabric, carpet, velvet, foliage canopy** — sheen. Retroreflective grazing
  response that metallic-roughness gets visibly wrong.
- **Glass, water, plastic, skin** — transmission and IOR. Currently a window is
  either opaque or alpha-blended, neither of which refracts.
- **Brushed metal, hair, satin** — anisotropy. A brushed metal panel under a
  practical is a very common set surface and currently reads as isotropic.

These are not exotic. They are the four surface types a dressed set is mostly
made of, and each is a well-specified glTF extension that `model_loader` could
read from assets that already carry them.

## Seams

- `engine/scene/include/mediaserver/scene/scene.hpp` — new `Material` fields.
  Every one must reach `scene::digest` **and** the `SceneBridge` payload digest,
  or two nodes shade differently while believing they agree.
- `engine/scene/include/mediaserver/scene/scene_schema.hpp`,
  `engine/scene/src/scene_model.cpp`, `apps/server/playback.cpp` — keys, clamped
  read at the model boundary, write.
- `engine/render/src/model_loader.cpp` — glTF extension parsing.
- `engine/render/src/scene_renderer.cpp` — packing into the instance record.
  Note the current per-batch texture budget: five material maps plus four shadow
  cascades plus two RSM attributes is eleven of D3D12's sixteen sampler slots.
  Clearcoat and transmission want more maps, so this budget needs a plan before
  it is hit — see open questions.
- `engine/shaders/scene.frag.hlsl` — additional BSDF lobes.
- `engine/editor/` — Inspector rows for the new properties.

## Design notes

**Follow glTF, do not invent.** `KHR_materials_clearcoat`, `_sheen`,
`_transmission`, `_ior`, `_specular`, `_anisotropy` are specified, widely
exported, and match the shading model already in the shader. Inventing a
parallel parameterisation means every asset needs conversion and every
conversion is a place for a mismatch.

**Refuse what you do not implement.** An asset carrying an extension this build
does not understand must be diagnosed, not silently dropped — the same rule the
brightness curve already follows, where an unknown curve fails the read rather
than being guessed at. A material that silently loses its clearcoat renders
plausibly and wrongly, which is the worst outcome.

**Layered materials and a material graph are a different product decision.**
A node graph for materials is a large authoring surface with its own UI, its own
document schema, and its own evaluation model. It is probably the right long-term
answer for a set-dressing workflow, and it is explicitly out of scope here. Say
so rather than drifting into it.

**USD and MaterialX stay behind their own ingest boundary.** Plan step 3 and step
12 both say this and both are right: USD is a scene description with its own
composition semantics, and coupling it to renderer bring-up would mean two
half-finished systems. The shape that works is an explicit, operator-invoked
import that produces ordinary `scene::Material` values plus texture paths — never
a live USD stage on the frame path, never a USD type crossing into `engine/scene`.

**Texture compression belongs here.** [13](13-texture-streaming.md)'s budget only
works with BC7/BC5 source. Producing compressed, mipped textures is an ingest
step, and it fits the existing rule that content reads are operator-invoked
actions on a worker with visible progress, not frame-path work. Note that the
Model Import and UV Mesh Browse commands are already flagged in
`advanced-renderer-status.md` as synchronous with no progress display —
converting those to an asynchronous probe/result flow is the same piece of UI
work and should be done once for all of them.

## Multi-node hazards

Fewer than most steps, because materials are authored state — but:

- A new field missing from `scene::digest` or the bridge digest. This is the only
  real hazard and it is easy to make: two nodes with different clearcoat and the
  same digest is an invisible divergence.
- An import that produces different values on different machines (float parsing,
  locale, texture path normalisation). Import happens once on one machine and the
  result is replicated, so this is bounded — but the imported values must be
  fully replicated, not re-derived per node from the source file.

## Tests

- Each new field round-trips through the document and is clamped once at the
  model boundary.
- Each new field changes `scene::digest` and the bridge payload digest — a test
  per field, because the failure mode is silent.
- A glTF carrying each supported extension imports with the expected values.
- A glTF carrying an unsupported extension is diagnosed, not silently dropped.
- GPU: a clearcoat sphere shows a second specular lobe; an anisotropic sphere
  shows a stretched highlight along the tangent; a transmissive sphere is not
  opaque. Analytic where possible rather than golden images.
- Slice determinism with each new lobe active.

## Definition of done

The four common surface families render correctly, every new field is replicated
and digested, unsupported extensions are diagnosed, and imported assets carry
their authored appearance without hand-fixing in the Inspector.

## Open questions

- **The sixteen-sampler budget.** Clearcoat normal, transmission and sheen maps
  push past it. Options: bindless (`bindless` is honestly `false` today and would
  be a significant RHI step), a texture array with per-batch indices, or an
  atlas. Bindless is the real answer and is a prerequisite for GPU-driven
  submission in [12](12-culling-instancing-and-lod.md) anyway — worth deciding
  once, for both.
- **Which extensions first?** Clearcoat and anisotropy have the widest visible
  payoff on a set; transmission is the most work because it needs a refraction
  path and possibly a scene colour copy.
- **Material graph: ever?** If yes, the schema decisions here should anticipate
  it. If no, say so, because the Inspector's shape depends on the answer.
