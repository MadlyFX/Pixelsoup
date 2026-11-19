"""Event-driven Blender scheduler: one extraction batch and one process request."""
import time
import uuid

import bpy

from .extract import Identities, copy_mesh, describe, qualified
from .geometry import MAX_RAW_BYTES
from .ipc import Worker
from .stage import StageReference
from .writeback import Writeback
from .wire import FEATURES

# Quiet time, after the last accepted or written-back change, before the linked
# file is saved: long enough that a drag or a run of edits is never interrupted.
SAVE_DELAY = 3.0


def percentile(samples, fraction):
    """Nearest rank, the convention the reference measurements and Director use."""
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
        self.status = 'Ready. In Pixelsoup click the B before a scene, or File > Connect to Blender.'
        self.auto_connect = False
        self.needs_new_session = False
        self.retry_at = 0
        self.retry_delay = 1
        self.discovered_at = 0
        self.membership = None
        self.membership_at = 0
        self.paused_by_director = False
        self.collection_id = ''
        self.scene_id = ''
        self.source = ''
        # Given by the Director with the connection: the identity it holds for
        # the bound Pixelsoup scene, and that scene's name.
        self.assigned_source = ''
        self.linked_scene = ''
        # Set when Pixelsoup created this .blend: link identities are stored
        # in the file, so it is saved once after the first accepted sync, and
        # reopening it finds the same objects instead of new ones.
        self.save_after_first_sync = ''
        # When the linked file is next due to be saved (see save_if_due).
        self.save_due = 0
        # One .blend holds the project: every linked Blender scene is tagged
        # with its Pixelsoup scene's source, and the Director sends a
        # pairing per linked scene. This Blender syncs the scene it shows.
        self.links = {}
        self.listed = False
        self.link_key = None
        # A scene the artist disconnected here: not synced again until
        # Pixelsoup shows it, or Blender switches to another scene.
        self.declined = ''
        # A scene the artist asked to add to Pixelsoup: (source, name).
        self.create_request = None
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
        # Qualification evidence. Blender time and Director time are recorded apart;
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
        self.writeback = Writeback(self)
        self.stage = StageReference()
        self.frame_sampling = False

    def mark(self, full=False, geometry=()):
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
            raise ValueError('In Pixelsoup click the B before a scene, or File > Connect to Blender')
        self.stop(reconnecting=True)
        self.needs_new_session = False
        self.scene_id = self.identities.identity(context.scene, context.scene)
        self.collection_id = self.identities.identity(collection, context.scene)
        source = self.assigned_source or str(uuid.uuid5(uuid.UUID(self.scene_id), self.collection_id))
        if source != self.source:
            # What Pixelsoup applied in the scene being left stays with its
            # objects, so returning to that scene reads it back.
            self.writeback.save({obj['ms_link_id']: obj for obj in bpy.data.objects
                                 if isinstance(obj.get('ms_link_id'), str)})
            self.writeback = Writeback(self)
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

    def stop(self, message='Disconnected; Pixelsoup keeps the last synced scene', reconnecting=False):
        if not reconnecting:
            self.needs_new_session = False
            self.auto_connect = False
            self.retry_at = 0
            self.assigned_source = ''
            self.linked_scene = ''
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
        # Pixelsoup's edits stay in the objects: they are the scene.
        self.writeback.rejected()

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
        # The timer runs at display rate during a drag; requests are rare.
        if scene is None or time.monotonic() - self.discovered_at < 0.1:
            return
        self.discovered_at = time.monotonic()
        label = (Path(bpy.data.filepath).name if bpy.data.filepath else 'Unsaved scene') + ' - Blender ' + bpy.app.version_string
        sources = [s.get(SOURCE) for s in bpy.data.scenes if isinstance(s.get(SOURCE), str)]
        if self.create_request and self.create_request[0] in self.links:
            self.create_request = None
        discovery.advertise(label, scene.name, self.status, self.needs_new_session, bpy.data.filepath,
                            scene_source(scene), sources, self.create_request)
        links = discovery.scene_links()
        if links is not None:
            self.listed = True
            self.links = links
        activation = discovery.activation()
        if activation:
            self.activate(activation['source'], activation['name'], activation['adopt'])
        if self.listed:
            self.follow()
        if self.auto_connect and not self.needs_new_session and not self.worker and self.retry_at and time.monotonic() >= self.retry_at:
            self.retry_at = 0
            self.start(bpy.context)

    def follow(self):
        """Sync the scene Blender shows: switching scenes switches pairings."""
        scene = bpy.context.scene
        source = scene_source(scene)
        if self.declined and self.declined != source:
            self.declined = ''
        entry = self.links.get(source) if source != self.declined else None
        if entry is None:
            if self.worker or self.link_key:
                self.stop('This Blender scene is not linked to Pixelsoup')
                self.link_key = None
            if not self.worker:
                self.status = ("Disconnected; click the scene's B in Pixelsoup to sync it again" if self.declined
                               else 'Not in Pixelsoup: Add to Pixelsoup to link this scene'
                               if self.links else 'Ready. In Pixelsoup click the B on a scene.')
            return
        key = (source, entry['port'], entry['code'])
        if key == self.link_key and (self.worker or self.retry_at):
            self.linked_scene = entry['name']
            return
        self.stop(reconnecting=True)
        self.link_key = key
        settings = scene.ms_livelink
        settings.port, settings.code = entry['port'], entry['code']
        self.assigned_source = source
        self.linked_scene = entry['name']
        settings.live = True
        self.auto_connect = True
        self.retry_delay = 1
        self.start(bpy.context)

    def activate(self, source, name, adopt=False):
        """Show the scene linked to `source`, adopting or creating it as `name`.
        `adopt` makes the scene shown now that scene, whatever it was tagged."""
        if self.declined == source:
            self.declined = ''
        target = next((s for s in bpy.data.scenes if s.get(SOURCE) == source), None)
        if target is None and adopt:
            target = bpy.context.scene
        if target is None and name:
            # A scene of that name whose tag no linked Pixelsoup scene uses
            # (the Pixelsoup project was not saved after it was linked).
            target = next((s for s in bpy.data.scenes if s.name == name and s.get(SOURCE) not in self.links), None)
        if target is None:
            shown = bpy.context.scene
            untagged = [s for s in bpy.data.scenes if not isinstance(s.get(SOURCE), str)]
            # A file's first scene (Blender's startup scene, or a file from an
            # add-on that linked one scene per file) becomes this scene.
            if shown and not isinstance(shown.get(SOURCE), str) and (len(bpy.data.scenes) == 1 or shown.name == name):
                target = shown
            elif len(untagged) == 1 and len(bpy.data.scenes) == 1:
                target = untagged[0]
            else:
                target = bpy.data.scenes.new(name or 'Pixelsoup Scene')
        if target.get(SOURCE) != source:
            target[SOURCE] = source
            if name and target.name != name and name not in bpy.data.scenes:
                target.name = name
        for window in bpy.context.window_manager.windows:
            window.scene = target
        return target

    def add_scene(self, scene):
        """Ask Pixelsoup to make a scene for `scene` and link it."""
        if not isinstance(scene.get(SOURCE), str):
            scene[SOURCE] = str(uuid.uuid4())
        self.create_request = (scene[SOURCE], scene.name)
        self.status = 'Adding ' + scene.name + ' to Pixelsoup...'

    def publish_now(self):
        # Explicit refresh must observe repacked bytes even if Blender reused
        # the same PackedFile allocation and encoded size.
        getattr(self.identities, 'packed_images', {}).clear()
        self.requested = True
        self.mark()

    def save_soon(self):
        self.save_due = time.monotonic() + SAVE_DELAY

    def save_if_due(self):
        """Keep the linked .blend holding what Pixelsoup holds.

        Pixelsoup can only be given a scene by Blender, so a file left
        unsaved is a scene waiting to be lost: the next Blender started on it
        (Pixelsoup's B button does exactly that when Blender is closed)
        publishes the older file, and objects added since come back with new
        link identities that Pixelsoup's edits no longer attach to. Once
        syncing has been quiet for SAVE_DELAY, an accepted or written-back
        change is written to the file."""
        if not self.save_due or time.monotonic() < self.save_due:
            return
        context = bpy.context
        if self.busy or self.pending or self.extracting or context.mode != 'OBJECT':
            # Mid-sync or mid-edit: wait for the next quiet moment.
            self.save_soon()
            return
        self.save_due = 0
        # Not gated on is_dirty: link identities the add-on writes into
        # objects push no undo step, so a file holding new ones reads clean.
        if not bpy.data.filepath or not context.scene.ms_livelink.keep_saved:
            return
        try:
            bpy.ops.wm.save_mainfile()
        except RuntimeError as error:
            print('Pixelsoup: could not save the linked file -', error)

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
                self.capabilities = tuple(reply.get('capabilities') or ('base',))
                self.status = 'Connected; syncing scene...'
            elif kind == 'accepted':
                self.busy = False
                self.retry_delay = 1
                self.writeback.accepted()
                if reply.get('payload_bytes'):
                    self.save_soon()
                if self.save_after_first_sync and bpy.data.filepath == self.save_after_first_sync:
                    self.save_after_first_sync = ''
                    try:
                        bpy.ops.wm.save_mainfile()
                    except RuntimeError as error:
                        print('Pixelsoup: could not save link identities -', error)
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
                self.writeback.rejected()
                self.busy = False
                self.pending = False
                self.requested = False
                self.unsupported = reply['message']
                self.status = reply['message']
            elif kind == 'paused':
                self.writeback.rejected()
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
                    self.writeback.receive(reply['payload'])
                except ValueError as error:
                    self.status = str(error)
            elif kind == 'stage':
                try:
                    self.stage.receive(reply['payload'])
                except ValueError as error:
                    if bpy.context.scene.ms_livelink.stage_reference:
                        self.status = 'Stage reference unavailable: ' + str(error)
        if self.writeback.refresh and self.connected:
            # Pixelsoup's edits, written into the objects they belong to.
            scene = bpy.context.scene
            scale = scene.unit_settings.scale_length
            from mathutils import Matrix
            from .extract import CONVERSION
            try:
                if self.writeback.apply(scene, scene.ms_livelink.collection or scene.collection,
                                        CONVERSION @ Matrix.Diagonal((scale, scale, scale, 1))):
                    self.save_soon()
            except (ValueError, ReferenceError) as error:
                self.status = str(error)
        self.save_if_due()
        # Imports, removals, renames and collection moves do not consistently emit
        # object geometry updates. A bounded metadata check catches these without
        # re-extracting meshes or restarting an in-progress batch on idle polls.
        if self.connected and time.monotonic() - self.membership_at >= 0.5:
            self.membership_at = time.monotonic()
            scene = bpy.context.scene
            collection = scene.ms_livelink.collection or scene.collection
            signature = tuple(sorted((o.session_uid, o.name, o.data.session_uid if o.data else 0,
                o.parent.session_uid if o.parent else 0, o.hide_render, o.hide_viewport)
                for o in collection.all_objects if not o.get('ms_link_stage_reference')))
            if self.membership is not None and signature != self.membership:
                self.mark()
            self.membership = signature
        if not self.connected or self.busy or not self.pending or not (self.live or self.requested):
            return
        scene = bpy.context.scene
        if self.identities.identity(scene, scene) != self.scene_id:
            if self.listed:
                # Another scene of the project: sync that one instead.
                self.follow()
            else:
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
                self.unsupported = ('This Pixelsoup build does not support ' + FEATURES['animation']
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
        now = time.monotonic()
        if (self.full or self.dirty) and now - self.changed_at < 0.15 and now - self.pending_since < 0.5:
            # Copying evaluated geometry is the costly part: wait for a pause
            # in a mesh edit, but never hold a continuous one back longer than
            # half a second.
            self.status = 'Pending edits (coalescing)'
            return
        # Placement, lights and materials stream while the artist is still
        # dragging: the next revision leaves as soon as the last is accepted.
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
                raise ValueError('Copied scene exceeds 8 GiB')
            # One non-preemptible Blender evaluation/copy per timer dispatch.
            return
        if batch['generation'] != self.generation:
            self.batch = None
            return
        metadata = {'source': self.source, 'objects': batch['objects'], 'refresh_images': self.requested, 'meshes': [
            {'id': identity, 'lengths': list(map(len, mesh))} for identity, mesh in batch['meshes'].items()],
            'absorb': self.writeback.submitted()}
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
        self.status = 'Syncing to Pixelsoup...'


# The Pixelsoup scene a Blender scene belongs to.
SOURCE = 'ms_scene_source'


def scene_source(scene):
    value = scene.get(SOURCE) if scene else None
    return value if isinstance(value, str) else ''


runtime = Runtime()


_shown = None


def timer():
    global _shown
    try:
        runtime.tick()
    except Exception as error:
        runtime.fail(error)
    # Poll only bounded IPC/status. Geometry work is enabled by dependency events.
    # Redraw only when what the header and panel say has changed.
    shown = (runtime.status, runtime.connected, runtime.busy, runtime.pending, runtime.linked_scene)
    if shown != _shown:
        _shown = shown
        for window in bpy.context.window_manager.windows:
            for area in window.screen.areas:
                if area.type == 'VIEW_3D':
                    area.tag_redraw()
    if not runtime.worker:
        return 0.25
    # While an edit is on its way, poll at display rate: the next revision of a
    # drag leaves the moment the last one is accepted.
    return 0.01 if runtime.busy or runtime.pending else 0.05
