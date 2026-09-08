# 12 — Culling, instancing and LOD

**Track 4.** No hard dependency on tracks 2–3, but GPU-driven submission needs
[04](04-compute-in-the-rhi.md).

## Implemented on 2026-09-06

CPU culling, imported-part instancing, and authored LOD chains now run through
`SceneRenderer` on Basic and Advanced. The original proposal below remains as
context; the implemented boundaries and verification are recorded here.

- `GeometryFrusta` derives a conservative sphere/box test from the authored
  full-canvas camera, expanded by the scene apron and one pixel for jitter.
  Its API accepts no node slice or local output resolution. Invalid bounds
  retain geometry. Selection is exposed as a stable sequence of source IDs,
  visibility decisions, and LOD levels for diagnostics and cohort tests.
- Camera batches omit invisible geometry. Directional cascades, RSM bounce,
  and local-light shadows still draw the complete opaque set. Culling never
  alters the cascade fit or caster count, and transform history still records
  the full scene. CPU instance uploads remain complete so shadow and camera
  draws can share them.
- Opaque submissions are ordered by asset, mesh/part, and document node ID.
  A shared material part uses one instanced draw, including repeated imported
  models. Material factors, inline effect programs, object IDs, and previous
  transforms remain per instance. Transparent compositing retains its existing
  depth and sibling order. Disabling instancing uses the same deterministic
  order with one instance per draw.
- A model's optional `.lod` companion supplies up to seven additional authored
  meshes. The ModelCache worker loads the complete chain as one immutable
  generation; every level counts against the existing CPU/GPU byte and arena
  bounds and is adopted transactionally. No filesystem operation or
  simplification is added to frame recording. Projection-mesh import and
  Library probing use `load_model_base`, so mutually exclusive levels are not
  accidentally combined into a wall surface or inflated Library metadata.
- LOD uses the projected bounds diameter in **authored canvas pixels**, ignoring
  mapped cameras, output resolution, frame number, previous levels, and local
  history. Near-plane crossings select the finest level. Asset bounds enclose
  all authored levels; cascade caster counting remains based on level zero's
  material runs even when the selected level has fewer parts.
- The project owns three switches: `frustum_culling`, `geometry_instancing`,
  and `model_lod`, all enabled by default. They persist through the ordinary
  typed document properties and enter scene/profile digests. An unconditional
  geometry version marker changes the renderer profile even at defaults: a
  build that cannot load LOD companions cannot satisfy the new readiness
  evidence. All render nodes must run the updated build.

### Deliberate conservative cases

The renderer receives a camera override for a free Stage view or a physical
wall projection, but `Scene` does not contain the complete replicated physical
wall volumes. Those views retain all geometry. Likewise an active tracked plate
uses an unbounded conservative union: the wall's overscanned rectangular plate
can extend beyond the ideal lens cone. Guessing that rectangle from the lens
would reintroduce false culls. These cases still instance and select LOD using
the authored full-canvas camera, identically across the cohort. Tight culling of
mapped/tracked views needs those replicated wall volumes at this boundary.

Occlusion culling, meshlets, indirect GPU submission, automatic decimation, and
per-node LOD hysteresis remain deferred. LOD files are production assets: every
node needs the same companions and referenced meshes. Same-source refresh is
available through the existing cache API; there is no new cohort reload command
in the editor, so restart the applications after changing files in place.

### Capacity decision

The default draw-instance arena rises from 4,096 to **16,384** records, and the
scene-model placement cap rises from 256 to **4,096**. Each model material part
consumes one record, including shadow-only opaque records; instancing reduces
draw calls, not that accounting. Exceeding the combined drawable count or the
expanded material-part count refuses the frame and counts the refusal.

The shared geometry arena allows 2,097,152 vertices and 6,291,456 indices,
including all LODs and retained refresh generations. This targets thousands of
repeated props plus roughly two million indexed triangles of unique set
geometry. Decoded residency remains byte-bounded at 512 MiB and 320 generations,
with 512 source records; 4,096 placements do not imply 4,096 unique resident
assets. The extra ordering, visibility, camera-batch, and instance-reorder
scratch is reserved at construction. Transform history remains shared across
views rather than multiplying instance storage by the 256 history-slot limit.

### Authoring model levels

Import the finest asset, for example `set.glb`, with this UTF-8 text file beside
it as **`set.glb.lod`**:

```text
MSLOD 1
256 "set-low.glb"
64 "set-lowest.glb"
```

A model at or below 256 pixels uses the first alternate; at or below 64 pixels
it uses the second. Values must be finite, positive, and strictly descending.
Paths are relative to the companion and may point to OBJ, glTF, or GLB; use
forward slashes for directories (backslashes are quote escapes). All
levels use the same model coordinate system. Each line contains only its
threshold and quoted path. A companion is limited to 64 KiB and eight total
levels. Missing, malformed, or undecodable levels refuse the complete new
asset generation; nested companions are not followed.

