#include "director_compositor_authoring.hpp"

#include <mediaserver/compositor/compositor_schema.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace mediaserver::app {
namespace {

constexpr core::u64 max_reference =
    static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());

[[nodiscard]] bool referenceable(compositor::EdgeEndpoint endpoint) noexcept {
    return endpoint.node != doc::NodeId{} &&
           endpoint.node.value() <= max_reference;
}

} // namespace

[[nodiscard]] doc::Node make_compositor_edge(doc::NodeId id,
                                             compositor::EdgeEndpoint from,
                                             compositor::EdgeEndpoint to) {
    doc::Node edge{};
    edge.id = id;
    edge.type = std::string{compositor::schema::edge_type};
    edge.properties.emplace(
        std::string{compositor::schema::from_node},
        doc::PropertyValue{static_cast<std::int64_t>(from.node.value())});
    edge.properties.emplace(
        std::string{compositor::schema::from_port},
        doc::PropertyValue{static_cast<std::int64_t>(from.port)});
    edge.properties.emplace(
        std::string{compositor::schema::to_node},
        doc::PropertyValue{static_cast<std::int64_t>(to.node.value())});
    edge.properties.emplace(
        std::string{compositor::schema::to_port},
        doc::PropertyValue{static_cast<std::int64_t>(to.port)});
    edge.properties.emplace(std::string{compositor::schema::order},
                            doc::PropertyValue{std::int64_t{0}});
    return edge;
}

