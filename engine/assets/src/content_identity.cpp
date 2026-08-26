#include <mediaserver/assets/content_identity.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <system_error>

namespace mediaserver::assets {
namespace {

class Sha256 final {
  public:
    void update(std::span<const core::u8> input) noexcept {
        for (const core::u8 byte : input) {
            block_[block_size_++] = byte;
            bit_count_ += 8U;
            if (block_size_ == block_.size()) {
                transform();
                block_size_ = 0U;
            }
        }
    }

    [[nodiscard]] Sha256Digest finish() noexcept {
        block_[block_size_++] = 0x80U;
        if (block_size_ > 56U) {
            std::fill(block_.begin() +
                          static_cast<std::ptrdiff_t>(block_size_),
                      block_.end(), core::u8{0U});
            transform();
            block_size_ = 0U;
        }
        std::fill(block_.begin() +
                      static_cast<std::ptrdiff_t>(block_size_),
                  block_.begin() + 56, core::u8{0U});
        for (std::size_t index = 0U; index < 8U; ++index) {
            block_[63U - index] = static_cast<core::u8>(
                (bit_count_ >> static_cast<core::u32>(index * 8U)) & 0xffU);
        }
        transform();

        Sha256Digest output{};
        for (std::size_t word = 0U; word < state_.size(); ++word) {
            for (std::size_t byte = 0U; byte < 4U; ++byte) {
                output[word * 4U + byte] = static_cast<core::u8>(
                    (state_[word] >> static_cast<core::u32>(
                                          (3U - byte) * 8U)) &
                    0xffU);
            }
        }
        return output;
    }

  private:
    [[nodiscard]] static core::u32 choose(core::u32 x, core::u32 y,
                                          core::u32 z) noexcept {
        return (x & y) ^ (~x & z);
    }

    [[nodiscard]] static core::u32 majority(core::u32 x, core::u32 y,
                                            core::u32 z) noexcept {
        return (x & y) ^ (x & z) ^ (y & z);
    }

