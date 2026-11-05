#include <mediaserver/render/particle_pass.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/particle_sprite_frag_dxil.h>
#include <mediaserver/shaders/particle_sprite_vert_dxil.h>
#include <mediaserver/shaders/particle_lights_comp_dxil.h>
#include <mediaserver/shaders/particle_velocity_frag_dxil.h>
#include <mediaserver/shaders/particle_volume_frag_dxil.h>
#include <mediaserver/shaders/particle_volume_vert_dxil.h>
#endif

#include <ajfx/cache.hpp>
#include <ajfx/rigid.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <bit>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <tuple>

namespace mediaserver::render {
namespace {

constexpr core::u32 absent = 0xffffffffU;

// Fragment textures a sprite draw binds: the texture, the scene depth, the
// six-way lightmap and four shadow cascades (particle_sprite_shade.hlsli).
constexpr core::u32 sprite_texture_slots = 7U;

// Matches the ParticleView constant buffer in particle_sprite.vert.hlsl.
struct ViewConstants final {
    std::array<float, 16U> view_projection{}; // column-major, as scene::Mat4
    std::array<float, 16U> world{};           // object to world, four columns
    std::array<float, 4U> camera_right{};
    std::array<float, 4U> camera_up{};
    std::array<float, 4U> camera_forward{};
    core::u32 stride{};
    core::u32 position_offset{absent};
    core::u32 velocity_offset{absent};
    core::u32 color_offset{absent};
    core::u32 size_offset{absent};
    core::u32 rotation_offset{absent};
    core::u32 facing_velocity{};
    core::u32 premultiplied{};
    // x: stretch, y: 1 for additive light, z: orientation offset bits,
    // w: the renderer's size scale, times the body kind's for a mesh draw
    std::array<float, 4U> extra{};
    core::u32 age_offset{absent};
    core::u32 lifetime_offset{absent};
    core::u32 flipbook{};
    core::u32 flags{};
    std::array<float, 16U> canvas_view_projection{};
    std::array<float, 16U> previous_view_projection{};
    std::array<float, 4U> motion{}; // x: seconds back, y: 1 when valid, z: soft distance
    std::array<float, 4U> depth_terms{}; // projection (2,2), (2,3), (3,2), (3,3)
    std::array<float, 4U> draw_tint{1.0F, 1.0F, 1.0F, 1.0F};
    // The scene's light, for lit emitters (see ParticleLighting).
    std::array<float, 4U> sky_ambient{};     // rgb sky, a: ambient strength
    std::array<float, 4U> ground_exposure{}; // rgb ground, a: exposure
    std::array<float, 4U> look{};            // curvature, wrap, ambient, shadow bias in texels
    // x: lights in the light buffer. y: shadow cascades. z: the emitter's
    // lighting (0 unlit, 1 lit, 2 six-way). w: 1 when it takes shadows.
    std::array<core::u32, 4U> lighting{};
    std::array<std::array<float, 16U>, 4U> shadow_view_projection{};
    std::array<float, 4U> shadow_texel_depth{};
    // x: for one depth slice of an interleaved emitter, the word of the
    // order that holds where the slice starts in it; absent otherwise.
    std::array<core::u32, 4U> order_slice{absent, 0U, 0U, 0U};
};
static_assert(sizeof(ViewConstants) == 768U);

// Matches ParticleVolumeView in particle_volume.vert.hlsl.
struct VolumeViewConstants final {
    std::array<float, 16U> view_projection{};
    std::array<float, 16U> world{};
    std::array<float, 4U> box_origin{};
    std::array<float, 4U> box_size{};
};
static_assert(sizeof(VolumeViewConstants) == 160U);

// Matches ParticleVolume in particle_volume.frag.hlsl.
struct VolumeConstants final {
    std::array<float, 16U> world{};
    std::array<float, 16U> inverse{};
    std::array<float, 4U> eye{};            // w: 1 perspective, 0 orthographic
    std::array<float, 4U> camera_forward{};
    std::array<float, 4U> depth_terms{};
    std::array<float, 4U> grid_origin{};    // xyz origin, w cell size
    std::array<core::u32, 4U> grid_cells{}; // xyz cells, w first density word
    std::array<float, 4U> march{};          // extinction, albedo, steps, light steps
    std::array<float, 4U> color{};
    std::array<float, 4U> sky_ambient{};
    std::array<float, 4U> ground_exposure{};
    std::array<core::u32, 4U> lighting{};   // lights, cascades, shadowed, particle lights
    std::array<std::array<float, 16U>, 4U> shadow_view_projection{};
    std::array<float, 4U> shadow_texel_depth{};
    std::array<float, 4U> shadow_bias{};
};
static_assert(sizeof(VolumeConstants) == 576U);

// Steps the volume raymarch takes along a view ray, and from each of them
// towards the light: the reference renderer's counts, so the two draw the
// same smoke, and fixed, so every node draws the same frame.
constexpr float volume_ray_steps = 64.0F;
constexpr float volume_light_steps = 8.0F;

// ViewConstants::flags.
constexpr core::u32 flag_textured = 1U;
constexpr core::u32 flag_blend_cells = 2U;
constexpr core::u32 flag_srgb = 4U;
constexpr core::u32 flag_sorted = 8U;
constexpr core::u32 flag_soft = 16U;
constexpr core::u32 flag_ribbon = 32U;
constexpr core::u32 flag_mesh = 64U;
constexpr core::u32 flag_additive = 128U;

// A prop's record, laid out for the sprite vertex stage to pull exactly as it
// pulls a particle's: a prop is drawn through the mesh-particle path, so a
// prop and the debris around it are lit alike. The pose comes from the rigid
// frame, the size from what the artist authored, the colour is plain white
// (a prop carries none), and `velocity` is the pose's own, which gives the
// motion vector the same way a particle's does.
struct PropRecord final {
    std::array<float, 3U> position{};
    float size{};
    std::array<float, 3U> velocity{};
    float unused{};
    std::array<float, 4U> orientation{}; // x, y, z, w, as the simulation stores one
    std::array<float, 4U> color{};
};
static_assert(sizeof(PropRecord) == 64U);
constexpr core::u32 prop_position_at = 0U;
constexpr core::u32 prop_size_at = 12U;
constexpr core::u32 prop_velocity_at = 16U;
constexpr core::u32 prop_orientation_at = 32U;
constexpr core::u32 prop_color_at = 48U;

core::u32 offset_or_absent(std::int32_t offset) {
    return offset >= 0 ? static_cast<core::u32>(offset) : absent;
}

ParticleError failure(ParticleErrorCode code, std::string message) {
    return ParticleError{code, std::move(message)};
}

} // namespace

// ------------------------------------------------------------ instance

struct ParticleInstance::State final {
    ajfx::Effect effect;
    std::optional<ajfx::gpu::GpuSimulation> simulation;
    // Checkpoint slots, made at creation and reused; `saved` holds the step
    // each one has, if any.
    std::vector<ajfx::gpu::GpuCheckpoint> checkpoints;
    std::vector<std::optional<core::u64>> saved;
    core::u64 target{0U};
    // Set by restart: the next advance resets the simulation first.
    bool restart{};
    // What the artist does to it (revise), and the earliest tick revised
    // since the last advance: a simulation past it goes back.
    std::shared_ptr<const ajfx::ActionProvider> actions;
    std::optional<core::u64> revised;
    // The cache it plays (create_instance), and which emitters the runner's
    // frames carry: the rigid physics ones and, with a cache, every one it
    // holds.
    std::shared_ptr<const ajfx::Cache> cache;
    std::vector<bool> hosted;
    std::optional<ajfx::gpu::ParticleSorter> sorter;
    // One per texture name, staged a recording's upload space at a time;
    // the texels (not the size or colour space) are dropped once they have
    // all been recorded.
    struct Upload final {
        rhi::Texture texture;
        ajfx::TextureData data;
        core::u32 mip{0U};
        core::u32 row{0U};
        bool landed{};
    };
    std::vector<Upload> textures;
    // Each emitter's index in `textures`, if it has one, and its six-way
    // lightmap's.
    std::vector<std::optional<std::size_t>> emitter_texture;
    std::vector<std::optional<std::size_t>> emitter_lightmap;
    // Each emitter's drawing order from the last draw, for its motion.
    std::vector<ajfx::gpu::Buffer> orders;
    // What each emitter's particles are multiplied by as they are drawn (the
    // renderer's tint, brightness, opacity and size scale), resolved against
    // the parameters at the step the simulation stands at. The simulation
    // never reads them, so a cache or a checkpoint carries none of them.
    std::vector<ajfx::DrawAdjustment> adjustments;
    // One package mesh on the GPU: its vertices interleaved as the sprite
    // stage pulls them (position, normal, uv) and its triangle list. Keyed by
    // mesh name rather than by emitter, because several emitters and props may
    // name one mesh and a prop's mesh need not be its emitter's. The vertices
    // and indices wait here until the first advance writes them.
    struct MeshUpload final {
        std::string name;
        ajfx::gpu::Buffer vertices;
        ajfx::gpu::Buffer indices;
        core::u32 index_count{};
        std::vector<float> unwritten_vertices;
        std::vector<std::uint32_t> unwritten_indices;
    };
    std::vector<MeshUpload> mesh_uploads;
    // Each mesh emitter's mesh and draw arguments (the mesh's index count,
    // then the live count copied from the simulation).
    struct Mesh final {
        std::size_t upload{};
        ajfx::gpu::Buffer args;
        // A rigid physics emitter whose body kinds name models of their own
        // draws a group per model rather than one draw for the whole
        // emitter, so a crate's panels draw as panels. A group's order
        // buffer lists which of the emitter's particles are drawn with its
        // model, and the count is this frame's: these particles come from
        // the CPU, which therefore knows it without asking the GPU. Empty
        // for an emitter drawn with one model, which keeps its indirect
        // draw.
        struct Group final {
            std::size_t upload{};
            ajfx::gpu::Buffer order;
            // Written each time, allocated once: never on the frame path.
            std::vector<std::uint32_t> scratch;
            core::u32 count{0U};
            // The body kind's own size scale, which the draw multiplies each
            // particle's size by. Kinds sharing a model at different scales
            // are different groups, since the scale is the draw's.
            float scale{1.0F};
            ajfx::Color tint{};
        };
        std::vector<Group> groups;
        // The group each body kind draws with, by kind. Empty when the
        // emitter draws everything with one model.
        std::vector<std::size_t> kind_group;
    };
    std::vector<std::optional<Mesh>> meshes;
    // The props drawn with one mesh, in the order the effect lists them, and
    // a record each, written whenever the rigid frame moves. A prop that
    // collides as a generated shape names no model, so it is not here: it is
    // a volume the artist gave no geometry to draw.
    struct PropGroup final {
        std::size_t upload{};
        ajfx::gpu::Buffer records;
        // Which prop each record is: its emitter, then its index in that
        // emitter's props.
        std::vector<std::pair<std::size_t, std::size_t>> props;
        // Written each time, allocated once: never on the frame path.
        std::vector<PropRecord> scratch;
    };
    std::vector<PropGroup> props;
    // Its rigid physics on the pass's runner, if it has any, and the step
    // whose particles the buffers hold (none since the simulation last moved).
    std::unique_ptr<ajfx::RigidTrack> rigid;
    std::optional<core::u64> rigid_written;
    ParticleDevice* device{};

