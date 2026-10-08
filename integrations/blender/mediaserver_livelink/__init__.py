"""Install this directory as a zipped legacy add-on in the pinned Blender build."""
# Blender reads this dict with ast.literal_eval before importing the module, so
# every value must stay a literal. wire.py holds the same version and the
# negotiated capability list; test_publisher_core.py checks that they agree.
bl_info = {
    'name': 'Media Server LiveLink', 'author': 'Media Server', 'version': (0, 8, 2),
    'blender': (5, 2, 0), 'location': '3D View > Sidebar > Media Server',
    'description': 'Automatically sync Blender scenes to Media Server Preview',
    'category': 'Import-Export',
}

import bpy
from bpy.app.handlers import persistent
from bpy.props import EnumProperty, BoolProperty, FloatProperty, IntProperty, PointerProperty, StringProperty

from .runtime import percentile, runtime, timer
from .wire import CAPABILITIES, PROTOCOL_VERSION


def live_changed(self, context):
    runtime.live = self.live
    if not self.live:
        runtime.batch = None
        runtime.status = 'Paused' + ('; submitted revision is finishing' if runtime.busy else '')
    else:
        runtime.mark()


def environment_changed(self, context):
    runtime.mark(full=True)


def stage_changed(self, context):
    runtime.stage.apply()


class MSLinkSettings(bpy.types.PropertyGroup):
    collection: PointerProperty(name='Collection', type=bpy.types.Collection)
    port: IntProperty(name='Director port', default=45001, min=1024, max=65535)
    # A Python-only getter/setter is required: SKIP_SAVE alone does not keep
    # custom Scene properties out of .blend serialization on the pinned build.
    code: StringProperty(name='Pairing code', subtype='PASSWORD', options={'SKIP_SAVE'},
                         get=lambda self: runtime.pairing_code,
                         set=lambda self, value: setattr(runtime, 'pairing_code', value))
    advanced: BoolProperty(name='Advanced publishing controls', default=False)
    live: BoolProperty(name='Live updates', default=True, update=live_changed)
    luminous_efficacy: FloatProperty(name='Light efficacy (lm/W)',default=683,min=1,max=683,update=environment_changed,
        description='Explicit RGB approximation: normalized Blender watts become lumens; sun W/m2 becomes lux')
    export_density: IntProperty(name='Export density (%)', default=100, min=0, max=100, update=environment_changed,
        description='Deterministic subset of evaluated scatter; does not change Blender viewport density')
    density_seed: IntProperty(name='Export sampling seed', default=0, min=0, update=environment_changed)
    publish_animation: BoolProperty(name='Animated assembly',default=False,
        description='Explicitly bake the playback range as one node/skin clip; Publish Now after source edits')
    animation_start: IntProperty(name='Start at show frame',default=0,min=0)
    animation_loop: BoolProperty(name='Loop clip',default=True)
    stage_reference: BoolProperty(name='Stage references',default=False,update=stage_changed)


class MSLinkStageView(bpy.types.Operator):
    bl_idname = 'ms_livelink.stage_view'
    bl_label = 'View Director Camera'
    bl_description = 'Use the managed reference camera and its sensor aspect in this viewport'

    def execute(self, context):
        try:
            runtime.stage.view(context)
        except ValueError as error:
            self.report({'ERROR'},str(error))
            return {'CANCELLED'}
        return {'FINISHED'}


class MSLinkConnect(bpy.types.Operator):
    bl_idname = 'ms_livelink.connect'
    bl_label = 'Connect & Publish'
    bl_description = 'Pair with the local Director and publish the selected collection to Preview'
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        try:
            runtime.start(context)
        except Exception as error:
            runtime.fail(error)
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        return {'FINISHED'}


class MSLinkDisconnect(bpy.types.Operator):
    bl_idname = 'ms_livelink.disconnect'
    bl_label = 'Disconnect'

    def execute(self, context):
        runtime.stop()
        return {'FINISHED'}


