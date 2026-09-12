"""Bounded deterministic environment serialization; no Blender or filesystem access."""
from array import array
import hashlib
import math
import struct
import uuid

if __package__:
    from .geometry import MAX_CORNERS, pack_array, unpack_array
else:
    from geometry import MAX_CORNERS, pack_array, unpack_array

MAX_INSTANCES = 4096
MAX_CHUNKS = 64
IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def batch_bytes(matrices, density=100, seed=0):
    """Order-independent content sampling. These are deliberately NOT shot IDs."""
    if not 0 <= density <= 100 or len(matrices) > 65536:
        raise ValueError('Scatter evaluation exceeds 65536 candidates or invalid export density')
    selected = []
    for matrix in matrices:
        if len(matrix) != 16 or not all(math.isfinite(v) for v in matrix):
            raise ValueError('Invalid scatter transform')
        values = [matrix[c*4+r] for c in range(4) for r in range(3)]
        raw = struct.pack('<12f', *values)
        rank = int.from_bytes(hashlib.sha256(str(seed).encode() + raw).digest()[:8], 'little')
        if rank * 100 < density * (1 << 64):
            selected.append(raw)
    if len(selected) > MAX_INSTANCES:
        raise ValueError('Export exceeds 4096 scatter instances; lower Export density')
    return b''.join(sorted(selected)).hex()


def terrain_chunks(mesh, size):
    """Clip evaluated triangles to local XY metre cells, retaining corner normals/UVs.

    Cell IDs are integer coordinates, independent of topology and vertex order.
    Shared boundaries use the same absolute coordinates and interpolated attributes.
    There are no skirts and no independently decimated boundary vertices.
    """
    if not math.isfinite(size) or size <= 0:
        raise ValueError('Terrain chunk size must be positive')
    p, vertex, normal, triangles, slots, uv = [unpack_array(t, b) for t, b in
        zip(('f', 'i', 'f', 'i', 'i', 'f'), mesh)]
    chunks = {}
    corners = 0

    def clip(poly, axis, boundary, keep_greater):
        result = []
        for a, b in zip(poly, poly[1:] + poly[:1]):
            inside_a = a[axis] >= boundary if keep_greater else a[axis] <= boundary
            inside_b = b[axis] >= boundary if keep_greater else b[axis] <= boundary
            if inside_a:
                result.append(a)
            if inside_a != inside_b:
                # Canonical edge orientation gives identical boundary arithmetic.
                lo, hi = sorted((a, b))
                t = (boundary - lo[axis]) / (hi[axis] - lo[axis])
                point = [x + (y - x) * t for x, y in zip(lo, hi)]
                point[axis] = boundary
                result.append(point)
        return result

    for tri, slot in enumerate(slots):
        poly = []
        for c in triangles[tri*3:tri*3+3]:
            v = vertex[c]
            poly.append([*p[v*3:v*3+3], *normal[c*3:c*3+3], *(uv[c*2:c*2+2] if uv else (0, 0))])
        if len(poly) != 3 or not all(math.isfinite(v) for point in poly for v in point):
            raise ValueError('Invalid terrain triangles')
        first = [math.floor(min(v[a] for v in poly)/size) for a in (0, 1)]
        last = [math.ceil(max(v[a] for v in poly)/size)-1 for a in (0, 1)]
        last = [max(a,b) for a,b in zip(first,last)]
        if (last[0]-first[0]+1)*(last[1]-first[1]+1) > MAX_CHUNKS:
            raise ValueError('Terrain triangle spans more than 64 cells; use larger chunks')
        for x in range(first[0],last[0]+1):
            for y in range(first[1],last[1]+1):
                clipped = poly
                for axis, bound, greater in ((0,x*size,True),(0,(x+1)*size,False),
                                              (1,y*size,True),(1,(y+1)*size,False)):
                    clipped = clip(clipped,axis,bound,greater) if clipped else []
                for i in range(1,len(clipped)-1):
                    face = [clipped[0],clipped[i],clipped[i+1]]
                    u = [face[1][j]-face[0][j] for j in range(3)]
                    v = [face[2][j]-face[0][j] for j in range(3)]
                    cross = [u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]]
                    if sum(z*z for z in cross) < 1e-20:
                        continue
                    corners += 3
                    if corners > MAX_CORNERS:
                        raise ValueError('Clipped terrain exceeds 300000 corners')
                    chunks.setdefault((x,y),[]).append((slot,face))
                    if len(chunks) > MAX_CHUNKS:
                        raise ValueError('Terrain exceeds 64 spatial chunks')
    result = {}
    for cell, faces in sorted(chunks.items()):
        values = [array(t) for t in ('f','i','f','i','i','f')]
        for slot, face in faces:
            values[4].append(slot)
            for point in face:
                index = len(values[1])
                values[0].extend(point[:3]); values[1].append(index)
                length = math.sqrt(sum(v*v for v in point[3:6]))
                if length <= 1e-12:
                    raise ValueError('Terrain has invalid interpolated normals')
                values[2].extend(v/length for v in point[3:6]); values[3].append(index)
                if uv:
                    values[5].extend(point[6:8])
        result[cell] = tuple(pack_array(a) for a in values)
    if not result:
        raise ValueError('Terrain has no publishable surface')
    return result


