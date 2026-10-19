"""Nonblocking main-thread IPC. One request in flight, no background threads."""
import json
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import sys
import time

from .geometry import MAX_RAW_BYTES


class Worker:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.listener.setblocking(False)
        self.token = secrets.token_hex(32).encode('ascii')
        self.socket = None
        self.auth = bytearray()
        self.input = bytearray()
        self.output = []
        self.packed_images = {}
        self.offset = 0
        self.deadline = time.monotonic() + 15
        try:
            self.process = subprocess.Popen([sys.executable, '-u', str(Path(__file__).with_name('worker.py')),
                str(self.listener.getsockname()[1]), self.token.decode('ascii')],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except Exception:
            self.listener.close()
            raise

    def submit(self, metadata, meshes=()):
        if self.output:
            raise RuntimeError('IPC request already pending')
        # Binary image attachments keep packed Blender images out of JSON and
        # reuse the same nonblocking, bounded send queue as mesh arrays.
        import copy
        metadata = copy.deepcopy(metadata)
        images = {}
        active_images = {}
        for obj in metadata.get('objects', []):
            for material in obj.get('materials', []):
                if isinstance(material, dict):
                    for view in material.get('textures', {}).values():
                        data = view.pop('packed_bytes', None)
                        if data is not None:
                            path = view['path']
                            active_images[path] = data
                            view['packed_image'] = True
                            if self.packed_images.get(path) is not data:
                                images[path] = data
        if len(active_images) > 256 or sum(map(len, active_images.values())) > 256 * 1024 * 1024:
            raise ValueError('Packed images exceed 256 images / 256 MiB')
        metadata['packed_images'] = [{'path': path, 'length': len(data)} for path, data in images.items()]
        meshes = tuple(meshes)
        payload = json.dumps(metadata, ensure_ascii=False, allow_nan=False).encode('utf-8')
        if len(payload) > 2 * 1024 * 1024 or sum(len(p) for m in meshes for p in m) > MAX_RAW_BYTES:
            raise ValueError('Copied scene exceeds IPC capacity')
        self.output = [memoryview(struct.pack('<I', len(payload)) + payload)]
        self.output.extend(memoryview(part) for mesh in meshes for part in mesh if part)
        self.output.extend(memoryview(data) for data in images.values())
        self.packed_images = active_images
        self.offset = 0

    def poll(self):
        if self.socket is None:
            if time.monotonic() > self.deadline or self.process.poll() is not None:
                raise RuntimeError('Publisher process did not start')
            try:
                self.socket, _ = self.listener.accept()
                self.socket.setblocking(False)
            except BlockingIOError:
                return []
        if len(self.auth) < 64:
            if time.monotonic() > self.deadline:
                raise RuntimeError('Publisher process handshake timed out')
            try:
                data = self.socket.recv(64 - len(self.auth))
                if not data:
                    raise ConnectionError('Publisher process disconnected')
                self.auth.extend(data)
            except BlockingIOError:
                return []
            if len(self.auth) < 64:
                return []
            if not secrets.compare_digest(self.auth, self.token):
                raise RuntimeError('Publisher process handshake rejected')
            self.listener.close()
        # Large imports need throughput without monopolizing Blender's UI.
        budget = 4 * 1024 * 1024
        until = time.perf_counter() + 0.004
        while self.output and budget and time.perf_counter() < until:
            try:
                sent = self.socket.send(self.output[0][self.offset:self.offset + budget])
            except BlockingIOError:
                break
            if not sent:
                raise ConnectionError('Publisher process disconnected')
            self.offset += sent
            budget -= sent
            if self.offset == len(self.output[0]):
                self.output.pop(0)
                self.offset = 0
        try:
            data = self.socket.recv(32768)
            if not data:
                # Drain an error/receipt from the previous tick before reporting EOF.
                raise ConnectionError('Publisher process disconnected; reconnect and resync')
            self.input.extend(data)
        except BlockingIOError:
            pass
        messages = []
        while len(self.input) >= 4:
            size, = struct.unpack_from('<I', self.input)
            if size > 1024 * 1024:
                raise ValueError('Publisher response exceeds capacity')
            if len(self.input) < 4 + size:
                break
            messages.append(json.loads(self.input[4:4 + size]))
            del self.input[:4 + size]
        return messages

    def close(self):
        self.listener.close()
        if self.socket:
            self.socket.close()
        self.output.clear()
        # Termination is asynchronous; a later timer reaps the child. No wait on UI.
        if self.process.poll() is None:
            self.process.kill()
        return self.process
