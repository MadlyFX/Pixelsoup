#include <mediaserver/editor/workspace_panels.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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
    scroll_ = 0.0F;
    content_height_ = 0.0F;
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
  const gui::Rect header{bounds.x + 10.0F, bounds.y + 6.0F,
                         bounds.width - 20.0F, 30.0F};
  ctx.label(header, target == SceneSettingsTarget::Renderer ? "Renderer"
                                                            : "Environment");
  ctx.panel({bounds.x, header.y + header.height + 4.0F, bounds.width, 1.0F},
            ctx.theme().divider);
  const gui::Rect body{bounds.x + 10.0F, header.y + header.height + 10.0F,
                       std::max(bounds.width - 24.0F, 1.0F),
                       std::max(bounds.height - 50.0F, 1.0F)};
  const float maximum = std::max(content_height_ - body.height, 0.0F);
  if (body.contains(ctx.pointer_x(), ctx.pointer_y()))
    scroll_ -= ctx.wheel() * 36.0F;
  scroll_ = std::clamp(scroll_, 0.0F, maximum);
  ctx.push_clip(body);
  float y = body.y - scroll_;
  const float label_width = std::clamp(body.width * 0.44F, 80.0F, 150.0F);
  const auto id = [&](std::string_view name) {
    return gui::ui_id(name) ^ 0x5CE0E5E77ULL ^ state.stage_node.value();
  };
  const auto row = [&] {
    const gui::Rect r{body.x, y, body.width, 26.0F};
    y += 32.0F;
    return r;
  };
  const auto well = [&](const gui::Rect &r, std::string_view label) {
    if (ctx.font_width(label) > label_width - 10.0F) {
      ctx.label(r, label, ctx.theme().text_muted);
      const auto field = row();
      return field;
    }
    ctx.label({r.x, r.y, label_width - 5.0F, r.height}, label,
              ctx.theme().text_muted);
    return gui::Rect{r.x + label_width, r.y,
                     std::max(r.width - label_width, 1.0F), r.height};
  };
  const auto section = [&](std::size_t index, std::string_view label) {
    y += 8.0F;
    const auto r = row();
    const auto interaction = ctx.grab(id(label), r);
    ctx.panel(r, ctx.theme().header);
    ctx.label({r.x + 7.0F, r.y, 12.0F, r.height}, sections_[index] ? "v" : ">");
    std::string title{label};
    for (std::size_t i = 1U; i < title.size(); ++i)
      if (title[i] >= 'A' && title[i] <= 'Z') title[i] += 'a' - 'A';
    ctx.label({r.x + 23.0F, r.y, r.width - 27.0F, r.height}, title);
    if (interaction.clicked)
      sections_[index] = !sections_[index];
    return sections_[index];
  };
  const auto check = [&](std::string_view label, bool &value) {
    const auto r = row();
    if (ctx.checkbox(id(label), well(r, label), value, ""))
      changed = true;
  };
  const auto choice = [&](std::string_view label, auto &value,
                          std::initializer_list<std::string_view> options) {
    const auto r = row();
    auto selected = static_cast<std::size_t>(value);
    if (ctx.dropdown(id(label), well(r, label),
                     std::span{options.begin(), options.size()}, selected)) {
      value = static_cast<std::remove_reference_t<decltype(value)>>(selected);
      changed = true;
    }
  };
  const auto number = [&](std::string_view label, auto &value, double minimum,
                          double max_value, double speed = 0.01,
                          std::string_view unit = "", int precision = 3) {
    const auto r = row();
    double edited{};
    if (fields_.draw(ctx, id(label), well(r, label), unit,
                     static_cast<double>(value), speed, edited, precision) &&
        std::isfinite(edited)) {
      value = static_cast<std::remove_reference_t<decltype(value)>>(
          std::clamp(edited, minimum, max_value));
      changed = true;
    }
  };
  const auto file = [&](std::string_view label, std::string &value, FilePickerKind kind) {
    ctx.label(row(), label, ctx.theme().text_muted);
    const auto r = row();
    constexpr float browse_width = 70.0F;
    const gui::Rect path{r.x, r.y, std::max(r.width - browse_width - 6.0F, 1.0F), r.height};
    if (ctx.text_field(id(label), path, value) && kind != FilePickerKind::BakeFolder)
      changed = true;
    ctx.tooltip(path, value.empty() ? "Choose a file or enter a path" : value);
    if (ctx.button(id(std::string{label} + ".browse"),
                   {r.x + r.width - browse_width, r.y, browse_width, r.height}, "Browse..."))
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
    if (ctx.dropdown(id(label), well(row(), label), options, selected)) {
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
    const auto r = row();
    auto field = well(r, label);
    field.width = std::max(field.width - 22.0F, 1.0F);
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
    const gui::Rect key{field.x + field.width + 2.0F, field.y, 20.0F, field.height};
    const auto interaction = ctx.grab(id(label) ^ 0xD1AULL, key);
    const float cx = key.x + 10.0F, cy = key.y + 11.0F;
    const auto color = interaction.hovered ? ctx.theme().accent
                       : keyed_here        ? ctx.theme().playhead
                       : animated          ? ctx.theme().keyframe
                                           : ctx.theme().text_muted;
    const std::array<gui::UiPoint, 4> diamond{
        {{cx, cy - 4.5F}, {cx + 4.5F, cy}, {cx, cy + 4.5F}, {cx - 4.5F, cy}}};
    ctx.draw_list().add_convex_poly(diamond, color);
    if (!keyed_here) {
      const std::array<gui::UiPoint, 4> inside{
          {{cx, cy - 2.5F}, {cx + 2.5F, cy}, {cx, cy + 2.5F}, {cx - 2.5F, cy}}};
      ctx.draw_list().add_convex_poly(inside, ctx.theme().panel);
    }
    if (interaction.clicked && state.stage_node != doc::NodeId{})
      result.keys.push_back({property, value, keyed_here});
  };
  if (target == SceneSettingsTarget::Renderer) {
    if (section(0, "RENDERING")) {
      if (state.program_renderer_differs)
        ctx.label(row(), "Program keeps its published settings until PUBLISH",
                  ctx.theme().text_muted);
      choice("Backend", renderer.backend, {"Basic", "Advanced"});
      if (renderer.backend == scene::RendererBackend::Advanced) {
        check("Require Advanced", renderer.required);
        choice("Quality", renderer.quality,
               {"Interactive", "Balanced", "Cinematic"});
        if (renderer.ray_tracing || renderer.ddgi || renderer.restir ||
            renderer.denoising) {
          ctx.label(row(), "Unsupported passes requested", ctx.theme().warning);
          if (ctx.button(id("raster only"), row(),
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
          ctx.label(row(), color_status_text(state.color_status),
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
            ctx.label(row(), environment_status_text(state.environment_status),
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
        const auto columns=static_cast<std::size_t>(std::max(16.0F,(bounds.width-24.0F)/8.0F));
        while (!remaining.empty()) {
          auto count=std::min(columns,remaining.size());
          if (count<remaining.size()) { const auto space=remaining.rfind(' ',count); if (space!=std::string_view::npos && space>0) count=space; }
          ctx.label(row(),remaining.substr(0,count),state.bake_status.starts_with("STALE") ? ctx.theme().warning : ctx.theme().text_muted);
          remaining.remove_prefix(count); while (remaining.starts_with(' ')) remaining.remove_prefix(1);
        }
      }
      if (state.bake_running) {
        if (ctx.button(id("cancel bake"), row(), "Cancel bake"))
          result.project.cancel_bake = true;
      } else {
        const bool ready = !bake_destination_.empty() &&
                           state.stage_node != doc::NodeId{} &&
                           renderer.backend == scene::RendererBackend::Advanced;
        const auto action = row();
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
  content_height_ = y + scroll_ - body.y + 6.0F;
  ctx.pop_clip();
  if (maximum > 0) {
    const float thumb = body.height * body.height / content_height_;
    ctx.panel({bounds.x + bounds.width - 5.0F,
               body.y + scroll_ / maximum * (body.height - thumb), 3.0F, thumb},
              ctx.theme().text_muted);
  }
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