class MSLinkPaste(bpy.types.Operator):
    bl_idname = 'ms_livelink.paste_details'
    bl_label = 'Paste Director Details'
    bl_description = 'Paste the endpoint and code copied from Media Server Scene Link Details'

    def execute(self, context):
        from .wire import pairing_details
        try:
            port, code = pairing_details(context.window_manager.clipboard)
        except ValueError as error:
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        context.scene.ms_livelink.port = port
        context.scene.ms_livelink.code = code
        runtime.status = 'Pairing details pasted; ready to connect'
        return {'FINISHED'}


class MSLinkPublish(bpy.types.Operator):
    bl_idname = 'ms_livelink.publish'
    bl_label = 'Publish Now'
    bl_description = 'Publish pending changes once, including while live updates are paused'

    @classmethod
    def poll(cls, context):
        return runtime.connected

    def execute(self, context):
        runtime.publish_now()
        return {'FINISHED'}


class MSLinkResync(bpy.types.Operator):
    bl_idname = 'ms_livelink.resync'
    bl_label = 'Resync'
    bl_description = 'Reconnect, register the current Director revision and publish a fresh evaluated snapshot'
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        return MSLinkConnect.execute(self, context)


class MSLinkBakeColor(bpy.types.Operator):
    bl_idname = 'ms_livelink.bake_color'
    bl_label = 'Bake Base Color Copy'
    bl_description = 'Bake a single-material mesh to UV0 on a separate Blender process; keep the original material'

    def execute(self, context):
        from .bake_color import start
        try:
            start(context)
        except Exception as error:
            self.report({'ERROR'},str(error))
            return {'CANCELLED'}
        return {'FINISHED'}


