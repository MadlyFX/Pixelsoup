"""Worker-only stable material surfaces and bounded embedded image resources."""
from array import array
import copy
import hashlib
from pathlib import Path
import uuid

from geometry import MAX_CORNERS, pack_array, unpack_array

IDENTITY = [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]


def surface_id(identity, material):
    return str(uuid.uuid5(uuid.UUID(identity), 'material-surface-v1:' + material))


def surface_mesh(raw, indices, decoded=None):
    """Retain only referenced corners so bounds, picking and budgets stay local."""
    decoded = decoded or [unpack_array(t,b) for t,b in zip(('f','i','f','i','i','f','f'),raw)]
    positions,vertices,normals,_,_,uv = decoded[:6]
    tangents = decoded[6] if len(decoded) == 7 else array('f')
    p,v,n,tri,slots,tex = [array(t) for t in ('f','i','f','i','i','f')]
    tan = array('f')
    remap, attributes = {}, {}
    for corner in indices:
        if not 0 <= corner < len(vertices):
            raise ValueError('Invalid surface triangle corner')
        if corner not in remap:
            vertex=vertices[corner]
            if not 0 <= vertex < len(positions)//3:
                raise ValueError('Invalid surface vertex')
            normal, texture = normals[corner*3:corner*3+3], uv[corner*2:corner*2+2]
            tangent = tangents[corner*4:corner*4+4]
            # Weld repeated corners only when the source vertex, split normal
            # and UV all agree. Preserve seams and hard edges without tripling
            # the memory footprint of smooth scanned meshes.
            key = (vertex, *normal, *texture, *tangent)
            index = attributes.get(key)
            if index is None:
                index = len(v)
                attributes[key] = index
                p.extend(positions[vertex*3:vertex*3+3]);v.append(index)
                n.extend(normal)
                if uv:tex.extend(texture)
                tan.extend(tangent)
            remap[corner] = index
        tri.append(remap[corner])
    slots.extend([0]*(len(tri)//3))
    return tuple(pack_array(a) for a in ((p,v,n,tri,slots,tex,tan) if tangents else (p,v,n,tri,slots,tex)))


def expand_surfaces(objects, meshes, cache=None):
    result, expanded = [], {}
    cache = {} if cache is None else cache
    active = set()
    for obj in objects:
        mats = obj.get('materials', [])
        if not obj['prototype'] or (not any(isinstance(m, dict) for m in mats)
                                   and len(meshes[obj['id']][3]) <= MAX_CORNERS * 4):
            result.append(obj)
            if obj['prototype']:
                expanded[obj['id']] = meshes[obj['id']]
            continue
        raw = meshes[obj['id']]
        active.add(obj['id'])
        keys = tuple(m['id'] if isinstance(m,dict) else ('default' if len(mats) == 1 else 'slot-' + str(slot))
                     for slot, m in enumerate(mats))
        entry = cache.get(obj['id'])
        if entry is None or entry[0] is not raw or entry[1] != keys:
            decoded = [unpack_array(t,b) for t,b in zip(('f','i','f','i','i','f','f'),raw)]
            slots, triangles = decoded[4], decoded[3]
            grouped = {}
            for face, slot in enumerate(slots):
                if not 0 <= slot < len(mats):
                    raise ValueError('Invalid evaluated material slot')
                indices = grouped.setdefault(keys[slot], array('i'))
                indices.extend(triangles[face*3:face*3+3])
            parts = []
            for key, indices in sorted(grouped.items()):
                for start in range(0, len(indices), MAX_CORNERS):
                    part = start // MAX_CORNERS
                    parts.append((key, part, surface_mesh(raw,indices[start:start+MAX_CORNERS],decoded)))
            entry = (raw, keys, parts)
            cache[obj['id']] = entry
        materials = {}
        for key, material in zip(keys, mats):
            if key in materials and materials[key] != material:
                raise ValueError('One material identity has conflicting definitions')
            materials[key] = material
        if not obj.get('resource_only'):
            parent = dict(obj,prototype='',lod_id='',batch='',materials=[])
            result.append(parent)
        for key, part, mesh in entry[2]:
            material = materials[key]
            # Keep the existing surface identity for the first part, even
            # when an edit grows or shrinks a surface across the limit.
            part_key = key if part == 0 else key + ':part-' + str(part)
            identity = surface_id(obj['id'],part_key)
            name = obj['name']+' / '+(material['name'] if isinstance(material,dict) else 'Default')
            child = dict(obj,id=identity, parent=obj['id'],matrix=list(IDENTITY),
                         name=name + (' / Part ' + str(part + 1) if part else ''),
                         prototype=surface_id(obj['prototype'],part_key),materials=[material])
            if obj.get('lod_id'):
                child['lod_id'] = surface_id(obj['lod_id'],part_key)
            expanded[identity] = mesh
            result.append(child)
    for identity in set(cache) - active:
        del cache[identity]
    available = {o['id'] for o in result if o['prototype']}
    for obj in result:
        if obj.get('lod_id') and obj['lod_id'] not in available:
            raise ValueError(obj['name']+': authored LOD must preserve every material identity')
    return result, expanded


def resolve_materials(objects, cache=None, refresh=False, warnings=None, blender='', progress=lambda message: None,
                      keepalive=lambda: None):
    """Read changed images once per update, on the serialization process.
    Explicit Publish Now rereads image content even when size/time are unchanged.

    Immutable hashes, not names or file timestamps, invalidate affected surfaces.
    No hashing, parsing or file reads occur in Blender's timer or render loop.
    """
    warnings = [] if warnings is None else warnings
    cache = {} if cache is None else cache
    from image_resize import dimensions, resize_images
    oversized, prepared, failed = {}, {}, {}
    for obj in objects:
        for mat in obj.get('materials', []):
            if not isinstance(mat, dict):
                continue
            for view in mat['textures'].values():
                path = view['path']
                if path in prepared or path in failed:
                    continue
                if len(prepared) >= 32:
                    failed[path] = 'Scene exceeds 32 distinct images'
                    continue
                try:
                    source = Path(path)
                    stat = source.stat()
                    stamp = (stat.st_size, stat.st_mtime_ns)
                    old = cache.get(path)
                    if old is not None and old[0] == stamp and not refresh:
                        prepared[path] = old
                        continue
                    if stat.st_size > 256 * 1024 * 1024:
                        raise ValueError('Source image exceeds 256 MiB')
                    width, height = dimensions(path)
                    if min(width, height) < 1 or max(width, height) > 32768:
                        raise ValueError('Invalid image dimensions')
                    if max(width, height) > 4096 or stat.st_size > 16 * 1024 * 1024:
                        oversized[path] = stamp
                    prepared[path] = None
                except (OSError, ValueError) as error:
                    failed[path] = str(error)
    converted = resize_images(oversized, blender, progress, keepalive)
    for path, stamp in oversized.items():
        data = converted.get(path)
        if data:
            prepared[path] = (stamp, hashlib.sha256(data).hexdigest(), data)
            warnings.append(Path(path).name + ': resized export texture to fit 1024 pixels')
        else:
            failed[path] = 'Could not prepare oversized texture'
    images, total = {}, 0
    result = copy.deepcopy(objects)
    for obj in result:
        for mat in obj.get('materials',[]):
            if not isinstance(mat,dict):
                continue
            for role, view in list(mat['textures'].items()):
                try:
                    path = view['path']
                    if path in failed:
                        raise ValueError(failed[path])
                    if path not in images:
                        source = Path(path)
                        stat=source.stat(); stamp=(stat.st_size,stat.st_mtime_ns)
                        old=prepared.get(path)
                        if old is None:
                            with source.open('rb') as stream:
                                data=stream.read(16*1024*1024 + 1)
                            if len(data)>16*1024*1024:
                                raise ValueError('Image grew beyond the 16 MiB limit while reading: '+path)
                            if not (data.startswith(b'\x89PNG\r\n\x1a\n') and b'IEND' in data[-12:]
                                or data.startswith(b'\xff\xd8') and data.endswith(b'\xff\xd9')):
                                raise ValueError('Invalid PNG/JPEG image: '+path)
                            old=(stamp,hashlib.sha256(data).hexdigest(),data)
                        data=old[2]
                        cache[path]=old
                        if total + len(data) > 64*1024*1024 or len(images) >= 32:
                            raise ValueError('Images exceed 64 MiB / 32 distinct image budget')
                        total += len(data)
                        images[path] = (old[1],data)
                    digest, data = images[path]
                    view['hash'], view['bytes'] = digest, data
                except (OSError, ValueError) as error:
                    del mat['textures'][role]
                    warnings.append(mat['name'] + ': ignored texture (' + str(error) + ')')
    for path in set(cache)-images.keys():
        del cache[path]
    return result


def gltf_material(material, doc, binary):
    if not isinstance(material,dict):
        return {'pbrMetallicRoughness': {'baseColorFactor': [*material[:3],1],
                'roughnessFactor':material[3],'metallicFactor':material[4]},'emissiveFactor':material[5:8]}
    m = material
    factors = m['factors']
    result = {'name':m['id'],'pbrMetallicRoughness':{'baseColorFactor':[*factors[:3],m['alpha']],
              'roughnessFactor':factors[3],'metallicFactor':factors[4]},'emissiveFactor':factors[5:8],
              'alphaMode':m['alpha_mode'],'alphaCutoff':m['alpha_cutoff'],'doubleSided':m['double_sided']}
    if m.get('extensions'):
        result['extensions'] = m['extensions']
        doc['extensionsUsed'] = sorted(set(doc.get('extensionsUsed',[])) | m['extensions'].keys())
    for role, texture in sorted(m['textures'].items()):
        data = texture['bytes']
        while len(binary)%4:
            binary.append(0)
        view = len(doc['bufferViews'])
        doc['bufferViews'].append({'buffer':0,'byteOffset':len(binary),'byteLength':len(data)})
        binary.extend(data)
        image = len(doc.setdefault('images',[]))
        doc['images'].append({'bufferView':view,'mimeType':'image/png' if data.startswith(b'\x89PNG') else 'image/jpeg'})
        index = len(doc.setdefault('textures',[]))
        doc.setdefault('samplers',[]).append({'wrapS':texture['wrap'],'wrapT':texture['wrap'],
            'minFilter':9987 if texture['filter']==9729 else 9984,'magFilter':texture['filter']})
        doc['textures'].append({'source':image,'sampler':index})
        target = result['pbrMetallicRoughness'] if role in ('baseColorTexture','metallicRoughnessTexture') else result
        target[role] = {'index':index}
        if 'scale' in texture:
            target[role]['scale'] = texture['scale']
    return result
