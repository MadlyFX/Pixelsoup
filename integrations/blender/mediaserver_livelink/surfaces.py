"""Worker-only stable material surfaces and bounded embedded image resources."""
from array import array
import copy
from contextlib import nullcontext
import hashlib
import os
import tempfile
from pathlib import Path
import uuid

import numpy

from geometry import MAX_CORNERS, numeric, pack_array, unpack_array

IDENTITY = [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
# Embedded image budget per scene. Linked packages hold textures as BC7/BC5
# (one byte per pixel), so 256 images fit the Director's publication limits.
MAX_IMAGES = 256
MAX_IMAGE_BYTES = 256 * 1024 * 1024


def surface_id(identity, material):
    return str(uuid.uuid5(uuid.UUID(identity), 'material-surface-v1:' + material))


def surface_mesh(raw, indices, decoded=None):
    """Retain only referenced corners so bounds, picking and budgets stay local.

    Corners weld only when the source vertex, split normal, UV and tangent all
    agree: seams and hard edges stay, smooth scanned meshes shrink. The welded
    vertices keep the order their corners first appear in."""
    decoded = decoded or [numeric(t, b) for t, b in zip(('f', 'i', 'f', 'i', 'i', 'f', 'f'), raw)]
    positions, vertices, normals, _, _, uv = decoded[:6]
    tangents = decoded[6] if len(decoded) == 7 else numeric('f', b'')
    corners = numpy.asarray(indices, dtype=numpy.int64)
    if len(corners) and (corners.min() < 0 or corners.max() >= len(vertices)):
        raise ValueError('Invalid surface triangle corner')
    source = vertices[corners].astype(numpy.int64)
    if len(source) and (source.min() < 0 or source.max() >= len(positions) // 3):
        raise ValueError('Invalid surface vertex')
    columns = [source.astype('<u4').reshape(-1, 1).view(numpy.uint32)]
    attributes = [normals.reshape(-1, 3)[corners]]
    if len(uv):
        attributes.append(uv.reshape(-1, 2)[corners])
    if len(tangents):
        attributes.append(tangents.reshape(-1, 4)[corners])
    for values in attributes:
        # +0.0 folds -0.0 into 0.0: the two compare equal and weld.
        columns.append((values + numpy.float32(0.0)).astype('<f4').view(numpy.uint32))
    keys = numpy.ascontiguousarray(numpy.hstack(columns))
    # Sort by a 64-bit hash of each key row; a run of equal hashes is one weld
    # only where the rows really are equal, so a collision can only miss a
    # weld, never merge two different corners. Stable, so each run starts at
    # the key's first appearance.
    hashed = numpy.full(len(keys), 0x9E3779B97F4A7C15, dtype=numpy.uint64)
    for column in keys.T:
        hashed ^= column.astype(numpy.uint64)
        hashed *= numpy.uint64(0xBF58476D1CE4E5B9)
        hashed ^= hashed >> numpy.uint64(31)
    order = numpy.argsort(hashed, kind='stable')
    starts = numpy.ones(len(order), dtype=bool)
    if len(order) > 1:
        ordered = keys[order]
        starts[1:] = (hashed[order][1:] != hashed[order][:-1]) | (ordered[1:] != ordered[:-1]).any(axis=1)
    run = numpy.cumsum(starts) - 1
    first = order[starts]
    # Welded vertices in the order their keys first appear.
    appearance = numpy.argsort(first, kind='stable')
    rank = numpy.empty_like(appearance)
    rank[appearance] = numpy.arange(len(appearance))
    inverse = numpy.empty(len(order), dtype=numpy.int64)
    inverse[order] = run
    kept = corners[first[appearance]]
    p = positions.reshape(-1, 3)[vertices[kept]]
    welded = (p.astype('<f4').tobytes(), numpy.arange(len(kept), dtype='<i4').tobytes(),
              normals.reshape(-1, 3)[kept].astype('<f4').tobytes(),
              rank[inverse].astype('<i4').tobytes(),
              numpy.zeros(len(corners) // 3, dtype='<i4').tobytes(),
              uv.reshape(-1, 2)[kept].astype('<f4').tobytes() if len(uv) else b'')
    if len(tangents):
        welded += (tangents.reshape(-1, 4)[kept].astype('<f4').tobytes(),)
    return welded


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
            decoded = [numeric(t,b) for t,b in zip(('f','i','f','i','i','f','f'),raw)]
            slots, triangles = decoded[4], decoded[3]
            if len(slots) and (slots.min() < 0 or slots.max() >= len(mats)):
                raise ValueError('Invalid evaluated material slot')
            # Faces grouped by material identity, in face order within a group.
            identities = sorted(set(keys))
            group = numpy.array([identities.index(key) for key in keys], dtype=numpy.int64)[slots]
            faces = triangles.reshape(-1, 3)
            jobs = []
            for number, key in enumerate(identities):
                indices = faces[group == number].reshape(-1)
                for start in range(0, len(indices), MAX_CORNERS):
                    jobs.append((key, start // MAX_CORNERS, indices[start:start+MAX_CORNERS]))
            # Parts are independent and numpy releases the GIL while it sorts
            # and gathers, so a detailed mesh welds on every core.
            if len(jobs) > 1:
                from concurrent.futures import ThreadPoolExecutor
                with ThreadPoolExecutor(max_workers=min(len(jobs), os.cpu_count() or 1, 16)) as pool:
                    welded = list(pool.map(lambda job: surface_mesh(raw, job[2], decoded), jobs))
            else:
                welded = [surface_mesh(raw, job[2], decoded) for job in jobs]
            parts = [(key, part, mesh) for (key, part, _), mesh in zip(jobs, welded)]
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
    packed_sources = {}
    for obj in objects:
        for mat in obj.get('materials', []):
            if not isinstance(mat, dict):
                continue
            for view in mat['textures'].values():
                path = view['path']
                if path in prepared or path in failed:
                    continue
                if len(prepared) >= MAX_IMAGES:
                    failed[path] = 'Scene exceeds ' + str(MAX_IMAGES) + ' distinct images'
                    continue
                try:
                    packed = view.get('packed_bytes')
                    if packed is not None:
                        if not isinstance(packed, bytes) or not 0 < len(packed) <= 16 * 1024 * 1024:
                            raise ValueError('Packed image exceeds 16 MiB')
                        old = cache.get(path)
                        if old is not None and len(old) > 3 and old[3] is packed and not refresh:
                            prepared[path] = old
                            continue
                        stamp = hashlib.sha256(packed).hexdigest()
                        if old is not None and old[0] == stamp and not refresh:
                            prepared[path] = (*old[:3], packed)
                            continue
                        width, height = dimensions(packed)
                        if min(width, height) < 1 or max(width, height) > 32768:
                            raise ValueError('Invalid packed image dimensions')
                        if max(width, height) > 4096:
                            oversized[path] = stamp
                            packed_sources[path] = packed
                            prepared[path] = None
                        else:
                            prepared[path] = (stamp, stamp, packed, packed)
                        continue
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
    # Packed images have no external file. Materialize only private worker
    # copies for the isolated image decoder; never unpack the user's blend.
    with (tempfile.TemporaryDirectory(prefix='ms-link-packed-') if packed_sources else nullcontext(None)) as directory:
        aliases = {}
        for index, (path, data) in enumerate(packed_sources.items()):
            file = Path(directory) / (str(index) + ('.png' if data.startswith(b'\x89PNG') else '.jpg'))
            file.write_bytes(data)
            aliases[path] = str(file)
        converted_paths = resize_images([aliases.get(path, path) for path in oversized], blender, progress, keepalive)
        converted = {path: converted_paths.get(aliases.get(path, path)) for path in oversized}
    for path, stamp in oversized.items():
        data = converted.get(path)
        if data:
            prepared[path] = (stamp, hashlib.sha256(data).hexdigest(), data)
            if path in packed_sources:
                prepared[path] += (packed_sources[path],)
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
                        old=prepared.get(path)
                        if old is None:
                            source = Path(path)
                            stat=source.stat(); stamp=(stat.st_size,stat.st_mtime_ns)
                            with source.open('rb') as stream:
                                data=stream.read(16*1024*1024 + 1)
                            if len(data)>16*1024*1024:
                                raise ValueError('Image grew beyond the 16 MiB limit while reading: '+path)
                            old=(stamp,hashlib.sha256(data).hexdigest(),data)
                        data=old[2]
                        if not (data.startswith(b'\x89PNG\r\n\x1a\n') and b'IEND' in data[-12:]
                            or data.startswith(b'\xff\xd8') and data.endswith(b'\xff\xd9')):
                            raise ValueError('Invalid PNG/JPEG image: '+path)
                        cache[path]=old
                        if total + len(data) > MAX_IMAGE_BYTES or len(images) >= MAX_IMAGES:
                            raise ValueError('Images exceed ' + str(MAX_IMAGE_BYTES >> 20) + ' MiB / '
                                             + str(MAX_IMAGES) + ' distinct image budget')
                        total += len(data)
                        images[path] = (old[1],data)
                    digest, data = images[path]
                    view.pop('packed_bytes', None)
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
