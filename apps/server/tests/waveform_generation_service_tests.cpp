#include "waveform_generation_service.hpp"

#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/assets/derivative.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace server = mediaserver::server;
namespace fs = std::filesystem;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define WAVE_CHECK(...)                                                        \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<core::u64> serial{};
    path_ = fs::temp_directory_path() / ("mediaserver-waveform-service-" +
                                         std::to_string(serial.fetch_add(1U)));
    std::error_code ignored;
    static_cast<void>(fs::remove_all(path_, ignored));
    WAVE_CHECK(fs::create_directories(path_));
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    static_cast<void>(fs::remove_all(path_, ignored));
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

private:
  fs::path path_;
};

void write_source(const fs::path &path, std::string_view bytes) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  WAVE_CHECK(static_cast<bool>(output));
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  WAVE_CHECK(static_cast<bool>(output));
}

[[nodiscard]] assets::DerivativeCache cache_at(const fs::path &root) {
  auto opened = assets::DerivativeCache::open(
      assets::DerivativeCacheConfig{root, 8U * 1'024U * 1'024U});
  WAVE_CHECK(opened);
  return std::move(opened).value();
}

[[nodiscard]] assets::MediaAsset media_asset(doc::NodeId id,
                                             const fs::path &source) {
  auto fingerprint = assets::source_fingerprint(source.string());
  WAVE_CHECK(fingerprint);
  assets::MediaAsset asset{};
  asset.id = id;
  asset.display_name = "Audio source";
  asset.source = source.string();
  asset.fingerprint = fingerprint.value();
  return asset;
}

[[nodiscard]] assets::DerivativeKey
waveform_key(const assets::MediaAsset &asset) {
  const assets::DerivativeProfile *const profile = assets::derivative_profile(
      assets::DerivativeProfileId::Waveform2048PeakRms);
  WAVE_CHECK(profile != nullptr);
  WAVE_CHECK(asset.fingerprint.has_value());
  return assets::make_derivative_key(asset.id, *asset.fingerprint, *profile);
}

struct BackendState final {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<doc::NodeId> starts;
  std::vector<server::WaveformDecodeRequest> requests;
  std::vector<float> samples{0.25F, -0.5F, 0.75F, -1.0F};
  core::u32 channel_count{2U};
  std::optional<assets::ContentFingerprint> reported_fingerprint;
  std::optional<server::WaveformBackendError> error;
  bool block_first{};
  bool first_entered{};
  bool release_first{};
  bool mutate_source{};
};

class TestBackend final : public server::WaveformDecoderBackend {
public:
  explicit TestBackend(std::shared_ptr<BackendState> state)
      : state_(std::move(state)) {}

  server::WaveformBackendResult
  decode(const server::WaveformDecodeRequest &request,
         std::stop_token stop) override {
    std::vector<float> samples;
    core::u32 channel_count{};
    std::optional<assets::ContentFingerprint> reported;
    std::optional<server::WaveformBackendError> error;
    bool mutate{};
    bool block{};
    {
      std::unique_lock lock{state_->mutex};
      const std::size_t ordinal = state_->starts.size();
      state_->starts.push_back(request.asset);
      state_->requests.push_back(request);
      block = ordinal == 0U && state_->block_first;
      if (block) {
        state_->first_entered = true;
        state_->changed.notify_all();
        while (!state_->release_first && !stop.stop_requested()) {
          state_->changed.wait_for(lock, std::chrono::milliseconds{2});
        }
      }
      samples = state_->samples;
      channel_count = state_->channel_count;
      reported = state_->reported_fingerprint;
      error = state_->error;
      mutate = ordinal == 0U && state_->mutate_source;
    }
    if (stop.stop_requested()) {
      return server::WaveformBackendResult::failure(
          server::WaveformBackendError{
              server::WaveformBackendErrorCode::Cancelled, {}});
    }
    if (error.has_value()) {
      return server::WaveformBackendResult::failure(*error);
    }
    if (mutate) {
      write_source(request.source, "changed while decoding");
    }
    return server::WaveformBackendResult::success(server::WaveformDecodedPcm{
        reported.value_or(request.source_fingerprint), channel_count,
        std::move(samples)});
  }

private:
  std::shared_ptr<BackendState> state_;
};

