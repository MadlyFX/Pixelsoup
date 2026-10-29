#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>

namespace mediaserver::app {

// The node a (target, component) channel lives in. Derived rather than
// allocated, so every key edit on one channel finds the same node and a copy
// of a channel for another target lands where that target's key edits look.
[[nodiscard]] constexpr doc::NodeId channel_node_id(
    doc::NodeId target, core::u64 component_index) noexcept {
    constexpr core::u64 legacy_channel_id_base = 0x7C00'0000'0000'0000ULL;
    constexpr core::u64 extended_channel_id_base = 0x7D00'0000'0000'0000ULL;
    if (component_index < 16U) {
        return doc::NodeId{legacy_channel_id_base | (target.value() << 4U) |
                           component_index};
    }
    return doc::NodeId{extended_channel_id_base | (target.value() << 5U) |
                       component_index};
}

} // namespace mediaserver::app
