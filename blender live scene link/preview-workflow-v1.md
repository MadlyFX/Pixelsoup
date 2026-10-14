# Linked static scenes in Preview

Step 07 completes the local static authoring loop built by steps 01–06. The
qualified Blender build and supported source subset remain those in the
[publisher contract](publisher-v1.md) and [shot contract](shot-placement-v1.md).
Use the [current operator workflow](operator-guide.md) to exercise the workflow.

## Hierarchy and editing

The Scene panel has a Linked collections section. Each attachment is a selectable
collection row with its source name and nested source objects. Empty source
parents and removed-object recovery records remain addressable. Collapse state
belongs to the local UI; filtering finds the matching collection context and
exposes its hierarchy. Range selection follows the rows actually displayed.

Selecting a collection exposes stage alignment and its connection controls.
Selecting a prop exposes shot world position, Hold/Follow placement policy and
the supported material controls. Reset actions appear for authored placement,
visibility and material overrides. Hide/Show creates an explicit shot visibility
override; Reset visibility resumes source visibility. Delete on a linked prop
hides it, retaining its identity for recovery. Collections use the explicit
Unlink action instead of ordinary Delete. A collection lock also blocks edits
to its children. Linked members cannot enter ordinary Group/Ungroup operations.

Incoming source updates remain worker-ingested, bounded, validated and atomically
admitted. Local shot edits can continue while geometry is being ingested; the
Director replans against the current document before committing. The existing
model readiness path keeps the last accepted drawable appearance until a complete
replacement is ready. No source parsing, mesh conversion or resource upload was
added to frame recording.

## Connection and update state

The collection and prop Inspectors show the source revision and typed status:
connected, updating, paused, disconnected or refused. Diagnostic text wraps to
the Inspector width. Collection rows show non-idle states beside their names.
Start/connect and connection details are beside the selected link; the existing
File menu provides the same listener controls. There is one local paired source
connection at a time. Disconnect stops the listener and revokes its credentials.

Pause source updates is saved on the attachment. It gates source resource
announcements and source transactions, with a second check before admission, so
pausing during ingestion also prevents that pending revision from committing.
The connection and shot controls remain usable. The add-on reports the pause
without disconnecting and retains its pending evaluated geometry. Resume the
collection, then select Publish Now in Blender. Resuming alone does not silently
retry an earlier refused edit.

Attachments of the same source share incoming geometry. Pausing any attachment
gates that source across the project until that attachment is resumed. A pause is distinct from
an asset refusal: both keep the last accepted content, while only the latter
sets the failure state. A disconnected scene still renders from accepted data.

## Copies, duplicates and unlink

| Action | Result | Future source updates |
| --- | --- | --- |
| Copy independent snapshot, or Ctrl+C with selection confined to one linked collection; Ctrl+V in unlocked Preview | Ordinary group and drawable models with new document IDs | Frozen at copy time |
| Duplicate with shared source | New linked attachment and occurrences; copied alignment, policies and overrides | Shared source geometry; independent shot edits |
| Project scene Create Copy | Copied scene with new attachment/occurrence IDs | Shared source geometry; independent shot edits |
| Unlink collection | Convert that attachment in place into an ordinary independent group and models | Frozen at unlink time |

Copying a collection member captures its whole collection. The snapshot clipboard
is Director-local and is cleared on project load. Independent copies reuse
immutable accepted package paths; they do not create a new live source dependency.

Snapshot/unlink resolves the current composed affine world placement, effective
visibility and supported material-factor mask. It retains the original package's
per-slot materials for factors without overrides. Models are flattened under an
identity group; source-only Empty parents and removed-object tombstones are
omitted. Translation stays editable in ordinary TRS fields, while the saved
linear affine residual preserves shear exactly. Later ordinary transforms compose
with that residual. The result has no `link_*` properties. Accepted source and
prototype records remain in the project for other attachments and history.

Unlink is one atomic Scene undo transaction. It stops the current pairing so the
next automatic source update cannot recreate the attachment. Undo restores the
complete link records and shot state; restarting pairing is explicit because
credentials are ephemeral. Locked collections or members must be unlocked before
unlink. Paste is also a single Scene undo transaction, and ordinary numeric
gestures use the existing semantic coalescing. Source revisions use a separate
history context so local placement undo retains subsequently accepted geometry.

The existing reverse-placement contract requires one unambiguous attachment in
Preview. Multiple shared attachments can receive source geometry, but Blender
shot display refuses the ambiguous reverse placement. Independent snapshots are
appropriate when another freely editable copy does not need continued publishing.

## Persistence and Program

A saved `.mstage` carries source and attachment UUIDs, accepted revisions, source
hierarchy, shot overrides, pause state and immutable `.msmodel` resource references.
An offline reopening on the same machine reproduces the saved accepted scene
without Blender or a listener. Disconnect and unlink do not remove accepted
packages. Keep the accepted resource directory with the project: v1 uses absolute
package references and does not yet embed a portable asset bundle. Moving the
project to another machine requires the separate resource-distribution work.

Save the `.blend` to retain its source IDs. After reopening both applications,
start Scene Link in the saved Preview, copy fresh details and Connect & Publish
from that collection. Registration recovers the accepted revision and publication
keeps the existing shot placement.

Preview/Program controls retain their existing authority. Authoring targets
unlocked Preview. A source used by Program is refused by the existing shared-source
guard, so an incoming revision cannot silently alter Program. Coordinated asset
distribution and frame-aligned Program adoption remain step 08.
