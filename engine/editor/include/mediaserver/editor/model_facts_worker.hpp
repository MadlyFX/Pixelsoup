#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/render/model_report.hpp>

#include <memory>
#include <string>

// Works out a decoded model package's facts for the Asset Viewer away from
// the editor's frame. model_package_facts reads every index of the package,
// which for a large scan is millions of reads, so it runs on one worker
// thread, one package at a time, newest request first. Results are kept for
// the last few packages by (source, cache generation), so going back to a
// model shows its facts at once and a refreshed file is worked out again.
namespace mediaserver::editor {

class ModelFactsWorker final {
public:
    ModelFactsWorker();
    ~ModelFactsWorker();
    ModelFactsWorker(const ModelFactsWorker&) = delete;
    ModelFactsWorker& operator=(const ModelFactsWorker&) = delete;

    // The facts of `package`, decoded from `source` at cache `generation`, or
    // null while they are being worked out. Never waits: a package not yet
    // asked for is queued, replacing whatever was queued and not started.
    [[nodiscard]] std::shared_ptr<const render::ModelPackageFacts> facts(
        const std::string& source, core::u64 generation,
        std::shared_ptr<const render::ModelAsset> package);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace mediaserver::editor
