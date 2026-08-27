#pragma once

#include <mediaserver/doc/document.hpp>
#include <mediaserver/sync/sync_policy.hpp>

#include <map>

namespace mediaserver::sync {

struct SyncPolicyCounters final {
    // Cumulative parse failures since construction/reset. A rejected policy
    // node leaves the project UNSYNCED rather than guessing a mode, so a
    // corrupt document can never widen a physical timing claim.
    core::u64 rejected{};
    core::u32 policies{};

    bool operator==(const SyncPolicyCounters&) const = default;
};

// The document projection of the project-wide synchronization policy. Exactly
// one policy node is meaningful; if a document somehow carries several, the
// lowest node id wins deterministically on every machine.
class SyncPolicyModel final {
public:
    SyncPolicyModel() = default;

    void reset(const doc::Root& root);
    void apply(const doc::Delta& delta);

    [[nodiscard]] SyncPolicy evaluate() const;
    [[nodiscard]] std::size_t size() const noexcept {
        return policies_.size();
    }
    [[nodiscard]] const SyncPolicyCounters& counters() const noexcept {
        return counters_;
    }

private:
    std::map<doc::NodeId, SyncPolicy> policies_;
    SyncPolicyCounters counters_{};

    void put(const doc::Node& node);
    void forget(doc::NodeId id) noexcept;
    void recount() noexcept;
};

// Writes `policy` into a document node's properties. Used by the authoring
// boundary so the schema is written in exactly one place.
void write_sync_policy_properties(doc::Node& node, const SyncPolicy& policy);

} // namespace mediaserver::sync
