#include <mediaserver/render/texture_streaming.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace mediaserver::render {
namespace {
bool before(const TextureRequest &a, const TextureRequest &b) noexcept {
    return a.source != b.source ? a.source < b.source : a.texture < b.texture;
}
bool same(const TextureRequest &a, const TextureRequest &b) noexcept {
    return a.source == b.source && a.texture == b.texture;
}
std::span<const core::u8> pixels(const ModelTexture &texture, core::u32 mip) {
    return mip == 0U ? std::span<const core::u8>{texture.rgba}
                     : std::span<const core::u8>{texture.lower_mips[mip - 1U].rgba};
}
} // namespace

core::u32 desired_texture_mip(core::u32 width, core::u32 height, float projected,
                              core::u32 bias) noexcept {
    core::u32 level{};
    core::u32 dimension = std::max(width, height);
    if (dimension == 0U || !std::isfinite(projected))
        return 0U;
    projected = std::max(projected, 1.0F);
    while (dimension > 1U && static_cast<float>(dimension / 2U) >= projected) {
        dimension /= 2U;
        ++level;
    }
    while (bias-- > 0U && dimension > 1U) {
        dimension /= 2U;
        ++level;
    }
    return level;
}

core::u64 texture_mip_bytes(const ModelTexture &texture, core::u32 first) noexcept {
    if (!texture.valid() || first > texture.lower_mips.size())
        return 0U;
    core::u64 total = first == 0U ? texture.rgba.size() : 0U;
    for (std::size_t i = std::max(first, 1U) - 1U; i < texture.lower_mips.size(); ++i)
        total += texture.lower_mips[i].rgba.size();
    return total;
}

bool plan_texture_residency(std::span<const TextureRequest> requests, core::u64 budget,
                            bool streaming, core::u32 bias,
                            std::vector<TextureSelection> &into) noexcept {
    into.clear();
    for (const auto &request : requests) {
        if (!request.asset || request.texture >= request.asset->textures.size())
            return false;
        const auto &texture = request.asset->textures[request.texture];
        if (!texture.valid())
            continue;
        if (into.size() == into.capacity())
            return false;
        const auto levels = static_cast<core::u32>(texture.lower_mips.size() + 1U);
        auto tail = levels > 3U ? levels - 3U : 0U;
        if (texture.encoding != ModelTextureEncoding::Rgba8) {
            // Every possible resident base must remain block aligned. Lower
            // levels inside that resource can use partial blocks normally.
            tail = 0U;
            while (tail + 1U < levels
                && (texture.width >> (tail + 1U)) % 4U == 0U
                && (texture.height >> (tail + 1U)) % 4U == 0U
                && (texture.width >> (tail + 1U)) >= 4U
                && (texture.height >> (tail + 1U)) >= 4U) ++tail;
        }
        // A no-mip glTF sampler names the original base image. Replacing it by
        // a smaller resident base would silently change the authored filter.
        if (texture.requires_base_level) tail = 0U;
        const auto desired =
            streaming
                ? std::min(tail, desired_texture_mip(texture.width, texture.height,
                                                     request.projected_pixels, bias))
                : 0U;
        into.push_back(
            {request, desired, desired, tail, texture_mip_bytes(texture, desired)});
    }
    std::sort(into.begin(), into.end(), [](const auto &a, const auto &b) {
        return before(a.request, b.request);
    });
    // Instances of one source share one texture; the finest request wins.
    std::size_t count{};
    for (std::size_t i = 0U; i < into.size(); ++i) {
        if (count != 0U && same(into[count - 1U].request, into[i].request)) {
            auto &previous = into[count - 1U];
            if (previous.request.generation != into[i].request.generation)
                return false;
            previous.desired_mip = std::min(previous.desired_mip, into[i].desired_mip);
            previous.first_mip = previous.desired_mip;
            previous.bytes = texture_mip_bytes(
                previous.request.asset->textures[previous.request.texture],
                previous.first_mip);
            previous.request.last_desired =
                std::max(previous.request.last_desired, into[i].request.last_desired);
        } else {
            if (i != count)
                into[count] = std::move(into[i]);
            ++count;
        }
    }
    into.resize(count);
    core::u64 total{};
    for (const auto &selection : into) {
        if (total > std::numeric_limits<core::u64>::max() - selection.bytes)
            return false;
        total += selection.bytes;
    }
    if (total <= budget)
        return true;
    if (!streaming)
        return false;
    // Re-sort the same fixed scratch, avoiding a frame allocation for priority.
    std::sort(into.begin(), into.end(), [](const auto &a, const auto &b) {
        return a.request.last_desired != b.request.last_desired
                   ? a.request.last_desired < b.request.last_desired
                   : before(a.request, b.request);
    });
    for (auto &selection : into) {
        const auto &texture =
            selection.request.asset->textures[selection.request.texture];
        while (total > budget && selection.first_mip < selection.tail_mip) {
            total -= selection.bytes;
            selection.bytes = texture_mip_bytes(texture, ++selection.first_mip);
            total += selection.bytes;
        }
    }
    std::sort(into.begin(), into.end(), [](const auto &a, const auto &b) {
        return before(a.request, b.request);
    });
    return total <= budget;
}

