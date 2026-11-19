# Coordinated Program publication v1

A frozen scene is published through the asset cohort and sealed control-frame
authority. Linked scenes go on air live, one Blender scene per Pixelsoup
scene; freezing is only for a Program that must not change while Blender keeps
editing. The operator freezes Preview with **File > Freeze Blender...**.
Publication does not add a second clock, a Blender-to-node connection, or a new
file-transfer protocol to the session transport.

## Revision and storage

Configure an absolute Program storage folder in the Freeze Blender dialog. The Director
needs write access and every required output process needs read access at the
same absolute path. A shared UNC path is suitable for separate Windows machines;
a local path works for the same-machine demonstration. Use one publishing
Director per storage folder. Storage provisioning remains an operator task.

The publication worker captures the accepted document version, evaluated linked
objects and shot overrides, the scene's camera, lights, clips, compositor, and
renderer/effect settings. Linked instances become independent native drawables.
Settings become scene-owned `publication.settings` records. The resulting scene
is read-only; editing Preview or refreshing its asset registry cannot change the
published resource generation. When the previous Program has never been
published, the worker captures it as an independent rollback revision too.

All authored file references in the snapshot are staged under
`<storage>/ms-publications-v1`. These include model packages, media source files,
environment maps, authored baked-lighting/IES files, LUTs and texture properties.
Ingested `.msmodel` packages contain the supported geometry, materials, texture
mips and package animation data. Arbitrary external model formats and image
sequence patterns are refused: ingest the model or package the sequence first.
Freezing a media source removes its mutable Library/proxy identity.

Active baked GI/reflections are qualified against their dependency revision
before any path is frozen. Stale or untagged derived lighting refuses the candidate
and retains Program. Rebake, or explicitly disable GI and every reflection capture,
before publishing. Probe v3 stores the input hash and travels in the same immutable
resource cohort as the models. See [Materials and lighting v1](materials-and-lighting-v1.md).

Each file is fingerprinted, copied to a temporary file, checked against the
captured SHA-256, and renamed into a content-addressed path. A source changing
during the copy fails the candidate. An existing destination must contain the
same bytes. Only after resources are complete is the manifest exposed.

The manifest is UTF-8 text with this versioned grammar:

```text
MSPUBLICATION 1 "<revision SHA-256>" <captured document version>
"<role>" "<resource SHA-256>" <byte length> "<absolute staged path>"
...
# Source revisions
# <source UUID>:<accepted source revision>
```

Quoted strings use the C++ `std::quoted` escaping convention. The revision hashes
the canonical snapshot, source revisions, document version and resource rows.
The scene stage records `publication_revision`, `publication_document_revision`,
`publication_manifest`, and `publication_manifest_hash`. Resource rows with role
`retained` reserve the immediately previous Program; older retained rows do not
accumulate recursively. The replicated document binds the frozen scene and the
manifest identity through the ordinary command digest.

## Readiness and reveal

Every output process, including the Director's local renderer, verifies the
manifest hash, revision, file sizes and resource hashes on a background worker.
Verification is scoped to the scene, manifest path, expected hash and revision.
A failed verification remains failed until a fresh candidate or restart.

The renderer prepares the candidate while its existing publication gate retains
the last complete Program image. Model/texture readiness, actual renderer capacity
and required renderer capabilities remain part of the existing readiness report.
Advanced texture-streamer requests retain active/rollback model textures while
preparing their replacement. This version can hold the previous image during
preparation; it does not promise simultaneous animated playback of two scenes.

Every configured render-server node participates, including nodes with no current
heartbeat. A missing, incompatible or unready member prevents authorization.
When the whole cohort agrees, the Director authors an ordinary asset permit
30 control frames ahead, with its derived show frame. At that exact control
frame it atomically selects the frozen Program scene and records the rollback
reference. Preview remains selected for authoring. Intervening Preview edits are
preserved because the scene-switch transaction is built at the activation frame.

Nodes use the existing `AssetPublicationGate`. Missing an exact permit frame
holds the previous image and uses the existing retry policy. Required physical
synchronization still has to satisfy the normal sync gate. A software frame
agreement does not establish hardware scanout synchronization.

## Capacity and lifecycle

The fixed publication admission envelope is:

| Limit | Value |
| --- | --- |
| New scene nodes | 4,096 |
| New resource files | 128 |
| Manifest rows, including retained resources | 256 |
| Unique old/new resource bytes | 512 MiB |
| Old/new decoded package texture bytes, including mips | 256 MiB |
| Old/new package geometry | 1,048,576 vertices / 3,145,728 indices |
| Manifest size | 1 MiB |
| Stored published scenes in a project | 16 |
| Storage admission watermark | 2 GiB / 2,048 files |

These are conservative preflight limits, not a hardware performance claim.
Actual GPU integration and the authored texture budget can refuse a candidate
below these limits. Immutable model geometry keeps the renderer's existing
bounded cache behavior; reaching capacity requires retiring/restarting the
renderer through the ordinary show workflow. Admission failure retains Program.

- **Cancel:** before a permit is authorized, cancel the worker, fence stale
  completion, and discard the unused candidate and its first-take rollback clone.
  Already exposed files remain available and count toward storage admission.
- **Supersede:** Freeze Blender again before authorization. A single worker and
  one latest request prevent an unbounded queue. Completion carries the captured
  project and Program identity; a changed project/Program discards it.
- **Authorized cut:** cancellation and superseding are disabled during the
  scheduled lead. The existing authority completes or holds that cut.
- **Rollback:** Roll back Program selects the retained immutable scene, removes
  its old permit, and verifies/prepares it through the same cohort gate. Rolling
  back again can return to the revision just left. Active, pending and immediate
  rollback scenes are protected from deletion; older inactive scenes may be
  removed through the normal project-scene UI.
- **Restart:** project save/reopen strips runtime permits and the pending
  candidate pointer. It retains accepted scenes, manifests and rollback identity.
  Resource verification and GPU readiness must be established again. Session
  recovery likewise creates fresh local verification state; it never restores
  an in-memory readiness result. Reopening requires the shared files, not Blender.
- **Disconnect:** Blender has no role in drawing the accepted snapshot. A required
  output node disconnect follows the existing frame-barrier hold policy. If that
  barrier has stopped control-frame advancement, queued UI changes wait with it.

See the [operator guide](operator-guide.md) for the procedure, and
[Environment scale](environment-scale-v1.md),
[Materials and lighting](materials-and-lighting-v1.md) and
[Animation and stage](animation-and-stage-v1.md) for the content a revision can
carry.
