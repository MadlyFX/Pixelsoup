#include "compile_worker.hpp"

#include <utility>

namespace ajfx::studio {

CompileWorker::CompileWorker() : thread_{[this] { run(); }} {}

CompileWorker::~CompileWorker() {
    {
        const std::lock_guard lock{mutex_};
        stop_ = true;
        waiting_.reset();
    }
    wake_.notify_all();
    thread_.join();
}

void CompileWorker::submit(const Document& document, std::uint64_t revision) {
    {
        const std::lock_guard lock{mutex_};
        // A page older than one already handed out is never wanted.
        if (revision <= fence_) {
            return;
        }
        waiting_.emplace(document, revision);
    }
    wake_.notify_one();
}

std::optional<CompileWorker::Result> CompileWorker::take() {
    const std::lock_guard lock{mutex_};
    if (!ready_) {
        return std::nullopt;
    }
    std::optional<Result> out = std::move(ready_);
    ready_.reset();
    if (out->revision <= fence_) {
        return std::nullopt;
    }
    fence_ = out->revision;
    return out;
}

void CompileWorker::fence(std::uint64_t revision) {
    const std::lock_guard lock{mutex_};
    fence_ = std::max(fence_, revision);
    if (ready_ && ready_->revision <= fence_) {
        ready_.reset();
    }
    if (waiting_ && waiting_->second <= fence_) {
        waiting_.reset();
    }
}

bool CompileWorker::idle() const {
    const std::lock_guard lock{mutex_};
    return !busy_ && !waiting_;
}

void CompileWorker::wait_idle() {
    std::unique_lock lock{mutex_};
    settled_.wait(lock, [this] { return !busy_ && !waiting_; });
}

void CompileWorker::run() {
    for (;;) {
        std::pair<Document, std::uint64_t> job;
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, [this] { return stop_ || waiting_.has_value(); });
            if (stop_) {
                busy_ = false;
                settled_.notify_all();
                return;
            }
            job = std::move(*waiting_);
            waiting_.reset();
            busy_ = true;
        }
        Result result;
        result.revision = job.second;
        result.compiled = compile(job.first, Purpose::Preview);
        result.natural_loop = natural_loop_seconds(result.compiled);
        result.document = std::move(job.first);
        {
            const std::lock_guard lock{mutex_};
            busy_ = false;
            // Newest wins, and nothing at or below the fence is kept.
            if (result.revision > fence_ && (!ready_ || ready_->revision < result.revision)) {
                ready_ = std::move(result);
            }
        }
        settled_.notify_all();
    }
}

} // namespace ajfx::studio
