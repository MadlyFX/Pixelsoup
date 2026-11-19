#include <mediaserver/render/bloom.hpp>
#include <mediaserver/render/volumetric_fog.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>
#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/output_transform.hpp>
#include <mediaserver/render/projection_mesh_warp.hpp>
#include <mediaserver/render/graph_transient_pool.hpp>
#include <mediaserver/render/render_graph.hpp>
#include <mediaserver/render/render_graph_executor.hpp>
#include <mediaserver/render/scene_mixer.hpp>
#include <mediaserver/render/scene_bridge.hpp>
#include <mediaserver/render/slice_apron.hpp>
#include <mediaserver/render/ambient_occlusion.hpp>
#include <mediaserver/render/temporal_history.hpp>
#include <mediaserver/render/temporal_resolve.hpp>
#include <mediaserver/scene/math.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/solid_frag_dxil.h>
#include <mediaserver/shaders/solid_vert_dxil.h>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace doc = mediaserver::doc;
namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} +
                                 std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define RENDER_CHECK(...)                                                      \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

void temporal_history_holds_repeats_and_resets_discontinuities() {
    render::TemporalHistory history;
    render::HistoryKey key{};
    key.scene = scene::SceneId{4U};
    key.document_version = doc::Version{3U};
    key.surface = render::HistorySurface::Program;
    key.surface_id = 7U;
    key.canvas = scene::Canvas{7'680U, 2'160U};
    key.slice = scene::CanvasRect{0U, 0U, 3'840U, 2'160U};
    key.width = 3'840U;
    key.height = 2'160U;
    key.camera_source = doc::NodeId{8U};
    key.camera_digest = 11U;
    key.profile_digest = 12U;

    RENDER_CHECK(history.observe(key, 100U, 22U).disposition ==
                 render::HistoryDisposition::Initialize);
    RENDER_CHECK(history.observe(key, 100U, 22U).disposition ==
                 render::HistoryDisposition::Hold);
    RENDER_CHECK(history.observe(key, 101U, 22U).disposition ==
                 render::HistoryDisposition::Advance);
    // An operator commit at a held frame is a discontinuity even when record
    // topology and the show-frame ordinal did not move.
    RENDER_CHECK(history.observe(key, 101U, 22U, 99U).disposition ==
                 render::HistoryDisposition::Reset);
    key.document_version = doc::Version{4U};
    RENDER_CHECK(history.observe(key, 102U, 22U, 99U).disposition ==
                 render::HistoryDisposition::Reset);
    RENDER_CHECK(history.observe(key, 95U, 22U, 99U).disposition ==
                 render::HistoryDisposition::Reset);
    RENDER_CHECK(history.reset_count() == 3U);
    RENDER_CHECK(history.observe(key, 96U, 23U, 99U).disposition ==
                 render::HistoryDisposition::Reset);

    render::HistoryKey stage = key;
    stage.surface = render::HistorySurface::Stage;
    RENDER_CHECK(history.observe(stage, 97U, 23U, 99U).disposition ==
                 render::HistoryDisposition::Reset);

    render::HistoryKey remapped = stage;
    remapped.slice.x = 3'840U;
    RENDER_CHECK(history.observe(remapped, 98U, 23U, 99U).disposition ==
                 render::HistoryDisposition::Reset);

    scene::Camera camera{};
    scene::Camera moved = camera;
    moved.eye.x += 1.0F;
    moved.target.x += 1.0F;
    RENDER_CHECK(render::camera_history_digest(camera) ==
                 render::camera_history_digest(moved));
    moved.field_of_view += 0.01F;
    RENDER_CHECK(render::camera_history_digest(camera) !=
                 render::camera_history_digest(moved));

    const scene::ViewMatrices ordinary = scene::view_for_slice(
        camera, scene::Canvas{1'920U, 1'080U},
        scene::CanvasRect{0U, 0U, 1'920U, 1'080U});
    scene::ViewMatrices shifted = ordinary;
    shifted.view_projection.m[12U] += 0.25F;
    RENDER_CHECK(render::view_history_digest(
                     ordinary, camera.eye,
                     scene::normalize(camera.target - camera.eye)) !=
                 render::view_history_digest(
                     shifted, camera.eye,
                     scene::normalize(camera.target - camera.eye)));

    const core::u64 resets_before_manual_reset = history.reset_count();
    history.reset();
    const auto reinitialized = history.observe(remapped, 98U, 23U, 99U);
    RENDER_CHECK(reinitialized.disposition ==
                 render::HistoryDisposition::Initialize);
    RENDER_CHECK(reinitialized.reset_count == resets_before_manual_reset);
}

void deterministic_samples_use_absolute_canvas_identity() {
    const auto first =
        render::deterministic_sample_bits(42U, 7U, 1'920U, 400U, 0U);
    RENDER_CHECK(first ==
                 render::deterministic_sample_bits(
                     42U, 7U, 1'920U, 400U, 0U));
    RENDER_CHECK(first !=
                 render::deterministic_sample_bits(
                     42U, 7U, 0U, 400U, 0U));
    const float unit =
        render::deterministic_sample_unit(42U, 7U, 1'920U, 400U, 0U);
    RENDER_CHECK(unit >= 0.0F);
    RENDER_CHECK(unit < 1.0F);
}

void slice_apron_expands_clamps_and_preserves_crop() {
    scene::Scene value{};
    value.canvas = scene::Canvas{1'000U, 500U};
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.denoising = true;
    value.renderer_settings.ray_tracing = true;
    value.renderer_settings.taau = true;
    scene::EffectInstance blur{};
    blur.enabled = true;
    blur.kind = scene::EffectKind::Blur;
    blur.value = 20.0F;
    value.effects.push_back(blur);

    const auto requirements = render::apron_requirements(value);
    RENDER_CHECK(requirements.maximum() == 54U);
    auto chain = requirements;
    chain.bloom = 32U; chain.transmission = 8U;
    RENDER_CHECK(chain.maximum() == 94U);
    RENDER_CHECK(!render::padded_slice(value.canvas, {400U,0U,200U,500U}, chain, 64U));

    const auto padded = render::padded_slice(
        value.canvas, scene::CanvasRect{400U, 0U, 200U, 500U}, requirements,
        64U);
    RENDER_CHECK(padded);
    RENDER_CHECK(padded.value().render ==
                 scene::CanvasRect{346U, 0U, 308U, 500U});
    RENDER_CHECK(padded.value().crop ==
                 scene::CanvasRect{54U, 0U, 200U, 500U});

    const auto refused = render::padded_slice(
        value.canvas, scene::CanvasRect{400U, 0U, 200U, 500U}, requirements,
        16U);
    RENDER_CHECK(!refused);
    RENDER_CHECK(refused.error() == render::SliceApronError::LimitExceeded);
}

void render_graph_compiles_hazards_and_queue_handoffs() {
    render::RenderGraph graph;
    const auto media = graph.add_resource(
        render::GraphResourceDesc{"media", render::GraphResourceKind::Texture,
                                  true, false});
    const auto gbuffer = graph.add_resource(
        render::GraphResourceDesc{"gbuffer", render::GraphResourceKind::Texture,
                                  false, false});
    const auto output = graph.add_resource(
        render::GraphResourceDesc{"output", render::GraphResourceKind::Texture,
                                  false, true});
    RENDER_CHECK(media && gbuffer && output);

    const std::array raster_access{
        render::GraphAccess{media.value(), render::GraphState::ShaderRead,
                            render::GraphAccessMode::Read},
        render::GraphAccess{gbuffer.value(), render::GraphState::ColorTarget,
                            render::GraphAccessMode::Write}};
    RENDER_CHECK(graph.add_pass(render::GraphPassDesc{
        "gbuffer", render::GraphPassKind::Raster,
        render::GraphQueue::Graphics, raster_access}));

    const std::array denoise_access{
        render::GraphAccess{gbuffer.value(), render::GraphState::StorageRead,
                            render::GraphAccessMode::Read},
        render::GraphAccess{output.value(), render::GraphState::StorageWrite,
                            render::GraphAccessMode::Write}};
    RENDER_CHECK(graph.add_pass(render::GraphPassDesc{
        "denoise", render::GraphPassKind::Compute,
        render::GraphQueue::Compute, denoise_access}));

    const auto plan = graph.compile();
    RENDER_CHECK(plan);
    RENDER_CHECK(plan.value().passes().size() == 2U);
    bool saw_queue_sync{};
    for (const render::GraphTransition& transition :
         plan.value().transitions()) {
        saw_queue_sync = saw_queue_sync || transition.queue_sync;
    }
    RENDER_CHECK(saw_queue_sync);

    render::RenderGraph invalid;
    const auto transient = invalid.add_resource(render::GraphResourceDesc{
        "transient", render::GraphResourceKind::Texture, false, false});
    RENDER_CHECK(transient);
    const std::array invalid_access{
        render::GraphAccess{transient.value(), render::GraphState::ShaderRead,
                            render::GraphAccessMode::Read}};
    RENDER_CHECK(invalid.add_pass(render::GraphPassDesc{
        "read first", render::GraphPassKind::Compute,
        render::GraphQueue::Compute, invalid_access}));
    const auto invalid_plan = invalid.compile();
    RENDER_CHECK(!invalid_plan);
    RENDER_CHECK(invalid_plan.error() ==
                 render::RenderGraphError::ReadBeforeWrite);
}

// Every graph state that names a real resource maps to exactly one RHI state.
//
// A switch rather than a cast, and asserted rather than assumed: the two
// enumerations are allowed to diverge, and a silent numeric correspondence
// between them survives right up until someone inserts a value in the middle of
// one of them.
void graph_states_map_to_resource_states() {
    RENDER_CHECK(render::resource_state_for(render::GraphState::ColorTarget)
                 == rhi::ResourceState::ColorTarget);
    RENDER_CHECK(render::resource_state_for(render::GraphState::DepthTarget)
                 == rhi::ResourceState::DepthTarget);
    RENDER_CHECK(render::resource_state_for(render::GraphState::ShaderRead)
                 == rhi::ResourceState::ShaderRead);
    RENDER_CHECK(render::resource_state_for(render::GraphState::StorageRead)
                 == rhi::ResourceState::StorageRead);
    RENDER_CHECK(render::resource_state_for(render::GraphState::StorageWrite)
                 == rhi::ResourceState::StorageWrite);
    RENDER_CHECK(render::resource_state_for(render::GraphState::CopySource)
                 == rhi::ResourceState::CopySource);
    RENDER_CHECK(
        render::resource_state_for(render::GraphState::CopyDestination)
        == rhi::ResourceState::CopyDestination);
    RENDER_CHECK(render::resource_state_for(render::GraphState::Present)
                 == rhi::ResourceState::Present);
}

// Everything the executor refuses, and all of it before a barrier is recorded.
//
// A plan that is going to be rejected must not leave half its transitions on
// the list: the caller would be holding a recording whose resource states came
// from a frame that never happened, and the next pass to touch one of those
// textures would barrier from a state it is not in.
void the_graph_executor_refuses_before_it_records() {
    render::RenderGraph graph;
    const auto colour = graph.add_resource(render::GraphResourceDesc{
        "colour", render::GraphResourceKind::Texture, false, true});
    RENDER_CHECK(colour);
    const std::array raster_access{render::GraphAccess{
        colour.value(), render::GraphState::ColorTarget,
        render::GraphAccessMode::Write}};
    RENDER_CHECK(graph.add_pass(render::GraphPassDesc{
        "raster", render::GraphPassKind::Raster,
        render::GraphQueue::Graphics, raster_access}));
    const auto plan = graph.compile();
    RENDER_CHECK(plan);

    render::RenderGraphExecutor executor;
    const auto body = [](rhi::CommandList&, void*) noexcept { return true; };
    const std::array<render::GraphPassBody, 1U> bodies{
        render::GraphPassBody{body, nullptr}};

    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    auto acquired = device.acquire_commands();
    RENDER_CHECK(acquired);
    auto commands = std::move(acquired).value();

    // One body too few, and one too many.
    RENDER_CHECK(!executor.execute(device, commands, plan.value(), {}));
    RENDER_CHECK(executor.execute(device, commands, plan.value(), {}).error()
                 == render::RenderGraphExecutionError::BodyCountMismatch);
    const std::array<render::GraphPassBody, 2U> too_many{
        render::GraphPassBody{body, nullptr},
        render::GraphPassBody{body, nullptr}};
    RENDER_CHECK(
        executor.execute(device, commands, plan.value(), too_many).error()
        == render::RenderGraphExecutionError::BodyCountMismatch);

    // A resource the plan transitions but nothing bound.
    RENDER_CHECK(
        executor.execute(device, commands, plan.value(), bodies).error()
        == render::RenderGraphExecutionError::UnboundResource);
    RENDER_CHECK(executor.recorded_transitions() == 0U);
    RENDER_CHECK(executor.recorded_passes() == 0U);

    // A plan naming the compute queue, while async compute is honestly false.
    // Running its passes on graphics instead would be invisible until someone
    // read a profile.
    RENDER_CHECK(!device.capabilities().async_compute);
    render::RenderGraph async;
    const auto storage = async.add_resource(render::GraphResourceDesc{
        "storage", render::GraphResourceKind::Texture, false, true});
    RENDER_CHECK(storage);
    const std::array compute_access{render::GraphAccess{
        storage.value(), render::GraphState::StorageWrite,
        render::GraphAccessMode::Write}};
    RENDER_CHECK(async.add_pass(render::GraphPassDesc{
        "denoise", render::GraphPassKind::Compute,
        render::GraphQueue::Compute, compute_access}));
    const auto async_plan = async.compile();
    RENDER_CHECK(async_plan);
    RENDER_CHECK(
        executor.execute(device, commands, async_plan.value(), bodies).error()
        == render::RenderGraphExecutionError::QueueUnsupported);

    // A pass body that fails stops execution at that pass rather than
    // continuing into a frame whose earlier half never happened.
    constexpr core::u32 extent = 8U;
    rhi::TextureDesc desc{};
    desc.width = extent;
    desc.height = extent;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.color_target = true;
    desc.sampled = true;
    auto texture = device.create_texture(desc);
    RENDER_CHECK(texture);
    RENDER_CHECK(executor.bind(colour.value(), texture.value()));
    const auto refuse = [](rhi::CommandList&, void*) noexcept { return false; };
    const std::array<render::GraphPassBody, 1U> failing{
        render::GraphPassBody{refuse, nullptr}};
    RENDER_CHECK(
        executor.execute(device, commands, plan.value(), failing).error()
        == render::RenderGraphExecutionError::PassFailed);
    RENDER_CHECK(executor.recorded_passes() == 0U);
    // The transition for that pass was recorded before its body ran, which is
    // the order a barrier has to happen in.
    RENDER_CHECK(executor.recorded_transitions() == 1U);
}

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)

struct SolidPassContext final {
    rhi::Texture* color{};
    rhi::Texture* depth{};
    const rhi::GraphicsPipeline* pipeline{};
    const rhi::DrawBatch* batches{};
    rhi::ViewportRect viewport{};
};

struct CopyPassContext final {
    rhi::Texture* source{};
    rhi::Texture* destination{};
    rhi::ViewportRect region{};
};

[[nodiscard]] bool record_solid_pass(
    rhi::CommandList& commands, void* context) noexcept {
    auto* pass = static_cast<SolidPassContext*>(context);
    rhi::PulledDraw draw{};
    draw.pipeline = pass->pipeline;
    draw.batches = std::span<const rhi::DrawBatch>{pass->batches, 1U};
    draw.viewport = pass->viewport;
    return commands.draw_pulled(
        *pass->color, rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, *pass->depth, 0.0F,
        draw);
}

[[nodiscard]] bool record_copy_pass(
    rhi::CommandList& commands, void* context) noexcept {
    auto* pass = static_cast<CopyPassContext*>(context);
    return commands.copy_region(
        *pass->source, pass->region, *pass->destination);
}

// The migration's proof: the same two passes recorded by hand and through the
// graph produce byte-identical pixels.
//
// This is the whole safety net for turning the Advanced frame into a graph. The
// executor inserts its own barriers from the compiled plan, and each pass body
// then transitions again on its own account; if that pair produced a different
// image than the bodies alone, no plan could be executed over the existing draw
// entry points and the migration would be a rewrite instead of a rewiring.
void a_graph_executed_frame_matches_the_hand_recorded_one() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
#if !defined(NDEBUG)
    config.debug = true;
#endif
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();

    constexpr core::u32 extent = 16U;
    constexpr core::u32 crop = 8U;
    constexpr core::u32 bytes = crop * crop * 4U;
    const rhi::ViewportRect viewport{4U, 2U, 8U, 6U};
    const rhi::ViewportRect region{2U, 1U, crop, crop};

    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = rhi::ShaderFormat::Dxil;
    vertex.bytecode = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_solid_vert_dxil),
        sizeof(mediaserver_solid_vert_dxil)};
    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = rhi::ShaderFormat::Dxil;
    fragment.bytecode = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_solid_frag_dxil),
        sizeof(mediaserver_solid_frag_dxil)};
    auto pipeline = device.create_pipeline(
        vertex, fragment, rhi::TextureFormat::Rgba8Unorm,
        rhi::ColorBlend::Disabled);
    RENDER_CHECK(pipeline);

    rhi::TextureDesc color_desc{};
    color_desc.width = extent;
    color_desc.height = extent;
    color_desc.format = rhi::TextureFormat::Rgba8Unorm;
    color_desc.color_target = true;
    color_desc.sampled = true;
    rhi::TextureDesc depth_desc{};
    depth_desc.width = extent;
    depth_desc.height = extent;
    depth_desc.format = rhi::TextureFormat::Depth32Float;
    depth_desc.depth_target = true;
    rhi::TextureDesc output_desc = color_desc;
    output_desc.width = crop;
    output_desc.height = crop;

    const std::array<rhi::DrawBatch, 1U> batches{rhi::DrawBatch{3U, 1U}};

    // Recorded by hand, exactly as the renderer does today.
    std::vector<core::u8> by_hand(bytes);
    {
        auto color = device.create_texture(color_desc);
        auto depth = device.create_texture(depth_desc);
        auto output = device.create_texture(output_desc);
        auto readback = device.create_readback(bytes);
        RENDER_CHECK(color && depth && output && readback);
        auto acquired = device.acquire_commands();
        RENDER_CHECK(acquired);
        auto commands = std::move(acquired).value();
        SolidPassContext solid{&color.value(), &depth.value(),
                               &pipeline.value(), batches.data(), viewport};
        CopyPassContext copy{&color.value(), &output.value(), region};
        RENDER_CHECK(record_solid_pass(commands, &solid));
        RENDER_CHECK(record_copy_pass(commands, &copy));
        RENDER_CHECK(commands.download(output.value(), readback.value()));
        auto submitted = device.submit(std::move(commands));
        RENDER_CHECK(submitted);
        RENDER_CHECK(std::move(submitted).value().wait());
        const auto pixels = readback.value().map();
        RENDER_CHECK(pixels.size() >= bytes);
        std::memcpy(by_hand.data(), pixels.data(), bytes);
        readback.value().unmap();
    }

    // The same two passes declared as a graph and executed from the plan.
    std::vector<core::u8> by_graph(bytes);
    {
        render::RenderGraph graph;
        const auto color_resource = graph.add_resource(
            render::GraphResourceDesc{"color",
                                      render::GraphResourceKind::Texture,
                                      false, false});
        const auto depth_resource = graph.add_resource(
            render::GraphResourceDesc{"depth",
                                      render::GraphResourceKind::Texture,
                                      false, false});
        const auto output_resource = graph.add_resource(
            render::GraphResourceDesc{"output",
                                      render::GraphResourceKind::Texture,
                                      false, true});
        RENDER_CHECK(color_resource && depth_resource && output_resource);

        const std::array raster_access{
            render::GraphAccess{color_resource.value(),
                                render::GraphState::ColorTarget,
                                render::GraphAccessMode::Write},
            render::GraphAccess{depth_resource.value(),
                                render::GraphState::DepthTarget,
                                render::GraphAccessMode::Write}};
        RENDER_CHECK(graph.add_pass(render::GraphPassDesc{
            "solid", render::GraphPassKind::Raster,
            render::GraphQueue::Graphics, raster_access}));

        const std::array copy_access{
            render::GraphAccess{color_resource.value(),
                                render::GraphState::CopySource,
                                render::GraphAccessMode::Read},
            render::GraphAccess{output_resource.value(),
                                render::GraphState::CopyDestination,
                                render::GraphAccessMode::Write}};
        RENDER_CHECK(graph.add_pass(render::GraphPassDesc{
            "crop", render::GraphPassKind::Copy,
            render::GraphQueue::Graphics, copy_access}));

        const auto plan = graph.compile();
        RENDER_CHECK(plan);
        RENDER_CHECK(plan.value().passes().size() == 2U);

        auto color = device.create_texture(color_desc);
        auto depth = device.create_texture(depth_desc);
        auto output = device.create_texture(output_desc);
        auto readback = device.create_readback(bytes);
        RENDER_CHECK(color && depth && output && readback);

        render::RenderGraphExecutor executor;
        RENDER_CHECK(executor.bind(color_resource.value(), color.value()));
        RENDER_CHECK(executor.bind(depth_resource.value(), depth.value()));
        RENDER_CHECK(executor.bind(output_resource.value(), output.value()));

        SolidPassContext solid{&color.value(), &depth.value(),
                               &pipeline.value(), batches.data(), viewport};
        CopyPassContext copy{&color.value(), &output.value(), region};
        const std::array<render::GraphPassBody, 2U> bodies{
            render::GraphPassBody{&record_solid_pass, &solid},
            render::GraphPassBody{&record_copy_pass, &copy}};

        auto acquired = device.acquire_commands();
        RENDER_CHECK(acquired);
        auto commands = std::move(acquired).value();
        RENDER_CHECK(executor.execute(device, commands, plan.value(), bodies));
        RENDER_CHECK(executor.recorded_passes() == 2U);
        // Four accesses, four first-touch or state-changing transitions.
        RENDER_CHECK(executor.recorded_transitions()
                     == plan.value().transitions().size());
        RENDER_CHECK(commands.download(output.value(), readback.value()));
        auto submitted = device.submit(std::move(commands));
        RENDER_CHECK(submitted);
        RENDER_CHECK(std::move(submitted).value().wait());
        const auto pixels = readback.value().map();
        RENDER_CHECK(pixels.size() >= bytes);
        std::memcpy(by_graph.data(), pixels.data(), bytes);
        readback.value().unmap();
    }

    RENDER_CHECK(by_hand == by_graph);
    // And the frame was not uniformly one value, which a byte comparison
    // between two blank images would also satisfy.
    RENDER_CHECK(
        std::adjacent_find(by_hand.begin(), by_hand.end(),
                           [](core::u8 left, core::u8 right) {
                               return left != right;
                           })
        != by_hand.end());
}

#else

void a_graph_executed_frame_matches_the_hand_recorded_one() {}

#endif

// The transient pool hands out whole textures by exact descriptor match,
// reuses them across frames, and refuses rather than allocating past its bound.
//
// Exact match rather than "big enough": handing back a larger texture would
// leave a pass's viewport and its target extent disagreeing, and every sampling
// coordinate downstream is derived from the target extent.
void the_transient_pool_reuses_and_refuses() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();

    render::GraphTransientPoolLimits limits{};
    limits.max_textures = 2U;
    render::GraphTransientPool pool{limits};

    rhi::TextureDesc desc{};
    desc.width = 16U;
    desc.height = 16U;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.color_target = true;
    desc.sampled = true;

    pool.begin_frame();
    auto first = pool.acquire(device, desc);
    RENDER_CHECK(first);
    // Held for this frame, so the second request cannot be the same texture.
    auto second = pool.acquire(device, desc);
    RENDER_CHECK(second);
    RENDER_CHECK(first.value() != second.value());
    // Two is the bound, so the third is refused and counted rather than
    // allocated around. A pool that grew on demand would put an allocation on
    // the frame path every time a show got slightly busier.
    auto third = pool.acquire(device, desc);
    RENDER_CHECK(!third);
    RENDER_CHECK(third.error() == render::RenderError::AllocationFailed);
    RENDER_CHECK(pool.stats().refusals == 1U);
    RENDER_CHECK(pool.stats().acquired == 2U);
    RENDER_CHECK(pool.stats().textures == 2U);

    // A new frame releases both, and neither is created again.
    pool.begin_frame();
    auto again = pool.acquire(device, desc);
    RENDER_CHECK(again);
    RENDER_CHECK(again.value() == first.value());
    RENDER_CHECK(pool.stats().reuses == 1U);
    RENDER_CHECK(pool.stats().textures == 2U);

    // A different descriptor is a different texture, not a reinterpretation of
    // one that happens to be free. Its own pool, because the one above is at
    // its bound on purpose and would refuse this for the wrong reason.
    render::GraphTransientPoolLimits roomy{};
    roomy.max_textures = 4U;
    render::GraphTransientPool mixed{roomy};
    mixed.begin_frame();
    rhi::TextureDesc taller = desc;
    taller.height = 32U;
    auto square = mixed.acquire(device, desc);
    auto other = mixed.acquire(device, taller);
    RENDER_CHECK(square && other);
    RENDER_CHECK(other.value() != square.value());
    // And a free texture of the wrong shape is not reused for it.
    mixed.begin_frame();
    auto again_taller = mixed.acquire(device, taller);
    RENDER_CHECK(again_taller);
    RENDER_CHECK(again_taller.value() == other.value());
    RENDER_CHECK(mixed.stats().textures == 2U);

    // A degenerate descriptor is a caller error, distinct from exhaustion.
    rhi::TextureDesc empty = desc;
    empty.width = 0U;
    auto refused = mixed.acquire(device, empty);
    RENDER_CHECK(!refused);
    RENDER_CHECK(refused.error() == render::RenderError::InvalidConfiguration);
}

// The Advanced frame's own declaration, compiled.
//
// This is the migration's first step and its cheapest insurance. Before any of
// `SceneRenderer::render` is rewritten in terms of the graph, the graph has to
// be able to express what that function does - the cascade loop writing a
// different map each iteration, the reflective-shadow-map attributes sharing
// the shadow depth buffer, the lit pass reading every one of them, the media
// composites loading the same colour and depth again, and the auxiliary
// products drawn against their own depth.
//
// If the model could not express this, the right time to find out is now, not
// halfway through moving a three-thousand-line function.
//
// The hazards asserted below are the ones a hand-written frame gets wrong: a
// shadow map still a colour target when the lit pass samples it, and a
// depth buffer shared by passes that write it in turn.
void the_advanced_frame_declaration_compiles_with_its_hazards() {
    render::RenderGraph graph;
    constexpr core::u32 cascades = 4U;

    std::array<render::GraphResource, cascades> shadow_maps{};
    for (core::u32 index = 0U; index < cascades; ++index) {
        const auto created = graph.add_resource(render::GraphResourceDesc{
            "shadow cascade", render::GraphResourceKind::Texture, false,
            false});
        RENDER_CHECK(created);
        shadow_maps[index] = created.value();
    }
    const auto shadow_depth = graph.add_resource(render::GraphResourceDesc{
        "shadow depth", render::GraphResourceKind::Texture, false, false});
    const auto rsm_normal = graph.add_resource(render::GraphResourceDesc{
        "rsm normal", render::GraphResourceKind::Texture, false, false});
    const auto rsm_flux = graph.add_resource(render::GraphResourceDesc{
        "rsm flux", render::GraphResourceKind::Texture, false, false});
    const auto colour = graph.add_resource(render::GraphResourceDesc{
        "colour", render::GraphResourceKind::Texture, false, true});
    const auto depth = graph.add_resource(render::GraphResourceDesc{
        "depth", render::GraphResourceKind::Texture, false, true});
    const auto auxiliary_depth = graph.add_resource(render::GraphResourceDesc{
        "auxiliary depth", render::GraphResourceKind::Texture, false, true});
    const auto object_id = graph.add_resource(render::GraphResourceDesc{
        "object id", render::GraphResourceKind::Texture, false, true});
    const auto velocity = graph.add_resource(render::GraphResourceDesc{
        "velocity", render::GraphResourceKind::Texture, false, true});
    const auto coverage = graph.add_resource(render::GraphResourceDesc{
        "coverage", render::GraphResourceKind::Texture, false, true});
    RENDER_CHECK(shadow_depth && rsm_normal && rsm_flux && colour && depth);
    RENDER_CHECK(auxiliary_depth && object_id && velocity && coverage);

    // One pass per cascade, each writing its own map and all of them sharing
    // the one shadow depth buffer. The shared depth is what makes these passes
    // ordered rather than independent, and it is exactly the dependency a
    // hand-written frame expresses only by the order the calls happen to be in.
    //
    // **The access mode mirrors the pass's load flag.** A pass that clears its
    // depth is a Write; only a pass that loads it is a ReadWrite. Every cascade
    // clears, so all four are writes. Declaring them ReadWrite instead is a
    // read of a resource nothing has written, and the compiler says so - which
    // is the first thing this declaration got wrong, and the reason to compile
    // it before moving any code.
    std::array<std::array<render::GraphAccess, 2U>, cascades> cascade_access{};
    for (core::u32 index = 0U; index < cascades; ++index) {
        cascade_access[index] = {
            render::GraphAccess{shadow_maps[index],
                                render::GraphState::ColorTarget,
                                render::GraphAccessMode::Write},
            render::GraphAccess{shadow_depth.value(),
                                render::GraphState::DepthTarget,
                                render::GraphAccessMode::Write}};
        RENDER_CHECK(graph.add_pass(render::GraphPassDesc{
            "cascade", render::GraphPassKind::Raster,
            render::GraphQueue::Graphics, cascade_access[index]}));
    }

    const std::array rsm_normal_access{
        render::GraphAccess{rsm_normal.value(),
                            render::GraphState::ColorTarget,
                            render::GraphAccessMode::Write},
        render::GraphAccess{shadow_depth.value(),
                            render::GraphState::DepthTarget,
                            render::GraphAccessMode::Write}};
    RENDER_CHECK(graph.add_pass(render::GraphPassDesc{
        "rsm normal", render::GraphPassKind::Raster,
        render::GraphQueue::Graphics, rsm_normal_access}));

    const std::array rsm_flux_access{
        render::GraphAccess{rsm_flux.value(), render::GraphState::ColorTarget,
                            render::GraphAccessMode::Write},
        render::GraphAccess{shadow_depth.value(),
                            render::GraphState::DepthTarget,
                            render::GraphAccessMode::Write}};
    RENDER_CHECK(graph.add_pass(render::GraphPassDesc{
        "rsm flux", render::GraphPassKind::Raster,
        render::GraphQueue::Graphics, rsm_flux_access}));

    // The lit pass reads every shadow product it just wrote. This is the
    // transition the graph exists to get right.
    std::array<render::GraphAccess, cascades + 4U> lit_access{};
    for (core::u32 index = 0U; index < cascades; ++index) {
        lit_access[index] = render::GraphAccess{
            shadow_maps[index], render::GraphState::ShaderRead,
            render::GraphAccessMode::Read};
    }
    lit_access[cascades] = render::GraphAccess{
        rsm_normal.value(), render::GraphState::ShaderRead,
        render::GraphAccessMode::Read};
    lit_access[cascades + 1U] = render::GraphAccess{
        rsm_flux.value(), render::GraphState::ShaderRead,
        render::GraphAccessMode::Read};
    lit_access[cascades + 2U] = render::GraphAccess{
        colour.value(), render::GraphState::ColorTarget,
        render::GraphAccessMode::Write};
    lit_access[cascades + 3U] = render::GraphAccess{
        depth.value(), render::GraphState::DepthTarget,
        render::GraphAccessMode::Write};
    RENDER_CHECK(graph.add_pass(render::GraphPassDesc{
        "opaque lit", render::GraphPassKind::Raster,
        render::GraphQueue::Graphics, lit_access}));

    // Composited media sets `load`, so unlike every pass above it reads the
    // colour and depth it then writes. This is the one place in the frame where
    // ReadWrite is the honest mode.
    const std::array media_access{
        render::GraphAccess{colour.value(), render::GraphState::ColorTarget,
                            render::GraphAccessMode::ReadWrite},
        render::GraphAccess{depth.value(), render::GraphState::DepthTarget,
                            render::GraphAccessMode::ReadWrite}};
    RENDER_CHECK(graph.add_pass(render::GraphPassDesc{
        "media composite", render::GraphPassKind::Composite,
        render::GraphQueue::Graphics, media_access}));

    // The auxiliary products share their own depth, drawn after the colour
    // pass and kept as the public Advanced depth.
    const std::array<render::GraphResource, 3U> products{
        object_id.value(), velocity.value(), coverage.value()};
    std::array<std::array<render::GraphAccess, 2U>, 3U> product_access{};
    for (std::size_t index = 0U; index < products.size(); ++index) {
        product_access[index] = {
            render::GraphAccess{products[index],
                                render::GraphState::ColorTarget,
                                render::GraphAccessMode::Write},
            render::GraphAccess{auxiliary_depth.value(),
                                render::GraphState::DepthTarget,
                                render::GraphAccessMode::Write}};
        RENDER_CHECK(graph.add_pass(render::GraphPassDesc{
            "auxiliary", render::GraphPassKind::Raster,
            render::GraphQueue::Graphics, product_access[index]}));
    }

    const auto plan = graph.compile();
    if (!plan) {
        std::cerr << "GRAPHDBG error " << static_cast<int>(plan.error())
                  << '\n';
    }
    RENDER_CHECK(plan);
    // Four cascades, two reflective-shadow-map passes, the lit pass, the media
    // composite, and three auxiliary products.
    RENDER_CHECK(plan.value().passes().size() == cascades + 7U);

    // Every shadow map goes colour target then shader read, in that order and
    // once each. A frame that sampled one while it was still a colour target
    // reads undefined data on some drivers and the right answer on others,
    // which is the failure this whole layer exists to make impossible.
    for (core::u32 index = 0U; index < cascades; ++index) {
        bool became_target{};
        bool became_readable{};
        for (const render::GraphTransition& transition :
             plan.value().transitions()) {
            if (transition.resource != shadow_maps[index]) {
                continue;
            }
            if (transition.after == render::GraphState::ColorTarget) {
                RENDER_CHECK(!became_readable);
                became_target = true;
            }
            if (transition.after == render::GraphState::ShaderRead) {
                RENDER_CHECK(became_target);
                RENDER_CHECK(transition.before
                             == render::GraphState::ColorTarget);
                became_readable = true;
            }
        }
        RENDER_CHECK(became_target && became_readable);
    }

    // The shared shadow depth is transitioned once, by the first cascade, and
    // not again by the five passes that go on using it in the same state. A
    // barrier per pass would be correct and wasteful; none at all would be
    // wrong. The compiler emits exactly one.
    core::u32 shadow_depth_transitions{};
    for (const render::GraphTransition& transition :
         plan.value().transitions()) {
        if (transition.resource == shadow_depth.value()) {
            ++shadow_depth_transitions;
        }
    }
    RENDER_CHECK(shadow_depth_transitions == 1U);

    // Nothing names a queue this build cannot run, so the whole declaration is
    // executable today rather than only after async compute lands.
    for (const render::GraphTransition& transition :
         plan.value().transitions()) {
        RENDER_CHECK(!transition.queue_sync);
    }
    for (const render::CompiledPass& pass : plan.value().passes()) {
        RENDER_CHECK(pass.queue == render::GraphQueue::Graphics);
    }

    // And it fits, with room. The fixed capacities are a bound on a frame, not
    // a bound this frame is anywhere near.
    RENDER_CHECK(graph.pass_count() < render::max_render_graph_passes);
    RENDER_CHECK(graph.resource_count() < render::max_render_graph_resources);
}

