"""Operator-requested base color baking in an isolated Blender process.

Only one-material meshes with UV0 are supported. The source material is copied;
lighting, normals, opacity and procedural geometry are not baked into albedo.
"""
import json
from pathlib import Path
import subprocess
import tempfile

import bpy

job = None
revision = 0


def invalidate():
    global revision
    if job is not None:
        revision += 1


def start(context):
    global job
    from .runtime import runtime
    if job is not None:
        raise ValueError('A base color bake is already running')
    obj = context.active_object
    if not obj or obj.type != 'MESH' or context.mode != 'OBJECT' or len(obj.material_slots) != 1 or not obj.active_material or not obj.data.uv_layers:
        raise ValueError('Select an Object-mode mesh with one material and a render UV map')
    material = obj.active_material
    if not material.use_nodes or not any(n.type == 'BSDF_PRINCIPLED' for n in material.node_tree.nodes):
        raise ValueError('Base color baking requires a Principled surface')
    directory = Path(tempfile.mkdtemp(prefix='mslink-color-'))
    config = {'object': obj.name, 'output': str(directory/'base-color.png')}
    (directory/'job.json').write_text(json.dumps(config))
    bpy.ops.wm.save_as_mainfile(filepath=str(directory/'source.blend'),copy=True)
    context.view_layer.update()
    log = (directory/'bake.log').open('w')
    process = subprocess.Popen([bpy.app.binary_path,'--background','--factory-startup',str(directory/'source.blend'),
        '--python-exit-code','1','--python',str(Path(__file__).resolve()),'--',str(directory)],
        stdout=log,stderr=subprocess.STDOUT,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    log.close()
    job = (process,directory,obj.session_uid,material.session_uid,runtime.generation,revision)
    runtime.status = 'Baking base color in a separate Blender process...'
    bpy.app.timers.register(poll,first_interval=0.25)


def poll():
    global job
    from .runtime import runtime
    if job is None:
        return None
    process,directory,object_id,material_id,generation,snapshot_revision = job
    if process.poll() is None:
        return 0.25
    job = None
    obj = next((o for o in bpy.data.objects if o.session_uid == object_id),None)
    if process.returncode or not (directory/'base-color.png').is_file():
        runtime.status = 'Base color bake failed; see '+str(directory/'bake.log')
    elif revision != snapshot_revision or runtime.generation != generation or not obj or not obj.active_material or obj.active_material.session_uid != material_id:
        runtime.status = 'Base color bake discarded: scene changed while baking; retry'
    else:
        material = obj.active_material.copy()
        material.name += ' (baked base color)'
        if 'ms_link_id' in material:
            del material['ms_link_id']
        shader = next(n for n in material.node_tree.nodes if n.type=='BSDF_PRINCIPLED')
        image = bpy.data.images.load(str(directory/'base-color.png'),check_existing=False)
        image.colorspace_settings.name = 'sRGB'
        node = material.node_tree.nodes.new('ShaderNodeTexImage'); node.image=image
        material.node_tree.links.new(node.outputs['Color'],shader.inputs['Base Color'])
        obj.material_slots[0].material = material
        runtime.mark(full=True)
        runtime.status = 'Baked base color copy assigned. Save the PNG with your project, then Publish Now.'
    return None


def cancel():
    global job
    if job:
        if job[0].poll() is None:
            job[0].terminate()
        job = None
    if bpy.app.timers.is_registered(poll):
        bpy.app.timers.unregister(poll)


def worker(directory):
    config = json.loads((directory/'job.json').read_text())
    obj = bpy.data.objects[config['object']]
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True); bpy.context.view_layer.objects.active=obj
    material = obj.active_material.copy(); obj.material_slots[0].material=material
    shader = next(n for n in material.node_tree.nodes if n.type=='BSDF_PRINCIPLED')
    output = next(n for n in material.node_tree.nodes if n.type=='OUTPUT_MATERIAL' and n.is_active_output)
    emission = material.node_tree.nodes.new('ShaderNodeEmission')
    color = shader.inputs['Base Color']
    if color.is_linked:
        material.node_tree.links.new(color.links[0].from_socket,emission.inputs['Color'])
    else:
        emission.inputs['Color'].default_value=color.default_value
    material.node_tree.links.new(emission.outputs[0],output.inputs['Surface'])
    image = bpy.data.images.new('Baked base color',width=1024,height=1024,alpha=False)
    image.colorspace_settings.name='sRGB'
    node = material.node_tree.nodes.new('ShaderNodeTexImage'); node.image=image
    material.node_tree.nodes.active=node
    bpy.context.scene.render.engine='CYCLES'; bpy.context.scene.cycles.device='CPU'
    bpy.context.scene.cycles.samples=16
    bpy.context.scene.render.bake.margin=8
    bpy.ops.object.bake(type='EMIT')
    image.filepath_raw=config['output']; image.file_format='PNG'; image.save()


if __name__ == '__main__':
    import sys
    worker(Path(sys.argv[sys.argv.index('--')+1]))
