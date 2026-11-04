#pragma once

#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/math.hpp>
#include <span>

namespace mediaserver::render {
inline constexpr core::u32 ltc_resolution = 64;
inline constexpr core::u32 ltc_table_entries = 2 * ltc_resolution * ltc_resolution;
// Immutable, pinned fit. All parsing/fitting happened offline. Construction
// reserves the GPU buffer; recording can only upload this pre-existing asset.
// It is uploaded once and again only if that recording never ran: an asset
// generation changes nothing in it, and re-sending it would collide with the
// frames still in flight that read it.
[[nodiscard]] std::span<const scene::Vec4> ltc_table_data() noexcept;
class LtcAsset final {
public:
    [[nodiscard]] bool create(rhi::Device& device);
    [[nodiscard]] bool prepare(rhi::CommandList& commands) noexcept;
    [[nodiscard]] const rhi::StorageBuffer& buffer() const noexcept { return buffer_; }
    [[nodiscard]] core::u64 upload_bytes() const noexcept { return upload_bytes_; }
private:
    rhi::StorageBuffer buffer_;
    rhi::CommandRecordingToken recording_;
    core::u64 upload_bytes_{};
    bool uploaded_{};
};
} // namespace mediaserver::render