    ~State() {
        if (device != nullptr) {
            for (const MeshUpload& upload : mesh_uploads) {
                device->destroy_buffer(upload.vertices);
                device->destroy_buffer(upload.indices);
            }
            for (const std::optional<Mesh>& mesh : meshes) {
                if (mesh) {
                    device->destroy_buffer(mesh->args);
                }
            }
            for (const PropGroup& group : props) {
                device->destroy_buffer(group.records);
            }
        }
    }

    [[nodiscard]] bool textures_landed() const noexcept {
        return std::all_of(textures.begin(), textures.end(), [](const Upload& upload) { return upload.landed; });
    }
};

ParticleInstance::ParticleInstance(std::unique_ptr<State> state) : state_{std::move(state)} {}
ParticleInstance::ParticleInstance(ParticleInstance&&) noexcept = default;
ParticleInstance& ParticleInstance::operator=(ParticleInstance&&) noexcept = default;
ParticleInstance::~ParticleInstance() {
    // Checkpoints hold device buffers the simulation's device must release
    // first-in, last-out: drop them before the simulation.
    if (state_) {
        state_->checkpoints.clear();
        state_->simulation.reset();
    }
}

const ajfx::Effect& ParticleInstance::effect() const noexcept {
    return state_->effect;
}

core::u64 ParticleInstance::step() const noexcept {
    return state_->simulation->tick();
}

core::u64 ParticleInstance::target_step() const noexcept {
    return state_->target;
}

bool ParticleInstance::caught_up() const noexcept {
    return step() == target_step() && state_->textures_landed() &&
           (!state_->rigid || state_->rigid_written == target_step());
}

std::optional<std::string> ParticleInstance::failure() const {
    if (state_->rigid) {
        if (std::optional<ajfx::Error> failed = state_->rigid->error()) {
            return "rigid physics in '" + state_->effect.name + "': " + failed->message;
        }
    }
    return std::nullopt;
}

bool ParticleInstance::safe_to_release() const noexcept {
    return !state_ || std::all_of(state_->textures.begin(), state_->textures.end(),
                                  [](const State::Upload& upload) { return upload.texture.safe_to_release(); });
}

ajfx::gpu::GpuSimulation& ParticleInstance::simulation() noexcept {
    return *state_->simulation;
}

// ------------------------------------------------------------ pass

struct ParticlePass::State final {
    rhi::Device* device{};
    std::unique_ptr<ParticleDevice> particles;
    rhi::GraphicsPipeline blended;  // alpha and premultiplied emitters
    rhi::GraphicsPipeline additive;
    // Blended meshes: as `blended`, writing depth.
    rhi::GraphicsPipeline mesh;
    // Sprite motion into the scene's velocity target.
    rhi::GraphicsPipeline velocity;
    // Grid volumes, raymarched over the scene; tests no depth itself (the
    // march stops at the scene's depth) and writes none.
    rhi::GraphicsPipeline volume;
    // Particle lights into the scene's particle light buffer.
    rhi::ComputePipeline lights;
    // Bound for emitters without a texture, which never sample it.
    std::optional<rhi::Texture> blank;
    std::optional<rhi::Sampler> sampler;
    ParticleSettings settings;
    // Made with the first rigid physics instance; the runner goes first.
    std::unique_ptr<ajfx::RigidSolver> rigid_solver;
    std::unique_ptr<ajfx::RigidRunner> rigid_runner;
    std::optional<ParticleError> error;

    void note(std::string message) {
        if (!error) {
            error = failure(ParticleErrorCode::DeviceFailure, std::move(message));
        }
    }
};

ParticlePass::ParticlePass(std::unique_ptr<State> state) : state_{std::move(state)} {}
ParticlePass::ParticlePass(ParticlePass&&) noexcept = default;
ParticlePass& ParticlePass::operator=(ParticlePass&&) noexcept = default;
ParticlePass::~ParticlePass() = default;

core::Result<ParticlePass, ParticleError> ParticlePass::create(rhi::Device& device, rhi::TextureFormat color_format,
                                                               ParticleSettings settings) {
    using Result = core::Result<ParticlePass, ParticleError>;
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    if (settings.step_budget == 0U || settings.checkpoint_interval == 0U) {
        return Result::failure(failure(ParticleErrorCode::InvalidEffect, "particle step budget and checkpoint interval must be positive"));
    }
    auto particles = ParticleDevice::create(device);
    if (!particles) {
        return Result::failure(failure(ParticleErrorCode::Unsupported,
                                       "this renderer cannot simulate particles: it needs compute with writable "
                                       "buffers and indirect dispatch"));
    }
    auto state = std::make_unique<State>();
    state->device = &device;
    state->particles = std::move(particles).value();
    state->settings = settings;

    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = rhi::ShaderFormat::Dxil;
    vertex.bytecode = std::span<const core::u8>{reinterpret_cast<const core::u8*>(mediaserver_particle_sprite_vert_dxil),
                                                sizeof(mediaserver_particle_sprite_vert_dxil)};
    // The records, order, counters, mesh vertices and indices, and the
    // scene's lights.
    vertex.storage_buffer_count = 6U;
    vertex.uniform_buffer_count = 1U;
    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = rhi::ShaderFormat::Dxil;
    fragment.bytecode = std::span<const core::u8>{reinterpret_cast<const core::u8*>(mediaserver_particle_sprite_frag_dxil),
                                                  sizeof(mediaserver_particle_sprite_frag_dxil)};
    // The texture, the scene depth soft particles fade against, the six-way
    // lightmap and the four shadow cascades.
    fragment.sampler_count = sprite_texture_slots;
    // Tested against the scene, never written: a particle is not a surface
    // anything behind it should be hidden by.
    rhi::DepthState depth{};
    depth.enabled = true;
    depth.write = false;
    depth.greater = true;
    auto blended = device.create_pipeline(vertex, fragment, color_format, rhi::ColorBlend::PremultipliedNormal, depth);
    auto additive = device.create_pipeline(vertex, fragment, color_format, rhi::ColorBlend::PremultipliedEmit, depth);
    rhi::ShaderDesc motion = fragment;
    motion.bytecode = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_particle_velocity_frag_dxil),
        sizeof(mediaserver_particle_velocity_frag_dxil)};
    auto velocity = device.create_pipeline(vertex, motion, rhi::TextureFormat::Rgba32Float, rhi::ColorBlend::Disabled,
                                           depth);
    rhi::DepthState solid = depth;
    solid.write = true;
    auto mesh = device.create_pipeline(vertex, fragment, color_format, rhi::ColorBlend::PremultipliedNormal, solid);
    rhi::ShaderDesc light_kernel{};
    light_kernel.stage = rhi::ShaderStage::Compute;
    light_kernel.format = rhi::ShaderFormat::Dxil;
    light_kernel.bytecode = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_particle_lights_comp_dxil),
        sizeof(mediaserver_particle_lights_comp_dxil)};
    light_kernel.storage_buffer_count = 2U;
    light_kernel.writable_buffer_count = 1U;
    light_kernel.uniform_buffer_count = 1U;
    auto lights = device.create_compute_pipeline(light_kernel);
    // The grid volumes' box and raymarch: the fragment stage binds the
    // sprite's seven texture slots (the scene depth and the cascades among
    // them), then the grid's cells, the scene's lights and the particles'.
    rhi::ShaderDesc volume_vertex{};
    volume_vertex.stage = rhi::ShaderStage::Vertex;
    volume_vertex.format = rhi::ShaderFormat::Dxil;
    volume_vertex.bytecode = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_particle_volume_vert_dxil),
        sizeof(mediaserver_particle_volume_vert_dxil)};
    volume_vertex.uniform_buffer_count = 1U;
    rhi::ShaderDesc volume_fragment{};
    volume_fragment.stage = rhi::ShaderStage::Fragment;
    volume_fragment.format = rhi::ShaderFormat::Dxil;
    volume_fragment.bytecode = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_particle_volume_frag_dxil),
        sizeof(mediaserver_particle_volume_frag_dxil)};
    volume_fragment.sampler_count = sprite_texture_slots;
    volume_fragment.storage_buffer_count = 3U;
    volume_fragment.uniform_buffer_count = 1U;
    // Depth-bound but untested: the march stops at the scene's depth itself,
    // where a test of the box's back face would hide smoke in front of a
    // surface behind it. Never written.
    rhi::DepthState untested{};
    untested.enabled = true;
    untested.write = false;
    untested.test = false;
    auto volume = device.create_pipeline(volume_vertex, volume_fragment, color_format,
                                         rhi::ColorBlend::PremultipliedNormal, untested);
    if (!blended || !additive || !velocity || !mesh || !lights || !volume) {
        return Result::failure(failure(ParticleErrorCode::Unsupported, "the particle sprite pipelines could not be made"));
    }
    state->blended = std::move(blended).value();
    state->additive = std::move(additive).value();
    state->velocity = std::move(velocity).value();
    state->mesh = std::move(mesh).value();
    state->lights = std::move(lights).value();
    state->volume = std::move(volume).value();
    rhi::TextureDesc blank{};
    blank.width = 1U;
    blank.height = 1U;
    blank.sampled = true;
    auto blank_texture = device.create_texture(blank);
    auto sampler = device.create_sampler(rhi::SamplerDesc{});
    if (!blank_texture || !sampler) {
        return Result::failure(failure(ParticleErrorCode::Unsupported, "the particle texture sampler could not be made"));
    }
    state->blank.emplace(std::move(blank_texture).value());
    state->sampler.emplace(std::move(sampler).value());
    return Result::success(ParticlePass{std::move(state)});
