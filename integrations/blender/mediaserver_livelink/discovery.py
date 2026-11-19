"""Same-user local discovery. No sockets or Blender API calls off the main thread.

Each enabled Blender advertises a short-lived instance record: its label, the
scene it shows, its status, the open .blend, the launch token Pixelsoup gave
the process when Open in Blender started it, its process id, and - since one
.blend holds a whole Pixelsoup project, each Blender scene tagged with its
Pixelsoup scene's source identity - the source of the scene shown, every
tagged scene in the file, and a scene the artist asked to add to Pixelsoup.

Pixelsoup answers with a scene list (a pairing per linked scene) and may ask
to show a scene. Both files are consumed and deleted; pairing secrets are never
stored in the blend file or advertised.
"""
import os
from pathlib import Path
import time
import uuid


def directory():
    base = Path(os.environ['LOCALAPPDATA']) if os.name == 'nt' else Path.home() / '.cache'
    return base / 'Pixelsoup' / 'blender'


def launch_token():
    token = os.environ.get('MEDIASERVER_LIVELINK_TOKEN', '')
    return token if len(token) == 32 and all(c in '0123456789abcdef' for c in token) else ''


def valid_source(value):
    try:
        return str(uuid.UUID(value)) == value and uuid.UUID(value).int != 0
    except (ValueError, TypeError, AttributeError):
        return False


class Discovery:
    def __init__(self, root=None):
        self.root = Path(root) if root else directory()
        self.identity = uuid.uuid4().hex
        self.last_write = 0
        self.enabled = False

    def start(self):
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.enabled = True

    def stop(self):
        self.enabled = False
        for suffix in ('.instance', '.links', '.activate', '.tmp'):
            (self.root / (self.identity + suffix)).unlink(missing_ok=True)

    def consume(self, suffix, magic):
        path = self.root / (self.identity + suffix)
        try:
            if path.stat().st_size > 16384 or time.time() - path.stat().st_mtime > 60:
                path.unlink(missing_ok=True)
                return None
            fields = path.read_text(encoding='utf-8').splitlines()
            path.unlink(missing_ok=True)
        except (FileNotFoundError, PermissionError):
            return None
        return fields[1:] if fields and fields[0] == magic else None

    def scene_links(self):
        """The Director's scene list: source -> pairing, or None if unchanged."""
        lines = self.consume('.links', 'MSLINKS 1')
        if lines is None:
            return None
        links = {}
        for line in lines[:64]:
            fields = line.split('\t')
            if len(fields) != 4 or not valid_source(fields[0]):
                continue
            try:
                port = int(fields[1])
            except ValueError:
                continue
            if 1024 <= port <= 65535 and len(fields[2]) == 64 and all(c in '0123456789abcdef' for c in fields[2]):
                links[fields[0]] = {'port': port, 'code': fields[2], 'name': fields[3].strip()[:256]}
        return links

    def activation(self):
        """A scene the Director asks Blender to show, or None."""
        lines = self.consume('.activate', 'MSACTIVATE 1')
        if not lines or not valid_source(lines[0]):
            return None
        return {'source': lines[0], 'name': (lines[1].strip() if len(lines) > 1 else '')[:256],
                'adopt': len(lines) > 2 and lines[2].strip() == 'adopt'}

    def advertise(self, label, scene, status, resync=False, file='', active='', sources=(), create=None):
        """Rewrite the instance record about once a second."""
        if not self.enabled:
            return
        now = time.monotonic()
        if now - self.last_write < 1:
            return
        clean = lambda s: str(s).replace('\n', ' ').replace('\r', ' ')[:512]
        temporary = self.root / (self.identity + '.tmp')
        fields = (label, scene, status, 'resync' if resync else 'ready', file, launch_token(), os.getpid(),
                  active if valid_source(active) else '',
                  ','.join(s for s in list(sources)[:64] if valid_source(s)),
                  (create[0] + '\t' + clean(create[1]).replace('\t', ' ')) if create and valid_source(create[0]) else '')
        try:
            temporary.write_text('MSBLENDER 2\n' + '\n'.join(map(clean, fields)) + '\n', encoding='utf-8')
            temporary.replace(self.root / (self.identity + '.instance'))
            self.last_write = now
        except PermissionError:
            # Windows refuses the replace while a reader has the record open;
            # the next call writes it again.
            pass


discovery = Discovery()