// Gives each existing wire its new order, skipping anything that is not a
// wire or already has it.
[[nodiscard]] bool
renumber_edges(cmd::DirectorScheduler &scheduler,
               std::span<const std::pair<doc::NodeId, core::i64>> orders,
               cmd::TransactionId transaction, cmd::FrameNumber apply_at) {
    for (const auto &[edge_id, order] : orders) {
        const doc::Node *const existing =
            scheduler.projected_document().get(edge_id);
        if (existing == nullptr ||
            existing->type != compositor::schema::edge_type) {
            continue;
        }
        doc::Node renumbered = *existing;
        renumbered.properties.insert_or_assign(
            std::string{compositor::schema::order}, doc::PropertyValue{order});
        if (renumbered != *existing &&
            !scheduler.schedule_edit(cmd::PutNode{std::move(renumbered)},
                                     cmd::PutNode{*existing},
                                     cmd::UndoContextId{6U}, transaction,
                                     apply_at)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string_view
compositor_type(compositor::NodeKind kind) noexcept {
    switch (kind) {
    case compositor::NodeKind::SceneLayer:
        return compositor::schema::scene_layer_type;
    case compositor::NodeKind::Brightness:
        return compositor::schema::brightness_type;
    case compositor::NodeKind::Saturation:
        return compositor::schema::saturation_type;
    case compositor::NodeKind::Levels:
        return compositor::schema::levels_type;
    case compositor::NodeKind::LiftGammaGain:
        return compositor::schema::lift_gamma_gain_type;
    case compositor::NodeKind::ContrastPivot:
        return compositor::schema::contrast_pivot_type;
    case compositor::NodeKind::ColorMatrix:
        return compositor::schema::color_matrix_type;
    case compositor::NodeKind::WhiteBalance:
        return compositor::schema::white_balance_type;
    case compositor::NodeKind::Curves:
        return compositor::schema::curves_type;
    case compositor::NodeKind::FilmGrain:
        return compositor::schema::film_grain_type;
    case compositor::NodeKind::Blur:
        return compositor::schema::blur_type;
    case compositor::NodeKind::BoxBlur:
        return compositor::schema::box_blur_type;
    case compositor::NodeKind::DirectionalBlur:
        return compositor::schema::directional_blur_type;
    case compositor::NodeKind::ChromaKey:
        return compositor::schema::chroma_key_type;
    case compositor::NodeKind::LumaKey:
        return compositor::schema::luma_key_type;
    case compositor::NodeKind::Matte:
        return compositor::schema::matte_type;
    case compositor::NodeKind::RadialArray:
        return compositor::schema::radial_array_type;
    case compositor::NodeKind::LedCarProcess:
        return compositor::schema::led_car_process_type;
    case compositor::NodeKind::Composite:
        return compositor::schema::composite_type;
    case compositor::NodeKind::Output:
        return compositor::schema::output_type;
    case compositor::NodeKind::Merge:
        return compositor::schema::merge_type;
    case compositor::NodeKind::Background:
        return compositor::schema::background_type;
    }
    return compositor::schema::brightness_type;
}

[[nodiscard]] doc::Node
make_compositor_node(doc::NodeId id, const editor::CompositorAddIntent &request,
                     doc::NodeId scene) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{compositor_type(request.kind)};
    // Graph objects are owned by the scene they were authored in, so a node
    // added while rehearsing composites Preview and leaves Program alone.
    node.properties.emplace(
        std::string{compositor::schema::scene_id},
        doc::PropertyValue{static_cast<std::int64_t>(scene.value())});
    node.properties.emplace(std::string{compositor::schema::label},
                            doc::PropertyValue{std::string{
                                compositor::node_kind_name(request.kind)}});
    node.properties.emplace(
        std::string{compositor::schema::editor_x},
        doc::PropertyValue{static_cast<double>(request.editor_x)});
    node.properties.emplace(
        std::string{compositor::schema::editor_y},
        doc::PropertyValue{static_cast<double>(request.editor_y)});
    switch (request.kind) {
    case compositor::NodeKind::SceneLayer:
        node.properties.emplace(std::string{compositor::schema::scene_node},
                                doc::PropertyValue{static_cast<std::int64_t>(
                                    request.scene_node.value())});
        break;
    case compositor::NodeKind::Brightness:
        node.properties.emplace(std::string{compositor::schema::amount},
                                doc::PropertyValue{1.0});
        break;
    case compositor::NodeKind::Saturation:
        node.properties.emplace(std::string{compositor::schema::amount},
                                doc::PropertyValue{1.0});
        break;
    case compositor::NodeKind::Levels: {
        const auto add_channel =
            [&](std::string_view input_black, std::string_view input_white,
                std::string_view gamma, std::string_view output_black,
                std::string_view output_white) {
                node.properties.emplace(std::string{input_black},
                                        doc::PropertyValue{0.0});
                node.properties.emplace(std::string{input_white},
                                        doc::PropertyValue{1.0});
                node.properties.emplace(std::string{gamma},
                                        doc::PropertyValue{1.0});
                node.properties.emplace(std::string{output_black},
                                        doc::PropertyValue{0.0});
                node.properties.emplace(std::string{output_white},
                                        doc::PropertyValue{1.0});
            };
        add_channel(compositor::schema::levels_master_input_black,
                    compositor::schema::levels_master_input_white,
                    compositor::schema::levels_master_gamma,
                    compositor::schema::levels_master_output_black,
                    compositor::schema::levels_master_output_white);
        add_channel(compositor::schema::levels_red_input_black,
                    compositor::schema::levels_red_input_white,
                    compositor::schema::levels_red_gamma,
                    compositor::schema::levels_red_output_black,
                    compositor::schema::levels_red_output_white);
        add_channel(compositor::schema::levels_green_input_black,
                    compositor::schema::levels_green_input_white,
                    compositor::schema::levels_green_gamma,
                    compositor::schema::levels_green_output_black,
                    compositor::schema::levels_green_output_white);
        add_channel(compositor::schema::levels_blue_input_black,
                    compositor::schema::levels_blue_input_white,
                    compositor::schema::levels_blue_gamma,
                    compositor::schema::levels_blue_output_black,
                    compositor::schema::levels_blue_output_white);
        break;
    }
    case compositor::NodeKind::LiftGammaGain:
        node.properties.emplace(std::string{compositor::schema::lift_r},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::lift_g},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::lift_b},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::gamma_r},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::gamma_g},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::gamma_b},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::gain_r},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::gain_g},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::gain_b},
                                doc::PropertyValue{1.0});
        break;
    case compositor::NodeKind::ContrastPivot:
        node.properties.emplace(std::string{compositor::schema::contrast},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::pivot},
                                doc::PropertyValue{0.5});
        break;
    case compositor::NodeKind::ColorMatrix:
        node.properties.emplace(std::string{compositor::schema::m00},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::m01},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::m02},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::m10},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::m11},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::m12},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::m20},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::m21},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::m22},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::offset_r},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::offset_g},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::offset_b},
                                doc::PropertyValue{0.0});
        break;
    case compositor::NodeKind::WhiteBalance:
        node.properties.emplace(
            std::string{compositor::schema::temperature_kelvin},
            doc::PropertyValue{static_cast<double>(
                scene::d65_white_balance_temperature_kelvin)});
        node.properties.emplace(std::string{compositor::schema::tint},
                                doc::PropertyValue{0.0});
        break;
    case compositor::NodeKind::Curves:
        node.properties.emplace(
            std::string{compositor::schema::curve_master_prefix} + "0" +
                std::string{compositor::schema::curve_x_suffix},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::curve_master_prefix} + "0" +
                std::string{compositor::schema::curve_y_suffix},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::curve_master_prefix} + "1" +
                std::string{compositor::schema::curve_x_suffix},
            doc::PropertyValue{1.0});
        node.properties.emplace(
            std::string{compositor::schema::curve_master_prefix} + "1" +
                std::string{compositor::schema::curve_y_suffix},
            doc::PropertyValue{1.0});
        break;
    case compositor::NodeKind::FilmGrain:
        node.properties.emplace(std::string{compositor::schema::amount},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::pixel_scale},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::chroma_ratio},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::seed},
                                doc::PropertyValue{std::int64_t{0}});
        node.properties.emplace(std::string{compositor::schema::mode},
                                doc::PropertyValue{std::string{
                                    compositor::schema::monochrome_mode}});
        break;
    case compositor::NodeKind::Blur:
        node.properties.emplace(std::string{compositor::schema::radius},
                                doc::PropertyValue{8.0});
        break;
    case compositor::NodeKind::BoxBlur:
        node.properties.emplace(
            std::string{compositor::schema::box_blur_radius},
            doc::PropertyValue{std::int64_t{8}});
        node.properties.emplace(
            std::string{compositor::schema::box_blur_passes},
            doc::PropertyValue{std::int64_t{1}});
        break;
    case compositor::NodeKind::DirectionalBlur:
        node.properties.emplace(
            std::string{compositor::schema::directional_blur_length},
            doc::PropertyValue{16.0});
        node.properties.emplace(
            std::string{compositor::schema::directional_blur_angle_turns},
            doc::PropertyValue{0.0});
        break;
    case compositor::NodeKind::ChromaKey:
        node.properties.emplace(
            std::string{compositor::schema::chroma_key_color_r},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::chroma_key_color_g},
            doc::PropertyValue{1.0});
        node.properties.emplace(
            std::string{compositor::schema::chroma_key_color_b},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::chroma_key_tolerance},
            doc::PropertyValue{0.1});
        node.properties.emplace(
            std::string{compositor::schema::chroma_key_softness},
            doc::PropertyValue{0.1});
        node.properties.emplace(
            std::string{compositor::schema::chroma_key_morphology_px},
            doc::PropertyValue{std::int64_t{0}});
        node.properties.emplace(
            std::string{compositor::schema::chroma_key_spill_suppression},
            doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::chroma_key_mix},
                                doc::PropertyValue{1.0});
        break;
    case compositor::NodeKind::LumaKey:
        node.properties.emplace(
            std::string{compositor::schema::luma_key_low_threshold},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::luma_key_high_threshold},
            doc::PropertyValue{1.0});
        node.properties.emplace(
            std::string{compositor::schema::luma_key_low_softness},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::luma_key_high_softness},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::luma_key_invert},
            doc::PropertyValue{false});
        node.properties.emplace(std::string{compositor::schema::luma_key_mix},
                                doc::PropertyValue{1.0});
        break;
    case compositor::NodeKind::Matte:
        node.properties.emplace(std::string{compositor::schema::matte_mode},
                                doc::PropertyValue{std::string{
                                    compositor::schema::matte_alpha_mode}});
        node.properties.emplace(std::string{compositor::schema::matte_invert},
                                doc::PropertyValue{false});
        node.properties.emplace(
            std::string{compositor::schema::matte_clamp_input},
            doc::PropertyValue{true});
        break;
    case compositor::NodeKind::RadialArray:
        node.properties.emplace(std::string{compositor::schema::count},
                                doc::PropertyValue{std::int64_t{9}});
        node.properties.emplace(std::string{compositor::schema::radius},
                                doc::PropertyValue{4.0});
        break;
    case compositor::NodeKind::LedCarProcess:
        node.properties.emplace(std::string{compositor::schema::position_x},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::position_y},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::position_z},
                                doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::rotation_yaw_turns},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::rotation_pitch_turns},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::rotation_roll_turns},
            doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::plate_scale},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::array_scale},
                                doc::PropertyValue{4.0});
        node.properties.emplace(
            std::string{compositor::schema::reflection_height},
            doc::PropertyValue{4.0});
        node.properties.emplace(
            std::string{compositor::schema::reflection_scale},
            doc::PropertyValue{1.0});
        node.properties.emplace(
            std::string{compositor::schema::horizontal_blend},
            doc::PropertyValue{0.05});
        break;
    case compositor::NodeKind::Merge:
        // A Merge with only BASE wired changes nothing until a blend that
        // uses its constant is chosen, so adding one never flashes Program.
        node.properties.emplace(
            std::string{compositor::schema::blend_mode},
            doc::PropertyValue{std::string{compositor::schema::normal_blend}});
        node.properties.emplace(std::string{compositor::schema::opacity},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::value},
                                doc::PropertyValue{1.0});
        break;
    case compositor::NodeKind::Background: {
        const compositor::BackgroundParameters defaults{};
        node.properties.emplace(
            std::string{compositor::schema::background_mode},
            doc::PropertyValue{
                std::string{compositor::schema::solid_background}});
        const auto color = [&node](const compositor::BackgroundColor& value,
                                   std::string_view r, std::string_view g,
                                   std::string_view b, std::string_view a) {
            node.properties.emplace(std::string{r},
                                    doc::PropertyValue{double{value.r}});
            node.properties.emplace(std::string{g},
                                    doc::PropertyValue{double{value.g}});
            node.properties.emplace(std::string{b},
                                    doc::PropertyValue{double{value.b}});
            node.properties.emplace(std::string{a},
                                    doc::PropertyValue{double{value.a}});
        };
        color(defaults.color_a, compositor::schema::color_a_r,
              compositor::schema::color_a_g, compositor::schema::color_a_b,
              compositor::schema::color_a_a);
        color(defaults.color_b, compositor::schema::color_b_r,
              compositor::schema::color_b_g, compositor::schema::color_b_b,
              compositor::schema::color_b_a);
        node.properties.emplace(std::string{compositor::schema::angle_turns},
                                doc::PropertyValue{double{defaults.angle}});
        node.properties.emplace(std::string{compositor::schema::center_x},
                                doc::PropertyValue{double{defaults.center_x}});
        node.properties.emplace(std::string{compositor::schema::center_y},
                                doc::PropertyValue{double{defaults.center_y}});
        node.properties.emplace(
            std::string{compositor::schema::gradient_size},
            doc::PropertyValue{double{defaults.size}});
        break;
    }
    case compositor::NodeKind::Composite:
    case compositor::NodeKind::Output:
        break;
    }
    return node;
}

