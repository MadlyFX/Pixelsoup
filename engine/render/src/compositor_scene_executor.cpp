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

[[nodiscard]] bool inspect_style(const scene::LayerStyle& style,
                                 std::optional<scene::BlendMode>& blend,
                                 std::optional<float>& radius) noexcept {
    const scene::BlendMode current_blend = valid_blend(style.blend);
    const float current_radius = blur_radius(style);
    if ((blend.has_value() && *blend != current_blend) ||
        (radius.has_value() && *radius != current_radius)) {
        return false;
    }
    blend = current_blend;
    radius = current_radius;
    return true;
}

struct SourceStyle final {
    scene::BlendMode blend{scene::BlendMode::Normal};
    float radius{};
    bool effect_isolation{};
    // The least opacity any drawable of this source carries. A source is a
    // candidate occluder only at full opacity, because anything you can see
    // through must not remove what is behind it from another layer.
    float opacity{1.0F};
    // Camera-space distance, accumulated across every drawable from this
    // source. Isolated passes have to be composited back-to-front; document
    // identity is only a deterministic tie breaker for equal depths.
    double depth_sum{};
    core::u32 drawables{};
};

using SourceStyles = std::map<doc::NodeId, SourceStyle>;

[[nodiscard]] float layer_opacity(const scene::LayerStyle& style) noexcept {
    return std::isfinite(style.opacity)
        ? std::clamp(style.opacity, 0.0F, 1.0F)
        : 1.0F;
}

[[nodiscard]] bool collect_source_styles(const scene::Scene& source,
                                         SourceStyles& styles) {
    const scene::Vec3 forward = scene::normalize(
        source.camera.target - source.camera.eye,
        scene::Vec3{0.0F, 0.0F, -1.0F});
    const auto add = [&styles](doc::NodeId source_node,
                               const scene::LayerStyle& style,
                               const scene::Mat4& world,
                               scene::Vec3 eye,
                               scene::Vec3 view_forward) {
        if (!effect_stack_is_executable(style.effects)) {
            return false;
        }
        const float depth = scene::dot(
            scene::transform_point(world, scene::Vec3{}) - eye,
            view_forward);
        const SourceStyle summary{valid_blend(style.blend), blur_radius(style),
                                  effect_stack_requires_isolation(style.effects),
                                  layer_opacity(style),
                                  std::isfinite(depth)
                                      ? static_cast<double>(depth)
                                      : 0.0,
                                  1U};
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
             found->second.effect_isolation == summary.effect_isolation);
    };
    for (const scene::MeshInstance& instance : source.instances) {
        if (!add(instance.source, instance.style, instance.world,
                 source.camera.eye, forward)) {
            return false;
        }
    }
    for (const scene::MediaInstance& instance : source.media) {
        if (!add(instance.source_node, instance.style, instance.world,
                 source.camera.eye, forward)) {
            return false;
        }
    }
    for (const scene::ModelInstance& instance : source.models) {
        if (!add(instance.source_node, instance.style, instance.world,
                 source.camera.eye, forward)) {
            return false;
        }
    }
    return true;
}

void append_source_passes(const SourceStyles& sources,
                          std::vector<CompositorLayerPass>& passes) {
    std::vector<doc::NodeId> ordinary;
    ordinary.reserve(sources.size());
    std::vector<std::pair<doc::NodeId, const SourceStyle*>> isolated;
    isolated.reserve(sources.size());
    for (const auto& [source_node, style] : sources) {
        const bool needs_isolation = style.effect_isolation ||
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
            return left_depth != right_depth ? left_depth > right_depth
                                             : left.first < right.first;
        });
    for (const auto& [source_node, style_pointer] : isolated) {
        const SourceStyle& style = *style_pointer;
        passes.push_back(CompositorLayerPass{
            {}, source_node, {}, style.blend, style.radius, {}});
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

} // namespace

