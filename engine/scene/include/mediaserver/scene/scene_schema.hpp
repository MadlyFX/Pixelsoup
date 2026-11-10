#pragma once

#include <string_view>
#include <array>

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
// Static imported affine residual, composed after the ordinary editable TRS.
inline constexpr std::string_view model_affine = "model_affine_";
inline constexpr std::string_view model_factor_overrides = "model_factor_overrides";

// ---------------------------------------------------------------------------
// Node types

// A named scene's canvas and surround. Each stage node is a first-class scene;
// its document node id is the SceneId used by scoped content.
inline constexpr std::string_view stage_type = "scene.stage";
// A scene camera, authored against the canvas and never against a slice. The
// lowest-id camera in one scene wins if that scene carries duplicates.
inline constexpr std::string_view camera_type = "scene.camera";
// A named camera-tracking feed. Its own node rather than a project setting,
// because a volume has more than one camera on it and each has to be able to
// say which feed is its own. Read by the Director alone: it names a socket,
// which is not something a render node has any business holding.
inline constexpr std::string_view tracking_source_type =
    "scene.tracking_source";
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
// A named place on one scene's timeline. With a cue action it is also a show
// cue the Director's transport stops or loops on. Markers and cues are one
// type because they are one thing - a frame with a name - and turning a marker
// into a cue in the middle of a rehearsal should be a property change.
inline constexpr std::string_view timeline_marker_type =
    "scene.timeline_marker";
// An imported 3D model (glTF or OBJ), placed and transformed like an object.
// Its geometry and material are loaded from `source` by the renderer, so the
// document carries only the reference and the transform. Distribution of the
// file to nodes is external (a node without it draws nothing).
inline constexpr std::string_view model_type = "scene.model";
// A placed ajfx particle effect: an .ajfx package simulated on the GPU from the
// exact show frame and drawn in the scene, placed and transformed like a
// model. The package carries the effect and its precompiled kernels; the
// document carries the reference, the placement, a seed and when it starts.
inline constexpr std::string_view particle_effect_type = "scene.particle_effect";
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
// The .blend this scene is edited in, and the link source identity the Director
// hands to that Blender, both set by Open in Blender. A copy of the scene
// starts without them, so it never takes over the original's Blender file.
inline constexpr std::string_view blender_file = "blender_file";
inline constexpr std::string_view blender_link_source = "blender_link_source";
inline constexpr std::string_view canvas_width = "canvas_width";
inline constexpr std::string_view canvas_height = "canvas_height";
inline constexpr std::string_view sky_r = "sky_r";
inline constexpr std::string_view sky_g = "sky_g";
inline constexpr std::string_view sky_b = "sky_b";
inline constexpr std::string_view ground_r = "ground_r";
inline constexpr std::string_view ground_g = "ground_g";
inline constexpr std::string_view ground_b = "ground_b";
inline constexpr std::string_view ambient = "ambient";
inline constexpr std::string_view environment_map = "environment_map";
inline constexpr std::string_view sky_physical_enabled = "sky_physical_enabled";
inline constexpr std::string_view sky_time_of_day = "sky_time_of_day";
inline constexpr std::string_view sky_latitude = "sky_latitude";
inline constexpr std::string_view sky_north_rotation = "sky_north_rotation";
inline constexpr std::string_view sky_haze = "sky_haze";
inline constexpr std::string_view sky_solar_illuminance = "sky_solar_illuminance";
inline constexpr std::string_view sky_angular_radius = "sky_angular_radius";
inline constexpr std::string_view sky_ground_albedo = "sky_ground_albedo";
inline constexpr std::string_view environment_map_intensity =
    "environment_map_intensity";
inline constexpr std::string_view environment_map_rotation =
    "environment_map_rotation";
// Atmosphere, on the stage node beside the sky it scatters. The colour is
// three keys rather than one, matching `sky_r/g/b` above - the document format
// has no vector value and inventing one here would be a second convention.
inline constexpr std::string_view environment_fog_density =
    "environment_fog_density";
inline constexpr std::string_view fog_r = "fog_r";
inline constexpr std::string_view fog_g = "fog_g";
inline constexpr std::string_view fog_b = "fog_b";
inline constexpr std::string_view environment_fog_anisotropy =
    "environment_fog_anisotropy";
