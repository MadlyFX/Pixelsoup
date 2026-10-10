"""Local discovery: instance records, scene lists and activation requests."""
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'integrations/blender/mediaserver_livelink'))
from discovery import Discovery

DESERT = '3f2504e0-4f89-41d3-9a0c-0305e82c3301'
NIGHT = '9c858901-8a57-4791-81fe-4c455b099bc9'
ADDED = 'f47ac10b-58cc-4372-a567-0e02b2c3d479'


class Tests(unittest.TestCase):
    def record(self, root, instance):
        return (Path(root) / (instance.identity + '.instance')).read_text(encoding='utf-8').splitlines()

    def test_record_names_file_launch_token_and_process(self):
        with tempfile.TemporaryDirectory() as root:
            instance = Discovery(root); instance.start()
            token = '0123456789abcdef' * 2
            previous = os.environ.get('MEDIASERVER_LIVELINK_TOKEN')
            os.environ['MEDIASERVER_LIVELINK_TOKEN'] = token
            try:
                instance.advertise('Desert.blend', 'Scene', 'Ready', file='C:/Shows/Desert.blend')
            finally:
                if previous is None:
                    del os.environ['MEDIASERVER_LIVELINK_TOKEN']
                else:
                    os.environ['MEDIASERVER_LIVELINK_TOKEN'] = previous
            self.assertEqual(self.record(root, instance), ['MSBLENDER 2', 'Desert.blend', 'Scene', 'Ready', 'ready',
                             'C:/Shows/Desert.blend', token, str(os.getpid()), '', '', ''])
            instance.stop()

    def test_record_names_every_linked_scene_and_a_scene_to_add(self):
        with tempfile.TemporaryDirectory() as root:
            instance = Discovery(root); instance.start()
            instance.advertise('Show.blend', 'Desert\nnight', 'Ready', file='C:/Shows/Show.blend', active=DESERT,
                               sources=(DESERT, NIGHT, 'bogus'), create=(ADDED, 'Sunrise\tdawn'))
            fields = self.record(root, instance)
            self.assertEqual(fields[2], 'Desert night')
            self.assertEqual(fields[8:], [DESERT, DESERT + ',' + NIGHT, ADDED + '\tSunrise dawn'])
            # The record is rewritten about once a second, not on every call.
            instance.advertise('Show.blend', 'Other', 'Ready')
            self.assertEqual(self.record(root, instance)[2], 'Desert night')
            instance.stop()

    def test_scene_list_and_activation_are_consumed(self):
        with tempfile.TemporaryDirectory() as root:
            first, second = Discovery(root), Discovery(root)
            first.start(); second.start()
            links = Path(root) / (second.identity + '.links')
            links.write_text('MSLINKS 1\n' + DESERT + '\t45001\t' + 'a' * 64 + '\tDesert\n'
                             + NIGHT + '\t45002\t' + 'b' * 64 + '\tNight\n'
                             + 'bogus\t45003\t' + 'c' * 64 + '\tBad\n' + NIGHT + '\t80\t' + 'd' * 64 + '\tLow port\n')
            self.assertIsNone(first.scene_links())
            self.assertEqual(second.scene_links(), {
                DESERT: {'port': 45001, 'code': 'a' * 64, 'name': 'Desert'},
                NIGHT: {'port': 45002, 'code': 'b' * 64, 'name': 'Night'}})
            self.assertFalse(links.exists())
            self.assertIsNone(second.scene_links())
            # An empty list unlinks every scene.
            links.write_text('MSLINKS 1\n')
            self.assertEqual(second.scene_links(), {})
            activate = Path(root) / (second.identity + '.activate')
            activate.write_text('MSACTIVATE 1\n' + NIGHT + '\nNight\nshow\n')
            self.assertEqual(second.activation(), {'source': NIGHT, 'name': 'Night', 'adopt': False})
            self.assertFalse(activate.exists())
            activate.write_text('MSACTIVATE 1\n' + NIGHT + '\nNight\nadopt\n')
            self.assertTrue(second.activation()['adopt'])
            activate.write_text('MSACTIVATE 1\nnot-a-source\nNight\nshow\n')
            self.assertIsNone(second.activation())
            first.stop(); second.stop()
            self.assertFalse(list(Path(root).iterdir()))

    def test_stale_or_oversized_files_are_discarded(self):
        with tempfile.TemporaryDirectory() as root:
            instance = Discovery(root); instance.start()
            links = Path(root) / (instance.identity + '.links')
            links.write_text('MSLINKS 1\n' + DESERT + '\t45001\t' + 'a' * 64 + '\tDesert\n')
            old = time.time() - 120
            os.utime(links, (old, old))
            self.assertIsNone(instance.scene_links())
            self.assertFalse(links.exists())
            links.write_text('MSLINKS 1\n' + 'x' * 20000)
            self.assertIsNone(instance.scene_links())
            self.assertFalse(links.exists())
            instance.stop()


if __name__ == '__main__':
    unittest.main()