// A graph past its fixed capacity reports it rather than truncating. A
// truncated plan is a frame missing passes, which draws something plausible.
void a_graph_past_capacity_is_refused() {
    render::RenderGraph graph;
    const auto resource = graph.add_resource(render::GraphResourceDesc{
        "target", render::GraphResourceKind::Texture, true, false});
    RENDER_CHECK(resource);
    const std::array access{render::GraphAccess{
        resource.value(), render::GraphState::ColorTarget,
        render::GraphAccessMode::Write}};
    for (std::size_t index = 0U; index < render::max_render_graph_passes;
         ++index) {
        RENDER_CHECK(graph.add_pass(render::GraphPassDesc{
            "pass", render::GraphPassKind::Raster,
            render::GraphQueue::Graphics, access}));
    }
    const auto refused = graph.add_pass(render::GraphPassDesc{
        "one too many", render::GraphPassKind::Raster,
        render::GraphQueue::Graphics, access});
    RENDER_CHECK(!refused);
    RENDER_CHECK(refused.error() == render::RenderGraphError::CapacityExceeded);

    render::RenderGraph resources;
    for (std::size_t index = 0U; index < render::max_render_graph_resources;
         ++index) {
        RENDER_CHECK(resources.add_resource(render::GraphResourceDesc{
            "resource", render::GraphResourceKind::Texture, true, false}));
    }
    const auto over = resources.add_resource(render::GraphResourceDesc{
        "one too many", render::GraphResourceKind::Texture, true, false});
    RENDER_CHECK(!over);
    RENDER_CHECK(over.error() == render::RenderGraphError::CapacityExceeded);
}

void scene_bridge_is_stable_bounded_and_keeps_previous_transform() {
    auto created = render::SceneBridge::create(4U);
    RENDER_CHECK(created);
    render::SceneBridge bridge = std::move(created).value();

    scene::Scene value{};
    scene::MeshInstance instance{};
    instance.source = doc::NodeId{9U};
    instance.mesh = scene::MeshKey{scene::Primitive::Box, 8U};
    instance.world = scene::identity();
    value.instances.push_back(instance);
    const auto first = bridge.update(value);
    RENDER_CHECK(first);
    RENDER_CHECK(first.value().added == 1U);
    RENDER_CHECK(first.value().records == 1U);

    const scene::Mat4 previous = value.instances.front().world;
    value.instances.front().world =
        scene::translation(scene::Vec3{1.0F, 2.0F, 3.0F});
    const auto second = bridge.update(value);
    RENDER_CHECK(second);
    RENDER_CHECK(second.value().updated == 1U);
    RENDER_CHECK(second.value().topology_digest ==
                 first.value().topology_digest);
    RENDER_CHECK(bridge.records().front().previous_world == previous);

    value.instances.front().world =
        scene::translation(scene::Vec3{4.0F, 5.0F, 6.0F});
    const auto staged = bridge.prepare(value);
    RENDER_CHECK(staged);
    RENDER_CHECK(bridge.records().front().world !=
                 value.instances.front().world);
    bridge.discard();
    RENDER_CHECK(bridge.records().front().world !=
                 value.instances.front().world);
    RENDER_CHECK(bridge.update(value));
    RENDER_CHECK(bridge.records().front().world ==
                 value.instances.front().world);

    // A Background fill is render-affecting: adding one, and then changing
    // any one of its values, each changes the visual digest.
    const auto unfilled = bridge.update(value);
    RENDER_CHECK(unfilled);
    value.instances.front().material.fill = scene::Fill{};
    const auto filled = bridge.update(value);
    RENDER_CHECK(filled);
    RENDER_CHECK(filled.value().visual_digest !=
                 unfilled.value().visual_digest);
    value.instances.front().material.fill->color_b.a = 0.25F;
    const auto retinted = bridge.update(value);
    RENDER_CHECK(retinted);
    RENDER_CHECK(retinted.value().visual_digest !=
                 filled.value().visual_digest);
    value.instances.front().material.fill->size = 2.0F;
    const auto resized = bridge.update(value);
    RENDER_CHECK(resized);
    RENDER_CHECK(resized.value().visual_digest !=
                 retinted.value().visual_digest);
    value.instances.front().material.fill.reset();

    scene::Scene too_large{};
    for (core::u64 index = 1U; index <= 5U; ++index) {
        scene::MeshInstance item{};
        item.source = doc::NodeId{index};
        item.world = scene::identity();
        too_large.instances.push_back(item);
    }
    const auto refused = bridge.update(too_large);
    RENDER_CHECK(!refused);
    RENDER_CHECK(refused.error() ==
                 render::SceneBridgeError::CapacityExceeded);
    RENDER_CHECK(bridge.records().size() == 1U);
}

void advanced_policy_fails_closed_and_optional_preview_falls_back() {
    scene::RendererSettings settings{};
    settings.backend = scene::RendererBackend::Advanced;
    settings.required = true;

    rhi::DeviceCapabilities capabilities{};
    capabilities.backend = rhi::BackendKind::D3d12;
    capabilities.advanced_scene_raster = true;
    capabilities.pixel_barycentrics = true;
    capabilities.offscreen = true;
    capabilities.rgba16_float = true;
    capabilities.sampled_depth = true;
    capabilities.integer_targets = true;

    // The current raster implementation does not call any speculative compute,
    // storage, bindless, explicit-barrier, or indirect API. Those flags remain
    // false and must not prevent the tested D3D12 raster path becoming Ready.
    const auto ready =
        render::evaluate_renderer_readiness(settings, capabilities, true);
    RENDER_CHECK(ready.satisfied);
    RENDER_CHECK(ready.code == render::RendererReadinessCode::Ready);
    RENDER_CHECK(ready.selected == scene::RendererBackend::Advanced);
    RENDER_CHECK(ready.missing_capabilities == 0U);

    struct BaselineRequirement final {
        bool rhi::DeviceCapabilities::* member;
        render::RendererCapability capability;
    };
    constexpr std::array baseline_requirements{
        BaselineRequirement{&rhi::DeviceCapabilities::advanced_scene_raster,
                            render::RendererCapability::
                                AdvancedSceneRasterImplementation},
        BaselineRequirement{&rhi::DeviceCapabilities::pixel_barycentrics,
                            render::RendererCapability::PixelBarycentrics},
        BaselineRequirement{&rhi::DeviceCapabilities::offscreen,
                            render::RendererCapability::Offscreen},
        BaselineRequirement{&rhi::DeviceCapabilities::rgba16_float,
                            render::RendererCapability::Rgba16Float},
        BaselineRequirement{&rhi::DeviceCapabilities::sampled_depth,
                            render::RendererCapability::SampledDepth},
        BaselineRequirement{&rhi::DeviceCapabilities::integer_targets,
                            render::RendererCapability::IntegerTargets},
    };
    for (const BaselineRequirement& requirement : baseline_requirements) {
        auto missing_capability = capabilities;
        missing_capability.*(requirement.member) = false;
        const auto required = render::evaluate_renderer_readiness(
            settings, missing_capability, true);
        RENDER_CHECK(!required.satisfied);
        RENDER_CHECK(required.code ==
                     render::RendererReadinessCode::MissingCapabilities);
        RENDER_CHECK(required.missing_capabilities ==
                     static_cast<core::u64>(requirement.capability));
    }

    // Authored future-pass requests name a missing software implementation,
    // not a deficient GPU. They stay unavailable even when a synthetic
    // adapter reports every hardware prerequisite; no pass implementation is
    // silently inferred from feature bits.
    struct FutureFeature final {
        bool scene::RendererSettings::* member;
        bool needs_ray_queries;
    };
    constexpr std::array future_features{
        FutureFeature{&scene::RendererSettings::ray_tracing, true},
        FutureFeature{&scene::RendererSettings::ddgi, true},
        FutureFeature{&scene::RendererSettings::restir, true},
        FutureFeature{&scene::RendererSettings::denoising, false},
    };
    for (const FutureFeature& feature : future_features) {
        auto requested = settings;
        requested.*(feature.member) = true;
        const auto unavailable = render::evaluate_renderer_readiness(
            requested, capabilities, true);
        RENDER_CHECK(!unavailable.satisfied);
        RENDER_CHECK(unavailable.code == render::RendererReadinessCode::
                                              RequestedFeaturesUnavailable);
        const core::u64 ray_query_capabilities =
            static_cast<core::u64>(
                render::RendererCapability::AccelerationStructures) |
            static_cast<core::u64>(render::RendererCapability::RayQueries);
        RENDER_CHECK(unavailable.missing_capabilities ==
                     (feature.needs_ray_queries ? ray_query_capabilities : 0U));

        auto hypothetical = capabilities;
        hypothetical.acceleration_structures = true;
        hypothetical.ray_queries = true;
        const auto still_unavailable = render::evaluate_renderer_readiness(
            requested, hypothetical, true);
        RENDER_CHECK(!still_unavailable.satisfied);
        RENDER_CHECK(still_unavailable.code ==
                     render::RendererReadinessCode::
                         RequestedFeaturesUnavailable);
        RENDER_CHECK(still_unavailable.missing_capabilities == 0U);
    }

    // Temporal antialiasing is no longer one of them. It sat in that list only
    // because there was no resolve to run; now there is, so authoring it is a
    // capability request this renderer satisfies rather than a request for
    // work that does not exist. The spatial fallback and the jitter period are
    // likewise ordinary authored policy.
    {
        auto temporal = settings;
        temporal.taau = true;
        temporal.spatial_antialiasing = true;
        temporal.taa_sequence_length = 16U;
        const auto available =
            render::evaluate_renderer_readiness(temporal, capabilities, true);
        RENDER_CHECK(available.satisfied);
        RENDER_CHECK(available.code == render::RendererReadinessCode::Ready);
        RENDER_CHECK(available.selected == scene::RendererBackend::Advanced);
        RENDER_CHECK(available.missing_capabilities == 0U);
    }

    // An adapter without pixel barycentrics (older than Turing, RDNA or Gen12)
    // cannot build the Advanced lit stage. An optional request previews on
    // Basic and names that one requirement, not a generic shortfall.
    {
        auto pre_barycentric = capabilities;
        pre_barycentric.pixel_barycentrics = false;
        auto optional = settings;
        optional.required = false;
        const auto fallback = render::evaluate_renderer_readiness(
            optional, pre_barycentric, true);
        RENDER_CHECK(fallback.satisfied);
        RENDER_CHECK(fallback.fallback);
        RENDER_CHECK(fallback.code == render::RendererReadinessCode::BasicFallback);
        RENDER_CHECK(fallback.selected == scene::RendererBackend::Basic);
        RENDER_CHECK(fallback.missing_capabilities ==
                     static_cast<core::u64>(
                         render::RendererCapability::PixelBarycentrics));
        RENDER_CHECK(!pre_barycentric.advanced_raster());
    }

    capabilities.advanced_scene_raster = false;
    settings.required = false;
    const auto preview =
        render::evaluate_renderer_readiness(settings, capabilities, true);
    RENDER_CHECK(preview.satisfied);
    RENDER_CHECK(preview.fallback);
    RENDER_CHECK(preview.selected == scene::RendererBackend::Basic);
    RENDER_CHECK(preview.missing_capabilities ==
                 static_cast<core::u64>(render::RendererCapability::
                                            AdvancedSceneRasterImplementation));

    settings = scene::RendererSettings{};
    const auto basic =
        render::evaluate_renderer_readiness(settings, capabilities);
    RENDER_CHECK(basic.satisfied);
    RENDER_CHECK(!basic.fallback);
    RENDER_CHECK(basic.code == render::RendererReadinessCode::Ready);
}

// One immediate, in-memory timeline plate for the compositor exposure proof.
// Its left half is transparent and its right half is opaque green, so one
// composited frame exposes the environment while pinning that display-referred
// plate pixels themselves do not receive camera exposure.
class CompositorExposurePlateSource final : public render::MediaSource {
public:
    [[nodiscard]] std::string key(
        const std::string& source, core::u32 frame_index) const override {
        return source + "#" + std::to_string(frame_index);
    }

    [[nodiscard]] render::MediaDecodeScheduling scheduling(
        const std::string&) const noexcept override {
        return render::MediaDecodeScheduling::SourceManaged;
    }

    [[nodiscard]] render::DecodedFrame decode(
        const std::string&, core::u32 frame_index) override {
        constexpr core::u32 width = 8U;
        constexpr core::u32 height = 8U;
        render::DecodedFrame frame{};
        frame.width = width;
        frame.height = height;
        frame.source_frame = frame_index;
        frame.pixels.resize(width * height * 4U);
        for (core::u32 y = 0U; y < height; ++y) {
            for (core::u32 x = 0U; x < width; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * width + x) * 4U;
                const bool opaque = x >= width / 2U;
                frame.pixels[offset + 0U] = 0U;
                frame.pixels[offset + 1U] = opaque ? 128U : 0U;
                frame.pixels[offset + 2U] = 0U;
                frame.pixels[offset + 3U] = opaque ? 255U : 0U;
            }
        }
        return frame;
    }
};

void native_compositor_exposes_only_advanced_environment() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    config.debug = true;
    config.frames_in_flight = 2U;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();

    constexpr core::u32 extent = 32U;
    render::RendererLimits limits{};
    limits.max_instances = 16U;
    limits.max_vertices = 1'024U;
    limits.max_indices = 3'072U;
    limits.max_lights = 8U;
    limits.max_inline_effect_ops = 64U;
    limits.max_inline_curve_luts = 4U;
    limits.max_inline_effect_parameter_vectors = 4'160U;

    const auto make_scene = [](scene::RendererBackend backend) {
        scene::Scene value{};
        value.id = scene::SceneId{701U};
        value.document_version = doc::Version{11U};
        value.camera_source = doc::NodeId{702U};
        value.canvas = scene::Canvas{extent, extent};
        value.camera.eye = scene::Vec3{0.0F, 0.0F, 3.0F};
        value.camera.target = scene::Vec3{};
        value.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
        value.camera.field_of_view = 0.125F;
        value.camera.near_plane = 0.1F;
        value.camera.far_plane = 100.0F;
        value.environment.sky_color = scene::Vec3{0.125F, 0.125F, 0.125F};
        value.renderer_settings.backend = backend;
        value.renderer_settings.required =
            backend == scene::RendererBackend::Advanced;
        return value;
    };

    struct LinearPixel final {
        core::u16 red{};
        core::u16 green{};
        core::u16 blue{};
        core::u16 alpha{};

        bool operator==(const LinearPixel&) const = default;
    };
    struct Capture final {
        LinearPixel left;
        LinearPixel right;
        core::u32 layers{};
        render::RendererReadiness readiness{};
        bool scene_linear{};
    };

    const auto capture = [&](render::CompositorSceneExecutor& executor,
                             const scene::Scene& value,
                             render::MediaCache* cache = nullptr) {
        constexpr std::size_t pixel_bytes = 8U;
        auto readback = device.create_readback(
            extent * extent * pixel_bytes);
        RENDER_CHECK(readback);
        auto acquired = device.acquire_commands();
        RENDER_CHECK(acquired);
        auto commands = std::move(acquired).value();
        if (cache != nullptr) {
            cache->begin_frame();
        }
        const auto result = executor.execute(
            commands, value, scene::whole(value.canvas), cache);
        RENDER_CHECK(result);
        RENDER_CHECK(result.value() != nullptr);
        RENDER_CHECK(result.value()->desc().format ==
                     rhi::TextureFormat::Rgba16Float);
        RENDER_CHECK(commands.download(*result.value(), readback.value()));
        if (cache != nullptr) {
            cache->end_frame();
        }
        auto submitted = device.submit(std::move(commands));
        RENDER_CHECK(submitted);
        RENDER_CHECK(std::move(submitted).value().wait());

        const auto bytes = readback.value().map();
        RENDER_CHECK(bytes.size() == extent * extent * pixel_bytes);
        const auto sample = [&](core::u32 x) {
            LinearPixel pixel{};
            const std::size_t offset =
                (static_cast<std::size_t>(extent / 2U) * extent + x) *
                pixel_bytes;
            std::memcpy(&pixel, bytes.data() + offset, sizeof(pixel));
            return pixel;
        };
        Capture captured{};
        captured.left = sample(extent / 4U);
        captured.right = sample((extent * 3U) / 4U);
        captured.layers = executor.stats().layers;
        captured.readiness = executor.renderer_readiness();
        captured.scene_linear = executor.scene_linear_output();
        readback.value().unmap();
        return captured;
    };

    const auto create_executor = [&]() {
        auto created = render::CompositorSceneExecutor::create(
            device, extent, extent, limits, render::MediaCacheConfig{},
            rhi::TextureFormat::Rgba16Float);
        RENDER_CHECK(created);
        return std::move(created).value();
    };

    scene::Scene empty = make_scene(scene::RendererBackend::Advanced);
    auto empty_executor = create_executor();
    empty.camera.exposure_compensation_ev = -1.0F;
    const Capture empty_minus = capture(empty_executor, empty);
    empty.camera.exposure_compensation_ev = 0.0F;
    const Capture empty_neutral = capture(empty_executor, empty);
    empty.camera.exposure_compensation_ev = 1.0F;
    const Capture empty_plus = capture(empty_executor, empty);
    RENDER_CHECK(empty_minus.layers == 0U && empty_neutral.layers == 0U &&
                 empty_plus.layers == 0U);
    RENDER_CHECK(empty_minus.readiness.selected ==
                     scene::RendererBackend::Advanced &&
                 empty_minus.scene_linear);
    RENDER_CHECK(empty_minus.left == empty_minus.right);
    RENDER_CHECK(empty_neutral.left == empty_neutral.right);
    RENDER_CHECK(empty_plus.left == empty_plus.right);
    RENDER_CHECK(empty_minus.left.red < empty_neutral.left.red &&
                 empty_neutral.left.red < empty_plus.left.red);

    scene::Scene plate = make_scene(scene::RendererBackend::Advanced);
    scene::MediaInstance clip{};
    clip.mesh = scene::MeshKey{scene::Primitive::Quad, 1U};
    clip.world = scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F});
    clip.normal = scene::normal_basis(clip.world);
    clip.source = "memory://compositor-exposure-plate";
    clip.source_node = doc::NodeId{703U};
    plate.media.push_back(clip);
    auto plate_executor = create_executor();
    render::MediaCache plate_cache{
        std::make_unique<CompositorExposurePlateSource>()};
    plate.camera.exposure_compensation_ev = -1.0F;
    const Capture plate_minus = capture(plate_executor, plate, &plate_cache);
    plate.camera.exposure_compensation_ev = 0.0F;
    const Capture plate_neutral =
        capture(plate_executor, plate, &plate_cache);
    plate.camera.exposure_compensation_ev = 1.0F;
    const Capture plate_plus = capture(plate_executor, plate, &plate_cache);
    RENDER_CHECK(plate_minus.layers == 1U && plate_neutral.layers == 1U &&
                 plate_plus.layers == 1U);
    RENDER_CHECK(plate_minus.readiness.selected ==
                     scene::RendererBackend::Advanced &&
                 plate_minus.scene_linear);

    const auto is_exposed_sky = [](const LinearPixel& minus,
                                   const LinearPixel& neutral,
                                   const LinearPixel& plus) {
        return minus.red < neutral.red && neutral.red < plus.red;
    };
    const auto is_unexposed_green_plate = [](const LinearPixel& minus,
                                              const LinearPixel& neutral,
                                              const LinearPixel& plus) {
        return minus == neutral && neutral == plus && neutral.red == 0U &&
            neutral.green != 0U;
    };
    const bool left_is_sky = is_exposed_sky(
        plate_minus.left, plate_neutral.left, plate_plus.left);
    const bool right_is_sky = is_exposed_sky(
        plate_minus.right, plate_neutral.right, plate_plus.right);
    const bool left_is_plate = is_unexposed_green_plate(
        plate_minus.left, plate_neutral.left, plate_plus.left);
    const bool right_is_plate = is_unexposed_green_plate(
        plate_minus.right, plate_neutral.right, plate_plus.right);
    RENDER_CHECK((left_is_sky && right_is_plate) ||
                 (right_is_sky && left_is_plate));

    scene::Scene basic_empty = make_scene(scene::RendererBackend::Basic);
    auto basic_empty_executor = create_executor();
    basic_empty.camera.exposure_compensation_ev = -1.0F;
    const Capture basic_empty_minus =
        capture(basic_empty_executor, basic_empty);
    basic_empty.camera.exposure_compensation_ev = 1.0F;
    const Capture basic_empty_plus =
        capture(basic_empty_executor, basic_empty);
    RENDER_CHECK(basic_empty_minus.left == basic_empty_plus.left);
    RENDER_CHECK(basic_empty_minus.right == basic_empty_plus.right);
    RENDER_CHECK(basic_empty_minus.readiness.selected ==
                     scene::RendererBackend::Basic &&
                 !basic_empty_minus.scene_linear);

    scene::Scene basic_plate = plate;
    basic_plate.renderer_settings.backend = scene::RendererBackend::Basic;
    basic_plate.renderer_settings.required = false;
    auto basic_plate_executor = create_executor();
    render::MediaCache basic_plate_cache{
        std::make_unique<CompositorExposurePlateSource>()};
    basic_plate.camera.exposure_compensation_ev = -1.0F;
    const Capture basic_plate_minus =
        capture(basic_plate_executor, basic_plate, &basic_plate_cache);
    basic_plate.camera.exposure_compensation_ev = 1.0F;
    const Capture basic_plate_plus =
        capture(basic_plate_executor, basic_plate, &basic_plate_cache);
    RENDER_CHECK(basic_plate_minus.left == basic_plate_plus.left);
    RENDER_CHECK(basic_plate_minus.right == basic_plate_plus.right);
    RENDER_CHECK(basic_plate_minus.layers == 1U &&
                 basic_plate_plus.layers == 1U);
    RENDER_CHECK(basic_plate_minus.readiness.selected ==
                     scene::RendererBackend::Basic &&
                 !basic_plate_minus.scene_linear);
}