class MSLinkPanel(bpy.types.Panel):
    bl_label = 'Media Server LiveLink'
    bl_idname = 'VIEW3D_PT_ms_livelink'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Media Server'

    def draw(self, context):
        layout = self.layout
        settings = context.scene.ms_livelink
        layout.label(text='Changes sync automatically to Preview')
        if not runtime.worker:
            layout.label(text='Media Server: File > Connect to Blender')
        else:
            row = layout.row(align=True)
            row.operator('ms_livelink.resync', text='Resync Scene')
            row.operator('ms_livelink.disconnect')
        import textwrap
        for line in textwrap.wrap(runtime.status, max(24, int(context.region.width / 7))):
            layout.label(text=line)
        if runtime.unsupported:
            box = layout.box()
            box.label(text='Adjusted or skipped during sync', icon='INFO')
            for notice in runtime.unsupported.splitlines()[:8]:
                for line in textwrap.wrap(notice, max(24, int(context.region.width / 7))):
                    box.label(text=line)
        layout.prop(settings, 'advanced')
        if not settings.advanced:
            return
        layout.prop(settings, 'collection', text='Limit to collection')
        layout.prop(settings, 'live')
        layout.operator('ms_livelink.publish')
        animation = layout.box()
        animation.label(text='Animation and stage context')
        mode = animation.row()
        mode.enabled = runtime.worker is None
        mode.prop(settings,'publish_animation')
        if settings.publish_animation:
            animation.prop(settings,'animation_start')
            animation.prop(settings,'animation_loop')
            animation.label(text=f'Bake range: {context.scene.frame_start} – {context.scene.frame_end}')
            animation.label(text='Publish Now bakes the edited clip')
            animation.label(text='Shot placement moves the assembly')
        animation.prop(settings,'stage_reference')
        if settings.stage_reference:
            animation.operator('ms_livelink.stage_view')
            if runtime.stage.latest:
                import time
                sample = runtime.stage.latest['header']
                state = 'Held reference' if sample[7] or time.monotonic()-runtime.stage.received_at > 1 else 'Tracked' if sample[6] else 'Authored camera'
                animation.label(text=f'{state} · show frame {sample[2]}')
                animation.label(text=f'Rate: {sample[3]}/{sample[4]}')
            else:
                animation.label(text='Waiting for Director sample')
            animation.label(text='Reference gate; no lens distortion')
            animation.label(text='Final ICVFX remains in Media Server')
        look = layout.box()
        look.label(text='Materials and lighting')
        look.prop(settings,'luminous_efficacy')
        active = context.active_object
        if active and active.type == 'LIGHT':
            look.prop(active,'ms_link_light_range')
            look.label(text='Normalized power; Point/Spot radius = 0')
        if active and active.active_material:
            look.label(text='Alpha handling')
            look.prop(active.active_material,'ms_link_alpha',text='')
            if active.active_material.ms_link_alpha == 'MASK':
                look.prop(active.active_material,'ms_link_alpha_cutoff')
            look.operator('ms_livelink.bake_color')
        look.label(text='UV0: sRGB color images')
        look.label(text='Non-Color normal / packed maps')
        look.label(text='Save images before publishing')
        environment = layout.box()
        environment.label(text='Foliage and terrain')
        environment.prop(settings,'export_density')
        if settings.export_density < 100:
            environment.prop(settings,'density_seed')
        environment.label(text='Scatter edits apply to groups',icon='INFO')
        active = context.active_object
        if active and active.type == 'MESH' and not active.get('ms_shot_wrapper'):
            environment.label(text=active.name)
            environment.prop(active,'ms_link_terrain_size')
            environment.label(text='Authored low detail mesh')
            environment.prop(active,'ms_link_lod',text='')
            if active.ms_link_lod:
                environment.prop(active,'ms_link_lod_pixels')
        environment.label(text='4096 instances / 64 geometry resources')
        if runtime.connected and runtime.shot.rows:
            shot = layout.box()
            shot.label(text='Display: ' + ('SOURCE' if runtime.shot.source_view else 'SHOT'))
            shot.label(text='Editing: ' + ('SHOT' if runtime.shot.gesture else 'SOURCE'))
            shot.operator('ms_livelink.shot', text='Show Shot Placement' if runtime.shot.source_view else 'Show Source Placement').action = 'view'
            if runtime.shot.conflict:
                shot.label(text='Shot conflict — resolution required', icon='ERROR')
                row = shot.row(align=True)
                row.operator('ms_livelink.shot', text='Accept Director').action = 'accept'
                row.operator('ms_livelink.shot', text='Retry My Shot').action = 'retry'
            elif runtime.shot.gesture:
                row = shot.row(align=True)
                row.enabled = runtime.shot.gesture['ready'] and not runtime.busy
                row.operator('ms_livelink.shot', text='Commit Shot').action = 'commit'
                row.operator('ms_livelink.shot', text='Cancel Shot').action = 'accept'
            else:
                controls = shot.column()
                controls.enabled = not runtime.busy and context.mode == 'OBJECT'
                controls.operator('ms_livelink.shot', text='Begin Shot Gesture').action = 'begin'
                row = controls.row(align=True)
                row.operator('ms_livelink.shot', text='Hold Shot').action = 'hold'
                row.operator('ms_livelink.shot', text='Follow Source').action = 'follow'
                controls.operator('ms_livelink.shot', text='Reset Shot Placement').action = 'reset'
                row = controls.row(align=True)
                row.operator('ms_livelink.shot', text='Hide').action = 'hide'
                row.operator('ms_livelink.shot', text='Show').action = 'show'
                controls.operator('ms_livelink.shot', text='Follow Source Visibility').action = 'visibility'
                if runtime.shot.history:
                    controls.operator('ms_livelink.shot', text='Undo My Last Shot Edit').action = 'undo'
            shot.label(text='Object transforms edit source')
            shot.label(text='Edit Mode changes source mesh')
        box = layout.box()
        # Wrap long diagnostics to the actual sidebar width.
        import textwrap
        width = max(24, int(context.region.width / 7))
        for line in textwrap.wrap(runtime.status, width):
            box.label(text=line)
        for index, line in enumerate(textwrap.wrap(runtime.unsupported, width)):
            box.label(text=line, icon='ERROR' if index == 0 else 'NONE')
        if runtime.revision:
            box.label(text='Revision: ' + runtime.revision[:12])
        if runtime.worker or runtime.exports:
            box.label(text=f'Mesh extractions: {runtime.exports}')
            box.label(text=f'GLB builds: {runtime.serializations}')
            box.label(text=f'Last update: {runtime.payload_bytes} bytes')
            if runtime.last_extract_ms > 50:
                box.label(text=f'Last evaluation/copy: {runtime.last_extract_ms:.0f} ms')
        support = layout.box()
        support.label(text='Compatibility and evidence')
        version = '.'.join(map(str, bl_info['version']))
        support.label(text=f'Add-on {version} · link protocol v{PROTOCOL_VERSION}')
        support.label(text='Blender ' + bpy.app.version_string)
        if runtime.capabilities:
            optional = [name for name in runtime.capabilities if name != 'base']
            support.label(text='Director serves: ' + (', '.join(optional) or 'base only'))
            missing = [name for name in CAPABILITIES if name not in runtime.capabilities]
            if missing:
                support.label(text='Not served: ' + ', '.join(missing), icon='ERROR')
        elif runtime.worker:
            support.label(text='Negotiating capabilities...')
        if runtime.latency:
            support.label(text=f'Event to Preview p50 {percentile(runtime.latency, 0.5)} ms'
                               f' · p95 {percentile(runtime.latency, 0.95)} ms')
            # The two shares are measured, not a partition: the difference is
            # process IPC and the receipt hop.
            support.label(text=f'Last {runtime.last_latency_ms} ms · Blender'
                               f' {runtime.last_blender_ms} · Director {runtime.last_director_ms}')
            support.label(text=f'Accepted revisions measured: {len(runtime.latency)}')


