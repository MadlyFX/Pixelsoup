#pragma once

#include <cstddef>
#include <span>
#include <variant>
#include <vector>

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/core/result.hpp>

namespace mediaserver::wire {

inline constexpr core::u16 current_format_version = 1U;

using Message = std::variant<
    cmd::Command,
    cmd::FrameSeal,
    cmd::StreamCursor,
    cmd::StreamCheckpoint>;

enum class EncodeError {
    InvalidCommand,
    InvalidSeal,
    InvalidCursor,
    InvalidCheckpoint,
    InvalidNode,
    InvalidProperty,
    SizeLimitExceeded,
};

enum class DecodeError {
    InvalidBuffer,
    UnsupportedVersion,
    MissingBody,
    MissingRequiredField,
    InvalidUnion,
    InvalidCommand,
    InvalidSeal,
    InvalidCursor,
    InvalidCheckpoint,
    InvalidNode,
    InvalidProperty,
    DuplicateNode,
    DuplicateProperty,
    InvalidUtf8,
    SizeLimitExceeded,
};

struct DecodeLimits final {
    std::size_t max_message_bytes{64U * 1024U * 1024U};
    std::size_t max_nodes{250'000U};
    std::size_t max_properties_per_node{4096U};
    std::size_t max_total_properties{1'000'000U};
    std::size_t max_sealed_commands{262'144U};
    std::size_t max_string_bytes{1U * 1024U * 1024U};
    std::size_t verifier_max_depth{16U};
    std::size_t verifier_max_tables{2'000'000U};
};

[[nodiscard]] core::Result<std::vector<std::byte>, EncodeError> encode(
    const Message& message,
    const DecodeLimits& limits = {});

[[nodiscard]] core::Result<Message, DecodeError> decode(
    std::span<const std::byte> bytes,
    const DecodeLimits& limits = {});

} // namespace mediaserver::wire