void native_advanced_raster_emits_the_complete_linear_product_set() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    config.debug = true;
    config.frames_in_flight = 2U;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();

    constexpr core::u32 extent = 32U;
    auto output_created =
        render::OutputTransform::create(device, extent, extent, true);
    RENDER_CHECK(output_created);
    auto output = std::move(output_created).value();
    RENDER_CHECK(output.target().desc().format
                 == rhi::TextureFormat::Rgba8Unorm);
    {
        auto mixer = render::SceneMixer::create(
            device, extent, extent, rhi::TextureFormat::Rgba16Float);
        RENDER_CHECK(mixer);
        RENDER_CHECK(mixer.value().target().desc().format
                     == rhi::TextureFormat::Rgba16Float);
        render::ProjectionMeshWarpLimits warp_limits{};
        warp_limits.max_vertices = 64U;
        warp_limits.max_indices = 192U;
        auto warp = render::ProjectionMeshWarp::create(
            device, extent, extent, warp_limits,
            rhi::TextureFormat::Rgba16Float);
        RENDER_CHECK(warp);
        RENDER_CHECK(warp.value().target().desc().format
                     == rhi::TextureFormat::Rgba16Float);
        render::RendererLimits small{};
        small.max_instances = 16U;
        small.max_vertices = 1'024U;
        small.max_indices = 3'072U;
        small.max_lights = 8U;
        small.max_inline_effect_ops = 64U;
        small.max_inline_curve_luts = 4U;
        small.max_inline_effect_parameter_vectors = 4'160U;
        auto compositor = render::CompositorSceneExecutor::create(
            device, extent, extent, small, render::MediaCacheConfig{},
            rhi::TextureFormat::Rgba16Float);
        RENDER_CHECK(compositor);
        RENDER_CHECK(compositor.value().target().desc().format
                     == rhi::TextureFormat::Rgba16Float);
    }
    scene::Scene value{};
    value.id = scene::SceneId{41U};
    value.document_version = doc::Version{7U};
    value.camera_source = doc::NodeId{40U};
    value.canvas = scene::Canvas{extent, extent};
    value.camera.eye = scene::Vec3{0.0F, 0.0F, 3.0F};
    value.camera.target = scene::Vec3{};
    value.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    value.camera.field_of_view = 0.125F;
    value.camera.near_plane = 0.1F;
    value.camera.far_plane = 100.0F;
    value.environment.ambient = 1.0F;
    value.environment.sky_color = scene::Vec3{0.05F, 0.07F, 0.1F};
    value.environment.ground_color = scene::Vec3{0.02F, 0.02F, 0.02F};
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.required = true;
    // This increment is the raster vertical slice. Features that require the
    // later RT/temporal passes remain explicit fail-closed requests.
    value.renderer_settings.ray_tracing = false;
    value.renderer_settings.ddgi = false;
    value.renderer_settings.restir = false;
    value.renderer_settings.denoising = false;
    value.renderer_settings.taau = false;
    value.renderer_settings.motion_blur = false;
    scene::MeshInstance box{};
    box.source = doc::NodeId{77U};
    box.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    box.world = scene::identity();
    box.normal = scene::normal_basis(box.world);
    box.material.base_color = scene::Vec3{0.8F, 0.3F, 0.2F};
    box.material.roughness = 0.5F;
    value.instances.push_back(box);
    scene::DirectionalLight light{};
    light.direction = scene::Vec3{-0.3F, -0.5F, -0.8F};
    light.color = scene::Vec3{1.0F, 1.0F, 1.0F};
    light.intensity = 3.0F;
    value.directional_lights.push_back(light);

    render::RendererLimits renderer_limits{};
    // One ordinary offscreen owner plus two simultaneous mapped views. A
    // fourth identity below proves deterministic bounded eviction.
    renderer_limits.max_history_slots = 3U;
    auto renderer_created = render::HybridSceneRenderer::create(
        device, extent, extent, renderer_limits);
    RENDER_CHECK(renderer_created);
    auto renderer = std::move(renderer_created).value();
    auto commands = device.acquire_commands();
    RENDER_CHECK(commands);
    auto list = std::move(commands).value();
    render::RenderFrameRequest request{};
    request.scene = &value;
    request.slice = scene::whole(value.canvas);
    request.output_width = extent;
    request.output_height = extent;
    request.options.transparent_background = true;
    request.history.scene = value.id;
    request.history.document_version = value.document_version;
    request.history.surface = render::HistorySurface::Offscreen;
    request.history.canvas = value.canvas;
    request.history.slice = request.slice;
    request.history.width = extent;
    request.history.height = extent;
    request.history.camera_source = value.camera_source;
    request.history.camera_digest =
        render::camera_history_digest(value.camera);
    request.history.profile_digest =
        render::renderer_profile_digest(value.renderer_settings);
    auto rendered = renderer.render_frame(list, request);
    if (!rendered) {
        std::cerr << "advanced render error="
                  << static_cast<int>(rendered.error())
                  << " readiness="
                  << static_cast<int>(
                         renderer.diagnostics().last_readiness.code)
                  << '\n';
    }
    RENDER_CHECK(rendered);
    RENDER_CHECK(renderer.stats().directional_shadow_rendered);
    RENDER_CHECK(renderer.stats().directional_shadow_casters == 1U);
    RENDER_CHECK(renderer.stats().directional_gi_rendered);
    const render::RenderProducts products = rendered.value();
    RENDER_CHECK(products.readiness.code
                 == render::RendererReadinessCode::Ready);
    RENDER_CHECK(products.scene_linear);
    RENDER_CHECK(products.color && products.depth && products.velocity
                 && products.object_id && products.coverage);
    RENDER_CHECK(products.color->desc().format
                 == rhi::TextureFormat::Rgba16Float);
    RENDER_CHECK(products.depth->desc().format
                 == rhi::TextureFormat::Depth32Float);
    RENDER_CHECK(products.velocity->desc().format
                 == rhi::TextureFormat::Rgba32Float);
    RENDER_CHECK(products.object_id->desc().format
                 == rhi::TextureFormat::R32Uint);
    RENDER_CHECK(products.coverage->desc().format
                 == rhi::TextureFormat::R32Float);
    RENDER_CHECK(output.render(
        list, *products.color, scene::CanvasRect{0U, 0U, extent, extent}));

    auto color = device.create_readback(extent * extent * 8U);
    auto depth = device.create_readback(extent * extent * 4U);
    auto velocity = device.create_readback(extent * extent * 16U);
    auto object_id = device.create_readback(extent * extent * 4U);
    auto coverage = device.create_readback(extent * extent * 4U);
    auto connector = device.create_readback(extent * extent * 4U);
    RENDER_CHECK(color && depth && velocity && object_id && coverage
                 && connector);
    RENDER_CHECK(list.download(*products.color, color.value()));
    RENDER_CHECK(list.download(*products.depth, depth.value()));
    RENDER_CHECK(list.download(*products.velocity, velocity.value()));
    RENDER_CHECK(list.download(*products.object_id, object_id.value()));
    RENDER_CHECK(list.download(*products.coverage, coverage.value()));
    RENDER_CHECK(list.download(output.target(), connector.value()));
    auto submitted = device.submit(std::move(list));
    RENDER_CHECK(submitted);
    RENDER_CHECK(submitted.value().wait());

    const std::size_t centre =
        static_cast<std::size_t>(extent / 2U) * extent + extent / 2U;
    const auto color_bytes = color.value().map();
    const auto depth_bytes = depth.value().map();
    const auto velocity_bytes = velocity.value().map();
    const auto id_bytes = object_id.value().map();
    const auto coverage_bytes = coverage.value().map();
    const auto connector_bytes = connector.value().map();
    RENDER_CHECK(color_bytes.size() == extent * extent * 8U);
    core::u16 centre_red{};
    std::memcpy(&centre_red, color_bytes.data() + centre * 8U,
                sizeof(centre_red));
    RENDER_CHECK(centre_red != 0U);
    core::u16 centre_alpha{};
    std::memcpy(&centre_alpha, color_bytes.data() + centre * 8U + 6U,
                sizeof(centre_alpha));
    RENDER_CHECK(centre_alpha != 0U);
    float centre_depth{};
    std::memcpy(&centre_depth, depth_bytes.data() + centre * 4U,
                sizeof(centre_depth));
    RENDER_CHECK(centre_depth > 0.0F);
    core::u32 centre_id{};
    std::memcpy(&centre_id, id_bytes.data() + centre * 4U,
                sizeof(centre_id));
    RENDER_CHECK(centre_id != 0U);
    float centre_coverage{};
    std::memcpy(&centre_coverage, coverage_bytes.data() + centre * 4U,
                sizeof(centre_coverage));
    RENDER_CHECK(centre_coverage > 0.99F);
    core::u32 centre_velocity_bits{};
    std::memcpy(&centre_velocity_bits,
                velocity_bytes.data() + centre * 16U,
                sizeof(centre_velocity_bits));
    RENDER_CHECK((centre_velocity_bits & 0x7FFF'FFFFU) == 0U);
    RENDER_CHECK(connector_bytes[centre * 4U + 3U] == 255U);
    RENDER_CHECK(connector_bytes[3U] == 0U);
    color.value().unmap();
    depth.value().unmap();
    velocity.value().unmap();
    object_id.value().unmap();
    coverage.value().unmap();
    connector.value().unmap();

    const auto render_color = [&](render::HistoryDisposition expected) {
        auto acquired = device.acquire_commands();
        RENDER_CHECK(acquired);
        auto list = std::move(acquired).value();
        auto recorded = renderer.render_frame(list, request);
        RENDER_CHECK(recorded);
        RENDER_CHECK(recorded.value().history.disposition == expected);
        auto readback = device.create_readback(extent * extent * 8U);
        RENDER_CHECK(readback);
        RENDER_CHECK(list.download(
            *recorded.value().color, readback.value()));
        auto work = device.submit(std::move(list));
        RENDER_CHECK(work);
        RENDER_CHECK(work.value().wait());
        const auto mapped = readback.value().map();
        RENDER_CHECK(mapped.size() == extent * extent * 8U);
        std::vector<core::u8> bytes{mapped.begin(), mapped.end()};
        readback.value().unmap();
        return bytes;
    };

    const auto render_velocity = [&](render::HistoryDisposition expected) {
        auto acquired = device.acquire_commands();
        RENDER_CHECK(acquired);
        auto list = std::move(acquired).value();
        auto recorded = renderer.render_frame(list, request);
        RENDER_CHECK(recorded);
        RENDER_CHECK(recorded.value().history.disposition == expected);
        auto readback = device.create_readback(extent * extent * 16U);
        RENDER_CHECK(readback);
        RENDER_CHECK(list.download(
            *recorded.value().velocity, readback.value()));
        auto work = device.submit(std::move(list));
        RENDER_CHECK(work);
        RENDER_CHECK(work.value().wait());
        const auto mapped = readback.value().map();
        RENDER_CHECK(mapped.size() == extent * extent * 16U);
        std::vector<core::u8> bytes{mapped.begin(), mapped.end()};
        readback.value().unmap();
        return bytes;
    };
    const auto velocity_is_zero = [](std::span<const core::u8> bytes) {
        for (std::size_t offset = 0U; offset < bytes.size(); offset += 16U) {
            std::array<float, 2U> xy{};
            std::memcpy(xy.data(), bytes.data() + offset, sizeof(xy));
            if (xy[0] != 0.0F || xy[1] != 0.0F) return false;
        }
        return true;
    };

    value.show_frame = 1U;
    request.options.transparent_background = false;
    const auto default_sky =
        render_color(render::HistoryDisposition::Advance);
    core::u16 default_sky_red{};
    std::memcpy(&default_sky_red, default_sky.data(),
                sizeof(default_sky_red));
    RENDER_CHECK(default_sky_red != 0U);

    value.camera.exposure_compensation_ev = 1.0F;
    request.history.camera_digest =
        render::camera_history_digest(value.camera);
    const auto exposed_color =
        render_color(render::HistoryDisposition::Reset);
    core::u16 exposed_red{};
    std::memcpy(&exposed_red, exposed_color.data() + centre * 8U,
                sizeof(exposed_red));
    RENDER_CHECK(exposed_red > centre_red);
    core::u16 exposed_sky_red{};
    std::memcpy(&exposed_sky_red, exposed_color.data(),
                sizeof(exposed_sky_red));
    RENDER_CHECK(exposed_sky_red > default_sky_red);

    value.show_frame = 2U;
    value.camera.exposure_compensation_ev = 0.0F;
    request.history.camera_digest =
        render::camera_history_digest(value.camera);
    const auto exposure_reset_velocity =
        render_velocity(render::HistoryDisposition::Reset);
    RENDER_CHECK(velocity_is_zero(exposure_reset_velocity));

    value.show_frame = 3U;
    value.instances.front().world =
        scene::translation(scene::Vec3{0.15F, 0.0F, 0.0F});
    value.instances.front().normal =
        scene::normal_basis(value.instances.front().world);
    // Sequential camera pose animation remains history-continuous. Pose is
    // previous/current render data, not a cut identity.
    value.camera.eye.x += 0.05F;
    value.camera.target.x += 0.05F;
    const auto advanced_velocity =
        render_velocity(render::HistoryDisposition::Advance);
    RENDER_CHECK(!velocity_is_zero(advanced_velocity));

    const auto held_velocity =
        render_velocity(render::HistoryDisposition::Hold);
    RENDER_CHECK(held_velocity == advanced_velocity);

    value.show_frame = 4U;
    // Editing that same camera while the show is running advances the
    // replicated document epoch and is a reset, not extreme camera motion.
    value.document_version = doc::Version{8U};
    value.camera.eye.x += 0.5F;
    value.camera.target.x += 0.5F;
    request.history.document_version = value.document_version;
    const auto edit_reset_velocity =
        render_velocity(render::HistoryDisposition::Reset);
    RENDER_CHECK(velocity_is_zero(edit_reset_velocity));

    value.show_frame = 5U;
    value.camera_source = doc::NodeId{401U};
    request.history.camera_source = value.camera_source;
    request.history.camera_digest =
        render::camera_history_digest(value.camera);
    const auto reset_velocity =
        render_velocity(render::HistoryDisposition::Reset);
    RENDER_CHECK(velocity_is_zero(reset_velocity));

    // Alternating output maps must advance independently. The first map at
    // N+1 commits the scene transforms, while the second still needs N as its
    // motion source. Both maps then reproduce that velocity on a held frame.
    render::SceneRenderView map_a{
        scene::view_for_slice(value.camera, value.canvas, request.slice),
        value.camera.eye,
        scene::normalize(value.camera.target - value.camera.eye),
        1'001U};
    render::SceneRenderView map_b = map_a;
    map_b.history_identity = 1'002U;
    value.show_frame = 10U;
    request.options.view = map_a;
    request.history.view_digest = map_a.history_identity;
    const auto map_a_initial =
        render_velocity(render::HistoryDisposition::Initialize);
    RENDER_CHECK(velocity_is_zero(map_a_initial));
    request.options.view = map_b;
    request.history.view_digest = map_b.history_identity;
    const auto map_b_initial =
        render_velocity(render::HistoryDisposition::Initialize);
    RENDER_CHECK(velocity_is_zero(map_b_initial));

    value.show_frame = 11U;
    value.instances.front().world =
        scene::translation(scene::Vec3{0.35F, 0.0F, 0.0F});
    value.instances.front().normal =
        scene::normal_basis(value.instances.front().world);
    request.options.view = map_a;
    request.history.view_digest = map_a.history_identity;
    const auto map_a_advanced =
        render_velocity(render::HistoryDisposition::Advance);
    RENDER_CHECK(!velocity_is_zero(map_a_advanced));
    request.options.view = map_b;
    request.history.view_digest = map_b.history_identity;
    const auto map_b_advanced =
        render_velocity(render::HistoryDisposition::Advance);
    RENDER_CHECK(map_b_advanced == map_a_advanced);

    request.options.view = map_a;
    request.history.view_digest = map_a.history_identity;
    RENDER_CHECK(
        render_velocity(render::HistoryDisposition::Hold) == map_a_advanced);
    request.options.view = map_b;
    request.history.view_digest = map_b.history_identity;
    RENDER_CHECK(
        render_velocity(render::HistoryDisposition::Hold) == map_b_advanced);

    render::SceneRenderView map_c = map_a;
    map_c.history_identity = 1'003U;
    request.options.view = map_c;
    request.history.view_digest = map_c.history_identity;
    RENDER_CHECK(velocity_is_zero(
        render_velocity(render::HistoryDisposition::Initialize)));
    RENDER_CHECK(renderer.diagnostics().history_slot_evictions == 1U);
    RENDER_CHECK(renderer.diagnostics().history_slots == 3U);
    RENDER_CHECK(renderer.diagnostics().history_slot_capacity == 3U);
    request.options.view = map_a;
    request.history.view_digest = map_a.history_identity;
    RENDER_CHECK(
        render_velocity(render::HistoryDisposition::Hold) == map_a_advanced);
}

void native_directional_shadow_and_diffuse_bounce_are_slice_deterministic() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    config.debug = true;
    config.frames_in_flight = 2U;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();

    constexpr core::u32 extent = 96U;
    scene::Scene receiver{};
    receiver.id = scene::SceneId{51U};
    receiver.document_version = doc::Version{1U};
    receiver.canvas = scene::Canvas{extent, extent};
    receiver.camera.eye = scene::Vec3{0.0F, 2.4F, 4.0F};
    receiver.camera.target = scene::Vec3{0.0F, -0.35F, 0.0F};
    receiver.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    receiver.camera.field_of_view = 0.14F;
    receiver.camera.near_plane = 0.1F;
    receiver.camera.far_plane = 100.0F;
    receiver.environment.ambient = 0.05F;
    receiver.environment.sky_color = scene::Vec3{1.0F, 1.0F, 1.0F};
    receiver.environment.ground_color = scene::Vec3{1.0F, 1.0F, 1.0F};

    scene::MeshInstance floor{};
    floor.source = doc::NodeId{501U};
    floor.mesh = scene::MeshKey{scene::Primitive::Plane, 1U};
    floor.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, -0.5F, 0.0F}),
        scene::scaling(scene::Vec3{5.0F, 1.0F, 5.0F}));
    floor.normal = scene::normal_basis(floor.world);
    floor.material.base_color = scene::Vec3{0.8F, 0.05F, 0.05F};
    floor.material.roughness = 0.8F;
    receiver.instances.push_back(floor);

    scene::DirectionalLight light{};
    light.direction = scene::normalize(
        scene::Vec3{-0.8F, -1.0F, -0.35F});
    light.color = scene::Vec3{1.0F, 1.0F, 1.0F};
    light.intensity = 4.0F;
    receiver.directional_lights.push_back(light);

    scene::Scene occluded = receiver;
    scene::MeshInstance box{};
    box.source = doc::NodeId{502U};
    box.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    box.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.0F, 0.0F}),
        scene::scaling(scene::Vec3{0.75F, 1.0F, 0.75F}));
    box.normal = scene::normal_basis(box.world);
    box.material.base_color = scene::Vec3{0.7F, 0.7F, 0.7F};
    box.material.roughness = 0.8F;
    occluded.instances.push_back(box);

    struct Capture final {
        std::vector<core::u8> color;
        std::vector<core::u8> object_id;
        render::RenderStats stats{};
    };
    const auto capture = [&device](
                             const scene::Scene& source,
                             scene::CanvasRect slice,
                             core::u32 output_width,
                             core::u32 output_height,
                             float gi_strength,
                             core::u32 cascades = 1U) {
        render::RendererLimits limits{};
        limits.max_instances = 8U;
        limits.max_vertices = 2'048U;
        limits.max_indices = 6'144U;
        limits.max_lights = 4U;
        limits.directional_shadow_resolution = 256U;
        // Shadow cascade count and bounce strength are replicated show state,
        // not deployment settings, so they arrive with the scene.
        scene::Scene value = source;
        value.renderer_settings.shadow_cascades = cascades;
        value.renderer_settings.gi_strength = gi_strength;
        auto created = render::SceneRenderer::create(
            device, output_width, output_height, limits,
            render::SceneRendererMode::AdvancedLinear);
        RENDER_CHECK(created);
        auto renderer = std::move(created).value();
        RENDER_CHECK(renderer.complete_products());

        auto color = device.create_readback(
            output_width * output_height * 8U);
        auto object_id = device.create_readback(
            output_width * output_height * 4U);
        RENDER_CHECK(color && object_id);
        auto acquired = device.acquire_commands();
        RENDER_CHECK(acquired);
        auto commands = std::move(acquired).value();
        RENDER_CHECK(renderer.render(commands, value, slice));
        Capture result{};
        result.stats = renderer.stats();
        RENDER_CHECK(commands.download(renderer.target(), color.value()));
        RENDER_CHECK(commands.download(
            renderer.object_id_target(), object_id.value()));
        auto submitted = device.submit(std::move(commands));
        RENDER_CHECK(submitted);
        RENDER_CHECK(submitted.value().wait());
        const auto color_bytes = color.value().map();
        const auto id_bytes = object_id.value().map();
        result.color.assign(color_bytes.begin(), color_bytes.end());
        result.object_id.assign(id_bytes.begin(), id_bytes.end());
        color.value().unmap();
        object_id.value().unmap();
        return result;
    };

    const Capture unshadowed = capture(
        receiver, scene::whole(receiver.canvas), extent, extent, 1.0F);
    const Capture shadowed = capture(
        occluded, scene::whole(occluded.canvas), extent, extent, 1.0F);
    const Capture direct_only = capture(
        occluded, scene::whole(occluded.canvas), extent, extent, 0.0F);
    RENDER_CHECK(unshadowed.stats.directional_shadow_rendered);
    RENDER_CHECK(unshadowed.stats.directional_shadow_casters == 1U);
    RENDER_CHECK(unshadowed.stats.directional_shadow_cascades == 1U);
    RENDER_CHECK(shadowed.stats.directional_shadow_rendered);
    RENDER_CHECK(shadowed.stats.directional_shadow_casters == 2U);
    RENDER_CHECK(shadowed.stats.directional_gi_rendered);
    RENDER_CHECK(!direct_only.stats.directional_gi_rendered);

    std::size_t visibly_shadowed_receiver_pixels{};
    for (std::size_t pixel = 0U;
         pixel < static_cast<std::size_t>(extent) * extent; ++pixel) {
        core::u32 reference_id{};
        core::u32 shadowed_id{};
        std::memcpy(
            &reference_id, unshadowed.object_id.data() + pixel * 4U,
            sizeof(reference_id));
        std::memcpy(
            &shadowed_id, shadowed.object_id.data() + pixel * 4U,
            sizeof(shadowed_id));
        if (reference_id == 0U || shadowed_id != reference_id) {
            continue;
        }
        core::u16 reference_red{};
        core::u16 shadowed_red{};
        std::memcpy(
            &reference_red, unshadowed.color.data() + pixel * 8U,
            sizeof(reference_red));
        std::memcpy(
            &shadowed_red, shadowed.color.data() + pixel * 8U,
            sizeof(shadowed_red));
        // Both values are finite non-negative scene-linear half floats, whose
        // bit patterns preserve ordering. Require a material difference rather
        // than counting PCF-edge noise or round-off.
        if (static_cast<core::u32>(shadowed_red) + 128U
            < static_cast<core::u32>(reference_red)) {
            ++visibly_shadowed_receiver_pixels;
        }
    }
    RENDER_CHECK(visibly_shadowed_receiver_pixels >= 8U);

    const auto gpu_object_id = [](core::u64 source) noexcept {
        core::u32 value = static_cast<core::u32>(source)
            ^ static_cast<core::u32>(source >> 32U);
        value ^= value >> 16U;
        value *= 0x7FEB352DU;
        value ^= value >> 15U;
        value *= 0x846CA68BU;
        value ^= value >> 16U;
        return value == 0U ? 1U : value;
    };
    const core::u32 box_object_id = gpu_object_id(box.source.value());
    std::size_t visibly_bounced_pixels{};
    for (std::size_t pixel = 0U;
         pixel < static_cast<std::size_t>(extent) * extent; ++pixel) {
        core::u32 lit_id{};
        core::u32 direct_id{};
        std::memcpy(
            &lit_id, shadowed.object_id.data() + pixel * 4U,
            sizeof(lit_id));
        std::memcpy(
            &direct_id, direct_only.object_id.data() + pixel * 4U,
            sizeof(direct_id));
        if (lit_id != box_object_id || lit_id != direct_id) {
            continue;
        }
        core::u16 lit_red{};
        core::u16 direct_red{};
        core::u16 lit_green{};
        std::memcpy(
            &lit_red, shadowed.color.data() + pixel * 8U,
            sizeof(lit_red));
        std::memcpy(
            &direct_red, direct_only.color.data() + pixel * 8U,
            sizeof(direct_red));
        std::memcpy(
            &lit_green, shadowed.color.data() + pixel * 8U + 2U,
            sizeof(lit_green));
        if (static_cast<core::u32>(lit_red)
                > static_cast<core::u32>(direct_red) + 8U
            && static_cast<core::u32>(lit_red)
                > static_cast<core::u32>(lit_green) + 8U) {
            ++visibly_bounced_pixels;
        }
    }
    RENDER_CHECK(visibly_bounced_pixels >= 8U);

    // Each output node renders only its authored canvas slice. The light-space
    // fit and caster set are full-scene inputs, so two adjacent nodes must
    // reproduce the exact bytes of the corresponding full-frame crop.
    constexpr core::u32 half_width = extent / 2U;
    const Capture left = capture(
        occluded, scene::CanvasRect{0U, 0U, half_width, extent},
        half_width, extent, 1.0F);
    const Capture right = capture(
        occluded,
        scene::CanvasRect{half_width, 0U, half_width, extent},
        half_width, extent, 1.0F);
    RENDER_CHECK(left.stats.directional_shadow_casters == 2U);
    RENDER_CHECK(right.stats.directional_shadow_casters == 2U);
    for (core::u32 row = 0U; row < extent; ++row) {
        const std::size_t full_row =
            static_cast<std::size_t>(row) * extent * 8U;
        const std::size_t half_row =
            static_cast<std::size_t>(row) * half_width * 8U;
        const std::size_t row_bytes =
            static_cast<std::size_t>(half_width) * 8U;
        RENDER_CHECK(std::equal(
            left.color.begin() + static_cast<std::ptrdiff_t>(half_row),
            left.color.begin()
                + static_cast<std::ptrdiff_t>(half_row + row_bytes),
            shadowed.color.begin()
                + static_cast<std::ptrdiff_t>(full_row)));
        RENDER_CHECK(std::equal(
            right.color.begin() + static_cast<std::ptrdiff_t>(half_row),
            right.color.begin()
                + static_cast<std::ptrdiff_t>(half_row + row_bytes),
            shadowed.color.begin()
                + static_cast<std::ptrdiff_t>(full_row + row_bytes)));
    }
}

void native_shadow_cascades_are_authored_and_slice_deterministic() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    config.debug = true;
    config.frames_in_flight = 2U;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();

    // A long floor the camera looks down, so the near cascades cover ground
    // the whole-scene fit resolves coarsely. That is the case cascades exist
    // for, and the case a slice-dependent fit would break.
    constexpr core::u32 extent = 96U;
    scene::Scene value{};
    value.id = scene::SceneId{57U};
    value.document_version = doc::Version{1U};
    value.canvas = scene::Canvas{extent, extent};
    value.camera.eye = scene::Vec3{0.0F, 1.6F, 5.0F};
    value.camera.target = scene::Vec3{0.0F, 0.4F, -14.0F};
    value.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    value.camera.field_of_view = 0.16F;
    value.camera.near_plane = 0.1F;
    value.camera.far_plane = 200.0F;
    value.environment.ambient = 0.05F;
    value.environment.sky_color = scene::Vec3{1.0F, 1.0F, 1.0F};
    value.environment.ground_color = scene::Vec3{1.0F, 1.0F, 1.0F};

    scene::MeshInstance floor{};
    floor.source = doc::NodeId{601U};
    floor.mesh = scene::MeshKey{scene::Primitive::Plane, 1U};
    floor.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, -0.5F, -6.0F}),
        scene::scaling(scene::Vec3{14.0F, 1.0F, 20.0F}));
    floor.normal = scene::normal_basis(floor.world);
    floor.material.base_color = scene::Vec3{0.75F, 0.75F, 0.75F};
    floor.material.roughness = 0.85F;
    value.instances.push_back(floor);

    for (core::u32 index = 0U; index < 3U; ++index) {
        scene::MeshInstance post{};
        post.source = doc::NodeId{602U + index};
        post.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
        post.world = scene::multiply(
            scene::translation(scene::Vec3{
                0.0F, 0.25F, 1.0F - static_cast<float>(index) * 5.0F}),
            scene::scaling(scene::Vec3{0.4F, 1.5F, 0.4F}));
        post.normal = scene::normal_basis(post.world);
        post.material.base_color = scene::Vec3{0.7F, 0.7F, 0.7F};
        post.material.roughness = 0.8F;
        value.instances.push_back(post);
    }

    scene::DirectionalLight light{};
    light.direction = scene::normalize(scene::Vec3{-0.7F, -1.0F, -0.3F});
    light.color = scene::Vec3{1.0F, 1.0F, 1.0F};
    light.intensity = 4.0F;
    value.directional_lights.push_back(light);

    struct Capture final {
        std::vector<core::u8> color;
        std::vector<core::u8> object_id;
        render::RenderStats stats{};
    };
    const auto capture = [&device](
                             const scene::Scene& source,
                             scene::CanvasRect slice,
                             core::u32 output_width,
                             core::u32 output_height,
                             core::u32 cascades) {
        render::RendererLimits limits{};
        limits.max_instances = 16U;
        limits.max_vertices = 4'096U;
        limits.max_indices = 12'288U;
        limits.max_lights = 4U;
        limits.directional_shadow_resolution = 256U;
        scene::Scene authored = source;
        authored.renderer_settings.shadow_cascades = cascades;
        authored.renderer_settings.gi_strength = 0.0F;
        auto created = render::SceneRenderer::create(
            device, output_width, output_height, limits,
            render::SceneRendererMode::AdvancedLinear);
        RENDER_CHECK(created);
        auto renderer = std::move(created).value();
        RENDER_CHECK(renderer.shadow_cascades_available()
                     == scene::max_shadow_cascades);

        auto color = device.create_readback(
            output_width * output_height * 8U);
        auto object_id = device.create_readback(
            output_width * output_height * 4U);
        RENDER_CHECK(color && object_id);
        auto acquired = device.acquire_commands();
        RENDER_CHECK(acquired);
        auto commands = std::move(acquired).value();
        RENDER_CHECK(renderer.render(commands, authored, slice));
        Capture result{};
        result.stats = renderer.stats();
        RENDER_CHECK(commands.download(renderer.target(), color.value()));
        RENDER_CHECK(commands.download(
            renderer.object_id_target(), object_id.value()));
        auto submitted = device.submit(std::move(commands));
        RENDER_CHECK(submitted);
        RENDER_CHECK(submitted.value().wait());
        const auto color_bytes = color.value().map();
        const auto id_bytes = object_id.value().map();
        result.color.assign(color_bytes.begin(), color_bytes.end());
        result.object_id.assign(id_bytes.begin(), id_bytes.end());
        color.value().unmap();
        object_id.value().unmap();
        return result;
    };

    const scene::CanvasRect canvas_slice = scene::whole(value.canvas);
    const Capture unshadowed =
        capture(value, canvas_slice, extent, extent, 0U);
    const Capture single = capture(value, canvas_slice, extent, extent, 1U);
    const Capture cascaded = capture(value, canvas_slice, extent, extent, 3U);

    // Zero cascades is an authored choice, not a degraded fallback: no shadow
    // pass runs and the stats say so rather than reporting a rendered map.
    RENDER_CHECK(!unshadowed.stats.directional_shadow_rendered);
    RENDER_CHECK(unshadowed.stats.directional_shadow_cascades == 0U);
    RENDER_CHECK(single.stats.directional_shadow_rendered);
    RENDER_CHECK(single.stats.directional_shadow_cascades == 1U);
    RENDER_CHECK(cascaded.stats.directional_shadow_rendered);
    RENDER_CHECK(cascaded.stats.directional_shadow_cascades == 3U);
    RENDER_CHECK(single.stats.directional_shadow_casters
                 == cascaded.stats.directional_shadow_casters);

    const auto hashed_object_id = [](core::u64 source) noexcept {
        core::u32 result = static_cast<core::u32>(source)
            ^ static_cast<core::u32>(source >> 32U);
        result ^= result >> 16U;
        result *= 0x7FEB352DU;
        result ^= result >> 15U;
        result *= 0x846CA68BU;
        result ^= result >> 16U;
        return result == 0U ? 1U : result;
    };
    const core::u32 floor_object_id =
        hashed_object_id(floor.source.value());

    const auto shadowed_floor_pixels = [&](const Capture& lit) {
        std::size_t count{};
        for (std::size_t pixel = 0U;
             pixel < static_cast<std::size_t>(extent) * extent; ++pixel) {
            core::u32 lit_id{};
            core::u32 open_id{};
            std::memcpy(&lit_id, lit.object_id.data() + pixel * 4U,
                        sizeof(lit_id));
            std::memcpy(&open_id, unshadowed.object_id.data() + pixel * 4U,
                        sizeof(open_id));
            if (lit_id != floor_object_id || lit_id != open_id) {
                continue;
            }
            core::u16 lit_red{};
            core::u16 open_red{};
            std::memcpy(&lit_red, lit.color.data() + pixel * 8U,
                        sizeof(lit_red));
            std::memcpy(&open_red, unshadowed.color.data() + pixel * 8U,
                        sizeof(open_red));
            if (static_cast<core::u32>(lit_red) + 128U
                < static_cast<core::u32>(open_red)) {
                ++count;
            }
        }
        return count;
    };
    RENDER_CHECK(shadowed_floor_pixels(single) >= 8U);
    RENDER_CHECK(shadowed_floor_pixels(cascaded) >= 8U);
    // Nearer cascades resolve the same casters over a smaller area, so the
    // cascaded render must actually differ from the single whole-scene map.
    RENDER_CHECK(cascaded.color != single.color);

    // Bias is expressed in shadow texels, so a near cascade's much smaller
    // texel does not inherit an offset sized for the whole-scene map. The
    // guard for the other direction is that a floor with nothing above it must
    // not shadow itself at any cascade count.
    scene::Scene bare = value;
    bare.instances.resize(1U);
    const Capture bare_open = capture(bare, canvas_slice, extent, extent, 0U);
    for (core::u32 cascades = 1U; cascades <= scene::max_shadow_cascades;
         ++cascades) {
        const Capture bare_lit =
            capture(bare, canvas_slice, extent, extent, cascades);
        RENDER_CHECK(bare_lit.stats.directional_shadow_cascades == cascades);
        std::size_t self_shadowed{};
        for (std::size_t pixel = 0U;
             pixel < static_cast<std::size_t>(extent) * extent; ++pixel) {
            core::u32 lit_id{};
            core::u32 open_id{};
            std::memcpy(&lit_id, bare_lit.object_id.data() + pixel * 4U,
                        sizeof(lit_id));
            std::memcpy(&open_id, bare_open.object_id.data() + pixel * 4U,
                        sizeof(open_id));
            if (lit_id != floor_object_id || lit_id != open_id) {
                continue;
            }
            core::u16 lit_red{};
            core::u16 open_red{};
            std::memcpy(&lit_red, bare_lit.color.data() + pixel * 8U,
                        sizeof(lit_red));
            std::memcpy(&open_red, bare_open.color.data() + pixel * 8U,
                        sizeof(open_red));
            if (static_cast<core::u32>(lit_red) + 128U
                < static_cast<core::u32>(open_red)) {
                ++self_shadowed;
            }
        }
        RENDER_CHECK(self_shadowed == 0U);
    }

    // Every cascade is fitted from the authored full-canvas camera and the
    // complete caster set, so a node rendering half the canvas must reproduce
    // the exact bytes of that half of the full-canvas render.
    constexpr core::u32 half_width = extent / 2U;
    const Capture left = capture(
        value, scene::CanvasRect{0U, 0U, half_width, extent}, half_width,
        extent, 3U);
    const Capture right = capture(
        value, scene::CanvasRect{half_width, 0U, half_width, extent},
        half_width, extent, 3U);
    RENDER_CHECK(left.stats.directional_shadow_cascades == 3U);
    RENDER_CHECK(right.stats.directional_shadow_cascades == 3U);
    for (core::u32 row = 0U; row < extent; ++row) {
        const std::size_t full_row =
            static_cast<std::size_t>(row) * extent * 8U;
        const std::size_t half_row =
            static_cast<std::size_t>(row) * half_width * 8U;
        const std::size_t row_bytes =
            static_cast<std::size_t>(half_width) * 8U;
        RENDER_CHECK(std::equal(
            left.color.begin() + static_cast<std::ptrdiff_t>(half_row),
            left.color.begin()
                + static_cast<std::ptrdiff_t>(half_row + row_bytes),
            cascaded.color.begin()
                + static_cast<std::ptrdiff_t>(full_row)));
        RENDER_CHECK(std::equal(
            right.color.begin() + static_cast<std::ptrdiff_t>(half_row),
            right.color.begin()
                + static_cast<std::ptrdiff_t>(half_row + row_bytes),
            cascaded.color.begin()
                + static_cast<std::ptrdiff_t>(full_row + row_bytes)));
    }
}

