#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/effect_execution.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/compositor_layer_frag_dxil.h>
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

[[nodiscard]] rhi::TextureDesc color_desc(core::u32 width,
                                          core::u32 height) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
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
    return shader_blobs().present && SceneRenderer::shaders_available() &&
           GaussianBlur::shaders_available() &&
           SpatialBlur::shaders_available() &&
           LayerKeyer::shaders_available() &&
           MaskRaster::shaders_available() &&
           LayerMatte::shaders_available();
}

CompositorSceneExecutor::CompositorSceneExecutor(
    rhi::Device& device, RendererLimits limits, MediaCacheConfig media_cache,
    std::vector<SceneRenderer> layer_renderers, GaussianBlur blur,
    SpatialBlur spatial_blur, LayerKeyer keyer, MaskRaster mask_raster,
    LayerMatte matte, rhi::Texture composite_a, rhi::Texture composite_b,
    rhi::GraphicsPipeline composite_pipeline, rhi::Sampler sampler) noexcept
    : device_(&device), limits_(limits),
      layer_renderers_(std::move(layer_renderers)),
      media_cache_(media_cache), blur_(std::move(blur)),
      spatial_blur_(std::move(spatial_blur)),
      keyer_(std::move(keyer)),
      mask_raster_(std::move(mask_raster)),
      matte_(std::move(matte)),
      composite_a_(std::move(composite_a)),
      composite_b_(std::move(composite_b)),
      composite_pipeline_(std::move(composite_pipeline)),
      sampler_(std::move(sampler)) {}

