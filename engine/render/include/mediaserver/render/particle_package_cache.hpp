#pragma once

#include <mediaserver/core/types.hpp>

#include <ajfx/cache.hpp>
#include <ajfx/effect.hpp>
#include <ajfx/gpu/kernels.hpp>
#include <ajfx/field.hpp>
#include <ajfx/mesh.hpp>
#include <ajfx/texture.hpp>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// .ajfx packages, read and checked on a worker thread so no file read, hash or
// parse happens while a frame is recorded. The renderer observes a source each
// frame; the first observation queues it, and later ones return the package
// once it is ready, or why it never will be.
//
// Only a package's precompiled kernels are used: the media server compiles
// no shaders at runtime, so a package without kernels for this device, or
// with kernels from a different ajfx, fails with that reason.
//
// A cache beside the package (`effect.ajfx` and `effect.ajfxcache`, as
// Studio exports them) is read with it and opened as it was baked: its
// cached emitters are played rather than simulated, with the instance seed,
// performance and bake-time values it was baked with. A cache that does not
// belong to the package (another effect, a changed layer, another version)
// fails the package with the reason, rather than playing it as something it
// is not.
namespace mediaserver::render {

struct ParticlePackage final {
    ajfx::Effect effect;
    ajfx::gpu::KernelSet kernels;
    // Every texture and mesh the effect's emitters draw, and every distance
    // field they collide with, checked.
    ajfx::TextureSet textures;
    ajfx::FieldSet fields;
    ajfx::MeshSet meshes;
    // The cache beside it, opened as baked, if there is one.
    std::shared_ptr<const ajfx::Cache> cache;
    // FNV-1a of the file's bytes, and its cache's: a different package or
    // cache at the same path is a different effect, and the renderer
    // rebuilds its instances.
    core::u64 identity{};
};

// Where a package's cache is looked for: beside it, `.ajfxcache` for its
// extension.
[[nodiscard]] std::string particle_cache_path(const std::string& package_source);

enum class ParticlePackageState : core::u8 {
    Pending,
    Ready,
    Failed,
    // The queue or source table is full; observed again later, it is retried.
    Busy,
};

struct ParticlePackageObservation final {
    std::shared_ptr<const ParticlePackage> package;
    ParticlePackageState state{ParticlePackageState::Pending};
    std::string error;
};

struct ParticlePackageCacheConfig final {
    std::size_t max_pending{16U};
    std::size_t max_sources{256U};
};

class ParticlePackageCache final {
public:
    explicit ParticlePackageCache(ajfx::gpu::ShaderFormat format, ParticlePackageCacheConfig config = {});
    ParticlePackageCache(const ParticlePackageCache&) = delete;
    ParticlePackageCache& operator=(const ParticlePackageCache&) = delete;
    ~ParticlePackageCache();

    // Never blocks on the file: queues an unseen source and returns its state.
    // A different `generation` (the scene's replicated asset generation, which
    // a Library rescan advances) reads the source again, keeping the current
    // package until the new one is ready, so a re-export in place swaps over
    // without a gap and every node swaps at the same generation.
    [[nodiscard]] ParticlePackageObservation observe(const std::string& source, core::u64 generation = 0U);
    // Blocks until nothing is queued or loading. For tests and tools.
    void wait_idle();

private:
    struct Entry final {
        std::shared_ptr<const ParticlePackage> package;
        ParticlePackageState state{ParticlePackageState::Pending};
        std::string error;
        core::u64 generation{};
        bool queued{};
    };

    void run();
    void start_worker();

    ajfx::gpu::ShaderFormat format_;
    ParticlePackageCacheConfig config_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::unordered_map<std::string, Entry> entries_;
    std::deque<std::string> queue_;
    std::size_t loading_{0U};
    bool stopping_{false};
    std::thread worker_;
};

} // namespace mediaserver::render
