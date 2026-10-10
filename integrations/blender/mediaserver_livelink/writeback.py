"""Media Server's edits to linked objects, shown in the objects themselves.

Media Server keeps an operator's edit to a linked object as an override on top
of what Blender sent: a placement offset (or a held world position), a
visibility, light settings, or a deletion. This module writes the result into
the Blender object - its transform, its hide flags, its light data, whether it
is in the scene at all - so the .blend holds what the wall shows: reopening it,
or rendering it offline, gives that scene.

What was applied is remembered per object, so extraction can still send
Blender's own value underneath it: Media Server's Undo keeps working on its
overrides. When the artist then edits one of those properties in Blender, the
edit replaces the override: the next revision sends the artist's value and
names the property to absorb, and the Director drops the override in the same
transaction, so the two never stack.

The record lives in the add-on, not on the objects, because Blender's own Undo
restores object properties along with transforms and would make a reverted
write look untouched. It is written into the .blend when the file is saved and
read back when it is opened. Each write is pushed as its own Blender undo step,
so the artist's Ctrl+Z reverts the most recent change - Media Server's included
- and the revert then reaches Media Server like any other edit.
"""
import json
import math
import shlex
import time

import bpy
from mathutils import Matrix

from .placement import source_parent, source_local, matrix, flatten

RECORD = 'ms_link_applied'
# Media Server light fields Blender can show, in describe_light's order.
LIGHT_INDEX = {'color_r': 0, 'color_g': 1, 'color_b': 2, 'intensity': 3, 'range': 4,
               'inner_cone_turns': 5, 'outer_cone_turns': 6, 'light_width': 7,
               'light_height': 8, 'angular_radius_turns': 9}
# Written in this order: a spot's blend is relative to its cone.
LIGHT_ORDER = ('color_r', 'color_g', 'color_b', 'intensity', 'range', 'outer_cone_turns',
               'inner_cone_turns', 'light_width', 'light_height', 'angular_radius_turns')


def pairs(text):
    result = {}
    for item in filter(None, text.split(',')):
        key, _, value = item.partition('=')
        try:
            number = float(value)
        except ValueError:
            continue
        if math.isfinite(number):
            result[key] = number
    return result


def parse(payload):
    """Rows by object identity, and whether one object has several occurrences."""
    lines = payload.splitlines()
    if not lines or lines[0] != 'MSSHOT 2' or len(lines) > 513:
        raise ValueError('Invalid Director shot state')
    rows, ambiguous = {}, False
    for line in lines[1:]:
        fields = shlex.split(line)
        if len(fields) != 75:
            raise ValueError('Invalid Director shot record')
        identity = fields[2]
        if identity in rows:
            # Two attachments of one source in a scene: one Blender object
            # cannot show both, so neither is written back.
            ambiguous = True
            continue
        rows[identity] = {'node': int(fields[0]), 'instance': int(fields[1]), 'id': identity,
            'held': bool(int(fields[3])), 'visible_override': int(fields[4]), 'visible': bool(int(fields[5])),
            'removed': bool(int(fields[6])), 'deleted': bool(int(fields[7])),
            **{key: list(map(float, fields[8 + i * 16:24 + i * 16]))
               for i, key in enumerate(('offset', 'held_world', 'alignment', 'world'))},
            'light': pairs(fields[72]), 'material': pairs(fields[73]), 'parent': fields[74]}
    return rows, ambiguous


# The record entry each absorbed property clears.
RECORD_KEYS = {'placement': 'placement', 'visibility': 'visible', 'light': 'light', 'deleted': 'deleted'}


def overridden(row):
    return bool(row) and (row['held'] or not is_identity(row['offset']) or row['visible_override'] != -1
                          or row['deleted'])


def close(a, b, tolerance=1e-4):
    return a is not None and b is not None and len(a) == len(b) and all(
        abs(x - y) <= tolerance * max(1.0, abs(y)) for x, y in zip(a, b))


def is_identity(values):
    return close(values, flatten(Matrix.Identity(4)), 1e-6)


