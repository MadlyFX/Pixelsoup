"""Install this directory as a zipped add-on in the pinned Blender build."""
# Blender reads this dict with ast.literal_eval before importing the module, so
# every value must stay a literal. wire.py holds the same version and the
# negotiated capability list; test_publisher_core.py checks that they agree.
bl_info = {
    'name': 'Pixelsoup Live Link', 'author': 'Pixelsoup', 'version': (0, 10, 1),
    'blender': (5, 2, 0), 'location': '3D View > Sidebar > Pixelsoup',
    'description': 'Live-link Blender scenes with Pixelsoup scenes',
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
    keep_saved: BoolProperty(name='Keep file saved', default=True,
        description="Save the .blend a few seconds after each change Pixelsoup accepts or writes back, "
                    "so reopening it - Pixelsoup's B button starts Blender on it - gives the scene "
                    "Pixelsoup holds")
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
    bl_description = 'Pair with the local Director and sync the selected collection to its Pixelsoup scene'
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
        from .runtime import scene_source
        runtime.declined = scene_source(context.scene)
        runtime.stop()
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


def session_only():
    """True when Pixelsoup loaded this add-on from beside itself for this
    session only, rather than from Blender's own add-on folder."""
    import os
    installed = bpy.utils.user_resource('SCRIPTS', path='addons')
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normcase(os.path.dirname(here)) != os.path.normcase(os.path.abspath(installed))


class MSLinkInstall(bpy.types.Operator):
    bl_idname = 'ms_livelink.install'
    bl_label = 'Install Add-on Permanently'
    bl_description = ('Copy this add-on into Blender so opening a linked .blend directly '
                      'also reconnects it to its Pixelsoup scene')

    def execute(self, context):
        import os
        import shutil
        source = os.path.dirname(os.path.abspath(__file__))
        target = os.path.join(bpy.utils.user_resource('SCRIPTS', path='addons', create=True), __name__)
        try:
            os.makedirs(target, exist_ok=True)
            for name in os.listdir(source):
                if name.endswith('.py'):
                    shutil.copy2(os.path.join(source, name), os.path.join(target, name))
            # Recorded as enabled; this session keeps running the copy it has.
            if not context.preferences.addons.get(__name__):
                context.preferences.addons.new().module = __name__
            bpy.ops.wm.save_userpref()
        except Exception as error:
            self.report({'ERROR'}, 'Could not install the add-on: ' + str(error))
            return {'CANCELLED'}
        self.report({'INFO'}, 'Pixelsoup Live Link installed for future Blender sessions')
        return {'FINISHED'}


class MSLinkAddScene(bpy.types.Operator):
    bl_idname = 'ms_livelink.add_scene'
    bl_label = 'Add to Pixelsoup'
    bl_description = 'Create a Pixelsoup scene for this Blender scene and link the two'

    def execute(self, context):
        runtime.add_scene(context.scene)
        return {'FINISHED'}


class MSLinkPanel(bpy.types.Panel):
    bl_label = 'Pixelsoup Live Link'
    bl_idname = 'VIEW3D_PT_ms_livelink'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Pixelsoup'

    def draw(self, context):
        layout = self.layout
        settings = context.scene.ms_livelink
        from .runtime import scene_source
        if runtime.worker and runtime.linked_scene:
            layout.label(text='Linked to Pixelsoup scene:', icon='LINKED')
            layout.label(text=runtime.linked_scene)
        elif runtime.listed and scene_source(context.scene) not in runtime.links:
            # Another scene of the project's file is linked; this one is not.
            box = layout.box()
            box.label(text='This scene is not in Pixelsoup', icon='UNLINKED')
            if runtime.create_request and runtime.create_request[0] == scene_source(context.scene):
                box.label(text='Adding it to Pixelsoup...')
            else:
                box.operator('ms_livelink.add_scene', icon='ADD')
        else:
            layout.label(text='Changes sync automatically to Pixelsoup')
        if runtime.listed and len(runtime.links) > 1:
            layout.label(text=f'{len(runtime.links)} scenes in this file are linked; the one shown syncs')
        if session_only():
            layout.operator('ms_livelink.install', icon='IMPORT')
        if not runtime.worker:
            if not runtime.listed:
                layout.label(text='Pixelsoup: click the B before a scene, or')
                layout.label(text='File > Connect to Blender')
        else:
            row = layout.row(align=True)
            row.operator('ms_livelink.resync', text='Resync Scene')
            row.operator('ms_livelink.disconnect')
        import textwrap
        for line in textwrap.wrap(runtime.status, max(24, int(context.region.width / 7))):
            layout.label(text=line)
        writeback = runtime.writeback
        if runtime.connected and (writeback.shown or writeback.skipped or writeback.material_only or writeback.ambiguous):
            edits = layout.box()
            edits.label(text='Pixelsoup edits', icon='SYNC')
            if writeback.shown:
                edits.label(text=f'{writeback.shown} objects show edits made in Pixelsoup')
            if writeback.skipped:
                edits.label(text=f'{writeback.skipped} kept in Pixelsoup only (constraints or animation)')
            if writeback.material_only:
                edits.label(text=f'{writeback.material_only} material overrides kept in Pixelsoup only')
            if writeback.ambiguous:
                edits.label(text='Linked twice in this scene: edits stay in Pixelsoup', icon='ERROR')
            edits.label(text='Edit anything here: the latest change wins')
        if runtime.unsupported and runtime.worker:
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
        layout.prop(settings, 'keep_saved')
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
            animation.label(text='Final ICVFX remains in Pixelsoup')
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
        if active and active.type == 'MESH':
            environment.label(text=active.name)
            environment.prop(active,'ms_link_terrain_size')
            environment.label(text='Authored low detail mesh')
            environment.prop(active,'ms_link_lod',text='')
            if active.ms_link_lod:
                environment.prop(active,'ms_link_lod_pixels')
        environment.label(text='4096 instances / 64 geometry resources')
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
            support.label(text=f'Edit to Pixelsoup p50 {percentile(runtime.latency, 0.5)} ms'
                               f' · p95 {percentile(runtime.latency, 0.95)} ms')
            # The two shares are measured, not a partition: the difference is
            # process IPC and the receipt hop.
            support.label(text=f'Last {runtime.last_latency_ms} ms · Blender'
                               f' {runtime.last_blender_ms} · Director {runtime.last_director_ms}')
            support.label(text=f'Accepted revisions measured: {len(runtime.latency)}')


def header_status(self, context):
    # Always visible while linked: which Pixelsoup scene this Blender edits.
    if not runtime.worker:
        if runtime.listed and runtime.links:
            self.layout.label(text='Pixelsoup: scene not linked', icon='UNLINKED')
        return
    text = 'Pixelsoup' + (': ' + runtime.linked_scene if runtime.linked_scene else '')
    if runtime.busy or runtime.pending:
        text += ' (syncing)'
    self.layout.label(text=text, icon='LINKED' if runtime.connected else 'UNLINKED')


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
            if isinstance(data, bpy.types.Image):
                getattr(runtime.identities, 'packed_images', {}).pop(data.session_uid, None)
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
        getattr(runtime.identities, 'packed_images', {}).clear()
        runtime.mark(full=True)
        # Undo may have restored objects from before a Pixelsoup edit;
        # extraction sends what Blender now shows, and the rows are checked again.
        runtime.writeback.refresh = True


@persistent
def load_pre(*args):
    from .bake_color import cancel
    cancel()
    runtime.stop('File changed; reconnect to publish the saved collection')
    runtime.pairing_code = ''
    # No RNA wrappers or per-file fallback identities survive file replacement.
    from .extract import Identities
    runtime.identities = Identities()
    from .writeback import Writeback
    runtime.writeback = Writeback(runtime)


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


@persistent
def save_pre(*args):
    # What Pixelsoup applied goes into the file with the objects it was
    # applied to, so reopening it extracts Blender's own values correctly.
    runtime.writeback.save({obj['ms_link_id']: obj for obj in bpy.data.objects
                            if isinstance(obj.get('ms_link_id'), str)})


@persistent
def frame_pre(*args):
    runtime.frame_sampling = True


@persistent
def frame_post(*args):
    runtime.frame_sampling = False
    if runtime.worker and not bpy.context.scene.ms_livelink.publish_animation:
        runtime.mark(full=True)


CLASSES = (MSLinkSettings, MSLinkInstall, MSLinkConnect, MSLinkDisconnect, MSLinkPublish, MSLinkResync, MSLinkBakeColor, MSLinkStageView, MSLinkAddScene, MSLinkPanel)
HANDLERS = (('depsgraph_update_post', depsgraph_changed), ('undo_post', history_changed),
            ('redo_post', history_changed), ('load_pre', load_pre), ('load_post', load_post), ('save_pre', save_pre),
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
    bpy.types.VIEW3D_HT_header.append(header_status)
    from .discovery import discovery
    discovery.start()
    load_post()


def unregister():
    bpy.types.VIEW3D_HT_header.remove(header_status)
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
