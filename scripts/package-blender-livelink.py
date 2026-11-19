"""Build a reproducible installable add-on ZIP, without preferences or caches.

The archive carries `manifest.json`: add-on version, pinned Blender build, link
protocol version, negotiated capabilities and per-file SHA-256 values. Operators
use it to check an installed copy, plan an upgrade and verify a removal; the
Director refuses features this add-on does not declare, and vice versa.
"""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import sys
import zipfile

repo = Path(__file__).resolve().parents[1]
source = repo / 'integrations/blender/mediaserver_livelink'
sys.path.insert(0, str(source))
import wire  # noqa: E402  Standard-library only; never imports bpy.

version = '.'.join(map(str, wire.ADDON_VERSION))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path,
                    default=repo / ('out/blender-livelink/mediaserver_livelink-' + version + '.zip'))
parser.add_argument('--report', type=Path, help='Also write the manifest beside the archive')
args = parser.parse_args()

# bl_info is read by Blender with ast.literal_eval before the module is
# imported, so it cannot reference wire.py. Fail the package instead of
# shipping an add-on whose advertised version disagrees with its wire.
declared = next(ast.literal_eval(node.value) for node in
                ast.parse((source / '__init__.py').read_text(encoding='utf-8')).body
                if isinstance(node, ast.Assign) and getattr(node.targets[0], 'id', '') == 'bl_info')
if tuple(declared['version']) != wire.ADDON_VERSION or tuple(declared['blender']) != wire.BLENDER_REQUIRED:
    raise SystemExit('bl_info disagrees with wire.py: ' + repr(declared['version']) + ' vs ' + version)

files = sorted(source.glob('*.py'))
manifest = {
    'add_on': 'mediaserver_livelink',
    'version': version,
    'link_protocol': wire.PROTOCOL_VERSION,
    'capabilities': list(wire.CAPABILITIES),
    'blender_minimum': '.'.join(map(str, wire.BLENDER_REQUIRED)),
    'blender_qualified': '5.2.0 LTS fbe6228777e7',
    'install': 'Edit > Preferences > Add-ons > Install from Disk, then enable Pixelsoup Live Link',
    'upgrade': 'Install the newer archive over the enabled add-on, then restart Blender',
    'remove': 'Disable and Remove in Preferences > Add-ons; no project or preference data is retained',
    'recover': 'File > Connect to Blender > Resync Scene; interrupted connections retry automatically',
    'files': {},
}
for path in files:
    manifest['files'][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
text = json.dumps(manifest, indent=2, sort_keys=True) + '\n'

args.output.parent.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(args.output, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
    for path in files + [None]:
        name = 'manifest.json' if path is None else path.name
        info = zipfile.ZipInfo('mediaserver_livelink/' + name, date_time=(2026, 9, 9, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(info, text.encode('utf-8') if path is None else path.read_bytes())
if args.report:
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(text, encoding='utf-8')
print(args.output)
