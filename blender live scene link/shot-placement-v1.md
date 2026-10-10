# Bidirectional shot placement v1

> **Superseded (add-on 0.10.0).** The managed wrapper layer and the claim/commit
> messages below were removed. Media Server's overrides are now written into the
> Blender objects themselves and an artist's edit absorbs them; see Writeback in
> [protocol v1](protocol-v1.md). This page records the step 06 design.

Step 06, add-on 0.2.0. Qualified on Blender 5.2.0 LTS / fbe6228777e7, Windows.
The Director remains the authority for Preview. Shot edits target a specific
occurrence and never modify source geometry, source placement or Program.

## Managed Blender layer

Each supported object gets a tagged Empty between its authored parent and itself.
The object's `matrix_basis`, original `matrix_parent_inverse`, mesh and modifiers
are retained. The Empty carries `ms_link_shot_wrapper = <source object UUID>`.
Extraction skips these wrappers, follows the authored parent, and extracts
`original_parent_inverse * matrix_basis`. It never exports the combined shot pose.
The wrapper correction is stored in its parent inverse, which retains affine
shear without decomposing it into location/rotation/scale.
Root wrappers use a tagged identity Empty named **Shot display origin** as their
parent, since Blender does not evaluate a parent inverse on an unparented object.
Extraction also skips that anchor and retains the object's authored root status.

For authored local L, effective parent P and desired world W, the wrapper stores
`inverse(P) * W * inverse(L)`. W uses the existing follow/hold contract and stage
alignment, converted from metres/Y-up into Blender coordinates. Corrections are
recomputed after source edits; incoming geometry and object rename do not clear
shot offsets. Updates explicitly drive the dependency graph under a suppression
guard. The source serializer compares source records, so a returned shot pose
does not generate a source revision or another mesh extraction.

**Show Source Placement** displays the authored hierarchy and **Show Shot
Placement** restores the accepted shot display. Ordinary source-object transforms
and Edit/Sculpt operations edit source data. Use Source display for reparenting
and source visibility gestures. **Begin Shot Gesture** claims placement and selects
the wrapper; transform that wrapper and click **Commit Shot** within 15 seconds.
Managed display changes themselves are not native Blender undo operations.

Visibility is a managed viewport display: the source render-visibility property
is untouched. `ms_link_source_hidden` retains source viewport hiding while the
Director's effective visibility is displayed via `hide_set`. To alter source
viewport hiding, use Source display. Shot Hide/Show/Follow Source Visibility change
only the occurrence. Blender final render visibility translation is outside this
Preview workflow. Saved wrappers retain the accepted pose across reopening;
pairing credentials are never saved. Reconnect refreshes authoritative state.

## Qualified and refused cases

One Preview attachment per linked source, at most 512 source objects including
ancestors. Multiple attachments are explicitly refused for reverse display because
one source object cannot depict two independent shot occurrences simultaneously.
Writable static mesh/empty hierarchies support positive scale and affine shear.
Local-space Bevel, Subdivision Surface, Solidify, Triangulate, Decimate, Edge Split,
Weighted Normal, Weld and Smooth modifiers are admitted for reverse display.
Other modifier types require qualification because changing their world context
could change the extracted geometry. Forward-only publishing retains step 05's
evaluated-content contract.

Constraints, animation/drivers on objects or parents, rigs, bone/vertex parenting,
library/override objects, and singular transforms are refused before creating
wrappers. If unsupported authoring is introduced later, the add-on restores the
source hierarchy and reports the reverse-placement refusal. Reflected drawables
retain the earlier rendering qualification restriction. [Step 11](animation-and-stage-v1.md)
adds a managed static assembly-placement empty around an animated collection;
that empty participates in these shot gestures while node/skin animation stays
inside the source hierarchy and published model package. The implementation follows Blender's
[object transform API](https://docs.blender.org/api/5.0/bpy.types.Object.html) and
[transform model](https://developer.blender.org/docs/features/objects/transform/).

## Transport and ownership

The existing MSL4/v1 envelope adds four kinds:

| Kind | ID | Request / response |
| --- | --- | --- |
| ShotState | 13 | Request base is last state hash; response revision is current hash, payload empty if unchanged |
| ShotClaim | 14 | Transaction is gesture UUID, offset is occurrence ID, payload `placement` or `visibility`, base is the property's revision |
| ShotCommit | 15 | Same gesture/occurrence/base, payload is a supported edit |
| ShotRelease | 16 | Release current gesture by transaction identity |

ShotState payload is `MSSHOT 1` then one line per occurrence. Each line contains:
node ID, attachment ID, quoted object UUID, quoted placement and visibility
revision hashes, held flag, visibility override (-1/follow, 0/hidden, 1/visible),
effective visible flag, removed flag, then four column-major 16-float matrices:
offset, held world, stage alignment, effective world. Formatting uses nine
significant digits. State and IPC responses are capped at 1 MiB; other shot
controls remain capped at 4096 bytes. Duplicate object UUIDs across attachments
produce the explicit multiple-attachment refusal in Blender.

Supported placement edits: `world <16 values>`, `hold`, `follow`, `reset`, and
`restore <held 0/1> <16 offset values> <16 held-world values>` for guarded undo.
Visibility edits: `hide`, `show`, `follow`. Trailing fields and invalid affine
matrices fail admission. Undo restore changes only placement properties.

Revisions are compare-value SHA-256 tokens over the property's canonical accepted
state, not global document revisions. Placement includes policy, stored matrices,
conversion basis and effective world; visibility includes its own override and
effective state. Geometry/material revisions cannot invalidate a placement claim.
Source parent and stage changes invalidate placement when they change conversion.
Equivalent accepted states have equivalent tokens, including after undo.

One claim per connection lasts 15 seconds, with no silent renewal. Completion,
explicit release, disconnection or pairing shutdown releases ownership. Director
gestures have priority: an intervening change to the claimed property causes the
Blender commit to fail with an explicit conflict. Validation repeats at Director
scheduling time, including expiry, Preview/lock/project/scene checks and the
property revision. A conflict never invokes an overwrite retry. **Accept Director**
restores the authoritative display; **Retry My Shot** explicitly claims the latest
state and submits the retained proposed edit. An expired uncommitted gesture
retains its displayed proposed pose for explicit retry. The underlying service
admits one connection.

Shot commit receipts use the same bounded, idempotent receipt table as source
transactions. Exact replay cannot apply a gesture twice. State is fetched by the
serializer process every 250 ms while idle and after each accepted transaction;
serialization is cached by immutable document version. Blender's main thread
only polls bounded IPC and applies event-driven state; it performs no network wait.
Origin is supplied by the paired envelope and returned state is marked Director
origin. State hashes and source/shot separation suppress echo, rather than relying
on timing delays to guess whether a transform is local or remote.

## Undo and recovery

Native Media Server Scene Undo/Redo keeps local gestures separate from incoming
authoring transactions (reserved undo context `0x4c494e4b`). A source geometry
publication cannot become the next local Scene undo item. Existing strict undo
postconditions report conflicts instead of replacing a changed remote node.

Blender **Undo My Last Shot Edit** retains up to 64 local accepted edits as scalar
property records. It claims the accepted postcondition and submits the prior
placement or visibility as a new validated transaction. Later source geometry is
preserved; intervening edits to the same property require explicit resolution.
Use native Blender Undo/Redo for source geometry and source placement; subsequent
publishing sends the resulting source state. File load clears ephemeral local
shot history. Network failure cancels an unfinished gesture and restores the last
accepted display. Accepted source resources remain usable offline.
