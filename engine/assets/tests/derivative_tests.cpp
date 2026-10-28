#include <mediaserver/assets/derivative.hpp>

#include <array>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} +
                                 std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define DERIVATIVE_CHECK(...)                                                \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] assets::ContentFingerprint fingerprint(
    std::string_view content) {
    const auto* data = reinterpret_cast<const core::u8*>(content.data());
    return assets::ContentFingerprint{
        static_cast<core::u64>(content.size()),
        assets::sha256_digest(
            std::span<const core::u8>{data, content.size()}),
    };
}

void profile_catalog_freezes_output_semantics() {
    const auto profiles = assets::derivative_profiles();
    DERIVATIVE_CHECK(profiles.size() == 6U);
    // A model's thumbnail is its own frozen profile, never the media one.
    const auto* model = assets::derivative_profile(
        assets::DerivativeProfileId::ModelTurntable320x180Srgb);
    DERIVATIVE_CHECK(model != nullptr);
    DERIVATIVE_CHECK(static_cast<core::u16>(model->id) == 6U);
    DERIVATIVE_CHECK(model->name == "model-turntable-320x180-srgb");
    DERIVATIVE_CHECK(model->width == 320U && model->height == 180U);
    DERIVATIVE_CHECK(model->sampling ==
                     assets::DerivativeSampling::BoundsFramedView);
    DERIVATIVE_CHECK(model->container == assets::DerivativeContainer::Png);
    DERIVATIVE_CHECK(assets::derivative_profile(
                         "model-turntable-320x180-srgb") == model);
    const auto* thumbnail = assets::derivative_profile(
        assets::DerivativeProfileId::Thumbnail320x180Srgb);
    DERIVATIVE_CHECK(thumbnail != nullptr);
    DERIVATIVE_CHECK(thumbnail->name == "thumb-320x180-srgb");
    DERIVATIVE_CHECK(thumbnail->sampling ==
                     assets::DerivativeSampling::
                         FirstValidAtOrAfterTenPercent);
    DERIVATIVE_CHECK(thumbnail->fit_policy ==
                     assets::DerivativeFitPolicy::TransparentLetterbox);
    DERIVATIVE_CHECK(thumbnail->orientation_policy ==
                     assets::DerivativeOrientationPolicy::ApplySourceRotation);

    const auto* proxy = assets::derivative_profile(
        assets::DerivativeProfileId::Proxy720pCfrH264);
    DERIVATIVE_CHECK(proxy != nullptr);
    DERIVATIVE_CHECK(proxy->frame_rate_policy ==
                     assets::DerivativeFrameRatePolicy::PreserveSource);
    DERIVATIVE_CHECK(proxy->audio_codec ==
                     assets::DerivativeAudioCodec::Aac);
    DERIVATIVE_CHECK(proxy->audio_sample_rate == 48'000U);

    const auto* full_resolution = assets::derivative_profile(
        assets::DerivativeProfileId::ProxySourceCfrHevcMain10);
    DERIVATIVE_CHECK(full_resolution != nullptr);
    DERIVATIVE_CHECK(full_resolution->name ==
                     "proxy-source-cfr-hevc-main10");
    DERIVATIVE_CHECK(full_resolution->codec ==
                     assets::DerivativeCodec::Hevc);
    DERIVATIVE_CHECK(full_resolution->pixel_format ==
                     assets::DerivativePixelFormat::Yuv420p10);
    DERIVATIVE_CHECK(full_resolution->fit_policy ==
                     assets::DerivativeFitPolicy::PreserveWithinBounds);
    DERIVATIVE_CHECK(full_resolution->profile_version == 1U);
    DERIVATIVE_CHECK(full_resolution->keyframe_interval == 30U);
}

void every_identity_member_changes_the_key() {
    const auto* profile = assets::derivative_profile(
        assets::DerivativeProfileId::Thumbnail320x180Srgb);
    DERIVATIVE_CHECK(profile != nullptr);
    const auto source = fingerprint("source-a");
    const auto base = assets::make_derivative_key(doc::NodeId{7U}, source,
                                                   *profile);
    const auto repeated = assets::make_derivative_key(
        doc::NodeId{7U}, source, *profile);
    DERIVATIVE_CHECK(base == repeated);
    DERIVATIVE_CHECK(assets::valid_derivative_key(base));

    const auto other_asset = assets::make_derivative_key(
        doc::NodeId{8U}, source, *profile);
    const auto other_source = assets::make_derivative_key(
        doc::NodeId{7U}, fingerprint("source-b"), *profile);
    assets::DerivativeProfile changed_profile = *profile;
    ++changed_profile.profile_version;
    const auto other_profile_version = assets::make_derivative_key(
        doc::NodeId{7U}, source, changed_profile);
    changed_profile = *profile;
    ++changed_profile.generator_version;
    const auto other_generator = assets::make_derivative_key(
        doc::NodeId{7U}, source, changed_profile);
    changed_profile = *profile;
    changed_profile.width += 1U;
    const auto other_semantics = assets::make_derivative_key(
        doc::NodeId{7U}, source, changed_profile);

    DERIVATIVE_CHECK(base.digest != other_asset.digest);
    DERIVATIVE_CHECK(base.digest != other_source.digest);
    DERIVATIVE_CHECK(base.digest != other_profile_version.digest);
    DERIVATIVE_CHECK(base.digest != other_generator.digest);
    DERIVATIVE_CHECK(base.digest != other_semantics.digest);
    DERIVATIVE_CHECK(base.profile_signature !=
                     other_semantics.profile_signature);
}

void cache_paths_are_content_addressed_and_machine_local() {
    const auto* profile = assets::derivative_profile(
        assets::DerivativeProfileId::Thumbnail320x180Srgb);
    DERIVATIVE_CHECK(profile != nullptr);
    const auto key = assets::make_derivative_key(
        doc::NodeId{42U}, fingerprint("C:/media/private/source.mov"),
        *profile);
    const std::filesystem::path relative =
        assets::derivative_cache_relative_path(key);
    const std::string encoded = relative.generic_string();
    DERIVATIVE_CHECK(encoded.starts_with("v1/thumb-320x180-srgb/"));
    DERIVATIVE_CHECK(encoded.ends_with(".msderiv"));
    DERIVATIVE_CHECK(encoded.find("source.mov") == std::string::npos);
    DERIVATIVE_CHECK(encoded.find("private") == std::string::npos);
    DERIVATIVE_CHECK(relative.filename().string().size() == 72U);
}

void thumbnail_probe_order_is_exact_and_falls_back() {
    DERIVATIVE_CHECK(!assets::thumbnail_probe_ordinal(0U, 0U).has_value());
    DERIVATIVE_CHECK(assets::thumbnail_probe_ordinal(1U, 0U) == 0U);
    DERIVATIVE_CHECK(!assets::thumbnail_probe_ordinal(1U, 1U).has_value());
    DERIVATIVE_CHECK(assets::thumbnail_probe_ordinal(100U, 0U) == 10U);
    DERIVATIVE_CHECK(assets::thumbnail_probe_ordinal(100U, 89U) == 99U);
    DERIVATIVE_CHECK(assets::thumbnail_probe_ordinal(100U, 90U) == 9U);
    DERIVATIVE_CHECK(assets::thumbnail_probe_ordinal(100U, 99U) == 0U);
    DERIVATIVE_CHECK(assets::thumbnail_probe_ordinal(11U, 0U) == 2U);
}

void filmstrip_ordinals_include_exact_endpoints() {
    DERIVATIVE_CHECK(!assets::filmstrip_source_ordinals(0U).has_value());
    const auto one = assets::filmstrip_source_ordinals(1U);
    DERIVATIVE_CHECK(one.has_value());
    DERIVATIVE_CHECK(*one == std::array<core::u64, 8U>{});
    const auto eight = assets::filmstrip_source_ordinals(8U);
    DERIVATIVE_CHECK(eight.has_value());
    DERIVATIVE_CHECK(*eight == std::array<core::u64, 8U>{
        0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U});
    const auto hundred_one = assets::filmstrip_source_ordinals(101U);
    DERIVATIVE_CHECK(hundred_one.has_value());
    DERIVATIVE_CHECK(*hundred_one == std::array<core::u64, 8U>{
        0U, 14U, 28U, 42U, 57U, 71U, 85U, 100U});
}

void state_transitions_reject_impossible_shortcuts() {
    using State = assets::DerivativeState;
    DERIVATIVE_CHECK(assets::valid_derivative_state_transition(
        State::Missing, State::Queued));
    DERIVATIVE_CHECK(!assets::valid_derivative_state_transition(
        State::Missing, State::Ready));
    DERIVATIVE_CHECK(assets::valid_derivative_state_transition(
        State::Generating, State::Cancelled));
    DERIVATIVE_CHECK(assets::valid_derivative_state_transition(
        State::Ready, State::Stale));
    DERIVATIVE_CHECK(!assets::valid_derivative_state_transition(
        State::Failed, State::Ready));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 6U> cases{{
    {"profile catalog freezes output semantics",
     &profile_catalog_freezes_output_semantics},
    {"every identity member changes the key",
     &every_identity_member_changes_the_key},
    {"cache paths are content addressed and machine local",
     &cache_paths_are_content_addressed_and_machine_local},
    {"thumbnail probe order is exact and falls back",
     &thumbnail_probe_order_is_exact_and_falls_back},
    {"filmstrip ordinals include exact endpoints",
     &filmstrip_ordinals_include_exact_endpoints},
    {"state transitions reject impossible shortcuts",
     &state_transitions_reject_impossible_shortcuts},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& caught) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << caught.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