TextureStreamer::TextureStreamer(core::u32 capacity) {
    slots_.resize(static_cast<std::size_t>(capacity) * 3U);
    active_.reserve(capacity);
    candidate_.reserve(capacity);
    plan_.reserve(capacity);
}
bool TextureStreamer::matches(const Slot &slot,
                              const TextureSelection &selection) const noexcept {
    return slot.texture && slot.source == selection.request.source &&
           slot.generation == selection.request.generation &&
           slot.index == selection.request.texture &&
           slot.first_mip == selection.first_mip;
}
bool TextureStreamer::complete(bool staged) const noexcept {
    if (!candidate_valid_)
        return false;
    return std::all_of(candidate_.begin(), candidate_.end(), [&](auto index) {
        const auto &slot = slots_[index];
        return (staged ? slot.staged_mip : slot.mip) >
               slot.asset->textures[slot.index].lower_mips.size();
    });
}
void TextureStreamer::reclaim() {
    for (auto &slot : slots_)
        slot.referenced = false;
    for (const auto index : active_)
        slots_[index].referenced = true;
    for (const auto index : candidate_)
        slots_[index].referenced = true;
    for (auto &slot : slots_) {
        if (slot.texture && !slot.referenced && slot.texture.safe_to_release()) {
            slot.texture = {};
            slot.asset.reset();
            slot.bytes = 0U;
            ++stats_.evictions;
        }
    }
}

void TextureStreamer::update_stats() {
    stats_.allocated_bytes = 0U;
    stats_.resident_bytes = 0U;
    for (const auto &slot : slots_)
        stats_.allocated_bytes += slot.bytes;
    const auto &draw = draw_candidate_ ? candidate_ : active_;
    stats_.textures = static_cast<core::u32>(draw.size());
    stats_.reduced = 0U;
    for (auto index : draw) {
        stats_.resident_bytes += slots_[index].bytes;
        if (slots_[index].first_mip > 0U)
            ++stats_.reduced;
    }
}

