"""Qualify the add-on in factory Blender against the real Director service."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

repo = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--blender', type=Path, default=Path('C:/Program Files/Blender Foundation/Blender 5.2/blender.exe'))
parser.add_argument('--build', choices=('debug', 'release'), default='debug')
parser.add_argument('--output', type=Path)
parser.add_argument('--shot', action='store_true', help='Run the bidirectional shot placement acceptance suite')
parser.add_argument('--materials', action='store_true', help='Qualify textured materials, native lights and explicit base color baking')
parser.add_argument('--animation', action='store_true', help='Qualify node/skin assemblies and managed stage references')
parser.add_argument('--environment', action='store_true', help='Qualify procedural scatter, terrain and export density')
parser.add_argument('--qualification', action='store_true',
                    help='Step 12 delivery: install/upgrade/removal, capability negotiation, scenario matrix and measured latency')
args = parser.parse_args()
directory = (args.output or repo / 'out/blender-livelink' / (args.build + '-' + uuid.uuid4().hex[:8])).resolve()
directory.mkdir(parents=True, exist_ok=False)
archive = directory / 'mediaserver_livelink.zip'
subprocess.run([sys.executable,
    str(repo / 'scripts/package-blender-livelink.py'), '--output', str(archive)], check=True,
    stdout=subprocess.DEVNULL)
environment = dict(os.environ, BLENDER_USER_SCRIPTS=str(directory / 'scripts'),
                   BLENDER_USER_CONFIG=str(directory / 'config'))
(directory / 'scripts').mkdir()
(directory / 'config').mkdir()
host = repo / f'out/build/windows-msvc-{args.build}-gui/engine/render/mediaserver_blender_publisher_host.exe'
flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
with (directory / 'host.log').open('w') as host_log:
    process = subprocess.Popen([str(host), str(directory)], stdout=host_log, stderr=subprocess.STDOUT, creationflags=flags)
    try:
        deadline = time.monotonic() + 15
        while not (directory / 'pairing.json').exists():
            if process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Director fixture did not start; see ' + str(directory / 'host.log'))
            time.sleep(0.05)
        with (directory / 'blender.log').open('w') as log:
            result = subprocess.run([str(args.blender), '--background', '--factory-startup', '--python-exit-code', '1',
                '--python', str(repo / ('tests/blender/exercise_qualification.py' if args.qualification else 'tests/blender/exercise_animation.py' if args.animation else 'tests/blender/exercise_materials.py' if args.materials else 'tests/blender/exercise_environment.py' if args.environment else 'tests/blender/exercise_shot.py' if args.shot else 'tests/blender/exercise_publisher.py')), '--', str(directory)],
                stdout=log, stderr=subprocess.STDOUT, timeout=420 if args.qualification else 150, creationflags=flags, env=environment)
        if result.returncode or not (directory / 'results.json').exists():
            raise RuntimeError((directory / 'blender.log').read_text(errors='replace'))
        print((directory / 'results.json').read_text())
    finally:
        (directory / 'stop').touch()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        # Pairing is revoked by fixture shutdown; keep only non-secret evidence.
        if (directory / 'pairing.json').exists():
            (directory / 'pairing.json').unlink()
print('Evidence:', directory)
