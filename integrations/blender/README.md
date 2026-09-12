# Media Server LiveLink for Blender

Installable collection and animation publisher for **Blender 5.2.0 LTS, Windows build
fbe6228777e7**. Add-on **0.7.0**, authoring link protocol **v1** with capability
negotiation. Implements roadmap steps 05–07 and 09–12. Blender remains the authoring
application; the paired local Director admits complete revisions into Media Server Preview.

Build the archive with `python scripts/package-blender-livelink.py`. In Blender,
open **Edit > Preferences > Add-ons**, use the menu's **Install from Disk**, select
`out/blender-livelink/mediaserver_livelink-0.7.0.zip`, and enable **Media Server
LiveLink**. Open the 3D View sidebar with **N**, then select **Media Server**.

The archive and the installed folder carry `manifest.json`: add-on version, link
protocol, negotiated capabilities, the pinned Blender build and a SHA-256 for
every file. Use it to check which build is installed on a machine; pass
`--report` to the packaging script for the same document beside the archive.

**Upgrade** by installing the newer archive over the enabled add-on, then
restarting Blender; copy fresh Director details afterwards, because the pairing
does not survive. **Remove** with **Disable** then **Remove** in Preferences:
no handlers, timer, scene properties or files remain, and Media Server keeps
every accepted revision. **Recovery** is always explicit — **Resync** reconnects,
registers the Director's current revision and sends a fresh snapshot.

**Compatibility and evidence** in the sidebar names the add-on version, the link
protocol and the Blender build; once paired it adds the capabilities the Director
serves, any it does not, and the measured event-to-Preview p50/p95 with the
Blender and Director split of the last revision. A feature this Media Server
build does not serve is refused by name before any bake, extraction or upload,
and the pairing keeps working without it. See the
[support boundary](../../blender%20live%20scene%20link/qualification-v1.md) and the
[operator guide](../../blender%20live%20scene%20link/operator-guide.md).

In Media Server, select **PREVIEW**, then **File > Start Scene Link** and **File >
Scene Link Details... > Copy details**. In Blender select the collection, click
**Paste Director Details**, then **Connect & Publish**. The panel reports the
accepted revision and extraction/GLB counts. The pairing code stays in Python
memory; it is cleared on file load and add-on disable, and is never a blend-file
property. A saved collection remembers its selection, IDs and source metadata.

**Live updates** coalesces dependency changes. Clear it to pause and use **Publish
Now** for an explicit update. A submitted transaction can finish after pausing.
**Resync** reconnects, registers the Director's current revision, and sends a fresh
snapshot. A network failure stops publishing and requires this explicit recovery;
there is no stale-state retry loop. If the Director pairing has restarted, copy
its new details before connecting. The last accepted scene stays available offline.

Media Server's linked collection Inspector also has **Pause source updates**.
This saved Director setting blocks new source geometry while keeping the pairing
and shot editing active. After **Resume source updates**, click **Publish Now** in
Blender to submit the retained edits. A paused response does not disconnect the
add-on. The Scene tree shows the Blender collection name and source hierarchy;
connection, override, copy, duplicate and unlink controls are available beside it.
See the [Preview workflow walkthrough](../../blender%20live%20scene%20link/ui-checks-step-07.md)
for offline reopening and independent versus shared copies.

**Foliage and terrain** publishes evaluated mesh-object Geometry Nodes instances
as shared scatter groups. Set **Export density (%)** and its sampling seed without
changing the viewport candidate count. On a selected mesh, set **Terrain chunk
size (m)** above zero for spatial chunks; choose an **Authored low detail mesh**
for one explicit LOD. Scatter edits apply to groups. Use the real emitter/terrain
parent for two-way shot gestures. The full scene is limited to 4096 instances and
64 geometry resources. See the [environment walkthrough](../../blender%20live%20scene%20link/ui-checks-step-09.md)
for supported output, terrain boundary rules, budgets and reproducible examples.

Edit and Sculpt modes display pending status. **Return to Object Mode** to publish
their completed evaluated result. Ordinary placement drags send sparse object
records and perform zero additional mesh extraction, serialization or upload.
The Director still validates the retained resources on its own worker.

**Materials and lighting** supports a defined Principled subset with saved PNG/JPEG
images on the active render UV map. Material identity, not slot order, binds the
Director's per-property overrides. Choose **Alpha handling** explicitly; procedural
base color on one-material meshes can use **Bake Base Color Copy**. The worker
assigns a supported copy and retains the source material. Save its generated PNG
with the project. **Light efficacy (lm/W)** and **Light cutoff (m)** define the
conversion of Sun, Point, Spot and square/rectangle Area lights to native photometry.
See the [look walkthrough](../../blender%20live%20scene%20link/ui-checks-step-10.md)
and [translation contract](../../blender%20live%20scene%20link/materials-and-lighting-v1.md)
for exact graph patterns, light restrictions and stale GI/reflection publication.

Accepted Director placement and visibility return through tagged shot wrappers.
**Animated assembly** explicitly bakes the collection's playback range to one
node/skin clip using **Publish Now**. Its start frame and loop settings map to
rational Media Server show time, independent of Blender playback. Select the
managed **Animation assembly placement** empty to make returned shot adjustments.
**Stage references** adds a managed stage origin and reference camera; **View
Director Camera** uses its sensor gate in the local viewport. See the
[animation/stage walkthrough](../../blender%20live%20scene%20link/ui-checks-step-11.md)
and [supported cache contract](../../blender%20live%20scene%20link/animation-and-stage-v1.md).

Use **Show Source Placement** to inspect/edit source parenting; use **Begin Shot
Gesture**, transform the selected wrapper and **Commit Shot** to change the shot.
The claim lasts 15 seconds. Hold/Follow/Reset, visibility overrides and **Undo My
Last Shot Edit** are available in the same sidebar. A concurrent Director edit
produces an explicit **Accept Director / Retry My Shot** choice.
See the [two-way placement walkthrough](../../blender%20live%20scene%20link/ui-checks-step-06.md)
and [shot contract](../../blender%20live%20scene%20link/shot-placement-v1.md) for
the qualified hierarchy, undo behavior and unsupported reverse-placement cases.

See the [UI walkthrough](../../blender%20live%20scene%20link/ui-checks-step-05.md)
and [publisher design](../../blender%20live%20scene%20link/publisher-v1.md) for the
supported subset, scheduling, resource limits and recorded checks.

For a disposable UI scene without installing or saving preferences:

```powershell
& 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe' `
  --factory-startup --python scripts/blender-livelink-demo.py
```

Tests, from the repository root after the corresponding GUI builds:

```powershell
python tests/blender/test_publisher_core.py
python scripts/test-blender-livelink.py --build debug
python scripts/test-blender-livelink.py --build release
python scripts/test-blender-livelink.py --build debug --shot
python scripts/test-blender-livelink.py --build release --shot
python tests/blender/test_environment_scale.py
python scripts/test-blender-livelink.py --build debug --environment
python scripts/test-blender-livelink.py --build release --environment
python tests/blender/test_materials_lighting.py
python scripts/test-blender-livelink.py --build debug --materials
python scripts/test-blender-livelink.py --build release --materials
python scripts/test-blender-livelink.py --build debug --qualification
python scripts/test-blender-livelink.py --build release --qualification
```

The integration runner builds the ZIP, installs it through Blender's add-on
installer into an isolated scripts directory, then tests the installed add-on
against the actual C++ Director service, importer and command scheduler.
It starts factory Blender with `--python-exit-code 1`; failures propagate to the
runner. It revokes its temporary pairing and retains non-secret result files in
`out/blender-livelink/<configuration>-<run>/`.
