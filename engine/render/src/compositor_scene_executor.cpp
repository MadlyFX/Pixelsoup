#include <mediaserver/render/compositor_scene_executor.hpp>

#include <mediaserver/core/idle_report.hpp>
#include <mediaserver/render/effect_execution.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/compositor_layer_frag_dxil.h>
#include <mediaserver/shaders/depth_merge_comp_dxil.h>
#include <mediaserver/shaders/output_vert_dxil.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using PlanResult = core::Result<CompositorScenePlan, CompositorScenePlanError>;
using CreateResult = core::Result<CompositorSceneExecutor, RenderError>;

[[nodiscard]] scene::BlendMode valid_blend(scene::BlendMode blend) noexcept {
    switch (blend) {
    case scene::BlendMode::Normal:
    case scene::BlendMode::Add:
    case scene::BlendMode::Multiply:
    case scene::BlendMode::Screen:
        return blend;
    }
    return scene::BlendMode::Normal;
}

[[nodiscard]] float blur_radius(const scene::LayerStyle& style) noexcept {
    const EffectExecutionTraits* const traits =
        effect_traits_for(scene::EffectKind::Blur);
    const auto application =
        resolve_effect(scene::EffectKind::Blur, style.effects);
    if (traits == nullptr || !application.has_value() ||
        traits->domain != EffectExecutionDomain::IsolatedLayer ||
        traits->processor != EffectProcessor::GaussianBlur ||
        traits->is_identity(*application)) {
        return 0.0F;
    }
    return std::clamp(application->value, 0.0F,
                      max_gaussian_blur_radius_px);
}

[[nodiscard]] std::vector<scene::SpatialEffectOperation>
spatial_operations(const scene::LayerStyle& style) {
    std::vector<scene::SpatialEffectOperation> result =
        style.effects.spatial_operations;
    const float radius = blur_radius(style);
    const bool has_gaussian = std::any_of(
        result.begin(), result.end(),
        [](const scene::SpatialEffectOperation& operation) noexcept {
            return operation.kind == scene::EffectKind::Blur;
        });
    if (radius > 0.0F && !has_gaussian) {
        result.insert(result.begin(), scene::SpatialEffectOperation{
                                          scene::EffectKind::Blur, radius,
                                          {}, {}});
    }
    return result;
}

[[nodiscard]] bool inspect_style(const scene::LayerStyle& style,
                                 std::optional<scene::BlendMode>& blend,
                                 std::optional<float>& radius,
                                 std::optional<std::vector<
                                     scene::SpatialEffectOperation>>&
                                     operations) {
    const scene::BlendMode current_blend = valid_blend(style.blend);
    const float current_radius = blur_radius(style);
    const auto current_operations = spatial_operations(style);
    if ((blend.has_value() && *blend != current_blend) ||
        (radius.has_value() && *radius != current_radius) ||
        (operations.has_value() && *operations != current_operations)) {
        return false;
    }
    blend = current_blend;
    radius = current_radius;
    operations = current_operations;
    return true;
}

enum class SourceOrderDomain : core::u8 {
    Identity,
    Sibling,
    Media,
};

struct SourceStyle final {
    scene::BlendMode blend{scene::BlendMode::Normal};
    float radius{};
    std::vector<scene::SpatialEffectOperation> spatial_effects;
    bool effect_isolation{};
    bool mask_isolation{};
    // The least opacity any drawable of this source carries. A source is a
    // candidate occluder only at full opacity, because anything you can see
    // through must not remove what is behind it from another layer.
    float opacity{1.0F};
    // Camera-space distance, accumulated across every drawable from this
    // source. Isolated passes have to be composited back-to-front; document
    // identity is only a deterministic tie breaker for equal depths.
    double depth_sum{};
    core::u32 drawables{};
    SourceOrderDomain order_domain{SourceOrderDomain::Identity};
    std::optional<doc::NodeId> sibling_parent;
    core::i64 sibling_order{};
    core::u64 media_order{};
};

using SourceStyles = std::map<doc::NodeId, SourceStyle>;
using MaskPrograms = std::map<doc::NodeId, const scene::MaskProgram*>;

// Where a SourceLayer mask lands in this frame's padded raster. Its
// coordinates name the target's source plane, the surface its UVs cover, so
// each pixel is traced back onto that plane through the same view, grid and
// padded rectangle the atmosphere resolves against. Measuring the mask across
// the raster instead put it wherever the layer's UV square would sit if it
// filled the padded frame, which is nowhere near the drawable the Stage
// outline is drawn on. No value when the target has no single source plane.
[[nodiscard]] std::optional<MaskRasterMapping> source_layer_mapping(
    const scene::Scene& scene, doc::NodeId target,
    const SceneRenderView* view, const PaddedSlice& padded,
    core::u32 target_width, core::u32 target_height) noexcept {
    const scene::Mat4* world{};
    scene::Primitive primitive{scene::Primitive::Quad};
    for (const scene::MediaInstance& media : scene.media) {
        if (media.source_node == target) {
            world = &media.world;
            primitive = media.mesh.primitive;
            break;
        }
    }
    if (world == nullptr) {
        for (const scene::MeshInstance& instance : scene.instances) {
            if (instance.source == target) {
                world = &instance.world;
                primitive = instance.mesh.primitive;
                break;
            }
        }
    }
    if (world == nullptr) {
        return std::nullopt;
    }
    const FogFrame frame =
        fog_frame_for(scene, view, padded, target_width, target_height);
    return source_layer_mask_mapping(
        frame.view_projection, frame.grid_canvas, frame.render, *world,
        primitive,
        scene::mask_source_extent(scene.camera, scene.canvas, *world,
                                  primitive));
}

