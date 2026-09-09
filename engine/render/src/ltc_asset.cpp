#include <mediaserver/render/ltc_asset.hpp>
#include <array>

namespace mediaserver::render {
namespace {
constexpr std::array<scene::Vec4, ltc_table_entries> tables{{
#include "../../../third_party/ltc/ltc_tables.inc"
}};
}
std::span<const scene::Vec4> ltc_table_data() noexcept { return tables; }
bool LtcAsset::create(rhi::Device& device) {
    auto result = device.create_storage_buffer(static_cast<core::u32>(sizeof(tables)));
    if (!result) return false;
    buffer_ = std::move(result).value();
    return true;
}
bool LtcAsset::prepare(rhi::CommandList& commands, core::u64 generation) noexcept {
    upload_bytes_ = 0;
    if (recording_) {
        const auto outcome = recording_.outcome();
        if (outcome == rhi::CommandRecordingOutcome::Canceled || outcome == rhi::CommandRecordingOutcome::Unavailable)
            uploaded_ = false;
        if (outcome != rhi::CommandRecordingOutcome::Pending) recording_ = {};
    }
    if (uploaded_ && generation_ == generation) return true;
    uploaded_ = false;
    const auto token = commands.recording_token();
    const auto bytes = std::span<const core::u8>{reinterpret_cast<const core::u8*>(tables.data()), sizeof(tables)};
    if (!token || !commands.upload(buffer_, bytes)) return false;
    generation_ = generation;
    recording_ = token;
    uploaded_ = true;
    upload_bytes_ = bytes.size();
    return true;
}
} // namespace mediaserver::render
