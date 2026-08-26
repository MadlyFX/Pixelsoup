#pragma once

#include <string_view>

namespace mediaserver::topology::schema {

inline constexpr std::string_view device_type = "hardware.device";
inline constexpr std::string_view link_type = "hardware.link";
inline constexpr std::string_view settings_type = "hardware.settings";
inline constexpr std::string_view output_binding_type =
    "hardware.output_binding";

inline constexpr std::string_view kind = "kind";
inline constexpr std::string_view name = "name";
inline constexpr std::string_view grid_x = "grid_x";
inline constexpr std::string_view grid_y = "grid_y";
inline constexpr std::string_view address = "address";
inline constexpr std::string_view lifecycle_endpoint = "lifecycle_endpoint";
inline constexpr std::string_view control_base_port = "control_base_port";
inline constexpr std::string_view node_index = "node_index";
inline constexpr std::string_view width = "width";
inline constexpr std::string_view height = "height";
inline constexpr std::string_view refresh_numerator = "refresh_numerator";
inline constexpr std::string_view refresh_denominator = "refresh_denominator";
inline constexpr std::string_view window_mode = "window_mode";
inline constexpr std::string_view desktop_x = "desktop_x";
inline constexpr std::string_view desktop_y = "desktop_y";
inline constexpr std::string_view render_server = "render_server";
inline constexpr std::string_view logical_port = "logical_port";
inline constexpr std::string_view feed_index = "feed_index";
inline constexpr std::string_view display_adapter_id = "display_adapter_id";
inline constexpr std::string_view display_target_id = "display_target_id";
inline constexpr std::string_view connector_instance = "connector_instance";

inline constexpr std::string_view inherit_window_mode = "inherit";
inline constexpr std::string_view windowed_window_mode = "windowed";
inline constexpr std::string_view borderless_window_mode = "borderless";
inline constexpr std::string_view fullscreen_window_mode = "fullscreen";

inline constexpr std::string_view render_server_kind = "render_server";
inline constexpr std::string_view operator_machine_kind = "operator_machine";
inline constexpr std::string_view led_processor_kind = "led_processor";
inline constexpr std::string_view led_wall_kind = "led_wall";

inline constexpr std::string_view from_device = "from_device";
inline constexpr std::string_view from_port = "from_port";
inline constexpr std::string_view to_device = "to_device";
inline constexpr std::string_view to_port = "to_port";

} // namespace mediaserver::topology::schema