### Checking it in the editor

1. Open **File > Project Settings > Geometry**. The three switches work with
   either renderer backend. Apply saves them as project policy; Cancel discards
   the draft.
2. Place several copies of an imported multipart model. With Instancing on,
   **Last Render** reports fewer draws and instanced batches. Turn it off and
   apply: the image remains the same while draw count increases. This status
   describes the last scene-render pass, not a sum across compositor layers.
3. In a canvas camera view, place geometry behind the authored camera and
   compare Frustum culling on/off. Visible/cull counts change; the visible
   picture and shadows do not. Physical mapped views and active tracked plates
   deliberately retain the full set as described above.
4. Prepare a companion and alternate meshes before importing/opening the
   project. Move the authored camera away from the model. **Models with levels**
   and **using lower detail** report selection. Model LOD off forces level zero.
   A mapped camera or a smaller Preview target must not choose another level.
5. If a load fails, correct the companion and its relative paths and restart
   the applications. A cold pending load and a failed load are reported in this
   tab; malformed chains cannot publish only some levels.

The Stage editor now supplies its free camera as an explicit render override,
retaining the scene's authored camera and canvas for detail selection. Its
neutral exposure and its own near/far planes still drive the preview; the fog
pass receives that explicit depth range through the compositor. The final
native Release check verified the Stage floor, sphere, and spotlight atmosphere
alongside Program after this change.

The native editor was opened and the Geometry tab inspected at runtime. Its
three controls, companion instructions, and last-render counters fit the card;
toggling culling changed the draft and enabled Apply, then Cancel discarded it.
The UI test also exercises all three switches through the real panel input
path and checks the resulting typed project settings.

### Automated evidence

`render_geometry` covers all six frustum planes, intersections, mirrored and
nonuniform transforms, invalid bounds, an independently calculated projected
pixel size, stateless LOD selection after a seek/edit, and policy digests.
`render_model_loader` covers level offsets, reduced triangle ranges, base-only
import, malformed headers/thresholds/paths/trailing input, and atomic failure.
`render_scene` covers byte-identical culling/instancing reference images,
identical visible sets across two slices, stable order after reordering and an
unrelated document edit, multipart model instancing, LOD agreement between
Program and a different mapped camera/resolution, 5,000 shared mesh placements,
and counted refusal at the configured cap. A native D3D12 case checks identical
shadow caster counts, exact cascade matrices, and HDR pixels with culling on/off.
Hybrid and compositor fog tests also compare an explicit Stage depth range
against an equivalent authored camera, detecting the black-preview regression
that occurs when fog integrates using the wrong near/far planes.

Both Windows GUI presets built successfully and each passed all **140 CTest
entries** with zero failures. After the final Stage-camera/fog correction, all
**48 affected render, presenter, and editor checks** passed again in each
preset. Native D3D12 fixtures ran on the available adapter.

The LOD pixel comparison uses geometrically equivalent authored levels. A
purposefully simplified mesh is allowed to change silhouette; reduced index
ranges and correct selection are tested separately. The scale fixture is
synthetic; this is not a captured photogrammetry throughput certification.

## Original proposal: why

The renderer has **no geometry culling at all**, and this is a documented
decision rather than an oversight. From `scene_renderer.hpp`:

> What is deliberately absent, so that its absence is a decision rather than an
> oversight: frustum culling (the scenes are small and a wrong cull is worse than
> a slow frame), instancing beyond per-mesh batching, and global illumination.

That reasoning was correct for procedural primitives and a handful of imported
models. It stops being correct the moment the content is photoreal, because
photoreal content is photogrammetry: a scanned set is millions of triangles
across thousands of objects, and the current caps —
`max_scene_instances = 4096`, `max_scene_models = 256` — are a fraction of one
real environment.

So this step is not a performance optimisation. It is the difference between the
renderer being able to open the kind of content that looks photoreal at all.

## The rule that makes all of this safe

**Cull against the authored full-canvas camera, never against this node's
slice.**

Slice-local culling is the obvious implementation and it is wrong. Two nodes
would then draw different object sets, and any pass that depends on what was
drawn — shadow casters, screen-space effects, even just the depth buffer's
contents in the apron region — differs across the seam. The performance win from
slice-local culling is real but it is not worth reintroducing the exact class of
fault this architecture exists to prevent.

Cull against:

- the full-canvas camera frustum, expanded by the apron,
- **union** the shadow cascade volumes, so a caster outside the camera but
  casting into it is kept.

