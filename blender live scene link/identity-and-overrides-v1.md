# Linked identity and shot ownership v1

The persistent document records for linked scenes. The public authoring API is
[`linked_scene.hpp`](../engine/scene/include/mediaserver/scene/linked_scene.hpp).

## Persistent records

| Record | Identity / ownership | Mutable content |
| --- | --- | --- |
| `link.source` | `link_uuid`: source scene UUID | `link_version = 1` |
| `link.object` | `link_uuid` scoped to `link_source` | Name, ordering, parent, local matrix, prototype/material references, visibility, tombstone |
| `link.prototype` | `link_uuid` scoped to `link_source` | `link_revision` and `source`: exact local-space model resource |
| `link.material` | `link_uuid` scoped to `link_source` | `link_revision` and supported material factors |
| `link.instance` | Project-local `link_uuid` and persistent document ID | `link_source`, owning `scene_id`, stage alignment |
| `scene.model` with `link_instance` | Persistent document ID for `(link_instance, link_object)` | Placement policy, affine shot offset / held world, individual material factors, local visibility |

All references are positive signed 64-bit document IDs. UUIDs are canonical,
lowercase, nonzero 36-character strings. Display names, node ordering, filenames
and revision strings never supply identity. An attachment UUID uses the reserved
project-local namespace plus its allocated document identity. Attach/duplicate
and scene clone allocate new attachment identities and remap occurrence records.
Cross-project merging must remap this namespace; it is not a globally minted
Blender UUID. Source UUIDs are supplied and persisted by the source author.

Source records are shared project assets. Attachments and shot records are owned
by `scene_id`; Preview cloning shares source assets and copies shot state to new
attachment/occurrence identities. Deleting a native scene removes its attachments
and shot records, while the shared source records remain in the project.

`plan_link_source_update` accepts only source-owned records, rejects duplicate
document IDs and identity reassignment, and creates missing occurrences for new
source objects in every attachment. Asset updates never copy over shot records.
`plan_linked_change` validates a complete proposed document before authoring. Its
result can be committed in one Director document replacement/undo transaction.
The existing project and wire codecs carry all scalar properties without a
parallel persistence format. SceneModel caches the accepted projection on document
changes. Evaluation uses cached affine worlds and material factors without source
parsing or filesystem work. Source changes also participate in the document/scene
digest, even when world placement is held or an object is locally hidden.

## Exact transform semantics

Matrices retain column-major affine storage from contract v1. Each matrix is
sixteen double document properties with a prefix and index `0`–`15`:

- Source local: `link_local_0` … `link_local_15` on `link.object`.
- Stage alignment: `link_alignment_0` … `link_alignment_15` on `link.instance`.
- Shot offset: `link_offset_0` … `link_offset_15` on the occurrence.
- Held world: `link_held_0` … `link_held_15` on the occurrence.

An absent matrix means identity. A partially present matrix is an error. Hold
requires an explicitly captured held-world matrix. Values must remain finite
after conversion to the renderer's float carrier. The bottom row must be exactly
`(0,0,0,1)`, so a perspective residual is never silently discarded. Basis lengths must be in `[1e-6,1e6]` and
the absolute determinant divided by their product must exceed `1e-6`. Validation
also checks composed worlds and conversion bases. The full cofactor affine
inverse supports shear; its singular-input identity fallback is never accepted
as a successful conversion.

For source object `i`, let `L_i` be the evaluated source local matrix. The source
world without shot decisions is recursively `S_i = S_parent * L_i` (identity root
parent). For each attachment, let `A` be stage alignment, `O_i` the local shot
offset, `H_i` the held world, and `W_parent` the parent's effective shot world:

```text
B_i = (root ? A : W_parent) * L_i
Follow source with offset: W_i = B_i * O_i
Hold shot placement:       W_i = H_i
```

Thus follow is a local post-multiplied offset; a parent's shot adjustment flows
to following descendants. Hold captures the object's entire world frame and
remains fixed through source parent edits, reparenting, parent shot edits and
stage alignment changes. A following child of a held parent follows that held
world. To set world placement under follow, store `O_i = inverse(B_i) * W_target`.
Switching from hold to follow uses this same conversion and preserves the picture.
Switching to hold captures the current world. Reset placement selects follow and
sets the offset to identity. No Euler/TRS decomposition is used.

Source reparenting replaces `link_parent` and the evaluated local matrix in one
candidate. Cycles, missing/cross-source parents, invalid references and singular
conversions reject the whole candidate. SceneModel retains its last valid scene,
linked projection and digest if an invalid replicated snapshot arrives; a later
repair can adopt the complete corrected state. A clean restart has no previous
valid revision to fall back to and reports the invalid snapshot.

`link_origin` is an optional source pivot-generation token. Changing it through
source admission rejects the candidate with an explicit rebase diagnostic.
The publisher must preserve the existing prototype coordinate frame; quietly
moving the pivot and compensating only the object origin would lose shot
decisions. Arbitrary animated source parents and linked placement channels are
outside this static hierarchy contract. [Animation assemblies](animation-and-stage-v1.md)
keep their animated hierarchy inside the immutable model package, with one outer
linked occurrence for shot placement and effective playback settings.

Affine reflection can be represented mathematically, but drawable occurrences
with a negative world determinant are refused because the renderer's front-face parity for reflections is not
qualified. Shear and positive nonuniform scale are supported. Document admission
limits are 8,192 source objects, 16,384 occurrence records, and the scene model
limit per scene; these are admission limits, not performance qualification.

## Property ownership and recovery

Source visibility and local visibility are separate properties on separate nodes.
An absent occurrence `visible` follows source visibility. A locally authored value
overrides that object, while effective parent visibility and source removal still
gate the subtree. The Inspector's **Hide this occurrence** / **Show this
occurrence** author the local value; **Follow source visibility** removes it.

Source removal sets `link_removed = true` on the source object. It suppresses
descendants and drawing but retains document IDs, source hierarchy and shot
records. A source undo/restoration clears the tombstone and reuses those same
occurrences, so Undo on either side restores the object with every Media Server
override. Removed objects are not listed in the editor. Source updates cannot
erase source records as an implicit cascade. Prototype/material records remain
available while tombstones reference them.

Supported material properties are base-color RGB, roughness, metallic, and
emissive RGB. Absent shot properties follow their source material factor.
Per-property masks apply these factors to imported model material slots while
preserving unoverridden factors, textures and other imported material properties.
**Reset material factors to source** removes the local properties. Material and geometry
revision updates cannot overwrite local placement or visibility.

## Blender identity rules for the publisher

1. Source scene, object, mesh prototype and material UUIDs persist as `ms_link_id`
   custom properties. The publisher keeps a registry of the original owner of each
   UUID. Rename/reorder leaves the UUID untouched.
2. On a Blender duplicate, the UUID stays on the registered original and a new UUID
   is minted and persisted on the writable copy before it is sent. Each source
   object has its own evaluated-prototype UUID derived from its object UUID; a
   copied datablock therefore never shares a prototype ID with its original.
   Generated occurrences use a durable source instance key; array indices are not
   identity.
3. Read-only library data uses an authored UUID if one exists; otherwise a
   persisted `ms_link_library_ids` mapping on the local scene, keyed by library
   path, ID type and library name (at most 4096 entries). Renaming or moving a
   library entry with no authored UUID changes that key. A duplicate UUID among
   library data is refused with a diagnostic naming the block, and the author
   repairs the ID in the library.
4. Delete/undo restores the same UUID and clears the Director tombstone. A deleted
   object's UUID is not reused for unrelated content.

See the [operator guide](operator-guide.md) for the editor workflow.
