#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/geometry_arena.hpp>
#include <mediaserver/render/model_loader.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/geometry.hpp>

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace mediaserver::render {

// Mesh geometry resident on one device, shared by every SceneRenderer created
// on it with the same arena limits. The editor alone holds a dozen renderers -
// Program, its transition target and the stage view, each with Basic and
// Advanced halves and a planar-reflection child - and a detailed scene is
// gigabytes of vertices and indices. Here a model generation or procedural
// mesh is written into one arena once, however many renderers draw it, and
// its slots go back to the arena only when no renderer holds it and no
// recording that read or wrote it is still pending.
//
// Every owner records its renderers into a frame's command list in turn on one
// thread. A renderer holds `lock()` for a whole recording, so renderers that
// share a residency from different threads take turns rather than race.
class GeometryResidency final {
public:
    // One mesh's slots and the progress of writing them. Pieces are written
    // across recordings, each landing once its recording is submitted; a
    // canceled piece, or one a pending recording wrote into buffers the arena
    // has since replaced, is written again. A mesh is drawable in a recording
    // once every byte has landed or was written earlier in that recording.
    struct Entry final {
        struct Write final {
            rhi::CommandRecordingToken recording;
            core::u64 begin{};
            core::u64 end{};
            bool invalid{};
        };
        GeometrySpan span{};
        // Owns `mesh` and `metadata` until every byte has landed.
        std::shared_ptr<const void> keepalive;
        const scene::MeshData* mesh{};
        std::span<const ModelVertexMetadata> metadata;
        core::u64 next{};
        std::vector<Write> writes;
        // Recordings, from any renderer, that drew this mesh.
        std::vector<rhi::CommandRecordingToken> uses;
        bool untracked_use{};
        // The decoded model this entry holds, and its sharing key. Kept for
        // the entry's whole life so the key's address cannot be reused by
        // another model while this geometry is resident. Null for a
        // procedural mesh.
        std::shared_ptr<const ModelAsset> model;

        [[nodiscard]] core::u64 total() const noexcept;
    };
    // A renderer's hold on a model's geometry. The last one dropped hands
    // the slots back once every recording that touched them has finished.
    using Handle = std::shared_ptr<Entry>;

    struct Stream final {
        // True when the mesh may be drawn in this recording.
        bool drawable{};
        core::u64 written{};
    };

    // The residency every renderer on `device` with these limits shares,
    // created by the first one. It lives as long as any renderer holds it.
    [[nodiscard]] static core::Result<std::shared_ptr<GeometryResidency>, rhi::RhiError>
    for_device(rhi::Device& device, const GeometryArenaLimits& limits) noexcept;

    GeometryResidency(const GeometryResidency&) = delete;
    GeometryResidency& operator=(const GeometryResidency&) = delete;
    ~GeometryResidency();

    // Held for a renderer's whole recording; re-entrant, for the planar
    // reflection a renderer records inside its own.
    [[nodiscard]] std::unique_lock<std::recursive_mutex> lock() {
        return std::unique_lock{mutex_};
    }

    [[nodiscard]] rhi::StorageBuffer& buffer(GeometryStream stream) noexcept {
        return arena_.buffer(stream);
    }
    [[nodiscard]] const GeometryArena& arena() const noexcept { return arena_; }
    // Advanced by every write, so a product drawn from this geometry can
    // tell that the bytes it read may have changed.
    [[nodiscard]] core::u64 epoch() const noexcept { return epoch_; }

    // The resident procedural mesh for a normalized key, allocated on first
    // use and kept for the residency's life. Null when it cannot be built or
    // the arena has no room. `grew` reports an arena growth.
    [[nodiscard]] Entry* procedural(
        scene::MeshKey key, rhi::CommandList& commands, bool& grew);
    [[nodiscard]] std::size_t procedural_count() const noexcept {
        return procedural_.size();
    }
    // The entry another renderer already made for this decoded model.
    [[nodiscard]] Handle find(const ModelAsset& asset) const noexcept;
    // Slots for a decoded model's mesh, registered so later renderers share
    // them. Empty when the largest arena cannot hold it.
    [[nodiscard]] Handle allocate(
        const std::shared_ptr<const ModelAsset>& asset,
        rhi::CommandList& commands, bool& grew);
    // Reconciles the entry's earlier writes and writes what this recording
    // can carry: up to `budget` bytes, or, for `complete_now`, all of it.
    [[nodiscard]] Stream stream(
        Entry& entry, rhi::CommandList& commands, bool complete_now,
        core::u64 budget);
    // Notes that this recording draws the entry.
    void note_use(Entry& entry, rhi::CommandList& commands);
    // Returns slots of released entries whose recordings have finished, frees
    // buffers the arena replaced when it grew and shadow products the device
    // is done with.
    void collect() noexcept;

    // A shadow map, or the reflective-shadow-map pair, drawn from this
    // geometry. Every renderer allocates its own at construction and is the
    // only one that draws into them; any renderer whose draw would read
    // exactly what this one read - `key` digests all of it - samples it
    // instead of drawing its own. The full-scene cascade and the reflective
    // shadow map depend on no camera, and the nearer cascades on the authored
    // camera alone, so the views of one scene draw the same maps.
    //
    // Scoped to one arena because the key names geometry by its arena ranges.
    struct ShadowProduct final {
        std::array<rhi::Texture, 2U> textures;
        // Zero while nothing usable is drawn.
        core::u64 key{};
        rhi::CommandRecordingToken recording;
        bool submitted{};
    };
    using ShadowHandle = std::shared_ptr<ShadowProduct>;
    // Registers a renderer's own product. Released once no renderer holds it
    // and the device is done with its textures. Empty when it cannot be.
    [[nodiscard]] ShadowHandle adopt_shadow(std::array<rhi::Texture, 2U> textures);
    // True when the product holds `key` and was drawn by a recording that was
    // submitted or is `current`. Lets go of the receipt once it is submitted,
    // and of the key when the recording was canceled.
    [[nodiscard]] static bool shadow_current(
        ShadowProduct& product, core::u64 key,
        const rhi::CommandRecordingToken& current) noexcept;
    // A product holding `key` that `current` may sample.
    [[nodiscard]] ShadowHandle find_shadow(
        core::u64 key, const rhi::CommandRecordingToken& current) noexcept;

private:
    struct Release final {
        std::weak_ptr<GeometryResidency> owner;
        void operator()(Entry* entry) const noexcept;
    };
    struct ReleaseShadow final {
        std::weak_ptr<GeometryResidency> owner;
        void operator()(ShadowProduct* product) const noexcept;
    };
    [[nodiscard]] static bool shadow_releasable(const ShadowProduct& product) noexcept;

    explicit GeometryResidency(GeometryArena arena) noexcept;

    [[nodiscard]] std::optional<GeometrySpan> reserve(
        rhi::CommandList& commands, core::u32 vertices, core::u32 indices,
        core::u32 skins, bool& grew);
    void retire(std::unique_ptr<Entry> entry) noexcept;
    [[nodiscard]] static bool in_flight(const Entry& entry) noexcept;

    std::recursive_mutex mutex_;
    std::weak_ptr<GeometryResidency> self_;
    GeometryArena arena_;
    std::map<scene::MeshKey, std::unique_ptr<Entry>> procedural_;
    std::map<const ModelAsset*, std::weak_ptr<Entry>> models_;
    // Released entries waiting for their last recording.
    std::vector<std::unique_ptr<Entry>> retiring_;
    std::vector<std::weak_ptr<ShadowProduct>> shadows_;
    std::vector<std::unique_ptr<ShadowProduct>> retiring_shadows_;
    scene::MeshData mesh_scratch_;
    // Converted tangents, skin records and absolute indices for one write;
    // bounded by the write piece size.
    std::vector<core::u8> scratch_;
    core::u64 epoch_{};
};

} // namespace mediaserver::render