[[nodiscard]] bool collect_mask_programs(const scene::Scene& source,
                                         MaskPrograms& programs) {
    const auto target_is_drawn = [&source](doc::NodeId target) noexcept {
        return std::any_of(
                   source.instances.begin(), source.instances.end(),
                   [target](const scene::MeshInstance& value) noexcept {
                       return value.source == target;
                   }) ||
            std::any_of(
                   source.media.begin(), source.media.end(),
                   [target](const scene::MediaInstance& value) noexcept {
                       return value.source_node == target;
                   }) ||
            std::any_of(
                   source.models.begin(), source.models.end(),
                   [target](const scene::ModelInstance& value) noexcept {
                       return value.source_node == target;
                   });
    };
    for (const scene::MaskProgram& program : source.mask_programs) {
        // Empty programs are authored identity carriers. As with disabled
        // effects, their stale target/space payload cannot allocate or reject
        // an otherwise executable frame.
        if (mask_program_is_identity(program)) {
            continue;
        }
        // AuthoredCanvas belongs to one retained group composite. The current
        // flat renderer owns no such image/depth pair, so reject it rather
        // than clone this program onto group descendants. This is judged
        // before the drawn test below on purpose: a group is never itself a
        // drawable, so testing it for coverage first would quietly accept a
        // group mask and then render the group unmasked.
        if (program.coordinate_space !=
            scene::MaskCoordinateSpace::SourceLayer) {
            return false;
        }
        // A mask cuts a layer. When that layer is not in this frame's picture -
        // a clip the playhead has moved past, a hidden object - the mask has
        // nothing to cut, so it is skipped exactly like an identity carrier
        // rather than allowed to reject the frame. Rejecting blanks the output
        // for every frame a masked clip is off air, which is an ordinary state
        // of an edited timeline and not a malformed document: masks whose
        // target is genuinely absent are dropped and diagnosed during scene
        // evaluation, so a program arriving here always names a real
        // scene-owned node. Skipping renders the same picture the frame would
        // have had anyway, which is what separates this from the effect-order
        // rejection above - that one would render something different.
        if (!target_is_drawn(program.target)) {
            continue;
        }
        if (!valid_mask_program(program) ||
            !programs.try_emplace(program.target, &program).second) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] core::u8 domain_order(
    EffectExecutionDomain domain) noexcept {
    switch (domain) {
    case EffectExecutionDomain::Geometry:
        return 0U;
    case EffectExecutionDomain::Inline:
        return 1U;
    case EffectExecutionDomain::IsolatedLayer:
        return 2U;
    case EffectExecutionDomain::FullScene:
        return 3U;
    }
    return 4U;
}

// EffectStack is a compact processor payload, not an ordered program. Support
// only authored orders consistent with the renderer's fixed domain sequence:
// geometry, inline material operations, isolated layers, then full scene.
// Rejecting the reverse sequence is the visible failure required by the effect
// contract; silently reordering it would render a different operation.
[[nodiscard]] bool effect_order_is_executable(const scene::Scene& source) {
    const auto executable = [](const auto& drawable) noexcept {
        return effect_stack_is_executable(drawable.style.effects);
    };
    if (!std::all_of(source.instances.begin(), source.instances.end(), executable)
        || !std::all_of(source.models.begin(), source.models.end(), executable)
        || !std::all_of(source.media.begin(), source.media.end(), executable)) {
        return false;
    }

    std::vector<const scene::EffectInstance*> ordered;
    ordered.reserve(source.effects.size());
    for (const scene::EffectInstance& effect : source.effects) {
        ordered.push_back(&effect);
    }
    std::stable_sort(
        ordered.begin(), ordered.end(), [](const auto* left,
                                           const auto* right) noexcept {
            if (left->target != right->target) {
                return left->target < right->target;
            }
            if (left->order != right->order) {
                return left->order < right->order;
            }
            return left->source < right->source;
        });

    doc::NodeId target{};
    core::u8 last_domain{};
    bool have_domain{};
    for (const scene::EffectInstance* const effect : ordered) {
        if (effect->target != target) {
            target = effect->target;
            last_domain = 0U;
            have_domain = false;
        }
        const EffectExecutionTraits* const traits =
            effect_traits_for(effect->kind);
        if (traits == nullptr) {
            return false;
        }
        const EffectApplication application{
            effect->enabled, effect->value, effect->levels,
            effect->lift_gamma_gain, effect->contrast_pivot,
            effect->color_matrix, effect->white_balance, &effect->curves,
            effect->film_grain, effect->box_blur,
            effect->directional_blur, effect->chroma_key,
            effect->luma_key};
        if (effect->kind == scene::EffectKind::Levels && effect->enabled
            && !scene::normalized_levels(effect->levels).has_value()) {
            return false;
        }
        if (effect->kind == scene::EffectKind::LiftGammaGain
            && effect->enabled
            && !scene::normalized_lift_gamma_gain(
                    effect->lift_gamma_gain).has_value()) {
            return false;
        }
        if (effect->kind == scene::EffectKind::ContrastPivot
            && effect->enabled
            && !scene::normalized_contrast_pivot(
                    effect->contrast_pivot).has_value()) {
            return false;
        }
        if (effect->kind == scene::EffectKind::ColorMatrix
            && effect->enabled
            && !scene::normalized_color_matrix(
                    effect->color_matrix).has_value()) {
            return false;
        }
        if (effect->kind == scene::EffectKind::WhiteBalance
            && effect->enabled
            && !scene::normalized_white_balance(
                    effect->white_balance).has_value()) {
            return false;
        }
        if (effect->kind == scene::EffectKind::Curves && effect->enabled
            && !scene::normalized_curves(effect->curves).has_value()) {
            return false;
        }
        if (effect->kind == scene::EffectKind::FilmGrain && effect->enabled
            && !scene::normalized_film_grain(
                    effect->film_grain).has_value()) {
            return false;
        }
        if (effect->kind == scene::EffectKind::BoxBlur && effect->enabled
            && !scene::normalized_box_blur(effect->box_blur).has_value()) {
            return false;
        }
        if (effect->kind == scene::EffectKind::DirectionalBlur &&
            effect->enabled && !scene::normalized_directional_blur(
                                   effect->directional_blur).has_value()) {
            return false;
        }
        if (effect->kind == scene::EffectKind::ChromaKey && effect->enabled &&
            !scene::normalized_chroma_key(effect->chroma_key).has_value()) {
            return false;
        }
        if (effect->kind == scene::EffectKind::LumaKey && effect->enabled &&
            !scene::normalized_luma_key(effect->luma_key).has_value()) {
            return false;
        }
        if (traits->is_identity(application)) {
            continue;
        }
        if (!effect_processor_is_implemented(*traits)) {
            return false;
        }
        const core::u8 current_domain = domain_order(traits->domain);
        if (have_domain && current_domain < last_domain) {
            return false;
        }
        last_domain = current_domain;
        have_domain = true;
    }
    return true;
}

[[nodiscard]] bool graph_effect_order_is_executable(
    const compositor::CompositorGraph& graph,
    const std::vector<const compositor::GraphNode*>& layers,
    const scene::Scene& source) {
    const auto observe_stack = [](const scene::EffectStack& effects,
                                  std::optional<core::u8>& last_domain) {
        if (!effect_stack_is_executable(effects)) {
            return false;
        }
        for (const EffectExecutionTraits& traits :
             all_effect_execution_traits()) {
            const EffectApplication application = traits.resolve(effects);
            if (traits.is_identity(application)) {
                continue;
            }
            const core::u8 current = domain_order(traits.domain);
            last_domain = last_domain.has_value()
                ? std::max(*last_domain, current)
                : current;
        }
        return true;
    };
    const auto compositor_effect_kind = [](compositor::NodeKind kind)
        -> std::optional<scene::EffectKind> {
        switch (kind) {
        case compositor::NodeKind::Brightness:
            return scene::EffectKind::Brightness;
        case compositor::NodeKind::Saturation:
            return scene::EffectKind::Saturation;
        case compositor::NodeKind::Blur:
            return scene::EffectKind::Blur;
        case compositor::NodeKind::Levels:
            return scene::EffectKind::Levels;
        case compositor::NodeKind::LiftGammaGain:
            return scene::EffectKind::LiftGammaGain;
        case compositor::NodeKind::ContrastPivot:
            return scene::EffectKind::ContrastPivot;
        case compositor::NodeKind::ColorMatrix:
            return scene::EffectKind::ColorMatrix;
        case compositor::NodeKind::WhiteBalance:
            return scene::EffectKind::WhiteBalance;
        case compositor::NodeKind::Curves:
            return scene::EffectKind::Curves;
        case compositor::NodeKind::FilmGrain:
            return scene::EffectKind::FilmGrain;
        case compositor::NodeKind::BoxBlur:
            return scene::EffectKind::BoxBlur;
        case compositor::NodeKind::DirectionalBlur:
            return scene::EffectKind::DirectionalBlur;
        case compositor::NodeKind::ChromaKey:
            return scene::EffectKind::ChromaKey;
        case compositor::NodeKind::LumaKey:
            return scene::EffectKind::LumaKey;
        case compositor::NodeKind::Matte:
        case compositor::NodeKind::SceneLayer:
        case compositor::NodeKind::RadialArray:
        case compositor::NodeKind::LedCarProcess:
        case compositor::NodeKind::Composite:
        case compositor::NodeKind::Output:
            return std::nullopt;
        }
        return std::nullopt;
    };

    for (const compositor::GraphNode* const layer : layers) {
        const auto& parameters =
            std::get<compositor::SceneLayerParameters>(layer->parameters);
        std::optional<core::u8> last_domain;
        for (const scene::MeshInstance& instance : source.instances) {
            if (instance.source == parameters.scene_node &&
                !observe_stack(instance.style.effects, last_domain)) {
                return false;
            }
        }
        for (const scene::MediaInstance& instance : source.media) {
            if ((instance.source_node == parameters.scene_node ||
                 instance.timeline_lane == parameters.scene_node) &&
                !observe_stack(instance.style.effects, last_domain)) {
                return false;
            }
        }
        for (const scene::ModelInstance& instance : source.models) {
            if (instance.source_node == parameters.scene_node &&
                !observe_stack(instance.style.effects, last_domain)) {
                return false;
            }
        }

        const compositor::PlanStep* const step = graph.step(layer->node);
        if (step == nullptr) {
            return false;
        }
        const auto observe_graph_modifier =
            [&](const compositor::PlanConnection& input) {
            if (input.type != compositor::PortValueType::Effect) {
                return true;
            }
            const compositor::GraphNode* const modifier = graph.node(input.source);
            if (modifier == nullptr) {
                return false;
            }
            if (compositor::effect_node_is_identity(*modifier)) {
                return true;
            }
            const auto effect_kind = compositor_effect_kind(modifier->kind);
            if (!effect_kind.has_value()) {
                return false;
            }
            const EffectExecutionTraits* const traits =
                effect_traits_for(*effect_kind);
            if (traits == nullptr || !effect_processor_is_implemented(*traits)) {
                return false;
            }
            const core::u8 current = domain_order(traits->domain);
            if (last_domain.has_value() && current < *last_domain) {
                return false;
            }
            last_domain = current;
            return true;
        };
        for (const compositor::PlanConnection& input : step->inputs) {
            if (!observe_graph_modifier(input)) {
                return false;
            }
        }
        for (const compositor::GraphNode& setup : graph.nodes) {
            if (setup.kind != compositor::NodeKind::LedCarProcess) {
                continue;
            }
            const compositor::GraphNodeMetadata* const metadata =
                graph.metadata(setup.node);
            if (metadata == nullptr || metadata->path_to_output.empty()) {
                continue;
            }
            const compositor::PlanStep* const setup_step =
                graph.step(setup.node);
            if (setup_step == nullptr) {
                return false;
            }
            const bool uses_layer = std::any_of(
                setup_step->inputs.begin(), setup_step->inputs.end(),
                [layer](const compositor::PlanConnection& input) noexcept {
                    return input.type == compositor::PortValueType::Image &&
                        input.source == layer->node;
                });
            if (!uses_layer) {
                continue;
            }
            for (const compositor::PlanConnection& input :
                 setup_step->inputs) {
                if (input.target_port == 9U &&
                    !observe_graph_modifier(input)) {
                    return false;
                }
            }
        }
    }
    return true;
}

[[nodiscard]] float layer_opacity(const scene::LayerStyle& style) noexcept {
    return std::isfinite(style.opacity)
        ? std::clamp(style.opacity, 0.0F, 1.0F)
        : 1.0F;
}

[[nodiscard]] bool collect_source_styles(const scene::Scene& source,
                                         const MaskPrograms& masks,
                                         SourceStyles& styles) {
    const scene::Vec3 forward = scene::normalize(
        source.camera.target - source.camera.eye,
        scene::Vec3{0.0F, 0.0F, -1.0F});
    const auto add = [&styles, &masks](
                         doc::NodeId source_node,
                         const scene::LayerStyle& style,
                         const scene::Mat4& world,
                         scene::Vec3 eye,
                         scene::Vec3 view_forward,
                         SourceOrderDomain order_domain,
                         std::optional<doc::NodeId> sibling_parent,
                         core::i64 sibling_order,
                         core::u64 media_order) {
        if (!effect_stack_is_executable(style.effects)) {
            return false;
        }
        const float depth = scene::dot(
            scene::transform_point(world, scene::Vec3{}) - eye,
            view_forward);
        const SourceStyle summary{valid_blend(style.blend), blur_radius(style),
                                  spatial_operations(style),
                                  effect_stack_requires_isolation(style.effects),
                                  masks.contains(source_node),
                                  layer_opacity(style),
                                  std::isfinite(depth)
                                      ? static_cast<double>(depth)
                                      : 0.0,
                                  1U,
                                  order_domain,
                                  sibling_parent,
                                  sibling_order,
                                  media_order};
        const auto [found, inserted] = styles.try_emplace(source_node, summary);
        if (!inserted) {
            found->second.opacity =
                std::min(found->second.opacity, summary.opacity);
            found->second.depth_sum += summary.depth_sum;
            ++found->second.drawables;
        }
        return inserted ||
            (found->second.blend == summary.blend &&
             found->second.radius == summary.radius &&
             found->second.spatial_effects == summary.spatial_effects &&
             found->second.effect_isolation == summary.effect_isolation &&
             found->second.mask_isolation == summary.mask_isolation &&
             found->second.order_domain == summary.order_domain &&
             found->second.sibling_parent == summary.sibling_parent &&
             found->second.sibling_order == summary.sibling_order &&
             found->second.media_order == summary.media_order);
    };
    for (const scene::MeshInstance& instance : source.instances) {
        if (!add(instance.source, instance.style, instance.world,
                 source.camera.eye, forward, SourceOrderDomain::Sibling,
                 instance.parent_group, instance.group_order, 0U)) {
            return false;
        }
    }
    for (const scene::MediaInstance& instance : source.media) {
        const bool loose = instance.timeline_lane == doc::NodeId{};
        if (!add(instance.source_node, instance.style, instance.world,
                 source.camera.eye, forward,
                 loose ? SourceOrderDomain::Sibling
                       : SourceOrderDomain::Media,
                 loose ? instance.parent_group
                       : std::optional<doc::NodeId>{},
                 loose ? instance.group_order : 0,
                 instance.compositing_order)) {
            return false;
        }
    }
    for (const scene::ModelInstance& instance : source.models) {
        if (!add(instance.source_node, instance.style, instance.world,
                 source.camera.eye, forward, SourceOrderDomain::Sibling,
                 instance.parent_group, instance.group_order, 0U)) {
            return false;
        }
    }
    return true;
}

void append_source_passes(const SourceStyles& sources,
                          const MaskPrograms& masks,
                          std::vector<CompositorLayerPass>& passes) {
    std::vector<doc::NodeId> ordinary;
    ordinary.reserve(sources.size());
    std::vector<std::pair<doc::NodeId, const SourceStyle*>> isolated;
    isolated.reserve(sources.size());
    for (const auto& [source_node, style] : sources) {
        const bool needs_isolation = style.effect_isolation ||
            style.mask_isolation ||
            style.blend != scene::BlendMode::Normal;
        if (needs_isolation) {
            isolated.emplace_back(source_node, &style);
        } else {
            ordinary.push_back(source_node);
        }
    }

    // Ordinary geometry must remain one shared-depth scene render. Splitting
    // it around effect layers turns the 3D view into a stack of flat images:
    // as the camera crosses a layer origin the stack changes order and whole
    // opaque surfaces can pop in or out. The isolated layers are rendered
    // afterward; their depth-only copies of the ordinary geometry mask them
    // wherever that geometry is physically in front.
    if (!ordinary.empty()) {
        CompositorLayerPass pass{};
        pass.scene_node = ordinary.front();
        pass.scene_nodes = std::move(ordinary);
        passes.push_back(std::move(pass));
    }

    std::stable_sort(
        isolated.begin(), isolated.end(), [](const auto& left,
                                             const auto& right) noexcept {
            const auto average_depth = [](const SourceStyle& style) noexcept {
                return style.drawables != 0U
                    ? style.depth_sum / static_cast<double>(style.drawables)
                    : 0.0;
            };
            const double left_depth = average_depth(*left.second);
            const double right_depth = average_depth(*right.second);
            if (left_depth != right_depth) {
                return left_depth > right_depth;
            }
            if (left.second->order_domain == SourceOrderDomain::Sibling &&
                right.second->order_domain == SourceOrderDomain::Sibling &&
                left.second->sibling_parent ==
                    right.second->sibling_parent &&
                left.second->sibling_order != right.second->sibling_order) {
                return left.second->sibling_order <
                    right.second->sibling_order;
            }
            if (left.second->order_domain == SourceOrderDomain::Media &&
                right.second->order_domain == SourceOrderDomain::Media &&
                left.second->media_order != right.second->media_order) {
                return left.second->media_order < right.second->media_order;
            }
            return left.first < right.first;
        });
    for (const auto& [source_node, style_pointer] : isolated) {
        const SourceStyle& style = *style_pointer;
        CompositorLayerPass pass{
            {}, source_node, {}, style.blend, style.radius, {}};
        pass.spatial_effects = style.spatial_effects;
        if (const auto found = masks.find(source_node);
            found != masks.end()) {
            pass.mask_program = *found->second;
        }
        passes.push_back(std::move(pass));
    }
}

[[nodiscard]] bool pass_contains(const CompositorLayerPass& pass,
                                 doc::NodeId scene_node) noexcept {
    if (pass.scene_nodes.empty()) {
        return pass.scene_node == scene_node;
    }
    return std::binary_search(pass.scene_nodes.begin(), pass.scene_nodes.end(),
                              scene_node);
}

[[nodiscard]] bool pass_occludes(const CompositorLayerPass& pass,
                                 doc::NodeId scene_node) noexcept {
    return std::binary_search(pass.depth_only_nodes.begin(),
                              pass.depth_only_nodes.end(), scene_node);
}

[[nodiscard]] bool pass_renders(const CompositorLayerPass& pass,
                                doc::NodeId scene_node) noexcept {
    return pass_contains(pass, scene_node) || pass_occludes(pass, scene_node);
}

// Whether one document node's drawables present a hard, fully covering
// silhouette. Summarised tolerantly rather than reported as an error: a source
// this cannot summarise simply stops being a candidate occluder, which costs
// depth correctness for that one object instead of refusing the whole plan.
struct OccluderCandidate final {
    bool eligible{true};
};

using OccluderCandidates = std::map<doc::NodeId, OccluderCandidate>;

[[nodiscard]] bool occludes(const scene::LayerStyle& style) noexcept {
    return layer_opacity(style) >= 1.0F &&
        (!std::isfinite(style.horizontal_blend) ||
         style.horizontal_blend <= 0.0F) &&
        effect_stack_preserves_hard_occlusion(style.effects) &&
        valid_blend(style.blend) == scene::BlendMode::Normal;
}

[[nodiscard]] OccluderCandidates
collect_occluder_candidates(const scene::Scene& source) {
    OccluderCandidates candidates;
    const auto add = [&candidates](doc::NodeId source_node,
                                   const scene::LayerStyle& style) {
        auto& candidate = candidates[source_node];
        candidate.eligible = candidate.eligible && occludes(style);
    };
    for (const scene::MeshInstance& instance : source.instances) {
        add(instance.source, instance.style);
    }
    for (const scene::ModelInstance& instance : source.models) {
        add(instance.source_node, instance.style);
    }
    // A clip qualifies: the depth-only media pass alpha-tests the frame, so a
    // clip occludes exactly where its own pixels are opaque.
    for (const scene::MediaInstance& instance : source.media) {
        add(instance.source_node, instance.style);
    }
    // Any active mask changes or softens coverage. Even an analytic hard mask
    // leaves only a partial silhouette, and a program may be feathered or
    // partially opaque, so the source cannot stand in as a hard depth-only
    // occluder for another isolated layer.
    for (const scene::MaskProgram& program : source.mask_programs) {
        if (!mask_program_is_identity(program)) {
            candidates[program.target].eligible = false;
        }
    }
    return candidates;
}

// Fills in every pass's depth-only set once the passes themselves are known.
//
// A source qualifies as an occluder when it is drawn by some pass of this very
// plan and is crisply opaque there. Drawn by *some* pass matters: a graph that
// replaces the scene deliberately renders nothing for the drawables it left
// out, and geometry that is not in the frame cannot hide anything in it.
void assign_depth_only_nodes(const scene::Scene& adjusted,
                             std::vector<CompositorLayerPass>& passes) {
    for (CompositorLayerPass& pass : passes) {
        pass.depth_only_nodes.clear();
    }
    if (passes.size() < 2U) {
        // One pass owns the whole frame and already shares one depth buffer.
        return;
    }
    std::vector<doc::NodeId> occluders;
    for (const auto& [source_node, candidate] :
         collect_occluder_candidates(adjusted)) {
        if (!candidate.eligible) {
            continue;
        }
        const bool rendered =
            std::any_of(passes.begin(), passes.end(),
                        [source_node](const CompositorLayerPass& pass) {
                            return pass_contains(pass, source_node);
                        });
        if (rendered) {
            // The candidate map is keyed by node id, so this stays ascending.
            occluders.push_back(source_node);
        }
    }
    for (CompositorLayerPass& pass : passes) {
        for (const doc::NodeId occluder : occluders) {
            if (!pass_contains(pass, occluder)) {
                pass.depth_only_nodes.push_back(occluder);
            }
        }
    }
}

[[nodiscard]] scene::Scene isolate(const scene::Scene& source,
                                   const CompositorLayerPass& pass) {
    scene::Scene result = source;
    std::erase_if(result.instances,
                  [&pass](const scene::MeshInstance& instance) noexcept {
                      return !pass_renders(pass, instance.source);
                  });
    std::erase_if(result.media,
                  [&pass](const scene::MediaInstance& instance) noexcept {
                      return !pass_renders(pass, instance.source_node);
                  });
    std::erase_if(result.models,
                  [&pass](const scene::ModelInstance& instance) noexcept {
                      return !pass_renders(pass, instance.source_node);
                  });
    std::erase_if(result.effects,
                  [&pass](const scene::EffectInstance& effect) noexcept {
                      return !pass_contains(pass, effect.target);
                  });

    // The layer's authored blend is applied once, when its completed texture
    // joins the graph accumulator. Internally, multiple radial instances and
    // alpha-bearing media use ordinary source-over against transparency.
    const auto normalize = [](auto& values) noexcept {
        for (auto& value : values) {
            value.style.blend = scene::BlendMode::Normal;
            disable_effects_in_domain(
                value.style.effects, EffectExecutionDomain::IsolatedLayer);
        }
    };
    normalize(result.instances);
    normalize(result.media);
    normalize(result.models);
    return result;
}

struct ShaderBlobs final {
    std::span<const core::u8> vertex;
    std::span<const core::u8> fragment;
    rhi::ShaderFormat format{rhi::ShaderFormat::Dxil};
    bool present{};
};

[[nodiscard]] ShaderBlobs shader_blobs() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return ShaderBlobs{
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_output_vert_dxil),
            sizeof(mediaserver_output_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_compositor_layer_frag_dxil),
            sizeof(mediaserver_compositor_layer_frag_dxil)},
        rhi::ShaderFormat::Dxil,
        true,
    };
