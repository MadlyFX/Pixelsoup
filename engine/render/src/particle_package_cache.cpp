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
    auto package = std::make_shared<ParticlePackage>();
    package->effect = std::move(effect).value();
    package->kernels = std::move(kernels).value();
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

ParticlePackageObservation ParticlePackageCache::observe(const std::string& source) {
    std::lock_guard lock{mutex_};
    auto found = entries_.find(source);
    if (found == entries_.end()) {
        if (entries_.size() >= config_.max_sources || queue_.size() + loading_ >= config_.max_pending) {
            return {nullptr, ParticlePackageState::Busy, "the particle package queue is full"};
        }
        found = entries_.emplace(source, Entry{}).first;
        found->second.queued = true;
        queue_.push_back(source);
        start_worker();
        wake_.notify_one();
    }
    const Entry& entry = found->second;
    return {entry.package, entry.package != nullptr ? ParticlePackageState::Ready : entry.state, entry.error};
}

void ParticlePackageCache::refresh(const std::string& source) {
    std::lock_guard lock{mutex_};
    Entry& entry = entries_[source];
    if (!entry.queued) {
        entry.queued = true;
        queue_.push_back(source);
        start_worker();
        wake_.notify_one();
    }
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
            entry.package = std::move(loaded.package);
            entry.state = ParticlePackageState::Ready;
            entry.error.clear();
        } else if (entry.package == nullptr) {
            // A failed refresh keeps the package that was working.
            entry.state = ParticlePackageState::Failed;
            entry.error = std::move(loaded.error);
        }
        if (queue_.empty() && loading_ == 0U) {
            idle_.notify_all();
        }
    }
}

} // namespace mediaserver::render
