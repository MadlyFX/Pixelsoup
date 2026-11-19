# Environment scale v1

Static foliage and terrain are qualified on the pinned Windows Blender 5.2.0 LTS
build `fbe6228777e7` (measurements taken with add-on 0.4.0). The Director owns
admission and publication; output nodes receive the same immutable packages and
explicit transforms. They never run a procedural generator or density sampler.

## Scatter and identity

The publisher reads the evaluated dependency graph on Blender's main thread.
Mesh-object instances share a prototype identified by the authored object's UUID.
Each emitter/prototype pair becomes one source object and one editable occurrence,
with a compact array of local affine transforms. There is no document node, mesh
copy, Inspector row or undo record per tree. Resource extraction remains bounded
and serialization runs in the existing separate worker process.

The native `model_instance_batch` property stores 12 little-endian binary32 values
per matrix: xyz from each column of a column-major affine matrix. Lowercase hex
encoding uses 96 characters per occurrence. The implicit bottom row is 0,0,0,1.
Validation refuses malformed, nonfinite, singular, reflected and oversized data.
The existing manifest, transaction, persistence and publication paths carry this
property, so save/reopen, source undo, independent copies and frozen Program all
retain the batch.

The qualified generators do not promise persistent individual occurrence IDs.
**Placement, picking, visibility and Hold/Follow/Reset address the scatter group.**
Group identity derives from emitter and prototype UUIDs and survives count, seed
and ordering changes. A missing group becomes an explicit orphan and restores if
the same UUID returns. A tree's array index never owns a shot override. A renderer
history key derived from group and local matrix distinguishes stationary trees;
it is not a persistent authoring identity. Moving a candidate resets its history.

Generated batch/chunk rows can receive Director-side overrides, but they are not
editable Blender objects, so those overrides stay in Pixelsoup. Individual-tree detachment is
not available; create an authored prop when it needs its own shot.

## Density and supported generators

`Export density (%)` samples the evaluated candidate set without changing Blender's
viewport. The hash of the export seed and the packed local matrix determines
selection; sorting selected matrices makes the bytes independent of enumeration
order. The same seed and candidate content reproduce the same published content.
Sampling is probabilistic in count, deterministic in membership. At 100%, every
candidate is retained; changing the sampling seed then has no effect.

The active view layer must contain the complete deterministic candidate set.
The link does not invoke a second render-mode dependency graph. Mismatched
modifier viewport/render switches, Is Viewport nodes (including nested groups),
particle/hair systems, non-mesh instance prototypes, reflected placements and
unqualified material shaders produce diagnostics. Anonymous procedural geometry
must be intentionally realized/baked into an evaluated mesh. Mesh-object Geometry
Nodes instances are the exercised procedural path. Artists must not use other
unqualified view-dependent inputs to alter the candidate set.

## Terrain and authored LOD

Set `Terrain chunk size (m)` above zero on an evaluated mesh to clip its triangles
to an origin-anchored grid in its local Blender XY plane, measured in metres.
Zero preserves ordinary mesh export. The source remains a transform parent;
children use UUIDs derived from source UUID, grid size and integer cell coordinates.
Changing topology inside a cell preserves identity. Changing grid size creates
new chunk identities and explicitly orphans old overrides.

Clipping interpolates corner normals and UVs and snaps shared boundary positions.
Canonical edge orientation gives adjacent triangles the same intersection
arithmetic. Chunks retain their absolute local coordinates, and the immutable
package builder recomputes bounds. A sculpt near an edge can correctly invalidate
multiple chunks if positions or normals change there. No unaffected resource is
serialized again merely because another cell changed.

`Authored low detail mesh` supplies one static lower-detail mesh in the same local
coordinate system. Its object placement is ignored. The full-canvas projected
size threshold selects the LOD through the existing renderer. For terrain, base
and low meshes must occupy identical cells and retain all boundary positions and
normals to five decimal places; the entire candidate is rejected otherwise.
Artists can simplify chunk interiors while retaining those boundaries. This is
an authored LOD workflow; it does not automatically decimate terrain or add skirts.
Base, LOD, bounds and chunk changes are admitted and published as one revision.

## Admission budgets

These limits are deliberate working budgets below the renderer's hard capacities.
All model parts use the most expensive authored LOD before culling. The complete
candidate is checked on the Director worker and again during Program preparation.
Existing package/cache/upload and whole-cohort gates remain authoritative.

| Item | Limit / accounting |
| --- | --- |
| Dependency graph enumeration | 65,536 entries in the active view layer |
| Expanded scene instances | 4,096, including ordinary props and terrain |
| Geometry records | 64, including authored low-detail resources |
| Terrain cells | 64 per source; total geometry limit still applies |
| Evaluated/clipped geometry | 300,000 corners per mesh / terrain result |
| Material submissions | 8,192 before instancing or camera culling |
| Shared geometry | 33,554,432 vertices and 100,663,296 indices, all LODs included (a ~30M-triangle scene; the renderer's arena holds 48M / 144M for the scene it replaces and refreshes) |
| Geometry/texture/upload estimate | 8 GiB; 128 bytes per vertex, 4 per index, texture mip bytes, 1,024 per submission |
| Full-scene triangle work | 32,000,000 triangles at the most expensive LOD |
| Worst shadow triangle work | 2,176,000,000: full geometry times four cascades plus 64 local atlas views |
| Existing payload bounds | 1 MiB manifest; 64 MiB per GLB; 16 GiB total declared GLBs |

Ordinary procedural primitives contribute their unique geometry and repeated
triangle cost; media contributes its quad geometry. This estimate covers scene
geometry, textures and submission/upload work, not total process RSS, video decoder
memory or the renderer's fixed render targets. Refusal retains the last complete
Preview/Program. The publisher status reports instances, resources and GLB work.

## Renderer audit and acceptance

Bounds and full-camera culling operate per expanded instance. Opaque equal
prototype/material/LOD draws use the existing GPU instancing path. Picking retains
the editor's group ID and gizmos use the group origin. Selection draws a single
aggregate bounds outline per scatter group, preventing thousands of selection
boxes from exhausting the UI vertex budget. Independent/frozen batches also keep
one editor row per group. The GPU scene bridge and
motion history use the separate render keys; sorted history lookup avoids scanning
all previous instances for every tree. Shadows retain the full caster set instead
of relying on the camera-visible list. LOD and culling use the complete camera,
so two output slices make identical decisions.

The measured ceiling fixture has 4,080 shared tree silhouettes, one authored tree
LOD and 16 terrain chunks: **4,096 instances in 62 document nodes**, 6,153 shared
vertices and 6,153 indices. Tests exercise repaint reuse, order-independent
density, group override preservation/orphans, boundary rejection, 4,097-instance
refusal, expensive ordinary-prop refusal, independent snapshots, two publication
verifiers and offline reopen. A D3D12 Advanced test measures four shadow cascades,
4,096 casters, instancing, LOD reductions and equal full-camera selection on both
halves of a 640×360 canvas. It allows bounded initial asset preparation, then
requires zero failed, pending or dropped models in steady rendering.

This synthetic dataset qualifies storage and submission scale. It does not claim
photorealistic foliage, alpha-tested leaf materials, real-time performance on every
GPU, or a physical two-machine frame-timing qualification.
[Material and lighting translation](materials-and-lighting-v1.md) supplies stable
material surfaces within scatter and terrain; that look test does not qualify a
photorealistic forest at this capacity ceiling. Frame-aligned publication is
covered by the asset cohort activation tests.
