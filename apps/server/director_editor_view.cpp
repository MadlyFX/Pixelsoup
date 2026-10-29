#include "director_editor_view.hpp"
#include "director_show_loop.hpp"

#include "blender_links.hpp"
#include "cluster_observability.hpp"
#include "director_live_data.hpp"
#include "director_project_load.hpp"
#include "edit_coalescing_policy.hpp"
#include "playback_media.hpp"
#include "playback_runtime.hpp"
#include "proxy_generation_service.hpp"
#include "renderer_lockstep.hpp"
#include "thumbnail_service.hpp"
#include "timeline_playback_cache.hpp"

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/assets/derivative_cache.hpp>
#include <mediaserver/assets/derivative_profile_id.hpp>
#include <mediaserver/cmd/command.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/core/asset_publication.hpp>
#include <mediaserver/core/render_conformance.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/live_data_panel.hpp>
#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/editor/workspace_panels.hpp>
#include <mediaserver/net/cluster_discovery.hpp>
#include <mediaserver/net/cluster_lifecycle.hpp>
#include <mediaserver/net/session_protocol.hpp>
#include <mediaserver/persist/settings.hpp>
#include <mediaserver/render/blender_discovery.hpp>
#include <mediaserver/render/environment_cache.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>
#include <mediaserver/render/media_source.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/linked_publication.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/manipulate.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/scene/selection_set.hpp>
#include <mediaserver/show/show_clock.hpp>
#include <mediaserver/show/switcher_state.hpp>
#include <mediaserver/show/transport_authority.hpp>
#include <mediaserver/sync/sync_group.hpp>
#include <mediaserver/sync/sync_policy.hpp>
#include <mediaserver/sync/sync_status.hpp>
#include <mediaserver/topology/hardware_runtime.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace mediaserver::app {

[[nodiscard]] double read_transform(const doc::Node &node,
                                    std::string_view name,
                                    double fallback) noexcept {
    const auto found = node.properties.find(name);
    if (found == node.properties.end()) {
        return fallback;
    }
    if (const auto *const real = std::get_if<double>(&found->second)) {
        return *real;
    }
    if (const auto *const whole = std::get_if<std::int64_t>(&found->second)) {
        return static_cast<double>(*whole);
    }
    if (const auto* flag = std::get_if<bool>(&found->second)) return *flag ? 1.0 : 0.0;
    return fallback;
}

