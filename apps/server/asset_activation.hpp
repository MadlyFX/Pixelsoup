#pragma once

#include <optional>

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/core/asset_publication.hpp>
#include <mediaserver/show/transport_state.hpp>

namespace mediaserver::app {

// An asset permit names the control frame a newly loaded asset set first
// appears on and the show frame presented there, and every presenter reveals
// the set only on that exact pair, so the whole cohort changes picture on the
// same frame. The show frame is a prediction: the Director makes it from the
// transport when it writes the permit, a lead of frames ahead. A pause, a
// seek, or a cue inside that lead changes the answer.

// The show frame an activation presents under `anchor`. A document with no
// transport presents frame zero, which is what the Director assumes too.
[[nodiscard]] inline core::u64 activation_show_frame(
    const std::optional<show::TransportAnchor>& anchor,
    core::u64 activation) noexcept {
    return anchor.has_value()
        ? show::derive_show_frame(*anchor, cmd::FrameNumber{activation}).value()
        : 0U;
}

// The activation frame a node keeps on screen until its renderer reveals the
// set, or zero for none. A permit whose show frame the transport no longer
// reaches can never be revealed. Holding it would park the node on that frame
// for good - a black wall that never again reads the retry the Director
// authors to replace it - so the node lets it go and keeps its previous
// picture until the retry lands.
[[nodiscard]] inline core::u64 held_asset_activation(
    core::AssetSetReadiness assets,
    core::AssetCommit permit,
    core::u64 presented_control_frame,
    const std::optional<show::TransportAnchor>& anchor) noexcept {
    const core::u64 activation = core::asset_presentation_frame(
        assets, permit, presented_control_frame);
    if (activation == 0U
        || activation_show_frame(anchor, activation) != permit.show_frame) {
        return 0U;
    }
    return activation;
}

// The show frame to restate on an outstanding permit once the transport has
// re-anchored at control frame `at`, or nothing when the permit is already
// behind the transport or still right. Restated in the same control frame as
// the anchor, no node ever compares against the stale prediction.
[[nodiscard]] inline std::optional<core::u64> restated_permit_show_frame(
    core::AssetCommit permit,
    const std::optional<show::TransportAnchor>& anchor,
    core::u64 at) noexcept {
    if (permit.digest == 0U || permit.control_frame == 0U
        || permit.control_frame < at) {
        return std::nullopt;
    }
    const core::u64 show_frame =
        activation_show_frame(anchor, permit.control_frame);
    if (show_frame == permit.show_frame) {
        return std::nullopt;
    }
    return show_frame;
}

} // namespace mediaserver::app
