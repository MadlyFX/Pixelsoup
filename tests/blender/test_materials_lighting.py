"""Per-surface identity, image revisions, embedded GLB and lighting fixtures."""
from array import array
import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

from test_publisher_core import triangle, object_row, FakeDirector, SOURCE, OBJECT
from worker import Publisher
from surfaces import expand_surfaces, surface_id
from geometry import manifest, records

MAT_A='aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
MAT_B='bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb'


def png(color):
    def chunk(tag,data):
        return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',2,2,8,6,0,0,0))+chunk(b'IDAT',zlib.compress((b'\0'+bytes(color)*2)*2))+chunk(b'IEND',b'')


def material(identity,name,path=None):
    return {'id':identity,'name':name,'factors':[0.8,0.3,0.1,0.35,0.7,0.1,0.2,0.3],
        'textures':{'baseColorTexture':{'path':str(path),'wrap':10497,'filter':9729}} if path else {},
        'alpha':0.8,'alpha_mode':'MASK','alpha_cutoff':0.4,'double_sided':True,
        'extensions':{'KHR_materials_clearcoat':{'clearcoatFactor':0.6,'clearcoatRoughnessFactor':0.2},
                      'KHR_materials_transmission':{'transmissionFactor':0.25},
                      'KHR_materials_ior':{'ior':1.45}}}


def mesh():
    original=triangle()
    # Two separated, front-facing panels with separate materials and UV seams.
    return (array('f',[-2,0,0, -0.1,0,0, -1,0,2, 0.1,0,0, 2,0,0, 1,0,2]).tobytes(),
        array('i',range(6)).tobytes(),array('f',[0,-1,0]*6).tobytes(),array('i',range(6)).tobytes(),
        array('i',[0,1]).tobytes(),original[5]*2)


def fixture(directory):
    directory=Path(directory).resolve(); directory.mkdir(parents=True,exist_ok=True)
    path=directory/'color.png'; path.write_bytes(png([220,110,50,255]))
    obj=object_row(); obj['name']='Material panels'
    obj['materials']=[material(MAT_A,'Copper',path),material(MAT_B,'Paint')]
    light=dict(object_row(),id='cccccccc-cccc-4ccc-8ccc-cccccccccccc',prototype='',name='Key rectangle',materials=[])
    light['matrix'][12:15]=[0,3,2]
    light['light']={'kind':'rectangle','unit':'lumens','values':[1,0.8,0.6,1800,20,0,0.125,2,1,0]}
    sun=dict(light,id='dddddddd-dddd-4ddd-8ddd-dddddddddddd',name='Sun',light={'kind':'directional','unit':'lux','values':[1,1,1,400,20,0,0.125,1,1,0.001]})
    obj['materials'][0]['factors'][5:8]=[0,0,0]
    obj['materials'][1]['factors']=[0.2,0.65,0.1,0.35,0,0,0,0]
    sun['matrix']=[1,0,0,0,0,0.6,0.8,0,0,-0.8,0.6,0,0,3,2,1]
    obj['materials'][1]['alpha_mode']='OPAQUE'
    obj['materials'][1]['alpha']=1
    obj['materials'][1]['extensions']['KHR_materials_transmission']['transmissionFactor']=0
    director=FakeDirector(); publisher=Publisher(director)
    job={'source':SOURCE,'objects':[obj,light,sun]}
    for revision in range(1,4):
        raw=mesh()
        if revision==2:
            path.write_bytes(png([30,180,240,255]))
        if revision==3:
            obj['materials'].reverse()
            raw=(*raw[:4],array('i',[1,0]).tobytes(),raw[5])
        publisher.publish(job,{OBJECT:raw})
        for digest,data in publisher.prototypes.values():
            (directory/(digest+'.glb')).write_bytes(data)
        (directory/f'look-r{revision}.mslink').write_text(manifest(SOURCE,f'look-{revision}',publisher.rows))
    return directory


class Tests(unittest.TestCase):
    def test_missing_uv_and_corrupt_texture_keep_geometry(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'broken.png'; path.write_bytes(b'not an image')
            obj=object_row(); obj['materials']=[material(MAT_A,'A',path)]
            publisher=Publisher(FakeDirector())
            result=publisher.publish({'source':SOURCE,'objects':[obj]}, {OBJECT:triangle()})
            self.assertTrue(result['warnings'])
            self.assertFalse(publisher.materials[surface_id(OBJECT,MAT_A)][0]['textures'])
            path.write_bytes(png([255,0,0,255]))
            raw=(*triangle()[:5],b'')
            result=publisher.publish({'source':SOURCE,'objects':[obj]}, {OBJECT:raw})
            self.assertTrue(result['warnings'])
            self.assertTrue(publisher.prototypes)
            self.assertFalse(publisher.materials[surface_id(OBJECT,MAT_A)][0]['textures'])

    def test_slot_reorder_is_identity_preserving(self):
        obj=object_row();obj['materials']=[material(MAT_A,'A'),material(MAT_B,'B')]
        objects,raw=expand_surfaces([obj],{OBJECT:mesh()})
        obj=copy.deepcopy(obj);obj['materials'].reverse()
        reordered=(*mesh()[:4],array('i',[1,0]).tobytes(),mesh()[5])
        other,new=expand_surfaces([obj],{OBJECT:reordered})
        self.assertEqual(objects,other);self.assertEqual(raw,new)
        self.assertIn(surface_id(OBJECT,MAT_A),raw)

    def test_texture_edit_only_rebuilds_affected_surface(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'color.png';path.write_bytes(png([255,0,0,255]))
            obj=object_row();obj['materials']=[material(MAT_A,'A',path),material(MAT_B,'B')]
            director=FakeDirector();publisher=Publisher(director)
            job={'source':SOURCE,'objects':[obj]};publisher.publish(job,{OBJECT:mesh()})
            old=dict(publisher.prototypes);path.write_bytes(png([0,0,255,255]))
            result=publisher.publish(job,{})
            self.assertEqual(publisher.serializations,3)
            self.assertEqual(old[surface_id(OBJECT,MAT_B)],publisher.prototypes[surface_id(OBJECT,MAT_B)])
            self.assertNotEqual(old[surface_id(OBJECT,MAT_A)],publisher.prototypes[surface_id(OBJECT,MAT_A)])
            self.assertEqual(result['transaction_kind'],'delta')
            _,data=publisher.prototypes[surface_id(OBJECT,MAT_A)]
            length,=struct.unpack_from('<I',data,12);doc=json.loads(data[20:20+length])
            self.assertEqual(doc['materials'][0]['name'],MAT_A)
            self.assertEqual(doc['materials'][0]['alphaMode'],'MASK')
            self.assertIn('bufferView',doc['images'][0])
            self.assertNotIn('uri',doc['images'][0])
            path.unlink()
            result=publisher.publish(job,{})
            self.assertTrue(result['warnings'])
            self.assertFalse(publisher.materials[surface_id(OBJECT,MAT_A)][0]['textures'])


if __name__=='__main__':
    if '--fixture' in sys.argv:
        print(fixture(sys.argv[sys.argv.index('--fixture')+1]))
    else:
        unittest.main()