Every node then computes the identical visible set from replicated state. The
saving is still large — a scanned set has most of its geometry behind the camera
— and it is uniform across the cohort.

The same rule applies to the inner frustum under
[01](01-camera-tracking-and-inner-outer-frustum.md): cull against the union of
the outer canvas frustum and the inner frustum, both derived from replicated
values.

## Seams

- `engine/scene/include/mediaserver/scene/scene.hpp` — bounds on `MeshInstance`
  and `SceneModel` records. Some bounds already exist for the shadow fit
  (`expand_directional_shadow_bounds` consumes local min/max per instance), so
  the data is partly there.
- `engine/render/include/mediaserver/render/scene_bridge.hpp` — the bridge
  already keys stable GPU records by document node and content identity and
  diffs between frames. It is the natural home for a persistent visible set and
  for instance grouping.
- `engine/render/src/scene_renderer.cpp` — batch construction; today one batch
  per mesh with per-batch material textures.
- `engine/render/include/mediaserver/render/model_cache.hpp` /
  `model_loader.hpp` — LOD chains as part of a decoded model asset.

## The three pieces

### Culling

Start with CPU frustum culling against the union volume above — it is simple,
deterministic by construction, and probably enough for a while given the bridge
already maintains a bounded record set. Sphere-then-box against the full-canvas
frustum planes.

Occlusion culling is the larger win on a dense set but is much harder to make
node-identical: a depth-buffer-based occlusion test is inherently view-dependent.
If it is needed, it must run against the **full-canvas** depth, which means
either a separate full-canvas depth prepass on every node (wasteful but
deterministic) or a Director-computed visible set replicated per frame
(bandwidth, and a frame of latency). Defer, and record the constraint so nobody
implements the easy view-dependent version.

### Instancing

The bridge already produces stable per-drawable GPU records. Group records
sharing a mesh and material into one instanced draw with a stable ordering — by
document node id, so the order is replicated and identical everywhere. The
existing `DrawBatch{vertex_count, instance_count}` and the pulled-draw base
instance convention (offsets ride in `batch_uniforms` precisely so D3D and Vulkan
agree) already support this; the work is in grouping, not in the draw path.

This matters most for the kind of content that makes a set look real — bolts,
bricks, foliage, crowd props — where the object count is high and the mesh count
is low.

### LOD

An imported model gains a chain of decoded levels, selected by projected screen
size. **Selection must be deterministic**, which means:

- Compute the projected size from the **authored full-canvas camera**, not the
  node's view or the inner frustum's.
- Any hysteresis must be state-free or replicated. A per-node "stay on the
  previous level until the threshold is crossed by a margin" is local state and
  two nodes will disagree after a seek.

Generating LOD chains is an ingest-time concern, not a frame-path one: the
`ModelCache` worker can decimate on load, or the chain arrives in the asset.
Prefer the latter — decimation quality matters and offline tools are better at it.

## Multi-node hazards

Summarised because this step has more of them than any other:

- Slice-local frustum culling.
- View-dependent occlusion culling.
- LOD selected from the local view.
- LOD hysteresis held as local state.
- Instance ordering that depends on traversal or hash order rather than document
  order.
- Culling changing the **shadow caster set**. Casters are fitted from the full
  opaque scene; a cull that removes a caster changes the cascade fit and the
  shadow map. Either cull casters against the cascade volumes only, or exclude
  casters from culling entirely.

## Tests

- A scene entirely inside the frustum renders byte-identically with culling on
  and off. This is the safety net for the whole step.
- Two nodes with different slices produce identical visible sets, asserted on the
  set, not on pixels.
- Two renderers with different cameras (Program versus a mapped view) select
  identical LOD levels.
- The shadow caster count and the cascade fit are unchanged by culling.
- Instanced and non-instanced renders of the same scene are byte-identical.
- Instance ordering is stable across a document edit that does not touch the
  instances.
- A scene past the instance cap refuses and counts rather than allocating.

## Definition of done

A scanned set of a realistic size opens and renders; culling, instancing and LOD
each produce byte-identical output to their disabled counterparts where the
content is fully visible; and every node in a cohort draws the same set at the
same detail.

## Open questions

- **Do the caps rise, and to what?** 4096 instances is low for a real set;
  raising it has memory consequences across the bridge, the instance buffer, and
  the history slots. Needs a target content size to design against.
- **Occlusion culling at all?** The deterministic versions are expensive. Perhaps
  the answer is aggressive authored visibility (rooms, zones) instead of an
  automatic solve — which is also how large game levels actually ship.
- **Meshlets and GPU-driven submission** are the ceiling of this work and need
  indirect draw plus compute. Worth designing the bridge's instance layout now so
  that path is not a rewrite later.