// The authored transform of each scene element, for the editor's Inspector.
// Read from the projected document (the base values, before channels), so the
// Inspector shows and edits what an edit changes rather than the animated
// result.
[[nodiscard]] std::vector<editor::ElementInfo> scene_elements(
    const cmd::DirectorScheduler &scheduler, const scene::Scene &scene,
    std::span<const scene::ClipPlacement> clips, core::u64 show_frame,
    std::span<const scene::TimelineLane> lanes,
    std::span<const scene::CameraPlacement> cameras,
    std::span<const scene::TrackingSource> tracking_sources,
    const scene::LinkedSceneState* linked) {
    std::vector<editor::ElementInfo> elements;
    const auto read = [&scheduler,linked](doc::NodeId node, editor::ElementKind kind) {
        editor::ElementInfo info{};
        info.node = node;
        info.kind = kind;
        const auto* doc_node = scheduler.projected_document().get(node);
        doc::Node effective;
        if (doc_node && linked) if (const auto p=linked->occurrences.find(node); p!=linked->occurrences.end() && !p->second.light.empty()) {
            effective=*doc_node;
            for (const auto& [key,value] : p->second.light) effective.properties[key]=value;
            doc_node=&effective;
        }
        if (doc_node == nullptr) {
            return info;
        }
        const auto number = [doc_node](std::string_view name, double fallback) {
            return static_cast<float>(
                read_transform(*doc_node, name, fallback));
        };
        const auto text_of = [](const doc::Node* source,
                               std::string_view name,
                               std::string_view fallback) {
            const auto found = source->properties.find(name);
            if (found == source->properties.end()) {
                return fallback;
            }
            const auto* const value =
                std::get_if<std::string>(&found->second);
            return value == nullptr ? fallback : std::string_view{*value};
        };
        const auto boolean = [doc_node](std::string_view name, bool fallback) {
            const auto found = doc_node->properties.find(name);
            if (found == doc_node->properties.end()) {
                return fallback;
            }
            if (const auto *const value = std::get_if<bool>(&found->second)) {
                return *value;
            }
            if (const auto *const value = std::get_if<double>(&found->second)) {
                return std::isfinite(*value) ? *value >= 0.5 : fallback;
            }
            if (const auto *const value =
                    std::get_if<std::int64_t>(&found->second)) {
                return *value != 0;
            }
            return fallback;
        };
        info.locked = boolean(scene::schema::locked, false);
        info.position = scene::Vec3{number(scene::schema::position_x, 0.0),
                                    number(scene::schema::position_y, 0.0),
                                    number(scene::schema::position_z, 0.0)};
        info.rotation = scene::Vec3{number(scene::schema::rotation_yaw, 0.0),
                                    number(scene::schema::rotation_pitch, 0.0),
                                    number(scene::schema::rotation_roll, 0.0)};
        info.scale = scene::Vec3{number(scene::schema::scale_x, 1.0),
                                 number(scene::schema::scale_y, 1.0),
                                 number(scene::schema::scale_z, 1.0)};
        info.color = scene::Vec3{number(scene::schema::color_r, 1.0),
                                 number(scene::schema::color_g, 1.0),
                                 number(scene::schema::color_b, 1.0)};
        info.intensity = number(scene::schema::intensity, 1.0);
        info.color_temperature_kelvin = std::clamp(number(scene::schema::color_temperature_kelvin, 6500.0), 1000.0F, 40000.0F);
        info.angular_radius = number(scene::schema::angular_radius_turns, 0.0);
        info.ies_source = text_of(doc_node, scene::schema::ies_source, "");
        info.ies_rotation = number(scene::schema::ies_rotation_turns, 0.0);
        info.light_width = number(scene::schema::light_width, 1.0);
        info.light_height = number(scene::schema::light_height, 1.0);
        info.light_radius = number(scene::schema::light_radius, 0.5);
        info.area_rotation = number(scene::schema::area_rotation_turns, 0.0);
        info.two_sided = boolean(scene::schema::two_sided, false);
        info.use_color_temperature = boolean(scene::schema::use_color_temperature, false);
        const auto unit = text_of(doc_node, scene::schema::light_unit, scene::schema::unitless_unit);
        info.light_unit = unit == scene::schema::lumens_unit ? scene::LightUnit::Lumens
            : unit == scene::schema::candela_unit ? scene::LightUnit::Candela
            : unit == scene::schema::lux_unit ? scene::LightUnit::Lux
            : unit == scene::schema::nits_unit ? scene::LightUnit::Nits : scene::LightUnit::Unitless;
        info.direction = scene::Vec3{number(scene::schema::direction_x, 0.0),
                                     number(scene::schema::direction_y, -1.0),
                                     number(scene::schema::direction_z, 0.0)};
        info.bake_static = boolean(scene::schema::bake_static, true);
        info.animation.enabled = boolean(scene::schema::animation_enabled,true);
        info.animation.loop = boolean(scene::schema::animation_loop,true);
        info.animation.clip = static_cast<core::u32>(std::clamp(number(scene::schema::animation_clip,0),0.0F,63.0F));
        info.animation.wind = {number(scene::schema::wind_x,0),number(scene::schema::wind_y,0),number(scene::schema::wind_z,0)};
        info.animation.wind_period_frames = static_cast<core::u32>(std::clamp(number(scene::schema::wind_period_frames,120),2.0F,1000000.0F));
        if (const auto found=doc_node->properties.find(std::string{scene::schema::animation_start_frame}); found!=doc_node->properties.end())
            if (const auto* value=std::get_if<std::int64_t>(&found->second)) info.animation.start_frame=static_cast<core::u64>(std::max<std::int64_t>(*value,0));
        info.bake_indirect = boolean(scene::schema::bake_indirect, true);
        info.range = number(scene::schema::range, 10.0);
        {
            const std::string_view light_flavour =
                text_of(doc_node, scene::schema::light_kind, "point");
            info.local_kind = light_flavour == scene::schema::spot_kind
                ? scene::LocalLightKind::Spot
                : (light_flavour == scene::schema::rectangle_kind
                       ? scene::LocalLightKind::Rectangle
                       : light_flavour == scene::schema::sphere_kind ? scene::LocalLightKind::Sphere : scene::LocalLightKind::Point);
            info.outer_cone = std::clamp(
                number(scene::schema::outer_cone_turns, 0.125), 0.001F, 0.249F);
            info.inner_cone = std::clamp(
                number(scene::schema::inner_cone_turns, 0.0625), 0.0F, info.outer_cone);
            info.casts_shadow =
                boolean(scene::schema::casts_shadow, false);
            const std::string_view tier =
                text_of(doc_node, scene::schema::shadow_tier,
                        scene::schema::medium_tier);
            info.shadow_tier = tier == scene::schema::small_tier
                ? scene::ShadowTier::Small
                : (tier == scene::schema::large_tier
                       ? scene::ShadowTier::Large
                       : scene::ShadowTier::Medium);
        }
        info.material.base_color = info.color;
        info.material.base_color_texture = std::string{text_of(
            doc_node, scene::schema::base_color_texture, "")};
        info.material.roughness = number(scene::schema::roughness, 0.5);
        info.material.metallic = number(scene::schema::metallic, 0.0);
        info.material.emissive =
            scene::Vec3{number(scene::schema::emissive_r, 0.0),
                        number(scene::schema::emissive_g, 0.0),
                        number(scene::schema::emissive_b, 0.0)};
        info.material.base_alpha = std::clamp(number(scene::schema::base_alpha, 1.0), 0.0F, 1.0F);
        info.material.clearcoat = std::clamp(number(scene::schema::clearcoat, 0.0), 0.0F, 1.0F);
        info.material.clearcoat_roughness = std::clamp(number(scene::schema::clearcoat_roughness, 0.0), 0.0F, 1.0F);
        info.material.clearcoat_normal_scale = std::clamp(number(scene::schema::clearcoat_normal_scale, 1.0), 0.0F, 4.0F);
        info.material.sheen_roughness = std::clamp(number(scene::schema::sheen_roughness, 0.0), 0.0F, 1.0F);
        info.material.transmission = std::clamp(number(scene::schema::transmission, 0.0), 0.0F, 1.0F);
        info.material.ior = std::clamp(number(scene::schema::ior, 1.5), 1.0F, 3.0F);
        info.material.specular = std::clamp(number(scene::schema::specular, 1.0), 0.0F, 1.0F);
        info.material.anisotropy_strength = std::clamp(number(scene::schema::anisotropy_strength, 0.0), 0.0F, 1.0F);
        info.material.anisotropy_rotation = std::clamp(number(scene::schema::anisotropy_rotation, 0.0), -6.283185307F, 6.283185307F);
        info.material.sheen_color.x = std::clamp(number(scene::schema::sheen_color_r, 0.0), 0.0F, 1.0F);
        info.material.sheen_color.y = std::clamp(number(scene::schema::sheen_color_g, 0.0), 0.0F, 1.0F);
        info.material.sheen_color.z = std::clamp(number(scene::schema::sheen_color_b, 0.0), 0.0F, 1.0F);
        info.material.specular_color.x = std::clamp(number(scene::schema::specular_color_r, 1.0), 0.0F, 1.0F);
        info.material.specular_color.y = std::clamp(number(scene::schema::specular_color_g, 1.0), 0.0F, 1.0F);
        info.material.specular_color.z = std::clamp(number(scene::schema::specular_color_b, 1.0), 0.0F, 1.0F);
        if (doc_node->type == scene::schema::object_type) {
            info.material.fill = scene::read_surface_fill(doc_node->properties);
        }
        info.style.opacity =
            std::clamp(number(scene::schema::opacity, 1.0), 0.0F, 1.0F);
        const int blend =
            static_cast<int>(number(scene::schema::blend_mode, 0.0));
        if (blend >= 0 && blend <= 3) {
            info.style.blend = static_cast<scene::BlendMode>(blend);
        }
        info.style.effects.brightness_enabled =
            boolean(scene::schema::brightness_enabled, false);
        info.style.effects.brightness = number(scene::schema::brightness, 1.0);
        info.style.effects.blur_enabled =
            boolean(scene::schema::blur_enabled, false);
        info.style.effects.blur_radius =
            number(scene::schema::blur_radius, 0.0);
        const auto text = [doc_node](std::string_view name) {
            const auto found = doc_node->properties.find(name);
            if (found == doc_node->properties.end()) {
                return std::string{};
            }
            if (const auto *const value =
                    std::get_if<std::string>(&found->second)) {
                return *value;
            }
            return std::string{};
        };
        const auto integer = [doc_node](std::string_view name,
                                        core::u32 fallback) {
            const auto found = doc_node->properties.find(name);
            if (found == doc_node->properties.end()) {
                return fallback;
            }
            if (const auto *const value =
                    std::get_if<std::int64_t>(&found->second);
                value != nullptr && *value >= 0 &&
                *value <= static_cast<std::int64_t>(UINT32_MAX)) {
                return static_cast<core::u32>(*value);
            }
            return fallback;
        };
        const auto signed_integer = [doc_node](std::string_view name,
                                               core::i64 fallback) {
            const auto found = doc_node->properties.find(name);
            if (found == doc_node->properties.end()) {
                return fallback;
            }
            if (const auto *const value =
                    std::get_if<std::int64_t>(&found->second)) {
                return static_cast<core::i64>(*value);
            }
            return fallback;
        };
        const auto optional_reference = [doc_node](std::string_view name) {
            const auto found = doc_node->properties.find(name);
            if (found == doc_node->properties.end()) {
                return std::optional<doc::NodeId>{};
            }
            if (const auto *const value =
                    std::get_if<std::int64_t>(&found->second);
                value != nullptr && *value > 0) {
                return std::optional<doc::NodeId>{
                    doc::NodeId{static_cast<core::u64>(*value)}};
            }
            return std::optional<doc::NodeId>{};
        };
        info.source = text(scene::schema::source);
        info.sequence_start = integer(scene::schema::sequence_start, 0U);
        info.sequence_count = integer(scene::schema::sequence_count, 1U);
        info.media_resolution =
            text(scene::schema::media_resolution) == scene::schema::media_resolution_half
            ? scene::MediaResolution::Half : scene::MediaResolution::Full;
        info.display_name = text(scene::schema::group_name);
        if (info.display_name.empty() && !info.source.empty() &&
            (kind == editor::ElementKind::Media ||
             kind == editor::ElementKind::Model)) {
            info.display_name = native_path_to_utf8(
                native_path_from_utf8(info.source).filename());
        }
        info.parent_group = optional_reference(scene::schema::parent_group);
        info.group_order = signed_integer(scene::schema::group_order, 0);
        // An index outside the palette is shown as no highlight rather than
        // wrapped onto a colour nobody picked.
        if (const auto highlight = signed_integer(scene::schema::outliner_color, -1);
            highlight >= 0 && highlight < scene::timeline_marker_colors) {
            info.highlight = static_cast<core::u8>(highlight);
        }
        info.visible = boolean(scene::schema::visible, true);
        info.effective_visible = info.visible;
        return info;
    };
    const auto apply_parent_state = [&scene](editor::ElementInfo &info) {
        if (!info.parent_group.has_value()) {
            return;
        }
        const auto parent =
            std::find_if(scene.groups.begin(), scene.groups.end(),
                         [&](const scene::SceneGroup &group) noexcept {
                             return group.node == *info.parent_group;
                         });
        if (parent == scene.groups.end()) {
            return;
        }
        info.locked = info.locked || parent->effective_locked;
        info.effective_visible =
            info.effective_visible && parent->effective_visible;
    };
    // Every authored clip placement, on air or not. A clip the playhead has
    // moved past is still the operator's to select, drag, and trim, so the
    // authored list rather than the evaluated Scene is what the panels get.
    // `clips` is empty for callers with no timeline, which keeps the original
    // evaluated-media behaviour.
    for (const auto &clip : clips) {
        auto info = read(clip.node, editor::ElementKind::Media);
        info.timing = clip.timing;
        info.source_frame_rate = clip.frame_rate;
        info.timeline_lane = clip.lane;
        info.clip_order = clip.order;
        if (const doc::Node *const plate =
                scheduler.projected_document().get(clip.node)) {
            const auto whole = [plate](std::string_view name) {
                const auto found = plate->properties.find(name);
                const auto *const value =
                    found != plate->properties.end()
                        ? std::get_if<std::int64_t>(&found->second)
                        : nullptr;
                return value != nullptr ? *value : std::int64_t{};
            };
            info.plateset = doc::NodeId{static_cast<core::u64>(
                std::max<std::int64_t>(whole(scene::schema::plateset_asset), 0))};
            info.plateset_take = whole(scene::schema::plateset_take);
            const auto slot =
                plate->properties.find(scene::schema::plateset_slot);
            if (slot != plate->properties.end()) {
                if (const auto *const key =
                        std::get_if<std::string>(&slot->second)) {
                    info.plateset_slot = *key;
                }
            }
        }
        info.parent_group = clip.parent_group;
        info.group_order = clip.group_order;
        info.locked = info.locked || clip.locked;
        const auto lane = std::find_if(lanes.begin(), lanes.end(),
                                       [&](const scene::TimelineLane &value) {
                                           return value.id == clip.lane;
                                       });
        if (lane != lanes.end()) {
            info.locked = info.locked || lane->locked;
            if (!lane->implicit) {
                info.screen_name = lane->name;
            }
        }
        const auto evaluated =
            std::find_if(scene.media.begin(), scene.media.end(),
                         [&clip](const scene::MediaInstance &value) noexcept {
                             return value.source_node == clip.node;
                         });
        info.on_air = evaluated != scene.media.end();
        if (info.on_air) {
            const float authored = info.style.opacity;
            info.style = evaluated->style;
            // A head or tail ramp belongs to the placement, not to the layer.
            // Leaving it folded into the opacity the Inspector shows would let
            // one nudge during a dissolve bake that dissolve into the authored
            // value, so it is divided back out. Where the ramp is fully out
            // there is nothing to divide by and the document's own value
            // stands.
            const float fade =
                clip.timing.placed()
                    ? scene::clip_fade(
                          clip.timing,
                          static_cast<core::i64>(std::min(
                              show_frame,
                              static_cast<core::u64>(
                                  std::numeric_limits<std::int64_t>::max()))))
                    : 1.0F;
            info.style.opacity =
                fade > 0.0F ? std::min(evaluated->style.opacity / fade, 1.0F)
                            : authored;
            info.locked = info.locked || evaluated->locked;
        }
        apply_parent_state(info);
        elements.push_back(std::move(info));
    }
    if (clips.empty()) {
        for (const auto &clip : scene.media) {
            auto info = read(clip.source_node, editor::ElementKind::Media);
            info.style = clip.style;
            info.locked = info.locked || clip.locked;
            info.parent_group = clip.parent_group;
            info.group_order = clip.group_order;
            apply_parent_state(info);
            elements.push_back(std::move(info));
        }
    }
    const std::size_t drawables_begin = elements.size();
    for (const auto &object : scene.instances) {
        auto info = read(object.source, editor::ElementKind::Object);
        info.style = object.style;
        info.locked = info.locked || object.locked;
        info.parent_group = object.parent_group;
        info.group_order = object.group_order;
        apply_parent_state(info);
        elements.push_back(std::move(info));
    }
    std::set<doc::NodeId> model_rows;
    for (const auto &model : scene.models) {
        if (linked && linked->occurrences.contains(model.source_node)) continue;
        if (!model_rows.insert(model.source_node).second) continue;
        auto info = read(model.source_node, editor::ElementKind::Model);
        info.style = model.style;
        info.locked = info.locked || model.locked;
        info.parent_group = model.parent_group;
        info.group_order = model.group_order;
        apply_parent_state(info);
        elements.push_back(std::move(info));
    }
    // The evaluated Scene holds only what draws, so a model or generator that
    // is hidden - itself or by a group above it - is listed from the document.
    // Otherwise hiding one would take its row, and with it the way to show it
    // again.
    {
        std::set<doc::NodeId> listed;
        for (const auto &element : elements) listed.insert(element.node);
        const doc::Delta contents = doc::diff(doc::Root{}, scheduler.projected_document());
        doc::NodeId default_scene{scene::legacy_scene_id};
        for (const auto &change : contents.changes) {
            if (change.after->type == scene::schema::stage_type) { default_scene = change.id; break; }
        }
        for (const auto &change : contents.changes) {
            const doc::Node &node = *change.after;
            const bool model = node.type == scene::schema::model_type;
            if ((!model && node.type != scene::schema::object_type) || listed.contains(change.id)
                || scene::is_linked_occurrence(node)) continue;
            const auto owner = static_cast<core::u64>(read_transform(node, scene::schema::scene_id, 0));
            if ((owner != 0U ? doc::NodeId{owner} : default_scene) != scene.id) continue;
            auto info = read(change.id, model ? editor::ElementKind::Model : editor::ElementKind::Object);
            apply_parent_state(info);
            elements.push_back(std::move(info));
        }
        // In the evaluated order - generators, then models, each by node - so
        // hiding one leaves its row where it was.
        std::stable_sort(elements.begin() + static_cast<std::ptrdiff_t>(drawables_begin), elements.end(),
            [](const editor::ElementInfo &a, const editor::ElementInfo &b) {
                const bool a_model = a.kind == editor::ElementKind::Model;
                const bool b_model = b.kind == editor::ElementKind::Model;
                return a_model != b_model ? b_model : a.node.value() < b.node.value();
            });
    }
    // Include hidden occurrences, so showing one never depends on whether it
    // happens to draw in this frame. Objects deleted in Blender or in Media
    // Server are gone from every list: their records stay in the document only
    // so Undo on either side can bring them back. This is a cached projection.
    std::map<std::pair<doc::NodeId,doc::NodeId>,doc::NodeId> linked_nodes;
    std::set<doc::NodeId> linked_collections;
    if (linked) for (const auto& [id,p] : linked->occurrences)
        if (p.scene == scene.id) linked_nodes[{p.instance,p.object}] = id;
    if (linked) for (const auto& [id, occurrence] : linked->occurrences) {
        if (occurrence.scene != scene.id || occurrence.removed || occurrence.deleted) continue;
        const auto light_kind=occurrence.light.find(scene::schema::light_kind);
        const bool light=light_kind != occurrence.light.end();
        auto info = read(id, !light ? editor::ElementKind::Model : std::get<std::string>(light_kind->second)==scene::schema::directional_kind
            ? editor::ElementKind::DirectionalLight : editor::ElementKind::PointLight);
        info.linked = true; info.linked_held = occurrence.held;
        info.linked_batch_instances = static_cast<core::u32>(occurrence.batch.size()/96);
        info.linked_instance = occurrence.instance;
        info.linked_parent = occurrence.instance;
        const auto& document = scheduler.projected_document();
        if (const auto* object = document.get(occurrence.object)) {
            const auto parent = object->properties.find(scene::link_schema::parent);
            if (parent != object->properties.end()) if (const auto* value = std::get_if<core::i64>(&parent->second)) {
                const auto found = linked_nodes.find({occurrence.instance,doc::NodeId{static_cast<core::u64>(*value)}});
                if (found != linked_nodes.end()) info.linked_parent = found->second;
            }
        }
        if (const auto* n = document.get(id)) {
            info.linked_visibility_override = n->properties.contains(scene::schema::visible);
            core::u32 bit = 1;
            for (const auto key : {scene::schema::color_r,scene::schema::color_g,scene::schema::color_b,scene::schema::roughness,
                    scene::schema::metallic,scene::schema::emissive_r,scene::schema::emissive_g,scene::schema::emissive_b}) {
                if (n->properties.contains(key)) info.linked_material_overrides |= bit;
                bit <<= 1U;
            }
            info.linked_placement_override = occurrence.held || occurrence.world != occurrence.basis;
        }
        if (light) if (const auto* n=document.get(id))
            for (const auto& [key,value] : occurrence.light) { static_cast<void>(value); if (n->properties.contains(key)) info.linked_material_overrides=1; }
        info.linked_world = scene::translation_of(occurrence.world);
        info.position = info.linked_world;
        info.source = occurrence.source; info.material = occurrence.material;
        info.animation=occurrence.animation;
        info.linked_animation=occurrence.animated;
        if (occurrence.animated || occurrence.animation.wind != scene::Vec3{}) info.bake_static=false;
        info.display_name = occurrence.name;
        if (info.linked_batch_instances) info.display_name += " (" + std::to_string(info.linked_batch_instances) + " instances)";
        info.effective_visible = occurrence.visible;
        if (const auto* attachment = document.get(occurrence.instance)) {
            info.linked_alignment = {static_cast<float>(read_transform(*attachment, "link_alignment_12", 0)),
                static_cast<float>(read_transform(*attachment, "link_alignment_13", 0)),
                static_cast<float>(read_transform(*attachment, "link_alignment_14", 0))};
            info.linked_paused = read_transform(*attachment,"link_paused",0) != 0;
            info.locked = info.locked || read_transform(*attachment,"locked",0) != 0;
            const auto source_id = static_cast<core::u64>(read_transform(*attachment,"link_source",0));
            const auto* source = document.get(doc::NodeId{source_id});
            const auto source_text = [&](std::string_view key) {
                if (source) { const auto p = source->properties.find(key);
                    if (p != source->properties.end()) if (const auto* text = std::get_if<std::string>(&p->second)) return *text; }
                return std::string{};
            };
            info.linked_source_uuid = source_text(scene::link_schema::uuid);
            info.linked_revision = source_text(scene::link_schema::revision);
            if (linked_collections.insert(occurrence.instance).second) {
                auto collection = read(occurrence.instance,editor::ElementKind::Group);
                collection.linked = collection.linked_collection = true;
                collection.linked_instance = occurrence.instance;
                collection.linked_alignment = info.linked_alignment;
                collection.linked_world = info.linked_alignment; collection.position = info.linked_alignment;
                collection.linked_source_uuid = info.linked_source_uuid;
                collection.linked_revision = info.linked_revision;
                collection.linked_paused = info.linked_paused;
                collection.display_name = source_text(scene::schema::group_name);
                if (collection.display_name.empty()) collection.display_name = "Blender collection";
                elements.push_back(std::move(collection));
            }
        }
        elements.push_back(std::move(info));
    }
    // Empty collections still need reconnect, pause and unlink controls, even
    // after every source tombstone's retained shot record has been discarded.
    if (linked) for (const auto& c : doc::diff({},scheduler.projected_document()).changes) {
        const auto& attachment = *c.after;
        if (attachment.type != scene::link_schema::instance_type || linked_collections.contains(c.id)
            || read_transform(attachment,"scene_id",0) != static_cast<double>(scene.id.value())) continue;
        auto info = read(c.id,editor::ElementKind::Group);
        info.linked = info.linked_collection = true; info.linked_instance = c.id;
        info.linked_paused = read_transform(attachment,"link_paused",0) != 0;
        info.linked_alignment = {static_cast<float>(read_transform(attachment,"link_alignment_12",0)),
            static_cast<float>(read_transform(attachment,"link_alignment_13",0)),static_cast<float>(read_transform(attachment,"link_alignment_14",0))};
        info.linked_world = info.position = info.linked_alignment;
        if (const auto* source = scheduler.projected_document().get(doc::NodeId{static_cast<core::u64>(read_transform(attachment,"link_source",0))})) {
            const auto value = [&](std::string_view key) {
                const auto p = source->properties.find(key);
                if (p != source->properties.end()) if (const auto* text = std::get_if<std::string>(&p->second)) return *text;
                return std::string{};
            };
            info.linked_source_uuid = value(scene::link_schema::uuid);
            info.linked_revision = value(scene::link_schema::revision);
            info.display_name = value(scene::schema::group_name);
        }
        if (info.display_name.empty()) info.display_name = "Blender collection";
        elements.push_back(std::move(info));
    }
    for (const auto &light : scene.directional_lights) {
        // The physical sun is generated by Environment, whose identity is
        // the stage. It has no separate authored light or selectable row.
        if (light.source == scene.id || (linked && linked->occurrences.contains(light.source))) continue;
        elements.push_back(
            read(light.source, editor::ElementKind::DirectionalLight));
    }
    for (const auto &light : scene.point_lights) {
        if (linked && linked->occurrences.contains(light.source)) continue;
        elements.push_back(read(light.source, editor::ElementKind::PointLight));
    }
    // Groups remain editor-visible even when they (and therefore their
    // drawables) are hidden. Their authored local transform is read from the
    // document; evaluated metadata supplies inherited lock/visibility truth.
    for (const scene::SceneGroup &group : scene.groups) {
        auto info = read(group.node, editor::ElementKind::Group);
        info.display_name = group.name;
        info.group_kind = group.kind;
        info.parent_group = group.parent_group;
        info.group_order = group.group_order;
        info.locked = group.effective_locked;
        info.visible = group.visible;
        info.effective_visible = group.effective_visible;
        elements.push_back(std::move(info));
    }
    // Tracks are elements because a track is a screen: it has a transform to
    // inspect, a format to set, and a look to grade. The implicit lane is not
    // one - it owns no plane, and a row that cannot be selected is better than
    // a selection that turns out to control nothing.
    for (const scene::TimelineLane &lane : lanes) {
        if (lane.implicit) {
            continue;
        }
        auto info = read(lane.id, editor::ElementKind::Track);
        info.display_name = lane.name;
        // The projection already resolved the aspect lock, so this is the
        // extent the screen renders with rather than the pair of numbers the
        // document happens to hold.
        info.scale = lane.scale;
        info.style.opacity = lane.opacity;
        info.style.blend = lane.compositing;
        info.raster_width = lane.raster_width;
        info.raster_height = lane.raster_height;
        info.aspect_locked = lane.aspect_locked;
        info.locked = lane.locked;
        // Muting a track is what hiding it means, so one control reads and
        // writes one state rather than the timeline and the Inspector each
        // owning half of it.
        info.visible = !lane.muted;
        info.effective_visible = info.visible;
        elements.push_back(std::move(info));
    }

    // Cameras. Authored, listed, and selected like anything else, and they
    // draw nothing themselves - a camera is a place to look from.
    //
    // Tracker feeds deliberately have no row here. A feed has no place on the
    // stage and nothing to select; it is project state named by the camera
    // that uses it, the way a camera names a lens.
    for (const scene::CameraPlacement &camera : cameras) {
        auto info = read(camera.node, editor::ElementKind::Camera);
        info.position = camera.position;
        info.rotation = camera.rotation;
        info.field_of_view = camera.camera.field_of_view;
        info.shutter_seconds = camera.camera.shutter_seconds;
        info.focus_distance_m = camera.camera.focus_distance_m;
        info.aperture_f_number = camera.camera.aperture_f_number;
        info.tracking_source = camera.tracking_source;
        info.icvfx = camera.icvfx;
        // A tracked camera reads as one at a glance, which is the question an
        // operator actually has when several are authored.
        if (info.display_name.empty()) {
            info.display_name = camera.tracking_source.value() != 0U
                                    ? std::string{"Camera (tracked)"}
                                    : std::string{"Camera"};
        }
        elements.push_back(std::move(info));
    }
    static_cast<void>(tracking_sources);
    return elements;
}

