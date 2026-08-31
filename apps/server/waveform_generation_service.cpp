#include "waveform_generation_service.hpp"

#include <mediaserver/assets/asset_path.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mediaserver::server {
namespace {

[[nodiscard]] WaveformServiceError
service_error(WaveformServiceErrorCode code) noexcept {
  return WaveformServiceError{code};
}

[[nodiscard]] WaveformGenerationConfig
normalized_config(WaveformGenerationConfig config) noexcept {
  config.worker_count =
      std::clamp(config.worker_count, 1U, maximum_waveform_workers);
  config.maximum_pending_requests = std::clamp(
      config.maximum_pending_requests, 1U, maximum_waveform_pending_requests);
  config.maximum_prefetch_requests = std::min(config.maximum_prefetch_requests,
                                              config.maximum_pending_requests);
  const core::u32 minimum_snapshots =
      config.maximum_pending_requests + config.worker_count;
  config.maximum_snapshots = std::clamp(
      config.maximum_snapshots, minimum_snapshots, maximum_waveform_snapshots);
  config.maximum_interleaved_samples =
      std::clamp(config.maximum_interleaved_samples, core::u64{1U},
                 maximum_waveform_interleaved_samples);
  return config;
}

[[nodiscard]] bool terminal(assets::DerivativeState state) noexcept {
  switch (state) {
  case assets::DerivativeState::Ready:
  case assets::DerivativeState::Stale:
  case assets::DerivativeState::Failed:
  case assets::DerivativeState::Cancelled:
    return true;
  case assets::DerivativeState::Missing:
  case assets::DerivativeState::Queued:
  case assets::DerivativeState::Generating:
    return false;
  }
  return true;
}

[[nodiscard]] const assets::DerivativeProfile *waveform_profile() noexcept {
  const assets::DerivativeProfile *const profile = assets::derivative_profile(
      assets::DerivativeProfileId::Waveform2048PeakRms);
  if (profile == nullptr ||
      profile->id != assets::DerivativeProfileId::Waveform2048PeakRms ||
      profile->kind != assets::DerivativeKind::Waveform ||
      profile->codec != assets::DerivativeCodec::PeakRmsF32 ||
      profile->sample_count != assets::waveform_peak_rms_bins) {
    return nullptr;
  }
  return profile;
}

class UnavailableWaveformDecoder final : public WaveformDecoderBackend {
public:
  WaveformBackendResult decode(const WaveformDecodeRequest &,
                               std::stop_token stop) override {
    return WaveformBackendResult::failure(WaveformBackendError{
        stop.stop_requested() ? WaveformBackendErrorCode::Cancelled
                              : WaveformBackendErrorCode::Unavailable,
        {},
    });
  }
};

} // namespace

WaveformDecoderFactory unavailable_waveform_decoder_factory() {
  return [] { return std::make_unique<UnavailableWaveformDecoder>(); };
}

struct WaveformGenerationService::Impl final {
  struct Job final {
    assets::DerivativeKey key;
    assets::MediaAsset asset;
    assets::DerivativeState state{assets::DerivativeState::Missing};
    WaveformRequestPriority priority{WaveformRequestPriority::Visible};
    WaveformFailureCode failure{WaveformFailureCode::None};
    std::optional<WaveformBackendError> backend_error;
    std::optional<assets::DerivativeCacheErrorCode> cache_error;
    std::optional<assets::WaveformError> waveform_error;
    core::u64 source_frames{};
    core::u64 interleaved_samples{};
    core::u64 revision{};
    core::u64 stamp{};
    std::stop_source cancellation;
    bool enqueued{};
    bool in_flight{};
  };

  Impl(assets::DerivativeCache value, WaveformGenerationConfig requested_config,
       WaveformDecoderFactory factory)
      : cache(std::move(value)), config(normalized_config(requested_config)),
        decoder_factory(std::move(factory)) {
    if (!decoder_factory) {
      decoder_factory = unavailable_waveform_decoder_factory();
    }
    workers.reserve(config.worker_count);
    try {
      for (core::u32 index = 0U; index < config.worker_count; ++index) {
        static_cast<void>(index);
        workers.emplace_back([this] { worker_loop(); });
      }
    } catch (...) {
      shutdown();
      throw;
    }
  }

