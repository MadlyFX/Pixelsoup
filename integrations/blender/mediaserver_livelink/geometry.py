"""Convert copied evaluated arrays into embedded static GLB. Never imports bpy."""
from array import array
import hashlib
import json
import math
import struct
import sys

import numpy

MAX_CORNERS = 300_000
# The GLB resources one scene retains. A ~30M-triangle scene is a few hundred
# bounded parts and well under this.
MAX_SCENE_BYTES = 4 * 1024 * 1024 * 1024
# Source meshes may span many bounded render resources. The corner limit applies
# to each emitted resource, never to the original Blender object.
# Copied corner arrays include unwelded tangent frames: about 40 bytes a
# corner. One mesh may copy MAX_MESH_BYTES (a ~30M-triangle object); one scene
# MAX_RAW_BYTES in all.
MAX_MESH_BYTES = 4 * 1024 * 1024 * 1024
MAX_RAW_BYTES = 8 * 1024 * 1024 * 1024
# Resources one scene may publish after splitting: within the Director's
# per-revision prototype limit.
MAX_RESOURCES = 1024
# Distinct resources one pairing keeps, edits included (the Director's
# link_resource_limit).
PAIRING_RESOURCES = 4096


def quoted(value):
    # std::quoted understands quotes/backslashes, not JSON's unicode escapes.
    value = str(value)
    if any(ord(c) < 32 for c in value):
        raise ValueError('Names must not contain control characters')
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def floats(values):
    if not all(math.isfinite(v) for v in values):
        raise ValueError('Non-finite mesh or placement values')
    return ' '.join(format(v, '.9g') for v in values)


def unpack_array(kind, data):
    result = array(kind)
    result.frombytes(data)
    if sys.byteorder != 'little':
        result.byteswap()
    return result


def pack_array(values):
    if sys.byteorder != 'little':
        values.byteswap()
    return values.tobytes()


def numeric(kind, data):
    """A read-only little-endian numpy view of packed array bytes."""
    return numpy.frombuffer(data, dtype='<f4' if kind == 'f' else '<i4')


def glb(mesh, materials):
    """Indexed triangle corners preserve Blender split normals and UV seams.

    Vectorised: a part is up to MAX_CORNERS corners and a detailed scene has
    hundreds of parts, so nothing here loops per corner in Python."""
    positions, vertices, normals, triangles, slots, uv = [numeric(t, b) for t, b in
        zip(('f', 'i', 'f', 'i', 'i', 'f'), mesh)]
    tangents = numeric('f', mesh[6]) if len(mesh) == 7 else numeric('f', b'')
    if not len(triangles) or len(triangles) > MAX_CORNERS or len(triangles) % 3:
        raise ValueError('Evaluated mesh must have 1..100000 triangles')
    if len(normals) != len(vertices) * 3 or len(slots) * 3 != len(triangles):
        raise ValueError('Invalid evaluated corner arrays')
    if len(uv) and len(uv) != len(vertices) * 2:
        raise ValueError('Invalid UV array')
    if len(tangents) and (not len(uv) or len(tangents) != len(vertices) * 4):
        raise ValueError('Invalid tangent array')
    if not all(numpy.isfinite(a).all() for a in (positions, normals, uv, tangents)):
        raise ValueError('Non-finite evaluated mesh')
    if len(tangents) and not (numpy.abs(tangents[3::4]) == 1).all():
        raise ValueError('Invalid tangent handedness')
    if len(vertices) and (vertices.min() < 0 or vertices.max() >= len(positions) // 3):
        raise ValueError('Invalid evaluated vertex index')
    doc = {'asset': {'version': '2.0', 'generator': 'Pixelsoup Live Link 0.1.0'},
           'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
           'meshes': [{'primitives': []}], 'buffers': [], 'bufferViews': [], 'accessors': [],
           'materials': []}
    binary = bytearray()

    def accessor(data, component, shape, bounds=False):
        while len(binary) % 4:
            binary.append(0)
        raw = data.astype('<f4' if component == 5126 else '<u4', copy=False).tobytes()
        view = len(doc['bufferViews'])
        doc['bufferViews'].append({'buffer': 0, 'byteOffset': len(binary), 'byteLength': len(raw)})
        binary.extend(raw)
        entry = {'bufferView': view, 'componentType': component, 'count': len(data), 'type': shape}
        if bounds:
            entry['min'] = [float(v) for v in data.min(axis=0)]
            entry['max'] = [float(v) for v in data.max(axis=0)]
        doc['accessors'].append(entry)
        return len(doc['accessors']) - 1

    # Blender Z-up to glTF Y-up: (x, y, z) -> (x, z, -y).
    def y_up(values):
        rotated = numpy.empty_like(values)
        rotated[:, 0], rotated[:, 1], rotated[:, 2] = values[:, 0], values[:, 2], -values[:, 1]
        if values.shape[1] == 4:
            # Match Blender's glTF exporter: retain the authored handedness.
            rotated[:, 3] = values[:, 3]
        return rotated

    attrs = {'POSITION': accessor(y_up(positions.reshape(-1, 3)[vertices]), 5126, 'VEC3', True),
             'NORMAL': accessor(y_up(normals.reshape(-1, 3)), 5126, 'VEC3')}
    if len(uv):
        flipped = uv.reshape(-1, 2).copy()
        # In double precision, as the per-corner encoder did, then stored as float.
        flipped[:, 1] = (1.0 - flipped[:, 1].astype(numpy.float64)).astype(numpy.float32)
        attrs['TEXCOORD_0'] = accessor(flipped, 5126, 'VEC2')
    if len(tangents):
        attrs['TANGENT'] = accessor(y_up(tangents.reshape(-1, 4)), 5126, 'VEC4')
    if slots.min() < 0 or slots.max() >= len(materials):
        raise ValueError('Invalid evaluated material slot')
    if triangles.min() < 0 or triangles.max() >= len(vertices):
        raise ValueError('Invalid triangle corner index')
    faces = triangles.reshape(-1, 3)
    for slot in numpy.unique(slots):
        doc['meshes'][0]['primitives'].append({'attributes': attrs, 'mode': 4, 'material': int(slot),
                                              'indices': accessor(faces[slots == slot].reshape(-1), 5125, 'SCALAR')})
    from surfaces import gltf_material
    if any(isinstance(m, dict) and m.get('textures') for m in materials) and not len(uv):
        raise ValueError('Textured surfaces require an active render UV map')
    doc['materials'] = [gltf_material(m, doc, binary) for m in materials]
    doc['buffers'] = [{'byteLength': len(binary)}]
    text = json.dumps(doc, separators=(',', ':'), allow_nan=False).encode('utf-8')
    text += b' ' * (-len(text) % 4)
    binary.extend(b'\0' * (-len(binary) % 4))
    result = (struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(text) + 8 + len(binary))
              + struct.pack('<I4s', len(text), b'JSON') + text
              + struct.pack('<I4s', len(binary), b'BIN\0') + binary)
    return hashlib.sha256(result).hexdigest(), result