#else
    (void)device;
    (void)color_format;
    (void)settings;
    return Result::failure(failure(ParticleErrorCode::Unsupported, "this build has no particle sprite shaders"));
#endif
}

core::Result<ParticleInstance, ParticleError> ParticlePass::create_instance(const ajfx::Effect& effect,
                                                                           const ajfx::gpu::KernelSet& kernels,
                                                                           const ajfx::TextureSet& textures,
                                                                           const ajfx::FieldSet& fields,
                                                                           const ajfx::MeshSet& meshes,
                                                                           core::u32 seed,
                                                                           std::shared_ptr<const ajfx::Cache> cache) {
    using Result = core::Result<ParticleInstance, ParticleError>;
    State& s = *state_;
    if (ajfx::Result<bool> checked = ajfx::check_textures(effect, textures); !checked) {
        return Result::failure(failure(ParticleErrorCode::InvalidEffect, checked.error().message));
    }
    if (ajfx::Result<bool> checked = ajfx::check_fields(effect, fields); !checked) {
        return Result::failure(failure(ParticleErrorCode::InvalidEffect, checked.error().message));
    }
    if (ajfx::Result<bool> checked = ajfx::check_meshes(effect, meshes); !checked) {
        return Result::failure(failure(ParticleErrorCode::InvalidEffect, checked.error().message));
    }
    // Rigid physics emitters are stepped by the pass's runner, not inline,
    // and a cache's emitters are read by it.
    if (cache != nullptr && seed != cache->identity().instance_seed) {
        return Result::failure(failure(ParticleErrorCode::InvalidEffect,
                                       "a cached instance takes the seed its cache was baked with"));
    }
    const bool rigid = ajfx::has_rigid_physics(effect) || (cache != nullptr && cache->caches_anything());
    ajfx::Result<ajfx::gpu::GpuSimulation> simulation = ajfx::gpu::GpuSimulation::create(
        *s.particles, effect, kernels, seed, ajfx::SimulationOptions{.fields = fields, .cache = cache},
        rigid ? ajfx::gpu::RigidHosting::Host : ajfx::gpu::RigidHosting::Inline);
    if (!simulation) {
        const ajfx::ErrorCode code = simulation.error().code;
        return Result::failure(failure(code == ajfx::ErrorCode::DeviceFailure ? ParticleErrorCode::DeviceFailure
                                                                                : ParticleErrorCode::InvalidEffect,
                                       simulation.error().message));
    }
    auto state = std::make_unique<ParticleInstance::State>();
    state->effect = effect;
    state->simulation.emplace(std::move(simulation).value());
    // Every checkpoint is allocated now, so saving one while recording a
    // frame copies into existing buffers. Nothing is recorded or submitted
    // here: creating an instance only allocates.
    for (core::u32 slot = 0U; slot < s.settings.checkpoint_count; ++slot) {
        ajfx::Result<ajfx::gpu::GpuCheckpoint> checkpoint = state->simulation->allocate_checkpoint();
        if (!checkpoint) {
            return Result::failure(failure(ParticleErrorCode::DeviceFailure, checkpoint.error().message));
        }
        state->checkpoints.push_back(std::move(checkpoint).value());
        state->saved.emplace_back();
    }
    ajfx::Result<ajfx::gpu::ParticleSorter> sorter = ajfx::gpu::ParticleSorter::create(*s.particles, effect, kernels);
    if (!sorter) {
        return Result::failure(failure(ParticleErrorCode::DeviceFailure, sorter.error().message));
    }
    state->sorter.emplace(std::move(sorter).value());
    state->device = s.particles.get();
    if (rigid) {
        if (!s.rigid_runner) {
            s.rigid_solver = ajfx::rigid::make_jolt_solver(ajfx::rigid::JoltOptions{.threads = s.settings.rigid_threads});
            s.rigid_runner = std::make_unique<ajfx::RigidRunner>(
                *s.rigid_solver, ajfx::RigidRunnerOptions{.lead = s.settings.rigid_lead,
                                                          .checkpoint_interval = s.settings.checkpoint_interval,
                                                          .checkpoint_count = s.settings.checkpoint_count});
        }
        state->rigid = s.rigid_runner->track(effect, seed, meshes, nullptr,
                                             cache != nullptr ? cache->performance() : nullptr, cache);
        if (std::optional<ajfx::Error> refused = state->rigid->error()) {
            return Result::failure(failure(ParticleErrorCode::InvalidEffect, refused->message));
        }
    }
    state->hosted.resize(effect.emitters.size());
    for (std::size_t e = 0U; e < effect.emitters.size(); ++e) {
        state->hosted[e] = ajfx::rigid_physics(effect.emitters[e]) != nullptr ||
                           (cache != nullptr && cache->mode(e) != ajfx::CacheMode::Live);
    }
    if (cache != nullptr) {
        state->actions = cache->performance();
    }
    state->cache = std::move(cache);
    // Buffers for one package mesh, shared by everything that names it: the
    // emitters that draw it and the props shaped by it. check_meshes above
    // has already refused an effect naming a mesh the package does not hold.
    std::string mesh_error;
    const auto mesh_upload = [&](const std::string& name) -> std::optional<std::size_t> {
        for (std::size_t u = 0U; u < state->mesh_uploads.size(); ++u) {
            if (state->mesh_uploads[u].name == name) {
                return u;
            }
        }
        const auto found = meshes.find(name);
        if (found == meshes.end()) {
            mesh_error = "mesh '" + name + "' is missing";
            return std::nullopt;
        }
        const ajfx::MeshData& data = found->second;
        ParticleInstance::State::MeshUpload upload;
        upload.name = name;
        upload.index_count = static_cast<core::u32>(data.indices.size());
        for (const ajfx::MeshVertex& v : data.vertices) {
            upload.unwritten_vertices.insert(upload.unwritten_vertices.end(),
                                             {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y,
                                              v.normal.z, v.u, v.v});
        }
        upload.unwritten_indices = data.indices;
        for (const auto& [bytes, target] : {std::pair{upload.unwritten_vertices.size() * 4U, &upload.vertices},
                                            std::pair{upload.unwritten_indices.size() * 4U, &upload.indices}}) {
            ajfx::Result<ajfx::gpu::Buffer> created = s.particles->create_buffer(bytes, "particle mesh");
            if (!created) {
                mesh_error = created.error().message;
                return std::nullopt;
            }
            *target = created.value();
        }
        state->mesh_uploads.push_back(std::move(upload));
        return state->mesh_uploads.size() - 1U;
    };
    for (const ajfx::Emitter& emitter : effect.emitters) {
        std::optional<ParticleInstance::State::Mesh>& out = state->meshes.emplace_back();
        if (emitter.renderer.geometry != ajfx::Geometry::Mesh) {
            continue;
        }
        const std::optional<std::size_t> upload = mesh_upload(emitter.renderer.mesh);
        if (!upload) {
            return Result::failure(failure(ParticleErrorCode::DeviceFailure, mesh_error));
        }
        ParticleInstance::State::Mesh mesh;
        mesh.upload = *upload;
        ajfx::Result<ajfx::gpu::Buffer> args = s.particles->create_buffer(16U, "particle mesh");
        if (!args) {
            return Result::failure(failure(ParticleErrorCode::DeviceFailure, args.error().message));
        }
        mesh.args = args.value();
        // A body kind may name the model its bodies are drawn as. Where any
        // of them does, the emitter draws a group per model instead of one
        // draw, so a crate and the panels it breaks into are each drawn as
        // themselves. The emitter's own model is group 0, and the kind that
        // names nothing is drawn with it.
        const ajfx::RigidPhysics* physics = ajfx::rigid_physics(emitter);
        // A kind with a size of its own needs a draw of its own as surely as
        // one with a model of its own does, since the scale is the draw's.
        const bool per_kind =
            physics != nullptr && std::any_of(physics->bodies.begin(), physics->bodies.end(),
                                              [](const ajfx::RigidBodyKind& kind) {
                                                  return !kind.draw.empty() || kind.scale != 1.0F || kind.draw_tint != ajfx::Color{};
                                              });
        if (per_kind) {
            // A group is one model at one scale: two kinds share a draw only
            // when both agree, so the same model at half the size is drawn
            // apart from it.
            std::vector<std::tuple<std::string, float, ajfx::Color>> models{{emitter.renderer.mesh, 1.0F, ajfx::Color{}}};
            mesh.kind_group.assign(physics->bodies.size() + 1U, 0U);
            for (std::size_t k = 0U; k < physics->bodies.size(); ++k) {
                const std::tuple<std::string, float, ajfx::Color> model{
                    physics->bodies[k].draw.empty() ? emitter.renderer.mesh : physics->bodies[k].draw,
                    physics->bodies[k].scale, physics->bodies[k].draw_tint};
                const auto at = std::find(models.begin(), models.end(), model);
                mesh.kind_group[k + 1U] = static_cast<std::size_t>(at - models.begin());
                if (at == models.end()) {
                    models.push_back(model);
                }
            }
            for (const auto& [model, scale, tint] : models) {
                const std::optional<std::size_t> group_upload = mesh_upload(model);
                if (!group_upload) {
                    return Result::failure(failure(ParticleErrorCode::DeviceFailure, mesh_error));
                }
                ajfx::Result<ajfx::gpu::Buffer> order = s.particles->create_buffer(
                    static_cast<std::size_t>(emitter.capacity) * 4U, "particle mesh group");
                if (!order) {
                    return Result::failure(failure(ParticleErrorCode::DeviceFailure, order.error().message));
                }
                ParticleInstance::State::Mesh::Group group;
                group.upload = *group_upload;
                group.order = order.value();
                group.scratch.reserve(emitter.capacity);
                group.scale = scale;
                group.tint = tint;
                mesh.groups.push_back(std::move(group));
            }
        }
        out = std::move(mesh);
    }
    // The props an artist stood in a rigid physics emitter's world. A prop
    // draws the model it names, whatever it collides as: a bucket collides as
    // a generated hollow cylinder (a collision mesh cannot move) and is still
    // drawn as the model the artist picked. Naming a model is what asks the
    // package for one, so check_meshes above has already refused an effect
    // whose package did not answer. Props sharing a mesh share its buffers
    // and draw together, gathered emitter by emitter and prop by prop so
    // every node draws them in the order the effect lists them.
    for (std::size_t e = 0U; e < effect.emitters.size(); ++e) {
        const ajfx::RigidPhysics* physics = ajfx::rigid_physics(effect.emitters[e]);
        if (physics == nullptr) {
            continue;
        }
        for (std::size_t p = 0U; p < physics->props.size(); ++p) {
            const ajfx::RigidProp& prop = physics->props[p];
            if (prop.mesh.empty()) {
                continue;
            }
            const std::optional<std::size_t> upload = mesh_upload(prop.mesh);
            if (!upload) {
                return Result::failure(failure(ParticleErrorCode::DeviceFailure, mesh_error));
            }
            auto group = std::find_if(state->props.begin(), state->props.end(),
                                      [&](const ParticleInstance::State::PropGroup& candidate) {
                                          return candidate.upload == *upload;
                                      });
            if (group == state->props.end()) {
                group = state->props.insert(state->props.end(), ParticleInstance::State::PropGroup{*upload});
            }
            group->props.push_back({e, p});
        }
    }
    for (ParticleInstance::State::PropGroup& group : state->props) {
        group.scratch.resize(group.props.size());
        ajfx::Result<ajfx::gpu::Buffer> created =
            s.particles->create_buffer(group.props.size() * sizeof(PropRecord), "particle props");
        if (!created) {
            return Result::failure(failure(ParticleErrorCode::DeviceFailure, created.error().message));
        }
        group.records = created.value();
    }
    // Textures are made now and filled by later advances, one per name.
    std::map<std::string, std::size_t, std::less<>> texture_index;
    const auto texture_slot = [&](const std::string& name) -> core::Result<std::optional<std::size_t>, ParticleError> {
        using Slot = core::Result<std::optional<std::size_t>, ParticleError>;
        if (name.empty()) {
            return Slot::success(std::nullopt);
        }
        if (const auto found = texture_index.find(name); found != texture_index.end()) {
            return Slot::success(found->second);
        }
        const ajfx::TextureData& data = textures.find(name)->second;
        rhi::TextureDesc desc{};
        desc.width = data.width;
        desc.height = data.height;
        desc.sampled = true;
        desc.mip_levels = static_cast<core::u32>(data.mips.size());
        auto texture = s.device->create_texture(desc);
        if (!texture) {
            return Slot::failure(failure(ParticleErrorCode::DeviceFailure, "texture '" + name + "' could not be made"));
        }
        const std::size_t index = state->textures.size();
        state->textures.push_back({std::move(texture).value(), data});
        texture_index.emplace(name, index);
        return Slot::success(index);
    };
    for (const ajfx::Emitter& emitter : effect.emitters) {
        auto texture = texture_slot(emitter.renderer.texture);
        if (!texture) {
            return Result::failure(std::move(texture).error());
        }
        auto lightmap = texture_slot(emitter.renderer.lightmap);
        if (!lightmap) {
            return Result::failure(std::move(lightmap).error());
        }
        state->emitter_texture.push_back(texture.value());
        state->emitter_lightmap.push_back(lightmap.value());
    }
    // The parameters' defaults until an advance brings values of its own.
    const std::vector<ajfx::ParameterValue> defaults = ajfx::parameter_values(state->effect, nullptr, 0U);
    for (std::size_t e = 0U; e < state->effect.emitters.size(); ++e) {
        state->adjustments.push_back(ajfx::draw_adjustment(state->effect, e, defaults));
    }
    return Result::success(ParticleInstance{std::move(state)});
}

