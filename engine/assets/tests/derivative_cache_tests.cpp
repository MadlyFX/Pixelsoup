#include <mediaserver/assets/derivative_cache.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace fs = std::filesystem;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} +
                                 std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define CACHE_CHECK(...)                                                     \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<core::u64> sequence{};
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch().count();
        path_ = fs::temp_directory_path() /
            ("mediaserver_derivative_cache_" + std::to_string(stamp) + "_" +
             std::to_string(sequence.fetch_add(1U)));
        std::error_code filesystem_error;
        if (!fs::create_directory(path_, filesystem_error) ||
            filesystem_error) {
            throw std::runtime_error{"could not create temporary directory"};
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        static_cast<void>(fs::remove_all(path_, ignored));
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

[[nodiscard]] std::vector<core::u8> bytes(std::string_view value) {
    return std::vector<core::u8>{value.begin(), value.end()};
}

[[nodiscard]] assets::ContentFingerprint fingerprint(
    std::string_view content) {
    const auto content_bytes = bytes(content);
    return assets::ContentFingerprint{
        static_cast<core::u64>(content_bytes.size()),
        assets::sha256_digest(content_bytes),
    };
}

[[nodiscard]] assets::DerivativeKey key(core::u64 asset,
                                        std::string_view revision) {
    const auto* profile = assets::derivative_profile(
        assets::DerivativeProfileId::Thumbnail320x180Srgb);
    if (profile == nullptr) {
        throw std::runtime_error{"missing derivative profile"};
    }
    return assets::make_derivative_key(doc::NodeId{asset},
                                       fingerprint(revision), *profile);
}

[[nodiscard]] assets::DerivativeCache open_cache(const fs::path& path,
                                                  core::u64 maximum_bytes) {
    auto opened = assets::DerivativeCache::open({path, maximum_bytes});
    if (!opened) {
        throw std::runtime_error{"could not open derivative cache"};
    }
    return std::move(opened).value();
}

void write_file(const fs::path& path, std::span<const core::u8> content) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(content.data()),
                 static_cast<std::streamsize>(content.size()));
    output.close();
    if (!output) {
        throw std::runtime_error{"could not write fixture"};
    }
}

