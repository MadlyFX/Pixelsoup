# Materials and lighting v1

Step 10 adds a bounded look translation to the existing evaluated scene link.
Qualification uses Blender **5.2.0 LTS, fbe6228777e7**, add-on **0.5.0**, and the
Windows D3D12 Advanced renderer. This is a property translation, not a promise
of matching Cycles or Eevee pixels. See the [UI walkthrough](ui-checks-step-10.md).

## Material contract and importer audit

The publisher reads one Principled BSDF connected directly to Material Output.
Materials without nodes retain their diffuse color, roughness and metallic values.
All RNA access uses Blender's main thread; file reads, hashing, GLB serialization
and texture ingestion run on workers. Evaluated materials use their **original
datablock** for identity, including modifier-created material slots.

| Blender property / graph | Published representation |
| --- | --- |
| Base Color | Linear RGB factor, or direct sRGB Image Texture with factor one |
| Roughness / Metallic | Factors in 0..1; or one Non-Color image through Separate Color RGB, green to Roughness and blue to Metallic |
| Normal | Non-Color image -> tangent-space Normal Map -> Principled Normal; constant strength |
| Emission Color / Strength | Linear color or direct sRGB image; separate `KHR_materials_emissive_strength` |
| Alpha | Explicit add-on Opaque / Mask / Blend setting; Mask cutoff; scalar alpha or base image's Alpha output |
| Backface culling | Inverse of glTF `doubleSided` |
| Coat Weight / Roughness | Constant `KHR_materials_clearcoat`; default coat IOR and white tint |
| Transmission Weight / IOR | Constant `KHR_materials_transmission` / `KHR_materials_ior`; thin-surface renderer approximation |
| Specular IOR Level | Twice the Blender value as `KHR_materials_specular.specularFactor`, within glTF's 0..1 range; white tint |

The actual importer consumes **TEXCOORD_0 without texture transforms**. The
publisher copies the active render UV map, converts `(u,v)` to `(u,1-v)` for glTF,
and uses the importer's tangent generation. Normal maps use the glTF/OpenGL +Y
convention. Color/emission sampling uses sRGB decoding; normal and packed
metallic/roughness values use linear data sampling and the existing role-specific
mip/compression path. Scalar colors remain scene linear. Blender display transforms
and exposure are not baked into these material values; Media Server's facility
color/output policy remains authoritative.

Supported image nodes use Flat projection, Repeat or Extend addressing, and
Linear or Closest filtering. An unconnected Vector input, Texture Coordinate UV,
or an unnamed UV Map node selects the active render map. Named alternate UVs,
Mapping nodes, `KHR_texture_transform`, triplanar/generated coordinates, UDIMs,
image sequences, packed/dirty images and nonsaved images are diagnosed. Save or
unpack PNG/JPEG images before publication. Each input image is at most 4096 pixels
per axis and 16 MiB; the publisher retains at most 32 distinct images / 64 MiB.
The Director additionally enforces its decoded texture and full-scene budgets.

Unchanged images reuse worker-owned bytes after a size/time check. **Publish Now**
forces a content reread, including a file whose size/time was deliberately retained.
Images are embedded inside immutable GLBs; the Director and output nodes do not
depend on Blender's working image paths. A changed image rebuilds the surface
packages that reference it. Other surface packages remain reusable.

Shader mixtures, linked unsupported sockets, procedural color, volumes, shader
displacement, subsurface, sheen, anisotropy, thin-film and nondefault unsupported
tints produce diagnostics. Existing renderer extension support is broader than
this publisher subset. Unsupported features are not silently approximated by the
publisher. Alpha Blend uses the renderer's existing transparency path; transmission
is a thin-surface approximation without volume absorption or thick refraction.

## Stable surfaces and shot overrides

Each authored mesh remains the placement parent. Its material surfaces become
children identified by UUIDv5 of **object identity + material identity**, with
prototype IDs similarly derived from the shared prototype UUID. Slot numbers and
names never identify a surface. Repeated slots using the same material merge into
one surface. A material replacement creates a different child and tombstones the
old one; restoring the material UUID restores its retained shot record.

Each child package contains only its referenced corners. Bounds, picking and
capacity estimates therefore describe that surface, not the entire parent mesh.
Splitting happens after terrain chunking and preserves scatter batches. Authored
LODs must retain each material identity. All generated packages and occurrences
remain subject to the existing 64-resource, 1024-record and 4096-instance limits.

Material records carry a separate look revision, independent of geometry. Source
factors appear in the Inspector. The eight editable shot factors are base RGB,
roughness, metallic and emissive RGB. Editing one keeps the other source values;
Reset material factors clears those overrides. Alpha, textures and extension
properties remain source-authored in this version. Source updates, slot reorder,
duplicate/unlink, persistence and immutable publication preserve the effective
material values. Select the parent for a whole-object placement gesture or a
surface row for a surface adjustment.

## Explicit base color baking

