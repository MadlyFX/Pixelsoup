#include "authority_gate.hpp"

#include <algorithm>

namespace mediaserver::app {

void AuthorityGate::enter_show() {
    std::unique_lock lock{mutex_};
    changed_.wait(lock, [this] {
        return holder_ == Holder::None && sealers_waiting_ == 0U;
    });
    holder_ = Holder::Show;
    show_entered_ = Clock::now();
}

void AuthorityGate::leave_show() {
    {
        const std::lock_guard lock{mutex_};
        max_hold_ = std::max(max_hold_, Clock::now() - show_entered_);
        holder_ = Holder::None;
    }
    changed_.notify_all();
}

void AuthorityGate::enter_seal() {
    const Clock::time_point asked = Clock::now();
    std::unique_lock lock{mutex_};
    ++sealers_waiting_;
    changed_.wait(lock, [this] { return holder_ == Holder::None; });
    --sealers_waiting_;
    holder_ = Holder::Seal;
    const Clock::duration waited = Clock::now() - asked;
    ++seals_;
    if (waited > delay_threshold) {
        ++delayed_;
    }
    max_wait_ = std::max(max_wait_, waited);
    total_wait_ += waited;
}

void AuthorityGate::leave_seal() {
    {
        const std::lock_guard lock{mutex_};
        holder_ = Holder::None;
    }
    changed_.notify_all();
}

AuthorityGate::Window AuthorityGate::take() {
    const std::lock_guard lock{mutex_};
    Window window;
    window.seals = seals_;
    window.delayed = delayed_;
    window.max_wait =
        std::chrono::duration_cast<std::chrono::microseconds>(max_wait_);
    if (seals_ != 0U) {
        window.mean_wait =
            std::chrono::duration_cast<std::chrono::microseconds>(
                total_wait_ / static_cast<Clock::rep>(seals_));
    }
    window.max_hold =
        std::chrono::duration_cast<std::chrono::microseconds>(max_hold_);
    seals_ = 0U;
    delayed_ = 0U;
    max_wait_ = Clock::duration{0};
    total_wait_ = Clock::duration{0};
    max_hold_ = Clock::duration{0};
    return window;
}

bool AuthorityGate::seal_waiting() {
    const std::lock_guard lock{mutex_};
    return sealers_waiting_ != 0U;
}

std::string authority_gate_text(const AuthorityGate::Window &window) {
    return " seal-wait-max-us=" + std::to_string(window.max_wait.count()) +
           " seal-wait-mean-us=" + std::to_string(window.mean_wait.count()) +
           " seal-waited=" + std::to_string(window.delayed) + '/' +
           std::to_string(window.seals) +
           " show-hold-max-us=" + std::to_string(window.max_hold.count());
}

} // namespace mediaserver::app
