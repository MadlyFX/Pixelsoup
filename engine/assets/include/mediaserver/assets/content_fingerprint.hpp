#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/sha256.hpp>
#include <mediaserver/core/types.hpp>

#include <array>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>

namespace mediaserver::assets {

using core::Sha256Digest;
using core::Sha256Hasher;
using core::sha256_digest;

struct ContentFingerprint final {
    core::u64 byte_length{};
    Sha256Digest sha256;

    bool operator==(const ContentFingerprint&) const = default;
};

// Lower-case hexadecimal is the canonical document/cache representation.
[[nodiscard]] std::string sha256_hex(const Sha256Digest& digest);
[[nodiscard]] std::optional<Sha256Digest>
sha256_from_hex(std::string_view encoded) noexcept;

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

// Fast source revision identity derived from file size and last-write time.
// It reads no media payload bytes. This is suitable for continuously running
// shows where import and readiness must remain immediate; it deliberately does
// not provide cryptographic proof that two equal-metadata files have identical
// contents.
[[nodiscard]] ContentFingerprintResult source_fingerprint(
    std::string_view source, std::stop_token stop = {}) noexcept;

// Streams one absolute authored source through SHA-256 without loading it into
// memory. The byte count and file metadata are checked again after the read so
// a file replaced or resized during ingest is never recorded as a stable
// revision. Cancellation is observed between bounded reads.
[[nodiscard]] ContentFingerprintResult content_fingerprint(
    std::string_view source, std::stop_token stop = {}) noexcept;

} // namespace mediaserver::assets