CreateResult CompositorSceneExecutor::create(rhi::Device& device,
                                             core::u32 width, core::u32 height,
                                             const RendererLimits& limits,
                                             MediaCacheConfig media_cache) {
    const ShaderBlobs blobs = shader_blobs();
    if (!blobs.present || !device.supports(blobs.format) ||
        !SceneRenderer::shaders_available() ||
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

    auto renderer = SceneRenderer::create(device, width, height, limits);
    if (!renderer) {
        return CreateResult::failure(renderer.error());
    }
    auto blur = GaussianBlur::create(device, width, height);
    if (!blur) {
        return CreateResult::failure(blur.error());
    }
    auto spatial_blur = SpatialBlur::create(device, width, height);
    if (!spatial_blur) {
        return CreateResult::failure(spatial_blur.error());
    }
    auto keyer = LayerKeyer::create(device, width, height);
    if (!keyer) {
        return CreateResult::failure(keyer.error());
    }
    auto mask_raster = MaskRaster::create(device, width, height);
    if (!mask_raster) {
        return CreateResult::failure(mask_raster.error());
    }
    auto matte = LayerMatte::create(device, width, height);
    if (!matte) {
        return CreateResult::failure(matte.error());
    }
    auto composite_a = device.create_texture(color_desc(width, height));
    auto composite_b = device.create_texture(color_desc(width, height));
    if (!composite_a || !composite_b) {
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
        device.create_pipeline(vertex, fragment, rhi::TextureFormat::Rgba8Unorm,
                               false, rhi::DepthState{});
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

    std::vector<SceneRenderer> layer_renderers;
    layer_renderers.reserve(1U);
    layer_renderers.push_back(std::move(renderer).value());
    return CreateResult::success(CompositorSceneExecutor{
        device, limits, media_cache, std::move(layer_renderers),
        std::move(blur).value(),
        std::move(spatial_blur).value(),
        std::move(keyer).value(),
        std::move(mask_raster).value(), std::move(matte).value(),
        std::move(composite_a).value(), std::move(composite_b).value(),
        std::move(pipeline).value(), std::move(sampler).value()});
}

bool CompositorSceneExecutor::resize(rhi::Device& device, core::u32 width,
                                     core::u32 height) noexcept {
    if (width == 0U || height == 0U || device_ == nullptr ||
        device_->backend_handle() != device.backend_handle()) {
        return false;
    }
    if (this->width() == width && this->height() == height) {
        return true;
    }
    try {
        std::vector<SceneRenderer> renderers;
        renderers.reserve(layer_renderers_.size());
        for (std::size_t index = 0U; index < layer_renderers_.size(); ++index) {
            auto renderer =
                SceneRenderer::create(device, width, height, limits_);
            if (!renderer) {
                return false;
            }
            renderers.push_back(std::move(renderer).value());
        }
        auto blur = GaussianBlur::create(device, width, height);
        auto spatial_blur = SpatialBlur::create(device, width, height);
        auto keyer = LayerKeyer::create(device, width, height);
        auto mask_raster = MaskRaster::create(device, width, height);
        auto composite_a = device.create_texture(color_desc(width, height));
        auto composite_b = device.create_texture(color_desc(width, height));
        if (!blur || !spatial_blur || !keyer || !mask_raster ||
            !composite_a || !composite_b) {
            return false;
        }
        layer_renderers_ = std::move(renderers);
        blur_ = std::move(blur).value();
        spatial_blur_ = std::move(spatial_blur).value();
        keyer_ = std::move(keyer).value();
        mask_raster_ = std::move(mask_raster).value();
        composite_a_ = std::move(composite_a).value();
        composite_b_ = std::move(composite_b).value();
        output_is_a_ = true;
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

MediaCacheStats CompositorSceneExecutor::media_cache_stats() const noexcept {
    return latest_media_cache_stats_;
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
    stats_ = CompositorSceneStats{};
    latest_media_cache_stats_ = MediaCacheStats{};
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

    try {
        if (device_ == nullptr) {
            return CompositorSceneResult::failure(RenderError::Unavailable);
        }
        while (layer_renderers_.size() < plan.layers.size()) {
            auto renderer =
                SceneRenderer::create(*device_, width(), height(), limits_);
            if (!renderer) {
                return CompositorSceneResult::failure(renderer.error());
            }
            layer_renderers_.push_back(std::move(renderer).value());
        }
        if (!commands.clear(composite_a_, initial_clear)) {
            return CompositorSceneResult::failure(RenderError::BackendFailure);
        }
        const rhi::Texture* accumulator = &composite_a_;
        for (std::size_t layer_index = 0U; layer_index < plan.layers.size();
             ++layer_index) {
            const CompositorLayerPass& pass = plan.layers[layer_index];
            SceneRenderer& renderer = layer_renderers_[layer_index];
            scene::Scene layer = isolate(plan.adjusted_scene, pass);
            SceneRenderOptions options{};
            options.transparent_background = true;
            options.view = view;
            options.depth_only_sources = pass.depth_only_nodes;
            if (!renderer.render(
                    commands, layer, slice, options, active_media_cache)) {
                return CompositorSceneResult::failure(
                    RenderError::BackendFailure);
            }
            std::vector<scene::SpatialEffectOperation> operations =
                pass.spatial_effects;
            if (operations.empty() && pass.blur_radius_px > 0.0F) {
                operations.push_back(scene::SpatialEffectOperation{
                    scene::EffectKind::Blur, pass.blur_radius_px, {}, {}});
            }
            const rhi::Texture* above = &renderer.target();
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
                const MaskRasterMapping mapping{
                    0.0F, 0.0F, 1.0F, 1.0F,
                    static_cast<float>(plan.adjusted_scene.canvas.width),
                    static_cast<float>(plan.adjusted_scene.canvas.height)};
                auto result = mask_raster_.apply(
                    commands, *above, *tested_depth, program, mapping);
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
                static_cast<float>(static_cast<core::u8>(pass.blend)), {}};
            if (!commands.draw_output_transform(
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
        stats_.keyer_passes = keyer_.stats().recorded_passes;
        stats_.keyer_pixels = keyer_.stats().processed_pixels;
        stats_.mask_passes = mask_raster_.stats().recorded_passes;
        stats_.mask_pixels = mask_raster_.stats().processed_pixels;
        output_is_a_ = accumulator == &composite_a_;
        return CompositorSceneResult::success(&target());
    } catch (const std::bad_alloc&) {
        return CompositorSceneResult::failure(RenderError::AllocationFailed);
    }
}

} // namespace mediaserver::render
