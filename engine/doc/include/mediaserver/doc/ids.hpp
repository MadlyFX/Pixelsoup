#pragma once

#include <mediaserver/core/strong_id.hpp>
#include <mediaserver/core/types.hpp>

namespace mediaserver::doc {

using NodeId = core::StrongId<struct NodeIdTag, core::u64>;
using Version = core::StrongId<struct VersionTag, core::u64>;

} // namespace mediaserver::doc

