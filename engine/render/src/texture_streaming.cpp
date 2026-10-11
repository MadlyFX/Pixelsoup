#include <mediaserver/render/texture_streaming.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
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


namespace {
struct Registered final {
    const void *device{};
    std::weak_ptr<TextureResidency> residency;
};
std::mutex registry_mutex;
std::vector<Registered> registry;

[[nodiscard]] constexpr core::u64 position(core::u64 level, core::u64 row) noexcept {
    return (level << 32U) | row;
}
[[nodiscard]] core::u64 end_position(const TextureResidency::Entry &entry) noexcept {
    return position(entry.asset->textures[entry.index].lower_mips.size() + 1U, 0U);
}
[[nodiscard]] bool is_current(const rhi::CommandRecordingToken &current,
                        const rhi::CommandRecordingToken &token) noexcept {
    return current && current.same_recording(token);
}
} // namespace

core::Result<std::shared_ptr<TextureResidency>, rhi::RhiError>
TextureResidency::for_device(rhi::Device &device) noexcept {
    using Result = core::Result<std::shared_ptr<TextureResidency>, rhi::RhiError>;
    try {
        const std::lock_guard lock{registry_mutex};
        std::erase_if(registry, [](const Registered &held) { return held.residency.expired(); });
        const void *const identity = device.backend_handle();
        for (const auto &held : registry) {
            if (held.device == identity) {
                if (auto shared = held.residency.lock())
                    return Result::success(std::move(shared));
            }
        }
        std::shared_ptr<TextureResidency> created{new TextureResidency{}};
        created->self_ = created;
        registry.push_back({identity, created});
        return Result::success(std::move(created));
    } catch (...) {
        return Result::failure(rhi::RhiError::AllocationFailed);
    }
}

TextureResidency::~TextureResidency() = default;

TextureResidency::Handle TextureResidency::find(const ModelAsset &asset, core::u32 index,
                                                core::u32 first_mip) const noexcept {
    const auto found = entries_.find(Key{&asset, index, first_mip});
    return found == entries_.end() ? Handle{} : found->second.lock();
}

TextureResidency::Handle TextureResidency::adopt(
    const std::shared_ptr<const ModelAsset> &asset, core::u32 index, core::u32 first_mip,
    core::u64 bytes, rhi::Texture texture) {
    if (!asset || index >= asset->textures.size() || !texture)
        return {};
    try {
        Handle entry{new Entry{}, Release{self_}};
        entry->asset = asset;
        entry->index = index;
        entry->first_mip = first_mip;
        entry->bytes = bytes;
        entry->serial = next_serial_++;
        entry->texture = std::move(texture);
        entry->next = position(first_mip, 0U);
        serials_[&entry->texture] = entry->serial;
        entries_[Key{asset.get(), index, first_mip}] = entry;
        resident_bytes_ += bytes;
        return entry;
    } catch (...) {
        return {};
    }
}

core::u64 TextureResidency::serial(const rhi::Texture *texture) const noexcept {
    const auto found = serials_.find(texture);
    return found == serials_.end() ? 0U : found->second;
}

void TextureResidency::Release::operator()(Entry *entry) const noexcept {
    std::unique_ptr<Entry> owned{entry};
    if (const auto residency = owner.lock()) {
        // A renderer may be destroyed on one thread while another records.
        try {
            const std::lock_guard held{residency->mutex_};
            residency->retire(std::move(owned));
        } catch (...) {
            // The lock could not be taken: keep the texture rather than free
            // it under a draw that may still read it.
            static_cast<void>(owned.release());
        }
    }
}

void TextureResidency::retire(std::unique_ptr<Entry> entry) noexcept {
    // The sharing key stays until collect(): a release can run while the key
    // map is being walked, and an expired key already reads as absent.
    if (entry->texture.safe_to_release()) {
        destroy(std::move(entry));
        return;
    }
    try {
        retiring_.push_back(std::move(entry));
    } catch (...) {
        // No room to wait for the device: the texture stays rather than
        // being freed under a draw that may still read it.
        static_cast<void>(entry.release());
    }
}

void TextureResidency::destroy(std::unique_ptr<Entry> entry) noexcept {
    serials_.erase(&entry->texture);
    resident_bytes_ -= std::min(resident_bytes_, entry->bytes);
}

void TextureResidency::collect() noexcept {
    std::erase_if(entries_, [](const auto &held) { return held.second.expired(); });
    for (auto &entry : retiring_) {
        if (entry->texture.safe_to_release())
            destroy(std::move(entry));
    }
    std::erase_if(retiring_, [](const std::unique_ptr<Entry> &entry) { return !entry; });
}

void TextureResidency::reconcile(Entry &entry) noexcept {
    using rhi::CommandRecordingOutcome;
    // A canceled recording never ran: write again from its first row.
    for (std::size_t index = 0U; index < entry.writes.size(); ++index) {
        const auto outcome = entry.writes[index].recording.outcome();
        if (outcome == CommandRecordingOutcome::Canceled ||
            outcome == CommandRecordingOutcome::Unavailable) {
            entry.next = std::min(entry.next, entry.writes[index].begin);
            entry.writes.erase(entry.writes.begin() + static_cast<std::ptrdiff_t>(index),
                               entry.writes.end());
            break;
        }
    }
    while (!entry.writes.empty() &&
           entry.writes.front().recording.outcome() == CommandRecordingOutcome::Submitted)
        entry.writes.erase(entry.writes.begin());
}

