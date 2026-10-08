"""Source tangent frames survive UV conversion and material/chunk splitting."""
from array import array
import json
import struct
import unittest
from unittest.mock import patch

from test_publisher_core import triangle, object_row, OBJECT
from test_materials_lighting import material, MAT_A
from geometry import glb, unpack_array
from surfaces import expand_surfaces, surface_mesh


def accessor(data, semantic):
    size, = struct.unpack_from('<I', data, 12)
    document = json.loads(data[20:20+size])
    primitive = document['meshes'][0]['primitives'][0]
    attribute = document['accessors'][primitive['attributes'][semantic]]
    view = document['bufferViews'][attribute['bufferView']]
    start = 28 + size + view.get('byteOffset', 0)
    return unpack_array('f', data[start:start+view['byteLength']])


class Tests(unittest.TestCase):
    def test_authored_tangents_rotate_without_reversing_handedness(self):
        raw = (*triangle(), array('f', [1, 0, 0, 1, 0, 1, 0, -1, 1, 0, 0, 1]).tobytes())
        _, data = glb(raw, object_row()['materials'])
        self.assertEqual(list(accessor(data, 'NORMAL')), [0, 1, 0] * 3)
        self.assertEqual(list(accessor(data, 'TEXCOORD_0')), [0, 1, 1, 1, 0, 0])
        self.assertEqual(list(accessor(data, 'TANGENT')), [1, 0, 0, 1, 0, 0, -1, -1, 1, 0, 0, 1])

    def test_chunk_boundary_keeps_the_source_tangent_frame(self):
        raw = triangle()
        raw = (*raw[:3], array('i', [0, 1, 2, 2, 1, 0]).tobytes(),
               array('i', [0, 0]).tobytes(), raw[5], array('f', [1, 0, 0, -1] * 3).tobytes())
        obj = object_row()
        obj['materials'] = [material(MAT_A, 'Scan')]
        with patch('surfaces.MAX_CORNERS', 3):
            objects, meshes = expand_surfaces([obj], {OBJECT: raw})
        parts = [o for o in objects if o['prototype']]
        self.assertEqual(len(parts), 2)
        for part in parts:
            _, data = glb(meshes[part['id']], part['materials'])
            self.assertEqual(list(accessor(data, 'TANGENT')), [1, 0, 0, -1] * 3)

    def test_welding_keeps_tangent_discontinuities(self):
        raw = triangle()
        raw = (raw[0], array('i', [0, 1, 2, 0, 1, 2]).tobytes(), raw[2]*2,
               array('i', range(6)).tobytes(), array('i', [0, 0]).tobytes(), raw[5]*2,
               array('f', [1, 0, 0, 1]*3 + [1, 0, 0, -1]*3).tobytes())
        welded = surface_mesh(raw, range(6))
        self.assertEqual(len(unpack_array('i', welded[1])), 6)
        self.assertEqual(welded[6], raw[6])

    def test_malformed_tangent_frames_are_rejected(self):
        for values in ([1, 0, 0, 1], [1, 0, 0, 0]*3, [float('nan'), 0, 0, 1]*3):
            with self.subTest(values=values), self.assertRaises(ValueError):
                glb((*triangle(), array('f', values).tobytes()), object_row()['materials'])


if __name__ == '__main__':
    unittest.main()
