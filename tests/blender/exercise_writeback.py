"""Pinned Blender acceptance for Pixelsoup writeback, against the real Director
service, importer and scheduler (blender_publisher_host).

Pixelsoup's edits to linked objects are written into the Blender objects
themselves, the source sent back stays Blender's own, and an artist's later
edit to the same property replaces Pixelsoup's override instead of stacking
on it.
"""
import json
from pathlib import Path
import sys
import time

import bpy
from mathutils import Matrix

directory = Path(sys.argv[sys.argv.index('--') + 1])
assert bpy.ops.preferences.addon_install(filepath=str(directory / 'mediaserver_livelink.zip')) == {'FINISHED'}
bpy.utils.refresh_script_paths()
sys.path.insert(0, str(directory / 'scripts/addons'))
import mediaserver_livelink as addon
from mediaserver_livelink.runtime import runtime as rt
from mediaserver_livelink.extract import CONVERSION
from mediaserver_livelink.placement import flatten
from mediaserver_livelink.writeback import is_identity

checks = []


def pump(label, condition=None, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        bpy.context.view_layer.update()
        rt.tick()
        assert rt.worker, rt.status
        if condition() if condition else (not rt.busy and not rt.pending):
            print('PASS', label, flush=True)
            checks.append(label)
            return
        time.sleep(0.02)
    raise AssertionError(label + ': ' + rt.status)


def stable(label):
    pump(label)
    for _ in range(25):
        bpy.context.view_layer.update()
        rt.tick()
        time.sleep(0.02)
    assert not rt.busy and not rt.pending, rt.status


def local(action, value=(0, 0, 0), target=None):
    done = directory / 'local-shot-done.txt'
    done.unlink(missing_ok=True)
    path = directory / 'local-shot.tmp'
    path.write_text(str(rt.writeback.rows[target or identity]['node']) + ' ' + action + ' ' + ' '.join(map(str, value)))
    path.rename(directory / 'local-shot.txt')
    pump('Pixelsoup ' + action, lambda: done.exists() and done.read_text() == action)


def close_matrix(a, b, tolerance=0.0001):
    return max(abs(x - y) for x, y in zip(flatten(a), flatten(b))) < tolerance


def media_server_world(identity):
    """The object's world as the Director holds it now (MS axes)."""
    values = rt.writeback.rows[identity]['world']
    return Matrix(tuple(tuple(values[c * 4 + r] for c in range(4)) for r in range(4)))


def blender_world(obj):
    return CONVERSION @ obj.matrix_world @ CONVERSION.inverted()


try:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    addon.register()
    scene = bpy.context.scene
    collection = bpy.data.collections.new('Writeback acceptance')
    scene.collection.children.link(collection)
    bpy.ops.mesh.primitive_cube_add(size=1)
    hero = bpy.context.object
    hero.name = 'Hero'
    for owner in list(hero.users_collection):
        owner.objects.unlink(hero)
    collection.objects.link(hero)
    parent = bpy.data.objects.new('Parent', None)
    collection.objects.link(parent)
    parent.location = (1, 2, 0)
    hero.parent = parent
    hero.location = (0, 0, 1)
    hero.modifiers.new('Artist bevel', 'BEVEL').width = 0.05
    lamp = bpy.data.objects.new('Lamp', bpy.data.lights.new('Lamp', 'POINT'))
    lamp.data.energy = 100
    lamp.data.shadow_soft_size = 0
    lamp.location = (0, 0, 3)
    collection.objects.link(lamp)
    config = json.loads((directory / 'pairing.json').read_text())
    scene.ms_livelink.collection = collection
    scene.ms_livelink.port, scene.ms_livelink.code = config['port'], config['code']
    bpy.context.view_layer.update()
    assert bpy.ops.ms_livelink.connect() == {'FINISHED'}
    pump('initial source publication')
    assert rt.capabilities and rt.capabilities[0] == 'base', rt.capabilities
    identity, lamp_id = hero['ms_link_id'], lamp['ms_link_id']
    pump('Director state arrives', lambda: identity in rt.writeback.rows and lamp_id in rt.writeback.rows)

    # A Pixelsoup move is written into the object itself: no wrapper empty.
    local('world', (4, 3, 2))
    pump('Pixelsoup placement written into the object',
         lambda: close_matrix(hero.matrix_world, Matrix.Translation((4, -2, 3))))
    assert hero.parent == parent
    exports, revision = rt.exports, rt.revision
    stable('writeback produces no source feedback')
    assert rt.exports == exports and rt.revision == revision, (rt.revision, revision)
    assert not is_identity(rt.writeback.rows[identity]['offset']), 'Pixelsoup keeps its override'

    # The artist moves it: their placement replaces Pixelsoup's, never stacks.
    hero.location.x += 1
    bpy.context.view_layer.update()
    target = hero.matrix_world.copy()
    stable('artist move publishes')
    pump('Pixelsoup override absorbed', lambda: is_identity(rt.writeback.rows[identity]['offset']))
    stable('absorb settles')
    assert close_matrix(hero.matrix_world, target)
    assert close_matrix(media_server_world(identity), blender_world(hero)), 'Pixelsoup shows the artist placement'

    # Pixelsoup hides it; Blender hides it; the artist shows it again.
    local('hide')
    pump('Pixelsoup hide reaches Blender', lambda: hero.hide_get() and hero.hide_render)
    stable('hidden without feedback')
    hero.hide_set(False)
    hero.hide_render = False
    rt.mark()
    stable('artist shows it again')
    pump('visibility override absorbed', lambda: rt.writeback.rows[identity]['visible_override'] == -1
         and rt.writeback.rows[identity]['visible'])
    assert not hero.hide_get() and not hero.hide_render

    # Hiding a scatter hides its instancer in Blender, which the depsgraph's
    # instance iterator then skips: the scatter must stay, hidden, not vanish
    # as though deleted.
    bpy.ops.mesh.primitive_plane_add(size=2)
    field = bpy.context.object
    field.name = 'Field'
    for owner in list(field.users_collection):
        owner.objects.unlink(field)
    collection.objects.link(field)
    field.location = (0, -4, 0)
    bpy.ops.mesh.primitive_cube_add(size=0.2)
    pebble = bpy.context.object
    pebble.name = 'Pebble'
    for owner in list(pebble.users_collection):
        owner.objects.unlink(pebble)
    collection.objects.link(pebble)
    pebble.location = (0, -8, 0)
    tree = bpy.data.node_groups.new('Scatter', 'GeometryNodeTree')
    tree.interface.new_socket('Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
    tree.interface.new_socket('Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
    group_in, group_out = tree.nodes.new('NodeGroupInput'), tree.nodes.new('NodeGroupOutput')
    points, scatter = tree.nodes.new('GeometryNodeMeshToPoints'), tree.nodes.new('GeometryNodeInstanceOnPoints')
    info = tree.nodes.new('GeometryNodeObjectInfo')
    info.inputs['Object'].default_value = pebble
    info.inputs['As Instance'].default_value = True
    tree.links.new(group_in.outputs[0], points.inputs['Mesh'])
    tree.links.new(points.outputs[0], scatter.inputs['Points'])
    tree.links.new(info.outputs['Geometry'], scatter.inputs['Instance'])
    tree.links.new(scatter.outputs[0], group_out.inputs[0])
    field.modifiers.new('Scatter', 'NODES').node_group = tree
    bpy.context.view_layer.update()
    stable('scatter publishes')
    field_id = field['ms_link_id']
    pump('scatter arrives', lambda: any(row['parent'] == field_id for row in rt.writeback.rows.values()))
    strewn = next(key for key, row in rt.writeback.rows.items() if row['parent'] == field_id)
    local('hide', target=strewn)
    pump('Pixelsoup hide reaches the instancer', lambda: field.hide_get() and field.hide_render)
    stable('hidden scatter settles')
    rt.mark(full=True)
    stable('hidden scatter extracts again')
    assert strewn in rt.writeback.rows, 'a hidden scatter stays in Pixelsoup'
    assert not rt.writeback.rows[strewn]['removed'] and not rt.writeback.rows[strewn]['visible']
    local('show', target=strewn)
    pump('Pixelsoup show reaches the instancer', lambda: not field.hide_get() and not field.hide_render)
    stable('shown scatter settles')
    assert not rt.writeback.rows[strewn]['removed'] and rt.writeback.rows[strewn]['visible']

    # Hold keeps the object where it is when its parent moves, in both.
    local('hold')
    pump('hold arrives', lambda: rt.writeback.rows[identity]['held'])
    held = hero.matrix_world.copy()
    parent.location.x += 2
    bpy.context.view_layer.update()
    stable('parent moves under a held object')
    pump('held object stays put in Blender', lambda: close_matrix(hero.matrix_world, held))
    stable('hold settles')
    assert close_matrix(media_server_world(identity), blender_world(hero))
    local('reset')
    pump('reset returns to Blender placement', lambda: not rt.writeback.rows[identity]['held'])
    stable('reset settles')
    assert close_matrix(media_server_world(identity), blender_world(hero))

    # A Pixelsoup light edit reaches the light; the artist's edit replaces it.
    local('intensity', (5000, 0, 0), lamp_id)
    efficacy = scene.ms_livelink.luminous_efficacy
    pump('Pixelsoup intensity written into the light', lambda: abs(lamp.data.energy - 5000 / efficacy) < 1e-3)
    stable('light writeback produces no feedback')
    assert rt.writeback.rows[lamp_id]['light'].get('intensity') == 5000
    lamp.data.energy = 20
    stable('artist light edit publishes')
    pump('light override absorbed', lambda: not rt.writeback.rows[lamp_id]['light'])
    assert abs(lamp.data.energy - 20) < 1e-6

    # A mesh with a material reaches Pixelsoup as an object and one part
    # per material; moving that part there moves the Blender object.
    bpy.ops.mesh.primitive_cube_add(size=1)
    painted = bpy.context.object
    painted.name = 'Painted'
    for owner in list(painted.users_collection):
        owner.objects.unlink(painted)
    collection.objects.link(painted)
    painted.location = (-2, 0, 0)
    painted.data.materials.append(bpy.data.materials.new('Paint'))
    bpy.context.view_layer.update()
    stable('painted cube publishes')
    painted_id = painted['ms_link_id']
    pump('painted part arrives', lambda: any(row['parent'] == painted_id for row in rt.writeback.rows.values()))
    part = next(key for key, row in rt.writeback.rows.items() if row['parent'] == painted_id)
    local('world', (-5, 1, 0), part)
    pump('part placement written into its object', lambda: close_matrix(painted.matrix_world, Matrix.Translation((-5, 0, 1))))
    stable('part writeback produces no feedback')
    assert not is_identity(rt.writeback.rows[part]['offset'])
    painted.location.z += 1
    bpy.context.view_layer.update()
    stable('artist moves the painted cube')
    pump('part override absorbed', lambda: is_identity(rt.writeback.rows[part]['offset']))
    stable('part absorb settles')
    assert close_matrix(media_server_world(part), blender_world(painted)), 'Pixelsoup shows the artist placement'

    # Deleting in Pixelsoup takes the object out of the Blender scene, and
    # Pixelsoup's Undo puts it back where it was.
    placed = painted.matrix_world.copy()
    local('delete', target=painted_id)
    pump('Pixelsoup delete takes the object out of the Blender scene', lambda: not painted.users_collection)
    pump('the removal reaches Pixelsoup', lambda: rt.writeback.rows[painted_id]['removed']
         and rt.writeback.rows[painted_id]['deleted'] and rt.writeback.rows[part]['deleted'])
    stable('deletion settles')
    local('undo')
    pump('Pixelsoup Undo puts the object back', lambda: collection in painted.users_collection)
    pump('the restored object syncs again', lambda: not rt.writeback.rows[painted_id]['removed']
         and not rt.writeback.rows[painted_id]['deleted'] and not rt.writeback.rows[part]['deleted'])
    stable('restore settles')
    assert close_matrix(painted.matrix_world, placed)
    assert close_matrix(media_server_world(part), blender_world(painted)), 'restored where it was'
    # The artist bringing it back (what Blender's Undo does) replaces the deletion.
    local('delete', target=painted_id)
    pump('deleted again', lambda: not painted.users_collection and rt.writeback.rows[painted_id]['removed'])
    stable('second deletion settles')
    collection.objects.link(painted)
    rt.mark()
    stable('artist restores the object')
    pump('deletion absorbed', lambda: not rt.writeback.rows[painted_id]['deleted']
         and not rt.writeback.rows[painted_id]['removed'])
    stable('artist restore settles')
    assert collection in painted.users_collection
    assert close_matrix(media_server_world(part), blender_world(painted))

    # A Director pause retains the last complete scene and the pairing.
    local('pause')
    paused_revision = rt.revision
    hero.data.vertices[0].co.x -= 0.07
    hero.data.update()
    rt.publish_now()
    pump('paused geometry remains on the same connection', lambda: not rt.busy and rt.status.startswith('Paused in Pixelsoup'))
    assert rt.connected and rt.revision == paused_revision
    local('resume')
    pump('resume automatically syncs retained geometry', lambda: not rt.busy and rt.revision != paused_revision)

    # Save, reopen and reconnect: the saved record keeps the source Blender's
    # own, so Pixelsoup's edit is not applied twice.
    local('world', (6, 3, 2))
    pump('second Pixelsoup move written', lambda: close_matrix(hero.matrix_world, Matrix.Translation((6, -2, 3))))
    stable('second move settles')
    world = hero.matrix_world.copy()
    bpy.ops.wm.save_as_mainfile(filepath=str(directory / 'writeback.blend'))
    bpy.ops.wm.open_mainfile(filepath=str(directory / 'writeback.blend'))
    hero = bpy.data.objects['Hero']
    assert hero['ms_link_id'] == identity and close_matrix(hero.matrix_world, world)
    assert 'ms_link_applied' in hero, 'the record is saved with the object'
    scene = bpy.context.scene
    scene.ms_livelink.port, scene.ms_livelink.code = config['port'], config['code']
    assert bpy.ops.ms_livelink.connect() == {'FINISHED'}
    stable('save reopen reconnect')
    pump('Director state after reopen', lambda: identity in rt.writeback.rows)
    stable('reopen settles')
    assert close_matrix(hero.matrix_world, world)
    assert close_matrix(media_server_world(identity), blender_world(hero)), 'no stacked override after reopen'
    assert not is_identity(rt.writeback.rows[identity]['offset']), 'Pixelsoup still holds its own edit'

    addon.unregister()
    (directory / 'results.json').write_text(json.dumps({'checks': checks, 'blender': bpy.app.version_string,
        'build_hash': bpy.app.build_hash.decode(), 'mesh_extractions': rt.exports}, indent=2))
except Exception:
    import traceback
    traceback.print_exc()
    try:
        addon.unregister()
    except Exception:
        pass
    raise