bool ParticlePass::advance(rhi::CommandList& commands, ParticleInstance& instance, core::u64 show_frame,
                           core::u32 rate_num, core::u32 rate_den, const ajfx::ParameterProvider* parameters) {
    State& s = *state_;
    ParticleInstance::State& i = *instance.state_;
    ajfx::gpu::GpuSimulation& simulation = *i.simulation;
    if (rate_num == 0U || rate_den == 0U) {
        s.note("advance needs a positive show rate");
        return false;
    }
    i.target = ajfx::tick_at(show_frame, rate_num, rate_den, i.effect.simulation_rate);

    // Textures first, as much as this recording's upload space holds.
    for (ParticleInstance::State::Upload& upload : i.textures) {
        while (!upload.landed) {
            const core::u32 width = ajfx::mip_size(upload.data.width, upload.mip);
            const core::u32 height = ajfx::mip_size(upload.data.height, upload.mip);
            const core::u32 room = commands.texture_upload_row_capacity(upload.texture);
            if (room == 0U) {
                break;
            }
            const core::u32 rows = std::min(height - upload.row, room);
            const std::size_t row_bytes = std::size_t{width} * 4U;
            const std::span<const core::u8> texels{upload.data.mips[upload.mip].data() + upload.row * row_bytes,
                                                   rows * row_bytes};
            if (!commands.upload_texture_rows(upload.texture, upload.mip, 0U, upload.row, rows, texels)) {
                s.note("a particle texture upload was refused");
                return false;
            }
            upload.row += rows;
            if (upload.row == height) {
                upload.row = 0U;
                if (++upload.mip == upload.data.mips.size()) {
                    upload.landed = true;
                    upload.data.mips = {};
                }
            }
        }
    }

    s.particles->begin_recording(commands);
    for (ParticleInstance::State::MeshUpload& upload : i.mesh_uploads) {
        if (!upload.unwritten_vertices.empty()) {
            s.particles->write_buffer(upload.vertices, 0U,
                                      std::span{reinterpret_cast<const std::uint8_t*>(upload.unwritten_vertices.data()),
                                                upload.unwritten_vertices.size() * 4U});
            s.particles->write_buffer(upload.indices, 0U,
                                      std::span{reinterpret_cast<const std::uint8_t*>(upload.unwritten_indices.data()),
                                                upload.unwritten_indices.size() * 4U});
            upload.unwritten_vertices = {};
            upload.unwritten_indices = {};
        }
    }
    const ajfx::ActionProvider* const actions = i.actions.get();
    const core::u64 held_step = simulation.tick();
    if (i.restart) {
        simulation.reset(parameters, actions);
        i.restart = false;
        i.revised.reset();
        i.rigid_written.reset();
    }
    // Actions changed from a tick this simulation has stepped past: it goes
    // back to the latest checkpoint at or before that tick (revise dropped
    // the later ones), or to the start. Tick 0 always starts again, since
    // its actions place bodies before the first step.
    bool revisit = false;
    if (i.revised) {
        revisit = simulation.tick() > *i.revised || *i.revised == 0U;
        i.revised.reset();
    }
    // Start from the latest checkpoint at or before the target when that is
    // closer than where the simulation is: always when going back, and when
    // a scrub forward passes a checkpoint already made.
    std::optional<std::size_t> best;
    for (std::size_t slot = 0U; slot < i.saved.size(); ++slot) {
        if (i.saved[slot] && *i.saved[slot] <= i.target && (!best || *i.saved[slot] > *i.saved[*best])) {
            best = slot;
        }
    }
    if (i.target < simulation.tick() || revisit) {
        if (best) {
            simulation.restore(i.checkpoints[*best]);
        } else {
            simulation.reset(parameters, actions);
        }
        i.rigid_written.reset();
    } else if (best && *i.saved[*best] > simulation.tick()) {
        simulation.restore(i.checkpoints[*best]);
        i.rigid_written.reset();
    }

    // Under rigid physics every step takes the runner's frame of the tick it
    // reaches, so the particle emitters meet those bodies and are born of
    // their contacts and motion as the reference's are. A frame the runner
    // has not reached yet stops the stepping there, without waiting: the
    // instance is not caught up, so it holds and is drawn on a later frame,
    // never stepped past bodies it did not meet.
    std::shared_ptr<const ajfx::RigidFrame> reached;
    for (core::u32 steps = 0U; simulation.tick() < i.target && steps < s.settings.step_budget; ++steps) {
        const core::u64 tick = simulation.tick();
        std::shared_ptr<const ajfx::RigidFrame> next;
        if (i.rigid) {
            next = i.rigid->frame(tick + 1U);
            if (!next) {
                break;
            }
        }
        const bool held = std::any_of(i.saved.begin(), i.saved.end(),
                                      [tick](const std::optional<core::u64>& saved) { return saved == tick; });
        if (!i.checkpoints.empty() && tick % s.settings.checkpoint_interval == 0U && !held) {
            // An empty slot, else the one holding the earliest step.
            std::size_t slot = 0U;
            for (std::size_t candidate = 0U; candidate < i.saved.size(); ++candidate) {
                if (!i.saved[candidate]) {
                    slot = candidate;
                    break;
                }
                if (i.saved[slot] && *i.saved[candidate] < *i.saved[slot]) {
                    slot = candidate;
                }
            }
            simulation.save_into(i.checkpoints[slot]);
            i.saved[slot] = tick;
        }
        if (next) {
            simulation.step(parameters, *next, actions);
        } else {
            simulation.step(parameters, actions);
        }
        reached = std::move(next);
    }
    if (simulation.tick() != held_step) {
        i.rigid_written.reset();
    }
    // The rigid physics emitters' particles at the target step, once the
    // runner has them. Asking also tells it where to work ahead from, so it
    // is asked only once the simulation stands at the target: asking for the
    // target while steps short of it still wait their frames would send the
    // runner past the ticks those steps need.
    if (i.rigid && simulation.tick() == i.target) {
        const std::shared_ptr<const ajfx::RigidFrame> frame =
            reached && reached->tick == i.target ? reached : i.rigid->frame(i.target);
        if (frame && i.rigid_written != i.target) {
            // A step that reached the target with its frame wrote these
            // particles already.
            if (frame != reached) {
                for (std::size_t e = 0U; e < i.effect.emitters.size(); ++e) {
                    if (i.hosted[e]) {
                        simulation.write_rigid(e, frame->emitters[e]);
                    }
                }
            }
            // The props of the same tick, so what is drawn is one moment of
            // one world: each pose is written as the particles' are, and the
            // placement turns it into world space in the vertex stage.
            // Nothing is written, and the instance stays behind, if a frame
            // somehow holds fewer props than the effect lists: a prop a frame
            // late is better than a prop snapped to the origin.
            bool poses = true;
            for (const ParticleInstance::State::PropGroup& group : i.props) {
                for (const auto& [e, p] : group.props) {
                    poses = poses && p < frame->emitters[e].props.size();
                }
            }
            if (!poses) {
                s.note("a rigid frame holds fewer props than the effect lists");
            } else {
                for (ParticleInstance::State::PropGroup& group : i.props) {
                    for (std::size_t r = 0U; r < group.props.size(); ++r) {
                        const auto [e, p] = group.props[r];
                        const ajfx::RigidBodyState& pose = frame->emitters[e].props[p];
                        PropRecord& record = group.scratch[r];
                        record.position = {pose.position.x, pose.position.y, pose.position.z};
                        // The model is held with its longest side one unit
                        // long, so the authored size is that side's length.
                        record.size = ajfx::rigid_physics(i.effect.emitters[e])->props[p].size;
                        record.velocity = {pose.velocity.x, pose.velocity.y, pose.velocity.z};
                        record.orientation = {pose.orientation.x, pose.orientation.y, pose.orientation.z,
                                              pose.orientation.w};
                        // A prop carries no colour of its own: its model is
                        // drawn as it is, lit like the debris around it.
                        const ajfx::Color tint = ajfx::rigid_physics(i.effect.emitters[e])->props[p].draw_tint;
                        record.color = {tint.r, tint.g, tint.b, tint.a};
                    }
                    s.particles->write_buffer(
                        group.records, 0U,
                        std::span{reinterpret_cast<const std::uint8_t*>(group.scratch.data()),
                                  group.scratch.size() * sizeof(PropRecord)});
                }
                // Which of an emitter's particles each model draws, where its
                // body kinds name models of their own. The particles were
                // written above in this frame's order, so a particle's record
                // is at its place in the frame.
                for (std::size_t e = 0U; e < i.meshes.size(); ++e) {
                    if (!i.meshes[e] || i.meshes[e]->groups.empty()) {
                        continue;
                    }
                    ParticleInstance::State::Mesh& mesh = *i.meshes[e];
                    const ajfx::EmitterState& particles = frame->emitters[e];
                    for (ParticleInstance::State::Mesh::Group& group : mesh.groups) {
                        group.scratch.clear();
                    }
                    for (std::size_t p = 0U; p < particles.alive(); ++p) {
                        const std::size_t kind = p < particles.body_kind.size() ? particles.body_kind[p] : 0U;
                        const std::size_t group = kind < mesh.kind_group.size() ? mesh.kind_group[kind] : 0U;
                        mesh.groups[group].scratch.push_back(static_cast<std::uint32_t>(p));
                    }
                    for (ParticleInstance::State::Mesh::Group& group : mesh.groups) {
                        group.count = static_cast<core::u32>(group.scratch.size());
                        if (group.count > 0U) {
                            s.particles->write_buffer(
                                group.order, 0U,
                                std::span{reinterpret_cast<const std::uint8_t*>(group.scratch.data()),
                                          group.scratch.size() * sizeof(std::uint32_t)});
                        }
                    }
                }
                i.rigid_written = i.target;
            }
        }
    }
    // An effect without parameters keeps the adjustments it was created with.
    if (!i.effect.parameters.empty()) {
        const std::vector<ajfx::ParameterValue> values =
            ajfx::parameter_values(i.effect, parameters, simulation.tick());
        for (std::size_t e = 0U; e < i.adjustments.size(); ++e) {
            i.adjustments[e] = ajfx::draw_adjustment(i.effect, e, values);
        }
    }
    s.particles->end_recording();
    if (std::optional<ajfx::Error> error = s.particles->take_error()) {
        s.note(error->message);
        return false;
    }
    return instance.caught_up();
}

