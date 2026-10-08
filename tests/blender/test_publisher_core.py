"""Standard-library tests for the process-side publisher and binary contract."""
from array import array
import copy
import hashlib
import json
from pathlib import Path
import socket
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'integrations/blender/mediaserver_livelink'))
import geometry
import wire
from worker import Publisher

SOURCE = '11111111-1111-4111-8111-111111111111'
OBJECT = '22222222-2222-4222-8222-222222222222'
PROTO = '33333333-3333-4333-8333-333333333333'


def triangle():
    return tuple(array(kind, values).tobytes() for kind, values in (
        ('f', [0, 0, 0, 1, 0, 0, 0, 1, 0]), ('i', [0, 1, 2]),
        ('f', [0, 0, 1] * 3), ('i', [0, 1, 2]), ('i', [0]), ('f', [0, 0, 1, 0, 0, 1])))


def object_row():
    return {'id': OBJECT, 'prototype': PROTO, 'name': 'Prop é "quoted"', 'parent': '',
            'visible': True, 'matrix': [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
            'materials': [[0.8, 0.2, 0.1, 0.5, 0.0, 0, 0, 0]]}


class FakeDirector:
    # The real refusal, so its wording is covered by these tests too.
    require = wire.Director.require

    def __init__(self):
        self.base = ''
        self.uploads = []
        self.transactions = []
        self.reject = False
        self.capabilities = wire.CAPABILITIES

    def upload(self, digest, data):
        assert hashlib.sha256(data).hexdigest() == digest
        self.uploads.append((digest, data))

    def publish(self, kind, revision, payload):
        if self.reject:
            raise RuntimeError('stale_revision')
        self.base = revision
        self.transactions.append((kind, payload))
        return 'accepted_in_preview; published_to_program=false'


class Tests(unittest.TestCase):
    def test_pairing_details_require_local_endpoint_and_complete_code(self):
        details = 'Waiting\nEndpoint: 127.0.0.1:4836\nPairing code: ' + 'a' * 64 + '\n\nLocal only'
        self.assertEqual(wire.pairing_details(details), (4836, 'a' * 64))
        with self.assertRaises(ValueError):
            wire.pairing_details(details.replace('127.0.0.1', '192.0.2.1'))
        with self.assertRaises(ValueError):
            wire.pairing_details(details.replace('a' * 64, 'a' * 63))

    def test_geometry_coordinate_normal_uv_and_glb_layout(self):
        digest, data = geometry.glb(triangle(), object_row()['materials'])
        self.assertEqual(digest, hashlib.sha256(data).hexdigest())
        self.assertEqual(struct.unpack_from('<4sII', data), (b'glTF', 2, len(data)))
        size, = struct.unpack_from('<I', data, 12)
        doc = json.loads(data[20:20 + size])
        self.assertEqual(doc['nodes'], [{'mesh': 0}])
        binary = data[28 + size:]
        positions = struct.unpack_from('<9f', binary)
        self.assertEqual(positions, (0, 0, 0, 1, 0, 0, 0, 0, -1))
        self.assertEqual(struct.unpack_from('<9f', binary, 36), (0, 1, 0) * 3)
        self.assertEqual(struct.unpack_from('<6f', binary, 72), (0, 1, 1, 1, 0, 0))

    def test_transform_and_rename_do_not_serialize_or_upload_mesh(self):
        director = FakeDirector()
        publisher = Publisher(director)
        obj = object_row()
        publisher.publish({'source': SOURCE, 'objects': [obj]}, {OBJECT: triangle()})
        obj = copy.deepcopy(obj)
        obj['matrix'][12] = 4
        obj['name'] = 'Renamed'
        result = publisher.publish({'source': SOURCE, 'objects': [obj]}, {})
        self.assertEqual(publisher.serializations, 1)
        self.assertEqual(len(director.uploads), 1)
        self.assertEqual(director.transactions[-1][0], wire.DELTA)
        self.assertNotIn('prototype ', director.transactions[-1][1])
        self.assertLess(result['payload_bytes'], 500)

    def test_material_only_rebuilds_glb_from_retained_copy(self):
        director = FakeDirector()
        publisher = Publisher(director)
        obj = object_row()
        publisher.publish({'source': SOURCE, 'objects': [copy.deepcopy(obj)]}, {OBJECT: triangle()})
        obj['materials'][0][0] = 0.3
        publisher.publish({'source': SOURCE, 'objects': [obj]}, {})
        self.assertEqual(publisher.serializations, 2)
        self.assertEqual(len(director.uploads), 2)
        self.assertIn('upsert prototype', director.transactions[-1][1])

    def test_delete_restore_and_unchanged_are_atomic(self):
        director = FakeDirector()
        publisher = Publisher(director)
        job = {'source': SOURCE, 'objects': [object_row()]}
        publisher.publish(job, {OBJECT: triangle()})
        publisher.publish(job, {})
        self.assertEqual(len(director.transactions), 1)
        publisher.publish({'source': SOURCE, 'objects': []}, {})
        self.assertIn('erase object', director.transactions[-1][1])
        self.assertFalse(publisher.raw)
        publisher.publish(job, {OBJECT: triangle()})
        self.assertIn(OBJECT, director.transactions[-1][1])
        self.assertEqual(len(director.uploads), 1)

    def test_stale_rejection_does_not_advance_baseline(self):
        director = FakeDirector()
        publisher = Publisher(director)
        publisher.publish({'source': SOURCE, 'objects': [object_row()]}, {OBJECT: triangle()})
        before = publisher.rows
        director.reject = True
        with self.assertRaisesRegex(RuntimeError, 'stale_revision'):
            publisher.publish({'source': SOURCE, 'objects': []}, {})
        self.assertIs(publisher.rows, before)

    def test_quoted_unicode_control_and_capacity(self):
        self.assertEqual(geometry.quoted('é"\\'), '"é\\"\\\\"')
        with self.assertRaises(ValueError):
            geometry.quoted('invalid\nname')
        with self.assertRaises(ValueError):
            geometry.floats([float('nan')])
        with self.assertRaises(ValueError):
            wire.encode({'kind': wire.HELLO, 'origin': 'x' * 129})
        with self.assertRaises(ValueError):
            wire.encode({'kind': wire.SNAPSHOT, 'payload': b'x' * wire.PACKET_LIMIT})

    def test_wire_round_trip_and_truncated_reply(self):
        left, right = socket.socketpair()
        try:
            packet = wire.encode({'kind': wire.ACK, 'revision': 'revision', 'payload': 'accepted'})
            left.sendall(packet)
            self.assertEqual(wire.receive(right)['revision'], 'revision')
            left.sendall(packet[:10])
            left.close()
            with self.assertRaises(ConnectionError):
                wire.receive(right)
        finally:
            left.close()
            right.close()

    def test_capability_declaration_and_welcome_parsing(self):
        self.assertEqual(wire.welcome_capabilities('version=1; preview_only; capabilities=base,shot'),
                         ('base', 'shot'))
        # A Director that predates negotiation advertises nothing.
        self.assertEqual(wire.welcome_capabilities('version=1; preview_only; one_inflight'),
                         wire.BASELINE_CAPABILITIES)
        self.assertEqual(wire.welcome_capabilities('capabilities=' + ','.join(wire.CAPABILITIES)),
                         wire.CAPABILITIES)
        self.assertTrue(wire.client_identity().startswith('mediaserver_livelink/'))
        self.assertLessEqual(len(wire.client_identity('blender/' + 'x' * 200).encode('ascii')), 96)

    def test_unsupported_feature_refuses_before_any_upload(self):
        director = FakeDirector()
        director.capabilities = wire.BASELINE_CAPABILITIES
        publisher = Publisher(director)
        with self.assertRaisesRegex(wire.CapabilityError, 'animated assemblies'):
            publisher.publish({'source': SOURCE, 'animated_package': 'absent.glb', 'assembly': {}}, {})
        scattered = copy.deepcopy(object_row())
        scattered['batch'] = 'x' * 96
        with self.assertRaisesRegex(wire.CapabilityError, 'foliage'):
            publisher.negotiate([scattered])
        lit = copy.deepcopy(object_row())
        lit['light'] = {'kind': 'point', 'unit': 'lm', 'values': [1000.0]}
        with self.assertRaisesRegex(wire.CapabilityError, 'native linked lights'):
            publisher.negotiate([lit])
        self.assertEqual(director.uploads, [])
        self.assertEqual(director.transactions, [])
        director.capabilities = wire.CAPABILITIES
        publisher.negotiate([scattered, lit])

    def test_installed_metadata_matches_the_wire(self):
        import ast
        module = Path(__file__).resolve().parents[2] / 'integrations/blender/mediaserver_livelink/__init__.py'
        info = next(ast.literal_eval(node.value) for node in
                    ast.parse(module.read_text(encoding='utf-8')).body
                    if isinstance(node, ast.Assign) and getattr(node.targets[0], 'id', '') == 'bl_info')
        self.assertEqual(tuple(info['version']), wire.ADDON_VERSION)
        self.assertEqual(tuple(info['blender']), wire.BLENDER_REQUIRED)

    def test_invalid_mesh_does_not_create_glb(self):
        mesh = list(triangle())
        mesh[3] = array('i', [0, 1, 900]).tobytes()
        with self.assertRaisesRegex(ValueError, 'triangle corner'):
            geometry.glb(mesh, object_row()['materials'])


if __name__ == '__main__':
    unittest.main()
