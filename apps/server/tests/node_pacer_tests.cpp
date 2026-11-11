#include "playback_runtime.hpp"

#include <mediaserver/cmd/node_command_bus.hpp>
#include <mediaserver/show/frame_boundary_driver.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {

namespace app = mediaserver::app;
namespace cmd = mediaserver::cmd;
namespace show = mediaserver::show;

using namespace std::chrono_literals;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            throw std::runtime_error{"check failed: " #condition};         \
        }                                                                   \
    } while (false)

[[nodiscard]] show::ShowClock::TimePoint at(std::chrono::nanoseconds value) {
    return show::ShowClock::TimePoint{
        std::chrono::duration_cast<show::ShowClock::Clock::duration>(value)};
}

// A node launched with no show arguments: the options still carry the default
// five-minute timeline, looping off. The node's pacer has to run through the
// end of that timeline and keep going, because the playhead it presents comes
// from the Director and may be anywhere when the node's 7 200th boundary
// passes.
void a_default_node_pacer_runs_past_the_default_timeline() {
    const app::PlaybackOptions options{};
    CHECK(options.last_frame == show::ShowFrame{7'199U});
    CHECK(!options.loop);

    cmd::NodeCommandBus node{};
    show::LocalNodeCommitSink sink{node};
    auto created = show::FrameBoundaryDriver::create(
        app::node_pacer_config(options), sink, at(0ns));
    CHECK(created.has_value());
    auto driver = std::move(created).value();
    CHECK(driver.play(at(0ns)).has_value());

    // 300 s at 24 fps is exactly the 7 200 boundaries of the timeline.
    const auto whole_timeline = driver.tick(at(300s));
    CHECK(whole_timeline.has_value());
    CHECK(whole_timeline.value().boundaries == 7'200U);
    CHECK(!whole_timeline.value().tick.reached_end);
    CHECK(driver.clock().state() == show::TransportState::Playing);

    // The second after it has its 24 boundaries too.
    const auto after = driver.tick(at(301s));
    CHECK(after.has_value());
    CHECK(after.value().boundaries == 24U);
    CHECK(driver.counters().boundaries == 7'224U);
    // A running pacer names a boundary still to come, so the loop sleeps.
    CHECK(driver.clock().next_boundary_at() > at(301s));
}

// The same with a show range and rate given on the command line: the range is
// the transport's, and only the rate reaches the pacer.
void a_show_range_belongs_to_the_transport_not_the_pacer() {
    app::PlaybackOptions options{};
    options.rate = show::FrameRate{60'000U, 1'001U};
    options.first_frame = show::ShowFrame{10U};
    options.last_frame = show::ShowFrame{47U};
    options.loop = false;

    const auto transport = app::clock_config(options);
    CHECK(transport.first_frame == show::ShowFrame{10U});
    CHECK(transport.last_frame == show::ShowFrame{47U});
    CHECK(!transport.loop);

    const auto pacer = app::node_pacer_config(options);
    CHECK(pacer.clock.rate == options.rate);

    cmd::NodeCommandBus node{};
    show::LocalNodeCommitSink sink{node};
    auto created = show::FrameBoundaryDriver::create(pacer, sink, at(0ns));
    CHECK(created.has_value());
    auto driver = std::move(created).value();
    CHECK(driver.play(at(0ns)).has_value());

    // 1.001 s at 60000/1001 is exactly 60 boundaries. Two of those is three
    // times the 38-frame range, and the third still arrives.
    const auto first = driver.tick(at(2'002ms));
    CHECK(first.has_value());
    CHECK(first.value().boundaries == 120U);
    const auto second = driver.tick(at(3'003ms));
    CHECK(second.has_value());
    CHECK(second.value().boundaries == 60U);
    CHECK(driver.clock().state() == show::TransportState::Playing);
}

struct Case final {
    const char* name;
    void (*run)();
};

constexpr std::array cases{
    Case{"a default node pacer runs past the default timeline",
         &a_default_node_pacer_runs_past_the_default_timeline},
    Case{"a show range belongs to the transport, not the pacer",
         &a_show_range_belongs_to_the_transport_not_the_pacer},
};

} // namespace

int main() {
    std::size_t failures{};
    for (const Case& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << cases.size() - failures << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
