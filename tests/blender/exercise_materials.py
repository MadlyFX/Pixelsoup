"""Qualified Blender -> serializer -> Director look acceptance, including baking."""
import json
from pathlib import Path
import sys
import time
import bpy

directory=Path(sys.argv[sys.argv.index('--')+1])
bpy.ops.preferences.addon_install(filepath=str(directory/'mediaserver_livelink.zip'))
bpy.utils.refresh_script_paths();sys.path.insert(0,str(directory/'scripts/addons'))
import mediaserver_livelink as addon
from mediaserver_livelink.runtime import runtime as rt
from mediaserver_livelink.extract import describe
from mediaserver_livelink import bake_color

bpy.ops.wm.read_factory_settings(use_empty=True);addon.register()
checks=[]
def pump(label):
    deadline=time.monotonic()+40
    while time.monotonic()<deadline:
        bpy.context.view_layer.update();rt.tick()
        if not rt.worker: raise AssertionError(label+': '+rt.status)
        if rt.connected and not rt.busy and not rt.pending:
            checks.append(label);print('PASS',label,flush=True);return
        time.sleep(0.02)
    raise AssertionError(label+': '+rt.status)

def latest():
    paths=list(directory.glob('commit-*.json'))
    return json.loads(max(paths,key=lambda p:int(p.stem.split('-')[-1])).read_text())