// Height falloff: the world height the air is densest at and below, and how
// quickly it thins above it. Zero falloff is uniform air.
inline constexpr std::string_view environment_fog_height =
    "environment_fog_height";
inline constexpr std::string_view environment_fog_height_falloff =
    "environment_fog_height_falloff";
inline constexpr std::string_view environment_fog_background_distance =
    "environment_fog_background_distance";

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
// The operator's highlight on a Scene panel row: an index into the editor's
// eight-colour palette (the same one timeline markers use), absent for none.
// Editor-only organisation, like `locked`: it never changes a rendered pixel
// and is not parsed into the Scene. Like every authored property it still
// enters the document hash the scene digest carries.
inline constexpr std::string_view outliner_color = "outliner_color";

// ---------------------------------------------------------------------------
// scene.group

inline constexpr std::string_view group_name = "group_name";
inline constexpr std::string_view visible = "visible";
// What the group is for. Absent is an ordinary folder; a vehicle process
// group is the container the vehicle processing workflow is built on. Any
// other word is a damaged node, not a folder.
inline constexpr std::string_view group_kind = "group_kind";
inline constexpr std::string_view group_kind_folder = "folder";
inline constexpr std::string_view group_kind_vehicle_process =
    "vehicle_process";

// ---------------------------------------------------------------------------
// scene.timeline_lane
//
// A track is a screen. It owns the plane its clips are drawn on, which is why
// it carries the ordinary position_/rotation_/scale_ and opacity properties
// declared for scene.object above rather than a private vocabulary: a screen
// is transformed, keyframed, and inspected exactly like anything else in the
// scene, and giving it parallel property names would have made every one of
// those paths need a special case.
//
// The clips inside keep their own transform properties untouched. They are
// simply not read while the clip is on a screen, so dragging a clip out of a
// track puts it back where it was rather than at the origin.

inline constexpr std::string_view lane_name = "lane_name";
inline constexpr std::string_view lane_order = "lane_order";
inline constexpr std::string_view lane_muted = "lane_muted";
inline constexpr std::string_view lane_locked = "lane_locked";
// A sync-locked track keeps time with ripple edits made on other tracks: its
// clips at or after the edit point move by the same amount. Not drawn.
inline constexpr std::string_view lane_sync_locked = "lane_sync_locked";
inline constexpr std::string_view lane_compositing_mode =
    "lane_compositing_mode";
inline constexpr std::string_view lane_normal_mode = "normal";
inline constexpr std::string_view lane_add_mode = "add";
inline constexpr std::string_view lane_multiply_mode = "multiply";
inline constexpr std::string_view lane_screen_mode = "screen";
// The screen's own format, in pixels. Authored, never derived from whatever
// media happens to be on it: an operator sets a screen to the format of the
// wall it drives, and a plate of the wrong size should look wrong rather than
// silently reshape the screen. Deliberately not called `screen_` anything,
// because `lane_screen_mode` above is the Screen blend mode.
inline constexpr std::string_view lane_raster_width = "lane_raster_width";
inline constexpr std::string_view lane_raster_height = "lane_raster_height";
// Ties the plane's height to its width and the raster above, so media on the
// screen cannot be stretched by a scale gesture. Enforced in evaluation rather
// than in the authoring path: a gizmo drag, a keyframe, and a typed field all
// have to obey it, and only evaluation sees all three.
inline constexpr std::string_view lane_aspect_locked = "lane_aspect_locked";

// ---------------------------------------------------------------------------
// scene.timeline_marker
//
// Owned by its scene through the shared scene_id property. The action is a
// word rather than an ordinal so a document read by eye says what the cue
// does.

inline constexpr std::string_view marker_frame = "marker_frame";
inline constexpr std::string_view marker_name = "marker_name";
inline constexpr std::string_view marker_note = "marker_note";
inline constexpr std::string_view marker_color = "marker_color";
inline constexpr std::string_view marker_cue = "marker_cue";
inline constexpr std::string_view marker_cue_none = "none";
inline constexpr std::string_view marker_cue_hold = "hold";
inline constexpr std::string_view marker_cue_loop = "loop";
// Optional, milliseconds; absent is zero.
inline constexpr std::string_view marker_loop_wait = "marker_loop_wait_ms";
inline constexpr std::string_view marker_loop_fade = "marker_loop_fade_ms";

