#include <mediaserver/editor/workspace_panels.hpp>

#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/rect_cut.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>

namespace mediaserver::editor {
namespace {
// The value playback shows at `frame`: the scene's own sampler, with its
// tangents and weights, so a key added here keeps the evaluated value.
double sample(const scene::ChannelKeyframes &channel, core::u64 frame,
              double base) {
  if (frame > static_cast<core::u64>(std::numeric_limits<core::i64>::max()))
    return base;
  const std::optional<double> value =
      scene::sample_keyframes(channel, static_cast<core::i64>(frame));
  return value.has_value() && std::isfinite(*value) ? *value : base;
}
} // namespace

SceneSettingsFrame SceneSettingsPanel::draw(
    gui::UiContext &ctx, const gui::Rect &bounds, SceneSettingsTarget target,
    const ProjectSettingsState &state,
    std::span<const scene::ChannelKeyframes> keyframes, core::u64 show_frame,
    std::span<const ElementInfo> elements, bool auto_key) {
  SceneSettingsFrame result;
  if (target == SceneSettingsTarget::None || bounds.width <= 0.0F ||
      bounds.height <= 0.0F)
    return result;
  if (target_ != target || stage_ != state.stage_node) {
    fields_.cancel(ctx);
    scroll_.reset();
    target_ = target;
    stage_ = state.stage_node;
  }
  ProjectSettingsValues values{state.settings, state.renderer,
                               state.environment, state.lens_profile};
  auto &renderer = values.renderer;
  auto &environment = values.environment;
  bool changed = false;
  ctx.panel(bounds);
  ctx.push_clip(bounds);

  // The docked settings rhythm: rows taller than a dialog's, sections 8px
  // apart, and the docked margin - with 4px more on the right, where the
  // scroll bar runs.
  constexpr float header_height = 40.0F;
  constexpr float row_height = 26.0F;
  constexpr float section_space = 8.0F;
  constexpr float scroll_gutter = 4.0F;
  constexpr float scroll_bar_width = 3.0F;
  gui::Metrics m = ctx.theme().metrics;
  m.control = row_height;
  m.inset = m.panel_inset;

  gui::Rect area = bounds;
  ctx.label(gui::inset(gui::cut_top(area, header_height),
                       gui::Padding{m.inset, m.gap, m.inset, 4.0F}),
            target == SceneSettingsTarget::Renderer ? "Renderer"
                                                    : "Environment");
  ctx.panel(gui::cut_top(area, m.divider), ctx.theme().divider);
  const gui::Rect body =
      gui::inset(area, gui::Padding{0.0F, 0.0F, scroll_gutter, 0.0F});
  gui::FormLayout form{body, m, scroll_.begin(ctx, body)};
  // Labels share a column that grows with the panel, within limits.
  const float label_width = std::clamp(body.width * 0.44F, 80.0F, 150.0F);
  constexpr float label_padding = 5.0F;
  const auto id = [&](std::string_view name) {
    return gui::ui_id(name) ^ 0x5CE0E5E77ULL ^ state.stage_node.value();
  };
  // A label and its field on one row - or, when the label would not fit its
  // column, the label on a row of its own with the field under it.
  const auto well = [&](std::string_view label) {
    gui::Rect field = form.control();
    if (ctx.font_width(label) > label_width - label_padding * 2.0F) {
      ctx.label(field, label, ctx.theme().text_muted);
      return form.control();
    }
    ctx.label(gui::inset(gui::cut_left(field, label_width),
                         gui::Padding{0.0F, 0.0F, label_padding, 0.0F}),
              label, ctx.theme().text_muted);
    return field;
  };
  const auto section = [&](std::size_t index, std::string_view label) {
    form.space(section_space);
    gui::Rect r = form.control();
    const auto interaction = ctx.grab(id(label), r);
    ctx.panel(r, ctx.theme().header);
    static_cast<void>(gui::cut_left(r, 7.0F));
    ctx.label(gui::cut_left(r, 16.0F), sections_[index] ? "v" : ">");
    std::string title{label};
    for (std::size_t i = 1U; i < title.size(); ++i)
      if (title[i] >= 'A' && title[i] <= 'Z') title[i] += 'a' - 'A';
    ctx.label(gui::inset(r, gui::Padding{0.0F, 0.0F, 4.0F, 0.0F}), title);
    if (interaction.clicked)
      sections_[index] = !sections_[index];
    return sections_[index];
  };
  const auto check = [&](std::string_view label, bool &value) {
    if (ctx.checkbox(id(label), well(label), value, ""))
      changed = true;
  };
  const auto choice = [&](std::string_view label, auto &value,
                          std::initializer_list<std::string_view> options) {
    auto selected = static_cast<std::size_t>(value);
    if (ctx.dropdown(id(label), well(label),
                     std::span{options.begin(), options.size()}, selected)) {
      value = static_cast<std::remove_reference_t<decltype(value)>>(selected);
      changed = true;
    }
  };
  // Every number below edits a member of the renderer or environment
  // settings in place, so its default is the same member of fresh settings:
  // found by where the value sits. Anything else has no default here.
  const ProjectSettingsValues fresh{};
  const auto default_of = [&]<typename Value>(const Value &value)
      -> std::optional<Value> {
    const auto at = reinterpret_cast<std::uintptr_t>(&value);
    const auto look = [&](const auto &block,
                          const auto &fresh_block) -> std::optional<Value> {
      const auto begin = reinterpret_cast<std::uintptr_t>(&block);
      if (at < begin || at + sizeof(Value) > begin + sizeof(block)) {
        return std::nullopt;
      }
      Value out{};
      std::memcpy(&out,
                  reinterpret_cast<const unsigned char *>(&fresh_block) +
                      (at - begin),
                  sizeof(Value));
      return out;
    };
    if (auto found = look(renderer, fresh.renderer)) return found;
    if (auto found = look(values.settings, fresh.settings)) return found;
    return look(environment, fresh.environment);
  };
  // The reset arrow at the end of a label while its value is not the
  // default; true on the frame it is pressed.
  const auto reset = [&](std::string_view label, const gui::Rect &field,
                         double value, double fallback, int precision) {
    if (std::abs(value - fallback) <= 1.0e-6 * std::max(1.0, std::abs(fallback)) ||
        field.x - label_width < bounds.x) {
      return false;
    }
    std::array<char, 48U> tip{};
    std::snprintf(tip.data(), tip.size(), "Reset to default (%.*f)",
                  std::max(precision, 0), fallback);
    return reset_button(ctx, id(label) ^ 0x5E5E7ULL,
                        gui::Rect{field.x - reset_button_width - 2.0F, field.y,
                                  reset_button_width, field.height},
                        tip.data());
  };
  const auto number = [&](std::string_view label, auto &value, double minimum,
                          double max_value, double speed = 0.01,
                          std::string_view unit = "", int precision = 3) {
    double edited{};
    const gui::Rect field = well(label);
    if (fields_.draw(ctx, id(label), field, unit,
                     static_cast<double>(value), speed, edited, precision) &&
        std::isfinite(edited)) {
      value = static_cast<std::remove_reference_t<decltype(value)>>(
          std::clamp(edited, minimum, max_value));
      changed = true;
    }
    if (const auto fallback = default_of(value)) {
      if (reset(label, field, static_cast<double>(value),
                static_cast<double>(*fallback), precision)) {
        value = *fallback;
        changed = true;
      }
    }
  };
  const auto file = [&](std::string_view label, std::string &value, FilePickerKind kind) {
    ctx.label(form.control(), label, ctx.theme().text_muted);
    gui::Rect path = form.control();
    constexpr float browse_width = 70.0F;
    const gui::Rect browse = gui::cut_right(path, browse_width);
    static_cast<void>(gui::cut_right(path, m.gap));
    if (ctx.text_field(id(label), path, value) && kind != FilePickerKind::BakeFolder)
      changed = true;
    ctx.tooltip(path, value.empty() ? "Choose a file or enter a path" : value);
    if (ctx.button(id(std::string{label} + ".browse"), browse, "Browse..."))
      result.file_picker = FilePickerRequest{kind, value, {}};
  };
  const auto object_choice = [&](std::string_view label, doc::NodeId &value) {
    std::vector<doc::NodeId> nodes{{}};
    std::vector<std::string> names{"None"};
    std::size_t selected = 0U;
    for (const auto &element : elements) {
      if (element.kind != ElementKind::Object &&
          element.kind != ElementKind::Model &&
          element.kind != ElementKind::Media)
        continue;
      nodes.push_back(element.node);
      names.push_back(element.display_name.empty()
                          ? std::string{kind_name(element.kind)} + " " +
                                std::to_string(element.node.value())
                          : element.display_name);
      if (element.node == value)
        selected = nodes.size() - 1U;
    }
    if (value != doc::NodeId{} && selected == 0U) {
      selected = nodes.size();
      nodes.push_back(value);
      names.push_back("Missing object");
    }
    std::vector<std::string_view> options;
    options.reserve(names.size());
    for (const auto &name : names)
      options.push_back(name);
    if (ctx.dropdown(id(label), well(label), options, selected)) {
      value = nodes[selected];
      changed = true;
    }
  };
  const auto environment_number = [&](std::string_view label,
                                      scene::PropertyChannel property,
                                      double minimum, double max_value,
                                      double speed = 0.01,
                                      std::string_view unit = "") {
    const auto channel =
        std::find_if(keyframes.begin(), keyframes.end(), [&](const auto &c) {
          return c.target == state.stage_node && c.component == property;
        });
    const bool animated =
        channel != keyframes.end() && !channel->frames.empty();
    const bool keyed_here =
        animated &&
        show_frame <=
            static_cast<core::u64>(std::numeric_limits<core::i64>::max()) &&
        std::find(channel->frames.begin(), channel->frames.end(),
                  static_cast<core::i64>(show_frame)) != channel->frames.end();
    double value = scene::environment_channel_value(environment, property);
    if (animated)
      value = sample(*channel, show_frame, value);
    value = std::clamp(value, minimum, max_value);
    gui::Rect field = well(label);
    constexpr float key_width = 20.0F;
    const gui::Rect key = gui::cut_right(field, key_width);
    static_cast<void>(gui::cut_right(field, 2.0F));
    double edited{};
    if (fields_.draw(ctx, id(label), field, unit, value, speed, edited, 3) &&
        std::isfinite(edited)) {
      value = std::clamp(edited, minimum, max_value);
      if (animated && auto_key)
        result.keys.push_back({property, value, false});
      else {
        scene::set_environment_channel(environment, property,
                                       static_cast<float>(value));
        changed = true;
      }
    }
    const double fallback = std::clamp(
        static_cast<double>(
            scene::environment_channel_value(fresh.environment, property)),
        minimum, max_value);
    if (reset(label, field, value, fallback, 3)) {
      if (animated && auto_key)
        result.keys.push_back({property, fallback, false});
      else {
        scene::set_environment_channel(environment, property,
                                       static_cast<float>(fallback));
        changed = true;
      }
    }
    if (key_button(ctx, id(label) ^ 0xD1AULL, key, animated, keyed_here,
                   ctx.theme().panel) &&
        state.stage_node != doc::NodeId{})
      result.keys.push_back({property, value, keyed_here});
  };
  if (target == SceneSettingsTarget::Renderer) {
    if (section(0, "RENDERING")) {
      if (state.program_renderer_differs)
        ctx.label(form.control(), "Program keeps its published settings until PUBLISH",
                  ctx.theme().text_muted);
      choice("Backend", renderer.backend, {"Basic", "Advanced"});
      if (renderer.backend == scene::RendererBackend::Advanced) {
        check("Require Advanced", renderer.required);
        choice("Quality", renderer.quality,
               {"Interactive", "Balanced", "Cinematic"});
        if (renderer.ray_tracing || renderer.ddgi || renderer.restir ||
            renderer.denoising) {
          ctx.label(form.control(), "Unsupported passes requested", ctx.theme().warning);
          if (ctx.button(id("raster only"), form.control(),
                         "Clear unsupported passes")) {
            renderer.ray_tracing = false;
            renderer.ddgi = false;
            renderer.restir = false;
            renderer.denoising = false;
            changed = true;
          }
        }
      }
      number("Frame budget", renderer.frame_budget_us, 0, 1000000, 100, "us",
             0);
    }
    if (renderer.backend == scene::RendererBackend::Advanced) {
      if (section(1, "IMAGE")) {
        std::size_t aa = renderer.taau                   ? 2U
                         : renderer.spatial_antialiasing ? 1U
                                                         : 0U;
        const auto previous = aa;
        choice("Antialiasing", aa, {"None", "Spatial", "Temporal"});
        if (aa != previous) {
          renderer.taau = aa == 2U;
          renderer.spatial_antialiasing = aa == 1U;
        }
        if (renderer.taau)
          number("Jitter samples", renderer.taa_sequence_length, 1, 64, 1, "",
                 0);
        check("Motion blur", renderer.motion_blur);
        if (renderer.motion_blur)
          number("Blur radius", renderer.motion_blur_radius, 0, 64, 1, "px", 0);
        check("Plate depth of field", renderer.plate_depth_of_field);
        if (renderer.plate_depth_of_field)
          number("Defocus radius", renderer.plate_dof_radius, 0, 64, 1, "px",
                 0);
        number("Bloom", renderer.bloom_strength, 0, 8);
        number("Occlusion radius", renderer.occlusion_radius, 0, 10, 0.01, "m");
        if (renderer.occlusion_radius > 0)
          number("Occlusion strength", renderer.occlusion_strength, 0, 4);
        number("Indirect light", renderer.gi_strength, 0, 8);
        number("Photometric scale", renderer.photometric_scale, 0, 1, 0.0001,
               "", 6);
      }
      if (section(2, "OUTPUT COLOR")) {
        choice("Display transform", renderer.color.display_transform,
               {"sRGB", "ACES Approximate", "ACES 1.3"});
        file("Show LUT", renderer.color.show_lut_source, FilePickerKind::ShowLut);
        check("Dither", renderer.color.dither);
        if (state.color_status != ColorStatus::Ready &&
            !renderer.color.show_lut_source.empty())
          ctx.label(form.control(), color_status_text(state.color_status),
                    ctx.theme().warning);
      }
      if (section(3, "SHADOWS")) {
        number("Cascades", renderer.shadow_cascades, 0, 4, 1, "", 0);
        number("Cascade blend", renderer.shadow_blend_band, 0, 0.25);
        number("Filter radius", renderer.shadow_filter_radius, 0, 4, 0.1, "px");
        number("Contact length", renderer.contact_shadow_length, 0, 10, 0.01,
               "m");
        if (renderer.contact_shadow_length > 0)
          number("Contact thickness", renderer.contact_shadow_thickness, 0, 1,
                 0.001, "m");
      }
    }
    if (section(4, "GEOMETRY & TEXTURES")) {
      check("Frustum culling", renderer.frustum_culling);
      check("Instancing", renderer.geometry_instancing);
      check("Model LOD", renderer.model_lod);
      check("Texture streaming", renderer.texture_streaming);
      if (renderer.texture_streaming) {
        number("Texture budget", renderer.texture_budget_mib, 16,
               scene::max_texture_budget_mib, 16, "MiB", 0);
        number("Mip bias", renderer.texture_mip_bias, 0, 16, 1, "", 0);
      }
      std::size_t anisotropy = 0;
      for (core::u32 n = renderer.texture_anisotropy; n > 1U; n /= 2U)
        ++anisotropy;
      const auto previous = anisotropy;
      choice("Anisotropy", anisotropy, {"1x", "2x", "4x", "8x", "16x"});
      if (previous != anisotropy)
        renderer.texture_anisotropy = 1U << anisotropy;
      number("Texture LOD bias", renderer.texture_lod_bias, -4, 4);
      number("Slice apron", renderer.slice_apron_limit, 0, 4096, 1, "px", 0);
    }
    if (section(5, "EFFECT DEFAULTS")) {
      number("Gaussian taps", values.settings.blur_taps, 3, 63, 2, "", 0);
      number("Gaussian sigma", values.settings.blur_sigma_ratio, 0.01, 1);
      number("Gaussian passes", values.settings.blur_passes, 1, 8, 1, "", 0);
      number("Directional taps", values.settings.directional_blur_taps, 3, 63,
             2, "", 0);
      choice("Brightness curve", values.settings.brightness_curve,
             {"Exposure", "Gain"});
      number("Brightness gamma", values.settings.brightness_gamma, 0.1, 4);
      number("Graph texture budget", values.settings.graph_texture_budget_gb,
             scene::min_graph_texture_budget_gb,
             scene::max_graph_texture_budget_gb, 0.1, "GB", 1);
    }
  } else {
    if (section(6, "SKY & LIGHTING")) {
      check("Physical sky", environment.physical_sky.enabled);
      if (environment.physical_sky.enabled) {
        environment_number("Solar time", scene::PropertyChannel::SkyTimeOfDay,
                           0, 24, 0.05, "h");
        number("Latitude", environment.physical_sky.latitude, -89, 89, 0.1,
               "deg");
        number("North rotation", environment.physical_sky.north_rotation, -1, 1,
               0.001, "turns");
        environment_number("Haze", scene::PropertyChannel::SkyHaze, 0, 8);
        environment_number("Sun illuminance",
                           scene::PropertyChannel::SkyIlluminance, 0, 200000,
                           100, "lux");
        float degrees = environment.physical_sky.angular_radius * 360.0F;
        number("Sun radius", degrees, 0.018, 9, 0.01, "deg");
        environment.physical_sky.angular_radius = degrees / 360.0F;
        number("Ground albedo", environment.physical_sky.ground_albedo, 0, 1);
      } else {
        file("Environment map", environment.map_source, FilePickerKind::EnvironmentMap);
        if (!environment.map_source.empty()) {
          environment_number("Intensity",
                             scene::PropertyChannel::EnvironmentIntensity, 0,
                             100000);
          environment_number("Rotation",
                             scene::PropertyChannel::EnvironmentRotation,
                             -10000, 10000, 0.001, "turns");
          if (state.environment_status != EnvironmentStatus::Ready &&
              state.environment_status != EnvironmentStatus::NotAuthored)
            ctx.label(form.control(), environment_status_text(state.environment_status),
                      ctx.theme().warning);
        }
      }
    }
    if (section(7, "ATMOSPHERE")) {
      environment_number("Fog density", scene::PropertyChannel::FogDensity, 0,
                         scene::max_fog_density, 0.001);
      environment_number("Height", scene::PropertyChannel::FogHeight,
                         -scene::max_fog_height, scene::max_fog_height, 0.01,
                         "m");
      environment_number("Height falloff", scene::PropertyChannel::FogFalloff,
                         0, scene::max_fog_height_falloff);
      environment_number("Anisotropy", scene::PropertyChannel::FogAnisotropy,
                         -scene::max_fog_anisotropy, scene::max_fog_anisotropy);
      number("Background depth", environment.fog_background_distance, 0,
             scene::max_fog_background_distance, 0.1, "m");
      number("Fog red", environment.fog_color.x, 0, 1);
      number("Fog green", environment.fog_color.y, 0, 1);
      number("Fog blue", environment.fog_color.z, 0, 1);
    }
    if (section(8, "BAKED LIGHTING")) {
      check("Baked lighting", environment.baked.enabled);
      number("Bake intensity", environment.baked.strength, 0, 16);
      number("Samples", environment.baked.samples, 64, 4096, 64, "", 0);
      number("Bounces", environment.baked.bounces, 1, 16, 1, "", 0);
      number("Grid X", environment.baked.count_x, 1, 16, 1, "", 0);
      number("Grid Y", environment.baked.count_y, 1, 16, 1, "", 0);
      number("Grid Z", environment.baked.count_z, 1, 16, 1, "", 0);
      number("Minimum X", environment.baked.minimum.x, -10000, 10000);
      number("Minimum Y", environment.baked.minimum.y, -10000, 10000);
      number("Minimum Z", environment.baked.minimum.z, -10000, 10000);
      number("Maximum X", environment.baked.maximum.x, -10000, 10000);
      number("Maximum Y", environment.baked.maximum.y, -10000, 10000);
      number("Maximum Z", environment.baked.maximum.z, -10000, 10000);
      file("Bake folder", bake_destination_, FilePickerKind::BakeFolder);
      if (!state.bake_status.empty()) {
        auto remaining=std::string_view{state.bake_status};
        const auto columns=static_cast<std::size_t>(std::max(16.0F,form.width()/8.0F));
        while (!remaining.empty()) {
          auto count=std::min(columns,remaining.size());
          if (count<remaining.size()) { const auto space=remaining.rfind(' ',count); if (space!=std::string_view::npos && space>0) count=space; }
          ctx.label(form.control(),remaining.substr(0,count),state.bake_status.starts_with("STALE") ? ctx.theme().warning : ctx.theme().text_muted);
          remaining.remove_prefix(count); while (remaining.starts_with(' ')) remaining.remove_prefix(1);
        }
      }
      if (state.bake_running) {
        if (ctx.button(id("cancel bake"), form.control(), "Cancel bake"))
          result.project.cancel_bake = true;
      } else {
        const bool ready = !bake_destination_.empty() &&
                           state.stage_node != doc::NodeId{} &&
                           renderer.backend == scene::RendererBackend::Advanced;
        const auto action = form.control();
        if (ready && ctx.button(id("bake"), action, "Bake lighting")) {
          result.project.bake = scene::clamped(environment.baked);
          result.project.bake_destination = bake_destination_;
        } else if (!ready) {
          ctx.label(action,
                    renderer.backend == scene::RendererBackend::Basic
                        ? "Advanced renderer required for baking"
                        : "Enter a bake path",
                    ctx.theme().text_muted);
        }
      }
    }
    if (section(9, "REFLECTIONS")) {
      auto &planar = environment.planar;
      check("Planar reflection", planar.enabled);
      if (planar.enabled) {
        object_choice("Receiver", planar.receiver);
        number("Plane X", planar.point.x, -10000, 10000);
        number("Plane Y", planar.point.y, -10000, 10000);
        number("Plane Z", planar.point.z, -10000, 10000);
        number("Normal X", planar.normal.x, -1, 1);
        number("Normal Y", planar.normal.y, -1, 1);
        number("Normal Z", planar.normal.z, -1, 1);
        number("Reflection strength", planar.strength, 0, 1);
      }
      for (std::size_t i = 0; i < environment.baked.reflections.size(); ++i) {
        auto &probe = environment.baked.reflections[i];
        const auto prefix = "Probe " + std::to_string(i + 1U) + " ";
        check(prefix + "enabled", probe.enabled);
        if (!probe.enabled)
          continue;
        object_choice(prefix + "object", probe.source);
        check(prefix + "sphere", probe.sphere);
        number(prefix + "X", probe.position.x, -10000, 10000);
        number(prefix + "Y", probe.position.y, -10000, 10000);
        number(prefix + "Z", probe.position.z, -10000, 10000);
        number(prefix + "extent X", probe.extent.x, 0.001, 10000);
        number(prefix + "extent Y", probe.extent.y, 0.001, 10000);
        number(prefix + "extent Z", probe.extent.z, 0.001, 10000);
        number(prefix + "blend", probe.blend, 0.001, 1);
        number(prefix + "intensity", probe.intensity, 0, 16);
      }
    }
  }
  scroll_.end(ctx, form.extent());
  // The bar runs down the gutter, 2px in from the panel's edge.
  const gui::Rect thumb = scroll_.thumb(gui::Rect{
      bounds.right() - scroll_bar_width - 2.0F, body.y, scroll_bar_width,
      body.height});
  if (thumb.height > 0.0F)
    ctx.panel(thumb, ctx.theme().text_muted);
  ctx.pop_clip();
  if (changed) {
    values.renderer = scene::clamped(values.renderer);
    values.settings = scene::clamped(values.settings);
    values.environment.physical_sky =
        scene::clamped(values.environment.physical_sky);
    values.environment.baked = scene::clamped(values.environment.baked);
    values.environment.planar = scene::clamped(values.environment.planar);
    result.project.apply = std::move(values);
  }
  return result;
}
} // namespace mediaserver::editor