def movable(obj):
    """Placement Media Server can write: a plain object-parented transform."""
    return not (obj.library or obj.override_library or obj.constraints or obj.animation_data
                or (obj.parent and obj.parent_type != 'OBJECT'))


class Writeback:
    def __init__(self, runtime):
        self.runtime = runtime
        self.rows = {}
        self.ambiguous = False
        self.refresh = False
        # Identity -> what Media Server last applied: {'placement': {...},
        # 'visible': {...}, 'light': {...}}. Survives Blender's Undo.
        self.applied = {}
        self.known = set()
        # Absorbs gathered for the revision being built - (row identity,
        # property, object identity) - those in the revision on its way, and
        # objects whose rows are ignored until the Director's answer to that
        # revision has arrived.
        self.absorbs = []
        self.sent = []
        self.absorbing = {}
        self.confirmed = set()
        # Last placement sent per identity: float noise from applying and
        # removing an override never becomes a new source revision.
        self.placements = {}
        self.skipped = 0
        self.shown = 0
        self.material_only = 0

    # Director state -------------------------------------------------------
    def receive(self, payload):
        self.rows, self.ambiguous = parse(payload)
        # Rows sent after the absorbing revision was accepted no longer carry
        # those overrides.
        for identity in self.confirmed:
            self.absorbing.pop(identity, None)
        self.confirmed.clear()
        self.material_only = sum(1 for row in self.rows.values() if row['material'])
        self.refresh = True

    def submitted(self):
        self.sent, self.absorbs = self.absorbs, []
        return [[row, kind] for row, kind, _ in self.sent]

    def accepted(self):
        self.confirmed.update(identity for _, _, identity in self.sent)
        self.sent = []

    def rejected(self):
        # The override was not absorbed; let the rows show it again.
        for _, _, identity in self.sent:
            self.absorbing.pop(identity, None)
        self.sent = []

    def effective_rows(self, objects):
        """The row whose placement and visibility each Blender object shows.

        A mesh with materials reaches Media Server as a transform-only object
        and a part per material. A single part is the whole mesh, so an edit to
        it is the object's edit. Several parts moved apart cannot be shown by
        one Blender object; their edits stay in Media Server."""
        parts = {}
        for identity, row in self.rows.items():
            if identity not in objects and row['parent'] in objects:
                parts.setdefault(row['parent'], []).append(identity)
        result, split = {}, 0
        for identity in objects:
            own = self.rows.get(identity)
            derived = parts.get(identity, [])
            if len(derived) == 1 and not overridden(own):
                result[identity] = (self.rows[derived[0]], derived[0])
            else:
                result[identity] = (own, identity)
                split += len(derived) > 1 and any(overridden(self.rows[d]) for d in derived)
        return result, split

    # Persistence ----------------------------------------------------------
    def save(self, objects):
        # Only objects this link has read or written: another scene's objects
        # keep the record their own link left.
        for identity, obj in objects.items():
            if identity not in self.known:
                continue
            record = self.applied.get(identity)
            if record:
                obj[RECORD] = json.dumps(record, separators=(',', ':'))
            elif RECORD in obj:
                del obj[RECORD]

    def load(self, objects):
        for identity, obj in objects.items():
            if identity in self.known:
                continue
            self.known.add(identity)
            if RECORD not in obj:
                continue
            try:
                record = json.loads(obj[RECORD])
            except (TypeError, ValueError):
                continue
            if isinstance(record, dict):
                self.applied[identity] = record

    # Writing Media Server's edits into Blender ------------------------------
    def objects(self, scene, collection):
        if scene.ms_livelink.publish_animation:
            # An animated assembly is placed by its managed anchor.
            from .animation import ensure_anchor, assembly_id
            identity = assembly_id(self.runtime.collection_id)
            return {identity: ensure_anchor(collection, identity)}
        from .extract import collection_objects
        result = {}
        for name in collection_objects(collection):
            obj = bpy.data.objects[name]
            result[self.runtime.identities.identity(obj, scene)] = obj
        return result

    def apply(self, scene, collection, conversion):
        """Write every override the rows carry that Blender does not show yet."""
        self.refresh = False
        if self.ambiguous or bpy.context.mode != 'OBJECT':
            return False
        objects = self.objects(scene, collection)
        self.load(objects)
        inverse = conversion.inverted()
        now = time.monotonic()
        for identity, deadline in list(self.absorbing.items()):
            if now > deadline:
                del self.absorbing[identity]

        effective, skipped = self.effective_rows(objects)
        wrote = self.delete(scene, collection, objects, effective)

        def depth(obj):
            count, parent = 0, source_parent(obj)
            while parent:
                count, parent = count + 1, source_parent(parent)
            return count
        levels = {}
        for identity, obj in objects.items():
            levels.setdefault(depth(obj), []).append((identity, obj))
        shown = 0
        efficacy = scene.ms_livelink.luminous_efficacy
        scale = scene.unit_settings.scale_length
        previous = self.runtime.extracting
        self.runtime.extracting = True
        try:
            for level in sorted(levels):
                moved = False
                for identity, obj in levels[level]:
                    if identity in self.absorbing:
                        continue
                    row, row_id = effective[identity]
                    record = self.applied.setdefault(identity, {})
                    try:
                        placed = self.place(obj, row, row_id, record, conversion, inverse)
                    except ValueError:
                        placed = None
                        skipped += 1
                    moved |= bool(placed)
                    wrote |= bool(placed) | self.show(obj, row, row_id, record) | self.light(obj, row, record, scale, efficacy)
                    if not record:
                        del self.applied[identity]
                    else:
                        shown += 1
                if moved:
                    bpy.context.view_layer.update()
        finally:
            self.runtime.extracting = previous
        self.skipped, self.shown = skipped, shown
        if wrote:
            self.undo_step()
        return wrote

    def delete(self, scene, collection, objects, effective):
        """Take objects Media Server deleted out of the scene, and put back
        those whose deletion Media Server undid.

        A deleted object is unlinked from its collections rather than removed:
        until the file is saved and reopened it can come back, with everything
        Media Server applied to it, whichever side's Undo asks."""
        if scene.ms_livelink.publish_animation:
            return False
        changed = False
        for identity, obj in list(objects.items()):
            row, row_id = effective[identity]
            if not (row and row['deleted']) or identity in self.absorbing:
                continue
            if 'deleted' in self.applied.get(identity, {}):
                # Back in the scene though Media Server still deletes it: the
                # artist restored it (Blender's Undo), which replaces the deletion.
                self.absorb(identity, 'deleted')
                changed = True
                continue
            masters = {s.collection: s.name for s in bpy.data.scenes}
            homes = [['scene', masters[c]] if c in masters else ['collection', c.name] for c in obj.users_collection]
            self.applied.setdefault(identity, {})['deleted'] = {'row': row_id, 'collections': homes}
            for home in list(obj.users_collection):
                home.objects.unlink(obj)
            del objects[identity]
            changed = True
        for identity, record in list(self.applied.items()):
            entry = record.get('deleted')
            if not entry or identity in objects:
                continue
            row = self.rows.get(entry['row'])
            if row is None or row['deleted']:
                continue
            obj = next((o for o in bpy.data.objects if o.get('ms_link_id') == identity), None)
            if obj is not None and not obj.users_collection:
                homes = [bpy.data.scenes[name].collection if kind == 'scene' and name in bpy.data.scenes
                         else bpy.data.collections.get(name) if kind == 'collection' else None
                         for kind, name in entry['collections']]
                homes = [home for home in homes if home is not None] or [collection]
                for home in homes:
                    home.objects.link(obj)
                changed = True
            del record['deleted']
            if not record:
                del self.applied[identity]
        if changed:
            # Membership changed: the next revision removes or restores the
            # source objects.
            self.runtime.mark()
        return changed

    def place(self, obj, row, row_id, record, conversion, inverse):
        want = None
        if row and row['held']:
            want = {'kind': 'held', 'matrix': row['held_world'], 'alignment': row['alignment'], 'row': row_id}
        elif row and not is_identity(row['offset']):
            want = {'kind': 'offset', 'matrix': row['offset'], 'row': row_id}
        have = record.get('placement')
        same = bool(want and have and want['kind'] == have['kind'] and close(want['matrix'], have['matrix'], 1e-6))
        # A held object is rewritten whenever its parent moved; any other
        # placement only when Media Server's override changed.
        if (want is None and have is None) or (same and want['kind'] != 'held'):
            return False
        if not movable(obj):
            raise ValueError(obj.name + ': constraints, animation or a non-object parent; kept in Media Server')
        local = conversion @ source_local(obj) @ inverse
        source = self.source_of(local, have)
        parent = source_parent(obj)
        if want and want['kind'] == 'held':
            parent_world = conversion @ parent.matrix_world @ inverse if parent else Matrix.Identity(4)
            target = parent_world.inverted_safe() @ matrix(want['alignment']).inverted_safe() @ matrix(want['matrix'])
        elif want:
            target = source @ matrix(want['matrix'])
        else:
            target = source
        written = inverse @ target @ conversion
        if same and close(flatten(source_local(obj)), flatten(written)):
            return False
        obj.matrix_world = parent.matrix_world @ written if parent else written
        if want:
            want['local'] = flatten(written)
            if want['kind'] == 'held':
                want['source'] = flatten(source)
            record['placement'] = want
        else:
            record.pop('placement', None)
        return True

    @staticmethod
    def source_of(local, placement):
        """Blender's own placement under what Media Server applied (MS axes)."""
        if not placement:
            return local
        if placement['kind'] == 'held':
            return matrix(placement['source'])
        return local @ matrix(placement['matrix']).inverted_safe()

    def show(self, obj, row, row_id, record):
        want = row['visible_override'] if row else -1
        have = record.get('visible')
        try:
            if want == -1:
                if not have:
                    return False
                obj.hide_render = have['render']
                obj.hide_set(have['hide'])
                del record['visible']
                return True
            if have and have['value'] == want:
                return False
            have = have or {'render': obj.hide_render, 'hide': obj.hide_get()}
            have['row'] = row_id
            hidden = want == 0
            obj.hide_render = hidden
            obj.hide_set(hidden)
        except RuntimeError:
            # Not in the active view layer; shown once it is.
            return False
        have['value'] = want
        record['visible'] = have
        return True

    def light(self, obj, row, record, scale, efficacy):
        if obj.type != 'LIGHT' or obj.data.type not in ('SUN', 'POINT', 'SPOT', 'AREA'):
            return False
        want = {key: value for key, value in (row['light'] if row else {}).items() if key in LIGHT_INDEX}
        have = record.get('light') or {'applied': {}, 'source': {}}
        if want.keys() == have['applied'].keys() and all(close([want[k]], [have['applied'][k]], 1e-6) for k in want):
            return False
        current = light_values(obj, scale, efficacy)
        for key in list(have['applied']):
            if key not in want:
                write_light(obj, key, have['source'][key], scale, efficacy)
                del have['applied'][key], have['source'][key]
        for key in LIGHT_ORDER:
            if key in want:
                have['source'].setdefault(key, current[LIGHT_INDEX[key]])
                write_light(obj, key, want[key], scale, efficacy)
                have['applied'][key] = want[key]
        if have['applied']:
            record['light'] = have
        else:
            record.pop('light', None)
        return True

    def undo_step(self):
        # One undo step per write: the artist's next Ctrl+Z reverts the most
        # recent change, Media Server's included. No window in background runs.
        window = bpy.context.window_manager.windows[0] if bpy.context.window_manager.windows else None
        if window is None:
            return
        try:
            with bpy.context.temp_override(window=window):
                bpy.ops.ed.undo_push(message='Media Server edit')
        except (RuntimeError, TypeError):
            pass

    # Extraction ----------------------------------------------------------
    def absorb(self, identity, kind):
        record = self.applied.get(identity, {})
        entry = record.pop(RECORD_KEYS[kind], None) or {}
        if not record:
            self.applied.pop(identity, None)
        # The override lives on the row it was applied from: the object's own,
        # or the one part of its mesh.
        item = (entry.get('row', identity), kind, identity)
        if item not in self.absorbs:
            self.absorbs.append(item)
        self.absorbing[identity] = time.monotonic() + 5

    def restored(self, identity):
        """An object Media Server deleted is in the scene again: the artist
        brought it back (Blender's Undo), which replaces the deletion."""
        if 'deleted' in self.applied.get(identity, {}):
            self.absorb(identity, 'deleted')

    def placement(self, obj, identity, local):
        """The placement to send for `obj`, given its Blender local (MS axes)."""
        placement = self.applied.get(identity, {}).get('placement')
        result = local
        if placement:
            if close(flatten(source_local(obj)), placement['local']):
                result = self.source_of(local, placement)
            else:
                # The artist moved it: their placement replaces Media Server's.
                self.absorb(identity, 'placement')
        values = flatten(result)
        previous = self.placements.get(identity)
        if previous and close(values, previous, 1e-5):
            values = previous
        self.placements[identity] = values
        return matrix(values)

    def visible(self, obj, identity, current):
        have = self.applied.get(identity, {}).get('visible')
        if not have:
            return current
        hidden = have['value'] == 0
        try:
            untouched = obj.hide_render == hidden and obj.hide_get() == hidden
        except RuntimeError:
            untouched = True
        if untouched:
            return not have['render'] and not have['hide']
        self.absorb(identity, 'visibility')
        return current

    def light_values(self, identity, values):
        have = self.applied.get(identity, {}).get('light')
        if not have:
            return values
        for key, value in have['applied'].items():
            if not close([values[LIGHT_INDEX[key]]], [value]):
                self.absorb(identity, 'light')
                return values
        result = list(values)
        for key, value in have['source'].items():
            result[LIGHT_INDEX[key]] = value
        return result


