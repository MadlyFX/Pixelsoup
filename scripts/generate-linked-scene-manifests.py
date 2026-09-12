"""Generate portable step-03 UI acceptance manifests from the pinned GLB fixtures."""
import hashlib
import shutil
import uuid
from pathlib import Path

repo = Path(__file__).resolve().parents[1]
out = repo / "out" / "blender-scene-ingest"
out.mkdir(parents=True, exist_ok=True)
for revision in (1, 2):
    shutil.copyfile(repo / "tests/fixtures/blender-scene-link" / f"prop-r{revision}.glb", out / f"prop-r{revision}.glb")

def uid(name):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "mediaserver:step03:" + name))

def quoted(value):
    return '"' + str(value) + '"'

def make(name, revision, move=0):
    lines = [f'MSLINK 1 "{uid("source")}" "{name}"']
    for prototype, rev in (("hero-mesh", revision), ("unchanged-mesh", 1)):
        filename = f"prop-r{rev}.glb"
        digest = hashlib.sha256((out / filename).read_bytes()).hexdigest()
        lines.append(f'prototype "{uid(prototype)}" "{digest}" "{filename}"')
    lines.append('object ' + ' '.join(map(quoted, [uid("parent"), "Placement parent", "", "", "", "origin-v1"])) + ' 1 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1')
    for index, name in enumerate(("Hero prop", "Shared prop", "Unchanged prop")):
        prototype = "unchanged-mesh" if index == 2 else "hero-mesh"
        matrix = [1,0,0,0,0,1,0,0,0,0,1,0,(index-1)*2+move,0.5,0,1]
        fields = [uid(name),name,uid("parent"),uid(prototype),"","origin-v1"]
        lines.append('object ' + ' '.join(map(quoted, fields)) + ' 1 ' + ' '.join(map(str,matrix)))
    return '\n'.join(lines) + '\n'

for filename, revision, move in (("revision-1",1,0),("revision-2",2,0),("placement-only",2,1)):
    (out / (filename + '.mslink')).write_text(make(filename,revision,move), encoding="utf-8")
(out / "missing-resource.mslink").write_text(make("missing",2).replace('"prop-r2.glb"','"missing.glb"'), encoding="utf-8")
(out / "malformed.mslink").write_text(make("invalid",1) + 'unsupported content\n', encoding="utf-8")
print(out)
