"""All bpy access lives on Blender's main thread; returned values own their bytes."""
from array import array
import json
import math
import uuid

import bpy
from mathutils import Matrix

from .translation import warn, geometry_error
from .geometry import MAX_MESH_BYTES, MAX_RESOURCES, pack_array
from .placement import source_parent, source_local, source_visible

PIN = (5, 2, 0)
BUILD_HASH = 'fbe6228777e7'
ID_KEY = 'ms_link_id'
DEFAULT_MATERIAL = [0.8, 0.8, 0.8, 0.5, 0.0, 0.0, 0.0, 0.0]
CONVERSION = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, 0, 0, 1)))


def qualified():
    if bpy.app.version < (5, 2, 0):
        raise ValueError('Media Server LiveLink requires Blender 5.2 or newer')


class Identities:
    def __init__(self):
        # Scalar session identifiers survive rename; never retain RNA wrappers.
        self.owners = {}
        self.sessions = {}

    def identity(self, block, scene):
        kind = block.bl_rna.identifier
        session = (kind, block.session_uid)
        if session not in self.sessions and len(self.sessions) >= 8192:
            raise ValueError('Identity session capacity reached; save and reopen the blend file')
        value = block.get(ID_KEY, self.sessions.get(session, ''))
        try:
            valid = str(uuid.UUID(value)) == value and uuid.UUID(value).int != 0
        except (ValueError, TypeError, AttributeError):
            valid = False
        if block.library and not block.override_library:
            # Explicit mapping for read-only library IDs without authored UUIDs.
            mapping = json.loads(scene.get('ms_link_library_ids', '{}'))
            key = '|'.join((block.library.filepath, kind, block.name_full))
            if not valid:
                if key not in mapping and len(mapping) >= 4096:
                    raise ValueError('Read-only identity mapping exceeds 4096 records')
                value = mapping.setdefault(key, str(uuid.uuid4()))
                scene['ms_link_library_ids'] = json.dumps(mapping, sort_keys=True)
            owner = self.owners.get((kind, value))
            if owner is not None and owner != block.session_uid:
                raise ValueError(block.name + ': duplicate read-only UUID; repair the ID in its library')
            self.owners[(kind, value)] = block.session_uid
            self.sessions[session] = value
            return value
        owner = self.owners.get((kind, value))
        if not valid or (owner is not None and owner != block.session_uid):
            value = self.sessions.get(session, str(uuid.uuid4()))
        if block.get(ID_KEY) != value:
            block[ID_KEY] = value
        self.owners[(kind, value)] = block.session_uid
        self.sessions[session] = value
        return value


def collection_objects(collection):
    objects = {}
    for obj in collection.all_objects:
        if obj.get('ms_link_stage_reference') or obj.get('ms_link_animation_anchor'):
            continue
        if obj.name not in bpy.context.view_layer.objects:
            warn(obj.name + ': excluded from the active view layer; skipped')
            continue
        if obj.type not in ('MESH', 'EMPTY', 'LIGHT', 'CURVE', 'SURFACE', 'FONT', 'META'):
            warn(obj.name + ': ' + obj.type.lower() + ' ignored')
            continue
        objects[obj.name] = True
        if len(objects) > 512:
            raise ValueError('Collection exceeds 512 objects including parents')
        parent = source_parent(obj)
        while parent:
            objects.setdefault(parent.name, False)
            if len(objects) > 512:
                raise ValueError('Collection exceeds 512 objects including parents')
            parent = source_parent(parent)
    return objects


def material_factors(material, mesh=None):
    from .materials import describe_material
    from .runtime import runtime
    uv = next((layer.name for layer in getattr(mesh, 'uv_layers', ()) if layer.active_render), '')
    return describe_material(material, runtime.identities, bpy.context.scene, uv)


def validate_generator(obj):
    if obj.particle_systems:
        raise ValueError(obj.name + ': particle/hair output is not qualified; bake or use mesh-object Geometry Nodes instances')
    visited = set()
    def inspect(group):
        if group is None or group.session_uid in visited:
            return
        visited.add(group.session_uid)
        if len(visited) > 128:
            raise ValueError(obj.name + ': generator exceeds 128 nested node groups')
        for node in group.nodes:
            if node.bl_idname == 'GeometryNodeIsViewport':
                raise ValueError(obj.name + ': Is Viewport output is unsupported; publish a deterministic candidate set')
            if node.bl_idname in ('GeometryNodeInputSceneTime','GeometryNodeSimulationInput','GeometryNodeSimulationOutput'):
                raise ValueError(obj.name + ': time-dependent geometry/simulation needs an unsupported vertex cache; publish node or skin animation')
            if node.type == 'GROUP':
                inspect(node.node_tree)
    for modifier in obj.modifiers:
        if modifier.show_viewport != modifier.show_render:
            raise ValueError(obj.name + ': match modifier viewport/render switches for deterministic export')
        if modifier.type == 'NODES':
            inspect(modifier.node_group)


