"""Launch with Blender --factory-startup --python this-file for a disposable UI demo.

Loads the repository add-on for this session only. No saved preferences change.
Optional -- --pairing-file path reads an ephemeral local port/code JSON.
"""
import argparse
import json
from pathlib import Path
import sys

import bpy

repo = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(repo / 'integrations/blender'))
import mediaserver_livelink

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--pairing-file', type=Path)
parser.add_argument('--animation', action='store_true', help='Create a node-animation and stage-reference demo')
parser.add_argument('--save-demo', type=Path, help='Save only this disposable fixture to a named blend file')
args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
def prepare():
    # Window/area initialization must finish before changing sidebar visibility.
    # --factory-startup isolates this disposable scene from the user's files.
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    mediaserver_livelink.register()
    collection = bpy.data.collections.new('LiveLink Demo')
    bpy.context.scene.collection.children.link(collection)
    bpy.ops.mesh.primitive_cube_add(size=1.5, location=(0, 0, 0.75))
    obj = bpy.context.view_layer.objects.active
    obj.name = 'LiveLink Hero'
    for owner in list(obj.users_collection):
        owner.objects.unlink(obj)
    collection.objects.link(obj)
    bevel = obj.modifiers.new('Artist bevel', 'BEVEL')
    bevel.width, bevel.segments = 0.12, 3
    material = bpy.data.materials.new('Copper')
    material.use_nodes = True
    material.node_tree.nodes.get('Principled BSDF').inputs['Base Color'].default_value = (0.6, 0.18, 0.04, 1)
    material.node_tree.nodes.get('Principled BSDF').inputs['Metallic'].default_value = 0.5
    obj.data.materials.append(material)
    settings = bpy.context.scene.ms_livelink
    settings.collection = collection
    if args.animation:
        bpy.ops.object.modifier_apply(modifier=bevel.name)
        parent=bpy.data.objects.new('Animated parent',None)
        collection.objects.link(parent);obj.parent=parent
        parent.location=(0,0,0);parent.keyframe_insert('location',frame=1)
        parent.location=(2,0,0);parent.keyframe_insert('location',frame=25)
        bpy.context.scene.frame_start=1;bpy.context.scene.frame_end=25
        bpy.context.scene.render.fps=24;bpy.context.scene.frame_set(1)
        settings.publish_animation=True;settings.animation_start=10;settings.stage_reference=True
    if args.pairing_file:
        config = json.loads(args.pairing_file.read_text())
        settings.port, settings.code = config['port'], config['code']
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                with bpy.context.temp_override(window=window, area=area):
                    area.spaces.active.show_region_ui = True
                    area.spaces.active.region_3d.view_distance = 7
    print('LiveLink demo ready: Sidebar > Media Server > Connect & Publish')
    if args.save_demo:
        bpy.ops.wm.save_as_mainfile(filepath=str(args.save_demo.resolve()))


if args.save_demo:
    # Background Blender exits before an app timer fires; the sample deliverable
    # is written by this run, not by a later interactive one.
    prepare()
else:
    bpy.app.timers.register(prepare, first_interval=0.5)