#else
    return ShaderBlobs{};
#endif
}

// The nearest-surface merge the atmosphere resolves against.
[[nodiscard]] std::span<const core::u8> depth_merge_bytecode() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_depth_merge_comp_dxil),
        sizeof(mediaserver_depth_merge_comp_dxil)};
#else
    return {};
#endif
}

// Reversed depth in a storage-writable R32F. A colour target as well, because
// it is cleared to "nothing drawn" before the layers land on it.
[[nodiscard]] rhi::TextureDesc scene_depth_desc(core::u32 width,
                                                core::u32 height) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::R32Float;
    desc.color_target = true;
    desc.sampled = true;
    desc.storage_read = true;
    desc.storage_write = true;
    return desc;
}

// depth_merge.comp.hlsl: cbuffer DepthMergeConstants.
struct DepthMergeConstants final {
    float extent[4U]{};
};

// Why a scene-level pass did not run, in words an operator can act on. Static
// literals: the pass status carries them by pointer and the idle report dedups
// by address. Kept short enough for one line of the settings card.
constexpr const char* fog_refusal_depth =
    "no combined depth for the atmosphere on this device";
constexpr const char* fog_refusal_targets =
    "no atmosphere targets at this extent on this device";
constexpr const char* bloom_refusal_unaligned =
    "slice does not start on the canvas's 64-pixel bloom grid";
constexpr const char* bloom_refusal_targets =
    "no bloom targets at this extent on this device";

[[nodiscard]] rhi::TextureDesc color_desc(core::u32 width,
                                           core::u32 height,
                                           rhi::TextureFormat format) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.color_target = true;
    desc.sampled = true;
    return desc;
}

struct alignas(16) CompositeUniform final {
    float blend_mode{};
    std::array<float, 3U> padding{};
};

static_assert(sizeof(CompositeUniform) == 16U);
static_assert(static_cast<core::u8>(scene::BlendMode::Normal) == 0U);
static_assert(static_cast<core::u8>(scene::BlendMode::Add) == 1U);
static_assert(static_cast<core::u8>(scene::BlendMode::Multiply) == 2U);
static_assert(static_cast<core::u8>(scene::BlendMode::Screen) == 3U);

[[nodiscard]] std::span<const core::u8>
bytes(const CompositeUniform& value) noexcept {
    return std::span<const core::u8>{reinterpret_cast<const core::u8*>(&value),
                                     sizeof(value)};
}

using LayerPassResult =
    core::Result<CompositorLayerPass, CompositorScenePlanError>;

[[nodiscard]] LayerPassResult make_graph_layer_pass(
    const compositor::GraphNode& layer,
    const scene::Scene& adjusted,
    const MaskPrograms& masks) {
    if (layer.kind != compositor::NodeKind::SceneLayer) {
        return LayerPassResult::failure(
            CompositorScenePlanError::UnsupportedTopology);
    }
    const auto& parameters =
        std::get<compositor::SceneLayerParameters>(layer.parameters);
    std::optional<scene::BlendMode> blend;
    std::optional<float> radius;
    std::optional<std::vector<scene::SpatialEffectOperation>> operations;
    bool found{};
    std::vector<doc::NodeId> track_sources;
    const auto screen = std::find_if(
        adjusted.screens.begin(), adjusted.screens.end(),
        [&](const scene::SceneScreen& candidate) noexcept {
            return candidate.node == parameters.scene_node;
        });
    const bool track = screen != adjusted.screens.end();
    const auto inspect = [&](doc::NodeId source_node,
                             const scene::LayerStyle& style) {
        if (source_node != parameters.scene_node) {
            return true;
        }
        found = true;
        return inspect_style(style, blend, radius, operations);
    };
    for (const scene::MeshInstance& instance : adjusted.instances) {
        if (!inspect(instance.source, instance.style)) {
            return LayerPassResult::failure(
                CompositorScenePlanError::InconsistentLayerStyle);
        }
    }
    for (const scene::MediaInstance& instance : adjusted.media) {
        if (track && instance.timeline_lane == parameters.scene_node) {
            found = true;
            track_sources.push_back(instance.source_node);
            if (!inspect_style(instance.style, blend, radius, operations)) {
                return LayerPassResult::failure(
                    CompositorScenePlanError::InconsistentLayerStyle);
            }
        } else if (!track &&
                   !inspect(instance.source_node, instance.style)) {
            return LayerPassResult::failure(
                CompositorScenePlanError::InconsistentLayerStyle);
        }
    }
    for (const scene::ModelInstance& instance : adjusted.models) {
        if (!inspect(instance.source_node, instance.style)) {
            return LayerPassResult::failure(
                CompositorScenePlanError::InconsistentLayerStyle);
        }
    }
    // An empty Track is a valid transparent graph input. Its screen remains in
    // the evaluated scene even when no clip is on air, so graph topology does
    // not flicker between valid and invalid while the playhead crosses a gap.
    if (track && !found) {
        found = true;
        if (!inspect_style(screen->style, blend, radius, operations)) {
            return LayerPassResult::failure(
                CompositorScenePlanError::InconsistentLayerStyle);
        }
    }
    if (!found || !blend.has_value() || !radius.has_value() ||
        !operations.has_value()) {
        return LayerPassResult::failure(
            CompositorScenePlanError::MissingSceneLayer);
    }
    CompositorLayerPass pass{
        layer.node, parameters.scene_node, {}, *blend, *radius, {}};
    if (track) {
        std::sort(track_sources.begin(), track_sources.end());
        track_sources.erase(
            std::unique(track_sources.begin(), track_sources.end()),
            track_sources.end());
        pass.scene_nodes = std::move(track_sources);
    }
    pass.spatial_effects = std::move(*operations);
    if (const auto mask = masks.find(parameters.scene_node);
        mask != masks.end()) {
        pass.mask_program = *mask->second;
    }
    return LayerPassResult::success(std::move(pass));
}

} // namespace

