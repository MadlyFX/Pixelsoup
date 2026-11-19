# Editing linked collections

How a linked Blender scene appears and behaves in the editor. The supported
Blender content is in the [publisher contract](publisher-v1.md); the operator
procedure is in the [operator guide](operator-guide.md).

## Hierarchy and editing

In the Scene panel each linked collection is a selectable row with its source
name and nested source objects. Empty source parents stay addressable.
Collapse state belongs to the local UI; filtering finds the matching collection
context and exposes its hierarchy. Range selection follows the rows actually
displayed.

Selecting a collection (Inspector section **BLENDER SCENE LINK**) exposes stage
alignment (**Link stage alignment**, meters). Selecting a prop exposes the shot
world position, the **Hold shot placement** / **Follow source with offset**
policy, and the supported material or light controls. Reset actions appear only
for authored placement (**Reset placement to source**), visibility (**Follow
source visibility**) and material or light overrides (**Reset material factors to
source** / **Reset light properties to source**); an **Overrides:** line lists
what is overridden. **Hide this occurrence** / **Show this occurrence** create an
explicit visibility override. Edits are written back into the Blender object
where Blender can hold them (see the [add-on README](../integrations/blender/README.md)).
A collection lock also blocks edits to its children, and linked members cannot
enter ordinary Group/Ungroup operations. Scatter groups show their shared
instance count and are edited as a group.

Incoming source updates are worker-ingested, bounded, validated and atomically
admitted. Local edits can continue while geometry is being ingested; the Director
replans against the current document before committing. The model readiness path
keeps the last accepted drawable appearance until a complete replacement is
ready. No source parsing, mesh conversion or resource upload happens during frame
recording.

## Connection and update state

The Inspector's **BLENDER CONNECTION** section shows the accepted source revision
and one state: syncing with Blender, updating, paused, update refused, or
Blender closed or on another scene (saved content shown). Diagnostic text wraps
to the Inspector width. **Blender connection...** opens the connection dialog
(**File > Connect to Blender...**).

**Pause source updates** is saved on the attachment. It gates source resource
announcements and source transactions, with a second check before admission, so
pausing during ingestion also prevents that pending revision from committing. The
add-on reports the pause without disconnecting and retains its pending evaluated
geometry; **Resume source updates** sends the retained edit. Attachments of the
same source share incoming geometry, so pausing any attachment gates that source
across the project until that attachment is resumed. A pause is distinct from an
asset refusal: both keep the last accepted content, while only the latter sets
the failed state. A disconnected scene still renders from accepted data.

## Copies, duplicates and unlink

| Action | Result | Source updates |
| --- | --- | --- |
| **Copy independent snapshot**, or Ctrl+C with selection confined to one linked collection; Ctrl+V in an unlocked scene | Ordinary group and drawable models with new document IDs | Frozen at copy time |
| **Duplicate with shared source** | New linked attachment and occurrences; copied alignment, policies and overrides | Shared source geometry; independent shot edits |
| Duplicating a project scene | Copied scene with new attachment/occurrence IDs | Shared source geometry; independent shot edits |
| **Unlink collection - keep accepted scene** | Converts that attachment in place into an ordinary independent group and models | Frozen at unlink time |

Copying a collection member captures its whole collection. The snapshot clipboard
is Director-local and is cleared on project load. Independent copies reuse
immutable accepted package paths; they do not create a new live source
dependency.

Snapshot and unlink resolve the current composed affine world placement,
effective visibility and supported material-factor mask. They retain the original
package's per-slot materials for factors without overrides. Models are flattened
under an identity group; source-only Empty parents and removed-object tombstones
are omitted. Translation stays editable in ordinary TRS fields, while the saved
linear affine residual preserves shear exactly. Later ordinary transforms compose
with that residual. The result has no `link_*` properties. Accepted source and
prototype records remain in the project for other attachments and history.

Unlink is one atomic Scene undo transaction. It ends the current pairing so the
next automatic source update cannot recreate the attachment; Undo restores the
complete link records and shot state, and the link resumes when Blender opens the
scene again. Locked collections or members must be unlocked before unlink. Paste
is also a single Scene undo transaction, and ordinary numeric gestures use the
existing semantic coalescing. Source revisions use a separate history context so
local placement undo retains subsequently accepted geometry.

Writeback needs one unambiguous attachment: when the same source is attached
twice in a Blender scene, geometry still updates every attachment but Media
Server's edits stay in Media Server and the Blender sidebar says so. Use an
independent snapshot when a freely editable copy does not need continued syncing.

## Persistence

A saved `.mstage` carries source and attachment UUIDs, accepted revisions, source
hierarchy, shot overrides, pause state and immutable `.msmodel` resource
references. Reopening it on the same machine reproduces the saved accepted scene
without Blender. Disconnect and unlink do not remove accepted packages. Package
references are absolute, so keep the accepted resource directory (the project's
`<project> Blender/Link Resources`) at the same path on every machine that opens
the project.

Save the `.blend` to retain its source IDs. After reopening both applications,
open the scene in Blender with the **B**; registration recovers the accepted
revision and the existing shot placement is kept.