void a_batch_heavy_advanced_frame_fits_the_fixed_descriptor_budget() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    config.debug = true;
    config.frames_in_flight = 2U;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();

    // Composited layers are one pass each, and every lit pass binds the five
    // material maps plus the cascade and reflective-shadow-map set. A show
    // with a few hundred of them must still fit the shader-visible sampler
    // heap, which D3D12 caps at 2,048 descriptors for the whole recording.
    constexpr core::u32 extent = 64U;
    constexpr core::u32 layers = 240U;
    scene::Scene value{};
    value.id = scene::SceneId{58U};
    value.document_version = doc::Version{1U};
    value.canvas = scene::Canvas{extent, extent};
    value.camera.eye = scene::Vec3{0.0F, 1.0F, 6.0F};
    value.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    value.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    value.environment.ambient = 0.2F;
    value.renderer_settings.shadow_cascades = scene::max_shadow_cascades;
    value.renderer_settings.gi_strength = 1.0F;

    for (core::u32 index = 0U; index < layers; ++index) {
        scene::MeshInstance pane{};
        pane.source = doc::NodeId{700U + index};
        pane.mesh = scene::MeshKey{scene::Primitive::Plane, 1U};
        pane.world = scene::translation(scene::Vec3{
            0.0F, 0.0F, -static_cast<float>(index) * 0.01F});
        pane.normal = scene::normal_basis(pane.world);
        pane.material.base_color = scene::Vec3{0.6F, 0.6F, 0.6F};
        // Below one, so each pane is a destination-reading layer with its own
        // pass rather than one batch of the shared opaque draw.
        pane.style.opacity = 0.5F;
        value.instances.push_back(pane);
    }

    // One opaque caster, so the cascade passes run and the lit pass binds the
    // complete cascade set rather than an unused one.
    scene::MeshInstance caster{};
    caster.source = doc::NodeId{699U};
    caster.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    caster.world = scene::translation(scene::Vec3{1.5F, 0.5F, 0.0F});
    caster.normal = scene::normal_basis(caster.world);
    caster.material.base_color = scene::Vec3{0.8F, 0.8F, 0.8F};
    value.instances.push_back(caster);

    scene::DirectionalLight light{};
    light.direction = scene::normalize(scene::Vec3{-0.4F, -1.0F, -0.4F});
    light.color = scene::Vec3{1.0F, 1.0F, 1.0F};
    light.intensity = 2.0F;
    value.directional_lights.push_back(light);

    render::RendererLimits limits{};
    limits.max_instances = layers + 8U;
    limits.max_vertices = 8'192U;
    limits.max_indices = 24'576U;
    limits.max_lights = 4U;
    limits.directional_shadow_resolution = 256U;
    auto created = render::SceneRenderer::create(
        device, extent, extent, limits,
        render::SceneRendererMode::AdvancedLinear);
    RENDER_CHECK(created);
    auto renderer = std::move(created).value();

    auto acquired = device.acquire_commands();
    RENDER_CHECK(acquired);
    auto commands = std::move(acquired).value();
    RENDER_CHECK(renderer.render(commands, value, scene::whole(value.canvas)));
    const render::RenderStats stats = renderer.stats();
    auto submitted = device.submit(std::move(commands));
    RENDER_CHECK(submitted);
    RENDER_CHECK(submitted.value().wait());

    // Nothing was dropped to make it fit: the frame drew every authored layer
    // with the complete cascade set.
    RENDER_CHECK(stats.dropped_instances == 0U);
    // Every layer drew its lit pass and its auxiliary replay, which this
    // device writes as one multiple-target pass.
    RENDER_CHECK(stats.graph.passes > 2U * layers);
    RENDER_CHECK(stats.graph.segments > 1U);
    RENDER_CHECK(stats.graph.refusals == 0U);
    RENDER_CHECK(stats.directional_shadow_rendered);
    RENDER_CHECK(stats.directional_shadow_cascades
                 == scene::max_shadow_cascades);
}

// Temporal antialiasing, on the GPU, against the contract that motivates it.
//
// The properties here are not image-quality ones. A repeated show frame has to
// come back byte for byte, or a paused timeline drifts while a running one
// does not; the jitter has to actually move the projection, or the pass blends
// identical samples and buys smear for nothing; and a surface that loses its
// history has to say so, because a node that restarts while its neighbour
// accumulates differs from it along the edge they share.
void temporal_antialiasing_holds_repeats_and_reports_restarts() {
    if (!render::HybridSceneRenderer::shaders_available()
        || !render::TemporalResolve::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()
        || !device.capabilities().compute) {
        return;
    }

    constexpr core::u32 extent = 64U;
    scene::Scene value{};
    value.id = scene::SceneId{91U};
    value.document_version = doc::Version{3U};
    value.camera_source = doc::NodeId{90U};
    value.canvas = scene::Canvas{extent, extent};
    value.camera.eye = scene::Vec3{0.0F, 0.0F, 3.0F};
    value.camera.target = scene::Vec3{};
    value.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    value.camera.field_of_view = 0.125F;
    value.camera.near_plane = 0.1F;
    value.camera.far_plane = 100.0F;
    value.environment.ambient = 1.0F;
    value.environment.sky_color = scene::Vec3{0.05F, 0.07F, 0.1F};
    value.environment.ground_color = scene::Vec3{0.02F, 0.02F, 0.02F};
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.required = true;
    value.renderer_settings.shadow_cascades = 1U;
    // A box rotated off axis, so its silhouette crosses pixels diagonally and
    // there is aliasing for the pass to have an opinion about.
    scene::MeshInstance box{};
    box.source = doc::NodeId{78U};
    box.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    box.world = scene::rotation(0.05F, 0.0F, 0.0F);
    box.normal = scene::normal_basis(box.world);
    box.material.base_color = scene::Vec3{0.9F, 0.4F, 0.2F};
    box.material.roughness = 0.5F;
    value.instances.push_back(box);
    scene::DirectionalLight light{};
    light.direction = scene::Vec3{-0.3F, -0.5F, -0.8F};
    light.color = scene::Vec3{1.0F, 1.0F, 1.0F};
    light.intensity = 3.0F;
    value.directional_lights.push_back(light);

    render::RendererLimits limits{};
    limits.max_instances = 8U;
    limits.max_vertices = 2048U;
    limits.max_indices = 6144U;
    limits.max_lights = 4U;
    limits.directional_shadow_resolution = 256U;
    limits.max_history_slots = 4U;
    // One target, deliberately: two surfaces then have to take it from each
    // other, which is what makes the restart counter observable.
    limits.max_temporal_history_targets = 1U;

    auto created = render::HybridSceneRenderer::create(
        device, extent, extent, limits);
    RENDER_CHECK(created);
    auto renderer = std::move(created).value();

    const auto request_for = [&](const scene::Scene& scn,
                                 core::u64 surface_id) {
        render::RenderFrameRequest request{};
        request.scene = &scn;
        request.slice = scene::whole(scn.canvas);
        request.output_width = extent;
        request.output_height = extent;
        request.history.scene = scn.id;
        request.history.document_version = scn.document_version;
        request.history.surface = render::HistorySurface::Offscreen;
        request.history.surface_id = surface_id;
        request.history.canvas = scn.canvas;
        request.history.slice = request.slice;
        request.history.width = extent;
        request.history.height = extent;
        request.history.camera_source = scn.camera_source;
        request.history.camera_digest =
            render::camera_history_digest(scn.camera);
        request.history.profile_digest =
            render::renderer_profile_digest(scn.renderer_settings);
        return request;
    };

    // Renders one frame at `show_frame` and reads the published colour back.
    // The published colour is the point: for an Advance it is the resolve, for
    // a Hold it is the previous resolve, and a caller must not have to know
    // which to read the right texture.
    const auto capture = [&](const scene::Scene& scn, core::u64 show_frame,
                             core::u64 surface_id) {
        scene::Scene framed = scn;
        framed.show_frame = show_frame;
        auto readback = device.create_readback(extent * extent * 8U);
        RENDER_CHECK(readback);
        auto acquired = device.acquire_commands();
        RENDER_CHECK(acquired);
        auto commands = std::move(acquired).value();
        auto products =
            renderer.render_frame(commands, request_for(framed, surface_id));
        RENDER_CHECK(products);
        RENDER_CHECK(products.value().color != nullptr);
        RENDER_CHECK(
            commands.download(*products.value().color, readback.value()));
        auto submitted = device.submit(std::move(commands));
        RENDER_CHECK(submitted);
        RENDER_CHECK(std::move(submitted).value().wait());
        const auto pixels = readback.value().map();
        RENDER_CHECK(pixels.size() >= extent * extent * 8U);
        std::vector<core::u8> image(pixels.begin(),
                                    pixels.begin() + extent * extent * 8U);
        readback.value().unmap();
        return image;
    };

    // Authoring temporal antialiasing is now a capability, not a refusal.
    scene::Scene temporal = value;
    temporal.renderer_settings.taau = true;
    temporal.renderer_settings.taa_sequence_length = 8U;
    RENDER_CHECK(scene::antialiasing_for(temporal.renderer_settings)
                 == scene::AntialiasingMode::Temporal);

    const std::vector<core::u8> first = capture(temporal, 1U, 1U);
    const std::vector<core::u8> second = capture(temporal, 2U, 1U);
    RENDER_CHECK(renderer.diagnostics().temporal_refusals == 0U);

    // The jitter moved the projection. Without it the second frame of a static
    // scene would be the first frame again and the whole pass would be a blur
    // with no antialiasing in it.
    RENDER_CHECK(first != second);

    // A repeated show frame is a hold: the previous resolve, unchanged, twice.
    // This is the contract the temporal state machine exists to enforce, and
    // it is the one a paused timeline exercises constantly.
    const std::vector<core::u8> held = capture(temporal, 2U, 1U);
    const std::vector<core::u8> held_again = capture(temporal, 2U, 1U);
    RENDER_CHECK(held == second);
    RENDER_CHECK(held == held_again);

    // A second surface takes the only history target, and the first then finds
    // its history gone on a frame the state machine still calls an Advance.
    //
    // The show frames matter here. A surface whose frame number jumps is a
    // Reset and starts over legitimately; a restart is the other thing - the
    // CPU decision said Advance and the GPU had nothing to advance from - and
    // only a contiguous frame on the evicted surface reaches it. That is the
    // case worth counting, because it is the one where a node quietly differs
    // from a neighbour that kept its target.
    const core::u64 restarts_before =
        renderer.diagnostics().temporal_restarts;
    static_cast<void>(capture(temporal, 3U, 2U));
    static_cast<void>(capture(temporal, 3U, 1U));
    RENDER_CHECK(renderer.diagnostics().temporal_restarts > restarts_before);

    // With temporal antialiasing off the same two show frames are identical,
    // because nothing jitters and nothing accumulates. This is the promise
    // that the feature costs exactly nothing when it is not authored.
    scene::Scene plain = value;
    plain.renderer_settings.taau = false;
    const std::vector<core::u8> plain_first = capture(plain, 11U, 3U);
    const std::vector<core::u8> plain_second = capture(plain, 12U, 3U);
    RENDER_CHECK(plain_first == plain_second);

    // The spatial alternative holds no state, so it is a pure function of the
    // frame it was handed: two different show frames of a static scene come
    // back identical, where the temporal pass made them differ. That is the
    // trade stated as an assertion - this one cannot ghost because it cannot
    // remember, and it cannot resolve thin geometry for the same reason.
    scene::Scene spatial = value;
    spatial.renderer_settings.taau = false;
    spatial.renderer_settings.spatial_antialiasing = true;
    RENDER_CHECK(scene::antialiasing_for(spatial.renderer_settings)
                 == scene::AntialiasingMode::Spatial);
    const std::vector<core::u8> spatial_first = capture(spatial, 21U, 4U);
    const std::vector<core::u8> spatial_second = capture(spatial, 22U, 4U);
    RENDER_CHECK(spatial_first == spatial_second);
    RENDER_CHECK(renderer.diagnostics().spatial_passes >= 2U);

    // And it changed something. A pass that ran and returned its input would
    // satisfy every assertion above.
    RENDER_CHECK(spatial_first != plain_first);

    // Authoring both is not two passes. Temporal wins, and the single place
    // that decides says so.
    scene::Scene both = value;
    both.renderer_settings.taau = true;
    both.renderer_settings.spatial_antialiasing = true;
    RENDER_CHECK(scene::antialiasing_for(both.renderer_settings)
                 == scene::AntialiasingMode::Temporal);
    const core::u64 spatial_passes_before =
        renderer.diagnostics().spatial_passes;
    static_cast<void>(capture(both, 31U, 5U));
    RENDER_CHECK(renderer.diagnostics().spatial_passes
                 == spatial_passes_before);
}

// Reads one binary16 lane out of a linear colour readback.
//
// Summing the raw bytes of a half float compares exponent patterns rather than
// light, which is a mistake this suite has made before and which passes for a
// while because brighter images usually do have larger bytes.
[[nodiscard]] float half_lane(const std::vector<core::u8>& image,
                              std::size_t pixel, std::size_t channel) noexcept {
    core::u16 bits{};
    std::memcpy(&bits, image.data() + pixel * 8U + channel * 2U, sizeof(bits));
    const core::u32 sign = static_cast<core::u32>(bits & 0x8000U) << 16U;
    const core::u32 exponent = (bits >> 10U) & 0x1FU;
    const core::u32 mantissa = bits & 0x3FFU;
    core::u32 result{};
    if (exponent == 0U) {
        result = sign;
    } else if (exponent == 31U) {
        result = sign | 0x7F80'0000U | (mantissa << 13U);
    } else {
        result = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    }
    float value{};
    std::memcpy(&value, &result, sizeof(value));
    return value;
}

// A spot light does not shine through a wall.
//
// This is the whole of Part B stated as one assertion. Sixty four practicals
// were allowed on a set and only the first directional light cast anything, so
// every lamp, window and screen lit straight through the geometry in front of
// it. On an interior set - which is what practicals light - that is more
// obviously wrong than almost anything else.
//
// The comparison against the same light with `casts_shadow` off is the
// load-bearing half. Without it, a floor that happens to be dark behind the
// wall would pass, and the wall's own cone falloff would be indistinguishable
// from a shadow.
void a_spot_does_not_shine_through_a_wall() {
    if (!render::HybridSceneRenderer::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()) {
        return;
    }

    constexpr core::u32 extent = 128U;
    scene::Scene value{};
    value.id = scene::SceneId{141U};
    value.document_version = doc::Version{1U};
    value.camera_source = doc::NodeId{140U};
    value.canvas = scene::Canvas{extent, extent};
    // Straight down at the floor. The lamp is above the middle and a low wall
    // stands between it and the far half, so the far half should be dark.
    value.camera.eye = scene::Vec3{0.0F, 4.0F, 0.0F};
    value.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    value.camera.up = scene::Vec3{0.0F, 0.0F, -1.0F};
    value.camera.field_of_view = 0.125F;
    value.camera.near_plane = 0.1F;
    value.camera.far_plane = 100.0F;
    // No ambient: every photon comes from the one spot, so anything dark is
    // dark because it was shadowed.
    value.environment.ambient = 0.0F;
    value.environment.sky_color = scene::Vec3{};
    value.environment.ground_color = scene::Vec3{};
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.required = true;
    value.renderer_settings.shadow_cascades = 0U;
    value.renderer_settings.gi_strength = 0.0F;

    scene::MeshInstance floor{};
    floor.source = doc::NodeId{150U};
    floor.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    floor.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, -0.05F, 0.0F}),
        scene::scaling(scene::Vec3{12.0F, 0.1F, 12.0F}));
    floor.normal = scene::normal_basis(floor.world);
    floor.material.base_color = scene::Vec3{0.9F, 0.9F, 0.9F};
    floor.material.roughness = 1.0F;
    value.instances.push_back(floor);

    // A blade standing on the floor, running left to right across the frame,
    // between the lamp and the far half.
    scene::MeshInstance wall{};
    wall.source = doc::NodeId{151U};
    wall.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    wall.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.4F, -0.35F}),
        scene::scaling(scene::Vec3{4.0F, 0.8F, 0.08F}));
    wall.normal = scene::normal_basis(wall.world);
    wall.material.base_color = scene::Vec3{0.9F, 0.9F, 0.9F};
    wall.material.roughness = 1.0F;
    value.instances.push_back(wall);

    scene::PointLight lamp{};
    lamp.source = doc::NodeId{152U};
    lamp.kind = scene::LocalLightKind::Spot;
    lamp.position = scene::Vec3{0.0F, 1.5F, 0.9F};
    lamp.color = scene::Vec3{1.0F, 1.0F, 1.0F};
    lamp.intensity = 40.0F;
    lamp.range = 12.0F;
    lamp.direction = scene::normalize(scene::Vec3{0.0F, -1.0F, -1.0F},
                                      scene::Vec3{0.0F, -1.0F, 0.0F});
    lamp.inner_cone = 0.09F;
    lamp.outer_cone = 0.12F;
    lamp.shadow_tier = scene::ShadowTier::Medium;

    render::RendererLimits limits{};
    limits.max_instances = 8U;
    limits.max_vertices = 4096U;
    limits.max_indices = 12288U;
    limits.max_lights = 4U;
    limits.directional_shadow_resolution = 256U;
    limits.max_history_slots = 2U;
    limits.shadow_atlas.extent = 1024U;

    auto created = render::HybridSceneRenderer::create(
        device, extent, extent, limits);
    RENDER_CHECK(created);
    auto renderer = std::move(created).value();

    const auto capture = [&](bool casts) {
        scene::Scene framed = value;
        scene::PointLight light = lamp;
        light.casts_shadow = casts;
        framed.point_lights.push_back(light);

        render::RenderFrameRequest request{};
        request.scene = &framed;
        request.slice = scene::whole(framed.canvas);
        request.output_width = extent;
        request.output_height = extent;
        request.history.scene = framed.id;
        request.history.document_version = framed.document_version;
        request.history.surface = render::HistorySurface::Offscreen;
        request.history.surface_id = casts ? 1U : 2U;
        request.history.canvas = framed.canvas;
        request.history.slice = request.slice;
        request.history.width = extent;
        request.history.height = extent;
        request.history.camera_source = framed.camera_source;
        request.history.camera_digest =
            render::camera_history_digest(framed.camera);
        request.history.profile_digest =
            render::renderer_profile_digest(framed.renderer_settings);

        auto readback = device.create_readback(extent * extent * 8U);
        RENDER_CHECK(readback);
        auto acquired = device.acquire_commands();
        RENDER_CHECK(acquired);
        auto commands = std::move(acquired).value();
        auto products = renderer.render_frame(commands, request);
        RENDER_CHECK(products);
        RENDER_CHECK(commands.download(*products.value().color,
                                       readback.value()));
        auto submitted = device.submit(std::move(commands));
        RENDER_CHECK(submitted);
        RENDER_CHECK(std::move(submitted).value().wait());
        const auto pixels = readback.value().map();
        RENDER_CHECK(pixels.size() >= extent * extent * 8U);
        std::vector<core::u8> image(pixels.begin(),
                                    pixels.begin() + extent * extent * 8U);
        readback.value().unmap();
        return image;
    };

    const std::vector<core::u8> shadowed = capture(true);
    // Read between the two renders: the second authors no caster, so asking
    // afterwards would report the frame that deliberately allocated nothing.
    RENDER_CHECK(renderer.stats().shadow_tiles == 1U);
    RENDER_CHECK(renderer.stats().shadow_lights_refused == 0U);
    const std::vector<core::u8> unshadowed = capture(false);
    RENDER_CHECK(renderer.stats().shadow_tiles == 0U);

    // Bands rather than single pixels: one pixel could sit on the blade's own
    // silhouette and prove nothing either way.
    const auto band = [&](const std::vector<core::u8>& image,
                          core::u32 first_row, core::u32 last_row) {
        double total = 0.0;
        std::size_t counted = 0U;
        for (core::u32 y = first_row; y <= last_row; ++y) {
            for (core::u32 x = extent / 3U; x < (extent * 2U) / 3U; ++x) {
                total += static_cast<double>(half_lane(
                    image, static_cast<std::size_t>(y) * extent + x, 1U));
                ++counted;
            }
        }
        return counted == 0U ? 0.0 : total / static_cast<double>(counted);
    };

    // Rows nearer the camera's top edge are the far side of the blade.
    const double behind_shadowed = band(shadowed, 16U, 30U);
    const double behind_unshadowed = band(unshadowed, 16U, 30U);
    // In front of the blade, where the lamp has line of sight either way.
    const double front_shadowed = band(shadowed, extent - 40U, extent - 26U);
    const double front_unshadowed =
        band(unshadowed, extent - 40U, extent - 26U);

    // The far side went dark.
    RENDER_CHECK(behind_shadowed < behind_unshadowed * 0.5);
    // And the near side did not, so this is an occluder rather than the light
    // simply being switched off.
    RENDER_CHECK(front_shadowed > front_unshadowed * 0.8);
    RENDER_CHECK(front_shadowed > 0.02);
}

// Two renderers looking from different places agree on every atlas
// assignment, and a floor under a spot does not shadow itself.
//
// The first half is the multi-node contract at the level a node actually
// experiences it: Program and Preview are two cameras on one scene, and if the
// atlas depended on either of them the same light would be shadowed at two
// resolutions on one wall. The allocator is camera-free by construction and
// the unit tests prove that directly; this proves the renderer did not
// reintroduce a camera on the way.
//
// The second half is the acne guard. A surface lit at a grazing angle stores a
// depth that is slightly nearer than the depth it later tests against, so
// without a slope-scaled bias every lit floor shadows itself in stripes.
void atlas_assignment_ignores_the_camera_and_a_floor_does_not_self_shadow() {
    if (!render::HybridSceneRenderer::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()) {
        return;
    }

    constexpr core::u32 extent = 96U;
    scene::Scene value{};
    value.id = scene::SceneId{161U};
    value.document_version = doc::Version{1U};
    value.camera_source = doc::NodeId{160U};
    value.canvas = scene::Canvas{extent, extent};
    value.camera.eye = scene::Vec3{0.0F, 3.0F, 0.0F};
    value.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    value.camera.up = scene::Vec3{0.0F, 0.0F, -1.0F};
    value.camera.field_of_view = 0.125F;
    value.camera.near_plane = 0.1F;
    value.camera.far_plane = 100.0F;
    value.environment.ambient = 0.0F;
    value.environment.sky_color = scene::Vec3{};
    value.environment.ground_color = scene::Vec3{};
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.required = true;
    value.renderer_settings.shadow_cascades = 0U;
    value.renderer_settings.gi_strength = 0.0F;

    scene::MeshInstance floor{};
    floor.source = doc::NodeId{170U};
    floor.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    floor.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, -0.05F, 0.0F}),
        scene::scaling(scene::Vec3{12.0F, 0.1F, 12.0F}));
    floor.normal = scene::normal_basis(floor.world);
    floor.material.base_color = scene::Vec3{0.9F, 0.9F, 0.9F};
    floor.material.roughness = 1.0F;
    value.instances.push_back(floor);

    // Three casters, deliberately out of node-id order in the vector.
    for (const core::u64 id : {31U, 7U, 19U}) {
        scene::PointLight lamp{};
        lamp.source = doc::NodeId{id};
        lamp.kind = scene::LocalLightKind::Spot;
        lamp.position = scene::Vec3{
            static_cast<float>(id) * 0.05F - 1.0F, 1.6F, 0.0F};
        lamp.color = scene::Vec3{1.0F, 1.0F, 1.0F};
        lamp.intensity = 30.0F;
        lamp.range = 10.0F;
        lamp.direction = scene::Vec3{0.0F, -1.0F, 0.0F};
        lamp.inner_cone = 0.05F;
        lamp.outer_cone = 0.08F;
        lamp.casts_shadow = true;
        // Medium rather than Large: a large tile is the whole of a
        // 1024 atlas, and the other two would then be legitimately refused -
        // which is correct behaviour and a different test.
        lamp.shadow_tier = id == 7U ? scene::ShadowTier::Medium
                                    : scene::ShadowTier::Small;
        value.point_lights.push_back(lamp);
    }

    render::RendererLimits limits{};
    limits.max_instances = 8U;
    limits.max_vertices = 4096U;
    limits.max_indices = 12288U;
    limits.max_lights = 8U;
    limits.directional_shadow_resolution = 256U;
    limits.max_history_slots = 2U;
    limits.shadow_atlas.extent = 1024U;

    // Two renderers, two cameras, one scene.
    const auto render_from = [&](scene::Vec3 eye, core::u64 surface) {
        scene::Scene framed = value;
        framed.camera.eye = eye;
        auto created = render::HybridSceneRenderer::create(
            device, extent, extent, limits);
        RENDER_CHECK(created);
        auto renderer = std::move(created).value();

        render::RenderFrameRequest request{};
        request.scene = &framed;
        request.slice = scene::whole(framed.canvas);
        request.output_width = extent;
        request.output_height = extent;
        request.history.scene = framed.id;
        request.history.document_version = framed.document_version;
        request.history.surface = render::HistorySurface::Offscreen;
        request.history.surface_id = surface;
        request.history.canvas = framed.canvas;
        request.history.slice = request.slice;
        request.history.width = extent;
        request.history.height = extent;
        request.history.camera_source = framed.camera_source;
        request.history.camera_digest =
            render::camera_history_digest(framed.camera);
        request.history.profile_digest =
            render::renderer_profile_digest(framed.renderer_settings);

        auto readback = device.create_readback(extent * extent * 8U);
        RENDER_CHECK(readback);
        auto acquired = device.acquire_commands();
        RENDER_CHECK(acquired);
        auto commands = std::move(acquired).value();
        auto products = renderer.render_frame(commands, request);
        RENDER_CHECK(products);
        RENDER_CHECK(commands.download(*products.value().color,
                                       readback.value()));
        auto submitted = device.submit(std::move(commands));
        RENDER_CHECK(submitted);
        RENDER_CHECK(std::move(submitted).value().wait());
        const auto pixels = readback.value().map();
        RENDER_CHECK(pixels.size() >= extent * extent * 8U);
        std::vector<core::u8> image(pixels.begin(),
                                    pixels.begin() + extent * extent * 8U);
        readback.value().unmap();
        return std::pair{image, renderer.stats()};
    };

    const auto overhead = render_from(scene::Vec3{0.0F, 3.0F, 0.0F}, 1U);
    const auto oblique = render_from(scene::Vec3{2.2F, 2.4F, 1.6F}, 2U);

    // Same tiles, same count, same refusals - from two different viewpoints.
    RENDER_CHECK(overhead.second.shadow_tiles == 3U);
    RENDER_CHECK(oblique.second.shadow_tiles == 3U);
    RENDER_CHECK(overhead.second.shadow_lights_refused == 0U);
    RENDER_CHECK(oblique.second.shadow_lights_refused == 0U);

    // A floor with nothing above it is lit, not striped. Averaging would hide
    // acne, so this counts pixels: under the cones a clear majority must be
    // lit, and self-shadowing shows up as a scattering of dark ones.
    std::size_t lit = 0U;
    std::size_t dark = 0U;
    for (core::u32 y = extent / 3U; y < (extent * 2U) / 3U; ++y) {
        for (core::u32 x = extent / 3U; x < (extent * 2U) / 3U; ++x) {
            const float green = half_lane(
                overhead.first, static_cast<std::size_t>(y) * extent + x, 1U);
            if (green > 0.02F) {
                ++lit;
            } else {
                ++dark;
            }
        }
    }
    RENDER_CHECK(lit > 0U);
    RENDER_CHECK(lit > dark * 4U);
}

// binary32 to binary16, for writing a scene-linear source by hand.
[[nodiscard]] core::u16 half_bits(float value) noexcept {
    core::u32 bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    const core::u32 sign = (bits >> 16U) & 0x8000U;
    const auto exponent = static_cast<int>((bits >> 23U) & 0xFFU) - 127;
    const core::u32 mantissa = bits & 0x007F'FFFFU;
    if (exponent >= 16) {
        return static_cast<core::u16>(sign | 0x7C00U);
    }
    if (exponent < -14) {
        return static_cast<core::u16>(sign);
    }
    return static_cast<core::u16>(
        sign | (static_cast<core::u32>(exponent + 15) << 10U)
        | (mantissa >> 13U));
}

