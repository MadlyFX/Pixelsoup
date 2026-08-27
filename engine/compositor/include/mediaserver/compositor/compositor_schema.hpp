#pragma once

#include <string_view>

namespace mediaserver::compositor::schema {

// Graph objects are ordinary document nodes. The editor can therefore create,
// move, undo, replicate, and persist them without a second graph file format.
inline constexpr std::string_view scene_layer_type = "compositor.scene_layer";
inline constexpr std::string_view brightness_type = "compositor.brightness";
inline constexpr std::string_view blur_type = "compositor.blur";
inline constexpr std::string_view radial_array_type = "compositor.radial_array";
inline constexpr std::string_view composite_type = "compositor.composite";
inline constexpr std::string_view output_type = "compositor.output";
inline constexpr std::string_view edge_type = "compositor.edge";

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
inline constexpr std::string_view count = "count";
inline constexpr std::string_view center_x = "center_x";
inline constexpr std::string_view center_y = "center_y";
inline constexpr std::string_view center_z = "center_z";
inline constexpr std::string_view start_angle_turns = "start_angle_turns";

// Output normally preserves scene drawables that are not represented by an
// Output-reachable SceneLayer. Set this explicitly to make the graph a full
// image replacement instead of a selective layer/effect override.
inline constexpr std::string_view replace_scene = "replace_scene";

inline constexpr std::string_view from_node = "from_node";
inline constexpr std::string_view from_port = "from_port";
inline constexpr std::string_view to_node = "to_node";
inline constexpr std::string_view to_port = "to_port";
inline constexpr std::string_view order = "order";

} // namespace mediaserver::compositor::schema