[[nodiscard]] bool
schedule_compositor_add(cmd::DirectorScheduler &scheduler,
                        const editor::CompositorAddIntent &request,
                        doc::NodeId id, doc::NodeId scene, core::u64 gesture,
                        cmd::FrameNumber apply_at) {
    if (request.kind == compositor::NodeKind::SceneLayer &&
        request.scene_node == doc::NodeId{}) {
        return true;
    }
    if (scene == doc::NodeId{}) {
        return true;
    }
    doc::Node added = make_compositor_node(id, request, scene);
    if (request.kind == compositor::NodeKind::SceneLayer) {
        const doc::Node *const source =
            scheduler.projected_document().get(request.scene_node);
        if (source != nullptr &&
            source->type == mediaserver::scene::schema::timeline_lane_type) {
            std::string label{"Track"};
            const auto name =
                source->properties.find(mediaserver::scene::schema::lane_name);
            if (name != source->properties.end()) {
                if (const auto *const text =
                        std::get_if<std::string>(&name->second);
                    text != nullptr && !text->empty()) {
                    label += ": ";
                    label += *text;
                }
            }
            added.properties.insert_or_assign(
                std::string{compositor::schema::label},
                doc::PropertyValue{std::move(label)});
        }
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(added)}, cmd::EraseNode{id},
        cmd::UndoContextId{6U}, cmd::TransactionId{gesture}, apply_at));
}

[[nodiscard]] bool
schedule_compositor_move(cmd::DirectorScheduler &scheduler,
                         const editor::CompositorMoveIntent &request,
                         core::u64 gesture, cmd::FrameNumber apply_at,
                         EditCoalescingPlan *coalescing) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.node);
    if (existing == nullptr) {
        return true;
    }
    doc::Node moved = *existing;
    moved.properties.insert_or_assign(
        std::string{compositor::schema::editor_x},
        doc::PropertyValue{static_cast<double>(request.editor_x)});
    moved.properties.insert_or_assign(
        std::string{compositor::schema::editor_y},
        doc::PropertyValue{static_cast<double>(request.editor_y)});
    if (moved == *existing) {
        return true;
    }
    if (coalescing != nullptr) {
        return static_cast<bool>(schedule_editor_coalesced_edit(
            scheduler, cmd::PutNode{std::move(moved)}, cmd::PutNode{*existing},
            *coalescing, apply_at));
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(moved)}, cmd::PutNode{*existing},
        cmd::UndoContextId{6U}, cmd::TransactionId{gesture}, apply_at));
}

[[nodiscard]] bool
schedule_compositor_rename(cmd::DirectorScheduler &scheduler,
                           const editor::CompositorRenameIntent &request,
                           core::u64 gesture, cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.node);
    if (existing == nullptr ||
        (!compositor::schema::is_node_type(existing->type) &&
         existing->type != compositor::schema::comment_type)) {
        return true;
    }
    doc::Node renamed = *existing;
    if (request.label.empty()) {
        renamed.properties.erase(std::string{compositor::schema::label});
    } else {
        renamed.properties.insert_or_assign(
            std::string{compositor::schema::label},
            doc::PropertyValue{request.label});
    }
    if (renamed == *existing) {
        return true;
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(renamed)}, cmd::PutNode{*existing},
        cmd::UndoContextId{6U}, cmd::TransactionId{gesture}, apply_at));
}

[[nodiscard]] bool
schedule_compositor_comment(cmd::DirectorScheduler &scheduler,
                            const editor::CompositorCommentIntent &request,
                            doc::NodeId id, doc::NodeId scene,
                            core::u64 gesture, cmd::FrameNumber apply_at) {
    const auto within = [](float value) noexcept {
        return std::isfinite(value) &&
               std::abs(value) <= compositor::max_spatial_extent;
    };
    if (!within(request.x) || !within(request.y) || !within(request.width) ||
        !within(request.height) ||
        request.width < compositor::min_comment_extent ||
        request.height < compositor::min_comment_extent ||
        request.color >= compositor::comment_color_count) {
        return true;
    }
    const doc::Node *const existing =
        request.node == doc::NodeId{}
            ? nullptr
            : scheduler.projected_document().get(request.node);
    if (request.node != doc::NodeId{} &&
        (existing == nullptr ||
         existing->type != compositor::schema::comment_type)) {
        return true;
    }
    if (existing == nullptr && scene == doc::NodeId{}) {
        return true;
    }
    doc::Node comment = existing != nullptr ? *existing : doc::Node{};
    if (existing == nullptr) {
        comment.id = id;
        comment.type = std::string{compositor::schema::comment_type};
        comment.properties.insert_or_assign(
            std::string{compositor::schema::scene_id},
            doc::PropertyValue{static_cast<std::int64_t>(scene.value())});
    }
    const auto put = [&comment](std::string_view name, doc::PropertyValue value) {
        comment.properties.insert_or_assign(std::string{name}, std::move(value));
    };
    put(compositor::schema::label,
        doc::PropertyValue{request.title.empty() ? std::string{"Comment"}
                                                 : request.title});
    put(compositor::schema::editor_x,
        doc::PropertyValue{static_cast<double>(request.x)});
    put(compositor::schema::editor_y,
        doc::PropertyValue{static_cast<double>(request.y)});
    put(compositor::schema::comment_width,
        doc::PropertyValue{static_cast<double>(request.width)});
    put(compositor::schema::comment_height,
        doc::PropertyValue{static_cast<double>(request.height)});
    put(compositor::schema::comment_color,
        doc::PropertyValue{static_cast<std::int64_t>(request.color)});
    if (existing != nullptr && comment == *existing) {
        return true;
    }
    return static_cast<bool>(
        existing != nullptr
            ? scheduler.schedule_edit(cmd::PutNode{std::move(comment)},
                                      cmd::PutNode{*existing},
                                      cmd::UndoContextId{6U},
                                      cmd::TransactionId{gesture}, apply_at)
            : scheduler.schedule_edit(cmd::PutNode{std::move(comment)},
                                      cmd::EraseNode{id},
                                      cmd::UndoContextId{6U},
                                      cmd::TransactionId{gesture}, apply_at));
}