// A bright block spreads into a halo that keeps its light and its symmetry.
//
// Radial symmetry is deliberately *not* what is asserted. A mip chain is
// block-structured by construction - that is what makes it cheap - and
// demanding a round halo would be demanding something the algorithm does not
// produce. What it does produce, and what a look depends on, is that the light
// goes somewhere rather than away, that it falls off with distance, and that
// nothing about the filter prefers one side of the image. The source here is a
// two-by-two block at the exact centre, which is symmetric under the half turn
// that also maps the chain's own grid onto itself, so the result must be too.
void a_bright_block_blooms_into_a_symmetric_halo() {
    if (!render::Bloom::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().compute
        || !device.capabilities().storage_images) {
        return;
    }

    constexpr core::u32 extent = 64U;
    constexpr float bright = 8.0F;
    std::vector<core::u8> source_bytes(
        static_cast<std::size_t>(extent) * extent * 8U, 0U);
    const auto write_texel = [&](core::u32 x, core::u32 y, float value) {
        const std::size_t texel =
            (static_cast<std::size_t>(y) * extent + x) * 8U;
        for (core::u32 channel = 0U; channel < 3U; ++channel) {
            const core::u16 half = half_bits(value);
            std::memcpy(source_bytes.data() + texel + channel * 2U, &half,
                        sizeof(half));
        }
        const core::u16 one = half_bits(1.0F);
        std::memcpy(source_bytes.data() + texel + 6U, &one, sizeof(one));
    };
    for (core::u32 y = 0U; y < extent; ++y) {
        for (core::u32 x = 0U; x < extent; ++x) {
            write_texel(x, y, 0.0F);
        }
    }
    // Centred, and two texels across so the block is symmetric under
    // `x -> extent - 1 - x` rather than merely near the middle.
    write_texel(31U, 31U, bright);
    write_texel(32U, 31U, bright);
    write_texel(31U, 32U, bright);
    write_texel(32U, 32U, bright);

    rhi::TextureDesc desc{};
    desc.width = extent;
    desc.height = extent;
    desc.format = rhi::TextureFormat::Rgba16Float;
    desc.sampled = true;
    desc.storage_read = true;
    desc.storage_write = true;
    auto created_source = device.create_texture(desc);
    check(created_source.has_value(), "source", __LINE__);
    auto source = std::move(created_source).value();

    auto created = render::Bloom::create(device, extent, extent);
    check(created.has_value(), "bloom", __LINE__);
    auto bloom = std::move(created).value();

    const scene::Canvas canvas{extent, extent};
    const auto plan = render::plan_bloom_chain(
        canvas, scene::CanvasRect{0U, 0U, extent, extent});
    check(plan.has_value(), "plan", __LINE__);
    RENDER_CHECK(plan.value().depth() > 1U);

    auto readback = device.create_readback(extent * extent * 8U);
    check(readback.has_value(), "readback", __LINE__);
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "acquire", __LINE__);
    auto commands = std::move(acquired).value();
    check(commands.upload(source, source_bytes), "upload", __LINE__);
    check(bloom.record(commands, source, plan.value()), "record", __LINE__);
    check(commands.download(bloom.target(), readback.value()), "download",
          __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "submit", __LINE__);
    check(std::move(submitted).value().wait(), "wait", __LINE__);

    std::vector<core::u8> halo;
    {
        const auto pixels = readback.value().map();
        const std::size_t bytes =
            static_cast<std::size_t>(extent) * extent * 8U;
        check(pixels.size() >= bytes, "map", __LINE__);
        halo.assign(pixels.begin(), pixels.begin() + bytes);
        readback.value().unmap();
    }
    const auto at = [&](core::u32 x, core::u32 y) {
        return static_cast<double>(half_lane(
            halo, static_cast<std::size_t>(y) * extent + x, 0U));
    };

    // The light went somewhere rather than away. Four texels of value eight
    // is thirty-two units in; the chain is averaging rather than summing its
    // levels, so what comes out is the same light spread over the frame.
    double total = 0.0;
    for (core::u32 y = 0U; y < extent; ++y) {
        for (core::u32 x = 0U; x < extent; ++x) {
            RENDER_CHECK(at(x, y) >= 0.0);
            total += at(x, y);
        }
    }
    const double emitted = 4.0 * static_cast<double>(bright);
    RENDER_CHECK(total > emitted * 0.5);
    RENDER_CHECK(total < emitted * 2.0);

    // It actually spread: the centre is no longer carrying all of it, and rows
    // ten pixels clear of the source are lit. Both rings are mirror pairs
    // about the centre, so neither is measuring the asymmetry of its own
    // position.
    //
    // Ten pixels rather than thirty: the light reaches about thirteen here,
    // which is the real span of a three-level chain on a sixty-four pixel
    // canvas and is comfortably inside the twenty-eight `bloom_chain_apron`
    // reserves for it. The apron is deliberately the conservative number - it
    // bounds the kernels rather than measuring their falloff - and this is what
    // that margin looks like from the other side.
    RENDER_CHECK(at(31U, 31U) < static_cast<double>(bright));
    double near_ring = 0.0;
    double far_ring = 0.0;
    for (core::u32 x = 0U; x < extent; ++x) {
        near_ring += at(x, 28U) + at(x, 35U);
        far_ring += at(x, 22U) + at(x, 41U);
    }
    RENDER_CHECK(near_ring > 0.0);
    RENDER_CHECK(far_ring > 0.0);
    // And it falls off, which is the difference between a bloom and a fog.
    RENDER_CHECK(near_ring > far_ring);

    // Symmetric under the half turn. Not asserted byte for byte: the mirrored
    // texel sums the same values in mirrored order, and floating-point addition
    // does not promise that lands on the same bits.
    double worst = 0.0;
    for (core::u32 y = 0U; y < extent; ++y) {
        for (core::u32 x = 0U; x < extent; ++x) {
            worst = std::max(
                worst,
                std::abs(at(x, y) - at(extent - 1U - x, extent - 1U - y)));
        }
    }
    RENDER_CHECK(worst < 1.0e-3);
}

// Authoring bloom changes the frame the renderer publishes.
//
// Embarrassingly, nothing asserted this. The two-node comparison drives a scene
// with bloom on, but everything it checks is whether two halves agree - and two
// halves of a frame with no bloom in it agree perfectly well. A pass wired to
// nothing would have sailed through every test in this file.
//
// So: the same scene twice through the whole `HybridSceneRenderer` path, once
// with the strength at zero and once high, and the two frames must differ.
void authored_bloom_changes_the_published_frame() {
    if (!render::HybridSceneRenderer::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()
        || !device.capabilities().compute
        || !device.capabilities().storage_images) {
        return;
    }

    constexpr core::u32 width = 128U;
    constexpr core::u32 height = 96U;

    scene::Scene value{};
    value.id = scene::SceneId{301U};
    value.document_version = doc::Version{1U};
    value.camera_source = doc::NodeId{300U};
    value.canvas = scene::Canvas{width, height};
    value.camera.eye = scene::Vec3{0.0F, 1.2F, 3.0F};
    value.camera.target = scene::Vec3{0.0F, 0.2F, 0.0F};
    value.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    value.camera.field_of_view = 0.125F;
    value.camera.near_plane = 0.1F;
    value.camera.far_plane = 100.0F;
    value.environment.ambient = 0.05F;
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.required = true;

    // A small, very bright emitter against a dark set - the case bloom exists
    // for, and the one where its absence is unmistakable.
    scene::MeshInstance lamp{};
    lamp.source = doc::NodeId{310U};
    lamp.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    lamp.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.6F, 0.0F}),
        scene::scaling(scene::Vec3{0.25F, 0.25F, 0.25F}));
    lamp.normal = scene::normal_basis(lamp.world);
    lamp.material.base_color = scene::Vec3{0.0F, 0.0F, 0.0F};
    lamp.material.emissive = scene::Vec3{1.0F, 0.95F, 0.85F};
    lamp.material.emissive_strength = 40.0F;
    value.instances.push_back(lamp);

    scene::MeshInstance floor{};
    floor.source = doc::NodeId{311U};
    floor.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    floor.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, -0.1F, 0.0F}),
        scene::scaling(scene::Vec3{6.0F, 0.1F, 6.0F}));
    floor.normal = scene::normal_basis(floor.world);
    floor.material.base_color = scene::Vec3{0.05F, 0.05F, 0.06F};
    floor.material.roughness = 1.0F;
    value.instances.push_back(floor);

    render::RendererLimits limits{};
    limits.max_instances = 8U;
    limits.max_vertices = 8192U;
    limits.max_indices = 24576U;
    limits.max_lights = 4U;
    limits.max_history_slots = 2U;

    const auto render_once = [&](float strength, core::u64 surface) {
        scene::Scene framed = value;
        framed.renderer_settings.bloom_strength = strength;
        framed.show_frame = 1U;

        auto created =
            render::HybridSceneRenderer::create(device, width, height, limits);
        check(created.has_value(), "renderer", __LINE__);
        auto renderer = std::move(created).value();
        auto readback = device.create_readback(width * height * 8U);
        check(readback.has_value(), "readback", __LINE__);

        render::RenderFrameRequest request{};
        request.scene = &framed;
        request.slice = scene::CanvasRect{0U, 0U, width, height};
        request.output_width = width;
        request.output_height = height;
        request.history.scene = framed.id;
        request.history.document_version = framed.document_version;
        request.history.surface = render::HistorySurface::Offscreen;
        request.history.surface_id = surface;
        request.history.canvas = framed.canvas;
        request.history.slice = request.slice;
        request.history.width = width;
        request.history.height = height;
        request.history.camera_source = framed.camera_source;
        request.history.camera_digest =
            render::camera_history_digest(framed.camera);
        request.history.profile_digest =
            render::renderer_profile_digest(framed.renderer_settings);

        auto acquired = device.acquire_commands();
        check(acquired.has_value(), "acquire", __LINE__);
        auto commands = std::move(acquired).value();
        auto rendered = renderer.render_frame(commands, request);
        check(rendered.has_value(), "render", __LINE__);
        check(commands.download(*rendered.value().color, readback.value()),
              "download", __LINE__);
        auto submitted = device.submit(std::move(commands));
        check(submitted.has_value(), "submit", __LINE__);
        check(std::move(submitted).value().wait(), "wait", __LINE__);

        std::vector<core::u8> frame;
        const auto pixels = readback.value().map();
        const std::size_t bytes =
            static_cast<std::size_t>(width) * height * 8U;
        check(pixels.size() >= bytes, "map", __LINE__);
        frame.assign(pixels.begin(), pixels.begin() + bytes);
        readback.value().unmap();
        // The chain count, so a frame that skipped bloom entirely is
        // distinguishable from one where it ran and did nothing visible.
        return std::pair<std::vector<core::u8>, core::u64>{
            std::move(frame), renderer.diagnostics().bloom_chains};
    };

    const auto without = render_once(0.0F, 1U);
    const auto with = render_once(3.0F, 2U);

    // It ran at all. Without this the comparison below could pass for the
    // wrong reason on some future refactor.
    RENDER_CHECK(without.second == 0U);
    RENDER_CHECK(with.second == 1U);

    // And it did something. Measured away from the emitter, because that is
    // where spill is the whole difference - the lamp itself is bright either
    // way.
    std::size_t brighter = 0U;
    double most = 0.0;
    for (std::size_t pixel = 0U;
         pixel < static_cast<std::size_t>(width) * height; ++pixel) {
        const double before = half_lane(without.first, pixel, 0U);
        const double after = half_lane(with.first, pixel, 0U);
        if (after > before + 1.0e-3) {
            ++brighter;
        }
        most = std::max(most, after - before);
    }
    RENDER_CHECK(brighter > 200U);
    RENDER_CHECK(most > 0.01);


}

// The renderer says which authored passes actually ran.
//
// The silent case is the one that matters: a project authors Advanced, does not
// require it, is handed Basic, and every Advanced control it owns goes quiet
// with nothing anywhere saying so. That is not hypothetical - it is how bloom
// looked for an afternoon, and how temporal antialiasing looked for far longer.
// `NotAuthored`, `BackendUnsupported` and `Active` have to be distinguishable
// from each other or the report is worth nothing.
void the_renderer_reports_which_passes_ran() {
    if (!render::HybridSceneRenderer::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()
        || !device.capabilities().compute
        || !device.capabilities().storage_images) {
        return;
    }

    constexpr core::u32 width = 128U;
    constexpr core::u32 height = 96U;

    scene::Scene value{};
    value.id = scene::SceneId{401U};
    value.document_version = doc::Version{1U};
    value.camera_source = doc::NodeId{400U};
    value.canvas = scene::Canvas{width, height};
    value.camera.eye = scene::Vec3{0.0F, 1.2F, 3.0F};
    value.camera.target = scene::Vec3{0.0F, 0.2F, 0.0F};
    value.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    value.camera.field_of_view = 0.125F;
    value.camera.near_plane = 0.1F;
    value.camera.far_plane = 100.0F;

    scene::MeshInstance block{};
    block.source = doc::NodeId{410U};
    block.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    block.world = scene::translation(scene::Vec3{0.0F, 0.3F, 0.0F});
    block.normal = scene::normal_basis(block.world);
    block.material.base_color = scene::Vec3{0.8F, 0.8F, 0.8F};
    value.instances.push_back(block);

    scene::DirectionalLight sun{};
    sun.direction = scene::Vec3{-0.4F, -0.8F, -0.45F};
    sun.color = scene::Vec3{1.0F, 1.0F, 1.0F};
    sun.intensity = 3.0F;
    value.directional_lights.push_back(sun);

    render::RendererLimits limits{};
    limits.max_instances = 4U;
    limits.max_vertices = 4096U;
    limits.max_indices = 12288U;
    limits.max_lights = 4U;
    limits.max_history_slots = 2U;

    const auto status_for = [&](scene::RendererBackend backend,
                                core::u64 surface) {
        scene::Scene framed = value;
        framed.show_frame = 1U;
        framed.renderer_settings.backend = backend;
        // Authored on both, so the only thing that changes between the two runs
        // is which backend was selected.
        framed.renderer_settings.shadow_cascades = 2U;
        framed.renderer_settings.gi_strength = 1.0F;
        framed.renderer_settings.occlusion_radius = 1.0F;
        framed.renderer_settings.bloom_strength = 0.5F;
        // Spatial deliberately left off, so one pass stays NotAuthored and the
        // three states are told apart rather than merely counted.

        auto created =
            render::HybridSceneRenderer::create(device, width, height, limits);
        check(created.has_value(), "renderer", __LINE__);
        auto renderer = std::move(created).value();

        render::RenderFrameRequest request{};
        request.scene = &framed;
        request.slice = scene::CanvasRect{0U, 0U, width, height};
        request.output_width = width;
        request.output_height = height;
        request.history.scene = framed.id;
        request.history.document_version = framed.document_version;
        request.history.surface = render::HistorySurface::Offscreen;
        request.history.surface_id = surface;
        request.history.canvas = framed.canvas;
        request.history.slice = request.slice;
        request.history.width = width;
        request.history.height = height;
        request.history.camera_source = framed.camera_source;
        request.history.camera_digest =
            render::camera_history_digest(framed.camera);
        request.history.profile_digest =
            render::renderer_profile_digest(framed.renderer_settings);

        auto acquired = device.acquire_commands();
        check(acquired.has_value(), "acquire", __LINE__);
        auto commands = std::move(acquired).value();
        auto rendered = renderer.render_frame(commands, request);
        check(rendered.has_value(), "render", __LINE__);
        auto submitted = device.submit(std::move(commands));
        check(submitted.has_value(), "submit", __LINE__);
        check(std::move(submitted).value().wait(), "wait", __LINE__);
        return rendered.value().passes;
    };

    const render::RendererPassStatus advanced =
        status_for(scene::RendererBackend::Advanced, 1U);
    RENDER_CHECK(advanced.selected == scene::RendererBackend::Advanced);
    RENDER_CHECK(!advanced.fell_back());
    RENDER_CHECK(advanced.shadows == render::PassState::Active);
    RENDER_CHECK(advanced.bounced_light == render::PassState::Active);
    RENDER_CHECK(advanced.occlusion == render::PassState::Active);
    RENDER_CHECK(advanced.bloom == render::PassState::Active);
    // Never authored, so it is not a fault and must not read as one.
    RENDER_CHECK(advanced.spatial_antialiasing
                 == render::PassState::NotAuthored);

    const render::RendererPassStatus basic =
        status_for(scene::RendererBackend::Basic, 2U);
    RENDER_CHECK(basic.selected == scene::RendererBackend::Basic);
    // Basic was *asked for* here, so this is not a fallback - the distinction
    // matters, because the warning belongs only to the project that asked for
    // Advanced and did not get it.
    RENDER_CHECK(!basic.fell_back());
    RENDER_CHECK(basic.shadows == render::PassState::BackendUnsupported);
    RENDER_CHECK(basic.bounced_light == render::PassState::BackendUnsupported);
    RENDER_CHECK(basic.occlusion == render::PassState::BackendUnsupported);
    RENDER_CHECK(basic.bloom == render::PassState::BackendUnsupported);
    RENDER_CHECK(basic.spatial_antialiasing == render::PassState::NotAuthored);
}

// Ray-traced reflections are refused, not quietly turned into environment ones.
//
// This is the central reflection requirement, and it was once an
// untested claim. The plan is explicit that reflections "must be an explicit
// authored policy that fails closed, not a per-node decision" - and the reason
// is sharper than it first looks. A node whose device could trace rays and a
// node whose device could not would each produce a *plausible* frame, one with
// screen-accurate reflections and one with the environment map, and the two
// halves of the wall would differ everywhere a surface was shiny. Degrading
// silently is the failure; refusing is the feature.
//
// `acceleration_structures` and `ray_queries` are honestly false on this
// backend, so authoring ray tracing here must leave the readiness unsatisfied
// and name what is missing.
void ray_traced_reflections_fail_closed() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    // The premise. If a device ever does report these, this test is measuring
    // nothing and should be told so rather than passing quietly.
    RENDER_CHECK(!device.capabilities().acceleration_structures);
    RENDER_CHECK(!device.capabilities().ray_queries);

    scene::RendererSettings settings{};
    settings.backend = scene::RendererBackend::Advanced;
    settings.required = true;

    const render::RendererReadiness plain = render::evaluate_renderer_readiness(
        settings, device.capabilities(), true);
    RENDER_CHECK(plain.satisfied);
    RENDER_CHECK(plain.selected == scene::RendererBackend::Advanced);

    // And with reflections asked for.
    settings.ray_tracing = true;
    const render::RendererReadiness traced =
        render::evaluate_renderer_readiness(settings, device.capabilities(),
                                            true);
    RENDER_CHECK(!traced.satisfied);
    // Named, not merely refused: an operator has to be able to tell "this
    // machine cannot" from "this project is wrong".
    RENDER_CHECK(!traced.headline().empty());

    // With Availability on OPTIONAL the renderer may proceed - and the shape of
    // that concession is the part that matters. It does **not** hand back
    // Advanced-without-reflections, which is the frame that would differ from a
    // neighbour's by exactly the shiny surfaces. It falls all the way to Basic
    // and says so, so two nodes disagreeing about ray tracing disagree about
    // the whole renderer rather than about one term in it - visible
    // immediately, in the one place an operator is already looking.
    settings.required = false;
    const render::RendererReadiness optional =
        render::evaluate_renderer_readiness(settings, device.capabilities(),
                                            true);
    RENDER_CHECK(optional.satisfied);
    RENDER_CHECK(optional.fallback);
    RENDER_CHECK(optional.selected == scene::RendererBackend::Basic);
    RENDER_CHECK(optional.code == render::RendererReadinessCode::BasicFallback);
    RENDER_CHECK(optional.requested == scene::RendererBackend::Advanced);
}

// Authoring atmosphere changes the frame the renderer publishes.
//
// The same assertion bloom needed and did not have: that the pass reaches the
// screen at all. A froxel volume can be built perfectly and thrown away, and
// every determinism test in this file would still pass, because two halves of a
// frame with no fog in them agree beautifully.
void authored_fog_changes_the_published_frame() {
    if (!render::HybridSceneRenderer::shaders_available()
        || !render::VolumetricFog::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()
        || !device.capabilities().compute
        || !device.capabilities().storage_images) {
        return;
    }

    constexpr core::u32 width = 128U;
    constexpr core::u32 height = 96U;

    scene::Scene value{};
    value.id = scene::SceneId{501U};
    value.document_version = doc::Version{1U};
    value.camera_source = doc::NodeId{500U};
    value.canvas = scene::Canvas{width, height};
    value.camera.eye = scene::Vec3{0.0F, 1.2F, 4.0F};
    value.camera.target = scene::Vec3{0.0F, 0.4F, 0.0F};
    value.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    value.camera.field_of_view = 0.125F;
    value.camera.near_plane = 0.1F;
    value.camera.far_plane = 60.0F;
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.required = true;

    // A floor running away from the camera, so there is a real spread of depth
    // for the integration to act across. Fog on a flat card would be a tint.
    scene::MeshInstance floor{};
    floor.source = doc::NodeId{510U};
    floor.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    floor.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, -0.05F, -20.0F}),
        scene::scaling(scene::Vec3{20.0F, 0.1F, 40.0F}));
    floor.normal = scene::normal_basis(floor.world);
    floor.material.base_color = scene::Vec3{0.7F, 0.7F, 0.72F};
    floor.material.roughness = 1.0F;
    value.instances.push_back(floor);

    scene::DirectionalLight sun{};
    sun.direction = scene::Vec3{-0.3F, -0.7F, -0.6F};
    sun.color = scene::Vec3{1.0F, 0.98F, 0.95F};
    sun.intensity = 4.0F;
    value.directional_lights.push_back(sun);

    render::RendererLimits limits{};
    limits.max_instances = 4U;
    limits.max_vertices = 4096U;
    limits.max_indices = 12288U;
    limits.max_lights = 4U;
    limits.max_history_slots = 2U;

    value.environment.fog_color = scene::Vec3{0.7F, 0.75F, 0.85F};
    const auto render_once = [&](float density, core::u64 surface) {
        scene::Scene framed = value;
        framed.show_frame = 1U;
        framed.environment.fog_density = density;
        framed.environment.fog_color = value.environment.fog_color;

        auto created =
            render::HybridSceneRenderer::create(device, width, height, limits);
        check(created.has_value(), "renderer", __LINE__);
        auto renderer = std::move(created).value();
        auto readback = device.create_readback(width * height * 8U);
        check(readback.has_value(), "readback", __LINE__);

        render::RenderFrameRequest request{};
        request.scene = &framed;
        request.slice = scene::CanvasRect{0U, 0U, width, height};
        request.output_width = width;
        request.output_height = height;
        request.history.scene = framed.id;
        request.history.document_version = framed.document_version;
        request.history.surface = render::HistorySurface::Offscreen;
        request.history.surface_id = surface;
        request.history.canvas = framed.canvas;
        request.history.slice = request.slice;
        request.history.width = width;
        request.history.height = height;
        request.history.camera_source = framed.camera_source;
        request.history.camera_digest =
            render::camera_history_digest(framed.camera);
        request.history.profile_digest =
            render::renderer_profile_digest(framed.renderer_settings);

        auto acquired = device.acquire_commands();
        check(acquired.has_value(), "acquire", __LINE__);
        auto commands = std::move(acquired).value();
        auto rendered = renderer.render_frame(commands, request);
        check(rendered.has_value(), "render", __LINE__);
        check(commands.download(*rendered.value().color, readback.value()),
              "download", __LINE__);
        auto submitted = device.submit(std::move(commands));
        check(submitted.has_value(), "submit", __LINE__);
        check(std::move(submitted).value().wait(), "wait", __LINE__);

        std::vector<core::u8> frame;
        const auto pixels = readback.value().map();
        const std::size_t bytes =
            static_cast<std::size_t>(width) * height * 8U;
        check(pixels.size() >= bytes, "map", __LINE__);
        frame.assign(pixels.begin(), pixels.begin() + bytes);
        readback.value().unmap();
        return std::pair<std::vector<core::u8>, render::PassState>{
            std::move(frame), rendered.value().passes.fog};
    };

    const auto clear = render_once(0.0F, 1U);
    const auto foggy = render_once(0.35F, 2U);


    // It ran, and it says so. Without this the comparison below could pass for
    // the wrong reason after some future refactor.
    RENDER_CHECK(clear.second == render::PassState::NotAuthored);
    RENDER_CHECK(foggy.second == render::PassState::Active);

    std::size_t changed = 0U;
    double most = 0.0;
    for (std::size_t pixel = 0U;
         pixel < static_cast<std::size_t>(width) * height; ++pixel) {
        const double before = half_lane(clear.first, pixel, 2U);
        const double after = half_lane(foggy.first, pixel, 2U);
        const double difference = std::abs(after - before);
        if (difference > 1.0e-3) {
            ++changed;
        }
        most = std::max(most, difference);
    }
    // Most of the frame, not a corner of it: atmosphere is everywhere there is
    // depth, which on this scene is everywhere.
    RENDER_CHECK(changed > (static_cast<std::size_t>(width) * height) / 4U);
    RENDER_CHECK(most > 0.01);

    // An HDRI without scene lights must light the fog as well as the sky.
    // A uniform white environment has a known spherical mean of one.
    const auto hdr = std::filesystem::temp_directory_path() / "mediaserver_fog_white.hdr";
    {
        std::ofstream file{hdr, std::ios::binary};
        file << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 4\n";
        const unsigned char pixel[4]{128U, 128U, 128U, 129U};
        for (int i = 0; i < 8; ++i) file.write(reinterpret_cast<const char*>(pixel), 4);
        RENDER_CHECK(file.good());
    }
    value.environment.map_source = hdr.string();
    render::EnvironmentCache cache;
    static_cast<void>(cache.observe(hdr.string(), cache.publication_epoch()));
    RENDER_CHECK(cache.wait_for_test());
    RENDER_CHECK(cache.observe(hdr.string(), cache.publication_epoch()).ready());
    value.directional_lights.clear();
    value.instances.clear();
    const auto ambient_fog = render_once(0.35F, 3U);
    RENDER_CHECK(ambient_fog.second == render::PassState::Active);
    const auto center = static_cast<std::size_t>(height / 2U) * width + width / 2U;
    RENDER_CHECK(half_lane(ambient_fog.first, center, 2U) > 0.5F);
    // With black scattering albedo, output is pure transmittance. Density
    // 0.15 across this 60-unit frustum should transmit about exp(-0.9).
    value.environment.fog_color = {};
    const auto light_haze = render_once(0.15F, 5U);
    const float transmitted = half_lane(light_haze.first, center, 2U);
    RENDER_CHECK(transmitted > 0.39F && transmitted < 0.43F);
    const auto dense_haze = render_once(5.0F, 6U);
    const float background = half_lane(dense_haze.first, center, 2U);
    RENDER_CHECK(background > 0.14F && background < 0.16F);

    // A five-metre indoor backdrop transmits exp(-0.015 * (5 - near)),
    // independently of the far clip. Geometry keeps its measured depth.
    value.environment.fog_background_distance = 5.0F;
    const auto indoor_haze = render_once(0.15F, 7U);
    const float indoor_transmitted = half_lane(indoor_haze.first, center, 2U);
    RENDER_CHECK(indoor_transmitted > 0.92F && indoor_transmitted < 0.94F);
    RENDER_CHECK(indoor_transmitted > transmitted + 0.5F);
    RENDER_CHECK(std::abs(half_lane(indoor_haze.first, 0U, 2U)
                          - indoor_transmitted) < 0.001F);
    // The finite depth gets real optical coverage, including dense air.
    const auto indoor_dense = render_once(5.0F, 8U);
    RENDER_CHECK(half_lane(indoor_dense.first, center, 2U) < 0.10F);
    // A backdrop before the near plane must not inherit the first full cell.
    value.environment.fog_background_distance = 0.05F;
    const auto before_near = render_once(0.15F, 9U);
    RENDER_CHECK(half_lane(before_near.first, center, 2U) > 0.99F);
    value.environment.fog_background_distance = 100'000.0F;
    const auto beyond_far = render_once(0.15F, 10U);
    RENDER_CHECK(std::abs(half_lane(beyond_far.first, center, 2U)
                          - transmitted) < 0.001F);

    value.instances.push_back(floor);
    value.environment.fog_background_distance = 0.0F;
    const auto surfaces_far = render_once(0.15F, 11U);
    const auto lighting_far = render_once(0.0F, 12U);
    value.environment.fog_background_distance = 5.0F;
    const auto surfaces_indoor = render_once(0.15F, 13U);
    const auto lighting_indoor = render_once(0.0F, 14U);
    const auto floor_pixel = center;
    RENDER_CHECK(half_lane(surfaces_far.first, floor_pixel, 2U)
                 == half_lane(surfaces_indoor.first, floor_pixel, 2U));
    RENDER_CHECK(std::abs(half_lane(surfaces_far.first, floor_pixel, 2U)
                          - half_lane(lighting_far.first, floor_pixel, 2U)) > 0.001F);
    RENDER_CHECK(half_lane(surfaces_indoor.first, width / 2U, 2U)
                 > half_lane(surfaces_far.first, width / 2U, 2U) + 0.5F);
    RENDER_CHECK(lighting_far.first == lighting_indoor.first);
    value.instances.clear();
    value.environment.fog_background_distance = 0.0F;
    value.environment.fog_color = scene::Vec3{0.7F, 0.75F, 0.85F};
    value.environment.map_intensity = 0.0F;
    const auto dark_fog = render_once(0.35F, 4U);
    RENDER_CHECK(half_lane(dark_fog.first, center, 2U) < 0.02F);
    std::filesystem::remove(hdr);
}

// A practical lights the air it points through, and is stopped by a wall.
//
// Two claims, and the second is the one that matters. Fog lit by a local light
// is easy; fog lit by an *unshadowed* local light is worse than no fog at all,
// because the light glows through every wall it is mounted on and gives the set
// a haze no lamp in the room can account for. That is why this pass binds the
// shadow atlas rather than skipping it, and why the test occludes the spot
// rather than only switching it on.
//
// Measured in the fog rather than on a surface: the comparison is between fog
// on with the spot and fog on without it, so what is isolated is the light the
// air returns rather than anything the floor does.
void a_practical_lights_the_air_and_a_wall_stops_it() {
    if (!render::HybridSceneRenderer::shaders_available()
        || !render::VolumetricFog::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()
        || !device.capabilities().compute
        || !device.capabilities().storage_images) {
        return;
    }

    constexpr core::u32 width = 128U;
    constexpr core::u32 height = 96U;

    scene::Scene value{};
    value.id = scene::SceneId{701U};
    value.document_version = doc::Version{1U};
    value.camera_source = doc::NodeId{700U};
    value.canvas = scene::Canvas{width, height};
    // Looking across the beam rather than down it, so the shaft crosses the
    // frame instead of being a bright dot.
    value.camera.eye = scene::Vec3{4.5F, 1.6F, 4.5F};
    value.camera.target = scene::Vec3{0.0F, 1.0F, 0.0F};
    value.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    value.camera.field_of_view = 0.14F;
    value.camera.near_plane = 0.1F;
    value.camera.far_plane = 40.0F;
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.required = true;
    value.renderer_settings.shadow_cascades = 1U;
    // Dark, so the only thing lifting the air is the practical.
    value.environment.ambient = 0.02F;
    value.environment.sky_color = scene::Vec3{0.01F, 0.01F, 0.015F};
    value.environment.ground_color = scene::Vec3{0.01F, 0.01F, 0.01F};
    value.environment.fog_density = 0.5F;
    value.environment.fog_color = scene::Vec3{0.9F, 0.9F, 0.9F};

    scene::MeshInstance floor{};
    floor.source = doc::NodeId{710U};
    floor.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    floor.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, -0.05F, 0.0F}),
        scene::scaling(scene::Vec3{12.0F, 0.1F, 12.0F}));
    floor.normal = scene::normal_basis(floor.world);
    floor.material.base_color = scene::Vec3{0.15F, 0.15F, 0.15F};
    floor.material.roughness = 1.0F;
    value.instances.push_back(floor);

    // The blocker, directly under the lamp. Present in both runs of the
    // occluded comparison so it cannot be what changes the frame.
    scene::MeshInstance wall{};
    wall.source = doc::NodeId{711U};
    wall.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    wall.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 2.2F, 0.0F}),
        scene::scaling(scene::Vec3{2.5F, 0.1F, 2.5F}));
    wall.normal = scene::normal_basis(wall.world);
    wall.material.base_color = scene::Vec3{0.2F, 0.2F, 0.2F};
    wall.material.roughness = 1.0F;

    scene::PointLight lamp{};
    lamp.source = doc::NodeId{720U};
    lamp.kind = scene::LocalLightKind::Spot;
    lamp.position = scene::Vec3{0.0F, 3.2F, 0.0F};
    lamp.color = scene::Vec3{1.0F, 0.96F, 0.9F};
    lamp.intensity = 400.0F;
    lamp.range = 20.0F;
    lamp.direction = scene::Vec3{0.0F, -1.0F, 0.0F};
    lamp.inner_cone = 0.05F;
    lamp.outer_cone = 0.09F;
    lamp.casts_shadow = true;
    lamp.shadow_tier = scene::ShadowTier::Medium;

    render::RendererLimits limits{};
    limits.max_instances = 8U;
    limits.max_vertices = 8192U;
    limits.max_indices = 24576U;
    limits.max_lights = 4U;
    limits.max_history_slots = 2U;
    limits.shadow_atlas.extent = 1024U;

    const auto render_once = [&](bool with_lamp, bool with_wall,
                                 core::u64 surface) {
        scene::Scene framed = value;
        framed.show_frame = 1U;
        if (with_wall) {
            framed.instances.push_back(wall);
        }
        if (with_lamp) {
            framed.point_lights.push_back(lamp);
        }

        auto created =
            render::HybridSceneRenderer::create(device, width, height, limits);
        check(created.has_value(), "renderer", __LINE__);
        auto renderer = std::move(created).value();
        auto readback = device.create_readback(width * height * 8U);
        check(readback.has_value(), "readback", __LINE__);

        render::RenderFrameRequest request{};
        request.scene = &framed;
        request.slice = scene::CanvasRect{0U, 0U, width, height};
        request.output_width = width;
        request.output_height = height;
        request.history.scene = framed.id;
        request.history.document_version = framed.document_version;
        request.history.surface = render::HistorySurface::Offscreen;
        request.history.surface_id = surface;
        request.history.canvas = framed.canvas;
        request.history.slice = request.slice;
        request.history.width = width;
        request.history.height = height;
        request.history.camera_source = framed.camera_source;
        request.history.camera_digest =
            render::camera_history_digest(framed.camera);
        request.history.profile_digest =
            render::renderer_profile_digest(framed.renderer_settings);

        auto acquired = device.acquire_commands();
        check(acquired.has_value(), "acquire", __LINE__);
        auto commands = std::move(acquired).value();
        auto rendered = renderer.render_frame(commands, request);
        check(rendered.has_value(), "render", __LINE__);
        check(commands.download(*rendered.value().color, readback.value()),
              "download", __LINE__);
        auto submitted = device.submit(std::move(commands));
        check(submitted.has_value(), "submit", __LINE__);
        check(std::move(submitted).value().wait(), "wait", __LINE__);

        std::vector<core::u8> frame;
        const auto pixels = readback.value().map();
        const std::size_t bytes =
            static_cast<std::size_t>(width) * height * 8U;
        check(pixels.size() >= bytes, "map", __LINE__);
        frame.assign(pixels.begin(), pixels.begin() + bytes);
        readback.value().unmap();
        return frame;
    };

    // How much light the lamp adds, summed over a band of rows.
    //
    // The band matters. Summed over the whole frame this measures the lamp
    // lighting the *floor*, which happens whether or not the scatter pass reads
    // a single local light - and an assertion that cannot tell those apart is
    // no assertion. Restricted to rows above the horizon there is no geometry
    // at all, so anything the lamp adds there was added by the air.
    const auto lamp_contribution = [&](const std::vector<core::u8>& lit,
                                       const std::vector<core::u8>& dark,
                                       core::u32 first_row,
                                       core::u32 last_row) {
        double total = 0.0;
        for (core::u32 y = first_row; y <= last_row; ++y) {
            for (core::u32 x = 0U; x < width; ++x) {
                const std::size_t pixel =
                    static_cast<std::size_t>(y) * width + x;
                const double with = half_lane(lit, pixel, 1U);
                const double without = half_lane(dark, pixel, 1U);
                total += std::max(with - without, 0.0);
            }
        }
        return total;
    };

    const std::vector<core::u8> open_dark = render_once(false, false, 1U);
    const std::vector<core::u8> open_lit = render_once(true, false, 2U);
    const std::vector<core::u8> blocked_dark = render_once(false, true, 3U);
    const std::vector<core::u8> blocked_lit = render_once(true, true, 4U);

    // Above the horizon: no geometry, so only the air can be lit.
    constexpr core::u32 sky_last_row = height / 4U;
    const double air =
        lamp_contribution(open_lit, open_dark, 0U, sky_last_row);
    // The whole frame, for the occlusion comparison - the shaft the wall stops
    // is mostly below the horizon, where the beam is.
    const double open = lamp_contribution(open_lit, open_dark, 0U, height - 1U);
    const double blocked =
        lamp_contribution(blocked_lit, blocked_dark, 0U, height - 1U);

    // It lights the *air*. Measured where nothing was drawn, so the scatter
    // pass reading local lights is the only thing that can produce it.
    RENDER_CHECK(air > 0.5);

    // And a wall stops it. Not to nothing - the lamp sits above the blocker and
    // the cone still reaches air beside it - but the shadowed volume has to be
    // markedly darker than the open one, which is exactly what an unshadowed
    // lookup fails to produce.
    RENDER_CHECK(blocked < open * 0.75);
}

