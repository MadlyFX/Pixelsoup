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

// A named scene's canvas and surround. Each stage node is a first-class scene;
// its document node id is the SceneId used by scoped content.
inline constexpr std::string_view stage_type = "scene.stage";
// A scene camera, authored against the canvas and never against a slice. The
// lowest-id camera in one scene wins if that scene carries duplicates.
inline constexpr std::string_view camera_type = "scene.camera";
// One drawable primitive.
inline constexpr std::string_view object_type = "scene.object";
// A transform/visibility/lock container. Groups never draw; their local
// transform is composed into descendant drawables and nested groups.
inline constexpr std::string_view group_type = "scene.group";
inline constexpr std::string_view light_type = "scene.light";
// A clip on a screen-facing quad: an image sequence textured onto geometry,
// placed and transformed in the scene like any other element.
inline constexpr std::string_view media_type = "scene.media";
// One authored timeline lane owned by a scene. Lanes are editorial ordering
// and show-state containers; operator-local Solo is deliberately absent.
inline constexpr std::string_view timeline_lane_type = "scene.timeline_lane";
// An imported 3D model (glTF or OBJ), placed and transformed like an object.
// Its geometry and material are loaded from `source` by the renderer, so the
// document carries only the reference and the transform. Distribution of the
// file to nodes is external (a node without it draws nothing).
inline constexpr std::string_view model_type = "scene.model";
// The binding from a node index to its rectangle of the canvas.
inline constexpr std::string_view slice_type = "scene.slice";
// A keyframe channel: it drives one transform component of a target node over
// show frames. This is the general form of the per-second rates below - a value
// from a parameter and the show frame, so a playing show still authors nothing -
// and it is what the operator's timeline edits. Lowest-id channel wins if two
// drive the same (target, component).
inline constexpr std::string_view channel_type = "scene.channel";
// An ordered, first-class effect targeting a drawable. Effects are nodes so
// they can be reordered, duplicated, deleted, shared by graph tooling, and
// independently keyframed.
inline constexpr std::string_view effect_type = "scene.effect";
// A first-class coverage operation targeting one drawable or group. Bezier
// geometry is never hidden in a scalar/array property: each control point is
// its own mask_point node below.
inline constexpr std::string_view mask_type = "scene.mask";
inline constexpr std::string_view mask_point_type = "scene.mask_point";
// Project-wide effect quality and response. Exactly one node is meaningful; a
// document carrying several resolves to the lowest id on every machine. Its
// absence is the built-in default, so a project that never opens the dialog
// renders exactly as it did before the dialog existed.
inline constexpr std::string_view settings_type = "scene.settings";

// ---------------------------------------------------------------------------
// scene.stage

inline constexpr std::string_view scene_name = "scene_name";
inline constexpr std::string_view canvas_width = "canvas_width";
inline constexpr std::string_view canvas_height = "canvas_height";
inline constexpr std::string_view sky_r = "sky_r";
inline constexpr std::string_view sky_g = "sky_g";
inline constexpr std::string_view sky_b = "sky_b";
inline constexpr std::string_view ground_r = "ground_r";
inline constexpr std::string_view ground_g = "ground_g";
inline constexpr std::string_view ground_b = "ground_b";
inline constexpr std::string_view ambient = "ambient";

// Shared ownership field for cameras, drawables, and lights. Existing node
// kinds may use the lowest-id stage when this is absent. New scene.mask and
// scene.mask_point nodes require this property explicitly and never adopt an
// implicit/default owner.
inline constexpr std::string_view scene_id = "scene_id";
// Editor interaction state shared by every selectable scene element. Locked
// elements still render normally, but editor pickers and hierarchy/timeline
// selection skip them until the lock control is opened again.
inline constexpr std::string_view locked = "locked";
// Optional hierarchy edge shared by drawables and groups. A missing property
// means the node is top-level. group_order is the stable sibling order used by
// hierarchy/editor projections; node id is the final tie-break.
inline constexpr std::string_view parent_group = "parent_group";
inline constexpr std::string_view group_order = "group_order";

// ---------------------------------------------------------------------------
// scene.group

