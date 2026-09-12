# 06 — Bidirectional shot placement

Status: implemented for the qualified static hierarchy. Depends on: 05.

Implementation: [shot layer and transport](shot-placement-v1.md),
[Blender add-on](../integrations/blender/README.md), and
[UI acceptance walkthrough](ui-checks-step-06.md).

The Director returns accepted affine placement and visibility. Blender displays
them through tagged parent wrappers while retaining authored mesh and transform
channels. Shot gestures use expiring claims, independent property revisions and
explicit conflict resolution. Source/shot display controls, placement policies,
visibility reset and guarded local shot undo are available in the Blender sidebar.

## Outcome

Media Server adjustments appear in Blender and survive subsequent source authoring.

## Work

- Return accepted shot placement and visibility overrides to the Blender client.
  Represent them using a managed transform wrapper or another tested non-destructive
  mechanism, separate from the original source transform and mesh.
- Tag managed state so extraction does not send the combined placement back as a
  new source placement. Track origin and revision to suppress feedback loops.
- Specify how artist gestures target source placement versus shot placement. The
  artist must be able to see which layer is being edited and reset an override.
- Use short ownership claims for a placement gesture and revision checks at commit.
  Release claims on completion/disconnect. Simultaneous changes to independent
  geometry and placement properties should proceed without conflict.
- Route conflicting writes to the same property through an explicit resolution;
  never silently allow an old source snapshot to erase a shot decision.
- Handle rigs, constraints, animated parents, and singular transforms explicitly.
  Qualify simple static hierarchy first; refuse unsupported reverse placement.
- Define undo as a new validated edit to the relevant layer. Undoing a local gesture
  must not roll back unrelated remote geometry or replay an entire stale snapshot.

## Acceptance

Move a prop in Media Server, edit its vertices in Blender, and confirm its shot
placement stays fixed. Repeat with both placement policies, parent edits, undo,
and reconnection. Both applications converge without oscillation. Simultaneous
placement drags have a deterministic winner or an explicit unresolved conflict.