[[nodiscard]] bool
schedule_compositor_reorder(cmd::DirectorScheduler &scheduler,
                            const editor::CompositorReorderIntent &request,
                            core::u64 gesture, cmd::FrameNumber apply_at) {
    return renumber_edges(scheduler, request.orders,
                          cmd::TransactionId{gesture}, apply_at);
}

namespace {

// Sets or clears one flag on each graph node `accepts`, in one transaction.
// Clearing removes the property, so an untouched node reads as it was.
template <typename Accepts>
[[nodiscard]] bool schedule_node_flag(cmd::DirectorScheduler &scheduler,
                                      std::span<const doc::NodeId> nodes,
                                      std::string_view property, bool value,
                                      Accepts accepts, core::u64 gesture,
                                      cmd::FrameNumber apply_at) {
    for (const doc::NodeId id : nodes) {
        const doc::Node *const existing = scheduler.projected_document().get(id);
        if (existing == nullptr ||
            !compositor::schema::is_node_type(existing->type) ||
            !accepts(*existing)) {
            continue;
        }
        doc::Node changed = *existing;
        if (value) {
            changed.properties.insert_or_assign(std::string{property},
                                                doc::PropertyValue{true});
        } else {
            changed.properties.erase(std::string{property});
        }
        if (changed == *existing) {
            continue;
        }
        if (!scheduler.schedule_edit(cmd::PutNode{std::move(changed)},
                                     cmd::PutNode{*existing},
                                     cmd::UndoContextId{6U},
                                     cmd::TransactionId{gesture}, apply_at)) {
            return false;
        }
    }
    return true;
}

} // namespace

[[nodiscard]] bool
schedule_compositor_collapse(cmd::DirectorScheduler &scheduler,
                             const editor::CompositorCollapseIntent &request,
                             core::u64 gesture, cmd::FrameNumber apply_at) {
    return schedule_node_flag(
        scheduler, request.nodes, compositor::schema::collapsed,
        request.collapsed, [](const doc::Node &) noexcept { return true; },
        gesture, apply_at);
}

[[nodiscard]] bool
schedule_compositor_bypass(cmd::DirectorScheduler &scheduler,
                           const editor::CompositorBypassIntent &request,
                           core::u64 gesture, cmd::FrameNumber apply_at) {
    return schedule_node_flag(
        scheduler, request.nodes, compositor::schema::bypassed,
        request.bypassed,
        [](const doc::Node &node) noexcept {
            for (std::size_t index = 0U; index < compositor::node_kind_count;
                 ++index) {
                const auto kind = static_cast<compositor::NodeKind>(index);
                if (compositor_type(kind) == node.type) {
                    return compositor::node_kind_bypassable(kind);
                }
            }
            return false;
        },
        gesture, apply_at);
}

[[nodiscard]] std::pair<float, float>
compositor_snapshot_origin(std::span<const doc::Node> snapshot) noexcept {
    const auto number = [](const doc::Node &node, std::string_view name) {
        const auto found = node.properties.find(name);
        if (found == node.properties.end()) {
            return 0.0F;
        }
        if (const auto *const real = std::get_if<double>(&found->second)) {
            return static_cast<float>(*real);
        }
        if (const auto *const whole = std::get_if<std::int64_t>(&found->second)) {
            return static_cast<float>(*whole);
        }
        return 0.0F;
    };
    float x = std::numeric_limits<float>::infinity();
    float y = std::numeric_limits<float>::infinity();
    for (const doc::Node &node : snapshot) {
        if (node.type == compositor::schema::edge_type) {
            continue;
        }
        x = std::min(x, number(node, compositor::schema::editor_x));
        y = std::min(y, number(node, compositor::schema::editor_y));
    }
    return std::isfinite(x) && std::isfinite(y) ? std::pair{x, y}
                                                : std::pair{0.0F, 0.0F};
}

[[nodiscard]] bool
schedule_compositor_paste(cmd::DirectorScheduler &scheduler,
                          std::span<const doc::Node> puts, core::u64 gesture,
                          cmd::FrameNumber apply_at, core::u64 &next_id) {
    for (const doc::Node &node : puts) {
        if (!scheduler.schedule_edit(cmd::PutNode{node}, cmd::EraseNode{node.id},
                                     cmd::UndoContextId{6U},
                                     cmd::TransactionId{gesture}, apply_at)) {
            return false;
        }
        next_id = std::max(next_id, node.id.value() + 1U);
    }
    return true;
}

