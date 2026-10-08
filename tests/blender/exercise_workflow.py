"""Actual Blender + C++ service: the editor's local discovery/connection path."""
import json
import math
import os
from pathlib import Path
import sys
import time

import bpy

directory = Path(sys.argv[sys.argv.index('--') + 1])
bpy.ops.preferences.addon_install(filepath=str(directory / 'mediaserver_livelink.zip'))
bpy.utils.refresh_script_paths()
sys.path.insert(0, str(directory / 'scripts/addons'))
import mediaserver_livelink as addon
from mediaserver_livelink.runtime import runtime as rt, timer
from mediaserver_livelink.discovery import discovery, Discovery
from mediaserver_livelink.extract import describe
from mediaserver_livelink.translation import notices

checks = []


def latest():
    paths = list(directory.glob('commit-*.json'))
    return json.loads(max(paths, key=lambda p: int(p.stem.split('-')[-1])).read_text()) if paths else {'objects': []}


def visible(name):
    return any(o['name'] == name and not o['removed'] for o in latest()['objects'])


def pump(label, predicate, timeout=40):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        bpy.context.view_layer.update()
        timer()
        if predicate():
            print('PASS', label, flush=True)
            checks.append(label)
            return
        time.sleep(0.03)
    raise AssertionError(label + ': ' + rt.status)