void ParticlePass::restart(ParticleInstance& instance,
                           std::shared_ptr<const ajfx::ParameterProvider> parameters) noexcept {
    ParticleInstance::State& i = *instance.state_;
    std::fill(i.saved.begin(), i.saved.end(), std::nullopt);
    i.restart = true;
    if (i.rigid) {
        i.rigid->set_parameters(std::move(parameters));
    }
}

void ParticlePass::revise(ParticleInstance& instance, std::shared_ptr<const ajfx::ActionProvider> actions,
                          core::u64 from) noexcept {
    ParticleInstance::State& i = *instance.state_;
    // A cache plays the performance it was baked with.
    if (i.cache != nullptr) {
        return;
    }
    // A checkpoint at tick t holds what the steps before t made, which read
    // the actions before t: one at `from` still stands.
    for (std::optional<core::u64>& saved : i.saved) {
        if (saved && (*saved > from || from == 0U)) {
            saved.reset();
        }
    }
    i.revised = i.revised ? std::min(*i.revised, from) : from;
    if (i.rigid) {
        i.rigid->set_lead(actions ? state_->settings.acting_rigid_lead : state_->settings.rigid_lead);
        i.rigid->set_actions(actions, from);
    }
    i.actions = std::move(actions);
}

core::u32 ParticlePass::rigid_lead(const ParticleInstance& instance) const noexcept {
    return instance.state_->actions && instance.state_->cache == nullptr ? state_->settings.acting_rigid_lead
                                                                        : state_->settings.rigid_lead;
}

