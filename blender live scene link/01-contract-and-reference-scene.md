# 01 — Contract and reference scene

Status: implemented foundation, 2026-09-09. Dependencies: none.

The versioned contract, reproducible fixtures, CPU regression suite, reference
editor launcher and measured baseline are delivered. This step does not implement
a live connection. Negative-scale rendering has a recorded visual qualification
failure; it must be resolved before that part of the live subset is enabled.

## Delivered artifacts

- [Contract v1](contract-v1.md): identities, ownership, matrix/normal/camera rules,
  support/failure contract, expected fixture transforms and implementation seams.
- [Baseline v1](baseline-v1.md): tested configuration, raw phase timings, datasets,
  provisional response targets and exact reproduction commands.
- [UI walkthrough](ui-checks.md): generated native projects, Preview checks,
  Blender inspection and current qualification gaps.
- [Fixture source](../scripts/blender-scene-reference.py) and
  [launcher](../scripts/run-blender-scene-reference.ps1).
- [Checked-in data](../tests/fixtures/blender-scene-link/README.md) and
  [CTest implementation](../engine/render/tests/blender_scene_reference_tests.cpp).

Both Debug and Release GUI builds passed. Seven relevant suites passed in each:
scene math/model/manipulation, material ingest, model loader/cache and the new
Blender reference suite. The final fixture was rechecked after visual QA changes.

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