// ---------------------------------------------------------------------------
// scene.camera
//
// A camera is placed like every other element: position_x..z and
// rotation_yaw/pitch/roll (see scene.object), with the rotation turning a
// camera that looks down -Z with +Y up.

// Vertical field of view, as a fraction of a full turn. 0.125 is 45 degrees.
inline constexpr std::string_view fov_turns = "fov_turns";
// The tracking source node feeding this camera, or absent for an ordinary
// authored camera. Assigning one is what makes a camera the inner frustum's.
inline constexpr std::string_view camera_tracking_source =
    "camera_tracking_source";
// Optional calibrated physical lens. Its absence keeps the historic turn-based
// camera path; when present the tracked inner view derives its window from the
// sensor/focal values below and the frame-stamped encoder channel.
inline constexpr std::string_view lens_profile_enabled =
    "lens_profile_enabled";
inline constexpr std::string_view lens_sensor_width_mm =
    "lens_sensor_width_mm";
inline constexpr std::string_view lens_sensor_height_mm =
    "lens_sensor_height_mm";
inline constexpr std::string_view lens_focal_length_mm =
    "lens_focal_length_mm";
inline constexpr std::string_view lens_entrance_pupil_offset_m =
    "lens_entrance_pupil_offset_m";
inline constexpr std::string_view lens_distortion_k1 = "lens_distortion_k1";
inline constexpr std::string_view lens_distortion_k2 = "lens_distortion_k2";
inline constexpr std::string_view lens_distortion_k3 = "lens_distortion_k3";
inline constexpr std::string_view lens_distortion_p1 = "lens_distortion_p1";
inline constexpr std::string_view lens_distortion_p2 = "lens_distortion_p2";
// Fixed-size calibration points are written as
// lens_calibration_<index>_<suffix>. The count is bounded by
// max_lens_calibration_points at the model boundary.
inline constexpr std::string_view lens_calibration_count =
    "lens_calibration_count";
inline constexpr std::string_view lens_calibration_prefix =
    "lens_calibration_";
inline constexpr std::string_view lens_calibration_zoom_suffix = "_zoom";
inline constexpr std::string_view lens_calibration_focus_suffix = "_focus";
inline constexpr std::string_view lens_calibration_focal_suffix = "_focal_mm";
inline constexpr std::string_view lens_calibration_pupil_suffix = "_pupil_m";
inline constexpr std::string_view lens_calibration_k1_suffix = "_k1";
inline constexpr std::string_view lens_calibration_k2_suffix = "_k2";
inline constexpr std::string_view lens_calibration_k3_suffix = "_k3";
inline constexpr std::string_view lens_calibration_p1_suffix = "_p1";
inline constexpr std::string_view lens_calibration_p2_suffix = "_p2";
inline constexpr std::string_view near_plane = "near";
inline constexpr std::string_view far_plane = "far";
inline constexpr std::string_view focus_distance_m = "focus_distance_m";
inline constexpr std::string_view aperture_f_number = "aperture_f_number";
inline constexpr std::string_view shutter_seconds = "shutter_seconds";
inline constexpr std::string_view iso = "iso";
inline constexpr std::string_view exposure_compensation_ev =
    "exposure_compensation_ev";
// Turns per second the camera sweeps around the stage's vertical axis (the
// line through the origin along +Y), turning with the sweep so it keeps
// facing the same way relative to that axis.
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
inline constexpr std::string_view emissive_strength = "emissive_strength";
inline constexpr std::string_view normal_scale = "normal_scale";
inline constexpr std::string_view occlusion_strength = "occlusion_strength";
inline constexpr std::string_view alpha_mode = "alpha_mode";
inline constexpr std::string_view alpha_opaque = "opaque";
inline constexpr std::string_view alpha_mask = "mask";
inline constexpr std::string_view alpha_blend = "blend";
inline constexpr std::string_view alpha_cutoff = "alpha_cutoff";
inline constexpr std::string_view double_sided = "double_sided";
inline constexpr std::string_view base_color_texture = "base_color_texture";
inline constexpr std::string_view metallic_roughness_texture =
    "metallic_roughness_texture";
