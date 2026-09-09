# 09 — Foliage and terrain scale

Status: proposed. Depends on: 05, 08.

## Outcome

Large procedural environments update through shared geometry, instance batches,
and terrain chunks with declared resource budgets.

## Work

- Extract evaluated scatter prototypes and occurrence transforms from Blender.
  Preserve prototype sharing instead of realizing every tree into unique geometry.
- Define instance batch storage and updates without requiring one heavyweight
  editor/document object for every blade of grass. Audit current renderer bounds,
  culling, shadow submission, picking, and instancing costs.
- Define stable scatter occurrence IDs where the generator can provide them.
  Index order is not stable after regeneration. If stable matching is unavailable,
  limit overrides to the scatter group or allow a selected prop to be detached;
  never guess which regenerated tree owns an old placement override.
- Split terrain into spatial chunks with stable IDs and boundary rules. Update
  affected chunks, normals, bounds, and authored LODs as a coherent revision.
- Support explicit render/export density separate from Blender viewport density.
  Choose deterministic published content, identical on all output nodes.
- Budget instance count, vertices, draw calls, shadow cost, memory, and upload work.
  Unsupported procedural output is diagnosed or intentionally baked by the artist.

## Acceptance

Measure a declared forest and terrain dataset. Repaint one region and prove
unaffected prototypes/chunks are reused. Change scatter count/seed and verify
overrides are either matched by stable ID or explicitly orphaned. Test terrain
boundaries and identical cohort LOD/content decisions under the full scene budget.
