"""Portable node/skin GLB fixture and stage wire validation."""
from array import array
import hashlib
import json
from pathlib import Path
import struct
import sys
import unittest

from test_publisher_core import triangle, object_row, SOURCE, OBJECT
from geometry import glb, records, manifest
from stage import parse


def fixture(directory):
    directory=Path(directory);directory.mkdir(parents=True,exist_ok=True)
    _, raw=glb(triangle(),[[0.8,0.2,0.05,0.5,0,0,0,0]])
    size,=struct.unpack_from('<I',raw,12)
    doc=json.loads(raw[20:20+size]);binary=bytearray(raw[28+size:])
    def accessor(values,shape,width,kind='f',component=5126):
        binary.extend(b'\0'*(-len(binary)%4))
        data=array(kind,values).tobytes();view=len(doc['bufferViews'])
        doc['bufferViews'].append({'buffer':0,'byteOffset':len(binary),'byteLength':len(data)})
        binary.extend(data);index=len(doc['accessors'])
        doc['accessors'].append({'bufferView':view,'componentType':component,'count':len(values)//width,'type':shape})
        return index
    joints=accessor([0]*12,'VEC4',4,'H',5123)
    weights=accessor([1,0,0,0]*3,'VEC4',4)
    inverse=accessor([float(i%5==0) for i in range(16)],'MAT4',16)
    times=accessor([0,1],'SCALAR',1)
    values=accessor([0,0,0,2,0,0],'VEC3',3)
    doc['meshes'][0]['primitives'][0]['attributes'].update(JOINTS_0=joints,WEIGHTS_0=weights)
    doc['nodes']=[{'name':'Animated parent','children':[1,2,3]},
                  {'name':'Rigid child','mesh':0,'translation':[-1,0,0]},
                  {'name':'Joint'}, {'name':'Skin','mesh':0,'skin':0,'translation':[1,0,0]}]
    doc['skins']=[{'joints':[2],'inverseBindMatrices':inverse}]
    doc['animations']=[{'name':'One second','samplers':[{'input':times,'output':values,'interpolation':'LINEAR'}],
                        'channels':[{'sampler':0,'target':{'node':0,'path':'translation'}}]}]
    doc['buffers']=[{'byteLength':len(binary)}]
    text=json.dumps(doc,separators=(',',':')).encode();text+=b' '*(-len(text)%4);binary.extend(b'\0'*(-len(binary)%4))
    data=struct.pack('<4sII',b'glTF',2,28+len(text)+len(binary))+struct.pack('<I4s',len(text),b'JSON')+text+struct.pack('<I4s',len(binary),b'BIN\0')+binary
    digest=hashlib.sha256(data).hexdigest();(directory/(digest+'.glb')).write_bytes(data)
    obj=object_row();obj.update(name='Animated assembly',animation={'start':10,'loop':True},materials=[])
    obj['matrix']=[float(i%5==0) for i in range(16)]
    (directory/'animation.mslink').write_text(manifest(SOURCE,'animation-v1',records([obj],{OBJECT:(digest,data)})))


class StageTests(unittest.TestCase):
    def payload(self):
        identity=' '.join(str(int(i%5==0)) for i in range(16))
        return f'MSSTAGE 1\n1 100 42 24000 1001 2 1 0 1\n{identity}\n{identity}\n36 24 50 0.1 1000 5 2.8\n'

    def test_rational_sample(self):
        sample=parse(self.payload())
        self.assertEqual(sample['header'][2:5],[42,24000,1001])

    def test_invalid_sample(self):
        for data in (self.payload().replace('24000','0'),self.payload().replace('36 24','nan 24'),
                     self.payload().replace('0.1 1000','1000 0.1'),self.payload()+'extra'):
            with self.assertRaises(ValueError):parse(data)


if __name__=='__main__':
    if len(sys.argv)==3 and sys.argv[1]=='--fixture':fixture(sys.argv[2])
    else:unittest.main()