inline constexpr std::string_view normal_texture = "normal_texture";
inline constexpr std::string_view occlusion_texture = "occlusion_texture";
inline constexpr std::string_view emissive_texture = "emissive_texture";
inline constexpr std::string_view base_alpha = "base_alpha";
inline constexpr std::string_view clearcoat = "clearcoat";
inline constexpr std::string_view clearcoat_roughness = "clearcoat_roughness";
inline constexpr std::string_view clearcoat_normal_scale = "clearcoat_normal_scale";
inline constexpr std::string_view sheen_roughness = "sheen_roughness";
inline constexpr std::string_view transmission = "transmission";
inline constexpr std::string_view ior = "ior";
inline constexpr std::string_view specular = "specular";
inline constexpr std::string_view anisotropy_strength = "anisotropy_strength";
inline constexpr std::string_view anisotropy_rotation = "anisotropy_rotation";
inline constexpr std::string_view sheen_color_r = "sheen_color_r";
inline constexpr std::string_view sheen_color_g = "sheen_color_g";
inline constexpr std::string_view sheen_color_b = "sheen_color_b";
inline constexpr std::string_view specular_color_r = "specular_color_r";
inline constexpr std::string_view specular_color_g = "specular_color_g";
inline constexpr std::string_view specular_color_b = "specular_color_b";
inline constexpr std::string_view clearcoat_texture = "clearcoat_texture";
inline constexpr std::string_view clearcoat_roughness_texture = "clearcoat_roughness_texture";
inline constexpr std::string_view clearcoat_normal_texture = "clearcoat_normal_texture";
inline constexpr std::string_view sheen_color_texture = "sheen_color_texture";
inline constexpr std::string_view sheen_roughness_texture = "sheen_roughness_texture";
inline constexpr std::string_view transmission_texture = "transmission_texture";
inline constexpr std::string_view specular_texture = "specular_texture";
inline constexpr std::string_view specular_color_texture = "specular_color_texture";
inline constexpr std::string_view anisotropy_texture = "anisotropy_texture";
// A Background surface's fill (scene::Fill), on a scene.object. Absent
// fill_mode is an ordinary surface; present, it is one of the three words
// below and the other keys default to scene::Fill's values. Centre and size
// are fractions of the surface's own width (local X) and height (local Z for
// a plane, Y for a quad), measured in its world size.
inline constexpr std::string_view fill_mode = "fill_mode";
inline constexpr std::string_view solid_fill = "solid";
inline constexpr std::string_view linear_fill = "linear";
inline constexpr std::string_view radial_fill = "radial";
inline constexpr std::string_view fill_a_r = "fill_a_r";
inline constexpr std::string_view fill_a_g = "fill_a_g";
inline constexpr std::string_view fill_a_b = "fill_a_b";
inline constexpr std::string_view fill_a_a = "fill_a_a";
inline constexpr std::string_view fill_b_r = "fill_b_r";
inline constexpr std::string_view fill_b_g = "fill_b_g";
inline constexpr std::string_view fill_b_b = "fill_b_b";
inline constexpr std::string_view fill_b_a = "fill_b_a";
inline constexpr std::string_view fill_angle_turns = "fill_angle_turns";
inline constexpr std::string_view fill_center_x = "fill_center_x";
inline constexpr std::string_view fill_center_y = "fill_center_y";
inline constexpr std::string_view fill_size = "fill_size";

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
// Rectangle/Ellipse geometry in normalized target space. Rotation is in
// clockwise turns about the bounds centre and defaults to zero for old shows.
inline constexpr std::string_view mask_left = "mask_left";
inline constexpr std::string_view mask_top = "mask_top";
inline constexpr std::string_view mask_right = "mask_right";
inline constexpr std::string_view mask_bottom = "mask_bottom";
inline constexpr std::string_view mask_rotation_turns =
    "mask_rotation_turns";

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
// A clip placed as one plate of a vehicle process plateset: the Library
// plateset it came from, the slot it plays in, and the take it belongs to.
// Clips sharing a take, a start and a length are edited as one; none of
// these change what is drawn, so they stay out of the scene digest.
inline constexpr std::string_view plateset_asset = "plateset_asset";
inline constexpr std::string_view plateset_slot = "plateset_slot";
inline constexpr std::string_view plateset_take = "plateset_take";

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
inline constexpr std::string_view graph_texture_budget_gb =
    "graph_texture_budget_gb";

