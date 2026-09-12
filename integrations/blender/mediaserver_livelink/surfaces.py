"""Worker-only stable material surfaces and bounded embedded image resources."""
from array import array
import copy
import hashlib
from pathlib import Path
import uuid

from geometry import pack_array, unpack_array

IDENTITY = [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]


def surface_id(identity, material):
    return str(uuid.uuid5(uuid.UUID(identity), 'material-surface-v1:' + material))


def surface_mesh(raw, indices):
    """Retain only referenced corners so bounds, picking and budgets stay local."""
    positions,vertices,normals,_,_,uv = [unpack_array(t,b) for t,b in zip(('f','i','f','i','i','f'),raw)]
    p,v,n,tri,slots,tex = [array(t) for t in ('f','i','f','i','i','f')]
    remap={}
    for corner in indices:
        if not 0 <= corner < len(vertices):
            raise ValueError('Invalid surface triangle corner')
        if corner not in remap:
            vertex=vertices[corner]
            if not 0 <= vertex < len(positions)//3:
                raise ValueError('Invalid surface vertex')
            remap[corner]=len(v)
            p.extend(positions[vertex*3:vertex*3+3]);v.append(len(v))
            n.extend(normals[corner*3:corner*3+3])
            if uv:tex.extend(uv[corner*2:corner*2+2])
        tri.append(remap[corner])
    slots.extend([0]*(len(tri)//3))
    return tuple(pack_array(a) for a in (p,v,n,tri,slots,tex))


def expand_surfaces(objects, meshes):
    result, expanded = [], {}
    for obj in objects:
        mats = obj.get('materials', [])
        if not obj['prototype'] or not any(isinstance(m, dict) for m in mats):
            result.append(obj)
            if obj['prototype']:
                expanded[obj['id']] = meshes[obj['id']]
            continue
        raw = meshes[obj['id']]
        slots, triangles = unpack_array('i',raw[4]), unpack_array('i',raw[3])
        grouped = {}
        for face, slot in enumerate(slots):
            if not 0 <= slot < len(mats):
                raise ValueError('Invalid evaluated material slot')
            m = mats[slot]
            key = m['id'] if isinstance(m,dict) else 'default'
            indices, material = grouped.setdefault(key, (array('i'), m))
            if material != m:
                raise ValueError('One material identity has conflicting definitions')
            indices.extend(triangles[face*3:face*3+3])
        if not obj.get('resource_only'):
            parent = dict(obj,prototype='',lod_id='',batch='',materials=[])
            result.append(parent)
        for key, (indices, material) in sorted(grouped.items()):
            identity = surface_id(obj['id'],key)
            child = dict(obj,id=identity, parent=obj['id'],matrix=list(IDENTITY),
                         name=obj['name']+' / '+(material['name'] if isinstance(material,dict) else 'Default'),
                         prototype=surface_id(obj['prototype'],key),materials=[material])
            if obj.get('lod_id'):
                child['lod_id'] = surface_id(obj['lod_id'],key)
            expanded[identity] = surface_mesh(raw,indices)
            result.append(child)
    available = {o['id'] for o in result if o['prototype']}
    for obj in result:
        if obj.get('lod_id') and obj['lod_id'] not in available:
            raise ValueError(obj['name']+': authored LOD must preserve every material identity')
    return result, expanded


def resolve_materials(objects, cache=None, refresh=False):
    """Read changed images once per update, on the serialization process.
    Explicit Publish Now rereads image content even when size/time are unchanged.

    Immutable hashes, not names or file timestamps, invalidate affected surfaces.
    No hashing, parsing or file reads occur in Blender's timer or render loop.
    """
    cache = {} if cache is None else cache
    images, total = {}, 0
    result = copy.deepcopy(objects)
    for obj in result:
        for mat in obj.get('materials',[]):
            if not isinstance(mat,dict):
                continue
            for view in mat['textures'].values():
                path = view['path']
                if path not in images:
                    source = Path(path)
                    stat=source.stat(); stamp=(stat.st_size,stat.st_mtime_ns)
                    if source.suffix.lower() not in ('.png','.jpg','.jpeg') or stat.st_size > 16*1024*1024:
                        raise ValueError('Image must be a PNG/JPEG no larger than 16 MiB: '+path)
                    old=cache.get(path)
                    if old is None or old[0]!=stamp or refresh:
                        data=source.read_bytes()
                        if len(data)>16*1024*1024:
                            raise ValueError('Image grew beyond the 16 MiB limit while reading: '+path)
                        old=(stamp,hashlib.sha256(data).hexdigest(),data)
                    data=old[2]
                    cache[path]=old
                    total += len(data)
                    if total > 64*1024*1024 or len(images) >= 32:
                        raise ValueError('Images exceed 64 MiB / 32 distinct image budget')
                    images[path] = (old[1],data)
                digest, data = images[path]
                view['hash'], view['bytes'] = digest, data
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