def hidden_instances(obj, depsgraph):
    """The instances `obj` makes while hidden, as (prototype, local matrix).

    The depsgraph's instance iterator skips hidden objects, and hiding one is
    how Media Server's visibility override shows in Blender. Its evaluated
    geometry still holds the instances, so a hidden instancer keeps its scatter
    (sent hidden) instead of losing it as though deleted."""
    result = []

    def collection_members(collection, matrix, depth):
        offset = Matrix.Translation(-collection.instance_offset)
        for member in collection.all_objects:
            emit(member, matrix @ offset @ member.matrix_world, depth)

    def emit(prototype, matrix, depth):
        if depth > 8:
            return
        if prototype.instance_type == 'COLLECTION' and prototype.instance_collection:
            collection_members(prototype.instance_collection, matrix, depth + 1)
        else:
            result.append((prototype, matrix))

    def instances(geometry, matrix, depth):
        points = geometry.instances_pointcloud()
        if points is None or depth > 8:
            return
        references = geometry.instance_references()
        indices = points.attributes['.reference_index'].data
        transforms = points.attributes['instance_transform'].data
        for index in range(len(transforms)):
            reference = references[indices[index].value]
            placed = matrix @ Matrix(transforms[index].value)
            if isinstance(reference, bpy.types.Collection):
                collection_members(reference.original, placed, depth + 1)
            elif isinstance(reference, bpy.types.Object):
                emit(reference.original, placed, depth + 1)
            else:
                # Object and collection references come wrapped in a geometry
                # set of one instance. Plain geometry is named by the iterator
                # after the instancer itself.
                if any((reference.mesh, reference.curves, reference.pointcloud, reference.volume,
                        reference.grease_pencil)):
                    result.append((obj, placed))
                instances(reference, placed, depth + 1)

    if obj.instance_type == 'COLLECTION' and obj.instance_collection:
        collection_members(obj.instance_collection, Matrix.Identity(4), 0)
    else:
        try:
            instances(obj.evaluated_get(depsgraph).evaluated_geometry(), Matrix.Identity(4), 0)
        except RuntimeError:
            # Disabled in viewports: not evaluated at all.
            pass
    return result


