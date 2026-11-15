#pragma once

// Who may touch the Director's document authority, and when.
//
// Two threads share the scheduler, the transport and the frame barrier's
// bookkeeping. The sealing thread wakes at each show boundary, seals the
// control frames that are due and goes back to sleep; the cluster presents
// what it seals, so it must never wait long. The show thread authors operator
// input, evaluates the committed frame, draws the editor and presents the
// Director's own output; that work is as slow as the scene and the GPU make
// it.
//
// The gate is how they take turns. Whoever is inside it has the authority to
// itself. The sealing thread goes first: once it is waiting, the show thread
// cannot enter (or re-enter) until it has been through. The show thread in
// turn leaves the gate for everything that can block - drawing, presenting,
// file access - so the sealing thread's wait is bounded by the show thread's
// document work and not by its slowest frame.
//
// The gate also keeps the measurement that says whether that holds: how long
// the sealing thread waited to get in, and the longest the show thread stayed.

#include <mediaserver/core/types.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

namespace mediaserver::app {

class AuthorityGate final {
  public:
    using Clock = std::chrono::steady_clock;

    // A sealing-thread wait this long is counted as a delay: the same
    // threshold a late boundary wake is reported at.
    static constexpr std::chrono::microseconds delay_threshold{2000};

    // What happened at the gate since the last take().
    struct Window final {
        // Times the sealing thread entered, and how many of them waited
        // longer than delay_threshold.
        core::u64 seals{};
        core::u64 delayed{};
        std::chrono::microseconds max_wait{0};
        std::chrono::microseconds mean_wait{0};
        // The longest single stay of the show thread.
        std::chrono::microseconds max_hold{0};

        bool operator==(const Window &) const = default;
    };

    AuthorityGate() = default;
    AuthorityGate(const AuthorityGate &) = delete;
    AuthorityGate &operator=(const AuthorityGate &) = delete;

    // The show thread. enter_show() waits while the sealing thread is inside
    // or waiting to be.
    void enter_show();
    void leave_show();

    // The sealing thread. enter_seal() waits only for a show thread already
    // inside.
    void enter_seal();
    void leave_seal();

    // The measurements so far, starting a new window. Any thread.
    [[nodiscard]] Window take();

    // Whether the sealing thread is waiting to get in. Any thread.
    [[nodiscard]] bool seal_waiting();

    // Holds the gate for the show thread for a scope.
    class Show final {
      public:
        explicit Show(AuthorityGate &gate) : gate_{gate} { gate_.enter_show(); }
        Show(const Show &) = delete;
        Show &operator=(const Show &) = delete;
        ~Show() { gate_.leave_show(); }

      private:
        AuthorityGate &gate_;
    };

    // Leaves the gate for a scope the show thread spends on something that
    // can block, and takes it back afterwards. Nothing read from the
    // authority before the scope may be relied on after it: the sealing
    // thread may have sealed frames meanwhile.
    class Away final {
      public:
        explicit Away(AuthorityGate &gate) : gate_{gate} { gate_.leave_show(); }
        Away(const Away &) = delete;
        Away &operator=(const Away &) = delete;
        ~Away() { gate_.enter_show(); }

      private:
        AuthorityGate &gate_;
    };

    // Holds the gate for the sealing thread for a scope.
    class Seal final {
      public:
        explicit Seal(AuthorityGate &gate) : gate_{gate} { gate_.enter_seal(); }
        Seal(const Seal &) = delete;
        Seal &operator=(const Seal &) = delete;
        ~Seal() { gate_.leave_seal(); }

      private:
        AuthorityGate &gate_;
    };

  private:
    enum class Holder : core::u8 { None, Show, Seal };

    std::mutex mutex_;
    std::condition_variable changed_;
    Holder holder_{Holder::None};
    core::u32 sealers_waiting_{};
    Clock::time_point show_entered_{};

    core::u64 seals_{};
    core::u64 delayed_{};
    Clock::duration max_wait_{0};
    Clock::duration total_wait_{0};
    Clock::duration max_hold_{0};
};

// The gate's window for a console status line.
[[nodiscard]] std::string
authority_gate_text(const AuthorityGate::Window &window);

} // namespace mediaserver::app
