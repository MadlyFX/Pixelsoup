"""Heavy geometry through the real Director service (blender_publisher_host).

Builds one dense grid - MS_HEAVY_SIDE quads a side, two triangles each - and
measures what a detailed import costs: Blender's evaluation and copy, the
publisher's split/weld/encode and upload, the Director's import, and then that
moving the object afterwards sends no geometry at all.
"""
import json
import os
from pathlib import Path
import sys
import time

import bpy

directory = Path(sys.argv[sys.argv.index('--') + 1])
assert bpy.ops.preferences.addon_install(filepath=str(directory / 'mediaserver_livelink.zip')) == {'FINISHED'}
bpy.utils.refresh_script_paths()
sys.path.insert(0, str(directory / 'scripts/addons'))
import mediaserver_livelink as addon
from mediaserver_livelink.runtime import runtime as rt

side = int(os.environ.get('MS_HEAVY_SIDE', '1415'))
checks, timings = [], {}


def pump(label, condition=None, timeout=1500):
    started = time.monotonic()
    deadline = started + timeout
    while time.monotonic() < deadline:
        bpy.context.view_layer.update()
        rt.tick()
        assert rt.worker, rt.status
        if condition() if condition else (not rt.busy and not rt.pending):
            elapsed = time.monotonic() - started
            print('PASS', label, round(elapsed, 2), 's', flush=True)
            checks.append(label)
            return elapsed
        time.sleep(0.005)
    raise AssertionError(label + ': ' + rt.status)


try:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    addon.register()
    scene = bpy.context.scene
    collection = bpy.data.collections.new('Heavy')
    scene.collection.children.link(collection)
    started = time.monotonic()
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=side + 1, y_subdivisions=side + 1, size=20)
    grid = bpy.context.object
    for owner in list(grid.users_collection):
        owner.objects.unlink(grid)
    collection.objects.link(grid)
    grid.data.materials.append(bpy.data.materials.new('Scan'))
    triangles = sum(len(p.vertices) - 2 for p in grid.data.polygons)
    timings['build_mesh_s'] = round(time.monotonic() - started, 2)
    print('grid', side, 'quads a side,', triangles, 'triangles', flush=True)

    config = json.loads((directory / 'pairing.json').read_text())
    scene.ms_livelink.collection = collection
    scene.ms_livelink.port, scene.ms_livelink.code = config['port'], config['code']
    started = time.monotonic()
    assert bpy.ops.ms_livelink.connect() == {'FINISHED'}
    timings['publish_s'] = round(pump('heavy publication accepted'), 2)
    timings['blender_copy_ms'] = round(rt.last_extract_ms, 1)
    assert rt.connected and rt.revision, rt.status
    timings['resources'] = rt.exports

    # Placement only: no geometry is evaluated, copied or sent again.
    exports, revision = rt.exports, rt.revision
    grid.location.x += 1
    bpy.context.view_layer.update()
    timings['move_s'] = round(pump('moving the heavy object publishes', lambda: rt.revision != revision and not rt.busy), 2)
    assert rt.exports == exports, 'a move re-extracted geometry'

    addon.unregister()
    (directory / 'results.json').write_text(json.dumps({
        'checks': checks, 'side': side, 'triangles': triangles, 'timings': timings,
        'blender': bpy.app.version_string}, indent=2))
except Exception:
    import traceback
    traceback.print_exc()
    try:
        addon.unregister()
    except Exception:
        pass
    raise
