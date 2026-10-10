#include <mediaserver/render/geometry_arena.hpp>

#include <algorithm>
#include <limits>

namespace mediaserver::render {

RangeAllocator::RangeAllocator(core::u32 capacity, std::size_t max_holes)
    : max_holes_(std::max<std::size_t>(max_holes, 1U)), capacity_(capacity) {
    holes_.reserve(max_holes_);
    if (capacity != 0U) {
        holes_.push_back({0U, capacity});
    }
}

std::optional<core::u32> RangeAllocator::allocate(core::u32 count) noexcept {
    if (count == 0U) {
        return std::nullopt;
    }
    for (auto hole = holes_.begin(); hole != holes_.end(); ++hole) {
        if (hole->count < count) {
            continue;
        }
        const auto offset = hole->offset;
        hole->offset += count;
        hole->count -= count;
        if (hole->count == 0U) {
            holes_.erase(hole);
        }
        used_ += count;
        return offset;
    }
    return std::nullopt;
}

void RangeAllocator::release(core::u32 offset, core::u32 count) noexcept {
    if (count == 0U || offset > capacity_ || count > capacity_ - offset) {
        return;
    }
    used_ -= std::min(used_, count);
    const auto next = std::lower_bound(
        holes_.begin(), holes_.end(), offset,
        [](const Hole& hole, core::u32 wanted) noexcept { return hole.offset < wanted; });
    const bool joins_previous = next != holes_.begin()
        && std::prev(next)->offset + std::prev(next)->count == offset;
    const bool joins_next = next != holes_.end() && offset + count == next->offset;
    if (joins_previous && joins_next) {
        std::prev(next)->count += count + next->count;
        holes_.erase(next);
    } else if (joins_previous) {
        std::prev(next)->count += count;
    } else if (joins_next) {
        next->offset = offset;
        next->count += count;
    } else if (holes_.size() < max_holes_) {
        holes_.insert(next, Hole{offset, count});
    } else {
        // Out of hole records: the slots stay unusable instead of corrupting
        // the list. Counted as used so the arena reports them honestly.
        used_ += count;
    }
}

void RangeAllocator::grow(core::u32 capacity) noexcept {
    if (capacity <= capacity_) {
        return;
    }
    const auto added = capacity - capacity_;
    if (!holes_.empty() && holes_.back().offset + holes_.back().count == capacity_) {
        holes_.back().count += added;
    } else if (holes_.size() < max_holes_) {
        holes_.push_back({capacity_, added});
    } else {
        used_ += added;
    }
    capacity_ = capacity;
}

core::u32 RangeAllocator::high_water() const noexcept {
    if (!holes_.empty() && holes_.back().offset + holes_.back().count == capacity_) {
        return holes_.back().offset;
    }
    return capacity_;
}

core::u32 RangeAllocator::largest_hole() const noexcept {
    core::u32 largest{};
    for (const auto& hole : holes_) {
        largest = std::max(largest, hole.count);
    }
    return largest;
}

core::u32 GeometryArena::element_bytes(GeometryStream stream) noexcept {
    switch (stream) {
    case GeometryStream::Vertices: return vertex_bytes;
    case GeometryStream::Tangents: return tangent_bytes;
    case GeometryStream::Skins: return skin_bytes;
    case GeometryStream::Indices: return index_bytes;
    }
    return 0U;
}

core::Result<GeometryArena, rhi::RhiError> GeometryArena::create(
    rhi::Device& device, GeometryArenaLimits limits) noexcept {
    using Result = core::Result<GeometryArena, rhi::RhiError>;
    // Every element count must also be a valid byte size for the RHI.
    const auto fits = [](core::u32 count, core::u32 bytes) noexcept {
        return count != 0U && count <= std::numeric_limits<core::u32>::max() / bytes;
    };
    if (!fits(limits.max_vertices, vertex_bytes) || !fits(limits.max_indices, index_bytes)
        || !fits(limits.max_skins, skin_bytes)) {
        return Result::failure(rhi::RhiError::InvalidConfiguration);
    }
    limits.initial_vertices = std::clamp(limits.initial_vertices, 1U, limits.max_vertices);
    limits.initial_indices = std::clamp(limits.initial_indices, 1U, limits.max_indices);
    limits.initial_skins = std::clamp(limits.initial_skins, 1U, limits.max_skins);
    GeometryArena arena;
    arena.device_ = &device;
    arena.limits_ = limits;
    const std::array<core::u32, 4U> counts{
        limits.initial_vertices, limits.initial_vertices, limits.initial_skins, limits.initial_indices};
    for (std::size_t index = 0U; index < counts.size(); ++index) {
        auto created = device.create_geometry_buffer(
            counts[index] * element_bytes(static_cast<GeometryStream>(index)));
        if (!created) {
            return Result::failure(created.error());
        }
        arena.streams_[index] = std::move(created).value();
    }
    try {
        arena.vertices_ = RangeAllocator{limits.initial_vertices};
        arena.indices_ = RangeAllocator{limits.initial_indices};
        arena.skins_ = RangeAllocator{limits.initial_skins};
        // Growth retires at most one buffer per stream per growth; this many
        // outstanding replacements covers back-to-back growth across frames.
        arena.retired_.reserve(64U);
    } catch (...) {
        return Result::failure(rhi::RhiError::AllocationFailed);
    }
    return Result::success(std::move(arena));
}

bool GeometryArena::grow(
    rhi::CommandList& commands, RangeAllocator& allocator,
    std::span<const GeometryStream> streams, core::u32 needed,
    core::u32 largest) noexcept {
    const auto current = allocator.capacity();
    // The new end must leave a hole of `needed` slots after the data in use.
    const auto high = allocator.high_water();
    if (needed > largest || high > largest - needed) {
        return false;
    }
    const core::u32 required = high + needed;
    const core::u32 doubled = current > largest / 2U ? largest : current * 2U;
    const core::u32 capacity = std::min(largest, std::max(required, doubled));
    if (capacity <= current || retired_.size() + streams.size() > retired_.capacity()) {
        return false;
    }
    std::array<rhi::StorageBuffer, 2U> grown{};
    for (std::size_t index = 0U; index < streams.size(); ++index) {
        auto created = device_->create_geometry_buffer(capacity * element_bytes(streams[index]));
        if (!created) {
            return false;
        }
        grown[index] = std::move(created).value();
    }
    for (std::size_t index = 0U; index < streams.size(); ++index) {
        auto& old = buffer(streams[index]);
        if (!commands.copy_buffer(old, grown[index], high * element_bytes(streams[index]))) {
            return false;
        }
    }
    for (std::size_t index = 0U; index < streams.size(); ++index) {
        retired_.push_back(std::exchange(buffer(streams[index]), std::move(grown[index])));
    }
    allocator.grow(capacity);
    ++generation_;
    return true;
}

std::optional<GeometrySpan> GeometryArena::allocate(
    rhi::CommandList& commands,
    core::u32 vertices, core::u32 indices, core::u32 skins) noexcept {
    if (device_ == nullptr) {
        return std::nullopt;
    }
    static constexpr std::array vertex_streams{GeometryStream::Vertices, GeometryStream::Tangents};
    static constexpr std::array index_streams{GeometryStream::Indices};
    static constexpr std::array skin_streams{GeometryStream::Skins};
    const auto reserve = [&](RangeAllocator& allocator, std::span<const GeometryStream> streams,
                             core::u32 count, core::u32 largest) -> std::optional<core::u32> {
        if (count == 0U) {
            return 0U;
        }
        if (auto offset = allocator.allocate(count)) {
            return offset;
        }
        if (!grow(commands, allocator, streams, count, largest)) {
            return std::nullopt;
        }
        return allocator.allocate(count);
    };
    GeometrySpan span{};
    const auto vertex = reserve(vertices_, vertex_streams, vertices, limits_.max_vertices);
    if (!vertex) {
        return std::nullopt;
    }
    span.first_vertex = *vertex;
    span.vertex_count = vertices;
    const auto index = reserve(indices_, index_streams, indices, limits_.max_indices);
    if (!index) {
        release(span);
        return std::nullopt;
    }
    span.first_index = *index;
    span.index_count = indices;
    const auto skin = reserve(skins_, skin_streams, skins, limits_.max_skins);
    if (!skin) {
        release(span);
        return std::nullopt;
    }
    span.first_skin = *skin;
    span.skin_count = skins;
    return span;
}

void GeometryArena::release(const GeometrySpan& span) noexcept {
    vertices_.release(span.first_vertex, span.vertex_count);
    indices_.release(span.first_index, span.index_count);
    skins_.release(span.first_skin, span.skin_count);
}

bool GeometryArena::write(
    rhi::CommandList& commands, GeometryStream stream,
    core::u32 first, std::span<const core::u8> bytes) noexcept {
    const auto size = element_bytes(stream);
    auto& target = buffer(stream);
    if (!target || bytes.size() % size != 0U
        || first > std::numeric_limits<core::u32>::max() / size) {
        return false;
    }
    return commands.upload_range(target, first * size, bytes);
}

void GeometryArena::collect() noexcept {
    std::erase_if(retired_, [](const rhi::StorageBuffer& buffer) noexcept {
        return buffer.safe_to_release();
    });
}

core::u64 GeometryArena::used_bytes() const noexcept {
    return static_cast<core::u64>(vertices_.used()) * (vertex_bytes + tangent_bytes)
        + static_cast<core::u64>(indices_.used()) * index_bytes
        + static_cast<core::u64>(skins_.used()) * skin_bytes;
}

core::u64 GeometryArena::capacity_bytes() const noexcept {
    return static_cast<core::u64>(vertices_.capacity()) * (vertex_bytes + tangent_bytes)
        + static_cast<core::u64>(indices_.capacity()) * index_bytes
        + static_cast<core::u64>(skins_.capacity()) * skin_bytes;
}

} // namespace mediaserver::render
