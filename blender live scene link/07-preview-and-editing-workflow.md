# 07 — Preview and editing workflow

Status: proposed. Depends on: 06.

## Outcome

The first complete user-facing workflow for linked static scenes in Preview.

## Work

- Show the linked collection as a scene hierarchy with ordinary selection and
  manipulation. Keep connection and update controls beside the linked scene.
- Expose placement policy, overridden properties, reset-to-source, unlink, and
  reconnect only where relevant. Unlink must define how assets and current composed
  placement become independent saved content.
- Show pending/failed content updates at the affected object or link. Distinguish
  a paused link from a disconnected source and a refused asset.
- Keep operator gestures responsive while ingestion runs. Retain the last valid
  appearance until a complete replacement is accepted.
- Persist the linked scene, overrides, and usable resource references. Opening a
  project without Blender must reproduce the last saved accepted scene.
- Integrate semantic undo coalescing and existing Preview/Program controls. Incoming
  authoring changes target Preview; do not silently change the current Program.

## Acceptance

Run the primary workflow without editing files or using developer controls.
Save, close Blender, reopen Media Server, and render the accepted scene. Reconnect
and receive another geometry change without losing placement. Confirm ordinary
copy/duplicate operations have defined independent versus shared-link semantics.
