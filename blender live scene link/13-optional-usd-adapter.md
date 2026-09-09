# 13 — Optional USD adapter

Status: proposed, optional. Depends on: 12 for delivery; research may start earlier.

## Outcome

Support larger multi-application scene assemblies without replacing the existing
Director document model, renderer, or publication authority.

## Decision gate

Prioritize USD when Houdini/Maya workflows, studio USD assets, variants, or layered
multi-artist assemblies are concrete requirements. A Blender-only deployment can
ship the preceding steps without embedding USD.

## Work

- Prototype a USD adapter into the linked-scene manifest and resource pipeline.
  Keep USD composition and asset resolution off the render path.
- Define source layers/references, shot override layers, variants, payload loading,
  and instance/prototype translation. Preserve a durable ID mapping; prim paths
  alone are vulnerable to namespace changes and USD supplies no automatic GUIDs.
- Test renaming and reparenting with authored overrides and external references.
  Specify which namespace edits the adapter can safely remap.
- Establish a supported material/light/animation schema subset. USD support does
  not imply arbitrary shader graphs or Blender procedural evaluation in Media Server.
- Keep the live protocol for session ordering, identity, feedback suppression,
  and Program publication. File composition alone is not a bidirectional connector.
- Evaluate MeshSync as a possible reusable implementation/reference only after
  checking its license, dependencies, supported Blender build, and actual property
  synchronization semantics. Avoid assuming its Unity integration is reusable as-is.
- Evaluate SDK build/distribution cost. A Hydra render delegate is a separate
  project and is not required to translate USD into the Advanced renderer.

## Acceptance

Load one layered scene with repeated assets and a shot override layer. Update the
source geometry and verify placement survives, then rename/reparent a source prim.
Demonstrate deliberate override resolution and publication across two output nodes.
Document any Blender round-trip losses and compare complexity against the native link.