[[nodiscard]] server::WaveformDecoderFactory
factory(const std::shared_ptr<BackendState> &state) {
  return [state] { return std::make_unique<TestBackend>(state); };
}

void wait_for_first_call(const std::shared_ptr<BackendState> &state) {
  std::unique_lock lock{state->mutex};
  WAVE_CHECK(state->changed.wait_for(lock, std::chrono::seconds{5},
                                     [&] { return state->first_entered; }));
}

void release_first_call(const std::shared_ptr<BackendState> &state) {
  {
    std::lock_guard lock{state->mutex};
    state->release_first = true;
  }
  state->changed.notify_all();
}

[[nodiscard]] server::WaveformSnapshot
wait_for_state(server::WaveformGenerationService &service,
               const assets::DerivativeKey &key,
               assets::DerivativeState expected) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < deadline) {
    const auto snapshot = service.snapshot(key);
    if (snapshot.has_value() && snapshot->state == expected) {
      return *snapshot;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
  }
  throw std::runtime_error{"waveform service state timed out"};
}

void invalid_inputs_and_production_backend_fail_closed() {
  TemporaryDirectory temporary;
  const fs::path source = temporary.path() / "source.bin";
  write_source(source, "source revision");

  server::WaveformGenerationService service{
      cache_at(temporary.path() / "cache"),
      server::WaveformGenerationConfig{1U, 4U, 2U, 16U, 1'024U}};
  assets::MediaAsset invalid{};
  invalid.source = source.string();
  auto requested = service.request(invalid);
  WAVE_CHECK(!requested);
  WAVE_CHECK(requested.error().code ==
             server::WaveformServiceErrorCode::InvalidAsset);

  invalid.id = doc::NodeId{1U};
  requested = service.request(invalid);
  WAVE_CHECK(!requested);
  WAVE_CHECK(requested.error().code ==
             server::WaveformServiceErrorCode::MissingFingerprint);

  const assets::MediaAsset valid = media_asset(doc::NodeId{2U}, source);
  requested = service.request(valid);
  WAVE_CHECK(requested);
  const server::WaveformSnapshot failed = wait_for_state(
      service, requested.value().key, assets::DerivativeState::Failed);
  WAVE_CHECK(failed.failure == server::WaveformFailureCode::BackendUnavailable);
  WAVE_CHECK(failed.backend_error.has_value());
  WAVE_CHECK(failed.backend_error->code ==
             server::WaveformBackendErrorCode::Unavailable);
  const auto loaded = service.load(valid);
  WAVE_CHECK(!loaded);
  WAVE_CHECK(loaded.error().code ==
             assets::WaveformDerivativeErrorCode::NotReady);
}

void successful_generation_uses_exact_identity_and_validates_cache() {
  TemporaryDirectory temporary;
  const fs::path source = temporary.path() / "audio.bin";
  write_source(source, "stable audio bytes");
  const assets::MediaAsset asset = media_asset(doc::NodeId{10U}, source);
  const assets::DerivativeKey expected_key = waveform_key(asset);
  auto backend = std::make_shared<BackendState>();
  const std::vector<float> expected_samples = backend->samples;

  server::WaveformGenerationService service{
      cache_at(temporary.path() / "cache"),
      server::WaveformGenerationConfig{1U, 4U, 2U, 16U, 1'024U},
      factory(backend)};
  auto requested = service.request(asset);
  WAVE_CHECK(requested);
  WAVE_CHECK(requested.value().key == expected_key);
  const server::WaveformSnapshot ready =
      wait_for_state(service, expected_key, assets::DerivativeState::Ready);
  WAVE_CHECK(ready.failure == server::WaveformFailureCode::None);
  WAVE_CHECK(ready.source_frames == 2U);
  WAVE_CHECK(ready.interleaved_samples == expected_samples.size());

  {
    std::lock_guard lock{backend->mutex};
    WAVE_CHECK(backend->requests.size() == 1U);
    const server::WaveformDecodeRequest &request = backend->requests.front();
    WAVE_CHECK(request.asset == asset.id);
    WAVE_CHECK(request.source == asset.source);
    WAVE_CHECK(request.source_fingerprint == *asset.fingerprint);
    WAVE_CHECK(request.key == expected_key);
    WAVE_CHECK(request.profile.id ==
               assets::DerivativeProfileId::Waveform2048PeakRms);
    WAVE_CHECK(request.maximum_interleaved_samples == 1'024U);
  }

  auto expected = assets::build_waveform_peak_rms(expected_samples, 2U);
  WAVE_CHECK(expected);
  auto loaded = service.load(asset);
  WAVE_CHECK(loaded);
  WAVE_CHECK(loaded.value().key == expected_key);
  WAVE_CHECK(loaded.value().entry.state == assets::DerivativeState::Ready);
  WAVE_CHECK(loaded.value().envelope == expected.value());

  requested = service.request(asset, server::WaveformRequestPriority::Prefetch);
  WAVE_CHECK(requested);
  WAVE_CHECK(requested.value().state == assets::DerivativeState::Ready);
  {
    std::lock_guard lock{backend->mutex};
    WAVE_CHECK(backend->starts.size() == 1U);
  }

  assets::MediaAsset other_identity = asset;
  other_identity.id = doc::NodeId{11U};
  auto other = service.request(other_identity);
  WAVE_CHECK(other);
  WAVE_CHECK(other.value().key.asset == other_identity.id);
  WAVE_CHECK(other.value().key != expected_key);
  static_cast<void>(wait_for_state(service, other.value().key,
                                   assets::DerivativeState::Ready));
}

void fingerprint_mismatches_are_stale_before_during_and_after_decode() {
  TemporaryDirectory temporary;

  {
    const fs::path source = temporary.path() / "before.bin";
    write_source(source, "revision one");
    const assets::MediaAsset asset = media_asset(doc::NodeId{20U}, source);
    const auto original_time = fs::last_write_time(source);
    write_source(source, "revision two");
    fs::last_write_time(source,
                        original_time + std::chrono::seconds{2});
    auto backend = std::make_shared<BackendState>();
    server::WaveformGenerationService service{
        cache_at(temporary.path() / "cache-before"),
        server::WaveformGenerationConfig{1U, 4U, 2U, 16U, 1'024U},
        factory(backend)};
    auto requested = service.request(asset);
    WAVE_CHECK(requested);
    const auto stale = wait_for_state(service, requested.value().key,
                                      assets::DerivativeState::Stale);
    WAVE_CHECK(stale.failure ==
               server::WaveformFailureCode::FingerprintMismatch);
    std::lock_guard lock{backend->mutex};
    WAVE_CHECK(backend->starts.empty());
  }

  {
    const fs::path source = temporary.path() / "consumed.bin";
    write_source(source, "stable revision");
    const assets::MediaAsset asset = media_asset(doc::NodeId{21U}, source);
    auto backend = std::make_shared<BackendState>();
    assets::ContentFingerprint wrong = *asset.fingerprint;
    ++wrong.byte_length;
    backend->reported_fingerprint = wrong;
    server::WaveformGenerationService service{
        cache_at(temporary.path() / "cache-consumed"),
        server::WaveformGenerationConfig{1U, 4U, 2U, 16U, 1'024U},
        factory(backend)};
    auto requested = service.request(asset);
    WAVE_CHECK(requested);
    const auto stale = wait_for_state(service, requested.value().key,
                                      assets::DerivativeState::Stale);
    WAVE_CHECK(stale.failure ==
               server::WaveformFailureCode::FingerprintMismatch);
    auto loaded = service.load(asset);
    WAVE_CHECK(!loaded);
    WAVE_CHECK(loaded.error().code ==
               assets::WaveformDerivativeErrorCode::NotReady);
  }

  {
    const fs::path source = temporary.path() / "after.bin";
    write_source(source, "stable before decode");
    const assets::MediaAsset asset = media_asset(doc::NodeId{22U}, source);
    auto backend = std::make_shared<BackendState>();
    backend->mutate_source = true;
    server::WaveformGenerationService service{
        cache_at(temporary.path() / "cache-after"),
        server::WaveformGenerationConfig{1U, 4U, 2U, 16U, 1'024U},
        factory(backend)};
    auto requested = service.request(asset);
    WAVE_CHECK(requested);
    const auto stale = wait_for_state(service, requested.value().key,
                                      assets::DerivativeState::Stale);
    WAVE_CHECK(stale.failure ==
               server::WaveformFailureCode::FingerprintMismatch);
  }
}

void invalid_cached_payload_is_never_reported_ready() {
  TemporaryDirectory temporary;
  const fs::path source = temporary.path() / "source.bin";
  write_source(source, "cache validation source");
  const assets::MediaAsset asset = media_asset(doc::NodeId{30U}, source);
  const assets::DerivativeKey key = waveform_key(asset);
  assets::DerivativeCache cache = cache_at(temporary.path() / "cache");
  constexpr std::array<core::u8, 4U> invalid_payload{{1U, 2U, 3U, 4U}};
  auto seeded = cache.publish(key, invalid_payload);
  WAVE_CHECK(seeded);
  WAVE_CHECK(seeded.value().state == assets::DerivativeState::Ready);

  auto backend = std::make_shared<BackendState>();
  backend->block_first = true;
  server::WaveformGenerationService service{
      std::move(cache),
      server::WaveformGenerationConfig{1U, 4U, 2U, 16U, 1'024U},
      factory(backend)};
  auto requested = service.request(asset);
  WAVE_CHECK(requested);
  WAVE_CHECK(requested.value().state == assets::DerivativeState::Stale);
  WAVE_CHECK(requested.value().failure ==
             server::WaveformFailureCode::InvalidCachedPayload);
  wait_for_first_call(backend);
  release_first_call(backend);
  static_cast<void>(
      wait_for_state(service, key, assets::DerivativeState::Ready));
  auto loaded = service.load(asset);
  WAVE_CHECK(loaded);
  WAVE_CHECK(loaded.value().entry.state == assets::DerivativeState::Ready);
}

void visible_work_evicts_prefetch_at_the_bounded_queue_limit() {
  TemporaryDirectory temporary;
  const fs::path first_path = temporary.path() / "first.bin";
  const fs::path second_path = temporary.path() / "second.bin";
  const fs::path third_path = temporary.path() / "third.bin";
  write_source(first_path, "first");
  write_source(second_path, "second");
  write_source(third_path, "third");
  const assets::MediaAsset first = media_asset(doc::NodeId{40U}, first_path);
  const assets::MediaAsset second = media_asset(doc::NodeId{41U}, second_path);
  const assets::MediaAsset third = media_asset(doc::NodeId{42U}, third_path);
  auto backend = std::make_shared<BackendState>();
  backend->block_first = true;
  server::WaveformGenerationService service{
      cache_at(temporary.path() / "cache"),
      server::WaveformGenerationConfig{1U, 1U, 1U, 8U, 1'024U},
      factory(backend)};

  auto first_request = service.request(first);
  WAVE_CHECK(first_request);
  wait_for_first_call(backend);
  auto second_request =
      service.request(second, server::WaveformRequestPriority::Prefetch);
  WAVE_CHECK(second_request);
  WAVE_CHECK(second_request.value().state == assets::DerivativeState::Queued);
  auto third_request =
      service.request(third, server::WaveformRequestPriority::Visible);
  WAVE_CHECK(third_request);
  WAVE_CHECK(third_request.value().state == assets::DerivativeState::Queued);
  const auto evicted = service.snapshot(second_request.value().key);
  WAVE_CHECK(evicted.has_value());
  WAVE_CHECK(evicted->state == assets::DerivativeState::Cancelled);
  WAVE_CHECK(evicted->failure == server::WaveformFailureCode::QueueCapacity);

  release_first_call(backend);
  static_cast<void>(wait_for_state(service, first_request.value().key,
                                   assets::DerivativeState::Ready));
  static_cast<void>(wait_for_state(service, third_request.value().key,
                                   assets::DerivativeState::Ready));
  {
    std::lock_guard lock{backend->mutex};
    WAVE_CHECK(backend->starts == std::vector<doc::NodeId>{first.id, third.id});
  }
}

void cancel_and_retain_stop_inflight_and_queued_work() {
  TemporaryDirectory temporary;
  const fs::path first_path = temporary.path() / "first.bin";
  const fs::path second_path = temporary.path() / "second.bin";
  write_source(first_path, "first retained source");
  write_source(second_path, "second retained source");
  const assets::MediaAsset first = media_asset(doc::NodeId{50U}, first_path);
  const assets::MediaAsset second = media_asset(doc::NodeId{51U}, second_path);
  auto backend = std::make_shared<BackendState>();
  backend->block_first = true;
  server::WaveformGenerationService service{
      cache_at(temporary.path() / "cache"),
      server::WaveformGenerationConfig{1U, 3U, 2U, 8U, 1'024U},
      factory(backend)};

  auto first_request = service.request(first);
  WAVE_CHECK(first_request);
  wait_for_first_call(backend);
  auto second_request = service.request(second);
  WAVE_CHECK(second_request);
  auto cancelled = service.cancel(second_request.value().key);
  WAVE_CHECK(cancelled.has_value());
  WAVE_CHECK(cancelled->state == assets::DerivativeState::Cancelled);

  // A fresh request for the cancelled key is a new queued job. Retain keeps
  // it and cancels the currently generating first job.
  second_request = service.request(second);
  WAVE_CHECK(second_request);
  const std::array retained{second_request.value().key};
  service.retain(retained);
  const auto first_cancelled = wait_for_state(
      service, first_request.value().key, assets::DerivativeState::Cancelled);
  WAVE_CHECK(first_cancelled.failure == server::WaveformFailureCode::None);
  static_cast<void>(wait_for_state(service, second_request.value().key,
                                   assets::DerivativeState::Ready));
}

void oversized_and_nonfinite_pcm_fail_without_publication() {
  TemporaryDirectory temporary;
  const fs::path source = temporary.path() / "invalid.bin";
  write_source(source, "invalid pcm source");
  const assets::MediaAsset asset = media_asset(doc::NodeId{60U}, source);

  {
    auto backend = std::make_shared<BackendState>();
    backend->samples = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    server::WaveformGenerationService service{
        cache_at(temporary.path() / "cache-large"),
        server::WaveformGenerationConfig{1U, 2U, 1U, 8U, 4U}, factory(backend)};
    auto requested = service.request(asset);
    WAVE_CHECK(requested);
    const auto failed = wait_for_state(service, requested.value().key,
                                       assets::DerivativeState::Failed);
    WAVE_CHECK(failed.failure == server::WaveformFailureCode::InvalidPcm);
  }

  {
    auto backend = std::make_shared<BackendState>();
    backend->samples = {0.0F, std::numeric_limits<float>::quiet_NaN()};
    server::WaveformGenerationService service{
        cache_at(temporary.path() / "cache-nan"),
        server::WaveformGenerationConfig{1U, 2U, 1U, 8U, 4U}, factory(backend)};
    auto requested = service.request(asset);
    WAVE_CHECK(requested);
    const auto failed = wait_for_state(service, requested.value().key,
                                       assets::DerivativeState::Failed);
    WAVE_CHECK(failed.failure == server::WaveformFailureCode::InvalidPcm);
    WAVE_CHECK(failed.waveform_error == assets::WaveformError::NonFiniteSample);
  }
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 7U> cases{{
    {"invalid inputs and production backend fail closed",
     &invalid_inputs_and_production_backend_fail_closed},
    {"successful generation uses exact identity and validates cache",
     &successful_generation_uses_exact_identity_and_validates_cache},
    {"fingerprint mismatches are stale before during and after decode",
     &fingerprint_mismatches_are_stale_before_during_and_after_decode},
    {"invalid cached payload is never reported ready",
     &invalid_cached_payload_is_never_reported_ready},
    {"visible work evicts prefetch at the bounded queue limit",
     &visible_work_evicts_prefetch_at_the_bounded_queue_limit},
    {"cancel and retain stop inflight and queued work",
     &cancel_and_retain_stop_inflight_and_queued_work},
    {"oversized and nonfinite PCM fail without publication",
     &oversized_and_nonfinite_pcm_fail_without_publication},
}};

} // namespace

int main() {
  std::size_t failures{};
  for (const Case &test : cases) {
    try {
      test.function();
      std::cout << "[pass] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
    }
  }
  std::cout << cases.size() - failures << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