// Fills the per-output physical-timing row of Output Monitor.
//
// Each row answers for one supervised render process: how many of its outputs
// the authored sync groups expect, how many actually report locked, whether
// its swapchains joined the expected barrier, and what its frame accounting
// looks like. A row with nothing to go on reports SYNC UNVERIFIED rather than
// an absence that could be mistaken for health.
void apply_node_sync_rows(std::vector<editor::OutputMonitorState::Node> &rows,
                          const sync::SyncPolicy &policy,
                          const sync::SyncTopology &topology,
                          const sync::SyncStatusSnapshot &status) {
    using RowSync = editor::OutputMonitorState::Node::SyncStatus;
    for (editor::OutputMonitorState::Node &row : rows) {
        row.sync = RowSync::Unsynced;
        row.sync_locked_outputs = 0U;
        row.sync_expected_outputs = 0U;
        row.barrier_joined_outputs = 0U;
        row.barrier_expected_outputs = 0U;
        row.barrier_presents = 0U;
        row.barrier_presents_in_sync = 0U;
        row.show_clock_reference = "UNKNOWN";
        row.show_clock_reference_frame = 0U;
        row.missed_frames = 0U;
        row.late_frames = 0U;
        row.held_frames = 0U;
        row.repeated_frames = 0U;
        row.loss_of_sync_events = 0U;
        row.sync_detail.clear();
        if (!policy.requires_physical_sync()) {
            continue;
        }

        const sync::NodeSyncStatus *const observed =
            status.node(row.node_index);
        for (const sync::SyncGroup &group : topology.groups) {
            for (const sync::SyncGroupMember &member : group.members) {
                if (member.node_index != row.node_index) {
                    continue;
                }
                ++row.sync_expected_outputs;
                if (observed == nullptr) {
                    continue;
                }
                for (const sync::SyncOutputStatus &output : observed->outputs) {
                    if (output.target == member.target &&
                        output.lock == sync::SyncLockState::Locked) {
                        ++row.sync_locked_outputs;
                        break;
                    }
                }
            }
        }

        if (observed == nullptr ||
            (!observed->machine_reported && !observed->runtime_reported)) {
            row.sync = RowSync::Unverified;
            continue;
        }
        row.sync_detail = observed->detail;
        row.barrier_joined_outputs = observed->barrier_joined_outputs;
        row.barrier_expected_outputs = observed->barrier_expected_outputs;
        row.barrier_presents = observed->barrier_presents;
        row.barrier_presents_in_sync = observed->barrier_presents_in_sync;
        row.show_clock_reference = sync::show_clock_reference_state_name(
            observed->show_clock_reference);
        row.show_clock_reference_frame =
            observed->show_clock_reference_frame;
        row.missed_frames = observed->counters.missed;
        row.late_frames = observed->counters.late;
        row.held_frames = observed->counters.held;
        row.repeated_frames = observed->counters.repeated;
        row.loss_of_sync_events = observed->counters.loss_of_sync_events;

        // Ordered from the most fundamental missing fact to the most
        // specific, so the row names the first thing an operator must fix.
        switch (observed->board) {
        case sync::SyncDeviceAvailability::Absent:
        case sync::SyncDeviceAvailability::Failed:
            row.sync = RowSync::SyncDeviceMissing;
            continue;
        case sync::SyncDeviceAvailability::Unknown:
        case sync::SyncDeviceAvailability::Unsupported:
            row.sync = RowSync::Unverified;
            continue;
        case sync::SyncDeviceAvailability::Present:
            break;
        }
        if (policy.requires_external_reference()) {
            switch (observed->reference) {
            case sync::SyncReferenceState::Absent:
            case sync::SyncReferenceState::Unstable:
                row.sync = RowSync::ReferenceLost;
                continue;
            case sync::SyncReferenceState::Present:
                row.sync = RowSync::ReferenceNotInUse;
                continue;
            case sync::SyncReferenceState::Unknown:
            case sync::SyncReferenceState::Unsupported:
                row.sync = RowSync::Unverified;
                continue;
            case sync::SyncReferenceState::InUse:
                break;
            }
            switch (observed->show_clock_reference) {
            case sync::ShowClockReferenceState::Locked:
                break;
            case sync::ShowClockReferenceState::Lost:
            case sync::ShowClockReferenceState::Failed:
                row.sync = RowSync::ShowClockReferenceLost;
                continue;
            case sync::ShowClockReferenceState::Unknown:
            case sync::ShowClockReferenceState::Unsupported:
            case sync::ShowClockReferenceState::Waiting:
                row.sync = RowSync::ShowClockReferenceUnavailable;
                continue;
            }
        }
        if (row.sync_locked_outputs < row.sync_expected_outputs) {
            row.sync = RowSync::Unlocked;
            continue;
        }
        if (observed->barrier != sync::PresentBarrierState::Joined ||
            observed->barrier_group != policy.group ||
            observed->barrier_id != policy.barrier ||
            observed->barrier_joined_outputs <
                observed->barrier_expected_outputs) {
            row.sync = observed->barrier == sync::PresentBarrierState::Unknown
                           ? RowSync::Unverified
                           : (observed->barrier ==
                                      sync::PresentBarrierState::Left ||
                              observed->barrier ==
                                      sync::PresentBarrierState::Failed)
                           ? RowSync::BarrierLost
                           : RowSync::BarrierIncomplete;
            continue;
        }
        if (observed->barrier_presents != 0U &&
            observed->barrier_presents_in_sync <
                observed->barrier_presents) {
            row.sync = RowSync::BarrierLost;
            continue;
        }
        row.sync = observed->simulated ? RowSync::Simulated : RowSync::Locked;
    }
}

