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

constexpr std::array<core::u32, 64U> round_constants{{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
}};

} // namespace

void Sha256Hasher::update(std::span<const core::u8> bytes) noexcept {
        for (const core::u8 byte : bytes) {
            buffer_[buffer_size_++] = byte;
            ++byte_length_;
            if (buffer_size_ == buffer_.size()) {
                transform(buffer_);
                buffer_size_ = 0U;
            }
        }
}

Sha256Digest Sha256Hasher::finish() const noexcept {
        Sha256Hasher copy = *this;
        const core::u64 bit_length = copy.byte_length_ * 8U;
        copy.buffer_[copy.buffer_size_++] = 0x80U;
        if (copy.buffer_size_ > 56U) {
            std::fill(copy.buffer_.begin() +
                          static_cast<std::ptrdiff_t>(copy.buffer_size_),
                      copy.buffer_.end(), core::u8{0U});
            copy.transform(copy.buffer_);
            copy.buffer_size_ = 0U;
        }
        std::fill(copy.buffer_.begin() +
                      static_cast<std::ptrdiff_t>(copy.buffer_size_),
                  copy.buffer_.begin() + 56, core::u8{0U});
        for (std::size_t index = 0U; index < 8U; ++index) {
            copy.buffer_[63U - index] = static_cast<core::u8>(
                (bit_length >> static_cast<core::u32>(index * 8U)) & 0xffU);
        }
        copy.transform(copy.buffer_);

        Sha256Digest result{};
        for (std::size_t word = 0U; word < copy.state_.size(); ++word) {
            for (std::size_t byte = 0U; byte < 4U; ++byte) {
                result.bytes[word * 4U + byte] = static_cast<core::u8>(
                    (copy.state_[word] >>
                     static_cast<core::u32>((3U - byte) * 8U)) &
                    0xffU);
            }
        }
        return result;
}

void Sha256Hasher::transform(
    const std::array<core::u8, 64U>& block) noexcept {
        std::array<core::u32, 64U> words{};
        for (std::size_t index = 0U; index < 16U; ++index) {
            const std::size_t offset = index * 4U;
            words[index] =
                (static_cast<core::u32>(block[offset]) << 24U) |
                (static_cast<core::u32>(block[offset + 1U]) << 16U) |
                (static_cast<core::u32>(block[offset + 2U]) << 8U) |
                static_cast<core::u32>(block[offset + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const core::u32 s0 = std::rotr(words[index - 15U], 7) ^
                std::rotr(words[index - 15U], 18) ^
                (words[index - 15U] >> 3U);
            const core::u32 s1 = std::rotr(words[index - 2U], 17) ^
                std::rotr(words[index - 2U], 19) ^
                (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
        }

        core::u32 a = state_[0U];
        core::u32 b = state_[1U];
        core::u32 c = state_[2U];
        core::u32 d = state_[3U];
        core::u32 e = state_[4U];
        core::u32 f = state_[5U];
        core::u32 g = state_[6U];
        core::u32 h = state_[7U];
        for (std::size_t index = 0U; index < words.size(); ++index) {
            const core::u32 sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^
                std::rotr(e, 25);
            const core::u32 choice = (e & f) ^ ((~e) & g);
            const core::u32 temporary1 = h + sum1 + choice +
                round_constants[index] + words[index];
            const core::u32 sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^
                std::rotr(a, 22);
            const core::u32 majority = (a & b) ^ (a & c) ^ (b & c);
            const core::u32 temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0U] += a;
        state_[1U] += b;
        state_[2U] += c;
        state_[3U] += d;
        state_[4U] += e;
        state_[5U] += f;
        state_[6U] += g;
        state_[7U] += h;
}

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

Sha256Digest sha256_digest(std::span<const core::u8> bytes) noexcept {
    Sha256Hasher hash;
    hash.update(bytes);
    return hash.finish();
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
