#pragma once

#include <mediaserver/net/session_protocol.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace mediaserver::net {

inline constexpr core::u32 automatic_node_index = 0xffffffffU;
enum class NodeAssignmentStatus : core::u8 { None, Assigned, Full, Conflict, Unavailable };

struct NodeIndexAssignment final {
    NodeAssignmentStatus status{NodeAssignmentStatus::Unavailable};
    core::u32 index{automatic_node_index};
};

// One Director/show's durable machine-ID -> output-index table. All access is
// serialized; disk reads/writes run only at startup or on network workers.
// An exclusive file lock prevents two Directors from issuing different tables
// from the same record. Entries are retained when a node disconnects.
class NodeIndexAssignments final {
public:
    explicit NodeIndexAssignments(std::filesystem::path file);
    ~NodeIndexAssignments();
    NodeIndexAssignments(const NodeIndexAssignments&) = delete;
    NodeIndexAssignments& operator=(const NodeIndexAssignments&) = delete;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::string error() const;
    // An explicit index reserves a manually configured node, using the same
    // collision checks as automatic enrollment. No successful assignment is
    // returned until its record has been saved atomically.
    [[nodiscard]] NodeIndexAssignment assign(
        Id128 node_id, core::u32 requested = automatic_node_index) noexcept;
private:
    class State;
    std::unique_ptr<State> state_;
};

// Generated once when the render-machine setup is saved, never per frame or
// per connection. Failure throws; an identity must never use a clock fallback.
[[nodiscard]] Id128 new_render_machine_id();

} // namespace mediaserver::net
