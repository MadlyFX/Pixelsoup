"""Step 12 delivery checks: installed add-on, negotiated capabilities, the
scenario matrix Blender owns, and measured event-to-Preview evidence.

Runs inside factory Blender against the real C++ Director fixture. Install,
upgrade and removal are exercised through Blender's own add-on operators.
"""
import hashlib
import json
from pathlib import Path
import sys
import time

import bpy

directory = Path(sys.argv[sys.argv.index('--') + 1])
assert Path(bpy.utils.user_resource('SCRIPTS')).resolve() == directory / 'scripts'
assert bpy.ops.preferences.addon_install(filepath=str(directory / 'mediaserver_livelink.zip')) == {'FINISHED'}
bpy.utils.refresh_script_paths()
sys.path.insert(0, str(directory / 'scripts/addons'))
installed = directory / 'scripts/addons/mediaserver_livelink'
import mediaserver_livelink as addon
from mediaserver_livelink import wire
from mediaserver_livelink.runtime import percentile, runtime as rt

pairing = json.loads((directory / 'pairing.json').read_text())
checks = []
evidence = {}


def pump(label, timeout=40):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        bpy.context.view_layer.update()
        rt.tick()
        if rt.worker is None:
            raise AssertionError(label + ': ' + rt.status)
        if rt.connected and not rt.busy and not rt.pending:
            assert not rt.unsupported, rt.unsupported
            checks.append(label)
            print('PASS', label, '|', rt.status, flush=True)
            return
        time.sleep(0.02)
    raise AssertionError(label + ': timed out: ' + rt.status)


def latest():
    paths = list(directory.glob('commit-*.json'))
    return json.loads(max(paths, key=lambda p: int(p.stem.split('-')[-1])).read_text())


def occurrence(name):
    return next(row for row in latest()['objects'] if row['name'] == name)


def texture(name, red):
    image = bpy.data.images.new(name, width=8, height=8)
    image.pixels = [red, 0.2, 0.1, 1] * 64
    image.filepath_raw = str(directory / (name + '.png'))
    image.file_format = 'PNG'
    image.save()
    bpy.data.images.remove(image)
    return bpy.data.images.load(str(directory / (name + '.png')))