[[nodiscard]] bool schedule_compositor_property(
    cmd::DirectorScheduler &scheduler,
    const editor::CompositorPropertyEditIntent &request, core::u64 gesture,
    cmd::FrameNumber apply_at, EditCoalescingPlan *coalescing) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.node);
    if (existing == nullptr) {
        return true;
    }

    if (request.property == editor::CompositorProperty::CurvesParameters) {
        const auto *const requested =
            std::get_if<compositor::CurvesParameters>(&request.value);
        const auto normalized = requested != nullptr
                                    ? scene::normalized_curves(*requested)
                                    : std::optional<scene::CurvesParameters>{};
        if (!normalized.has_value() ||
            existing->type != compositor::schema::curves_type) {
            return true;
        }
        doc::Node changed = *existing;
        constexpr std::array<std::string_view, 4U> prefixes{{
            compositor::schema::curve_master_prefix,
            compositor::schema::curve_red_prefix,
            compositor::schema::curve_green_prefix,
            compositor::schema::curve_blue_prefix,
        }};
        const auto indexed_property = [](std::string_view prefix,
                                         std::size_t index,
                                         std::string_view suffix) {
            return std::string{prefix} + std::to_string(index) +
                   std::string{suffix};
        };
        for (const std::string_view prefix : prefixes) {
            for (std::size_t index = 0U;
                 index < scene::max_curve_control_points; ++index) {
                changed.properties.erase(indexed_property(
                    prefix, index, compositor::schema::curve_x_suffix));
                changed.properties.erase(indexed_property(
                    prefix, index, compositor::schema::curve_y_suffix));
            }
        }
        const auto write_channel =
            [&](std::string_view prefix,
                const scene::CurveChannelParameters &channel) {
                for (std::size_t index = 0U; index < channel.points.size();
                     ++index) {
                    changed.properties.insert_or_assign(
                        indexed_property(prefix, index,
                                         compositor::schema::curve_x_suffix),
                        doc::PropertyValue{
                            static_cast<double>(channel.points[index].x)});
                    changed.properties.insert_or_assign(
                        indexed_property(prefix, index,
                                         compositor::schema::curve_y_suffix),
                        doc::PropertyValue{
                            static_cast<double>(channel.points[index].y)});
                }
            };
        write_channel(compositor::schema::curve_master_prefix,
                      normalized->master);
        if (normalized->red.has_value()) {
            write_channel(compositor::schema::curve_red_prefix,
                          *normalized->red);
        }
        if (normalized->green.has_value()) {
            write_channel(compositor::schema::curve_green_prefix,
                          *normalized->green);
        }
        if (normalized->blue.has_value()) {
            write_channel(compositor::schema::curve_blue_prefix,
                          *normalized->blue);
        }
        if (coalescing != nullptr) {
            return static_cast<bool>(schedule_editor_coalesced_edit(
                scheduler, cmd::PutNode{std::move(changed)},
                cmd::PutNode{*existing}, *coalescing, apply_at));
        }
        return static_cast<bool>(scheduler.schedule_edit(
            cmd::PutNode{std::move(changed)}, cmd::PutNode{*existing},
            cmd::UndoContextId{6U}, cmd::TransactionId{gesture}, apply_at));
    }

    std::string_view property;
    bool integer_value = false;
    bool film_grain_mode_value = false;
    bool signed_integer_value = false;
    bool switch_value = false;
    bool matte_mode_value = false;
    bool merge_blend_value = false;
    bool background_mode_value = false;
    switch (request.property) {
    case editor::CompositorProperty::BrightnessAmount:
    case editor::CompositorProperty::SaturationAmount:
        property = compositor::schema::amount;
        break;
    case editor::CompositorProperty::LevelsMasterInputBlack:
        property = compositor::schema::levels_master_input_black;
        break;
    case editor::CompositorProperty::LevelsMasterInputWhite:
        property = compositor::schema::levels_master_input_white;
        break;
    case editor::CompositorProperty::LevelsMasterGamma:
        property = compositor::schema::levels_master_gamma;
        break;
    case editor::CompositorProperty::LevelsMasterOutputBlack:
        property = compositor::schema::levels_master_output_black;
        break;
    case editor::CompositorProperty::LevelsMasterOutputWhite:
        property = compositor::schema::levels_master_output_white;
        break;
    case editor::CompositorProperty::LevelsRedInputBlack:
        property = compositor::schema::levels_red_input_black;
        break;
    case editor::CompositorProperty::LevelsRedInputWhite:
        property = compositor::schema::levels_red_input_white;
        break;
    case editor::CompositorProperty::LevelsRedGamma:
        property = compositor::schema::levels_red_gamma;
        break;
    case editor::CompositorProperty::LevelsRedOutputBlack:
        property = compositor::schema::levels_red_output_black;
        break;
    case editor::CompositorProperty::LevelsRedOutputWhite:
        property = compositor::schema::levels_red_output_white;
        break;
    case editor::CompositorProperty::LevelsGreenInputBlack:
        property = compositor::schema::levels_green_input_black;
        break;
    case editor::CompositorProperty::LevelsGreenInputWhite:
        property = compositor::schema::levels_green_input_white;
        break;
    case editor::CompositorProperty::LevelsGreenGamma:
        property = compositor::schema::levels_green_gamma;
        break;
    case editor::CompositorProperty::LevelsGreenOutputBlack:
        property = compositor::schema::levels_green_output_black;
        break;
    case editor::CompositorProperty::LevelsGreenOutputWhite:
        property = compositor::schema::levels_green_output_white;
        break;
    case editor::CompositorProperty::LevelsBlueInputBlack:
        property = compositor::schema::levels_blue_input_black;
        break;
    case editor::CompositorProperty::LevelsBlueInputWhite:
        property = compositor::schema::levels_blue_input_white;
        break;
    case editor::CompositorProperty::LevelsBlueGamma:
        property = compositor::schema::levels_blue_gamma;
        break;
    case editor::CompositorProperty::LevelsBlueOutputBlack:
        property = compositor::schema::levels_blue_output_black;
        break;
    case editor::CompositorProperty::LevelsBlueOutputWhite:
        property = compositor::schema::levels_blue_output_white;
        break;
    case editor::CompositorProperty::LiftGammaGainLiftR:
        property = compositor::schema::lift_r;
        break;
    case editor::CompositorProperty::LiftGammaGainLiftG:
        property = compositor::schema::lift_g;
        break;
    case editor::CompositorProperty::LiftGammaGainLiftB:
        property = compositor::schema::lift_b;
        break;
    case editor::CompositorProperty::LiftGammaGainGammaR:
        property = compositor::schema::gamma_r;
        break;
    case editor::CompositorProperty::LiftGammaGainGammaG:
        property = compositor::schema::gamma_g;
        break;
    case editor::CompositorProperty::LiftGammaGainGammaB:
        property = compositor::schema::gamma_b;
        break;
    case editor::CompositorProperty::LiftGammaGainGainR:
        property = compositor::schema::gain_r;
        break;
    case editor::CompositorProperty::LiftGammaGainGainG:
        property = compositor::schema::gain_g;
        break;
    case editor::CompositorProperty::LiftGammaGainGainB:
        property = compositor::schema::gain_b;
        break;
    case editor::CompositorProperty::ContrastPivotContrast:
        property = compositor::schema::contrast;
        break;
    case editor::CompositorProperty::ContrastPivotPivot:
        property = compositor::schema::pivot;
        break;
    case editor::CompositorProperty::ColorMatrixM00:
        property = compositor::schema::m00;
        break;
    case editor::CompositorProperty::ColorMatrixM01:
        property = compositor::schema::m01;
        break;
    case editor::CompositorProperty::ColorMatrixM02:
        property = compositor::schema::m02;
        break;
    case editor::CompositorProperty::ColorMatrixM10:
        property = compositor::schema::m10;
        break;
    case editor::CompositorProperty::ColorMatrixM11:
        property = compositor::schema::m11;
        break;
    case editor::CompositorProperty::ColorMatrixM12:
        property = compositor::schema::m12;
        break;
    case editor::CompositorProperty::ColorMatrixM20:
        property = compositor::schema::m20;
        break;
    case editor::CompositorProperty::ColorMatrixM21:
        property = compositor::schema::m21;
        break;
    case editor::CompositorProperty::ColorMatrixM22:
        property = compositor::schema::m22;
        break;
    case editor::CompositorProperty::ColorMatrixOffsetR:
        property = compositor::schema::offset_r;
        break;
    case editor::CompositorProperty::ColorMatrixOffsetG:
        property = compositor::schema::offset_g;
        break;
    case editor::CompositorProperty::ColorMatrixOffsetB:
        property = compositor::schema::offset_b;
        break;
    case editor::CompositorProperty::WhiteBalanceTemperatureKelvin:
        property = compositor::schema::temperature_kelvin;
        break;
    case editor::CompositorProperty::WhiteBalanceTint:
        property = compositor::schema::tint;
        break;
    case editor::CompositorProperty::CurvesParameters:
        // Handled as one typed payload above.
        return true;
    case editor::CompositorProperty::FilmGrainAmount:
        property = compositor::schema::amount;
        break;
    case editor::CompositorProperty::FilmGrainPixelScale:
        property = compositor::schema::pixel_scale;
        break;
    case editor::CompositorProperty::FilmGrainChromaRatio:
        property = compositor::schema::chroma_ratio;
        break;
    case editor::CompositorProperty::FilmGrainSeed:
        property = compositor::schema::seed;
        integer_value = true;
        break;
    case editor::CompositorProperty::FilmGrainMode:
        property = compositor::schema::mode;
        film_grain_mode_value = true;
        break;
    case editor::CompositorProperty::BlurRadius:
    case editor::CompositorProperty::RadialRadius:
        property = compositor::schema::radius;
        break;
    case editor::CompositorProperty::BoxBlurRadius:
        property = compositor::schema::box_blur_radius;
        integer_value = true;
        break;
    case editor::CompositorProperty::BoxBlurPasses:
        property = compositor::schema::box_blur_passes;
        integer_value = true;
        break;
    case editor::CompositorProperty::DirectionalBlurLength:
        property = compositor::schema::directional_blur_length;
        break;
    case editor::CompositorProperty::DirectionalBlurAngleTurns:
        property = compositor::schema::directional_blur_angle_turns;
        break;
    case editor::CompositorProperty::RadialCount:
        property = compositor::schema::count;
        integer_value = true;
        break;
    case editor::CompositorProperty::RadialStartAngleTurns:
        property = compositor::schema::start_angle_turns;
        break;
    case editor::CompositorProperty::RadialCenterX:
        property = compositor::schema::center_x;
        break;
    case editor::CompositorProperty::RadialCenterY:
        property = compositor::schema::center_y;
        break;
    case editor::CompositorProperty::LedCarPositionX:
        property = compositor::schema::position_x;
        break;
    case editor::CompositorProperty::LedCarPositionY:
        property = compositor::schema::position_y;
        break;
    case editor::CompositorProperty::LedCarPositionZ:
        property = compositor::schema::position_z;
        break;
    case editor::CompositorProperty::LedCarYawTurns:
        property = compositor::schema::rotation_yaw_turns;
        break;
    case editor::CompositorProperty::LedCarPitchTurns:
        property = compositor::schema::rotation_pitch_turns;
        break;
    case editor::CompositorProperty::LedCarRollTurns:
        property = compositor::schema::rotation_roll_turns;
        break;
    case editor::CompositorProperty::LedCarPlateScale:
        property = compositor::schema::plate_scale;
        break;
    case editor::CompositorProperty::LedCarArrayScale:
        property = compositor::schema::array_scale;
        break;
    case editor::CompositorProperty::LedCarReflectionHeight:
        property = compositor::schema::reflection_height;
        break;
    case editor::CompositorProperty::LedCarReflectionScale:
        property = compositor::schema::reflection_scale;
        break;
    case editor::CompositorProperty::LedCarHorizontalBlend:
        property = compositor::schema::horizontal_blend;
        break;
    case editor::CompositorProperty::ChromaKeyRed:
        property = compositor::schema::chroma_key_color_r;
        break;
    case editor::CompositorProperty::ChromaKeyGreen:
        property = compositor::schema::chroma_key_color_g;
        break;
    case editor::CompositorProperty::ChromaKeyBlue:
        property = compositor::schema::chroma_key_color_b;
        break;
    case editor::CompositorProperty::ChromaKeyTolerance:
        property = compositor::schema::chroma_key_tolerance;
        break;
    case editor::CompositorProperty::ChromaKeySoftness:
        property = compositor::schema::chroma_key_softness;
        break;
    case editor::CompositorProperty::ChromaKeyEdgePx:
        property = compositor::schema::chroma_key_morphology_px;
        signed_integer_value = true;
        break;
    case editor::CompositorProperty::ChromaKeySpill:
        property = compositor::schema::chroma_key_spill_suppression;
        break;
    case editor::CompositorProperty::ChromaKeyMix:
        property = compositor::schema::chroma_key_mix;
        break;
    case editor::CompositorProperty::LumaKeyLow:
        property = compositor::schema::luma_key_low_threshold;
        break;
    case editor::CompositorProperty::LumaKeyHigh:
        property = compositor::schema::luma_key_high_threshold;
        break;
    case editor::CompositorProperty::LumaKeyLowSoftness:
        property = compositor::schema::luma_key_low_softness;
        break;
    case editor::CompositorProperty::LumaKeyHighSoftness:
        property = compositor::schema::luma_key_high_softness;
        break;
    case editor::CompositorProperty::LumaKeyInvert:
        property = compositor::schema::luma_key_invert;
        switch_value = true;
        break;
    case editor::CompositorProperty::LumaKeyMix:
        property = compositor::schema::luma_key_mix;
        break;
    case editor::CompositorProperty::MatteMode:
        property = compositor::schema::matte_mode;
        matte_mode_value = true;
        break;
    case editor::CompositorProperty::MatteInvert:
        property = compositor::schema::matte_invert;
        switch_value = true;
        break;
    case editor::CompositorProperty::MatteClampInput:
        property = compositor::schema::matte_clamp_input;
        switch_value = true;
        break;
    case editor::CompositorProperty::MergeBlendMode:
        property = compositor::schema::blend_mode;
        merge_blend_value = true;
        break;
    case editor::CompositorProperty::MergeOpacity:
        property = compositor::schema::opacity;
        break;
    case editor::CompositorProperty::MergeValue:
        property = compositor::schema::value;
        break;
    case editor::CompositorProperty::BackgroundMode:
        property = compositor::schema::background_mode;
        background_mode_value = true;
        break;
    case editor::CompositorProperty::BackgroundColorAR:
        property = compositor::schema::color_a_r;
        break;
    case editor::CompositorProperty::BackgroundColorAG:
        property = compositor::schema::color_a_g;
        break;
    case editor::CompositorProperty::BackgroundColorAB:
        property = compositor::schema::color_a_b;
        break;
    case editor::CompositorProperty::BackgroundColorAA:
        property = compositor::schema::color_a_a;
        break;
    case editor::CompositorProperty::BackgroundColorBR:
        property = compositor::schema::color_b_r;
        break;
    case editor::CompositorProperty::BackgroundColorBG:
        property = compositor::schema::color_b_g;
        break;
    case editor::CompositorProperty::BackgroundColorBB:
        property = compositor::schema::color_b_b;
        break;
    case editor::CompositorProperty::BackgroundColorBA:
        property = compositor::schema::color_b_a;
        break;
    case editor::CompositorProperty::BackgroundAngleTurns:
        property = compositor::schema::angle_turns;
        break;
    case editor::CompositorProperty::BackgroundCenterX:
        property = compositor::schema::center_x;
        break;
    case editor::CompositorProperty::BackgroundCenterY:
        property = compositor::schema::center_y;
        break;
    case editor::CompositorProperty::BackgroundSize:
        property = compositor::schema::gradient_size;
        break;
    }

    doc::PropertyValue value;
    if (merge_blend_value || background_mode_value) {
        static constexpr std::array<std::string_view,
                                    compositor::merge_blend_mode_count>
            blends{{compositor::schema::normal_blend,
                    compositor::schema::add_blend,
                    compositor::schema::multiply_blend,
                    compositor::schema::screen_blend,
                    compositor::schema::subtract_blend,
                    compositor::schema::overlay_blend,
                    compositor::schema::lighten_blend,
                    compositor::schema::darken_blend,
                    compositor::schema::difference_blend}};
        static constexpr std::array<std::string_view, 3U> backgrounds{
            {compositor::schema::solid_background,
             compositor::schema::linear_background,
             compositor::schema::radial_background}};
        const std::span<const std::string_view> names =
            merge_blend_value ? std::span<const std::string_view>{blends}
                              : std::span<const std::string_view>{backgrounds};
        const auto *const number = std::get_if<core::u32>(&request.value);
        if (number == nullptr || *number >= names.size()) {
            return true;
        }
        value = std::string{names[*number]};
    } else if (switch_value || matte_mode_value) {
        const auto *const number = std::get_if<core::u32>(&request.value);
        if (number == nullptr || *number > 1U) {
            return true;
        }
        if (matte_mode_value) {
            value = std::string{*number == 0U
                                    ? compositor::schema::matte_alpha_mode
                                    : compositor::schema::matte_luma_mode};
        } else {
            value = *number == 1U;
        }
    } else if (signed_integer_value) {
        const auto *const number = std::get_if<float>(&request.value);
        if (number == nullptr || !std::isfinite(*number)) {
            return true;
        }
        value = static_cast<std::int64_t>(std::lround(*number));
    } else if (film_grain_mode_value) {
        const auto *const number = std::get_if<core::u32>(&request.value);
        if (number == nullptr || *number > 1U) {
            return true;
        }
        value = std::string{*number == 0U ? compositor::schema::monochrome_mode
                                          : compositor::schema::rgb_mode};
    } else if (integer_value) {
        const auto *const number = std::get_if<core::u32>(&request.value);
        if (number == nullptr) {
            return true;
        }
        value = static_cast<std::int64_t>(*number);
    } else {
        const auto *const number = std::get_if<float>(&request.value);
        if (number == nullptr || !std::isfinite(*number)) {
            return true;
        }
        value = static_cast<double>(*number);
    }

    doc::Node changed = *existing;
    changed.properties.insert_or_assign(std::string{property},
                                        std::move(value));
    // A luma key's low threshold may not pass its high one; the graph would
    // refuse the node rather than guess which the operator meant.
    if (changed.type == compositor::schema::luma_key_type) {
        const auto threshold = [&](std::string_view name, double fallback) {
            const auto found = changed.properties.find(name);
            const auto *const real =
                found != changed.properties.end()
                    ? std::get_if<double>(&found->second)
                    : nullptr;
            return real != nullptr ? *real : fallback;
        };
        if (threshold(compositor::schema::luma_key_low_threshold, 0.0) >
            threshold(compositor::schema::luma_key_high_threshold, 1.0)) {
            return true;
        }
    }
    if (coalescing != nullptr) {
        return static_cast<bool>(schedule_editor_coalesced_edit(
            scheduler, cmd::PutNode{std::move(changed)},
            cmd::PutNode{*existing}, *coalescing, apply_at));
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(changed)}, cmd::PutNode{*existing},
        cmd::UndoContextId{6U}, cmd::TransactionId{gesture}, apply_at));
}