// Renderer selection and advanced quality are show state. A node that cannot
// satisfy a required backend reports unready; it never rewrites these values or
// chooses a local quality tier.
inline constexpr std::string_view photometric_scale = "photometric_scale";
inline constexpr std::string_view renderer_backend = "renderer_backend";
inline constexpr std::string_view basic_renderer = "basic";
inline constexpr std::string_view advanced_renderer = "advanced";
inline constexpr std::string_view renderer_required = "renderer_required";
inline constexpr std::string_view renderer_quality = "renderer_quality";
inline constexpr std::string_view interactive_quality = "interactive";
inline constexpr std::string_view balanced_quality = "balanced";
inline constexpr std::string_view cinematic_quality = "cinematic";
inline constexpr std::string_view ray_tracing_enabled =
    "ray_tracing_enabled";
inline constexpr std::string_view ddgi_enabled = "ddgi_enabled";
inline constexpr std::string_view restir_enabled = "restir_enabled";
inline constexpr std::string_view denoising_enabled = "denoising_enabled";
inline constexpr std::string_view taau_enabled = "taau_enabled";
inline constexpr std::string_view spatial_antialiasing_enabled =
    "spatial_antialiasing_enabled";
inline constexpr std::string_view taa_sequence_length =
    "taa_sequence_length";
inline constexpr std::string_view motion_blur_enabled =
    "motion_blur_enabled";
inline constexpr std::string_view motion_blur_radius = "motion_blur_radius";
inline constexpr std::string_view plate_depth_of_field = "plate_depth_of_field";
inline constexpr std::string_view plate_dof_radius = "plate_dof_radius";
inline constexpr std::string_view ddgi_probe_count_x = "ddgi_probe_count_x";
inline constexpr std::string_view ddgi_probe_count_y = "ddgi_probe_count_y";
inline constexpr std::string_view ddgi_probe_count_z = "ddgi_probe_count_z";
inline constexpr std::string_view ddgi_rays_per_probe =
    "ddgi_rays_per_probe";
inline constexpr std::string_view slice_apron_limit = "slice_apron_limit";
// Directional shadow and bounced-light policy for the Advanced raster. Both
// are show state for the same reason the backend is: a node that chose its
// own cascade count or bounce strength would light its share of the wall
// differently from the node beside it.
inline constexpr std::string_view frustum_culling = "frustum_culling";
inline constexpr std::string_view geometry_instancing = "geometry_instancing";
inline constexpr std::string_view model_lod = "model_lod";
inline constexpr std::string_view texture_streaming = "texture_streaming";
inline constexpr std::string_view texture_budget_mib = "texture_budget_mib";
inline constexpr std::string_view frame_budget_us = "frame_budget_us";
inline constexpr std::string_view texture_mip_bias = "texture_mip_bias";
inline constexpr std::string_view texture_anisotropy = "texture_anisotropy";
inline constexpr std::string_view texture_lod_bias = "texture_lod_bias";
inline constexpr std::string_view shadow_cascades = "shadow_cascades";
inline constexpr std::string_view shadow_blend_band = "shadow_blend_band";
inline constexpr std::string_view shadow_filter_radius = "shadow_filter_radius";
inline constexpr std::string_view contact_shadow_length = "contact_shadow_length";
inline constexpr std::string_view contact_shadow_thickness = "contact_shadow_thickness";

