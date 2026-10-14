# Scene link contract v1

Step 01 qualification contract, 2026-09-09. This version defines the static
workflow to implement in steps 02–08. The fixture generator and importer checks
are executable today; a live connection and reverse placement are later steps.
An unknown contract version must be rejected before changing a linked document.

## Qualified environment

Initial Blender pin: **5.2.0 LTS**, Windows release build **fbe6228777e7**.
The generator refuses other version tuples. Record the exact build hash with
every run; another build hash, patch release, OS, or exporter needs qualification.
The bundled `io_scene_gltf2` exporter is used with GLB, Y-up, materials, custom
properties and cameras enabled, animation and lights disabled. No optional mesh
compression is enabled. Blender factory settings isolate user add-ons/preferences.

See [baseline](baseline-v1.md) for measured host, renderer, network and timing
boundaries. The fixture is procedural and has no licensed external assets.

## Identity and ownership

| Concept | Identity and meaning |
| --- | --- |
| Source scene | Persisted UUID identifying the Blender scene, independent of its file path. |
| Linked collection | Persisted source UUID selecting the subtree/content to publish. A collection's display name is editable. |
| Link instance | Media Server UUID for one attachment of a source scene/collection. Two attachments have independent stage alignment and shot decisions. |
| Source object | Persisted Blender UUID for an authored object, including transform-only parents; renaming/reordering does not replace it. |
| Geometry prototype | UUID for local-space evaluated mesh identity. Distinct content hashes identify its immutable revisions. Shared mesh datablocks can share a prototype only while evaluated geometry and relevant bindings agree. |
| Material | Persisted UUID plus immutable resource revision; material binding and editable factor overrides are separate. |
| Occurrence | Persistent identity within a link instance for a placed prototype. Direct objects use the link/object pair; generated instances require a stable source key. Array order, pointer and content hash are not occurrence IDs. |
| Shot override | Property-level Media Server state owned by an occurrence, separate from source properties and stage alignment. |
| Publication revision | Immutable candidate containing a consistent hierarchy, bindings and exact resource revisions; monotonically ordered within a link session, with source/base revision checks. It is not an object identity. |

The fixture uses deterministic UUIDv5 constants to make regeneration comparable.
This is fixture generation only; a publisher must mint/persist identities, repair
duplicates deliberately, and retain identity through undo. Copied objects require
new object IDs; copied mesh datablocks require new prototype IDs. Read-only library
data requires a persisted external identity mapping. Do not derive production IDs
from mutable names using this generator's convenience function.

## Space, matrices and cameras

All link distances are meters. Fixture Blender `scale_length` is 1. A publisher
must apply `scale_length` to both geometry and local translations exactly once.
Blender uses right-handed Z-up; Media Server uses right-handed Y-up, with -Z view
forward. Convert a point with `C * p`, where the rows of C are:

```text
 1  0  0  0
 0  0  1  0
 0 -1  0  0
 0  0  0  1
```

Thus `(x,y,z) -> (x,z,-y)`, determinant +1. Convert local/world transforms with
`M_ms = C * M_blender * inverse(C)` after unit normalization. The glTF exporter
already performs this basis conversion: **do not apply C again when loading GLB**.
Prototype payloads use identity occurrence transforms and converted local geometry.

Matrices multiply column vectors; stored element `(row,column)` is at
`column*4+row`, matching `scene::Mat4` and HLSL `mul(matrix,vector)`. Translation is
at indices 12–14. Source world transforms are `parent_world * local`; root parents
are identity. Parent inverse, constraints and modifiers must be resolved into the
evaluated local matrix before publishing. Parents precede children in evaluation;
missing parents, cycles and duplicate IDs reject the candidate atomically.

Keep full affine matrices through hierarchy composition: rotated children under
nonuniform parents may introduce shear. No lossy Euler/TRS decomposition is
allowed. The existing document's turns-based yaw/pitch/roll is not an interchange
rotation representation. Step 02 needs an affine carrier/validation path.

Normals use normalized `transpose(inverse(M_3x3)) * n`. Tangents transform as
directions, are orthogonalized against the normal, and their handedness changes
by the sign of the determinant. Negative scale is meaningful; reflected triangle
winding/front-face parity must agree with outward normals. If baking a reflection
into geometry, reverse triangle winding exactly once. Never fix it by negating
positions or disabling culling globally. The reference tests check positions and
normals of the reflected occurrence; pixel/front-face qualification remains
required before advertising arbitrary negative-scale live scenes.

