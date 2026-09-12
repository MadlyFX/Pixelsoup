"""Nonblocking main-thread IPC. One request in flight, no background threads."""
import json
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import sys
import time

from .geometry import MAX_SCENE_BYTES


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
        payload = json.dumps(metadata, ensure_ascii=False, allow_nan=False).encode('utf-8')
        if len(payload) > 2 * 1024 * 1024 or sum(len(p) for m in meshes for p in m) > MAX_SCENE_BYTES:
            raise ValueError('Copied scene exceeds IPC capacity')
        self.output = [memoryview(struct.pack('<I', len(payload)) + payload)]
        self.output.extend(memoryview(part) for mesh in meshes for part in mesh if part)
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
        budget = 256 * 1024
        while self.output and budget:
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