inline constexpr std::string_view baked_enabled = "baked_enabled";
inline constexpr std::string_view baked_min_x = "baked_min_x";
inline constexpr std::string_view baked_min_y = "baked_min_y";
inline constexpr std::string_view baked_min_z = "baked_min_z";
inline constexpr std::string_view baked_max_x = "baked_max_x";
inline constexpr std::string_view baked_max_y = "baked_max_y";
inline constexpr std::string_view baked_max_z = "baked_max_z";
inline constexpr std::string_view baked_count_x = "baked_count_x";
inline constexpr std::string_view baked_count_y = "baked_count_y";
inline constexpr std::string_view baked_count_z = "baked_count_z";
inline constexpr std::string_view baked_samples = "baked_samples";
inline constexpr std::string_view baked_bounces = "baked_bounces";
inline constexpr std::string_view baked_strength = "baked_strength";
inline constexpr std::string_view baked_source = "baked_source";
inline constexpr std::string_view baked_content = "baked_content";
inline constexpr std::string_view baked_input_revision = "baked_input_revision";
inline constexpr std::string_view asset_refresh_generation = "asset_refresh_generation";
inline constexpr std::string_view asset_commit_digest = "asset_commit_digest";
inline constexpr std::string_view asset_commit_control = "asset_commit_control";
inline constexpr std::string_view asset_commit_show = "asset_commit_show";
inline constexpr std::string_view bake_static = "bake_static";
inline constexpr std::string_view animation_enabled = "animation_enabled";
inline constexpr std::string_view animation_clip = "animation_clip";
inline constexpr std::string_view animation_loop = "animation_loop";
inline constexpr std::string_view animation_start_frame = "animation_start_frame";
inline constexpr std::string_view wind_x = "wind_x";
inline constexpr std::string_view wind_y = "wind_y";
inline constexpr std::string_view wind_z = "wind_z";
inline constexpr std::string_view wind_period_frames = "wind_period_frames";
inline constexpr std::string_view bake_indirect = "bake_indirect";
inline constexpr std::string_view gi_strength = "gi_strength";
// Screen-space occlusion. Prefixed because `occlusion_strength` is
// already a material property and these are renderer policy - two
// different things that would otherwise share one name.
inline constexpr std::string_view screen_occlusion_radius =
    "screen_occlusion_radius";
inline constexpr std::string_view screen_occlusion_strength =
    "screen_occlusion_strength";
// Bloom. Only the strength is authored; the chain's depth is a function of the
// canvas, so there is nothing here for a node to disagree about.
inline constexpr std::string_view bloom_strength = "bloom_strength";
// Colour at the output boundary. Show state for the same reason the backend
// is: two nodes grading the two halves of one wall differently is a seam. The
// transform names are stable identifiers, not display strings.
inline constexpr std::string_view display_transform = "display_transform";
inline constexpr std::string_view srgb_display_transform = "srgb";
inline constexpr std::string_view aces_display_transform =
    "aces_approximate";
inline constexpr std::string_view aces13_display_transform = "aces_1_3";
inline constexpr std::string_view wall_color_enabled = "wall_color_enabled";
inline constexpr std::string_view camera_inverse_lut = "camera_inverse_lut";
inline constexpr std::array<std::string_view,9> wall_primaries{
    "wall_r_r", "wall_r_g", "wall_r_b", "wall_g_r", "wall_g_g", "wall_g_b", "wall_b_r", "wall_b_g", "wall_b_b"};
// Path to the `.cube` show look composed after the display transform. Read by
// the residency worker, never by the frame path.
inline constexpr std::string_view show_lut = "show_lut";
inline constexpr std::string_view output_dither = "output_dither";

// The show's inner-frustum master switch and its latency compensation. Show
// state because they describe the wall and the wire rather than one camera: two
// nodes compensating differently would show two different moments.
inline constexpr std::string_view inner_frustum_enabled =
    "inner_frustum_enabled";
inline constexpr std::string_view tracking_latency_frames =
    "tracking_latency_frames";
inline constexpr std::string_view tracking_pose_prediction = "tracking_pose_prediction";
inline constexpr std::string_view tracking_prediction_window = "tracking_prediction_window";
inline constexpr std::string_view tracking_jitter_filter_percent = "tracking_jitter_filter_percent";