def describe(scene, collection, identities):
    names = collection_objects(collection)
    render_visible = set()
    pending_collections = [(collection, not collection.hide_render and not collection.hide_viewport)]
    visited = set()
    while pending_collections:
        child, visible = pending_collections.pop()
        key = (child.session_uid, visible)
        if key in visited:
            continue
        visited.add(key)
        if len(visited) > 1024:
            raise ValueError('Collection hierarchy exceeds 1024 entries')
        if visible:
            render_visible.update(obj.name for obj in child.objects)
        pending_collections.extend((item, visible and not item.hide_render and not item.hide_viewport) for item in child.children)
    depsgraph = bpy.context.evaluated_depsgraph_get()
    scatter = {}

    def scattered(parent, prototype, matrix):
        if prototype.type != 'MESH' or prototype.name not in bpy.data.objects:
            warn(parent.name + ': unsupported procedural instance skipped')
            return
        scatter.setdefault((parent.name,prototype.name),[]).append(matrix.copy())

    instancers = set()
    for count, instance in enumerate(depsgraph.object_instances):
        if count >= 65536:
            raise ValueError('View layer instance enumeration exceeds 65536 entries; isolate the linked collection')
        if instance.is_instance and instance.parent and names.get(instance.parent.original.name):
            parent = instance.parent.original
            instancers.add(parent.name)
            scattered(parent, instance.object.original, parent.matrix_world.inverted() @ instance.matrix_world)
    count = 0
    for name, member in names.items():
        obj = bpy.data.objects[name]
        if not member or name in instancers or obj.visible_get():
            continue
        for prototype, matrix in hidden_instances(obj, depsgraph):
            count += 1
            if count >= 65536:
                raise ValueError('Hidden instance enumeration exceeds 65536 entries; isolate the linked collection')
            scattered(obj, prototype, matrix)
    ids = {name: identities.identity(bpy.data.objects[name], scene) for name in names}
    # Media Server's edits are shown in these objects; what was applied comes
    # off again here, so the source sent stays Blender's own.
    from .runtime import runtime
    writeback = runtime.writeback
    writeback.load({ids[name]: bpy.data.objects[name] for name in names})
    scale = scene.unit_settings.scale_length
    if not math.isfinite(scale) or scale <= 0:
        raise ValueError('Scene units must have a finite positive scale')
    conversion = CONVERSION @ Matrix.Diagonal((scale, scale, scale, 1))
    inverse = conversion.inverted()
    objects = []
    for name, member in names.items():
        obj = bpy.data.objects[name]
        writeback.restored(ids[name])
        if obj.animation_data or obj.constraints or (obj.data and getattr(obj.data, 'animation_data', None)):
            warn(name + ': syncing the evaluated current pose')
        if obj.data and isinstance(obj.data, bpy.types.Mesh):
            identities.identity(obj.data, scene)
        mesh = member and obj.type in ('MESH', 'CURVE', 'SURFACE', 'FONT', 'META')
        if mesh and obj.type == 'MESH' and not len(obj.evaluated_get(depsgraph).data.polygons) and (any(k[0] == name for k in scatter)
                or any(m.type == 'NODES' for m in obj.modifiers)):
            mesh = False
        world = obj.matrix_world
        if mesh and world.to_3x3().determinant() <= 1e-10:
            geometry_error(name + ': singular or reflected placement; skipped geometry')
            mesh = False
        try:
            if obj.constraints or obj.animation_data:
                parent = source_parent(obj)
                world_pose = obj.evaluated_get(depsgraph).matrix_world
                local = parent.evaluated_get(depsgraph).matrix_world.inverted_safe() @ world_pose if parent else world_pose
            else:
                local = source_local(obj)
        except ValueError as error:
            geometry_error(name + ': singular parent placement; skipped geometry')
            local = Matrix.Identity(4)
            mesh = False
        matrix = conversion @ local @ inverse
        if not (obj.constraints or obj.animation_data):
            matrix = writeback.placement(obj, ids[name], matrix)
        if not all(math.isfinite(v) for row in matrix for v in row) or abs(matrix.to_3x3().determinant()) < 1e-10:
            geometry_error(name + ': invalid placement; skipped geometry')
            matrix = Matrix.Identity(4)
            mesh = False
        materials = []
        if mesh:
            for slot in obj.material_slots:
                if slot.material:
                    identities.identity(slot.material, scene)
                materials.append(material_factors(slot.material, obj.data))
        parent = source_parent(obj)
        objects.append({'id': ids[name], 'name': name, 'parent': ids.get(parent.name, '') if parent else '',
                        'prototype': str(uuid.uuid5(uuid.UUID(ids[name]), 'evaluated-mesh-v1')) if mesh else '',
                        'visible': not member or (name in render_visible and writeback.visible(obj, ids[name], source_visible(obj))),
                        'matrix': [float(matrix[r][c]) for c in range(4) for r in range(4)],
                        'materials': materials or [list(DEFAULT_MATERIAL)],
                        'session': obj.session_uid, 'data_session': obj.data.session_uid if mesh else 0,
                        'terrain_size': obj.ms_link_terrain_size if mesh else 0})
        if member and obj.type == 'LIGHT':
            from .materials import describe_light
            light = describe_light(obj, scale, scene.ms_livelink.luminous_efficacy)
            if light:
                light['values'] = writeback.light_values(ids[name], light['values'])
                objects[-1]['light'] = light
    from .scale import batch_bytes
    for (parent_name,prototype_name), matrices in sorted(scatter.items()):
        parent_id = ids[parent_name]
        prototype = bpy.data.objects[prototype_name]
        prototype_id = identities.identity(prototype,scene)
        identity = str(uuid.uuid5(uuid.UUID(parent_id),'scatter-v1:'+prototype_id))
        transformed = [conversion @ m @ inverse for m in matrices]
        for m in transformed:
            if m.to_3x3().determinant() <= 1e-10:
                raise ValueError(parent_name + ': reflected/singular scatter is not qualified')
        packed = batch_bytes([[float(m[r][c]) for c in range(4) for r in range(4)] for m in transformed],
                             scene.ms_livelink.export_density,scene.ms_livelink.density_seed)
        if not packed:
            continue
        objects.append({'id':identity,'name':parent_name+' / '+prototype_name,'mesh_name':prototype_name,
            'parent':parent_id,'prototype':str(uuid.uuid5(uuid.UUID(prototype_id),'evaluated-mesh-v1')),
            'visible':next(o['visible'] for o in objects if o['id']==parent_id),
            'matrix':[float(c == r) for c in range(4) for r in range(4)],
            'materials':[material_factors(s.material, prototype.data) for s in prototype.material_slots] or [list(DEFAULT_MATERIAL)],
            'session':prototype.session_uid,'data_session':prototype.data.session_uid,'batch':packed})
    for obj in list(objects):
        if not obj['prototype']:
            continue
        source = bpy.data.objects[obj.get('mesh_name',obj['name'])]
        low = source.ms_link_lod
        if low:
            if low.type != 'MESH' or low == source or low.ms_link_lod:
                raise ValueError(source.name + ': select one mesh LOD without a nested LOD')
            low_id = str(uuid.uuid5(uuid.UUID(obj['id']),'authored-lod-v1'))
            obj['lod_id'],obj['lod_pixels'] = low_id,source.ms_link_lod_pixels
            objects.append(dict(obj,id=low_id,name=low.name,mesh_name=low.name,
                prototype=str(uuid.uuid5(uuid.UUID(low_id),'mesh')),session=low.session_uid,
                data_session=low.data.session_uid,resource_only=True,lod_id='',terrain_size=0,batch='',
                materials=[material_factors(s.material, low.data) for s in low.material_slots] or [list(DEFAULT_MATERIAL)]))
    if sum(bool(obj['prototype']) for obj in objects) > MAX_RESOURCES:
        raise ValueError('Collection exceeds ' + str(MAX_RESOURCES) + ' evaluated prototypes')
    by_id = {obj['id']: obj for obj in objects}
    for obj in objects:
        parent = by_id.get(obj['parent'])
        if obj['visible'] and parent and not parent['visible']:
            obj['visible'] = False
    return objects


