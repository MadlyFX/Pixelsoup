#include <mediaserver/assets/content_fingerprint.hpp>

#include <mediaserver/assets/asset_path.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <system_error>

namespace mediaserver::assets {
namespace {

[[nodiscard]] ContentFingerprintError error(
    ContentFingerprintErrorCode code,
    std::error_code system_error = {}) noexcept {
    return ContentFingerprintError{code, system_error};
}

[[nodiscard]] int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

} // namespace

std::string sha256_hex(const Sha256Digest& digest) {
    constexpr std::array<char, 16U> alphabet{{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    }};
    std::string result(digest.bytes.size() * 2U, '0');
    for (std::size_t index = 0U; index < digest.bytes.size(); ++index) {
        result[index * 2U] = alphabet[digest.bytes[index] >> 4U];
        result[index * 2U + 1U] = alphabet[digest.bytes[index] & 0x0fU];
    }
    return result;
}

std::optional<Sha256Digest>
sha256_from_hex(std::string_view encoded) noexcept {
    Sha256Digest result{};
    if (encoded.size() != result.bytes.size() * 2U) {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < result.bytes.size(); ++index) {
        const int high = hex_value(encoded[index * 2U]);
        const int low = hex_value(encoded[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        result.bytes[index] = static_cast<core::u8>((high << 4) | low);
    }
    return result;
}

ContentFingerprintResult content_fingerprint(
    std::string_view source, std::stop_token stop) noexcept {
    try {
        if (!valid_asset_source(source)) {
            return ContentFingerprintResult::failure(
                error(ContentFingerprintErrorCode::InvalidSource));
        }
        if (stop.stop_requested()) {
            return ContentFingerprintResult::failure(
                error(ContentFingerprintErrorCode::Cancelled));
        }

        const std::filesystem::path path = asset_path_from_source(source);
        std::error_code filesystem_error;
        const auto status = std::filesystem::status(path, filesystem_error);
        if (filesystem_error || !std::filesystem::is_regular_file(status)) {
            return ContentFingerprintResult::failure(error(
                ContentFingerprintErrorCode::SourceMissing,
                filesystem_error));
        }
        const std::uintmax_t expected_size =
            std::filesystem::file_size(path, filesystem_error);
        if (filesystem_error) {
            return ContentFingerprintResult::failure(error(
                ContentFingerprintErrorCode::OpenFailed,
                filesystem_error));
        }
        constexpr core::u64 maximum_sha256_bytes =
            std::numeric_limits<core::u64>::max() / 8U;
        if (expected_size > maximum_sha256_bytes) {
            return ContentFingerprintResult::failure(
                error(ContentFingerprintErrorCode::SizeLimitExceeded));
        }
        const auto expected_modified =
            std::filesystem::last_write_time(path, filesystem_error);
        if (filesystem_error) {
            return ContentFingerprintResult::failure(error(
                ContentFingerprintErrorCode::OpenFailed,
                filesystem_error));
        }

        std::ifstream input{path, std::ios::binary};
        if (!input) {
            return ContentFingerprintResult::failure(
                error(ContentFingerprintErrorCode::OpenFailed));
        }
        std::array<core::u8, 64U * 1024U> buffer{};
        Sha256Hasher hash;
        core::u64 length = 0U;
        while (true) {
            if (stop.stop_requested()) {
                return ContentFingerprintResult::failure(
                    error(ContentFingerprintErrorCode::Cancelled));
            }
            input.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(buffer.size()));
            const std::streamsize amount = input.gcount();
            if (amount < 0) {
                return ContentFingerprintResult::failure(
                    error(ContentFingerprintErrorCode::ReadFailed));
            }
            if (amount > 0) {
                const auto count = static_cast<std::size_t>(amount);
                if (count > maximum_sha256_bytes - length) {
                    return ContentFingerprintResult::failure(
                        error(ContentFingerprintErrorCode::SizeLimitExceeded));
                }
                hash.update(std::span<const core::u8>{buffer.data(), count});
                length += static_cast<core::u64>(count);
            }
            if (input.eof()) {
                break;
            }
            if (!input) {
                return ContentFingerprintResult::failure(
                    error(ContentFingerprintErrorCode::ReadFailed));
            }
        }

        const std::uintmax_t final_size =
            std::filesystem::file_size(path, filesystem_error);
        if (filesystem_error) {
            return ContentFingerprintResult::failure(error(
                ContentFingerprintErrorCode::ReadFailed,
                filesystem_error));
        }
        const auto final_modified =
            std::filesystem::last_write_time(path, filesystem_error);
        if (filesystem_error) {
            return ContentFingerprintResult::failure(error(
                ContentFingerprintErrorCode::ReadFailed,
                filesystem_error));
        }
        if (final_size != expected_size || final_modified != expected_modified ||
            final_size != length) {
            return ContentFingerprintResult::failure(
                error(ContentFingerprintErrorCode::SourceChanged));
        }
        return ContentFingerprintResult::success(
            ContentFingerprint{length, hash.finish()});
    } catch (const std::bad_alloc&) {
        return ContentFingerprintResult::failure(
            error(ContentFingerprintErrorCode::AllocationFailed));
    } catch (const std::filesystem::filesystem_error& caught) {
        return ContentFingerprintResult::failure(error(
            ContentFingerprintErrorCode::ReadFailed, caught.code()));
    } catch (...) {
        return ContentFingerprintResult::failure(
            error(ContentFingerprintErrorCode::ReadFailed));
    }
}

ContentFingerprintResult source_fingerprint(
    std::string_view source, std::stop_token stop) noexcept {
    try {
        if (!valid_asset_source(source)) {
            return ContentFingerprintResult::failure(
                error(ContentFingerprintErrorCode::InvalidSource));
        }
        if (stop.stop_requested()) {
            return ContentFingerprintResult::failure(
                error(ContentFingerprintErrorCode::Cancelled));
        }

        const std::filesystem::path path = asset_path_from_source(source);
        std::error_code filesystem_error;
        const auto status = std::filesystem::status(path, filesystem_error);
        if (filesystem_error || !std::filesystem::is_regular_file(status)) {
            return ContentFingerprintResult::failure(error(
                ContentFingerprintErrorCode::SourceMissing,
                filesystem_error));
        }
        const std::uintmax_t size =
            std::filesystem::file_size(path, filesystem_error);
        if (filesystem_error || size > std::numeric_limits<core::u64>::max()) {
            return ContentFingerprintResult::failure(error(
                ContentFingerprintErrorCode::OpenFailed,
                filesystem_error));
        }
        const auto modified =
            std::filesystem::last_write_time(path, filesystem_error);
        if (filesystem_error) {
            return ContentFingerprintResult::failure(error(
                ContentFingerprintErrorCode::OpenFailed,
                filesystem_error));
        }

        constexpr std::string_view domain =
            "mediaserver.fast-source-fingerprint.v1";
        Sha256Hasher signature;
        signature.update(std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(domain.data()),
            domain.size()});
        std::array<core::u8, 16U> metadata{};
        const core::u64 byte_length = static_cast<core::u64>(size);
        const core::u64 modified_ticks = static_cast<core::u64>(
            modified.time_since_epoch().count());
        for (std::size_t index = 0U; index < 8U; ++index) {
            const core::u32 shift = static_cast<core::u32>(index * 8U);
            metadata[index] = static_cast<core::u8>(byte_length >> shift);
            metadata[index + 8U] =
                static_cast<core::u8>(modified_ticks >> shift);
        }
        signature.update(metadata);
        return ContentFingerprintResult::success(
            ContentFingerprint{byte_length, signature.finish()});
    } catch (const std::bad_alloc&) {
        return ContentFingerprintResult::failure(
            error(ContentFingerprintErrorCode::AllocationFailed));
    } catch (const std::filesystem::filesystem_error& caught) {
        return ContentFingerprintResult::failure(error(
            ContentFingerprintErrorCode::ReadFailed, caught.code()));
    } catch (...) {
        return ContentFingerprintResult::failure(
            error(ContentFingerprintErrorCode::ReadFailed));
    }
}

} // namespace mediaserver::assets