[[nodiscard]] bool
schedule_compositor_connect(cmd::DirectorScheduler &scheduler,
                            const editor::CompositorConnectIntent &request,
                            doc::NodeId id, core::u64 gesture,
                            cmd::FrameNumber apply_at) {
    if (request.from.node.value() > max_reference ||
        request.to.node.value() > max_reference) {
        return true;
    }
    const cmd::UndoContextId context{6U};
    const cmd::TransactionId transaction{gesture};
    if (request.replace_edge.has_value()) {
        const doc::Node *const replaced =
            scheduler.projected_document().get(*request.replace_edge);
        if (replaced != nullptr) {
            const auto target =
                replaced->properties.find(compositor::schema::to_node);
            const auto port =
                replaced->properties.find(compositor::schema::to_port);
            const auto *const target_value =
                target == replaced->properties.end()
                    ? nullptr
                    : std::get_if<std::int64_t>(&target->second);
            const auto *const port_value =
                port == replaced->properties.end()
                    ? nullptr
                    : std::get_if<std::int64_t>(&port->second);
            const bool matches_target =
                replaced->type == compositor::schema::edge_type &&
                target_value != nullptr && *target_value > 0 &&
                static_cast<core::u64>(*target_value) ==
                    request.to.node.value() &&
                port_value != nullptr && *port_value >= 0 &&
                static_cast<core::u64>(*port_value) == request.to.port;
            if (!matches_target) {
                return true;
            }
            if (!scheduler.schedule_edit(cmd::EraseNode{replaced->id},
                                         cmd::PutNode{*replaced}, context,
                                         transaction, apply_at)) {
                return false;
            }
        }
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{make_compositor_edge(id, request.from, request.to)},
        cmd::EraseNode{id}, context, transaction, apply_at));
}

