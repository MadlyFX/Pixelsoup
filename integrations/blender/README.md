# Media Server LiveLink for Blender

Add-on **0.8.2**, Blender **5.2 or newer**, authoring link protocol **v1**.
Tested with Blender 5.2.0 LTS (fbe6228777e7) on Windows.

## Connect and work

1. Build the add-on with `python scripts/package-blender-livelink.py`.
2. In Blender, open **Edit > Preferences > Add-ons > Install from Disk**, install
   `out/blender-livelink/mediaserver_livelink-0.8.2.zip`, and enable **Media Server LiveLink**.
3. In Media Server choose **File > Connect to Blender...**, select the detected
   Blender instance, and click **Connect**. Unsaved scenes and multiple Blender
   instances appear separately. No clipboard, endpoint or pairing-code entry is required.
4. Edit Blender normally. Media Server switches to Preview and syncs the entire
   active Blender scene, including models imported into newly created collections.
   A first connection creates a Preview scene when the project needs one.

Transforms, geometry, names, materials, lights, collection membership and removals
sync automatically. Changes are coalesced; placement changes reuse mesh resources.
Edit Mode geometry is flushed to the evaluated mesh without leaving Edit Mode.
Blender remains the source. Sync adjustments do not change authored materials,
lights, object types or camera settings.

The add-on must be enabled in each Blender process to discover it. Discovery is
local to the same OS user; a plain Blender process without the add-on does not
expose a scene for connection. Instance records expire after ten seconds, and
connection credentials are consumed from a temporary local request, never stored
in a blend file or shown in the editor. Disabling the add-on removes its record.

## Recovery and status

**File > Connect to Blender...** is the connection home: select an instance,
**Connect**, **Resync Scene**, or **Disconnect**. Resync starts a new link session
and rebuilds the current scene snapshot, including textures. The last accepted
scene remains available while syncing or disconnected. Interrupted publisher
connections retry automatically with bounded backoff. A full transfer cache
requests a fresh session automatically. Opening a different blend
file disconnects the previous source; select that instance again to link its new scene.

The Blender sidebar normally shows sync status, adjustments and recovery controls.
**Advanced publishing controls** reveals collection-limited publishing, pause,
animation, stage references, source/shot placement and export options.
Media Server's linked collection Inspector can pause source updates. Resuming
there automatically sends the retained edits. **Blender connection...** opens the
same connection dialog.

Updates enter Preview. Program publishing and its storage settings live separately
under **File > Publish Preview...**.

## Automatic translation

- Cameras and unsupported object types are skipped. A camera used as a mesh's
  parent can be retained only as a transform parent.
- Curves, text and surfaces are copied as evaluated meshes. Modifiers are evaluated;
  constraints and animated objects use their current pose for ordinary live sync.
- Point and spot radius, custom contribution settings, temperature, non-normalized
  lights and disk/ellipse area lights are approximated using Media Server's light
  representation. Power, dimensions and cone values are clamped to valid ranges.
- Supported Principled values and directly assigned PNG/JPEG textures are retained.
  Oversized textures are resized to at most 1024 pixels in a separate Blender
  process using temporary export copies. Original texture files remain unchanged.
  Unassigned, missing, unsupported or unsaved textures are omitted; unsupported
  shader graphs fall back to viewport material color. Missing UV maps omit textures.
  Unsupported inputs use constant values. Link alpha honors Opaque, Mask and Blend
  explicitly; wiring a texture's alpha socket never turns an Opaque surface transparent.
- Large meshes are automatically divided by material and into pieces of at most
  100,000 triangles. Every triangle, UV seam, split normal and Blender tangent frame
  is retained. Tangents are computed before splitting, so mirrored UVs and chunk
  boundaries keep the source normal-map orientation. Repeated corners share vertices
  only when all attributes agree; placement and material edits reuse prepared geometry.
- Empty meshes or geometry beyond the 256 MiB evaluated-array budget are skipped with
  a **Partial sync** diagnostic that takes priority over material notices, allowing other
  supported objects to sync. Singular/reflected placements and unsupported procedural
  data may still need simpler source geometry; resource and scene-size limits remain
  enforced to keep the publisher and renderer bounded. Published GLB resources retain
  a separate 128 MiB scene limit after welding and splitting.

The sidebar lists adjustments so an artist can refine visual fidelity if needed.
Automatic translation approximates supported appearance; it does not reproduce
all Cycles/Eevee shading or bake arbitrary simulation/vertex animation.
Explicit animation assemblies and base-color baking remain available in Advanced.
The older [translation contract](../../blender%20live%20scene%20link/materials-and-lighting-v1.md)
and [animation contract](../../blender%20live%20scene%20link/animation-and-stage-v1.md)
describe the renderer's native representations; 0.8.0 adds the fallbacks above.

## Upgrade and verification

Install the newer archive and restart Blender. Connect from Media Server again.
The ZIP contains a versioned `manifest.json` with protocol, capabilities and file hashes.

```powershell
python tests/blender/test_discovery.py
python tests/blender/test_publisher_core.py
python tests/blender/test_environment_scale.py
python tests/blender/test_materials_lighting.py
python scripts/test-blender-livelink.py --build debug --workflow
python scripts/test-blender-livelink.py --build debug
python scripts/test-blender-livelink.py --build debug --shot
python scripts/test-blender-livelink.py --build debug --materials
```

The workflow suite installs the ZIP into an isolated Blender profile and exercises
actual C++ discovery, instance selection, full-scene import/update/removal, material
fallback, light conversion, reconnect, resync, and credential cleanup. Use
`--build release` to run it against the optimized build. Other suites cover
collection publishing, source/shot round trips and native material fidelity.
