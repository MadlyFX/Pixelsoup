#pragma once
#include <mediaserver/render/director_link_service.hpp>

namespace mediaserver::render {
// Media Server's overrides on one source's occurrences in the target scene, for
// the publisher to show in its own objects (writeback). `MSSHOT 2`, then per
// occurrence: node, attachment, object uuid, held, visibility override
// (-1 none, 0 hidden, 1 shown), effective visibility, removed, deleted in
// Media Server (itself or through an ancestor), the offset,
// held-world, attachment-alignment and world matrices (16 column-major values
// each), then three quoted fields: light overrides and material overrides as
// `key=value,...`, and the source parent's uuid. Serialized only on the
// authoring worker, bounded at 512 occurrences.
[[nodiscard]] std::string linked_shot_state(const DirectorLinkContext& context, const std::string& source);
}
