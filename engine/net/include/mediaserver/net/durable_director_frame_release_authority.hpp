#pragma once

#include <mediaserver/net/director_frame_barrier.hpp>
#include <mediaserver/persist/durable_director.hpp>

namespace mediaserver::net {

// Thin dependency-direction adapter: net owns the barrier interface and
// translates its immutable publication handle back to the persist capability
// that minted it. The borrowed DurableDirector must outlive this adapter and
// every barrier that refers to it.
class DurableDirectorFrameReleaseAuthority final
    : public DirectorFrameReleaseAuthority {
public:
    explicit DurableDirectorFrameReleaseAuthority(
        persist::DurableDirector& authority) noexcept
        : authority_(&authority) {}

    [[nodiscard]] core::u64 next_release_ordinal() const noexcept override;
    [[nodiscard]] std::optional<core::u64> release_ordinal(
        const cmd::StreamCursor& before,
        const cmd::StreamCursor& after) const noexcept override;

    [[nodiscard]] bool persist_release(
        const PublishedDirectorFrame& frame,
        core::u64 barrier_ordinal) noexcept override;

private:
    persist::DurableDirector* authority_;
};

} // namespace mediaserver::net
