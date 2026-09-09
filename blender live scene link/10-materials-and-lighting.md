# 10 — Materials and lighting

Status: proposed. Depends on: 05, 08.

## Outcome

A documented material/light translation contract and predictable lighting refresh.

## Work

- Map a supported Principled/PBR subset to the renderer's existing material model.
  Audit current support: UV0, texture transforms, extensions, thin transmission,
  and light types must be checked against the actual importer and shaders.
- Keep stable material IDs and per-property shot overrides. Material slot reorder
  must not move an override onto an unrelated surface.
- Diagnose arbitrary shader graphs and procedural textures. Offer explicit baking
  to supported textures where feasible; do not imply Cycles/Eevee appearance parity.
- Define color spaces, normal-map convention, alpha handling, image roles, and unit
  conversion. Reuse existing role-correct texture ingest and facility color policy.
- Translate supported lights with explicit photometric semantics and dimensions.
  Distinguish lights authored in Blender from Media Server shot overrides.
- Track which geometry, transforms, materials, and lights affect baked GI and
  reflection probes. Mark derived resources stale when their dependency revision
  changes and run requested rebakes on workers.
- Define publication policy for stale lighting: retain a previous complete revision,
  or deliberately publish geometry with clearly identified stale/disabled derived
  lighting. Do not advertise an unrebaked revision as fully refreshed.

## Acceptance

Use a material/light reference scene and compare supported properties numerically
and visually in the Advanced renderer. Change one texture and verify independent
ingestion. Move a static contributor, start a bake, then change it again: the old
bake cannot become current. Test publication of geometry and lighting together.