inline constexpr std::string_view group_name = "group_name";
inline constexpr std::string_view visible = "visible";

// ---------------------------------------------------------------------------
// scene.timeline_lane

inline constexpr std::string_view lane_name = "lane_name";
inline constexpr std::string_view lane_order = "lane_order";
inline constexpr std::string_view lane_muted = "lane_muted";
inline constexpr std::string_view lane_locked = "lane_locked";
inline constexpr std::string_view lane_compositing_mode =
    "lane_compositing_mode";
inline constexpr std::string_view lane_normal_mode = "normal";
inline constexpr std::string_view lane_add_mode = "add";
inline constexpr std::string_view lane_multiply_mode = "multiply";
inline constexpr std::string_view lane_screen_mode = "screen";

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
// Shared layer/compositing properties for objects, media, and imported models.
// Blend mode is stored as 0=normal, 1=add, 2=multiply, 3=screen.
inline constexpr std::string_view opacity = "opacity";
inline constexpr std::string_view blend_mode = "blend_mode";
// Effect presence is explicit so a neutral effect remains visible/editable.
inline constexpr std::string_view brightness_enabled = "brightness_enabled";
inline constexpr std::string_view brightness = "brightness";
inline constexpr std::string_view saturation = "saturation";
inline constexpr std::string_view blur_enabled = "blur_enabled";
inline constexpr std::string_view blur_radius = "blur_radius";
inline constexpr std::string_view film_grain_amount = "film_grain_amount";

// ---------------------------------------------------------------------------
// scene.mask / scene.mask_point

inline constexpr std::string_view mask_target = "mask_target";
inline constexpr std::string_view mask_name = "mask_name";
inline constexpr std::string_view mask_kind = "mask_kind";
inline constexpr std::string_view mask_rectangle_kind = "rectangle";
inline constexpr std::string_view mask_ellipse_kind = "ellipse";
inline constexpr std::string_view mask_bezier_kind = "bezier";
inline constexpr std::string_view mask_enabled = "mask_enabled";
// Canvas-pixel units remain explicit in the property name. For drawable masks
// the path itself is normalized source-layer space; for group masks it is
// normalized authored-canvas space.
inline constexpr std::string_view mask_feather_px = "mask_feather_px";
inline constexpr std::string_view mask_expansion_px = "mask_expansion_px";
inline constexpr std::string_view mask_opacity = "mask_opacity";
inline constexpr std::string_view mask_invert = "mask_invert";
inline constexpr std::string_view mask_order = "mask_order";
inline constexpr std::string_view mask_combine = "mask_combine";
inline constexpr std::string_view mask_add = "add";
inline constexpr std::string_view mask_subtract = "subtract";
inline constexpr std::string_view mask_intersect = "intersect";
inline constexpr std::string_view mask_difference = "difference";
// Axis-aligned Rectangle/Ellipse geometry in normalized target space.
inline constexpr std::string_view mask_left = "mask_left";
inline constexpr std::string_view mask_top = "mask_top";
inline constexpr std::string_view mask_right = "mask_right";
inline constexpr std::string_view mask_bottom = "mask_bottom";

// First-class child control points for a closed Bezier path. Handles are
// absolute positions in the same normalized target space as the anchor.
inline constexpr std::string_view mask_id = "mask_id";
inline constexpr std::string_view mask_point_order = "mask_point_order";
inline constexpr std::string_view mask_point_x = "mask_point_x";
inline constexpr std::string_view mask_point_y = "mask_point_y";
inline constexpr std::string_view mask_point_in_x = "mask_point_in_x";
inline constexpr std::string_view mask_point_in_y = "mask_point_in_y";
inline constexpr std::string_view mask_point_out_x = "mask_point_out_x";
inline constexpr std::string_view mask_point_out_y = "mask_point_out_y";

// Timeline placement ownership/order. A missing lane_id selects the implicit
// compatibility lane without rewriting the document. clip_order resolves
// overlaps whose starts are equal, with node id as the final stable tie-break.
inline constexpr std::string_view lane_id = "lane_id";
inline constexpr std::string_view clip_order = "clip_order";