def copy_mesh(name, scale):
    obj = bpy.data.objects.get(name)
    if obj is None:
        raise ValueError('Object removed during extraction; retry pending')
    if obj.mode == 'EDIT':
        obj.update_from_editmode()
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    try:
        mesh = evaluated.to_mesh(preserve_all_data_layers=True, depsgraph=bpy.context.evaluated_depsgraph_get())
        if mesh is None or not len(mesh.polygons):
            raise ValueError(name + ': evaluated prototype has no faces')
        mesh.calc_loop_triangles()
        uv_layer = next((layer for layer in mesh.uv_layers if layer.active_render), None)
        raw_bytes = (len(mesh.vertices) * 12 + len(mesh.loops) * (40 if uv_layer else 16)
                     + len(mesh.loop_triangles) * 16)
        if raw_bytes > MAX_MESH_BYTES:
            raise ValueError(name + ': evaluated geometry exceeds the 4 GiB copy budget')

        def copy(items, prop, kind, width):
            values = array(kind, [0]) * (len(items) * width)
            items.foreach_get(prop, values)
            return values

        positions = copy(mesh.vertices, 'co', 'f', 3)
        if scale != 1:
            # In double precision, as before, then stored as float.
            import numpy
            positions = array('f', (numpy.frombuffer(positions, dtype=numpy.float32)
                                    .astype(numpy.float64) * scale).astype(numpy.float32).tobytes())
        values = (positions, copy(mesh.loops, 'vertex_index', 'i', 1),
                  copy(mesh.corner_normals, 'vector', 'f', 3),
                  copy(mesh.loop_triangles, 'loops', 'i', 3),
                  copy(mesh.loop_triangles, 'material_index', 'i', 1),
                  copy(uv_layer.uv, 'vector', 'f', 2) if uv_layer else array('f'))
        # Calculate on the complete evaluated mesh, before material/chunk
        # splitting. This preserves the basis used by Blender normal maps at
        # UV mirrors and at boundaries between published geometry resources.
        if uv_layer:
            try:
                mesh.calc_tangents(uvmap=uv_layer.name)
                directions = copy(mesh.loops, 'tangent', 'f', 3)
                signs = copy(mesh.loops, 'bitangent_sign', 'f', 1)
                tangents = array('f', [0]) * (len(mesh.loops) * 4)
                for axis in range(3):
                    tangents[axis::4] = directions[axis::3]
                tangents[3::4] = signs
                values += (tangents,)
            except RuntimeError:
                warn(name + ': tangent calculation unavailable; renderer will generate a basis')
        result = tuple(pack_array(v) for v in values)
        if sum(map(len, result)) > MAX_MESH_BYTES:
            raise ValueError(name + ': evaluated geometry exceeds the 4 GiB copy budget')
        # Evaluated modifiers may introduce their own material slots.
        materials = [material_factors(m, mesh) for m in mesh.materials] or [list(DEFAULT_MATERIAL)]
        return result, materials
    finally:
        evaluated.to_mesh_clear()
