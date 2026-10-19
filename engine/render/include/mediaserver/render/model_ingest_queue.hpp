#pragma once

#include <mediaserver/render/model_package.hpp>
#include <mediaserver/output/feed_map.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <string_view>

namespace mediaserver::render {
enum class ModelIngestStage : core::u8 { Queued, Reading, Compressing, Writing, Ready, Failed };
struct ModelTextureAssignment final {
    core::u32 part{}, role{};
    std::string image_source;
};

class ModelIngestJob final {
public:
    [[nodiscard]] ModelIngestStage stage() const noexcept { return stage_.load(std::memory_order_acquire); }
    [[nodiscard]] const ModelIngestResult* result() const noexcept {
        const auto value = stage();
        return value == ModelIngestStage::Ready || value == ModelIngestStage::Failed ? &result_ : nullptr;
    }
    // One process worker, at most 16 outstanding jobs. Dropping a handle never
    // joins a thread. Completed values are published with release/acquire.
    [[nodiscard]] static std::shared_ptr<ModelIngestJob> start(
        std::string source, std::optional<output::PhysicalSurface> projection = {}, float scale = 1.0F,
        std::optional<ModelTextureAssignment> texture = {});
    static std::string_view label(ModelIngestStage stage) noexcept;
private:
    struct Executor;
    std::atomic<ModelIngestStage> stage_{ModelIngestStage::Queued};
    ModelIngestResult result_;
    std::string source_;
    std::optional<output::PhysicalSurface> projection_;
    float scale_{1.0F};
    std::optional<ModelTextureAssignment> texture_;
};
} // namespace mediaserver::render
