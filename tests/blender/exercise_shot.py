"""Pinned Blender acceptance against the real Director, importer and scheduler."""
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
from mediaserver_livelink.extract import describe
from mediaserver_livelink.shot import source_local, source_parent, qualify, flatten

checks = []


def pump(label, condition=None, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        bpy.context.view_layer.update()
        rt.tick()
        assert rt.worker, rt.status
        if condition() if condition else (not rt.busy and not rt.pending and not rt.shot.gesture):
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


def select(obj):
    for current in bpy.context.selected_objects:
        current.select_set(False)
    obj.hide_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def local(action, position=(0,0,0)):
    path = directory / 'local-shot.tmp'
    path.write_text(str(rt.shot.rows[identity]['node']) + ' ' + action + ' ' + ' '.join(map(str, position)))
    path.rename(directory / 'local-shot.txt')


def close_matrix(a, b):
    return max(abs(x-y) for x,y in zip(flatten(a),flatten(b))) < 0.0001


try:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    addon.register()
    scene = bpy.context.scene
    collection = bpy.data.collections.new('Shot acceptance')
    scene.collection.children.link(collection)
    bpy.ops.mesh.primitive_cube_add(size=1)
    hero = bpy.context.object
    hero.name = 'Hero'
    for owner in list(hero.users_collection):
        owner.objects.unlink(hero)
    collection.objects.link(hero)
    parent = bpy.data.objects.new('Parent',None)
    collection.objects.link(parent)
    parent.location = (1,2,0)
    hero.parent = parent
    hero.location = (0,0,1)
    bevel = hero.modifiers.new('Artist bevel','BEVEL')
    bevel.width = 0.05
    config = json.loads((directory / 'pairing.json').read_text())
    scene.ms_livelink.collection = collection
    scene.ms_livelink.port, scene.ms_livelink.code = config['port'],config['code']
    bpy.context.view_layer.update()
    original_basis = hero.matrix_basis.copy()
    original_local = source_local(hero)
    assert bpy.ops.ms_livelink.connect() == {'FINISHED'}
    pump('initial source publication')
    identity = hero['ms_link_id']
    pump('Director reverse state creates managed wrapper',lambda: identity in rt.shot.rows and hero.parent.get('ms_link_shot_wrapper'))
    assert close_matrix(original_basis,hero.matrix_basis) and close_matrix(original_local,source_local(hero))
    assert source_parent(hero) == parent and len(hero.data.vertices)==8 and len(hero.modifiers)==1
    rows_before = describe(scene,collection,rt.identities)

    local('world',(4,3,2))
    pump('Director placement appears in Blender',lambda: abs(hero.matrix_world.translation.x-4)<0.001)
    assert close_matrix(hero.matrix_world,Matrix.Translation((4,-2,3)))
    assert describe(scene,collection,rt.identities) == rows_before
    exports = rt.exports
    revision = rt.revision
    stable('reverse placement produces no source feedback')
    assert rt.exports==exports and rt.revision==revision

    # Step 07: a Director pause retains the last complete scene and the pairing.
    local('pause')
    done = directory / 'local-shot-done.txt'
    pump('Director pauses source updates', lambda: done.exists() and done.read_text() == 'pause')
    paused_revision = rt.revision
    hero.data.vertices[0].co.x -= 0.07
    hero.data.update()
    rt.publish_now()
    pump('paused geometry remains on the same connection', lambda: not rt.busy and rt.status.startswith('Paused in Media Server'))
    assert rt.connected and rt.revision == paused_revision
    local('resume')
    pump('Director resumes source updates', lambda: done.exists() and done.read_text() == 'resume')
    pump('resume automatically syncs retained geometry', lambda: not rt.busy and rt.revision != paused_revision)
    assert close_matrix(hero.matrix_world,Matrix.Translation((4,-2,3)))

    for policy in ('hold','follow'):
        local(policy)
        pump(policy+' policy arrives',lambda: rt.shot.rows[identity]['held']==(policy=='hold'))
        world = hero.matrix_world.copy()
        hero.data.vertices[0].co.x -= 0.1
        hero.data.update()
        stable(policy+' retains placement through vertex edit')
        assert close_matrix(hero.matrix_world,world)
        parent.location.x += 2
        stable(policy+' handles source parent movement')
        expected = world if policy=='hold' else Matrix.Translation((2,0,0)) @ world
        assert close_matrix(hero.matrix_world,expected),(hero.matrix_world,expected)

    select(hero)
    assert bpy.ops.ms_livelink.shot(action='begin') == {'FINISHED'}
    pump('Blender claims shot gesture',lambda: rt.shot.gesture and rt.shot.gesture['ready'])
    assert bpy.context.object == hero.parent
    before_world = hero.matrix_world.copy()
    hero.parent.location.x += 1.25
    bpy.context.view_layer.update()
    target = hero.matrix_world.copy()
    assert bpy.ops.ms_livelink.shot(action='commit') == {'FINISHED'}
    stable('Blender shot commit converges')
    assert close_matrix(hero.matrix_world,target)
    assert close_matrix(hero.matrix_basis,original_basis)
    hero.data.vertices[1].co.z += 0.15
    hero.data.update()
    stable('remote geometry after local shot commit')
    mesh_after = hero.data.vertices[1].co.copy()
    assert bpy.ops.ms_livelink.shot(action='undo') == {'FINISHED'}
    stable('shot undo preserves subsequent geometry')
    assert close_matrix(hero.matrix_world,before_world)
    assert hero.data.vertices[1].co == mesh_after

    select(hero)
    bpy.ops.ms_livelink.shot(action='begin')
    pump('concurrent gesture claim',lambda: rt.shot.gesture and rt.shot.gesture['ready'])
    hero.parent.location.x += 1
    bpy.context.view_layer.update()
    local('world',(12,3,2))
    pump('Director concurrent move received',lambda: rt.shot.rows[identity]['world'][12]==12)
    bpy.ops.ms_livelink.shot(action='commit')
    pump('same property produces explicit conflict',lambda: bool(rt.shot.conflict))
    bpy.ops.ms_livelink.shot(action='retry')
    stable('explicit retry applies retained gesture against current revision')
    assert abs(hero.matrix_world.translation.x-12)>0.5

    select(hero)
    rt.shot.begin()
    pump('expiring gesture claim',lambda: rt.shot.gesture and rt.shot.gesture['ready'])
    hero.parent.location.y += 0.75
    bpy.context.view_layer.update()
    expired_world=hero.matrix_world.copy()
    pump('uncommitted gesture expires explicitly',lambda: bool(rt.shot.conflict))
    assert not rt.shot.gesture['ready'] and not rt.shot.gesture['action']
    rt.shot.retry()
    stable('expiry retry retains the uncommitted proposed placement')
    assert close_matrix(hero.matrix_world,expired_world)

    select(hero)
    bpy.ops.ms_livelink.shot(action='hide')
    pump('shot visibility reaches Blender',lambda: not rt.shot.gesture and hero.hide_get())
    assert next(row for row in describe(scene,collection,rt.identities) if row['id']==identity)['visible']
    # Hidden objects remain addressable in the Outliner; the wrapper is selectable.
    select(hero.parent)
    bpy.ops.ms_livelink.shot(action='visibility')
    stable('reset visibility follows source')
    assert not hero.hide_get()
    bpy.ops.ms_livelink.shot(action='view')
    stable('source layer visibly separates authored placement')
    assert rt.shot.source_view and close_matrix(hero.matrix_world,parent.matrix_world @ source_local(hero))
    source_world=hero.matrix_world.copy()
    bpy.ops.ms_livelink.shot(action='view')
    stable('return to shot display preserves source visibility')
    assert not hero.hide_get()
    assert not close_matrix(source_world,hero.matrix_world)

    # Exact shear resides in parent inverse, avoiding Blender TRS decomposition.
    select(hero)
    rt.shot.begin()
    pump('affine gesture claim',lambda: rt.shot.gesture and rt.shot.gesture['ready'])
    shear = Matrix.Identity(4); shear[0][1]=0.3
    hero.parent.matrix_parent_inverse = shear @ hero.parent.matrix_parent_inverse
    bpy.context.view_layer.update()
    affine = hero.matrix_world.copy()
    rt.shot.commit()
    stable('nonuniform affine/shear round trip')
    assert close_matrix(hero.matrix_world,affine)
    # Reparent in Source view, preserving the authored local channels.
    bpy.ops.ms_livelink.shot(action='view')
    stable('source view before reparent')
    new_parent=bpy.data.objects.new('New parent',None)
    collection.objects.link(new_parent)
    new_parent.location=(0,1,0)
    hero.parent=new_parent
    hero.matrix_parent_inverse=Matrix.Identity(4)
    stable('source reparent publishes supported hierarchy')
    assert source_parent(hero)==new_parent
    bpy.ops.ms_livelink.shot(action='view')
    stable('shot display after source reparent')
    assert source_parent(hero)==new_parent

    scene.ms_livelink.live=False
    constraint = hero.constraints.new('COPY_LOCATION')
    try:
        qualify(hero)
        raise AssertionError('constraint accepted')
    except ValueError as error:
        assert 'constraints' in str(error)
    hero.constraints.remove(constraint)
    hero.keyframe_insert(data_path='location',frame=1)
    try:
        qualify(hero)
        raise AssertionError('animation accepted')
    except ValueError as error:
        assert 'animation' in str(error)
    hero.animation_data_clear()
    checks.append('constraints and animated parents explicitly refused')
    scene.ms_livelink.live=True
    stable('source remains usable after reverse refusal')

    world=hero.matrix_world.copy()
    bpy.ops.wm.save_as_mainfile(filepath=str(directory/'shot.blend'))
    bpy.ops.wm.open_mainfile(filepath=str(directory/'shot.blend'))
    hero=bpy.data.objects['Hero']
    assert hero['ms_link_id']==identity and close_matrix(hero.matrix_world,world)
    scene=bpy.context.scene
    scene.ms_livelink.port,scene.ms_livelink.code=config['port'],config['code']
    bpy.ops.ms_livelink.connect()
    stable('save reopen reconnect retains source and shot layers')
    assert close_matrix(hero.matrix_world,world)
    assert close_matrix(hero.matrix_basis,original_basis)
    # A root wrapper needs an identity parent: Blender ignores its parent
    # inverse otherwise. Exercise root translation and shear explicitly.
    bpy.ops.ms_livelink.shot(action='view')
    stable('source display before root detachment')
    hero.parent=None
    stable('publish root source object')
    bpy.ops.ms_livelink.shot(action='view')
    stable('shot display for root object')
    local('world',(3,2,1))
    pump('Director root placement reaches Blender',lambda: close_matrix(hero.matrix_world,Matrix.Translation((3,-1,2))))
    assert source_parent(hero) is None
    assert hero.parent.parent.get('ms_link_shot_anchor')
    select(hero)
    rt.shot.begin()
    pump('root affine gesture claim',lambda: rt.shot.gesture and rt.shot.gesture['ready'])
    shear=Matrix.Identity(4); shear[1][0]=0.2
    hero.parent.matrix_parent_inverse=shear @ hero.parent.matrix_parent_inverse
    bpy.context.view_layer.update()
    root_affine=hero.matrix_world.copy()
    rt.shot.commit()
    stable('root affine placement converges without decomposition')
    assert close_matrix(hero.matrix_world,root_affine)
    addon.unregister()
    (directory/'results.json').write_text(json.dumps({'checks':checks,'blender':bpy.app.version_string,
        'build_hash':bpy.app.build_hash.decode(),'mesh_extractions':rt.exports},indent=2))
except Exception:
    import traceback
    traceback.print_exc()
    try:
        addon.unregister()
    except Exception:
        pass
    raise
