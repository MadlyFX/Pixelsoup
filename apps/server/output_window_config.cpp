#include "output_window_config.hpp"

namespace mediaserver::app {

PresenterWindowSettings presenter_window_settings(
    const PlaybackOptions& options,
    const topology::HardwareMap& hardware) noexcept {
    PresenterWindowSettings result{
        options.surface_width, options.surface_height, options.window_x,
        options.window_y, options.window_position_explicit, options.windowed,
        options.fullscreen, options.borderless, 0U, 0U, false};
    if (!options.windowed || !options.output) {
        return result;
    }
    const topology::HardwareDevice* selected{};
    for (const topology::HardwareDevice& device : hardware.devices) {
        if (device.kind != topology::HardwareDeviceKind::RenderServer ||
            device.node_index != options.node_index) {
            continue;
        }
        if (selected != nullptr) {
            return result;
        }
        selected = &device;
    }
    if (selected == nullptr ||
        selected->window_mode == topology::HardwareWindowMode::Inherit) {
        return result;
    }
    if (selected->width == 0U || selected->height == 0U ||
        selected->width > topology::max_hardware_output_width ||
        selected->height > topology::max_hardware_output_height) {
        return result;
    }

    result.width = selected->width;
    result.height = selected->height;
    result.position_x = selected->desktop_x;
    result.position_y = selected->desktop_y;
    result.position_explicit = true;
    result.windowed = true;
    result.fullscreen =
        selected->window_mode == topology::HardwareWindowMode::Fullscreen;
    result.borderless =
        selected->window_mode == topology::HardwareWindowMode::Borderless;
    return result;
}

std::optional<PresenterWindowSettings> presenter_window_update(
    const PlaybackOptions& options,
    const topology::HardwareMap& hardware,
    core::u64 rejected_nodes) noexcept {
    const PresenterWindowSettings fallback =
        presenter_window_settings(options, {});
    if (!options.windowed || !options.output) {
        return fallback;
    }

    const topology::HardwareDevice* selected{};
    for (const topology::HardwareDevice& device : hardware.devices) {
        if (device.kind != topology::HardwareDeviceKind::RenderServer ||
            device.node_index != options.node_index) {
            continue;
        }
        if (selected != nullptr) {
            return std::nullopt;
        }
        selected = &device;
    }
    if (selected == nullptr) {
        return rejected_nodes == 0U
            ? std::optional<PresenterWindowSettings>{fallback}
            : std::nullopt;
    }
    if (selected->window_mode != topology::HardwareWindowMode::Inherit &&
        (selected->width == 0U || selected->height == 0U ||
         selected->width > topology::max_hardware_output_width ||
         selected->height > topology::max_hardware_output_height)) {
        return std::nullopt;
    }
    return presenter_window_settings(options, hardware);
}

PlaybackOptions presenter_options(
    const PlaybackOptions& base,
    const PresenterWindowSettings& window) {
    PlaybackOptions result = base;
    result.surface_width = window.width;
    result.surface_height = window.height;
    result.window_x = window.position_x;
    result.window_y = window.position_y;
    result.window_position_explicit = window.position_explicit;
    result.windowed = window.windowed;
    result.fullscreen = window.fullscreen;
    result.borderless = window.borderless;
    return result;
}

} // namespace mediaserver::app
