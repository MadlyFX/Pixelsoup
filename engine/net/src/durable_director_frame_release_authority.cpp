#include <mediaserver/net/durable_director_frame_release_authority.hpp>

#include <utility>

namespace mediaserver::net {
namespace {

[[nodiscard]] persist::LogCursor as_log_cursor(
    const cmd::StreamCursor& cursor) noexcept {
    return persist::LogCursor{
        cursor.through_frame.value(),
        cursor.revision.value(),
        cursor.next_command.value(),
    };
}

} // namespace

core::u64 DurableDirectorFrameReleaseAuthority::next_release_ordinal()
    const noexcept {
    return authority_->next_release_ordinal();
}

std::optional<core::u64>
DurableDirectorFrameReleaseAuthority::release_ordinal(
    const cmd::StreamCursor& before,
    const cmd::StreamCursor& after) const noexcept {
    return authority_->release_ordinal(before, after);
}

bool DurableDirectorFrameReleaseAuthority::persist_release(
    const PublishedDirectorFrame& frame,
    core::u64 barrier_ordinal) noexcept {
    if (!frame || barrier_ordinal == 0U) {
        return false;
    }
    try {
        const auto& batch = frame.batch();
        // Which decision record the release lands in is the coordinator's
        // cadence policy. The barrier's contract is unchanged either way: true
        // means this exact publication now holds that exact ordinal.
        if (authority_->frame_durability() == persist::FrameDurability::EditsOnly) {
            return authority_->record_release(batch, barrier_ordinal);
        }

        auto persisted = authority_->persist_release(batch, barrier_ordinal);
        if (!persisted) {
            return false;
        }
        auto receipt = std::move(persisted).value();
        const auto& append = batch.receipt();
        const auto expected = persist::WalFrameIdentity{
            as_log_cursor(batch.before_cursor()),
            append.cursor,
            append.offset,
            append.bytes_written,
            append.entry_crc32c,
        };
        return receipt.durable() && receipt.ordinal() == barrier_ordinal
            && receipt.frame() == expected;
    } catch (...) {
        // The barrier interprets false as fail-closed and emits no permit.
        return false;
    }
}

} // namespace mediaserver::net
