#include <mediaserver/render/model_ingest_queue.hpp>
#include <mediaserver/render/projection_mesh_loader.hpp>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

namespace mediaserver::render {
struct ModelIngestJob::Executor {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::deque<std::shared_ptr<ModelIngestJob>> pending;
    // Declared last so shutdown joins before the queue/mutex are destroyed.
    std::jthread worker{[this](std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::shared_ptr<ModelIngestJob> job;
            {
                std::unique_lock lock{mutex};
                changed.wait(lock, stop, [this] { return !pending.empty(); });
                if (stop.stop_requested()) break;
                job = std::move(pending.front()); pending.pop_front();
            }
            try {
                job->stage_.store(ModelIngestStage::Reading, std::memory_order_release);
                ModelAsset asset;
                if (!load_model(job->source_, asset)) throw std::runtime_error{asset.diagnostic};
                job->stage_.store(ModelIngestStage::Compressing, std::memory_order_release);
                if (!compress_model_textures(asset)) throw std::runtime_error{"Cannot prepare model textures"};
                job->stage_.store(ModelIngestStage::Writing, std::memory_order_release);
                const auto source = std::filesystem::path{job->source_}.extension() == ".msmodel"
                    ? job->source_ : write_model_package(job->source_, asset);
                auto& result = job->result_;
                result.source = source;
                result.vertex_count = asset.mesh.vertices.size();
                result.triangle_count = asset.mesh.indices.size() / 3U;
                result.material_count = static_cast<core::u32>(asset.parts.size());
                result.bounds = asset.bounds;
                if (job->projection_) {
                    const auto mesh = load_projection_mesh(source, *job->projection_, job->scale_);
                    if (!mesh) throw std::runtime_error{"Invalid UV projection mesh: check UV0 (0 to 1), geometry, calibration and scale"};
                    result.vertex_count = mesh.value().vertices.size();
                    result.triangle_count = mesh.value().indices.size() / 3U;
                }
                job->stage_.store(ModelIngestStage::Ready, std::memory_order_release);
            } catch (const std::exception& e) {
                job->result_.diagnostic = e.what();
                job->stage_.store(ModelIngestStage::Failed, std::memory_order_release);
            }
        }
    }};
};

std::shared_ptr<ModelIngestJob> ModelIngestJob::start(std::string source,
    std::optional<output::PhysicalSurface> projection, float scale) {
    static Executor executor;
    std::unique_lock lock{executor.mutex, std::try_to_lock};
    if (!lock.owns_lock() || executor.pending.size() >= 15U) return {};
    auto job = std::make_shared<ModelIngestJob>();
    job->source_ = std::move(source); job->projection_ = projection; job->scale_ = scale;
    executor.pending.push_back(job);
    executor.changed.notify_one();
    return job;
}

std::string_view ModelIngestJob::label(ModelIngestStage stage) noexcept {
    switch (stage) {
    case ModelIngestStage::Queued: return "Model import queued";
    case ModelIngestStage::Reading: return "Reading model and textures";
    case ModelIngestStage::Compressing: return "Filtering mips / compressing BC7 + BC5";
    case ModelIngestStage::Writing: return "Writing immutable model package";
    case ModelIngestStage::Ready: return "Model import complete";
    case ModelIngestStage::Failed: return "Model import failed";
    }
    return "Model import";
}
} // namespace mediaserver::render