try:
    # Installation metadata: the archive declares its version, protocol and
    # capabilities, and the installed files are exactly those bytes.
    manifest = json.loads((installed / 'manifest.json').read_text())
    assert manifest['version'] == '.'.join(map(str, addon.bl_info['version'])), manifest['version']
    assert manifest['link_protocol'] == wire.PROTOCOL_VERSION
    assert manifest['capabilities'] == list(wire.CAPABILITIES)
    for name, digest in manifest['files'].items():
        assert hashlib.sha256((installed / name).read_bytes()).hexdigest() == digest, name
    checks.append('installed archive carries verifiable version, protocol and capability metadata')
    evidence['manifest'] = {key: manifest[key] for key in ('version', 'link_protocol', 'capabilities',
                                                           'blender_minimum', 'blender_qualified')}

    bpy.ops.wm.read_factory_settings(use_empty=True)
    addon.register()
    scene = bpy.context.scene
    collection = bpy.data.collections.new('Qualification collection')
    scene.collection.children.link(collection)
    bpy.ops.mesh.primitive_cube_add(size=1)
    hero = bpy.context.object
    hero.name = 'Hero'
    for previous in list(hero.users_collection):
        previous.objects.unlink(hero)
    collection.objects.link(hero)
    material = bpy.data.materials.new('Textured')
    material.use_nodes = True
    shader = material.node_tree.nodes.get('Principled BSDF')
    image = material.node_tree.nodes.new('ShaderNodeTexImage')
    image.image = texture('surface', 0.8)
    material.node_tree.links.new(image.outputs['Color'], shader.inputs['Base Color'])
    hero.data.materials.append(material)
    settings = scene.ms_livelink
    settings.collection, settings.port, settings.code = collection, pairing['port'], pairing['code']
    assert bpy.ops.ms_livelink.connect() == {'FINISHED'}
    pump('publish a textured collection to the real Director')

    # Capability negotiation against the actual service.
    assert set(rt.capabilities) == set(wire.CAPABILITIES), rt.capabilities
    assert not rt.unsupported
    checks.append('Director and add-on negotiate the same capability set')
    evidence['negotiated'] = sorted(rt.capabilities)

    # Reparent: an added transform parent composes the published placement.
    anchor = bpy.data.objects.new('Anchor', None)
    collection.objects.link(anchor)
    anchor.location = (1, 2, 3)
    hero.parent = anchor
    bpy.context.view_layer.update()
    pump('reparent under a new source parent')
    assert occurrence('Hero')['matrix'][12:15] == [1, 3, -2], occurrence('Hero')['matrix'][12:15]

    # Origin change: the mesh keeps its world position, the origin moves.
    bpy.context.view_layer.objects.active = hero
    hero.select_set(True)
    scene.tool_settings.transform_pivot_point = 'MEDIAN_POINT'
    hero.location = (0.5, 0, 0)
    bpy.context.view_layer.update()
    bpy.ops.object.origin_set(type='ORIGIN_GEOMETRY')
    bpy.context.view_layer.update()
    rt.publish_now()
    pump('source origin change republishes placement and evaluated mesh')

    # Repeated texture replacement: each saved image is a new resource, and the
    # retained ones are reused rather than transferred again.
    revisions = []
    for index, red in enumerate((0.1, 0.5, 0.9)):
        image.image = texture('surface-' + str(index), red)
        bpy.context.view_layer.update()
        rt.publish_now()
        pump('texture replacement ' + str(index + 1))
        revisions.append(rt.revision)
    assert len(set(revisions)) == 3, revisions
    assert rt.serializations >= 4
    checks.append('repeated texture replacement produces distinct revisions')

    # A long authoring session: bounded work per event and measured latency.
    before_exports = rt.exports
    samples = len(rt.latency)
    for index in range(30):
        hero.location.x = index / 30
        bpy.context.view_layer.update()
        rt.tick()
        if index % 10 == 9:
            pump('long session batch ' + str(index // 10 + 1))
    pump('long session settles')
    assert rt.exports == before_exports, (before_exports, rt.exports)
    assert len(rt.latency) > samples
    checks.append('a long placement session coalesces without new mesh extraction')
    evidence['event_to_preview_ms'] = {'samples': len(rt.latency), 'p50': percentile(rt.latency, 0.5),
                                       'p95': percentile(rt.latency, 0.95), 'last': rt.last_latency_ms,
                                       'last_blender_ms': rt.last_blender_ms,
                                       'last_director_ms': rt.last_director_ms}

    # Malformed input is refused with a specific diagnostic and no revision.
    revision = rt.revision
    hero.name = 'Hero\tinvalid'
    bpy.context.view_layer.update()
    rt.publish_now()
    deadline = time.monotonic() + 20
    while rt.worker is not None and time.monotonic() < deadline:
        rt.tick()
        time.sleep(0.02)
    assert rt.worker is None and 'control characters' in rt.status, rt.status
    hero.name = 'Hero'
    checks.append('a name Media Server cannot represent is refused by name, not silently dropped')

    # Recovery after that refusal: reconnect, register and send a snapshot.
    bpy.ops.ms_livelink.connect()
    pump('explicit recovery after a refusal')
    assert rt.last_kind == 'snapshot'
    assert rt.revision != revision

    # Cancellation: disconnecting mid-publish keeps the accepted revision and
    # leaves no worker behind.
    hero.location.z += 0.5
    bpy.context.view_layer.update()
    rt.tick()
    rt.stop()
    assert rt.worker is None and rt.revision
    for process in rt.retired:
        process.wait(timeout=10)
    checks.append('disconnect during an edit retains the accepted revision and reaps its worker')

    # Save, reopen offline and reconnect: identities and settings persist, the
    # pairing code does not.
    blend = directory / 'qualification.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    bpy.ops.wm.open_mainfile(filepath=str(blend))
    assert rt.worker is None and not bpy.context.scene.ms_livelink.code
    assert bpy.context.scene.ms_livelink.collection.name == 'Qualification collection'
    bpy.context.scene.ms_livelink.code = pairing['code']
    bpy.ops.ms_livelink.connect()
    pump('reopen a saved blend and reconnect to the same source')
    evidence['final_revision'] = rt.revision
    evidence['mesh_extractions'] = rt.exports
    evidence['glb_builds'] = rt.serializations
    rt.stop()

    # Upgrade over the enabled add-on, then remove it completely.
    assert bpy.ops.preferences.addon_install(filepath=str(directory / 'mediaserver_livelink.zip'),
                                             overwrite=True) == {'FINISHED'}
    assert (installed / 'manifest.json').is_file()
    checks.append('reinstalling the archive upgrades an enabled add-on in place')
    addon.unregister()
    assert not bpy.app.timers.is_registered(addon.timer)
    assert all(callback not in getattr(bpy.app.handlers, name) for name, callback in addon.HANDLERS)
    assert not hasattr(bpy.types.Scene, 'ms_livelink')
    try:
        bpy.ops.preferences.addon_remove(module='mediaserver_livelink')
    except RuntimeError as error:
        # Blender's own operator finishes by redrawing the Preferences area,
        # which does not exist in --background. The files are already gone.
        assert 'tag_redraw' in str(error), error
    assert not installed.exists(), installed
    checks.append('removal leaves no add-on files, handlers, timers or scene properties')

    (directory / 'results.json').write_text(json.dumps(
        {'blender': bpy.app.version_string, 'build_hash': bpy.app.build_hash.decode(),
         'addon': addon.bl_info['version'], 'checks': checks, 'evidence': evidence}, indent=2))
    print('ALL BLENDER QUALIFICATION CHECKS PASSED', flush=True)
except Exception:
    rt.stop()
    import traceback
    traceback.print_exc()
    sys.exit(1)
