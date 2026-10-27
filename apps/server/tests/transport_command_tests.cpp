#include "transport_commands.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

// The operator transport vocabulary on a real TransportAuthority: J, K and L
// play backwards, stop and play forwards from where the show stands, and
// Space always plays forwards.
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

#define TRANSPORT_CHECK(...)                                                   \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] cmd::FrameNumber at(core::u64 value) noexcept {
    return cmd::FrameNumber{value};
}

// A 25 fps show over frames 0..999, stopped on frame 0 at control frame 0.
[[nodiscard]] show::TransportAuthority make_transport() {
    const show::ShowClockConfig config{show::FrameRate{25U, 1U},
                                       show::ShowFrame{0U},
                                       show::ShowFrame{999U}, false};
    auto created = show::TransportAuthority::create(config, at(0U));
    TRANSPORT_CHECK(created);
    return std::move(created).value();
}

void j_k_and_l_play_backwards_stop_and_play_forwards_in_place() {
    show::TransportAuthority transport = make_transport();
    TRANSPORT_CHECK(transport.seek(show::ShowFrame{500U}, at(1U)));

    // L runs forwards: 40 control frames later the show is 40 frames on.
    app::apply_command(transport, show::PresenterCommand::PlayForward, at(10U));
    TRANSPORT_CHECK(transport.state() == show::TransportState::Playing);
    TRANSPORT_CHECK(transport.frame_at(at(50U)) == show::ShowFrame{540U});

    // J turns it round where it stands, without stopping or moving it.
    app::apply_command(transport, show::PresenterCommand::PlayReverse, at(50U));
    TRANSPORT_CHECK(transport.state() == show::TransportState::Playing);
    TRANSPORT_CHECK(transport.anchor().direction ==
                    show::TransportDirection::Reverse);
    TRANSPORT_CHECK(transport.frame_at(at(50U)) == show::ShowFrame{540U});
    TRANSPORT_CHECK(transport.frame_at(at(80U)) == show::ShowFrame{510U});

    // A second J while already running backwards changes nothing.
    const show::TransportAnchor before = transport.anchor();
    app::apply_command(transport, show::PresenterCommand::PlayReverse, at(60U));
    TRANSPORT_CHECK(transport.anchor() == before);

    // K stops it on the frame it reached.
    app::apply_command(transport, show::PresenterCommand::Pause, at(80U));
    TRANSPORT_CHECK(transport.state() == show::TransportState::Paused);
    TRANSPORT_CHECK(transport.frame_at(at(200U)) == show::ShowFrame{510U});
    // K on a stopped show is nothing.
    const show::TransportAnchor paused = transport.anchor();
    app::apply_command(transport, show::PresenterCommand::Pause, at(90U));
    TRANSPORT_CHECK(transport.anchor() == paused);

    // Space plays forwards even after J left the show reversed.
    app::apply_command(transport, show::PresenterCommand::PlayReverse, at(100U));
    app::apply_command(transport, show::PresenterCommand::TogglePlay, at(110U));
    TRANSPORT_CHECK(transport.state() == show::TransportState::Paused);
    TRANSPORT_CHECK(transport.frame_at(at(110U)) == show::ShowFrame{500U});
    app::apply_command(transport, show::PresenterCommand::TogglePlay, at(120U));
    TRANSPORT_CHECK(transport.state() == show::TransportState::Playing);
    TRANSPORT_CHECK(transport.anchor().direction ==
                    show::TransportDirection::Forward);
    TRANSPORT_CHECK(transport.frame_at(at(130U)) == show::ShowFrame{510U});
}

void running_backwards_holds_the_first_frame() {
    show::TransportAuthority transport = make_transport();
    TRANSPORT_CHECK(transport.seek(show::ShowFrame{20U}, at(1U)));
    app::apply_command(transport, show::PresenterCommand::PlayReverse, at(2U));
    TRANSPORT_CHECK(transport.frame_at(at(12U)) == show::ShowFrame{10U});
    TRANSPORT_CHECK(transport.frame_at(at(500U)) == show::ShowFrame{0U});
}

struct Case final {
    const char *name;
    void (*function)();
};

constexpr std::array<Case, 2U> cases{{
    {"J, K and L play backwards, stop and play forwards in place",
     &j_k_and_l_play_backwards_stop_and_play_forwards_in_place},
    {"running backwards holds the first frame",
     &running_backwards_holds_the_first_frame},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case &entry : cases) {
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
