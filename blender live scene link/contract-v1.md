# Scene link contract v1

This contract defines how a Blender scene is represented in Media Server:
identities, units and matrices, the placement policies, and what content the
link carries. The reference fixture and its importer checks are executable
(see the [fixture README](../tests/fixtures/blender-scene-link/README.md)).
An unknown contract version is rejected before a linked document changes.

## Qualified environment

Blender pin: **5.2.0 LTS**, Windows release build **fbe6228777e7**. The
fixture generator refuses other version tuples. Record the exact build hash
with every run; another build hash, patch release, OS or exporter needs its own
qualification. The generator uses the bundled `io_scene_gltf2` exporter with
GLB, Y-up, materials, custom properties and cameras enabled, animation and
lights disabled, and no mesh compression. Blender factory settings isolate user
add-ons and preferences.

[Reference fixture measurements](reference-fixture-measurements.md) record the
host and timing boundaries. The fixture is procedural and has no licensed
external assets.

## Identity and ownership

| Concept | Identity and meaning |
| --- | --- |
| Source scene | Persisted UUID identifying the Blender scene, independent of its file path. |
| Linked collection | Persisted source UUID selecting the subtree/content to link. A collection's display name is editable. |
| Link instance | Media Server UUID for one attachment of a source scene/collection. Two attachments have independent stage alignment and shot decisions. |
| Source object | Persisted Blender UUID for an authored object, including transform-only parents; renaming/reordering does not replace it. |
| Geometry prototype | UUID for local-space evaluated mesh identity. Distinct content hashes identify its immutable revisions. |
| Material | Persisted UUID plus immutable resource revision; material binding and editable factor overrides are separate. |
| Occurrence | Persistent identity within a link instance for a placed prototype. Direct objects use the link/object pair; generated instances use a stable source key. Array order, pointer and content hash are not occurrence IDs. |
| Shot override | Property-level Media Server state owned by an occurrence, separate from source properties and stage alignment. |
| Revision | Immutable candidate containing a consistent hierarchy, bindings and exact resource revisions; ordered within a link session, with source/base revision checks. It is not an object identity. |

The fixture uses deterministic UUIDv5 constants so regeneration is comparable.
That is fixture generation only; the Blender add-on mints and persists its own
identities (see [Identity and overrides](identity-and-overrides-v1.md)), repairs
duplicates deliberately and retains identity through undo. Copied objects get new
object IDs; copied mesh datablocks get new prototype IDs. Do not derive
production IDs from mutable names.

## Space, matrices and cameras

All link distances are meters. Fixture Blender `scale_length` is 1. A publisher
applies `scale_length` to geometry and local translations exactly once. Blender
uses right-handed Z-up; Media Server uses right-handed Y-up with -Z view
forward. A point converts as `C * p`, where the rows of C are:

```text
 1  0  0  0
 0  0  1  0
 0 -1  0  0
 0  0  0  1
```

Thus `(x,y,z) -> (x,z,-y)`, determinant +1. Local/world transforms convert as
`M_ms = C * M_blender * inverse(C)` after unit normalization. The glTF exporter
already performs this basis conversion: **do not apply C again when loading GLB**.
Prototype payloads use identity occurrence transforms and converted local
geometry.

Matrices multiply column vectors; stored element `(row,column)` is at
`column*4+row`, matching `scene::Mat4` and HLSL `mul(matrix,vector)`. Translation
is at indices 12-14. Source world transforms are `parent_world * local`; root
parents are identity. Parent inverse, constraints and modifiers are resolved into
the evaluated local matrix before the link sends it. Parents precede children in
evaluation; missing parents, cycles and duplicate IDs reject the candidate
atomically.

Full affine matrices are kept through hierarchy composition: rotated children
under nonuniform parents may introduce shear, and no lossy Euler/TRS
decomposition is used. The document's turns-based yaw/pitch/roll is not an
interchange rotation representation.

Normals use normalized `transpose(inverse(M_3x3)) * n`. Tangents transform as
directions, are orthogonalized against the normal, and their handedness changes
by the sign of the determinant. Negative scale is meaningful: reflected triangle
winding/front-face parity must agree with outward normals, and a baked
reflection reverses winding exactly once. Drawable occurrences with a negative
world determinant are currently refused (see the feature table).

Admission rejects nonfinite matrices, nonaffine bottom rows and
singular/ill-conditioned bases: the bottom row must be exactly `(0,0,0,1)`,
basis-column lengths lie in [1e-6, 1e6], and the absolute determinant divided
by the product of column lengths exceeds 1e-6. These are admission tolerances,
not renderer math. The inverse helpers' identity fallback on singular input is
never treated as successful validation. Fixture positions and material factors
compare at 2e-5 absolute tolerance and normals at 2e-4: the pinned exporter's
`primitive_extract.py` rounds normals to four decimal places before normalizing
(`io/com/constants.py`, `ROUNDING_DIGIT = 4`), which the sloped reference face
exposes. Deterministic node evaluation after admission uses the repository's
floating-point rules.

The composition camera is perspective, square pixels, 1280x720, horizontal sensor
fit, 36 mm sensor, 45 mm lens, zero shift, near/far 0.05/200 m. It looks from
Blender `(7,-10,6)` to `(0,0,0.75)`, or Media Server `(7,6,10)` to `(0,0.75,0)`.
Media Server's world-up hint is `(0,1,0)`; camera world orientation derives from
the converted camera matrix. Vertical FOV in turns is
`2*atan((sensor_width/aspect)/(2*focal_length))/(2*pi)` (recorded in
reference.json). Blender camera-local -Z is forward and +Y is up; no additional
camera reflection is needed. Shifted, orthographic and panoramic cameras and lens
distortion are outside the link. The fixture camera is an acceptance reference,
not tracking or physical-stage calibration; stage alignment is a separate
transform.