PlanResult plan_compositor_scene(const compositor::CompositorGraph& graph,
                                 const scene::Scene& source) {
    if (!graph.valid() || graph.plan.steps.empty()) {
        return PlanResult::failure(CompositorScenePlanError::InvalidGraph);
    }

    try {
        if (!effect_order_is_executable(source)) {
            return PlanResult::failure(
                CompositorScenePlanError::InvalidConfiguration);
        }
        MaskPrograms mask_programs;
        if (!collect_mask_programs(source, mask_programs)) {
            return PlanResult::failure(
                CompositorScenePlanError::InvalidConfiguration);
        }
        const compositor::GraphNode* output{};
        for (const compositor::GraphNode& node : graph.nodes) {
            if (node.kind != compositor::NodeKind::Output) {
                continue;
            }
            if (output != nullptr) {
                return PlanResult::failure(
                    CompositorScenePlanError::UnsupportedTopology);
            }
            output = &node;
        }
        if (output == nullptr) {
            return PlanResult::failure(CompositorScenePlanError::InvalidGraph);
        }
        const compositor::PlanStep* const output_step =
            graph.step(output->node);
        if (output_step == nullptr || output_step->inputs.size() != 1U) {
            return PlanResult::failure(
                CompositorScenePlanError::UnsupportedTopology);
        }

        struct ImageLayerTopology final {
            const compositor::GraphNode* source{};
            const compositor::GraphNode* matte_node{};
            const compositor::GraphNode* matte_source{};
        };
        std::vector<ImageLayerTopology> image_layers;
        std::vector<const compositor::GraphNode*> layer_nodes;
        std::set<doc::NodeId> group_ids;
        for (const scene::SceneGroup& group : source.groups) {
            group_ids.insert(group.node);
        }
        const auto append_image = [&](const compositor::GraphNode& node) {
            if (node.kind == compositor::NodeKind::SceneLayer) {
                image_layers.push_back(ImageLayerTopology{&node, {}, {}});
                layer_nodes.push_back(&node);
                return true;
            }
            if (node.kind != compositor::NodeKind::Matte) {
                return false;
            }
            const compositor::PlanStep* const matte_step =
                graph.step(node.node);
            const auto* const parameters =
                std::get_if<compositor::MatteParameters>(&node.parameters);
            if (matte_step == nullptr || parameters == nullptr ||
                matte_step->inputs.size() != 2U ||
                matte_step->inputs[0].target_port != 0U ||
                matte_step->inputs[1].target_port != 1U ||
                matte_step->inputs[0].type !=
                    compositor::PortValueType::Image ||
                matte_step->inputs[1].type !=
                    compositor::PortValueType::Image) {
                return false;
            }
            const compositor::GraphNode* const source_layer =
                graph.node(matte_step->inputs[0].source);
            const compositor::GraphNode* const matte_layer =
                graph.node(matte_step->inputs[1].source);
            if (source_layer == nullptr || matte_layer == nullptr ||
                source_layer->kind != compositor::NodeKind::SceneLayer ||
                matte_layer->kind != compositor::NodeKind::SceneLayer) {
                return false;
            }
            const auto* const source_parameters =
                std::get_if<compositor::SceneLayerParameters>(
                    &source_layer->parameters);
            const auto* const matte_parameters =
                std::get_if<compositor::SceneLayerParameters>(
                    &matte_layer->parameters);
            if (source_parameters == nullptr || matte_parameters == nullptr ||
                source_layer->node == matte_layer->node ||
                source_parameters->scene_node == matte_parameters->scene_node ||
                group_ids.contains(source_parameters->scene_node) ||
                group_ids.contains(matte_parameters->scene_node)) {
                return false;
            }
            image_layers.push_back(
                ImageLayerTopology{source_layer, &node, matte_layer});
            layer_nodes.push_back(source_layer);
            layer_nodes.push_back(matte_layer);
            return true;
        };
        const auto append_led_car_process =
            [&](const compositor::GraphNode& setup) {
            if (setup.kind != compositor::NodeKind::LedCarProcess) {
                return false;
            }
            const compositor::PlanStep* const setup_step =
                graph.step(setup.node);
            if (setup_step == nullptr) {
                return false;
            }
            for (const compositor::PlanConnection& input :
                 setup_step->inputs) {
                if (input.type != compositor::PortValueType::Image) {
                    continue;
                }
                const compositor::GraphNode* const image =
                    graph.node(input.source);
                if (input.target_port > 8U || image == nullptr ||
                    image->kind != compositor::NodeKind::SceneLayer ||
                    !append_image(*image)) {
                    return false;
                }
            }
            // Empty and partially populated plate rigs are valid. An omitted
            // socket contributes transparency at that direction instead of
            // invalidating the entire setup.
            return true;
        };
        const compositor::GraphNode* const image_source =
            graph.node(output_step->inputs.front().source);
        if (image_source == nullptr) {
            return PlanResult::failure(CompositorScenePlanError::InvalidGraph);
        }
        if (image_source->kind == compositor::NodeKind::SceneLayer ||
            image_source->kind == compositor::NodeKind::Matte) {
            if (!append_image(*image_source)) {
                return PlanResult::failure(
                    CompositorScenePlanError::UnsupportedTopology);
            }
        } else if (image_source->kind == compositor::NodeKind::Composite) {
            const compositor::PlanStep* const composite_step =
                graph.step(image_source->node);
            if (composite_step == nullptr || composite_step->inputs.empty()) {
                return PlanResult::failure(
                    CompositorScenePlanError::UnsupportedTopology);
            }
            image_layers.reserve(composite_step->inputs.size());
            layer_nodes.reserve(composite_step->inputs.size() * 2U);
            for (const compositor::PlanConnection& input :
                 composite_step->inputs) {
                const compositor::GraphNode* const image =
                    graph.node(input.source);
                if (input.type != compositor::PortValueType::Image ||
                    image == nullptr ||
                    (!append_image(*image) &&
                     !append_led_car_process(*image))) {
                    return PlanResult::failure(
                        CompositorScenePlanError::UnsupportedTopology);
                }
            }
        } else if (image_source->kind ==
                   compositor::NodeKind::LedCarProcess) {
            image_layers.reserve(9U);
            layer_nodes.reserve(9U);
            if (!append_led_car_process(*image_source)) {
                return PlanResult::failure(
                    CompositorScenePlanError::UnsupportedTopology);
            }
        } else {
            return PlanResult::failure(
                CompositorScenePlanError::UnsupportedTopology);
        }
        if (layer_nodes.size() > max_compositor_scene_layers) {
            return PlanResult::failure(
                CompositorScenePlanError::LayersOverCapacity);
        }
        // Every live branch is one independently rendered image. A scene id
        // may not be reused, and a consumed matte branch may not also appear
        // as a visible layer in the output stack.
        std::map<doc::NodeId, doc::NodeId> graph_layer_for_source;
        std::set<doc::NodeId> visible_graph_layers;
        std::set<doc::NodeId> consumed_matte_layers;
        for (const ImageLayerTopology& topology : image_layers) {
            visible_graph_layers.insert(topology.source->node);
            if (topology.matte_source != nullptr) {
                consumed_matte_layers.insert(topology.matte_source->node);
            }
        }
        for (const compositor::GraphNode* const layer : layer_nodes) {
            const auto* const parameters =
                std::get_if<compositor::SceneLayerParameters>(
                    &layer->parameters);
            if (parameters == nullptr) {
                return PlanResult::failure(
                    CompositorScenePlanError::UnsupportedTopology);
            }
            std::vector<doc::NodeId> sources{parameters->scene_node};
            const bool track = std::any_of(
                source.screens.begin(), source.screens.end(),
                [&](const scene::SceneScreen& screen) noexcept {
                    return screen.node == parameters->scene_node;
                });
            if (track) {
                for (const scene::MediaInstance& media : source.media) {
                    if (media.timeline_lane == parameters->scene_node) {
                        sources.push_back(media.source_node);
                    }
                }
            }
            for (const doc::NodeId source_node : sources) {
                if (!graph_layer_for_source
                         .try_emplace(source_node, layer->node)
                         .second) {
                    return PlanResult::failure(
                        CompositorScenePlanError::UnsupportedTopology);
                }
            }
        }
        for (const doc::NodeId consumed : consumed_matte_layers) {
            if (visible_graph_layers.contains(consumed)) {
                return PlanResult::failure(
                    CompositorScenePlanError::UnsupportedTopology);
            }
        }
        // A SceneLayer graph is downstream of the first-class effects already
        // folded into its source drawable. Validate that combined authored
        // sequence as one program; checking the two representations in
        // isolation would silently move a graph inline effect ahead of an
        // existing scene Blur.
        if (!graph_effect_order_is_executable(graph, layer_nodes, source)) {
            return PlanResult::failure(
                CompositorScenePlanError::InvalidConfiguration);
        }

        CompositorScenePlan plan{};
        plan.adjusted_scene = compositor::apply(graph, source);
        plan.replaces_scene =
            std::get<compositor::OutputParameters>(output->parameters)
                .replace_scene;

        // A graph is an opt-in override of the layers on its executable Output
        // path, not an accidental scene crop. Preserve every other drawable as
        // an implicit bottom stack, including its intrinsic blur/blend style.
        if (!plan.replaces_scene) {
            SourceStyles pass_through;
            if (!collect_source_styles(
                    plan.adjusted_scene, mask_programs, pass_through)) {
                return PlanResult::failure(
                    CompositorScenePlanError::InconsistentLayerStyle);
            }
            for (const compositor::GraphNode* const layer : layer_nodes) {
                const auto& parameters =
                    std::get<compositor::SceneLayerParameters>(
                        layer->parameters);
                pass_through.erase(parameters.scene_node);
                for (const scene::MediaInstance& media :
                     plan.adjusted_scene.media) {
                    if (media.timeline_lane == parameters.scene_node) {
                        pass_through.erase(media.source_node);
                    }
                }
            }
            append_source_passes(pass_through, mask_programs, plan.layers);
        }
        plan.layers.reserve(plan.layers.size() + image_layers.size());
        for (const ImageLayerTopology& topology : image_layers) {
            auto built = make_graph_layer_pass(
                *topology.source, plan.adjusted_scene, mask_programs);
            if (!built) {
                return PlanResult::failure(built.error());
            }
            CompositorLayerPass pass = std::move(built).value();
            if (topology.matte_node != nullptr &&
                topology.matte_source != nullptr) {
                auto matte_built = make_graph_layer_pass(
                    *topology.matte_source, plan.adjusted_scene,
                    mask_programs);
                if (!matte_built) {
                    return PlanResult::failure(matte_built.error());
                }
                CompositorLayerPass matte_pass =
                    std::move(matte_built).value();
                CompositorMatteBranch branch{};
                branch.graph_node = matte_pass.graph_node;
                branch.scene_node = matte_pass.scene_node;
                branch.blur_radius_px = matte_pass.blur_radius_px;
                branch.spatial_effects =
                    std::move(matte_pass.spatial_effects);
                branch.mask_program = std::move(matte_pass.mask_program);
                pass.matte_branch = std::move(branch);
                pass.matte = std::get<compositor::MatteParameters>(
                    topology.matte_node->parameters);
            }
            plan.layers.push_back(std::move(pass));
        }
        std::size_t renderer_count = plan.layers.size();
        renderer_count += static_cast<std::size_t>(std::count_if(
            plan.layers.begin(), plan.layers.end(),
            [](const CompositorLayerPass& pass) noexcept {
                return pass.matte_branch.has_value();
            }));
        if (renderer_count > max_compositor_scene_layers) {
            return PlanResult::failure(
                CompositorScenePlanError::LayersOverCapacity);
        }
        assign_depth_only_nodes(plan.adjusted_scene, plan.layers);
        return PlanResult::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return PlanResult::failure(CompositorScenePlanError::AllocationFailed);
    }
}

PlanResult plan_scene_effects(const scene::Scene& source) {
    try {
        if (!effect_order_is_executable(source)) {
            return PlanResult::failure(
                CompositorScenePlanError::InvalidConfiguration);
        }
        MaskPrograms mask_programs;
        if (!collect_mask_programs(source, mask_programs)) {
            return PlanResult::failure(
                CompositorScenePlanError::InvalidConfiguration);
        }
        SourceStyles sources;
        if (!collect_source_styles(source, mask_programs, sources)) {
            return PlanResult::failure(
                CompositorScenePlanError::InconsistentLayerStyle);
        }

        CompositorScenePlan plan{};
        plan.adjusted_scene = source;
        append_source_passes(sources, mask_programs, plan.layers);
        if (plan.layers.size() > max_compositor_scene_layers) {
            return PlanResult::failure(
                CompositorScenePlanError::LayersOverCapacity);
        }
        assign_depth_only_nodes(plan.adjusted_scene, plan.layers);
        return PlanResult::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return PlanResult::failure(CompositorScenePlanError::AllocationFailed);
    }
}

