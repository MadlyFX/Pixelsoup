#pragma once

#include <vector>
#include <variant>

#include <mediaserver/core/strong_id.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/document.hpp>

namespace mediaserver::cmd {

using CommandId = core::StrongId<struct CommandIdTag, core::u64>;
using TransactionId = core::StrongId<struct TransactionIdTag, core::u64>;
using UndoContextId = core::StrongId<struct UndoContextIdTag, core::u64>;
using FrameNumber = core::StrongId<struct FrameNumberTag, core::u64>;
using CommandFingerprint = core::StrongId<struct CommandFingerprintTag, core::u64>;

// Stable discriminants in the version 1 fingerprint byte stream. These values
// are part of the persisted protocol contract and intentionally do not follow
// std::variant's alternative indexes.
namespace fingerprint_v1 {

enum class PropertyTag : core::u8 {
    Boolean = 1U,
    Int64 = 2U,
    Float64 = 3U,
    String = 4U,
};

enum class PayloadTag : core::u8 {
    PutNode = 1U,
    EraseNode = 2U,
};

} // namespace fingerprint_v1

struct PutNode final {
    doc::Node node;

    bool operator==(const PutNode&) const = default;
};

struct EraseNode final {
    doc::NodeId id;

    bool operator==(const EraseNode&) const = default;
};

using Payload = std::variant<PutNode, EraseNode>;

struct Command final {
    CommandId id;
    doc::Version base;
    doc::Version result;
    FrameNumber apply_at;
    TransactionId transaction;
    UndoContextId undo_context;
    Payload forward;
    Payload inverse;

    bool operator==(const Command&) const = default;
};

struct SealedCommand final {
    CommandId id;
    CommandFingerprint fingerprint;

    bool operator==(const SealedCommand&) const = default;
};

struct FrameSeal final {
    FrameNumber frame;
    doc::Version base;
    doc::Version result;
    std::vector<SealedCommand> commands;

    bool operator==(const FrameSeal&) const = default;
};

struct StreamCursor final {
    FrameNumber through_frame;
    doc::Version revision;
    CommandId next_command;

    bool operator==(const StreamCursor&) const = default;
};

struct StreamCheckpoint final {
    StreamCursor cursor;
    doc::Root document;
};

// Deterministic version 1 compatibility fingerprint over every command field
// and both payloads. This is a corruption/protocol guard, not a cryptographic
// signature.
[[nodiscard]] CommandFingerprint fingerprint(const Command& command) noexcept;

} // namespace mediaserver::cmd