PhaseExit DirectorShowLoop::refresh_editor(DirectorTick &tick) {
    const auto &now = tick.now;
    const auto &control_frame = tick.control_frame;
    auto &input = tick.input;
    auto &scene_frame = tick.scene_frame;
    auto &feed_map = tick.feed_map;
    auto &hardware_map = tick.hardware_map;
    auto &compositor_graph = tick.compositor_graph;
    auto &handled = tick.handled;
    auto &fast_show = tick.fast_show;
    assets::AssetRegistry &asset_registry = director_asset_registry;
    if (options.edit && editor) handled = true;
    // Keep pointer feedback independent of the authored show rate. The
    // control loop also wakes at this deadline, so a 50 Hz (or slower)
    // show cannot cap panel interaction below 60 Hz. For faster shows,
    // retain one preview per control frame. Rate-limit the preview work
    // rather than rendering on every control-loop poll.
    fast_show = static_cast<core::u64>(input.rate.numerator) >
        static_cast<core::u64>(input.rate.denominator) * 60U;
    if (handled && (now >= next_editor_refresh ||
                    (fast_show && last_editor_control_frame != control_frame))) {
        last_editor_control_frame = control_frame;
        EditorViewInputs inputs;
        if (const PhaseExit exit = project_stage_contents(tick, inputs)) {
            return exit;
        }
        project_switcher_and_links(tick, inputs);
        if (const PhaseExit exit = project_output_monitor(tick, inputs)) {
            return exit;
        }
        project_stage_settings(tick, inputs);
        if (const PhaseExit exit = observe_library_thumbnails(inputs)) {
            return exit;
        }
        if (const PhaseExit exit = observe_timeline_filmstrips(inputs)) {
            return exit;
        }
        project_asset_viewer(inputs);
        if (const PhaseExit exit = project_output_media(tick)) {
            return exit;
        }
        auto &timeline = inputs.timeline;
        const auto &stage_lanes = inputs.stage_lanes;
        const auto &stage_markers = inputs.stage_markers;
        auto &selection_set_summaries = inputs.selection_set_summaries;
        const auto &authored_tracking_sources = inputs.authored_tracking_sources;
        auto &elements = inputs.elements;
        auto &effects = inputs.effects;
        auto &keyframes = inputs.keyframes;
        auto &switcher_view = inputs.switcher_view;
        const auto &project_view = inputs.project_view;
        auto &hardware_runtime = inputs.hardware_runtime;
        auto &output_monitor = inputs.output_monitor;
        auto &project_settings_view = inputs.project_settings_view;
        auto &thumbnail_observations = inputs.thumbnail_observations;
        auto &filmstrip_observations = inputs.filmstrip_observations;
        // Read only. The tracker was sampled and stamped at the frame
        // boundary above; the panel reports that, and never drives it.
        const editor::LiveDataState live_data_view =
            director_live_data(tracker_listeners, director_pose_channels, director_pose_predictors,
                               scene_frame, feed_map,
                               input.control_frame.value(),
                               timeline.program_frame,
                               director_node_tracking);
        observe_timeline_playback(tick, inputs);
        next_editor_refresh = Clock::now() + editor_refresh_interval;
        refresh_history_state();
        // Stage's menu also shows how far the whole project's editor
        // proxies have got.
        const ProxyGenerationWorker::Totals all_proxies =
            director_proxy_generator != nullptr
                ? director_proxy_generator->totals()
                : ProxyGenerationWorker::Totals{};
        editor->set_view_media_counts(
            editor::ViewMediaCounts{program_view_proxies.proxied,
                                    program_view_proxies.waiting,
                                    program_view_proxies.waiting_made_frames,
                                    program_view_proxies.waiting_total_frames},
            editor::ViewMediaCounts{stage_view_proxies.proxied,
                                    stage_view_proxies.waiting,
                                    stage_view_proxies.waiting_made_frames,
                                    stage_view_proxies.waiting_total_frames,
                                    all_proxies.made_frames,
                                    all_proxies.total_frames});
        auto editor_frame = editor->frame(
            scene_frame, timeline, keyframes, elements, effects,
            hardware_map, feed_map, switcher_view, asset_registry,
            thumbnail_observations, compositor_graph, project_view,
            output_monitor, hardware_runtime, director_sync.policy,
            preferences_view, project_settings_view, library_proxy_runtime,
            stage_lanes, clip_clipboard.span_duration(),
            selection_set_summaries, key_scale_preview,
            filmstrip_observations, live_data_view,
            authored_tracking_sources, stage_markers, inputs.asset_viewer,
            history_state);
        ++editor_frame_calls;
        // Shown again next frame only while the panel keeps asking.
        asset_viewer_request = editor_frame.asset_viewer;
        editor_program_full_media =
            editor_frame.program_media == editor::MediaRendition::Full;
        editor_stage_full_media =
            editor_frame.stage_media == editor::MediaRendition::Full;
        plateset_viewed = editor_frame.plateset_viewed;
        if (editor_frame.plateset_analyse.has_value() &&
            plateset_motion != nullptr) {
            const assets::Asset *const asset =
                director_asset_registry.asset(*editor_frame.plateset_analyse);
            const auto *const set =
                asset != nullptr ? std::get_if<assets::PlatesetAsset>(asset)
                                 : nullptr;
            if (set != nullptr) {
                server::PlateSources plates{};
                for (std::size_t slot = 0U; slot < plates.size(); ++slot) {
                    const assets::Asset *const plate =
                        director_asset_registry.asset(set->slots[slot].media);
                    const auto *const media =
                        plate != nullptr
                            ? std::get_if<assets::MediaAsset>(plate)
                            : nullptr;
                    if (media == nullptr || !media->frame_rate.has_value()) {
                        continue;
                    }
                    plates[slot] = server::PlateSource{
                        media->source, media->fingerprint,
                        media->frame_count.value_or(0U), *media->frame_rate};
                }
                plateset_motion->analyse(
                    set->id, std::move(plates),
                    editor_frame.plateset_pathway ==
                            editor::PlatesetAnalysisPathway::Moments
                        ? server::PlatesetMotionService::Pathway::Moments
                        : server::PlatesetMotionService::Pathway::Sync);
            }
        }
        if (const PhaseExit exit =
                apply_editor_services(inputs, editor_frame)) {
            return exit;
        }
        if (const PhaseExit exit = request_library_thumbnails(editor_frame)) {
            return exit;
        }
        if (const PhaseExit exit = apply_preferences_request(editor_frame)) {
            return exit;
        }
        if (const PhaseExit exit = apply_proxy_cache_clear(editor_frame)) {
            return exit;
        }
        if (editor_frame.output_lifecycle.has_value() &&
            lifecycle_controller) {
            lifecycle_controller->submit(*editor_frame.output_lifecycle);
        }
        if (editor_frame.status == editor::EditorStatus::Closed) {
            closed = true;
        }
        if (const PhaseExit exit =
                queue_editor_edits(editor_frame)) {
            return exit;
        }
        if (const PhaseExit exit =
                queue_editor_requests(tick, editor_frame)) {
            return exit;
        }
    }
    return std::nullopt;
}

PhaseExit DirectorShowLoop::project_stage_contents(DirectorTick &tick,
                                                   EditorViewInputs &inputs) {
    const auto &control_frame = tick.control_frame;
    auto &input = tick.input;
    const auto &stage_show_frame = tick.stage_show_frame;
    const auto &stage_playing = tick.stage_playing;
    auto &stage_scene_frame = tick.stage_scene_frame;
    auto &feed_map = tick.feed_map;
    auto &timeline = inputs.timeline;
    auto &stage_clips = inputs.stage_clips;
    auto &stage_lanes = inputs.stage_lanes;
    auto &stage_markers = inputs.stage_markers;
    auto &selection_set_summaries = inputs.selection_set_summaries;
    auto &authored_tracking_sources = inputs.authored_tracking_sources;
    auto &elements = inputs.elements;
    auto &effects = inputs.effects;
    auto &keyframes = inputs.keyframes;
    // The timeline authors against the Stage playhead and reports the
    // Program one beside it, so an operator rehearsing at frame 900
    // can still read where the live show is.
    timeline = editor::EditorTimeline{
        stage_show_frame.value(),
        input.show_frame.value(),
        transport.anchor().first_frame.value(),
        transport.anchor().last_frame.value(),
        stage_playing,
        transport.state() == show::TransportState::Playing,
        transport.anchor().loop,
        transport.anchor().rate.numerator,
        transport.anchor().rate.denominator};
    {
        // The cue picture belongs to the playhead the timeline
        // drives: Preview's while rehearsing, the show's otherwise.
        const show::CueStatus cues =
            (preview_transport.has_value() ? *preview_transport
                                           : transport)
                .cue_status(control_frame);
        timeline.cues_armed = transport.anchor().cues_armed;
        timeline.held_cue = doc::NodeId{cues.held};
        timeline.looping_cue = doc::NodeId{cues.looping};
        timeline.next_cue = doc::NodeId{cues.next};
        timeline.frames_to_next_cue = cues.frames_to_next;
        timeline.frames_to_loop_restart = cues.frames_to_restart;
    }
    stage_clips = scene_model.clips(stage_scene_frame.id);
    stage_lanes = scene_model.lanes(stage_scene_frame.id);
    stage_markers =
        scene_model.markers(stage_scene_frame.id);
    auto selection_set_catalog = scene::selection_sets(
        scheduler.sealed_document(), stage_scene_frame.id);
    if (selection_set_catalog) {
        selection_set_summaries =
            std::move(selection_set_catalog).value().sets;
    } else if (selection_set_catalog.error().code ==
               scene::SelectionSetErrorCode::AllocationFailed) {
        std::cerr
            << "could not project the Stage Selection Sets catalog\n";
        return 1;
    }
    const auto stage_cameras =
        scene_model.cameras(stage_scene_frame.id);
    authored_tracking_sources =
        scene_model.tracking_sources();
    if (scene_model.linked_error()) project_message = "Linked scene: " + scene_model.linked_error()->message;
    elements = scene_elements(
        scheduler, stage_scene_frame, stage_clips,
        stage_show_frame.value(), stage_lanes, stage_cameras,
        authored_tracking_sources, &scene_model.linked_scene());
    for (auto& element : elements) if (element.linked) {
        const auto link_status = blender_links.status_for_source(element.linked_source_uuid);
        element.linked_listening = link_status && link_status->listening;
        if (link_status) {
            element.linked_connected = link_status->connected;
            element.linked_pending = link_status->pending;
            element.linked_failed = link_status->failed;
            if (element.linked_failed || element.linked_pending) element.linked_status = link_status->message;
        }
        if (element.linked_paused) element.linked_status = "Resume source updates to sync pending Blender changes automatically.";
    }
    const auto all_effects = scene_model.effects();
    for (const scene::EffectInstance &effect : all_effects) {
        const bool in_stage =
            std::any_of(elements.begin(), elements.end(),
                        [&](const editor::ElementInfo &element) {
                            return element.node == effect.target;
                        });
        if (in_stage) {
            effects.push_back(effect);
        }
    }
    const auto all_keyframes = scene_model.channel_keyframes();
    for (const scene::ChannelKeyframes &channel : all_keyframes) {
        const bool element_channel =
            std::any_of(elements.begin(), elements.end(),
                        [&](const editor::ElementInfo &element) {
                            return element.node == channel.target;
                        });
        const bool effect_channel =
            std::any_of(effects.begin(), effects.end(),
                        [&](const scene::EffectInstance &effect) {
                            return effect.source == channel.target;
                        });
        const bool surface_channel =
            feed_map.surface_for(channel.target) != nullptr;
        const bool environment_channel =
            channel.target == stage_scene_frame.id &&
            scene::is_environment_channel(channel.component);
        if (element_channel || effect_channel || surface_channel || environment_channel) {
            keyframes.push_back(channel);
        }
    }
    return std::nullopt;
}