bool CompositorSceneExecutor::shaders_available() noexcept {
    return shader_blobs().present && HybridSceneRenderer::shaders_available() &&
           GaussianBlur::shaders_available() &&
           SpatialBlur::shaders_available() &&
           LayerKeyer::shaders_available() &&
           MaskRaster::shaders_available() &&
           LayerMatte::shaders_available();
}

CompositorSceneExecutor::CompositorSceneExecutor(
    rhi::Device& device, RendererLimits limits, MediaCacheConfig media_cache,
    std::vector<HybridSceneRenderer> layer_renderers, GaussianBlur blur,
    SpatialBlur spatial_blur, LayerKeyer keyer, MaskRaster mask_raster,
    LayerMatte matte, rhi::Texture composite_a, rhi::Texture composite_b,
    rhi::Texture output, rhi::GraphicsPipeline composite_pipeline,
    rhi::Sampler sampler, rhi::TextureFormat working_format) noexcept
    : device_(&device), limits_(limits),
      layer_renderers_(std::move(layer_renderers)),
      media_cache_(media_cache), blur_(std::move(blur)),
      spatial_blur_(std::move(spatial_blur)),
      keyer_(std::move(keyer)),
      mask_raster_(std::move(mask_raster)),
      matte_(std::move(matte)),
      composite_a_(std::move(composite_a)),
      composite_b_(std::move(composite_b)),
      output_(std::move(output)),
      composite_pipeline_(std::move(composite_pipeline)),
      sampler_(std::move(sampler)), working_format_(working_format) {}

CreateResult CompositorSceneExecutor::create(rhi::Device& device,
                                              core::u32 width, core::u32 height,
                                              const RendererLimits& limits,
                                              MediaCacheConfig media_cache,
                                              rhi::TextureFormat working_format) {
    const ShaderBlobs blobs = shader_blobs();
    if (!blobs.present || !device.supports(blobs.format) ||
        !HybridSceneRenderer::shaders_available() ||
        !GaussianBlur::shaders_available() ||
        !SpatialBlur::shaders_available() ||
        !LayerKeyer::shaders_available() ||
        !MaskRaster::shaders_available() ||
        !LayerMatte::shaders_available()) {
        return CreateResult::failure(RenderError::ShaderFormatUnsupported);
    }
    if (width == 0U || height == 0U) {
        return CreateResult::failure(RenderError::InvalidConfiguration);
    }

    auto renderer = HybridSceneRenderer::create(device, width, height, limits, SceneEffectOwner::Compositor);
    if (!renderer) {
        return CreateResult::failure(renderer.error());
    }
    auto blur = GaussianBlur::create(device, width, height, working_format);
    if (!blur) {
        return CreateResult::failure(blur.error());
    }
    auto spatial_blur =
        SpatialBlur::create(device, width, height, working_format);
    if (!spatial_blur) {
        return CreateResult::failure(spatial_blur.error());
    }
    auto keyer = LayerKeyer::create(device, width, height, working_format);
    if (!keyer) {
        return CreateResult::failure(keyer.error());
    }
    auto mask_raster =
        MaskRaster::create(device, width, height, working_format);
    if (!mask_raster) {
        return CreateResult::failure(mask_raster.error());
    }
    auto matte = LayerMatte::create(device, width, height, working_format);
    if (!matte) {
        return CreateResult::failure(matte.error());
    }
    auto composite_a =
        device.create_texture(color_desc(width, height, working_format));
    auto composite_b =
        device.create_texture(color_desc(width, height, working_format));
    // Allocated at the presented extent from the start. It only ever changes
    // when the caller resizes this executor; the apron never touches it.
    auto output = device.create_texture(color_desc(width, height,
                                                   working_format));
    if (!composite_a || !composite_b || !output) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }

    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = blobs.format;
    vertex.bytecode = blobs.vertex;
    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = blobs.format;
    fragment.bytecode = blobs.fragment;
    fragment.sampler_count = 2U;
    fragment.uniform_buffer_count = 1U;
    auto pipeline =
        device.create_pipeline(vertex, fragment, working_format, false,
                               rhi::DepthState{});
    if (!pipeline) {
        return CreateResult::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }
    auto sampler = device.create_sampler(rhi::Filter::Linear);
    if (!sampler) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }

    std::vector<HybridSceneRenderer> layer_renderers;
    layer_renderers.reserve(1U);
    layer_renderers.push_back(std::move(renderer).value());
    CompositorSceneExecutor executor{
        device, limits, media_cache, std::move(layer_renderers),
        std::move(blur).value(),
        std::move(spatial_blur).value(),
        std::move(keyer).value(),
        std::move(mask_raster).value(), std::move(matte).value(),
        std::move(composite_a).value(), std::move(composite_b).value(),
        std::move(output).value(),
        std::move(pipeline).value(), std::move(sampler).value(),
        working_format};

    // The depth merge and the scene-level effects. Absent, not failed, where
    // the device cannot run compute: a Basic-only device has no Advanced frame
    // to apply them to, and an authored pass is refused at execution with a
    // reason rather than here without one.
    if (device.capabilities().compute && device.capabilities().storage_images
        && !depth_merge_bytecode().empty()) {
        rhi::ShaderDesc merge{};
        merge.stage = rhi::ShaderStage::Compute;
        merge.format = rhi::ShaderFormat::Dxil;
        merge.bytecode = depth_merge_bytecode();
        merge.sampler_count = 1U;
        merge.storage_texture_count = 1U;
        merge.uniform_buffer_count = 1U;
        auto merge_pipeline = device.create_compute_pipeline(merge);
        if (merge_pipeline) {
            executor.depth_merge_.emplace(std::move(merge_pipeline).value());
        }
    }
    executor.create_effects(width, height);

    return CreateResult::success(std::move(executor));
}

bool CompositorSceneExecutor::resize(rhi::Device& device, core::u32 width,
                                     core::u32 height) noexcept {
    if (width == 0U || height == 0U || device_ == nullptr ||
        device_->native_handle() != device.native_handle()) {
        return false;
    }
    if (this->width() == width && this->height() == height) {
        return true;
    }
    auto output = device.create_texture(
        color_desc(width, height, working_format_));
    if (!output) {
        return false;
    }
    if (!reserve_padded(width, height)) {
        return false;
    }
    output_ = std::move(output).value();
    return true;
}

bool CompositorSceneExecutor::reserve_padded(core::u32 width,
                                             core::u32 height) noexcept {
    if (width == 0U || height == 0U || device_ == nullptr) {
        return false;
    }
    if (padded_width() == width && padded_height() == height) {
        return true;
    }
    rhi::Device& device = *device_;
    try {
        std::vector<HybridSceneRenderer> renderers;
        renderers.reserve(layer_renderers_.size());
        for (std::size_t index = 0U; index < layer_renderers_.size(); ++index) {
            auto layer_limits = limits_;
            if (index != 0U) layer_limits.planar_reflections = false;
            auto renderer =
                HybridSceneRenderer::create(device, width, height, layer_limits, SceneEffectOwner::Compositor);
            if (!renderer) {
                return false;
            }
            renderers.push_back(std::move(renderer).value());
        }
        auto blur = GaussianBlur::create(
            device, width, height, working_format_);
        auto spatial_blur = SpatialBlur::create(
            device, width, height, working_format_);
        auto keyer = LayerKeyer::create(
            device, width, height, working_format_);
        auto mask_raster = MaskRaster::create(
            device, width, height, working_format_);
        auto matte = LayerMatte::create(
            device, width, height, working_format_);
        auto composite_a = device.create_texture(
            color_desc(width, height, working_format_));
        auto composite_b = device.create_texture(
            color_desc(width, height, working_format_));
        if (!blur || !spatial_blur || !keyer || !mask_raster || !matte ||
            !composite_a || !composite_b) {
            return false;
        }
        layer_renderers_ = std::move(renderers);
        blur_ = std::move(blur).value();
        spatial_blur_ = std::move(spatial_blur).value();
        keyer_ = std::move(keyer).value();
        mask_raster_ = std::move(mask_raster).value();
        matte_ = std::move(matte).value();
        composite_a_ = std::move(composite_a).value();
        composite_b_ = std::move(composite_b).value();
        output_is_a_ = true;
        // The effects follow the accumulators to the padded extent. Left at
        // the authored one, every authored atmosphere is refused on an extent
        // mismatch - which is how the first attempt at this failed.
        create_effects(width, height);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

void CompositorSceneExecutor::create_effects(core::u32 width,
                                             core::u32 height) noexcept {
    fog_.reset();
    bloom_.reset();
    motion_blur_.reset();
    plate_dof_.reset();
    scene_depth_ = rhi::Texture{};
    if (device_ == nullptr || width == 0U || height == 0U) {
        return;
    }
    rhi::Device& device = *device_;
    if (!device.capabilities().advanced_raster()
        || !device.capabilities().compute
        || !device.capabilities().storage_images) {
        return;
    }
    // In the working format, so the frame can be cropped straight out of the
    // effect's result: the crop refuses a format that differs from the
    // output's, and a conversion pass would be a second thing to get wrong.
    if (VolumetricFog::shaders_available()) {
        auto fog = VolumetricFog::create(device, width, height,
                                         FroxelGridLimits{}, working_format_);
        if (fog) {
            fog_.emplace(std::move(fog).value());
        }
    }
    if (Bloom::shaders_available()) {
        auto bloom = Bloom::create(device, width, height, working_format_);
        if (bloom) {
            bloom_.emplace(std::move(bloom).value());
        }
    }
    auto dof = PlateDepthOfField::create(device, width, height, working_format_);
    if (dof) plate_dof_.emplace(std::move(dof).value());
    auto motion = ObjectMotionBlur::create(device, width, height, working_format_, true);
    if (motion) motion_blur_.emplace(std::move(motion).value());
    auto depth = device.create_texture(scene_depth_desc(width, height));
    if (depth) {
        scene_depth_ = std::move(depth).value();
    }
}

MediaCacheStats CompositorSceneExecutor::media_cache_stats() const noexcept {
    return latest_media_cache_stats_;
}

ModelCacheStats CompositorSceneExecutor::model_cache_stats() const noexcept {
    return layer_renderers_.empty()
        ? ModelCacheStats{}
        : layer_renderers_.front().model_cache_stats();
}

bool CompositorSceneExecutor::refresh_model_source(
    const std::string& source) noexcept {
    return !layer_renderers_.empty()
        && layer_renderers_.front().refresh_model_source(source);
}

CompositorSceneResult CompositorSceneExecutor::execute(
    rhi::CommandList& commands, const compositor::CompositorGraph& graph,
    const scene::Scene& source, scene::CanvasRect slice,
    MediaCache* shared_media_cache) noexcept {
    stats_ = CompositorSceneStats{};
    const auto planned = plan_compositor_scene(graph, source);
    if (!planned) {
        return CompositorSceneResult::failure(
            planned.error() == CompositorScenePlanError::AllocationFailed
                ? RenderError::AllocationFailed
                : RenderError::InvalidConfiguration);
    }

    return execute_plan(
        commands, planned.value(), slice,
        planned.value().replaces_scene
            ? rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}
            : rhi::Color{source.environment.sky_color.x,
                         source.environment.sky_color.y,
                         source.environment.sky_color.z, 1.0F},
        std::nullopt, shared_media_cache);
}

CompositorSceneResult CompositorSceneExecutor::execute(
    rhi::CommandList& commands, const compositor::CompositorGraph& graph,
    const scene::Scene& source, const SceneRenderView& view,
    MediaCache* shared_media_cache) noexcept {
    stats_ = CompositorSceneStats{};
    const auto planned = plan_compositor_scene(graph, source);
    if (!planned) {
        return CompositorSceneResult::failure(
            planned.error() == CompositorScenePlanError::AllocationFailed
                ? RenderError::AllocationFailed
                : RenderError::InvalidConfiguration);
    }
    return execute_plan(
        commands, planned.value(), scene::whole(source.canvas),
        planned.value().replaces_scene
            ? rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}
            : rhi::Color{source.environment.sky_color.x,
                         source.environment.sky_color.y,
                         source.environment.sky_color.z, 1.0F},
        view, shared_media_cache);
}

