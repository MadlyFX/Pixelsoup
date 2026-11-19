"""Reference fixture, run with Blender --background --factory-startup --python.

This is an offline qualification tool, not a publisher or scene-link add-on.
The checked-in GLBs let importer tests run without Blender on the test host.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import platform
import sys
import time
import uuid

import bpy
from mathutils import Matrix, Vector

PIN = (5, 2, 0)
NAMESPACE = uuid.UUID("e949ab57-135d-45f7-b987-86682a103a9a")
CONVERSION = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, 0, 0, 1)))


def uid(label):
    return str(uuid.uuid5(NAMESPACE, label))


def stamp(block, kind, label):
    block["ms_link_id"] = uid(kind + "/" + label)
    return block


def matrix_values(matrix):
    return [round(matrix[row][col], 7) for col in range(4) for row in range(4)]


def material(name, color, metallic, roughness):
    result = stamp(bpy.data.materials.new(name), "material", name)
    result.use_nodes = True
    node = result.node_tree.nodes.get("Principled BSDF")
    node.inputs["Base Color"].default_value = (*color, 1)
    node.inputs["Metallic"].default_value = metallic
    node.inputs["Roughness"].default_value = roughness
    result.diffuse_color = (*color, 1)
    return result


def build():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = stamp(bpy.context.scene, "scene", "reference")
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1
    collection = stamp(bpy.data.collections.new("Scene Link Reference"), "collection", "reference")
    scene.collection.children.link(collection)
    copper = material("Copper", (0.8, 0.2, 0.1), 0.7, 0.35)
    cyan = material("Cyan", (0.05, 0.75, 0.9), 0.1, 0.8)
    floor = material("Floor", (0.18, 0.18, 0.18), 0, 0.9)
    bpy.ops.mesh.primitive_cube_add(size=1)
    first = bpy.context.object
    for held in list(first.users_collection):
        held.objects.unlink(first)
    collection.objects.link(first)
    first.name = "Prop before rename"
    stamp(first, "object", "prop-a")
    stamp(first.data, "prototype", "prop")
    first.data.materials.append(copper)
    first.data.materials.append(cyan)
    for polygon in first.data.polygons:
        polygon.material_index = 1 if polygon.normal.z > 0.5 else 0
    parent = stamp(bpy.data.objects.new("Placement parent", None), "object", "parent")
    collection.objects.link(parent)
    parent.location = (1, 0, 0)
    parent.rotation_euler.z = math.pi / 2
    first.parent = parent
    first.location = (1, 0, 1)
    first.scale = (2, 1, 0.5)
    first.name = "Hero prop"  # The object identity is intentionally unchanged.
    second = stamp(bpy.data.objects.new("Shared prop", first.data), "object", "prop-b")
    collection.objects.link(second)
    second.location = (-2, 0, 0.5)
    mirrored = stamp(bpy.data.objects.new("Mirrored prop", first.data), "object", "mirror")
    collection.objects.link(mirrored)
    mirrored.location = (3, 0, 0.5)
    mirrored.scale = (-1, 1, 1)
    hidden = stamp(bpy.data.objects.new("Hidden prop", first.data), "object", "hidden")
    collection.objects.link(hidden)
    hidden.location = (0, 4, 0.5)
    hidden.hide_render = True
    hidden.hide_set(True)
    mesh = stamp(bpy.data.meshes.new("Floor mesh"), "prototype", "floor")
    mesh.from_pydata([(-5, -3, 0), (5, -3, 0), (5, 4, 0), (-5, 4, 0)], [], [(0, 1, 2, 3)])
    ground = stamp(bpy.data.objects.new("Floor", mesh), "object", "floor")
    collection.objects.link(ground)
    mesh.materials.append(floor)
    camera = stamp(bpy.data.objects.new("Composition camera", bpy.data.cameras.new("Composition camera")), "object", "camera")
    collection.objects.link(camera)
    camera.location = (7, -10, 6)
    camera.rotation_euler = (Vector((0, 0, 0.75)) - camera.location).to_track_quat("-Z", "Y").to_euler()
    camera.data.lens = 45
    camera.data.sensor_width = 36
    camera.data.sensor_fit = "HORIZONTAL"
    camera.data.clip_start = 0.05
    camera.data.clip_end = 200
    scene.camera = camera
    scene.render.resolution_x, scene.render.resolution_y = 1280, 720
    scene.render.resolution_percentage = 100
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 16
    scene.world = bpy.data.worlds.new("Reference world")
    scene.world.use_nodes = True
    scene.world.node_tree.nodes.get("Background").inputs["Strength"].default_value = 0.4
    light = bpy.data.objects.new("Reference key (comparison only)", bpy.data.lights.new("Reference key", "AREA"))
    scene.collection.objects.link(light)
    light.location = (2, -4, 7)
    light.rotation_euler = (Vector((0, 0, 0)) - light.location).to_track_quat("-Z", "Y").to_euler()
    light.data.energy, light.data.shape, light.data.size = 1500, "DISK", 5
    bpy.context.view_layer.update()
    # Independent, hand-derived world-transform golden values.
    expected = Matrix(((0, -1, 0, 1), (2, 0, 0, 1), (0, 0, 0.5, 1), (0, 0, 0, 1)))
    assert max(abs(first.matrix_world[r][c] - expected[r][c]) for r in range(4) for c in range(4)) < 1e-6
    assert first.data is second.data is mirrored.data
    return collection, first


def export(path, objects):
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    started = time.perf_counter()
    result = bpy.ops.export_scene.gltf(filepath=str(path), export_format="GLB", use_selection=True,
        export_yup=True, export_apply=False, export_animations=False, export_extras=True,
        export_cameras=True, export_lights=False, export_materials="EXPORT")
    if result != {"FINISHED"}:
        raise RuntimeError("glTF export failed: " + str(result))
    return (time.perf_counter() - started) * 1000


def oracle(path, objects):
    # Triangle-corner positions/normals/material factors from Blender's evaluated
    # geometry, independently of glTF exporter output and Pixelsoup's loader.
    rows = []
    for obj in objects:
        if obj.type != "MESH":
            continue
        evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        mesh.calc_loop_triangles()
        world = CONVERSION @ obj.matrix_world
        normal = world.to_3x3().inverted().transposed()
        for tri in mesh.loop_triangles:
            shader = mesh.materials[tri.material_index].node_tree.nodes.get("Principled BSDF")
            color = shader.inputs["Base Color"].default_value[:3]
            for corner in tri.loops:
                loop = mesh.loops[corner]
                p = world @ mesh.vertices[loop.vertex_index].co
                n = (normal @ mesh.corner_normals[corner].vector).normalized()
                rows.append((*p, *n, *color, shader.inputs["Metallic"].default_value,
                             shader.inputs["Roughness"].default_value))
        evaluated.to_mesh_clear()
    path.write_text(str(len(rows)) + "\n" + "\n".join(" ".join(f"{v:.7f}" for v in row) for row in rows) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=20)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    if bpy.app.version != PIN:
        raise RuntimeError(f"Unqualified Blender {bpy.app.version_string}; required {PIN}")
    if not 1 <= args.samples <= 1000:
        raise ValueError("samples must be in 1..1000")
    args.output.mkdir(parents=True, exist_ok=True)
    collection, prop = build()
    objects = [o for o in collection.objects if not o.hide_render]
    manifest = {"contract_version": 1, "blender": bpy.app.version_string,
        "blender_build_hash": bpy.app.build_hash.decode(), "os": platform.platform(),
        "source_scene_id": bpy.context.scene["ms_link_id"], "linked_collection_id": collection["ms_link_id"],
        "link_instance_id": uid("link/reference"),
        "matrix_storage": "column-major", "units": "meters", "objects": [], "revisions": [],
        "materials": [{"material_id": m["ms_link_id"], "name": m.name,
            "base_color": list(m.node_tree.nodes.get("Principled BSDF").inputs["Base Color"].default_value),
            "metallic": m.node_tree.nodes.get("Principled BSDF").inputs["Metallic"].default_value,
            "roughness": m.node_tree.nodes.get("Principled BSDF").inputs["Roughness"].default_value}
            for m in bpy.data.materials if "ms_link_id" in m]}
    for obj in collection.objects:
        manifest["objects"].append({"object_id": obj["ms_link_id"], "name": obj.name,
            "occurrence_id": uid("occurrence/reference/" + obj["ms_link_id"]),
            "parent_id": obj.parent["ms_link_id"] if obj.parent else None,
            "prototype_id": obj.data["ms_link_id"] if obj.type == "MESH" else None,
            "material_ids": [m["ms_link_id"] for m in obj.data.materials] if obj.type == "MESH" else [],
            "visible": not obj.hide_render, "world": matrix_values(CONVERSION @ obj.matrix_world @ CONVERSION.inverted()),
            "local": matrix_values(CONVERSION @ obj.matrix_local @ CONVERSION.inverted())})
    manifest["camera"] = {"eye": [7, 6, 10], "target": [0, 0.75, 0], "up": [0, 1, 0],
        "fov_y_turns": 2 * math.atan((36 / (1280 / 720)) / (2 * 45)) / (2 * math.pi),
        "near": 0.05, "far": 200, "raster": [1280, 720], "shift": [0, 0]}
    timings = []
    for revision in (1, 2):
        if revision == 2:
            # Edit shared geometry in local space; every occurrence must change.
            for vertex in prop.data.vertices:
                if vertex.co.z > 0 and vertex.co.x > 0:
                    vertex.co.z += 0.25
            prop.data.update()
            bpy.context.view_layer.update()
        path = args.output / f"reference-r{revision}.glb"
        elapsed = export(path, objects)
        oracle(path.with_suffix(".expected"), objects)
        manifest["revisions"].append({"revision": revision, "file": path.name,
            "publication_id": uid(f"publication/{revision}"),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "bytes": path.stat().st_size,
            "export_ms": elapsed})
        bpy.ops.wm.save_as_mainfile(filepath=str(args.output / f"reference-r{revision}.blend"))
        # A local-space prototype, avoiding baked occurrence placement.
        prototype = bpy.data.objects.new("Prototype export", prop.data)
        bpy.context.scene.collection.objects.link(prototype)
        export(args.output / f"prop-r{revision}.glb", [prototype])
        bpy.data.objects.remove(prototype, do_unlink=True)
    for _ in range(args.samples):
        timings.append(export(args.output / "manual-export.glb", objects))
    ordered = sorted(timings)
    manifest["manual_export"] = {"samples_ms": timings, "samples": len(timings),
        "p50_ms": ordered[math.ceil(len(ordered) * 0.5) - 1],
        "p95_ms": ordered[math.ceil(len(ordered) * 0.95) - 1],
        "scope": "Blender operator evaluation/serialization/file write; excludes human clicks and process startup"}
    # Reopen verifies actual .blend persistence, including rename and shared data.
    bpy.ops.wm.open_mainfile(filepath=str(args.output / "reference-r2.blend"))
    assert bpy.data.objects["Hero prop"]["ms_link_id"] == uid("object/prop-a")
    assert bpy.data.objects["Hero prop"].data is bpy.data.objects["Shared prop"].data
    # A second, explicitly separate throughput dataset: 256 placements of the
    # revised mesh. The current importer flattens these into 3,072 triangles.
    workload = []
    for index in range(256):
        obj = bpy.data.objects.new(f"Grid {index:03d}", bpy.data.objects["Hero prop"].data)
        bpy.context.scene.collection.objects.link(obj)
        obj.location = ((index % 16) * 2, (index // 16) * 2, 0)
        workload.append(obj)
    workload_path = args.output / "workload-256.glb"
    workload_times = [export(workload_path, workload) for _ in range(args.samples)]
    sorted_times = sorted(workload_times)
    manifest["workload_256"] = {"file": workload_path.name, "occurrences": 256,
        "prototypes": 1, "triangles_after_flattening": 3072,
        "sha256": hashlib.sha256(workload_path.read_bytes()).hexdigest(),
        "bytes": workload_path.stat().st_size, "samples_ms": workload_times,
        "p50_ms": sorted_times[math.ceil(len(sorted_times) * 0.5) - 1],
        "p95_ms": sorted_times[math.ceil(len(sorted_times) * 0.95) - 1]}
    (args.output / "reference.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("REFERENCE_OK " + str(args.output))


if __name__ == "__main__":
    main()