void DirectorShowLoop::project_switcher_and_links(DirectorTick &tick,
                                                  EditorViewInputs &inputs) {
    const auto &control_frame = tick.control_frame;
    auto &scene_frame = tick.scene_frame;
    auto &stage_scene_frame = tick.stage_scene_frame;
    auto &transition_from_frame = tick.transition_from_frame;
    auto &transition_to_frame = tick.transition_to_frame;
    auto &scene_infos = tick.scene_infos;
    auto &switcher_anchor = tick.switcher_anchor;
    auto &switcher_progress = tick.switcher_progress;
    auto &loop_blend = tick.loop_blend;
    auto &program_compositor_graph = tick.program_compositor_graph;
    auto &transition_from_graph = tick.transition_from_graph;
    auto &transition_to_graph = tick.transition_to_graph;
    auto &switcher_view = inputs.switcher_view;
    auto &blender_state = inputs.blender_state;
    auto &blender_preview_instance = inputs.blender_preview_instance;
    auto &project_view = inputs.project_view;
    // A published revision is listed only while it is on air; the
    // retained rollback is reached through Roll back Program.
    listed_scenes.clear();
    for (const auto& info : scene_infos) {
        const bool revision = !scene::publication::text(scheduler.sealed_document().get(info.id),
                                                        scene::publication::revision).empty();
        if (!revision || (switcher_anchor && (info.id == switcher_anchor->program_scene
                || info.id == switcher_anchor->rehearsal_scene)))
            listed_scenes.push_back(info);
    }
    switcher_view.scenes = listed_scenes;
    switcher_view.program_scene = switcher_anchor.has_value()
                                      ? switcher_anchor->program_scene
                                      : scene_frame.id;
    switcher_view.rehearsal_scene =
        switcher_anchor.has_value() ? switcher_anchor->rehearsal_scene
                                    : scene::SceneId{};
    switcher_view.stage_scene = &stage_scene_frame;
    switcher_view.stage_view_override =
        tick.stage_view_frame.has_value() ? &*tick.stage_view_frame : nullptr;
    switcher_view.stage_view_caption =
        tick.stage_view_frame.has_value()
            ? std::string_view{tick.stage_view_caption}
            : std::string_view{};
    switcher_view.program_graph = &program_compositor_graph;
    const bool program_mixing =
        switcher_progress.active || loop_blend.has_value();
    switcher_view.transition_from =
        program_mixing ? &transition_from_frame : nullptr;
    switcher_view.transition_to =
        program_mixing ? &transition_to_frame : nullptr;
    switcher_view.transition_from_graph =
        program_mixing ? &transition_from_graph : nullptr;
    switcher_view.transition_to_graph =
        program_mixing ? &transition_to_graph : nullptr;
    switcher_view.transition_mix = static_cast<float>(
        loop_blend.has_value() ? loop_blend->mix()
                               : switcher_progress.mix());
    switcher_view.transition_active = switcher_progress.active;
    switcher_view.loop_fade_active = loop_blend.has_value();
    switcher_view.rehearsal_mode = rehearsal_mode;
    switcher_view.rehearsal_locked = switcher_progress.active;
    switcher_view.dissolve_compatible = false;
    if (switcher_anchor.has_value()) {
        const auto program_info = std::find_if(
            scene_infos.begin(), scene_infos.end(),
            [&](const scene::SceneInfo &info) {
                return info.id == switcher_anchor->program_scene;
            });
        const auto rehearsal_info = std::find_if(
            scene_infos.begin(), scene_infos.end(),
            [&](const scene::SceneInfo &info) {
                return info.id == switcher_anchor->rehearsal_scene;
            });
        switcher_view.dissolve_compatible =
            program_info != scene_infos.end() &&
            rehearsal_info != scene_infos.end() &&
            show::dissolve_canvases_compatible(program_info->canvas,
                                               rehearsal_info->canvas);
    }
    const auto publication_program_node=switcher_anchor ? scheduler.sealed_document().get(switcher_anchor->program_scene) : nullptr;
    publication_storage=scene::publication::text(publication_program_node,scene::publication::storage);
    const bool publication_waiting=publication_staging || scene::publication::integer(publication_program_node,scene::publication::candidate) != 0;
    if (!director_publication_verification.diagnostic().empty() && !publication_staging)
        publication_message=std::string{director_publication_verification.diagnostic()};
    else if (publication_waiting && !publication_staging && publication_activation <= control_frame.value())
        publication_message="Waiting for complete GPU residency: "+std::to_string(director_asset_cohort.ready_members)+" / "+std::to_string(director_asset_cohort.members)+" nodes ready.";
    else if (publication_activation && control_frame.value() >= publication_activation && director_asset_cohort.visible)
        publication_message="Program published at control frame "+std::to_string(director_asset_cohort.visible_commit)+"; Blender may keep editing Preview.";
    // A Blender-linked scene is live wherever it is, so taking it is
    // an ordinary cut. Only a frozen revision in Preview publishes.
    switcher_view.linked_publication=switcher_anchor
        && !scene::publication::text(scheduler.sealed_document().get(switcher_anchor->rehearsal_scene),scene::publication::revision).empty();
    switcher_view.publication_pending=publication_waiting || publication_activation > control_frame.value();
    if (publication_message != publication_message_shown) {
        publication_message_shown = publication_message;
        project_message = "PUBLISH - " + publication_message;
    }
    blender_state = blender_links.discovery();
    // The Publish card and the Connect dialog speak about the Preview
    // scene's link; every scene row speaks about its own.
    const doc::NodeId blender_preview = switcher_anchor ? switcher_anchor->rehearsal_scene : doc::NodeId{};
    const auto preview_link = blender_links.status_for_scene(blender_preview);
    scene_link_details = preview_link ? preview_link->message : std::string{};
    scene_link_diagnostics = preview_link ? link_diagnostics(*preview_link) : std::string{};
    blender_preview_instance = blender_links.instance_for_scene(blender_preview);
    blender_views = blender_links.views();
    blender_scene_states.clear();
    for (const auto& view : blender_views) {
        const auto saved = std::find_if(blender_associations.begin(), blender_associations.end(),
            [&](const BlenderAssociation& item) { return item.scene == view.scene; });
        blender_scene_states.push_back(editor::BlenderSceneState{view.scene,
            saved == blender_associations.end() ? std::string_view{} : std::string_view{saved->file},
            view.running, view.opening, view.connected, view.pending, view.failed, view.status});
    }
    for (const auto& saved : blender_associations)
        if (!saved.file.empty() && std::none_of(blender_views.begin(), blender_views.end(),
                [&](const BlenderLinks::View& view) { return view.scene == saved.scene; }))
            blender_scene_states.push_back(editor::BlenderSceneState{saved.scene, saved.file});
    blender_bound_names.clear();
    for (const auto& [instance, scene] : blender_links.bound_instances()) {
        const auto saved = std::find_if(blender_associations.begin(), blender_associations.end(),
            [&](const BlenderAssociation& item) { return item.scene == scene; });
        blender_bound_names.emplace_back(instance, saved == blender_associations.end() || saved->name.empty()
            ? std::string{"Untitled Scene"} : saved->name);
    }
    publication_default_storage = default_publication_storage(project_path);
    changed_library_assets = still_changed_library_sources(
        director_asset_registry, changed_library_sources);
    project_view = editor::EditorProjectState{
        project_path, project_message,
        project_explicitly_dirty ||
            project_content_changed(saved_project_snapshot,
                                    scheduler.projected_document()),
        project_generation, scene_link_details,scene_link_diagnostics,
        publication_storage,publication_message,publication_waiting,
        publication_activation > control_frame.value(),scene::publication::integer(publication_program_node,scene::publication::rollback) != 0,
        blender_state ? std::span<const render::BlenderInstance>{blender_state->instances} : std::span<const render::BlenderInstance>{},
        blender_preview_instance, blender_connection_error.empty() && blender_state ? std::string_view{blender_state->error} : std::string_view{blender_connection_error},
        preview_link && preview_link->connected, blender_scene_states, blender_bound_names,
        publication_default_storage,recent_project_paths,
        changed_library_assets};
}

PhaseExit DirectorShowLoop::project_output_monitor(DirectorTick &tick,
                                                   EditorViewInputs &inputs) {
    const auto &control_frame = tick.control_frame;
    auto &scene_frame = tick.scene_frame;
    auto &hardware_map = tick.hardware_map;
    auto &asset_manifest = tick.asset_manifest;
    auto &timeline = inputs.timeline;
    auto &switcher_view = inputs.switcher_view;
    auto &output_nodes = inputs.output_nodes;
    auto &hardware_runtime = inputs.hardware_runtime;
    auto &renderer = inputs.renderer;
    auto &output_monitor = inputs.output_monitor;
    if (lifecycle_controller) {
        cluster_configuration_authored = true;
        try {
            lifecycle_controller->sync(hardware_map, asset_manifest,
                                       project_generation);
            output_nodes = lifecycle_controller->snapshot(&barrier);
            hardware_runtime =
                lifecycle_controller->hardware_snapshot();
        } catch (const std::bad_alloc &) {
            std::cerr << "could not build hardware runtime snapshot\n";
            return 1;
        }
    }
    if (options.lifecycle_discovery_port != 0U) {
        topology::HardwareDiscovery &discovery =
            hardware_runtime.discovery;
        discovery.port = options.lifecycle_discovery_port;
        discovery.password_set =
            preferences_view.active.render_node_password;
        const net::ClusterDiscoverySnapshot found =
            discovery_browser ? discovery_browser->snapshot()
                              : net::ClusterDiscoverySnapshot{};
        if (!found.running) {
            const std::error_code reason =
                found.error ? found.error : discovery_error;
            discovery.state =
                topology::HardwareDiscoveryState::Unavailable;
            discovery.detail = reason ? reason.message()
                                      : std::string{"not running"};
        } else {
            discovery.state =
                topology::HardwareDiscoveryState::Searching;
            try {
                discovery.machines.reserve(found.machines.size());
                for (const net::ClusterDiscoveredMachine &machine :
                     found.machines) {
                    topology::HardwareDiscoveredMachine shown{};
                    shown.hostname = machine.hostname;
                    shown.address = machine.address.to_string();
                    for (const asio::ip::address_v4 &address :
                         machine.addresses) {
                        shown.addresses.push_back(address.to_string());
                    }
                    shown.node_index = machine.node_index;
                    shown.control_port = machine.control_port;
                    shown.password_set = machine.password_set;
                    shown.this_machine = machine.this_machine;
                    discovery.machines.push_back(std::move(shown));
                }
            } catch (const std::bad_alloc &) {
                discovery.machines.clear();
            }
        }
    }
    configured_cluster_nodes = static_cast<core::u32>(
        std::min<std::size_t>(output_nodes.size(),
                              std::numeric_limits<core::u32>::max()));
    // Asset adoption is reported, not enforced. Distributing media to
    // render nodes is an external concern in this build, so a node that
    // cannot yet prove it holds every file is a warning on the Output
    // Monitor rather than a hold on the switcher.
    bool assets_adopted = director_asset_availability.render_ready();
    if (assets_adopted && lifecycle_controller) {
        assets_adopted = lifecycle_controller->program_ready(
            asset_manifest, hardware_map, project_generation, barrier);
    }
    if (lifecycle_controller) {
        director_sync.status =
            lifecycle_controller->sync_snapshot(&barrier);
        // The Director's own clock, alongside the nodes'. It is not a
        // node row: nothing about it comes off the wire, and an
        // operator reading a wall of locked nodes has no other way to
        // learn that the cadence they are all locked to is this
        // machine's system clock.
        director_sync.status.director_clock = director_clock;
        director_node_tracking =
            lifecycle_controller->tracking_snapshot(&barrier);
    }
    // The profile Program is drawn with: a published revision keeps the
    // renderer settings it was published with, so Preview's settings
    // (an Advanced trial, say) must not mark every output stale and
    // hold the very PUBLISH that would carry them to Program.
    renderer =
        scene_frame.id != scene::SceneId{} ? scene_frame.renderer_settings
                                           : scene_model.renderer_settings();
    director_renderer_lockstep =
        evaluate_renderer_lockstep(renderer, hardware_map);
    director_sync.readiness =
        evaluate_sync_gate(
            assets_adopted,
            director_renderer_lockstep.satisfied());
    // The one condition that still holds a promotion, and only when
    // the project asked for it. A policy that makes no physical timing
    // claim, or that declares its failures Degraded, never blocks.
    switcher_view.program_held = program_sync_held();
    output_monitor = editor::OutputMonitorState{
        timeline.program_frame,
        scene_frame.canvas.width,
        scene_frame.canvas.height,
        static_cast<float>(timeline.rate_numerator) /
            static_cast<float>(std::max(timeline.rate_denominator, 1U)),
        timeline.program_playing,
        output_nodes};
    const ClusterObservation cluster =
        observe_cluster(timeline.program_frame);
    using UiHealth = editor::OutputMonitorState::Cluster::Health;
    switch (cluster.health) {
    case ClusterControlHealth::Unmonitored:
        output_monitor.cluster.health = UiHealth::Unmonitored;
        break;
    case ClusterControlHealth::Healthy:
        output_monitor.cluster.health = UiHealth::Healthy;
        break;
    case ClusterControlHealth::Degraded:
        output_monitor.cluster.health = UiHealth::Degraded;
        break;
    case ClusterControlHealth::Faulted:
        output_monitor.cluster.health = UiHealth::Faulted;
        break;
    }
    output_monitor.cluster.configured_nodes = cluster.configured_nodes;
    output_monitor.cluster.live_sessions = cluster.live_sessions;
    output_monitor.cluster.catching_up_sessions =
        cluster.catching_up_sessions;
    output_monitor.cluster.release_lag_frames =
        cluster.release_lag_frames;
    output_monitor.cluster.pacer_lag_frames = cluster.pacer_lag_frames;
    output_monitor.cluster.control_stalled = cluster.control_stalled;
    output_monitor.cluster.asset_cohort = director_asset_cohort;
    output_monitor.cluster.asset_waiting_node = asset_waiting_node;
    // An output that never opened outranks every other reason here. It is
    // upstream of all of them: a node with no presenters reports no renderer
    // and no assets, so the cohort and lockstep headlines below would both
    // describe the silence rather than its cause.
    const auto failed_output = std::find_if(
        output_nodes.begin(), output_nodes.end(),
        [](const editor::OutputMonitorState::Node& row) noexcept {
            return row.output_failed();
        });
    // The label is a view over a literal, which is what this field holds; the
    // row beside it already names which node, so the headline carries the
    // reason alone rather than owning a composed string for one frame.
    output_monitor.cluster.renderer_headline =
        failed_output != output_nodes.end()
        ? editor::output_node_deployment_label(*failed_output)
        : director_asset_cohort.digest != 0 && !director_asset_cohort.visible
            ? director_asset_cohort.some_visible
                ? core::asset_commit_confirmation_expired(last_asset_commit_frame, control_frame.value())
                    ? "ASSET COMMIT MISSED - RETRY PENDING"
                    : last_asset_commit_frame > control_frame.value()
                        ? "ASSET COMMIT RETRY SCHEDULED"
                        : "ASSET COMMIT AWAITING CONFIRMATION"
            : director_asset_cohort.can_commit() ? "ASSET COHORT READY - COMMIT PENDING" : "ASSET COHORT LOADING"
            : server::renderer_lockstep_headline(director_renderer_lockstep.code);
    output_monitor.cluster.renderer_active =
        director_renderer_lockstep.code !=
        server::RendererLockstepCode::Inactive;
    output_monitor.cluster.renderer_satisfied =
        director_renderer_lockstep.satisfied();
    output_monitor.cluster.renderer_fallback =
        director_renderer_lockstep.code ==
            server::RendererLockstepCode::Ready &&
        director_renderer_lockstep.requested ==
            net::RuntimeRendererBackend::Advanced &&
        director_renderer_lockstep.selected ==
            net::RuntimeRendererBackend::Basic;
    output_monitor.cluster.renderer_selected_advanced =
        director_renderer_lockstep.selected ==
        net::RuntimeRendererBackend::Advanced;
    output_monitor.cluster.renderer_failure_local =
        director_renderer_lockstep.local_failure;
    output_monitor.cluster.renderer_failure_node =
        director_renderer_lockstep.failing_node;
    output_monitor.cluster.conformance_active = renderer.backend == scene::RendererBackend::Advanced;
    output_monitor.cluster.conformance = observed_local_renderer.conformance;
    output_monitor.cluster.frame_timing = observed_local_renderer.timing;
    output_monitor.cluster.frame_budget_us = renderer.frame_budget_us;
    output_monitor.cluster.conformance_difference = director_renderer_lockstep.conformance_difference;
    output_monitor.cluster.heartbeat_timeouts =
        cluster.sessions.heartbeat_timeouts;
    output_monitor.cluster.session_failures =
        cluster.sessions.session_failures;
    output_monitor.cluster.barrier_rejections =
        cluster.barrier.rejected_registrations +
        cluster.barrier.rejected_acknowledgements;
    output_monitor.cluster.lockstep_demotions =
        cluster.barrier.lockstep_demotions;
    output_monitor.cluster.present_failures = cluster.present_failures;
    output_monitor.cluster.diagnostics_recording =
        cluster.diagnostics_recording;
    output_monitor.cluster.control_unsecured =
        !preferences_view.active.render_node_password;
    output_monitor.cluster.external_time_sync_verified =
        cluster.external_time_sync_verified;
    output_monitor.cluster.hardware_frame_lock_verified =
        cluster.hardware_frame_lock_verified;
    output_monitor.cluster.sync_mode =
        sync::sync_mode_name(director_sync.policy.mode);
    // The timing line reports the timing fact. An unrelated hold -
    // assets not adopted, a node still catching up - has its own
    // indicators and must not be dressed up as a raster problem.
    output_monitor.cluster.sync_headline =
        director_sync.readiness.physical_headline();
    output_monitor.cluster.sync_required =
        director_sync.policy.requires_physical_sync();
    output_monitor.cluster.sync_satisfied =
        director_sync.readiness.satisfied;
    output_monitor.cluster.sync_degraded =
        director_sync.readiness.degraded;
    output_monitor.cluster.sync_groups = director_sync.readiness.groups;
    output_monitor.cluster.sync_locked_outputs =
        director_sync.readiness.locked_outputs;
    output_monitor.cluster.sync_expected_outputs =
        director_sync.readiness.expected_outputs;
    output_monitor.cluster.barrier_joined_outputs =
        director_sync.readiness.barrier_joined_outputs;
    output_monitor.cluster.sync_configuration_issues =
        static_cast<core::u32>(
            director_sync.topology.diagnostics.size());
    for (auto& row : output_nodes) {
        net::RuntimeRendererReadiness report{};
        if (barrier.runtime_renderer_readiness(row.node_index, report)) {
            row.conformance = report.conformance;
            row.frame_timing = report.timing;
        }
    }
    apply_node_sync_rows(output_nodes, director_sync.policy,
                         director_sync.topology, director_sync.status);
    return std::nullopt;
}

