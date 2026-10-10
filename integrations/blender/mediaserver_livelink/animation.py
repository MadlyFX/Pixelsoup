"""Explicit immutable node/skin clip export on a separate Blender process."""
import json
import math
from pathlib import Path
import subprocess
import tempfile
import time
import uuid

import bpy

TAG = 'ms_link_animation_anchor'
job = None
revision = 0


def invalidate():
    global revision
    if job is not None:
        revision += 1


def assembly_id(collection_id):
    return str(uuid.uuid5(uuid.UUID(collection_id), 'animation-assembly-v1'))


def action_curves(action):
    if action:
        for layer in action.layers:
            for strip in layer.strips:
                for bag in getattr(strip,'channelbags',[]):
                    yield from bag.fcurves


def members(collection):
    from .stage import TAG as STAGE
    return [o for o in collection.all_objects if not any(o.get(tag) for tag in (TAG,STAGE))]


def validate(scene, collection):
    from .materials import describe_material
    from .runtime import runtime
    objects = members(collection)
    if not objects or len(objects) > 64 or not any(o.type == 'MESH' for o in objects):
        raise ValueError('Animation assembly requires meshes and at most 64 objects')
    if not 1 <= scene.frame_end-scene.frame_start <= 2400:
        raise ValueError('Animation playback range requires 2..2401 sampled frames')
    if abs(scene.unit_settings.scale_length-1) > 1e-8:
        raise ValueError('Animation assembly currently requires scene unit scale 1 (metres)')
    if not math.isfinite(scene.render.fps_base) or scene.render.fps_base <= 0:
        raise ValueError('Animation requires a finite positive source frame rate')
    materials = {}
    nodes = len(objects)
    for obj in objects:
        if obj.type not in ('MESH','EMPTY','ARMATURE') or obj.instance_type != 'NONE':
            raise ValueError(obj.name + ': animation assemblies support mesh, empty and armature nodes; publish lights/scatter separately')
        if obj.name not in bpy.context.view_layer.objects or obj.hide_render or obj.hide_viewport:
            raise ValueError(obj.name + ': animation assembly members must be visible in the active view layer')
        if obj.parent and obj.parent not in objects and not (obj.parent.get(TAG) and obj.parent.name in collection.all_objects):
            raise ValueError(obj.name + ': include the entire animated parent hierarchy in the collection')
        if obj.rigid_body or obj.particle_systems or (obj.type == 'MESH' and obj.data.shape_keys):
            raise ValueError(obj.name + ': simulations and morph/vertex caches are unsupported; publish node or skin animation')
        if obj.type == 'MESH' and any(m.type != 'ARMATURE' for m in obj.modifiers):
            raise ValueError(obj.name + ': apply mesh modifiers before animation export; only Armature deformation is supported')
        if obj.type == 'MESH' and obj.data.animation_data:
            raise ValueError(obj.name + ': animated mesh data requires unsupported vertex caches')
        if obj.type == 'MESH' and obj.modifiers:
            if len(obj.modifiers) != 1 or obj.modifiers[0].object not in objects:
                raise ValueError(obj.name + ': skin requires one armature inside the assembly')
            modifier=obj.modifiers[0]
            if modifier.use_deform_preserve_volume or modifier.use_bone_envelopes or not modifier.use_vertex_groups or not modifier.show_render or not modifier.show_viewport:
                raise ValueError(obj.name + ': skin requires linear vertex-group deformation with matching viewport/render visibility')
            bones = obj.modifiers[0].object.data.bones
            bone_groups = {g.index for g in obj.vertex_groups if g.name in bones}
            if any(sum(g.weight>0 for g in v.groups if g.group in bone_groups)>4 for v in obj.data.vertices):
                raise ValueError(obj.name + ': skin exceeds four bone influences per vertex')
            if obj.parent != obj.modifiers[0].object:
                raise ValueError(obj.name + ': parent a skinned mesh to its armature before publishing')
        if obj.type == 'ARMATURE':
            nodes += len(obj.data.bones)
            for bone in obj.pose.bones:
                if bone.bone.bbone_segments != 1:
                    raise ValueError(obj.name + ': B-Bone segments require an unsupported deformation cache')
        for block in (obj, obj.data):
            if block and getattr(block,'animation_data',None) and block.animation_data.drivers:
                raise ValueError(obj.name + ': scripted drivers are unsupported in published animation')
        if obj.animation_data:
            actions = [obj.animation_data.action]
            actions.extend(strip.action for track in obj.animation_data.nla_tracks for strip in track.strips)
            for action in actions:
                for curve in action_curves(action):
                    path=curve.data_path
                    if not (path in ('location','rotation_euler','rotation_quaternion','rotation_axis_angle','scale')
                            or path.startswith('pose.bones[') or path.startswith('constraints[')):
                        raise ValueError(obj.name+': animation channel '+path+' is outside the node/skin cache contract')
        for slot in obj.material_slots:
            if slot.material:
                material = slot.material
                if material.animation_data or (material.node_tree and material.node_tree.animation_data):
                    raise ValueError(material.name + ': animated material properties are unsupported')
                materials[material.name] = describe_material(material,runtime.identities,scene)
    if nodes > 128 or nodes * (scene.frame_end-scene.frame_start+1) * 3 > 1_000_000:
        raise ValueError('Animation exceeds 128 nodes / one million channel keys')
    return objects, materials


