#include <mediaserver/editor/model_facts_worker.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace mediaserver::editor {
namespace {

// Packages whose facts are kept. The viewer shows one at a time; the rest are
// for stepping back through its history.
constexpr std::size_t kept_results = 16U;

struct Key final {
    std::string source;
    core::u64 generation{};

    bool operator==(const Key&) const = default;
};

struct Job final {
    Key key;
    std::shared_ptr<const render::ModelAsset> package;
};

struct Result final {
    Key key;
    std::shared_ptr<const render::ModelPackageFacts> facts;
    core::u64 stamp{};
};

} // namespace

struct ModelFactsWorker::State final {
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping{};
    std::optional<Job> queued;
    std::optional<Key> running;
    std::vector<Result> results;
    core::u64 next_stamp{};
    std::thread thread;

    void run() {
        std::unique_lock lock{mutex};
        for (;;) {
            wake.wait(lock, [this] { return stopping || queued.has_value(); });
            if (stopping) {
                return;
            }
            Job job = std::move(*queued);
            queued.reset();
            running = job.key;
            lock.unlock();
            std::shared_ptr<const render::ModelPackageFacts> facts;
            try {
                facts = std::make_shared<const render::ModelPackageFacts>(
                    render::model_package_facts(*job.package));
            } catch (const std::bad_alloc&) {
                facts.reset();
            }
            job.package.reset();
            lock.lock();
            running.reset();
            if (facts != nullptr) {
                if (results.size() >= kept_results) {
                    results.erase(std::min_element(
                        results.begin(), results.end(),
                        [](const Result& a, const Result& b) {
                            return a.stamp < b.stamp;
                        }));
                }
                results.push_back(Result{job.key, std::move(facts), ++next_stamp});
            }
        }
    }
};

ModelFactsWorker::ModelFactsWorker() : state_(std::make_shared<State>()) {
    state_->thread = std::thread{[state = state_.get()] { state->run(); }};
}

ModelFactsWorker::~ModelFactsWorker() {
    {
        const std::lock_guard lock{state_->mutex};
        state_->stopping = true;
    }
    state_->wake.notify_all();
    if (state_->thread.joinable()) {
        state_->thread.join();
    }
}

std::shared_ptr<const render::ModelPackageFacts> ModelFactsWorker::facts(
    const std::string& source, core::u64 generation,
    std::shared_ptr<const render::ModelAsset> package) {
    if (package == nullptr) {
        return nullptr;
    }
    std::unique_lock lock{state_->mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        // The worker is storing a result this instant; ask again next frame.
        return nullptr;
    }
    Key key{source, generation};
    for (Result& result : state_->results) {
        if (result.key == key) {
            result.stamp = ++state_->next_stamp;
            return result.facts;
        }
    }
    if (state_->running == key ||
        (state_->queued.has_value() && state_->queued->key == key)) {
        return nullptr;
    }
    state_->queued = Job{std::move(key), std::move(package)};
    lock.unlock();
    state_->wake.notify_one();
    return nullptr;
}

} // namespace mediaserver::editor