// A chain the show cannot pay an apron for is refused, never shortened.
//
// This is the case the plan is emphatic about and it is worth being explicit
// why: a node that shortened its chain under budget pressure would blur its
// half of the wall to a smaller radius than the node beside it, and no
// downstream pass can reconcile two different filters. Refusing is visible and
// fixable; shortening is invisible and permanent.
void a_bloom_chain_past_the_apron_budget_is_refused() {
    scene::Scene value{};
    value.canvas = scene::Canvas{1920U, 1080U};
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.bloom_strength = 1.0F;

    const scene::CanvasRect slice{640U, 0U, 640U, 1080U};
    const core::u32 reach =
        render::bloom_chain_apron(render::bloom_chain_depth(value.canvas));
    // Four whole blocks of the coarsest level, which is the kernels' 252-pixel
    // reach rounded up so the padded rectangle stays on the canvas grid.
    RENDER_CHECK(reach == 256U);

    // A budget below the chain's reach refuses the whole padded slice. The
    // renderer never sees a shortened chain because there is no frame to put
    // one in.
    value.renderer_settings.slice_apron_limit = reach - 1U;
    // The 1280-pixel slice below starts on a 64-pixel block boundary, so this
    // refusal is the budget and nothing else.
    const auto refused = render::padded_slice_for(value, slice, slice.width,
                                                  slice.height, true);
    RENDER_CHECK(!refused);
    RENDER_CHECK(refused.error() == render::SliceApronError::LimitExceeded);

    // The default budget pays for it, and pays for exactly it: the apron is
    // the chain's full reach rather than whatever was left over.
    value.renderer_settings.slice_apron_limit =
        scene::RendererSettings{}.slice_apron_limit;
    const auto allowed = render::padded_slice_for(value, slice, slice.width,
                                                  slice.height, true);
    RENDER_CHECK(allowed.has_value());
    RENDER_CHECK(allowed.value().apron == reach);

    // And with bloom off the lane costs nothing, so a show that does not want
    // it does not pay for it.
    value.renderer_settings.bloom_strength = 0.0F;
    const auto unpaid = render::padded_slice_for(value, slice, slice.width,
                                                 slice.height, true);
    RENDER_CHECK(unpaid.has_value());
    RENDER_CHECK(unpaid.value().apron == 0U);
}

// Two nodes rendering half a canvas each reconstruct what one node rendering
// all of it produces.
//
// This is the promise the whole plan is built on, and until it was written
// nothing tested it for any pass added after the first Advanced passes. The older halves check
// drives the Basic renderer on a scene that never selects a backend; the
// environment, temporal antialiasing, occlusion, the shadow atlas and the
// display transform were each *designed* around cross-node agreement -
// canvas-derived jitter rather than slice-derived, atlas tiles keyed by node id
// rather than by camera distance, filtering written out by hand rather than
// sampled - and every one of those arguments was untested end to end.
//
// It found three real defects, all fixed: velocity expressed in the render
// target's own UV space rather than the canvas, a sub-pixel jitter that landed
// on thirds and so did not survive being added to a pixel index, and - from the
// same measurements - the confirmation that the remaining difference is a floor
// rather than a drift.
//
// Multi-frame, because temporal state is where a per-node divergence compounds.
// One frame agrees even when the history is seeded differently.
void two_nodes_and_one_node_agree_byte_for_byte() {
    if (!render::HybridSceneRenderer::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()
        || !device.capabilities().compute
        || !device.capabilities().storage_images) {
        return;
    }

    constexpr core::u32 canvas_width = 128U;
    constexpr core::u32 canvas_height = 96U;
    constexpr core::u32 half = canvas_width / 2U;

    scene::Scene value{};
    value.id = scene::SceneId{201U};
    value.document_version = doc::Version{1U};
    value.camera_source = doc::NodeId{200U};
    value.canvas = scene::Canvas{canvas_width, canvas_height};
    value.camera.eye = scene::Vec3{0.0F, 1.5F, 3.4F};
    value.camera.target = scene::Vec3{0.0F, 0.2F, 0.0F};
    value.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    value.camera.field_of_view = 0.125F;
    value.camera.near_plane = 0.1F;
    value.camera.far_plane = 100.0F;
    value.environment.ambient = 1.0F;
    value.environment.sky_color = scene::Vec3{0.4F, 0.45F, 0.5F};
    value.environment.ground_color = scene::Vec3{0.2F, 0.19F, 0.18F};
    // Atmosphere. The froxel volume is the newest thing here with an
    // opportunity to disagree - it partitions the canvas into cells and
    // integrates along a ray - so it belongs in the comparison that would
    // catch it.
    value.environment.fog_color = scene::Vec3{0.7F, 0.75F, 0.85F};
    value.environment.fog_anisotropy = 0.4F;
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.required = true;
    // Everything on at once. A pass tested alone can be node-independent while
    // the combination is not - the apron is shared, and a lane that reads
    // further than the padding reserves is only wrong when something else is
    // also asking for padding.
    value.renderer_settings.shadow_cascades = 2U;
    value.renderer_settings.gi_strength = 1.0F;
    value.renderer_settings.taa_sequence_length = 8U;
    value.renderer_settings.occlusion_radius = 1.0F;
    value.renderer_settings.occlusion_strength = 1.0F;
    // Bloom, whose chain partitions the canvas into blocks and averages them.
    // Of everything here it is the pass most obviously able to disagree across
    // a seam - a level whose blocks begin in a different place is a different
    // filter, not a boundary artefact - so it belongs in this test more than
    // most.
    value.renderer_settings.bloom_strength = 0.8F;

    // Geometry that straddles the seam, so a disagreement has somewhere to
    // show. A scene that stopped at the halfway line would agree trivially.
    scene::MeshInstance floor{};
    floor.source = doc::NodeId{210U};
    floor.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    floor.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, -0.05F, 0.0F}),
        scene::scaling(scene::Vec3{10.0F, 0.1F, 10.0F}));
    floor.normal = scene::normal_basis(floor.world);
    floor.material.base_color = scene::Vec3{0.85F, 0.84F, 0.82F};
    floor.material.roughness = 1.0F;
    value.instances.push_back(floor);

    scene::MeshInstance block{};
    block.source = doc::NodeId{211U};
    block.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    // Rotated and sitting on the centre line: its silhouette crosses the seam
    // diagonally, which is the hardest case for a jittered edge.
    block.world = scene::multiply(
        scene::multiply(scene::translation(scene::Vec3{0.0F, 0.35F, 0.0F}),
                        scene::rotation(0.07F, 0.0F, 0.0F)),
        scene::scaling(scene::Vec3{0.8F, 0.8F, 0.8F}));
    block.normal = scene::normal_basis(block.world);
    block.material.base_color = scene::Vec3{0.8F, 0.5F, 0.35F};
    block.material.roughness = 0.6F;
    value.instances.push_back(block);

    scene::DirectionalLight sun{};
    sun.direction = scene::Vec3{-0.4F, -0.8F, -0.45F};
    sun.color = scene::Vec3{1.0F, 0.97F, 0.92F};
    sun.intensity = 3.0F;
    value.directional_lights.push_back(sun);

    // A casting spot, so the atlas partition is exercised too.
    scene::PointLight lamp{};
    lamp.source = doc::NodeId{212U};
    lamp.kind = scene::LocalLightKind::Spot;
    lamp.position = scene::Vec3{0.1F, 2.4F, 1.0F};
    lamp.color = scene::Vec3{1.0F, 1.0F, 1.0F};
    lamp.intensity = 24.0F;
    lamp.range = 12.0F;
    lamp.direction = scene::normalize(scene::Vec3{0.0F, -1.0F, -0.35F},
                                      scene::Vec3{0.0F, -1.0F, 0.0F});
    lamp.inner_cone = 0.07F;
    lamp.outer_cone = 0.10F;
    lamp.casts_shadow = true;
    lamp.shadow_tier = scene::ShadowTier::Small;
    value.point_lights.push_back(lamp);

    render::RendererLimits limits{};
    limits.max_instances = 8U;
    limits.max_vertices = 8192U;
    limits.max_indices = 24576U;
    limits.max_lights = 4U;
    limits.directional_shadow_resolution = 256U;
    limits.max_history_slots = 2U;
    limits.shadow_atlas.extent = 512U;

    // Renders one slice over a run of frames and returns the authored
    // rectangle, cropped out of whatever padded target the apron produced.
    const auto render_slice_sequence = [&](scene::CanvasRect slice,
                                           core::u64 surface,
                                           core::u64 frames) {
        // Exactly the padded extent, not merely enough for it. The renderer
        // refuses a target of the wrong size rather than rendering into part of
        // it, which is the right call - a mismatch there would put the authored
        // rectangle somewhere other than where the crop looks.
        const auto plan = render::padded_slice_for(
            value, slice, slice.width, slice.height, true);
        check(plan.has_value(), "padded slice", __LINE__);
        const core::u32 target_width = plan.value().render.width;
        const core::u32 target_height = plan.value().render.height;
        auto created = render::HybridSceneRenderer::create(
            device, target_width, target_height, limits);
        check(created.has_value(), "renderer creation", __LINE__);
        auto renderer = std::move(created).value();

        const core::u32 padded_bytes = target_width * target_height * 8U;
        auto readback = device.create_readback(padded_bytes);
        check(readback.has_value(), "readback", __LINE__);

        render::RenderProducts products{};
        std::vector<core::u8> padded;
        for (core::u64 frame = 1U; frame <= frames; ++frame) {
            scene::Scene framed = value;
            framed.show_frame = frame;
            render::RenderFrameRequest request{};
            request.scene = &framed;
            request.slice = slice;
            request.output_width = slice.width;
            request.output_height = slice.height;
            request.history.scene = framed.id;
            request.history.document_version = framed.document_version;
            request.history.surface = render::HistorySurface::Offscreen;
            request.history.surface_id = surface;
            request.history.canvas = framed.canvas;
            request.history.slice = slice;
            request.history.width = slice.width;
            request.history.height = slice.height;
            request.history.camera_source = framed.camera_source;
            request.history.camera_digest =
                render::camera_history_digest(framed.camera);
            request.history.profile_digest =
                render::renderer_profile_digest(framed.renderer_settings);

            auto acquired = device.acquire_commands();
            check(acquired.has_value(), "acquire", __LINE__);
            auto commands = std::move(acquired).value();
            auto rendered = renderer.render_frame(commands, request);
            check(rendered.has_value(), "render", __LINE__);
            products = rendered.value();
            const bool last = frame == frames;
            if (last) {
                check(commands.download(*products.color, readback.value()),
                      "download", __LINE__);
            }
            auto submitted = device.submit(std::move(commands));
            check(submitted.has_value(), "submit", __LINE__);
            check(std::move(submitted).value().wait(), "wait", __LINE__);
            if (last) {
                const auto pixels = readback.value().map();
                check(pixels.size() >= padded_bytes, "map", __LINE__);
                padded.assign(pixels.begin(), pixels.begin() + padded_bytes);
                readback.value().unmap();
            }
        }

        // The authored rectangle inside the padded target. Cropping here is the
        // same crop a real output performs, and getting it wrong would make two
        // correct renders look different.
        std::vector<core::u8> cropped(
            static_cast<std::size_t>(slice.width) * slice.height * 8U, 0U);
        for (core::u32 y = 0U; y < slice.height; ++y) {
            const std::size_t source =
                (static_cast<std::size_t>(products.crop.y + y) * target_width
                 + products.crop.x) * 8U;
            const std::size_t target =
                static_cast<std::size_t>(y) * slice.width * 8U;
            std::memcpy(cropped.data() + target, padded.data() + source,
                        static_cast<std::size_t>(slice.width) * 8U);
        }
        return cropped;
    };

    struct Comparison final {
        std::size_t differing_bytes{};
        double worst{};
        float spread{};
    };

    const auto compare_halves_against_whole = [&](core::u64 frames) {
        const std::vector<core::u8> whole = render_slice_sequence(
            scene::CanvasRect{0U, 0U, canvas_width, canvas_height}, 1U, frames);
        const std::vector<core::u8> left = render_slice_sequence(
            scene::CanvasRect{0U, 0U, half, canvas_height}, 2U, frames);
        const std::vector<core::u8> right = render_slice_sequence(
            scene::CanvasRect{half, 0U, half, canvas_height}, 3U, frames);

        Comparison result{};
        float lowest = 1.0e30F;
        float highest = -1.0e30F;
        for (core::u32 y = 0U; y < canvas_height; ++y) {
            for (core::u32 x = 0U; x < canvas_width; ++x) {
                const std::size_t whole_pixel =
                    static_cast<std::size_t>(y) * canvas_width + x;
                const bool on_left = x < half;
                const std::size_t part_pixel = static_cast<std::size_t>(y)
                        * half + (on_left ? x : x - half);
                const std::vector<core::u8>& part = on_left ? left : right;
                for (core::u32 channel = 0U; channel < 4U; ++channel) {
                    const float mine = half_lane(whole, whole_pixel, channel);
                    const float theirs = half_lane(part, part_pixel, channel);
                    if (channel == 0U) {
                        lowest = std::min(lowest, mine);
                        highest = std::max(highest, mine);
                    }
                    const double difference = std::abs(
                        static_cast<double>(mine) - static_cast<double>(theirs));
                    if (difference > 0.0) {
                        ++result.differing_bytes;
                        result.worst = std::max(result.worst, difference);
                    }
                }
            }
        }
        result.spread = highest - lowest;
        return result;
    };

    // Without the temporal pass: byte for byte, and nothing weaker.
    //
    // The environment and its sky, the occlusion kernel rotated by absolute
    // canvas coordinate, both shadow cascades, the reflective shadow map, the
    // spot atlas keyed by document node id and the bloom chain partitioned on
    // the canvas grid are all live here. A near-comparison would pass for
    // exactly the drift this is looking for.
    value.renderer_settings.taau = false;
    const Comparison plain = compare_halves_against_whole(8U);
    // The frame has to vary, or agreement proves nothing.
    RENDER_CHECK(plain.spread > 0.05F);
    RENDER_CHECK(plain.differing_bytes == 0U);

    // With it: one unit in the last place of the target format, and it stays
    // there.
    //
    // Byte equality is not reachable through a temporal history and this is not
    // a tolerance chosen to make a test pass, so here is the reasoning. Two
    // slices are two *different frustums*: the whole canvas is projected
    // through one window and a half through another, and a pixel's shaded value
    // is the same computation over inputs that round differently in their last
    // bit. Without a history that difference stays below the target format and
    // the comparison above really is exact. With one, the resolve reads its own
    // previous output, so a difference that would have been rounded away is
    // instead carried forward.
    //
    // What matters is whether it *compounds*, and the two runs below are the
    // measurement: the same bound after thirty-two frames as after eight. When
    // this test was first written it did compound - 0.11 of a unit of linear
    // light, growing - because velocity was expressed in the render target's
    // own UV space, and again because the jitter sequence landed on thirds that
    // did not survive being added to a pixel index. Both are fixed; a
    // regression in either would break the second check long before it broke
    // the first.
    // With atmosphere, and byte equality is no longer the claim.
    //
    // The volume itself is node-identical by construction - every node
    // integrates the whole grid from the canvas-wide matrix, and the apply
    // stage keys each pixel's lookup to its absolute position on the wall - so
    // nothing in the fog arithmetic differs. What differs is what reaches it:
    // the depth of a pixel is rasterised through two different frustums, and a
    // last-bit difference there moves the froxel lookup a hair and the
    // trilinear weight with it. Five of 24576 byte lanes, at one unit in the
    // last place. It cannot accumulate - fog keeps no history - so this is a
    // floor rather than a drift, and the run below at thirty-two frames is what
    // says so.
    //
    // The depth product as the cause is inferred from the magnitude and from
    // the fog arithmetic having no other node-dependent input; it has not been
    // isolated directly.
    constexpr double one_ulp_near_half = 1.0 / 2048.0;
    value.environment.fog_density = 0.12F;
    const Comparison foggy = compare_halves_against_whole(8U);
    RENDER_CHECK(foggy.spread > 0.05F);
    RENDER_CHECK(foggy.worst <= one_ulp_near_half);
    const Comparison foggy_sustained = compare_halves_against_whole(32U);
    RENDER_CHECK(foggy_sustained.worst <= one_ulp_near_half);
    value.environment.fog_background_distance = 5.0F;
    const Comparison indoor_fog = compare_halves_against_whole(8U);
    RENDER_CHECK(indoor_fog.worst <= one_ulp_near_half);
    value.environment.fog_background_distance = 0.0F;

    value.renderer_settings.taau = true;
    // One unit in the last place of binary16 at the magnitudes this scene
    // produces, which sit near one.
    constexpr double one_ulp = 1.0 / 1024.0;
    // Four of them, and the factor is bloom rather than slack. The temporal
    // residual on its own is a single unit; bloom then adds `strength` times a
    // sum over the whole chain, so a one-unit difference at the input arrives
    // at the output multiplied by roughly that. Measured at four with the
    // strength of 0.8 authored above.
    //
    // What matters is the comparison between the two runs rather than against
    // the bound: **the same amplitude after thirty-two frames as after eight**.
    // The affected area spreads - thirty-two byte lanes become sixty-eight,
    // because spreading a difference is what a blur does - while the amplitude
    // does not move. That is a floor, not a drift.
    constexpr double bloom_amplification = 4.0;
    const Comparison brief = compare_halves_against_whole(8U);
    RENDER_CHECK(brief.spread > 0.05F);
    RENDER_CHECK(brief.worst <= one_ulp * bloom_amplification);
    const Comparison sustained = compare_halves_against_whole(32U);
    RENDER_CHECK(sustained.worst <= one_ulp * bloom_amplification);
}

// A spot light lights a cone, and a point light in its place lights a sphere.
//
// `LocalLightKind::Spot` has been in the document for a long time - kind,
// direction, inner and outer cone, all parsed and validated - and nothing in
// the shader ever read the cone, so every spot on a set lit like a bare bulb.
// On an interior set that is more obviously wrong than almost anything else,
// because practicals are what light interiors.
//
// The comparison against the same light as a point is the load-bearing half:
// it rules out the reading where the cone "works" because the light is simply
// dim or far away.
void photometric_lights_reach_pixels_and_slices() {
    if (!render::HybridSceneRenderer::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()) {
        return;
    }

    constexpr core::u32 extent = 128U;
    scene::Scene value{};
    value.id = scene::SceneId{121U};
    value.document_version = doc::Version{1U};
    value.camera_source = doc::NodeId{120U};
    value.canvas = scene::Canvas{extent, extent};
    // Looking straight down at a floor, so the cone's footprint is a disc in
    // the middle of the frame and the corners are outside it.
    value.camera.eye = scene::Vec3{0.0F, 3.0F, 0.0F};
    value.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    value.camera.up = scene::Vec3{0.0F, 0.0F, -1.0F};
    value.camera.field_of_view = 0.125F;
    value.camera.near_plane = 0.1F;
    value.camera.far_plane = 100.0F;
    // No ambient at all: every photon in this frame comes from the one light.
    value.environment.ambient = 0.0F;
    value.environment.sky_color = scene::Vec3{};
    value.environment.ground_color = scene::Vec3{};
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.required = true;
    value.renderer_settings.shadow_cascades = 0U;
    value.renderer_settings.gi_strength = 0.0F;

    scene::MeshInstance floor{};
    floor.source = doc::NodeId{130U};
    floor.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    floor.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, -0.05F, 0.0F}),
        scene::scaling(scene::Vec3{12.0F, 0.1F, 12.0F}));
    floor.normal = scene::normal_basis(floor.world);
    floor.material.base_color = scene::Vec3{0.18F, 0.18F, 0.18F};
    floor.material.roughness = 1.0F;
    value.instances.push_back(floor);

    scene::PointLight lamp{};
    lamp.source = doc::NodeId{131U};
    lamp.position = scene::Vec3{0.0F, 1.6F, 0.0F};
    lamp.color = scene::Vec3{1.0F, 1.0F, 1.0F};
    lamp.intensity = 24.0F;
    lamp.range = 12.0F;
    lamp.direction = scene::Vec3{0.0F, -1.0F, 0.0F};
    // A narrow cone: about eleven degrees inside, eighteen outside.
    lamp.inner_cone = 0.03F;
    lamp.outer_cone = 0.05F;

    render::RendererLimits limits{};
    limits.max_instances = 8U;
    limits.max_vertices = 4096U;
    limits.max_indices = 12288U;
    limits.max_lights = 4U;
    limits.directional_shadow_resolution = 256U;
    limits.max_history_slots = 2U;

    auto created = render::HybridSceneRenderer::create(
        device, extent, extent, limits);
    RENDER_CHECK(created);
    auto renderer = std::move(created).value();

    const auto capture = [&](scene::CanvasRect slice) {
        RENDER_CHECK(renderer.resize(device, slice.width, slice.height));
        render::RenderFrameRequest request{};
        request.scene = &value;
        request.slice = slice;
        request.output_width = slice.width;
        request.output_height = slice.height;
        request.history.scene = value.id;
        request.history.document_version = value.document_version;
        request.history.surface = render::HistorySurface::Offscreen;
        request.history.surface_id = 1U;
        request.history.canvas = value.canvas;
        request.history.slice = slice;
        request.history.width = slice.width;
        request.history.height = slice.height;
        request.history.camera_source = value.camera_source;
        request.history.camera_digest = render::camera_history_digest(value.camera);
        request.history.profile_digest = render::renderer_profile_digest(value.renderer_settings);
        const core::u32 bytes = slice.width * slice.height * 8U;
        auto readback = device.create_readback(bytes);
        RENDER_CHECK(readback);
        auto acquired = device.acquire_commands(); RENDER_CHECK(acquired);
        auto commands = std::move(acquired).value();
        auto products = renderer.render_frame(commands, request); RENDER_CHECK(products);
        RENDER_CHECK(commands.download(*products.value().color, readback.value()));
        auto submitted = device.submit(std::move(commands)); RENDER_CHECK(submitted);
        RENDER_CHECK(std::move(submitted).value().wait());
        const auto pixels = readback.value().map(); RENDER_CHECK(pixels.size() >= bytes);
        std::vector<core::u8> result(pixels.begin(), pixels.begin() + bytes);
        readback.value().unmap(); return result;
    };
    const auto full = scene::whole(value.canvas);
    const auto center = static_cast<std::size_t>(extent / 2U) * extent + extent / 2U;
    const auto close = [](const auto& a, const auto& b) {
        RENDER_CHECK(a.size() == b.size());
        for (std::size_t pixel = 0; pixel < a.size() / 8U; ++pixel)
            for (unsigned lane = 0; lane < 3; ++lane)
                RENDER_CHECK(std::abs(half_lane(a, pixel, lane) - half_lane(b, pixel, lane)) < 0.001F);
    };

    scene::DirectionalLight sun;
    sun.direction = {0, -1, 0};
    sun.intensity = 3.14159265F;
    value.directional_lights.push_back(sun);
    const auto legacy = capture(full);
    value.directional_lights[0].color_temperature_kelvin = 3200;
    RENDER_CHECK(capture(full) == legacy); // disabled temperature is no work
    value.directional_lights[0].use_color_temperature = true;
    value.directional_lights[0].color_temperature_kelvin = 6500;
    RENDER_CHECK(capture(full) == legacy); // neutral temperature is bit exact
    value.renderer_settings.photometric_scale *= 4;
    RENDER_CHECK(capture(full) == legacy); // unitless ignores the anchor
    value.renderer_settings.photometric_scale /= 4;
    value.directional_lights[0].unit = scene::LightUnit::Lux;
    value.directional_lights[0].intensity = 1000;
    const auto lux = capture(full);
    close(lux, legacy);
    // A real reference setup, not an assertion about the packer's formula.
    const float grey = half_lane(lux, center, 1);
    RENDER_CHECK(grey > 0.165F && grey < 0.20F);
    value.camera.shutter_seconds *= 0.5F;
    const auto short_exposure = capture(full);
    RENDER_CHECK(std::abs(half_lane(short_exposure, center, 1) / grey - 0.5F) < 0.002F);
    value.camera.shutter_seconds *= 2;
    value.directional_lights[0].color_temperature_kelvin = 3200;
    const auto warm = capture(full);
    value.directional_lights[0].color_temperature_kelvin = 8000;
    const auto cool = capture(full);
    RENDER_CHECK(half_lane(warm, center, 0) > 2 * half_lane(warm, center, 2));
    RENDER_CHECK(half_lane(cool, center, 2) > 1.25F * half_lane(cool, center, 0));
    value.directional_lights.clear();

    lamp.unit = scene::LightUnit::Candela;
    lamp.intensity = 400;
    value.point_lights.push_back(lamp);
    const auto candela = capture(full);
    value.point_lights[0].unit = scene::LightUnit::Lumens;
    value.point_lights[0].intensity = 5026.548246F; // isotropic 400 cd source
    close(capture(full), candela);
    value.point_lights[0].unit = scene::LightUnit::Candela;
    value.point_lights[0].intensity = 400;
    value.point_lights[0].use_color_temperature = true;
    RENDER_CHECK(capture(full) == candela);
    value.point_lights[0].color_temperature_kelvin = 3200;
    const auto warm_point = capture(full);
    RENDER_CHECK(half_lane(warm_point, center, 0) > 2 * half_lane(warm_point, center, 2));
    value.point_lights[0].kind = scene::LocalLightKind::Spot;
    value.point_lights[0].unit = scene::LightUnit::Lumens;
    value.point_lights[0].intensity = 900;
    const auto spot = capture(full);
    RENDER_CHECK(half_lane(spot, center, 1) > 0.01F);
    RENDER_CHECK(half_lane(spot, 6U * extent + 6U, 1) == 0);
    const auto left = capture({0, 0, extent / 2U, extent});
    const auto right = capture({extent / 2U, 0, extent / 2U, extent});
    for (unsigned y = 0; y < extent; ++y) {
        const auto row = static_cast<std::size_t>(y) * extent * 8U;
        const auto half_row = row / 2U;
        RENDER_CHECK(std::equal(left.begin() + half_row, left.begin() + half_row + extent * 4U, spot.begin() + row));
        RENDER_CHECK(std::equal(right.begin() + half_row, right.begin() + half_row + extent * 4U, spot.begin() + row + extent * 4U));
    }
    value.point_lights[0].range = 1.6F; // floor is exactly at or beyond range
    const auto out_of_range = capture(full);
    for (std::size_t pixel = 0; pixel < extent * extent; ++pixel)
        RENDER_CHECK(half_lane(out_of_range, pixel, 0) == 0 && half_lane(out_of_range, pixel, 1) == 0 && half_lane(out_of_range, pixel, 2) == 0);
}

