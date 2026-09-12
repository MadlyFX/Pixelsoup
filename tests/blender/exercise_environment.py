"""Real Blender Geometry Nodes scatter + terrain -> paired C++ Director."""
import json
from pathlib import Path
import sys
import time
import bpy

directory=Path(sys.argv[sys.argv.index('--')+1])
assert bpy.ops.preferences.addon_install(filepath=str(directory/'mediaserver_livelink.zip'))=={'FINISHED'}
bpy.utils.refresh_script_paths()
sys.path.insert(0,str(directory/'scripts/addons'))
import mediaserver_livelink as addon
from mediaserver_livelink.runtime import runtime as rt

checks=[]
def pump(label):
    start=time.monotonic()
    while time.monotonic()-start < 45:
        bpy.context.view_layer.update(); rt.tick()
        if rt.worker is None:
            raise AssertionError(label+': '+rt.status)
        if rt.connected and not rt.busy and not rt.pending:
            checks.append(label); print('PASS',label,rt.status,flush=True); return
        time.sleep(.02)
    raise AssertionError(label+': timeout '+rt.status)

def latest():
    return json.loads(max(directory.glob('commit-*.json'),key=lambda p:int(p.stem.split('-')[-1])).read_text())

try:
    bpy.ops.wm.read_factory_settings(use_empty=True); addon.register()
    scene=bpy.context.scene
    collection=bpy.data.collections.new('Forest and Terrain'); scene.collection.children.link(collection)
    bpy.ops.mesh.primitive_cone_add(vertices=8,radius1=.35,radius2=0,depth=2,location=(0,0,1))
    tree=bpy.context.object; tree.name='Tree prototype'; tree.location=(60,0,0)
    bpy.ops.mesh.primitive_cone_add(vertices=3,radius1=.35,radius2=0,depth=2)
    low=bpy.context.object; low.name='Tree authored LOD'; low.location=(65,0,0)
    tree.ms_link_lod=low
    points=bpy.data.meshes.new('Candidate points')
    points.from_pydata([(x*.8,y*.8,1) for y in range(32) for x in range(64)],[],[])
    emitter=bpy.data.objects.new('Forest',points); collection.objects.link(emitter)
    group=bpy.data.node_groups.new('Deterministic forest','GeometryNodeTree')
    group.interface.new_socket(name='Geometry',in_out='INPUT',socket_type='NodeSocketGeometry')
    group.interface.new_socket(name='Geometry',in_out='OUTPUT',socket_type='NodeSocketGeometry')
    inp=group.nodes.new('NodeGroupInput'); output=group.nodes.new('NodeGroupOutput')
    info=group.nodes.new('GeometryNodeObjectInfo'); info.inputs['Object'].default_value=tree
    info.transform_space='ORIGINAL'; info.inputs['As Instance'].default_value=True
    instances=group.nodes.new('GeometryNodeInstanceOnPoints')
    group.links.new(inp.outputs['Geometry'],instances.inputs['Points'])
    group.links.new(info.outputs['Geometry'],instances.inputs['Instance'])
    group.links.new(instances.outputs['Instances'],output.inputs['Geometry'])
    emitter.modifiers.new('Scatter','NODES').node_group=group
    mesh=bpy.data.meshes.new('Terrain grid')
    n=32; faces=[]
    for y in range(n):
        for x in range(n):
            a=y*(n+1)+x; faces.append((a,a+1,a+n+2,a+n+1))
    mesh.from_pydata([(x,y,0) for y in range(n+1) for x in range(n+1)],[],faces); mesh.update()
    ground=bpy.data.objects.new('Terrain',mesh); collection.objects.link(ground); ground.ms_link_terrain_size=8
    pairing=json.loads((directory/'pairing.json').read_text())
    scene.ms_livelink.collection=collection; scene.ms_livelink.port=pairing['port']; scene.ms_livelink.code=pairing['code']
    bpy.context.view_layer.update(); rt.start(bpy.context); pump('2048 evaluated shared trees and spatial terrain')
    first=latest(); assert first['instances']==2064,first
    assert len(first['objects'])==19,first
    assert first['ingested']==18,first
    assert tree.data.users==1,'Prototype sharing does not create realized meshes'
    from mediaserver_livelink.extract import describe, Identities
    hidden_before=emitter.hide_render
    emitter.hide_render=True
    hidden=describe(scene,collection,Identities())
    assert all(not o['visible'] for o in hidden if o.get('batch'))
    emitter.hide_render=hidden_before
    nested=bpy.data.node_groups.new('Viewport-dependent subgroup','GeometryNodeTree')
    nested.nodes.new('GeometryNodeIsViewport')
    node=group.nodes.new('GeometryNodeGroup'); node.node_tree=nested
    try:
        describe(scene,collection,Identities())
        raise AssertionError('Nested viewport-dependent generator must be refused')
    except ValueError as error:
        assert 'Is Viewport' in str(error),error
    group.nodes.remove(node)
    checks.append('hidden groups inherit visibility; nested viewport dependence is diagnosed')
    scene.ms_livelink.export_density=50; pump('export density independent of viewport candidates')
    assert 900<latest()['instances']<1200
    assert sum(i.is_instance for i in bpy.context.evaluated_depsgraph_get().object_instances)==2048
    count=rt.serializations
    scene.ms_livelink.density_seed=4; pump('deterministic density seed update')
    assert rt.serializations==count
    ground.data.vertices[3*33+3].co.z=1; ground.data.update(); rt.mark(geometry=(ground.data.session_uid,))
    pump('repaint one terrain region reuses unaffected resources')
    repaint=latest(); assert repaint['ingested']==1 and repaint['reused']>=17,repaint
    assert rt.serializations==count+1
    points.vertices[0].co.x+=.2; points.update(); rt.mark(geometry=(points.session_uid,))
    pump('scatter regeneration retains group identity and shared prototypes')
    assert latest()['ingested']==0
    scene.ms_livelink.export_density=0; pump('zero density removes batch explicitly')
    assert latest()['instances']==16
    scene.ms_livelink.export_density=100; pump('restoring density restores stable group')
    assert latest()['instances']==2064
    (directory/'results.json').write_text(json.dumps({'blender':bpy.app.version_string,'checks':checks,
        'objects':len(latest()['objects']),'instances':latest()['instances'],'serializations':rt.serializations,
        'last_extract_ms':rt.last_extract_ms},indent=2))
finally:
    rt.stop(); addon.unregister()
