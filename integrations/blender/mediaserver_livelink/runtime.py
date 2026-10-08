"""Event-driven Blender scheduler: one extraction batch and one process request."""
import time

import bpy

from .extract import Identities, copy_mesh, describe, qualified
from .geometry import MAX_RAW_BYTES
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
        self.status = 'Ready. In Media Server choose File > Connect to Blender.'
        self.auto_connect = False
        self.needs_new_session = False
        self.retry_at = 0
        self.retry_delay = 1
        self.membership = None
        self.membership_at = 0
        self.paused_by_director = False
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
        self.publish_notices = []
        self.publish_errors = []
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
        collection = settings.collection or context.scene.collection
        if not settings.code.strip():
            raise ValueError('Connect from File > Connect to Blender in Media Server')
        self.stop(reconnecting=True)
        self.needs_new_session = False
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
                            'collection_name': collection.name, 'host': 'blender/' + bpy.app.version_string,
                            'blender_binary': bpy.app.binary_path})
        self.status = 'Connecting to Director...'
        self.busy = True
        self.live = settings.live
        self.requested = True
        self.mark(full=True)

    def stop(self, message='Disconnected; Preview retains its accepted revision', reconnecting=False):
        if not reconnecting:
            self.needs_new_session = False
            self.auto_connect = False
            self.retry_at = 0
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
        self.membership = None
        self.paused_by_director = False
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
        self.needs_new_session = 'resource capacity' in message.lower() or message == 'resource_capacity'
        retry = self.auto_connect and self.live
        self.stop(message, reconnecting=retry)
        if retry:
            self.retry_at = time.monotonic() + self.retry_delay
            self.retry_delay = min(15, self.retry_delay * 2)
            self.status = 'Reconnecting automatically: ' + message

    def discover(self):
        from pathlib import Path
        from .discovery import discovery
        scene = bpy.context.scene
        if scene is None:
            return
        label = (Path(bpy.data.filepath).name if bpy.data.filepath else 'Unsaved scene') + ' - Blender ' + bpy.app.version_string
        command = discovery.poll(label, scene.name, self.status, self.needs_new_session)
        if command:
            if command.get('disconnect'):
                self.stop()
                self.pairing_code = ''
                return
            settings = scene.ms_livelink
            settings.port, settings.code = command['port'], command['code']
            settings.collection = None  # All current and future scene collections.
            settings.live = True
            settings.publish_animation = False
            self.auto_connect = True
            self.retry_delay = 1
            self.start(bpy.context)
        if self.auto_connect and not self.needs_new_session and not self.worker and self.retry_at and time.monotonic() >= self.retry_at:
            self.retry_at = 0
            self.start(bpy.context)

    def publish_now(self):
        self.requested = True
        self.mark()

    def tick(self):
        self.discover()
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
                self.status = 'Connected; syncing scene...'
            elif kind == 'accepted':
                self.busy = False
                self.retry_delay = 1
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
                from .translation import sync_feedback
                self.status, self.unsupported = sync_feedback(self.payload_bytes,
                    self.publish_notices + reply.get('warnings', []), self.publish_errors, reply.get('budget', ''))
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
                self.paused_by_director = True
            elif kind == 'resumed':
                if self.paused_by_director:
                    self.paused_by_director = False
                    self.mark(full=True)
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
        # Imports, removals, renames and collection moves do not consistently emit
        # object geometry updates. A bounded metadata check catches these without
        # re-extracting meshes or restarting an in-progress batch on idle polls.
        if self.connected and time.monotonic() - self.membership_at >= 0.5:
            self.membership_at = time.monotonic()
            scene = bpy.context.scene
            collection = scene.ms_livelink.collection or scene.collection
            signature = tuple(sorted((o.session_uid, o.name, o.data.session_uid if o.data else 0,
                o.parent.session_uid if o.parent else 0, o.hide_render, o.hide_viewport)
                for o in collection.all_objects if not o.get('ms_link_shot_wrapper')
                and not o.get('ms_link_shot_anchor') and not o.get('ms_link_stage_reference')))
            if self.membership is not None and signature != self.membership:
                self.mark()
            self.membership = signature
        if not self.connected or self.busy or not self.pending or not (self.live or self.requested):
            return
        scene = bpy.context.scene
        if self.identities.identity(scene, scene) != self.scene_id:
            self.stop('Scene changed; select collection and reconnect')
            return
        collection = scene.ms_livelink.collection or scene.collection
        if self.identities.identity(collection, scene) != self.collection_id:
            self.stop('Linked collection changed; reconnect')
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
            from .translation import notices, geometry_errors
            notices.clear()
            geometry_errors.clear()
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
                try:
                    mesh, materials = copy_mesh(obj.get('mesh_name',obj['name']), batch['scale'])
                except (ValueError, RuntimeError) as error:
                    from .translation import geometry_error
                    geometry_error(str(error) + '; skipped geometry')
                    obj['prototype'] = ''
                    obj.pop('lod_id', None)
                    return
            finally:
                self.extracting = False
            self.last_extract_ms = (time.perf_counter() - started) * 1000
            self.exports += 1
            if materials != obj['materials']:
                obj['materials'] = materials
            batch['meshes'][identity] = mesh
            batch['bytes'] += sum(map(len, mesh))
            if batch['bytes'] > MAX_RAW_BYTES:
                raise ValueError('Copied scene exceeds 256 MiB')
            # One non-preemptible Blender evaluation/copy per timer dispatch.
            return
        if batch['generation'] != self.generation:
            self.batch = None
            return
        metadata = {'source': self.source, 'objects': batch['objects'], 'refresh_images': self.requested, 'meshes': [
            {'id': identity, 'lengths': list(map(len, mesh))} for identity, mesh in batch['meshes'].items()]}
        self.worker.submit(metadata, batch['meshes'].values())
        from .translation import notices, geometry_errors
        self.publish_notices = list(notices)
        self.publish_errors = list(geometry_errors)
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