bool TextureStreamer::prepare(rhi::Device &device, rhi::CommandList &commands,
                              std::span<const TextureRequest> requests,
                              core::u64 total_budget, bool streaming, core::u32 bias) {
    const auto current = commands.recording_token();
    if (!current) {
        stats_.state = TextureStreamState::Busy;
        return false;
    }
    if (recording_ && recording_.outcome() == rhi::CommandRecordingOutcome::Pending) {
        if (!recording_.same_recording(current)) {
            stats_.state = TextureStreamState::Busy;
            return false;
        }
        return !failed_; // Publication/selection is frozen for this recording.
    }
    if (recording_) {
        if (recording_.outcome() == rhi::CommandRecordingOutcome::Submitted) {
            for (auto index : candidate_) {
                auto &slot = slots_[index];
                slot.mip = slot.staged_mip;
                slot.row = slot.staged_row;
            }
            if (!failed_ && complete(false)) {
                active_.swap(candidate_);
                candidate_.clear();
                candidate_valid_ = false;
            }
        } else {
            for (auto index : candidate_) {
                auto &slot = slots_[index];
                slot.staged_mip = slot.mip;
                slot.staged_row = slot.row;
            }
        }
        recording_ = {};
    }
    draw_candidate_ = false;
    stats_.uploads = stats_.evictions = 0U;
    stats_.upload_bytes = 0U;
    stats_.budget_bytes = total_budget;
    // A lower authored budget takes effect after its smaller replacement is
    // complete. Keep only the already-authorized old envelope until then;
    // otherwise an active set larger than the new budget could never shrink.
    allocation_limit_ = std::max(allocation_limit_, total_budget);
    reclaim();
    if (!plan_texture_residency(requests, total_budget / 2U, streaming, bias, plan_)) {
        stats_.state = TextureStreamState::CapacityRefused;
        update_stats();
        return false;
    }
    core::u64 identity = 14695981039346656037ULL;
    const auto mix = [&](core::u64 v) { identity = (identity ^ v) * 1099511628211ULL; };
    mix(total_budget);
    for (const auto &selection : plan_) {
        for (const unsigned char c : selection.request.source)
            mix(c);
        mix(selection.request.generation);
        mix(selection.request.texture);
        mix(selection.first_mip);
    }
    if (failed_identity_ != 0U && failed_identity_ == identity) {
        stats_.state = failure_state_;
        update_stats();
        return false;
    }
    const auto fail = [&](TextureStreamState reason) {
        failed_identity_ = identity;
        failure_state_ = stats_.state = reason;
        failed_ = true;
        update_stats();
        return false;
    };
    const auto same_plan = [&](const std::vector<core::u32> &indices) {
        if (indices.size() != plan_.size())
            return false;
        for (std::size_t i = 0U; i < plan_.size(); ++i)
            if (!matches(slots_[indices[i]], plan_[i]))
                return false;
        return true;
    };
    if (same_plan(active_)) {
        candidate_.clear();
        candidate_valid_ = failed_ = false;
        reclaim();
        stats_.state = TextureStreamState::Ready;
        recording_ = current;
        update_stats();
        if (stats_.allocated_bytes <= total_budget)
            allocation_limit_ = total_budget;
        return true;
    }
    if (!candidate_valid_ || !same_plan(candidate_)) {
        candidate_.clear();
        candidate_valid_ = failed_ = false;
        reclaim();
        update_stats();
        core::u64 additional{};
        for (const auto &selection : plan_) {
            const auto found =
                std::find_if(slots_.begin(), slots_.end(), [&](const auto &slot) {
                    return matches(slot, selection);
                });
            if (found == slots_.end())
                additional += selection.bytes;
        }
        if (additional > allocation_limit_ ||
            stats_.allocated_bytes > allocation_limit_ - additional) {
            stats_.state =
                TextureStreamState::Busy; // Fence headroom, never a coarser local plan.
            return false;
        }
        for (const auto &selection : plan_) {
            auto found =
                std::find_if(slots_.begin(), slots_.end(), [&](const auto &slot) {
                    return matches(slot, selection);
                });
            if (found == slots_.end()) {
                found = std::find_if(slots_.begin(), slots_.end(),
                                     [](const auto &slot) { return !slot.texture; });
                if (found == slots_.end()) {
                    stats_.state = TextureStreamState::CapacityRefused;
                    return false;
                }
                const auto &source =
                    selection.request.asset->textures[selection.request.texture];
                rhi::TextureDesc desc{};
                desc.width = std::max(1U, source.width >> selection.first_mip);
                desc.height = std::max(1U, source.height >> selection.first_mip);
                desc.format = model_texture_format(source);
                desc.sampled = true;
                desc.color_target = false;
                desc.mip_levels =
                    static_cast<core::u32>(source.lower_mips.size() + 1U) -
                    selection.first_mip;
                auto created = device.create_texture(desc);
                if (!created)
                    return fail(TextureStreamState::AllocationFailed);
                try {
                    found->source = selection.request.source;
                } catch (const std::bad_alloc &) {
                    return fail(TextureStreamState::AllocationFailed);
                }
                found->asset = selection.request.asset;
                found->generation = selection.request.generation;
                found->index = selection.request.texture;
                found->first_mip = selection.first_mip;
                found->bytes = selection.bytes;
                found->texture = std::move(created).value();
                found->mip = found->staged_mip = selection.first_mip;
                found->row = found->staged_row = 0U;
            }
            candidate_.push_back(static_cast<core::u32>(found - slots_.begin()));
        }
        candidate_valid_ = true;
    }
    if (failed_) {
        update_stats();
        return false;
    }
    // 16 MiB per recording leaves room in the native 80 MiB command pages for
    // geometry, media and uniforms. A large mip is split into complete rows.
    core::u64 remaining = 16ULL * 1024ULL * 1024ULL;
    recording_ = current;
    for (auto index : candidate_) {
        auto &slot = slots_[index];
        const auto &source = slot.asset->textures[slot.index];
        while (slot.staged_mip <= source.lower_mips.size()) {
            const auto mip = slot.staged_mip;
            const auto width = std::max(1U, source.width >> mip);
            const auto height = std::max(1U, source.height >> mip);
            const core::u32 block = source.encoding == ModelTextureEncoding::Rgba8 ? 1U : 4U;
            const core::u64 row_bytes = block == 1U ? static_cast<core::u64>(width) * 4U
                : static_cast<core::u64>((width + 3U) / 4U) * 16U;
            const auto block_rows = static_cast<core::u32>(std::min<core::u64>(
                (height - slot.staged_row + block - 1U) / block, remaining / row_bytes));
            const auto rows = std::min(height - slot.staged_row, block_rows * block);
            if (rows == 0U)
                break;
            const auto bytes =
                pixels(source, mip)
                    .subspan(static_cast<std::size_t>((slot.staged_row / block) * row_bytes),
                             static_cast<std::size_t>(block_rows * row_bytes));
            if (!commands.upload_texture_rows(slot.texture, mip - slot.first_mip, 0U,
                                              slot.staged_row, rows, bytes)) {
                return fail(TextureStreamState::UploadFailed);
            }
            ++stats_.uploads;
            stats_.upload_bytes += bytes.size();
            remaining -= bytes.size();
            slot.staged_row += rows;
            if (slot.staged_row == height) {
                ++slot.staged_mip;
                slot.staged_row = 0U;
            }
        }
    }
    draw_candidate_ = complete(true);
    stats_.state =
        draw_candidate_ ? TextureStreamState::Ready : TextureStreamState::Loading;
    update_stats();
    return true;
}

