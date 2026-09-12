"""Latest Director sample and managed reference objects; no source publication."""
import math
import time

TAG = 'ms_link_stage_reference'


def parse(payload):
    rows = payload.splitlines()
    if len(rows) != 5 or rows[0] != 'MSSTAGE 1':
        raise ValueError('Invalid Director stage reference')
    header = [int(v) for v in rows[1].split()]
    alignment, camera, lens = ([float(v) for v in row.split()] for row in rows[2:])
    if len(header) != 9 or len(alignment) != 16 or len(camera) != 16 or len(lens) != 7:
        raise ValueError('Invalid stage reference fields')
    if not all(math.isfinite(v) for v in alignment + camera + lens) or min(header) < 0 or not header[3] or not header[4]:
        raise ValueError('Invalid stage reference numbers')
    if not (0 < lens[0] <= 200 and 0 < lens[1] <= 200 and 0 < lens[2] <= 2000 and 0 < lens[3] < lens[4]):
        raise ValueError('Invalid stage reference lens')
    return {'header':header, 'alignment':alignment, 'camera':camera, 'lens':lens}


class StageReference:
    def __init__(self):
        self.latest = None
        self.received_at = 0
        self.applying = False

    def receive(self, payload):
        value = parse(payload)
        if self.latest and value['header'][1] <= self.latest['header'][1]:
            return
        self.latest = value
        self.received_at = time.monotonic()
        self.apply()

    def apply(self):
        import bpy
        from mathutils import Matrix
        from .extract import CONVERSION
        from .shot import matrix
        from .runtime import runtime
        if not self.latest or not bpy.context.scene.ms_livelink.stage_reference:
            return
        scene = bpy.context.scene
        scale = scene.unit_settings.scale_length
        if not math.isfinite(scale) or scale <= 0:
            raise ValueError('Stage references require finite positive scene units')
        self.applying = True
        try:
            collection = next((c for c in scene.collection.children if c.get(TAG)), None)
            if collection is None:
                collection = bpy.data.collections.new('Media Server · Stage references')
                collection[TAG] = True
                scene.collection.children.link(collection)
            def object_for(role, camera=False):
                obj = next((o for o in collection.objects if o.get(TAG) == role), None)
                if obj is None:
                    data = bpy.data.cameras.new('Director reference lens') if camera else None
                    obj = bpy.data.objects.new('Director reference camera' if camera else 'Physical stage origin', data)
                    obj[TAG] = role
                    obj.hide_render = True
                    obj.hide_select = True
                    collection.objects.link(obj)
                return obj
            conversion = CONVERSION @ Matrix.Diagonal((scale,scale,scale,1))
            # Shot display already includes stage alignment. Source display
            # keeps authored coordinates and transforms the reference back.
            source_view = runtime.shot.source_view or not runtime.shot.rows
            world_to_source = conversion.inverted()
            if source_view:
                world_to_source @= matrix(self.latest['alignment']).inverted()
            origin = object_for('origin')
            origin.empty_display_type = 'ARROWS'
            origin.matrix_world = world_to_source @ CONVERSION
            camera = object_for('camera',True)
            # Camera local +Y up / -Z forward is already the renderer convention.
            camera.matrix_world = world_to_source @ matrix(self.latest['camera'])
            width,height,focal,near,far,focus,aperture = self.latest['lens']
            camera.data.type = 'PERSP'
            camera.data.sensor_fit = 'VERTICAL'
            camera.data.sensor_width = width
            camera.data.sensor_height = height
            camera.data.lens = focal
            camera.data.clip_start, camera.data.clip_end = near/scale, far/scale
            camera.data.dof.focus_distance = max(0,focus/scale)
            camera.data.dof.aperture_fstop = max(0.1,aperture)
            camera['sensor_aspect'] = width/height
            camera['show_frame'] = str(self.latest['header'][2])
            camera['show_rate'] = f'{self.latest["header"][3]}/{self.latest["header"][4]}'
            bpy.context.view_layer.update()
        finally:
            self.applying = False

    def view(self, context):
        import bpy
        self.apply()
        camera = next((o for o in bpy.context.scene.objects if o.get(TAG) == 'camera'), None)
        if camera is None or context.area.type != 'VIEW_3D':
            raise ValueError('Enable stage references after connecting to Director')
        context.space_data.use_local_camera = True
        context.space_data.camera = camera
        context.space_data.region_3d.view_perspective = 'CAMERA'
        # A local camera still uses the scene raster for its gate. Set this only
        # on explicit user action; sample delivery never edits the render setup.
        context.scene.render.resolution_x = round(1000 * camera['sensor_aspect'])
        context.scene.render.resolution_y = 1000
        context.scene.render.pixel_aspect_x = context.scene.render.pixel_aspect_y = 1
