# Pixelsoup Live Link for Blender

Add-on **0.10.1**, Blender **5.2 or newer**, authoring link protocol **v1**.
Tested with Blender 5.2.0 LTS (fbe6228777e7) on Windows.

## One project, one Blender file

A Pixelsoup project has one `.blend`: `<project> Blender/<project>.blend` beside the
saved project (`Documents/Pixelsoup/Blender` while the project is unsaved). Each
Pixelsoup scene is a Blender scene in that file, tagged with the scene's identity.

1. In **PROJECT SCENES**, point at a scene and click the orange **B** that appears
   before its name (or right-click the row > **Open in Blender**).
2. The first time, Pixelsoup creates the file, starts Blender with the LiveLink
   add-on running, and turns the file's first scene into this scene. The **B** on
   another scene switches that same Blender to it, creating an empty Blender scene the
   first time. Blender's 3D view header reads **Pixelsoup: <scene>**.
3. Edit Blender normally. The whole Blender scene syncs, including models imported into
   new collections. The **B** stays before the names of scenes Blender holds: filled
   while Blender is syncing that scene, outlined while Blender shows another scene or
   is closed. Clicking it brings Blender to that scene, reopening Blender if needed.

Every scene in the file stays linked at once; Blender syncs the scene it shows, so
switching scenes in Blender's scene selector switches which Pixelsoup scene follows.
A scene made in Blender (**Scene > New**) shows **Add to Pixelsoup** in the
Pixelsoup sidebar tab: Pixelsoup creates a scene of that name, in Preview, linked to it.

Nothing needs installing first: Pixelsoup finds the newest Blender 5.2+ (set
`MEDIASERVER_BLENDER` to `blender.exe` to choose another) and runs the add-on that
ships beside it for that Blender session, unless the installed copy is the same or
newer. **Install Add-on Permanently** in the Blender sidebar installs it, so opening the
project's `.blend` by hand relinks every scene in it, even after Pixelsoup restarts.

## Live, both ways

A linked scene is the scene, wherever it is. Blender edits reach Preview, Program and
every output node on the next control frame, so there is nothing to publish: take the
scene to Program with the ordinary switcher. Placement, lights and materials stream
while you are still dragging; a mesh or texture edit is loaded by every node and
revealed on the same frame (Program briefly holds its last frame while nodes load).

Edits made in Pixelsoup to a Blender object are written into that object in Blender:
placement (including Hold), visibility and light settings. The `.blend` therefore holds
what the wall shows, so reopening it or rendering it offline gives that scene.
Pixelsoup keeps its edit as its own override, so Pixelsoup's Undo still works on it;
Blender shows the result. Whoever edits a property last wins: when the artist then moves,
shows/hides or relights that object in Blender, the artist's value replaces
Pixelsoup's override in the same update.

- A mesh with materials reaches Pixelsoup as an object and a part per material.
  Moving the single part of a one-material mesh moves the Blender object. Parts of a
  multi-material mesh moved apart, material overrides, and objects driven by constraints
  or animation keep their Pixelsoup edits in Pixelsoup only; the sidebar counts them.
- Pixelsoup's edits are applied as their own Blender undo step: the artist's Ctrl+Z
  undoes the most recent change, Pixelsoup's included, and Pixelsoup follows.
- **File > Connect to Blender...** links a Blender you opened yourself to the Preview
  scene and adopts its file.
