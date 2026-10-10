# Operator guide — Blender live scene link

For the person standing at the machine. Install, link, adjust, take to air,
disconnect, rebake, recover. The measured limits and the support boundary are in
[qualification v1](qualification-v1.md); this page is the procedure.

## Open a scene in Blender

No installation is needed for this: Media Server runs the add-on that ships beside it.

1. Save the project, so its Blender file and linked content land beside it.
2. In **PROJECT SCENES**, point at the scene and click the orange **B** that appears
   before its name (or right-click > **Open in Blender**). The first time, Media Server
   creates `<project> Blender/<project>.blend` and starts Blender on it; the file's
   first scene becomes this scene.
3. The **B** stays before the scene's name: filled while Blender syncs the scene,
   outlined while Blender shows another one or is closed. Its tooltip says which.
   Blender's 3D view header names the scene it syncs.
4. The **B** on another scene switches the same Blender to that scene, creating it in
   the project's file the first time. Every scene in the file stays linked; Blender
   syncs the one it shows, so switching scenes in Blender switches which one follows.

A scene the artist makes in Blender shows **Add to Media Server** in Blender's Media
Server sidebar; Media Server then creates a scene of that name, in Preview, linked to it.
**File > Connect to Blender...** links a Blender you opened yourself to the Preview
scene. Every link stays loopback-only and same-user; no credential is saved in a
project or sent to output nodes.

**Install permanently:** **Install Add-on Permanently** in Blender's Media Server
sidebar, or **Edit > Preferences > Add-ons > Install from Disk** with the archive from
`python scripts/package-blender-livelink.py`. Once installed, opening the project's
`.blend` by hand relinks every scene in it.

**Remove:** **Disable**, then **Remove** in Preferences > Add-ons. Nothing is left
behind: no handlers, no timer, no scene properties, no saved credentials. Blend files
keep their harmless `ms_link_*` / `ms_scene_source` identity keys, and Media Server
keeps every accepted revision.

**Verify an installed copy:** the installed folder contains `manifest.json` with the
version, protocol, capability list and a SHA-256 for every file. Compare it against
the archive's `--report` output when you need to prove which build is on a machine.

## Adjust content on the stage

A linked scene is live wherever it is: Blender edits reach Preview, Program and every
output node on the next control frame. Drag an object in Blender and the wall follows
while you drag; nothing needs publishing. Mesh and texture edits load on every node
first and are revealed on the same frame.

Edits in Media Server to a Blender object - placement, Hold, visibility, light settings
- are written into the object in Blender, so the `.blend` matches the wall and can be
reopened or rendered offline. They stay Media Server overrides underneath, so Undo in
Media Server reverts them (and Blender follows). The latest edit wins: when the artist
changes the same property in Blender, the artist's value replaces the override.

- **Live updates** (Blender, Advanced) can be cleared to pause; **Publish Now** sends
  one explicit update.
- **Pause source updates** (Media Server, linked collection Inspector) blocks new
  source content while keeping the link. Resuming sends the retained edit.
- **Follow Source / Hold / Reset placement to source** and **Hide / Show** work as
  before in the Inspector; Blender shows each result.
- **Delete** in Media Server deletes the object, with its children, in Blender too.
  Deleting it in Blender removes it from Media Server. Undo on either side brings it
  back with its Media Server edits, until the `.blend` is saved and reopened.
- Kept in Media Server only, and counted in Blender's sidebar: material overrides,
  parts of a multi-material mesh moved apart, and objects driven by constraints or
  animation.

## Take it to air

Use the switcher as for any scene: take the linked scene into Preview, then **SWAP** (or
CUT). Blender keeps editing it on air. Renderer settings (for example Advanced) apply
live to whichever scene they are set on.

For remote render nodes the linked content in `<project> Blender/Link Resources` must
be visible at the same absolute path on every node: keep the project on such a share. A
node missing a file reports it in Output Monitor and keeps its last complete picture.

**File > Publish Preview...** still freezes Preview into an immutable revision on shared
storage with **Roll back Program**, for a Program that must not change while Blender
edits; it is not needed to go on air.

## Disconnect and run the show

**Disconnect** in Blender, or right-click the scene > **Disconnect Blender** in Media Server. Playback
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
| `pairing_required` | The link was restarted. Click the scene's **B** again. |
| `Resync required` / stale revision | Click **Resync**: it reconnects, registers the Director's current revision and sends a fresh snapshot. |
| `source_paused` | Resume source updates in the Inspector, then **Publish Now**. |
| A capability is not served | The add-on and Media Server are from different deliveries. Clear the named feature to keep working, or install the matching build. |
| `resource_capacity` / `receipt_capacity` | The pairing is full. **File > Connect to Blender... > Resync Scene** starts a fresh session and snapshot. |
| `resource_cache_full` | The project's store `<project> Blender/Link Resources/shared` holds 64 GiB of distinct linked content; re-linking reuses it, so this means new content, not repeated pairings. Move the store aside only once no saved project uses it (or re-publish those scenes from Blender); the next link starts an empty store. Older per-pairing directories beside it are not counted: retire the ones no project references to free disk. |
| Publisher process lost | The add-on stops and says so. **Resync** to reconnect; there is no silent retry loop. |
| Blender refuses a name or mesh | The diagnostic names what Media Server cannot represent. Fix the source; nothing partial was admitted. |
| Editor reopened, Blender gone | Nothing to do. Accepted revisions and overrides are in the project; reopening the `.blend` relinks it. |
| `director_transaction_failed` | Media Server refused to schedule the revision. Click **Resync Scene**; if it persists, report the build. |

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
