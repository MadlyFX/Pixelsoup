"""Worker-side image preparation using Blender's bundled codecs in isolation."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time


def dimensions(path):
    """Read PNG/JPEG dimensions without decoding pixels in the authoring UI."""
    with Path(path).open('rb') as stream:
        header = stream.read(24)
        if header.startswith(b'\x89PNG\r\n\x1a\n') and header[12:16] == b'IHDR' and len(header) == 24:
            return struct.unpack('>II', header[16:24])
        if header[:2] != b'\xff\xd8':
            raise ValueError('Image is not a PNG/JPEG')
        stream.seek(2)
        while stream.tell() < 1024 * 1024:
            marker = stream.read(1)
            if marker != b'\xff':
                raise ValueError('Invalid JPEG marker')
            while marker == b'\xff':
                marker = stream.read(1)
            if not marker or marker in (b'\xda', b'\xd9'):
                break
            if marker == b'\x01' or 0xd0 <= marker[0] <= 0xd7:
                continue
            length = stream.read(2)
            if len(length) != 2:
                break
            size, = struct.unpack('>H', length)
            if size < 2:
                break
            if marker[0] in (0xc0, 0xc1, 0xc2):
                frame = stream.read(5)
                if len(frame) == 5:
                    height, width = struct.unpack('>HH', frame[1:])
                    return width, height
                break
            stream.seek(size - 2, 1)
    raise ValueError('Image dimensions unavailable')


def resize_images(paths, blender, progress=lambda message: None, keepalive=lambda: None):
    if not paths or not blender:
        return {}
    progress('Preparing ' + str(len(paths)) + ' large textures...')
    # A separate empty Blender process supplies the codecs already installed
    # with the add-on. It never opens or saves the user's scene or source images.
    with tempfile.TemporaryDirectory(prefix='ms-link-images-') as directory:
        root = Path(directory)
        job = root / 'images.json'
        job.write_text(json.dumps(list(paths)), encoding='utf-8')
        try:
            process = subprocess.Popen([blender, '--background', '--factory-startup', '--threads', '2',
                '--python-exit-code', '1', '--python', str(Path(__file__).resolve()), '--', str(job)],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            try:
                deadline = time.monotonic() + 180
                while process.poll() is None:
                    keepalive()
                    done = len(list(root.glob('*.png')))
                    progress('Preparing large textures: ' + str(done) + '/' + str(len(paths)))
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        if time.monotonic() > deadline:
                            break
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
        except (OSError, subprocess.SubprocessError):
            # Keep any completed conversions; failed textures use material color.
            pass
        return {path: (root / (str(index) + '.png')).read_bytes() for index, path in enumerate(paths)
                if (root / (str(index) + '.png')).exists() and (root / (str(index) + '.png')).stat().st_size <= 16*1024*1024}


def main():
    import bpy
    job = Path(sys.argv[sys.argv.index('--') + 1])
    for index, path in enumerate(json.loads(job.read_text(encoding='utf-8'))):
        image = None
        try:
            image = bpy.data.images.load(path, check_existing=False)
            width, height = image.size
            if min(width, height) < 1 or max(width, height) > 32768:
                continue
            ratio = min(1, 1024 / max(width, height))
            # No render/view transform or exposure is applied to texture data.
            image.scale(max(1, round(width * ratio)), max(1, round(height * ratio)))
            image.filepath_raw = str(job.parent / (str(index) + '.png'))
            image.file_format = 'PNG'
            image.save()
        except (RuntimeError, ValueError, OSError):
            pass
        finally:
            if image is not None:
                bpy.data.images.remove(image)


if __name__ == '__main__':
    main()
