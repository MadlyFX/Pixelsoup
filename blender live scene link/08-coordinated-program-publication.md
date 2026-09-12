# 08 — Coordinated Program publication

Status: implemented for the supported static collection workflow. Depends on: 07.

Implementation contract: [Program publication v1](program-publication-v1.md).
Operator walkthrough and qualification scope: [step 08 UI checks](ui-checks-step-08.md).
The reproducible local test uses one Director and two output processes; physical
two-machine performance/synchronization qualification remains a stage test.

## Outcome

All Program nodes reveal the same complete linked-scene revision on the same
authorized show frame, including the resource bytes required to draw it.

## Work

- Extend or reuse the existing asset-generation readiness and frame-permit path.
  Audit what it currently covers; do not introduce a competing frame authority.
- Add a publication manifest identifying the exact document revision, geometry,
  textures, animation, and required derived lighting resources.
- Provide asset staging through configured shared storage or a bounded background
  transfer service. Existing session permits transport identities, not file bytes.
- Write resources to temporary locations and expose only complete immutable files.
  Perform transfer verification off the control/render path for these new assets.
- Gather readiness for the whole required cohort and candidate revision, including
  capacity and GPU residency requirements. Schedule the scene and asset switch
  together only after complete readiness.
- Bound simultaneous old/new residency. If capacity cannot hold a replacement,
  report it before publication rather than evicting active Program resources.
- Preserve existing missed-frame and hold policy. Define cancellation, superseding
  a candidate, restart recovery, and rollback to a previously available revision.
- Offer deliberate promotion first. Any later continuous Program-follow mode must
  use the same complete-revision gate and make its behavior explicit to the operator.

## Acceptance

On two nodes, delay one texture transfer, disconnect one node, and force a capacity
refusal. No node reveals a partial/new scene ahead of the cohort. Verify publication
frame and revision evidence on both nodes. A Blender disconnect during a take has
no effect on playback of the already published scene.
