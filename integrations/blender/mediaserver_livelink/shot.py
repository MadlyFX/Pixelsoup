"""Managed static shot layer. Exact affine corrections live in parent inverses.

The artist object retains its matrix_basis, original parent inverse, mesh and
modifier stack. A tagged empty sits between it and its authored parent. Source
extraction skips this empty and reads the original local frame directly.
"""
import shlex
import time
import uuid

import bpy
from mathutils import Matrix

TAG = 'ms_link_shot_wrapper'
ANCHOR = 'ms_link_shot_anchor'
HIDDEN = 'ms_link_source_hidden'


def flatten(matrix):
    return [float(matrix[r][c]) for c in range(4) for r in range(4)]


def matrix(values):
    return Matrix(tuple(tuple(values[c * 4 + r] for c in range(4)) for r in range(4)))


def source_parent(obj):
    parent = obj.parent
    parent = parent.parent if parent and parent.get(TAG) else parent
    return None if parent and parent.get(ANCHOR) else parent


def source_local(obj):
    if obj.parent and obj.parent.get(TAG):
        return obj.matrix_parent_inverse @ obj.matrix_basis
    return obj.parent.matrix_world.inverted() @ obj.matrix_world if obj.parent else obj.matrix_world.copy()


def source_visible(obj):
    if HIDDEN not in obj:
        return not obj.hide_render and obj.visible_get()
    return not obj.hide_render and not obj.hide_viewport and not obj.get(HIDDEN, obj.hide_get())


def qualify(obj):
    current = obj
    while current:
        if not current.get(TAG):
            if current.library or current.override_library or current.type not in ('MESH', 'EMPTY'):
                raise ValueError(obj.name + ': reverse placement requires writable static mesh/empty hierarchy')
            if current.constraints or current.animation_data or current.parent_type != 'OBJECT':
                raise ValueError(obj.name + ': reverse placement refuses constraints, animation, drivers and bone/vertex parents')
            if any(mod.type == 'ARMATURE' for mod in current.modifiers):
                raise ValueError(obj.name + ': reverse placement refuses rigs')
            local_modifiers = {'BEVEL', 'SUBSURF', 'SOLIDIFY', 'TRIANGULATE', 'DECIMATE',
                               'EDGE_SPLIT', 'WEIGHTED_NORMAL', 'WELD', 'SMOOTH'}
            if any(mod.type not in local_modifiers for mod in current.modifiers):
                raise ValueError(obj.name + ': reverse placement requires qualified local-space modifiers')
            if abs(source_local(current).to_3x3().determinant()) < 1e-10:
                raise ValueError(obj.name + ': reverse placement refuses singular transforms')
        current = source_parent(current)


def parse(payload):
    lines = payload.splitlines()
    if not lines or lines[0] != 'MSSHOT 1' or len(lines) > 513:
        raise ValueError('Invalid Director shot state')
    result = {}
    for line in lines[1:]:
        fields = shlex.split(line)
        if len(fields) != 73:
            raise ValueError('Invalid Director shot record')
        node, instance = map(int, fields[:2])
        identity, placement, visibility = fields[2:5]
        if identity in result:
            raise ValueError('Multiple Preview attachments: reverse placement requires one attachment per source')
        result[identity] = {'node': node, 'instance': instance, 'id': identity,
            'placement': placement, 'visibility': visibility, 'held': bool(int(fields[5])),
            'visible_override': int(fields[6]), 'visible': bool(int(fields[7])), 'removed': bool(int(fields[8])),
            **{key: list(map(float, fields[9 + i * 16:25 + i * 16]))
               for i, key in enumerate(('offset', 'held_world', 'alignment', 'world'))}}
    return result