try:
    scene=bpy.context.scene
    collection=bpy.data.collections.new('Look reference');scene.collection.children.link(collection)
    bpy.ops.mesh.primitive_cube_add(size=1)
    obj=bpy.context.object;obj.name='Two materials'
    for previous in list(obj.users_collection): previous.objects.unlink(obj)
    collection.objects.link(obj)
    color=bpy.data.images.new('Checker',width=8,height=8)
    color.pixels=[0.8,0.2,0.1,1]*64;color.filepath_raw=str(directory/'color.png');color.file_format='PNG';color.save()
    # Saved FILE data matches what the worker will ingest.
    bpy.data.images.remove(color);color=bpy.data.images.load(str(directory/'color.png'))
    for index in range(2):
        material=bpy.data.materials.new('Copper' if index==0 else 'Paint');material.use_nodes=True
        shader=material.node_tree.nodes.get('Principled BSDF')
        shader.inputs['Roughness'].default_value=0.35
        if index==0:
            image=material.node_tree.nodes.new('ShaderNodeTexImage');image.image=color
            material.node_tree.links.new(image.outputs['Color'],shader.inputs['Base Color'])
            material.node_tree.links.new(image.outputs['Color'],shader.inputs['Emission Color'])
            shader.inputs['Emission Strength'].default_value=0.2
            for role,pixels in (('Normal',[0.5,0.5,1,1]),('Packed',[0.2,0.6,0.9,1])):
                data=bpy.data.images.new(role,width=8,height=8);data.colorspace_settings.name='Non-Color'
                data.pixels=pixels*64;data.filepath_raw=str(directory/(role+'.png'));data.file_format='PNG';data.save()
                bpy.data.images.remove(data);data=bpy.data.images.load(str(directory/(role+'.png')));data.colorspace_settings.name='Non-Color'
                texture=material.node_tree.nodes.new('ShaderNodeTexImage');texture.image=data
                if role=='Normal':
                    normal=material.node_tree.nodes.new('ShaderNodeNormalMap')
                    material.node_tree.links.new(texture.outputs['Color'],normal.inputs['Color'])
                    material.node_tree.links.new(normal.outputs['Normal'],shader.inputs['Normal'])
                else:
                    separate=material.node_tree.nodes.new('ShaderNodeSeparateColor')
                    material.node_tree.links.new(texture.outputs['Color'],separate.inputs['Color'])
                    material.node_tree.links.new(separate.outputs['Green'],shader.inputs['Roughness'])
                    material.node_tree.links.new(separate.outputs['Blue'],shader.inputs['Metallic'])
            material.ms_link_alpha='MASK'
            shader.inputs['Coat Weight'].default_value=0.4
        obj.data.materials.append(material)
    for polygon in obj.data.polygons: polygon.material_index=polygon.index%2
    for kind in ('SUN','POINT','SPOT','AREA'):
        light=bpy.data.lights.new(kind,kind);light.energy=2;light.color=(0.8,0.9,1)
        if kind in ('POINT','SPOT'):light.shadow_soft_size=0
        if kind=='AREA':light.shape='RECTANGLE';light.size=2;light.size_y=1
        lamp=bpy.data.objects.new(kind,light);collection.objects.link(lamp);lamp.location=(1,2,3)
    settings=scene.ms_livelink;settings.collection=collection
    pairing=json.loads((directory/'pairing.json').read_text());settings.port=pairing['port'];settings.code=pairing['code']
    bpy.context.view_layer.update();rt.start(bpy.context);pump('textured Principled and all four native light types')
    first=latest();assert first['instances']==2
    ids={o['name']:o['id'] for o in first['objects']}
    serializations=rt.serializations
    color.pixels=[0.1,0.4,0.9,1]*64;color.save();color.reload()
    rt.publish_now();pump('one saved texture changes one immutable surface')
    assert rt.serializations==serializations+1,(rt.serializations,serializations)
    assert latest()['ingested']==1
    materials=list(obj.data.materials)
    obj.data.materials[0]=materials[1];obj.data.materials[1]=materials[0]
    for polygon in obj.data.polygons:polygon.material_index=1-polygon.material_index
    rt.mark(full=True);pump('material slot reorder retains surface identity and packages')
    assert latest()['instances']==2
    assert {o['name']:o['id'] for o in latest()['objects']}==ids
    assert rt.serializations==serializations+1
    lamp=bpy.data.objects['AREA'];lamp.data.energy=5;lamp.data.size=3
    rt.publish_now();pump('light power/dimension update sends no GLB')
    assert latest()['ingested']==0
    # Explicit unsupported graph diagnostics, without replacing the last Preview.
    copper=bpy.data.materials['Copper'];shader=copper.node_tree.nodes.get('Principled BSDF')
    noise=copper.node_tree.nodes.new('ShaderNodeTexNoise')
    copper.node_tree.links.new(noise.outputs['Color'],shader.inputs['Base Color'])
    try: describe(scene,collection,rt.identities);raise AssertionError('procedural input accepted silently')
    except ValueError as error:assert 'bake procedural color' in str(error)
    checks.append('procedural base color produces an explicit baking diagnostic')
    rt.stop();settings.live=False
    # Exercise the actual isolated color baker on a single-material authoring copy.
    obj.data.materials.clear();obj.data.materials.append(copper)
    for polygon in obj.data.polygons:polygon.material_index=0
    bpy.context.view_layer.objects.active=obj;bpy.context.view_layer.update()
    bake_color.start(bpy.context)
    deadline=time.monotonic()+80
    while bake_color.job is not None and time.monotonic()<deadline:
        bake_color.poll();time.sleep(0.05)
    assert bake_color.job is None and 'Baked base color copy assigned' in rt.status,rt.status
    assert obj.active_material!=copper and copper.node_tree.nodes.get('Noise Texture') is not None
    describe(scene,collection,rt.identities)
    checks.append('worker base color bake assigns a supported copy and retains source material')
    bpy.context.view_layer.update()
    bake_color.start(bpy.context)
    obj.data.vertices[0].co.x += 0.1
    bpy.context.view_layer.update()
    deadline=time.monotonic()+80
    while bake_color.job is not None and time.monotonic()<deadline:
        bake_color.poll();time.sleep(0.05)
    assert bake_color.job is None and 'discarded' in rt.status,rt.status
    checks.append('editing while disconnected invalidates an in-flight base color bake')
    bpy.ops.wm.save_as_mainfile(filepath=str(directory/'look-reference.blend'))
    (directory/'results.json').write_text(json.dumps({'blender':bpy.app.version_string,'checks':checks,'surface_serializations':rt.serializations},indent=2))
finally:
    bake_color.cancel();rt.stop()
