"""Large imports retain every face, seam and stable material surface identity."""
from array import array
import copy
import json
import struct
import unittest

from test_publisher_core import triangle, object_row, FakeDirector, SOURCE, OBJECT
from test_materials_lighting import material, MAT_A
from geometry import MAX_CORNERS, unpack_array
from surfaces import expand_surfaces, surface_id
from translation import sync_feedback
from worker import Publisher


def large_mesh(faces=100_001):
    raw = triangle()
    # Repeated indexed faces exercise the resource limit without a huge fixture.
    return (*raw[:3], raw[3] * faces, raw[4] * faces, raw[5])


class Tests(unittest.TestCase):
    def test_large_import_splits_without_losing_faces_or_reuploading_on_move(self):
        obj = object_row()
        obj['materials'] = [material(MAT_A, 'Scan')]
        publisher = Publisher(FakeDirector())
        publisher.publish({'source': SOURCE, 'objects': [obj]}, {OBJECT: large_mesh()})
        self.assertEqual(len(publisher.prototypes), 2)
        counts = []
        for _, data in publisher.prototypes.values():
            length, = struct.unpack_from('<I', data, 12)
            doc = json.loads(data[20:20+length])
            counts.append(sum(doc['accessors'][p['indices']]['count'] // 3 for p in doc['meshes'][0]['primitives']))
        self.assertEqual(sorted(counts), [1, 100_000])
        self.assertIn(surface_id(OBJECT, MAT_A), publisher.prototypes)
        obj = copy.deepcopy(obj)
        obj['matrix'][12] = 8
        publisher.publish({'source': SOURCE, 'objects': [obj]}, {})
        self.assertEqual(publisher.serializations, 2)
        self.assertEqual(len(publisher.director.uploads), 2)
        publisher.publish({'source': SOURCE, 'objects': [obj]}, {OBJECT: triangle()})
        self.assertEqual(set(publisher.prototypes), {surface_id(OBJECT, MAT_A)})

    def test_large_mesh_without_assigned_material_is_split(self):
        objects, meshes = expand_surfaces([object_row()], {OBJECT: large_mesh()})
        self.assertEqual(len(meshes), 2)
        self.assertEqual(sum(len(m[3]) // 12 for m in meshes.values()), 100_001)
        self.assertTrue(all(len(m[3]) <= MAX_CORNERS * 4 for m in meshes.values()))

    def test_welding_keeps_normal_and_uv_seams(self):
        obj = object_row()
        obj['materials'] = [material(MAT_A, 'Scan')]
        raw = triangle()
        # Same source vertices, second face has a different UV and split normal.
        raw = (raw[0], raw[1]*2, raw[2] + array('f', [0,1,0]*3).tobytes(),
               array('i', range(6)).tobytes(), raw[4]*2, raw[5] + array('f', [0.5]*6).tobytes())
        _, meshes = expand_surfaces([obj], {OBJECT: raw})
        emitted = next(iter(meshes.values()))
        self.assertEqual(len(unpack_array('i', emitted[1])), 6)
        self.assertEqual(emitted[2], raw[2])
        self.assertEqual(emitted[5], raw[5])

    def test_geometry_failure_is_visible_ahead_of_shader_notices_even_on_noop(self):
        notices = ['Ignored shader input ' + str(i) for i in range(64)]
        failure = 'Scan: invalid evaluated geometry; skipped geometry'
        status, details = sync_feedback(False, notices, [failure], '1 instance')
        self.assertIn('Partial sync', status)
        self.assertIn('Scan', status)
        self.assertNotIn('up to date', status)
        self.assertEqual(details.splitlines()[0], failure)


if __name__ == '__main__':
    unittest.main()
