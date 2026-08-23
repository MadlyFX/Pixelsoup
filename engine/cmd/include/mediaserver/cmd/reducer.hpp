#pragma once

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/core/result.hpp>

namespace mediaserver::cmd {

enum class PayloadError {
    MissingNode,
};

[[nodiscard]] core::Result<doc::Root, PayloadError> apply_payload(
    const doc::Root& root,
    const Payload& payload);

// True when the document reflects the state produced by this payload. Used by
// Director-side undo conflict detection before inverse commands are authored.
[[nodiscard]] bool payload_postcondition_matches(
    const doc::Root& root,
    const Payload& payload) noexcept;

} // namespace mediaserver::cmd
