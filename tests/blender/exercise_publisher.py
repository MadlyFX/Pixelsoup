"""Run inside factory Blender against the actual C++ Director service fixture."""
import json
from pathlib import Path
import sys
import time

import bpy
import bmesh

repo = Path(__file__).resolve().parents[2]
directory = Path(sys.argv[sys.argv.index('--') + 1])
assert Path(bpy.utils.user_resource('SCRIPTS')).resolve() == directory / 'scripts'
assert bpy.ops.preferences.addon_install(filepath=str(directory / 'mediaserver_livelink.zip')) == {'FINISHED'}
bpy.utils.refresh_script_paths()
sys.path.insert(0, str(directory / 'scripts/addons'))
import mediaserver_livelink as addon
from mediaserver_livelink.runtime import runtime as rt
from mediaserver_livelink.extract import ID_KEY
from mediaserver_livelink.extract import describe

pairing = json.loads((directory / 'pairing.json').read_text())
checks = ['install ZIP with Blender addon_install into isolated scripts directory']


def pump(label, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        bpy.context.view_layer.update()
        rt.tick()
        if rt.worker is None:
            raise AssertionError(label + ': ' + rt.status)
        if rt.connected and not rt.busy and not rt.pending:
            checks.append(label)
            print('PASS', label, 'exports', rt.exports, 'GLBs', rt.serializations, 'bytes', rt.payload_bytes, flush=True)
            return
        time.sleep(0.02)
    raise AssertionError(label + ': timed out: ' + rt.status)


def latest():
    paths = list(directory.glob('commit-*.json'))
    return json.loads(max(paths, key=lambda p: int(p.stem.split('-')[-1])).read_text())


def occurrence(name):
    return next(obj for obj in latest()['objects'] if obj['name'] == name)


def setup():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    addon.register()
    scene = bpy.context.scene
    collection = bpy.data.collections.new('LiveLink test collection')
    scene.collection.children.link(collection)
    bpy.ops.mesh.primitive_cube_add(size=1)
    obj = bpy.context.object
    obj.name = 'Hero'
    for previous in list(obj.users_collection):
        previous.objects.unlink(obj)
    collection.objects.link(obj)
    material = bpy.data.materials.new('Copper')
    material.use_nodes = True
    material.node_tree.nodes.get('Principled BSDF').inputs['Base Color'].default_value = (0.8, 0.2, 0.1, 1)
    obj.data.materials.append(material)
    modifier = obj.modifiers.new('Artist bevel', 'BEVEL')
    modifier.width, modifier.segments = 0.1, 2
    parent = bpy.data.objects.new('Parent', None)
    collection.objects.link(parent)
    parent.location = (1, 2, 3)
    obj.parent = parent
    obj.location = (0, 0, 1)
    settings = scene.ms_livelink
    settings.collection, settings.port, settings.code = collection, pairing['port'], pairing['code']
    bpy.context.view_layer.update()
    assert bpy.ops.ms_livelink.connect() == {'FINISHED'}
    return scene


try:
    scene = setup()
    pump('initial evaluated collection')
    initial_id = bpy.data.objects['Hero'][ID_KEY]
    source_id = rt.source
    assert occurrence('Hero')['id'] == initial_id
    assert occurrence('Hero')['matrix'][12:15] == [1, 4, -2]
    assert len(bpy.data.objects['Hero'].data.vertices) == 8
    assert len(bpy.data.objects['Hero'].modifiers) == 1
    assert latest()['ingested'] == 1

    # Ancestors outside the selected collection provide placement without hiding
    # their selected children. Validate this independently of generated GLB bytes.
    collection = scene.ms_livelink.collection
    parent = bpy.data.objects['Parent']
    scene.collection.objects.link(parent)
    collection.objects.unlink(parent)
    bpy.context.view_layer.update()
    described = describe(scene, collection, rt.identities)
    assert next(row for row in described if row['name'] == 'Parent')['visible']
    assert not next(row for row in described if row['name'] == 'Parent')['prototype']
    pump('external transform parent remains visible to descendants')

    before = rt.exports
    before_glb = rt.serializations
    for i in range(50):
        bpy.data.objects['Hero'].location.x = i / 10
        bpy.context.view_layer.update()
    pump('50 coalesced transform updates')
    assert rt.exports == before, (before, rt.exports)
    assert rt.serializations == before_glb
    assert rt.last_kind == 'delta' and rt.payload_bytes < 600
    assert abs(occurrence('Hero')['matrix'][12] - 5.9) < 0.0001
    assert latest()['ingested'] == 0

    bpy.data.objects['Hero'].name = 'Renamed Hero'
    bpy.context.view_layer.update()
    rt.publish_now()  # A rename may not emit a geometry dependency update.
    pump('rename preserves identity')
    assert occurrence('Renamed Hero')['id'] == initial_id
    assert rt.exports == before

    obj = bpy.data.objects['Renamed Hero']
    obj.modifiers['Artist bevel'].segments = 3
    bpy.context.view_layer.update()
    pump('modifier evaluates without changing source mesh')
    assert rt.exports == before + 1
    assert len(obj.data.vertices) == 8 and len(obj.modifiers) == 1
    assert latest()['ingested'] == 1

    mat = bpy.data.materials['Copper']
    mat.node_tree.nodes.get('Principled BSDF').inputs['Base Color'].default_value = (0.2, 0.5, 0.8, 1)
    bpy.context.view_layer.update()
    pump('material revision')
    assert latest()['ingested'] == 1

    obj = bpy.data.objects['Renamed Hero']
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')
    mesh = bmesh.from_edit_mesh(obj.data)
    mesh.verts.ensure_lookup_table()
    mesh.verts[0].co.x -= 0.2
    bmesh.update_edit_mesh(obj.data)
    rt.mark(full=True)
    before = rt.exports
    for _ in range(5):
        rt.tick()
    assert rt.exports == before and 'finish Edit/Sculpt' in rt.status
    bpy.ops.object.mode_set(mode='OBJECT')
    pump('edit-mode completion')
    assert rt.exports == before + 1

    settings = bpy.context.scene.ms_livelink
    settings.live = False
    obj.location.y += 1
    bpy.context.view_layer.update()
    revision = rt.revision
    for _ in range(5):
        rt.tick()
    assert rt.revision == revision and rt.pending
    bpy.ops.ms_livelink.publish()
    pump('publish once while paused')
    settings.live = True
    pump('resume live')

    bpy.context.preferences.edit.use_global_undo = True
    bpy.ops.ed.undo_push(message='Before LiveLink duplicate')
    original = bpy.data.objects['Renamed Hero']
    duplicate = original.copy()
    duplicate.data = original.data.copy()
    duplicate.name = 'Copy'
    settings.collection.objects.link(duplicate)
    bpy.context.view_layer.update()
    pump('copied object and mesh receive distinct IDs')
    duplicate_id = bpy.data.objects['Copy'][ID_KEY]
    assert duplicate_id != initial_id
    assert bpy.data.objects['Copy'].data[ID_KEY] != bpy.data.objects['Renamed Hero'].data[ID_KEY]
    bpy.ops.ed.undo_push(message='After LiveLink duplicate')
    bpy.ops.ed.undo()
    pump('Blender undo removes duplicate')
    assert bpy.data.objects.get('Copy') is None
    assert next(row for row in latest()['objects'] if row['id'] == duplicate_id)['removed']
    bpy.ops.ed.redo()
    pump('Blender redo restores duplicate identity')
    assert bpy.data.objects['Copy'][ID_KEY] == duplicate_id
    assert not next(row for row in latest()['objects'] if row['id'] == duplicate_id)['removed']

    bpy.data.objects.remove(bpy.data.objects['Copy'], do_unlink=True)
    bpy.context.view_layer.update()
    pump('deletion becomes Director tombstone')
    assert next(row for row in latest()['objects'] if row['id'] == duplicate_id)['removed']

    before_mesh_count = len(bpy.data.meshes)
    for i in range(12):
        bpy.data.objects['Renamed Hero'].modifiers['Artist bevel'].width = 0.05 + i * 0.002
        bpy.context.view_layer.update()
    pump('rapid geometry edits coalesce')
    assert len(bpy.data.meshes) == before_mesh_count
    assert rt.batch is None and not rt.worker.output

    # Simulate backpressure without manufacturing a network transaction queue.
    rt.busy = True
    for i in range(1100):
        rt.mark(geometry=(100000 + i,))
    rt.tick()
    assert rt.full and len(rt.dirty) <= 1024 and rt.batch is None and not rt.worker.output
    rt.busy = False
    pump('backpressure collapses to one bounded pending state')

    rt.mark(full=True)
    rt.changed_at = 0
    rt.tick()  # Copies one prototype but has not submitted the batch.
    assert rt.batch is not None
    bpy.data.objects['Renamed Hero'].modifiers['Artist bevel'].width = 0.11
    bpy.context.view_layer.update()
    assert rt.batch is None
    pump('new geometry invalidates an incomplete extraction batch')
    assert len(bpy.data.meshes) == before_mesh_count

    blend_path = directory / 'publisher.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path))
    bpy.ops.wm.open_mainfile(filepath=str(blend_path))
    assert rt.worker is None
    assert bpy.data.objects['Renamed Hero'][ID_KEY] == initial_id
    # SKIP_SAVE RNA properties are not a credential store.
    assert not bpy.context.scene.ms_livelink.code
    bpy.context.scene.ms_livelink.code = pairing['code']
    bpy.ops.ms_livelink.connect()
    pump('save reopen reconnect preserves source identity')
    assert rt.source == source_id
    assert occurrence('Renamed Hero')['id'] == initial_id

    rt.worker.process.kill()
    time.sleep(0.1)
    try:
        rt.tick()
    except (OSError, RuntimeError) as error:
        rt.fail(error)
    assert rt.worker is None
    bpy.ops.ms_livelink.resync()
    pump('interrupted worker reconnects with fresh snapshot')
    assert rt.last_kind == 'snapshot'

    addon.unregister()
    assert not bpy.app.timers.is_registered(addon.timer)
    assert all(callback not in getattr(bpy.app.handlers, name) for name, callback in addon.HANDLERS)
    assert rt.worker is None
    for process in rt.retired:
        process.wait(timeout=5)
    checks.append('unregister releases handlers, timer and subprocesses')
    (directory / 'results.json').write_text(json.dumps({'checks': checks, 'blender': bpy.app.version_string,
        'build_hash': bpy.app.build_hash.decode(), 'mesh_extractions': rt.exports,
        'last_payload_bytes': rt.payload_bytes}, indent=2))
    print('ALL BLENDER PUBLISHER CHECKS PASSED', flush=True)
except Exception:
    rt.stop()
    import traceback
    traceback.print_exc()
    sys.exit(1)