// What a newly added camera starts from. Defaults, not policy - once a camera
// exists it carries its own values under the camera keys below, and editing
// these never reaches back and changes a camera an operator already tuned.
inline constexpr std::string_view inner_frustum_margin_px =
    "inner_frustum_margin_px";
inline constexpr std::string_view inner_frustum_soft_edge_px =
    "inner_frustum_soft_edge_px";
inline constexpr std::string_view inner_frustum_scale_numerator =
    "inner_frustum_scale_numerator";
inline constexpr std::string_view inner_frustum_scale_denominator =
    "inner_frustum_scale_denominator";

// How one camera draws its plate. On the camera node, because two cameras on
// a volume are doing different jobs and a stage sets overscan and softness on
// each of them.
inline constexpr std::string_view camera_inner_frustum =
    "camera_inner_frustum";
inline constexpr std::string_view camera_inner_frustum_margin_px =
    "camera_inner_frustum_margin_px";
inline constexpr std::string_view camera_inner_frustum_soft_edge_px =
    "camera_inner_frustum_soft_edge_px";
inline constexpr std::string_view camera_inner_frustum_scale_numerator =
    "camera_inner_frustum_scale_numerator";
inline constexpr std::string_view camera_inner_frustum_scale_denominator =
    "camera_inner_frustum_scale_denominator";
// Bigger draws over, ties broken on the camera node id.
inline constexpr std::string_view camera_render_order = "camera_render_order";

// The camera-tracking feeds. Project state, saved with the show, but read by
// the Director alone: they are deliberately absent from the evaluated scene and
// from the renderer profile, because a listen endpoint is not something a
// render node has any business holding.
inline constexpr std::string_view tracking_source_name =
    "tracking_source_name";
inline constexpr std::string_view tracking_enabled = "tracking_enabled";
inline constexpr std::string_view tracking_protocol = "tracking_protocol";
inline constexpr std::string_view freed_protocol = "freed";
inline constexpr std::string_view opentrackio_protocol = "opentrackio";
inline constexpr std::string_view natnet_protocol = "natnet";
inline constexpr std::string_view tracking_port = "tracking_port";
inline constexpr std::string_view tracking_camera_id = "tracking_camera_id";
// NatNet only: the multicast group as dotted decimal, and the server's
// command port. Absent means Motive's defaults.
inline constexpr std::string_view tracking_multicast_group =
    "tracking_multicast_group";
inline constexpr std::string_view tracking_command_port =
    "tracking_command_port";
inline constexpr std::string_view tracking_sensor_height_mm =
    "tracking_sensor_height_mm";
inline constexpr std::string_view tracking_sensor_aspect =
    "tracking_sensor_aspect";
inline constexpr std::string_view tracking_fallback_focal_mm =
    "tracking_fallback_focal_mm";
// Where this tracking system's numbers sit relative to the stage. Absent means
// the tracker already reports in stage space.
inline constexpr std::string_view tracking_axes = "tracking_axes";
inline constexpr std::string_view tracking_axes_stage = "stage";
inline constexpr std::string_view tracking_axes_z_up = "z_up";
inline constexpr std::string_view tracking_origin_x = "tracking_origin_x";
inline constexpr std::string_view tracking_origin_y = "tracking_origin_y";
inline constexpr std::string_view tracking_origin_z = "tracking_origin_z";
inline constexpr std::string_view tracking_heading = "tracking_heading";
inline constexpr std::string_view tracking_scale = "tracking_scale";

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
// Exact native video rate, recorded when a video is imported. Both fields
// must be present and positive. An image sequence has none and shows one file
// per show frame.
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
// The size a video clip is held at once decoded: "full", or "half" to average
// each 2x2 block as the frame is decoded and hold a quarter of the memory. A
// missing property is full. Stills ignore it.
inline constexpr std::string_view media_resolution = "media_resolution";
inline constexpr std::string_view media_resolution_full = "full";
inline constexpr std::string_view media_resolution_half = "half";

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
// Every per-key tangent property, in a fixed order. Anything that moves,
// copies or re-indexes keys carries all of them with the key.
inline constexpr std::array<std::string_view, 5U> channel_key_tangent_suffixes{
    channel_key_tangent_mode_suffix, channel_key_in_slope_suffix,
    channel_key_out_slope_suffix, channel_key_in_weight_suffix,
    channel_key_out_weight_suffix};
