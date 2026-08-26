#include "project_asset_resolution_worker.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;

using namespace std::chrono_literals;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} +
                                 std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define WORKER_CHECK(...)                                                      \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] assets::ContentIdentity identity(core::u8 seed) {
    assets::ContentIdentity result{};
    result.sha256.fill(seed);
    result.byte_size = 3U;
    return result;
}

[[nodiscard]] app::ProjectAssetResolutionPlan plan_for(
    std::initializer_list<core::u64> ids, core::u64 digest) {
    app::ProjectAssetResolutionPlan plan{};
    plan.manifest.digest = digest;
    for (const core::u64 raw : ids) {
        const doc::NodeId id{raw};
        plan.manifest.assets.push_back(app::RequiredProjectAsset{
            id, assets::AssetKind::Media,
            assets::AssetContent{
                assets::AssetDistributionPolicy::SharedStore,
                identity(static_cast<core::u8>(raw))},
            "asset-" + std::to_string(raw) + ".mp4"});
        plan.fail_closed.assets.emplace(id, std::string{});
        plan.fail_closed.observations.push_back(
            app::LocalAssetObservation{id, app::LocalAssetState::Checking});
    }
    return plan;
}

[[nodiscard]] std::shared_ptr<const app::ResolvedProjectAssets>
deny_overlay(const std::shared_ptr<const app::ProjectAssetResolutionPlan>&
                 plan) {
    return std::shared_ptr<const app::ResolvedProjectAssets>{
        plan, &plan->fail_closed};
}