@persistent
def depsgraph_changed(scene, depsgraph):
    if runtime.extracting or runtime.stage.applying or runtime.frame_sampling:
        return
    from .bake_color import invalidate
    invalidate()
    from .animation import invalidate as animation_invalidate
    # Managed references and their camera datablock are transient display data.
    authored = [u for u in depsgraph.updates if not (isinstance(u.id.original,bpy.types.Object)
        and u.id.original.get('ms_link_stage_reference')) and not isinstance(u.id.original,bpy.types.Camera)]
    if authored:
        animation_invalidate()
    if not runtime.worker:
        return
    geometry = set()
    changed = False
    full = False
    for update in depsgraph.updates:
        data = update.id.original
        if isinstance(data, bpy.types.Object):
            if data.get('ms_link_shot_wrapper') or data.get('ms_link_shot_anchor'):
                runtime.shot.refresh = True
                continue
            if data.get('ms_link_stage_reference'):
                continue
            changed = True
            if update.is_updated_geometry:
                geometry.add(data.session_uid)
        elif isinstance(data, bpy.types.Mesh):
            changed = True
            geometry.add(data.session_uid)
        elif isinstance(data, (bpy.types.Material, bpy.types.Collection, bpy.types.Scene, bpy.types.Light, bpy.types.Image)):
            changed = True
        elif isinstance(data, bpy.types.NodeTree):
            # Shader trees only change bindings; geometry node groups may affect any prototype.
            changed = True
            full |= data.bl_idname == 'GeometryNodeTree'
    if changed:
        runtime.mark(full=full, geometry=geometry)


@persistent
def history_changed(*args):
    from .animation import invalidate
    invalidate()
    if runtime.worker:
        runtime.mark(full=True)


@persistent
def load_pre(*args):
    from .bake_color import cancel
    cancel()
    runtime.stop('File changed; reconnect to publish the saved collection')
    runtime.pairing_code = ''
    # No RNA wrappers or per-file fallback identities survive file replacement.
    from .extract import Identities
    runtime.identities = Identities()
    from .shot import ShotLayer
    runtime.shot = ShotLayer(runtime)


@persistent
def load_post(*args):
    if not bpy.app.timers.is_registered(timer):
        bpy.app.timers.register(timer, first_interval=0.25, persistent=True)
    subscribe()


def metadata_changed():
    if runtime.worker:
        runtime.mark()


def subscribe():
    bpy.msgbus.clear_by_owner(runtime)
    for kind, property_name in ((bpy.types.Object, 'name'), (bpy.types.Collection, 'name'),
                                (bpy.types.Object, 'hide_render'), (bpy.types.Object, 'mode')):
        bpy.msgbus.subscribe_rna(key=(kind, property_name), owner=runtime, args=(),
                                notify=metadata_changed, options={'PERSISTENT'})