    void transform() noexcept {
        static constexpr std::array<core::u32, 64U> constants{{
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
        std::array<core::u32, 64U> schedule{};
        for (std::size_t index = 0U; index < 16U; ++index) {
            const std::size_t offset = index * 4U;
            schedule[index] =
                (static_cast<core::u32>(block_[offset]) << 24U) |
                (static_cast<core::u32>(block_[offset + 1U]) << 16U) |
                (static_cast<core::u32>(block_[offset + 2U]) << 8U) |
                static_cast<core::u32>(block_[offset + 3U]);
        }
        for (std::size_t index = 16U; index < schedule.size(); ++index) {
            const core::u32 s0 =
                std::rotr(schedule[index - 15U], 7) ^
                std::rotr(schedule[index - 15U], 18) ^
                (schedule[index - 15U] >> 3U);
            const core::u32 s1 =
                std::rotr(schedule[index - 2U], 17) ^
                std::rotr(schedule[index - 2U], 19) ^
                (schedule[index - 2U] >> 10U);
            schedule[index] = schedule[index - 16U] + s0 +
                              schedule[index - 7U] + s1;
        }

        core::u32 a = state_[0U];
        core::u32 b = state_[1U];
        core::u32 c = state_[2U];
        core::u32 d = state_[3U];
        core::u32 e = state_[4U];
        core::u32 f = state_[5U];
        core::u32 g = state_[6U];
        core::u32 h = state_[7U];
        for (std::size_t index = 0U; index < schedule.size(); ++index) {
            const core::u32 sigma1 =
                std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const core::u32 first = h + sigma1 + choose(e, f, g) +
                                    constants[index] + schedule[index];
            const core::u32 sigma0 =
                std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const core::u32 second = sigma0 + majority(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + first;
            d = c;
            c = b;
            b = a;
            a = first + second;
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

    std::array<core::u32, 8U> state_{{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    }};
    std::array<core::u8, 64U> block_{};
    std::size_t block_size_{};
    core::u64 bit_count_{};
};

[[nodiscard]] ContentIntegrityError error(
    ContentIntegrityErrorCode code,
    std::error_code system_error = {}) noexcept {
    return ContentIntegrityError{code, system_error};
}

[[nodiscard]] ContentIdentityResult hash_file_impl(
    const std::filesystem::path& path,
    std::optional<core::u64> known_size,
    std::stop_token stop) {
    const auto cancelled = [&]() {
        return ContentIdentityResult::failure(
            error(ContentIntegrityErrorCode::Cancelled));
    };
    if (stop.stop_requested()) {
        return cancelled();
    }
    if (path.empty()) {
        return ContentIdentityResult::failure(
            error(ContentIntegrityErrorCode::InvalidPath));
    }

    std::error_code status_error;
    const bool regular = std::filesystem::is_regular_file(path, status_error);
    if (stop.stop_requested()) {
        return cancelled();
    }
    if (status_error) {
        return ContentIdentityResult::failure(
            error(ContentIntegrityErrorCode::OpenFailed, status_error));
    }
    if (!regular) {
        return ContentIdentityResult::failure(
            error(ContentIntegrityErrorCode::NotRegularFile));
    }

    std::error_code size_error;
    const std::uintmax_t initial_size =
        std::filesystem::file_size(path, size_error);
    if (stop.stop_requested()) {
        return cancelled();
    }
    if (size_error) {
        return ContentIdentityResult::failure(
            error(ContentIntegrityErrorCode::OpenFailed, size_error));
    }
    if (initial_size >
        static_cast<std::uintmax_t>(std::numeric_limits<core::u64>::max())) {
        return ContentIdentityResult::failure(
            error(ContentIntegrityErrorCode::FileTooLarge));
    }
    const core::u64 expected_size = static_cast<core::u64>(initial_size);
    if (known_size.has_value() && *known_size != expected_size) {
        return ContentIdentityResult::failure(
            error(ContentIntegrityErrorCode::FileChanged));
    }
    std::error_code timestamp_error;
    const auto initial_timestamp =
        std::filesystem::last_write_time(path, timestamp_error);
    if (stop.stop_requested()) {
        return cancelled();
    }
    if (timestamp_error) {
        return ContentIdentityResult::failure(
            error(ContentIntegrityErrorCode::OpenFailed, timestamp_error));
    }

    std::ifstream input{path, std::ios::binary};
    if (stop.stop_requested()) {
        return cancelled();
    }
    if (!input.is_open()) {
        return ContentIdentityResult::failure(
            error(ContentIntegrityErrorCode::OpenFailed));
    }

    constexpr std::size_t buffer_bytes = 64U * 1024U;
    std::array<core::u8, buffer_bytes> buffer{};
    Sha256 hash;
    core::u64 bytes_read = 0U;
    while (input) {
        if (stop.stop_requested()) {
            return cancelled();
        }
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        if (stop.stop_requested()) {
            return cancelled();
        }
        const std::streamsize count = input.gcount();
        if (count > 0) {
            const auto narrow = static_cast<std::size_t>(count);
            hash.update(std::span<const core::u8>{buffer.data(), narrow});
            if (stop.stop_requested()) {
                return cancelled();
            }
            if (bytes_read > std::numeric_limits<core::u64>::max() -
                                 static_cast<core::u64>(narrow)) {
                return ContentIdentityResult::failure(
                    error(ContentIntegrityErrorCode::FileTooLarge));
            }
            bytes_read += static_cast<core::u64>(narrow);
        }
    }
    if (!input.eof()) {
        return ContentIdentityResult::failure(
            error(ContentIntegrityErrorCode::ReadFailed));
    }

    size_error.clear();
    const std::uintmax_t final_size = std::filesystem::file_size(path, size_error);
    if (stop.stop_requested()) {
        return cancelled();
    }
    if (size_error) {
        return ContentIdentityResult::failure(
            error(ContentIntegrityErrorCode::ReadFailed, size_error));
    }
    if (final_size != initial_size || bytes_read != expected_size) {
        return ContentIdentityResult::failure(
            error(ContentIntegrityErrorCode::FileChanged));
    }
    timestamp_error.clear();
    const auto final_timestamp =
        std::filesystem::last_write_time(path, timestamp_error);
    if (stop.stop_requested()) {
        return cancelled();
    }
    if (timestamp_error) {
        return ContentIdentityResult::failure(
            error(ContentIntegrityErrorCode::ReadFailed, timestamp_error));
    }
    if (final_timestamp != initial_timestamp) {
        return ContentIdentityResult::failure(
            error(ContentIntegrityErrorCode::FileChanged));
    }
    return ContentIdentityResult::success(
        ContentIdentity{hash.finish(), bytes_read});
}

[[nodiscard]] bool equal_digest(const Sha256Digest& left,
                                const Sha256Digest& right) noexcept {
    // Do not teach callers to compare integrity values with an early exit.
    // This is not an authentication boundary, but a constant-work comparison
    // costs nothing at this fixed 32-byte size.
    core::u8 difference = 0U;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        difference = static_cast<core::u8>(difference |
                                           (left[index] ^ right[index]));
    }
    return difference == 0U;
}

} // namespace

std::optional<Sha256Digest>
parse_sha256(std::string_view hexadecimal) noexcept {
    if (hexadecimal.size() != sha256_hex_characters) {
        return std::nullopt;
    }
    const auto nibble = [](char character) noexcept -> std::optional<core::u8> {
        if (character >= '0' && character <= '9') {
            return static_cast<core::u8>(character - '0');
        }
        if (character >= 'a' && character <= 'f') {
            return static_cast<core::u8>(character - 'a' + 10);
        }
        return std::nullopt;
    };

    Sha256Digest result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const auto high = nibble(hexadecimal[index * 2U]);
        const auto low = nibble(hexadecimal[index * 2U + 1U]);
        if (!high.has_value() || !low.has_value()) {
            return std::nullopt;
        }
        result[index] = static_cast<core::u8>((*high << 4U) | *low);
    }
    return result;
}

std::string format_sha256(const Sha256Digest& digest) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result(sha256_hex_characters, '0');
    for (std::size_t index = 0U; index < digest.size(); ++index) {
        result[index * 2U] = digits[digest[index] >> 4U];
        result[index * 2U + 1U] = digits[digest[index] & 0x0fU];
    }
    return result;
}

ContentIdentityResult hash_file(const std::filesystem::path& path,
                                std::stop_token stop) {
    return hash_file_impl(path, std::nullopt, stop);
}

ContentVerificationResult verify_file(const std::filesystem::path& path,
                                      const ContentIdentity& expected,
                                      std::stop_token stop) {
    const auto cancelled = [&]() {
        return ContentVerificationResult::failure(
            error(ContentIntegrityErrorCode::Cancelled));
    };
    if (stop.stop_requested()) {
        return cancelled();
    }
    if (path.empty()) {
        return ContentVerificationResult::failure(
            error(ContentIntegrityErrorCode::InvalidPath));
    }
    std::error_code size_error;
    const std::uintmax_t size = std::filesystem::file_size(path, size_error);
    if (stop.stop_requested()) {
        return cancelled();
    }
    if (size_error) {
        std::error_code status_error;
        const bool regular =
            std::filesystem::is_regular_file(path, status_error);
        if (!status_error && !regular) {
            return ContentVerificationResult::failure(
                error(ContentIntegrityErrorCode::NotRegularFile));
        }
        return ContentVerificationResult::failure(
            error(ContentIntegrityErrorCode::OpenFailed, size_error));
    }
    if (size > static_cast<std::uintmax_t>(
                   std::numeric_limits<core::u64>::max())) {
        return ContentVerificationResult::failure(
            error(ContentIntegrityErrorCode::FileTooLarge));
    }
    if (static_cast<core::u64>(size) != expected.byte_size) {
        return ContentVerificationResult::success(
            ContentVerification::SizeMismatch);
    }
    auto actual = hash_file_impl(path, expected.byte_size, stop);
    if (!actual) {
        return ContentVerificationResult::failure(actual.error());
    }
    return ContentVerificationResult::success(
        equal_digest(actual.value().sha256, expected.sha256)
            ? ContentVerification::Verified
            : ContentVerification::DigestMismatch);
}

} // namespace mediaserver::assets
