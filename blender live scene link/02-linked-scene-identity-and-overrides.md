# 02 — Linked scene identity and overrides

Status: proposed. Depends on: 01.

## Outcome

A persisted linked-scene model that preserves shot decisions across source updates.

## Work

- Add a source-scene ID, link-instance ID, stable source-object ID, prototype ID,
  and material ID. Map each linked occurrence to a persistent document identity.
  Multiple links to the same Blender scene must have independent shot adjustments.
- Store source hierarchy and local transforms separately from stage alignment and
  per-occurrence shot overrides. Keep asset identity separate from content revision.
- Implement **Follow source with offset** and **Hold shot placement**. Specify
  local versus world-space behavior and exact matrix order in the schema contract.
  Holding world placement must compensate for upstream parent changes.
- Define behavior for reparenting, origin changes, singular transforms, animated
  parents, and deletion. Reject unsupported transform cases rather than drift.
- Store overrides by property; a geometry update must not replace placement,
  visibility, or a locally overridden material factor.
- Define source removal and local hiding separately. Preserve recoverable override
  records for deleted source objects and expose deliberate resolution when needed.
- Specify duplicate-ID repair in Blender, library-linked/read-only identity, and
  undo restoration. Names, memory addresses, and array positions are not identities.
- Extend schema validation, persistence, scene evaluation, and digests together.

## Acceptance

Rename, reorder, save/reopen, duplicate, delete/restore, and reparent the reference
objects. IDs and intended overrides survive. A changed source mesh updates every
linked occurrence while keeping their different placements. Invalid hierarchies
and singular hold-placement conversions leave the last valid revision intact.
