#include "edit_coalescing_policy.hpp"

#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

namespace app = mediaserver::app;
namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            throw std::runtime_error{"check failed: " #condition};         \
        }                                                                   \
    } while (false)

[[nodiscard]] doc::Node node(std::int64_t value) {
    doc::Node result{};
    result.id = doc::NodeId{1U};
    result.type = "test";
    result.properties.emplace("value", doc::PropertyValue{value});
    return result;
}

[[nodiscard]] app::EditCoalescingIdentity identity(core::u64 property = 3U) {
    return app::EditCoalescingIdentity{
        cmd::UndoContextId{2U}, cmd::CoalescingKey{1U, 2U, property, 4U, 5U}};
}

void exact_inactivity_and_key_boundaries_are_local() {
    using namespace std::chrono_literals;
    app::EditCoalescingPolicy policy;
    const auto origin = app::EditCoalescingPolicy::TimePoint{};
    CHECK(policy.observe(identity(), cmd::TransactionId{1U}, origin).action ==
          app::EditCoalescingAction::Begin);
    CHECK(policy.observe(identity(), cmd::TransactionId{2U}, origin + 349ms)
              .action == app::EditCoalescingAction::Continue);
    CHECK(policy.observe(identity(), cmd::TransactionId{3U}, origin + 699ms)
              .action == app::EditCoalescingAction::Begin);
    CHECK(policy.observe(identity(9U), cmd::TransactionId{4U}, origin + 700ms)
              .action == app::EditCoalescingAction::Begin);
    CHECK(!policy.expire(origin + 1'049ms));
    CHECK(policy.expire(origin + 1'050ms));
    CHECK(!policy.expire(origin + 2s));
}

void policy_close_forces_the_next_repeat_to_begin() {
    app::EditCoalescingPolicy policy;
    const auto origin = app::EditCoalescingPolicy::TimePoint{};
    static_cast<void>(
        policy.observe(identity(), cmd::TransactionId{1U}, origin));
    policy.close();
    CHECK(!policy.active());
    CHECK(policy.observe(identity(), cmd::TransactionId{2U}, origin).action ==
          app::EditCoalescingAction::Begin);
}

void scheduling_reuses_one_transaction_and_recovers_from_intervention() {
    using namespace std::chrono_literals;
    doc::Root initial;
    initial = initial.set(node(0));
    cmd::DirectorScheduler scheduler{initial};
    app::EditCoalescingPolicy policy;
    const auto origin = app::EditCoalescingPolicy::TimePoint{};

    auto first = policy.observe(identity(), cmd::TransactionId{10U}, origin);
    CHECK(app::schedule_editor_coalesced_edit(
              scheduler, cmd::PutNode{node(1)}, cmd::PutNode{node(0)}, first,
              cmd::FrameNumber{1U})
              .has_value());
    auto repeated = policy.observe(
        identity(), cmd::TransactionId{11U}, origin + 10ms);
    auto continued = app::schedule_editor_coalesced_edit(
        scheduler, cmd::PutNode{node(2)}, cmd::PutNode{node(1)}, repeated,
        cmd::FrameNumber{1U});
    CHECK(continued.has_value());
    CHECK(continued.value().transaction == cmd::TransactionId{10U});
    CHECK(scheduler.undo_depth(cmd::UndoContextId{2U}) == 1U);

    // An ordinary accepted command closes the scheduler even though the local
    // timer still says this key is within its window. The helper begins the
    // next repeat with its own fresh transaction instead of rejecting it.
    CHECK(scheduler
              .schedule_edit(
                  cmd::PutNode{doc::Node{doc::NodeId{2U}, "other", {}}},
                  cmd::EraseNode{doc::NodeId{2U}}, cmd::UndoContextId{9U},
                  cmd::TransactionId{20U}, cmd::FrameNumber{1U})
              .has_value());
    auto after_command = policy.observe(
        identity(), cmd::TransactionId{12U}, origin + 20ms);
    CHECK(after_command.action == app::EditCoalescingAction::Continue);
    auto restarted = app::schedule_editor_coalesced_edit(
        scheduler, cmd::PutNode{node(3)}, cmd::PutNode{node(2)}, after_command,
        cmd::FrameNumber{1U});
    CHECK(restarted.has_value());
    CHECK(restarted.value().transaction == cmd::TransactionId{12U});
    CHECK(scheduler.undo_depth(cmd::UndoContextId{2U}) == 2U);
}

struct Case final {
    const char* name;
    void (*run)();
};

constexpr std::array cases{
    Case{"exact inactivity and key boundaries are local",
         &exact_inactivity_and_key_boundaries_are_local},
    Case{"policy close forces the next repeat to begin",
         &policy_close_forces_the_next_repeat_to_begin},
    Case{"scheduling reuses one transaction and recovers from intervention",
         &scheduling_reuses_one_transaction_and_recovers_from_intervention},
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