// ---------------------------------------------------------------------------
// scene.effect

inline constexpr std::string_view effect_target = "effect_target";
inline constexpr std::string_view effect_kind = "effect_kind";
inline constexpr std::string_view brightness_kind = "brightness";
inline constexpr std::string_view saturation_kind = "saturation";
inline constexpr std::string_view blur_kind = "blur";
inline constexpr std::string_view box_blur_kind = "box_blur";
inline constexpr std::string_view directional_blur_kind =
    "directional_blur";
inline constexpr std::string_view chroma_key_kind = "chroma_key";
inline constexpr std::string_view luma_key_kind = "luma_key";
inline constexpr std::string_view levels_kind = "levels";
inline constexpr std::string_view lift_gamma_gain_kind = "lift_gamma_gain";
inline constexpr std::string_view contrast_pivot_kind = "contrast_pivot";
inline constexpr std::string_view color_matrix_kind = "color_matrix";
inline constexpr std::string_view white_balance_kind = "white_balance";
inline constexpr std::string_view curves_kind = "curves";
inline constexpr std::string_view film_grain_kind = "film_grain";
inline constexpr std::string_view effect_order = "effect_order";
inline constexpr std::string_view effect_enabled = "effect_enabled";
// Spatial filters use distinct properties so a document never relies on a
// node-kind-dependent interpretation of one generic scalar.
inline constexpr std::string_view box_blur_radius = "box_blur_radius";
inline constexpr std::string_view box_blur_passes = "box_blur_passes";
inline constexpr std::string_view directional_blur_length =
    "directional_blur_length";
inline constexpr std::string_view directional_blur_angle_turns =
    "directional_blur_angle_turns";
inline constexpr std::string_view chroma_key_color_r = "chroma_key_color_r";
inline constexpr std::string_view chroma_key_color_g = "chroma_key_color_g";
inline constexpr std::string_view chroma_key_color_b = "chroma_key_color_b";
inline constexpr std::string_view chroma_key_tolerance =
    "chroma_key_tolerance";
inline constexpr std::string_view chroma_key_softness =
    "chroma_key_softness";
inline constexpr std::string_view chroma_key_morphology_px =
    "chroma_key_morphology_px";
inline constexpr std::string_view chroma_key_spill_suppression =
    "chroma_key_spill_suppression";
inline constexpr std::string_view chroma_key_mix = "chroma_key_mix";
inline constexpr std::string_view luma_key_low_threshold =
    "luma_key_low_threshold";
inline constexpr std::string_view luma_key_high_threshold =
    "luma_key_high_threshold";
inline constexpr std::string_view luma_key_low_softness =
    "luma_key_low_softness";
inline constexpr std::string_view luma_key_high_softness =
    "luma_key_high_softness";
inline constexpr std::string_view luma_key_invert = "luma_key_invert";
inline constexpr std::string_view luma_key_mix = "luma_key_mix";

// Levels stores four complete transfer curves. Property order here mirrors
// evaluation order: master, red, green, blue.
inline constexpr std::string_view levels_master_input_black =
    "levels_master_input_black";
inline constexpr std::string_view levels_master_input_white =
    "levels_master_input_white";
inline constexpr std::string_view levels_master_gamma = "levels_master_gamma";
inline constexpr std::string_view levels_master_output_black =
    "levels_master_output_black";
inline constexpr std::string_view levels_master_output_white =
    "levels_master_output_white";
inline constexpr std::string_view levels_red_input_black =
    "levels_red_input_black";
inline constexpr std::string_view levels_red_input_white =
    "levels_red_input_white";
inline constexpr std::string_view levels_red_gamma = "levels_red_gamma";
inline constexpr std::string_view levels_red_output_black =
    "levels_red_output_black";
inline constexpr std::string_view levels_red_output_white =
    "levels_red_output_white";
inline constexpr std::string_view levels_green_input_black =
    "levels_green_input_black";
inline constexpr std::string_view levels_green_input_white =
    "levels_green_input_white";
inline constexpr std::string_view levels_green_gamma = "levels_green_gamma";
inline constexpr std::string_view levels_green_output_black =
    "levels_green_output_black";
