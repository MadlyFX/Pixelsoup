#pragma once

#include <string_view>

// The document's scene vocabulary, in one place.
//
// A 3D scene in this system is not a file the renderer opens. It is document
// nodes - the same immutable, versioned, command-edited nodes as everything
// else - which means a scene edit is an ordinary command, replicates through
// the ordinary bus, undoes through the ordinary stack, and is journalled by
// the ordinary WAL. Nothing about 3D needed a second mechanism, and building
// one would have been the mistake.
//
// The names are strings because the document's property map is
// string-to-scalar. That is a real cost: every property below is a lookup and
// a type check rather than a field access, and the parse happens on every
// document change. It is paid once per edit rather than once per frame, and
// the alternative - a typed side table the document does not know about -
// would need its own replication and its own recovery story.
//
// Vectors are three scalar properties rather than one packed string. Verbose,
// but it keeps every value independently editable, independently undoable, and
// visible to a future property inspector without a parser.
namespace mediaserver::scene::schema {

// ---------------------------------------------------------------------------
// Node types

// The canvas and its surround. The lowest-numbered stage node wins if a
// document somehow carries two; see `SceneCounters::duplicate_singletons`.
inline constexpr std::string_view stage_type = "scene.stage";
// The camera the whole cohort shares. Authored against the canvas, never
// against a slice.
inline constexpr std::string_view camera_type = "scene.camera";
// One drawable primitive.
inline constexpr std::string_view object_type = "scene.object";
inline constexpr std::string_view light_type = "scene.light";
// A clip on a screen-facing quad: an image sequence textured onto geometry,
// placed and transformed in the scene like any other element.
inline constexpr std::string_view media_type = "scene.media";
// The binding from a node index to its rectangle of the canvas.
inline constexpr std::string_view slice_type = "scene.slice";
// A keyframe channel: it drives one transform component of a target node over
// show frames. This is the general form of the per-second rates below - a value
// from a parameter and the show frame, so a playing show still authors nothing -
// and it is what the operator's timeline edits. Lowest-id channel wins if two
// drive the same (target, component).
inline constexpr std::string_view channel_type = "scene.channel";

// ---------------------------------------------------------------------------
// scene.stage

inline constexpr std::string_view canvas_width = "canvas_width";
inline constexpr std::string_view canvas_height = "canvas_height";
inline constexpr std::string_view sky_r = "sky_r";
inline constexpr std::string_view sky_g = "sky_g";
inline constexpr std::string_view sky_b = "sky_b";
inline constexpr std::string_view ground_r = "ground_r";
inline constexpr std::string_view ground_g = "ground_g";
inline constexpr std::string_view ground_b = "ground_b";
inline constexpr std::string_view ambient = "ambient";

// ---------------------------------------------------------------------------
// scene.camera

inline constexpr std::string_view eye_x = "eye_x";
inline constexpr std::string_view eye_y = "eye_y";
inline constexpr std::string_view eye_z = "eye_z";
inline constexpr std::string_view target_x = "target_x";
inline constexpr std::string_view target_y = "target_y";
inline constexpr std::string_view target_z = "target_z";
inline constexpr std::string_view up_x = "up_x";
inline constexpr std::string_view up_y = "up_y";
inline constexpr std::string_view up_z = "up_z";
// Vertical field of view, as a fraction of a full turn. 0.125 is 45 degrees.
inline constexpr std::string_view fov_turns = "fov_turns";
inline constexpr std::string_view near_plane = "near";
inline constexpr std::string_view far_plane = "far";
// Turns per second the eye sweeps around the target, about the up axis.
//
// This is the shape every animated property here takes, and the reason it
// exists: a show that is playing must make no document edits at all (§0.1 of
// the implementation plan), so motion cannot come from the Director writing a
// new transform each frame. It comes from a parameter plus the show frame,
// which every node already holds exactly. The timeline and keyframe layer that
// eventually replaces these will work the same way and for the same reason.
inline constexpr std::string_view orbit_turns_per_second =
    "orbit_turns_per_second";

// ---------------------------------------------------------------------------
// scene.object

// One of the names in `primitive_name`.
inline constexpr std::string_view primitive = "primitive";
inline constexpr std::string_view segments = "segments";
inline constexpr std::string_view position_x = "position_x";
inline constexpr std::string_view position_y = "position_y";
inline constexpr std::string_view position_z = "position_z";
inline constexpr std::string_view rotation_yaw = "rotation_yaw";
inline constexpr std::string_view rotation_pitch = "rotation_pitch";
inline constexpr std::string_view rotation_roll = "rotation_roll";
inline constexpr std::string_view scale_x = "scale_x";
inline constexpr std::string_view scale_y = "scale_y";
inline constexpr std::string_view scale_z = "scale_z";
inline constexpr std::string_view color_r = "color_r";
inline constexpr std::string_view color_g = "color_g";
inline constexpr std::string_view color_b = "color_b";
inline constexpr std::string_view roughness = "roughness";
inline constexpr std::string_view metallic = "metallic";
inline constexpr std::string_view emissive_r = "emissive_r";
inline constexpr std::string_view emissive_g = "emissive_g";
inline constexpr std::string_view emissive_b = "emissive_b";
// Added to the authored yaw, at the show frame.
inline constexpr std::string_view spin_turns_per_second =
    "spin_turns_per_second";
// A sinusoidal offset on Y, in world units, at the show frame.
inline constexpr std::string_view bob_amplitude = "bob_amplitude";
inline constexpr std::string_view bob_turns_per_second = "bob_turns_per_second";

// ---------------------------------------------------------------------------
// scene.media
//
// Placement reuses the object transform names (position_*, rotation_*, scale_*)
// so an operator moves a clip the same way they move any element.

// A printf-style path template for the image sequence, one unsigned frame
// number substituted, e.g. "plate_%04u.png".
inline constexpr std::string_view source = "source";
// The file number of the first frame, and how many frames the sequence holds.
// The show frame maps to a file number by counting from the start and looping
// once it reaches the end.
inline constexpr std::string_view sequence_start = "sequence_start";
inline constexpr std::string_view sequence_count = "sequence_count";

// ---------------------------------------------------------------------------
// scene.channel
//
// The target is a node id (an integer). The component is one of the transform
// property names above (position_x .. scale_z); the channel replaces that
// component of the target at the show frame. Keys are indexed properties -
// key0_frame / key0_value / key0_interp, key1_frame, ... - parsed in order until
// the first index with no _frame. A key's interp names how it reaches the NEXT
// key: hold, linear, or cubic (a smoothstep ease). Value beyond the first and
// last key is clamped.
inline constexpr std::string_view channel_target = "channel_target";
inline constexpr std::string_view channel_component = "channel_component";
inline constexpr std::string_view channel_key_prefix = "key";
inline constexpr std::string_view channel_key_frame_suffix = "_frame";
inline constexpr std::string_view channel_key_value_suffix = "_value";
inline constexpr std::string_view channel_key_interp_suffix = "_interp";
inline constexpr std::string_view interp_hold = "hold";
inline constexpr std::string_view interp_linear = "linear";
inline constexpr std::string_view interp_cubic = "cubic";

// ---------------------------------------------------------------------------
// scene.light

// "directional" or "point".
inline constexpr std::string_view light_kind = "kind";
inline constexpr std::string_view directional_kind = "directional";
inline constexpr std::string_view point_kind = "point";
// The direction a directional light travels, not the direction towards it.
inline constexpr std::string_view direction_x = "direction_x";
inline constexpr std::string_view direction_y = "direction_y";
inline constexpr std::string_view direction_z = "direction_z";
inline constexpr std::string_view intensity = "intensity";
inline constexpr std::string_view range = "range";

// ---------------------------------------------------------------------------
// scene.slice

inline constexpr std::string_view node_index = "node_index";
inline constexpr std::string_view slice_x = "x";
inline constexpr std::string_view slice_y = "y";
inline constexpr std::string_view slice_width = "width";
inline constexpr std::string_view slice_height = "height";

} // namespace mediaserver::scene::schema