PlanResult plan_compositor_scene(const compositor::CompositorGraph& graph,
                                 const scene::Scene& source) {
    if (!graph.valid() || graph.plan.steps.empty()) {
        return PlanResult::failure(CompositorScenePlanError::InvalidGraph);
    }

    try {
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

        std::vector<const compositor::GraphNode*> layer_nodes;
        const compositor::GraphNode* const image_source =
            graph.node(output_step->inputs.front().source);
        if (image_source == nullptr) {
            return PlanResult::failure(CompositorScenePlanError::InvalidGraph);
        }
        if (image_source->kind == compositor::NodeKind::SceneLayer) {
            layer_nodes.push_back(image_source);
        } else if (image_source->kind == compositor::NodeKind::Composite) {
            const compositor::PlanStep* const composite_step =
                graph.step(image_source->node);
            if (composite_step == nullptr || composite_step->inputs.empty()) {
                return PlanResult::failure(
                    CompositorScenePlanError::UnsupportedTopology);
            }
            layer_nodes.reserve(composite_step->inputs.size());
            for (const compositor::PlanConnection& input :
                 composite_step->inputs) {
                const compositor::GraphNode* const layer =
                    graph.node(input.source);
                if (layer == nullptr ||
                    layer->kind != compositor::NodeKind::SceneLayer) {
                    return PlanResult::failure(
                        CompositorScenePlanError::UnsupportedTopology);
                }
                layer_nodes.push_back(layer);
            }
        } else {
            return PlanResult::failure(
                CompositorScenePlanError::UnsupportedTopology);
        }
        if (layer_nodes.size() > max_compositor_scene_layers) {
            return PlanResult::failure(
                CompositorScenePlanError::LayersOverCapacity);
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
            if (!collect_source_styles(plan.adjusted_scene, pass_through)) {
                return PlanResult::failure(
                    CompositorScenePlanError::InconsistentLayerStyle);
            }
            for (const compositor::GraphNode* const layer : layer_nodes) {
                const auto& parameters =
                    std::get<compositor::SceneLayerParameters>(
                        layer->parameters);
                pass_through.erase(parameters.scene_node);
            }
            append_source_passes(pass_through, plan.layers);
        }
        plan.layers.reserve(plan.layers.size() + layer_nodes.size());
        for (const compositor::GraphNode* const layer : layer_nodes) {
            const auto& parameters =
                std::get<compositor::SceneLayerParameters>(layer->parameters);
            std::optional<scene::BlendMode> blend;
            std::optional<float> radius;
            bool found{};
            for (const scene::MeshInstance& instance :
                 plan.adjusted_scene.instances) {
                if (instance.source == parameters.scene_node) {
                    found = true;
                    if (!inspect_style(instance.style, blend, radius)) {
                        return PlanResult::failure(
                            CompositorScenePlanError::InconsistentLayerStyle);
                    }
                }
            }
            for (const scene::MediaInstance& instance :
                 plan.adjusted_scene.media) {
                if (instance.source_node == parameters.scene_node) {
                    found = true;
                    if (!inspect_style(instance.style, blend, radius)) {
                        return PlanResult::failure(
                            CompositorScenePlanError::InconsistentLayerStyle);
                    }
                }
            }
            for (const scene::ModelInstance& instance :
                 plan.adjusted_scene.models) {
                if (instance.source_node == parameters.scene_node) {
                    found = true;
                    if (!inspect_style(instance.style, blend, radius)) {
                        return PlanResult::failure(
                            CompositorScenePlanError::InconsistentLayerStyle);
                    }
                }
            }
            if (!found || !blend.has_value() || !radius.has_value()) {
                return PlanResult::failure(
                    CompositorScenePlanError::MissingSceneLayer);
            }
            plan.layers.push_back(CompositorLayerPass{
                layer->node, parameters.scene_node, {}, *blend, *radius, {}});
        }
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

PlanResult plan_scene_effects(const scene::Scene& source) {
    try {
        SourceStyles sources;
        if (!collect_source_styles(source, sources)) {
            return PlanResult::failure(
                CompositorScenePlanError::InconsistentLayerStyle);
        }

        CompositorScenePlan plan{};
        plan.adjusted_scene = source;
        append_source_passes(sources, plan.layers);
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
           GaussianBlur::shaders_available();
}

CompositorSceneExecutor::CompositorSceneExecutor(
    rhi::Device& device, RendererLimits limits,
    std::vector<SceneRenderer> layer_renderers, GaussianBlur blur,
    rhi::Texture composite_a, rhi::Texture composite_b,
    rhi::GraphicsPipeline composite_pipeline, rhi::Sampler sampler) noexcept
    : device_(&device), limits_(limits),
      layer_renderers_(std::move(layer_renderers)), blur_(std::move(blur)),
      composite_a_(std::move(composite_a)),
      composite_b_(std::move(composite_b)),
      composite_pipeline_(std::move(composite_pipeline)),
      sampler_(std::move(sampler)) {}

CreateResult CompositorSceneExecutor::create(rhi::Device& device,
                                             core::u32 width, core::u32 height,
                                             const RendererLimits& limits) {
    const ShaderBlobs blobs = shader_blobs();
    if (!blobs.present || !device.supports(blobs.format) ||
        !SceneRenderer::shaders_available() ||
        !GaussianBlur::shaders_available()) {
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
        device, limits, std::move(layer_renderers), std::move(blur).value(),
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
        auto composite_a = device.create_texture(color_desc(width, height));
        auto composite_b = device.create_texture(color_desc(width, height));
        if (!blur || !composite_a || !composite_b) {
            return false;
        }
        layer_renderers_ = std::move(renderers);
        blur_ = std::move(blur).value();
        composite_a_ = std::move(composite_a).value();
        composite_b_ = std::move(composite_b).value();
        output_is_a_ = true;
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

MediaCacheStats CompositorSceneExecutor::media_cache_stats() const noexcept {
    MediaCacheStats result{};
    for (const auto& renderer : layer_renderers_) {
        const MediaCacheStats& layer = renderer.media_cache_stats();
        result.resident_frames += layer.resident_frames;
        result.resident_bytes += layer.resident_bytes;
        result.byte_budget += layer.byte_budget;
        result.cache_hits += layer.cache_hits;
        result.uploads += layer.uploads;
        result.pending_requests += layer.pending_requests;
        result.held_frames += layer.held_frames;
    }
    return result;
}

CompositorSceneResult CompositorSceneExecutor::execute(
    rhi::CommandList& commands, const compositor::CompositorGraph& graph,
    const scene::Scene& source, scene::CanvasRect slice) noexcept {
    stats_ = CompositorSceneStats{};
    const auto planned = plan_compositor_scene(graph, source);
    if (!planned) {
        return CompositorSceneResult::failure(
            planned.error() == CompositorScenePlanError::AllocationFailed
                ? RenderError::AllocationFailed
                : RenderError::InvalidConfiguration);
    }

    return execute_plan(commands, planned.value(), slice,
                        planned.value().replaces_scene
                            ? rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}
                            : rhi::Color{source.environment.sky_color.x,
                                         source.environment.sky_color.y,
                                         source.environment.sky_color.z, 1.0F});
}

CompositorSceneResult CompositorSceneExecutor::execute(
    rhi::CommandList& commands, const compositor::CompositorGraph& graph,
    const scene::Scene& source, const SceneRenderView& view) noexcept {
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
        view);
}

CompositorSceneResult
CompositorSceneExecutor::execute(rhi::CommandList& commands,
                                 const scene::Scene& source,
                                 scene::CanvasRect slice) noexcept {
    stats_ = CompositorSceneStats{};
    const auto planned = plan_scene_effects(source);
    if (!planned) {
        return CompositorSceneResult::failure(
            planned.error() == CompositorScenePlanError::AllocationFailed
                ? RenderError::AllocationFailed
                : RenderError::InvalidConfiguration);
    }
    return execute_plan(commands, planned.value(), slice,
                        rhi::Color{source.environment.sky_color.x,
                                   source.environment.sky_color.y,
                                   source.environment.sky_color.z, 1.0F});
}

CompositorSceneResult
CompositorSceneExecutor::execute(rhi::CommandList& commands,
                                 const scene::Scene& source,
                                 const SceneRenderView& view) noexcept {
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
        view);
}

CompositorSceneResult CompositorSceneExecutor::execute_plan(
    rhi::CommandList& commands, const CompositorScenePlan& plan,
    scene::CanvasRect slice, rhi::Color initial_clear,
    const std::optional<SceneRenderView>& view) noexcept {
    stats_ = CompositorSceneStats{};
    if ((!view.has_value() &&
         !scene::contains(plan.adjusted_scene.canvas, slice)) ||
        !scene::valid(plan.adjusted_scene.camera)) {
        return CompositorSceneResult::failure(
            RenderError::InvalidConfiguration);
    }

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
            if (!renderer.render(commands, layer, slice, options)) {
                return CompositorSceneResult::failure(
                    RenderError::BackendFailure);
            }
            // The project's kernel, not a per-effect one: how a radius is
            // sampled is a show-wide decision, and every node must sample it
            // the same way or the two halves of a wall would differ.
            auto blurred = blur_.apply(
                commands, renderer.target(), pass.blur_radius_px,
                kernel_of(plan.adjusted_scene.effect_settings));
            if (!blurred) {
                return CompositorSceneResult::failure(blurred.error());
            }
            const rhi::Texture* const above = blurred.value();
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
            if (pass.blur_radius_px > 0.0F) {
                ++stats_.blurred_layers;
            }
        }
        output_is_a_ = accumulator == &composite_a_;
        return CompositorSceneResult::success(&target());
    } catch (const std::bad_alloc&) {
        return CompositorSceneResult::failure(RenderError::AllocationFailed);
    }
}

} // namespace mediaserver::render
