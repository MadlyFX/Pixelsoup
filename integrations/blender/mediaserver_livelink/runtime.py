"""Event-driven Blender scheduler: one extraction batch and one process request."""
import time

import bpy

from .extract import Identities, copy_mesh, describe, qualified
from .geometry import MAX_SCENE_BYTES
from .ipc import Worker
from .shot import ShotLayer
from .stage import StageReference
from .wire import BASELINE_CAPABILITIES, FEATURES


def percentile(samples, fraction):
    """Nearest rank, the convention the step 01 baseline and Director use."""
    if not samples:
        return 0
    ordered = sorted(samples)
    rank = max(1, -(-int(fraction * 100) * len(ordered) // 100))
    return ordered[min(rank, len(ordered)) - 1]


class Runtime:
    def __init__(self):
        self.worker = None
        self.pairing_code = ''
        self.retired = []
        self.identities = Identities()
        self.status = 'Select a collection and connect to the local Director'
        self.collection_id = ''
        self.scene_id = ''
        self.source = ''
        self.connected = False
        self.busy = False
        self.live = True
        self.pending = False
        self.full = True
        self.dirty = set()
        self.known = set()
        self.generation = 0
        self.changed_at = 0
        self.batch = None
        self.requested = False
        self.extracting = False
        self.exports = 0
        self.serializations = 0
        self.payload_bytes = 0
        self.revision = ''
        self.last_kind = ''
        self.last_extract_ms = 0
        self.last_scale = None
        # Step 12 evidence. Blender time and Director time are recorded apart;
        # the session percentiles cover accepted revisions that changed content.
        self.capabilities = ()
        self.unsupported = ''
        self.pending_since = 0
        self.submitted_at = 0
        self.latency = []
        self.last_latency_ms = 0
        self.last_blender_ms = 0
        self.last_director_ms = 0
        self.shot = ShotLayer(self)
        self.stage = StageReference()
        self.frame_sampling = False

    def mark(self, full=False, geometry=()):
        self.shot.refresh = True
        self.generation += 1
        if not self.pending:
            # The measured wait starts at the first unpublished event, so
            # deliberate coalescing is inside the recorded latency.
            self.pending_since = time.monotonic()
        self.pending = True
        self.full |= full
        self.dirty.update(geometry)
        if len(self.dirty) > 1024:
            self.full = True
            self.dirty.clear()
        self.changed_at = time.monotonic()
        self.batch = None

    def start(self, context):
        import json
        import uuid
        qualified()
        settings = context.scene.ms_livelink
        collection = settings.collection
        if collection is None:
            raise ValueError('Select a collection first')
        if not settings.code.strip():
            raise ValueError('Paste the pairing code from File > Scene Link Details in Media Server')
        self.stop()
        self.scene_id = self.identities.identity(context.scene, context.scene)
        self.collection_id = self.identities.identity(collection, context.scene)
        source = str(uuid.uuid5(uuid.UUID(self.scene_id), self.collection_id))
        if source != self.source:
            self.shot = ShotLayer(self)
        self.source = source
        # Seed existing owners before copies are made; validate before starting I/O.
        if settings.publish_animation:
            from .animation import validate, ensure_anchor, assembly_id
            validate(context.scene,collection)
            ensure_anchor(collection,assembly_id(self.collection_id))
        else:
            describe(context.scene, collection, self.identities)
        collection['ms_link_source_metadata'] = json.dumps({'version': 1, 'source': self.source,
            'scene': self.scene_id, 'collection': self.collection_id, 'blender': bpy.app.version_string,
            'build_hash': bpy.app.build_hash.decode()})
        self.worker = Worker()
        self.capabilities = ()
        self.unsupported = ''
        self.worker.submit({'port': settings.port, 'code': settings.code.strip(), 'source': self.source,
                            'collection_name': collection.name, 'host': 'blender/' + bpy.app.version_string})
        self.status = 'Connecting to Director...'
        self.busy = True
        self.live = settings.live
        self.requested = True
        self.mark(full=True)

    def stop(self, message='Disconnected; Preview retains its accepted revision'):
        from .animation import cancel
        cancel()
        if self.worker:
            self.retired.append(self.worker.close())
        self.worker = None
        self.connected = False
        self.busy = False
        self.batch = None
        self.known.clear()
        self.dirty.clear()
        self.pending = False
        self.full = True
        self.requested = False
        self.last_scale = None
        self.status = message
        self.stage = StageReference()
        self.shot.gesture = None
        self.shot.conflict = ''
        if self.shot.rows and hasattr(bpy.context.scene, 'ms_livelink'):
            try:
                self.shot.apply()
            except (ValueError, ReferenceError):
                pass

    def fail(self, error):
        message = str(error)
        if message == 'pairing_required':
            message = 'Pairing code rejected. Copy fresh Director details and reconnect.'
        self.stop(message)

    def publish_now(self):
        self.requested = True
        self.mark()

    def tick(self):
        from .animation import poll
        poll()
        self.retired[:] = [process for process in self.retired if process.poll() is None]
        if not self.worker:
            return
        for reply in self.worker.poll():
            kind = reply['kind']
            if kind == 'error':
                self.fail(reply['message'])
                return
            if kind == 'progress':
                self.status = 'Director: ' + reply['message']
            elif kind == 'connected':
                self.connected, self.busy = True, False
                self.revision = reply['revision']
                self.capabilities = tuple(reply.get('capabilities') or BASELINE_CAPABILITIES)
                self.status = 'Connected; initial collection publish pending'
            elif kind == 'accepted':
                self.busy = False
                # An accepted revision means the refused feature is gone.
                self.unsupported = ''
                self.revision = reply['revision']
                self.serializations = reply['serializations']
                self.payload_bytes = reply['payload_bytes']
                self.last_kind = reply.get('transaction_kind', 'unchanged')
                self.last_director_ms = reply.get('director_ms', 0)
                if self.payload_bytes and self.pending_since:
                    self.last_latency_ms = int((time.monotonic() - self.pending_since) * 1000)
                    self.last_blender_ms = max(0, int((self.submitted_at - self.pending_since) * 1000))
                    self.latency.append(self.last_latency_ms)
                    del self.latency[:-256]
                self.pending_since = 0
                self.status = 'Accepted in Preview' if self.payload_bytes else 'Preview is up to date'
                if reply.get('budget'):
                    self.status += '; ' + reply['budget']
            elif kind == 'unsupported':
                self.busy = False
                self.pending = False
                self.requested = False
                self.unsupported = reply['message']
                self.status = reply['message']
            elif kind == 'paused':
                self.busy = False
                self.pending = False
                self.requested = False
                self.status = reply['message']
            elif kind == 'shots':
                try:
                    self.shot.receive(reply['payload'])
                except ValueError as error:
                    self.shot.detach()
                    self.shot.rows = {}
                    self.status = str(error)
            elif kind == 'stage':
                try:
                    self.stage.receive(reply['payload'])
                except ValueError as error:
                    if bpy.context.scene.ms_livelink.stage_reference:
                        self.status = 'Stage reference unavailable: ' + str(error)
            elif kind == 'shot_result':
                self.shot.result(reply)
        if self.shot.refresh and self.shot.rows:
            try:
                self.shot.apply()
            except ValueError as error:
                self.status = str(error)
                self.shot.detach()
                self.shot.refresh = False
        if self.shot.gesture:
            if time.monotonic() >= self.shot.gesture['expires'] and not self.shot.conflict:
                self.shot.conflict = 'Claim expired; accept Director or retry the placement'
                self.shot.gesture['ready'] = False
                self.status = self.shot.conflict
            return
        if not self.connected or self.busy or not self.pending or not (self.live or self.requested):
            return
        scene = bpy.context.scene
        if self.identities.identity(scene, scene) != self.scene_id:
            self.stop('Scene changed; select collection and reconnect')
            return
        collection = scene.ms_livelink.collection
        if collection is None or self.identities.identity(collection, scene) != self.collection_id:
            self.stop('Linked collection changed; reconnect')
            return
        if bpy.context.mode.startswith('EDIT') or bpy.context.mode == 'SCULPT':
            self.status = 'Pending geometry: finish Edit/Sculpt mode to publish'
            return
        if scene.ms_livelink.publish_animation:
            # Refuse before the bake process starts, not after it finishes.
            if self.capabilities and 'animation' not in self.capabilities:
                self.pending = False
                self.requested = False
                self.unsupported = ('This Media Server build does not support ' + FEATURES['animation']
                                    + "; clear Animated assembly or install the matching build")
                self.status = self.unsupported
                return
            if self.requested:
                from .animation import start
                start(bpy.context)
            else:
                self.pending = False
                self.status = 'Animation source changed; Publish Now to bake a new clip'
            return
        if time.monotonic() - self.changed_at < 0.2:
            self.status = 'Pending edits (coalescing)'
            return
        if self.batch is None:
            self.extracting = True
            try:
                objects = describe(scene, collection, self.identities)
            finally:
                self.extracting = False
            scale = scene.unit_settings.scale_length
            full = self.full or self.last_scale != scale
            todo = [obj['id'] for obj in objects if obj['prototype'] and (full or obj['id'] not in self.known
                or obj['session'] in self.dirty or obj['data_session'] in self.dirty)]
            self.batch = {'generation': self.generation, 'objects': objects, 'todo': todo,
                          'meshes': {}, 'bytes': 0, 'scale': scale}
        batch = self.batch
        if batch['todo']:
            identity = batch['todo'].pop(0)
            obj = next(obj for obj in batch['objects'] if obj['id'] == identity)
            self.status = 'Pending evaluated mesh: ' + obj['name']
            self.extracting = True
            started = time.perf_counter()
            try:
                mesh, materials = copy_mesh(obj.get('mesh_name',obj['name']), batch['scale'])
            finally:
                self.extracting = False
            self.last_extract_ms = (time.perf_counter() - started) * 1000
            self.exports += 1
            if materials != obj['materials']:
                obj['materials'] = materials
            batch['meshes'][identity] = mesh
            batch['bytes'] += sum(map(len, mesh))
            if batch['bytes'] > MAX_SCENE_BYTES:
                raise ValueError('Copied scene exceeds 128 MiB')
            # One non-preemptible Blender evaluation/copy per timer dispatch.
            return
        if batch['generation'] != self.generation:
            self.batch = None
            return
        metadata = {'source': self.source, 'objects': batch['objects'], 'refresh_images': self.requested, 'meshes': [
            {'id': identity, 'lengths': list(map(len, mesh))} for identity, mesh in batch['meshes'].items()]}
        self.worker.submit(metadata, batch['meshes'].values())
        self.submitted_at = time.monotonic()
        self.known = {obj['id'] for obj in batch['objects'] if obj['prototype']}
        self.last_scale = batch['scale']
        self.batch = None
        self.dirty.clear()
        self.full = False
        self.pending = False
        self.requested = False
        self.busy = True
        self.status = 'Publishing evaluated collection to Preview...'


runtime = Runtime()


def timer():
    try:
        runtime.tick()
    except Exception as error:
        runtime.fail(error)
    # Poll only bounded IPC/status. Geometry work is enabled by dependency events.
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                area.tag_redraw()
    return 0.05 if runtime.worker else 0.25
