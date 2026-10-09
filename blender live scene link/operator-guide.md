# Operator guide — Blender live scene link

For the person standing at the machine. Install, link, adjust, publish,
disconnect, rebake, recover. The measured limits and the support boundary are in
[qualification v1](qualification-v1.md); this page is the procedure.

## Install the add-on

1. Build the archive: `python scripts/package-blender-livelink.py`. It writes
   `out/blender-livelink/mediaserver_livelink-0.7.0.zip`.
2. In **Blender 5.2.0 LTS**: **Edit > Preferences > Add-ons**, the menu's
   **Install from Disk**, select the archive, enable **Media Server LiveLink**.
3. Open the 3D View sidebar with **N** and choose the **Media Server** tab. The
   **Compatibility and evidence** box names the add-on version, the link protocol
   and the Blender build you are on.

**Upgrade:** install the newer archive over the enabled add-on and restart
Blender. Projects, collection selections and source identities are unaffected;
the pairing is not, so copy fresh Director details afterwards.

**Remove:** **Disable**, then **Remove** in Preferences > Add-ons. Nothing is
left behind: no handlers, no timer, no scene properties, no saved credentials.
Blend files keep their harmless `ms_link_*` identity keys, and Media Server keeps
every accepted revision — removal stops publishing, it does not unpublish.

**Verify an installed copy:** the installed folder contains `manifest.json` with
the version, protocol, capability list and a SHA-256 for every file. Compare it
against the archive's `--report` output when you need to prove which build is on
a machine.

## Link a collection

1. In Media Server, click **PREVIEW**, then **File > Start Scene Link**, then
   **File > Scene Link Details...**. The card shows the loopback endpoint and a
   one-time pairing code. **Copy details**.
2. In Blender, select the collection in the sidebar, click **Paste Director
   Details**, then **Connect & Publish**.
3. The Scene Link card then shows the publisher build, the negotiated
   capabilities, and the measured event-to-Preview times. **Copy link evidence**
   copies those figures without the pairing code.

The pairing is loopback-only, ephemeral, and never saved in a project or sent to
output nodes. **Stop Scene Link** revokes it immediately.

## Adjust content on the stage

- **Live updates** coalesces Blender dependency changes. Clear it to pause and
  use **Publish Now** for one explicit update.
- **Pause source updates** (Media Server, linked collection Inspector) blocks new
  source geometry while keeping the pairing and shot editing alive. Resume, then
  **Publish Now** in Blender to submit the retained edit.
- Placement policy per occurrence: **Follow Source** tracks Blender,
  **Hold** freezes the current world placement through source changes,
  **Reset Placement** discards the shot offset. Visibility has the same
  three-way choice. Placement and visibility are shot state: they survive source
  geometry revisions, deletion and restoration, save/reopen and restart.
- From Blender, **Begin Shot Gesture**, transform the tagged wrapper and
  **Commit Shot**. The claim lasts 15 seconds and a concurrent Director edit
  wins, with an explicit **Accept Director / Retry My Shot** choice.

## Publish to Program

1. In **Scene Link Details**, set **shared storage** to a folder visible at the
   same absolute path on every node, then **Set folder**.
2. **Publish Preview**. The revision is staged and frozen; Program keeps showing
   its current cut. The card reports staging, node verification and GPU
   residency.
3. All required nodes reveal the new scene on one exact show frame. A node that
   is missing bytes holds; it never shows a partial revision.
4. **Roll back Program** returns to the retained previous revision.
   **Cancel publication** abandons a staging candidate.

Blender may keep editing Preview throughout. Program changes only on an explicit
publication.

## Disconnect and run the show

**Disconnect** in Blender, or **Stop Scene Link** in Media Server. Playback
continues from the accepted immutable packages: a take runs with Blender closed,
the machine gone, or the project reopened on another machine with the same
absolute asset paths. Saved projects reopen offline and evaluate identically.

## Rebake derived lighting

Baked GI and reflections are tied to a geometry/material/light revision. After a
Blender revision changes them, the linked scene reports stale lighting and
publication is refused until you either **rebake** in the Environment controls or
disable derived lighting. Animated assemblies stay outside static baking.

## Recover

| Symptom | Action |
| --- | --- |
| `pairing_required` | The code is stale. Copy fresh details and Connect & Publish. |
| `Resync required` / stale revision | Click **Resync**: it reconnects, registers the Director's current revision and sends a fresh snapshot. |
| `source_paused` | Resume source updates in the Inspector, then **Publish Now**. |
| A capability is not served | The add-on and Media Server are from different deliveries. Clear the named feature to keep working, or install the matching build. |
| `resource_capacity` / `receipt_capacity` | The pairing is full. **Stop Scene Link**, start it again, reconnect and publish a fresh snapshot. |
| `resource_cache_full` | The shared store `out/scene-link-resources/shared` holds 1.5 GiB of distinct linked content; re-linking reuses it, so this means new content, not repeated pairings. Move the store aside only once no saved project uses it (or re-publish those scenes from Blender); the next link starts an empty store. Older per-pairing directories beside it are not counted: retire the ones no project references to free disk. |
| Publisher process lost | The add-on stops and says so. **Resync** to reconnect; there is no silent retry loop. |
| Blender refuses a name or mesh | The diagnostic names what Media Server cannot represent. Fix the source; nothing partial was admitted. |
| Editor reopened, Blender gone | Nothing to do. Accepted revisions and shot state are in the project. |

## Delivered sample

`powershell -ExecutionPolicy Bypass -File scripts/run-blender-qualification.ps1
-Build release` writes a bundle you can hand to an operator:

| File | What it is |
| --- | --- |
| `mediaserver_livelink.zip` | The installable add-on |
| `mediaserver_livelink.manifest.json` | Version, protocol, capabilities, file hashes |
| `sample-collection.blend` | A sample Blender collection ready to link |
| `evidence/preview.mstage` | A saved Preview project with accepted linked content |
| `evidence/published.mstage` | The same content published to Program, verifiable offline |
| `evidence/qualification.json` | The measured report for that machine |

Both projects reference their immutable packages by absolute path, inside the
bundle directory, so keep the bundle together and at the same path on every
machine that opens them — the same rule as every other asset in a project.

It then starts a Director editor on the delivered Preview project with isolated
local settings. `-Gpu` also renders the published revision through the Advanced
D3D12 renderer; `-NoBlender` skips the sample scene; `-NoLaunch` only builds the
bundle.