inline constexpr std::string_view levels_green_output_white =
    "levels_green_output_white";
inline constexpr std::string_view levels_blue_input_black =
    "levels_blue_input_black";
inline constexpr std::string_view levels_blue_input_white =
    "levels_blue_input_white";
inline constexpr std::string_view levels_blue_gamma = "levels_blue_gamma";
inline constexpr std::string_view levels_blue_output_black =
    "levels_blue_output_black";
inline constexpr std::string_view levels_blue_output_white =
    "levels_blue_output_white";

inline constexpr std::string_view lift_r = "lift_r";
inline constexpr std::string_view lift_g = "lift_g";
inline constexpr std::string_view lift_b = "lift_b";
inline constexpr std::string_view gamma_r = "gamma_r";
inline constexpr std::string_view gamma_g = "gamma_g";
inline constexpr std::string_view gamma_b = "gamma_b";
inline constexpr std::string_view gain_r = "gain_r";
inline constexpr std::string_view gain_g = "gain_g";
inline constexpr std::string_view gain_b = "gain_b";
inline constexpr std::string_view contrast = "contrast";
inline constexpr std::string_view pivot = "pivot";
inline constexpr std::string_view m00 = "m00";
inline constexpr std::string_view m01 = "m01";
inline constexpr std::string_view m02 = "m02";
inline constexpr std::string_view m10 = "m10";
inline constexpr std::string_view m11 = "m11";
inline constexpr std::string_view m12 = "m12";
inline constexpr std::string_view m20 = "m20";
inline constexpr std::string_view m21 = "m21";
inline constexpr std::string_view m22 = "m22";
inline constexpr std::string_view offset_r = "offset_r";
inline constexpr std::string_view offset_g = "offset_g";
inline constexpr std::string_view offset_b = "offset_b";
inline constexpr std::string_view temperature_kelvin = "temperature_kelvin";
inline constexpr std::string_view tint = "tint";
// Curves reuse the safe bounded indexed-property representation already used
// by keyframe channels: e.g. curve_master_0_x / curve_master_0_y. Parsing
// stops at the first gap and rejects later points or an index beyond the cap.
inline constexpr std::string_view curve_master_prefix = "curve_master_";
inline constexpr std::string_view curve_red_prefix = "curve_red_";
inline constexpr std::string_view curve_green_prefix = "curve_green_";
inline constexpr std::string_view curve_blue_prefix = "curve_blue_";
inline constexpr std::string_view curve_x_suffix = "_x";
inline constexpr std::string_view curve_y_suffix = "_y";
inline constexpr std::string_view amount = "amount";
inline constexpr std::string_view pixel_scale = "pixel_scale";
inline constexpr std::string_view chroma_ratio = "chroma_ratio";
inline constexpr std::string_view seed = "seed";
inline constexpr std::string_view mode = "mode";
inline constexpr std::string_view monochrome_mode = "monochrome";
inline constexpr std::string_view rgb_mode = "rgb";
// Added to the authored yaw, at the show frame.
inline constexpr std::string_view spin_turns_per_second =
    "spin_turns_per_second";
// A sinusoidal offset on Y, in world units, at the show frame.
inline constexpr std::string_view bob_amplitude = "bob_amplitude";
inline constexpr std::string_view bob_turns_per_second = "bob_turns_per_second";

// ---------------------------------------------------------------------------
// scene.settings
//
// These change the image, so they belong to the project rather than to the
// machine: two nodes disagreeing about a kernel width would render a visibly
// different frame of the same show.

inline constexpr std::string_view blur_taps = "blur_taps";
inline constexpr std::string_view blur_sigma_ratio = "blur_sigma_ratio";
inline constexpr std::string_view blur_passes = "blur_passes";
inline constexpr std::string_view directional_blur_taps =
    "directional_blur_taps";
inline constexpr std::string_view brightness_curve = "brightness_curve";
inline constexpr std::string_view exposure_curve = "exposure";
inline constexpr std::string_view gain_curve = "gain";
inline constexpr std::string_view brightness_gamma = "brightness_gamma";