bool ParticlePass::draw(rhi::CommandList& commands, ParticleInstance& instance, const scene::ViewMatrices& view,
                        const scene::Mat4& object_to_world, const ParticleTargets& targets) {
    State& s = *state_;
    if (!instance.caught_up() || targets.color == nullptr || targets.depth == nullptr) {
        return false;
    }
    ParticleInstance::State& i = *instance.state_;

    // Back to front along the camera's forward axis, in the effect's own
    // space: depth = dot(world(p), forward) is dot(p, W^T forward) plus a
    // constant, for W the 3x3 part of object_to_world (column-major).
    std::array<float, 3U> forward{};
    for (core::u32 column = 0U; column < 3U; ++column) {
        forward[column] = -view.view.at(2U, column);
    }
    ajfx::Vec3 axis{};
    float* const axis_out[3] = {&axis.x, &axis.y, &axis.z};
    for (core::u32 column = 0U; column < 3U; ++column) {
        *axis_out[column] = object_to_world.m[column * 4U + 0U] * forward[0] +
                            object_to_world.m[column * 4U + 1U] * forward[1] +
                            object_to_world.m[column * 4U + 2U] * forward[2];
    }
    i.orders.assign(i.effect.emitters.size(), ajfx::gpu::Buffer{});
    s.particles->begin_recording(commands);
    for (std::size_t e = 0U; e < i.orders.size(); ++e) {
        i.orders[e] = i.sorter->sort(*i.simulation, e, axis);
        // A mesh draws its index count of vertices for each live particle.
        if (i.meshes[e]) {
            const std::uint32_t args[4] = {i.mesh_uploads[i.meshes[e]->upload].index_count, 0U, 0U, 0U};
            s.particles->write_buffer(i.meshes[e]->args, 0U,
                                      std::span{reinterpret_cast<const std::uint8_t*>(args), sizeof(args)});
            const ajfx::gpu::EmitterBuffers buffers = i.simulation->emitter_buffers(e);
            s.particles->copy_buffer(i.meshes[e]->args, 4U, buffers.args, buffers.draw_args_offset + 4U, 4U);
        }
    }
    i.sorter->slice(*i.simulation);
    s.particles->end_recording();
    if (std::optional<ajfx::Error> error = s.particles->take_error()) {
        s.note(error->message);
        return false;
    }
    return record_sprites(commands, instance, view, object_to_world, targets, false);
}

bool ParticlePass::draw_motion(rhi::CommandList& commands, ParticleInstance& instance,
                               const scene::ViewMatrices& view, const scene::Mat4& object_to_world,
                               const ParticleTargets& targets) {
    const ParticleInstance::State& i = *instance.state_;
    if (!instance.caught_up() || targets.velocity == nullptr || targets.depth == nullptr ||
        i.orders.size() != i.effect.emitters.size()) {
        return false;
    }
    return record_sprites(commands, instance, view, object_to_world, targets, true);
}