void DirectorShowLoop::project_stage_settings(DirectorTick &tick,
                                              EditorViewInputs &inputs) {
    auto &scene_frame = tick.scene_frame;
    auto &stage_scene_frame = tick.stage_scene_frame;
    auto &project_settings_view = inputs.project_settings_view;
    // What became of the authored map.
    //
    // Read straight from the residency cache rather than plumbed back
    // out of the renderer: default handles share one process-wide
    // state, so this is the same decode the renderer observed. The
    // backend check comes first because a Basic project ignores a
    // perfectly good map, and reporting "loading" there would send an
    // operator to look at the file.
    editor::EnvironmentStatus environment_status =
        editor::EnvironmentStatus::NotAuthored;
    // Checked whether or not a map is authored: with nowhere to store
    // one, typing a path and pressing Apply silently discards it, and
    // the operator sees an empty field rather than a reason.
    const doc::Node* const authored_stage_node =
        scheduler.projected_document().get(stage_scene_frame.id);
    const bool stage_writable = authored_stage_node != nullptr
        && authored_stage_node->type == scene::schema::stage_type;
    if (!stage_writable) {
        environment_status = editor::EnvironmentStatus::NoStage;
    } else if (!stage_scene_frame.environment.physical_sky.enabled && !stage_scene_frame.environment.map_source.empty()) {
        // The backend the renderer actually *selected*, not the one the
        // project asked for. They differ exactly where this matters: a
        // project authoring Advanced while requesting a pass that does
        // not exist yet has its Advanced request refused, and reporting
        // the authored value would call the environment Ready while
        // nothing was using it.
        const bool advanced_running =
            observed_local_renderer.selected
                == net::RuntimeRendererBackend::Advanced
            && observed_local_renderer.satisfied;
        if (!advanced_running) {
            environment_status =
                editor::EnvironmentStatus::BackendUnsupported;
        } else {
            render::EnvironmentCache environment_probe;
            const auto observed = environment_probe.observe(
                stage_scene_frame.environment.map_source, 0U);
            environment_status = observed.ready()
                ? editor::EnvironmentStatus::Ready
                : (observed.state
                           == render::EnvironmentCacheState::Failed
                       ? editor::EnvironmentStatus::Failed
                       : editor::EnvironmentStatus::Loading);
        }
    }
    project_settings_view = editor::ProjectSettingsState{
        scene_model.effect_settings(),
        scene_model.renderer_settings(),
        // The lighting environment and the stage node that owns it.
        // Inspector edits start from authored values. Using the
        // evaluated frame here would bake animated values back into
        // the base whenever an unrelated environment field changed.
        scene_model.environment(stage_scene_frame.id),
        // The stage node of the scene this dialog is reading from.
        //
        // A scene's id *is* its stage node's id, so this addresses the
        // exact node the environment above came from. Asking the model
        // for "the stage node" instead would answer with the first one
        // in the document, which on a project with more than one scene
        // is a different node - the write would land somewhere the
        // dialog never reads, and the field would silently revert.
        stage_scene_frame.id,
        environment_status,
        scene_model.effect_settings_node(),
        stage_scene_frame.camera.lens_profile,
        stage_scene_frame.camera_source};
    project_settings_view.program_renderer_differs =
        scene_frame.id != scene::SceneId{} &&
        !scene::publication::text(scheduler.sealed_document().get(scene_frame.id),
                                  scene::publication::revision).empty() &&
        scene::renderer_settings_digest(scene_frame.renderer_settings) !=
            scene::renderer_settings_digest(scene_model.renderer_settings());
}

PhaseExit DirectorShowLoop::observe_library_thumbnails(EditorViewInputs &inputs) {
    const auto &project_view = inputs.project_view;
    auto &thumbnail_observations = inputs.thumbnail_observations;
    assets::AssetRegistry &asset_registry = director_asset_registry;
    if (thumbnail_service) {
        editor->reset_library_thumbnail_textures(
            project_view.generation);
        const assets::DerivativeProfile *const profile =
            assets::derivative_profile(
                assets::DerivativeProfileId::Thumbnail320x180Srgb);
        // A model's thumbnail is its own profile, drawn by the editor.
        const assets::DerivativeProfile *const model_profile =
            assets::derivative_profile(
                assets::DerivativeProfileId::ModelTurntable320x180Srgb);
        if (profile != nullptr && model_profile != nullptr) {
            try {
                thumbnail_observations.reserve(
                    asset_registry.assets.size());
                for (const assets::Asset &asset :
                     asset_registry.assets) {
                    const bool media =
                        std::holds_alternative<assets::MediaAsset>(asset);
                    const auto &fingerprint =
                        assets::asset_fingerprint(asset);
                    if (!fingerprint.has_value()) {
                        continue;
                    }
                    const assets::DerivativeKey key =
                        assets::make_derivative_key(
                            assets::asset_id(asset), *fingerprint,
                            media ? *profile : *model_profile);
                    editor::LibraryThumbnailState state =
                        editor::LibraryThumbnailState::Placeholder;
                    const rhi::Texture *texture =
                        editor->library_thumbnail_texture(key);
                    if (const auto snapshot =
                            thumbnail_service->snapshot(key);
                        snapshot.has_value()) {
                        switch (snapshot->state) {
                        case assets::DerivativeState::Queued:
                            state =
                                editor::LibraryThumbnailState::Queued;
                            break;
                        case assets::DerivativeState::Generating:
                            state = editor::LibraryThumbnailState::
                                Generating;
                            break;
                        case assets::DerivativeState::Ready:
                            state =
                                editor::LibraryThumbnailState::Ready;
                            if (texture == nullptr &&
                                std::find(
                                    retained_thumbnail_keys.begin(),
                                    retained_thumbnail_keys.end(),
                                    key) !=
                                    retained_thumbnail_keys.end()) {
                                auto loaded =
                                    thumbnail_service->load(key);
                                if (loaded) {
                                    assets::DerivativeCacheLoad data =
                                        std::move(loaded).value();
                                    if (editor
                                            ->upload_library_thumbnail(
                                                key, data.payload)) {
                                        texture =
                                            editor
                                                ->library_thumbnail_texture(
                                                    key);
                                    } else {
                                        state = editor::
                                            LibraryThumbnailState::
                                                Failed;
                                    }
                                } else {
                                    state = editor::
                                        LibraryThumbnailState::Failed;
                                }
                            }
                            break;
                        case assets::DerivativeState::Stale:
                            state =
                                editor::LibraryThumbnailState::Stale;
                            break;
                        case assets::DerivativeState::Failed:
                            state =
                                editor::LibraryThumbnailState::Failed;
                            break;
                        case assets::DerivativeState::Missing:
                        case assets::DerivativeState::Cancelled:
                            break;
                        }
                    }
                    thumbnail_observations.push_back(
                        editor::LibraryThumbnailObservation{key, state,
                                                            texture});
                }
            } catch (const std::bad_alloc &) {
                std::cerr << "could not project Library thumbnail "
                             "observations\n";
                return 1;
            }
        }
    }
    return std::nullopt;
}