// ---------------------------------------------------------------------------
// scene.media
//
// Placement reuses the object transform names (position_*, rotation_*, scale_*)
// so an operator moves a clip the same way they move any element.

// A printf-style path template for the image sequence, one unsigned frame
// number substituted, e.g. "plate_%04u.png".
inline constexpr std::string_view source = "source";
// Optional stable project-library identity. Rendering keeps using `source` as
// a backward-compatible fallback; the asset id survives path/name changes and
// lets the editor trace a placement back to its imported asset.
inline constexpr std::string_view asset_id = "asset_id";
// The file number of the first frame, and how many frames the sequence holds.
// The show frame maps to a file number by counting from the start and looping
// once it reaches the end.
inline constexpr std::string_view sequence_start = "sequence_start";
inline constexpr std::string_view sequence_count = "sequence_count";
// Optional exact native video rate. Both fields must be present and positive.
// Their absence is the backward-compatible image-sequence/legacy 1:1 cadence.
inline constexpr std::string_view source_rate_numerator =
    "source_rate_numerator";
inline constexpr std::string_view source_rate_denominator =
    "source_rate_denominator";

// Placement on the show timeline: which show frame the clip starts on, how
// many show frames it occupies, and which source frame its first show frame
// shows. A missing or zero `clip_duration` is the unplaced legacy form - the
// clip is always on and loops from show frame zero - so a project authored
// before the timeline had cuts in it plays exactly as it did.
//
// Integers, in frames, never seconds. The frame a cut lands on is replicated
// state and every node has to agree on it exactly.
inline constexpr std::string_view clip_start = "clip_start";
inline constexpr std::string_view clip_duration = "clip_duration";
inline constexpr std::string_view source_in = "source_in";
// Opacity ramps at the head and tail, in show frames. Overlapping ramps on two
// clips are a cross dissolve; a tail ramp over the scene's black surround is a
// fade to black.
inline constexpr std::string_view fade_in = "fade_in";
inline constexpr std::string_view fade_out = "fade_out";

// ---------------------------------------------------------------------------
// scene.channel
//
// The target is a node id (an integer). The component is one of the transform
// property names above (position_x .. scale_z); the channel replaces that
// component of the target at the show frame. Keys are indexed properties -
// key0_frame / key0_value / key0_interp, key1_frame, ... - parsed in order until
// the first index with no _frame. A key's interp names how it reaches the NEXT
// key: hold, linear, or cubic. Cubic keys optionally carry a tangent mode,
// finite in/out slopes (value per show frame), and positive in/out Bezier
// handle weights (show frames). Auto is the backwards-compatible default and
// derives monotone PCHIP/Fritsch-Carlson slopes with one-third-interval
// weights. Aligned evaluates both authored slopes as their symmetric line
// direction while retaining independent weights; Broken retains each slope;
// Flat uses zero slopes. Hold and Linear have no tangent carrier. Value beyond
// the first and last key is clamped.
inline constexpr std::string_view channel_target = "channel_target";
inline constexpr std::string_view channel_component = "channel_component";
inline constexpr std::string_view channel_key_prefix = "key";
inline constexpr std::string_view channel_key_frame_suffix = "_frame";
inline constexpr std::string_view channel_key_value_suffix = "_value";
inline constexpr std::string_view channel_key_interp_suffix = "_interp";
inline constexpr std::string_view channel_key_tangent_mode_suffix =
    "_tangent_mode";
inline constexpr std::string_view channel_key_in_slope_suffix = "_in_slope";
inline constexpr std::string_view channel_key_out_slope_suffix = "_out_slope";
inline constexpr std::string_view channel_key_in_weight_suffix = "_in_weight";
inline constexpr std::string_view channel_key_out_weight_suffix =
    "_out_weight";
inline constexpr std::string_view interp_hold = "hold";
inline constexpr std::string_view interp_linear = "linear";
inline constexpr std::string_view interp_cubic = "cubic";
inline constexpr std::string_view tangent_auto = "auto";
inline constexpr std::string_view tangent_aligned = "aligned";
inline constexpr std::string_view tangent_broken = "broken";
inline constexpr std::string_view tangent_flat = "flat";

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
