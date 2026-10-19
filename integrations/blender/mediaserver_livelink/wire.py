"""Bounded v1 Director wire codec; standard library only, also used by tests."""
import socket
import struct
import uuid

HELLO, WELCOME, REGISTER, SNAPSHOT, DELTA, ANNOUNCE, CHUNK, COMPLETE, ACK, REJECT, RESYNC, STATUS = range(1, 13)
SHOT_STATE, STAGE_CONTEXT = 13, 14
FIELDS = ('epoch', 'scene', 'transaction', 'origin', 'source', 'base', 'revision', 'resource')
PACKET_LIMIT = 1024 * 1024 + 4096

ADDON_VERSION = (0, 10, 1)
PROTOCOL_VERSION = 1
BLENDER_REQUIRED = (5, 2, 0)
# Optional wire features: each side names what it can serve so a mismatched pair
# refuses the specific operation with a diagnostic instead of failing inside a
# record it cannot parse. `base` covers objects, prototypes, material records,
# resource transfer and writeback (ShotState and `absorb` records).
CAPABILITIES = ('base', 'stage', 'environment', 'materials', 'animation')
FEATURES = {'stage': 'stage and camera references',
            'environment': 'foliage, terrain and authored LODs',
            'materials': 'native linked lights and baked color',
            'animation': 'animated assemblies'}


class CapabilityError(RuntimeError):
    """The peer cannot serve a feature. The pairing stays usable without it."""


def client_identity(host=''):
    name = 'mediaserver_livelink/%d.%d.%d' % ADDON_VERSION
    return (name + ' ' + host).strip()[:96]


def welcome_capabilities(payload):
    for part in payload.replace(';', ' ').split():
        if part.startswith('capabilities='):
            return tuple(name for name in part[len('capabilities='):].split(',') if name)
    return ('base',)


def encode(message):
    body = bytearray(b'MSL4' + struct.pack('<HH', 1, message['kind']))
    for field in FIELDS:
        value = message.get(field, '').encode('ascii')
        if len(value) > 128 or any(c < 32 or c > 126 for c in value):
            raise ValueError('Invalid envelope field: ' + field)
        body.extend(struct.pack('<H', len(value)) + value)
    payload = message.get('payload', b'')
    if isinstance(payload, str):
        payload = payload.encode('utf-8')
    body.extend(struct.pack('<QQI', message.get('offset', 0), message.get('length', 0), len(payload)))
    body.extend(payload)
    if len(body) + 4 > PACKET_LIMIT:
        raise ValueError('Director packet exceeds capacity')
    return struct.pack('<I', len(body)) + body


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise ConnectionError('Director disconnected; reconnect and resync')
        data.extend(part)
    return data


def receive(sock):
    size, = struct.unpack('<I', exact(sock, 4))
    if not 44 <= size <= PACKET_LIMIT - 4:
        raise ValueError('Invalid Director reply size')
    body = exact(sock, size)
    if body[:4] != b'MSL4':
        raise ValueError('Invalid Director magic')
    version, kind = struct.unpack_from('<HH', body, 4)
    if version != 1 or not 1 <= kind <= STAGE_CONTEXT:
        raise ValueError('Unsupported Director protocol')
    cursor, out = 8, {'kind': kind}
    for field in FIELDS:
        if cursor + 2 > size:
            raise ValueError('Truncated Director field')
        length, = struct.unpack_from('<H', body, cursor)
        cursor += 2
        if length > 128 or cursor + length > size:
            raise ValueError('Invalid Director field')
        out[field] = body[cursor:cursor + length].decode('ascii')
        cursor += length
    if cursor + 20 > size:
        raise ValueError('Truncated Director payload')
    out['offset'], out['length'], length = struct.unpack_from('<QQI', body, cursor)
    cursor += 20
    if cursor + length != size:
        raise ValueError('Invalid Director payload length')
    out['payload'] = body[cursor:].decode('utf-8')
    return out


class Director:
    def __init__(self, port, code, source, progress=lambda value: None, collection_name='', host=''):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.socket.settimeout(60)
        self.envelope = {'origin': 'blender-' + str(uuid.uuid4())}
        self.progress = progress
        self.capabilities = ('base',)
        try:
            # The pairing code, then this add-on's declarations.
            reply = self.request(HELLO, payload='\n'.join([code,
                'capabilities=' + ','.join(CAPABILITIES), 'client=' + client_identity(host)]))
            if reply['kind'] != WELCOME:
                raise RuntimeError(reply['payload'])
            self.capabilities = welcome_capabilities(reply['payload'])
            self.envelope.update(epoch=reply['epoch'], scene=reply['scene'], source=source)
            reply = self.request(REGISTER, payload=collection_name)
            if reply['kind'] not in (ACK, RESYNC):
                raise RuntimeError('Source registration failed')
            self.base = reply['revision']
        except Exception:
            self.close()
            raise

    def close(self):
        self.socket.close()

    def require(self, capability):
        """Refuse one unsupported operation by name, without disconnecting."""
        if capability not in self.capabilities:
            raise CapabilityError('This Media Server build does not support '
                + FEATURES.get(capability, capability) + " (capability '" + capability
                + "'); install the Media Server and add-on from one delivery")

    def request(self, kind, **fields):
        self.socket.sendall(encode(dict(self.envelope, kind=kind, **fields)))
        while True:
            reply = receive(self.socket)
            if reply['kind'] == STATUS and reply['payload'] in ('received', 'validated'):
                self.progress(reply['payload'])
                continue
            if reply['kind'] == REJECT:
                if reply['payload'].startswith(('capability_not_negotiated', 'unsupported_capability')):
                    raise CapabilityError(reply['payload'])
                raise RuntimeError(reply['payload'])
            return reply

    def upload(self, digest, data):
        reply = self.request(ANNOUNCE, resource=digest, length=len(data))
        if reply['kind'] != ACK:
            raise RuntimeError(reply['payload'])
        if reply['payload'] == 'resource_present':
            return
        offset = reply['offset']
        while offset < len(data):
            chunk = data[offset:offset + 256 * 1024]
            reply = self.request(CHUNK, resource=digest, offset=offset, payload=chunk)
            if reply['kind'] != ACK:
                raise RuntimeError(reply['payload'])
            offset += len(chunk)
        reply = self.request(COMPLETE, resource=digest)
        if reply['kind'] != ACK:
            raise RuntimeError(reply['payload'])

    def publish(self, kind, revision, payload):
        reply = self.request(kind, transaction=str(uuid.uuid4()), base=self.base,
                             revision=revision, payload=payload)
        if reply['kind'] != ACK:
            raise RuntimeError('Resync required: ' + reply['payload'])
        self.base = revision
        return reply['payload']
