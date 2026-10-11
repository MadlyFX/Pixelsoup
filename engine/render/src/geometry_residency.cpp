#include <mediaserver/render/geometry_residency.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

namespace mediaserver::render {
namespace {

// scene.vert.hlsl: struct Skin.
struct GpuSkin final {
    std::array<core::u32, 4U> joints{};
    scene::Vec4 weights{1.0F, 0.0F, 0.0F, 0.0F};
};
static_assert(sizeof(GpuSkin) == GeometryArena::skin_bytes);

// Geometry is written in pieces of at most this size, which bounds the
// conversion scratch, and leaves this much of a recording's upload space for
// the per-frame buffers recorded after it.
constexpr std::size_t write_piece_bytes = 4U * 1024U * 1024U;
constexpr core::u64 upload_reserve = 16ULL * 1024ULL * 1024ULL;

struct Registered final {
    const void* device{};
    GeometryArenaLimits limits{};
    std::weak_ptr<GeometryResidency> residency;
};

[[nodiscard]] bool same_limits(const GeometryArenaLimits& a, const GeometryArenaLimits& b) noexcept {
    return a.initial_vertices == b.initial_vertices && a.max_vertices == b.max_vertices
        && a.initial_indices == b.initial_indices && a.max_indices == b.max_indices
        && a.initial_skins == b.initial_skins && a.max_skins == b.max_skins;
}

std::mutex registry_mutex;
std::vector<Registered> registry;

} // namespace

core::u64 GeometryResidency::Entry::total() const noexcept {
    return static_cast<core::u64>(span.vertex_count)
            * (GeometryArena::vertex_bytes + GeometryArena::tangent_bytes)
        + static_cast<core::u64>(span.skin_count) * GeometryArena::skin_bytes
        + static_cast<core::u64>(span.index_count) * GeometryArena::index_bytes;
}

GeometryResidency::GeometryResidency(GeometryArena arena) noexcept
    : arena_(std::move(arena)) {}

GeometryResidency::~GeometryResidency() = default;

core::Result<std::shared_ptr<GeometryResidency>, rhi::RhiError>
GeometryResidency::for_device(rhi::Device& device, const GeometryArenaLimits& limits) noexcept {
    using Result = core::Result<std::shared_ptr<GeometryResidency>, rhi::RhiError>;
    try {
        const std::lock_guard lock{registry_mutex};
        std::erase_if(registry, [](const Registered& held) { return held.residency.expired(); });
        const void* const identity = device.backend_handle();
        for (const auto& held : registry) {
            if (held.device == identity && same_limits(held.limits, limits)) {
                if (auto shared = held.residency.lock()) {
                    return Result::success(std::move(shared));
                }
            }
        }
        auto arena = GeometryArena::create(device, limits);
        if (!arena) {
            return Result::failure(arena.error());
        }
        std::shared_ptr<GeometryResidency> created{
            new GeometryResidency{std::move(arena).value()}};
        created->self_ = created;
        created->scratch_.reserve(write_piece_bytes);
        registry.push_back({identity, limits, created});
        return Result::success(std::move(created));
    } catch (...) {
        return Result::failure(rhi::RhiError::AllocationFailed);
    }
}

std::optional<GeometrySpan> GeometryResidency::reserve(
    rhi::CommandList& commands, core::u32 vertices, core::u32 indices,
    core::u32 skins, bool& grew) {
    const auto before = arena_.generation();
    auto span = arena_.allocate(commands, vertices, indices, skins);
    if (arena_.generation() != before) {
        // The arena moved to new buffers in this recording, after copying
        // their contents. A write still pending in another recording targets
        // a replaced buffer and may run after that copy, so it is made again.
        grew = true;
        const auto current = commands.recording_token();
        const auto invalidate = [&](Entry& entry) noexcept {
            for (auto& write : entry.writes) {
                if (write.recording.outcome() == rhi::CommandRecordingOutcome::Pending
                    && !(current && current.same_recording(write.recording))) {
                    write.invalid = true;
                }
            }
        };
        for (auto& [key, entry] : procedural_) {
            static_cast<void>(key);
            invalidate(*entry);
        }
        for (auto& [asset, held] : models_) {
            static_cast<void>(asset);
            if (const auto entry = held.lock()) {
                invalidate(*entry);
            }
        }
    }
    return span;
}

GeometryResidency::Entry* GeometryResidency::procedural(
    scene::MeshKey key, rhi::CommandList& commands, bool& grew) {
    const auto found = procedural_.find(key);
    if (found != procedural_.end()) {
        return found->second.get();
    }
    if (!scene::build_mesh(key, mesh_scratch_)) {
        return nullptr;
    }
    const auto span = reserve(
        commands, static_cast<core::u32>(mesh_scratch_.vertices.size()),
        static_cast<core::u32>(mesh_scratch_.indices.size()), 0U, grew);
    if (!span) {
        return nullptr;
    }
    try {
        auto entry = std::make_unique<Entry>();
        entry->span = *span;
        auto owned = std::make_shared<const scene::MeshData>(mesh_scratch_);
        entry->mesh = owned.get();
        entry->keepalive = std::move(owned);
        return procedural_.emplace(key, std::move(entry)).first->second.get();
    } catch (...) {
        arena_.release(*span);
        return nullptr;
    }
}

GeometryResidency::Handle GeometryResidency::find(const ModelAsset& asset) const noexcept {
    const auto found = models_.find(&asset);
    return found == models_.end() ? Handle{} : found->second.lock();
}

GeometryResidency::Handle GeometryResidency::allocate(
    const std::shared_ptr<const ModelAsset>& asset, rhi::CommandList& commands, bool& grew) {
    if (!asset) {
        return {};
    }
    const auto vertex_count = static_cast<core::u32>(asset->mesh.vertices.size());
    const auto index_count = static_cast<core::u32>(asset->mesh.indices.size());
    // Skin records only for meshes with deformation bindings: the vertex
    // shader reads them only for instances that carry a pose.
    const auto skin_count = asset->pose_bindings.empty() ? 0U : vertex_count;
    const auto span = reserve(commands, vertex_count, index_count, skin_count, grew);
    if (!span) {
        return {};
    }
    try {
        Handle entry{new Entry{}, Release{self_}};
        entry->span = *span;
        entry->keepalive = asset;
        entry->mesh = &asset->mesh;
        entry->metadata = asset->vertex_metadata;
        entry->model = asset;
        models_[asset.get()] = entry;
        return entry;
    } catch (...) {
        arena_.release(*span);
        return {};
    }
}

bool GeometryResidency::in_flight(const Entry& entry) noexcept {
    const auto pending = [](const rhi::CommandRecordingToken& token) {
        return token.outcome() == rhi::CommandRecordingOutcome::Pending;
    };
    return entry.untracked_use
        || std::any_of(entry.uses.begin(), entry.uses.end(), pending)
        || std::any_of(entry.writes.begin(), entry.writes.end(),
               [&](const Entry::Write& write) { return pending(write.recording); });
}

void GeometryResidency::Release::operator()(Entry* entry) const noexcept {
    std::unique_ptr<Entry> owned{entry};
    if (const auto residency = owner.lock()) {
        // A renderer may be destroyed on one thread while another records.
        try {
            const std::lock_guard held{residency->mutex_};
            residency->retire(std::move(owned));
        } catch (...) {
            // The lock could not be taken: keep the slots rather than reuse
            // them unguarded.
            static_cast<void>(owned.release());
        }
    }
}

void GeometryResidency::retire(std::unique_ptr<Entry> entry) noexcept {
    // The sharing key stays until collect(): a release can run while the
    // key map is being walked, and an expired key already reads as absent.
    if (!in_flight(*entry)) {
        arena_.release(entry->span);
        return;
    }
    try {
        retiring_.push_back(std::move(entry));
    } catch (...) {
        // No room to wait for its recordings: the slots stay unusable rather
        // than being reused under a draw that may still read them.
    }
}

void GeometryResidency::collect() noexcept {
    std::erase_if(models_, [](const auto& held) { return held.second.expired(); });
    std::erase_if(retiring_, [&](const std::unique_ptr<Entry>& entry) {
        if (in_flight(*entry)) {
            return false;
        }
        arena_.release(entry->span);
        return true;
    });
    arena_.collect();
    std::erase_if(shadows_, [](const auto& held) { return held.expired(); });
    std::erase_if(retiring_shadows_, [](const std::unique_ptr<ShadowProduct>& product) {
        return shadow_releasable(*product);
    });
}

GeometryResidency::ShadowHandle GeometryResidency::adopt_shadow(
    std::array<rhi::Texture, 2U> textures) {
    try {
        ShadowHandle product{new ShadowProduct{}, ReleaseShadow{self_}};
        product->textures = std::move(textures);
        shadows_.push_back(product);
        return product;
    } catch (...) {
        return {};
    }
}

bool GeometryResidency::shadow_current(
    ShadowProduct& product, core::u64 key,
    const rhi::CommandRecordingToken& current) noexcept {
    if (product.recording) {
        switch (product.recording.outcome()) {
        case rhi::CommandRecordingOutcome::Submitted:
            product.submitted = true;
            product.recording = {};
            break;
        case rhi::CommandRecordingOutcome::Canceled:
        case rhi::CommandRecordingOutcome::Unavailable:
            product.key = 0U;
            product.recording = {};
            product.submitted = false;
            break;
        case rhi::CommandRecordingOutcome::Pending:
            break;
        }
    }
    if (key == 0U || product.key != key || !current) {
        return false;
    }
    return product.submitted
        || (product.recording && current.same_recording(product.recording));
}

GeometryResidency::ShadowHandle GeometryResidency::find_shadow(
    core::u64 key, const rhi::CommandRecordingToken& current) noexcept {
    for (const auto& held : shadows_) {
        if (auto product = held.lock(); product && shadow_current(*product, key, current)) {
            return product;
        }
    }
    return {};
}

bool GeometryResidency::shadow_releasable(const ShadowProduct& product) noexcept {
    return std::all_of(product.textures.begin(), product.textures.end(),
        [](const rhi::Texture& texture) { return texture.safe_to_release(); });
}

void GeometryResidency::ReleaseShadow::operator()(ShadowProduct* product) const noexcept {
    std::unique_ptr<ShadowProduct> owned{product};
    if (shadow_releasable(*owned)) {
        return;
    }
    if (const auto residency = owner.lock()) {
        try {
            const std::lock_guard held{residency->mutex_};
            residency->retiring_shadows_.push_back(std::move(owned));
        } catch (...) {
            // Nowhere to wait for the device: keep the textures rather than
            // free them under a draw that may still sample them.
            static_cast<void>(owned.release());
        }
    }
}

void GeometryResidency::note_use(Entry& entry, rhi::CommandList& commands) {
    const auto current = commands.recording_token();
    if (!current) {
        // Without a token this use cannot be followed; such an entry is never
        // reclaimed rather than reclaimed early.
        entry.untracked_use = true;
        return;
    }
    std::erase_if(entry.uses, [](const rhi::CommandRecordingToken& token) {
        return token.outcome() != rhi::CommandRecordingOutcome::Pending;
    });
    for (const auto& token : entry.uses) {
        if (token.same_recording(current)) {
            return;
        }
    }
    entry.uses.push_back(current);
}

GeometryResidency::Stream GeometryResidency::stream(
    Entry& entry, rhi::CommandList& commands, bool complete_now, core::u64 budget) {
    using rhi::CommandRecordingOutcome;
    Stream result{};
    const auto current = commands.recording_token();
    const auto is_current = [&](const rhi::CommandRecordingToken& token) noexcept {
        return current && current.same_recording(token);
    };
    // A canceled recording never ran, and an invalidated write went to a
    // buffer the arena has since replaced: write again from there.
    for (std::size_t index = 0U; index < entry.writes.size(); ++index) {
        const auto& write = entry.writes[index];
        const auto outcome = write.recording.outcome();
        if (write.invalid || outcome == CommandRecordingOutcome::Canceled
            || outcome == CommandRecordingOutcome::Unavailable) {
            entry.next = std::min(entry.next, write.begin);
            entry.writes.erase(
                entry.writes.begin() + static_cast<std::ptrdiff_t>(index),
                entry.writes.end());
            break;
        }
    }
    while (!entry.writes.empty()
           && entry.writes.front().recording.outcome() == CommandRecordingOutcome::Submitted) {
        entry.writes.erase(entry.writes.begin());
    }
    const auto total = entry.total();
    if (entry.next >= total && entry.writes.empty()) {
        // Everything has landed: the source bytes are no longer needed.
        entry.keepalive.reset();
        entry.mesh = nullptr;
        entry.metadata = {};
        result.drawable = true;
        return result;
    }
    if (entry.next < total) {
        if (!current || entry.mesh == nullptr) {
            return result;
        }
        const auto space = commands.upload_space();
        core::u64 room = space > upload_reserve ? space - upload_reserve : 0U;
        if (complete_now) {
            if (room < total - entry.next) {
                return result;
            }
        } else {
            room = std::min(room, budget);
        }
        const auto& span = entry.span;
        const auto& mesh = *entry.mesh;
        const core::u64 vertex_end =
            static_cast<core::u64>(span.vertex_count) * GeometryArena::vertex_bytes;
        const core::u64 tangent_end = vertex_end
            + static_cast<core::u64>(span.vertex_count) * GeometryArena::tangent_bytes;
        const core::u64 skin_end = tangent_end
            + static_cast<core::u64>(span.skin_count) * GeometryArena::skin_bytes;
        const bool metadata_parallel = entry.metadata.size() == mesh.vertices.size();
        core::u64 written{};
        while (entry.next < total) {
            GeometryStream stream = GeometryStream::Indices;
            core::u64 stream_begin = skin_end;
            core::u32 first = span.first_index;
            core::u32 count = span.index_count;
            if (entry.next < vertex_end) {
                stream = GeometryStream::Vertices;
                stream_begin = 0U;
                first = span.first_vertex;
                count = span.vertex_count;
            } else if (entry.next < tangent_end) {
                stream = GeometryStream::Tangents;
                stream_begin = vertex_end;
                first = span.first_vertex;
                count = span.vertex_count;
            } else if (entry.next < skin_end) {
                stream = GeometryStream::Skins;
                stream_begin = tangent_end;
                first = span.first_skin;
                count = span.skin_count;
            }
            const core::u32 size = GeometryArena::element_bytes(stream);
            const auto element = static_cast<core::u32>((entry.next - stream_begin) / size);
            const core::u64 allowed = std::min<core::u64>(room - written, write_piece_bytes);
            const auto elements = static_cast<core::u32>(
                std::min<core::u64>(count - element, allowed / size));
            if (elements == 0U) {
                break;
            }
            const std::size_t bytes = static_cast<std::size_t>(elements) * size;
            std::span<const core::u8> data;
            switch (stream) {
            case GeometryStream::Vertices:
                data = {reinterpret_cast<const core::u8*>(mesh.vertices.data() + element), bytes};
                break;
            case GeometryStream::Tangents: {
                scratch_.resize(bytes);
                auto* const out = reinterpret_cast<scene::Vec4*>(scratch_.data());
                for (core::u32 offset = 0U; offset < elements; ++offset) {
                    const auto vertex = static_cast<std::size_t>(element) + offset;
                    if (metadata_parallel && entry.metadata[vertex].has_tangent) {
                        out[offset] = entry.metadata[vertex].tangent;
                        continue;
                    }
                    // Procedural geometry and material parts without a normal
                    // map do not consume this basis, but every vertex still
                    // needs a valid record in the pulled stream. Choose a
                    // deterministic axis perpendicular to the geometric
                    // normal rather than leaving NaNs for a future map.
                    const auto normal = mesh.vertices[vertex].normal;
                    const auto axis = std::abs(normal.y) < 0.999F
                        ? scene::Vec3{0.0F, 1.0F, 0.0F}
                        : scene::Vec3{1.0F, 0.0F, 0.0F};
                    const auto tangent = scene::normalize(
                        scene::cross(axis, normal), scene::Vec3{1.0F, 0.0F, 0.0F});
                    out[offset] = scene::Vec4{tangent.x, tangent.y, tangent.z, 1.0F};
                }
                data = scratch_;
                break;
            }
            case GeometryStream::Skins: {
                scratch_.resize(bytes);
                for (core::u32 offset = 0U; offset < elements; ++offset) {
                    const auto vertex = static_cast<std::size_t>(element) + offset;
                    GpuSkin skin{};
                    if (metadata_parallel) {
                        skin.joints = entry.metadata[vertex].joints;
                        skin.weights = entry.metadata[vertex].weights;
                    }
                    std::memcpy(scratch_.data() + static_cast<std::size_t>(offset) * size,
                                &skin, sizeof(skin));
                }
                data = scratch_;
                break;
            }
            case GeometryStream::Indices: {
                scratch_.resize(bytes);
                auto* const out = reinterpret_cast<core::u32*>(scratch_.data());
                for (core::u32 offset = 0U; offset < elements; ++offset) {
                    // Indices are made absolute on the way in, so a batch is a
                    // range in the index arena and nothing else - no
                    // base-vertex argument, and no per-backend disagreement
                    // about whether one is applied.
                    out[offset] = span.first_vertex
                        + mesh.indices[static_cast<std::size_t>(element) + offset];
                }
                data = scratch_;
                break;
            }
            }
            if (!arena_.write(commands, stream, first + element, data)) {
                break;
            }
            const auto begin = entry.next;
            entry.next += bytes;
            written += bytes;
            if (!entry.writes.empty() && is_current(entry.writes.back().recording)
                && !entry.writes.back().invalid && entry.writes.back().end == begin) {
                entry.writes.back().end = entry.next;
            } else {
                entry.writes.push_back({current, begin, entry.next, false});
            }
        }
        if (written != 0U) {
            ++epoch_;
            result.written = written;
        }
    }
    if (entry.next < total) {
        return result;
    }
    // Drawable here only when no byte waits on another, unsubmitted recording.
    result.drawable = std::all_of(entry.writes.begin(), entry.writes.end(),
        [&](const Entry::Write& write) {
            return write.recording.outcome() == CommandRecordingOutcome::Submitted
                || is_current(write.recording);
        });
    return result;
}

} // namespace mediaserver::render
