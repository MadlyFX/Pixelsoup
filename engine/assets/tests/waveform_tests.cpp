#include <mediaserver/assets/waveform.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace fs = std::filesystem;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} +
                                 std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define WAVEFORM_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<core::u64> sequence{};
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch().count();
        path_ = fs::temp_directory_path() /
            ("mediaserver_waveform_" + std::to_string(stamp) + "_" +
             std::to_string(sequence.fetch_add(1U)));
        std::error_code error;
        if (!fs::create_directory(path_, error) || error) {
            throw std::runtime_error{"could not create temporary directory"};
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        static_cast<void>(fs::remove_all(path_, ignored));
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

[[nodiscard]] assets::ContentFingerprint fingerprint(
    std::string_view revision) {
    const auto* bytes = reinterpret_cast<const core::u8*>(revision.data());
    return assets::ContentFingerprint{
        static_cast<core::u64>(revision.size()),
        assets::sha256_digest(
            std::span<const core::u8>{bytes, revision.size()}),
    };
}

[[nodiscard]] assets::MediaAsset media_asset(
    std::optional<assets::ContentFingerprint> content =
        fingerprint("waveform-revision")) {
    assets::MediaAsset asset{};
    asset.id = doc::NodeId{701U};
    asset.display_name = "Dialogue";
    asset.source = "C:\\show\\dialogue.wav";
    asset.fingerprint = std::move(content);
    return asset;
}

[[nodiscard]] assets::DerivativeCache open_cache(const fs::path& root) {
    auto opened = assets::DerivativeCache::open({root, 1U << 20U});
    WAVEFORM_CHECK(opened);
    return std::move(opened).value();
}

void validation_fails_closed() {
    const std::array samples{0.0F, 0.5F};
    WAVEFORM_CHECK(assets::build_waveform_peak_rms(samples, 0U).error() ==
                   assets::WaveformError::InvalidChannelCount);
    WAVEFORM_CHECK(assets::build_waveform_peak_rms(
                       samples, assets::maximum_waveform_channels + 1U)
                       .error() ==
                   assets::WaveformError::InvalidChannelCount);
    WAVEFORM_CHECK(assets::build_waveform_peak_rms({}, 1U).error() ==
                   assets::WaveformError::EmptySource);
    WAVEFORM_CHECK(assets::build_waveform_peak_rms(samples, 3U).error() ==
                   assets::WaveformError::IncompleteFrame);
    const std::array nonfinite{
        0.0F, std::numeric_limits<float>::quiet_NaN()};
    WAVEFORM_CHECK(assets::build_waveform_peak_rms(nonfinite, 1U).error() ==
                   assets::WaveformError::NonFiniteSample);
}

void fixed_bins_repeat_short_sources_and_aggregate_channels() {
    // Four stereo frames. Each occupies 512 bins under the fixed profile.
    const std::array samples{
        -1.0F, 0.0F,
        0.5F, -0.5F,
        0.25F, 0.75F,
        2.0F, -2.0F,
    };
    const auto built = assets::build_waveform_peak_rms(samples, 2U);
    WAVEFORM_CHECK(built);
    const auto& bins = built.value();
    WAVEFORM_CHECK(bins.size() == assets::waveform_peak_rms_bins);
    WAVEFORM_CHECK(bins[0U].peak == 1.0F);
    WAVEFORM_CHECK(std::fabs(bins[0U].rms -
                             static_cast<float>(std::sqrt(0.5))) < 1.0e-6F);
    WAVEFORM_CHECK(bins[511U] == bins[0U]);
    WAVEFORM_CHECK(bins[512U] == assets::WaveformPeakRms{0.5F, 0.5F});
    WAVEFORM_CHECK(bins[1'024U].peak == 0.75F);
    WAVEFORM_CHECK(bins[1'536U] == assets::WaveformPeakRms{1.0F, 1.0F});
    WAVEFORM_CHECK(bins.back() == bins[1'536U]);
}

void payload_is_versioned_little_endian_and_round_trips_exactly() {
    const std::array samples{-0.0F, 0.25F, -0.75F, 1.0F};
    const auto built = assets::build_waveform_peak_rms(samples, 1U);
    WAVEFORM_CHECK(built);
    std::vector<core::u8> payload =
        assets::encode_waveform_peak_rms(built.value());
    WAVEFORM_CHECK(payload.size() ==
                   12U + assets::waveform_peak_rms_bins * 8U);
    WAVEFORM_CHECK(payload[0U] == 'M' && payload[1U] == 'S' &&
                   payload[2U] == 'W' && payload[3U] == 'F');
    WAVEFORM_CHECK(payload[4U] == 1U && payload[5U] == 0U);
    const auto decoded = assets::decode_waveform_peak_rms(payload);
    WAVEFORM_CHECK(decoded);
    WAVEFORM_CHECK(decoded.value() == built.value());

    payload[4U] = 2U;
    WAVEFORM_CHECK(assets::decode_waveform_peak_rms(payload).error() ==
                   assets::WaveformError::UnsupportedVersion);
    payload[4U] = 1U;
    payload.pop_back();
    WAVEFORM_CHECK(assets::decode_waveform_peak_rms(payload).error() ==
                   assets::WaveformError::InvalidPayload);
}

void payload_rejects_nonfinite_or_impossible_envelopes() {
    assets::WaveformEnvelope envelope{};
    envelope[3U] = assets::WaveformPeakRms{0.25F, 0.5F};
    auto impossible = assets::encode_waveform_peak_rms(envelope);
    WAVEFORM_CHECK(assets::decode_waveform_peak_rms(impossible).error() ==
                   assets::WaveformError::InvalidPayload);

    envelope[3U] = assets::WaveformPeakRms{
        std::numeric_limits<float>::infinity(), 0.0F};
    auto nonfinite = assets::encode_waveform_peak_rms(envelope);
    WAVEFORM_CHECK(assets::decode_waveform_peak_rms(nonfinite).error() ==
                   assets::WaveformError::InvalidPayload);
}

void manifest_identity_publishes_and_loads_the_exact_cache_key() {
    TemporaryDirectory temporary;
    auto cache = open_cache(temporary.path() / "cache");
    const auto asset = media_asset();
    const std::array samples{-1.0F, 0.5F, 0.25F, -0.75F};

    auto published = assets::publish_waveform_derivative(
        cache, asset, samples, 1U);
    WAVEFORM_CHECK(published);
    WAVEFORM_CHECK(published.value().key.asset == asset.id);
    WAVEFORM_CHECK(published.value().key.fingerprint == *asset.fingerprint);
    WAVEFORM_CHECK(published.value().key.profile ==
                   assets::DerivativeProfileId::Waveform2048PeakRms);
    WAVEFORM_CHECK(published.value().entry.state ==
                   assets::DerivativeState::Ready);

    auto loaded = assets::load_waveform_derivative(cache, asset);
    WAVEFORM_CHECK(loaded);
    WAVEFORM_CHECK(loaded.value().key == published.value().key);
    WAVEFORM_CHECK(loaded.value().envelope == published.value().envelope);
    WAVEFORM_CHECK(loaded.value().entry.state ==
                   assets::DerivativeState::Ready);

    auto changed = asset;
    changed.fingerprint = fingerprint("different-revision");
    auto missing = assets::load_waveform_derivative(cache, changed);
    WAVEFORM_CHECK(!missing);
    WAVEFORM_CHECK(missing.error().code ==
                   assets::WaveformDerivativeErrorCode::NotReady);
    WAVEFORM_CHECK(missing.error().cache_state ==
                   assets::DerivativeState::Missing);
}

void derivative_boundary_fails_closed_for_identity_and_payload_errors() {
    TemporaryDirectory temporary;
    auto cache = open_cache(temporary.path() / "cache");
    const std::array samples{0.25F};

    auto without_fingerprint = media_asset(std::nullopt);
    auto missing_fingerprint = assets::publish_waveform_derivative(
        cache, without_fingerprint, samples, 1U);
    WAVEFORM_CHECK(!missing_fingerprint);
    WAVEFORM_CHECK(missing_fingerprint.error().code ==
                   assets::WaveformDerivativeErrorCode::MissingFingerprint);

    auto invalid = media_asset();
    invalid.id = {};
    auto invalid_asset = assets::load_waveform_derivative(cache, invalid);
    WAVEFORM_CHECK(!invalid_asset);
    WAVEFORM_CHECK(invalid_asset.error().code ==
                   assets::WaveformDerivativeErrorCode::InvalidAsset);

    const auto asset = media_asset();
    const auto* profile = assets::derivative_profile(
        assets::DerivativeProfileId::Waveform2048PeakRms);
    WAVEFORM_CHECK(profile != nullptr);
    const auto key = assets::make_derivative_key(
        asset.id, *asset.fingerprint, *profile);
    const std::array<core::u8, 4U> malformed{{'N', 'O', 'P', 'E'}};
    WAVEFORM_CHECK(cache.publish(key, malformed));
    auto corrupt = assets::load_waveform_derivative(cache, asset);
    WAVEFORM_CHECK(!corrupt);
    WAVEFORM_CHECK(corrupt.error().code ==
                   assets::WaveformDerivativeErrorCode::InvalidCachedPayload);
    WAVEFORM_CHECK(corrupt.error().waveform_error ==
                   assets::WaveformError::InvalidPayload);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 6U> cases{{
    {"validation fails closed", &validation_fails_closed},
    {"fixed bins repeat short sources and aggregate channels",
     &fixed_bins_repeat_short_sources_and_aggregate_channels},
    {"payload is versioned little endian and round trips exactly",
     &payload_is_versioned_little_endian_and_round_trips_exactly},
    {"payload rejects nonfinite or impossible envelopes",
     &payload_rejects_nonfinite_or_impossible_envelopes},
    {"manifest identity publishes and loads the exact cache key",
     &manifest_identity_publishes_and_loads_the_exact_cache_key},
    {"derivative boundary fails closed for identity and payload errors",
     &derivative_boundary_fails_closed_for_identity_and_payload_errors},
}};

} // namespace

int main() {
    std::size_t failures{};
    for (const Case& test : cases) {
        try {
            std::cout << "[run ] " << test.name << std::endl;
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": "
                      << exception.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