def node_factors(light):
    color, strength = (1, 1, 1), 1
    if getattr(light, 'use_nodes', False) and light.node_tree:
        output = next((n for n in light.node_tree.nodes if n.type == 'OUTPUT_LIGHT' and n.is_active_output), None)
        emission = output.inputs['Surface'].links[0].from_node if output and len(output.inputs['Surface'].links) == 1 else None
        if emission and emission.type == 'EMISSION' and not any(i.is_linked for i in emission.inputs):
            color = emission.inputs['Color'].default_value[:3]
            strength = emission.inputs['Strength'].default_value
    return color, strength


def light_values(obj, scale, efficacy):
    from .materials import describe_light
    from .translation import notices
    kept = list(notices)
    try:
        described = describe_light(obj, scale, efficacy)
    finally:
        notices[:] = kept
    return described['values'] if described else [0] * 10


def write_light(obj, key, value, scale, efficacy):
    light = obj.data
    color, strength = node_factors(light)
    if key in ('color_r', 'color_g', 'color_b'):
        index = LIGHT_INDEX[key]
        light.color[index] = min(1.0, max(0.0, value / color[index] if color[index] > 0 else value))
    elif key == 'intensity':
        light.energy = max(0.0, value / max(efficacy * strength, 1e-9))
    elif key == 'range':
        obj.ms_link_light_range = min(10000.0, max(0.01, value))
    elif key == 'outer_cone_turns' and light.type == 'SPOT':
        light.spot_size = min(math.pi, max(0.001, value * 4 * math.pi))
    elif key == 'inner_cone_turns' and light.type == 'SPOT':
        outer = light.spot_size / (4 * math.pi)
        light.spot_blend = min(1.0, max(0.0, 1 - value / outer)) if outer > 0 else 0.0
    elif key == 'light_width' and light.type == 'AREA':
        light.size = max(0.001, value / scale)
    elif key == 'light_height' and light.type == 'AREA':
        if light.shape not in ('RECTANGLE', 'ELLIPSE'):
            light.shape = 'RECTANGLE'
        light.size_y = max(0.001, value / scale)
    elif key == 'angular_radius_turns' and light.type == 'SUN':
        light.angle = min(math.pi, max(0.0, value * 4 * math.pi))