def boundary(mesh, cell, size):
    p, _, normals, _, _, _ = [unpack_array(t,b) for t,b in zip(('f','i','f','i','i','f'),mesh)]
    result = set()
    for i in range(len(p)//3):
        point = p[i*3:i*3+3]
        if any(abs(point[a]-edge*size) < 1e-5 for a in (0,1) for edge in (cell[a],cell[a]+1)):
            result.add(tuple(round(v,5) for v in (*point,*normals[i*3:i*3+3])))
    return result


def expand_terrain(objects, raw):
    """One coherent base/LOD revision; reject authored LOD boundary mismatches."""
    result, copied = [], dict(raw)
    by_id={o['id']:o for o in objects}
    for obj in objects:
        if not obj.get('terrain_size'):
            result.append(obj)
            continue
        base = terrain_chunks(raw[obj['id']],obj['terrain_size'])
        lod = terrain_chunks(raw[obj['lod_id']],obj['terrain_size']) if obj.get('lod_id') else None
        if lod is not None:
            if base.keys() != lod.keys() or any(boundary(base[c],c,obj['terrain_size']) !=
                                                boundary(lod[c],c,obj['terrain_size']) for c in base):
                raise ValueError(obj['name'] + ': authored terrain LOD must preserve every chunk boundary position and normal')
        result.append(dict(obj,prototype='',terrain_size=0))
        for cell, mesh in base.items():
            identity = str(uuid.uuid5(uuid.UUID(obj['id']), 'terrain-v1:%.9g:%d:%d' % (obj['terrain_size'],*cell)))
            child = dict(obj,id=identity,name=obj['name']+' [%d, %d]' % cell,parent=obj['id'],
                prototype=str(uuid.uuid5(uuid.UUID(identity),'mesh')),matrix=list(IDENTITY),terrain_size=0)
            copied[identity] = mesh
            if lod is not None:
                low_id = str(uuid.uuid5(uuid.UUID(identity),'lod'))
                child['lod_id'] = low_id
                result.append(dict(child,id=low_id,prototype=str(uuid.uuid5(uuid.UUID(low_id),'mesh')),
                                   resource_only=True,lod_id='',materials=by_id[obj['lod_id']]['materials']))
                copied[low_id] = lod[cell]
            result.append(child)
    # An authored terrain LOD is replaced by its chunk resources.
    terrain_lods = {o.get('lod_id') for o in objects if o.get('terrain_size')}
    return [o for o in result if o['id'] not in terrain_lods], copied
