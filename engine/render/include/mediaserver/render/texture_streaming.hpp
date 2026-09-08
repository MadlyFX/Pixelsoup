#pragma once

#include <mediaserver/render/model_loader.hpp>
#include <mediaserver/rhi/device.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mediaserver::render {
[[nodiscard]] inline rhi::TextureFormat model_texture_format(const ModelTexture& texture) noexcept {
    switch (texture.encoding) {
    case ModelTextureEncoding::Bc7: return rhi::TextureFormat::Bc7Unorm;
    case ModelTextureEncoding::Bc5: return rhi::TextureFormat::Bc5Unorm;
    case ModelTextureEncoding::Rgba8: return rhi::TextureFormat::Rgba8Unorm;
    }
    return rhi::TextureFormat::Rgba8Unorm;
}

struct TextureRequest final {
    std::string_view source;
    std::shared_ptr<const ModelAsset> asset;
    core::u64 generation{};
    core::u32 texture{};
    float projected_pixels{};
    // Replicated show ordinal, never an observation/access counter. Current
    // scene references are equally recent; stable source/index breaks ties.
    core::u64 last_desired{};
};

struct TextureSelection final {
    TextureRequest request;
    core::u32 desired_mip{};
    core::u32 first_mip{};
    core::u32 tail_mip{};
    core::u64 bytes{};
};

[[nodiscard]] core::u32 desired_texture_mip(core::u32 width, core::u32 height,
                                            float pixels, core::u32 bias) noexcept;
[[nodiscard]] core::u64 texture_mip_bytes(const ModelTexture &texture,
                                          core::u32 first_mip) noexcept;

// Scratch must be reserved by the caller. Every texture keeps the lowest three
// levels. Over-budget detail is evicted oldest-desired first, then source and
// image index. Insufficient tail capacity refuses the entire plan.
[[nodiscard]] bool plan_texture_residency(std::span<const TextureRequest> requests,
                                          core::u64 bytes, bool streaming,
                                          core::u32 bias,
                                          std::vector<TextureSelection> &into) noexcept;

enum class TextureStreamState : core::u8 {
    Ready,
    Loading,
    CapacityRefused,
    AllocationFailed,
    UploadFailed,
    Busy
};
struct TextureStreamingStats final {
    TextureStreamState state{TextureStreamState::Ready};
    core::u32 textures{};
    core::u32 reduced{};
    core::u32 uploads{};
    core::u32 evictions{};
    core::u64 resident_bytes{};
    core::u64 allocated_bytes{};
    core::u64 budget_bytes{};
    core::u64 upload_bytes{};
    bool operator==(const TextureStreamingStats &) const = default;
};

// One device/view owner; immutable CPU pyramids remain process-shared through
// ModelCache. All metadata/scratch is bounded at construction. Driver texture
// allocation happens only at an asset/detail change, just like model adoption.
class TextureStreamer final {
  public:
    explicit TextureStreamer(core::u32 capacity = 4096U);
    TextureStreamer(TextureStreamer &&) = default;
    TextureStreamer &operator=(TextureStreamer &&) = default;
    TextureStreamer(const TextureStreamer &) = delete;
    TextureStreamer &operator=(const TextureStreamer &) = delete;

    // Total includes active, candidate and fence-retired bytes. Half is the
    // deterministic working set, half transactional replacement headroom.
    // Upload progress is accepted only with its command receipt. A canceled
    // recording reuses the exact allocation and repeats only canceled rows.
    [[nodiscard]] bool prepare(rhi::Device &device, rhi::CommandList &commands,
                               std::span<const TextureRequest> requests,
                               core::u64 total_budget, bool streaming, core::u32 bias);
    [[nodiscard]] const rhi::Texture *
    find(std::string_view source, core::u64 generation, core::u32 index) const noexcept;
    [[nodiscard]] bool contains(std::string_view source, core::u64 generation,
                                const ModelAsset &asset) const noexcept;
    [[nodiscard]] core::u32 first_mip(std::string_view source,
                                      core::u32 index) const noexcept;
    [[nodiscard]] const TextureStreamingStats &stats() const noexcept { return stats_; }

  private:
    struct Slot final {
        std::string source;
        std::shared_ptr<const ModelAsset> asset;
        core::u64 generation{};
        core::u32 index{};
        core::u32 first_mip{};
        core::u64 bytes{};
        rhi::Texture texture;
        bool referenced{};
        core::u32 mip{};
        core::u32 row{};
        core::u32 staged_mip{};
        core::u32 staged_row{};
    };
    [[nodiscard]] bool matches(const Slot &, const TextureSelection &) const noexcept;
    [[nodiscard]] bool complete(bool staged) const noexcept;
    void reclaim();
    void update_stats();
    std::vector<Slot> slots_;
    std::vector<core::u32> active_;
    std::vector<core::u32> candidate_;
    std::vector<TextureSelection> plan_;
    rhi::CommandRecordingToken recording_;
    bool candidate_valid_{};
    bool draw_candidate_{};
    bool failed_{};
    core::u64 failed_identity_{};
    core::u64 allocation_limit_{};
    TextureStreamState failure_state_{TextureStreamState::Ready};
    TextureStreamingStats stats_{};
};
} // namespace mediaserver::render
