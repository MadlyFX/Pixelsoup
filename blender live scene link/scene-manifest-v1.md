# Scene manifest v1

Step 03 provides an offline, complete static scene import. Steps 04 and 05 add
transport and the Blender publisher using this same contract. File extension:
`.mslink`; UTF-8 without BOM.
Each record occupies one line. Strings use C++ `std::quoted` escaping (`\"` and
`\\`); blank lines and lines beginning with `#` are ignored. Unknown records,
extra fields and incomplete values reject the entire candidate.

```text
MSLINK 1 "source-scene-uuid" "scene-revision"
prototype "prototype-uuid" "64-character-sha256" "relative/path.glb"
material "material-uuid" "material-revision" R G B roughness metallic ER EG EB
object "object-uuid" "display name" "parent-uuid-or-empty" "prototype-uuid-or-empty" "material-uuid-or-empty" "origin-revision" visible M0 M1 M2 M3 M4 M5 M6 M7 M8 M9 M10 M11 M12 M13 M14 M15
```

The header and UUIDs are persistent identities, not array indexes. UUIDs must be
canonical lowercase, nonzero UUID strings. Scene/material revisions are nonempty
opaque strings; prototype revisions are SHA-256 of the exact GLB bytes. UUIDs are
unique per record kind in a source scene. References may precede their targets,
but must resolve inside this complete manifest. A source object omitted from the
next manifest becomes a tombstone; reintroducing its UUID restores its existing
shot occurrence. Referenced parent/prototype/material records cannot be omitted.

The 16 matrix values are column-major affine transforms in Media Server metres,
Y-up, using step 01's coordinate conversion. Visibility is 0 or 1. Empty
prototype means a transform-only parent. Empty material preserves the GLB's
per-primitive material slots. A material reference applies the eight linked
scalar factors across that object's imported parts. Step 10's publisher splits
meshes into stable material children; see [Materials and lighting v1](materials-and-lighting-v1.md).
Origin revisions must remain
stable; changing a pivot is rejected by the existing source-update planner.

Use per-prototype **embedded GLB** payloads. External glTF buffers/images, `.lod`
companions, animation, skins, morphs, cameras/lights, non-triangle primitives and
nonidentity GLB node transforms are outside this initial subset. Export evaluated
triangles in prototype-local space and put all scene placement in the manifest.
The copied hashed GLB has no `.lod` companion, so unrelated sidecar content never
becomes an implicit dependency. Negative-determinant drawable placement remains
blocked by step 01's renderer qualification gate.

Step 11 extends that static subset only for explicitly declared animation assemblies:

```text
object ... M15 animation start_show_frame loop
```

`start_show_frame` is a nonnegative signed 64-bit integer and `loop` is 0 or 1.
This object must reference one bounded embedded node/skin GLB with one sampled
LINEAR clip; its internal transforms are preserved, and the manifest matrix
places the complete assembly. Animated resources without this suffix reject
admission. Scatter and animated external LODs are excluded. See the
[animation contract](animation-and-stage-v1.md) for timing, skinning and budgets.

Step 09 adds two explicit optional suffixes, keeping ordinary records unchanged:

```text
prototype "prototype-uuid" "base-sha256" "base.glb" lod canvas_pixels "low-sha256" "low.glb"
object ... M15 batch "lowercase-hex-affine-array"
```

The batch array contains 1–4096 affine matrices, each 12 little-endian binary32
xyz column components (96 hex characters). A batch requires a prototype. The
object matrix places the group; each local batch matrix places one instance.
LOD input is a second explicitly hashed embedded GLB with a finite positive
full-canvas pixel threshold (the add-on exposes 1–4096). The worker validates both and creates its own bounded
`.lod` companion inside the immutable build cache; arbitrary external companions
remain forbidden. Package identity hashes the base hash, low hash and threshold,
so either level or the selection rule invalidates the revision. Terrain and
instance limits are specified in [Environment scale v1](environment-scale-v1.md).

Step 10 adds an optional typed light suffix to an object without a prototype:

```text
object ... M15 light "kind" "unit" R G B power range inner_cone outer_cone width height angular_radius
```

