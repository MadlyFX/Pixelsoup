# 03 — Scene ingestion and asset revisions

Status: proposed. Depends on: 02.

## Outcome

Import a complete hierarchy without collapsing the environment into one model,
and replace individual assets without rebuilding unaffected content.

## Work

- Define a scene manifest with IDs, parent references, prototype references,
  transforms, material bindings, and required immutable resource revisions.
- Use per-prototype glTF/GLB payloads initially where the importer supports them.
  Preserve local geometry space; avoid baking scene placement into each prototype.
  Carry identity in the manifest rather than relying on exporter node order.
- Reuse background ingestion and model packages. Cache unchanged geometry and
  textures independently so moving an object never recompresses its textures.
- Stage a complete candidate revision, validate references and capacity, then
  commit its document changes as one transaction. Never expose half a hierarchy.
- Add cancellation and supersession. A late worker result cannot resurrect a
  deleted object or replace a newer asset revision.
- Keep previous resources until users of the prior revision have released them.
  Bound pending bytes, jobs, CPU scratch, upload work, and retained generations.
- Separate persistent resource identity from machine-local cache locations.

## Acceptance

Replace one prop in a multi-object scene and prove unaffected assets are reused.
Reject missing resources and malformed manifests atomically. Rapidly submit two
revisions, finish the older one last, and verify that it cannot overwrite the newer
one. Check that a placement-only change performs no geometry or texture ingestion.