**Bake Base Color Copy** is an operator action for one-material meshes with a
render UV map. It saves a disposable scene copy and launches a separate instance
of the qualified Blender executable. That worker evaluates the Principled Base
Color input through an emission bake to a 1024x1024 sRGB PNG, with eight-pixel
margins and 16 samples. Lighting is excluded from albedo.

Completion creates and assigns a material copy with the baked image; it preserves
the source material. Other material inputs remain unchanged and must satisfy the
translation contract. This is not automatic baking of arbitrary complete graphs.
The status gives the result or worker log location. Save the generated PNG with
the project; its initial location is a temporary `mslink-color-*` directory.
Dependency graph changes while the worker runs discard the result, including
when the link is disconnected. Opening another file or disabling the add-on
cancels the worker. Baking is never scheduled by a timer without an explicit request.

## Lights and photometry

Lights travel as typed manifest data, not glTF mesh lights. They become native
Director light occurrences, so the existing Inspector, placement, visibility,
shot ownership, undo, save/reopen and publication paths apply.

| Source | Renderer | Power and dimensions |
| --- | --- | --- |
| Sun | Directional | Normalized W/m2 times explicit lm/W efficacy -> lux; half angular diameter expressed in turns |
| Point | Point | Normalized W times efficacy -> lumens; source radius must be zero |
| Spot | Spot | Lumens; half cone angle in turns; inner angle is outer angle times `(1 - spot_blend)` |
| Square / Rectangle Area | Rectangle | Lumens; width/height in metres, including positive orthogonal object scale; orientation/roll follows the source |

The add-on exposes **Light efficacy (lm/W)**, default 683 and range 1..683. It is
an explicit RGB approximation, not a spectral reconstruction or electrical bulb
wattage. A constant Emission light node multiplies color and power; arbitrary
light graphs are refused. Normalize must be enabled. Blender temperature and
per-contribution multipliers must remain disabled/default. RGB light color is
linear. Point/spot finite radii, disc/ellipse area lights, shear and reflected
light placement are diagnosed. Existing Advanced rectangle shadow limitations
remain visible in the native Inspector.

Blender's shadow-enable switch is not translated. Native shadow and contribution
controls are Director-owned; local shadows start disabled. The light contract
qualifies color, power, pose and dimensions rather than Blender shadow parity.

The add-on's **Light cutoff (m)** is an explicit finite renderer range, independent
of object scale. Units and type remain source-authored. The Director exposes shot
color, intensity, range, dimensions, cone/radius, aim, roll, shadow/contribution
and temperature controls appropriate to the type. Reset light properties removes
those overrides. The source and occurrence keep separate property maps. Full
admission counts four directional lights, including a physical sun, and 64 local
lights before the fixed renderer arrays can truncate anything.

## Derived lighting and Program policy

The existing bake worker snapshots static geometry, effective transforms and
material factors/package identities, indirect-light contributors, environment
lighting, reflection captures and the intentional asset refresh generation.
Transport readiness manifests do not affect this dependency hash. Opening a
project or staging identical assets therefore cannot stale the look.

Probe format v3 stores the input digest with the GI field and reflection captures.
The accepted environment stores `baked_input_revision`. A worker completion is
still fenced against its submitting stage and current input digest before the
Director authors a change. Old completions cannot become current after a static
contributor changes. GI and reflection currentness use the same dependency revision.
The Inspector and status bar display **STALE LIGHTING** after an affected edit;
the status is cached on document/refresh changes rather than rehashing each frame.

Preview retains the previous derived field and identifies it as stale. Program
publication **retains the previous complete revision** until the operator either:

1. Rebakes and publishes matching geometry, materials, lights, GI and reflections;
2. Explicitly disables baked GI **and all reflection captures**, then publishes.

Publication validates the recorded revision and probe file on its worker before
rewriting paths into shared storage. Probe bytes join the same immutable resource
set and whole-cohort readiness gate as the models. Legacy untagged fields require
a rebake before publication. Retained frozen Program revisions are already qualified
and stay usable without Blender.

The existing offline integrator accepts opaque static surfaces. Alpha/transmitting
surfaces must have **Static geometry** cleared in the Inspector; they still render live but do not
contribute to the GI/reflection bake. The reference fixture deliberately exercises
this distinction. This step does not extend the integrator to transparent transport.

## Sources and qualification scope

The audit was checked against the repository's `model_loader.cpp`,
`model_compression.cpp`, material shaders, `lighting.cpp` and `baked_lighting.cpp`.
The interchange patterns are documented in the
[Blender glTF manual](https://docs.blender.org/manual/en/4.0/addons/import_export/scene_gltf2.html)
and [glTF specification](https://github.com/KhronosGroup/glTF/blob/main/specification/2.0/Specification.adoc).
The installed, pinned Blender binary is the compatibility authority for RNA names
and light node behavior. Tests compare numeric properties and resource identities;
Advanced GPU and native UI checks verify usability. They do not establish general
photorealistic parity or new physical two-machine frame timing.
