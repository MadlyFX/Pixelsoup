"""Run with Blender --factory-startup --python; creates a disposable UI demo."""
from pathlib import Path
import sys
import bpy

repo=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(repo/'integrations/blender'))
import mediaserver_livelink as addon
def prepare():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj,do_unlink=True)
    addon.register()
    scene=bpy.context.scene
    collection=bpy.data.collections.new('Forest and Terrain'); scene.collection.children.link(collection)
    bpy.ops.mesh.primitive_cone_add(vertices=8,radius1=.35,radius2=0,depth=2)
    tree=bpy.context.view_layer.objects.active; tree.name='Tree prototype'; tree.location=(60,0,0)
    bpy.ops.mesh.primitive_cone_add(vertices=3,radius1=.35,radius2=0,depth=2)
    low=bpy.context.view_layer.objects.active; low.name='Tree authored LOD'; low.location=(65,0,0); tree.ms_link_lod=low
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
    mesh=bpy.data.meshes.new('Terrain grid'); n=32; faces=[]
    for y in range(n):
        for x in range(n):
            a=y*(n+1)+x; faces.append((a,a+1,a+n+2,a+n+1))
    mesh.from_pydata([(x,y,0) for y in range(n+1) for x in range(n+1)],[],faces); mesh.update()
    ground=bpy.data.objects.new('Terrain',mesh); collection.objects.link(ground); ground.ms_link_terrain_size=8
    scene.ms_livelink.collection=collection
    for obj in bpy.context.selected_objects: obj.select_set(False)
    ground.select_set(True); bpy.context.view_layer.objects.active=ground
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type=='VIEW_3D':
                area.spaces.active.show_region_ui=True
                area.spaces.active.region_3d.view_distance=60
                area.spaces.active.region_3d.view_location=(24,12,0)

bpy.app.timers.register(prepare,first_interval=0.5)