class ShotLayer:
    def __init__(self, runtime):
        self.runtime = runtime
        self.rows = {}
        self.gesture = None
        self.conflict = ''
        self.history = []
        self.source_view = False
        self.applied_source_view = False
        self.refresh = False

    def objects(self):
        collection = bpy.context.scene.ms_livelink.collection
        if not collection:
            return {}
        from .extract import collection_objects
        previous = self.runtime.extracting
        self.runtime.extracting = True
        try:
            if bpy.context.scene.ms_livelink.publish_animation:
                from .animation import ensure_anchor, assembly_id
                identity = assembly_id(self.runtime.collection_id)
                return {identity:ensure_anchor(collection,identity)}
            return {self.runtime.identities.identity(bpy.data.objects[name], bpy.context.scene): bpy.data.objects[name]
                    for name in collection_objects(collection)}
        finally:
            self.runtime.extracting = previous

    def receive(self, payload):
        self.rows = parse(payload)
        if self.history and 'after' not in self.history[-1]:
            previous = self.history[-1]
            if previous['id'] in self.rows:
                previous['after'] = self.rows[previous['id']][previous['property']]
        self.refresh = True

    def apply(self):
        from .extract import CONVERSION
        if self.gesture or bpy.context.mode != 'OBJECT':
            return
        objects = self.objects()
        if self.source_view and self.applied_source_view:
            for obj in objects.values():
                if HIDDEN in obj and obj[HIDDEN] != obj.hide_get():
                    obj[HIDDEN] = obj.hide_get()
        # Validate the complete hierarchy before making any wrapper changes.
        for identity, obj in objects.items():
            if identity in self.rows:
                qualify(obj)
        scale = bpy.context.scene.unit_settings.scale_length
        conversion = CONVERSION @ Matrix.Diagonal((scale, scale, scale, 1))
        anchor = None
        def root_anchor():
            nonlocal anchor
            if anchor is None:
                collection = bpy.context.scene.ms_livelink.collection
                anchor = next((item for item in collection.objects if item.get(ANCHOR)), None)
                if anchor is None:
                    anchor = bpy.data.objects.new('Shot display origin', None)
                    anchor[ANCHOR] = True
                    collection.objects.link(anchor)
                anchor.parent = None
                anchor.matrix_world = Matrix.Identity(4)
                anchor.hide_select = True
            return anchor
        worlds = {}
        def place(identity):
            if identity in worlds:
                return worlds[identity]
            obj = objects[identity]
            row = self.rows.get(identity)
            parent = source_parent(obj)
            parent_id = parent.get('ms_link_id') if parent else None
            upstream = place(parent_id) if parent_id is not None and parent_id in objects else Matrix.Identity(4)
            local = source_local(obj)
            if row and not self.source_view:
                alignment = conversion.inverted() @ matrix(row['alignment']) @ conversion
                basis = (upstream if parent else alignment) @ local
                target = (conversion.inverted() @ matrix(row['held_world']) @ conversion if row['held']
                          else basis @ conversion.inverted() @ matrix(row['offset']) @ conversion)
            else:
                target = upstream @ local
            if row:
                wrapper = obj.parent if obj.parent and obj.parent.get(TAG) == identity else None
                if wrapper is None:
                    wrapper = bpy.data.objects.new('Shot layer · ' + obj.name, None)
                    wrapper[TAG] = identity
                    wrapper.empty_display_type = 'PLAIN_AXES'
                    wrapper.empty_display_size = 0.3
                    obj.users_collection[0].objects.link(wrapper)
                    wrapper.parent = parent if parent else root_anchor()
                    original_inverse, original_basis = obj.matrix_parent_inverse.copy(), obj.matrix_basis.copy()
                    obj.parent = wrapper
                    obj.matrix_parent_inverse, obj.matrix_basis = original_inverse, original_basis
                    obj[HIDDEN] = obj.hide_get()
                if parent is None:
                    wrapper.parent = root_anchor()
                wrapper.matrix_basis = Matrix.Identity(4)
                wrapper.matrix_parent_inverse = upstream.inverted() @ target @ local.inverted()
                obj.hide_set(bool(obj.get(HIDDEN, False)) if self.source_view else not row['visible'])
            worlds[identity] = target
            return target
        self.runtime.extracting = True
        try:
            for identity in objects:
                place(identity)
            bpy.context.view_layer.update()
        finally:
            self.runtime.extracting = False
        self.refresh = False
        self.applied_source_view = self.source_view
        self.runtime.stage.apply()

    def detach(self):
        """Restore authored parenting and visibility; preserve editable channels."""
        self.runtime.extracting = True
        try:
            objects = self.objects()
            wrappers = []
            for obj in objects.values():
                if obj.parent and obj.parent.get(TAG):
                    wrapper = obj.parent
                    inverse, basis = obj.matrix_parent_inverse.copy(), obj.matrix_basis.copy()
                    obj.parent = source_parent(obj)
                    obj.matrix_parent_inverse, obj.matrix_basis = inverse, basis
                    obj.hide_set(bool(obj.get(HIDDEN, False)))
                    if HIDDEN in obj:
                        del obj[HIDDEN]
                    if wrapper not in wrappers:
                        wrappers.append(wrapper)
            for wrapper in wrappers:
                if not wrapper.children:
                    bpy.data.objects.remove(wrapper, do_unlink=True)
            bpy.context.view_layer.update()
        finally:
            self.runtime.extracting = False

    def selected(self):
        obj = bpy.context.view_layer.objects.active
        identity = obj.get(TAG) or obj.get('ms_link_id') if obj else None
        if identity not in self.rows:
            raise ValueError('Select a linked source object or its Shot layer wrapper')
        return identity

    def submit(self, command, payload=''):
        gesture = self.gesture
        self.runtime.worker.submit({'command': command, 'claim': gesture['claim'], 'node': gesture['before']['node'],
                                    'base': gesture['base'], 'payload': payload})
        self.runtime.busy = True

    def begin(self, action='', property_name='placement', identity=None, base=None):
        if self.runtime.busy or not self.runtime.connected or self.gesture:
            raise ValueError('Wait for the pending update or finish the current shot gesture')
        identity = identity or self.selected()
        qualify(self.objects()[identity])
        row = self.rows[identity]
        self.source_view = False
        self.apply()
        self.gesture = {'claim': str(uuid.uuid4()), 'id': identity, 'before': dict(row),
            'base': base or row[property_name], 'property': property_name, 'action': action,
            'ready': False, 'expires': time.monotonic() + 15}
        self.conflict = ''
        self.submit('claim', property_name)

    def world_action(self):
        from .extract import CONVERSION
        scale = bpy.context.scene.unit_settings.scale_length
        conversion = CONVERSION @ Matrix.Diagonal((scale, scale, scale, 1))
        obj = self.objects()[self.gesture['id']]
        return 'world ' + ' '.join(map(str, flatten(conversion @ obj.matrix_world @ conversion.inverted())))

    def commit(self):
        if not self.gesture or not self.gesture['ready']:
            raise ValueError('Wait for the placement claim')
        self.gesture['action'] = self.world_action()
        self.submit('commit', self.gesture['action'])

    def result(self, reply):
        self.runtime.busy = False
        gesture = self.gesture
        if not gesture:
            return
        if not reply['ok']:
            self.conflict = reply['message']
            gesture['ready'] = False
            self.runtime.status = 'Shot conflict: ' + self.conflict
            return
        if reply['command'] == 'claim':
            if gesture['action']:
                self.submit('commit', gesture['action'])
            else:
                self.source_view = False
                gesture['ready'] = True
                obj = self.objects()[gesture['id']]
                for selected in bpy.context.selected_objects:
                    selected.select_set(False)
                obj.parent.select_set(True)
                bpy.context.view_layer.objects.active = obj.parent
                self.runtime.status = 'Editing SHOT layer: move the wrapper, then Commit Shot within 15 seconds'
        else:
            if reply['command'] == 'commit':
                if not gesture.get('undo'):
                    self.history.append(gesture)
                    self.history[:] = self.history[-64:]
                else:
                    self.history.pop()
                self.runtime.status = 'Shot accepted in Preview'
            self.gesture = None
            self.refresh = True

    def undo(self):
        if not self.history:
            raise ValueError('No accepted local shot edit to undo')
        previous = self.history[-1]
        before = previous['before']
        if previous['property'] == 'placement':
            action = 'restore ' + str(int(before['held'])) + ' ' + ' '.join(map(str, before['offset'] + before['held_world']))
        else:
            action = {-1: 'follow', 0: 'hide', 1: 'show'}[before['visible_override']]
        # The accepted postcondition is filled by the immediately following state.
        self.begin(action, previous['property'], previous['id'], previous.get('after', 'missing-postcondition'))
        self.gesture['undo'] = True

    def accept(self):
        if self.gesture and not self.runtime.busy:
            self.submit('release')
        self.gesture = None
        self.conflict = ''
        self.refresh = True

    def retry(self):
        if not self.gesture or not self.conflict:
            return
        previous = self.gesture
        if not previous['action']:
            previous['action'] = self.world_action()
        self.gesture = None
        self.begin(previous['action'], previous['property'], previous['id'])
        self.gesture['undo'] = previous.get('undo', False)
