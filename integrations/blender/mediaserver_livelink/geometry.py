"""Convert copied evaluated arrays into embedded static GLB. Never imports bpy."""
from array import array
import hashlib
import json
import math
import struct
import sys

MAX_CORNERS = 300_000
MAX_SCENE_BYTES = 128 * 1024 * 1024
# Source meshes may span many bounded render resources. The corner limit applies
# to each emitted resource, never to the original Blender object.
MAX_RAW_BYTES = 256 * 1024 * 1024
# Copied corner arrays include unwelded tangent frames. The immutable resources
# sent to the Director still have the separate 128 MiB scene limit above.


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


def glb(mesh, materials):
    """Indexed triangle corners preserve Blender split normals and UV seams."""
    positions, vertices, normals, triangles, slots, uv = [unpack_array(t, b) for t, b in
        zip(('f', 'i', 'f', 'i', 'i', 'f'), mesh)]
    tangents = unpack_array('f', mesh[6]) if len(mesh) == 7 else array('f')
    if not triangles or len(triangles) > MAX_CORNERS or len(triangles) % 3:
        raise ValueError('Evaluated mesh must have 1..100000 triangles')
    if len(normals) != len(vertices) * 3 or len(slots) * 3 != len(triangles):
        raise ValueError('Invalid evaluated corner arrays')
    if uv and len(uv) != len(vertices) * 2:
        raise ValueError('Invalid UV array')
    if tangents and (not uv or len(tangents) != len(vertices) * 4):
        raise ValueError('Invalid tangent array')
    if not all(math.isfinite(x) for a in (positions, normals, uv, tangents) for x in a):
        raise ValueError('Non-finite evaluated mesh')
    if any(w not in (-1, 1) for w in tangents[3::4]):
        raise ValueError('Invalid tangent handedness')
    doc = {'asset': {'version': '2.0', 'generator': 'Media Server LiveLink 0.1.0'},
           'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
           'meshes': [{'primitives': []}], 'buffers': [], 'bufferViews': [], 'accessors': [],
           'materials': []}
    binary = bytearray()

    def accessor(data, component, shape, width, bounds=False):
        while len(binary) % 4:
            binary.append(0)
        raw = pack_array(data)
        view = len(doc['bufferViews'])
        doc['bufferViews'].append({'buffer': 0, 'byteOffset': len(binary), 'byteLength': len(raw)})
        binary.extend(raw)
        entry = {'bufferView': view, 'componentType': component, 'count': len(data) // width, 'type': shape}
        if bounds:
            entry['min'] = [min(data[c::width]) for c in range(width)]
            entry['max'] = [max(data[c::width]) for c in range(width)]
        doc['accessors'].append(entry)
        return len(doc['accessors']) - 1

    p, n, tex, tan = array('f'), array('f'), array('f'), array('f')
    for corner, vertex in enumerate(vertices):
        if not 0 <= vertex < len(positions) // 3:
            raise ValueError('Invalid evaluated vertex index')
        x, y, z = positions[vertex * 3:vertex * 3 + 3]
        p.extend((x, z, -y))
        x, y, z = normals[corner * 3:corner * 3 + 3]
        n.extend((x, z, -y))
        if tangents:
            x, y, z, sign = tangents[corner * 4:corner * 4 + 4]
            # Match Blender's glTF exporter: rotate XYZ, retain the authored
            # handedness. Rebuilding from flipped V would reverse the bitangent.
            tan.extend((x, z, -y, sign))
        if uv:
            u, v = uv[corner * 2:corner * 2 + 2]
            tex.extend((u, 1 - v))
    attrs = {'POSITION': accessor(p, 5126, 'VEC3', 3, True),
             'NORMAL': accessor(n, 5126, 'VEC3', 3)}
    if tex:
        attrs['TEXCOORD_0'] = accessor(tex, 5126, 'VEC2', 2)
    if tan:
        attrs['TANGENT'] = accessor(tan, 5126, 'VEC4', 4)
    grouped = {}
    for tri, slot in enumerate(slots):
        if not 0 <= slot < len(materials):
            raise ValueError('Invalid evaluated material slot')
        indices = triangles[tri * 3:tri * 3 + 3]
        if any(not 0 <= i < len(vertices) for i in indices):
            raise ValueError('Invalid triangle corner index')
        grouped.setdefault(slot, array('I')).extend(int(i) for i in indices)
    for slot, indices in sorted(grouped.items()):
        doc['meshes'][0]['primitives'].append({'attributes': attrs, 'mode': 4, 'material': slot,
                                              'indices': accessor(indices, 5125, 'SCALAR', 1)})
    from surfaces import gltf_material
    if any(isinstance(m, dict) and m.get('textures') for m in materials) and not uv:
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


def manifest(source, revision, rows, previous=None):
    lines = [('MSLINK' if previous is None else 'MSDELTA') + ' 1 ' + quoted(source) + ' ' + quoted(revision)]
    if previous is None:
        lines.extend(rows[key] for key in sorted(rows))
    else:
        lines.extend('erase ' + key[0] + ' ' + quoted(key[1]) for key in sorted(previous.keys() - rows.keys()))
        lines.extend('upsert ' + rows[key] for key in sorted(rows) if rows[key] != previous.get(key))
    result = '\n'.join(lines) + '\n'
    # A very large replacement can exceed delta operation capacity; snapshot instead.
    if previous is not None and len(lines) > 1025:
        return manifest(source, revision, rows)
    if len(result.encode('utf-8')) > 1024 * 1024:
        raise ValueError('Scene exceeds 1 MiB manifest capacity')
    return result
