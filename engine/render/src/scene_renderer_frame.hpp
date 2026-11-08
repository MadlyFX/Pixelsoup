#pragma once

// One SceneRenderer::render as an object. render() builds a Frame from its
// arguments and runs the recording's phases in order, stopping at the first
// that refuses; state one phase hands to another lives here, everything else
// stays local to its phase. Internal to the render library.

#include "scene_renderer_internal.hpp"

#include <mediaserver/render/geometry_visibility.hpp>
#include <mediaserver/render/particle_pass.hpp>

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace mediaserver::render {

struct SceneRenderer::Frame final {
    SceneRenderer& renderer;
    rhi::CommandList& commands;
    const scene::Scene& scene;
    scene::CanvasRect slice;
    const SceneRenderOptions& options;
    MediaCache& media_cache;

    // begin_recording(): the model publication epoch this recording reads.
    core::u64 model_publication_fence{};

    // resolve_view(): the view drawn and its shading aid.
    SceneRenderView render_view{};
    ViewShading shading{ViewShading::Lit};
    bool wireframe{};

    // prepare_light_assets(): the IES profiles in the atlas, by slot.
    std::array<std::string_view, max_ies_profiles> ies_sources{};
    std::array<bool, max_ies_profiles> ies_available{};
    core::u32 ies_count{};

    // prepare_baked_lighting(): whether any reflection probe is authored.
    bool reflections_authored{};

    // begin_history(): motion history for this view, and the bounds every
    // directional shadow caster expands.
    scene_detail::DirectionalShadowBounds shadow_bounds{};
    ViewHistoryState* view_history{};
    const std::vector<PreviousTransform>* motion_transforms{};
    scene::Mat4 motion_view_projection{};
    bool advance_history{};
    bool hold_history{};

    // The gather phases: what they have claimed so far.
    core::u32 curve_luts_used{};
    core::u32 next_ordinal{};
    std::array<const rhi::Texture*, 14U> fallback_material_textures{};
    std::optional<GeometryFrusta> frusta;
    // build_opaque_batches(): whether batched geometry is drawn indexed.
    bool indexed{};

    // prepare_view_constants().
    rhi::RasterViewport raster_viewport{};
    scene_detail::ViewConstants view_constants{};

    // fit_shadow_cascades().
    scene::RendererSettings renderer_settings{};
    std::array<scene::Mat4, scene::max_shadow_cascades> cascade_view_projection{};
    std::array<float, scene::max_shadow_cascades> cascade_texel_depth{};
    scene::Mat4 shadow_inverse_view_projection{};
    core::u32 cascade_count{};
    bool directional_shadow_enabled{};
    bool directional_gi_enabled{};

    // prepare_scene_constants().
    scene_detail::SceneConstants scene_constants{};
    ParticleLighting particle_lighting{};

    // prepare_bindings(): the buffers every pass binds.
    std::array<const rhi::StorageBuffer*, 6U> vertex_bindings{};
    // The batched passes read their indices through the input assembler.
    // Single composited draws stay pulled; their batch constants say so.
    const rhi::StorageBuffer* batch_indices{};
    std::array<const rhi::StorageBuffer*, 7U> lit_fragment_buffers{};
    std::span<const rhi::StorageBuffer* const> lit_fragment_bindings;
    std::array<const rhi::StorageBuffer*, 2U> media_fragment_bindings{};
    std::array<float, 4U> coverage_sampling{};

    // record_lit_pass(): the colour every colour pass clears to.
    rhi::Color clear{};

    // A layer's place among its siblings, where it has one.
    struct SiblingOrder final {
        std::optional<doc::NodeId> parent;
        core::i64 order{};
    };

    // The IES atlas slot `light` samples: -1 for none, -2 for one that is
    // unavailable or refused.
    [[nodiscard]] int ies_slot_for(const scene::PointLight& light) const noexcept;
    // Instances, composites and depth-only clips claimed so far, against
    // the renderer's instance limit.
    [[nodiscard]] std::size_t held_instances() const noexcept;
    // `instance` as the GPU reads it, with its previous transform from the
    // motion history and its pose and wind if it deforms.
    [[nodiscard]] scene_detail::GpuInstance instance_record(
        const scene::MeshInstance& instance, core::u32 first_effect,
        core::u32 effect_count, bool depth_only = false,
        DecodedPixelLayout media_layout = DecodedPixelLayout::Rgba8,
        bool has_normal_map = false, bool has_occlusion_map = false,
        bool normal_bc5 = false, bool coat_bc5 = false,
        doc::NodeId motion_source = {}) const;
    void append_material_textures(
        const std::array<const rhi::Texture*, 14U>& textures,
        const std::array<ModelSampler, 14U>& samplers);
    // The inline effect program for `source`, compiled once per recording.
    [[nodiscard]] bool inline_range_for(
        doc::NodeId source, const scene::EffectStack& effects,
        InlineProgramRange& range,
        const std::optional<scene_detail::SurfaceFillProgram>& fill = std::nullopt);
    // Queues a destination-reading layer for the sorted composite passes;
    // false when the instance limit drops it.
    bool queue_composite(
        const MeshRange& range, const scene::MeshInstance& instance,
        ResolvedMediaFrame media,
        const std::array<const rhi::Texture*, 14U>& material_textures,
        float depth, doc::NodeId source,
        std::optional<core::u64> timeline_order = std::nullopt,
        std::optional<SiblingOrder> sibling_order = std::nullopt,
        std::array<ModelSampler, 14U> samplers = {},
        ModelBounds bounds = scene_detail::unit_bounds);
    [[nodiscard]] bool camera_visible(const scene::Mat4& world,
                                      const ModelBounds& bounds) const;
    // Records one geometry selection for the residency statistics.
    void selected(doc::NodeId source, bool model, bool visible, core::u32 lod);
};

} // namespace mediaserver::render
