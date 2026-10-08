"""Image preparation stays outside bpy and never replaces source texture files."""
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from test_publisher_core import object_row
from test_materials_lighting import material, MAT_A, png
from image_resize import dimensions
from surfaces import resolve_materials


class Tests(unittest.TestCase):
    def test_dimensions_read_without_pixel_decoder(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'image.png'
            path.write_bytes(png([1, 2, 3, 255]))
            self.assertEqual(dimensions(path), (2, 2))
            path.write_bytes(b'\xff\xd8\xff\xc0' + struct.pack('>HBHH', 7, 8, 100, 200))
            self.assertEqual(dimensions(path), (200, 100))
            path.write_bytes(b'corrupt image')
            with self.assertRaises(ValueError):
                dimensions(path)

    def test_large_texture_uses_export_copy_and_reuses_it_until_source_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'large.png'
            original = b'\x89PNG\r\n\x1a\n' + struct.pack('>I', 13) + b'IHDR' + struct.pack('>II', 8192, 8192)
            path.write_bytes(original)
            obj = object_row()
            obj['materials'] = [material(MAT_A, 'Scan', path)]
            cache, warnings = {}, []
            converted = png([100, 200, 50, 255])
            with patch('image_resize.resize_images', return_value={str(path): converted}) as prepare:
                result = resolve_materials([obj], cache, warnings=warnings, blender='blender')
                self.assertEqual(result[0]['materials'][0]['textures']['baseColorTexture']['bytes'], converted)
                self.assertTrue(warnings)
                self.assertEqual(path.read_bytes(), original)
                result = resolve_materials([obj], cache, blender='blender')
                self.assertFalse(prepare.call_args.args[0])
                resolve_materials([obj], cache, refresh=True, blender='blender')
                self.assertIn(str(path), prepare.call_args.args[0])

    def test_failed_conversion_keeps_material_without_texture(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'large.png'
            path.write_bytes(b'\x89PNG\r\n\x1a\n' + struct.pack('>I', 13) + b'IHDR' + struct.pack('>II', 8192, 8192))
            obj = object_row()
            obj['materials'] = [material(MAT_A, 'Scan', path)]
            warnings = []
            with patch('image_resize.resize_images', return_value={}):
                result = resolve_materials([obj], warnings=warnings, blender='blender')
            self.assertTrue(result[0]['prototype'])
            self.assertFalse(result[0]['materials'][0]['textures'])
            self.assertTrue(warnings)


if __name__ == '__main__':
    unittest.main()