[[nodiscard]] std::optional<doc::NodeId>
node_reference(const doc::Node &node, std::string_view property) noexcept {
    const auto found = node.properties.find(property);
    if (found == node.properties.end()) {
        return std::nullopt;
    }
    const auto *const value = std::get_if<std::int64_t>(&found->second);
    if (value == nullptr || *value <= 0) {
        return std::nullopt;
    }
    return doc::NodeId{static_cast<core::u64>(*value)};
}

[[nodiscard]] bool schedule_erase_node(cmd::DirectorScheduler &scheduler,
                                       doc::NodeId id,
                                       cmd::UndoContextId context,
                                       cmd::TransactionId transaction,
                                       cmd::FrameNumber apply_at) {
    const doc::Node *const existing = scheduler.projected_document().get(id);
    if (existing == nullptr) {
        return true;
    }
    return static_cast<bool>(
        scheduler.schedule_edit(cmd::EraseNode{id}, cmd::PutNode{*existing},
                                context, transaction, apply_at));
}

namespace {

[[nodiscard]] bool
schedule_compositor_delete_one(cmd::DirectorScheduler &scheduler,
                               const editor::CompositorSelection &selected,
                               core::u64 gesture, cmd::FrameNumber apply_at) {
    if (selected.kind == editor::CompositorSelectionKind::None ||
        selected.node == doc::NodeId{}) {
        return true;
    }
    const doc::Root &root = scheduler.projected_document();
    const doc::Node *const target = root.get(selected.node);
    if (target == nullptr) {
        return true;
    }
    const doc::NodeId target_id = target->id;
    const std::string target_type = target->type;
    const cmd::UndoContextId context{6U};
    const cmd::TransactionId transaction{gesture};
    if (selected.kind == editor::CompositorSelectionKind::Edge) {
        return target_type != compositor::schema::edge_type ||
               schedule_erase_node(scheduler, target_id, context, transaction,
                                   apply_at);
    }
    if (selected.kind == editor::CompositorSelectionKind::Comment) {
        return target_type != compositor::schema::comment_type ||
               schedule_erase_node(scheduler, target_id, context, transaction,
                                   apply_at);
    }
    // Output is structural: every newly-authored composition receives one and
    // the editor presents it as required rather than as a disposable effect.
    if (target_type == compositor::schema::output_type) {
        return true;
    }
    if (!compositor::schema::is_node_type(target_type)) {
        return true;
    }

    try {
        std::vector<doc::NodeId> incident;
        const doc::Delta contents = doc::diff(doc::Root{}, root);
        for (const doc::NodeChange &change : contents.changes) {
            if (!change.after ||
                change.after->type != compositor::schema::edge_type) {
                continue;
            }
            const auto from =
                node_reference(*change.after, compositor::schema::from_node);
            const auto to =
                node_reference(*change.after, compositor::schema::to_node);
            if ((from && *from == target_id) || (to && *to == target_id)) {
                incident.push_back(change.after->id);
            }
        }
        for (const doc::NodeId edge : incident) {
            if (!schedule_erase_node(scheduler, edge, context, transaction,
                                     apply_at)) {
                return false;
            }
        }
        return schedule_erase_node(scheduler, target_id, context, transaction,
                                   apply_at);
    } catch (const std::bad_alloc &) {
        return false;
    }
}

} // namespace

