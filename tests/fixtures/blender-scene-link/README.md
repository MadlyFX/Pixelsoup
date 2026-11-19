# Blender scene-link reference fixtures

Procedural fixtures authored for this repository by
`scripts/blender-scene-reference.py`, using Blender 5.2.0 LTS `fbe6228777e7`.
No external assets or textures are used.

- `reference-r1.glb` and `reference-r2.glb`: full visible reference scene, with
  stable custom object IDs and camera, shared mesh, parenting and signed scale.
- `prop-r1.glb` and `prop-r2.glb`: the same shared mesh at identity placement for
  per-occurrence assembly in Pixelsoup. Revision 2 slopes the top +X edge.
- `.expected`: first line is triangle-corner count, then eleven floats per corner:
  world XYZ, world normal XYZ, linear base RGB, metallic, roughness. Generated
  from Blender evaluated meshes independently of GLB export. Corner order is
  incidental; the C++ check matches the multiset.
- `reference.json`: contract version, exact Blender build, source/collection/link/
  object/occurrence/material identities, column-major local/world matrices,
  required appearance, GLB hashes and recorded export samples.
- `workload-256.glb`: 256 repeated placements, used only by the requested offline
  performance benchmark. This is not a large-scatter qualification scene.

The generator also writes editable `.blend` files under the requested output
directory. They are reproduced on demand rather than checked in as opaque binary
sources. The C++ reference checker creates `.mstage` files and content-addressed
packages in its scratch directory, never beside these source fixtures.

See the [contract](../../../blender%20live%20scene%20link/contract-v1.md),
[measurements](../../../blender%20live%20scene%20link/reference-fixture-measurements.md) and
[operator guide](../../../blender%20live%20scene%20link/operator-guide.md).
