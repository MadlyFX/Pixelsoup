#include "authority_gate.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

namespace app = mediaserver::app;

using namespace std::chrono_literals;

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define AG_CHECK(...)                                                          \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// Spins until the sealing thread is parked at the gate, so a test's next step
// is taken with it known to be waiting and not merely started.
void wait_for_sealer(app::AuthorityGate &gate) {
    while (!gate.seal_waiting()) {
        std::this_thread::yield();
    }
}

// The property the whole split rests on: a show thread that leaves the gate
// and comes straight back does not get in ahead of a sealing thread that was
// already waiting, however quickly it returns.
void a_waiting_sealing_thread_goes_before_a_returning_show_thread() {
    app::AuthorityGate gate;
    std::atomic<int> sealed{0};
    gate.enter_show();
    std::thread sealing{[&] {
        const app::AuthorityGate::Seal inside{gate};
        sealed.store(1);
    }};
    wait_for_sealer(gate);
    AG_CHECK(sealed.load() == 0);

    gate.leave_show();
    gate.enter_show();
    // Back inside only after the sealing thread has been through.
    AG_CHECK(sealed.load() == 1);
    gate.leave_show();
    sealing.join();
}

// Repeated, because one hand-over proves little about a show thread that
// returns hundreds of times while frames come due.
void every_waiting_seal_gets_in_within_one_show_stay() {
    constexpr int seals = 37;
    app::AuthorityGate gate;
    std::atomic<int> sealed{0};
    gate.enter_show();
    for (int index = 0; index < seals; ++index) {
        std::thread sealing{[&] {
            const app::AuthorityGate::Seal inside{gate};
            sealed.fetch_add(1);
        }};
        wait_for_sealer(gate);
        AG_CHECK(sealed.load() == index);
        gate.leave_show();
        gate.enter_show();
        AG_CHECK(sealed.load() == index + 1);
        sealing.join();
    }
    gate.leave_show();
    AG_CHECK(gate.take().seals == static_cast<std::uint64_t>(seals));
}

// What Away is for: while the show thread draws or writes a file, sealing
// does not wait for it at all.
void a_show_thread_that_is_away_does_not_hold_sealing() {
    app::AuthorityGate gate;
    const app::AuthorityGate::Show inside{gate};
    std::atomic<int> sealed{0};
    {
        const app::AuthorityGate::Away drawing{gate};
        auto sealing = std::async(std::launch::async, [&] {
            const app::AuthorityGate::Seal seal{gate};
            sealed.store(1);
        });
        // Not a timing bound: a gate still held here would never let the
        // sealing thread finish, and the test would report that rather than
        // hang.
        AG_CHECK(sealing.wait_for(30s) == std::future_status::ready);
        AG_CHECK(sealed.load() == 1);
    }
    // And Away took the gate back: a sealing thread now has to wait again.
    std::thread sealing{[&] {
        const app::AuthorityGate::Seal seal{gate};
        sealed.store(2);
    }};
    wait_for_sealer(gate);
    AG_CHECK(sealed.load() == 1);
    {
        const app::AuthorityGate::Away writing{gate};
        sealing.join();
    }
    AG_CHECK(sealed.load() == 2);
}

// Both threads change the same plain counters inside the gate. Lost updates
// or two threads inside at once show up in the totals.
void the_two_threads_are_never_inside_together() {
    constexpr std::uint64_t show_turns = 2'903U;
    constexpr std::uint64_t seal_turns = 1'741U;
    app::AuthorityGate gate;
    std::uint64_t turns = 0U;
    int inside = 0;
    std::uint64_t overlaps = 0U;
    const auto turn = [&] {
        if (++inside != 1) {
            ++overlaps;
        }
        ++turns;
        --inside;
    };
    std::thread sealing{[&] {
        for (std::uint64_t index = 0U; index < seal_turns; ++index) {
            const app::AuthorityGate::Seal seal{gate};
            turn();
        }
    }};
    for (std::uint64_t index = 0U; index < show_turns; ++index) {
        const app::AuthorityGate::Show show{gate};
        turn();
    }
    sealing.join();
    AG_CHECK(turns == show_turns + seal_turns);
    AG_CHECK(overlaps == 0U);
    AG_CHECK(gate.take().seals == seal_turns);
}

void the_window_reports_what_sealing_waited_and_the_longest_show_stay() {
    app::AuthorityGate gate;
    constexpr auto stay = 40ms;
    gate.enter_show();
    std::thread sealing{[&] {
        // One entry that has to wait out the show thread's stay ...
        { const app::AuthorityGate::Seal seal{gate}; }
        // ... and three with nobody in the way.
        for (int index = 0; index < 3; ++index) {
            const app::AuthorityGate::Seal seal{gate};
        }
    }};
    wait_for_sealer(gate);
    std::this_thread::sleep_for(stay);
    gate.leave_show();
    sealing.join();

    const app::AuthorityGate::Window window = gate.take();
    AG_CHECK(window.seals == 4U);
    // The wait began no later than when the show thread saw it waiting, and
    // a sleep never returns early.
    AG_CHECK(window.max_wait >= stay);
    AG_CHECK(window.delayed >= 1U);
    AG_CHECK(window.delayed <= window.seals);
    // Four waits, one of them long: the mean is at least a quarter of it and
    // cannot reach it.
    AG_CHECK(window.mean_wait >= stay / 4);
    AG_CHECK(window.mean_wait <= window.max_wait);
    AG_CHECK(window.max_hold >= stay);

    AG_CHECK(gate.take() == app::AuthorityGate::Window{});
}

void the_status_text_names_every_figure() {
    app::AuthorityGate::Window window;
    window.seals = 53U;
    window.delayed = 6U;
    window.max_wait = 3'127us;
    window.mean_wait = 418us;
    window.max_hold = 9'650us;
    AG_CHECK(app::authority_gate_text(window) ==
             " seal-wait-max-us=3127 seal-wait-mean-us=418 seal-waited=6/53"
             " show-hold-max-us=9650");
}

struct Case final {
    const char *name;
    void (*function)();
};

constexpr std::array cases{
    Case{"a waiting sealing thread goes before a returning show thread",
         &a_waiting_sealing_thread_goes_before_a_returning_show_thread},
    Case{"every waiting seal gets in within one show stay",
         &every_waiting_seal_gets_in_within_one_show_stay},
    Case{"a show thread that is away does not hold sealing",
         &a_show_thread_that_is_away_does_not_hold_sealing},
    Case{"the two threads are never inside together",
         &the_two_threads_are_never_inside_together},
    Case{"the window reports what sealing waited and the longest show stay",
         &the_window_reports_what_sealing_waited_and_the_longest_show_stay},
    Case{"the status text names every figure",
         &the_status_text_names_every_figure},
};

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
