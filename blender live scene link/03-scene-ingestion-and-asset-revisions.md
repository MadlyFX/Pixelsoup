# 03 — Scene ingestion and asset revisions

Status: implemented (2026-09-09). Depends on: 02.

Use **File > Import Linked Scene...** in Preview. See the
[manifest contract](scene-manifest-v1.md) and [UI checks](ui-checks-step-03.md).

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

## Implementation and evidence

`render/linked_scene_ingest` stages a strict, complete `MSLINK 1` manifest on one
background worker. UUIDs map to existing document identities; omitted source
objects become recovery tombstones. Affine transforms, materials, references,
capacity, embedded GLB payloads and SHA-256 revisions are validated before a
candidate reaches the Director. Each UI import gets a fresh gesture/transaction
ID, so importing after a placement edit works and remains one undo step.

Prototype placement must be identity in GLB; the manifest owns hierarchy and
placement. Immutable packages use the existing model loader, mip/compression and
package writer. SHA-256 package indexes allow unchanged geometry reuse without
loading or decoding geometry. A separate bounded texture compression cache reuses
identical role-filtered images across geometry revisions. Worker-side hashing
still verifies input and cached package integrity for placement-only imports.

One running job and one replaceable pending request bound work. Cancellation and
request generations discard stale completions. The Director additionally rejects
results after project replacement, linked-source/attachment edits, occurrence
removal, target-scene deletion or a transition lock. Shot property edits made
while staging are preserved by planning against the latest document. Program
source attachments prevent a Preview import from updating Program indirectly.

Previous packages are never overwritten or garbage-collected automatically;
existing renderer generation ownership and bounded residency remain in force.
Admission limits, disk retention policy and remaining scope are documented in the
manifest contract. Frame recording gains no source parsing, compression or file
polling. Atomicity here concerns document admission; synchronized GPU adoption
across output nodes belongs to step 08.

Debug and Release GUI builds passed. Seven relevant CTest suites passed in each
configuration: linked-scene identity, linked ingestion, material ingestion,
Blender reference, model loader, model cache and Inspector. The new suite checks
single-prop replacement/reuse, placement preservation, no ingestion on placement
updates, malformed/missing/corrupt resources, graph errors, cancellation,
supersession, independent texture reuse, and a Director import/edit/update/undo
sequence. Serial execution prevents out-of-order workers from running in parallel;
the latest-request gate rejects an older completion after newer submission.

Native UI verification imported revision 1, held a prop, imported revision 2,
imported placement-only changes, and rejected a missing resource with the prior
scene intact. See the linked UI walkthrough and captured evidence.
