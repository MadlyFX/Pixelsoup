#pragma once

#include <mediaserver/core/types.hpp>

#include <ajfx/effect.hpp>
#include <ajfx/gpu/kernels.hpp>

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
namespace mediaserver::render {

struct ParticlePackage final {
    ajfx::Effect effect;
    ajfx::gpu::KernelSet kernels;
    // FNV-1a of the file's bytes: a different package at the same path is a
    // different effect, and the renderer rebuilds its instances.
    core::u64 identity{};
};

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
    [[nodiscard]] ParticlePackageObservation observe(const std::string& source);
    // Reads the source again, keeping the current package until the new one
    // is ready (so a re-export in place swaps over without a gap).
    void refresh(const std::string& source);
    // Blocks until nothing is queued or loading. For tests and tools.
    void wait_idle();

private:
    struct Entry final {
        std::shared_ptr<const ParticlePackage> package;
        ParticlePackageState state{ParticlePackageState::Pending};
        std::string error;
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