void DirectorShowLoop::observe_timeline_playback(DirectorTick &tick,
                                                 EditorViewInputs &inputs) {
    const auto &stage_clips = inputs.stage_clips;
    auto &timeline = inputs.timeline;
    const scene::SceneTime rate{0U, timeline.rate_numerator,
                                timeline.rate_denominator};
    try {
        // A placement on air names the file it really decodes - a proxy, or
        // a source the asset overlay moved - and its numbering in that file.
        for (const scene::MediaInstance &instance :
             tick.stage_scene_frame.media) {
            const auto clip = std::find_if(
                stage_clips.begin(), stage_clips.end(),
                [&](const scene::ClipPlacement &candidate) noexcept {
                    return candidate.node == instance.source_node;
                });
            if (clip == stage_clips.end() || instance.offline) {
                continue;
            }
            const core::u32 document_frame = scene::clip_source_frame(
                *clip, rate, tick.stage_show_frame.value());
            timeline_decode_sources[clip->node] = TimelineDecodeSource{
                clip->source, instance.source,
                static_cast<core::i64>(instance.frame_index) -
                    static_cast<core::i64>(document_frame)};
        }
        std::erase_if(timeline_decode_sources, [&](const auto &entry) {
            const auto clip = std::find_if(
                stage_clips.begin(), stage_clips.end(),
                [&](const scene::ClipPlacement &candidate) noexcept {
                    return candidate.node == entry.first;
                });
            return clip == stage_clips.end() ||
                   clip->source != entry.second.document_source;
        });

        // The window the bar can say anything about. Decode residency holds
        // a few hundred frames at most, so a wider one only costs time.
        constexpr core::u64 window_frames = 300U;
        const core::u64 first_frame =
            std::min(timeline.first_frame, timeline.last_frame);
        const core::u64 last_frame =
            std::max(timeline.first_frame, timeline.last_frame);
        const core::u64 playhead =
            std::clamp(timeline.show_frame, first_frame, last_frame);
        const core::u64 window_first = std::max(
            first_frame, playhead > window_frames ? playhead - window_frames
                                                  : core::u64{0U});
        const core::u64 window_last =
            std::min(last_frame, playhead + window_frames);

        // Only clips something would draw. A hidden clip or a muted track
        // decodes nothing, and must not mark its frames as waiting.
        struct Candidate final {
            const scene::ClipPlacement *clip{};
            std::string source;
            core::i64 offset{};
        };
        std::vector<Candidate> candidates;
        std::vector<std::string> sources;
        for (const scene::ClipPlacement &clip : stage_clips) {
            const auto element = std::find_if(
                inputs.elements.begin(), inputs.elements.end(),
                [&](const editor::ElementInfo &info) noexcept {
                    return info.node == clip.node;
                });
            if (element != inputs.elements.end() &&
                !element->effective_visible) {
                continue;
            }
            const auto lane = std::find_if(
                inputs.stage_lanes.begin(), inputs.stage_lanes.end(),
                [&](const scene::TimelineLane &candidate) noexcept {
                    return candidate.id == clip.lane;
                });
            if (lane != inputs.stage_lanes.end() && lane->muted) {
                continue;
            }
            if (clip.timing.placed()) {
                const core::i64 end = clip.timing.start + clip.timing.duration;
                if (end <= static_cast<core::i64>(window_first) ||
                    clip.timing.start > static_cast<core::i64>(window_last)) {
                    continue;
                }
            }
            const auto decoded = timeline_decode_sources.find(clip.node);
            Candidate candidate{&clip, clip.source, 0};
            if (decoded != timeline_decode_sources.end()) {
                candidate.source = decoded->second.decode_source;
                candidate.offset = decoded->second.offset;
            }
            if (std::find(sources.begin(), sources.end(), candidate.source) ==
                sources.end()) {
                sources.push_back(candidate.source);
            }
            candidates.push_back(std::move(candidate));
        }
        // A busy scheduler leaves the previous snapshot standing; it is a
        // frame old, which is all the bar can claim anyway.
        std::vector<render::MediaReadyFrames> ready;
        if (render::media_decode_ready_frames(sources, ready)) {
            timeline_ready_sources = std::move(sources);
            timeline_ready_frames = std::move(ready);
        }
        std::vector<BufferedClip> videos;
        videos.reserve(candidates.size());
        for (const Candidate &candidate : candidates) {
            const auto found =
                std::find(timeline_ready_sources.begin(),
                          timeline_ready_sources.end(), candidate.source);
            if (found == timeline_ready_sources.end()) {
                continue;
            }
            const render::MediaReadyFrames &frames =
                timeline_ready_frames[static_cast<std::size_t>(
                    found - timeline_ready_sources.begin())];
            if (!frames.video) {
                continue;
            }
            videos.push_back(
                BufferedClip{candidate.clip, frames.frames, candidate.offset});
        }
        for (const ShowFrameRun &run :
             buffered_show_frames(videos, rate, window_first, window_last)) {
            timeline.buffered.push_back(
                editor::TimelineFrameRun{run.first, run.last});
        }
    } catch (const std::bad_alloc &) {
        timeline.buffered.clear();
        std::cerr << "could not project the Timeline cache bar\n";
    }

    // The nodes play Program. Their position belongs on this timeline only
    // while the Stage shows that same scene. One playhead, not one per node:
    // the Render Server marked as the main node in Hardware Map, or the only
    // live node when none is marked.
    if (tick.stage_scene_frame.id == tick.scene_frame.id) {
        try {
            const std::vector<net::DirectorNodePlayhead> live =
                barrier.runtime_playheads();
            const std::optional<core::u32> main =
                topology::main_render_node(tick.hardware_map);
            const auto shown = std::find_if(
                live.begin(), live.end(),
                [&](const net::DirectorNodePlayhead &node) noexcept {
                    return main.has_value() ? node.node_index == *main
                                            : live.size() == 1U;
                });
            if (shown != live.end()) {
                timeline.node_playheads.push_back(editor::TimelineNodePlayhead{
                    shown->node_index,
                    lifecycle_controller
                        ? lifecycle_controller->node_name(shown->node_index)
                        : std::string{},
                    shown->playhead.show_frame,
                    shown->playhead.latency_frames});
            }
        } catch (const std::bad_alloc &) {
            timeline.node_playheads.clear();
        }
    }
}

PhaseExit DirectorShowLoop::observe_timeline_filmstrips(
    EditorViewInputs &inputs) {
    const auto &stage_clips = inputs.stage_clips;
    const auto &project_view = inputs.project_view;
    auto &filmstrip_observations = inputs.filmstrip_observations;
    assets::AssetRegistry &asset_registry = director_asset_registry;
    if (thumbnail_service) {
        editor->reset_timeline_filmstrip_textures(
            project_view.generation);
        const assets::DerivativeProfile *const profile =
            assets::derivative_profile(
                assets::DerivativeProfileId::Filmstrip8x160x90Srgb);
        if (profile != nullptr) {
            try {
                filmstrip_observations.reserve(stage_clips.size());
                for (const scene::ClipPlacement &clip : stage_clips) {
                    const doc::Node *const clip_node =
                        scheduler.projected_document().get(clip.node);
                    if (clip_node == nullptr) {
                        continue;
                    }
                    const auto asset_property =
                        clip_node->properties.find(
                            scene::schema::asset_id);
                    const auto *const asset_reference =
                        asset_property != clip_node->properties.end()
                            ? std::get_if<std::int64_t>(
                                  &asset_property->second)
                            : nullptr;
                    if (asset_reference == nullptr ||
                        *asset_reference <= 0) {
                        continue;
                    }
                    const doc::NodeId asset_id{
                        static_cast<core::u64>(*asset_reference)};
                    const assets::Asset *const asset =
                        asset_registry.asset(asset_id);
                    const auto *const media =
                        asset != nullptr
                            ? std::get_if<assets::MediaAsset>(asset)
                            : nullptr;
                    if (media == nullptr ||
                        !media->fingerprint.has_value()) {
                        continue;
                    }
                    const assets::DerivativeKey key =
                        assets::make_derivative_key(
                            media->id, *media->fingerprint, *profile);
                    assets::DerivativeState state =
                        assets::DerivativeState::Missing;
                    const rhi::Texture *texture =
                        editor->timeline_filmstrip_texture(key);
                    if (const auto snapshot =
                            thumbnail_service->snapshot(key);
                        snapshot.has_value()) {
                        state = snapshot->state;
                        if (state == assets::DerivativeState::Ready &&
                            texture == nullptr &&
                            std::find(retained_filmstrip_keys.begin(),
                                      retained_filmstrip_keys.end(),
                                      key) !=
                                retained_filmstrip_keys.end()) {
                            auto loaded = thumbnail_service->load(key);
                            if (loaded &&
                                editor->upload_timeline_filmstrip(
                                    key, loaded.value().payload)) {
                                texture =
                                    editor->timeline_filmstrip_texture(
                                        key);
                            } else {
                                state = assets::DerivativeState::Failed;
                            }
                        }
                    }
                    filmstrip_observations.push_back(
                        editor::TimelineFilmstripObservation{
                            clip.node, key, state, texture});
                }
            } catch (const std::bad_alloc &) {
                std::cerr << "could not project Timeline filmstrip "
                             "observations\n";
                return 1;
            }
        }
    }
    return std::nullopt;
}

void DirectorShowLoop::project_asset_viewer(EditorViewInputs &inputs) {
    editor::AssetViewerInput &viewer = inputs.asset_viewer;
    if (plateset_viewed.has_value() && plateset_motion != nullptr) {
        viewer.plateset = *plateset_viewed;
        viewer.plateset_analysis = plateset_motion->view(*plateset_viewed);
    }
    if (!asset_viewer_request.has_value() ||
        !asset_viewer_frame.has_value()) {
        return;
    }
    viewer.scene = &*asset_viewer_frame;
    viewer.shown = *asset_viewer_request;
    viewer.rendition = asset_viewer_rendition;
    // The asset's filmstrip, loaded onto the editor's device once it is
    // ready and the editor is retaining it, as a Timeline clip's is.
    const assets::Asset *const asset =
        director_asset_registry.asset(asset_viewer_request->asset);
    const auto *const media =
        asset != nullptr ? std::get_if<assets::MediaAsset>(asset) : nullptr;
    const assets::DerivativeProfile *const profile =
        assets::derivative_profile(
            assets::DerivativeProfileId::Filmstrip8x160x90Srgb);
    if (!thumbnail_service || media == nullptr ||
        !media->fingerprint.has_value() || profile == nullptr) {
        return;
    }
    editor::AssetViewerFilmstrip filmstrip{};
    filmstrip.key =
        assets::make_derivative_key(media->id, *media->fingerprint, *profile);
    filmstrip.texture = editor->timeline_filmstrip_texture(filmstrip.key);
    if (const auto snapshot = thumbnail_service->snapshot(filmstrip.key);
        snapshot.has_value()) {
        filmstrip.state = snapshot->state;
        if (filmstrip.state == assets::DerivativeState::Ready &&
            filmstrip.texture == nullptr &&
            std::find(retained_filmstrip_keys.begin(),
                      retained_filmstrip_keys.end(),
                      filmstrip.key) != retained_filmstrip_keys.end()) {
            auto loaded = thumbnail_service->load(filmstrip.key);
            if (loaded && editor->upload_timeline_filmstrip(
                              filmstrip.key, loaded.value().payload)) {
                filmstrip.texture =
                    editor->timeline_filmstrip_texture(filmstrip.key);
            } else {
                filmstrip.state = assets::DerivativeState::Failed;
            }
        }
    }
    viewer.filmstrip = filmstrip;
}

