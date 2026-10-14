# Step 12 — Qualification and delivery checks

This reproduces the explicit publication qualification harness. For the current
live project workflow, use the [add-on README](../integrations/blender/README.md)
and [operator guide](operator-guide.md). The recorded figures below describe
the historical delivery, rather than a fresh qualification of this checkout.

Qualified with Blender **5.2.0 LTS, fbe6228777e7**, add-on **0.7.0**, link
protocol **v1 with capability negotiation**, and the Advanced D3D12 renderer.
The support boundary, declared limits and measured figures are in
[qualification v1](qualification-v1.md); the procedure an operator follows is the
[operator guide](operator-guide.md).

## Build the delivery bundle and open it

Build the Debug and Release GUI presets, then run:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/run-blender-qualification.ps1 -Build release -Gpu
```

One command packages the add-on, runs the qualification harness, writes the
sample Blender scene and the two delivered projects, prints the bundle, and
starts a Director editor on the delivered Preview project with isolated local
settings. `-NoLaunch` builds the bundle only; `-NoBlender` skips the sample
scene. Use
`cmake --build --preset windows-msvc-debug-gui` and `cmake --build --preset
windows-msvc-release-gui` from a Visual Studio developer shell.

The bundle it prints is the deliverable:

```text
mediaserver_livelink.zip                 installable add-on
mediaserver_livelink.manifest.json       version, protocol, capabilities, file hashes
sample-collection.blend                  sample collection ready to link
evidence/preview.mstage                  offline Preview with accepted linked content
evidence/published.mstage                the same content published to Program
evidence/qualification.json              the measured report for this machine
```

1. In the editor, **File > Open Project** and choose `evidence/preview.mstage`.
   Click **PREVIEW**. The **Blender Preview** scene contains the two accepted
   occurrences the harness published: **Renamed hero** (held placement, X 2) and
   **Duplicate**. No Blender process is involved.
2. Open `evidence/published.mstage` in the same way and click **PROGRAM**. The
   frozen revision renders from its own immutable packages; the **Scene** tree
   shows independent drawables, not linked records. This is the reopened
   published project, so it proves a take runs with Blender absent.

## Scene Link card: capabilities and measured link

3. Back in the Preview project, choose **File > Start Scene Link**, then
   **File > Scene Link Details...**. Above the publication controls the card now
   shows **NEGOTIATED CAPABILITIES AND MEASURED LINK**. With nothing paired it
   reads "No paired publisher yet."
4. Pair Blender (below). The same card then shows the publisher build
   (`mediaserver_livelink/0.7.0 blender/5.2.0`), the negotiated capability list,
   what this Media Server build serves, accepted/refused/resync counts, the
   event-to-Preview p50/p95, the last revision's transfer/ingest/Director split,
   and peak pending bytes with retained resources and receipts.
5. **Copy link evidence** copies those figures to the clipboard. It deliberately
   excludes the pairing code, unlike **Copy details**, which is what Blender
   needs. Paste it into a report; the same numbers are in
   `evidence/qualification.json`.

To populate the card without opening Blender, publish through the step 04
reference client, which declares only the `base` capability:

```powershell
python scripts/scene-link-client.py <manifest.mslink> --port <port> --pairing-code <code>
```

Its Welcome line prints this build's advertised capabilities, and the card then
reads `Negotiated: base` beside the fuller `This build serves:` list - the
mismatch case, shown without needing a second build.

## Blender: install, negotiate, refuse, remove

```powershell
& 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe' `
  --factory-startup --python scripts/blender-livelink-demo.py
```

Or install `mediaserver_livelink.zip` from the bundle through **Edit >
Preferences > Add-ons > Install from Disk** for the real install path.

6. Open the 3D View sidebar with **N**, select **Media Server**, and scroll to
   **Compatibility and evidence**. Before connecting it names the add-on version,
   the link protocol and the Blender build. Paste the Director details, click
   **Connect & Publish**, and it adds **Director serves:** with the optional
   capabilities and, once a revision is accepted, **Event to Preview p50/p95**
   with the Blender and Director split of the last revision.
7. To see a refusal without another build, click **Stop Scene Link** in Media
   Server: the publisher reports the lost pairing and keeps the accepted revision.
   A genuine mismatch — an add-on whose feature this Media Server does not serve —
   is refused by name before any bake or upload, shown with an error icon under
   **Not served**, and does not disconnect the pairing. The offline
   `tests/blender/test_publisher_core.py` checks assert that exact wording, and
   the Director-side refusals are in `mediaserver_director_link_service_tests`.
8. Install the archive a second time over the enabled add-on: it upgrades in
   place, keeping the collection selection and source identity. **Disable** then
   **Remove** takes it away completely — no handlers, timer, scene properties or
   files remain, and Media Server keeps every accepted revision.
9. The installed folder contains `manifest.json`. Compare it against
   `mediaserver_livelink.manifest.json` from the bundle to prove which build is
   on a machine; every file carries a SHA-256.

## Reproducible qualification

```powershell
ctest --preset windows-msvc-debug-gui --output-on-failure -R "linked_qualification|director_link|linked_scene_ingest|linked_publication|blender_publisher_unit|scene_linked|linked_animation_stage|linked_look|linked_environment|editor"
ctest --preset windows-msvc-release-gui --output-on-failure -R "linked_qualification|director_link|linked_scene_ingest|linked_publication|blender_publisher_unit|scene_linked|linked_animation_stage|linked_look|linked_environment|editor"
python scripts/test-blender-livelink.py --build debug --qualification --output out/step12-blender-debug
python scripts/test-blender-livelink.py --build release --qualification --output out/step12-blender-release
```

That filter is **32/32 targeted CTests in each configuration**: the new
qualification harness (CPU and `--gpu`), the Director protocol, linked scene
ingestion, publication, the step 09-11 linked fixtures with their GPU variants,
the Blender publisher unit tests, and all sixteen editor panel tests. The full
suite passes as well, 180/180 in each configuration.

`mediaserver_linked_qualification_tests` drives the real link service, ingest
planner, command scheduler and publication path through **17 scenarios** (18 with
`--gpu`): first publication, rename, duplicate, reparent, origin change, delete,
undo, resync after undo, interrupted transfer, reconnect, malformed input, a
24-revision session with repeated resource replacement, cancellation, Director
restart, save and reopen, resource capacity, two-node publication with controlled
faults, and Advanced D3D12 rendering of the published revision. It writes
`qualification.json` with the counters and percentiles.

The installed add-on adds **20 recorded checks** against the real Director
fixture: verified install metadata, capability negotiation, reparent, origin
change, three texture replacements, a 30-edit session in batches, a refused
unrepresentable name, explicit recovery, disconnect during an edit,
save/reopen/reconnect, in-place upgrade and complete removal. Its `results.json`
also records the measured event-to-Preview percentiles and the Blender/Director
split of the last revision.

Each integration output directory must be new, and keep its path short:
immutable package names have to stay inside the legacy Windows path limit.

This is local process, resource and GPU qualification on one machine. It does not
claim physical two-machine scanout, genlock, tracker-hardware calibration or lens
agreement between Blender's reference viewport and calibrated output.

The historical Director and installed-Blender qualification reports are retained
in `evidence/step12-*` and linked from [qualification v1](qualification-v1.md).
Build logs, screenshots and the original package manifest are recoverable in Git
history. Run the checks above to gather evidence for the current delivery.