[[nodiscard]] std::optional<app::ProjectAssetResolutionCompletion>
wait_for_completion(app::ProjectAssetResolutionWorker& worker,
                    std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto completed = worker.take_completed()) {
            return completed;
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

void submit_is_nonblocking_and_full_completion_is_ready() {
    std::atomic_bool entered{};
    std::atomic_bool released{};
    app::ProjectAssetResolutionWorker worker{
        [&](const app::RequiredProjectAsset& required,
            const assets::ContentResolutionRoots&, std::stop_token stop) {
            entered.store(true, std::memory_order_release);
            while (!released.load(std::memory_order_acquire) &&
                   !stop.stop_requested()) {
                std::this_thread::yield();
            }
            return app::ProjectAssetLocalCheck{
                required.asset, app::LocalAssetState::Verified,
                "verified.mp4"};
        }};
    auto plan = std::make_shared<const app::ProjectAssetResolutionPlan>(
        plan_for({1U}, 101U));
    const auto baseline = deny_overlay(plan);
    const auto started = std::chrono::steady_clock::now();
    const auto submitted = worker.submit(app::ProjectAssetResolutionRequest{
        app::ProjectAssetResolutionKey{1U, 101U, 7U, {}}, plan, baseline,
        app::ProjectAssetResolutionMode::Full});
    WORKER_CHECK(submitted ==
                 app::ProjectAssetResolutionSubmitResult::Accepted);
    WORKER_CHECK(std::chrono::steady_clock::now() - started < 100ms);
    const auto entry_deadline = std::chrono::steady_clock::now() + 2s;
    while (!entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entry_deadline) {
        std::this_thread::yield();
    }
    WORKER_CHECK(entered.load(std::memory_order_acquire));
    WORKER_CHECK(!worker.take_completed().has_value());
    released.store(true, std::memory_order_release);
    auto completed = wait_for_completion(worker);
    WORKER_CHECK(completed.has_value());
    WORKER_CHECK(completed->succeeded);
    WORKER_CHECK(completed->locally_usable);
    WORKER_CHECK(completed->ready);
    WORKER_CHECK(completed->resolved != nullptr);
    WORKER_CHECK(completed->resolved->assets.at(doc::NodeId{1U}) ==
                 "verified.mp4");
}

void latest_request_wins_and_aba_cannot_publish_stale_work() {
    std::atomic_uint32_t calls{};
    std::atomic_bool first_entered{};
    app::ProjectAssetResolutionWorker worker{
        [&](const app::RequiredProjectAsset& required,
            const assets::ContentResolutionRoots&, std::stop_token stop) {
            const core::u32 call =
                calls.fetch_add(1U, std::memory_order_relaxed);
            if (call == 0U) {
                first_entered.store(true, std::memory_order_release);
                while (!stop.stop_requested()) {
                    std::this_thread::yield();
                }
            }
            return app::ProjectAssetLocalCheck{
                required.asset, app::LocalAssetState::Verified,
                "latest.mp4"};
        }};
    auto plan = std::make_shared<const app::ProjectAssetResolutionPlan>(
        plan_for({1U}, 202U));
    const auto baseline = deny_overlay(plan);
    WORKER_CHECK(worker.submit(app::ProjectAssetResolutionRequest{
                     {1U, 202U, 1U, {}}, plan, baseline,
                     app::ProjectAssetResolutionMode::Full}) ==
                 app::ProjectAssetResolutionSubmitResult::Accepted);
    const auto entry_deadline = std::chrono::steady_clock::now() + 2s;
    while (!first_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entry_deadline) {
        std::this_thread::yield();
    }
    WORKER_CHECK(first_entered.load(std::memory_order_acquire));

    // A -> B -> A uses the same manifest digest, but request_id prevents the
    // first A from being mistaken for the latest roots/generation snapshot.
    WORKER_CHECK(worker.submit(app::ProjectAssetResolutionRequest{
                     {2U, 202U, 2U, {}}, plan, baseline,
                     app::ProjectAssetResolutionMode::Full}) ==
                 app::ProjectAssetResolutionSubmitResult::Accepted);
    WORKER_CHECK(worker.submit(app::ProjectAssetResolutionRequest{
                     {3U, 202U, 1U, {}}, plan, baseline,
                     app::ProjectAssetResolutionMode::Full}) ==
                 app::ProjectAssetResolutionSubmitResult::Accepted);
    auto completed = wait_for_completion(worker);
    WORKER_CHECK(completed.has_value());
    WORKER_CHECK(completed->succeeded);
    WORKER_CHECK(completed->key.request_id == 3U);
    WORKER_CHECK(completed->key.project_generation == 1U);
    WORKER_CHECK(calls.load(std::memory_order_relaxed) >= 2U);
    WORKER_CHECK(!worker.take_completed().has_value());
}

void retry_checks_only_unresolved_observations() {
    std::mutex calls_mutex;
    std::vector<doc::NodeId> calls;
    app::ProjectAssetResolutionWorker worker{
        [&](const app::RequiredProjectAsset& required,
            const assets::ContentResolutionRoots&, std::stop_token) {
            {
                const std::scoped_lock lock{calls_mutex};
                calls.push_back(required.asset);
            }
            return app::ProjectAssetLocalCheck{
                required.asset, app::LocalAssetState::Verified,
                "arrived.mp4"};
        }};
    auto plan = std::make_shared<const app::ProjectAssetResolutionPlan>(
        plan_for({1U, 2U}, 303U));
    auto mutable_baseline =
        std::make_shared<app::ResolvedProjectAssets>(plan->fail_closed);
    mutable_baseline->observations[0U].state =
        app::LocalAssetState::Verified;
    mutable_baseline->observations[1U].state =
        app::LocalAssetState::Missing;
    mutable_baseline->assets[doc::NodeId{1U}] = "cached.mp4";
    std::shared_ptr<const app::ResolvedProjectAssets> baseline{
        std::move(mutable_baseline)};
    WORKER_CHECK(worker.submit(app::ProjectAssetResolutionRequest{
                     {1U, 303U, 4U, {}}, plan, baseline,
                     app::ProjectAssetResolutionMode::RetryUnresolved}) ==
                 app::ProjectAssetResolutionSubmitResult::Accepted);
    auto completed = wait_for_completion(worker);
    WORKER_CHECK(completed.has_value());
    WORKER_CHECK(completed->succeeded);
    WORKER_CHECK(completed->ready);
    {
        const std::scoped_lock lock{calls_mutex};
        WORKER_CHECK(calls.size() == 1U);
        WORKER_CHECK(calls[0U] == doc::NodeId{2U});
    }
    WORKER_CHECK(completed->resolved->assets.at(doc::NodeId{1U}) ==
                 "cached.mp4");
    WORKER_CHECK(completed->resolved->assets.at(doc::NodeId{2U}) ==
                 "arrived.mp4");
}

void local_completion_accepts_legacy_fallback_without_cluster_adoption() {
    app::ProjectAssetResolutionPlan plan{};
    plan.manifest.digest = 350U;
    plan.manifest.unverified_assets = 1U;
    plan.manifest.non_shared_assets = 2U;
    plan.manifest.assets.push_back(app::RequiredProjectAsset{
        doc::NodeId{1U}, assets::AssetKind::Media,
        assets::AssetContent{
            assets::AssetDistributionPolicy::External, identity(1U)},
        "identified-external.mp4"});
    plan.manifest.assets.push_back(app::RequiredProjectAsset{
        doc::NodeId{2U}, assets::AssetKind::Media,
        assets::AssetContent{
            assets::AssetDistributionPolicy::External, std::nullopt},
        "legacy.mov"});
    plan.fail_closed.assets.emplace(doc::NodeId{1U}, std::string{});
    plan.fail_closed.observations.push_back(app::LocalAssetObservation{
        doc::NodeId{1U}, app::LocalAssetState::Checking});
    plan.fail_closed.observations.push_back(app::LocalAssetObservation{
        doc::NodeId{2U}, app::LocalAssetState::Unverified});
    auto shared_plan =
        std::make_shared<const app::ProjectAssetResolutionPlan>(
            std::move(plan));
    app::ProjectAssetResolutionWorker worker{
        [](const app::RequiredProjectAsset& required,
           const assets::ContentResolutionRoots&, std::stop_token) {
            return app::ProjectAssetLocalCheck{
                required.asset, app::LocalAssetState::Verified,
                "verified-external.mp4"};
        }};
    WORKER_CHECK(worker.submit(app::ProjectAssetResolutionRequest{
                     {1U, 350U, 1U, {}}, shared_plan,
                     deny_overlay(shared_plan),
                     app::ProjectAssetResolutionMode::Full}) ==
                 app::ProjectAssetResolutionSubmitResult::Accepted);
    auto completed = wait_for_completion(worker);
    WORKER_CHECK(completed.has_value());
    WORKER_CHECK(completed->succeeded);
    WORKER_CHECK(completed->locally_usable);
    WORKER_CHECK(!completed->ready);
    WORKER_CHECK(completed->resolved->observations[1U].state ==
                 app::LocalAssetState::Unverified);
}

void destruction_requests_cooperative_cancellation_and_joins() {
    std::atomic_bool entered{};
    const auto started = std::chrono::steady_clock::now();
    {
        app::ProjectAssetResolutionWorker worker{
            [&](const app::RequiredProjectAsset& required,
                const assets::ContentResolutionRoots&,
                std::stop_token stop) {
                entered.store(true, std::memory_order_release);
                while (!stop.stop_requested()) {
                    std::this_thread::yield();
                }
                return app::ProjectAssetLocalCheck{
                    required.asset, app::LocalAssetState::Error, {}};
            }};
        auto plan =
            std::make_shared<const app::ProjectAssetResolutionPlan>(
                plan_for({1U}, 404U));
        WORKER_CHECK(worker.submit(app::ProjectAssetResolutionRequest{
                         {1U, 404U, 1U, {}}, plan, deny_overlay(plan),
                         app::ProjectAssetResolutionMode::Full}) ==
                     app::ProjectAssetResolutionSubmitResult::Accepted);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!entered.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        WORKER_CHECK(entered.load(std::memory_order_acquire));
    }
    WORKER_CHECK(std::chrono::steady_clock::now() - started < 2s);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr Case cases[]{
    {"submit is nonblocking and full completion is ready",
     &submit_is_nonblocking_and_full_completion_is_ready},
    {"latest request wins and ABA cannot publish stale work",
     &latest_request_wins_and_aba_cannot_publish_stale_work},
    {"retry checks only unresolved observations",
     &retry_checks_only_unresolved_observations},
    {"local completion accepts legacy fallback without cluster adoption",
     &local_completion_accepts_legacy_fallback_without_cluster_adoption},
    {"destruction requests cooperative cancellation and joins",
     &destruction_requests_cooperative_cancellation_and_joins},
};

} // namespace

int main() {
    std::size_t failures{};
    for (const Case& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (std::size(cases) - failures) << '/' << std::size(cases)
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