Reject nonfinite matrices, nonaffine bottom rows and singular/ill-conditioned
bases before accepting a revision. Proposed validation: affine residual <= 1e-6,
basis-column lengths in [1e-6, 1e6], and absolute determinant divided by the product
of column lengths > 1e-6. These are admission tolerances, not changes to renderer
math. Existing inverse helpers return identity on singular input; that fallback
must not be treated as successful link validation. Compare fixture positions and
material factors at 2e-5 absolute tolerance, normals at 2e-4: the pinned Blender
exporter's `primitive_extract.py` rounds normals to four decimal places before
normalizing (`io/com/constants.py`, `ROUNDING_DIGIT = 4`). The sloped reference
face exposes this quantization. Deterministic node evaluation still uses the
repository's floating-point rules after admission.

The composition camera is perspective, square pixels, 1280x720, horizontal sensor
fit, 36 mm sensor, 45 mm lens, zero shift, near/far 0.05/200 m. It looks from
Blender `(7,-10,6)` to `(0,0,0.75)`, or Media Server `(7,6,10)` to `(0,0.75,0)`.
Media Server's world-up hint is `(0,1,0)`; derive camera world orientation from
the converted camera matrix. Vertical FOV in turns is
`2*atan((sensor_width/aspect)/(2*focal_length))/(2*pi)` (recorded in reference.json).
Blender camera-local -Z is forward and +Y is up; no additional camera reflection
is needed. Shifted/orthographic/panoramic cameras and lens distortion are outside
the initial link subset. Camera import is an acceptance reference, not tracking or
physical-stage calibration. Stage alignment remains a separate transform.

## Placement policy and primary acceptance

Let `A` be physical-stage alignment, `P` the effective parent world transform in
linked-scene space, and `L` the incoming source-local matrix. With no override:
`W = A * P * L`. Descendants inherit effective parent placement.

For **Follow source with offset**, store occurrence-local right offset `O`:
`W = A * P * L * O`. A user requests world placement `D`; store
`O = inverse(A * P * L) * D`. Subsequent source/parent changes follow with this
same offset. A geometry-only revision never touches L or O.

For **Hold shot placement**, store `H = inverse(A) * D` in linked-scene world
space, and evaluate `W = A * H`. Compensating local placement for Blender is
`inverse(P) * H`. The managed wrapper must compensate for the original source
local matrix as well if it parents that original object. Upstream parent edits
must not move the held occurrence. Stage alignment still moves the entire link;
it is deliberately outside H. Source-local and shot state must never be fed back
as a new combined source transform.

Origin/pivot changes are a prototype-space change, not an ordinary placement
edit. Initial live support must reject them with `unsupported_pivot_change` and
keep the last complete revision unless an explicit invertible basis compensation
is supplied and applied atomically to geometry and every occurrence. For a later
supported rebase `p_old = B * p_new`, update L to `L*B` and a follow offset to
`inverse(B)*O*B`; held H must also account for the chosen geometric anchor. Do not
claim arbitrary pivot preservation from a geometry-only test. Constraints, rigs,
animated parents, and unsupported shear in the reverse wrapper similarly refuse
reverse placement with a specific diagnostic.

Primary future integration scenario:

1. Link revision 1, place Hero prop to camera in Media Server, and receive its
   managed shot placement in Blender.
2. Raise the shared mesh's top +X edge by 0.25 m in Blender local Z. Publish
   revision 2; every shared occurrence changes geometry, with each shot placement
   unchanged in both apps.
3. Repeat under both placement policies, source-parent changes, save/reopen,
   reconnect and undo. Undo placement must not undo a remote geometry revision.

Step 01's executable test verifies the existing local document/source replacement
and save/reopen seam. It is **not** evidence of bidirectional synchronization.
The full scenario is a step 06/07 gate.

## Initial feature and failure contract

| Content | Initial behavior |
| --- | --- |
| Static meshes | Evaluated triangulated geometry, normals, UV0, local prototype space; immutable GLB resources. |
| Hierarchy | Transform-only parents and mesh occurrences with stable IDs and full affine source transforms. |
| Repeated assets | Multiple occurrences may share one prototype revision; independent placements and visibility. |
| PBR | Core metallic/roughness, linear base-color/emissive factors, alpha mode/cutoff, double-sided flag; supported glTF image slots only. Base color/emissive images decode sRGB, data/normal maps linearly. |
| Visibility | Source visibility and local override are distinct. Parent hiding is inherited. Hidden content is different from source deletion. |
| Placement | Source placement plus explicit follow-offset or hold-placement policy. |
| Arbitrary Blender shader graphs | Diagnose unsupported inputs or explicitly baked equivalents; never promise Cycles/EEVEE pixel parity. |
| Geometry Nodes, scatter, terrain tools | Authoring remains in Blender; only supported evaluated static geometry participates initially. Stable generated-instance identity and scale qualification belong to step 09. |
| Animation, rigs, cameras/lights beyond fixture | Deferred to steps 10/11; do not silently publish a misleading static substitute. |