void publish_load_and_restart_validate_the_envelope() {
    TemporaryDirectory temporary;
    const fs::path root = temporary.path() / "cache";
    const auto derivative_key = key(1U, "revision-one");
    const auto payload = bytes("thumbnail-payload");
    {
        auto cache = open_cache(root, 4'096U);
        const auto published = cache.publish(derivative_key, payload);
        CACHE_CHECK(published);
        CACHE_CHECK(published.value().state == assets::DerivativeState::Ready);
        CACHE_CHECK(fs::exists(published.value().path));
        const auto loaded = cache.load(derivative_key, payload.size());
        CACHE_CHECK(loaded);
        CACHE_CHECK(loaded.value().entry.state ==
                    assets::DerivativeState::Ready);
        CACHE_CHECK(loaded.value().payload == payload);
        CACHE_CHECK(cache.stats().entry_count == 1U);
    }
    {
        auto restarted = open_cache(root, 4'096U);
        CACHE_CHECK(restarted.stats().entry_count == 1U);
        const auto loaded = restarted.load(derivative_key, payload.size());
        CACHE_CHECK(loaded);
        CACHE_CHECK(loaded.value().payload == payload);
    }
}

void corruption_is_stale_not_a_cache_hit() {
    TemporaryDirectory temporary;
    auto cache = open_cache(temporary.path() / "cache", 4'096U);
    const auto derivative_key = key(2U, "revision-two");
    const auto payload = bytes("payload");
    const auto published = cache.publish(derivative_key, payload);
    CACHE_CHECK(published);

    std::fstream file{published.value().path,
                      std::ios::binary | std::ios::in | std::ios::out};
    file.seekp(96, std::ios::beg);
    const char replacement = 'X';
    file.write(&replacement, 1);
    file.close();
    CACHE_CHECK(file);

    const auto stale = cache.inspect(derivative_key);
    CACHE_CHECK(stale);
    CACHE_CHECK(stale.value().state == assets::DerivativeState::Stale);
    CACHE_CHECK(!fs::exists(published.value().path));
    const auto missing = cache.inspect(derivative_key);
    CACHE_CHECK(missing);
    CACHE_CHECK(missing.value().state == assets::DerivativeState::Missing);
}

void cancellation_preserves_the_last_atomic_value_and_cleans_temps() {
    TemporaryDirectory temporary;
    auto cache = open_cache(temporary.path() / "cache", 4'096U);
    const auto derivative_key = key(3U, "revision-three");
    const auto original = bytes("original");
    CACHE_CHECK(cache.publish(derivative_key, original));

    std::stop_source cancellation;
    cancellation.request_stop();
    const auto cancelled = cache.publish(
        derivative_key, bytes("replacement"), false,
        cancellation.get_token());
    CACHE_CHECK(cancelled);
    CACHE_CHECK(cancelled.value().state ==
                assets::DerivativeState::Cancelled);
    const auto loaded = cache.load(derivative_key, 64U);
    CACHE_CHECK(loaded);
    CACHE_CHECK(loaded.value().payload == original);

    for (const fs::directory_entry& item :
         fs::recursive_directory_iterator{cache.root()}) {
        CACHE_CHECK(item.path().filename().string().find(".tmp.") ==
                    std::string::npos);
    }
}

[[nodiscard]] bool temporary_entry_exists(const fs::path& root) noexcept {
    std::error_code filesystem_error;
    fs::recursive_directory_iterator iterator{
        root, fs::directory_options::skip_permission_denied, filesystem_error};
    const fs::recursive_directory_iterator end;
    while (!filesystem_error && iterator != end) {
        if (iterator->path().filename().string().find(".tmp.") !=
            std::string::npos) {
            return true;
        }
        iterator.increment(filesystem_error);
    }
    return false;
}

void cancellation_after_staging_removes_the_partial_file() {
    TemporaryDirectory temporary;
    const fs::path root = temporary.path() / "cache";
    auto cache = open_cache(root, 64U * 1'024U * 1'024U);
    const auto derivative_key = key(4U, "staged-cancellation");
    const std::vector<core::u8> payload(32U * 1'024U * 1'024U, 0x5aU);
    std::stop_source cancellation;
    std::atomic_bool finished{};
    std::atomic_bool observed_temporary{};
    std::jthread canceller{[&] {
        while (!finished.load()) {
            if (temporary_entry_exists(root)) {
                observed_temporary.store(true);
                cancellation.request_stop();
                return;
            }
            std::this_thread::yield();
        }
    }};

    const auto cancelled = cache.publish(
        derivative_key, payload, false, cancellation.get_token());
    finished.store(true);
    canceller.join();
    CACHE_CHECK(observed_temporary.load());
    CACHE_CHECK(cancelled);
    CACHE_CHECK(cancelled.value().state ==
                assets::DerivativeState::Cancelled);
    CACHE_CHECK(!fs::exists(
        assets::derivative_cache_path(cache.root(), derivative_key)));
    CACHE_CHECK(!temporary_entry_exists(root));
}

void lru_eviction_touches_hits_and_is_deterministic() {
    TemporaryDirectory temporary;
    // Each four-byte payload occupies 96 envelope bytes plus four payload
    // bytes, so exactly two entries fit.
    auto cache = open_cache(temporary.path() / "cache", 200U);
    const auto first = key(10U, "one");
    const auto second = key(11U, "two");
    const auto third = key(12U, "three");
    const auto payload = bytes("data");
    CACHE_CHECK(cache.publish(first, payload));
    CACHE_CHECK(cache.publish(second, payload));
    CACHE_CHECK(cache.inspect(first));
    CACHE_CHECK(cache.publish(third, payload));

    CACHE_CHECK(cache.inspect(first).value().state ==
                assets::DerivativeState::Ready);
    CACHE_CHECK(cache.inspect(second).value().state ==
                assets::DerivativeState::Missing);
    CACHE_CHECK(cache.inspect(third).value().state ==
                assets::DerivativeState::Ready);
    CACHE_CHECK(cache.stats().resident_bytes == 200U);
}

void pins_may_exceed_budget_then_unpin_reclaims_lru() {
    TemporaryDirectory temporary;
    auto cache = open_cache(temporary.path() / "cache", 100U);
    const auto first = key(20U, "one");
    const auto second = key(21U, "two");
    const auto payload = bytes("data");
    CACHE_CHECK(cache.publish(first, payload, true));
    CACHE_CHECK(cache.publish(second, payload, true));
    CACHE_CHECK(cache.stats().resident_bytes == 200U);
    CACHE_CHECK(cache.stats().over_budget);
    CACHE_CHECK(cache.stats().pinned_entries == 2U);

    const auto unpinned = cache.unpin(first);
    CACHE_CHECK(unpinned);
    CACHE_CHECK(unpinned.value().resident_bytes == 100U);
    CACHE_CHECK(!unpinned.value().over_budget);
    CACHE_CHECK(cache.inspect(first).value().state ==
                assets::DerivativeState::Missing);
    CACHE_CHECK(cache.inspect(second).value().state ==
                assets::DerivativeState::Ready);
    CACHE_CHECK(cache.unpin(second));
}

void file_publish_streams_and_bounded_load_refuses_large_payloads() {
    TemporaryDirectory temporary;
    const auto payload = bytes("proxy-file-payload");
    const fs::path source = temporary.path() / "generator-output.bin";
    write_file(source, payload);
    auto cache = open_cache(temporary.path() / "cache", 4'096U);
    const auto derivative_key = key(30U, "proxy-revision");
    const auto published = cache.publish_file(derivative_key, source);
    CACHE_CHECK(published);
    CACHE_CHECK(published.value().state == assets::DerivativeState::Ready);
    CACHE_CHECK(fs::exists(source));

    const auto bounded = cache.load(derivative_key, payload.size() - 1U);
    CACHE_CHECK(!bounded);
    CACHE_CHECK(bounded.error().code ==
                assets::DerivativeCacheErrorCode::PayloadTooLarge);
    const auto loaded = cache.load(derivative_key, payload.size());
    CACHE_CHECK(loaded);
    CACHE_CHECK(loaded.value().payload == payload);
}

void invalid_configuration_and_keys_are_rejected() {
    const auto relative = assets::DerivativeCache::open(
        {fs::path{"relative-cache"}, 1'024U});
    CACHE_CHECK(!relative);
    CACHE_CHECK(relative.error().code ==
                assets::DerivativeCacheErrorCode::InvalidRoot);

    TemporaryDirectory temporary;
    auto cache = open_cache(temporary.path() / "cache", 1'024U);
    assets::DerivativeKey invalid{};
    CACHE_CHECK(!cache.inspect(invalid));
    CACHE_CHECK(cache.inspect(invalid).error().code ==
                assets::DerivativeCacheErrorCode::InvalidKey);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 8U> cases{{
    {"publish load and restart validate the envelope",
     &publish_load_and_restart_validate_the_envelope},
    {"corruption is stale not a cache hit",
     &corruption_is_stale_not_a_cache_hit},
    {"cancellation preserves the last atomic value and cleans temps",
     &cancellation_preserves_the_last_atomic_value_and_cleans_temps},
    {"cancellation after staging removes the partial file",
     &cancellation_after_staging_removes_the_partial_file},
    {"LRU eviction touches hits and is deterministic",
     &lru_eviction_touches_hits_and_is_deterministic},
    {"pins may exceed budget then unpin reclaims LRU",
     &pins_may_exceed_budget_then_unpin_reclaims_lru},
    {"file publish streams and bounded load refuses large payloads",
     &file_publish_streams_and_bounded_load_refuses_large_payloads},
    {"invalid configuration and keys are rejected",
     &invalid_configuration_and_keys_are_rejected},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& caught) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << caught.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