try:
    discovery.root = directory / 'discovery'
    addon.register()
    # Factory startup contains a camera and a point light with nonzero radius.
    scene = bpy.context.scene
    assert scene.camera and bpy.data.objects['Light'].data.shadow_soft_size > 0
    other = Discovery(discovery.root)
    other.start()
    other.poll('Other Blender', 'Other scene', 'Ready')
    stale = Discovery(discovery.root); stale.start(); stale.poll('Closed Blender', 'Old scene', 'Ready')
    stale_path = discovery.root / (stale.identity + '.instance')
    os.utime(stale_path, (time.time()-60, time.time()-60))
    def detected():
        path = directory / 'instances.txt'
        try:
            text = path.read_text()
            return discovery.identity in text and other.identity in text and stale.identity not in text
        except FileNotFoundError:
            return False
    pump('C++ discovers two Blender instances', detected)
    (directory / 'connect-instance').write_text(discovery.identity)
    pump('select instance and sync default scene without clipboard pairing', lambda: rt.connected and visible('Cube'))
    assert not (discovery.root / (other.identity + '.request')).exists()
    assert scene.ms_livelink.collection is None and rt.auto_connect
    rows = describe(scene, scene.collection, rt.identities)
    assert not any(r['name'] == 'Camera' for r in rows)
    assert any(r.get('light') for r in rows)
    assert bpy.data.objects['Light'].data.shadow_soft_size > 0
    pump('optional reverse placement tolerates lights in the same scene', lambda: bool(rt.shot.rows) and not rt.shot.refresh)
    assert 'reverse placement' not in rt.status
    checks.append('camera skipped and default light translated without altering source')
    bpy.ops.object.light_add(type='SPOT')
    spot = bpy.context.object; spot.name = 'Wide spot'; spot.data.spot_size = math.pi
    pump('unsupported wide spotlight cone is clamped during sync', lambda: visible('Wide spot'))
    row = next(r for r in describe(scene, scene.collection, rt.identities) if r['name'] == 'Wide spot')
    assert row['light']['values'][6] < 0.249 and abs(spot.data.spot_size-math.pi) < 1e-5

    before = rt.exports
    revision = rt.revision
    bpy.data.objects['Cube'].location.x += 2
    pump('transform updates automatically without extracting geometry', lambda: rt.revision != revision and not rt.busy)
    assert rt.exports == before

    imported = bpy.data.collections.new('Imported model collection')
    scene.collection.children.link(imported)
    bpy.ops.mesh.primitive_cube_add()
    obj = bpy.context.object
    obj.name = 'Imported model'
    for collection in list(obj.users_collection):
        collection.objects.unlink(obj)
    imported.objects.link(obj)
    pump('new model in a new collection syncs automatically', lambda: visible('Imported model'))
    obj.name = 'Renamed import'
    pump('rename detected without manual publishing', lambda: visible('Renamed import'))

    large_mesh = bpy.data.meshes.new('Imported high resolution mesh')
    large_mesh.from_pydata([(0,0,0), (1,0,0), (0,1,0)], [], [(0,1,2)] * 100_001)
    large = bpy.data.objects.new('Large imported model', large_mesh)
    imported.objects.link(large)
    pump('large import automatically creates every bounded geometry part',
         lambda: visible('Large imported model / Default') and visible('Large imported model / Default / Part 2'))
    assert len(large_mesh.polygons) == 100_001
    before, serialized, revision = rt.exports, rt.serializations, rt.revision
    large.location.x += 3
    pump('moving a large import reuses all geometry resources', lambda: rt.revision != revision and not rt.busy)
    assert rt.exports == before and rt.serializations == serialized
    bpy.data.objects.remove(large, do_unlink=True)
    pump('removing a large import removes its geometry parts automatically',
         lambda: all(o['removed'] for o in latest()['objects'] if o['name'].startswith('Large imported model')))

    mat = bpy.data.materials.new('Unassigned texture')
    mat.use_nodes = True
    shader = mat.node_tree.nodes.get('Principled BSDF')
    texture = mat.node_tree.nodes.new('ShaderNodeTexImage')
    mat.node_tree.links.new(texture.outputs['Color'], shader.inputs['Base Color'])
    obj.data.materials.append(mat)
    revision = rt.revision
    pump('unassigned texture uses material fallback and continues syncing', lambda: rt.revision != revision and not rt.busy)
    assert rt.connected and rt.unsupported
    light = bpy.data.objects['Light'].data
    light.use_temperature = True
    light.energy = 321
    revision = rt.revision
    pump('light datablock edit syncs without Publish Now', lambda: rt.revision != revision and not rt.busy)
    assert light.use_temperature and light.energy == 321

    # Curves are copied from evaluated geometry without modifying source type.
    bpy.ops.curve.primitive_bezier_circle_add()
    curve = bpy.context.object
    curve.name = 'Evaluated curve'
    curve.data.bevel_depth = 0.1
    pump('curve converts automatically to evaluated mesh', lambda: visible('Evaluated curve'))
    assert curve.type == 'CURVE'

    bpy.data.objects.remove(obj, do_unlink=True)
    pump('deleted import disappears automatically', lambda: any(o['name'] == 'Renamed import' and o['removed'] for o in latest()['objects']))
    revision = rt.revision
    rt.worker.process.kill()
    pump('interrupted publisher reconnects and resyncs automatically', lambda: rt.connected and rt.revision != revision and not rt.busy)
    revision = rt.revision
    (directory / 'restart-link').touch()
    pump('editor Resync Scene rotates session and sends a fresh snapshot', lambda: rt.connected and rt.revision != revision and not rt.busy)
    assert rt.last_kind == 'snapshot'
    code = rt.pairing_code
    rt.fail('Pairing resource capacity reached')
    pump('resource exhaustion requests a fresh session automatically', lambda: rt.pairing_code != code and rt.connected and not rt.busy)
    saved = directory / 'workflow.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(saved))
    code = rt.pairing_code
    bpy.ops.wm.open_mainfile(filepath=str(saved))
    assert not rt.pairing_code and rt.worker is None
    assert code.encode() not in saved.read_bytes()
    checks.append('connection credential not saved in blend file')
    other.stop(); stale.stop()
    addon.unregister()
    assert not (discovery.root / (discovery.identity + '.instance')).exists()
    checks.append('add-on disable removes discovery and releases publisher')
    (directory / 'results.json').write_text(json.dumps({'checks': checks, 'blender': bpy.app.version_string}, indent=2))
finally:
    rt.stop()
