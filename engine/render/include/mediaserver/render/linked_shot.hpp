#pragma once
#include <mediaserver/render/director_link_service.hpp>

namespace mediaserver::render {
// Serialized only on the authoring worker, bounded by the link manifest limit.
[[nodiscard]] std::string linked_shot_state(const DirectorLinkContext& context, const std::string& source);
[[nodiscard]] std::string linked_shot_revision(const DirectorLinkContext& context,
    const std::string& source, doc::NodeId node, const std::string& property);
[[nodiscard]] scene::LinkedPlanResult plan_linked_shot(const DirectorLinkCandidate& candidate,
    const DirectorLinkContext& context);
}