  ~Impl() { shutdown(); }

  void shutdown() noexcept {
    {
      std::lock_guard lock{mutex};
      if (!stopping) {
        stopping = true;
        for (auto &pair : jobs) {
          Job &job = *pair.second;
          job.cancellation.request_stop();
          if (job.enqueued) {
            release_queue_slot_locked(job);
            set_state_locked(job, assets::DerivativeState::Cancelled);
          }
        }
      }
    }
    ready.notify_all();
    for (std::thread &worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers.clear();
  }

  [[nodiscard]] WaveformSnapshot snapshot_locked(const Job &job) const {
    return WaveformSnapshot{
        job.key,
        job.state,
        job.priority,
        job.failure,
        job.backend_error,
        job.cache_error,
        job.waveform_error,
        job.source_frames,
        job.interleaved_samples,
        job.revision,
        assets::derivative_cache_path(cache.root(), job.key),
    };
  }

  void set_state_locked(
      Job &job, assets::DerivativeState state,
      WaveformFailureCode failure = WaveformFailureCode::None) noexcept {
    job.state = state;
    job.failure = failure;
    job.backend_error.reset();
    job.cache_error.reset();
    job.waveform_error.reset();
    job.stamp = ++clock;
    ++job.revision;
  }

  void release_queue_slot_locked(Job &job) noexcept {
    if (!job.enqueued) {
      return;
    }
    job.enqueued = false;
    --queued_count;
    if (job.priority == WaveformRequestPriority::Prefetch) {
      --prefetch_count;
    }
  }

  void cancel_locked(Job &job, WaveformFailureCode failure =
                                   WaveformFailureCode::None) noexcept {
    job.cancellation.request_stop();
    release_queue_slot_locked(job);
    if (job.state == assets::DerivativeState::Queued ||
        job.state == assets::DerivativeState::Generating ||
        (job.state == assets::DerivativeState::Stale && job.in_flight)) {
      set_state_locked(job, assets::DerivativeState::Cancelled, failure);
    }
  }

  [[nodiscard]] bool evict_prefetch_locked() noexcept {
    while (!prefetch.empty()) {
      std::shared_ptr<Job> candidate = std::move(prefetch.front());
      prefetch.pop_front();
      if (!candidate->enqueued ||
          candidate->priority != WaveformRequestPriority::Prefetch) {
        continue;
      }
      cancel_locked(*candidate, WaveformFailureCode::QueueCapacity);
      return true;
    }
    return false;
  }

  [[nodiscard]] bool enqueue_locked(const std::shared_ptr<Job> &job) noexcept {
    if (job->priority == WaveformRequestPriority::Prefetch &&
        prefetch_count >= config.maximum_prefetch_requests) {
      set_state_locked(*job, assets::DerivativeState::Cancelled,
                       WaveformFailureCode::QueueCapacity);
      return false;
    }
    if (queued_count >= config.maximum_pending_requests) {
      if (job->priority == WaveformRequestPriority::Prefetch ||
          !evict_prefetch_locked()) {
        set_state_locked(*job, assets::DerivativeState::Cancelled,
                         WaveformFailureCode::QueueCapacity);
        return false;
      }
    }
    job->enqueued = true;
    ++queued_count;
    if (job->priority == WaveformRequestPriority::Visible) {
      visible.push_back(job);
    } else {
      ++prefetch_count;
      prefetch.push_back(job);
    }
    ready.notify_one();
    return true;
  }

  [[nodiscard]] std::shared_ptr<Job> take_locked() noexcept {
    const auto take_from = [this](std::deque<std::shared_ptr<Job>> &queue) {
      while (!queue.empty()) {
        std::shared_ptr<Job> job = std::move(queue.front());
        queue.pop_front();
        if (!job->enqueued || job->cancellation.stop_requested()) {
          continue;
        }
        release_queue_slot_locked(*job);
        return job;
      }
      return std::shared_ptr<Job>{};
    };
    if (std::shared_ptr<Job> job = take_from(visible)) {
      return job;
    }
    return take_from(prefetch);
  }

  void prune_locked() {
    while (jobs.size() > config.maximum_snapshots) {
      auto oldest = jobs.end();
      for (auto candidate = jobs.begin(); candidate != jobs.end();
           ++candidate) {
        const Job &job = *candidate->second;
        if (job.enqueued || job.in_flight || !terminal(job.state)) {
          continue;
        }
        if (oldest == jobs.end() || job.stamp < oldest->second->stamp) {
          oldest = candidate;
        }
      }
      if (oldest == jobs.end()) {
        return;
      }
      jobs.erase(oldest);
    }
  }

  void finish(const std::shared_ptr<Job> &job, assets::DerivativeState state,
              WaveformFailureCode failure = WaveformFailureCode::None,
              std::optional<WaveformBackendError> backend_error = std::nullopt,
              std::optional<assets::DerivativeCacheErrorCode> cache_error =
                  std::nullopt,
              std::optional<assets::WaveformError> waveform_error =
                  std::nullopt) noexcept {
    std::lock_guard lock{mutex};
    job->in_flight = false;
    if (job->state == assets::DerivativeState::Cancelled ||
        job->cancellation.stop_requested()) {
      if (job->state != assets::DerivativeState::Cancelled) {
        set_state_locked(*job, assets::DerivativeState::Cancelled);
      }
    } else {
      set_state_locked(*job, state, failure);
      job->backend_error = std::move(backend_error);
      job->cache_error = cache_error;
      job->waveform_error = waveform_error;
    }
    prune_locked();
  }

  void finish_fingerprint_error(
      const std::shared_ptr<Job> &job,
      const assets::ContentFingerprintError &error) noexcept {
    switch (error.code) {
    case assets::ContentFingerprintErrorCode::Cancelled:
      finish(job, assets::DerivativeState::Cancelled);
      return;
    case assets::ContentFingerprintErrorCode::SourceChanged:
      finish(job, assets::DerivativeState::Stale,
             WaveformFailureCode::FingerprintMismatch);
      return;
    case assets::ContentFingerprintErrorCode::InvalidSource:
    case assets::ContentFingerprintErrorCode::SourceMissing:
    case assets::ContentFingerprintErrorCode::OpenFailed:
    case assets::ContentFingerprintErrorCode::ReadFailed:
    case assets::ContentFingerprintErrorCode::SizeLimitExceeded:
    case assets::ContentFingerprintErrorCode::AllocationFailed:
      finish(job, assets::DerivativeState::Failed,
             WaveformFailureCode::SourceUnavailable);
      return;
    }
  }

  void finish_backend_error(const std::shared_ptr<Job> &job,
                            const WaveformBackendError &error) noexcept {
    switch (error.code) {
    case WaveformBackendErrorCode::Cancelled:
      finish(job, assets::DerivativeState::Cancelled);
      return;
    case WaveformBackendErrorCode::Unavailable:
      finish(job, assets::DerivativeState::Failed,
             WaveformFailureCode::BackendUnavailable, error);
      return;
    case WaveformBackendErrorCode::SourceUnavailable:
      finish(job, assets::DerivativeState::Failed,
             WaveformFailureCode::SourceUnavailable, error);
      return;
    case WaveformBackendErrorCode::DecodeFailed:
    case WaveformBackendErrorCode::AllocationFailed:
      finish(job, assets::DerivativeState::Failed,
             WaveformFailureCode::BackendFailure, error);
      return;
    }
  }

  void finish_derivative_error(
      const std::shared_ptr<Job> &job,
      const assets::WaveformDerivativeError &error) noexcept {
    switch (error.code) {
    case assets::WaveformDerivativeErrorCode::BuildFailed:
      finish(job, assets::DerivativeState::Failed,
             WaveformFailureCode::InvalidPcm, std::nullopt, std::nullopt,
             error.waveform_error);
      return;
    case assets::WaveformDerivativeErrorCode::InvalidCachedPayload:
      finish(job, assets::DerivativeState::Failed,
             WaveformFailureCode::InvalidCachedPayload, std::nullopt,
             std::nullopt, error.waveform_error);
      return;
    case assets::WaveformDerivativeErrorCode::CacheFailure:
      finish(job, assets::DerivativeState::Failed,
             WaveformFailureCode::CacheFailure, std::nullopt,
             error.cache_error.has_value()
                 ? std::optional{error.cache_error->code}
                 : std::nullopt);
      return;
    case assets::WaveformDerivativeErrorCode::NotReady:
      if (error.cache_state == assets::DerivativeState::Cancelled) {
        finish(job, assets::DerivativeState::Cancelled);
      } else {
        finish(job, assets::DerivativeState::Failed,
               WaveformFailureCode::CacheFailure);
      }
      return;
    case assets::WaveformDerivativeErrorCode::InvalidAsset:
    case assets::WaveformDerivativeErrorCode::MissingFingerprint:
    case assets::WaveformDerivativeErrorCode::ProfileUnavailable:
      finish(job, assets::DerivativeState::Failed,
             WaveformFailureCode::BackendFailure);
      return;
    }
  }

  void process(const std::shared_ptr<Job> &job,
               WaveformDecoderBackend *decoder) {
    const std::stop_token stop = job->cancellation.get_token();
    const assets::DerivativeProfile *const profile = waveform_profile();
    if (profile == nullptr || !job->asset.fingerprint.has_value() ||
        assets::make_derivative_key(job->asset.id, *job->asset.fingerprint,
                                    *profile) != job->key) {
      finish(job, assets::DerivativeState::Failed,
             WaveformFailureCode::BackendFailure);
      return;
    }

    const auto initial_fingerprint =
        assets::source_fingerprint(job->asset.source, stop);
    if (!initial_fingerprint) {
      finish_fingerprint_error(job, initial_fingerprint.error());
      return;
    }
    if (initial_fingerprint.value() != *job->asset.fingerprint) {
      finish(job, assets::DerivativeState::Stale,
             WaveformFailureCode::FingerprintMismatch);
      return;
    }
    if (stop.stop_requested()) {
      finish(job, assets::DerivativeState::Cancelled);
      return;
    }
    if (decoder == nullptr) {
      finish_backend_error(
          job, WaveformBackendError{WaveformBackendErrorCode::Unavailable, {}});
      return;
    }

    WaveformDecodeRequest request{
        job->asset.id, job->asset.source, *job->asset.fingerprint,
        job->key,      *profile,          config.maximum_interleaved_samples,
    };
    WaveformBackendResult decoded = decoder->decode(request, stop);
    if (!decoded) {
      finish_backend_error(job, decoded.error());
      return;
    }
    WaveformDecodedPcm pcm = std::move(decoded).value();
    if (pcm.decoded_source_fingerprint != *job->asset.fingerprint) {
      finish(job, assets::DerivativeState::Stale,
             WaveformFailureCode::FingerprintMismatch);
      return;
    }
    if (pcm.channel_count == 0U ||
        pcm.channel_count > assets::maximum_waveform_channels ||
        pcm.interleaved_samples.empty() ||
        pcm.interleaved_samples.size() > config.maximum_interleaved_samples ||
        pcm.interleaved_samples.size() % pcm.channel_count != 0U) {
      finish(job, assets::DerivativeState::Failed,
             WaveformFailureCode::InvalidPcm);
      return;
    }
    {
      std::lock_guard lock{mutex};
      job->interleaved_samples = pcm.interleaved_samples.size();
      job->source_frames = pcm.interleaved_samples.size() / pcm.channel_count;
      job->stamp = ++clock;
      ++job->revision;
    }

    const auto final_fingerprint =
        assets::source_fingerprint(job->asset.source, stop);
    if (!final_fingerprint) {
      finish_fingerprint_error(job, final_fingerprint.error());
      return;
    }
    if (final_fingerprint.value() != *job->asset.fingerprint) {
      finish(job, assets::DerivativeState::Stale,
             WaveformFailureCode::FingerprintMismatch);
      return;
    }
    if (stop.stop_requested()) {
      finish(job, assets::DerivativeState::Cancelled);
      return;
    }

    auto published = assets::publish_waveform_derivative(
        cache, job->asset, pcm.interleaved_samples, pcm.channel_count, false,
        stop);
    if (!published) {
      finish_derivative_error(job, published.error());
      return;
    }
    assets::WaveformDerivative generated = std::move(published).value();
    if (generated.key != job->key ||
        generated.entry.state == assets::DerivativeState::Cancelled) {
      finish(job, assets::DerivativeState::Cancelled);
      return;
    }
    if (generated.entry.state != assets::DerivativeState::Ready) {
      finish(job, assets::DerivativeState::Failed,
             WaveformFailureCode::CacheFailure);
      return;
    }

    // Validate the newly published envelope and its independently
    // versioned waveform payload before reporting Ready.
    auto loaded =
        assets::load_waveform_derivative(cache, job->asset, false, stop);
    if (!loaded) {
      finish_derivative_error(job, loaded.error());
      return;
    }
    const assets::WaveformDerivative &verified = loaded.value();
    if (verified.key != job->key ||
        verified.entry.state != assets::DerivativeState::Ready ||
        verified.envelope != generated.envelope) {
      finish(job, assets::DerivativeState::Failed,
             WaveformFailureCode::InvalidCachedPayload);
      return;
    }
    finish(job, assets::DerivativeState::Ready);
  }

  void worker_loop() noexcept {
    std::unique_ptr<WaveformDecoderBackend> decoder;
    try {
      decoder = decoder_factory();
    } catch (...) {
      decoder.reset();
    }
    while (true) {
      std::shared_ptr<Job> job;
      {
        std::unique_lock lock{mutex};
        ready.wait(lock, [this] { return stopping || queued_count != 0U; });
        if (stopping) {
          return;
        }
        job = take_locked();
        if (!job) {
          continue;
        }
        job->in_flight = true;
        set_state_locked(*job, assets::DerivativeState::Generating);
      }
      try {
        process(job, decoder.get());
      } catch (const std::bad_alloc &) {
        finish_backend_error(
            job, WaveformBackendError{
                     WaveformBackendErrorCode::AllocationFailed, {}});
      } catch (...) {
        finish_backend_error(
            job,
            WaveformBackendError{WaveformBackendErrorCode::DecodeFailed, {}});
      }
    }
  }

  assets::DerivativeCache cache;
  WaveformGenerationConfig config;
  WaveformDecoderFactory decoder_factory;
  mutable std::mutex mutex;
  std::condition_variable ready;
  bool stopping{};
  std::vector<std::thread> workers;
  std::deque<std::shared_ptr<Job>> visible;
  std::deque<std::shared_ptr<Job>> prefetch;
  std::unordered_map<std::string, std::shared_ptr<Job>> jobs;
  core::u32 queued_count{};
  core::u32 prefetch_count{};
  core::u64 clock{};
};

WaveformGenerationService::WaveformGenerationService(
    assets::DerivativeCache cache, WaveformGenerationConfig config)
    : WaveformGenerationService(std::move(cache), config,
                                unavailable_waveform_decoder_factory()) {}

WaveformGenerationService::WaveformGenerationService(
    assets::DerivativeCache cache, WaveformGenerationConfig config,
    WaveformDecoderFactory decoder_factory)
    : implementation_(std::make_unique<Impl>(std::move(cache), config,
                                             std::move(decoder_factory))) {}

WaveformGenerationService::~WaveformGenerationService() = default;

WaveformRequestResult
WaveformGenerationService::request(const assets::MediaAsset &asset,
                                   WaveformRequestPriority priority) {
  try {
    if (!asset.id || !assets::valid_asset_source(asset.source)) {
      return WaveformRequestResult::failure(
          service_error(WaveformServiceErrorCode::InvalidAsset));
    }
    if (!asset.fingerprint.has_value()) {
      return WaveformRequestResult::failure(
          service_error(WaveformServiceErrorCode::MissingFingerprint));
    }
    const assets::DerivativeProfile *const profile = waveform_profile();
    if (profile == nullptr) {
      return WaveformRequestResult::failure(
          service_error(WaveformServiceErrorCode::ProfileUnavailable));
    }
    const assets::DerivativeKey key =
        assets::make_derivative_key(asset.id, *asset.fingerprint, *profile);
    const std::string encoded_key = assets::derivative_key_string(key);

    {
      std::lock_guard lock{implementation_->mutex};
      const auto found = implementation_->jobs.find(encoded_key);
      if (found != implementation_->jobs.end() &&
          (found->second->enqueued || found->second->in_flight)) {
        if (priority == WaveformRequestPriority::Visible &&
            found->second->priority == WaveformRequestPriority::Prefetch) {
          found->second->priority = priority;
          found->second->stamp = ++implementation_->clock;
          ++found->second->revision;
          if (found->second->enqueued) {
            --implementation_->prefetch_count;
            implementation_->visible.push_back(found->second);
          }
        }
        return WaveformRequestResult::success(
            implementation_->snapshot_locked(*found->second));
      }
    }

    auto cached =
        assets::load_waveform_derivative(implementation_->cache, asset);
    auto job = std::make_shared<Impl::Job>();
    job->key = key;
    job->asset = asset;
    job->priority = priority;
    std::optional<WaveformSnapshot> initial_snapshot;
    {
      std::lock_guard lock{implementation_->mutex};
      const auto raced = implementation_->jobs.find(encoded_key);
      if (raced != implementation_->jobs.end() &&
          (raced->second->enqueued || raced->second->in_flight)) {
        return WaveformRequestResult::success(
            implementation_->snapshot_locked(*raced->second));
      }
      job->stamp = ++implementation_->clock;
      bool enqueue = false;
      if (cached) {
        implementation_->set_state_locked(*job, assets::DerivativeState::Ready);
      } else {
        const assets::WaveformDerivativeError &error = cached.error();
        switch (error.code) {
        case assets::WaveformDerivativeErrorCode::NotReady:
          if (error.cache_state == assets::DerivativeState::Stale) {
            implementation_->set_state_locked(
                *job, assets::DerivativeState::Stale,
                WaveformFailureCode::CacheFailure);
            initial_snapshot = implementation_->snapshot_locked(*job);
            enqueue = true;
          } else if (error.cache_state == assets::DerivativeState::Failed) {
            implementation_->set_state_locked(
                *job, assets::DerivativeState::Failed,
                WaveformFailureCode::CacheFailure);
          } else {
            enqueue = true;
          }
          break;
        case assets::WaveformDerivativeErrorCode::InvalidCachedPayload:
          implementation_->set_state_locked(
              *job, assets::DerivativeState::Stale,
              WaveformFailureCode::InvalidCachedPayload);
          job->waveform_error = error.waveform_error;
          initial_snapshot = implementation_->snapshot_locked(*job);
          enqueue = true;
          break;
        case assets::WaveformDerivativeErrorCode::CacheFailure:
          implementation_->set_state_locked(*job,
                                            assets::DerivativeState::Failed,
                                            WaveformFailureCode::CacheFailure);
          if (error.cache_error.has_value()) {
            job->cache_error = error.cache_error->code;
          }
          break;
        case assets::WaveformDerivativeErrorCode::InvalidAsset:
        case assets::WaveformDerivativeErrorCode::MissingFingerprint:
        case assets::WaveformDerivativeErrorCode::ProfileUnavailable:
          return WaveformRequestResult::failure(
              service_error(WaveformServiceErrorCode::ProfileUnavailable));
        case assets::WaveformDerivativeErrorCode::BuildFailed:
          implementation_->set_state_locked(*job,
                                            assets::DerivativeState::Failed,
                                            WaveformFailureCode::InvalidPcm);
          job->waveform_error = error.waveform_error;
          break;
        }
      }
      if (enqueue) {
        implementation_->set_state_locked(*job,
                                          assets::DerivativeState::Queued);
        static_cast<void>(implementation_->enqueue_locked(job));
      }
      implementation_->jobs.insert_or_assign(encoded_key, job);
      implementation_->prune_locked();
      if (initial_snapshot.has_value()) {
        return WaveformRequestResult::success(std::move(*initial_snapshot));
      }
      return WaveformRequestResult::success(
          implementation_->snapshot_locked(*job));
    }
  } catch (const std::bad_alloc &) {
    return WaveformRequestResult::failure(
        service_error(WaveformServiceErrorCode::AllocationFailed));
  }
}

std::optional<WaveformSnapshot>
WaveformGenerationService::snapshot(const assets::DerivativeKey &key) {
  try {
    const std::string encoded = assets::derivative_key_string(key);
    std::lock_guard lock{implementation_->mutex};
    const auto found = implementation_->jobs.find(encoded);
    if (found == implementation_->jobs.end() || found->second->key != key) {
      return std::nullopt;
    }
    found->second->stamp = ++implementation_->clock;
    return implementation_->snapshot_locked(*found->second);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<WaveformSnapshot>
WaveformGenerationService::cancel(const assets::DerivativeKey &key) {
  try {
    const std::string encoded = assets::derivative_key_string(key);
    std::lock_guard lock{implementation_->mutex};
    const auto found = implementation_->jobs.find(encoded);
    if (found == implementation_->jobs.end() || found->second->key != key) {
      return std::nullopt;
    }
    implementation_->cancel_locked(*found->second);
    return implementation_->snapshot_locked(*found->second);
  } catch (...) {
    return std::nullopt;
  }
}

void WaveformGenerationService::retain(
    std::span<const assets::DerivativeKey> needed) {
  std::unordered_set<std::string> retained;
  retained.reserve(needed.size());
  for (const assets::DerivativeKey &key : needed) {
    retained.insert(assets::derivative_key_string(key));
  }
  std::lock_guard lock{implementation_->mutex};
  for (auto &pair : implementation_->jobs) {
    Impl::Job &job = *pair.second;
    if ((job.enqueued || job.in_flight) &&
        retained.find(pair.first) == retained.end()) {
      implementation_->cancel_locked(job);
    }
  }
}

assets::WaveformDerivativeResult
WaveformGenerationService::load(const assets::MediaAsset &asset,
                                std::stop_token stop) noexcept {
  return assets::load_waveform_derivative(implementation_->cache, asset, false,
                                          stop);
}

WaveformGenerationStats WaveformGenerationService::stats() const noexcept {
  WaveformGenerationStats result{};
  std::lock_guard lock{implementation_->mutex};
  for (const auto &pair : implementation_->jobs) {
    switch (pair.second->state) {
    case assets::DerivativeState::Queued:
      ++result.queued;
      break;
    case assets::DerivativeState::Generating:
      ++result.generating;
      break;
    case assets::DerivativeState::Ready:
      ++result.ready;
      break;
    case assets::DerivativeState::Stale:
      ++result.stale;
      break;
    case assets::DerivativeState::Failed:
      ++result.failed;
      break;
    case assets::DerivativeState::Cancelled:
      ++result.cancelled;
      break;
    case assets::DerivativeState::Missing:
      break;
    }
  }
  result.workers = implementation_->config.worker_count;
  result.pending_limit = implementation_->config.maximum_pending_requests;
  result.prefetch_limit = implementation_->config.maximum_prefetch_requests;
  return result;
}

} // namespace mediaserver::server
