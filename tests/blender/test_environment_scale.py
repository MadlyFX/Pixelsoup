from array import array
import copy
import json
from pathlib import Path
import struct
import sys
import unittest
import uuid

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'integrations/blender/mediaserver_livelink'))
from scale import batch_bytes, terrain_chunks, boundary, IDENTITY
from geometry import glb, pack_array, records, manifest
from worker import Publisher
from test_publisher_core import FakeDirector, object_row, triangle


def terrain(n=32):
    p = [v for y in range(n+1) for x in range(n+1) for v in (x,y,0)]
    faces = []
    for y in range(n):
        for x in range(n):
            a=y*(n+1)+x
            faces.extend((a,a+1,a+n+2,a,a+n+2,a+n+1))
    return tuple(pack_array(array(k,v)) for k,v in (('f',p),('i',faces),('f',[0,0,1]*len(faces)),
        ('i',range(len(faces))),('i',[0]*(len(faces)//3)),('f',[])))


def translated(x,y,z=0):
    result=list(IDENTITY); result[12:15]=[x,y,z]; return result


class EnvironmentTests(unittest.TestCase):
    def test_density_order_seed_and_capacity(self):
        rows=[translated(x,y) for x in range(64) for y in range(32)]
        full=batch_bytes(rows)
        self.assertEqual(len(full),2048*96)
        self.assertEqual(full,batch_bytes(rows[::-1]))
        self.assertEqual(batch_bytes(rows,50,12),batch_bytes(rows[::-1],50,12))
        self.assertNotEqual(batch_bytes(rows,50,12),batch_bytes(rows,50,13))
        self.assertEqual(batch_bytes(rows,0),'')
        with self.assertRaisesRegex(ValueError,'4096'):
            batch_bytes(rows*3)

    def test_spatial_repaint_reuses_unaffected_chunks_and_shared_boundaries(self):
        raw=terrain()
        chunks=terrain_chunks(raw,8)
        self.assertEqual(len(chunks),16)
        modified=list(raw); p=array('f'); p.frombytes(raw[0]); p[(3*33+3)*3+2]=1
        modified[0]=pack_array(p)
        repainted=terrain_chunks(modified,8)
        self.assertEqual([c for c in chunks if chunks[c] != repainted[c]],[(0,0)])
        a={v for v in boundary(chunks[(0,0)],(0,0),8) if v[0] == 8}
        b={v for v in boundary(chunks[(1,0)],(1,0),8) if v[0] == 8}
        self.assertEqual(a,b)
        # A diagonal triangle crossing multiple cells is clipped, never copied whole.
        cross=list(triangle()); cross[0]=pack_array(array('f',[0,0,0,4,0,0,0,4,0]))
        split=terrain_chunks(cross,2)
        self.assertEqual(set(split),{(0,0),(0,1),(1,0)})
        for (x,y), mesh in split.items():
            p=array('f'); p.frombytes(mesh[0])
            self.assertTrue(all(x*2 <= a <= (x+1)*2 for a in p[0::3]))
            self.assertTrue(all(y*2 <= a <= (y+1)*2 for a in p[1::3]))

    def test_terrain_serialization_reuse_and_lod_boundary_refusal(self):
        director=FakeDirector(); publisher=Publisher(director)
        obj=dict(object_row(),terrain_size=8)
        raw=terrain()
        publisher.publish({'source':str(uuid.uuid4()),'objects':[obj]},{obj['id']:raw})
        count=publisher.serializations; uploaded=len(director.uploads)
        publisher.publish({'source':str(uuid.uuid4()),'objects':[obj]}, {})
        self.assertEqual(publisher.serializations,count)
        modified=list(raw); p=array('f'); p.frombytes(raw[0]); p[(3*33+3)*3+2]=1
        modified[0]=pack_array(p)
        publisher.publish({'source':str(uuid.uuid4()),'objects':[obj]},{obj['id']:tuple(modified)})
        self.assertEqual(publisher.serializations,count+1)
        self.assertEqual(len(director.uploads),uploaded+1)
        low=dict(obj,id=str(uuid.uuid4()),prototype=str(uuid.uuid4()),resource_only=True,terrain_size=0)
        obj['lod_id'],obj['lod_pixels']=low['id'],64
        p[0]=0.25; modified[0]=pack_array(p)
        with self.assertRaisesRegex(ValueError,'boundary'):
            publisher.publish({'source':str(uuid.uuid4()),'objects':[obj,low]},
                {obj['id']:raw,low['id']:tuple(modified)})


def fixture(directory):
    """Deterministic forest plus 16 terrain cells for CPU/GPU/cohort qualification."""
    directory.mkdir(parents=True,exist_ok=True)
    director=FakeDirector(); publisher=Publisher(director)
    forest=dict(object_row(),name='Forest',id=str(uuid.UUID(int=101)),prototype=str(uuid.UUID(int=102)))
    forest['batch']=batch_bytes([translated((x-32)*0.9,0,-y*0.9) for x in range(64) for y in range(32)])
    ground=dict(object_row(),id=str(uuid.UUID(int=103)),prototype=str(uuid.UUID(int=104)),
                name='Terrain',terrain_size=8,matrix=translated(-16,0,-1))
    # A tree silhouette, with a smaller authored LOD.
    vertices=[(-.4,0,0),(.4,0,0),(0,0,1.5), (0,-.4,0),(0,.4,0),(0,0,1.5)]
    tree=tuple(pack_array(array(k,v)) for k,v in (('f',[v for p in vertices for v in p]),('i',range(6)),
        ('f',[0,-1,0]*3+[1,0,0]*3),('i',range(6)),('i',[0,0]),('f',[])))
    low=dict(forest,id=str(uuid.UUID(int=105)),prototype=str(uuid.UUID(int=106)),resource_only=True,batch='')
    forest['lod_id'],forest['lod_pixels']=low['id'],64
    objects=[forest,ground,low]
    raw=terrain()
    publisher.publish({'source':str(uuid.UUID(int=100)),'objects':objects},{forest['id']:tree,ground['id']:raw,low['id']:triangle()})
    def save(name):
        for digest,data in director.uploads:
            (directory/(digest+'.glb')).write_bytes(data)
        (directory/name).write_text(manifest(str(uuid.UUID(int=100)),name,publisher.rows),encoding='utf-8')
    save('forest-r1.mslink')
    modified=list(raw); p=array('f'); p.frombytes(raw[0]); p[(3*33+3)*3+2]=1; modified[0]=pack_array(p)
    publisher.publish({'source':str(uuid.UUID(int=100)),'objects':objects},{ground['id']:tuple(modified)})
    save('forest-r2.mslink')
    forest['batch']=batch_bytes([translated((x-32)*0.9,0,-y*0.9) for x in range(64) for y in range(32)],75,2)
    publisher.publish({'source':str(uuid.UUID(int=100)),'objects':objects},{})
    save('forest-r3.mslink')


if __name__ == '__main__':
    if len(sys.argv)>1 and sys.argv[1]=='--fixture':
        fixture(Path(sys.argv[2]).resolve())
    else:
        unittest.main()