bool TextureResidency::drawable(const Entry &entry,
                                const rhi::CommandRecordingToken &current) noexcept {
    return entry.next >= end_position(entry) &&
           std::all_of(entry.writes.begin(), entry.writes.end(), [&](const Entry::Write &write) {
               return write.recording.outcome() == rhi::CommandRecordingOutcome::Submitted ||
                      is_current(current, write.recording);
           });
}

bool TextureResidency::landed(Entry &entry) noexcept {
    reconcile(entry);
    return entry.next >= end_position(entry) && entry.writes.empty();
}

TextureResidency::Streamed TextureResidency::stream(Entry &entry, rhi::CommandList &commands,
                                                    core::u64 &budget) {
    Streamed result{};
    reconcile(entry);
    const auto current = commands.recording_token();
    if (!current || entry.next >= end_position(entry))
        return result;
    // Another unsubmitted recording is writing it: that one finishes it.
    for (const auto &write : entry.writes) {
        if (write.recording.outcome() == rhi::CommandRecordingOutcome::Pending &&
            !is_current(current, write.recording))
            return result;
    }
    // A large level is split into complete rows.
    const auto &source = entry.asset->textures[entry.index];
    while (entry.next < end_position(entry)) {
        const auto mip = static_cast<core::u32>(entry.next >> 32U);
        const auto row = static_cast<core::u32>(entry.next & 0xFFFF'FFFFULL);
        const auto width = std::max(1U, source.width >> mip);
        const auto height = std::max(1U, source.height >> mip);
        const core::u32 block = source.encoding == ModelTextureEncoding::Rgba8 ? 1U : 4U;
        const core::u64 row_bytes = block == 1U ? static_cast<core::u64>(width) * 4U
            : static_cast<core::u64>((width + 3U) / 4U) * 16U;
        const auto block_rows = static_cast<core::u32>(std::min<core::u64>(
            (height - row + block - 1U) / block, budget / row_bytes));
        const auto rows = std::min(height - row, block_rows * block);
        if (rows == 0U)
            break;
        const auto bytes =
            pixels(source, mip)
                .subspan(static_cast<std::size_t>((row / block) * row_bytes),
                         static_cast<std::size_t>(block_rows * row_bytes));
        if (!commands.upload_texture_rows(entry.texture, mip - entry.first_mip, 0U, row, rows,
                                          bytes)) {
            result.uploaded = false;
            return result;
        }
        ++result.uploads;
        result.bytes += bytes.size();
        budget -= bytes.size();
        const auto begin = entry.next;
        entry.next = row + rows == height ? position(mip + 1U, 0U) : position(mip, row + rows);
        if (!entry.writes.empty() && is_current(current, entry.writes.back().recording) &&
            entry.writes.back().end == begin) {
            entry.writes.back().end = entry.next;
        } else {
            entry.writes.push_back({current, begin, entry.next});
        }
    }
    return result;
}

TextureStreamer::TextureStreamer(core::u32 capacity,
                                 std::shared_ptr<TextureResidency> residency)
    : residency_(std::move(residency)) {
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
bool TextureStreamer::landed(const std::vector<core::u32> &indices) noexcept {
    return std::all_of(indices.begin(), indices.end(), [&](auto index) {
        return residency_->landed(*slots_[index].texture);
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
        // Held until the device is done with it, so fence-retired bytes count
        // against this streamer's envelope as its own allocations did.
        if (slot.texture && !slot.referenced && slot.texture->texture.safe_to_release()) {
            slot.texture.reset();
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
    stats_.device_bytes = residency_ ? residency_->resident_bytes() : 0U;
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
    if (!residency_) {
        auto shared = TextureResidency::for_device(device);
        if (!shared) {
            stats_.state = TextureStreamState::AllocationFailed;
            return false;
        }
        residency_ = std::move(shared).value();
    }
    const auto held = residency_->lock();
    residency_->collect();
    recording_ = {};
    // A candidate whose every row has landed, by this streamer's recordings
    // or another's, replaces the active set.
    if (candidate_valid_ && !failed_ && landed(candidate_)) {
        active_.swap(candidate_);
        candidate_.clear();
        candidate_valid_ = false;
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
        // A texture another streamer holds costs this one its bytes all the
        // same: the envelope bounds what a streamer holds, whoever made it.
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
                const auto &asset = selection.request.asset;
                auto entry = residency_->find(*asset, selection.request.texture,
                                              selection.first_mip);
                if (!entry) {
                    const auto &source = asset->textures[selection.request.texture];
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
                    entry = residency_->adopt(asset, selection.request.texture,
                                              selection.first_mip, selection.bytes,
                                              std::move(created).value());
                    if (!entry)
                        return fail(TextureStreamState::AllocationFailed);
                }
                try {
                    found->source = selection.request.source;
                } catch (const std::bad_alloc &) {
                    return fail(TextureStreamState::AllocationFailed);
                }
                found->generation = selection.request.generation;
                found->index = selection.request.texture;
                found->first_mip = selection.first_mip;
                found->bytes = selection.bytes;
                found->texture = std::move(entry);
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
    // geometry, media and uniforms. Rows another streamer wrote earlier in
    // this recording are not written again.
    core::u64 remaining = 16ULL * 1024ULL * 1024ULL;
    recording_ = current;
    for (auto index : candidate_) {
        const auto streamed = residency_->stream(*slots_[index].texture, commands, remaining);
        stats_.uploads += streamed.uploads;
        stats_.upload_bytes += streamed.bytes;
        if (!streamed.uploaded)
            return fail(TextureStreamState::UploadFailed);
    }
    draw_candidate_ = std::all_of(candidate_.begin(), candidate_.end(), [&](auto index) {
        return TextureResidency::drawable(*slots_[index].texture, current);
    });
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
            return &slot.texture->texture;
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
