# Animation and stage context v1

Implemented by add-on **0.6.0**, qualified against Blender **5.2.0 LTS,
fbe6228777e7** on Windows. Update the Director and add-on together.

## Published animation

Select **Animated assembly** before connecting a collection. **Connect & Publish**
and **Publish Now** explicitly snapshot the source file and launch a separate
Blender process. That process evaluates the playback range and exports one glTF
node/skin clip with one sample per source frame. The existing translator supplies
the supported materials and embedded images. The network worker transfers the
immutable GLB through the existing hashed resource path; Director ingests and
admits it as one complete revision. A source edit during export discards the
completion. Disconnect/file replacement cancels the worker. Scrubbing the Blender
timeline does not publish poses or drive Media Server playback.

The collection is one selectable assembly in Media Server. Its persistent object
and prototype UUIDs derive from the collection identity; the internal node and
bone hierarchy remains inside the model package. The assembly is the unit of
shot placement and look overrides. Use separate linked collections for independently
placed animated actors, static environments, and authored lights. Changing between
static and animated modes changes occurrence granularity; those individual static
occurrence overrides do not become internal animation-node overrides.

Supported content:

- Meshes, empties and armatures with local translation, rotation and positive scale.
- A complete parent hierarchy inside the selected collection, in metre units
  (Blender `scale_length = 1`).
- Evaluated transform/pose constraints baked to node/bone TRS samples. The source
  rig remains editable; the renderer uses the sampled cache without running constraints.
- Linear vertex-group skinning, at most four nonzero bone influences per vertex.
  A skinned mesh must be parented to its sole armature, inside the assembly.
- The existing material subset, embedded textures, and immutable package loader.
  Material surfaces remain inside the assembly; there are no per-surface linked
  children or independently editable bone nodes in this mode.

Morphs, animated mesh data, Geometry Nodes deformation, particles, cloth/rigid-body
simulation, scripted drivers, B-Bone segments, preserve-volume skinning, envelope
skinning, animated materials, scatter, external animated LODs, and unsupported
animation paths are rejected. Mesh modifiers must be applied before exporting,
apart from the supported Armature modifier. Sampled shear, singular scale and
reflections are rejected instead of approximated. No vertex cache is introduced.
Static publishing also rejects animation, constraints, armatures and time-dependent
Geometry Nodes output rather than freezing an arbitrary current pose.

## Time and interpolation

Source frame `frame_start` becomes clip time zero. Source time is sampled from
Blender's `fps / fps_base`; GLB key times are binary32 seconds. The sampled cache
uses **LINEAR** interpolation: linear translation/scale and shortest-path
quaternion interpolation. Authored Bézier curves and constraints are evaluated
at integer source frames, then interpolated between those samples. Subframe
simulation or curve parity is outside this contract.

For committed show frame `F`, rational show rate `N/D`, and start frame `S`,
elapsed time is `max(F-S, 0) * D/N`. Before the start, hold the first pose.
Non-looping clips clamp to the last pose. Looping clips use elapsed time modulo
the clip duration; the endpoint wraps to the first pose. Example: source frames
1–25 at 24 fps create a one-second clip. With show start 10 at 24 fps, frames 10,
22 and 34 sample the first pose, half-second pose, and looped first pose.
Nonmatching first/last source poses produce an intentional loop discontinuity.

The renderer's existing modular time evaluator preserves every show-frame bit,
including ordinals above 2^53. Seek/restart evaluation is stateless. Wind uses the
committed integer show frame and configured period, never packet arrival time or
Blender wall time. Start, loop, enable and wind settings participate in document
and renderer identities. Replacing topology/packages or animation bindings resets
the existing temporal history; sequential poses retain the existing palette motion
path, and seek/cut uses the renderer's discontinuity path.

## Placement, persistence and publication

World deformation is `assembly placement * sampled model deformation * bind vertex`.
Source animation is not baked into the assembly's static placement matrix. Follow
placement composes stage alignment/source placement/shot offset. **Hold placement**
holds that outer assembly matrix and continues playing the internal animation;
it does not freeze the current animated pose. A tagged Blender placement empty
receives the returned shot and parents the authored roots without changing their
animation channels. Select that empty for two-way shot gestures.

Source show-start and loop settings are defaults. Inspector changes are occurrence
overrides and survive source updates. Snapshot/copy/unlink and step 08 publication
materialize effective animation settings alongside immutable package references.
Blender and all its workers can be disconnected during offline show playback.
Animated assemblies are dynamic GI contributors, including when paused at their
bind pose; they cannot be opted into the static lighting bake.

Limits: 64 authored objects, 128 total exported hierarchy nodes, 32 skins, one clip,
2–2401 sampled source frames, one million channel keys, 64 MiB GLB, and the existing
image/geometry/package limits. Full-scene admission also counts animation tables,
palette uploads and **65,536 pose bindings per frame**, before camera culling.
The existing 256 MiB environment memory/upload bound still applies. Export source
copies and logs remain in `mslink-animation-*` temporary directories for diagnosis;
they are disposable after the worker has stopped. The explicit source snapshot
can take time on a large blend file; there is no claim of instant animation export.

## Stage reference channel

`StageContext` is wire kind **17**, with an empty request payload. Pairing, source,
epoch and scene validation are unchanged. The response is empty if unchanged,
unavailable, or rate limited. Nonempty responses contain:

```text
MSSTAGE 1
scene control_frame show_frame rate_numerator rate_denominator camera tracked held calibrated
alignment_0 ... alignment_15
camera_world_0 ... camera_world_15
sensor_width_mm sensor_height_mm focal_length_mm near_m far_m focus_m aperture_f_number
```

Matrices are column-major. Alignment maps source metres to physical-stage metres.
Camera local axes are **+X right, +Y up, -Z forward**. The reference uses the
topmost drawing tracked camera, including the renderer's calibrated entrance-pupil
offset, or the selected authored camera when no tracked camera draws. Tracking
metadata is copied from the evaluated Director frame. The sample is a fixed-size
mailbox overwritten with `try_lock` on the control thread; contention drops the
sample. Formatting and sockets run on the service worker. Delivery is capped at
10 Hz, with the current client polling at approximately 4 Hz while idle. Long
ingestion/uploads can delay it, with no catch-up queue. No camera sample schedules
a document command, source revision, undo record or persistent tracking history.

Enable **Stage references** to create managed camera and stage-origin objects in
a separate collection. Their updates bypass source extraction and undo operations.
In Source display they use inverse stage alignment; Shot display already includes
alignment, so references use stage coordinates directly. After a delayed sample
the UI marks the reference held. On disconnect the last managed objects remain
available, and reconnect starts a fresh sample stream.

The Blender camera uses **VERTICAL sensor fit**, explicit sensor dimensions,
focal length and clipping. An unauthored physical lens uses a 36 mm synthetic
sensor width and a focal length derived from the authored vertical FOV and aspect.
**View Director Camera** selects a local viewport camera and explicitly adjusts
the Blender render aspect to the sensor gate (height 1000 pixels); passive samples
do not change the scene raster or playhead. Show time is informational in the panel.

Blender's reference gate does not reproduce Brown–Conrady distortion, overscan,
off-axis LED-wall slices, feed mapping, projection meshes, inner/outer-frustum
compositing, plate DOF, facility color, temporal processing, or final calibrated
output. It is a pose/lens reference for authoring. Media Server remains authoritative
for every final ICVFX view. Physical tracker hardware accuracy and two-machine
scanout timing remain qualification work outside this local test.