[[nodiscard]] bool
schedule_compositor_delete(cmd::DirectorScheduler &scheduler,
                           const editor::CompositorDeleteIntent &request,
                           core::u64 gesture, cmd::FrameNumber apply_at) {
    for (const editor::CompositorSelection &target : request.targets) {
        if (!schedule_compositor_delete_one(scheduler, target, gesture,
                                            apply_at)) {
            return false;
        }
    }
    return true;
}

namespace {

// Erases each id that names a wire, as part of one transaction. Anything else
// - a node, or an id already gone - is left alone.
[[nodiscard]] bool erase_edges(cmd::DirectorScheduler &scheduler,
                               std::span<const doc::NodeId> edges,
                               cmd::TransactionId transaction,
                               cmd::FrameNumber apply_at) {
    for (const doc::NodeId edge : edges) {
        const doc::Node *const existing =
            scheduler.projected_document().get(edge);
        if (existing == nullptr ||
            existing->type != compositor::schema::edge_type) {
            continue;
        }
        if (!schedule_erase_node(scheduler, edge, cmd::UndoContextId{6U},
                                 transaction, apply_at)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool graph_node(const doc::Root &root, doc::NodeId id) noexcept {
    const doc::Node *const node = root.get(id);
    return node != nullptr && node->type != compositor::schema::edge_type &&
           compositor::schema::is_node_type(node->type);
}

} // namespace

[[nodiscard]] bool schedule_compositor_add_linked(
    cmd::DirectorScheduler &scheduler,
    const editor::CompositorAddIntent &request, doc::NodeId id,
    doc::NodeId scene, std::span<const doc::NodeId> edge_ids,
    core::u64 gesture, cmd::FrameNumber apply_at) {
    if (edge_ids.size() < request.links.size()) {
        return false;
    }
    if (!schedule_compositor_add(scheduler, request, id, scene, gesture,
                                 apply_at)) {
        return false;
    }
    // A node that could not be added - a Scene Layer with nothing to show,
    // or no scene being authored - takes its wires and removals with it.
    if (!graph_node(scheduler.projected_document(), id)) {
        return true;
    }
    const cmd::TransactionId transaction{gesture};
    if (!erase_edges(scheduler, request.remove_edges, transaction, apply_at)) {
        return false;
    }
    if (!renumber_edges(scheduler, request.reorder, transaction, apply_at)) {
        return false;
    }
    for (std::size_t index = 0U; index < request.links.size(); ++index) {
        const editor::CompositorAddLink &link = request.links[index];
        if (!referenceable(link.existing) ||
            !graph_node(scheduler.projected_document(), link.existing.node)) {
            continue;
        }
        const compositor::EdgeEndpoint fresh{id, link.port};
        const doc::NodeId edge = edge_ids[index];
        doc::Node wire = make_compositor_edge(
            edge, link.from_new ? fresh : link.existing,
            link.from_new ? link.existing : fresh);
        wire.properties.insert_or_assign(std::string{compositor::schema::order},
                                         doc::PropertyValue{link.order});
        if (!scheduler.schedule_edit(cmd::PutNode{std::move(wire)},
                                     cmd::EraseNode{edge},
                                     cmd::UndoContextId{6U}, transaction,
                                     apply_at)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool
schedule_compositor_rewire(cmd::DirectorScheduler &scheduler,
                           const editor::CompositorRewireIntent &request,
                           std::span<const doc::NodeId> edge_ids,
                           core::u64 gesture, cmd::FrameNumber apply_at) {
    if (edge_ids.size() < request.add_edges.size()) {
        return false;
    }
    const cmd::TransactionId transaction{gesture};
    if (!erase_edges(scheduler, request.remove_edges, transaction, apply_at)) {
        return false;
    }
    for (std::size_t index = 0U; index < request.add_edges.size(); ++index) {
        const editor::CompositorWire &wire = request.add_edges[index];
        const doc::Root &root = scheduler.projected_document();
        if (!referenceable(wire.from) || !referenceable(wire.to) ||
            !graph_node(root, wire.from.node) ||
            !graph_node(root, wire.to.node)) {
            continue;
        }
        const doc::NodeId edge = edge_ids[index];
        if (!scheduler.schedule_edit(
                cmd::PutNode{make_compositor_edge(edge, wire.from, wire.to)},
                cmd::EraseNode{edge}, cmd::UndoContextId{6U}, transaction,
                apply_at)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool
schedule_compositor_waypoints(cmd::DirectorScheduler &scheduler,
                              const editor::CompositorWaypointIntent &request,
                              core::u64 gesture, cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.edge);
    if (existing == nullptr ||
        existing->type != compositor::schema::edge_type ||
        request.points.size() > compositor::max_wire_points) {
        return true;
    }
    doc::Node routed = *existing;
    std::erase_if(routed.properties, [](const auto &property) {
        return property.first.starts_with(compositor::schema::point_prefix);
    });
    for (std::size_t index = 0U; index < request.points.size(); ++index) {
        const compositor::WirePoint point = request.points[index];
        if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
            std::abs(point.x) > compositor::max_spatial_extent ||
            std::abs(point.y) > compositor::max_spatial_extent) {
            return true;
        }
        const std::string base = std::string{compositor::schema::point_prefix} +
                                 std::to_string(index);
        routed.properties.insert_or_assign(
            base + std::string{compositor::schema::point_x_suffix},
            doc::PropertyValue{static_cast<double>(point.x)});
        routed.properties.insert_or_assign(
            base + std::string{compositor::schema::point_y_suffix},
            doc::PropertyValue{static_cast<double>(point.y)});
    }
    if (routed == *existing) {
        return true;
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(routed)}, cmd::PutNode{*existing},
        cmd::UndoContextId{6U}, cmd::TransactionId{gesture}, apply_at));
}

} // namespace mediaserver::app