CompositorSceneResult
CompositorSceneExecutor::execute(rhi::CommandList& commands,
                                 const scene::Scene& source,
                                 scene::CanvasRect slice,
                                 MediaCache* shared_media_cache) noexcept {
    stats_ = CompositorSceneStats{};
    const auto planned = plan_scene_effects(source);
    if (!planned) {
        return CompositorSceneResult::failure(
            planned.error() == CompositorScenePlanError::AllocationFailed
                ? RenderError::AllocationFailed
                : RenderError::InvalidConfiguration);
    }
    return execute_plan(
        commands, planned.value(), slice,
        rhi::Color{source.environment.sky_color.x,
                   source.environment.sky_color.y,
                   source.environment.sky_color.z, 1.0F},
        std::nullopt, shared_media_cache);
}

CompositorSceneResult
CompositorSceneExecutor::execute(rhi::CommandList& commands,
                                 const scene::Scene& source,
                                 const SceneRenderView& view,
                                 MediaCache* shared_media_cache) noexcept {
    stats_ = CompositorSceneStats{};
    const auto planned = plan_scene_effects(source);
    if (!planned) {
        return CompositorSceneResult::failure(
            planned.error() == CompositorScenePlanError::AllocationFailed
                ? RenderError::AllocationFailed
                : RenderError::InvalidConfiguration);
    }
    return execute_plan(
        commands, planned.value(), scene::whole(source.canvas),
        rhi::Color{source.environment.sky_color.x,
                   source.environment.sky_color.y,
                   source.environment.sky_color.z, 1.0F},
        view, shared_media_cache);
}