- **Keep file saved** (on by default; the sidebar's advanced controls) saves the `.blend`
  a few seconds after syncing goes quiet following an accepted or written-back change.
  Pixelsoup can only be given a scene by Blender, so an unsaved file is a scene
  waiting to be lost: the next Blender Pixelsoup's `B` starts on it would publish the
  older file, and objects added since would come back with new link identities that
  Pixelsoup's edits no longer attach to.

Transforms, geometry, names, materials, lights, collection membership and removals sync
automatically. Edit Mode geometry is flushed to the evaluated mesh without leaving Edit
Mode. Sync adjustments do not change authored materials, lights or object types.
Deleting a light in Blender also removes its illumination and Stage handle in
Pixelsoup; restoring it in Blender restores the same linked object.

- Content Blender sends is stored beside the project in `<project> Blender/Link
  Resources`. **Remote render nodes** read it at the same absolute path, so keep the
  project on a share mapped identically on every machine. A node that cannot read it
  reports the files missing in Output Monitor and keeps its last complete picture.
- **Save As** in Blender moves the project's scenes to the new file. Opening another file
  in that Blender releases scenes that file does not hold.
- Right-click a row > **Disconnect Blender** ends that scene's link; the scene keeps the
  last synced content and does not relink until you open it again.
- **File > Freeze Blender...** freezes Preview into an immutable revision on
  shared storage, for a Program that must not change; it is not needed to go on air.

## Recovery and status

**File > Connect to Blender...** lists every running Blender and the scene each one is
linked to: **Connect** (to Preview), **Resync Scene**, **Disconnect**. Resync starts a
new link session and rebuilds the scene snapshot, including textures. The last
accepted scene remains available while syncing or disconnected. Interrupted publisher
connections retry automatically with bounded backoff, and a connection request
Blender missed is sent again. A full transfer cache requests a fresh session
automatically.

The add-on must be running in a Blender process for Pixelsoup to see it. Discovery
is local to the same OS user. Instance records expire after ten seconds, and
connection credentials are consumed from a temporary local request, never stored in
a blend file or shown in the editor. Disabling the add-on removes its record.

The Blender sidebar normally shows sync status, adjustments and recovery controls.
**Advanced publishing controls** reveals collection-limited publishing, pause,
animation, stage references and export options.
Pixelsoup's linked collection Inspector can pause source updates. Resuming
there automatically sends the retained edits. **Blender connection...** opens the
same connection dialog.

## Automatic translation

- Cameras and unsupported object types are skipped. A camera used as a mesh's
  parent can be retained only as a transform parent.
- Curves, text and surfaces are copied as evaluated meshes. Modifiers are evaluated;
  constraints and animated objects use their current pose for ordinary live sync.
- Point and spot radius, custom contribution settings, temperature, non-normalized
  lights and disk/ellipse area lights are approximated using Pixelsoup's light
  representation. Power, dimensions and cone values are clamped to valid ranges.
- Supported Principled values and directly assigned PNG/JPEG textures are retained.
  Oversized textures are resized to at most 1024 pixels in a separate Blender
  process using temporary export copies. Original texture files remain unchanged.
  The glTF importer's base color factor (a full-strength Multiply of the texture and
  a constant) is kept, as are UV and Normal Map nodes naming the active render UV map.
  PNG/JPEG images packed into the .blend (the glTF importer's default) are retained
  even when their original files are absent. Image reroutes are supported. Packed
  files up to 16 MiB are copied without decoding; unchanged images are retained by
  the serializer, so moving a model neither recopies nor hashes its image files.
  Unpack larger packed files so the worker can prepare them. A scene publishes at
  most 256 distinct images / 256 MiB.
  Unassigned, missing, unsupported or unsaved textures are omitted; unsupported
  shader graphs fall back to viewport material color. Missing UV maps omit textures.
  Unsupported inputs use constant values. Link alpha honors Opaque, Mask and Blend
  explicitly; wiring a texture's alpha socket never turns an Opaque surface transparent.
- Large meshes are automatically divided by material and into pieces of at most
  100,000 triangles. Every triangle, UV seam, split normal and Blender tangent frame
  is retained. Tangents are computed before splitting, so mirrored UVs and chunk
  boundaries keep the source normal-map orientation. Repeated corners share vertices
  only when all attributes agree; placement and material edits reuse prepared geometry.
- Detailed geometry is supported up to a ~30M-triangle scene: one mesh may copy 4 GiB
  of evaluated arrays (about 30M triangles with UVs), a scene 8 GiB, in up to 1024
  geometry resources (4 GiB of GLBs). Splitting, welding and encoding are vectorised
  and run on every core. Measured: a 30M-triangle object syncs the first time in about
  90 s (Blender is busy for about 25 s of that, evaluating and copying the mesh); after
  that, moving or hiding it sends no geometry and takes well under a second. Empty
  meshes or geometry beyond those budgets are skipped with a **Partial sync**
  diagnostic that takes priority over material notices, allowing other supported
  objects to sync. Singular/reflected placements and unsupported procedural data may
  still need simpler source geometry.

The sidebar lists adjustments so an artist can refine visual fidelity if needed.
Automatic translation approximates supported appearance; it does not reproduce
all Cycles/Eevee shading or bake arbitrary simulation/vertex animation.
Explicit animation assemblies and base-color baking are available under Advanced.
The [material and light contract](../../blender%20live%20scene%20link/materials-and-lighting-v1.md)
and [animation contract](../../blender%20live%20scene%20link/animation-and-stage-v1.md)
describe the renderer's native representations that the automatic fallbacks above
produce; the [scene-link reference](../../blender%20live%20scene%20link/README.md)
lists the other contracts.

## Upgrade and verification

Open in Blender (the **B**) always runs the add-on that ships beside it (from
`integrations/blender` in a build tree, or `blender/` in a release package), so an
upgrade only matters for opening files by hand: use **Install Add-on Permanently**, or
install the newer archive and restart Blender.
The ZIP contains a versioned `manifest.json` with protocol, capabilities and file hashes.

```powershell
python tests/blender/test_discovery.py
python tests/blender/test_publisher_core.py
python tests/blender/test_environment_scale.py
python tests/blender/test_materials_lighting.py
python tests/blender/test_image_resize.py
python tests/blender/test_large_meshes.py
python tests/blender/test_shading_export.py
& "C:/Program Files/Blender Foundation/Blender 5.2/blender.exe" --factory-startup --background --python tests/blender/exercise_shading.py
python scripts/test-blender-livelink.py --build debug --workflow
python scripts/test-blender-livelink.py --build debug
python scripts/test-blender-livelink.py --build debug --writeback
python scripts/test-blender-livelink.py --build debug --materials
ctest --preset windows-msvc-debug-gui -R "server_blender_links|director_link|scene_linked_scene"
```

The workflow suite installs the ZIP into an isolated Blender profile and exercises
actual C++ discovery, instance selection, full-scene import/update/removal, material
fallback, light conversion, reconnect, resync, and credential cleanup. The writeback
suite drives Pixelsoup edits into Blender objects and back. Use `--build release`
to run them against the optimized build. Other suites cover collection publishing and
native material fidelity; `exercise_shading.py` checks material alpha modes, UV maps
and mirrored tangent frames against Blender itself. The Python unit tests also run in
CTest (`-R "_unit|_fixture"`) and need an interpreter with numpy.
