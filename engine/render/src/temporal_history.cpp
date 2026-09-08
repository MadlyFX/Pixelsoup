#include <mediaserver/render/temporal_history.hpp>

#include <bit>

namespace mediaserver::render {
namespace {

[[nodiscard]] core::u64 splitmix64(core::u64 value) noexcept {
    value += 0x9E37'79B9'7F4A'7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D0'49BB'1331'11EBULL;
    return value ^ (value >> 31U);
}

void mix(core::u64& state, core::u64 value) noexcept {
    state = splitmix64(state ^ splitmix64(value));
}

void mix(core::u64& state, float value) noexcept {
    mix(state, static_cast<core::u64>(std::bit_cast<core::u32>(value)));
}

void mix(core::u64& state, const scene::Vec3& value) noexcept {
    mix(state, value.x);
    mix(state, value.y);
    mix(state, value.z);
}

void mix(core::u64& state, const scene::Mat4& value) noexcept {
    for (const float lane : value.m) {
        mix(state, lane);
    }
}

} // namespace

HistoryDecision TemporalHistory::observe(
    const HistoryKey& key,
    core::u64 show_frame,
    core::u64 topology_digest,
    core::u64 visual_digest) noexcept {
    HistoryDecision decision{};
    decision.show_frame = show_frame;
    decision.previous_show_frame = show_frame_;

    if (!initialized_) {
        key_ = key;
        show_frame_ = show_frame;
        topology_digest_ = topology_digest;
        visual_digest_ = visual_digest;
        initialized_ = true;
        decision.disposition = HistoryDisposition::Initialize;
        decision.reset_count = reset_count_;
        return decision;
    }

    auto previous_identity = key_;
    auto current_identity = key;
    if (previous_identity.temporal_document_digest && current_identity.temporal_document_digest) {
        previous_identity.document_version = current_identity.document_version = {};
    }
    const bool same_identity = previous_identity == current_identity;
    const bool same_topology = topology_digest_ == topology_digest;
    const bool same_visual = visual_digest_ == visual_digest;
    if (same_identity && same_topology && same_visual
        && show_frame == show_frame_) {
        key_ = key;
        decision.disposition = HistoryDisposition::Hold;
        decision.reset_count = reset_count_;
        return decision;
    }
    if (same_identity && same_topology && show_frame_ != ~core::u64{0U} &&
        show_frame == show_frame_ + 1U) {
        decision.disposition = HistoryDisposition::Advance;
        key_ = key;
        show_frame_ = show_frame;
        visual_digest_ = visual_digest;
        decision.reset_count = reset_count_;
        return decision;
    }

    ++reset_count_;
    key_ = key;
    show_frame_ = show_frame;
    topology_digest_ = topology_digest;
    visual_digest_ = visual_digest;
    decision.disposition = HistoryDisposition::Reset;
    decision.reset_count = reset_count_;
    return decision;
}

void TemporalHistory::reset() noexcept {
    initialized_ = false;
    key_ = HistoryKey{};
    show_frame_ = 0U;
    topology_digest_ = 0U;
    visual_digest_ = 0U;
}

core::u64 deterministic_sample_bits(
    core::u64 show_frame,
    core::u64 stable_identity,
    core::u32 canvas_x,
    core::u32 canvas_y,
    core::u32 dimension) noexcept {
    core::u64 state = splitmix64(show_frame);
    mix(state, stable_identity);
    mix(state, (static_cast<core::u64>(canvas_x) << 32U) |
                   static_cast<core::u64>(canvas_y));
    mix(state, static_cast<core::u64>(dimension));
    return state;
}

float deterministic_sample_unit(
    core::u64 show_frame,
    core::u64 stable_identity,
    core::u32 canvas_x,
    core::u32 canvas_y,
    core::u32 dimension) noexcept {
    // Exactly 24 random bits fit in a binary32 significand. Scaling by 2^-24
    // is exact and produces [0, 1) on every supported compiler.
    const core::u32 bits = static_cast<core::u32>(
        deterministic_sample_bits(show_frame, stable_identity, canvas_x,
                                  canvas_y, dimension) >> 40U);
    return static_cast<float>(bits) * 0x1p-24F;
}

core::u64 camera_history_digest(const scene::Camera& camera) noexcept {
    core::u64 state = 0x4341'4D45'5241'7631ULL; // "CAMERAv1"
    // Pose is deliberately absent. A sequentially animated camera must use
    // its previous view-projection to produce camera motion. The authored
    // camera node identity in HistoryKey detects a cut; projection and
    // exposure changes remain explicit temporal resets.
    mix(state, camera.field_of_view);
    mix(state, camera.near_plane);
    mix(state, camera.far_plane);
    mix(state, camera.aperture_f_number);
    mix(state, camera.shutter_seconds);
    mix(state, camera.iso);
    mix(state, camera.exposure_compensation_ev);
    return state;
}

core::u64 view_history_digest(
    const scene::ViewMatrices& matrices,
    scene::Vec3 eye,
    scene::Vec3 forward) noexcept {
    core::u64 state = 0x5649'4557'7631'0001ULL; // "VIEWv1"
    mix(state, matrices.view);
    mix(state, matrices.projection);
    mix(state, matrices.view_projection);
    mix(state, eye);
    mix(state, forward);
    return state;
}

core::u64 renderer_profile_digest(
    const scene::RendererSettings& settings) noexcept {
    core::u64 state = 0x5052'4F46'494C'4531ULL; // "PROFILE1"
    mix(state, static_cast<core::u64>(settings.backend));
    mix(state, static_cast<core::u64>(settings.required));
    mix(state, static_cast<core::u64>(settings.quality));
    mix(state, static_cast<core::u64>(settings.ray_tracing));
    mix(state, static_cast<core::u64>(settings.ddgi));
    mix(state, static_cast<core::u64>(settings.restir));
    mix(state, static_cast<core::u64>(settings.denoising));
    mix(state, static_cast<core::u64>(settings.taau));
    mix(state, static_cast<core::u64>(settings.spatial_antialiasing));
    mix(state, static_cast<core::u64>(settings.taa_sequence_length));
    mix(state, static_cast<core::u64>(settings.motion_blur));
    mix(state, static_cast<core::u64>(settings.ddgi_probe_count_x));
    mix(state, static_cast<core::u64>(settings.ddgi_probe_count_y));
    mix(state, static_cast<core::u64>(settings.ddgi_probe_count_z));
    mix(state, static_cast<core::u64>(settings.ddgi_rays_per_probe));
    mix(state, static_cast<core::u64>(settings.slice_apron_limit));
    mix(state, scene::renderer_settings_digest(settings));
    return state;
}

} // namespace mediaserver::render