def ensure_anchor(collection, identity):
    from mathutils import Matrix
    anchor = next((o for o in collection.objects if o.get(TAG)),None)
    if anchor is None:
        anchor = bpy.data.objects.new('Animation assembly placement',None)
        anchor[TAG] = True
        anchor['ms_link_id'] = identity
        collection.objects.link(anchor)
    if anchor.get('ms_link_id') != identity:
        anchor['ms_link_id'] = identity
    for obj in members(collection):
        if obj.parent is None:
            # Identity parent leaves all animated local channels untouched.
            basis = obj.matrix_basis.copy()
            obj.parent = anchor
            obj.matrix_parent_inverse = Matrix.Identity(4)
            obj.matrix_basis = basis
    return anchor


def start(context):
    global job
    from .runtime import runtime
    if job is not None:
        raise ValueError('Animation export is already running')
    scene = context.scene
    collection = scene.ms_livelink.collection
    objects, materials = validate(scene,collection)
    identity = assembly_id(runtime.collection_id)
    ensure_anchor(collection,identity)
    directory = Path(tempfile.mkdtemp(prefix='mslink-animation-'))
    config = {'collection':collection.name,'objects':[o.name for o in objects], 'materials':materials,
              'first':scene.frame_start,'last':scene.frame_end}
    (directory/'job.json').write_text(json.dumps(config,allow_nan=False))
    bpy.ops.wm.save_as_mainfile(filepath=str(directory/'source.blend'),copy=True)
    context.view_layer.update()
    with (directory/'export.log').open('w') as log:
        process = subprocess.Popen([bpy.app.binary_path,'--background','--factory-startup',str(directory/'source.blend'),
            '--python-exit-code','1','--python',str(Path(__file__).resolve()),'--',str(directory)],
            stdout=log,stderr=subprocess.STDOUT,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    assembly = {'id':identity,'name':collection.name+' (animated assembly)','parent':'',
        'prototype':str(uuid.uuid5(uuid.UUID(identity),'node-skin-v1')),'visible':True,
        'matrix':[float(i%5==0) for i in range(16)],'materials':[],
        'animation':{'start':scene.ms_livelink.animation_start,'loop':scene.ms_livelink.animation_loop}}
    job = (process,directory,revision,runtime.worker,assembly)
    runtime.busy = True
    runtime.pending = False
    runtime.requested = False
    runtime.status = 'Baking node/skin animation on a separate Blender process...'


def poll():
    global job
    from .runtime import runtime
    if job is None or job[0].poll() is None:
        return
    process,directory,snapshot_revision,owner,assembly = job
    job = None
    if owner is not runtime.worker:
        return
    runtime.busy = False
    if not runtime.requested:
        runtime.pending = False
    if process.returncode or not (directory/'assembly.glb').is_file():
        runtime.status = 'Animation export failed; see '+str(directory/'export.log')
    elif revision != snapshot_revision:
        runtime.status = 'Animation export discarded: source changed while baking; Publish Now to retry'
    else:
        runtime.worker.submit({'source':runtime.source,'animated_package':str(directory/'assembly.glb'),'assembly':assembly})
        runtime.submitted_at = time.monotonic()
        runtime.busy = True
        runtime.status = 'Publishing immutable animated assembly...'


def cancel():
    global job
    if job:
        if job[0].poll() is None:
            job[0].terminate()
        job = None


def worker(directory):
    import struct
    import sys
    sys.path.insert(0,str(Path(__file__).parent))
    from surfaces import resolve_materials, gltf_material
    config = json.loads((directory/'job.json').read_text())
    scene = bpy.context.scene
    # Remove only the managed display parent; source node transforms and rig
    # hierarchy remain in GLB. The parent inverse contains the shot, never a key.
    bpy.ops.object.select_all(action='DESELECT')
    for name in config['objects']:
        obj = bpy.data.objects[name]
        if obj.parent and obj.parent.get(TAG):
            basis = obj.matrix_basis.copy()
            obj.parent = None
            obj.matrix_basis = basis
        obj.select_set(True)
    scene.frame_start,scene.frame_end = config['first'],config['last']
    # Sampling transform constraints is supported; deformations, singularities
    # and affine shear cannot be silently approximated by the glTF TRS cache.
    from mathutils import Matrix
    for frame in range(scene.frame_start,scene.frame_end+1):
        scene.frame_set(frame)
        depsgraph=bpy.context.evaluated_depsgraph_get()
        for name in config['objects']:
            obj=bpy.data.objects[name].evaluated_get(depsgraph)
            local=obj.parent.matrix_world.inverted() @ obj.matrix_world if obj.parent else obj.matrix_world
            transforms=[local]
            if obj.type=='ARMATURE':
                transforms.extend(b.parent.matrix.inverted() @ b.matrix if b.parent else b.matrix for b in obj.pose.bones)
            for transform in transforms:
                location,rotation,scale=transform.decompose()
                reconstructed=Matrix.LocRotScale(location,rotation,scale)
                if min(scale)<=1e-6 or max(abs(transform[r][c]-reconstructed[r][c]) for r in range(4) for c in range(4))>1e-5:
                    raise ValueError(name+': sampled node/bone transform has reflection, singular scale or shear')
    scene.frame_set(scene.frame_start)
    result = bpy.ops.export_scene.gltf(filepath=str(directory/'raw.glb'),export_format='GLB',use_selection=True,
        export_animations=True,export_animation_mode='SCENE',export_anim_scene_split_object=False,
        export_frame_range=True,export_frame_step=1,export_force_sampling=True,export_bake_animation=True,
        export_anim_slide_to_zero=True,export_current_frame=True,export_rest_position_armature=True,
        export_skins=True,export_morph=False,export_cameras=False,export_lights=False,export_apply=False,
        export_yup=True,export_extras=False,export_image_format='NONE',export_sampling_interpolation_fallback='LINEAR')
    if result != {'FINISHED'}:
        raise ValueError('Blender glTF animation export did not finish')
    raw = (directory/'raw.glb').read_bytes()
    size, = struct.unpack_from('<I',raw,12)
    doc = json.loads(raw[20:20+size])
    binary = bytearray(raw[28+size:])
    if len(doc.get('animations',[])) != 1:
        raise ValueError('Assembly must export exactly one sampled scene animation')
    resolved = resolve_materials([{'materials':list(config['materials'].values())}],refresh=True)[0]['materials']
    by_name = dict(zip(config['materials'],resolved))
    old_materials = doc.pop('materials',[])
    for key in ('images','textures','samplers','extensionsUsed','extensionsRequired'):
        doc.pop(key,None)
    doc['materials'] = [gltf_material(by_name[m['name']],doc,binary) for m in old_materials]
    # Always use the explicit sampled cache interpolation contract.
    for clip in doc['animations']:
        for sampler in clip['samplers']:
            sampler['interpolation'] = 'LINEAR'
    doc['buffers'] = [{'byteLength':len(binary)}]
    text = json.dumps(doc,separators=(',',':'),allow_nan=False).encode()
    text += b' '*(-len(text)%4)
    binary.extend(b'\0'*(-len(binary)%4))
    data = struct.pack('<4sII',b'glTF',2,28+len(text)+len(binary))+struct.pack('<I4s',len(text),b'JSON')+text+struct.pack('<I4s',len(binary),b'BIN\0')+binary
    if len(data) > 64*1024*1024:
        raise ValueError('Animation assembly exceeds 64 MiB')
    (directory/'assembly.glb').write_bytes(data)


if __name__ == '__main__':
    import sys
    worker(Path(sys.argv[sys.argv.index('--')+1]))
