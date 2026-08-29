#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

#include <array>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>

namespace mediaserver::assets {

struct Sha256Digest final {
    std::array<core::u8, 32U> bytes{};

    bool operator==(const Sha256Digest&) const = default;
};

// Incremental SHA-256 for cache envelopes and other bounded streaming callers.
// finish() does not consume the hasher, so validation can inspect a digest
// without making the state dynamically allocated or platform-dependent.
class Sha256Hasher final {
public:
    void update(std::span<const core::u8> bytes) noexcept;
    [[nodiscard]] Sha256Digest finish() const noexcept;

private:
    void transform(const std::array<core::u8, 64U>& block) noexcept;

    std::array<core::u32, 8U> state_{{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    }};
    std::array<core::u8, 64U> buffer_{};
    std::size_t buffer_size_{};
    core::u64 byte_length_{};
};

struct ContentFingerprint final {
    core::u64 byte_length{};
    Sha256Digest sha256;

    bool operator==(const ContentFingerprint&) const = default;
};

// Lower-case hexadecimal is the canonical document/cache representation.
[[nodiscard]] std::string sha256_hex(const Sha256Digest& digest);
[[nodiscard]] std::optional<Sha256Digest>
sha256_from_hex(std::string_view encoded) noexcept;
// SHA-256 over an in-memory canonical byte stream. Derivative identities use
// this alongside the streaming file helper below so profile/key hashing and
// source hashing share one implementation.
[[nodiscard]] Sha256Digest
sha256_digest(std::span<const core::u8> bytes) noexcept;

enum class ContentFingerprintErrorCode : core::u8 {
    InvalidSource,
    SourceMissing,
    OpenFailed,
    ReadFailed,
    SourceChanged,
    SizeLimitExceeded,
    Cancelled,
    AllocationFailed,
};

struct ContentFingerprintError final {
    ContentFingerprintErrorCode code{
        ContentFingerprintErrorCode::InvalidSource};
    std::error_code system_error;

    bool operator==(const ContentFingerprintError&) const = default;
};

using ContentFingerprintResult =
    core::Result<ContentFingerprint, ContentFingerprintError>;

// Streams one absolute authored source through SHA-256 without loading it into
// memory. The byte count and file metadata are checked again after the read so
// a file replaced or resized during ingest is never recorded as a stable
// revision. Cancellation is observed between bounded reads.
[[nodiscard]] ContentFingerprintResult content_fingerprint(
    std::string_view source, std::stop_token stop = {}) noexcept;

} // namespace mediaserver::assets