CompositorSceneResult CompositorSceneExecutor::execute_plan(
    rhi::CommandList& commands, const CompositorScenePlan& plan,
    scene::CanvasRect slice, rhi::Color initial_clear,
    const std::optional<SceneRenderView>& view,
    MediaCache* shared_media_cache) noexcept {
    graph_.begin();
    stats_ = CompositorSceneStats{};
    latest_media_cache_stats_ = MediaCacheStats{};
    last_readiness_ = RendererReadiness{};
    last_passes_ = RendererPassStatus{};
    const auto refuse_dof = [this](const char* reason) {
        ++stats_.plate_dof_refusals;
        last_passes_.plate_dof = PassState::Refused;
        last_passes_.plate_dof_reason = reason;
        static_cast<void>(core::report_idle_once("plate depth of field", reason));
        return CompositorSceneResult::failure(RenderError::InvalidConfiguration);
    };
    const bool dof_authored = plan.adjusted_scene.renderer_settings.plate_depth_of_field;
    const bool dof_requested = dof_authored && plan.adjusted_scene.renderer_settings.plate_dof_radius > 0U
        && view && view->plate_lens;
    const core::u64 profile = scene::renderer_settings_digest(plan.adjusted_scene.renderer_settings);
    if (last_renderer_profile_digest_ != profile
        || temporal_document_version_ != plan.adjusted_scene.document_version) {
        temporal_restart_baseline_ = 0U;
        for (const auto& renderer : layer_renderers_)
            temporal_restart_baseline_ += renderer.diagnostics().temporal_restarts;
        temporal_restarts_ = temporal_restart_frame_ = 0U;
        temporal_document_version_ = plan.adjusted_scene.document_version;
    }
    last_renderer_profile_digest_ = profile;
    last_scene_linear_ = false;
    keyer_.reset_stats();
    mask_raster_.reset_stats();
    if ((!view.has_value() &&
         !scene::contains(plan.adjusted_scene.canvas, slice)) ||
        !scene::valid(plan.adjusted_scene.camera)) {
        return CompositorSceneResult::failure(
            RenderError::InvalidConfiguration);
    }

    MediaCache& active_media_cache = shared_media_cache == nullptr
        ? media_cache_
        : *shared_media_cache;
    const bool owns_media_frame = shared_media_cache == nullptr;
    if (owns_media_frame) {
        active_media_cache.begin_frame();
    }
    struct FinishMediaFrame final {
        MediaCache* cache{};
        MediaCacheStats* snapshot{};
        bool owns{};
        ~FinishMediaFrame() {
            if (owns) {
                cache->end_frame();
            }
            *snapshot = cache->stats();
        }
    } finish_media_frame{
        &active_media_cache, &latest_media_cache_stats_, owns_media_frame};
    if(!active_media_cache.adopt_generation(scene::asset_generation_key(plan.adjusted_scene),plan.adjusted_scene.media)) {
        return CompositorSceneResult::failure(RenderError::Unavailable);
    }

    try {
        if (device_ == nullptr) {
            return CompositorSceneResult::failure(RenderError::Unavailable);
        }
        if (layer_renderers_.empty()) {
            return CompositorSceneResult::failure(RenderError::Unavailable);
        }
        // A scene with no drawable layers still has a renderer policy and an
        // environment clear. Enforce required Advanced and mark its colour domain.
        last_readiness_ = layer_renderers_.front().readiness(
            plan.adjusted_scene.renderer_settings);
        last_scene_linear_ =
            last_readiness_.selected == scene::RendererBackend::Advanced;
        if (!last_readiness_.satisfied) {
            return CompositorSceneResult::failure(
                RenderError::CapabilityUnsupported);
        }
        // The rectangle this frame renders. A wall slice is the authored
        // slice of the canvas. A view-driven frame - an ICVFX plate, a mapped
        // surface - fills this executor's own raster, whatever the canvas
        // measures: it resolves one asymmetric viewpoint itself and derives
        // nothing from a canvas rectangle. Sizing it to the canvas instead
        // rebuilt every target at the canvas extent while the crop and the
        // renderer's validation still expected this executor's, so any plate
        // or mapped output whose raster was not exactly the canvas refused
        // every frame. The jitter an explicit view applies is in its own
        // target pixels, which is one more reason the rectangle has to be the
        // target's.
        const scene::CanvasRect frame_rect = view.has_value()
            ? scene::CanvasRect{0U, 0U, width(), height()}
            : slice;
        // The apron this frame needs, decided from the same function the
        // renderers use so the two cannot disagree about the rectangle.
        //
        // An inner frustum widens its projection as well as its target. Other
        // explicitly mapped views retain their authored raster policy.
        const auto padded = view.has_value()
            ? (view->guard_band
                ? padded_view_for(plan.adjusted_scene, width(), height(),
                    last_readiness_.selected == scene::RendererBackend::Advanced, dof_requested)
                : core::Result<PaddedSlice, SliceApronError>::success(
                    unpadded(frame_rect)))
            : padded_slice_for(
                  plan.adjusted_scene, frame_rect, width(), height(),
                  last_readiness_.selected
                      == scene::RendererBackend::Advanced);
        if (!padded) {
            if (dof_requested) return refuse_dof("plate filter guard exceeds the authored limit");
            return CompositorSceneResult::failure(
                RenderError::InvalidConfiguration);
        }
        auto render_view = view;
        if (render_view && render_view->guard_band) {
            auto& matrices = render_view->matrices;
            const float sx = static_cast<float>(width()) / padded.value().render.width;
            const float sy = static_cast<float>(height()) / padded.value().render.height;
            for (core::u32 column = 0U; column < 4U; ++column) {
                matrices.projection.set(0U, column, matrices.projection.at(0U, column) * sx);
                matrices.projection.set(1U, column, matrices.projection.at(1U, column) * sy);
            }
            matrices.view_projection = scene::multiply(matrices.projection, matrices.view);
        }
        // Everything that carries the apron is sized to it before anything is
        // recorded. Exact, not a reservation for the largest apron the project
        // permits: every neighbourhood pass in here clamps at its texture edge,
        // so the texture edge has to be the padded edge or a node at the canvas
        // boundary clamps somewhere the full-canvas render does not.
        if (!reserve_padded(
                padded.value().render.width, padded.value().render.height)) {
            return CompositorSceneResult::failure(
                RenderError::AllocationFailed);
        }
        while (layer_renderers_.size() < plan.layers.size()) {
            // The first layer captures the complete residency scene once and
            // every following layer samples that exact capture. Constructing
            // a nested full renderer for each borrower wasted GPU memory and
            // made cold multi-plate compositions stall before their first draw.
            auto layer_limits = limits_;
            layer_limits.planar_reflections = false;
            auto renderer =
                HybridSceneRenderer::create(
                    *device_, padded_width(), padded_height(), layer_limits, SceneEffectOwner::Compositor);
            if (!renderer) {
                return CompositorSceneResult::failure(renderer.error());
            }
            layer_renderers_.push_back(std::move(renderer).value());
        }
        // Isolated layer renderers are deliberately transparent, so the
        // compositor owns the visible environment behind them. Use the one
        // capability/policy decision above rather than re-deriving backend
        // selection here. A transparent graph replacement is still exact
        // transparent black, Basic remains the reference path, and only the
        // authored sky RGB receives photographic exposure.
        // The same options the layer renders will use, so the background's
        // view is resolved from exactly the inputs the geometry's is. Only the
        // transparency flag differs, and the background does not read it.
        SceneRenderOptions background_options{};
        background_options.view = render_view;

        rhi::Color accumulator_clear = initial_clear;
        if (accumulator_clear.a > 0.0F &&
            last_readiness_.selected == scene::RendererBackend::Advanced) {
            const float exposure = scene::camera_exposure_multiplier(
                plan.adjusted_scene.camera);
            accumulator_clear.r *= exposure;
            accumulator_clear.g *= exposure;
            accumulator_clear.b *= exposure;
        }
        // The environment, if one is resident, drawn as the background the
        // comment above promises. It owns the clear and fills every pixel, so
        // the flat one is the fallback rather than something drawn under it.
        //
        // Withheld when the composite is meant to carry transparency: the sky
        // is opaque by construction, and filling an alpha-zero accumulator
        // with it would quietly make a transparent output opaque.
        //
        // A sky-only scene still needs asset preparation and a current readiness
        // report. Without a geometry pass its default Ready=false held Program
        // forever. Use the ordinary renderer's residency contract, then draw
        // the background below; no transparent layer is added to the stack.
        if (plan.layers.empty()) {
            auto empty_scene = isolate(plan.adjusted_scene, CompositorLayerPass{});
            auto options = background_options;
            options.transparent_background = true;
            options.residency_scene = &plan.adjusted_scene;
            auto request = layer_renderers_.front().convenience_request(
                empty_scene, frame_rect, options, width(), height());
            request.padded = padded.value();
            request.defer_scene_effects = true;
            auto prepared = layer_renderers_.front().render_frame(commands, request, &active_media_cache);
            last_passes_ = layer_renderers_.front().last_products().passes;
            if (!prepared) return CompositorSceneResult::failure(RenderError::BackendFailure);
        }

        // Any layer renderer would derive the same view from the same options;
        // the first is used because there is always at least one. It has not
        // rendered this frame yet, so the environment it draws is the one it
        // adopted on an earlier frame - a decode that lands this frame lights
        // the objects one frame before it appears behind them, which is the
        // same lag the HDRI path already has against authoring. Physical sky
        // tables are synchronous and adopted before this draw, so an animated
        // sun and its atmosphere always describe the same show frame.
        const bool sky_drawn = accumulator_clear.a > 0.0F
            && !layer_renderers_.empty()
            && layer_renderers_.front().record_background(
                commands, composite_a_, plan.adjusted_scene, frame_rect,
                background_options);
        if (accumulator_clear.a > 0.0F && plan.adjusted_scene.environment.physical_sky.enabled
            && (last_readiness_.selected != scene::RendererBackend::Advanced || !sky_drawn))
            return CompositorSceneResult::failure(RenderError::CapabilityUnsupported);
        if (!sky_drawn
            && !graph_.clear(commands, core::RenderPass::Composite, composite_a_, accumulator_clear)) {
            return CompositorSceneResult::failure(RenderError::BackendFailure);
        }
        if (sky_drawn) {
            ++stats_.environment_backgrounds;
        }
        const rhi::Texture* accumulator = &composite_a_;

        // Atmosphere and bloom are applied once, below, to the finished
        // composite. Atmosphere is light scattered by the air between the
        // camera and whatever it can see - not a property of any one layer -
        // and applying it inside each layer's render fogged a shared
        // background once per layer, while the alpha-weighted alternative
        // left the sky with no atmosphere at all. So every layer is told to
        // leave both alone, and the nearest surface any layer drew is
        // gathered here for the atmosphere to resolve against.
        const bool advanced =
            last_readiness_.selected == scene::RendererBackend::Advanced;
        const scene::RendererSettings authored =
            scene::clamped(plan.adjusted_scene.renderer_settings);
        if (dof_requested) {
            if (!advanced) {
                last_passes_.plate_dof = PassState::BackendUnsupported;
                last_passes_.plate_dof_reason = "plate depth of field requires Advanced";
                return CompositorSceneResult::failure(RenderError::CapabilityUnsupported);
            }
            if (!view->guard_band) return refuse_dof("plate depth of field requires a guarded plate");
            if (!valid(*view->plate_lens)) return refuse_dof("missing or invalid physical focus, focal length, aperture or wall geometry");
            if (!plate_dof_) return refuse_dof("plate depth of field targets are unavailable");
            // A blurred, masked, matted or blended layer merges its geometry's
            // depth like any other, so the plate focuses it where it stands.
            // Refusing the whole plate for one layer's look blanked the wall.
        }
        const bool fog_authored =
            plan.adjusted_scene.environment.fog_density > 0.0F;
        const bool bloom_authored = authored.bloom_strength > 0.0F;
        const bool motion_authored = authored.motion_blur && authored.motion_blur_radius > 0U;
        const auto refuse_motion = [this](const char* reason) {
            last_passes_.motion_blur = PassState::Refused;
            last_passes_.motion_blur_reason = reason;
            static_cast<void>(core::report_idle_once("object motion blur", reason));
            return CompositorSceneResult::failure(RenderError::InvalidConfiguration);
        };
        // A spatial filter, mask or matte changes a layer's visible coverage
        // after geometry, so its motion product no longer describes what is
        // on screen. Such a layer joins the merge as a still occluder - its
        // depth, no motion - and composites sharp in time, rather than the
        // whole frame being refused for one layer's look. A blend mode alone
        // leaves coverage as drawn, so that layer keeps its real motion.
        const auto coverage_processed = [](const CompositorLayerPass& pass) noexcept {
            return !pass.spatial_effects.empty() || pass.blur_radius_px > 0.0F
                || pass.mask_program || pass.matte;
        };
        std::size_t still_layers{};
        if (advanced && motion_authored) {
            if (!motion_blur_ || !motion_blur_->begin(commands))
                return refuse_motion("object motion targets are unavailable");
        }
        // Refused rather than skipped, with the reason published, for the
        // reason the renderer refuses its own passes: one present on one node
        // and absent on its neighbour is a seam down the middle of the wall,
        // and from the wall a refusal and a switch that is off look the same.
        const auto refuse_fog = [this](const char* reason) {
            ++stats_.fog_refusals;
            static_cast<void>(core::report_idle_once("atmosphere", reason));
            last_passes_.fog = PassState::Refused;
            last_passes_.fog_refusal = reason;
            return CompositorSceneResult::failure(
                RenderError::InvalidConfiguration);
        };
        const auto refuse_bloom = [this](const char* reason) {
            ++stats_.bloom_refusals;
            static_cast<void>(core::report_idle_once("bloom", reason));
            last_passes_.bloom = PassState::Refused;
            last_passes_.bloom_refusal = reason;
            return CompositorSceneResult::failure(
                RenderError::InvalidConfiguration);
        };
        const bool merge_depth =
            advanced && (fog_authored || dof_requested);
        if (merge_depth) {
            if (!depth_merge_.has_value() || !scene_depth_
                || scene_depth_.width() != padded_width()
                || scene_depth_.height() != padded_height()
                || !graph_.clear(commands, core::RenderPass::Composite, scene_depth_,
                                   rhi::Color{0.0F, 0.0F, 0.0F, 0.0F})) {
                if (dof_requested) return refuse_dof("plate scene depth is unavailable");
                return refuse_fog(fog_refusal_depth);
            }
        }
        const auto merge_layer_depth = [&](const rhi::Texture& layer_depth) {
            DepthMergeConstants constants{};
            constants.extent[0U] = static_cast<float>(padded_width());
            constants.extent[1U] = static_cast<float>(padded_height());
            const std::array<rhi::SampledTexture, 1U> sources{
                rhi::SampledTexture{&layer_depth, &sampler_}};
            const std::array<rhi::StorageTexture, 1U> targets{
                rhi::StorageTexture{&scene_depth_, 0U, 0U, true}};
            rhi::ComputeDispatch work{};
            work.pipeline = &*depth_merge_;
            work.sampled_textures = sources;
            work.storage_textures = targets;
            work.uniform = std::span<const core::u8>{
                reinterpret_cast<const core::u8*>(&constants),
                sizeof(constants)};
            work.groups_x = (padded_width() + 7U) / 8U;
            work.groups_y = (padded_height() + 7U) / 8U;
            return graph_.dispatch(commands, core::RenderPass::Depth, work);
        };

        const rhi::Texture* shared_planar = nullptr;
        for (std::size_t layer_index = 0U; layer_index < plan.layers.size();
             ++layer_index) {
            const CompositorLayerPass& pass = plan.layers[layer_index];
            HybridSceneRenderer& renderer = layer_renderers_[layer_index];
            scene::Scene layer = isolate(plan.adjusted_scene, pass);
            SceneRenderOptions options{};
            options.transparent_background = true;
            options.residency_scene = &plan.adjusted_scene;
            options.shared_planar = shared_planar;
            options.view = render_view;
            options.depth_only_sources = pass.depth_only_nodes;
            // The authored slice, not the padded one: the renderer expands it
            // itself, from the same decision made above, and handing it an
            // already-expanded rectangle would apply the apron twice.
            //
            // The request is built here rather than through the convenience
            // overload so the unpadded output extent can be filled in. The
            // renderer's own targets are already padded, so it cannot work that
            // extent out for itself.
            RenderFrameRequest request = renderer.convenience_request(
                layer, frame_rect, options, width(), height());
            // Decided once, above, from the scene before it was split into
            // layers. Each isolated layer has had its effects lifted into its
            // pass, so none of them can work out the apron the frame needs.
            request.padded = padded.value();
            // Atmosphere, object shutter blur and bloom belong to the composite.
            request.defer_scene_effects = true;
            auto layer_result =
                renderer.render_frame(commands, request, &active_media_cache);
            if (!layer_result) {
                last_readiness_ =
                    renderer.diagnostics().last_readiness;
                // A refusal's reason travels with it, so the operator is told
                // why the frame stopped rather than that it did.
                last_passes_ = renderer.last_products().passes;
                return CompositorSceneResult::failure(
                    last_readiness_.satisfied
                        ? RenderError::BackendFailure
                        : RenderError::CapabilityUnsupported);
            }
            if (!shared_planar) shared_planar = renderer.planar_target();
            const RenderProducts& products = renderer.last_products();
            if (advanced && motion_authored) {
                const bool still = coverage_processed(pass);
                if (!products.object_velocity
                    || !motion_blur_->merge(commands, *products.object_velocity, still))
                    return refuse_motion("visible object motion could not be combined");
                still_layers += still ? 1U : 0U;
            }
            if (layer_index == 0U) {
                last_readiness_ = products.readiness;
                last_passes_ = products.passes;
                last_scene_linear_ = products.scene_linear;
            } else if (products.scene_linear != last_scene_linear_ ||
                       products.readiness.selected !=
                           last_readiness_.selected) {
                return CompositorSceneResult::failure(
                    RenderError::InvalidConfiguration);
            }
            // Every layer owns its uploads. A later layer still loading must
            // hold presentation even when the first layer is already ready.
            last_passes_.geometry.assets.ready = last_passes_.geometry.assets.ready && products.passes.geometry.assets.ready;
            last_passes_.geometry.baked_ready = last_passes_.geometry.baked_ready && products.passes.geometry.baked_ready;
            if (!products.passes.geometry.baked_ready) last_passes_.geometry.baked_state = products.passes.geometry.baked_state;
            last_passes_.geometry.reflection_probes = std::max(last_passes_.geometry.reflection_probes, products.passes.geometry.reflection_probes);
            last_passes_.geometry.reflection_refusals = std::max(last_passes_.geometry.reflection_refusals, products.passes.geometry.reflection_refusals);
            last_passes_.geometry.planar_passes = std::max(last_passes_.geometry.planar_passes, products.passes.geometry.planar_passes);
            last_passes_.geometry.planar_refusals = std::max(last_passes_.geometry.planar_refusals, products.passes.geometry.planar_refusals);
            if (!products.passes.geometry.reflection_reason.empty()) last_passes_.geometry.reflection_reason = products.passes.geometry.reflection_reason;
            if (products.passes.geometry.texture_streaming.state !=
                    TextureStreamState::Ready) {
                last_passes_.geometry.texture_streaming.state =
                    products.passes.geometry.texture_streaming.state;
            }
            if (merge_depth) {
                // The nearest surface so far, folded in as each layer lands.
                if (products.depth == nullptr
                    || !merge_layer_depth(*products.depth)) {
                    if (dof_requested) return refuse_dof("plate scene depth is unavailable");
                    return refuse_fog(fog_refusal_depth);
                }
            }
            std::vector<scene::SpatialEffectOperation> operations =
                pass.spatial_effects;
            if (operations.empty() && pass.blur_radius_px > 0.0F) {
                operations.push_back(scene::SpatialEffectOperation{
                    scene::EffectKind::Blur, pass.blur_radius_px, {}, {}});
            }
            // The *published* colour, not the renderer's own target.
            //
            // `target()` is the raw scene target - what the geometry pass drew,
            // before the temporal resolve, the spatial filter and bloom, each of
            // which publishes its result into a texture of its own and leaves
            // that one untouched. Reading `target()` here silently discarded all
            // three: an authored bloom at full strength changed nothing on
            // screen, and neither did antialiasing, because the frame the
            // compositor picked up was the one from before any of them ran.
            const rhi::Texture* above =
                products.color != nullptr ? products.color : &renderer.target();

            const rhi::Texture* tested_depth = &renderer.depth_target();
            bool gaussian_layer{};
            bool box_layer{};
            bool directional_layer{};
            bool chroma_key_layer{};
            bool luma_key_layer{};
            bool masked_layer{};
            for (const scene::SpatialEffectOperation& operation : operations) {
                switch (operation.kind) {
                case scene::EffectKind::Blur: {
                    gaussian_layer = gaussian_layer || operation.value > 0.0F;
                    // Gaussian sample layout is a show-wide setting.
                    auto result = blur_.apply(
                        commands, *above, operation.value,
                        kernel_of(plan.adjusted_scene.effect_settings));
                    if (!result) {
                        return CompositorSceneResult::failure(result.error());
                    }
                    above = result.value();
                    break;
                }
                case scene::EffectKind::BoxBlur: {
                    box_layer = box_layer ||
                        !scene::box_blur_is_identity(operation.box_blur);
                    auto result = spatial_blur_.apply_box(
                        commands, *above,
                        BoxBlurKernel{operation.box_blur.radius_px,
                                      operation.box_blur.passes});
                    if (!result) {
                        return CompositorSceneResult::failure(result.error());
                    }
                    above = result.value();
                    break;
                }
                case scene::EffectKind::DirectionalBlur: {
                    directional_layer = directional_layer ||
                        !scene::directional_blur_is_identity(
                            operation.directional_blur);
                    auto result = spatial_blur_.apply_directional(
                        commands, *above,
                        DirectionalBlurKernel{
                            operation.directional_blur.length_px,
                            operation.directional_blur.angle_turns,
                            scene::clamped(plan.adjusted_scene.effect_settings)
                                .directional_blur_taps});
                    if (!result) {
                        return CompositorSceneResult::failure(result.error());
                    }
                    above = result.value();
                    break;
                }
                case scene::EffectKind::ChromaKey: {
                    chroma_key_layer = chroma_key_layer ||
                        !scene::chroma_key_is_identity(
                            operation.chroma_key);
                    ChromaKeyParameters parameters{};
                    parameters.key_color =
                        rhi::Color{operation.chroma_key.key_color.x,
                                   operation.chroma_key.key_color.y,
                                   operation.chroma_key.key_color.z, 1.0F};
                    parameters.tolerance = operation.chroma_key.tolerance;
                    parameters.softness = operation.chroma_key.softness;
                    parameters.morphology_px =
                        operation.chroma_key.morphology_px;
                    parameters.spill_suppression =
                        operation.chroma_key.spill_suppression;
                    parameters.mix = operation.chroma_key.mix;
                    auto result = keyer_.apply_chroma(
                        commands, *above, *tested_depth, parameters);
                    if (!result || result.value().color == nullptr ||
                        result.value().depth != tested_depth) {
                        return CompositorSceneResult::failure(
                            result ? RenderError::InvalidConfiguration
                                   : result.error());
                    }
                    above = result.value().color;
                    tested_depth = result.value().depth;
                    break;
                }
                case scene::EffectKind::LumaKey: {
                    luma_key_layer = luma_key_layer ||
                        !scene::luma_key_is_identity(operation.luma_key);
                    LumaKeyParameters parameters{};
                    parameters.low_threshold =
                        operation.luma_key.low_threshold;
                    parameters.high_threshold =
                        operation.luma_key.high_threshold;
                    parameters.low_softness =
                        operation.luma_key.low_softness;
                    parameters.high_softness =
                        operation.luma_key.high_softness;
                    parameters.invert = operation.luma_key.invert;
                    parameters.mix = operation.luma_key.mix;
                    auto result = keyer_.apply_luma(
                        commands, *above, *tested_depth, parameters);
                    if (!result || result.value().color == nullptr ||
                        result.value().depth != tested_depth) {
                        return CompositorSceneResult::failure(
                            result ? RenderError::InvalidConfiguration
                                   : result.error());
                    }
                    above = result.value().color;
                    tested_depth = result.value().depth;
                    break;
                }
                case scene::EffectKind::Brightness:
                case scene::EffectKind::Saturation:
                case scene::EffectKind::Levels:
                case scene::EffectKind::LiftGammaGain:
                case scene::EffectKind::ContrastPivot:
                case scene::EffectKind::ColorMatrix:
                case scene::EffectKind::WhiteBalance:
                case scene::EffectKind::Curves:
                case scene::EffectKind::FilmGrain:
                case scene::EffectKind::Count:
                    return CompositorSceneResult::failure(
                        RenderError::InvalidConfiguration);
                }
            }
            // A layer mask owns final layer coverage, after the drawable's
            // complete ordered effect stack. This preserves the ordinary
            // editorial expectation that blur/key output is clipped by the
            // layer mask rather than letting a later blur leak outside it.
            if (pass.mask_program.has_value()) {
                const scene::MaskProgram& program = *pass.mask_program;
                masked_layer = !mask_program_is_identity(program);
                const auto mapping = source_layer_mapping(
                    plan.adjusted_scene, program.target,
                    render_view.has_value() ? &*render_view : nullptr,
                    padded.value(), padded_width(), padded_height());
                if (!mapping.has_value()) {
                    // A volume or imported model has no one surface for the
                    // mask's coordinates to name, so there is nowhere
                    // honest to put it.
                    static_cast<void>(core::report_idle_once(
                        "layer mask",
                        "masked layer has no source plane; masks need a "
                        "quad or plane"));
                    return CompositorSceneResult::failure(
                        RenderError::CapabilityUnsupported);
                }
                auto result = mask_raster_.apply(
                    commands, *above, *tested_depth, program, *mapping);
                if (!result || result.value().color == nullptr ||
                    result.value().depth != tested_depth) {
                    return CompositorSceneResult::failure(
                        result ? RenderError::InvalidConfiguration
                               : result.error());
                }
                above = result.value().color;
                tested_depth = result.value().depth;
            }
            // The isolated colour was already tested against this exact depth
            // target while SceneRenderer produced it (including depth-only
            // occluders). The fullscreen premultiplied blend must not invent a
            // card depth or re-test filtered coverage, but it does consume the
            // pair's provenance here: a processor that substitutes or drops
            // tested depth makes the layer uncompositable and fails closed.
            if (tested_depth != &renderer.depth_target()) {
                return CompositorSceneResult::failure(
                    RenderError::InvalidConfiguration);
            }
            rhi::Texture& destination =
                accumulator == &composite_a_ ? composite_b_ : composite_a_;
            const std::array<rhi::SampledTexture, 2U> inputs{{
                rhi::SampledTexture{accumulator, &sampler_},
                rhi::SampledTexture{above, &sampler_},
            }};
            const CompositeUniform uniform{
                static_cast<float>(static_cast<core::u8>(pass.blend)), {last_scene_linear_ ? 1.0F : 0.0F, 0.0F, 0.0F}};
            if (!graph_.fullscreen(commands, core::RenderPass::Composite,
                    destination, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F},
                    composite_pipeline_, inputs, bytes(uniform))) {
                return CompositorSceneResult::failure(
                    RenderError::BackendFailure);
            }
            accumulator = &destination;
            ++stats_.layers;
            ++stats_.composite_passes;
            if (gaussian_layer) {
                ++stats_.blurred_layers;
            }
            if (box_layer) {
                ++stats_.box_blurred_layers;
            }
            if (directional_layer) {
                ++stats_.directional_blurred_layers;
            }
            if (chroma_key_layer) {
                ++stats_.chroma_keyed_layers;
            }
            if (luma_key_layer) {
                ++stats_.luma_keyed_layers;
            }
            if (masked_layer) {
                ++stats_.masked_layers;
            }
        }
        // Atmosphere over the whole composite - sky and every layer alike -
        // resolved against the nearest surface any layer drew, then bloom
        // over that. Both at the padded extent, before the crop, in linear
        // light: the same order for the same reasons as the renderer's own
        // path, which this replaces for every frame that reaches a screen.
        if (fog_authored && !plan.layers.empty()) {
            if (!advanced) {
                last_passes_.fog = PassState::BackendUnsupported;
            } else {
                // The air is lit by exactly the lights that lit the surfaces,
                // read from the buffer the first layer's draw uploaded. Every
                // layer keeps the whole light list, so any would do; the
                // first always exists.
                HybridSceneRenderer& first = layer_renderers_.front();
                const FogFrame fog_frame = fog_frame_for(
                    plan.adjusted_scene, render_view.has_value() ? &*render_view : nullptr,
                    padded.value(), padded_width(), padded_height());
                if (!fog_.has_value()
                    || !fog_->record(commands, *accumulator, scene_depth_,
                                     first.shadow_atlas_target(),
                                     first.light_storage(),
                                     first.light_shadow_storage(),
                                     first.light_storage_count(),
                                     plan.adjusted_scene,
                                     fog_frame.view_projection,
                                     fog_frame.grid_canvas,
                                     fog_frame.render,
                                     fog_frame.clip_planes,
                                     first.fog_environment(), &first.ies_atlas())) {
                    return refuse_fog(fog_refusal_targets);
                }
                accumulator = &fog_->target();
                last_passes_.fog = PassState::Active;
                last_passes_.fog_refusal = {};
                ++stats_.fog_volumes;
            }
        }
        if (motion_authored) {
            if (!advanced) {
                last_passes_.motion_blur = PassState::BackendUnsupported;
                last_passes_.motion_blur_reason = "object motion blur requires Advanced";
            } else {
                const float shutter = object_shutter_fraction(plan.adjusted_scene,
                    render_view ? &*render_view : nullptr);
                const float motion_width = render_view ? static_cast<float>(padded_width())
                    : static_cast<float>(plan.adjusted_scene.canvas.width) * padded_width() / padded.value().render.width;
                const float motion_height = render_view ? static_cast<float>(padded_height())
                    : static_cast<float>(plan.adjusted_scene.canvas.height) * padded_height() / padded.value().render.height;
                auto blurred = motion_blur_->apply(commands, *accumulator, motion_blur_->merged_motion(),
                    shutter, authored.motion_blur_radius, motion_width, motion_height);
                if (!blurred) return refuse_motion("object motion reconstruction failed");
                accumulator = blurred.value();
                last_passes_.motion_blur = PassState::Active;
                last_passes_.motion_blur_reason = shutter == 0.0F
                    ? "shutter duration or show rate is zero"
                    : (still_layers != 0U
                           ? "blurred, masked or matted layers are not motion blurred"
                           : "");
            }
        }
        if (dof_requested) {
            const auto blurred = plate_dof_->apply(commands, *accumulator, scene_depth_,
                *render_view->plate_lens, render_view->matrices.view_projection,
                width(), height(), authored.plate_dof_radius);
            if (!blurred) return refuse_dof("plate depth of field gather failed");
            accumulator = blurred.value();
            last_passes_.plate_dof = PassState::Active;
            last_passes_.plate_dof_reason = {};
            ++stats_.plate_dof_passes;
        } else if (dof_authored) {
            last_passes_.plate_dof = PassState::Idle;
            last_passes_.plate_dof_reason = authored.plate_dof_radius == 0U
                ? "plate maximum radius is zero" : "this view is not a tracked plate";
            static_cast<void>(core::report_idle_once("plate depth of field", last_passes_.plate_dof_reason.data()));
        }
        if (bloom_authored && !plan.layers.empty()) {
            if (!advanced) {
                last_passes_.bloom = PassState::BackendUnsupported;
            } else {
                const auto chain = bloom_plan_for(
                    plan.adjusted_scene, render_view.has_value() ? &*render_view : nullptr,
                    padded.value(), padded_width(), padded_height());
                if (!chain) {
                    return refuse_bloom(
                        chain.error() == BloomChainError::Unaligned
                            ? bloom_refusal_unaligned
                            : bloom_refusal_targets);
                }
                // A canvas too small to halve has no chain and nothing to
                // spread; that is the pass's own answer, not a refusal.
                if (!chain.value().levels.empty()) {
                    if (!bloom_.has_value()
                        || !bloom_->record(commands, *accumulator,
                                           chain.value())
                        || !bloom_->combine(commands, *accumulator,
                                            authored.bloom_strength)) {
                        return refuse_bloom(bloom_refusal_targets);
                    }
                    accumulator = &bloom_->target();
                    ++stats_.bloom_chains;
                }
                last_passes_.bloom = PassState::Active;
                last_passes_.bloom_refusal = {};
            }
        }
        stats_.keyer_passes = keyer_.stats().recorded_passes;
        stats_.keyer_pixels = keyer_.stats().processed_pixels;
        stats_.mask_passes = mask_raster_.stats().recorded_passes;
        stats_.mask_pixels = mask_raster_.stats().processed_pixels;
        output_is_a_ = accumulator == &composite_a_;
        // The one crop, after the last pass that reads a neighbouring pixel.
        // Cropping earlier would hand the next such pass a boundary again,
        // which is the whole thing the apron exists to remove.
        core::u64 total_restarts = 0U;
        for (const auto& renderer : layer_renderers_)
            total_restarts += renderer.diagnostics().temporal_restarts;
        const core::u64 restarts = total_restarts >= temporal_restart_baseline_
            ? total_restarts - temporal_restart_baseline_ : 0U;
        if (restarts != temporal_restarts_) {
            temporal_restarts_ = restarts;
            temporal_restart_frame_ = plan.adjusted_scene.show_frame;
        }
        const rhi::ViewportRect crop{
            padded.value().crop.x, padded.value().crop.y,
            padded.value().crop.width, padded.value().crop.height};
        if (!graph_.copy(commands, core::RenderPass::Output, *accumulator, crop, output_)) {
            return CompositorSceneResult::failure(RenderError::BackendFailure);
        }
        return CompositorSceneResult::success(&target());
    } catch (const std::bad_alloc&) {
        return CompositorSceneResult::failure(RenderError::AllocationFailed);
    }
}

} // namespace mediaserver::render
