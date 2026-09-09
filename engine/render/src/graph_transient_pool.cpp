#include <mediaserver/render/graph_transient_pool.hpp>

#include <algorithm>

namespace mediaserver::render {

bool GraphTransientPool::adopt(rhi::Texture&& texture) noexcept {
    if (sealed_ || !texture || count_ >= std::min(limits_.max_textures, capacity)) {
        ++stats_.refusals;
        return false;
    }
    auto& entry = entries_[count_++];
    entry.desc = texture.desc();
    entry.texture = std::move(texture);
    stats_.textures = count_;
    return true;
}

void GraphTransientPool::begin_frame() noexcept {
    for (std::size_t index = 0U; index < count_; ++index) {
        entries_[index].in_use = false;
    }
    stats_.acquired = 0U;
    stats_.refusals = 0U;
    stats_.reuses = 0U;
}

core::Result<rhi::Texture*, RenderError> GraphTransientPool::acquire(
    rhi::Device& device, const rhi::TextureDesc& desc) noexcept {
    using Result = core::Result<rhi::Texture*, RenderError>;
    if (desc.width == 0U || desc.height == 0U) {
        ++stats_.refusals;
        return Result::failure(RenderError::InvalidConfiguration);
    }
    for (std::size_t index = 0U; index < count_; ++index) {
        Entry& entry = entries_[index];
        if (!entry.in_use && entry.desc == desc) {
            entry.in_use = true;
            ++stats_.acquired;
            ++stats_.reuses;
            return Result::success(&entry.texture);
        }
    }
    // Nothing free matches, so this frame needs one more than it has had
    // before. Bounded by the authored limit and by the fixed array behind it -
    // past either, refuse and count it. A pool that grew on demand would put an
    // allocation on the frame path every time a show got slightly busier.
    if (sealed_ || count_ >= std::min(limits_.max_textures, capacity)) {
        ++stats_.refusals;
        return Result::failure(RenderError::AllocationFailed);
    }
    auto created = device.create_texture(desc);
    if (!created) {
        ++stats_.refusals;
        return Result::failure(
            created.error() == rhi::RhiError::InvalidConfiguration
                ? RenderError::InvalidConfiguration
                : RenderError::AllocationFailed);
    }
    Entry& entry = entries_[count_];
    entry.texture = std::move(created).value();
    entry.desc = desc;
    entry.in_use = true;
    ++count_;
    stats_.textures = count_;
    ++stats_.acquired;
    return Result::success(&entry.texture);
}

} // namespace mediaserver::render