inline constexpr std::string_view interp_hold = "hold";
inline constexpr std::string_view interp_linear = "linear";
inline constexpr std::string_view interp_cubic = "cubic";
inline constexpr std::string_view tangent_auto = "auto";
inline constexpr std::string_view tangent_aligned = "aligned";
inline constexpr std::string_view tangent_broken = "broken";
inline constexpr std::string_view tangent_flat = "flat";

// ---------------------------------------------------------------------------
// scene.light

// "directional", "point", "spot", or "rectangle".
inline constexpr std::string_view light_kind = "kind";
inline constexpr std::string_view directional_kind = "directional";
inline constexpr std::string_view point_kind = "point";
inline constexpr std::string_view spot_kind = "spot";
inline constexpr std::string_view rectangle_kind = "rectangle";
inline constexpr std::string_view sphere_kind = "sphere";
inline constexpr std::string_view light_radius = "light_radius";
inline constexpr std::string_view area_rotation_turns = "area_rotation_turns";
// The direction a directional light travels, not the direction towards it.
inline constexpr std::string_view direction_x = "direction_x";
inline constexpr std::string_view direction_y = "direction_y";
inline constexpr std::string_view direction_z = "direction_z";
inline constexpr std::string_view intensity = "intensity";
inline constexpr std::string_view range = "range";
inline constexpr std::string_view light_unit = "light_unit";
inline constexpr std::string_view unitless_unit = "unitless";
inline constexpr std::string_view lumens_unit = "lumens";
inline constexpr std::string_view candela_unit = "candela";
inline constexpr std::string_view lux_unit = "lux";
inline constexpr std::string_view nits_unit = "nits";
inline constexpr std::string_view inner_cone_turns = "inner_cone_turns";
inline constexpr std::string_view outer_cone_turns = "outer_cone_turns";
inline constexpr std::string_view light_width = "light_width";
inline constexpr std::string_view light_height = "light_height";
inline constexpr std::string_view two_sided = "two_sided";
// Whether a local light gets a shadow atlas tile, and how big.
// "small", "medium" or "large".
inline constexpr std::string_view casts_shadow = "casts_shadow";
inline constexpr std::string_view shadow_tier = "shadow_tier";
inline constexpr std::string_view small_tier = "small";
inline constexpr std::string_view medium_tier = "medium";
inline constexpr std::string_view large_tier = "large";
inline constexpr std::string_view ies_source = "ies_source";
inline constexpr std::string_view ies_rotation_turns = "ies_rotation_turns";
inline constexpr std::string_view angular_radius_turns =
    "angular_radius_turns";
inline constexpr std::string_view color_temperature_kelvin =
    "color_temperature_kelvin";
inline constexpr std::string_view use_color_temperature =
    "use_color_temperature";

// ---------------------------------------------------------------------------
// scene.particle_effect
//
// Also `source` (the .ajfx package), the drawable transform, `visible`,
// `locked`, `parent_group` and `group_order`.

// Separates placements of one effect: two instances with different seeds
// differ, and every node draws the same particles for the same seed.
inline constexpr std::string_view particle_seed = "particle_seed";
// The show frame at which the effect's own time begins. Before it nothing is
// drawn; from it, the effect is at (show frame - start) in its own time.
inline constexpr std::string_view particle_start_frame = "particle_start_frame";
// A published parameter component's base value: `particle_parameter.` then
// the key, e.g. `particle_parameter.rate.0` or `particle_parameter.wind.2`.
// A scene.channel whose channel_component is the same text keys it. Names
// are the package's own, so a re-exported package keeps its keys; a key the
// package no longer publishes is kept and ignored.
inline constexpr std::string_view particle_parameter_prefix = "particle_parameter.";

// ---------------------------------------------------------------------------
// scene.slice

inline constexpr std::string_view node_index = "node_index";
inline constexpr std::string_view slice_x = "x";
inline constexpr std::string_view slice_y = "y";
inline constexpr std::string_view slice_width = "width";
inline constexpr std::string_view slice_height = "height";

} // namespace mediaserver::scene::schema