def records(objects, prototypes):
    result = {}
    for obj in objects:
        proto = obj['prototype']
        if proto:
            digest = prototypes[obj['id']][0]
            result[('prototype', proto)] = 'prototype ' + ' '.join(map(quoted, (proto, digest, digest + '.glb')))
            if obj.get('lod_id'):
                low = prototypes[obj['lod_id']][0]
                result[('prototype',proto)] += ' lod ' + floats([obj['lod_pixels']]) + ' ' + quoted(low) + ' ' + quoted(low+'.glb')
        if obj.get('resource_only'):
            continue
        material_id = ''
        if proto and len(obj.get('materials',[])) == 1 and isinstance(obj['materials'][0],dict):
            m=obj['materials'][0];material_id=m['id']
            canonical = {k:v for k,v in m.items() if k not in ('name','textures')}
            canonical['textures']={role:{k:v for k,v in view.items() if k not in ('bytes','path')} for role,view in m['textures'].items()}
            revision=hashlib.sha256(json.dumps(canonical,sort_keys=True,separators=(',',':')).encode('utf-8')).hexdigest()
            result[('material',material_id)]='material '+quoted(material_id)+' '+quoted(revision)+' '+floats(m['factors'])
        fields = (obj['id'], obj['name'], obj['parent'], proto, material_id, 'origin-v1')
        result[('object', obj['id'])] = ('object ' + ' '.join(map(quoted, fields)) + ' '
            + str(int(obj['visible'])) + ' ' + floats(obj['matrix']))
        if obj.get('light'):
            light = obj['light']
            result[('object',obj['id'])] += ' light ' + quoted(light['kind']) + ' ' + quoted(light['unit']) + ' ' + floats(light['values'])
        if obj.get('batch'):
            result[('object',obj['id'])] += ' batch ' + quoted(obj['batch'])
        if obj.get('animation'):
            result[('object',obj['id'])] += ' animation ' + str(obj['animation']['start']) + ' ' + str(int(obj['animation']['loop']))
    if len(result) > 1024:
        raise ValueError('Scene exceeds 1024 manifest records')
    return result


def manifest(source, revision, rows, previous=None, absorbs=()):
    lines = [('MSLINK' if previous is None else 'MSDELTA') + ' 1 ' + quoted(source) + ' ' + quoted(revision)]
    if previous is None:
        lines.extend(rows[key] for key in sorted(rows))
    else:
        lines.extend('erase ' + key[0] + ' ' + quoted(key[1]) for key in sorted(previous.keys() - rows.keys()))
        lines.extend('upsert ' + rows[key] for key in sorted(rows) if rows[key] != previous.get(key))
    # Pixelsoup overrides the artist has just replaced (writeback).
    lines.extend('absorb ' + quoted(identity) + ' ' + kind for identity, kind in absorbs)
    result = '\n'.join(lines) + '\n'
    # A very large replacement can exceed delta operation capacity; snapshot instead.
    if previous is not None and len(lines) > 1025:
        return manifest(source, revision, rows, None, absorbs)
    if len(result.encode('utf-8')) > 1024 * 1024:
        raise ValueError('Scene exceeds 1 MiB manifest capacity')
    return result
