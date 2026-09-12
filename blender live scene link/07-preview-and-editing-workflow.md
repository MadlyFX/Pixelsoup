# 07 — Preview and editing workflow

Status: implemented for the qualified static Preview workflow. Depends on: 06.

Implementation: [Preview workflow contract](preview-workflow-v1.md),
[Blender add-on](../integrations/blender/README.md), and
[UI acceptance walkthrough](ui-checks-step-07.md).

Linked collections now have a selectable, collapsible source hierarchy, nearby
connection and pause controls, explicit override/reset controls, independent
copy/paste, shared-source duplication and atomic unlink with undo. Accepted
geometry and shot placement persist for offline project reopening. Program
publication remains the separate step 08 deliverable.

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