PhaseExit DirectorShowLoop::project_output_media(DirectorTick &tick) {
    const auto &now = tick.now;
    auto &hardware_map = tick.hardware_map;
    auto &asset_manifest = tick.asset_manifest;
    assets::AssetRegistry &asset_registry = director_asset_registry;
    if (now >= next_output_media_projection) {
        try {
            project_output_node_media_runtime(
                asset_registry, asset_manifest.digest, hardware_map,
                barrier, library_proxy_runtime);
            // Beside what the output nodes report: the editor's own proxy
            // of each asset, made here for Stage, Program and the Asset
            // Viewer.
            if (director_proxy_generator != nullptr) {
                for (editor::LibraryProxyRuntimeObservation &observation :
                     library_proxy_runtime) {
                    const auto status =
                        director_proxy_generator->status_for(
                            observation.asset);
                    if (!status.has_value()) {
                        continue;
                    }
                    using From = ProxyGenerationWorker::AssetStatus::State;
                    using To = editor::LibraryEditorProxyState;
                    const To state =
                        status->state == From::Queued   ? To::Queued
                        : status->state == From::Making ? To::Making
                        : status->state == From::Ready  ? To::Ready
                                                        : To::Failed;
                    observation.editor_proxy = editor::LibraryEditorProxy{
                        state, status->made_frames, status->total_frames};
                }
            }
        } catch (const std::bad_alloc &) {
            std::cerr << "could not project output-node media runtime "
                         "observations\n";
            return 1;
        }
        next_output_media_projection =
            now + library_proxy_retry_interval;
    }
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_editor_services(
    EditorViewInputs &inputs, editor::EditorFrame &editor_frame) {
    const auto &renderer = inputs.renderer;
    if (editor_frame.renderer_readiness.has_value()) {
        const render::RendererReadiness &observed =
            *editor_frame.renderer_readiness;
        observed_local_renderer = net::RuntimeRendererReadiness{
            scene::renderer_settings_digest(renderer),
            runtime_renderer_state(observed.code),
            runtime_renderer_backend(observed.requested),
            runtime_renderer_backend(observed.selected),
            observed.missing_capabilities,
            observed.satisfied,
            observed.fallback,
            editor_frame.assets,
            editor_frame.temporal_restarts,
            editor_frame.temporal_restart_frame,
            core::RenderConformance{.adapter = editor_frame.render_adapter},
            editor_frame.frame_timing,
        };
    }
    if (editor_frame.check_render_conformance) {
        ++conformance_request;
        barrier.request_render_conformance(conformance_request);
        observed_local_renderer.conformance.state = core::ConformanceState::Unknown;
        std::cout << "render conformance requested: " << conformance_request << '\n';
    }
    if (editor_frame.save_workspace_layout.has_value() &&
        workspace_layout_file.has_value()) {
        const auto saved_layout = persist::save_workspace_layout(
            *workspace_layout_file,
            *editor_frame.save_workspace_layout);
        if (!saved_layout) {
            std::cerr
                << "could not save workspace layout (code "
                << static_cast<unsigned>(saved_layout.error().code)
                << ')';
            if (saved_layout.error().system_error) {
                std::cerr
                    << ": "
                    << saved_layout.error().system_error.message();
            }
            std::cerr << '\n';
        }
    }
    if (editor_frame.close_semantic_coalescing) {
        editor_coalescing_policy.close();
        scheduler.end_coalescing();
        pending_editor_coalescing_close = true;
    }
    if (editor_frame.semantic_coalescing.has_value()) {
        const editor::SemanticCoalescingRequest &request =
            *editor_frame.semantic_coalescing;
        const EditCoalescingIdentity identity{
            undo_context_for(request.context),
            cmd::CoalescingKey{request.key.focus, request.key.selection,
                               request.key.property,
                               request.key.direction,
                               request.key.modifiers}};
        try {
            EditCoalescingPlan plan = editor_coalescing_policy.observe(
                identity, cmd::TransactionId{request.gesture},
                Clock::now());
            if (plan.action == EditCoalescingAction::Begin) {
                // Close the previous semantic stream even when every
                // payload in this new repeat later proves to be a
                // no-op.
                scheduler.end_coalescing();
            }
            pending_editor_coalescing.insert_or_assign(request.gesture,
                                                       std::move(plan));
            // A same-frame semantic repeat follows (rather than
            // precedes) any focus/pointer boundary reported with it.
            pending_editor_coalescing_close = false;
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue numeric edit coalescing\n";
            return 1;
        }
    }
    return std::nullopt;
}

PhaseExit DirectorShowLoop::request_library_thumbnails(
    editor::EditorFrame &editor_frame) {
    assets::AssetRegistry &asset_registry = director_asset_registry;
    if (thumbnail_service) {
        for (const editor::LibraryThumbnailRequestIntent &request :
             editor_frame.thumbnail_requests) {
            const assets::Asset *const asset =
                asset_registry.asset(request.key.asset);
            const auto *const media =
                asset != nullptr
                    ? std::get_if<assets::MediaAsset>(asset)
                    : nullptr;
            if (media == nullptr || !media->fingerprint.has_value()) {
                continue;
            }
            const assets::DerivativeProfile *const profile =
                assets::derivative_profile(
                    assets::DerivativeProfileId::Thumbnail320x180Srgb);
            if (profile == nullptr ||
                assets::make_derivative_key(media->id,
                                            *media->fingerprint,
                                            *profile) != request.key) {
                continue;
            }
            const server::ThumbnailRequestPriority priority =
                request.priority ==
                        editor::LibraryThumbnailRequestPriority::Visible
                    ? server::ThumbnailRequestPriority::Visible
                    : server::ThumbnailRequestPriority::Prefetch;
            const auto scheduled =
                thumbnail_service->request(*media, priority);
            if (!scheduled) {
                std::cerr
                    << "could not request Library thumbnail for "
                    << media->id.value() << " (code "
                    << static_cast<unsigned>(scheduled.error().code)
                    << ")\n";
            }
        }
        // Models are drawn by the editor, which has the GPU: the service
        // hands out what is waiting, as much as the editor has room for,
        // and takes the pictures back to encode and cache.
        for (const editor::LibraryThumbnailRequestIntent &request :
             editor_frame.thumbnail_requests) {
            const assets::Asset *const asset =
                asset_registry.asset(request.key.asset);
            const auto *const model =
                asset != nullptr
                    ? std::get_if<assets::ModelAsset>(asset)
                    : nullptr;
            if (model == nullptr || !model->fingerprint.has_value()) {
                continue;
            }
            const assets::DerivativeProfile *const profile =
                assets::derivative_profile(
                    assets::DerivativeProfileId::ModelTurntable320x180Srgb);
            if (profile == nullptr ||
                assets::make_derivative_key(model->id,
                                            *model->fingerprint,
                                            *profile) != request.key) {
                continue;
            }
            const auto scheduled = thumbnail_service->request_model(
                *model,
                request.priority ==
                        editor::LibraryThumbnailRequestPriority::Visible
                    ? server::ThumbnailRequestPriority::Visible
                    : server::ThumbnailRequestPriority::Prefetch);
            if (!scheduled) {
                std::cerr << "could not request a model thumbnail for "
                          << model->id.value() << " (code "
                          << static_cast<unsigned>(scheduled.error().code)
                          << ")\n";
            }
        }
        if (const core::u32 room = editor->model_thumbnail_room();
            room != 0U) {
            for (server::ModelThumbnailRender &render :
                 thumbnail_service->take_model_renders(room)) {
                if (!editor->queue_model_thumbnail(render.key,
                                                   std::move(render.source))) {
                    thumbnail_service->complete_model_render(render.key, {});
                }
            }
        }
        for (editor::RenderedModelThumbnail &drawn :
             editor->take_model_thumbnails()) {
            thumbnail_service->complete_model_render(drawn.key,
                                                     std::move(drawn.rgba));
        }
        for (const editor::TimelineFilmstripRequestIntent &request :
             editor_frame.filmstrip_requests) {
            const assets::Asset *const asset =
                asset_registry.asset(request.key.asset);
            const auto *const media =
                asset != nullptr
                    ? std::get_if<assets::MediaAsset>(asset)
                    : nullptr;
            if (media == nullptr || !media->fingerprint.has_value()) {
                continue;
            }
            const assets::DerivativeProfile *const profile =
                assets::derivative_profile(
                    assets::DerivativeProfileId::Filmstrip8x160x90Srgb);
            if (profile == nullptr ||
                assets::make_derivative_key(media->id,
                                            *media->fingerprint,
                                            *profile) != request.key) {
                continue;
            }
            const server::ThumbnailRequestPriority priority =
                request.priority ==
                        editor::TimelineFilmstripRequestPriority::
                            Visible
                    ? server::ThumbnailRequestPriority::Visible
                    : server::ThumbnailRequestPriority::Prefetch;
            const auto scheduled =
                thumbnail_service->request_filmstrip(*media, priority);
            if (!scheduled) {
                std::cerr
                    << "could not request Timeline filmstrip for "
                    << media->id.value() << " (code "
                    << static_cast<unsigned>(scheduled.error().code)
                    << ")\n";
            }
        }
        if (editor_frame.thumbnail_retain.has_value()) {
            try {
                retained_thumbnail_keys =
                    editor_frame.thumbnail_retain->keys;
            } catch (const std::bad_alloc &) {
                std::cerr
                    << "could not retain Library thumbnail keys\n";
                return 1;
            }
            editor->retain_library_thumbnail_textures(
                retained_thumbnail_keys);
        }
        if (editor_frame.filmstrip_retain.has_value()) {
            try {
                retained_filmstrip_keys =
                    editor_frame.filmstrip_retain->keys;
            } catch (const std::bad_alloc &) {
                std::cerr
                    << "could not retain Timeline filmstrip keys\n";
                return 1;
            }
            editor->retain_timeline_filmstrip_textures(
                retained_filmstrip_keys);
        }
        try {
            std::vector<assets::DerivativeKey> retained_derivatives =
                retained_thumbnail_keys;
            for (const assets::DerivativeKey &key :
                 retained_filmstrip_keys) {
                if (std::find(retained_derivatives.begin(),
                              retained_derivatives.end(),
                              key) == retained_derivatives.end()) {
                    retained_derivatives.push_back(key);
                }
            }
            thumbnail_service->retain(retained_derivatives);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not retain thumbnail derivative keys\n";
            return 1;
        }
    }
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_preferences_request(
    editor::EditorFrame &editor_frame) {
    if (editor_frame.save_preferences.has_value()) {
        const editor::EditorPreferences &wanted =
            *editor_frame.save_preferences;
        // Onto the record as it is now, not as this run found it:
        // the render-machine settings belong to that window, which
        // may have saved since this editor started.
        persist::Preferences next = options.stored_preferences;
        if (!options.preferences_file.empty()) {
            if (auto current =
                    persist::load_preferences(options.preferences_file)) {
                next = std::move(current).value();
            }
        }
        next.director_listen = wanted.director_listen;
        next.show_id = wanted.show_id;
        next.director_epoch = wanted.director_epoch;
        // A newly typed password is kept only as the key it
        // derives; an untouched one keeps the stored key.
        const bool password_changed =
            editor_frame.render_node_password.has_value();
        next.lifecycle_password_key =
            !password_changed
                ? stored_password_key
                : (editor_frame.render_node_password->empty()
                       ? std::string{}
                       : net::lifecycle_key_from_password(
                             *editor_frame.render_node_password));
        next.wal_path = wanted.wal_path;
        next.proxy_cache_directory = wanted.proxy_cache_directory;
        next.proxy_cache_next_to_source =
            wanted.proxy_cache_next_to_source;
        next.proxy_cache_gibibytes = wanted.proxy_cache_gibibytes;
        next.output_buffer_frames = wanted.output_buffer_frames;
        next.output_read_ahead_frames = wanted.output_read_ahead_frames;
        next.playback_cache_mebibytes = wanted.playback_cache_mebibytes;
        next.playback_trail_frames = wanted.playback_trail_frames;
        switch (wanted.proxy_transcode_priority) {
        case editor::EditorPreferences::TranscodePriority::Background:
            next.proxy_transcode_priority =
                persist::ProxyTranscodePriority::Background;
            break;
        case editor::EditorPreferences::TranscodePriority::Balanced:
            next.proxy_transcode_priority =
                persist::ProxyTranscodePriority::Balanced;
            break;
        case editor::EditorPreferences::TranscodePriority::Fast:
            next.proxy_transcode_priority =
                persist::ProxyTranscodePriority::Fast;
            break;
        }
        if (!valid_proxy_cache_preference(
                next.proxy_cache_directory,
                next.proxy_cache_next_to_source)) {
            preferences_view.message =
                "PROXY CACHE FOLDER MUST BE AN ABSOLUTE FOLDER";
        } else if (!next.valid_tuning()) {
            preferences_view.message =
                "PLAYBACK SETTINGS ARE OUTSIDE SUPPORTED LIMITS";
        } else if (options.preferences_file.empty()) {
            preferences_view.message =
                "NO PER-USER SETTINGS LOCATION ON THIS MACHINE";
        } else if (auto saved = persist::save_preferences(
                       options.preferences_file, next)) {
            // The panel keeps editing the stored record, so adopt it
            // immediately; the running process keeps its own values
            // until it is restarted, which the panel already says.
            preferences_view.stored = wanted;
            preferences_view.message = "SAVED - APPLIES AT NEXT LAUNCH";
            stored_password_key = next.lifecycle_password_key;
            // The password is the one setting that applies at once:
            // every render machine is asked again with the new key.
            // A password given on the command line keeps this run.
            if (password_changed &&
                !options.lifecycle_password_from_command_line &&
                lifecycle_controller) {
                const std::string key =
                    next.lifecycle_password_key.empty()
                        ? net::lifecycle_key_from_password({})
                        : next.lifecycle_password_key;
                lifecycle_controller->set_shared_secret(key);
                preferences_view.active.render_node_password =
                    !next.lifecycle_password_key.empty();
                preferences_view.message =
                    "SAVED - RENDER NODE PASSWORD APPLIED NOW; OTHER "
                    "CHANGES APPLY AT NEXT LAUNCH";
            }
        } else {
            preferences_view.message = "COULD NOT SAVE PREFERENCES";
            std::cerr << "could not save preferences (code "
                      << static_cast<unsigned>(saved.error().code)
                      << ')';
            if (saved.error().system_error) {
                std::cerr << ": "
                          << saved.error().system_error.message();
            }
            std::cerr << '\n';
        }
    }
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_proxy_cache_clear(
    editor::EditorFrame &editor_frame) {
    assets::AssetRegistry &asset_registry = director_asset_registry;
    if (editor_frame.clear_proxy_cache) {
        // Stop the only writer and join any materialization read before
        // removing service-owned cache children. The long-lived
        // playback handle reconciles its index on the next inspect.
        director_proxy_generator.reset();
        director_proxy_playback.reset();
        const ProxyCacheClearResult cleared =
            clear_proxy_cache(options, asset_registry);
        if (cleared) {
            preferences_view.message = "PROXY CACHE CLEARED";
            if (cleared.roots_cleared > 1U) {
                preferences_view.message += " - ";
                preferences_view.message +=
                    std::to_string(cleared.roots_cleared);
                preferences_view.message += " LOCATIONS";
            }
        } else {
            preferences_view.message = "COULD NOT CLEAR PROXY CACHE";
            std::cerr << "could not clear proxy cache: "
                      << cleared.error.message() << '\n';
        }

        auto generation_artifacts =
            open_proxy_playback_artifacts(options, "proxy generator");
        if (generation_artifacts != nullptr) {
            director_proxy_generator =
                std::make_unique<ProxyGenerationWorker>(
                    std::move(generation_artifacts));
            for (const assets::Asset &entry :
                 director_playback_asset_registry.assets) {
                const auto *const media =
                    std::get_if<assets::MediaAsset>(&entry);
                if (media != nullptr &&
                    media->requested_proxy_profile ==
                        assets::DerivativeProfileId::Proxy720pCfrH264) {
                    director_proxy_generator->request(*media);
                }
            }
        }
    }
    return std::nullopt;
}

void DirectorShowLoop::refresh_history_state() {
    // Describing a context walks its retained steps, so it happens only
    // when that context's history actually moved, never per frame.
    const auto refresh = [&](cmd::UndoContextId context,
                             editor::HistoryContextState &state) {
        if (scheduler.history_revision(context) != state.revision) {
            state = history_descriptions.describe(scheduler, context);
        }
    };
    refresh(scene_undo_context, history_state.scene);
    refresh(compositor_undo_context, history_state.compositor);
}

} // namespace mediaserver::app