const rhi::Texture *TextureStreamer::find(std::string_view source, core::u64 generation,
                                          core::u32 index) const noexcept {
    const auto &draw = draw_candidate_ ? candidate_ : active_;
    const auto found = std::lower_bound(
        draw.begin(), draw.end(), index, [&](core::u32 held, core::u32 image) {
            const auto &slot = slots_[held];
            return slot.source != source ? slot.source < source : slot.index < image;
        });
    if (found != draw.end()) {
        const auto &slot = slots_[*found];
        if (slot.source == source && slot.generation == generation &&
            slot.index == index)
            return &slot.texture;
    }
    return nullptr;
}
bool TextureStreamer::contains(std::string_view source, core::u64 generation,
                               const ModelAsset &asset) const noexcept {
    for (core::u32 i = 0U; i < asset.textures.size(); ++i)
        if (asset.textures[i].valid() && find(source, generation, i) == nullptr)
            return false;
    return true;
}
core::u32 TextureStreamer::first_mip(std::string_view source,
                                     core::u32 index) const noexcept {
    const auto &draw = draw_candidate_ ? candidate_ : active_;
    for (auto held : draw)
        if (slots_[held].source == source && slots_[held].index == index)
            return slots_[held].first_mip;
    return std::numeric_limits<core::u32>::max();
}
} // namespace mediaserver::render
