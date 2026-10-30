#include <mediaserver/render/particle_package_cache.hpp>

#include <ajfx/package.hpp>

#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

namespace mediaserver::render {
namespace {

core::u64 fnv1a(const std::vector<std::uint8_t>& bytes) {
    core::u64 hash = 0xcbf29ce484222325ULL;
    for (const std::uint8_t byte : bytes) {
        hash = (hash ^ byte) * 0x100000001b3ULL;
    }
    return hash;
}

struct Loaded final {
    std::shared_ptr<const ParticlePackage> package;
    std::string error;
};

Loaded load(const std::string& source, ajfx::gpu::ShaderFormat format) {
    std::ifstream stream{source, std::ios::binary};
    if (!stream) {
        return {nullptr, "the particle package '" + source + "' cannot be read"};
    }
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{stream}, {}};
    ajfx::Result<ajfx::Effect> effect = ajfx::read_package(bytes);
    if (!effect) {
        return {nullptr, effect.error().message};
    }
    ajfx::Result<ajfx::gpu::KernelSet> kernels = ajfx::gpu::read_package_kernels(bytes, effect.value(), format);
    if (!kernels) {
        return {nullptr, kernels.error().message};
    }
    ajfx::Result<ajfx::TextureSet> textures = ajfx::read_package_textures(bytes, effect.value());
    if (!textures) {
        return {nullptr, textures.error().message};
    }
    ajfx::Result<ajfx::FieldSet> fields = ajfx::read_package_fields(bytes, effect.value());
    if (!fields) {
        return {nullptr, fields.error().message};
    }
    auto package = std::make_shared<ParticlePackage>();
    package->effect = std::move(effect).value();
    package->kernels = std::move(kernels).value();
    package->textures = std::move(textures).value();
    package->fields = std::move(fields).value();
    package->identity = fnv1a(bytes);
    return {std::move(package), {}};
}

} // namespace

ParticlePackageCache::ParticlePackageCache(ajfx::gpu::ShaderFormat format, ParticlePackageCacheConfig config)
    : format_{format}, config_{config} {}

ParticlePackageCache::~ParticlePackageCache() {
    {
        std::lock_guard lock{mutex_};
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ParticlePackageCache::start_worker() {
    // A renderer with no particles in its scenes never starts a thread.
    if (!worker_.joinable()) {
        worker_ = std::thread{[this] { run(); }};
    }
}

ParticlePackageObservation ParticlePackageCache::observe(const std::string& source, core::u64 generation) {
    std::lock_guard lock{mutex_};
    auto found = entries_.find(source);
    const bool unseen = found == entries_.end();
    if (unseen || (found->second.generation != generation && !found->second.queued)) {
        if ((unseen && entries_.size() >= config_.max_sources) || queue_.size() + loading_ >= config_.max_pending) {
            if (unseen) {
                return {nullptr, ParticlePackageState::Busy, "the particle package queue is full"};
            }
        } else {
            if (unseen) {
                found = entries_.emplace(source, Entry{}).first;
            }
            found->second.generation = generation;
            found->second.queued = true;
            queue_.push_back(source);
            start_worker();
            wake_.notify_one();
        }
    }
    const Entry& entry = found->second;
    return {entry.package, entry.package != nullptr ? ParticlePackageState::Ready : entry.state, entry.error};
}

void ParticlePackageCache::wait_idle() {
    std::unique_lock lock{mutex_};
    idle_.wait(lock, [this] { return queue_.empty() && loading_ == 0U; });
}

void ParticlePackageCache::run() {
    std::unique_lock lock{mutex_};
    while (true) {
        wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_) {
            return;
        }
        const std::string source = std::move(queue_.front());
        queue_.pop_front();
        ++loading_;
        lock.unlock();
        Loaded loaded = load(source, format_);
        lock.lock();
        --loading_;
        Entry& entry = entries_[source];
        entry.queued = false;
        if (loaded.package != nullptr) {
            // An unchanged file keeps the package (and every instance made
            // from it) rather than swapping in an identical copy.
            if (entry.package == nullptr || entry.package->identity != loaded.package->identity) {
                entry.package = std::move(loaded.package);
            }
            entry.state = ParticlePackageState::Ready;
            entry.error.clear();
        } else if (entry.package == nullptr) {
            // A failed reread keeps the package that was working.
            entry.state = ParticlePackageState::Failed;
            entry.error = std::move(loaded.error);
        }
        if (queue_.empty() && loading_ == 0U) {
            idle_.notify_all();
        }
    }
}

} // namespace mediaserver::render
