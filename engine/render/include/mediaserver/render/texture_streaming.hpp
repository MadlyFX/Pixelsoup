#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/render/model_loader.hpp>
#include <mediaserver/rhi/device.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
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
    // Model texture bytes resident on the device for every streamer on it,
    // each texture counted once however many of them hold it.
    core::u64 device_bytes{};
    bool operator==(const TextureStreamingStats &) const = default;
};

// Model textures resident on one device, shared by every TextureStreamer that
// prepares on it. The editor holds a renderer per view, compositor layer and
// planar reflection, and each plans from the complete authored scene, so they
// want the same images at the same detail: here a decoded image from a given
// level down is one texture, uploaded once, whichever streamer asked first.
//
// Rows are written across recordings, each landing once its recording is
// submitted; a canceled piece is written again, and a texture is never
// written by two unsubmitted recordings at once. A texture is drawable in a
// recording once every row has landed or was written earlier in that
// recording. It is released when no streamer holds it and the device is done
// with it.
class TextureResidency final {
  public:
    struct Entry final {
        struct Write final {
            rhi::CommandRecordingToken recording;
            core::u64 begin{};
            core::u64 end{};
        };
        // Owns the source pyramid, and keeps its address from naming another
        // model while this texture lives.
        std::shared_ptr<const ModelAsset> asset;
        core::u32 index{};
        core::u32 first_mip{};
        core::u64 bytes{};
        // Distinct for every texture the residency ever made, so a product
        // drawn from this one can tell it from a later one at the same address.
        core::u64 serial{};
        rhi::Texture texture;
        // The next row to write, as (level << 32) | row in source levels.
        core::u64 next{};
        std::vector<Write> writes;
    };
    // A streamer's hold on a texture. The last one dropped releases it once
    // the device has finished with it.
    using Handle = std::shared_ptr<Entry>;

    struct Streamed final {
        bool uploaded{true};
        core::u32 uploads{};
        core::u64 bytes{};
    };

    // The residency every streamer on `device` shares, created by the first
    // one. It lives as long as any streamer holds it.
    [[nodiscard]] static core::Result<std::shared_ptr<TextureResidency>, rhi::RhiError>
    for_device(rhi::Device &device) noexcept;

    TextureResidency(const TextureResidency &) = delete;
    TextureResidency &operator=(const TextureResidency &) = delete;
    ~TextureResidency();

    // Re-entrant, so a streamer's own release can run inside its prepare.
    [[nodiscard]] std::unique_lock<std::recursive_mutex> lock() {
        return std::unique_lock{mutex_};
    }

    // The texture another streamer already made for these levels of an image.
    [[nodiscard]] Handle find(const ModelAsset &asset, core::u32 index,
                              core::u32 first_mip) const noexcept;
    // Registers a texture just created for these levels, so later streamers
    // share it. Empty when it cannot be recorded.
    [[nodiscard]] Handle adopt(const std::shared_ptr<const ModelAsset> &asset,
                               core::u32 index, core::u32 first_mip, core::u64 bytes,
                               rhi::Texture texture);
    // Writes the rows this recording can carry, up to `budget` bytes, which it
    // reduces by what was written.
    [[nodiscard]] Streamed stream(Entry &entry, rhi::CommandList &commands,
                                  core::u64 &budget);
    // True when every row has landed or was written by this recording.
    [[nodiscard]] static bool drawable(const Entry &entry,
                                       const rhi::CommandRecordingToken &current) noexcept;
    // True when every row has landed.
    [[nodiscard]] bool landed(Entry &entry) noexcept;
    // Frees released textures the device has finished with.
    void collect() noexcept;

    // The serial of a texture this residency holds; zero for any other.
    [[nodiscard]] core::u64 serial(const rhi::Texture *texture) const noexcept;
    [[nodiscard]] core::u64 resident_bytes() const noexcept { return resident_bytes_; }

  private:
    struct Release final {
        std::weak_ptr<TextureResidency> owner;
        void operator()(Entry *entry) const noexcept;
    };
    using Key = std::tuple<const ModelAsset *, core::u32, core::u32>;

    TextureResidency() noexcept = default;
    static void reconcile(Entry &entry) noexcept;
    void retire(std::unique_ptr<Entry> entry) noexcept;
    void destroy(std::unique_ptr<Entry> entry) noexcept;

    std::recursive_mutex mutex_;
    std::weak_ptr<TextureResidency> self_;
    std::map<Key, std::weak_ptr<Entry>> entries_;
    std::unordered_map<const rhi::Texture *, core::u64> serials_;
    // Released textures waiting for the device.
    std::vector<std::unique_ptr<Entry>> retiring_;
    core::u64 next_serial_{1U};
    core::u64 resident_bytes_{};
};

// One device/view owner; immutable CPU pyramids remain process-shared through
// ModelCache, and the textures made from them through TextureResidency. All
// metadata/scratch is bounded at construction. Driver texture allocation
// happens only at an asset/detail change, just like model adoption.
class TextureStreamer final {
  public:
    // Without a residency, the device's own is taken on the first prepare.
    explicit TextureStreamer(core::u32 capacity = 4096U,
                             std::shared_ptr<TextureResidency> residency = {});
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
    [[nodiscard]] const std::shared_ptr<TextureResidency> &residency() const noexcept {
        return residency_;
    }

  private:
    struct Slot final {
        std::string source;
        core::u64 generation{};
        core::u32 index{};
        core::u32 first_mip{};
        core::u64 bytes{};
        TextureResidency::Handle texture;
        bool referenced{};
    };
    [[nodiscard]] bool matches(const Slot &, const TextureSelection &) const noexcept;
    [[nodiscard]] bool landed(const std::vector<core::u32> &indices) noexcept;
    void reclaim();
    void update_stats();
    // Declared first so the slots' holds are released into it.
    std::shared_ptr<TextureResidency> residency_;
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
