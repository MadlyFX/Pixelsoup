#!/usr/bin/env python3
"""Reference publisher: send a portable .mslink to a paired local Director.

This exercises the transport independently of Blender. The Blender collection
publisher is a separate add-on. Only Python's standard library is required.
"""
import argparse
import hashlib
import json
import shlex
import socket
import struct
import sys
import uuid
from pathlib import Path

HELLO, WELCOME, REGISTER, SNAPSHOT, DELTA, ANNOUNCE, CHUNK, COMPLETE, ACK, REJECT, RESYNC, STATUS = range(1, 13)
FIELDS = ("epoch", "scene", "transaction", "origin", "source", "base", "revision", "resource")
PACKET_LIMIT = 1024 * 1024 + 4096


def encode(message):
    body = b"MSL4" + struct.pack("<HH", message.get("version", 1), message["kind"])
    for field in FIELDS:
        value = message.get(field, "").encode("ascii")
        if len(value) > 128:
            raise ValueError(f"{field} exceeds 128 bytes")
        body += struct.pack("<H", len(value)) + value
    payload = message.get("payload", b"")
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    body += struct.pack("<QQI", message.get("offset", 0), message.get("length", 0), len(payload)) + payload
    if len(body) + 4 > PACKET_LIMIT:
        raise ValueError("Packet exceeds capacity")
    return struct.pack("<I", len(body)) + body


def receive(sock):
    def exact(length):
        data = bytearray()
        while len(data) < length:
            chunk = sock.recv(length - len(data))
            if not chunk:
                raise ConnectionError("Director disconnected; reconnect and register the current revision")
            data.extend(chunk)
        return bytes(data)
    length, = struct.unpack("<I", exact(4))
    if not 4 <= length <= PACKET_LIMIT - 4:
        raise ValueError("Invalid reply length")
    body = exact(length)
    if body[:4] != b"MSL4":
        raise ValueError("Invalid reply magic")
    version, kind = struct.unpack_from("<HH", body, 4)
    if version != 1:
        raise ValueError("Unsupported reply version")
    out = {"kind": kind}
    cursor = 8
    for field in FIELDS:
        n, = struct.unpack_from("<H", body, cursor)
        cursor += 2
        if n > 128 or cursor + n > len(body):
            raise ValueError("Invalid reply field")
        out[field] = body[cursor:cursor+n].decode("ascii")
        cursor += n
    out["offset"], out["length"], n = struct.unpack_from("<QQI", body, cursor)
    cursor += 20
    if cursor + n != len(body):
        raise ValueError("Invalid reply payload")
    out["payload"] = body[cursor:].decode("utf-8")
    return out


class Client:
    def __init__(self, port, code, origin="reference-publisher"):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=60)
        self.envelope = {"origin": origin}
        # Hello's first line is the pairing code; the declarations after it name
        # the optional wire features this publisher uses. It sends plain object,
        # prototype and material records, so `base` is the whole declaration.
        welcome = self.request(HELLO, payload="\n".join(
            [code, "capabilities=base", "client=reference-publisher/1.0"]))
        if welcome["kind"] != WELCOME:
            raise RuntimeError(welcome["payload"])
        print(f"Welcome: {welcome['payload']}")
        self.envelope.update(epoch=welcome["epoch"], scene=welcome["scene"])

    def request(self, kind, **fields):
        self.socket.sendall(encode(dict(self.envelope, kind=kind, **fields)))
        while True:
            reply = receive(self.socket)
            if reply["kind"] == STATUS and reply["payload"] in ("received", "validated"):
                print(reply["payload"], flush=True)
                continue
            if reply["kind"] == REJECT:
                raise RuntimeError(reply["payload"])
            return reply

    def register(self, source, base=""):
        self.envelope["source"] = source
        reply = self.request(REGISTER, base=base)
        print(f"Registration: {reply['payload']}; Director revision: {reply['revision']!r}")
        return reply["revision"]

    def upload(self, hash_id, path):
        reply = self.request(ANNOUNCE, resource=hash_id, length=path.stat().st_size)
        if reply["payload"] == "resource_present":
            return
        offset = 0
        with path.open("rb") as source:
            while data := source.read(256 * 1024):
                self.request(CHUNK, resource=hash_id, offset=offset, payload=data)
                offset += len(data)
        self.request(COMPLETE, resource=hash_id)


def portable_manifest(path):
    """Resolve local fixture paths, then send only hash.glb resource references."""
    lines = path.read_text(encoding="utf-8").splitlines()
    header = shlex.split(lines[0])
    if len(header) != 4 or header[:2] != ["MSLINK", "1"]:
        raise ValueError("Expected MSLINK 1 manifest")
    resources = {}
    for index, line in enumerate(lines[1:], 1):
        fields = shlex.split(line)
        if fields and fields[0] == "prototype":
            _, identity, expected, relative = fields
            resource = (path.parent / relative).resolve()
            if not resource.is_relative_to(path.parent.resolve()):
                raise ValueError("Resource must stay inside manifest directory")
            with resource.open("rb") as data:
                actual = hashlib.file_digest(data, "sha256").hexdigest()
            if actual != expected:
                raise ValueError(f"Resource hash mismatch: {relative}")
            resources[actual] = resource
            lines[index] = "prototype " + " ".join(json.dumps(value) for value in (identity, actual, actual + ".glb"))
    return header[2], header[3], "\n".join(lines) + "\n", resources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--port", type=int, required=True, help="Port from File > Scene Link Details")
    parser.add_argument("--pairing-code", required=True, help="Code from the local editor pairing dialog")
    parser.add_argument("--base", help="Expected Director revision; omitted uses registration reply")
    parser.add_argument("--transaction", default=str(uuid.uuid4()), help="Keep identical for an exact replay")
    parser.add_argument("--duplicate", action="store_true", help="Replay the identical transaction and show its retained receipt")
    args = parser.parse_args()
    source, revision, payload, resources = portable_manifest(args.manifest)
    client = Client(args.port, args.pairing_code)
    try:
        current = client.register(source, args.base or "")
        base = current if args.base is None else args.base
        for hash_id, path in resources.items():
            client.upload(hash_id, path)
        transaction = dict(transaction=args.transaction, base=base, revision=revision, payload=payload)
        result = client.request(SNAPSHOT, **transaction)
        print(result["payload"], flush=True)
        if result["kind"] != ACK:
            raise RuntimeError("Revision was not accepted; register again before retrying")
        if args.duplicate:
            print("Replay: " + client.request(SNAPSHOT, **transaction)["payload"], flush=True)
    finally:
        client.socket.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        print(f"Scene Link: {error}", file=sys.stderr)
        sys.exit(1)
