"""Create an isolated plate DOF review project, or send local OpenTrackIO poses.

From the repository root:
  python "advanced icvfx renderer/fixtures/step28-plate-dof.py"
  python "advanced icvfx renderer/fixtures/step28-plate-dof.py" --send --seconds 180
The sender defaults to missing physical focus/iris so camera fallback controls
can be exercised. --focus 2 --aperture 1.4 demonstrates measured overrides.
"""
from pathlib import Path
import argparse
import json
import socket
import struct
import subprocess
import time


def node(identity, kind, **props):
    return dict(id=identity, node_type=kind, properties=[dict(name=k, value_type=
        "BoolProperty" if isinstance(v, bool) else "Int64Property" if isinstance(v, int)
        else "Float64Property" if isinstance(v, float) else "StringProperty", value=dict(value=v))
        for k, v in sorted(props.items())])


def crc(data):
    value = 0xffffffff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0x82f63b78 if value & 1 else 0)
    return value ^ 0xffffffff


def project():
    nodes = [
        node(1, "show.transport", transport_state=0, direction=0, anchor_control_frame=0,
             anchor_show_frame=0, loop=True, first_frame=0, last_frame=120, rate_numerator=24, rate_denominator=1),
        node(2, "show.switcher", program_scene=100, rehearsal_scene=200, transition_from=100,
             transition_to=100, anchor_control_frame=0, duration_control_frames=0),
        node(10, "scene.settings", renderer_backend="advanced", renderer_required=True,
             shadow_cascades=0, gi_strength=0.0, display_transform="srgb", slice_apron_limit=512,
             inner_frustum_enabled=True, plate_depth_of_field=True, plate_dof_radius=32),
        node(100, "scene.stage", scene_name="Plate depth of field review", canvas_width=640, canvas_height=360,
             ambient=0.0, sky_r=0.01, sky_g=0.01, sky_b=0.01),
        node(101, "scene.camera", scene_id=100, eye_x=0.0, eye_y=0.0, eye_z=3.0,
             target_x=0.0, target_y=0.0, target_z=0.0, fov_turns=0.04, camera_tracking_source=300,
             focus_distance_m=2.0, aperture_f_number=1.4, lens_profile_enabled=True,
             lens_sensor_width_mm=36.0, lens_sensor_height_mm=20.25, lens_focal_length_mm=100.0,
             camera_inner_frustum=True, camera_inner_frustum_soft_edge_px=0),
        node(102, "scene.object", scene_id=100, primitive="quad", name="Sharp blue / wall depth",
             position_x=-0.14, scale_x=0.025, scale_y=0.5, scale_z=1.0,
             color_r=0.0, color_g=0.0, color_b=0.0, emissive_r=0.1, emissive_g=0.4, emissive_b=1.0, emissive_strength=1.0),
        node(200, "scene.stage", scene_name="Rehearsal", canvas_width=640, canvas_height=360),
        node(300, "scene.tracking_source", tracking_source_name="Local lens test", tracking_enabled=True,
             tracking_protocol="opentrackio", tracking_port=40328, tracking_camera_id=0,
             tracking_sensor_height_mm=20.25, tracking_sensor_aspect=1.77777778, tracking_fallback_focal_mm=100.0),
        node(400, "output.surface", name="Review wall / 3 metres", center_x=0.0, center_y=0.0, center_z=0.0,
             right_x=1.0, right_y=0.0, right_z=0.0, up_x=0.0, up_y=1.0, up_z=0.0,
             physical_width=2.0, physical_height=1.125, raster_width=640, raster_height=360),
        node(401, "output.map", node_index=0, source_x=0, source_y=0, source_width=640, source_height=360,
             viewport_x=0, viewport_y=0, viewport_width=640, viewport_height=360,
             projection="planar_surface", surface_id=400),
    ]
    for i in range(5):
        nodes.append(node(110+i, "scene.object", scene_id=100, primitive="quad", name=f"Gold background {i+1} / 12 metres",
            position_x=(i-2)*0.2, position_z=-9.0, scale_x=0.04, scale_y=1.8, scale_z=1.0,
            color_r=0.0, color_g=0.0, color_b=0.0, emissive_r=1.0, emissive_g=0.6, emissive_b=0.1, emissive_strength=1.0))
    path = Path("out/step28-review.json")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(dict(format_version=1, body_type="DocumentSnapshot",
        body=dict(cursor=dict(revision=len(nodes), next_command_id=1), nodes=nodes))))
    subprocess.run(["out/build/windows-msvc-debug-gui/vcpkg_installed/x64-windows/tools/flatbuffers/flatc.exe",
        "-b", "--size-prefixed", "--strict-json", "-o", "out", "engine/wire/schema/protocol.fbs", str(path)], check=True)
    payload = path.with_suffix(".mswp").read_bytes()
    header = struct.pack("<8sHHIQI", b"MSPROJ01", 1, 32, 0, len(payload), crc(payload))
    path.with_suffix(".mstage").write_bytes(header + struct.pack("<I", crc(header)) + payload)
    settings = Path("out/step28-profile/MediaServer/settings")
    settings.mkdir(parents=True, exist_ok=True)
    payload = str(path.with_suffix(".mstage").resolve()).encode()
    header = struct.pack("<8sHHII", b"MSLAST01", 1, 24, len(payload), crc(payload))
    (settings / "recent-project.bin").write_bytes(header + struct.pack("<I", crc(header)) + payload)
    print(path.with_suffix(".mstage"))


def send(args):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        for frame in range(int(args.seconds * 24)):
            sample = dict(protocol=dict(name="OpenTrackIO", version=[1, 0, 0]), sourceNumber=0,
                timing=dict(sequenceNumber=frame+1, sampleRate=dict(num=24, denom=1)),
                lens=dict(focalLength=100.0, focusDistance=args.focus, fStop=args.aperture),
                transforms=[dict(id="Camera", translation=dict(x=0.0, y=0.0, z=3.0),
                    rotation=dict(pan=0.0, tilt=0.0, roll=0.0))])
            sock.sendto(json.dumps(sample).encode(), ("127.0.0.1", 40328))
            time.sleep(1/24)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--send", action="store_true")
    parser.add_argument("--seconds", type=float, default=180)
    parser.add_argument("--focus", type=float, default=0)
    parser.add_argument("--aperture", type=float, default=0)
    args = parser.parse_args()
    send(args) if args.send else project()
