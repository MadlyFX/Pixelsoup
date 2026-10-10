"""Open a Media Server scene's .blend with the LiveLink add-on running.

Media Server's Open in Blender starts:

    blender [project.blend] --python mediaserver_open.py --
        --mediaserver-file <project.blend> --mediaserver-addon <this directory>
        [--mediaserver-scene-source <uuid> --mediaserver-scene-name <name>]

One .blend holds the project, a Blender scene per Media Server scene. The
scene source picks which one Blender shows: an existing scene tagged with it,
else the file's startup scene, else a new scene.

* The add-on beside this script is used for this session whenever the copy
  installed in Blender is missing, disabled or older, so the Director and the
  add-on always speak the same protocol. Preferences are not changed.
* A file that does not exist yet is created from Blender's startup scene and
  saved where Media Server asked, so the scene and its .blend stay associated.
* The process advertises the launch token Media Server put in its environment;
  the Director recognizes it and connects without any further selection.
"""
import ast
import os
import sys

import addon_utils
import bpy

ADDON = 'mediaserver_livelink'


def arguments():
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    return {argv[i]: argv[i + 1] for i in range(0, len(argv) - 1, 2) if argv[i].startswith('--mediaserver-')}


def declared_version(directory):
    try:
        with open(os.path.join(directory, ADDON, '__init__.py'), encoding='utf-8') as source:
            tree = ast.parse(source.read())
    except OSError:
        return ()
    for node in tree.body:
        if isinstance(node, ast.Assign) and getattr(node.targets[0], 'id', '') == 'bl_info':
            return tuple(ast.literal_eval(node.value).get('version', ()))
    return ()


def ensure_addon(directory):
    bundled = declared_version(directory) if directory else ()
    _, loaded = addon_utils.check(ADDON)
    module = sys.modules.get(ADDON)
    installed = tuple(getattr(module, 'bl_info', {}).get('version', ())) if loaded and module else ()
    if loaded and (not bundled or installed >= bundled):
        return 'installed ' + '.'.join(map(str, installed))
    if not bundled:
        # No copy shipped beside Media Server: enable the installed add-on.
        addon_utils.enable(ADDON, default_set=False)
        return 'installed'
    if loaded:
        addon_utils.disable(ADDON, default_set=False)
    for name in [name for name in sys.modules if name == ADDON or name.startswith(ADDON + '.')]:
        del sys.modules[name]
    if directory not in sys.path:
        sys.path.insert(0, directory)
    try:
        addon_utils.enable(ADDON, default_set=False)
    except Exception:
        pass
    if not addon_utils.check(ADDON)[1]:
        # Fall back to registering the module directly for this session.
        import importlib
        importlib.import_module(ADDON).register()
    return 'bundled ' + '.'.join(map(str, bundled))


def show_sidebar():
    try:
        for window in bpy.context.window_manager.windows:
            for area in window.screen.areas:
                if area.type != 'VIEW_3D':
                    continue
                area.spaces.active.show_region_ui = True
                for region in area.regions:
                    if region.type == 'UI':
                        try:
                            region.active_panel_category = 'Media Server'
                        except (AttributeError, TypeError, ValueError):
                            pass
    except (AttributeError, ReferenceError):
        pass


def create(path, splash, source, name):
    # Runs once the window manager is up; a new file is saved immediately so
    # Media Server keeps pointing at the file the artist is editing.
    bpy.context.preferences.view.show_splash = splash
    try:
        from mediaserver_livelink.runtime import runtime
        if source:
            # Tagged before the first save, so the file keeps which scene it is.
            runtime.activate(source, name)
        if not bpy.data.filepath and path and not os.path.exists(path):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            bpy.ops.wm.save_as_mainfile(filepath=path, check_existing=False)
            # The new file has no link identities yet; keep them once assigned.
            runtime.save_after_first_sync = bpy.data.filepath
    except Exception as error:
        print('Media Server: could not open', path, '-', error)
    show_sidebar()
    return None


options = arguments()
# The splash would cover the scene Media Server just opened. It is decided
# after this script runs, so hide it now and restore the preference once the
# window is up.
show_splash = bpy.context.preferences.view.show_splash
bpy.context.preferences.view.show_splash = False
try:
    print('Media Server LiveLink:', ensure_addon(options.get('--mediaserver-addon', '')))
except Exception as error:
    print('Media Server: the LiveLink add-on could not be enabled -', error)
bpy.app.timers.register(lambda: create(options.get('--mediaserver-file', ''), show_splash,
                                       options.get('--mediaserver-scene-source', ''),
                                       options.get('--mediaserver-scene-name', '')), first_interval=0.3)