class MSLinkShot(bpy.types.Operator):
    bl_idname = 'ms_livelink.shot'
    bl_label = 'Shot Layer'
    bl_description = 'Validate a shot-layer edit with the Director; source data remains independently editable'
    action: StringProperty()

    def execute(self, context):
        try:
            shot = runtime.shot
            if self.action == 'begin':
                shot.begin()
            elif self.action == 'commit':
                shot.commit()
            elif self.action in ('accept', 'retry', 'undo'):
                getattr(shot, self.action)()
            elif self.action == 'view':
                shot.source_view = not shot.source_view
                shot.refresh = True
            elif self.action in ('hide', 'show', 'visibility'):
                shot.begin('follow' if self.action == 'visibility' else self.action, 'visibility')
            else:
                shot.begin(self.action)
        except Exception as error:
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        return {'FINISHED'}


@persistent
def frame_pre(*args):
    runtime.frame_sampling = True


@persistent
def frame_post(*args):
    runtime.frame_sampling = False
    if runtime.worker and not bpy.context.scene.ms_livelink.publish_animation:
        runtime.mark(full=True)


CLASSES = (MSLinkSettings, MSLinkConnect, MSLinkDisconnect, MSLinkPaste, MSLinkPublish, MSLinkResync, MSLinkShot, MSLinkBakeColor, MSLinkStageView, MSLinkPanel)
HANDLERS = (('depsgraph_update_post', depsgraph_changed), ('undo_post', history_changed),
            ('redo_post', history_changed), ('load_pre', load_pre), ('load_post', load_post),
            ('frame_change_pre',frame_pre),('frame_change_post',frame_post))


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.ms_livelink = PointerProperty(type=MSLinkSettings)
    bpy.types.Material.ms_link_alpha = EnumProperty(name='Link alpha',items=[('OPAQUE','Opaque','Ignore alpha'),('MASK','Mask','Cutout foliage'),('BLEND','Blend','Transparent surface')],default='OPAQUE',update=environment_changed)
    bpy.types.Material.ms_link_alpha_cutoff = FloatProperty(name='Alpha cutoff',default=0.5,min=0,max=1,update=environment_changed)
    bpy.types.Object.ms_link_light_range = FloatProperty(name='Light cutoff (m)',default=20,min=0.01,max=10000,update=environment_changed)
    bpy.types.Object.ms_link_terrain_size = FloatProperty(name='Terrain chunk size (m)',default=0,min=0,
        description='Zero exports a mesh; positive values split terrain in its local XY plane',update=environment_changed)
    bpy.types.Object.ms_link_lod = PointerProperty(name='Authored low detail mesh',type=bpy.types.Object,update=environment_changed)
    bpy.types.Object.ms_link_lod_pixels = FloatProperty(name='LOD below canvas pixels',default=64,min=1,max=4096,update=environment_changed)
    for name, callback in HANDLERS:
        handlers = getattr(bpy.app.handlers, name)
        if callback not in handlers:
            handlers.append(callback)
    from .discovery import discovery
    discovery.start()
    load_post()


def unregister():
    from .discovery import discovery
    discovery.stop()
    from .bake_color import cancel
    cancel()
    runtime.stop('Add-on disabled')
    runtime.pairing_code = ''
    bpy.msgbus.clear_by_owner(runtime)
    if bpy.app.timers.is_registered(timer):
        bpy.app.timers.unregister(timer)
    for name, callback in HANDLERS:
        handlers = getattr(bpy.app.handlers, name)
        if callback in handlers:
            handlers.remove(callback)
    if hasattr(bpy.types.Scene, 'ms_livelink'):
        del bpy.types.Scene.ms_livelink
    for name in ('ms_link_alpha','ms_link_alpha_cutoff'):
        if hasattr(bpy.types.Material,name): delattr(bpy.types.Material,name)
    for name in ('ms_link_terrain_size','ms_link_lod','ms_link_lod_pixels','ms_link_light_range'):
        if hasattr(bpy.types.Object,name):
            delattr(bpy.types.Object,name)
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == '__main__':
    register()
