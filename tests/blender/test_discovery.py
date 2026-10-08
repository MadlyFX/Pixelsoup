"""Local discovery lifecycle, independent instances and bounded commands."""
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'integrations/blender/mediaserver_livelink'))
from discovery import Discovery


class Tests(unittest.TestCase):
    def test_selection_consumes_secret_without_advertising_it(self):
        with tempfile.TemporaryDirectory() as root:
            first, second = Discovery(root), Discovery(root)
            first.start(); second.start()
            first.poll('First\nfile', 'Scene', 'Ready')
            second.poll('Second', 'Scene', 'Ready')
            request = Path(root) / (second.identity + '.request')
            code = 'a' * 64
            request.write_text('MSCONNECT 1\nconnect\n45001\n' + code + '\n')
            self.assertIsNone(first.poll('First', 'Scene', 'Ready'))
            self.assertEqual(second.poll('Second', 'Scene', 'Ready'), {'port': 45001, 'code': code})
            self.assertFalse(request.exists())
            self.assertIsNone(second.poll('Second', 'Scene', 'Ready'))
            self.assertNotIn(code, (Path(root) / (second.identity + '.instance')).read_text())
            first.stop(); second.stop()
            self.assertFalse(list(Path(root).iterdir()))

    def test_invalid_and_expired_requests_do_not_connect(self):
        with tempfile.TemporaryDirectory() as root:
            instance = Discovery(root); instance.start()
            request = Path(root) / (instance.identity + '.request')
            for content in ('x' * 4096, 'MSCONNECT 2\nconnect\n45001\n' + 'a' * 64,
                            'MSCONNECT 1\nconnect\n99999\n' + 'a' * 64,
                            'MSCONNECT 1\nconnect\n45001\ninvalid'):
                request.write_text(content)
                self.assertIsNone(instance.poll('File', 'Scene', 'Ready'))
            request.write_text('MSCONNECT 1\nconnect\n45001\n' + 'a' * 64)
            os.utime(request, (time.time() - 60, time.time() - 60))
            self.assertIsNone(instance.poll('File', 'Scene', 'Ready'))
            request.write_text('MSCONNECT 1\ndisconnect\n')
            self.assertEqual(instance.poll('File', 'Scene', 'Ready'), {'disconnect': True})
            instance.stop()


if __name__ == '__main__':
    unittest.main()
