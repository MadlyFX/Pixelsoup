#pragma once

#include <mediaserver/scene/scene_schema.hpp>

#include <array>
#include <string_view>

namespace mediaserver::compositor::schema {

// Graph objects are ordinary document nodes. The editor can therefore create,
// move, undo, replicate, and persist them without a second graph file format.
inline constexpr std::string_view scene_layer_type = "compositor.scene_layer";
inline constexpr std::string_view brightness_type = "compositor.brightness";
inline constexpr std::string_view saturation_type = "compositor.saturation";
inline constexpr std::string_view blur_type = "compositor.blur";
inline constexpr std::string_view levels_type = "compositor.levels";
inline constexpr std::string_view lift_gamma_gain_type =
    "compositor.lift_gamma_gain";
inline constexpr std::string_view contrast_pivot_type =
    "compositor.contrast_pivot";
inline constexpr std::string_view color_matrix_type = "compositor.color_matrix";
inline constexpr std::string_view white_balance_type =
    "compositor.white_balance";
inline constexpr std::string_view curves_type = "compositor.curves";
inline constexpr std::string_view film_grain_type = "compositor.film_grain";
inline constexpr std::string_view box_blur_type = "compositor.box_blur";
inline constexpr std::string_view directional_blur_type =
    "compositor.directional_blur";
inline constexpr std::string_view chroma_key_type = "compositor.chroma_key";
inline constexpr std::string_view luma_key_type = "compositor.luma_key";
inline constexpr std::string_view matte_type = "compositor.matte";
inline constexpr std::string_view radial_array_type = "compositor.radial_array";
inline constexpr std::string_view led_car_process_type =
    "compositor.led_car_process";
inline constexpr std::string_view composite_type = "compositor.composite";
inline constexpr std::string_view output_type = "compositor.output";
inline constexpr std::string_view edge_type = "compositor.edge";

// The document command layer uses this catalog to distinguish persistent
// compositor nodes from unrelated document objects. Keeping the node types in
// one place prevents a newly-added node from becoming impossible to delete.
inline constexpr std::array<std::string_view, 20U> node_types{{
    scene_layer_type,      brightness_type,
    saturation_type,       blur_type,
    levels_type,           lift_gamma_gain_type,
    contrast_pivot_type,   color_matrix_type,
    white_balance_type,    curves_type,
    film_grain_type,       box_blur_type,
    directional_blur_type, chroma_key_type,
    luma_key_type,         matte_type,
    radial_array_type,     led_car_process_type,
    composite_type,        output_type,
}};

[[nodiscard]] constexpr bool is_node_type(std::string_view type) noexcept {
  for (const std::string_view candidate : node_types) {
    if (candidate == type) {
      return true;
    }
  }
  return false;
}

// Graph objects belong to exactly one scene, using the same shared ownership
// field the scene vocabulary uses for cameras, drawables, and lights. That is
// what lets Preview carry its own composite: an operator can rebuild the graph
// of the scene they are rehearsing without touching the graph Program renders.
// A missing field is legacy content and belongs to the lowest-id stage.
inline constexpr std::string_view scene_id = "scene_id";

inline constexpr std::string_view label = "label";
inline constexpr std::string_view editor_x = "editor_x";
inline constexpr std::string_view editor_y = "editor_y";

inline constexpr std::string_view scene_node = "scene_node";
inline constexpr std::string_view amount = "amount";
inline constexpr std::string_view radius = "radius";
inline constexpr std::string_view box_blur_radius =
    scene::schema::box_blur_radius;
inline constexpr std::string_view box_blur_passes =
    scene::schema::box_blur_passes;
inline constexpr std::string_view directional_blur_length =
    scene::schema::directional_blur_length;
inline constexpr std::string_view directional_blur_angle_turns =
    scene::schema::directional_blur_angle_turns;
inline constexpr std::string_view chroma_key_color_r =
    scene::schema::chroma_key_color_r;
inline constexpr std::string_view chroma_key_color_g =
    scene::schema::chroma_key_color_g;
inline constexpr std::string_view chroma_key_color_b =
    scene::schema::chroma_key_color_b;
inline constexpr std::string_view chroma_key_tolerance =
    scene::schema::chroma_key_tolerance;
inline constexpr std::string_view chroma_key_softness =
    scene::schema::chroma_key_softness;
inline constexpr std::string_view chroma_key_morphology_px =
    scene::schema::chroma_key_morphology_px;
inline constexpr std::string_view chroma_key_spill_suppression =
    scene::schema::chroma_key_spill_suppression;
inline constexpr std::string_view chroma_key_mix =
    scene::schema::chroma_key_mix;
inline constexpr std::string_view luma_key_low_threshold =
    scene::schema::luma_key_low_threshold;
inline constexpr std::string_view luma_key_high_threshold =
    scene::schema::luma_key_high_threshold;
inline constexpr std::string_view luma_key_low_softness =
    scene::schema::luma_key_low_softness;
inline constexpr std::string_view luma_key_high_softness =
    scene::schema::luma_key_high_softness;
inline constexpr std::string_view luma_key_invert =
    scene::schema::luma_key_invert;
inline constexpr std::string_view luma_key_mix = scene::schema::luma_key_mix;
inline constexpr std::string_view matte_mode = "matte_mode";
inline constexpr std::string_view matte_alpha_mode = "alpha";
inline constexpr std::string_view matte_luma_mode = "luma";
inline constexpr std::string_view matte_invert = "matte_invert";
inline constexpr std::string_view matte_clamp_input = "matte_clamp_input";
inline constexpr std::string_view levels_master_input_black =
    scene::schema::levels_master_input_black;
inline constexpr std::string_view levels_master_input_white =
    scene::schema::levels_master_input_white;
inline constexpr std::string_view levels_master_gamma =
    scene::schema::levels_master_gamma;
inline constexpr std::string_view levels_master_output_black =
    scene::schema::levels_master_output_black;
inline constexpr std::string_view levels_master_output_white =
    scene::schema::levels_master_output_white;
inline constexpr std::string_view levels_red_input_black =
    scene::schema::levels_red_input_black;
inline constexpr std::string_view levels_red_input_white =
    scene::schema::levels_red_input_white;
inline constexpr std::string_view levels_red_gamma =
    scene::schema::levels_red_gamma;
inline constexpr std::string_view levels_red_output_black =
    scene::schema::levels_red_output_black;
inline constexpr std::string_view levels_red_output_white =
    scene::schema::levels_red_output_white;
inline constexpr std::string_view levels_green_input_black =
    scene::schema::levels_green_input_black;
inline constexpr std::string_view levels_green_input_white =
    scene::schema::levels_green_input_white;
inline constexpr std::string_view levels_green_gamma =
    scene::schema::levels_green_gamma;
inline constexpr std::string_view levels_green_output_black =
    scene::schema::levels_green_output_black;
inline constexpr std::string_view levels_green_output_white =
    scene::schema::levels_green_output_white;
inline constexpr std::string_view levels_blue_input_black =
    scene::schema::levels_blue_input_black;
inline constexpr std::string_view levels_blue_input_white =
    scene::schema::levels_blue_input_white;
inline constexpr std::string_view levels_blue_gamma =
    scene::schema::levels_blue_gamma;
inline constexpr std::string_view levels_blue_output_black =
    scene::schema::levels_blue_output_black;
inline constexpr std::string_view levels_blue_output_white =
    scene::schema::levels_blue_output_white;
inline constexpr std::string_view lift_r = scene::schema::lift_r;
inline constexpr std::string_view lift_g = scene::schema::lift_g;
inline constexpr std::string_view lift_b = scene::schema::lift_b;
inline constexpr std::string_view gamma_r = scene::schema::gamma_r;
inline constexpr std::string_view gamma_g = scene::schema::gamma_g;
inline constexpr std::string_view gamma_b = scene::schema::gamma_b;
inline constexpr std::string_view gain_r = scene::schema::gain_r;
inline constexpr std::string_view gain_g = scene::schema::gain_g;
inline constexpr std::string_view gain_b = scene::schema::gain_b;
inline constexpr std::string_view contrast = scene::schema::contrast;
inline constexpr std::string_view pivot = scene::schema::pivot;
inline constexpr std::string_view m00 = scene::schema::m00;
inline constexpr std::string_view m01 = scene::schema::m01;
inline constexpr std::string_view m02 = scene::schema::m02;
inline constexpr std::string_view m10 = scene::schema::m10;
inline constexpr std::string_view m11 = scene::schema::m11;
inline constexpr std::string_view m12 = scene::schema::m12;
inline constexpr std::string_view m20 = scene::schema::m20;
inline constexpr std::string_view m21 = scene::schema::m21;
inline constexpr std::string_view m22 = scene::schema::m22;
inline constexpr std::string_view offset_r = scene::schema::offset_r;
inline constexpr std::string_view offset_g = scene::schema::offset_g;
inline constexpr std::string_view offset_b = scene::schema::offset_b;
inline constexpr std::string_view temperature_kelvin =
    scene::schema::temperature_kelvin;
inline constexpr std::string_view tint = scene::schema::tint;
inline constexpr std::string_view curve_master_prefix =
    scene::schema::curve_master_prefix;
inline constexpr std::string_view curve_red_prefix =
    scene::schema::curve_red_prefix;
inline constexpr std::string_view curve_green_prefix =
    scene::schema::curve_green_prefix;
inline constexpr std::string_view curve_blue_prefix =
    scene::schema::curve_blue_prefix;
inline constexpr std::string_view curve_x_suffix =
    scene::schema::curve_x_suffix;
inline constexpr std::string_view curve_y_suffix =
    scene::schema::curve_y_suffix;
inline constexpr std::string_view pixel_scale = scene::schema::pixel_scale;
inline constexpr std::string_view chroma_ratio = scene::schema::chroma_ratio;
inline constexpr std::string_view seed = scene::schema::seed;
inline constexpr std::string_view mode = scene::schema::mode;
inline constexpr std::string_view monochrome_mode =
    scene::schema::monochrome_mode;
inline constexpr std::string_view rgb_mode = scene::schema::rgb_mode;
inline constexpr std::string_view count = "count";
inline constexpr std::string_view center_x = "center_x";
inline constexpr std::string_view center_y = "center_y";
inline constexpr std::string_view center_z = "center_z";
inline constexpr std::string_view start_angle_turns = "start_angle_turns";
inline constexpr std::string_view position_x = "position_x";
inline constexpr std::string_view position_y = "position_y";
inline constexpr std::string_view position_z = "position_z";
inline constexpr std::string_view rotation_yaw_turns = "rotation_yaw_turns";
inline constexpr std::string_view rotation_pitch_turns = "rotation_pitch_turns";
inline constexpr std::string_view rotation_roll_turns = "rotation_roll_turns";
inline constexpr std::string_view plate_scale = "plate_scale";
inline constexpr std::string_view array_scale = "array_scale";
inline constexpr std::string_view reflection_height = "reflection_height";
inline constexpr std::string_view reflection_scale = "reflection_scale";
inline constexpr std::string_view horizontal_blend = "horizontal_blend";

// Output normally preserves scene drawables that are not represented by an
// Output-reachable SceneLayer. Set this explicitly to make the graph a full
// image replacement instead of a selective layer/effect override.
inline constexpr std::string_view replace_scene = "replace_scene";

inline constexpr std::string_view from_node = "from_node";
inline constexpr std::string_view from_port = "from_port";
inline constexpr std::string_view to_node = "to_node";
inline constexpr std::string_view to_port = "to_port";
inline constexpr std::string_view order = "order";
// A wire's reroute points use the bounded indexed-property form curves use:
// point_0_x / point_0_y, contiguous from zero.
inline constexpr std::string_view point_prefix = "point_";
inline constexpr std::string_view point_x_suffix = "_x";
inline constexpr std::string_view point_y_suffix = "_y";

} // namespace mediaserver::compositor::schema