bool ParticlePass::record_sprites(rhi::CommandList& commands, ParticleInstance& instance,
                                  const scene::ViewMatrices& view, const scene::Mat4& object_to_world,
                                  const ParticleTargets& targets, bool motion) {
    State& s = *state_;
    const ParticleInstance::State& i = *instance.state_;
    ViewConstants constants;
    constants.view_projection = view.view_projection.m;
    constants.world = object_to_world.m;
    // The view matrix's rows are the camera's axes in world space; it looks
    // down its own -Z.
    for (core::u32 column = 0U; column < 3U; ++column) {
        constants.camera_right[column] = view.view.at(0U, column);
        constants.camera_up[column] = view.view.at(1U, column);
        constants.camera_forward[column] = -view.view.at(2U, column);
    }
    constants.canvas_view_projection = targets.canvas_view_projection.m;
    constants.previous_view_projection = targets.previous_view_projection.m;
    // How reversed device depth maps back to distance, for soft particles.
    constants.depth_terms = {view.projection.at(2U, 2U), view.projection.at(2U, 3U), view.projection.at(3U, 2U),
                             view.projection.at(3U, 3U)};
    // The scene's light. Without one, no lights and no cascades: the vertex
    // stage then lights from the camera.
    const ParticleLighting* lighting = targets.lighting;
    const rhi::StorageBuffer* scene_lights = nullptr;
    std::array<const rhi::Texture*, scene::max_shadow_cascades> cascades{};
    if (lighting != nullptr) {
        scene_lights = lighting->lights;
        constants.sky_ambient = lighting->sky_ambient;
        constants.ground_exposure = lighting->ground_exposure;
        constants.lighting[0] = scene_lights != nullptr ? lighting->light_count : 0U;
        constants.lighting[1] = std::min<core::u32>(lighting->cascade_count, scene::max_shadow_cascades);
        for (core::u32 c = 0U; c < constants.lighting[1]; ++c) {
            if (lighting->cascades[c] == nullptr) {
                constants.lighting[1] = c;
                break;
            }
            cascades[c] = lighting->cascades[c];
            constants.shadow_view_projection[c] = lighting->cascade_view_projection[c].m;
            constants.shadow_texel_depth[c] = lighting->cascade_texel_depth[c];
        }
    }
    const float shadow_bias = lighting != nullptr ? lighting->shadow_bias_texels : 0.0F;
    const auto sampled_textures = [&](const rhi::Texture* texture, const rhi::Texture* scene_depth,
                                      const rhi::Texture* lightmap) {
        std::array<rhi::SampledTexture, sprite_texture_slots> set{};
        const rhi::Texture* textures[sprite_texture_slots] = {texture, scene_depth, lightmap, cascades[0], cascades[1],
                                                               cascades[2], cascades[3]};
        for (std::size_t t = 0U; t < sprite_texture_slots; ++t) {
            set[t] = rhi::SampledTexture{textures[t] != nullptr ? textures[t] : &*s.blank, &*s.sampler};
        }
        return set;
    };

    bool first = true;
    // The props first. A prop is authored, solid, lit geometry that stands in
    // the world from before the first step, so it belongs with the scene's
    // models rather than among the particles: it draws and writes depth ahead
    // of them, which leaves the emitters' own draws and their sort untouched.
    if (!i.props.empty()) {
        ViewConstants prop = constants;
        prop.stride = static_cast<core::u32>(sizeof(PropRecord));
        prop.position_offset = prop_position_at;
        prop.velocity_offset = prop_velocity_at;
        prop.color_offset = prop_color_at;
        prop.size_offset = prop_size_at;
        // A mesh each, turned by the pose's own orientation: the mesh
        // particles' path exactly, so a prop and the debris around it are lit
        // alike. Untextured, unsorted (a record's index is its instance) and
        // with no life, so no flipbook.
        // A prop is placed and sized by the effect, not by a body kind, so
        // its mesh draw leaves the size as the record gives it.
        prop.extra = {0.0F, 0.0F, std::bit_cast<float>(prop_orientation_at), 1.0F};
        prop.motion = {targets.motion_valid ? targets.motion_seconds : 0.0F,
                       targets.motion_valid ? 1.0F : 0.0F, 0.0F, 0.0F};
        prop.flags = flag_mesh;
        // A prop stands in the scene, so the scene's light falls on it: lit
        // through its own normals, taking the hemisphere whole and the sun's
        // shadows. Without a scene the pass lights it from the camera.
        prop.look = {0.0F, 0.0F, 1.0F, shadow_bias};
        prop.lighting[2] = static_cast<core::u32>(ajfx::Lighting::Lit);
        prop.lighting[3] = 1U;
        const std::array<rhi::SampledTexture, sprite_texture_slots> untextured = sampled_textures(nullptr, nullptr, nullptr);
        for (const ParticleInstance::State::PropGroup& group : i.props) {
            const ParticleInstance::State::MeshUpload& uploaded = i.mesh_uploads[group.upload];
            const rhi::StorageBuffer* poses = s.particles->buffer(group.records);
            const rhi::StorageBuffer* vertices = s.particles->buffer(uploaded.vertices);
            const rhi::StorageBuffer* indices = s.particles->buffer(uploaded.indices);
            if (poses == nullptr || vertices == nullptr || indices == nullptr) {
                s.note("a prop's buffers are missing");
                return false;
            }
            const rhi::StorageBuffer* vertex_buffers[] = {poses, poses, poses, vertices, indices, poses};
            const std::array<rhi::DrawBatch, 1U> batches{
                rhi::DrawBatch{uploaded.index_count, static_cast<core::u32>(group.props.size())}};
            rhi::PulledDraw draw{};
            draw.pipeline = motion ? &s.velocity : &s.mesh;
            draw.vertex_buffers = vertex_buffers;
            draw.vertex_uniform = std::span<const core::u8>{reinterpret_cast<const core::u8*>(&prop), sizeof(prop)};
            draw.fragment_textures = untextured;
            draw.batches = batches;
            draw.load = motion || !first || targets.load;
            draw.store_depth = true;
            draw.viewport = targets.viewport;
            draw.raster_viewport = targets.raster_viewport;
            rhi::Texture& target = motion ? *targets.velocity : *targets.color;
            if (!commands.draw_pulled(target, targets.clear, *targets.depth, targets.depth_clear, draw)) {
                s.note(motion ? "a prop motion draw was refused" : "a prop draw was refused");
                return false;
            }
            first = false;
        }
    }
    // The grid volumes next, raymarched, so the particles draw over the
    // smoke as they do in the reference renderer. A volume writes no motion:
    // smoke drifts within its box, which does not move.
    if (!motion && !i.effect.volumes.empty()) {
        VolumeConstants volume{};
        volume.world = object_to_world.m;
        volume.inverse = scene::invert_affine(object_to_world).m;
        // The eye from the view matrix (its rows are the camera's axes, its
        // last column where the origin lands): eye = -R^T t. A projection
        // whose w depends on depth fans its rays from the eye; one that does
        // not runs them along the forward axis.
        for (core::u32 column = 0U; column < 3U; ++column) {
            float eye = 0.0F;
            for (core::u32 row = 0U; row < 3U; ++row) {
                eye -= view.view.at(row, column) * view.view.at(row, 3U);
            }
            volume.eye[column] = eye;
        }
        volume.eye[3] = view.projection.at(3U, 2U) != 0.0F ? 1.0F : 0.0F;
        volume.camera_forward = constants.camera_forward;
        volume.depth_terms = constants.depth_terms;
        volume.march = {0.0F, 0.0F, volume_ray_steps, volume_light_steps};
        volume.sky_ambient = constants.sky_ambient;
        volume.ground_exposure = constants.ground_exposure;
        const rhi::StorageBuffer* particle_lights = lighting != nullptr ? lighting->particle_lights : nullptr;
        volume.lighting = {constants.lighting[0], constants.lighting[1], 1U,
                           particle_lights != nullptr ? lighting->particle_light_count : 0U};
        volume.shadow_view_projection = constants.shadow_view_projection;
        volume.shadow_texel_depth = constants.shadow_texel_depth;
        volume.shadow_bias = {shadow_bias, 0.0F, 0.0F, 0.0F};
        VolumeViewConstants box{};
        box.view_projection = view.view_projection.m;
        box.world = object_to_world.m;
        const std::array<rhi::SampledTexture, sprite_texture_slots> sampled =
            sampled_textures(nullptr, targets.scene_depth, nullptr);
        for (std::size_t v = 0U; v < i.effect.volumes.size(); ++v) {
            const ajfx::Volume& grid = i.effect.volumes[v];
            const ajfx::gpu::VolumeBuffers cells = i.simulation->volume_buffers(v);
            const rhi::StorageBuffer* buffer = s.particles->buffer(cells.buffer);
            if (buffer == nullptr) {
                s.note("a grid volume's cells are missing");
                return false;
            }
            volume.grid_origin = {cells.origin.x, cells.origin.y, cells.origin.z, cells.cell_size};
            volume.grid_cells = {cells.nx, cells.ny, cells.nz, cells.cells_at};
            volume.march[0] = grid.extinction;
            volume.march[1] = grid.albedo;
            volume.color = {grid.color.x, grid.color.y, grid.color.z, 1.0F};
            box.box_origin = {cells.origin.x, cells.origin.y, cells.origin.z, 0.0F};
            box.box_size = {static_cast<float>(cells.nx) * cells.cell_size,
                            static_cast<float>(cells.ny) * cells.cell_size,
                            static_cast<float>(cells.nz) * cells.cell_size, 0.0F};
            // The scene's lights and the particles', or the cells standing
            // in where there are none (the counts say not to read them).
            const rhi::StorageBuffer* fragment_buffers[] = {buffer, scene_lights != nullptr ? scene_lights : buffer,
                                                            particle_lights != nullptr ? particle_lights : buffer};
            const std::array<rhi::DrawBatch, 1U> batches{rhi::DrawBatch{36U, 1U}};
            rhi::PulledDraw draw{};
            draw.pipeline = &s.volume;
            draw.fragment_buffers = fragment_buffers;
            draw.vertex_uniform = std::span<const core::u8>{reinterpret_cast<const core::u8*>(&box), sizeof(box)};
            draw.fragment_uniform =
                std::span<const core::u8>{reinterpret_cast<const core::u8*>(&volume), sizeof(volume)};
            draw.fragment_textures = sampled;
            draw.batches = batches;
            draw.load = !first || targets.load;
            draw.store_depth = true;
            draw.viewport = targets.viewport;
            draw.raster_viewport = targets.raster_viewport;
            if (!commands.draw_pulled(*targets.color, targets.clear, *targets.depth, targets.depth_clear, draw)) {
                s.note("a grid volume draw was refused");
                return false;
            }
            first = false;
        }
    }
    // One emitter's draw; for an interleaved emitter, one depth slice of it.
    const auto draw_emitter = [&](std::size_t e, const ajfx::gpu::DepthSlice* slice) {
        const ajfx::SpriteRenderer& sprite = i.effect.emitters[e].renderer;
        const ajfx::gpu::EmitterBuffers buffers = i.simulation->emitter_buffers(e);
        const rhi::StorageBuffer* records = s.particles->buffer(buffers.particles);
        const rhi::StorageBuffer* arguments = s.particles->buffer(buffers.args);
        if (records == nullptr || arguments == nullptr ||
            buffers.draw_args_offset > std::numeric_limits<core::u32>::max()) {
            s.note("an emitter's particle buffers are missing");
            return false;
        }
        constants.stride = buffers.layout.stride;
        constants.position_offset = offset_or_absent(buffers.layout.position);
        constants.velocity_offset = offset_or_absent(buffers.layout.velocity);
        constants.color_offset = offset_or_absent(buffers.layout.color);
        constants.size_offset = offset_or_absent(buffers.layout.size);
        constants.rotation_offset = offset_or_absent(buffers.layout.rotation);
        constants.facing_velocity = sprite.facing == ajfx::Facing::Velocity ? 1U : 0U;
        constants.premultiplied = sprite.blend == ajfx::BlendMode::Premultiplied ? 1U : 0U;
        const ajfx::DrawAdjustment& adjustment = i.adjustments[e];
        constants.extra = {sprite.stretch, sprite.blend == ajfx::BlendMode::Additive ? 1.0F : 0.0F,
                           std::bit_cast<float>(offset_or_absent(buffers.layout.orientation)), adjustment.size};
        constants.draw_tint = {adjustment.color.r, adjustment.color.g, adjustment.color.b, adjustment.color.a};
        constants.age_offset = offset_or_absent(buffers.layout.age);
        constants.lifetime_offset = offset_or_absent(buffers.layout.lifetime);
        constants.flipbook = sprite.flipbook_columns | (sprite.flipbook_rows << 16U);
        const bool soft = !motion && targets.scene_depth != nullptr && sprite.soft_distance > 0.0F;
        constants.motion = {targets.motion_valid ? targets.motion_seconds : 0.0F, targets.motion_valid ? 1.0F : 0.0F,
                            soft ? sprite.soft_distance : 0.0F, 0.0F};
        const std::optional<std::size_t> texture = i.emitter_texture[e];
        const std::optional<std::size_t> lightmap = i.emitter_lightmap[e];
        constants.flags = (texture ? flag_textured : 0U) | (sprite.flipbook_blend ? flag_blend_cells : 0U) |
                          (texture && i.textures[*texture].data.color_space == ajfx::ColorSpace::Srgb ? flag_srgb : 0U) |
                          (soft ? flag_soft : 0U) | (sprite.geometry == ajfx::Geometry::Ribbon ? flag_ribbon : 0U) |
                          (sprite.geometry == ajfx::Geometry::Mesh ? flag_mesh : 0U) |
                          (sprite.blend == ajfx::BlendMode::Additive ? flag_additive : 0U);
        constants.look = {sprite.curvature, sprite.wrap, sprite.ambient, shadow_bias};
        constants.lighting[2] = static_cast<core::u32>(sprite.lighting);
        constants.lighting[3] = sprite.shadowed && sprite.lighting != ajfx::Lighting::Unlit ? 1U : 0U;
        const rhi::StorageBuffer* order = i.orders[e].valid() ? s.particles->buffer(i.orders[e]) : records;
        if (order == nullptr) {
            s.note("an emitter's sort order is missing");
            return false;
        }
        if (i.orders[e].valid()) {
            constants.flags |= flag_sorted;
        }
        constants.order_slice[0] = slice != nullptr ? slice->start_word : absent;
        const rhi::StorageBuffer* counters = s.particles->buffer(buffers.counters);
        if (counters == nullptr) {
            s.note("an emitter's counters are missing");
            return false;
        }
        const std::span<const core::u8> emitter_uniform{reinterpret_cast<const core::u8*>(&constants),
                                                        sizeof(constants)};
        const std::array<rhi::SampledTexture, sprite_texture_slots> sampled =
            sampled_textures(texture ? &i.textures[*texture].texture : nullptr, soft ? targets.scene_depth : nullptr,
                             lightmap ? &i.textures[*lightmap].texture : nullptr);
        // The scene's lights ride in the sixth vertex slot; with none, the
        // records stand in and the count says not to read them.
        const rhi::StorageBuffer* lights = scene_lights != nullptr ? scene_lights : records;

        const std::optional<ParticleInstance::State::Mesh>& mesh = i.meshes[e];
        // An emitter whose body kinds name models of their own draws a group
        // per model, each over the particles drawn with it, so a crate and
        // its panels are each drawn as themselves. The group's order buffer
        // stands in for the sort's, and its count is this frame's rather
        // than the simulation's, so these draws are not indirect. Drawing
        // model by model gives up the depth order between models; the mesh
        // pipeline writes depth, which is what settles it.
        if (mesh && !mesh->groups.empty()) {
            constants.flags |= flag_sorted;
            for (const ParticleInstance::State::Mesh::Group& group : mesh->groups) {
                if (group.count == 0U) {
                    continue;
                }
                const ParticleInstance::State::MeshUpload& model = i.mesh_uploads[group.upload];
                const rhi::StorageBuffer* ordered = s.particles->buffer(group.order);
                const rhi::StorageBuffer* vertices = s.particles->buffer(model.vertices);
                const rhi::StorageBuffer* indices = s.particles->buffer(model.indices);
                if (ordered == nullptr || vertices == nullptr || indices == nullptr) {
                    s.note("a mesh group's buffers are missing");
                    return false;
                }
                const rhi::StorageBuffer* group_buffers[] = {records, ordered, counters, vertices, indices, lights};
                const std::array<rhi::DrawBatch, 1U> group_batches{rhi::DrawBatch{model.index_count, group.count}};
                // The kind's own size scale is this draw's, so each group
                // carries the emitter's constants with its own scale in them.
                ViewConstants group_constants = constants;
                group_constants.extra[3] = group.scale * adjustment.size;
                group_constants.draw_tint = {group.tint.r * adjustment.color.r, group.tint.g * adjustment.color.g,
                                             group.tint.b * adjustment.color.b, group.tint.a * adjustment.color.a};
                const std::span<const core::u8> group_uniform{reinterpret_cast<const core::u8*>(&group_constants),
                                                              sizeof(group_constants)};
                rhi::PulledDraw group_draw{};
                group_draw.pipeline =
                    motion ? &s.velocity : sprite.blend == ajfx::BlendMode::Additive ? &s.additive : &s.mesh;
                group_draw.vertex_buffers = group_buffers;
                group_draw.vertex_uniform = group_uniform;
                group_draw.fragment_textures = sampled;
                group_draw.batches = group_batches;
                group_draw.load = motion || !first || targets.load;
                group_draw.store_depth = true;
                group_draw.viewport = targets.viewport;
                group_draw.raster_viewport = targets.raster_viewport;
                rhi::Texture& group_target = motion ? *targets.velocity : *targets.color;
                if (!commands.draw_pulled(group_target, targets.clear, *targets.depth, targets.depth_clear,
                                          group_draw)) {
                    s.note(motion ? "a mesh group motion draw was refused" : "a mesh group draw was refused");
                    return false;
                }
                first = false;
            }
            return true;
        }
        const ParticleInstance::State::MeshUpload* uploaded = mesh ? &i.mesh_uploads[mesh->upload] : nullptr;
        const rhi::StorageBuffer* mesh_vertices = uploaded ? s.particles->buffer(uploaded->vertices) : records;
        const rhi::StorageBuffer* mesh_indices = uploaded ? s.particles->buffer(uploaded->indices) : records;
        const rhi::StorageBuffer* mesh_args = mesh ? s.particles->buffer(mesh->args) : arguments;
        const rhi::StorageBuffer* slice_args = slice != nullptr ? s.particles->buffer(slice->args) : arguments;
        if (mesh_vertices == nullptr || mesh_indices == nullptr || mesh_args == nullptr || slice_args == nullptr) {
            s.note("an emitter's mesh buffers are missing");
            return false;
        }
        const rhi::StorageBuffer* vertex_buffers[] = {records, order, counters, mesh_vertices, mesh_indices, lights};
        // The counts come from the simulation; this batch only carries them.
        const std::array<rhi::DrawBatch, 1U> batches{rhi::DrawBatch{6U, 1U}};
        rhi::PulledDraw draw{};
        draw.pipeline = motion ? &s.velocity
            : sprite.blend == ajfx::BlendMode::Additive ? &s.additive
            : mesh ? &s.mesh
                   : &s.blended;
        draw.vertex_buffers = vertex_buffers;
        draw.vertex_uniform = emitter_uniform;
        draw.fragment_textures = sampled;
        draw.batches = batches;
        draw.indirect = mesh    ? rhi::IndirectArguments{mesh_args, 0U}
                        : slice ? rhi::IndirectArguments{slice_args, static_cast<core::u32>(slice->args_offset)}
                                : rhi::IndirectArguments{arguments, static_cast<core::u32>(buffers.draw_args_offset)};
        draw.load = motion || !first || targets.load;
        draw.store_depth = true;
        draw.viewport = targets.viewport;
        draw.raster_viewport = targets.raster_viewport;
        rhi::Texture& target = motion ? *targets.velocity : *targets.color;
        if (!commands.draw_pulled(target, targets.clear, *targets.depth, targets.depth_clear, draw)) {
            s.note(motion ? "a particle motion draw was refused" : "a particle draw was refused");
            return false;
        }
        first = false;
        return true;
    };
    // Page order, except that the interleaved emitters all draw where the
    // first of them would, slice by slice, farthest first, so particles of
    // different emitters cover each other by depth rather than by order.
    bool sliced = false;
    for (std::size_t e = 0U; e < i.effect.emitters.size(); ++e) {
        if (!ajfx::gpu::interleaved(i.effect, e) || !i.orders[e].valid()) {
            if (!draw_emitter(e, nullptr)) {
                return false;
            }
            continue;
        }
        if (sliced) {
            continue;
        }
        sliced = true;
        for (core::u32 slice = 0U; slice < ajfx::gpu::depth_slices; ++slice) {
            for (std::size_t other = e; other < i.effect.emitters.size(); ++other) {
                const ajfx::gpu::DepthSlice part = i.sorter->depth_slice(other, slice);
                if (part.args.valid() && i.orders[other].valid() && !draw_emitter(other, &part)) {
                    return false;
                }
            }
        }
    }
    return true;
}