Initial reference appearance: copper side faces `(0.8,0.2,0.1)`, metallic 0.7,
roughness 0.35; cyan top `(0.05,0.75,0.9)`, metallic 0.1, roughness 0.8; neutral
gray floor 0.18, roughness 0.9. Values are linear. The fixture has no texture maps;
image translation/performance needs its own dataset. Blender's area light and
Media Server's directional light provide visibility for inspection, not a
photometric image-match claim. Display transforms are not baked into factors.

| Fixture | Expected Media Server world placement in revision 1 |
| --- | --- |
| Placement parent | Translation `(1,0,0)`, +90 degrees around Y. |
| Hero prop, renamed from Prop before rename | Translation `(1,1,-1)`; matrix rows `[0,0,1,1]`, `[0,0.5,0,1]`, `[-2,0,0,-1]`, `[0,0,0,1]`. |
| Shared prop | Translation `(-2,0.5,0)`, identity basis. Same mesh as Hero. |
| Mirrored prop | Translation `(3,0.5,0)`, scale `(-1,1,1)`. Same mesh. |
| Hidden prop | Translation `(0,0.5,-4)`, excluded from visible export. |
| Floor | Y=0, X=[-5,5], Z=[-4,3]. |

Revision 2 slopes the cyan top, changing prototype max local Y from 0.5 to 0.75 m
without moving any origin. Its diagonal top normals exercise inverse-transpose
normal conversion under Hero's nonuniform scale. Full per-object matrices, IDs, resource hashes and material bindings are
in [reference.json](../tests/fixtures/blender-scene-link/reference.json). Each
`.expected` file records the Blender-evaluated world position, normal, RGB,
metallic and roughness for each triangle corner. The importer test matches their
multiset, so exporter triangle and material ordering is not an identity contract.

## Reusable seams and required changes

| Current seam | Reuse / required follow-on |
| --- | --- |
| `engine/cmd/.../command.hpp`, Director scheduler and reducer | PutNode/EraseNode with forward/inverse, transaction ID, base/result versions and apply frame. Link changes must enter Director validation, group as one transaction and integrate with undo. No Blender writes to render nodes. |
| `SceneModel::reset/apply/evaluate`, `scene.group`, `scene.model` | Delta-driven document projection, hierarchy and source/placement separation are reusable. Step 02 must persist source/link/object/prototype/occurrence IDs, full affine source matrices, property overrides, ownership/base revisions and deletion tombstones. Update validation, persistence, wire and digests together. |
| `model_loader.hpp` | Loads glTF/GLB/OBJ and bakes source node transforms into combined geometry. Animation nodes are specialized data, not an editable source hierarchy. Step 03 needs manifest-driven per-prototype ingestion. |
| `model_package.hpp`, `model_ingest_queue.hpp` | Worker ingestion, material validation/mips/compression and content-addressed immutable `.msmodel` packages. Keep placement out of prototype payloads; independently reuse unchanged resources. |
| `ModelCache::observe/refresh` and publication epochs | Bounded async decoded residency; stale complete assets survive refresh. Command-list epochs prevent mid-recording adoption. Add bounded cancellation/supersession without polling files on the frame path. |
| Asset readiness, commit permits, Director frame authority | Asset set/revision and renderer readiness gate scheduled Program release. Existing session messages carry control/identity, not the asset bytes. Step 08 must stage exact resources on every node and keep the previous complete generation until release. |
| Preview/Program switcher | Preview has independent authoring scene/playhead; Program stays on its published scene until swap at an exact frame. Step 07 adds link controls/status; step 08 coordinates complete publication. Disconnection cannot stop playback. |

Step 01 introduces no persistent production link schema or new editor panel.
Its UI deliverable is two ordinary `.mstage` reference projects, generated by the
qualification executable and usable via File > Open. The [current operator workflow](operator-guide.md)
exercises the existing scene, transform and import controls that later steps reuse.
