# Blender scene-link reference

Start with the [add-on README](../integrations/blender/README.md) for the current
one-project workflow, automatic discovery, live scene edits and writeback.
The [operator guide](operator-guide.md) covers pairing, offline playback and
troubleshooting. The [protocol](protocol-v1.md) describes current transport
and capabilities.

The native contracts below describe the linked representations used by the
renderer and qualification harness. Older collection/shot-layer and explicit
publication workflows were superseded by the live project workflow; consult
the add-on README and protocol before treating those procedures as current UI.

| Reference | Purpose |
| --- | --- |
| [Contract](contract-v1.md) and [baseline](baseline-v1.md) | Coordinates, reference scene and recorded small-fixture measurements |
| [Identity and overrides](identity-and-overrides-v1.md) | Stable object identities and native shot representation |
| [Scene manifest](scene-manifest-v1.md) | Manifest records, immutable resource revisions and ingestion |
| [Publisher](publisher-v1.md) | Blender collection export and resource ownership |
| [Shot placement](shot-placement-v1.md) | Affine placement and feedback semantics |
| [Preview workflow](preview-workflow-v1.md) | Native linked-scene editing model |
| [Program publication](program-publication-v1.md) | Explicit immutable publication API and qualification path |
| [Environment scale](environment-scale-v1.md) | Scatter, terrain, LOD and admission |
| [Materials and lighting](materials-and-lighting-v1.md) | Native material/light representations and derived-lighting fences |
| [Animation and stage](animation-and-stage-v1.md) | Immutable animation assemblies and stage/camera context |
| [Qualification](qualification-v1.md) | Historical delivery measurements, declared limits and hardware boundary |
| [Delivery checks](ui-checks-step-12.md) | Reproduce the explicit qualification bundle |
| [Optional USD adapter](13-optional-usd-adapter.md) | Future decision gate and acceptance requirements |

Pinned source fixtures live in `tests/fixtures/blender-scene-link`; supported
generators, launchers and checks remain in `scripts/`. Retained `evidence/`
contains only the raw baseline and delivery reports cited by these references.
Completed step plans, obsolete UI walkthroughs and review screenshots are
recoverable from Git history.