core::u32 ParticlePass::light_count(const ParticleInstance& instance) const noexcept {
    core::u32 count = 0U;
    for (const ajfx::Emitter& emitter : instance.state_->effect.emitters) {
        count += emitter.renderer.light_count;
    }
    return count;
}

bool ParticlePass::write_lights(rhi::CommandList& commands, const ParticleInstance& instance,
                                const scene::Mat4& object_to_world, rhi::StorageBuffer& lights,
                                core::u32 first_slot) {
    State& s = *state_;
    const ParticleInstance::State& i = *instance.state_;
    // Matches ParticleLightParameters in particle_lights.comp.hlsl.
    struct Parameters final {
        std::array<float, 16U> world{};
        core::u32 stride{};
        core::u32 position_offset{};
        core::u32 color_offset{};
        core::u32 light_count{};
        core::u32 first_slot{};
        core::u32 spacing{};
        core::u32 age_offset{};
        core::u32 lifetime_offset{};
        core::u32 spawn_offset{};
        core::u32 salt{};
        core::u32 color_keys{};
        core::u32 curve_keys{};
        float intensity_min{};
        float intensity_max{};
        float range_min{};
        float range_max{};
        std::array<float, 8U> color_times{};
        std::array<std::array<float, 4U>, 8U> color_values{};
        std::array<float, 8U> curve_times{};
        std::array<float, 8U> curve_values{};
    };
    static_assert(sizeof(Parameters) == 352U);
    static_assert(ajfx::SpriteRenderer::max_light_keys == 8U);
    core::u32 slot = first_slot;
    for (std::size_t e = 0U; e < i.effect.emitters.size(); ++e) {
        const ajfx::Emitter& emitter = i.effect.emitters[e];
        const ajfx::SpriteRenderer& sprite = emitter.renderer;
        if (sprite.light_count == 0U) {
            continue;
        }
        const ajfx::gpu::EmitterBuffers buffers = i.simulation->emitter_buffers(e);
        const rhi::StorageBuffer* records = s.particles->buffer(buffers.particles);
        const rhi::StorageBuffer* counters = s.particles->buffer(buffers.counters);
        if (records == nullptr || counters == nullptr) {
            s.note("an emitter's particle buffers are missing");
            return false;
        }
        Parameters parameters;
        parameters.world = object_to_world.m;
        parameters.stride = buffers.layout.stride;
        parameters.position_offset = static_cast<core::u32>(buffers.layout.position);
        parameters.color_offset = offset_or_absent(buffers.layout.color);
        parameters.light_count = sprite.light_count;
        parameters.first_slot = slot;
        parameters.spacing = ajfx::light_spacing(i.effect, emitter);
        parameters.age_offset = static_cast<core::u32>(buffers.layout.age);
        parameters.lifetime_offset = static_cast<core::u32>(buffers.layout.lifetime);
        parameters.spawn_offset = static_cast<core::u32>(buffers.layout.spawn_index);
        parameters.salt = static_cast<core::u32>(e) * 0x9E37'79B9U;
        parameters.intensity_min = sprite.light_intensity_min;
        parameters.intensity_max = sprite.light_intensity_max;
        parameters.range_min = sprite.light_range_min;
        parameters.range_max = sprite.light_range_max;
        if (sprite.light_own_color) {
            parameters.color_keys = static_cast<core::u32>(sprite.light_colors.size());
            for (std::size_t k = 0U; k < sprite.light_colors.size(); ++k) {
                const ajfx::ColorKey& key = sprite.light_colors[k];
                parameters.color_times[k] = key.t;
                parameters.color_values[k] = {key.value.r, key.value.g, key.value.b, key.value.a};
            }
        }
        parameters.curve_keys = static_cast<core::u32>(sprite.light_curve.size());
        for (std::size_t k = 0U; k < sprite.light_curve.size(); ++k) {
            parameters.curve_times[k] = sprite.light_curve[k].t;
            parameters.curve_values[k] = sprite.light_curve[k].value;
        }
        const rhi::StorageBuffer* reads[] = {records, counters};
        const rhi::StorageBuffer* writes[] = {&lights};
        rhi::ComputeDispatch work{};
        work.pipeline = &s.lights;
        work.storage_buffers = reads;
        work.writable_buffers = writes;
        work.uniform = std::span<const core::u8>{reinterpret_cast<const core::u8*>(&parameters), sizeof(parameters)};
        work.groups_x = (sprite.light_count + 63U) / 64U;
        if (!commands.dispatch(work)) {
            s.note("a particle light dispatch was refused");
            return false;
        }
        slot += sprite.light_count;
    }
    return true;
}

std::optional<ParticleError> ParticlePass::take_error() noexcept {
    return std::exchange(state_->error, std::nullopt);
}

} // namespace mediaserver::render
