#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/wire/protocol_codec.hpp>

namespace mediaserver::persist {

// A project file is a small integrity/version envelope around the existing
// wire StreamCheckpoint payload. Node and property encoding therefore stays
// identical to replication/checkpoint encoding, including unknown types and
// all PropertyValue alternatives.
inline constexpr std::uint16_t current_project_format_version = 1U;

enum class ProjectFileErrorCode {
    InvalidPath,
    EncodeFailed,
    OpenFailed,
    ReadFailed,
    WriteFailed,
    SyncFailed,
    ReplaceFailed,
    InvalidFormat,
    UnsupportedVersion,
    CorruptFile,
    UnexpectedPayload,
    InvalidProjectCursor,
    SizeLimitExceeded,
    AllocationFailed,
};

struct ProjectFileError final {
    ProjectFileErrorCode code{ProjectFileErrorCode::InvalidFormat};
    std::error_code system_error;
    std::optional<wire::EncodeError> encode_error;
    std::optional<wire::DecodeError> decode_error;
    // A failure after atomic replacement (currently only a POSIX parent-
    // directory sync failure) leaves the new project installed, although its
    // survival across a power loss is uncertain.
    bool committed{};

    bool operator==(const ProjectFileError&) const = default;
};

struct ProjectSaveReceipt final {
    doc::Version version{};
    std::uint64_t bytes_written{};
    // True means the temporary file was flushed before replacement and the
    // namespace replacement was issued with the platform's durable semantics.
    bool durable{};

    bool operator==(const ProjectSaveReceipt&) const = default;
};

using ProjectSaveResult =
    core::Result<ProjectSaveReceipt, ProjectFileError>;
using ProjectLoadResult = core::Result<doc::Root, ProjectFileError>;

// Encodes the complete immutable Root, writes a unique sibling temporary file,
// flushes it, then atomically replaces `path`. The prior project remains intact
// if encoding or temporary-file writing fails. A ReplaceFailed/SyncFailed result
// may require the caller to inspect/reload `path`, because the atomic rename may
// already have completed before a final durability operation reported failure.
[[nodiscard]] ProjectSaveResult save_project_file(
    const std::filesystem::path& path,
    const doc::Root& project,
    const wire::DecodeLimits& limits = {});

// Loads only a standalone-project StreamCheckpoint with the sentinel cursor
// {frame 0, document revision, next command 1}. Container magic/version, length,
// CRC32C, FlatBuffers verification, wire version, and decode limits are all
// checked before a Root is returned.
[[nodiscard]] ProjectLoadResult load_project_file(
    const std::filesystem::path& path,
    const wire::DecodeLimits& limits = {});

} // namespace mediaserver::persist
