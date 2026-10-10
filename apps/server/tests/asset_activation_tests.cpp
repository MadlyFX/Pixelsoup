#include "asset_activation.hpp"

#include <array>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

namespace app = mediaserver::app;
namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace show = mediaserver::show;

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define ACTIVATION_CHECK(...)                                                  \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] show::TransportAnchor anchor(core::u64 control, core::u64 frame,
                                           show::TransportState state) noexcept {
    show::TransportAnchor made{};
    made.anchor_control_frame = cmd::FrameNumber{control};
    made.anchor_show_frame = show::ShowFrame{frame};
    made.state = state;
    made.rate = show::FrameRate{50U, 1U};
    made.first_frame = show::ShowFrame{0U};
    made.last_frame = show::ShowFrame{600U};
    return made;
}

// The case that wedged a wall: a set loads, the Director permits it for
// control frame 594 while the show is playing towards frame 86 there, and the
// operator pauses on 85 one frame before the activation.
constexpr core::AssetSetReadiness loaded{7U, true, 0U};
constexpr core::AssetCommit permit{7U, 594U, 86U};

void a_permit_the_transport_still_reaches_is_held() {
    const auto playing = anchor(564U, 56U, show::TransportState::Playing);
    ACTIVATION_CHECK(app::activation_show_frame(playing, 594U) == 86U);
    ACTIVATION_CHECK(app::held_asset_activation(loaded, permit, 594U, playing) ==
                     594U);
    // Approaching it, the activation is where a catch-up drain must stop.
    ACTIVATION_CHECK(app::held_asset_activation(loaded, permit, 590U, playing) ==
                     594U);
}

void a_pause_inside_the_lead_lets_the_node_move_on() {
    const auto paused = anchor(593U, 85U, show::TransportState::Paused);
    // No presenter can reveal show frame 86 on control frame 594 any more.
    // Holding the activation would park the node there for good.
    ACTIVATION_CHECK(app::held_asset_activation(loaded, permit, 594U, paused) ==
                     0U);
}

void a_revealed_set_is_not_held() {
    const auto playing = anchor(564U, 56U, show::TransportState::Playing);
    constexpr core::AssetSetReadiness shown{7U, true, 594U};
    ACTIVATION_CHECK(app::held_asset_activation(shown, permit, 594U, playing) ==
                     0U);
    constexpr core::AssetSetReadiness other{8U, true, 0U};
    ACTIVATION_CHECK(app::held_asset_activation(other, permit, 594U, playing) ==
                     0U);
}

void a_reanchor_inside_the_lead_restates_the_permit() {
    const auto paused = anchor(593U, 85U, show::TransportState::Paused);
    ACTIVATION_CHECK(app::restated_permit_show_frame(permit, paused, 593U) ==
                     std::optional<core::u64>{85U});
    // Restated, the activation can be honoured and is held again.
    const core::AssetCommit restated{7U, 594U, 85U};
    ACTIVATION_CHECK(app::held_asset_activation(loaded, restated, 594U,
                                                paused) == 594U);
    // A re-anchor on the activation frame itself still counts.
    const auto seek = anchor(594U, 300U, show::TransportState::Playing);
    ACTIVATION_CHECK(app::restated_permit_show_frame(permit, seek, 594U) ==
                     std::optional<core::u64>{300U});
}

void a_permit_already_right_or_behind_is_left_alone() {
    const auto playing = anchor(564U, 56U, show::TransportState::Playing);
    ACTIVATION_CHECK(!app::restated_permit_show_frame(permit, playing, 570U));
    const auto later = anchor(600U, 10U, show::TransportState::Paused);
    ACTIVATION_CHECK(!app::restated_permit_show_frame(permit, later, 600U));
    ACTIVATION_CHECK(
        !app::restated_permit_show_frame(core::AssetCommit{}, later, 10U));
}

void a_document_without_a_transport_presents_frame_zero() {
    const std::optional<show::TransportAnchor> none;
    constexpr core::AssetCommit still{7U, 40U, 0U};
    ACTIVATION_CHECK(app::held_asset_activation(loaded, still, 40U, none) ==
                     40U);
}

struct Case final {
    const char *name;
    void (*function)();
};

constexpr std::array<Case, 6U> cases{{
    {"a permit the transport still reaches is held",
     &a_permit_the_transport_still_reaches_is_held},
    {"a pause inside the lead lets the node move on",
     &a_pause_inside_the_lead_lets_the_node_move_on},
    {"a revealed set is not held", &a_revealed_set_is_not_held},
    {"a re-anchor inside the lead restates the permit",
     &a_reanchor_inside_the_lead_restates_the_permit},
    {"a permit already right or behind is left alone",
     &a_permit_already_right_or_behind_is_left_alone},
    {"a document without a transport presents frame zero",
     &a_document_without_a_transport_presents_frame_zero},
}};

} // namespace

int main() {
    std::size_t failures{};
    for (const auto &entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
