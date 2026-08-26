#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>

// Stable identity and integrity checks for project content.
//
// A document NodeId identifies an imported library entry inside one project.
// ContentIdentity identifies the bytes themselves, across projects and render
// nodes. SHA-256 is deliberately named in the type and persisted vocabulary so
// a future algorithm transition cannot silently reinterpret existing hashes.
namespace mediaserver::assets {

inline constexpr std::size_t sha256_bytes = 32U;
inline constexpr std::size_t sha256_hex_characters = sha256_bytes * 2U;

using Sha256Digest = std::array<core::u8, sha256_bytes>;

struct ContentIdentity final {
    Sha256Digest sha256{};
    core::u64 byte_size{};

    bool operator==(const ContentIdentity&) const = default;
};

// Canonical document/storage spelling: exactly 64 lowercase hexadecimal
// characters. Uppercase and decorated forms are rejected instead of being
// normalised differently by different callers.
[[nodiscard]] std::optional<Sha256Digest>
parse_sha256(std::string_view hexadecimal) noexcept;
[[nodiscard]] std::string format_sha256(const Sha256Digest& digest);

enum class ContentIntegrityErrorCode : core::u8 {
    InvalidPath,
    NotRegularFile,
    OpenFailed,
    ReadFailed,
    FileTooLarge,
    FileChanged,
    Cancelled,
};

struct ContentIntegrityError final {
    ContentIntegrityErrorCode code{ContentIntegrityErrorCode::InvalidPath};
    std::error_code system_error;

    bool operator==(const ContentIntegrityError&) const = default;
};

enum class ContentVerification : core::u8 {
    Verified,
    SizeMismatch,
    DigestMismatch,
};

using ContentIdentityResult =
    core::Result<ContentIdentity, ContentIntegrityError>;
using ContentVerificationResult =
    core::Result<ContentVerification, ContentIntegrityError>;

// Streams a regular file through a fixed-size buffer. Memory use is bounded
// independently of asset size. A file whose size changes during the read is
// refused rather than publishing an identity for an unstable snapshot.
[[nodiscard]] ContentIdentityResult
hash_file(const std::filesystem::path& path,
          std::stop_token stop = {});

// Checks size first, then hashes only a same-sized candidate. A mismatch is a
// successful integrity observation rather than an I/O error.
[[nodiscard]] ContentVerificationResult
verify_file(const std::filesystem::path& path,
            const ContentIdentity& expected,
            std::stop_token stop = {});

} // namespace mediaserver::assets