## Placement policy

Let `A` be physical-stage alignment, `P` the effective parent world transform in
linked-scene space, and `L` the incoming source-local matrix. With no override:
`W = A * P * L`. Descendants inherit effective parent placement.

For **Follow source with offset**, the occurrence stores a local right offset
`O`: `W = A * P * L * O`. A user requests world placement `D`; the stored offset
is `O = inverse(A * P * L) * D`. Later source or parent changes follow with the
same offset. A geometry-only revision never touches L or O.

For **Hold shot placement**, the occurrence stores `H = inverse(A) * D` in
linked-scene world space and evaluates `W = A * H`. The Blender-side value is
`inverse(P) * H`, compensated for the source local matrix. Upstream parent edits
do not move a held occurrence. Stage alignment still moves the entire link and
is deliberately outside H. Source-local and shot state are never fed back as a
new combined source transform.

Origin/pivot changes are a prototype-space change, not an ordinary placement
edit. They are refused (`Origin changes require an explicit geometry/placement
rebase`) and the last complete revision stays, because compensating only the
object origin would lose shot decisions. Constraints, rigs, animated parents and
shear in a written-back object refuse reverse placement with a specific
diagnostic; the edit stays in Media Server (see [Writeback](protocol-v1.md#writeback)).

Reference scenario for placement and geometry edits together:

1. Link revision 1, place the Hero prop relative to the camera in Media Server,
   and receive that placement in Blender.
2. Raise the shared mesh's top +X edge by 0.25 m in Blender local Z. Every shared
   occurrence changes geometry, with each shot placement unchanged in both
   applications.
3. Repeat under both placement policies, source-parent changes, save/reopen,
   reconnect and undo. Undoing a placement does not undo a remote geometry
   revision.

## Content and failure contract

| Content | Behavior |
| --- | --- |
| Static meshes | Evaluated triangulated geometry, normals, UV0, local prototype space; immutable GLB resources. |
| Hierarchy | Transform-only parents and mesh occurrences with stable IDs and full affine source transforms. |
| Repeated assets | Multiple occurrences may share one prototype revision; independent placements and visibility. |
| PBR | Metallic/roughness, linear base-color/emissive factors, alpha mode/cutoff, double-sided flag; supported glTF image slots only. Base color/emissive images decode sRGB, data/normal maps linearly. |
| Visibility | Source visibility and local override are distinct. Parent hiding is inherited. Hidden content is different from source deletion. |
| Placement | Source placement plus an explicit follow-offset or hold-placement policy. |
| Arbitrary Blender shader graphs | Unsupported inputs are diagnosed or use an explicitly baked equivalent; Cycles/EEVEE pixel parity is not claimed. |
| Geometry Nodes, scatter, terrain | Authoring stays in Blender; evaluated geometry participates. See [Environment scale](environment-scale-v1.md). |
| Animation, lights | Explicit [animation assemblies](animation-and-stage-v1.md) and [native lights](materials-and-lighting-v1.md); content outside those subsets is diagnosed, never published as a misleading static substitute. |

Reference appearance: copper side faces `(0.8,0.2,0.1)`, metallic 0.7, roughness
0.35; cyan top `(0.05,0.75,0.9)`, metallic 0.1, roughness 0.8; neutral gray
floor 0.18, roughness 0.9. Values are linear. The fixture has no texture maps.
Blender's area light and Media Server's directional light provide visibility for
inspection, not a photometric image match. Display transforms are not baked into
factors.

| Fixture object | Expected Media Server world placement in revision 1 |
| --- | --- |
| Placement parent | Translation `(1,0,0)`, +90 degrees around Y. |
| Hero prop, renamed from Prop before rename | Translation `(1,1,-1)`; matrix rows `[0,0,1,1]`, `[0,0.5,0,1]`, `[-2,0,0,-1]`, `[0,0,0,1]`. |
| Shared prop | Translation `(-2,0.5,0)`, identity basis. Same mesh as Hero. |
| Mirrored prop | Translation `(3,0.5,0)`, scale `(-1,1,1)`. Same mesh. |
| Hidden prop | Translation `(0,0.5,-4)`, excluded from visible export. |
| Floor | Y=0, X=[-5,5], Z=[-4,3]. |

Revision 2 slopes the cyan top, changing prototype max local Y from 0.5 to 0.75 m
without moving any origin. Its diagonal top normals exercise inverse-transpose
normal conversion under Hero's nonuniform scale. Full per-object matrices, IDs,
resource hashes and material bindings are in
[reference.json](../tests/fixtures/blender-scene-link/reference.json). Each
`.expected` file records the Blender-evaluated world position, normal, RGB,
metallic and roughness for each triangle corner. The importer test matches their
multiset, so exporter triangle and material ordering is not an identity contract.

## Integration seams

- Link changes enter the Director through its validated command path: a
  document replacement with a transaction ID, base/result versions and an apply
  frame, grouped as one undoable transaction. Blender never writes to render
  nodes.
- Prototype and resource payloads are content-addressed immutable `.msmodel`
  packages built on a worker; placement stays out of prototype payloads and
  unchanged resources are reused. See [Scene manifest](scene-manifest-v1.md).
- Asset readiness and the Director's frame authority gate when a changed asset is
  revealed on every node; the link carries control and identity, not render-node
  traffic. See [Program publication](program-publication-v1.md) for frozen
  revisions.
- Disconnecting Blender never stops playback; accepted packages keep rendering.