`kind` is `directional`, `point`, `spot` or `rectangle`. Directional lights use
`lux`; the other kinds use `lumens`. Lengths are metres, angles are turns, and
all ten values are finite nonnegative numbers. RGB is linear; positive
range/dimensions and ordered bounded cones are validated. Source position and aim
come from the object matrix; the native light axis is -Y after Blender conversion.
Lights may have transform parents but no drawable prototype or instance batch.
They use the same object upsert/erase operations as other occurrences. Admission
counts the complete native scene's four directional and 64 local light slots.
GLB camera/light extensions remain forbidden; typed lights are explicit manifest
content. Legacy manifests without this suffix remain valid.

## Resources and ownership

The worker verifies every required GLB hash, copies new content to a hashed name
under the manifest directory's `.mslink-cache-v1`, qualifies its structure, and
uses the existing model loader/compressor/package writer. A package index stores
only the package filename and its SHA-256. Both source and package bytes are
verified on reimport. A missing or corrupt dependency rejects the candidate even
when another version is resident. An unchanged package is not geometry-decoded,
filtered or compressed again. Placement-only import performs worker filesystem
and hash verification but zero geometry ingestion or texture compression.

The Director's shared resource store (see [protocol](protocol-v1.md)) is its own
cache instead: its `<hash>.glb` files are already immutable, so nothing is copied,
and every pairing resolves the same package index. Ingests into one cache are
serialized within the process; indexes, companions and packages appear by rename.

The document stores `link_resource = sha256:<GLB hash>` and `link_revision`
separately from `source`, its machine-local immutable package fallback. Copy the
manifest and GLBs together to another machine and reimport to resolve local
paths. The cache is not a transport protocol. Existing projects can render from
their retained packages without Blender or the original export directory.

Compression independently caches prepared texture images by their bytes, mip
dimensions, normal-map encoding role and base-level requirement. This cache is
limited to 64 MiB and 256 entries; eviction does not invalidate package bytes.
Replacing geometry may still decode/filter its embedded images, but identical
prepared images reuse compressed bytes. The generated fixture is untextured;
the independent cache test uses a synthetic RGBA image and compares exact mips.

## Admission and lifetime limits

| Resource | Initial limit |
| --- | --- |
| Work | One executing import and one replaceable pending request |
| Manifest | 1 MiB, 1024 records, 64 prototype references |
| GLB inputs | 64 MiB each, 256 MiB total declared inputs |
| Prototype graph | 64 meshes, 128 nodes, 32 embedded images |
| Geometry scratch inputs | 1 million elements per accessor, 8 million across accessors, 2 million expanded position/index elements across mesh nodes |
| Images | 4096 × 4096 maximum per image; 16 million decoded pixels total before role splitting/mip preparation |
| Candidate package bytes | 256 MiB across distinct required packages |
| Disk admission | Reject a new generation at 128 package indexes (4096 in the Director's shared store) or 1.5 GiB already retained, with headroom for the bounded in-progress generation |
| Renderer | Existing bounded ModelCache pending jobs, snapshots, decoded byte residency and frame-fenced uploads remain authoritative |

Package creation, parsing, decode, filtering, compression and hashing run on the
worker. The geometry and image limits bound input-driven CPU scratch growth;
they are not a claim that peak process RSS equals the compressed package size.
Cancellation is cooperative between worker stages; an executing codec may finish
before cancellation is observed. No second codec worker is started to supersede
it. Late results are discarded by a generation check, including completed results
waiting to be taken. Cancel or another import never blocks on a worker join.

Old files are retained conservatively for document history and live render users.
No automatic disk deletion risks invalidating an older project. When disk
admission is full, put future manifests/GLBs in a new directory; retain the old
cache while any saved project uses it. This bounded refusal policy is deliberately
simpler than introducing project-wide resource garbage collection.

The Director plans against the current document and commits all changes in one
transaction. A new UI import always receives a fresh transaction ID. Undo restores
the previous revision in one step while preserving an intervening shot edit.
Results are discarded if the project, linked source/attachment, target scene or
relevant deletion changed during staging. Preview-only imports reject a source
already attached to Program, since source records are shared across attachments.
Frame-aligned, complete GPU adoption on multiple render nodes remains step 08.
