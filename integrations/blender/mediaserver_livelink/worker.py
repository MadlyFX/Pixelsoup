"""Isolated serializer/network process. This file must never import Blender."""
import json
import select
import socket
import struct
import sys
import time
import uuid

from geometry import MAX_RAW_BYTES, MAX_RESOURCES, MAX_SCENE_BYTES, PAIRING_RESOURCES, glb, manifest, records
from scale import expand_terrain
from surfaces import expand_surfaces, resolve_materials
from wire import Director, CapabilityError, SNAPSHOT, DELTA, STATUS, SHOT_STATE, STAGE_CONTEXT, exact


def send(sock, message):
    payload = json.dumps(message, ensure_ascii=False, allow_nan=False).encode('utf-8')
    if len(payload) > 1024 * 1024:
        raise ValueError('Worker response exceeds capacity')
    sock.sendall(struct.pack('<I', len(payload)) + payload)


def receive_job(sock):
    size, = struct.unpack('<I', exact(sock, 4))
    if size > 2 * 1024 * 1024:
        raise ValueError('Worker metadata exceeds capacity')
    meta = json.loads(exact(sock, size))
    total = 0
    meshes = {}
    for entry in meta.get('meshes', []):
        lengths = entry['lengths']
        if len(lengths) not in (6, 7) or any(not isinstance(n, int) or n < 0 for n in lengths):
            raise ValueError('Invalid copied mesh lengths')
        total += sum(lengths)
        if total > MAX_RAW_BYTES:
            raise ValueError('Copied scene exceeds 8 GiB')
        meshes[entry['id']] = tuple(exact(sock, n) for n in lengths)
    return meta, meshes


class Publisher:
    def __init__(self, director, blender=''):
        self.director = director
        self.blender = blender
        self.raw = {}
        self.prototypes = {}
        self.materials = {}
        self.images = {}
        self.rows = None
        self.uploaded = set()
        self.serializations = 0
        self.serialized_raw = {}
        self.surfaces = {}

    def negotiate(self, objects):
        """Refuse before any upload, naming the feature the Director lacks."""
        for obj in objects:
            if obj.get('batch') or obj.get('lod_id'):
                self.director.require('environment')
            if obj.get('light'):
                self.director.require('materials')

    def publish(self, job, meshes):
        if job.get('animated_package'):
            self.director.require('animation')
            return self.publish_animation(job)
        objects = job['objects']
        mesh_ids = {obj['id'] for obj in objects if obj['prototype']}
        if len(mesh_ids) > MAX_RESOURCES:
            raise ValueError('Collection exceeds ' + str(MAX_RESOURCES) + ' evaluated prototypes')
        self.raw = {key: value for key, value in self.raw.items() if key in mesh_ids}
        self.raw.update(meshes)
        objects, expanded = expand_terrain(objects,self.raw)
        objects, expanded = expand_surfaces(objects,expanded,self.surfaces)
        warnings = []
        for obj in objects:
            if obj['prototype'] and not expanded[obj['id']][5]:
                for material in obj.get('materials', []):
                    if isinstance(material, dict) and material.get('textures'):
                        material['textures'] = {}
                        warnings.append(obj['name'] + ': no UV map; ignored textures')
        objects = resolve_materials(objects, self.images, job.get('refresh_images',False), warnings,
                                    self.blender, getattr(self.director, 'progress', lambda message: None),
                                    lambda: self.director.request(STATUS))
        mesh_ids = {obj['id'] for obj in objects if obj['prototype']}
        if len(mesh_ids) > MAX_RESOURCES:
            raise ValueError('Scene exceeds ' + str(MAX_RESOURCES)
                             + ' geometry resources after splitting large meshes and materials')
        self.prototypes = {key: value for key, value in self.prototypes.items() if key in mesh_ids}
        self.materials = {key: value for key, value in self.materials.items() if key in mesh_ids}
        if sum(sum(len(part) for part in mesh) for mesh in self.raw.values()) > MAX_RAW_BYTES:
            raise ValueError('Retained evaluated arrays exceed 8 GiB')
        stale = []
        for obj in objects:
            identity = obj['id']
            if not obj['prototype']:
                continue
            mats = obj['materials']
            if self.serialized_raw.get(identity) != expanded.get(identity) or self.materials.get(identity) != mats or identity not in self.prototypes:
                if identity not in expanded:
                    raise ValueError('Missing evaluated mesh; resync required')
                stale.append((identity, mats))
        # Each resource encodes independently; numpy and hashing release the
        # GIL, so a detailed scene's parts encode on every core.
        if len(stale) > 1:
            from concurrent.futures import ThreadPoolExecutor
            import os
            with ThreadPoolExecutor(max_workers=min(len(stale), os.cpu_count() or 1, 16)) as pool:
                encoded = list(pool.map(lambda item: glb(expanded[item[0]], item[1]), stale))
        else:
            encoded = [glb(expanded[identity], mats) for identity, mats in stale]
        for (identity, mats), prototype in zip(stale, encoded):
            self.prototypes[identity] = prototype
            self.serialized_raw[identity] = expanded[identity]
            self.materials[identity] = mats
            self.serializations += 1
        self.serialized_raw = {k:v for k,v in self.serialized_raw.items() if k in mesh_ids}
        instances = sum(len(o.get('batch',''))//96 if o.get('batch') else 1 for o in objects
                        if o['prototype'] and not o.get('resource_only'))
        if instances > 4096:
            raise ValueError('Scene exceeds 4096 instances including terrain and ordinary props')
        budget = str(instances)+' instances, '+str(len({v[0] for v in self.prototypes.values()}))+' shared resources'
        if sum(len(data) for _, data in self.prototypes.values()) > MAX_SCENE_BYTES:
            raise ValueError('Retained GLB resources exceed 4 GiB')
        rows = records(objects, self.prototypes)
        # Overrides the artist replaced travel with the revision that replaces
        # them, even when Blender's own values happen not to change.
        absorbs = [tuple(item) for item in job.get('absorb', [])]
        if rows == self.rows and not absorbs:
            return {'kind': 'accepted', 'revision': self.director.base, 'message': 'Up to date',
                    'serializations': self.serializations, 'payload_bytes': 0,'budget':budget,
                    'director_ms': 0, 'warnings': warnings}
        self.negotiate(objects)
        started = time.monotonic()
        revision = str(uuid.uuid4())
        payload = manifest(job['source'], revision, rows, self.rows, absorbs)
        needed = {digest for digest, _ in self.prototypes.values()}
        if len(self.uploaded | needed) > PAIRING_RESOURCES:
            raise ValueError('Pairing resource capacity reached; restart Scene Link in Director, then reconnect')
        for digest, data in self.prototypes.values():
            if digest not in self.uploaded:
                self.director.upload(digest, data)
                self.uploaded.add(digest)
        kind = SNAPSHOT if payload.startswith('MSLINK') else DELTA
        message = self.director.publish(kind, revision, payload)
        self.rows = rows
        return {'kind': 'accepted', 'revision': revision, 'message': message,
                'serializations': self.serializations, 'payload_bytes': len(payload.encode('utf-8')),
                'transaction_kind': 'snapshot' if kind == SNAPSHOT else 'delta','budget':budget, 'warnings': warnings,
                'director_ms': int((time.monotonic()-started)*1000)}

    def publish_animation(self, job):
        from pathlib import Path
        import hashlib
        path = Path(job['animated_package'])
        if not 0 < path.stat().st_size <= 64 * 1024 * 1024:
            raise ValueError('Animation assembly exceeds 64 MiB')
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        obj = job['assembly']
        rows = records([obj], {obj['id']: (digest, data)})
        if rows == self.rows:
            return {'kind':'accepted','revision':self.director.base,'serializations':self.serializations,
                    'payload_bytes':0,'director_ms':0}
        started = time.monotonic()
        if len(self.uploaded | {digest}) > PAIRING_RESOURCES:
            raise ValueError('Pairing resource capacity reached; restart Scene Link and reconnect')
        if digest not in self.uploaded:
            self.director.upload(digest, data)
            self.uploaded.add(digest)
        revision = str(uuid.uuid4())
        payload = manifest(job['source'], revision, rows, self.rows)
        kind = SNAPSHOT if self.rows is None else DELTA
        self.director.publish(kind, revision, payload)
        self.rows = rows
        self.serializations += 1
        return {'kind':'accepted','revision':revision,'serializations':self.serializations,'payload_bytes':len(payload),
                'transaction_kind':'animation','budget':'Animated assembly; show-frame playback',
                'director_ms': int((time.monotonic()-started)*1000)}


