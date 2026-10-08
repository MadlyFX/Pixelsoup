"""Same-user local discovery. No sockets or Blender API calls off the main thread.

Each enabled Blender advertises a short-lived instance record. Media Server
atomically places a single connection request beside it; secrets are consumed
and deleted, and never stored in the blend file or advertisement.
"""
import os
from pathlib import Path
import time
import uuid


def directory():
    base = Path(os.environ['LOCALAPPDATA']) if os.name == 'nt' else Path.home() / '.cache'
    return base / 'MediaServer' / 'blender'


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
        for suffix in ('.instance', '.request', '.tmp'):
            (self.root / (self.identity + suffix)).unlink(missing_ok=True)

    def poll(self, label, scene, status, resync=False):
        if not self.enabled:
            return None
        now = time.monotonic()
        if now - self.last_write >= 1:
            clean = lambda s: str(s).replace('\n', ' ').replace('\r', ' ')[:512]
            temporary = self.root / (self.identity + '.tmp')
            temporary.write_text('MSBLENDER 1\n' + '\n'.join(map(clean, (label, scene, status, 'resync' if resync else 'ready'))) + '\n', encoding='utf-8')
            temporary.replace(self.root / (self.identity + '.instance'))
            self.last_write = now
        request = self.root / (self.identity + '.request')
        try:
            if request.stat().st_size > 2048 or time.time() - request.stat().st_mtime > 15:
                request.unlink(missing_ok=True)
                return None
            fields = request.read_text(encoding='utf-8').splitlines()
            request.unlink(missing_ok=True)
        except FileNotFoundError:
            return None
        if fields == ['MSCONNECT 1', 'disconnect']:
            return {'disconnect': True}
        if len(fields) != 4 or fields[0:2] != ['MSCONNECT 1', 'connect']:
            return None
        try:
            port = int(fields[2])
        except ValueError:
            return None
        if not 1024 <= port <= 65535 or len(fields[3]) != 64 or any(c not in '0123456789abcdef' for c in fields[3]):
            return None
        return {'port': port, 'code': fields[3]}


discovery = Discovery()
