# Blender scene-link reference

Start with the [add-on README](../integrations/blender/README.md) for the
one-project workflow, automatic discovery, live scene edits and writeback. The
[operator guide](operator-guide.md) covers linking, offline playback and
troubleshooting. The [protocol](protocol-v1.md) describes transport and
capabilities.

The documents below describe the native representations the renderer and the
qualification harness use.

| Reference | Purpose |
| --- | --- |
| [Contract](contract-v1.md) | Coordinates, identities, placement policies and the reference scene |
| [Reference fixture measurements](reference-fixture-measurements.md) | Export, ingest, upload and adoption timings on the small fixtures |
| [Protocol](protocol-v1.md) | Director authoring link: framing, capabilities, resources, writeback, discovery |
| [Identity and overrides](identity-and-overrides-v1.md) | Stable object identities and native shot representation |
| [Scene manifest](scene-manifest-v1.md) | Manifest records, immutable resource revisions and ingestion |
| [Publisher](publisher-v1.md) | Blender-side evaluation, extraction and resource ownership |
| [Editing linked collections](linked-collections-editing.md) | Inspector controls, copies, duplicates and unlink |
| [Program publication](program-publication-v1.md) | Frozen, immutable Program revisions |
| [Environment scale](environment-scale-v1.md) | Scatter, terrain, LOD and admission |
| [Materials and lighting](materials-and-lighting-v1.md) | Native material/light representations and derived-lighting fences |
| [Animation and stage](animation-and-stage-v1.md) | Immutable animation assemblies and stage/camera context |
| [Qualification](qualification-v1.md) | Supported builds, declared limits, measured behavior and the delivery bundle |

Pinned source fixtures live in `tests/fixtures/blender-scene-link`; supported
generators, launchers and checks are in `scripts/`. The `evidence/` folder holds
the raw fixture timing CSVs and the Director and add-on qualification reports
cited by these references.
