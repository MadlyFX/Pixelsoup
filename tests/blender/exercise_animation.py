"""Actual installed Blender add-on + real Director animation acceptance."""
import json
from pathlib import Path
import sys
import time

import bpy

directory = Path(sys.argv[sys.argv.index('--')+1])
bpy.ops.preferences.addon_install(filepath=str(directory/'mediaserver_livelink.zip'))
bpy.ops.preferences.addon_enable(module='mediaserver_livelink')
import mediaserver_livelink as addon
from mediaserver_livelink.runtime import runtime as rt
from mediaserver_livelink import animation
pairing = json.loads((directory/'pairing.json').read_text())
checks = []


def pump(label, timeout=50):
    deadline = time.monotonic()+timeout
    while time.monotonic()<deadline:
        bpy.context.view_layer.update()
        rt.tick()
        if rt.worker is None:
            raise AssertionError(label+': '+rt.status)
        if rt.connected and not rt.busy and not rt.pending:
            assert rt.revision, rt.status
            assert 'failed' not in rt.status and 'discarded' not in rt.status, rt.status
            checks.append(label)
            return
        time.sleep(0.025)
    raise AssertionError(label+': '+rt.status)


try:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    addon.register()
    scene = bpy.context.scene
    collection = bpy.data.collections.new('Animated test')
    scene.collection.children.link(collection)
    parent = bpy.data.objects.new('Moving parent',None)
    collection.objects.link(parent)
    parent.location=(0,0,0);parent.keyframe_insert('location',frame=1)
    parent.location=(2,0,0);parent.keyframe_insert('location',frame=25)
    follower=bpy.data.objects.new('Constrained parent',None);collection.objects.link(follower)
    constraint=follower.constraints.new('COPY_LOCATION');constraint.target=parent
    bpy.ops.mesh.primitive_cube_add(size=1)
    cube=bpy.context.object;cube.name='Rigid child';cube.parent=follower
    for c in list(cube.users_collection):c.objects.unlink(cube)
    collection.objects.link(cube)
    material=bpy.data.materials.new('Animated copper');material.use_nodes=True
    material.node_tree.nodes.get('Principled BSDF').inputs['Base Color'].default_value=(0.8,0.2,0.05,1)
    cube.data.materials.append(material)
    # A skinned child proves rigs use palettes rather than evaluated static mesh.
    armature=bpy.data.armatures.new('Skeleton');rig=bpy.data.objects.new('Rig',armature)
    collection.objects.link(rig);rig.parent=parent
    bpy.context.view_layer.objects.active=rig;rig.select_set(True);cube.select_set(False)
    bpy.ops.object.mode_set(mode='EDIT')
    bone=armature.edit_bones.new('Bend');bone.head=(0,0,0);bone.tail=(0,0,1)
    bpy.ops.object.mode_set(mode='OBJECT')
    skin=cube.copy();skin.data=cube.data.copy();skin.name='Skinned child';collection.objects.link(skin)
    skin.location=(0,2,0)
    skin.parent=rig
    group=skin.vertex_groups.new(name='Bend');group.add(list(range(len(skin.data.vertices))),1,'REPLACE')
    modifier=skin.modifiers.new('Skin','ARMATURE');modifier.object=rig
    bone=rig.pose.bones['Bend'];bone.location=(0,0,0);bone.keyframe_insert('location',frame=1)
    bone.location=(0,0.5,0);bone.keyframe_insert('location',frame=25)
    scene.frame_start,scene.frame_end=1,25;scene.render.fps=24;scene.frame_set(1)
    settings=scene.ms_livelink
    settings.collection=collection;settings.port=pairing['port'];settings.code=pairing['code']
    settings.publish_animation=True;settings.animation_start=10;settings.animation_loop=True
    settings.stage_reference=True
    assert bpy.ops.ms_livelink.connect()=={'FINISHED'}
    pump('publish actual node and skin animation as one immutable assembly')
    revision=rt.revision
    for _ in range(30):
        rt.tick();time.sleep(0.025)
    assert rt.stage.latest and rt.stage.latest['header'][2:5]==[12,24,1]
    assert rt.revision==revision
    camera=next(o for o in scene.objects if o.get('ms_link_stage_reference')=='camera')
    assert abs(camera.location.z-2)<1e-5 and abs(camera.location.y+5)<1e-5
    assert camera.data.sensor_fit=='VERTICAL' and camera.hide_render
    checks.append('stage markers, camera axes and rational time arrive without source revisions')
    import copy
    saved_reference=copy.deepcopy(rt.stage.latest)
    rt.stage.latest['alignment'][12]=2
    rt.stage.apply()
    assert abs(camera.location.x+2)<1e-5
    rt.stage.latest=saved_reference;rt.stage.apply()
    assert abs(camera.location.x)<1e-5
    checks.append('stage reference is shown in Blender coordinates, without stage alignment')
    anchor=next(o for o in collection.objects if o.get(animation.TAG))
    row=next(iter(rt.writeback.rows.values()))
    (directory/'local-shot.txt').write_text(f'{row["node"]} world 3 0 0')
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        rt.tick();bpy.context.view_layer.update()
        if abs(anchor.matrix_world.translation.x-3)<1e-4:break
        time.sleep(0.025)
    assert abs(anchor.matrix_world.translation.x-3)<1e-4, rt.status
    scene.frame_set(13);bpy.context.view_layer.update()
    assert cube.matrix_world.translation.x>3
    checks.append('returned shot offset remains outside animated parent transforms')
    rt.publish_now();pump('republish retains source identity and shot placement')
    assert abs(anchor.matrix_world.translation.x-3)<1e-4
    # Scrubbing does not serialize or publish poses.
    revision=rt.revision
    for frame in (25,1,13,4):scene.frame_set(frame);rt.tick()
    assert rt.revision==revision and not rt.busy
    checks.append('source timeline seeks do not publish per-frame revisions')
    # Ground truth from Blender's evaluated rigid/skin meshes, in assembly space.
    pose_rows=[]
    for frame in (1,7,13,19,25):
        scene.frame_set(frame);bpy.context.view_layer.update()
        points=[]
        for obj in (cube,skin):
            evaluated=obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh=evaluated.to_mesh()
            try:
                transform=anchor.matrix_world.inverted() @ evaluated.matrix_world
                for vertex in mesh.vertices:
                    p=transform @ vertex.co;points.append((p.x,p.z,-p.y))
            finally:evaluated.to_mesh_clear()
        bounds=[min(p[i] for p in points) for i in range(3)]+[max(p[i] for p in points) for i in range(3)]
        pose_rows.append(' '.join(map(str,[frame,*bounds])))
    (directory/'pose-reference.txt').write_text('\n'.join(pose_rows))
    checks.append('record evaluated Blender node-constraint and skin poses for numeric renderer comparison')
    # Source changes during the worker bake must not replace the accepted clip.
    previous_revision=rt.revision
    rt.publish_now()
    deadline=time.monotonic()+5
    while animation.job is None and time.monotonic()<deadline:rt.tick();time.sleep(0.02)
    assert animation.job is not None
    cube.data.vertices[0].co.x+=0.05
    cube.data.update();bpy.context.view_layer.update()
    deadline=time.monotonic()+30
    while animation.job is not None and time.monotonic()<deadline:rt.tick();time.sleep(0.025)
    assert animation.job is None and 'discarded' in rt.status and rt.revision==previous_revision,rt.status
    checks.append('source edits discard a stale animation bake without changing the accepted revision')
    skin.data.shape_keys if skin.data.shape_keys else skin.shape_key_add(name='Basis')
    try:
        animation.validate(scene,collection)
        raise AssertionError('Morph target should be rejected')
    except ValueError as error:
        assert 'vertex caches' in str(error)
    checks.append('unsupported vertex caches rejected explicitly')
    rt.stop()
    assert animation.job is None
    checks.append('Blender disconnect leaves accepted offline project')
    (directory/'results.json').write_text(json.dumps({'blender':bpy.app.version_string,'addon':addon.bl_info['version'],'checks':checks},indent=2))
finally:
    rt.stop()
