#include <mediaserver/track/pose_slot.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

namespace core = mediaserver::core;
namespace track = mediaserver::track;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define SLOT_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] track::TrackedPose pose_at(float x, core::u64 sequence) noexcept {
    track::TrackedPose pose{};
    pose.protocol = track::TrackerProtocol::FreeD;
    pose.x = x;
    pose.y = x * 2.0F;
    pose.z = x * 3.0F;
    pose.pan = x * 0.001F;
    pose.sequence = sequence;
    return pose;
}

void an_empty_slot_reports_nothing() {
    const track::PoseSlot slot;
    SLOT_CHECK(!slot.sample().has_value());
    SLOT_CHECK(slot.publications() == 0U);
    SLOT_CHECK(slot.refusals() == 0U);
}

void a_published_pose_is_the_one_sampled() {
    track::PoseSlot slot;
    SLOT_CHECK(slot.publish(pose_at(1.5F, 10U)));
    const auto first = slot.sample();
    SLOT_CHECK(first.has_value());
    SLOT_CHECK(first->pose == pose_at(1.5F, 10U));
    SLOT_CHECK(first->publications == 1U);

    // Sampling twice without a publish is the same pose and the same count,
    // which is how a caller tells a stalled tracker from a still one.
    const auto again = slot.sample();
    SLOT_CHECK(again.has_value());
    SLOT_CHECK(*again == *first);

    SLOT_CHECK(slot.publish(pose_at(2.5F, 11U)));
    const auto second = slot.sample();
    SLOT_CHECK(second.has_value());
    SLOT_CHECK(second->pose.x == 2.5F);
    SLOT_CHECK(second->publications == 2U);
}

void an_invalid_pose_is_refused_and_never_observable() {
    track::PoseSlot slot;
    SLOT_CHECK(slot.publish(pose_at(1.0F, 1U)));

    track::TrackedPose broken = pose_at(2.0F, 2U);
    broken.y = std::nanf("");
    SLOT_CHECK(!slot.publish(broken));

    track::TrackedPose unsourced = pose_at(3.0F, 3U);
    unsourced.protocol = track::TrackerProtocol::None;
    SLOT_CHECK(!slot.publish(unsourced));

    const auto observed = slot.sample();
    SLOT_CHECK(observed.has_value());
    SLOT_CHECK(observed->pose.x == 1.0F);
    SLOT_CHECK(observed->publications == 1U);
    SLOT_CHECK(slot.refusals() == 2U);
}

// The property the slot exists for: a reader concurrent with a writer sees a
// complete pose or nothing, never one publish's position with another's
// rotation. Every field here is a fixed multiple of `x`, so a torn read is
// detectable rather than merely unlikely.
void a_concurrent_sample_is_never_a_mixture() {
    track::PoseSlot slot;
    std::atomic_bool stop{false};
    std::atomic_uint64_t torn{0U};
    std::atomic_uint64_t observed{0U};

    std::thread writer{[&slot, &stop]() {
        core::u64 sequence = 0U;
        while (!stop.load(std::memory_order_relaxed)) {
            ++sequence;
            (void)slot.publish(
                pose_at(static_cast<float>(sequence % 4'096U), sequence));
        }
    }};

    // Read until a meaningful number of samples have actually been observed
    // rather than for a fixed number of attempts. An optimized build can run
    // a fixed loop to completion before the writer thread has even started,
    // which would make the tear check below vacuous while still passing.
    constexpr core::u64 wanted = 4'096U;
    constexpr int attempt_limit = 200'000'000;
    int attempts = 0;
    while (observed.load(std::memory_order_relaxed) < wanted
           && attempts < attempt_limit) {
        ++attempts;
        const auto sample = slot.sample();
        if (!sample.has_value()) {
            continue;
        }
        observed.fetch_add(1U, std::memory_order_relaxed);
        const track::TrackedPose& pose = sample->pose;
        if (pose.y != pose.x * 2.0F || pose.z != pose.x * 3.0F
            || pose.pan != pose.x * 0.001F) {
            torn.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();

    SLOT_CHECK(torn.load() == 0U);
    // If every read had lost its race the check above would prove nothing.
    SLOT_CHECK(observed.load() >= wanted);
    SLOT_CHECK(slot.sample().has_value());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 4U> cases{{
    {"an empty slot reports nothing", &an_empty_slot_reports_nothing},
    {"a published pose is the one sampled",
     &a_published_pose_is_the_one_sampled},
    {"an invalid pose is refused and never observable",
     &an_invalid_pose_is_refused_and_never_observable},
    {"a concurrent sample is never a mixture",
     &a_concurrent_sample_is_never_a_mixture},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