void a_spot_lights_a_cone_and_a_point_does_not() {
    if (!render::HybridSceneRenderer::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()) {
        return;
    }

    constexpr core::u32 extent = 128U;
    scene::Scene value{};
    value.id = scene::SceneId{121U};
    value.document_version = doc::Version{1U};
    value.camera_source = doc::NodeId{120U};
    value.canvas = scene::Canvas{extent, extent};
    // Looking straight down at a floor, so the cone's footprint is a disc in
    // the middle of the frame and the corners are outside it.
    value.camera.eye = scene::Vec3{0.0F, 3.0F, 0.0F};
    value.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    value.camera.up = scene::Vec3{0.0F, 0.0F, -1.0F};
    value.camera.field_of_view = 0.125F;
    value.camera.near_plane = 0.1F;
    value.camera.far_plane = 100.0F;
    // No ambient at all: every photon in this frame comes from the one light.
    value.environment.ambient = 0.0F;
    value.environment.sky_color = scene::Vec3{};
    value.environment.ground_color = scene::Vec3{};
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.required = true;
    value.renderer_settings.shadow_cascades = 0U;
    value.renderer_settings.gi_strength = 0.0F;

    scene::MeshInstance floor{};
    floor.source = doc::NodeId{130U};
    floor.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    floor.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, -0.05F, 0.0F}),
        scene::scaling(scene::Vec3{12.0F, 0.1F, 12.0F}));
    floor.normal = scene::normal_basis(floor.world);
    floor.material.base_color = scene::Vec3{0.9F, 0.9F, 0.9F};
    floor.material.roughness = 1.0F;
    value.instances.push_back(floor);

    scene::PointLight lamp{};
    lamp.source = doc::NodeId{131U};
    lamp.position = scene::Vec3{0.0F, 1.6F, 0.0F};
    lamp.color = scene::Vec3{1.0F, 1.0F, 1.0F};
    lamp.intensity = 24.0F;
    lamp.range = 12.0F;
    lamp.direction = scene::Vec3{0.0F, -1.0F, 0.0F};
    // A narrow cone: about eleven degrees inside, eighteen outside.
    lamp.inner_cone = 0.03F;
    lamp.outer_cone = 0.05F;

    render::RendererLimits limits{};
    limits.max_instances = 8U;
    limits.max_vertices = 4096U;
    limits.max_indices = 12288U;
    limits.max_lights = 4U;
    limits.directional_shadow_resolution = 256U;
    limits.max_history_slots = 2U;

    auto created = render::HybridSceneRenderer::create(
        device, extent, extent, limits);
    RENDER_CHECK(created);
    auto renderer = std::move(created).value();

    const auto capture = [&](scene::LocalLightKind kind) {
        scene::Scene framed = value;
        scene::PointLight light = lamp;
        light.kind = kind;
        framed.point_lights.push_back(light);

        render::RenderFrameRequest request{};
        request.scene = &framed;
        request.slice = scene::whole(framed.canvas);
        request.output_width = extent;
        request.output_height = extent;
        request.history.scene = framed.id;
        request.history.document_version = framed.document_version;
        request.history.surface = render::HistorySurface::Offscreen;
        request.history.surface_id =
            static_cast<core::u64>(kind) + 1U;
        request.history.canvas = framed.canvas;
        request.history.slice = request.slice;
        request.history.width = extent;
        request.history.height = extent;
        request.history.camera_source = framed.camera_source;
        request.history.camera_digest =
            render::camera_history_digest(framed.camera);
        request.history.profile_digest =
            render::renderer_profile_digest(framed.renderer_settings);

        auto readback = device.create_readback(extent * extent * 8U);
        RENDER_CHECK(readback);
        auto acquired = device.acquire_commands();
        RENDER_CHECK(acquired);
        auto commands = std::move(acquired).value();
        auto products = renderer.render_frame(commands, request);
        RENDER_CHECK(products);
        RENDER_CHECK(commands.download(*products.value().color,
                                       readback.value()));
        auto submitted = device.submit(std::move(commands));
        RENDER_CHECK(submitted);
        RENDER_CHECK(std::move(submitted).value().wait());
        const auto pixels = readback.value().map();
        RENDER_CHECK(pixels.size() >= extent * extent * 8U);
        std::vector<core::u8> image(pixels.begin(),
                                    pixels.begin() + extent * extent * 8U);
        readback.value().unmap();
        return image;
    };

    const std::vector<core::u8> spot = capture(scene::LocalLightKind::Spot);
    const std::vector<core::u8> point = capture(scene::LocalLightKind::Point);

    const auto green_at = [&](const std::vector<core::u8>& image, core::u32 x,
                              core::u32 y) {
        return half_lane(image, static_cast<std::size_t>(y) * extent + x, 1U);
    };
    const core::u32 middle = extent / 2U;
    const core::u32 edge = 6U;

    // Under the lamp both light the floor, so the cone is not simply off.
    RENDER_CHECK(green_at(spot, middle, middle) > 0.05F);
    RENDER_CHECK(green_at(point, middle, middle) > 0.05F);

    // Away from the axis the spot has stopped and the point has not. This is
    // the whole feature: the same light, the same position, the same
    // intensity, and only the kind differs.
    RENDER_CHECK(green_at(spot, edge, edge) <= 0.001F);
    RENDER_CHECK(green_at(point, edge, edge) > green_at(spot, edge, edge));

    // And the cone has an inside rather than being a single lit pixel: the
    // ring just off the axis is still lit, so this is a cone and not a
    // degenerate one.
    RENDER_CHECK(green_at(spot, middle + 4U, middle) > 0.05F);
}

// Occlusion darkens a crease and leaves an open surface alone.
//
// That contrast is the whole feature. An occlusion term that darkens
// everything is a global dimmer, and one that darkens nothing is absent; the
// only useful result is the one that can tell the inside of a corner from the
// middle of a floor. The two are measured in the same frame so no difference
// in lighting, exposure or camera can explain the gap between them.
void occlusion_darkens_a_crease_and_not_an_open_plane() {
    if (!render::HybridSceneRenderer::shaders_available()
        || !render::AmbientOcclusion::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()
        || !device.capabilities().compute
        || !device.capabilities().storage_images) {
        return;
    }

    constexpr core::u32 extent = 128U;
    scene::Scene value{};
    value.id = scene::SceneId{93U};
    value.document_version = doc::Version{2U};
    value.camera_source = doc::NodeId{92U};
    value.canvas = scene::Canvas{extent, extent};
    // Looking down into the corner between a floor and a wall, so the crease
    // runs across the middle of the frame and open floor sits below it.
    value.camera.eye = scene::Vec3{0.0F, 1.2F, 2.6F};
    value.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    value.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    value.camera.field_of_view = 0.125F;
    value.camera.near_plane = 0.1F;
    value.camera.far_plane = 100.0F;
    value.environment.ambient = 1.0F;
    value.environment.sky_color = scene::Vec3{0.5F, 0.5F, 0.5F};
    value.environment.ground_color = scene::Vec3{0.3F, 0.3F, 0.3F};
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.required = true;
    value.renderer_settings.shadow_cascades = 0U;
    value.renderer_settings.occlusion_radius = 1.0F;
    value.renderer_settings.occlusion_strength = 1.0F;

    // A wide flat floor.
    scene::MeshInstance floor{};
    floor.source = doc::NodeId{81U};
    floor.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    floor.world = scene::multiply(scene::translation(
                                      scene::Vec3{0.0F, -0.55F, 0.0F}),
                                  scene::scaling(scene::Vec3{6.0F, 0.1F, 6.0F}));
    floor.normal = scene::normal_basis(floor.world);
    floor.material.base_color = scene::Vec3{0.8F, 0.8F, 0.8F};
    floor.material.roughness = 1.0F;
    value.instances.push_back(floor);

    // A wall standing on it. The join between the two is the crease.
    scene::MeshInstance wall{};
    wall.source = doc::NodeId{82U};
    wall.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    wall.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.5F, -0.8F}),
        scene::scaling(scene::Vec3{6.0F, 2.0F, 0.1F}));
    wall.normal = scene::normal_basis(wall.world);
    wall.material.base_color = scene::Vec3{0.8F, 0.8F, 0.8F};
    wall.material.roughness = 1.0F;
    value.instances.push_back(wall);

    render::RendererLimits limits{};
    limits.max_instances = 8U;
    limits.max_vertices = 4096U;
    limits.max_indices = 12288U;
    limits.max_lights = 4U;
    limits.directional_shadow_resolution = 256U;
    limits.max_history_slots = 2U;

    auto created = render::HybridSceneRenderer::create(
        device, extent, extent, limits);
    RENDER_CHECK(created);
    auto renderer = std::move(created).value();

    render::RenderFrameRequest request{};
    request.scene = &value;
    request.slice = scene::whole(value.canvas);
    request.output_width = extent;
    request.output_height = extent;
    request.history.scene = value.id;
    request.history.document_version = value.document_version;
    request.history.surface = render::HistorySurface::Offscreen;
    request.history.canvas = value.canvas;
    request.history.slice = request.slice;
    request.history.width = extent;
    request.history.height = extent;
    request.history.camera_source = value.camera_source;
    request.history.camera_digest = render::camera_history_digest(value.camera);
    request.history.profile_digest =
        render::renderer_profile_digest(value.renderer_settings);

    auto readback = device.create_readback(extent * extent * 4U);
    auto occluded_color = device.create_readback(extent * extent * 8U);
    RENDER_CHECK(readback);
    RENDER_CHECK(occluded_color);
    auto acquired = device.acquire_commands();
    RENDER_CHECK(acquired);
    auto commands = std::move(acquired).value();
    auto products = renderer.render_frame(commands, request);
    RENDER_CHECK(products);
    // The lit colour of the very frame that produced the occlusion, so no
    // difference in state between the two can explain the comparison below.
    RENDER_CHECK(commands.download(*products.value().color,
                                   occluded_color.value()));
    // The product exists only because the scene authored a radius. A scene
    // that did not would publish nothing here, and a consumer that assumed it
    // was always present would read a null.
    RENDER_CHECK(products.value().occlusion != nullptr);
    RENDER_CHECK(products.value().normal != nullptr);
    RENDER_CHECK(commands.download(*products.value().occlusion,
                                   readback.value()));
    auto submitted = device.submit(std::move(commands));
    RENDER_CHECK(submitted);
    RENDER_CHECK(std::move(submitted).value().wait());

    const auto pixels = readback.value().map();
    RENDER_CHECK(pixels.size() >= extent * extent * 4U);
    std::vector<float> occlusion(static_cast<std::size_t>(extent) * extent);
    for (std::size_t index = 0U; index < occlusion.size(); ++index) {
        std::memcpy(&occlusion[index], pixels.data() + index * 4U,
                    sizeof(float));
    }
    readback.value().unmap();

    // Every value is a visibility fraction. A pass that wrote garbage outside
    // the unit range would still pass a "darker here than there" check.
    for (const float value_at : occlusion) {
        RENDER_CHECK(value_at >= 0.0F && value_at <= 1.0F);
    }

    // Average a band just below the crease against a band on the open floor
    // nearer the camera. Bands rather than single pixels, because one pixel
    // could sit on a silhouette and prove nothing either way.
    const auto band_average = [&](core::u32 first_row, core::u32 last_row) {
        double total = 0.0;
        std::size_t counted = 0U;
        for (core::u32 y = first_row; y <= last_row; ++y) {
            for (core::u32 x = extent / 4U; x < (extent * 3U) / 4U; ++x) {
                total += static_cast<double>(
                    occlusion[static_cast<std::size_t>(y) * extent + x]);
                ++counted;
            }
        }
        return counted == 0U ? 1.0 : total / static_cast<double>(counted);
    };

    const double crease = band_average(extent / 2U, extent / 2U + 6U);
    const double open_floor = band_average(extent - 20U, extent - 14U);
    RENDER_CHECK(crease < open_floor);
    // And the open floor is close to unoccluded, so this is occlusion in a
    // corner rather than a global dimmer with a gradient.
    RENDER_CHECK(open_floor > 0.85);

    // With no authored radius the pass does not run at all and publishes
    // nothing, which is what makes it cost exactly zero when unused.
    scene::Scene unoccluded = value;
    unoccluded.renderer_settings.occlusion_radius = 0.0F;
    request.scene = &unoccluded;
    request.history.profile_digest =
        render::renderer_profile_digest(unoccluded.renderer_settings);
    // Created before the recording starts, like every other readback here.
    auto plain_color = device.create_readback(extent * extent * 8U);
    RENDER_CHECK(plain_color);
    auto second = device.acquire_commands();
    RENDER_CHECK(second);
    auto second_list = std::move(second).value();
    auto plain = renderer.render_frame(second_list, request);
    RENDER_CHECK(plain);
    RENDER_CHECK(plain.value().occlusion == nullptr);
    // And the two frames read as colour, so the occlusion is shown to reach
    // the image rather than only the product.
    //
    // This is what the auxiliary reorder bought. Depth and normals used to be
    // written after the colour pass, which meant the lit shader could never
    // read this frame's occlusion; a term that is computed and never applied
    // grounds nothing.
    RENDER_CHECK(second_list.download(*plain.value().color,
                                      plain_color.value()));
    auto plain_submitted = device.submit(std::move(second_list));
    RENDER_CHECK(plain_submitted);
    RENDER_CHECK(std::move(plain_submitted).value().wait());
    std::vector<core::u8> unoccluded_image;
    {
        const auto mapped = plain_color.value().map();
        RENDER_CHECK(mapped.size() >= extent * extent * 8U);
        unoccluded_image.assign(mapped.begin(),
                                mapped.begin() + extent * extent * 8U);
        plain_color.value().unmap();
    }
    std::vector<core::u8> occluded_image;
    {
        const auto mapped = occluded_color.value().map();
        RENDER_CHECK(mapped.size() >= extent * extent * 8U);
        occluded_image.assign(mapped.begin(),
                              mapped.begin() + extent * extent * 8U);
        occluded_color.value().unmap();
    }

    const auto luminance_band = [&](const std::vector<core::u8>& image,
                                    core::u32 first_row, core::u32 last_row) {
        double total = 0.0;
        std::size_t counted = 0U;
        for (core::u32 y = first_row; y <= last_row; ++y) {
            for (core::u32 x = extent / 4U; x < (extent * 3U) / 4U; ++x) {
                const std::size_t index =
                    static_cast<std::size_t>(y) * extent + x;
                total += static_cast<double>(half_lane(image, index, 0U))
                    + static_cast<double>(half_lane(image, index, 1U))
                    + static_cast<double>(half_lane(image, index, 2U));
                ++counted;
            }
        }
        return counted == 0U ? 0.0 : total / static_cast<double>(counted);
    };

    const double crease_lit =
        luminance_band(occluded_image, extent / 2U, extent / 2U + 6U);
    const double crease_plain =
        luminance_band(unoccluded_image, extent / 2U, extent / 2U + 6U);
    const double floor_lit =
        luminance_band(occluded_image, extent - 20U, extent - 14U);
    const double floor_plain =
        luminance_band(unoccluded_image, extent - 20U, extent - 14U);

    // The crease got darker.
    RENDER_CHECK(crease_lit < crease_plain);
    // And by more than the open floor did, which is what makes it occlusion
    // rather than an exposure change. A term that dimmed the whole frame
    // equally would satisfy the first check on its own.
    const double crease_drop = crease_plain - crease_lit;
    const double floor_drop = floor_plain - floor_lit;
    RENDER_CHECK(crease_drop > floor_drop * 2.0);
}

// Fog and bloom are keyed to the frustum a frame was actually rendered from.
//
// The one derivation both the renderer and the compositor call. A wall slice
// takes the canvas-wide matrix and the canvas; a frame rendered from an
// explicit view - an ICVFX plate, a mapped surface - takes that view's own
// matrix and its own raster, whatever the authored camera says. This is the
// regression guard the adversarial review asked for and the first
// two attempts could not discriminate: it states the property directly rather
// than hoping a pixel comparison notices.
void fog_and_bloom_are_keyed_to_the_frustum_they_were_rendered_from() {
    scene::Scene value{};
    value.canvas = scene::Canvas{256U, 128U};
    value.camera.eye = scene::Vec3{0.0F, 1.0F, 5.0F};
    value.camera.target = scene::Vec3{0.0F, 0.5F, 0.0F};
    value.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    value.camera.field_of_view = 0.125F;
    value.camera.near_plane = 0.1F;
    value.camera.far_plane = 50.0F;
    const render::PaddedSlice padded{scene::CanvasRect{64U, 0U, 128U, 128U},
                                     scene::CanvasRect{0U, 0U, 128U, 128U},
                                     0U};

    // The slice path: the canvas-wide matrix, the canvas, the padded
    // rectangle. Nothing about this node's target enters it.
    const render::FogFrame slice =
        render::fog_frame_for(value, nullptr, padded, 128U, 128U);
    RENDER_CHECK(slice.view_projection
                 == scene::view_for_slice(value.camera, value.canvas,
                                          scene::whole(value.canvas))
                        .view_projection);
    RENDER_CHECK(slice.grid_canvas == value.canvas);
    RENDER_CHECK(slice.render == padded.render);
    RENDER_CHECK(slice.clip_planes == (render::SceneClipPlanes{0.1F, 50.0F}));

    // A view-driven frame: the view's own matrix and its own raster. The
    // authored camera is somewhere else entirely and must not be consulted.
    scene::Camera other = value.camera;
    other.eye = scene::Vec3{3.0F, 0.5F, -2.0F};
    other.target = scene::Vec3{0.0F, 0.5F, 0.0F};
    const scene::Canvas raster{96U, 64U};
    const render::SceneRenderView view{
        scene::view_for_slice(other, raster, scene::whole(raster)), other.eye,
        scene::normalize(other.target - other.eye)};
    const render::FogFrame plate =
        render::fog_frame_for(value, &view, padded, 96U, 64U);
    RENDER_CHECK(plate.view_projection == view.matrices.view_projection);
    RENDER_CHECK(plate.view_projection != slice.view_projection);
    RENDER_CHECK(plate.grid_canvas == raster);
    RENDER_CHECK(plate.render == (scene::CanvasRect{0U, 0U, 96U, 64U}));

    RENDER_CHECK(plate.clip_planes == slice.clip_planes);
    auto stage_view = view;
    stage_view.clip_planes = render::SceneClipPlanes{0.05F, 400.0F};
    const auto stage = render::fog_frame_for(value, &stage_view, padded, 96U, 64U);
    RENDER_CHECK(stage.clip_planes == *stage_view.clip_planes);

    // Bloom the same way. A plate is planned on its own raster, and the plan
    // describes the plate's targets rather than the canvas's - planned on the
    // canvas, it described targets the plate never had and refused every
    // plate and every mapped output that authored bloom.
    const auto slice_plan =
        render::bloom_plan_for(value, nullptr, padded, 128U, 128U);
    RENDER_CHECK(slice_plan.has_value());
    RENDER_CHECK(!slice_plan.value().levels.empty());
    RENDER_CHECK(slice_plan.value().levels.front().canvas == padded.render);
    const auto plate_plan =
        render::bloom_plan_for(value, &view, padded, 96U, 64U);
    RENDER_CHECK(plate_plan.has_value());
    RENDER_CHECK(!plate_plan.value().levels.empty());
    RENDER_CHECK(plate_plan.value().levels.front().width == 96U);
    RENDER_CHECK(plate_plan.value().levels.front().height == 64U);
}

// Fog on a view-driven frame uses the view's own matrix.
//
// The GPU half of the guard above. The authored camera looks straight down
// from high above the set; the plate's view is low, looking along the floor.
// A plate fogged through the authored camera's matrix unprojects depths that
// belong to no camera at all and disagrees with the reference - the plate's
// camera authored and rendered through the ordinary slice path - by far more
// than a last-bit difference. With the view's own matrix the two agree.
void fog_on_a_view_driven_frame_uses_the_views_own_matrix() {
    if (!render::HybridSceneRenderer::shaders_available()
        || !render::VolumetricFog::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()
        || !device.capabilities().compute
        || !device.capabilities().storage_images) {
        return;
    }

    constexpr core::u32 width = 128U;
    constexpr core::u32 height = 96U;
    scene::Scene base{};
    base.id = scene::SceneId{601U};
    base.document_version = doc::Version{1U};
    base.camera_source = doc::NodeId{600U};
    base.canvas = scene::Canvas{width, height};
    base.camera.eye = scene::Vec3{0.0F, 12.0F, 0.01F};
    base.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    base.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    base.camera.field_of_view = 0.125F;
    base.camera.near_plane = 0.1F;
    base.camera.far_plane = 60.0F;
    base.renderer_settings.backend = scene::RendererBackend::Advanced;
    base.renderer_settings.required = true;
    // No cascades and no bounce: both fit themselves to the *authored*
    // camera's frustum, which is exactly the thing this test moves, and the
    // question here is about fog alone.
    base.renderer_settings.shadow_cascades = 0U;
    base.renderer_settings.gi_strength = 0.0F;
    base.environment.ambient = 0.6F;
    base.environment.sky_color = scene::Vec3{0.1F, 0.12F, 0.2F};
    base.environment.fog_density = 0.3F;
    base.environment.fog_color = scene::Vec3{0.7F, 0.75F, 0.85F};

    scene::MeshInstance floor{};
    floor.source = doc::NodeId{610U};
    floor.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    floor.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, -0.05F, -15.0F}),
        scene::scaling(scene::Vec3{30.0F, 0.1F, 40.0F}));
    floor.normal = scene::normal_basis(floor.world);
    floor.material.base_color = scene::Vec3{0.7F, 0.7F, 0.72F};
    floor.material.roughness = 1.0F;
    base.instances.push_back(floor);

    scene::MeshInstance backdrop{};
    backdrop.source = doc::NodeId{611U};
    backdrop.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    backdrop.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 6.0F, -30.0F}),
        scene::scaling(scene::Vec3{40.0F, 20.0F, 0.2F}));
    backdrop.normal = scene::normal_basis(backdrop.world);
    backdrop.material.base_color = scene::Vec3{0.6F, 0.62F, 0.68F};
    backdrop.material.roughness = 1.0F;
    base.instances.push_back(backdrop);

    scene::DirectionalLight sun{};
    sun.direction = scene::Vec3{-0.4F, -0.7F, -0.5F};
    sun.color = scene::Vec3{1.0F, 1.0F, 1.0F};
    sun.intensity = 3.0F;
    base.directional_lights.push_back(sun);

    scene::Camera plate_camera = base.camera;
    plate_camera.eye = scene::Vec3{0.0F, 1.2F, 4.0F};
    plate_camera.target = scene::Vec3{0.0F, 0.6F, -6.0F};
    plate_camera.near_plane = 0.05F;
    plate_camera.far_plane = 400.0F;

    render::RendererLimits limits{};
    limits.max_instances = 4U;
    limits.max_vertices = 4096U;
    limits.max_indices = 12288U;
    limits.max_lights = 4U;
    limits.max_history_slots = 2U;

    constexpr core::u64 view_identity = 77U;
    const auto render_once = [&](const scene::Scene& framed_source,
                                 const render::SceneRenderView* view) {
        scene::Scene framed = framed_source;
        framed.show_frame = 1U;
        auto created =
            render::HybridSceneRenderer::create(device, width, height, limits);
        check(created.has_value(), "renderer", __LINE__);
        auto renderer = std::move(created).value();
        auto readback = device.create_readback(width * height * 8U);
        check(readback.has_value(), "readback", __LINE__);

        render::RenderFrameRequest request{};
        request.scene = &framed;
        request.slice = scene::whole(framed.canvas);
        request.output_width = width;
        request.output_height = height;
        if (view != nullptr) {
            request.options.view = *view;
        }
        request.history.scene = framed.id;
        request.history.document_version = framed.document_version;
        request.history.surface = render::HistorySurface::Offscreen;
        request.history.surface_id = 1U;
        request.history.canvas = framed.canvas;
        request.history.slice = request.slice;
        request.history.width = width;
        request.history.height = height;
        request.history.camera_source = framed.camera_source;
        request.history.camera_digest =
            render::camera_history_digest(framed.camera);
        request.history.view_digest = view != nullptr ? view_identity : 0U;
        request.history.profile_digest =
            render::renderer_profile_digest(framed.renderer_settings);

        auto acquired = device.acquire_commands();
        check(acquired.has_value(), "acquire", __LINE__);
        auto commands = std::move(acquired).value();
        auto rendered = renderer.render_frame(commands, request);
        check(rendered.has_value(), "render", __LINE__);
        check(commands.download(*rendered.value().color, readback.value()),
              "download", __LINE__);
        auto submitted = device.submit(std::move(commands));
        check(submitted.has_value(), "submit", __LINE__);
        check(std::move(submitted).value().wait(), "wait", __LINE__);
        std::vector<core::u8> frame;
        const auto pixels = readback.value().map();
        const std::size_t bytes =
            static_cast<std::size_t>(width) * height * 8U;
        check(pixels.size() >= bytes, "map", __LINE__);
        frame.assign(pixels.begin(), pixels.begin() + bytes);
        readback.value().unmap();
        return std::pair<std::vector<core::u8>, render::PassState>{
            std::move(frame), rendered.value().passes.fog};
    };

    // The reference: the plate's camera authored, through the slice path.
    scene::Scene reference_scene = base;
    reference_scene.camera = plate_camera;
    const auto reference = render_once(reference_scene, nullptr);
    RENDER_CHECK(reference.second == render::PassState::Active);

    // The plate: the authored camera stays high above, and the frame is
    // rendered from the plate's view.
    render::SceneRenderView view{
        scene::view_for_slice(plate_camera, base.canvas,
                              scene::whole(base.canvas)),
        plate_camera.eye,
        scene::normalize(plate_camera.target - plate_camera.eye)};
    view.history_identity = view_identity;
    view.clip_planes = render::SceneClipPlanes{plate_camera.near_plane,
                                               plate_camera.far_plane};
    const auto plate = render_once(base, &view);
    RENDER_CHECK(plate.second == render::PassState::Active);

    // The same plate with no atmosphere, so the comparison below is known to
    // be about fog and not about two frames that happen to agree.
    scene::Scene clear_scene = base;
    clear_scene.environment.fog_density = 0.0F;
    const auto clear = render_once(clear_scene, &view);

    double worst_against_reference = 0.0;
    double most_against_clear = 0.0;
    std::size_t changed = 0U;
    for (std::size_t pixel = 0U;
         pixel < static_cast<std::size_t>(width) * height; ++pixel) {
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            const double mine = half_lane(plate.first, pixel, channel);
            worst_against_reference = std::max(
                worst_against_reference,
                std::abs(mine - half_lane(reference.first, pixel, channel)));
            const double difference =
                std::abs(mine - half_lane(clear.first, pixel, channel));
            most_against_clear = std::max(most_against_clear, difference);
            if (channel == 2U && difference > 1.0e-3) {
                ++changed;
            }
        }
    }
    // Fogged - most of the frame, by a real amount.
    RENDER_CHECK(changed > (static_cast<std::size_t>(width) * height) / 4U);
    RENDER_CHECK(most_against_clear > 0.01);
    // And fogged through the plate's own matrix: within a last-bit difference
    // of the reference. Through the authored camera's matrix the disagreement
    // is a large fraction of the fog itself.
    RENDER_CHECK(worst_against_reference <= 1.0 / 1024.0);
}

// An orthographic view is drawn only as an explicit view. Its view vector is
// the same at every pixel, so a glossy floor under a sun shades uniformly where
// the perspective view of it shows a highlight; its atmosphere is reported
// idle with the reason rather than integrated from an eye that stands far
// behind the picture; and a scene whose own camera is orthographic is refused.
void an_orthographic_view_shades_along_the_view_and_idles_fog() {
    const float tolerance = render::orthographic_history_depth_tolerance(
        scene::view_for_slice(
            [] {
                scene::Camera camera{};
                camera.eye = scene::Vec3{0.0F, 0.0F, 10.0F};
                camera.near_plane = 0.5F;
                camera.far_plane = 40.5F;
                camera.orthographic_height = 10.0F;
                return camera;
            }(),
            scene::Canvas{1200U, 900U}, scene::CanvasRect{0U, 0U, 1200U, 900U})
            .projection,
        900U);
    RENDER_CHECK(std::abs(tolerance -
                          (1.0e-5F + 2.0F * (10.0F / 900.0F) / 40.0F)) <=
                 1.0e-7F);
    RENDER_CHECK(render::orthographic_history_depth_tolerance(
                     scene::view_for_slice(scene::Camera{},
                                           scene::Canvas{1200U, 900U},
                                           scene::CanvasRect{0U, 0U, 1200U, 900U})
                         .projection,
                     900U) == 0.0F);
    {
        scene::Camera orthographic{};
        orthographic.orthographic_height = 4.0F;
        const render::RendererReadiness refused = render::readiness_for_camera(
            render::RendererReadiness{}, orthographic);
        RENDER_CHECK(!refused.satisfied);
        RENDER_CHECK(refused.code ==
                     render::RendererReadinessCode::OrthographicCamera);
        RENDER_CHECK(refused.headline() ==
                     std::string_view{"ORTHOGRAPHIC CAMERA NEEDS A STAGE VIEW"});
        RENDER_CHECK(render::readiness_for_camera(render::RendererReadiness{},
                                                  scene::Camera{})
                         .satisfied);
    }

    if (!render::HybridSceneRenderer::shaders_available()
        || !render::VolumetricFog::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()
        || !device.capabilities().compute
        || !device.capabilities().storage_images) {
        return;
    }

    constexpr core::u32 width = 96U;
    constexpr core::u32 height = 96U;
    scene::Scene base{};
    base.id = scene::SceneId{701U};
    base.document_version = doc::Version{1U};
    base.camera_source = doc::NodeId{700U};
    base.canvas = scene::Canvas{width, height};
    base.camera.eye = scene::Vec3{0.0F, 3.0F, 6.0F};
    base.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    base.camera.field_of_view = 0.125F;
    base.camera.near_plane = 0.1F;
    base.camera.far_plane = 100.0F;
    base.renderer_settings.backend = scene::RendererBackend::Advanced;
    base.renderer_settings.required = true;
    base.renderer_settings.shadow_cascades = 0U;
    base.renderer_settings.gi_strength = 0.0F;
    base.renderer_settings.occlusion_radius = 0.0F;
    base.renderer_settings.contact_shadow_length = 0.0F;
    base.environment.ambient = 0.2F;
    base.environment.sky_color = scene::Vec3{0.1F, 0.12F, 0.2F};
    base.environment.fog_density = 0.4F;
    base.environment.fog_color = scene::Vec3{0.7F, 0.75F, 0.85F};

    // A glossy floor far wider than either view.
    scene::MeshInstance floor{};
    floor.source = doc::NodeId{710U};
    floor.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    floor.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, -0.05F, 0.0F}),
        scene::scaling(scene::Vec3{60.0F, 0.1F, 60.0F}));
    floor.normal = scene::normal_basis(floor.world);
    floor.material.base_color = scene::Vec3{0.5F, 0.5F, 0.52F};
    floor.material.roughness = 0.15F;
    floor.material.metallic = 0.6F;
    base.instances.push_back(floor);

    scene::DirectionalLight sun{};
    sun.direction = scene::Vec3{-0.3F, -0.9F, -0.2F};
    sun.intensity = 3.0F;
    base.directional_lights.push_back(sun);

    render::RendererLimits limits{};
    limits.max_instances = 4U;
    limits.max_vertices = 4096U;
    limits.max_indices = 12288U;
    limits.max_lights = 4U;
    limits.max_history_slots = 2U;

    struct Rendered final {
        bool ok{};
        std::vector<core::u8> frame;
        render::RenderProducts products;
        render::RendererReadiness readiness;
    };
    const auto render_once = [&](const scene::Scene& framed_source,
                                 const render::SceneRenderView* view) {
        scene::Scene framed = framed_source;
        framed.show_frame = 1U;
        auto created =
            render::HybridSceneRenderer::create(device, width, height, limits);
        check(created.has_value(), "renderer", __LINE__);
        auto renderer = std::move(created).value();
        auto readback = device.create_readback(width * height * 8U);
        check(readback.has_value(), "readback", __LINE__);
        render::RenderFrameRequest request{};
        request.scene = &framed;
        request.slice = scene::whole(framed.canvas);
        request.output_width = width;
        request.output_height = height;
        if (view != nullptr) {
            request.options.view = *view;
        }
        request.history.scene = framed.id;
        request.history.document_version = framed.document_version;
        request.history.surface = render::HistorySurface::Offscreen;
        request.history.surface_id = 1U;
        request.history.canvas = framed.canvas;
        request.history.slice = request.slice;
        request.history.width = width;
        request.history.height = height;
        request.history.camera_source = framed.camera_source;
        request.history.camera_digest =
            render::camera_history_digest(framed.camera);
        request.history.view_digest = view != nullptr ? 91U : 0U;
        request.history.profile_digest =
            render::renderer_profile_digest(framed.renderer_settings);
        Rendered result{};
        auto acquired = device.acquire_commands();
        check(acquired.has_value(), "acquire", __LINE__);
        auto commands = std::move(acquired).value();
        auto rendered = renderer.render_frame(commands, request);
        result.readiness = renderer.diagnostics().last_readiness;
        if (!rendered) {
            return result;
        }
        result.ok = true;
        result.products = rendered.value();
        check(commands.download(*rendered.value().color, readback.value()),
              "download", __LINE__);
        auto submitted = device.submit(std::move(commands));
        check(submitted.has_value(), "submit", __LINE__);
        check(std::move(submitted).value().wait(), "wait", __LINE__);
        const auto pixels = readback.value().map();
        const std::size_t bytes =
            static_cast<std::size_t>(width) * height * 8U;
        check(pixels.size() >= bytes, "map", __LINE__);
        result.frame.assign(pixels.begin(), pixels.begin() + bytes);
        readback.value().unmap();
        return result;
    };
    const auto spread = [&](const std::vector<core::u8>& frame) {
        double low = 1.0e9;
        double high = -1.0e9;
        for (std::size_t pixel = 0U;
             pixel < static_cast<std::size_t>(width) * height; ++pixel) {
            const double value = half_lane(frame, pixel, 1U);
            low = std::min(low, value);
            high = std::max(high, value);
        }
        return high - low;
    };

    // Looking straight down on the floor from twenty metres, twelve wide.
    scene::Camera top = base.camera;
    top.eye = scene::Vec3{0.0F, 20.0F, 0.0F};
    top.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    top.up = scene::Vec3{0.0F, 0.0F, -1.0F};
    top.near_plane = 0.05F;
    top.far_plane = 40.0F;
    top.orthographic_height = 12.0F;
    const auto view_from = [&](const scene::Camera& camera) {
        render::SceneRenderView view{
            scene::view_for_slice(camera, base.canvas,
                                  scene::whole(base.canvas)),
            camera.eye, scene::normalize(camera.target - camera.eye)};
        view.history_identity = 91U;
        view.clip_planes =
            render::SceneClipPlanes{camera.near_plane, camera.far_plane};
        return view;
    };
    const render::SceneRenderView orthographic_view = view_from(top);
    RENDER_CHECK(scene::orthographic(orthographic_view.matrices.projection));
    const Rendered plan = render_once(base, &orthographic_view);
    RENDER_CHECK(plan.ok);
    RENDER_CHECK(plan.products.passes.fog == render::PassState::Idle);
    RENDER_CHECK(plan.products.passes.fog_refusal ==
                 std::string_view{
                     "atmosphere is not drawn in an orthographic view"});
    // One view vector: the whole floor is one shade.
    RENDER_CHECK(spread(plan.frame) <= 1.0e-3);

    // The same pose in perspective spreads the highlight across the floor,
    // and draws its fog.
    scene::Camera perspective = top;
    perspective.orthographic_height = 0.0F;
    perspective.field_of_view = 0.1F;
    const render::SceneRenderView perspective_view = view_from(perspective);
    const Rendered seen = render_once(base, &perspective_view);
    RENDER_CHECK(seen.ok);
    RENDER_CHECK(seen.products.passes.fog == render::PassState::Active);
    scene::Scene clear = base;
    clear.environment.fog_density = 0.0F;
    const Rendered clear_seen = render_once(clear, &perspective_view);
    RENDER_CHECK(clear_seen.ok);
    RENDER_CHECK(spread(clear_seen.frame) > 0.02);

    // The scene's own camera orthographic, with no view: refused by name.
    scene::Scene authored = base;
    authored.camera = top;
    const Rendered refused = render_once(authored, nullptr);
    RENDER_CHECK(!refused.ok);
    RENDER_CHECK(refused.readiness.code ==
                 render::RendererReadinessCode::OrthographicCamera);
}