def main():
    sock = socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=10)
    sock.settimeout(60)
    sock.sendall(sys.argv[2].encode('ascii'))
    director = None
    try:
        config, _ = receive_job(sock)
        director = Director(config['port'], config['code'], config['source'],
                            lambda stage: send(sock, {'kind': 'progress', 'message': stage}),
                            collection_name=config.get('collection_name', ''),
                            host=config.get('host', ''))
        publisher = Publisher(director, config.get('blender_binary', ''))
        send(sock, {'kind': 'connected', 'revision': director.base,
                    'capabilities': list(director.capabilities)})
        shot_revision = ''
        source_paused = False
        def shots():
            nonlocal shot_revision
            # Media Server's overrides on this scene, for writeback.
            reply = director.request(SHOT_STATE, base=shot_revision)
            if reply['kind'] != SHOT_STATE:
                raise RuntimeError(reply['payload'])
            if reply['payload']:
                shot_revision = reply['revision']
                send(sock, {'kind': 'shots', 'payload': reply['payload'], 'revision': shot_revision})
            # A Director without stage references is never polled for them.
            if 'stage' in director.capabilities:
                reply = director.request(STAGE_CONTEXT)
                if reply['kind'] == STAGE_CONTEXT and reply['payload']:
                    send(sock, {'kind': 'stage', 'payload': reply['payload']})
        while True:
            if not select.select([sock], [], [], 0.25)[0]:
                reply = director.request(STATUS)
                if reply['revision'] != director.base:
                    raise RuntimeError('Director revision changed; resync required')
                if source_paused and reply['payload'].startswith('source_resumed;'):
                    source_paused = False
                    send(sock, {'kind': 'resumed'})
                shots()
                continue
            job, meshes = receive_job(sock)
            try:
                send(sock, publisher.publish(job, meshes))
            except CapabilityError as error:
                # An unsupported feature refuses one operation; the pairing,
                # retained resources and accepted revision all survive.
                send(sock, {'kind': 'unsupported', 'message': str(error)})
            except RuntimeError as error:
                if not str(error).startswith('source_paused'):
                    raise
                # The serializer retains the latest copied geometry. Pause is
                # recoverable on this pairing; an explicit Publish retries it.
                source_paused = True
                send(sock, {'kind': 'paused', 'message': 'Paused in Media Server. Resume source updates there to sync automatically.'})
            shots()
    except Exception as error:
        try:
            send(sock, {'kind': 'error', 'message': str(error)[:2000]})
        except OSError:
            pass
    finally:
        if director:
            director.close()
        sock.close()


if __name__ == '__main__':
    main()
