# 01 — Contract and reference scene

Status: proposed. Dependencies: none.

## Outcome

A versioned feature contract and a small reproducible Blender scene that define
what successful synchronization means before transport or UI work begins.

## Work

- Pin an initial Blender release and record tested OS, renderer backend, hardware,
  and network configuration. Support other Blender releases only after testing.
- Define source scene, linked collection, object, geometry prototype, material,
  occurrence, shot override, and publication revision as distinct concepts.
- Specify meters, axis conversion, handedness, matrix convention, parent order,
  negative/nonuniform scale, normals, pivot changes, and camera conventions.
- Define the initial supported subset: static meshes, hierarchy, repeated assets,
  PBR materials, visibility, and object placement. Later steps add richer content.
- Create fixtures with a parented prop, two occurrences sharing one mesh, multiple
  materials, a renamed object, nonuniform scale, and a fixed composition camera.
- Inventory current document commands, model package boundaries, readiness permits,
  and Preview/Program behavior. Record reusable seams and necessary schema changes.
- Measure manual export, ingest, upload, and scene adoption separately. Establish
  target scene sizes and latency budgets from these measurements.

## Acceptance

Record the expected world transforms and appearance of each fixture. The primary
scenario is: place a prop to camera in Media Server, change its geometry in
Blender, and retain its shot placement in both applications.

Latency targets are provisional: small placement updates should feel interactive;
mesh latency depends on evaluation and payload size. Report measured percentiles
and dataset sizes rather than promising a universal update time.