// An editor view's shading aids, through the Advanced renderer: Unlit shows
// a surface's base colour whatever the light; Lights only shows the light on
// a neutral grey whatever the colour; Wireframe draws edges and leaves a
// face's interior to the background, and is refused by a renderer not built
// for an editor view. Unlit and Wireframe idle the fog with their reason.
void view_shading_shows_colour_light_or_edges() {
    if (!render::HybridSceneRenderer::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()
        || !device.capabilities().wireframe_fill) {
        return;
    }

    constexpr core::u32 width = 96U;
    constexpr core::u32 height = 96U;
    scene::Scene base{};
    base.id = scene::SceneId{801U};
    base.document_version = doc::Version{1U};
    base.camera_source = doc::NodeId{800U};
    base.canvas = scene::Canvas{width, height};
    base.camera.eye = scene::Vec3{0.0F, 0.0F, 5.0F};
    base.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    base.camera.field_of_view = 0.125F;
    base.camera.near_plane = 0.1F;
    base.camera.far_plane = 50.0F;
    base.renderer_settings.backend = scene::RendererBackend::Advanced;
    base.renderer_settings.required = true;
    base.renderer_settings.shadow_cascades = 0U;
    base.renderer_settings.gi_strength = 0.0F;
    base.renderer_settings.occlusion_radius = 0.0F;
    base.environment.ambient = 0.0F;
    base.environment.sky_color = scene::Vec3{0.0F, 0.0F, 0.0F};

    // A red wall facing the camera, filling the middle of the frame.
    scene::MeshInstance wall{};
    wall.source = doc::NodeId{810U};
    wall.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    wall.world = scene::scaling(scene::Vec3{2.0F, 2.0F, 0.2F});
    wall.normal = scene::normal_basis(wall.world);
    wall.material.base_color = scene::Vec3{0.8F, 0.1F, 0.1F};
    wall.material.roughness = 1.0F;
    base.instances.push_back(wall);
    scene::DirectionalLight sun{};
    sun.direction = scene::Vec3{0.0F, 0.0F, -1.0F};
    sun.intensity = 2.0F;
    base.directional_lights.push_back(sun);

    struct Rendered final {
        bool ok{};
        std::vector<core::u8> frame;
        render::RenderProducts products;
    };
    const auto render_once = [&](const scene::Scene& source,
                                 render::ViewShading shading,
                                 bool editor_view) {
        scene::Scene framed = source;
        framed.show_frame = 1U;
        render::RendererLimits limits{};
        limits.max_instances = 4U;
        limits.max_vertices = 4096U;
        limits.max_indices = 12288U;
        limits.max_lights = 4U;
        limits.max_history_slots = 2U;
        limits.view_shading = editor_view;
        auto created =
            render::HybridSceneRenderer::create(device, width, height, limits);
        check(created.has_value(), "renderer", __LINE__);
        auto renderer = std::move(created).value();
        auto readback = device.create_readback(width * height * 8U);
        check(readback.has_value(), "readback", __LINE__);
        render::SceneRenderView view{
            scene::view_for_slice(framed.camera, framed.canvas,
                                  scene::whole(framed.canvas)),
            framed.camera.eye,
            scene::normalize(framed.camera.target - framed.camera.eye)};
        view.history_identity = 93U;
        view.shading = shading;
        render::RenderFrameRequest request{};
        request.scene = &framed;
        request.slice = scene::whole(framed.canvas);
        request.output_width = width;
        request.output_height = height;
        request.options.view = view;
        request.history.scene = framed.id;
        request.history.document_version = framed.document_version;
        request.history.surface = render::HistorySurface::Offscreen;
        request.history.surface_id = 1U;
        request.history.canvas = framed.canvas;
        request.history.slice = request.slice;
        request.history.width = width;
        request.history.height = height;
        request.history.camera_source = framed.camera_source;
        request.history.camera_digest =
            render::camera_history_digest(framed.camera);
        request.history.view_digest =
            93U ^ (static_cast<core::u64>(shading) * 0x9E37'79B9'7F4A'7C15ULL);
        request.history.profile_digest =
            render::renderer_profile_digest(framed.renderer_settings);
        Rendered result{};
        auto acquired = device.acquire_commands();
        check(acquired.has_value(), "acquire", __LINE__);
        auto commands = std::move(acquired).value();
        auto rendered = renderer.render_frame(commands, request);
        if (!rendered) {
            return result;
        }
        result.ok = true;
        result.products = rendered.value();
        check(commands.download(*rendered.value().color, readback.value()),
              "download", __LINE__);
        auto submitted = device.submit(std::move(commands));
        check(submitted.has_value(), "submit", __LINE__);
        check(std::move(submitted).value().wait(), "wait", __LINE__);
        const auto pixels = readback.value().map();
        const std::size_t bytes =
            static_cast<std::size_t>(width) * height * 8U;
        check(pixels.size() >= bytes, "map", __LINE__);
        result.frame.assign(pixels.begin(), pixels.begin() + bytes);
        readback.value().unmap();
        return result;
    };
    // The wall's middle, and a ring of pixels inside its face but clear of
    // its edges.
    const std::size_t centre = (height / 2U) * width + width / 2U;
    const auto channel = [&](const Rendered& r, std::size_t pixel,
                             std::size_t lane) {
        return static_cast<double>(half_lane(r.frame, pixel, lane));
    };

    scene::Scene brighter = base;
    brighter.directional_lights.front().intensity = 6.0F;
    scene::Scene green = base;
    green.instances.front().material.base_color =
        scene::Vec3{0.1F, 0.8F, 0.1F};

    // Lit follows the light.
    const Rendered lit = render_once(base, render::ViewShading::Lit, false);
    const Rendered lit_bright =
        render_once(brighter, render::ViewShading::Lit, false);
    RENDER_CHECK(lit.ok && lit_bright.ok);
    RENDER_CHECK(channel(lit_bright, centre, 0U) >
                 channel(lit, centre, 0U) * 2.0);

    // Unlit: the base colour itself, whatever the light.
    const Rendered unlit = render_once(base, render::ViewShading::Unlit, true);
    const Rendered unlit_bright =
        render_once(brighter, render::ViewShading::Unlit, true);
    RENDER_CHECK(unlit.ok && unlit_bright.ok);
    RENDER_CHECK(std::abs(channel(unlit, centre, 0U) - 0.8) <= 0.01);
    RENDER_CHECK(std::abs(channel(unlit, centre, 1U) - 0.1) <= 0.01);
    RENDER_CHECK(std::abs(channel(unlit_bright, centre, 0U) -
                          channel(unlit, centre, 0U)) <= 1.0e-3);

    // Lights only: grey whatever the colour, brighter with the light.
    const Rendered lights =
        render_once(base, render::ViewShading::LightsOnly, true);
    const Rendered lights_green =
        render_once(green, render::ViewShading::LightsOnly, true);
    const Rendered lights_bright =
        render_once(brighter, render::ViewShading::LightsOnly, true);
    RENDER_CHECK(lights.ok && lights_green.ok && lights_bright.ok);
    RENDER_CHECK(std::abs(channel(lights, centre, 0U) -
                          channel(lights, centre, 1U)) <= 1.0e-3);
    RENDER_CHECK(std::abs(channel(lights, centre, 0U) -
                          channel(lights_green, centre, 0U)) <= 1.0e-3);
    RENDER_CHECK(channel(lights, centre, 0U) > 0.05);
    RENDER_CHECK(channel(lights_bright, centre, 0U) >
                 channel(lights, centre, 0U) * 2.0);

    // Wireframe: inside the face, clear of the diagonals that split its
    // triangles, is the black background, while edges are drawn elsewhere.
    const Rendered edges =
        render_once(base, render::ViewShading::Wireframe, true);
    RENDER_CHECK(edges.ok);
    RENDER_CHECK(channel(edges, centre + 12U, 0U) <= 1.0e-3);
    RENDER_CHECK(channel(lit, centre + 12U, 0U) > 0.01);
    std::size_t drawn = 0U;
    for (std::size_t pixel = 0U;
         pixel < static_cast<std::size_t>(width) * height; ++pixel) {
        drawn += channel(edges, pixel, 0U) > 0.1 ? 1U : 0U;
    }
    RENDER_CHECK(drawn > 40U);
    RENDER_CHECK(drawn < static_cast<std::size_t>(width) * height / 4U);
    // Not built for an editor view: refused rather than drawn lit.
    RENDER_CHECK(!render_once(base, render::ViewShading::Wireframe, false).ok);

    // Fog stays out of Unlit and Wireframe, with the reason.
    scene::Scene foggy = base;
    foggy.environment.fog_density = 0.4F;
    const Rendered foggy_unlit =
        render_once(foggy, render::ViewShading::Unlit, true);
    RENDER_CHECK(foggy_unlit.ok);
    RENDER_CHECK(foggy_unlit.products.passes.fog == render::PassState::Idle);
    RENDER_CHECK(foggy_unlit.products.passes.fog_refusal ==
                 std::string_view{
                     "atmosphere is drawn only in Lit and Lights only views"});
}

// Fog settles below its height.
//
// With a falloff authored the air above the base height thins, so the rows of
// the frame above the horizon - which see only air - carry far less atmosphere
// than they do with uniform fog, while the rows looking down the floor still
// carry some. Uniform is the default, and the uniform render is what every
// scene had before the parameter existed.
void fog_settles_below_its_height() {
    if (!render::HybridSceneRenderer::shaders_available()
        || !render::VolumetricFog::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()
        || !device.capabilities().compute
        || !device.capabilities().storage_images) {
        return;
    }

    constexpr core::u32 width = 128U;
    constexpr core::u32 height = 96U;
    scene::Scene value{};
    value.id = scene::SceneId{701U};
    value.document_version = doc::Version{1U};
    value.camera_source = doc::NodeId{700U};
    value.canvas = scene::Canvas{width, height};
    value.camera.eye = scene::Vec3{0.0F, 1.2F, 4.0F};
    value.camera.target = scene::Vec3{0.0F, 1.0F, 0.0F};
    value.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    value.camera.field_of_view = 0.125F;
    value.camera.near_plane = 0.1F;
    value.camera.far_plane = 60.0F;
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.required = true;
    value.environment.sky_color = scene::Vec3{0.05F, 0.08F, 0.16F};

    scene::MeshInstance floor{};
    floor.source = doc::NodeId{710U};
    floor.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    floor.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, -0.05F, -20.0F}),
        scene::scaling(scene::Vec3{20.0F, 0.1F, 40.0F}));
    floor.normal = scene::normal_basis(floor.world);
    floor.material.base_color = scene::Vec3{0.7F, 0.7F, 0.72F};
    floor.material.roughness = 1.0F;
    value.instances.push_back(floor);

    scene::DirectionalLight sun{};
    sun.direction = scene::Vec3{-0.3F, -0.7F, -0.6F};
    sun.color = scene::Vec3{1.0F, 0.98F, 0.95F};
    sun.intensity = 4.0F;
    value.directional_lights.push_back(sun);

    render::RendererLimits limits{};
    limits.max_instances = 4U;
    limits.max_vertices = 4096U;
    limits.max_indices = 12288U;
    limits.max_lights = 4U;
    limits.max_history_slots = 2U;

    const auto render_once = [&](float density, float falloff,
                                 core::u64 surface) {
        scene::Scene framed = value;
        framed.show_frame = 1U;
        framed.environment.fog_density = density;
        framed.environment.fog_color = scene::Vec3{0.7F, 0.75F, 0.85F};
        framed.environment.fog_height = 0.0F;
        framed.environment.fog_height_falloff = falloff;
        auto created =
            render::HybridSceneRenderer::create(device, width, height, limits);
        check(created.has_value(), "renderer", __LINE__);
        auto renderer = std::move(created).value();
        auto readback = device.create_readback(width * height * 8U);
        check(readback.has_value(), "readback", __LINE__);
        render::RenderFrameRequest request{};
        request.scene = &framed;
        request.slice = scene::whole(framed.canvas);
        request.output_width = width;
        request.output_height = height;
        request.history.scene = framed.id;
        request.history.document_version = framed.document_version;
        request.history.surface = render::HistorySurface::Offscreen;
        request.history.surface_id = surface;
        request.history.canvas = framed.canvas;
        request.history.slice = request.slice;
        request.history.width = width;
        request.history.height = height;
        request.history.camera_source = framed.camera_source;
        request.history.camera_digest =
            render::camera_history_digest(framed.camera);
        request.history.profile_digest =
            render::renderer_profile_digest(framed.renderer_settings);
        auto acquired = device.acquire_commands();
        check(acquired.has_value(), "acquire", __LINE__);
        auto commands = std::move(acquired).value();
        auto rendered = renderer.render_frame(commands, request);
        check(rendered.has_value(), "render", __LINE__);
        check(commands.download(*rendered.value().color, readback.value()),
              "download", __LINE__);
        auto submitted = device.submit(std::move(commands));
        check(submitted.has_value(), "submit", __LINE__);
        check(std::move(submitted).value().wait(), "wait", __LINE__);
        std::vector<core::u8> frame;
        const auto pixels = readback.value().map();
        const std::size_t bytes =
            static_cast<std::size_t>(width) * height * 8U;
        check(pixels.size() >= bytes, "map", __LINE__);
        frame.assign(pixels.begin(), pixels.begin() + bytes);
        readback.value().unmap();
        return frame;
    };

    const auto clear = render_once(0.0F, 0.0F, 1U);
    const auto uniform = render_once(0.35F, 0.0F, 2U);
    const auto settled = render_once(0.35F, 3.0F, 3U);

    // Mean atmosphere over a band of rows: how far the fogged frame moved
    // from the clear one, in the blue lane where the fog colour is strongest.
    const auto atmosphere = [&](const std::vector<core::u8>& fogged,
                                core::u32 first_row, core::u32 last_row) {
        double total = 0.0;
        std::size_t counted = 0U;
        for (core::u32 y = first_row; y < last_row; ++y) {
            for (core::u32 x = 0U; x < width; ++x) {
                const std::size_t pixel =
                    static_cast<std::size_t>(y) * width + x;
                total += std::abs(half_lane(fogged, pixel, 2U)
                                  - half_lane(clear, pixel, 2U));
                ++counted;
            }
        }
        return total / static_cast<double>(counted);
    };
    // Above the horizon there is only air, and settled air up there is thin.
    const double uniform_sky = atmosphere(uniform, 0U, height / 6U);
    const double settled_sky = atmosphere(settled, 0U, height / 6U);
    RENDER_CHECK(uniform_sky > 0.01);
    RENDER_CHECK(settled_sky < uniform_sky * 0.5);
    // Down the floor the settled fog is still there, and thicker than above.
    const double settled_floor =
        atmosphere(settled, (height * 5U) / 6U, height);
    RENDER_CHECK(settled_floor > settled_sky);
}

// Two of these renderers stand behind every output a node drives, and each
// one used to build the Advanced renderer whether or not the show had any use
// for it - full-extent targets, cascade maps, a shadow atlas and a temporal
// history, held for the renderer's life. A node on a six-gigabyte card had
// nothing left for media before a single plate was decoded, and choosing
// Basic in the project bought back none of it, because the only question ever
// asked was whether the *device* could run Advanced.
//
// So: a show authored on Basic leaves it unbuilt, a show that authors
// Advanced is still answered with Advanced without its owner doing anything
// new, and an owner that knows the policy early can pay for it early.
void a_basic_show_builds_no_advanced_renderer() {
    if (!render::HybridSceneRenderer::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()
        || !device.capabilities().compute
        || !device.capabilities().storage_images) {
        return;
    }

    constexpr core::u32 width = 96U;
    constexpr core::u32 height = 64U;
    render::RendererLimits limits{};
    limits.max_instances = 8U;
    limits.max_vertices = 1024U;
    limits.max_indices = 4096U;
    limits.max_history_slots = 2U;
    limits.max_temporal_history_targets = 1U;
    limits.directional_shadow_resolution = 16U;
    limits.shadow_atlas.extent = 32U;

    scene::Scene base{};
    base.id = scene::SceneId{911U};
    base.document_version = doc::Version{1U};
    base.camera_source = doc::NodeId{910U};
    base.canvas = scene::Canvas{width, height};
    base.camera.eye = scene::Vec3{0.0F, 0.0F, 3.0F};
    base.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    base.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    base.camera.field_of_view = 0.25F;
    base.camera.near_plane = 0.1F;
    base.camera.far_plane = 20.0F;
    base.environment.ambient = 0.5F;
    base.renderer_settings.shadow_cascades = 0U;
    base.renderer_settings.gi_strength = 0.0F;
    const auto slice = scene::whole(base.canvas);

    const auto draw = [&](render::HybridSceneRenderer& renderer,
                          const scene::Scene& value,
                          scene::RendererBackend expected) {
        auto acquired = device.acquire_commands();
        RENDER_CHECK(acquired);
        auto commands = std::move(acquired).value();
        auto products = renderer.render_frame(
            commands, renderer.convenience_request(value, slice, {}));
        RENDER_CHECK(products);
        RENDER_CHECK(products.value().readiness.selected == expected);
        // Which renderer actually drew, rather than the same readiness field
        // asserted twice: only the Advanced renderer works scene-linear, and
        // this is read back off the renderer that recorded the frame.
        RENDER_CHECK(products.value().scene_linear
                     == (expected == scene::RendererBackend::Advanced));
        auto submitted = device.submit(std::move(commands));
        RENDER_CHECK(submitted);
        RENDER_CHECK(std::move(submitted).value().wait());
    };

    {
        auto created =
            render::HybridSceneRenderer::create(device, width, height, limits);
        RENDER_CHECK(created);
        auto renderer = std::move(created).value();
        RENDER_CHECK(!renderer.diagnostics().advanced_resident);
        auto value = base;
        value.renderer_settings.backend = scene::RendererBackend::Basic;
        draw(renderer, value, scene::RendererBackend::Basic);
        // A whole frame through the renderer and still nothing built. The
        // planar target belongs to Advanced too, so it is absent with it.
        RENDER_CHECK(!renderer.diagnostics().advanced_resident);
        RENDER_CHECK(renderer.planar_target() == nullptr);
    }

    {
        auto created =
            render::HybridSceneRenderer::create(device, width, height, limits);
        RENDER_CHECK(created);
        auto renderer = std::move(created).value();
        RENDER_CHECK(!renderer.diagnostics().advanced_resident);
        auto value = base;
        value.renderer_settings.backend = scene::RendererBackend::Advanced;
        value.renderer_settings.required = true;
        // Nothing prepared it: the first frame asks and is answered.
        draw(renderer, value, scene::RendererBackend::Advanced);
        RENDER_CHECK(renderer.diagnostics().advanced_resident);
        // And a second frame reuses what the first built.
        draw(renderer, value, scene::RendererBackend::Advanced);
        RENDER_CHECK(renderer.diagnostics().advanced_resident);
    }

    {
        auto created =
            render::HybridSceneRenderer::create(device, width, height, limits);
        RENDER_CHECK(created);
        auto renderer = std::move(created).value();
        scene::RendererSettings settings{};
        settings.backend = scene::RendererBackend::Basic;
        renderer.prepare(settings);
        RENDER_CHECK(!renderer.diagnostics().advanced_resident);
        settings.backend = scene::RendererBackend::Advanced;
        renderer.prepare(settings);
        RENDER_CHECK(renderer.diagnostics().advanced_resident);
        RENDER_CHECK(renderer.readiness(settings).selected
                     == scene::RendererBackend::Advanced);
        // Idempotent: asking again is not a second allocation.
        renderer.prepare(settings);
        RENDER_CHECK(renderer.diagnostics().advanced_resident);
    }

    // The case that must not be optimised away. A compositor draws the sky
    // behind its layers through the Advanced renderer whatever backend the
    // show selected, so a Basic show that authored one still needs it built
    // - and a Basic show that authored no sky is the only one that may go
    // without. Each marker is checked on its own, against a renderer that
    // has seen nothing else.
    const auto resident_for = [&](const scene::Scene& value) {
        auto created =
            render::HybridSceneRenderer::create(device, width, height, limits);
        RENDER_CHECK(created);
        auto renderer = std::move(created).value();
        renderer.prepare(value);
        return renderer.diagnostics().advanced_resident;
    };
    auto plain = base;
    plain.renderer_settings.backend = scene::RendererBackend::Basic;
    RENDER_CHECK(!resident_for(plain));
    auto mapped = plain;
    mapped.environment.map_source = "environment/dome.exr";
    RENDER_CHECK(resident_for(mapped));
    auto sky = plain;
    sky.environment.physical_sky.enabled = true;
    RENDER_CHECK(resident_for(sky));

    // The same bargain one level down. The planar mirror is a second complete
    // renderer at this renderer's extent - it very nearly doubles an Advanced
    // one, 559 MiB at 1080p and 1450 MiB at 4K - and the scene here has no
    // reflective surface. Built from the authored flag rather than from a
    // resolved receiver, for the same reason the sky is: what a frame can
    // find must not decide what exists.
    const auto mirror_resident_for = [&](const scene::Scene& value) {
        auto created =
            render::HybridSceneRenderer::create(device, width, height, limits);
        RENDER_CHECK(created);
        auto renderer = std::move(created).value();
        renderer.prepare(value);
        return renderer.diagnostics().planar_resident;
    };
    auto deep = base;
    deep.renderer_settings.backend = scene::RendererBackend::Advanced;
    deep.renderer_settings.required = true;
    RENDER_CHECK(resident_for(deep));
    RENDER_CHECK(!mirror_resident_for(deep));
    auto mirrored = deep;
    mirrored.environment.planar.enabled = true;
    mirrored.environment.planar.receiver = doc::NodeId{912U};
    RENDER_CHECK(mirror_resident_for(mirrored));

    // A Basic show whose sky has already built the Advanced renderer still
    // gets no mirror. The pass it would feed needs a scene-linear renderer
    // and is refused outright on Basic, so paying for it would buy nothing.
    auto sky_and_mirror = mirrored;
    sky_and_mirror.renderer_settings.backend = scene::RendererBackend::Basic;
    sky_and_mirror.renderer_settings.required = false;
    sky_and_mirror.environment.map_source = "environment/dome.exr";
    RENDER_CHECK(resident_for(sky_and_mirror));
    RENDER_CHECK(!mirror_resident_for(sky_and_mirror));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array cases{
    Case{"a basic show builds no advanced renderer",
         &a_basic_show_builds_no_advanced_renderer},
    Case{"photometric lights reach pixels and slices", &photometric_lights_reach_pixels_and_slices},
    Case{"temporal history holds repeats and resets discontinuities",
         &temporal_history_holds_repeats_and_resets_discontinuities},
    Case{"temporal antialiasing holds repeats and reports restarts",
         &temporal_antialiasing_holds_repeats_and_reports_restarts},
    Case{"occlusion darkens a crease and not an open plane",
         &occlusion_darkens_a_crease_and_not_an_open_plane},
    Case{"a spot lights a cone and a point does not",
         &a_spot_lights_a_cone_and_a_point_does_not},
    Case{"two nodes and one node agree byte for byte",
         &two_nodes_and_one_node_agree_byte_for_byte},
    Case{"a bright block blooms into a symmetric halo",
         &a_bright_block_blooms_into_a_symmetric_halo},
    Case{"a bloom chain past the apron budget is refused",
         &a_bloom_chain_past_the_apron_budget_is_refused},
    Case{"authored bloom changes the published frame",
         &authored_bloom_changes_the_published_frame},
    Case{"the renderer reports which passes ran",
         &the_renderer_reports_which_passes_ran},
    Case{"ray traced reflections fail closed",
         &ray_traced_reflections_fail_closed},
    Case{"authored fog changes the published frame",
         &authored_fog_changes_the_published_frame},
    Case{"a practical lights the air and a wall stops it",
         &a_practical_lights_the_air_and_a_wall_stops_it},
    Case{"fog and bloom are keyed to the frustum they were rendered from",
         &fog_and_bloom_are_keyed_to_the_frustum_they_were_rendered_from},
    Case{"fog on a view driven frame uses the views own matrix",
         &fog_on_a_view_driven_frame_uses_the_views_own_matrix},
    Case{"fog settles below its height", &fog_settles_below_its_height},
    Case{"an orthographic view shades along the view and idles fog",
         &an_orthographic_view_shades_along_the_view_and_idles_fog},
    Case{"view shading shows colour, light or edges",
         &view_shading_shows_colour_light_or_edges},
    Case{"a spot does not shine through a wall",
         &a_spot_does_not_shine_through_a_wall},
    Case{"atlas assignment ignores the camera and a floor does not self shadow",
         &atlas_assignment_ignores_the_camera_and_a_floor_does_not_self_shadow},
    Case{"deterministic samples use absolute canvas identity",
         &deterministic_samples_use_absolute_canvas_identity},
    Case{"slice apron expands clamps and preserves crop",
         &slice_apron_expands_clamps_and_preserves_crop},
    Case{"the advanced frame declaration compiles with its hazards",
         &the_advanced_frame_declaration_compiles_with_its_hazards},
    Case{"a graph past capacity is refused", &a_graph_past_capacity_is_refused},
    Case{"graph states map to resource states",
         &graph_states_map_to_resource_states},
    Case{"the graph executor refuses before it records",
         &the_graph_executor_refuses_before_it_records},
    Case{"a graph executed frame matches the hand recorded one",
         &a_graph_executed_frame_matches_the_hand_recorded_one},
    Case{"the transient pool reuses and refuses",
         &the_transient_pool_reuses_and_refuses},
    Case{"render graph compiles hazards and queue handoffs",
         &render_graph_compiles_hazards_and_queue_handoffs},
    Case{"scene bridge is stable bounded and keeps previous transform",
         &scene_bridge_is_stable_bounded_and_keeps_previous_transform},
    Case{"advanced policy fails closed and optional preview falls back",
         &advanced_policy_fails_closed_and_optional_preview_falls_back},
    Case{"native compositor exposes only Advanced environment",
         &native_compositor_exposes_only_advanced_environment},
    Case{"native advanced raster emits the complete linear product set",
         &native_advanced_raster_emits_the_complete_linear_product_set},
    Case{"native directional shadow and diffuse bounce are slice deterministic",
         &native_directional_shadow_and_diffuse_bounce_are_slice_deterministic},
    Case{"native shadow cascades are authored and slice deterministic",
         &native_shadow_cascades_are_authored_and_slice_deterministic},
    Case{"a batch heavy advanced frame fits the fixed descriptor budget",
         &a_batch_heavy_advanced_frame_fits_the_fixed_descriptor_budget},
};

} // namespace

int main(int argc, char** argv) {
    const std::string_view filter =
        argc == 2 && argv[1] != nullptr ? std::string_view{argv[1]}
                                       : std::string_view{};
    std::size_t failures{};
    std::size_t executed{};
    for (const Case& test : cases) {
        if (!filter.empty() && std::string_view{test.name} != filter) {
            continue;
        }
        ++executed;
        try {
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    if (executed == 0U) {
        std::cerr << "[fail] no test matched filter: " << filter << '\n';
        return 1;
    }
    std::cout << executed - failures << '/' << executed
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
