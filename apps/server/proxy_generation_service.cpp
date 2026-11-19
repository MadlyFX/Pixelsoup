#include "proxy_generation_service.hpp"

#include <mediaserver/assets/asset_path.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/assets/proxy_policy.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <span>
#include <string_view>
#include <utility>

namespace mediaserver::server {
namespace {

namespace fs = std::filesystem;

constexpr std::array<core::u8, 8U> artifact_magic{{
    // Version 2 records the key's segment.
    'M', 'S', 'P', 'R', 'O', 'X', 'Y', 2U,
}};
constexpr core::u64 maximum_service_payload_bytes =
    16ULL * 1'024ULL * 1'024ULL * 1'024ULL;
constexpr core::u64 maximum_service_proxy_frames = 10'000'000U;
constexpr core::u64 fixed_artifact_bytes = 278U;
constexpr core::u64 bytes_per_frame_map_entry = 16U;
constexpr std::size_t copy_buffer_bytes = 1U * 1'024U * 1'024U;

[[nodiscard]] ProxyGenerationConfig normalized_config(
    ProxyGenerationConfig config) noexcept {
    config.maximum_encoded_payload_bytes = std::clamp(
        config.maximum_encoded_payload_bytes, core::u64{1U},
        maximum_service_payload_bytes);
    config.maximum_proxy_frames = std::clamp(
        config.maximum_proxy_frames, core::u64{1U},
        maximum_service_proxy_frames);
    return config;
}

[[nodiscard]] bool valid_rate(assets::MediaFrameRate rate) noexcept {
    return rate.numerator != 0U && rate.denominator != 0U &&
        std::gcd(rate.numerator, rate.denominator) == 1U;
}

[[nodiscard]] std::optional<assets::MediaFrameRate> normalized_rate(
    assets::MediaFrameRate rate) noexcept {
    if (rate.numerator == 0U || rate.denominator == 0U) {
        return std::nullopt;
    }
    const core::u32 divisor = std::gcd(rate.numerator, rate.denominator);
    return assets::MediaFrameRate{rate.numerator / divisor,
                                  rate.denominator / divisor};
}

[[nodiscard]] bool valid_primaries(
    assets::DerivativeColorPrimaries value) noexcept {
    switch (value) {
    case assets::DerivativeColorPrimaries::None:
    case assets::DerivativeColorPrimaries::Rec709:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_transfer(
    assets::DerivativeTransfer value) noexcept {
    switch (value) {
    case assets::DerivativeTransfer::None:
    case assets::DerivativeTransfer::Srgb:
    case assets::DerivativeTransfer::Rec709:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_matrix(assets::DerivativeMatrix value) noexcept {
    switch (value) {
    case assets::DerivativeMatrix::None:
    case assets::DerivativeMatrix::Rgb:
    case assets::DerivativeMatrix::Rec709:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_range(
    assets::DerivativeColorRange value) noexcept {
    switch (value) {
    case assets::DerivativeColorRange::None:
    case assets::DerivativeColorRange::Full:
    case assets::DerivativeColorRange::Limited:
        return true;
    }
    return false;
}

[[nodiscard]] bool preserved_video_metadata(
    const ProxySourceMetadata& source,
    const ProxyEncodedMetadata& encoded) noexcept {
    return encoded.frame_rate == source.frame_rate &&
        encoded.pixel_aspect_numerator == source.pixel_aspect_numerator &&
        encoded.pixel_aspect_denominator == source.pixel_aspect_denominator &&
        encoded.clockwise_quarter_turns == source.clockwise_quarter_turns &&
        encoded.color_primaries == source.color_primaries &&
        encoded.transfer == source.transfer &&
        encoded.matrix == source.matrix &&
        encoded.color_range == source.color_range;
}

[[nodiscard]] bool authored_probe_matches(
    const assets::MediaAsset& asset,
    const ProxySourceMetadata& source) noexcept {
    if (asset.dimensions.has_value() &&
        *asset.dimensions != source.dimensions) {
        return false;
    }
    if (asset.frame_count.has_value() &&
        static_cast<core::u64>(*asset.frame_count) != source.frame_count) {
        return false;
    }
    if (asset.frame_rate.has_value()) {
        const auto authored = normalized_rate(*asset.frame_rate);
        if (!authored.has_value() || *authored != source.frame_rate) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_encoded_metadata(
    const ProxySourceMetadata& source,
    const ProxyEncodedMetadata& encoded,
    const assets::DerivativeProfile& profile) noexcept {
    if (encoded.dimensions.width == 0U || encoded.dimensions.height == 0U ||
        encoded.dimensions.width > profile.width ||
        encoded.dimensions.height > profile.height ||
        encoded.frame_count == 0U || !valid_rate(encoded.frame_rate) ||
        encoded.pixel_aspect_numerator == 0U ||
        encoded.pixel_aspect_denominator == 0U ||
        std::gcd(encoded.pixel_aspect_numerator,
                 encoded.pixel_aspect_denominator) != 1U ||
        encoded.clockwise_quarter_turns >= 4U ||
        !valid_primaries(encoded.color_primaries) ||
        !valid_transfer(encoded.transfer) ||
        !valid_matrix(encoded.matrix) ||
        !valid_range(encoded.color_range) ||
        encoded.container != profile.container ||
        encoded.video_codec != profile.codec ||
        encoded.pixel_format != profile.pixel_format ||
        !preserved_video_metadata(source, encoded)) {
        return false;
    }
    // A full-size proxy keeps the source size, except that 4:2:0 cannot
    // hold an odd side: that one is a single row or column larger.
    if (profile.fit_policy ==
            assets::DerivativeFitPolicy::PreserveWithinBounds &&
        encoded.dimensions !=
            assets::proxy_encoded_dimensions(source.dimensions, profile)) {
        return false;
    }
    if (source.has_audio &&
        profile.audio_codec != assets::DerivativeAudioCodec::None) {
        return encoded.audio_layout == profile.audio_layout &&
            encoded.audio_codec == profile.audio_codec &&
            encoded.audio_sample_rate == profile.audio_sample_rate &&
            encoded.audio_bitrate == profile.audio_bitrate;
    }
    return encoded.audio_layout == assets::DerivativeAudioLayout::None &&
        encoded.audio_codec == assets::DerivativeAudioCodec::None &&
        encoded.audio_sample_rate == 0U && encoded.audio_bitrate == 0U;
}

[[nodiscard]] bool exact_duration_and_map(
    const ProxySourceMetadata& source,
    const ProxyEncodedMetadata& encoded,
    const assets::ProxyFrameMap& map,
    core::u64 maximum_proxy_frames,
    const ProxySourceRange& range) noexcept {
    if (encoded.frame_count > maximum_proxy_frames ||
        map.source_time_by_proxy_frame.size() != encoded.frame_count ||
        map.source_time_by_proxy_frame.empty() ||
        map.proxy_frame_rate != encoded.frame_rate ||
        !assets::valid_proxy_frame_map(map) ||
        range.count == 0U || range.first > source.frame_count ||
        range.count > source.frame_count - range.first) {
        return false;
    }
    // The output spans exactly the range: as long in time, starting on its
    // first frame and ending before the frame after its last.
    const auto range_duration = assets::presentation_time_for_frame(
        range.count, source.frame_rate);
    const auto encoded_duration = assets::presentation_time_for_frame(
        encoded.frame_count, encoded.frame_rate);
    const auto range_start = assets::presentation_time_for_frame(
        range.first, source.frame_rate);
    const auto range_end = assets::presentation_time_for_frame(
        range.first + range.count, source.frame_rate);
    if (!range_duration || !encoded_duration || !range_start || !range_end ||
        assets::compare_presentation_time(range_duration.value(),
                                          encoded_duration.value()) != 0 ||
        map.source_time_by_proxy_frame.front() != range_start.value()) {
        return false;
    }
    for (const assets::PresentationTime& source_time :
         map.source_time_by_proxy_frame) {
        if (assets::compare_presentation_time(source_time,
                                              range_end.value()) >= 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] ProxyGenerationError generation_error(
    ProxyGenerationErrorCode code, doc::NodeId asset,
    std::optional<assets::DerivativeKey> key = std::nullopt) noexcept {
    ProxyGenerationError result{};
    result.code = code;
    result.asset = asset;
    result.key = std::move(key);
    return result;
}

[[nodiscard]] ProxyGenerationError backend_generation_error(
    const ProxyBackendError& backend, doc::NodeId asset,
    const assets::DerivativeKey& key) noexcept {
    ProxyGenerationErrorCode code = ProxyGenerationErrorCode::BackendFailure;
    switch (backend.code) {
    case ProxyBackendErrorCode::Unavailable:
        code = ProxyGenerationErrorCode::BackendUnavailable;
        break;
    case ProxyBackendErrorCode::SourceUnavailable:
        code = ProxyGenerationErrorCode::SourceUnavailable;
        break;
    case ProxyBackendErrorCode::Cancelled:
        code = ProxyGenerationErrorCode::Cancelled;
        break;
    case ProxyBackendErrorCode::AllocationFailed:
        code = ProxyGenerationErrorCode::AllocationFailed;
        break;
    case ProxyBackendErrorCode::MetadataUnavailable:
    case ProxyBackendErrorCode::EncodeFailed:
    case ProxyBackendErrorCode::OutputValidationFailed:
        break;
    }
    ProxyGenerationError result = generation_error(code, asset, key);
    result.backend_error = backend;
    result.system_error = backend.system_error;
    return result;
}

[[nodiscard]] ProxyGenerationError cache_generation_error(
    const assets::DerivativeCacheError& cache_error, doc::NodeId asset,
    const assets::DerivativeKey& key) noexcept {
    ProxyGenerationError result = generation_error(
        ProxyGenerationErrorCode::CacheFailure, asset, key);
    result.cache_error = cache_error;
    result.system_error = cache_error.system_error;
    return result;
}

[[nodiscard]] assets::DerivativeCacheError routed_cache_error(
    assets::DerivativeCacheErrorCode code,
    std::error_code system_error = {}) noexcept {
    return assets::DerivativeCacheError{code, system_error};
}

class UnavailableProxyEncoder final : public ProxyEncoderBackend {
public:
    ProxyBackendResult encode(const ProxyEncodeRequest&,
                              std::stop_token stop) override {
        return ProxyBackendResult::failure(ProxyBackendError{
            stop.stop_requested() ? ProxyBackendErrorCode::Cancelled
                                  : ProxyBackendErrorCode::Unavailable,
            {},
        });
    }
};

class TemporaryFile final {
public:
    explicit TemporaryFile(fs::path path) : path_(std::move(path)) {}
    ~TemporaryFile() {
        std::error_code ignored;
        static_cast<void>(fs::remove(path_, ignored));
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

class ArtifactWriter final {
public:
    explicit ArtifactWriter(const fs::path& path)
        : output_(path, std::ios::binary | std::ios::trunc) {}

    [[nodiscard]] bool good() const noexcept {
        return static_cast<bool>(output_);
    }

    void bytes(std::span<const core::u8> value) {
        if (failed_ || value.empty()) {
            return;
        }
        if (value.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::streamsize>::max())) {
            failed_ = true;
            return;
        }
        output_.write(reinterpret_cast<const char*>(value.data()),
                      static_cast<std::streamsize>(value.size()));
        failed_ = !output_;
    }

    void u8(core::u8 value) { bytes(std::span<const core::u8>{&value, 1U}); }

    void u16(core::u16 value) {
        const std::array<core::u8, 2U> encoded{{
            static_cast<core::u8>((value >> 8U) & 0xffU),
            static_cast<core::u8>(value & 0xffU),
        }};
        bytes(encoded);
    }

    void u32(core::u32 value) {
        const std::array<core::u8, 4U> encoded{{
            static_cast<core::u8>((value >> 24U) & 0xffU),
            static_cast<core::u8>((value >> 16U) & 0xffU),
            static_cast<core::u8>((value >> 8U) & 0xffU),
            static_cast<core::u8>(value & 0xffU),
        }};
        bytes(encoded);
    }

    void u64(core::u64 value) {
        std::array<core::u8, 8U> encoded{};
        for (std::size_t index = 0U; index < encoded.size(); ++index) {
            encoded[index] = static_cast<core::u8>(
                (value >> static_cast<core::u32>((7U - index) * 8U)) &
                0xffU);
        }
        bytes(encoded);
    }

    [[nodiscard]] bool finish() {
        output_.flush();
        failed_ = failed_ || !output_;
        output_.close();
        return !failed_ && static_cast<bool>(output_);
    }

private:
    std::ofstream output_;
    bool failed_{};
};

void write_source_metadata(ArtifactWriter& writer,
                           const ProxySourceMetadata& metadata) {
    writer.u32(metadata.dimensions.width);
    writer.u32(metadata.dimensions.height);
    writer.u64(metadata.frame_count);
    writer.u32(metadata.frame_rate.numerator);
    writer.u32(metadata.frame_rate.denominator);
    writer.u32(metadata.pixel_aspect_numerator);
    writer.u32(metadata.pixel_aspect_denominator);
    writer.u8(metadata.clockwise_quarter_turns);
    writer.u8(static_cast<core::u8>(metadata.color_primaries));
    writer.u8(static_cast<core::u8>(metadata.transfer));
    writer.u8(static_cast<core::u8>(metadata.matrix));
    writer.u8(static_cast<core::u8>(metadata.color_range));
    writer.u8(metadata.has_audio ? 1U : 0U);
}

void write_encoded_metadata(ArtifactWriter& writer,
                            const ProxyEncodedMetadata& metadata) {
    writer.u32(metadata.dimensions.width);
    writer.u32(metadata.dimensions.height);
    writer.u64(metadata.frame_count);
    writer.u32(metadata.frame_rate.numerator);
    writer.u32(metadata.frame_rate.denominator);
    writer.u32(metadata.pixel_aspect_numerator);
    writer.u32(metadata.pixel_aspect_denominator);
    writer.u8(metadata.clockwise_quarter_turns);
    writer.u8(static_cast<core::u8>(metadata.color_primaries));
    writer.u8(static_cast<core::u8>(metadata.transfer));
    writer.u8(static_cast<core::u8>(metadata.matrix));
    writer.u8(static_cast<core::u8>(metadata.color_range));
    writer.u8(static_cast<core::u8>(metadata.container));
    writer.u8(static_cast<core::u8>(metadata.video_codec));
    writer.u8(static_cast<core::u8>(metadata.pixel_format));
    writer.u8(static_cast<core::u8>(metadata.audio_layout));
    writer.u8(static_cast<core::u8>(metadata.audio_codec));
    writer.u32(metadata.audio_sample_rate);
    writer.u32(metadata.audio_bitrate);
}

[[nodiscard]] bool write_artifact(
    const fs::path& package_file, const fs::path& encoded_file,
    const assets::DerivativeKey& key, const ProxyBackendOutput& output,
    const assets::ContentFingerprint& payload_fingerprint,
    std::stop_token stop, std::error_code& system_error) {
    if (stop.stop_requested()) {
        return false;
    }
    ArtifactWriter writer{package_file};
    if (!writer.good()) {
        return false;
    }
    writer.bytes(artifact_magic);
    writer.u64(key.asset.value());
    writer.u64(key.fingerprint.byte_length);
    writer.bytes(key.fingerprint.sha256.bytes);
    writer.u16(static_cast<core::u16>(key.profile));
    writer.u32(key.profile_version);
    writer.u32(key.generator_version);
    writer.bytes(key.profile_signature.bytes);
    writer.bytes(key.digest.bytes);
    writer.u8(key.segment.has_value() ? 1U : 0U);
    writer.u32(key.segment.value_or(0U));
    write_source_metadata(writer, output.source_metadata);
    write_encoded_metadata(writer, output.encoded_metadata);
    writer.u32(output.frame_map.proxy_frame_rate.numerator);
    writer.u32(output.frame_map.proxy_frame_rate.denominator);
    writer.u64(static_cast<core::u64>(
        output.frame_map.source_time_by_proxy_frame.size()));
    for (const assets::PresentationTime& time :
         output.frame_map.source_time_by_proxy_frame) {
        writer.u64(time.numerator);
        writer.u64(time.denominator);
    }
    writer.u64(payload_fingerprint.byte_length);
    writer.bytes(payload_fingerprint.sha256.bytes);
    if (!writer.good()) {
        return false;
    }

    std::ifstream input{encoded_file, std::ios::binary};
    if (!input) {
        return false;
    }
    std::vector<core::u8> buffer(copy_buffer_bytes);
    assets::Sha256Hasher copied_hash;
    core::u64 copied_bytes = 0U;
    while (!stop.stop_requested()) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            const auto size = static_cast<std::size_t>(count);
            writer.bytes(std::span<const core::u8>{buffer.data(), size});
            copied_hash.update(
                std::span<const core::u8>{buffer.data(), size});
            copied_bytes += static_cast<core::u64>(size);
        }
        if (input.eof()) {
            break;
        }
        if (!input || !writer.good()) {
            system_error = std::make_error_code(std::errc::io_error);
            return false;
        }
    }
    if (stop.stop_requested()) {
        return false;
    }
    if (copied_bytes != payload_fingerprint.byte_length ||
        copied_hash.finish() != payload_fingerprint.sha256) {
        system_error = std::make_error_code(std::errc::state_not_recoverable);
        return false;
    }
    return writer.finish();
}

class ArtifactReader final {
public:
    explicit ArtifactReader(std::span<const core::u8> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool bytes(std::size_t count,
                             std::span<const core::u8>& value) noexcept {
        if (count > bytes_.size() - offset_) {
            return false;
        }
        value = bytes_.subspan(offset_, count);
        offset_ += count;
        return true;
    }

    [[nodiscard]] bool u8(core::u8& value) noexcept {
        std::span<const core::u8> encoded;
        if (!bytes(1U, encoded)) {
            return false;
        }
        value = encoded[0U];
        return true;
    }

    [[nodiscard]] bool u16(core::u16& value) noexcept {
        std::span<const core::u8> encoded;
        if (!bytes(2U, encoded)) {
            return false;
        }
        value = static_cast<core::u16>(
            (static_cast<core::u16>(encoded[0U]) << 8U) | encoded[1U]);
        return true;
    }

    [[nodiscard]] bool u32(core::u32& value) noexcept {
        std::span<const core::u8> encoded;
        if (!bytes(4U, encoded)) {
            return false;
        }
        value = (static_cast<core::u32>(encoded[0U]) << 24U) |
            (static_cast<core::u32>(encoded[1U]) << 16U) |
            (static_cast<core::u32>(encoded[2U]) << 8U) |
            static_cast<core::u32>(encoded[3U]);
        return true;
    }

    [[nodiscard]] bool u64(core::u64& value) noexcept {
        std::span<const core::u8> encoded;
        if (!bytes(8U, encoded)) {
            return false;
        }
        value = 0U;
        for (const core::u8 byte : encoded) {
            value = (value << 8U) | byte;
        }
        return true;
    }

    [[nodiscard]] std::span<const core::u8> remaining() const noexcept {
        return bytes_.subspan(offset_);
    }

private:
    std::span<const core::u8> bytes_;
    std::size_t offset_{};
};

[[nodiscard]] bool read_digest(ArtifactReader& reader,
                               assets::Sha256Digest& digest) noexcept {
    std::span<const core::u8> bytes;
    if (!reader.bytes(digest.bytes.size(), bytes)) {
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), digest.bytes.begin());
    return true;
}

[[nodiscard]] bool read_source_metadata(
    ArtifactReader& reader, ProxySourceMetadata& metadata) noexcept {
    core::u8 primaries = 0U;
    core::u8 transfer = 0U;
    core::u8 matrix = 0U;
    core::u8 range = 0U;
    core::u8 has_audio = 0U;
    if (!reader.u32(metadata.dimensions.width) ||
        !reader.u32(metadata.dimensions.height) ||
        !reader.u64(metadata.frame_count) ||
        !reader.u32(metadata.frame_rate.numerator) ||
        !reader.u32(metadata.frame_rate.denominator) ||
        !reader.u32(metadata.pixel_aspect_numerator) ||
        !reader.u32(metadata.pixel_aspect_denominator) ||
        !reader.u8(metadata.clockwise_quarter_turns) ||
        !reader.u8(primaries) || !reader.u8(transfer) ||
        !reader.u8(matrix) || !reader.u8(range) ||
        !reader.u8(has_audio) || has_audio > 1U) {
        return false;
    }
    metadata.color_primaries =
        static_cast<assets::DerivativeColorPrimaries>(primaries);
    metadata.transfer = static_cast<assets::DerivativeTransfer>(transfer);
    metadata.matrix = static_cast<assets::DerivativeMatrix>(matrix);
    metadata.color_range =
        static_cast<assets::DerivativeColorRange>(range);
    metadata.has_audio = has_audio != 0U;
    return true;
}

[[nodiscard]] bool read_encoded_metadata(
    ArtifactReader& reader, ProxyEncodedMetadata& metadata) noexcept {
    core::u8 primaries = 0U;
    core::u8 transfer = 0U;
    core::u8 matrix = 0U;
    core::u8 range = 0U;
    core::u8 container = 0U;
    core::u8 codec = 0U;
    core::u8 pixel_format = 0U;
    core::u8 audio_layout = 0U;
    core::u8 audio_codec = 0U;
    if (!reader.u32(metadata.dimensions.width) ||
        !reader.u32(metadata.dimensions.height) ||
        !reader.u64(metadata.frame_count) ||
        !reader.u32(metadata.frame_rate.numerator) ||
        !reader.u32(metadata.frame_rate.denominator) ||
        !reader.u32(metadata.pixel_aspect_numerator) ||
        !reader.u32(metadata.pixel_aspect_denominator) ||
        !reader.u8(metadata.clockwise_quarter_turns) ||
        !reader.u8(primaries) || !reader.u8(transfer) ||
        !reader.u8(matrix) || !reader.u8(range) ||
        !reader.u8(container) || !reader.u8(codec) ||
        !reader.u8(pixel_format) || !reader.u8(audio_layout) ||
        !reader.u8(audio_codec) ||
        !reader.u32(metadata.audio_sample_rate) ||
        !reader.u32(metadata.audio_bitrate)) {
        return false;
    }
    metadata.color_primaries =
        static_cast<assets::DerivativeColorPrimaries>(primaries);
    metadata.transfer = static_cast<assets::DerivativeTransfer>(transfer);
    metadata.matrix = static_cast<assets::DerivativeMatrix>(matrix);
    metadata.color_range =
        static_cast<assets::DerivativeColorRange>(range);
    metadata.container =
        static_cast<assets::DerivativeContainer>(container);
    metadata.video_codec = static_cast<assets::DerivativeCodec>(codec);
    metadata.pixel_format =
        static_cast<assets::DerivativePixelFormat>(pixel_format);
    metadata.audio_layout =
        static_cast<assets::DerivativeAudioLayout>(audio_layout);
    metadata.audio_codec =
        static_cast<assets::DerivativeAudioCodec>(audio_codec);
    return true;
}

[[nodiscard]] std::optional<ProxyArtifactLoad> parse_artifact(
    const assets::DerivativeKey& expected_key,
    assets::DerivativeCacheEntry cache_entry,
    std::vector<core::u8> package,
    core::u64 maximum_proxy_frames, bool verify_payload_digest) {
    ArtifactReader reader{package};
    std::span<const core::u8> magic;
    assets::DerivativeKey key{};
    core::u64 asset_id = 0U;
    core::u16 profile = 0U;
    if (!reader.bytes(artifact_magic.size(), magic) ||
        !std::equal(magic.begin(), magic.end(), artifact_magic.begin()) ||
        !reader.u64(asset_id) ||
        !reader.u64(key.fingerprint.byte_length) ||
        !read_digest(reader, key.fingerprint.sha256) ||
        !reader.u16(profile) || !reader.u32(key.profile_version) ||
        !reader.u32(key.generator_version) ||
        !read_digest(reader, key.profile_signature) ||
        !read_digest(reader, key.digest)) {
        return std::nullopt;
    }
    core::u8 segmented = 0U;
    core::u32 segment = 0U;
    if (!reader.u8(segmented) || segmented > 1U || !reader.u32(segment) ||
        (segmented == 0U && segment != 0U)) {
        return std::nullopt;
    }
    if (segmented != 0U) {
        key.segment = segment;
    }
    key.asset = doc::NodeId{asset_id};
    key.profile = static_cast<assets::DerivativeProfileId>(profile);
    if (key != expected_key || !assets::valid_derivative_key(key)) {
        return std::nullopt;
    }

    ProxyBackendOutput output{};
    output.encoded_source_fingerprint = key.fingerprint;
    if (!read_source_metadata(reader, output.source_metadata) ||
        !read_encoded_metadata(reader, output.encoded_metadata) ||
        !reader.u32(output.frame_map.proxy_frame_rate.numerator) ||
        !reader.u32(output.frame_map.proxy_frame_rate.denominator)) {
        return std::nullopt;
    }
    core::u64 map_count = 0U;
    if (!reader.u64(map_count) || map_count > maximum_proxy_frames ||
        map_count > output.frame_map.source_time_by_proxy_frame.max_size()) {
        return std::nullopt;
    }
    output.frame_map.source_time_by_proxy_frame.reserve(
        static_cast<std::size_t>(map_count));
    for (core::u64 index = 0U; index < map_count; ++index) {
        assets::PresentationTime time{};
        if (!reader.u64(time.numerator) || !reader.u64(time.denominator)) {
            return std::nullopt;
        }
        output.frame_map.source_time_by_proxy_frame.push_back(time);
    }
    assets::ContentFingerprint payload_fingerprint{};
    if (!reader.u64(payload_fingerprint.byte_length) ||
        !read_digest(reader, payload_fingerprint.sha256)) {
        return std::nullopt;
    }
    const std::span<const core::u8> payload = reader.remaining();
    if (payload.size() != payload_fingerprint.byte_length ||
        (verify_payload_digest &&
         assets::sha256_digest(payload) != payload_fingerprint.sha256)) {
        return std::nullopt;
    }
    const assets::DerivativeProfile* const definition =
        assets::derivative_profile(key.profile);
    std::optional<ProxySourceRange> range;
    if (key.segment.has_value()) {
        range = proxy_segment_source_range(*key.segment,
                                           output.source_metadata);
        if (!range.has_value()) {
            return std::nullopt;
        }
    }
    if (definition == nullptr ||
        !valid_proxy_backend_output(output, *definition,
                                    maximum_proxy_frames, range)) {
        return std::nullopt;
    }

    ProxyArtifactLoad loaded{};
    loaded.receipt = ProxyGenerationReceipt{
        key.asset,
        key,
        output.source_metadata,
        output.encoded_metadata,
        std::move(output.frame_map),
        payload_fingerprint,
        std::move(cache_entry),
        true,
    };
    loaded.encoded_payload.assign(payload.begin(), payload.end());
    return loaded;
}

[[nodiscard]] core::u64 maximum_artifact_bytes(
    const ProxyGenerationConfig& config) noexcept {
    const core::u64 map_bytes =
        config.maximum_proxy_frames * bytes_per_frame_map_entry;
    if (config.maximum_encoded_payload_bytes >
        std::numeric_limits<core::u64>::max() - fixed_artifact_bytes -
            map_bytes) {
        return std::numeric_limits<core::u64>::max();
    }
    return config.maximum_encoded_payload_bytes + fixed_artifact_bytes +
        map_bytes;
}

[[nodiscard]] fs::path work_path(const fs::path& root,
                                 const assets::DerivativeKey& key,
                                 std::string_view suffix) {
    static std::atomic<core::u64> sequence{};
    const std::string name = assets::derivative_key_string(key) + "." +
        std::to_string(sequence.fetch_add(1U)) + std::string{suffix};
    return root / ".proxy-work" / name;
}

} // namespace

bool valid_proxy_source_metadata(
    const ProxySourceMetadata& metadata) noexcept {
    return metadata.dimensions.width != 0U &&
        metadata.dimensions.height != 0U && metadata.frame_count != 0U &&
        valid_rate(metadata.frame_rate) &&
        metadata.pixel_aspect_numerator != 0U &&
        metadata.pixel_aspect_denominator != 0U &&
        std::gcd(metadata.pixel_aspect_numerator,
                 metadata.pixel_aspect_denominator) == 1U &&
        metadata.clockwise_quarter_turns < 4U &&
        valid_primaries(metadata.color_primaries) &&
        valid_transfer(metadata.transfer) &&
        valid_matrix(metadata.matrix) && valid_range(metadata.color_range);
}

std::optional<ProxySourceRange>
proxy_segment_source_range(core::u32 segment,
                           const ProxySourceMetadata& source) noexcept {
    const auto range = assets::proxy_segment_range(
        segment, source.frame_count, source.frame_rate);
    if (!range.has_value()) {
        return std::nullopt;
    }
    return ProxySourceRange{range->first, range->count};
}

bool valid_proxy_backend_output(
    const ProxyBackendOutput& output,
    const assets::DerivativeProfile& profile,
    core::u64 maximum_proxy_frames,
    std::optional<ProxySourceRange> range) noexcept {
    return profile.kind == assets::DerivativeKind::Proxy &&
        profile.frame_rate_policy ==
            assets::DerivativeFrameRatePolicy::PreserveSource &&
        profile.orientation_policy ==
            assets::DerivativeOrientationPolicy::PreserveSourceMetadata &&
        profile.pixel_aspect_policy ==
            assets::DerivativePixelAspectPolicy::PreserveSourceMetadata &&
        profile.width != 0U && profile.height != 0U &&
        valid_proxy_source_metadata(output.source_metadata) &&
        valid_encoded_metadata(output.source_metadata,
                               output.encoded_metadata, profile) &&
        exact_duration_and_map(
            output.source_metadata, output.encoded_metadata, output.frame_map,
            maximum_proxy_frames,
            range.value_or(
                ProxySourceRange{0U, output.source_metadata.frame_count}));
}

std::unique_ptr<ProxyEncoderBackend> unavailable_proxy_encoder_backend() {
    return std::make_unique<UnavailableProxyEncoder>();
}

ProxyCacheClearResult clear_proxy_cache_root(const fs::path& root) noexcept {
    try {
        const fs::path normalized = root.lexically_normal();
        if (normalized.empty() || !normalized.is_absolute() ||
            normalized == normalized.root_path()) {
            return ProxyCacheClearResult{
                std::make_error_code(std::errc::invalid_argument)};
        }
        constexpr std::array<assets::DerivativeProfileId, 2U>
            proxy_profiles{
                assets::DerivativeProfileId::Proxy720pCfrH264,
                assets::DerivativeProfileId::ProxySourceCfrHevcMain10};
        std::array<fs::path, 4U> owned_children{
            normalized / ".proxy-work",
            normalized / ".proxy-playback",
            {},
            {}};
        for (std::size_t index = 0U; index < proxy_profiles.size(); ++index) {
            const assets::DerivativeProfile* const profile =
                assets::derivative_profile(proxy_profiles[index]);
            if (profile == nullptr) {
                return ProxyCacheClearResult{
                    std::make_error_code(std::errc::invalid_argument)};
            }
            owned_children[index + 2U] =
                normalized / "v1" / profile->name;
        }
        for (const fs::path& owned_child : owned_children) {
            const fs::path child = owned_child.lexically_normal();
            const fs::path version_root = normalized / "v1";
            if (child == normalized ||
                (child.parent_path() != normalized &&
                 (child.parent_path() != version_root ||
                  version_root.parent_path() != normalized))) {
                return ProxyCacheClearResult{
                    std::make_error_code(std::errc::invalid_argument)};
            }
            std::error_code remove_error;
            static_cast<void>(fs::remove_all(child, remove_error));
            if (remove_error) {
                return ProxyCacheClearResult{remove_error};
            }
        }
        return {};
    } catch (const std::bad_alloc&) {
        return ProxyCacheClearResult{
            std::make_error_code(std::errc::not_enough_memory)};
    } catch (const fs::filesystem_error& caught) {
        return ProxyCacheClearResult{caught.code()};
    } catch (...) {
        return ProxyCacheClearResult{
            std::make_error_code(std::errc::io_error)};
    }
}

ProxyGenerationService::ProxyGenerationService(
    assets::DerivativeCache cache, ProxyGenerationConfig config)
    : ProxyGenerationService(std::move(cache),
                             unavailable_proxy_encoder_backend(), config) {}

ProxyGenerationService::ProxyGenerationService(
    assets::DerivativeCache cache,
    std::unique_ptr<ProxyEncoderBackend> backend,
    ProxyGenerationConfig config)
    : backend_(backend ? std::move(backend)
                       : unavailable_proxy_encoder_backend()),
      config_(normalized_config(config)) {
    runtime_root_ = cache.root();
    fixed_cache_ =
        std::make_unique<assets::DerivativeCache>(std::move(cache));
}

ProxyGenerationService::ProxyGenerationService(
    NextToSourceProxyCacheConfig cache,
    std::unique_ptr<ProxyEncoderBackend> backend,
    ProxyGenerationConfig config)
    : next_to_source_cache_(std::move(cache)),
      runtime_root_(next_to_source_cache_->runtime_root),
      backend_(backend ? std::move(backend)
                       : unavailable_proxy_encoder_backend()),
      config_(normalized_config(config)) {}

assets::DerivativeCache* ProxyGenerationService::cache_for(
    const assets::MediaAsset& asset,
    assets::DerivativeCacheError& error) noexcept {
    if (fixed_cache_ != nullptr) {
        return fixed_cache_.get();
    }
    if (!next_to_source_cache_.has_value() ||
        next_to_source_cache_->maximum_bytes == 0U ||
        !assets::valid_asset_source(asset.source)) {
        error = routed_cache_error(
            assets::DerivativeCacheErrorCode::InvalidRoot);
        return nullptr;
    }
    try {
        const fs::path root =
            (assets::asset_path_from_source(asset.source).parent_path() /
             ".mediaserver-proxies")
                .lexically_normal();
        if (root.empty() || !root.is_absolute()) {
            error = routed_cache_error(
                assets::DerivativeCacheErrorCode::InvalidRoot);
            return nullptr;
        }
        std::scoped_lock lock{routed_caches_mutex_};
        const auto existing = std::find_if(
            routed_caches_.begin(), routed_caches_.end(),
            [&](const RoutedCache& candidate) {
                return candidate.root == root;
            });
        if (existing != routed_caches_.end()) {
            return existing->cache.get();
        }
        auto opened = assets::DerivativeCache::open(
            assets::DerivativeCacheConfig{
                root, next_to_source_cache_->maximum_bytes});
        if (!opened) {
            error = opened.error();
            return nullptr;
        }
        auto cache = std::make_unique<assets::DerivativeCache>(
            std::move(opened).value());
        assets::DerivativeCache* const result = cache.get();
        routed_caches_.push_back(RoutedCache{root, std::move(cache)});
        return result;
    } catch (const std::bad_alloc&) {
        error = routed_cache_error(
            assets::DerivativeCacheErrorCode::AllocationFailed);
        return nullptr;
    } catch (const fs::filesystem_error& caught) {
        error = routed_cache_error(
            assets::DerivativeCacheErrorCode::OpenFailed, caught.code());
        return nullptr;
    } catch (...) {
        error = routed_cache_error(
            assets::DerivativeCacheErrorCode::OpenFailed);
        return nullptr;
    }
}

ProxyArtifactLoadResult ProxyGenerationService::load_from_cache(
    assets::DerivativeCache& cache, const assets::DerivativeKey& key,
    std::stop_token stop, bool verify_payload_digest) noexcept {
    try {
        auto loaded = cache.load(key, maximum_artifact_bytes(config_), false,
                                 stop, verify_payload_digest);
        if (!loaded) {
            return ProxyArtifactLoadResult::failure(
                cache_generation_error(loaded.error(), key.asset, key));
        }
        if (loaded.value().entry.state == assets::DerivativeState::Cancelled ||
            stop.stop_requested()) {
            return ProxyArtifactLoadResult::failure(generation_error(
                ProxyGenerationErrorCode::Cancelled, key.asset, key));
        }
        if (loaded.value().entry.state != assets::DerivativeState::Ready) {
            return ProxyArtifactLoadResult::failure(generation_error(
                ProxyGenerationErrorCode::CacheNotReady, key.asset, key));
        }
        auto artifact = parse_artifact(
            key, std::move(loaded.value().entry),
            std::move(loaded.value().payload), config_.maximum_proxy_frames,
            verify_payload_digest);
        if (!artifact.has_value()) {
            return ProxyArtifactLoadResult::failure(generation_error(
                ProxyGenerationErrorCode::ArtifactInvalid, key.asset, key));
        }
        return ProxyArtifactLoadResult::success(std::move(*artifact));
    } catch (const std::bad_alloc&) {
        return ProxyArtifactLoadResult::failure(generation_error(
            ProxyGenerationErrorCode::AllocationFailed, key.asset, key));
    } catch (...) {
        return ProxyArtifactLoadResult::failure(generation_error(
            ProxyGenerationErrorCode::ArtifactInvalid, key.asset, key));
    }
}

ProxyArtifactLoadResult ProxyGenerationService::load(
    const assets::DerivativeKey& key, std::stop_token stop,
    bool verify_payload_digest) noexcept {
    if (fixed_cache_ == nullptr) {
        const auto error = routed_cache_error(
            assets::DerivativeCacheErrorCode::InvalidRoot);
        return ProxyArtifactLoadResult::failure(
            cache_generation_error(error, key.asset, key));
    }
    return load_from_cache(*fixed_cache_, key, stop, verify_payload_digest);
}

ProxyArtifactLoadResult ProxyGenerationService::load(
    const assets::MediaAsset& asset, const assets::DerivativeKey& key,
    std::stop_token stop, bool verify_payload_digest) noexcept {
    assets::DerivativeCacheError error{};
    assets::DerivativeCache* const cache = cache_for(asset, error);
    if (cache == nullptr) {
        return ProxyArtifactLoadResult::failure(
            cache_generation_error(error, key.asset, key));
    }
    return load_from_cache(*cache, key, stop, verify_payload_digest);
}

fs::path proxy_piece_path(const fs::path& output_file, core::u32 piece) {
    // Like every work file, a piece ends in .tmp: `<name>.mp4.<piece>.tmp`.
    fs::path path = output_file;
    path.replace_extension();
    path += "." + std::to_string(piece) + ".tmp";
    return path;
}

ProxyGenerationResult ProxyGenerationService::publish_encoded(
    const assets::MediaAsset& asset, assets::DerivativeCache& cache,
    const assets::DerivativeKey& key, const fs::path& raw_path,
    const ProxyBackendOutput& encoded,
    std::optional<ProxySourceRange> range,
    const assets::ContentFingerprint& before, std::stop_token stop) {
    std::error_code filesystem_error;
    const fs::path artifact_path =
        work_path(cache.root(), key, ".artifact.tmp");
    TemporaryFile artifact_file{artifact_path};
    const assets::DerivativeProfile* const profile =
        assets::derivative_profile(*asset.requested_proxy_profile);
    if (profile == nullptr) {
        return ProxyGenerationResult::failure(generation_error(
            ProxyGenerationErrorCode::InvalidProfile, asset.id, key));
    }
    if (stop.stop_requested()) {
        return ProxyGenerationResult::failure(generation_error(
            ProxyGenerationErrorCode::Cancelled, asset.id, key));
    }

    const auto after = assets::source_fingerprint(asset.source, stop);
    if (!after) {
        if (after.error().code ==
                assets::ContentFingerprintErrorCode::Cancelled ||
            stop.stop_requested()) {
            return ProxyGenerationResult::failure(generation_error(
                ProxyGenerationErrorCode::Cancelled, asset.id, key));
        }
        ProxyGenerationError error = generation_error(
            ProxyGenerationErrorCode::SourceUnavailable, asset.id, key);
        error.system_error = after.error().system_error;
        return ProxyGenerationResult::failure(std::move(error));
    }
    if (after.value() != *asset.fingerprint ||
        after.value() != before) {
        return ProxyGenerationResult::failure(generation_error(
            ProxyGenerationErrorCode::FingerprintMismatch, asset.id,
            key));
    }
    if (encoded.encoded_source_fingerprint !=
        *asset.fingerprint) {
        return ProxyGenerationResult::failure(generation_error(
            ProxyGenerationErrorCode::FingerprintMismatch, asset.id,
            key));
    }
    if (!authored_probe_matches(asset, encoded.source_metadata) ||
        !valid_proxy_backend_output(encoded, *profile,
                                    config_.maximum_proxy_frames,
                                    range)) {
        return ProxyGenerationResult::failure(generation_error(
            ProxyGenerationErrorCode::InvalidMetadata, asset.id, key));
    }

    filesystem_error.clear();
    if (!fs::is_regular_file(raw_path, filesystem_error) ||
        filesystem_error) {
        ProxyGenerationError error = generation_error(
            ProxyGenerationErrorCode::OutputMissing, asset.id, key);
        error.system_error = filesystem_error;
        return ProxyGenerationResult::failure(std::move(error));
    }
    const core::u64 output_bytes =
        fs::file_size(raw_path, filesystem_error);
    if (filesystem_error || output_bytes == 0U) {
        ProxyGenerationError error = generation_error(
            ProxyGenerationErrorCode::OutputMissing, asset.id, key);
        error.system_error = filesystem_error;
        return ProxyGenerationResult::failure(std::move(error));
    }
    if (output_bytes > config_.maximum_encoded_payload_bytes) {
        return ProxyGenerationResult::failure(generation_error(
            ProxyGenerationErrorCode::OutputTooLarge, asset.id, key));
    }
    const auto payload_fingerprint = assets::content_fingerprint(
        raw_path.generic_string(), stop);
    if (!payload_fingerprint) {
        if (payload_fingerprint.error().code ==
                assets::ContentFingerprintErrorCode::Cancelled ||
            stop.stop_requested()) {
            return ProxyGenerationResult::failure(generation_error(
                ProxyGenerationErrorCode::Cancelled, asset.id, key));
        }
        ProxyGenerationError error = generation_error(
            ProxyGenerationErrorCode::ArtifactWriteFailed, asset.id,
            key);
        error.system_error = payload_fingerprint.error().system_error;
        return ProxyGenerationResult::failure(std::move(error));
    }
    if (payload_fingerprint.value().byte_length != output_bytes) {
        return ProxyGenerationResult::failure(generation_error(
            ProxyGenerationErrorCode::ArtifactWriteFailed, asset.id,
            key));
    }

    filesystem_error.clear();
    if (!write_artifact(artifact_file.path(), raw_path, key,
                        encoded, payload_fingerprint.value(),
                        stop, filesystem_error)) {
        ProxyGenerationError error = generation_error(
            stop.stop_requested()
                ? ProxyGenerationErrorCode::Cancelled
                : ProxyGenerationErrorCode::ArtifactWriteFailed,
            asset.id, key);
        error.system_error = filesystem_error;
        return ProxyGenerationResult::failure(std::move(error));
    }
    auto published = cache.publish_file(key, artifact_file.path(),
                                          false, stop);
    // Counted even when it failed: the entry on disk may still have changed.
    publications_.fetch_add(1U, std::memory_order_release);
    if (!published) {
        return ProxyGenerationResult::failure(
            cache_generation_error(published.error(), asset.id, key));
    }
    if (published.value().state == assets::DerivativeState::Cancelled ||
        stop.stop_requested()) {
        return ProxyGenerationResult::failure(generation_error(
            ProxyGenerationErrorCode::Cancelled, asset.id, key));
    }
    if (published.value().state != assets::DerivativeState::Ready) {
        return ProxyGenerationResult::failure(generation_error(
            ProxyGenerationErrorCode::CacheNotReady, asset.id, key));
    }

    ProxyGenerationReceipt receipt{
        asset.id,
        key,
        encoded.source_metadata,
        encoded.encoded_metadata,
        encoded.frame_map,
        payload_fingerprint.value(),
        std::move(published.value()),
        false,
    };
    return ProxyGenerationResult::success(std::move(receipt));
}

ProxyGenerationResult ProxyGenerationService::generate(
    const assets::MediaAsset& asset, std::stop_token stop,
    ProxyEncodeProgressCallback progress, Step step) noexcept {
    try {
        if (!asset.id || !assets::valid_asset_source(asset.source)) {
            return ProxyGenerationResult::failure(generation_error(
                ProxyGenerationErrorCode::InvalidAsset, asset.id));
        }
        if (!asset.fingerprint.has_value()) {
            return ProxyGenerationResult::failure(generation_error(
                ProxyGenerationErrorCode::MissingFingerprint, asset.id));
        }
        if (!asset.requested_proxy_profile.has_value()) {
            return ProxyGenerationResult::failure(generation_error(
                ProxyGenerationErrorCode::ProxyNotConfigured, asset.id));
        }
        if (!assets::valid_requested_proxy_profile(
                *asset.requested_proxy_profile)) {
            return ProxyGenerationResult::failure(generation_error(
                ProxyGenerationErrorCode::InvalidProfile, asset.id));
        }
        const assets::DerivativeProfile* const profile =
            assets::derivative_profile(*asset.requested_proxy_profile);
        auto key = assets::requested_proxy_key(asset);
        if (profile == nullptr || !key.has_value() ||
            (step.segment.has_value() && step.join_segments) ||
            step.segments == 0U ||
            (step.segments != 1U && !step.segment.has_value())) {
            return ProxyGenerationResult::failure(generation_error(
                ProxyGenerationErrorCode::InvalidProfile, asset.id));
        }
        // Segments are cut from the authored probe, which the encoder's own
        // probe must match, so a segment's frames are the same either way.
        const auto segment_range = [&](core::u32 segment)
            -> std::optional<ProxySourceRange> {
            if (!asset.frame_count.has_value() ||
                !asset.frame_rate.has_value()) {
                return std::nullopt;
            }
            const auto range = assets::proxy_segment_range(
                segment, *asset.frame_count, *asset.frame_rate);
            if (!range.has_value()) {
                return std::nullopt;
            }
            return ProxySourceRange{range->first, range->count};
        };
        const core::u32 segment_count =
            asset.frame_count.has_value() && asset.frame_rate.has_value()
                ? assets::proxy_segment_count(*asset.frame_count,
                                              *asset.frame_rate)
                : 0U;
        const assets::DerivativeKey whole_key = *key;
        std::optional<ProxySourceRange> range;
        const bool run = step.segments != 1U;
        // The run left to make once segments the cache already holds are
        // skipped.
        core::u32 run_first = step.segment.value_or(0U);
        core::u32 run_count = step.segments;
        if (step.segment.has_value()) {
            range = segment_range(*step.segment);
            const auto last_range = segment_range(static_cast<core::u32>(
                std::min<core::u64>(static_cast<core::u64>(*step.segment) +
                                        step.segments - 1U,
                                    std::numeric_limits<core::u32>::max())));
            if (!range.has_value() || !last_range.has_value() ||
                static_cast<core::u64>(*step.segment) + step.segments - 1U >=
                    segment_count) {
                return ProxyGenerationResult::failure(generation_error(
                    ProxyGenerationErrorCode::InvalidProfile, asset.id));
            }
            // A run encodes every frame its segments cover.
            range->count = last_range->first + last_range->count - range->first;
            key = assets::derivative_segment_key(whole_key, *step.segment);
        }
        if (step.join_segments && segment_count == 0U) {
            return ProxyGenerationResult::failure(generation_error(
                ProxyGenerationErrorCode::InvalidProfile, asset.id));
        }
        if (stop.stop_requested()) {
            return ProxyGenerationResult::failure(generation_error(
                ProxyGenerationErrorCode::Cancelled, asset.id, key));
        }

        assets::DerivativeCacheError cache_error{};
        assets::DerivativeCache* const cache = cache_for(asset, cache_error);
        if (cache == nullptr) {
            return ProxyGenerationResult::failure(
                cache_generation_error(cache_error, asset.id, *key));
        }

        // A run reports the segments it starts with that are already made,
        // as it would ones it made, and encodes from the first missing one.
        std::optional<ProxyGenerationResult> last_published;
        while (true) {
            auto cached = load_from_cache(*cache, *key, stop);
            if (!cached) {
                if (cached.error().code !=
                        ProxyGenerationErrorCode::CacheNotReady &&
                    cached.error().code !=
                        ProxyGenerationErrorCode::ArtifactInvalid) {
                    return ProxyGenerationResult::failure(cached.error());
                }
                break;
            }
            ProxyGenerationReceipt receipt =
                std::move(cached).value().receipt;
            if (!authored_probe_matches(asset, receipt.source_metadata)) {
                return ProxyGenerationResult::failure(generation_error(
                    ProxyGenerationErrorCode::InvalidMetadata, asset.id,
                    key));
            }
            receipt.cache_hit = true;
            if (!run) {
                return ProxyGenerationResult::success(std::move(receipt));
            }
            last_published = ProxyGenerationResult::success(std::move(receipt));
            if (step.published &&
                !step.published(run_first, last_published->value())) {
                return ProxyGenerationResult::failure(generation_error(
                    ProxyGenerationErrorCode::Cancelled, asset.id, key));
            }
            ++run_first;
            --run_count;
            if (run_count == 0U) {
                return std::move(*last_published);
            }
            key = assets::derivative_segment_key(whole_key, run_first);
            const auto next = segment_range(run_first);
            if (!next.has_value()) {
                return ProxyGenerationResult::failure(generation_error(
                    ProxyGenerationErrorCode::InvalidProfile, asset.id));
            }
            range->count -= next->first - range->first;
            range->first = next->first;
        }

        const auto before = assets::source_fingerprint(asset.source, stop);
        if (!before) {
            if (before.error().code ==
                    assets::ContentFingerprintErrorCode::Cancelled ||
                stop.stop_requested()) {
                return ProxyGenerationResult::failure(generation_error(
                    ProxyGenerationErrorCode::Cancelled, asset.id, key));
            }
            ProxyGenerationError error = generation_error(
                ProxyGenerationErrorCode::SourceUnavailable, asset.id, key);
            error.system_error = before.error().system_error;
            return ProxyGenerationResult::failure(std::move(error));
        }
        if (before.value() != *asset.fingerprint) {
            return ProxyGenerationResult::failure(generation_error(
                ProxyGenerationErrorCode::FingerprintMismatch, asset.id,
                key));
        }

        const fs::path raw_path = work_path(cache->root(), *key, ".mp4.tmp");
        std::error_code filesystem_error;
        static_cast<void>(fs::create_directories(raw_path.parent_path(),
                                                 filesystem_error));
        if (filesystem_error) {
            ProxyGenerationError error = generation_error(
                ProxyGenerationErrorCode::ArtifactWriteFailed, asset.id,
                key);
            error.system_error = filesystem_error;
            return ProxyGenerationResult::failure(std::move(error));
        }
        TemporaryFile raw_file{raw_path};
        // A join reads each published segment back out to a work file; the
        // backend remuxes those files rather than the cache's envelopes.
        std::vector<std::unique_ptr<TemporaryFile>> segment_files;
        std::vector<fs::path> join_paths;
        if (step.join_segments) {
            segment_files.reserve(segment_count);
            join_paths.reserve(segment_count);
            for (core::u32 segment = 0U; segment < segment_count; ++segment) {
                const assets::DerivativeKey segment_key =
                    assets::derivative_segment_key(whole_key, segment);
                auto loaded = load_from_cache(*cache, segment_key, stop, false);
                if (!loaded) {
                    return ProxyGenerationResult::failure(generation_error(
                        stop.stop_requested()
                            ? ProxyGenerationErrorCode::Cancelled
                            : ProxyGenerationErrorCode::CacheNotReady,
                        asset.id, key));
                }
                auto file = std::make_unique<TemporaryFile>(work_path(
                    cache->root(), segment_key, ".segment.mp4.tmp"));
                std::ofstream output{file->path(),
                                     std::ios::binary | std::ios::trunc};
                const auto& bytes = loaded.value().encoded_payload;
                output.write(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size()));
                output.close();
                if (!output) {
                    return ProxyGenerationResult::failure(generation_error(
                        ProxyGenerationErrorCode::ArtifactWriteFailed,
                        asset.id, key));
                }
                join_paths.push_back(file->path());
                segment_files.push_back(std::move(file));
            }
        }
        ProxyEncodeProgressCallback guarded_progress;
        if (progress) {
            guarded_progress =
                [callback = std::move(progress)](core::u64 completed,
                                                  core::u64 total) noexcept {
                    try {
                        callback(completed, total);
                    } catch (...) {
                        // Status reporting is observational and must not turn
                        // a valid encode into a failed cache publication.
                    }
                };
        }
        ProxyEncodeRequest request{
            asset.id,
            asset.source,
            *asset.fingerprint,
            *key,
            *profile,
            raw_file.path(),
            std::move(guarded_progress),
            step.yield_to_playback,
            range,
            std::move(join_paths),
        };
        // A run publishes each segment from inside the encode, as soon as
        // the backend hands it over, so it can be drawn while the next one
        // is made.
        core::u32 published_count{};
        bool run_stopped = false;
        if (run) {
            request.piece_frames = assets::proxy_segment_frames(
                *asset.frame_rate);
            request.piece_done = [&](core::u32 piece,
                                     const ProxyBackendOutput& output) {
                const TemporaryFile file{
                    proxy_piece_path(raw_file.path(), piece)};
                try {
                    const core::u32 segment = run_first + piece;
                    if (piece != published_count || piece >= run_count) {
                        last_published = ProxyGenerationResult::failure(
                            generation_error(
                                ProxyGenerationErrorCode::InvalidMetadata,
                                asset.id, key));
                        return false;
                    }
                    const assets::DerivativeKey segment_key =
                        assets::derivative_segment_key(whole_key, segment);
                    // A segment made earlier, by another run, is kept.
                    auto made = load_from_cache(*cache, segment_key, stop,
                                                false);
                    if (made) {
                        last_published = ProxyGenerationResult::success(
                            std::move(made).value().receipt);
                    } else {
                        last_published = publish_encoded(
                            asset, *cache, segment_key, file.path(), output,
                            segment_range(segment), before.value(), stop);
                    }
                    if (!*last_published) {
                        return false;
                    }
                    ++published_count;
                    if (step.published &&
                        !step.published(segment, last_published->value())) {
                        run_stopped = true;
                        return false;
                    }
                    return true;
                } catch (const std::bad_alloc&) {
                    last_published = ProxyGenerationResult::failure(
                        generation_error(
                            ProxyGenerationErrorCode::AllocationFailed,
                            asset.id, key));
                    return false;
                }
            };
        }
        ProxyBackendResult encoded = backend_->encode(request, stop);
        if (run) {
            // Pieces the backend wrote but never handed over.
            for (core::u32 piece = 0U; piece < run_count; ++piece) {
                std::error_code ignored;
                static_cast<void>(fs::remove(
                    proxy_piece_path(raw_file.path(), piece), ignored));
            }
            if (last_published.has_value() && !*last_published) {
                return std::move(*last_published);
            }
            if (run_stopped || stop.stop_requested()) {
                return ProxyGenerationResult::failure(generation_error(
                    ProxyGenerationErrorCode::Cancelled, asset.id, key));
            }
            if (!encoded) {
                return ProxyGenerationResult::failure(
                    backend_generation_error(encoded.error(), asset.id,
                                             *key));
            }
            if (published_count != run_count ||
                !last_published.has_value()) {
                return ProxyGenerationResult::failure(generation_error(
                    ProxyGenerationErrorCode::InvalidMetadata, asset.id,
                    key));
            }
            return std::move(*last_published);
        }
        if (!encoded) {
            return ProxyGenerationResult::failure(
                backend_generation_error(encoded.error(), asset.id, *key));
        }
        return publish_encoded(asset, *cache, *key, raw_file.path(),
                               encoded.value(), range, before.value(), stop);
    } catch (const std::bad_alloc&) {
        return ProxyGenerationResult::failure(generation_error(
            ProxyGenerationErrorCode::AllocationFailed, asset.id));
    } catch (const fs::filesystem_error& caught) {
        ProxyGenerationError error = generation_error(
            ProxyGenerationErrorCode::ArtifactWriteFailed, asset.id);
        error.system_error = caught.code();
        return ProxyGenerationResult::failure(std::move(error));
    } catch (...) {
        return ProxyGenerationResult::failure(generation_error(
            ProxyGenerationErrorCode::BackendFailure, asset.id));
    }
}

core::Result<assets::DerivativeCacheEntry, assets::DerivativeCacheError>
ProxyGenerationService::inspect(const assets::DerivativeKey& key) noexcept {
    if (fixed_cache_ == nullptr) {
        return core::Result<assets::DerivativeCacheEntry,
                            assets::DerivativeCacheError>::failure(
            routed_cache_error(
                assets::DerivativeCacheErrorCode::InvalidRoot));
    }
    return fixed_cache_->inspect(key);
}

core::Result<assets::DerivativeCacheEntry, assets::DerivativeCacheError>
ProxyGenerationService::inspect(
    const assets::MediaAsset& asset,
    const assets::DerivativeKey& key) noexcept {
    assets::DerivativeCacheError error{};
    assets::DerivativeCache* const cache = cache_for(asset, error);
    if (cache == nullptr) {
        return core::Result<assets::DerivativeCacheEntry,
                            assets::DerivativeCacheError>::failure(error);
    }
    return cache->inspect(key);
}

const fs::path& ProxyGenerationService::cache_root() const noexcept {
    return runtime_root_;
}

core::u64 ProxyGenerationService::publications() const noexcept {
    return publications_.load(std::memory_order_acquire);
}

} // namespace mediaserver::server
