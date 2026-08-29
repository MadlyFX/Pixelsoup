#include <mediaserver/assets/derivative_cache.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace mediaserver::assets {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t envelope_size = 96U;
constexpr core::u32 envelope_version = 1U;
constexpr std::array<core::u8, 8U> envelope_magic{{
    'M', 'S', 'D', 'E', 'R', 'I', 'V', '1',
}};
constexpr std::size_t io_chunk_size = 64U * 1'024U;

[[nodiscard]] DerivativeCacheError error(
    DerivativeCacheErrorCode code,
    std::error_code system_error = {}) noexcept {
    return DerivativeCacheError{code, system_error};
}

void write_u32(std::array<core::u8, envelope_size>& bytes,
               std::size_t offset, core::u32 value) noexcept {
    bytes[offset] = static_cast<core::u8>((value >> 24U) & 0xffU);
    bytes[offset + 1U] = static_cast<core::u8>((value >> 16U) & 0xffU);
    bytes[offset + 2U] = static_cast<core::u8>((value >> 8U) & 0xffU);
    bytes[offset + 3U] = static_cast<core::u8>(value & 0xffU);
}

void write_u64(std::array<core::u8, envelope_size>& bytes,
               std::size_t offset, core::u64 value) noexcept {
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[offset + index] = static_cast<core::u8>(
            (value >> static_cast<core::u32>((7U - index) * 8U)) & 0xffU);
    }
}

[[nodiscard]] core::u32 read_u32(
    const std::array<core::u8, envelope_size>& bytes,
    std::size_t offset) noexcept {
    return (static_cast<core::u32>(bytes[offset]) << 24U) |
        (static_cast<core::u32>(bytes[offset + 1U]) << 16U) |
        (static_cast<core::u32>(bytes[offset + 2U]) << 8U) |
        static_cast<core::u32>(bytes[offset + 3U]);
}

[[nodiscard]] core::u64 read_u64(
    const std::array<core::u8, envelope_size>& bytes,
    std::size_t offset) noexcept {
    core::u64 value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
    return value;
}

struct Envelope final {
    DerivativeProfileId profile{};
    Sha256Digest key_digest;
    Sha256Digest payload_digest;
    core::u64 payload_bytes{};
};

[[nodiscard]] std::array<core::u8, envelope_size> make_envelope(
    const DerivativeKey& key, const Sha256Digest& payload_digest,
    core::u64 payload_bytes) noexcept {
    std::array<core::u8, envelope_size> bytes{};
    std::copy(envelope_magic.begin(), envelope_magic.end(), bytes.begin());
    write_u32(bytes, 8U, envelope_version);
    write_u32(bytes, 12U, static_cast<core::u32>(envelope_size));
    write_u32(bytes, 16U, static_cast<core::u32>(key.profile));
    write_u32(bytes, 20U, 0U);
    std::copy(key.digest.bytes.begin(), key.digest.bytes.end(),
              bytes.begin() + 24);
    std::copy(payload_digest.bytes.begin(), payload_digest.bytes.end(),
              bytes.begin() + 56);
    write_u64(bytes, 88U, payload_bytes);
    return bytes;
}

[[nodiscard]] std::optional<Envelope> parse_envelope(
    const std::array<core::u8, envelope_size>& bytes) noexcept {
    if (!std::equal(envelope_magic.begin(), envelope_magic.end(),
                    bytes.begin()) ||
        read_u32(bytes, 8U) != envelope_version ||
        read_u32(bytes, 12U) != envelope_size ||
        read_u32(bytes, 20U) != 0U) {
        return std::nullopt;
    }
    const core::u32 raw_profile = read_u32(bytes, 16U);
    if (raw_profile > std::numeric_limits<core::u16>::max()) {
        return std::nullopt;
    }
    const auto profile = static_cast<DerivativeProfileId>(raw_profile);
    if (derivative_profile(profile) == nullptr) {
        return std::nullopt;
    }
    Envelope envelope{};
    envelope.profile = profile;
    std::copy_n(bytes.begin() + 24, envelope.key_digest.bytes.size(),
                envelope.key_digest.bytes.begin());
    std::copy_n(bytes.begin() + 56, envelope.payload_digest.bytes.size(),
                envelope.payload_digest.bytes.begin());
    envelope.payload_bytes = read_u64(bytes, 88U);
    return envelope;
}

enum class ValidationCode : core::u8 {
    Ready,
    Missing,
    Stale,
    Failed,
    Cancelled,
    PayloadTooLarge,
    AllocationFailed,
};

struct Validation final {
    ValidationCode code{ValidationCode::Failed};
    Envelope envelope;
    core::u64 disk_bytes{};
    std::error_code system_error;
};

[[nodiscard]] Validation validate_entry(
    const fs::path& path, const DerivativeKey* expected_key,
    bool verify_payload, std::vector<core::u8>* loaded_payload,
    core::u64 maximum_payload_bytes, std::stop_token stop) noexcept {
    try {
        if (stop.stop_requested()) {
            return Validation{ValidationCode::Cancelled};
        }
        std::error_code filesystem_error;
        const bool exists = fs::exists(path, filesystem_error);
        if (!exists) {
            if (!filesystem_error || filesystem_error ==
                    std::errc::no_such_file_or_directory) {
                return Validation{ValidationCode::Missing};
            }
            return Validation{ValidationCode::Failed, {}, 0U,
                              filesystem_error};
        }
        if (filesystem_error) {
            return Validation{ValidationCode::Failed, {}, 0U,
                              filesystem_error};
        }
        if (!fs::is_regular_file(path, filesystem_error) || filesystem_error) {
            return Validation{ValidationCode::Stale, {}, 0U,
                              filesystem_error};
        }
        const std::uintmax_t raw_disk_bytes =
            fs::file_size(path, filesystem_error);
        if (filesystem_error) {
            return Validation{ValidationCode::Failed, {}, 0U,
                              filesystem_error};
        }
        if (raw_disk_bytes > std::numeric_limits<core::u64>::max()) {
            return Validation{ValidationCode::Stale};
        }
        const core::u64 disk_bytes = static_cast<core::u64>(raw_disk_bytes);
        if (disk_bytes < envelope_size) {
            return Validation{ValidationCode::Stale, {}, disk_bytes};
        }

        std::ifstream input{path, std::ios::binary};
        if (!input) {
            return Validation{ValidationCode::Failed, {}, disk_bytes};
        }
        std::array<core::u8, envelope_size> header{};
        input.read(reinterpret_cast<char*>(header.data()),
                   static_cast<std::streamsize>(header.size()));
        if (input.gcount() != static_cast<std::streamsize>(header.size())) {
            return Validation{input.bad() ? ValidationCode::Failed
                                          : ValidationCode::Stale,
                              {}, disk_bytes};
        }
        const std::optional<Envelope> parsed = parse_envelope(header);
        if (!parsed.has_value()) {
            return Validation{ValidationCode::Stale, {}, disk_bytes};
        }
        const Envelope envelope = *parsed;
        if (envelope.payload_bytes >
            std::numeric_limits<core::u64>::max() - envelope_size ||
            envelope.payload_bytes + envelope_size != disk_bytes) {
            return Validation{ValidationCode::Stale, envelope, disk_bytes};
        }
        if (expected_key != nullptr &&
            (envelope.profile != expected_key->profile ||
             envelope.key_digest != expected_key->digest)) {
            return Validation{ValidationCode::Stale, envelope, disk_bytes};
        }
        if (loaded_payload != nullptr &&
            envelope.payload_bytes > maximum_payload_bytes) {
            return Validation{ValidationCode::PayloadTooLarge, envelope,
                              disk_bytes};
        }
        if (!verify_payload) {
            return Validation{ValidationCode::Ready, envelope, disk_bytes};
        }
        if (loaded_payload != nullptr) {
            if (envelope.payload_bytes > loaded_payload->max_size()) {
                return Validation{ValidationCode::AllocationFailed, envelope,
                                  disk_bytes};
            }
            loaded_payload->resize(
                static_cast<std::size_t>(envelope.payload_bytes));
        }

        Sha256Hasher hash;
        std::array<core::u8, io_chunk_size> buffer{};
        core::u64 remaining = envelope.payload_bytes;
        std::size_t output_offset = 0U;
        while (remaining != 0U) {
            if (stop.stop_requested()) {
                if (loaded_payload != nullptr) {
                    loaded_payload->clear();
                }
                return Validation{ValidationCode::Cancelled, envelope,
                                  disk_bytes};
            }
            const std::size_t amount = static_cast<std::size_t>(
                std::min<core::u64>(remaining, buffer.size()));
            input.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(amount));
            if (input.gcount() != static_cast<std::streamsize>(amount)) {
                if (loaded_payload != nullptr) {
                    loaded_payload->clear();
                }
                return Validation{input.bad() ? ValidationCode::Failed
                                              : ValidationCode::Stale,
                                  envelope, disk_bytes};
            }
            hash.update(std::span<const core::u8>{buffer.data(), amount});
            if (loaded_payload != nullptr) {
                std::copy_n(buffer.begin(), amount,
                            loaded_payload->begin() +
                                static_cast<std::ptrdiff_t>(output_offset));
                output_offset += amount;
            }
            remaining -= static_cast<core::u64>(amount);
        }
        if (hash.finish() != envelope.payload_digest) {
            if (loaded_payload != nullptr) {
                loaded_payload->clear();
            }
            return Validation{ValidationCode::Stale, envelope, disk_bytes};
        }
        return Validation{ValidationCode::Ready, envelope, disk_bytes};
    } catch (const std::bad_alloc&) {
        if (loaded_payload != nullptr) {
            loaded_payload->clear();
        }
        return Validation{ValidationCode::AllocationFailed};
    } catch (const fs::filesystem_error& caught) {
        if (loaded_payload != nullptr) {
            loaded_payload->clear();
        }
        return Validation{ValidationCode::Failed, {}, 0U, caught.code()};
    } catch (...) {
        if (loaded_payload != nullptr) {
            loaded_payload->clear();
        }
        return Validation{ValidationCode::Failed};
    }
}

[[nodiscard]] fs::path unique_temporary_path(const fs::path& target) {
    static std::atomic<core::u64> sequence{};
    const core::u64 number = sequence.fetch_add(1U) + 1U;
    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch().count();
#if defined(_WIN32)
    const std::wstring suffix = L".tmp." +
        std::to_wstring(static_cast<core::u64>(GetCurrentProcessId())) + L"." +
        std::to_wstring(static_cast<core::u64>(stamp)) + L"." +
        std::to_wstring(number);
    return fs::path{target.native() + suffix};
#else
    const std::string suffix = ".tmp." +
        std::to_string(static_cast<core::u64>(stamp)) + "." +
        std::to_string(number);
    return fs::path{target.native() + suffix};
#endif
}

class TemporaryFile final {
public:
    explicit TemporaryFile(fs::path path) : path_(std::move(path)) {}
    ~TemporaryFile() {
        if (!published_) {
            std::error_code ignored;
            static_cast<void>(fs::remove(path_, ignored));
        }
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
    void published() noexcept { published_ = true; }

private:
    fs::path path_;
    bool published_{};
};

[[nodiscard]] std::error_code atomic_replace(
    const fs::path& temporary, const fs::path& target) noexcept {
#if defined(_WIN32)
    if (MoveFileExW(temporary.c_str(), target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return {};
    }
    return std::error_code{static_cast<int>(GetLastError()),
                           std::system_category()};
#else
    std::error_code filesystem_error;
    fs::rename(temporary, target, filesystem_error);
    return filesystem_error;
#endif
}

[[nodiscard]] DerivativeCacheEntry entry_for_validation(
    const fs::path& path, const Validation& validation) {
    DerivativeState state = DerivativeState::Failed;
    switch (validation.code) {
    case ValidationCode::Ready:
        state = DerivativeState::Ready;
        break;
    case ValidationCode::Missing:
        state = DerivativeState::Missing;
        break;
    case ValidationCode::Stale:
        state = DerivativeState::Stale;
        break;
    case ValidationCode::Failed:
    case ValidationCode::PayloadTooLarge:
    case ValidationCode::AllocationFailed:
        state = DerivativeState::Failed;
        break;
    case ValidationCode::Cancelled:
        state = DerivativeState::Cancelled;
        break;
    }
    return DerivativeCacheEntry{state, path,
                                validation.envelope.payload_bytes,
                                validation.disk_bytes, 0U,
                                validation.system_error};
}

} // namespace

struct DerivativeCache::Impl final {
    struct Metadata final {
        fs::path path;
        core::u64 payload_bytes{};
        core::u64 disk_bytes{};
        core::u64 last_used{};
        core::u32 pins{};
    };

    explicit Impl(DerivativeCacheConfig value)
        : config(std::move(value)) {}

    [[nodiscard]] DerivativeCacheStats stats_locked() const noexcept {
        core::u64 pinned = 0U;
        for (const auto& pair : entries) {
            if (pair.second.pins != 0U) {
                ++pinned;
            }
        }
        return DerivativeCacheStats{
            config.maximum_bytes,
            resident_bytes,
            static_cast<core::u64>(entries.size()),
            pinned,
            resident_bytes > config.maximum_bytes,
        };
    }

    void erase_locked(const std::string& digest) noexcept {
        const auto found = entries.find(digest);
        if (found == entries.end()) {
            return;
        }
        resident_bytes -= found->second.disk_bytes;
        entries.erase(found);
    }

    Metadata& upsert_locked(const std::string& digest, const fs::path& path,
                            core::u64 payload_bytes, core::u64 disk_bytes) {
        const auto found = entries.find(digest);
        core::u32 pins = 0U;
        if (found != entries.end()) {
            pins = found->second.pins;
            resident_bytes -= found->second.disk_bytes;
        }
        Metadata value{path, payload_bytes, disk_bytes, ++clock, pins};
        const auto result = entries.insert_or_assign(digest, std::move(value));
        resident_bytes += disk_bytes;
        return result.first->second;
    }

    [[nodiscard]] std::optional<DerivativeCacheError> trim_locked() noexcept {
        while (resident_bytes > config.maximum_bytes) {
            auto oldest = entries.end();
            for (auto candidate = entries.begin(); candidate != entries.end();
                 ++candidate) {
                if (candidate->second.pins != 0U) {
                    continue;
                }
                if (oldest == entries.end() ||
                    candidate->second.last_used < oldest->second.last_used ||
                    (candidate->second.last_used == oldest->second.last_used &&
                     candidate->second.path.generic_string() <
                         oldest->second.path.generic_string())) {
                    oldest = candidate;
                }
            }
            if (oldest == entries.end()) {
                break;
            }
            std::error_code filesystem_error;
            const bool removed = fs::remove(oldest->second.path,
                                            filesystem_error);
            if (filesystem_error) {
                return error(DerivativeCacheErrorCode::RemoveFailed,
                             filesystem_error);
            }
            if (!removed) {
                // An external cleaner already reclaimed it; the index still
                // must release its accounting.
            }
            resident_bytes -= oldest->second.disk_bytes;
            entries.erase(oldest);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<DerivativeCacheError> scan_locked() noexcept {
        try {
            struct Candidate final {
                std::string digest;
                Metadata metadata;
                fs::file_time_type modified;
            };
            std::vector<Candidate> candidates;
            std::error_code filesystem_error;
            fs::recursive_directory_iterator iterator{
                config.root, fs::directory_options::skip_permission_denied,
                filesystem_error};
            const fs::recursive_directory_iterator end;
            if (filesystem_error) {
                return error(DerivativeCacheErrorCode::OpenFailed,
                             filesystem_error);
            }
            while (iterator != end) {
                const fs::directory_entry item = *iterator;
                iterator.increment(filesystem_error);
                if (filesystem_error) {
                    return error(DerivativeCacheErrorCode::OpenFailed,
                                 filesystem_error);
                }
                if (!item.is_regular_file(filesystem_error)) {
                    if (filesystem_error) {
                        filesystem_error.clear();
                    }
                    continue;
                }
                const fs::path path = item.path();
                if (path.extension() != ".msderiv") {
                    continue;
                }
                const std::string encoded = path.stem().string();
                const std::optional<Sha256Digest> filename_digest =
                    sha256_from_hex(encoded);
                const Validation validation = validate_entry(
                    path, nullptr, false, nullptr, 0U, {});
                bool accepted = filename_digest.has_value() &&
                    validation.code == ValidationCode::Ready &&
                    validation.envelope.key_digest == *filename_digest;
                if (accepted) {
                    DerivativeKey path_key{};
                    path_key.profile = validation.envelope.profile;
                    path_key.digest = validation.envelope.key_digest;
                    accepted = path.lexically_normal() ==
                        derivative_cache_path(config.root, path_key)
                            .lexically_normal();
                }
                if (!accepted) {
                    std::error_code ignored;
                    static_cast<void>(fs::remove(path, ignored));
                    continue;
                }
                const fs::file_time_type modified =
                    fs::last_write_time(path, filesystem_error);
                if (filesystem_error) {
                    return error(DerivativeCacheErrorCode::OpenFailed,
                                 filesystem_error);
                }
                candidates.push_back(Candidate{
                    encoded,
                    Metadata{path, validation.envelope.payload_bytes,
                             validation.disk_bytes, 0U, 0U},
                    modified,
                });
            }
            std::sort(candidates.begin(), candidates.end(),
                      [](const Candidate& left, const Candidate& right) {
                          if (left.modified != right.modified) {
                              return left.modified < right.modified;
                          }
                          return left.metadata.path.generic_string() <
                              right.metadata.path.generic_string();
                      });
            for (Candidate& candidate : candidates) {
                static_cast<void>(upsert_locked(
                    candidate.digest, candidate.metadata.path,
                    candidate.metadata.payload_bytes,
                    candidate.metadata.disk_bytes));
            }
            return trim_locked();
        } catch (const std::bad_alloc&) {
            return error(DerivativeCacheErrorCode::AllocationFailed);
        } catch (const fs::filesystem_error& caught) {
            return error(DerivativeCacheErrorCode::OpenFailed, caught.code());
        } catch (...) {
            return error(DerivativeCacheErrorCode::OpenFailed);
        }
    }

    DerivativeCacheConfig config;
    mutable std::mutex mutex;
    std::unordered_map<std::string, Metadata> entries;
    core::u64 resident_bytes{};
    core::u64 clock{};
};

DerivativeCache::DerivativeCache(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

DerivativeCache::~DerivativeCache() = default;
DerivativeCache::DerivativeCache(DerivativeCache&&) noexcept = default;
DerivativeCache& DerivativeCache::operator=(DerivativeCache&&) noexcept =
    default;

core::Result<DerivativeCache, DerivativeCacheError>
DerivativeCache::open(DerivativeCacheConfig config) noexcept {
    try {
        if (config.root.empty() || !config.root.is_absolute()) {
            return core::Result<DerivativeCache, DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::InvalidRoot));
        }
        if (config.maximum_bytes < envelope_size) {
            return core::Result<DerivativeCache, DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::InvalidCapacity));
        }
        config.root = config.root.lexically_normal();
        std::error_code filesystem_error;
        static_cast<void>(fs::create_directories(config.root,
                                                 filesystem_error));
        if (filesystem_error ||
            !fs::is_directory(config.root, filesystem_error) ||
            filesystem_error) {
            return core::Result<DerivativeCache, DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::CreateDirectoryFailed,
                      filesystem_error));
        }
        auto implementation = std::make_unique<Impl>(std::move(config));
        if (const auto scan_error = implementation->scan_locked();
            scan_error.has_value()) {
            return core::Result<DerivativeCache, DerivativeCacheError>::failure(
                *scan_error);
        }
        return core::Result<DerivativeCache, DerivativeCacheError>::success(
            DerivativeCache{std::move(implementation)});
    } catch (const std::bad_alloc&) {
        return core::Result<DerivativeCache, DerivativeCacheError>::failure(
            error(DerivativeCacheErrorCode::AllocationFailed));
    } catch (const fs::filesystem_error& caught) {
        return core::Result<DerivativeCache, DerivativeCacheError>::failure(
            error(DerivativeCacheErrorCode::OpenFailed, caught.code()));
    } catch (...) {
        return core::Result<DerivativeCache, DerivativeCacheError>::failure(
            error(DerivativeCacheErrorCode::OpenFailed));
    }
}

core::Result<DerivativeCacheEntry, DerivativeCacheError>
DerivativeCache::inspect(const DerivativeKey& key, bool pin) noexcept {
    try {
        if (!valid_derivative_key(key)) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::InvalidKey));
        }
        std::scoped_lock lock{implementation_->mutex};
        const fs::path target = derivative_cache_path(
            implementation_->config.root, key);
        const std::string digest = derivative_key_string(key);
        const Validation validation = validate_entry(
            target, &key, true, nullptr, 0U, {});
        DerivativeCacheEntry entry = entry_for_validation(target, validation);
        if (validation.code == ValidationCode::Ready) {
            Impl::Metadata& metadata = implementation_->upsert_locked(
                digest, target, validation.envelope.payload_bytes,
                validation.disk_bytes);
            if (pin) {
                if (metadata.pins == std::numeric_limits<core::u32>::max()) {
                    return core::Result<DerivativeCacheEntry,
                                        DerivativeCacheError>::failure(
                        error(DerivativeCacheErrorCode::PinLimitExceeded));
                }
                ++metadata.pins;
            }
            if (const auto trim_error = implementation_->trim_locked();
                trim_error.has_value()) {
                return core::Result<DerivativeCacheEntry,
                                    DerivativeCacheError>::failure(*trim_error);
            }
            const auto found = implementation_->entries.find(digest);
            if (found == implementation_->entries.end()) {
                entry.state = DerivativeState::Missing;
                entry.payload_bytes = 0U;
                entry.disk_bytes = 0U;
            } else {
                entry.pin_count = found->second.pins;
            }
        } else if (validation.code == ValidationCode::Missing ||
                   validation.code == ValidationCode::Stale) {
            implementation_->erase_locked(digest);
            if (validation.code == ValidationCode::Stale) {
                std::error_code ignored;
                static_cast<void>(fs::remove(target, ignored));
            }
        }
        if (validation.code == ValidationCode::AllocationFailed) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::AllocationFailed));
        }
        return core::Result<DerivativeCacheEntry,
                            DerivativeCacheError>::success(std::move(entry));
    } catch (const std::bad_alloc&) {
        return core::Result<DerivativeCacheEntry,
                            DerivativeCacheError>::failure(
            error(DerivativeCacheErrorCode::AllocationFailed));
    } catch (...) {
        return core::Result<DerivativeCacheEntry,
                            DerivativeCacheError>::failure(
            error(DerivativeCacheErrorCode::ReadFailed));
    }
}

core::Result<DerivativeCacheLoad, DerivativeCacheError>
DerivativeCache::load(const DerivativeKey& key,
                      core::u64 maximum_payload_bytes, bool pin,
                      std::stop_token stop) noexcept {
    try {
        if (!valid_derivative_key(key)) {
            return core::Result<DerivativeCacheLoad,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::InvalidKey));
        }
        std::scoped_lock lock{implementation_->mutex};
        const fs::path target = derivative_cache_path(
            implementation_->config.root, key);
        const std::string digest = derivative_key_string(key);
        std::vector<core::u8> payload;
        const Validation validation = validate_entry(
            target, &key, true, &payload, maximum_payload_bytes, stop);
        if (validation.code == ValidationCode::PayloadTooLarge) {
            return core::Result<DerivativeCacheLoad,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::PayloadTooLarge));
        }
        if (validation.code == ValidationCode::AllocationFailed) {
            return core::Result<DerivativeCacheLoad,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::AllocationFailed));
        }
        DerivativeCacheEntry entry = entry_for_validation(target, validation);
        if (validation.code == ValidationCode::Ready) {
            Impl::Metadata& metadata = implementation_->upsert_locked(
                digest, target, validation.envelope.payload_bytes,
                validation.disk_bytes);
            if (pin) {
                if (metadata.pins == std::numeric_limits<core::u32>::max()) {
                    return core::Result<DerivativeCacheLoad,
                                        DerivativeCacheError>::failure(
                        error(DerivativeCacheErrorCode::PinLimitExceeded));
                }
                ++metadata.pins;
            }
            if (const auto trim_error = implementation_->trim_locked();
                trim_error.has_value()) {
                return core::Result<DerivativeCacheLoad,
                                    DerivativeCacheError>::failure(*trim_error);
            }
            const auto found = implementation_->entries.find(digest);
            if (found == implementation_->entries.end()) {
                entry.state = DerivativeState::Missing;
                entry.payload_bytes = 0U;
                entry.disk_bytes = 0U;
                payload.clear();
            } else {
                entry.pin_count = found->second.pins;
            }
        } else if (validation.code == ValidationCode::Missing ||
                   validation.code == ValidationCode::Stale) {
            implementation_->erase_locked(digest);
            if (validation.code == ValidationCode::Stale) {
                std::error_code ignored;
                static_cast<void>(fs::remove(target, ignored));
            }
        }
        return core::Result<DerivativeCacheLoad,
                            DerivativeCacheError>::success(
            DerivativeCacheLoad{std::move(entry), std::move(payload)});
    } catch (const std::bad_alloc&) {
        return core::Result<DerivativeCacheLoad,
                            DerivativeCacheError>::failure(
            error(DerivativeCacheErrorCode::AllocationFailed));
    } catch (...) {
        return core::Result<DerivativeCacheLoad,
                            DerivativeCacheError>::failure(
            error(DerivativeCacheErrorCode::ReadFailed));
    }
}

namespace {

[[nodiscard]] core::Result<DerivativeCacheEntry, DerivativeCacheError>
cancelled_publish(const fs::path& target) {
    return core::Result<DerivativeCacheEntry, DerivativeCacheError>::success(
        DerivativeCacheEntry{DerivativeState::Cancelled, target});
}

} // namespace

core::Result<DerivativeCacheEntry, DerivativeCacheError>
DerivativeCache::publish(const DerivativeKey& key,
                         std::span<const core::u8> payload, bool pin,
                         std::stop_token stop) noexcept {
    try {
        if (!valid_derivative_key(key)) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::InvalidKey));
        }
        const fs::path target = derivative_cache_path(
            implementation_->config.root, key);
        if (stop.stop_requested()) {
            return cancelled_publish(target);
        }
        const core::u64 payload_bytes = static_cast<core::u64>(payload.size());
        if (payload_bytes > std::numeric_limits<core::u64>::max() -
                                envelope_size ||
            (!pin && payload_bytes + envelope_size >
                         implementation_->config.maximum_bytes)) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::PayloadTooLarge));
        }
        std::scoped_lock lock{implementation_->mutex};
        Sha256Hasher hash;
        std::size_t hashed = 0U;
        while (hashed < payload.size()) {
            if (stop.stop_requested()) {
                return cancelled_publish(target);
            }
            const std::size_t amount = std::min(
                io_chunk_size, payload.size() - hashed);
            hash.update(payload.subspan(hashed, amount));
            hashed += amount;
        }
        const auto header = make_envelope(key, hash.finish(), payload_bytes);
        std::error_code filesystem_error;
        static_cast<void>(fs::create_directories(target.parent_path(),
                                                 filesystem_error));
        if (filesystem_error) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::CreateDirectoryFailed,
                      filesystem_error));
        }
        TemporaryFile temporary{unique_temporary_path(target)};
        std::ofstream output{temporary.path(),
                             std::ios::binary | std::ios::trunc};
        if (!output) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::OpenFailed));
        }
        output.write(reinterpret_cast<const char*>(header.data()),
                     static_cast<std::streamsize>(header.size()));
        std::size_t written = 0U;
        while (output && written < payload.size()) {
            if (stop.stop_requested()) {
                output.close();
                return cancelled_publish(target);
            }
            const std::size_t amount = std::min(
                io_chunk_size, payload.size() - written);
            output.write(reinterpret_cast<const char*>(payload.data() + written),
                         static_cast<std::streamsize>(amount));
            written += amount;
        }
        output.flush();
        output.close();
        if (!output) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::WriteFailed));
        }
        const Validation validation = validate_entry(
            temporary.path(), &key, true, nullptr, 0U, stop);
        if (validation.code == ValidationCode::Cancelled) {
            return cancelled_publish(target);
        }
        if (validation.code != ValidationCode::Ready) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::PublishFailed,
                      validation.system_error));
        }
        filesystem_error = atomic_replace(temporary.path(), target);
        if (filesystem_error) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::PublishFailed,
                      filesystem_error));
        }
        temporary.published();
        const std::string digest = derivative_key_string(key);
        Impl::Metadata& metadata = implementation_->upsert_locked(
            digest, target, payload_bytes, payload_bytes + envelope_size);
        if (pin) {
            if (metadata.pins == std::numeric_limits<core::u32>::max()) {
                return core::Result<DerivativeCacheEntry,
                                    DerivativeCacheError>::failure(
                    error(DerivativeCacheErrorCode::PinLimitExceeded));
            }
            ++metadata.pins;
        }
        if (const auto trim_error = implementation_->trim_locked();
            trim_error.has_value()) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(*trim_error);
        }
        const auto found = implementation_->entries.find(digest);
        if (found == implementation_->entries.end()) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::success(
                DerivativeCacheEntry{DerivativeState::Missing, target});
        }
        return core::Result<DerivativeCacheEntry,
                            DerivativeCacheError>::success(
            DerivativeCacheEntry{
                DerivativeState::Ready, target, payload_bytes,
                payload_bytes + envelope_size, found->second.pins, {}});
    } catch (const std::bad_alloc&) {
        return core::Result<DerivativeCacheEntry,
                            DerivativeCacheError>::failure(
            error(DerivativeCacheErrorCode::AllocationFailed));
    } catch (const fs::filesystem_error& caught) {
        return core::Result<DerivativeCacheEntry,
                            DerivativeCacheError>::failure(
            error(DerivativeCacheErrorCode::WriteFailed, caught.code()));
    } catch (...) {
        return core::Result<DerivativeCacheEntry,
                            DerivativeCacheError>::failure(
            error(DerivativeCacheErrorCode::WriteFailed));
    }
}

core::Result<DerivativeCacheEntry, DerivativeCacheError>
DerivativeCache::publish_file(const DerivativeKey& key,
                              const fs::path& payload_file, bool pin,
                              std::stop_token stop) noexcept {
    try {
        if (!valid_derivative_key(key)) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::InvalidKey));
        }
        const fs::path target = derivative_cache_path(
            implementation_->config.root, key);
        if (stop.stop_requested()) {
            return cancelled_publish(target);
        }
        std::error_code filesystem_error;
        if (!fs::is_regular_file(payload_file, filesystem_error)) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::SourceMissing,
                      filesystem_error));
        }
        const std::uintmax_t raw_payload_bytes =
            fs::file_size(payload_file, filesystem_error);
        if (filesystem_error ||
            raw_payload_bytes > std::numeric_limits<core::u64>::max()) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::ReadFailed,
                      filesystem_error));
        }
        const core::u64 payload_bytes =
            static_cast<core::u64>(raw_payload_bytes);
        if (payload_bytes > std::numeric_limits<core::u64>::max() -
                                envelope_size ||
            (!pin && payload_bytes + envelope_size >
                         implementation_->config.maximum_bytes)) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::PayloadTooLarge));
        }
        const fs::file_time_type expected_modified =
            fs::last_write_time(payload_file, filesystem_error);
        if (filesystem_error) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::ReadFailed,
                      filesystem_error));
        }
        std::ifstream input{payload_file, std::ios::binary};
        if (!input) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::OpenFailed));
        }
        std::scoped_lock lock{implementation_->mutex};
        static_cast<void>(fs::create_directories(target.parent_path(),
                                                 filesystem_error));
        if (filesystem_error) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::CreateDirectoryFailed,
                      filesystem_error));
        }
        TemporaryFile temporary{unique_temporary_path(target)};
        std::ofstream output{temporary.path(),
                             std::ios::binary | std::ios::trunc};
        if (!output) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::OpenFailed));
        }
        const std::array<core::u8, envelope_size> placeholder{};
        output.write(reinterpret_cast<const char*>(placeholder.data()),
                     static_cast<std::streamsize>(placeholder.size()));
        std::array<core::u8, io_chunk_size> buffer{};
        Sha256Hasher hash;
        core::u64 copied = 0U;
        while (copied < payload_bytes) {
            if (stop.stop_requested()) {
                input.close();
                output.close();
                return cancelled_publish(target);
            }
            const std::size_t amount = static_cast<std::size_t>(
                std::min<core::u64>(payload_bytes - copied, buffer.size()));
            input.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(amount));
            if (input.gcount() != static_cast<std::streamsize>(amount)) {
                return core::Result<DerivativeCacheEntry,
                                    DerivativeCacheError>::failure(
                    error(DerivativeCacheErrorCode::SourceChanged));
            }
            hash.update(std::span<const core::u8>{buffer.data(), amount});
            output.write(reinterpret_cast<const char*>(buffer.data()),
                         static_cast<std::streamsize>(amount));
            if (!output) {
                return core::Result<DerivativeCacheEntry,
                                    DerivativeCacheError>::failure(
                    error(DerivativeCacheErrorCode::WriteFailed));
            }
            copied += static_cast<core::u64>(amount);
        }
        const std::uintmax_t final_size =
            fs::file_size(payload_file, filesystem_error);
        const fs::file_time_type final_modified =
            fs::last_write_time(payload_file, filesystem_error);
        if (filesystem_error || final_size != raw_payload_bytes ||
            final_modified != expected_modified) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::SourceChanged,
                      filesystem_error));
        }
        const auto header = make_envelope(key, hash.finish(), payload_bytes);
        output.seekp(0, std::ios::beg);
        output.write(reinterpret_cast<const char*>(header.data()),
                     static_cast<std::streamsize>(header.size()));
        output.flush();
        output.close();
        input.close();
        if (!output) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::WriteFailed));
        }
        const Validation validation = validate_entry(
            temporary.path(), &key, true, nullptr, 0U, stop);
        if (validation.code == ValidationCode::Cancelled) {
            return cancelled_publish(target);
        }
        if (validation.code != ValidationCode::Ready) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::PublishFailed,
                      validation.system_error));
        }
        filesystem_error = atomic_replace(temporary.path(), target);
        if (filesystem_error) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::PublishFailed,
                      filesystem_error));
        }
        temporary.published();
        const std::string digest = derivative_key_string(key);
        Impl::Metadata& metadata = implementation_->upsert_locked(
            digest, target, payload_bytes, payload_bytes + envelope_size);
        if (pin) {
            if (metadata.pins == std::numeric_limits<core::u32>::max()) {
                return core::Result<DerivativeCacheEntry,
                                    DerivativeCacheError>::failure(
                    error(DerivativeCacheErrorCode::PinLimitExceeded));
            }
            ++metadata.pins;
        }
        if (const auto trim_error = implementation_->trim_locked();
            trim_error.has_value()) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::failure(*trim_error);
        }
        const auto found = implementation_->entries.find(digest);
        if (found == implementation_->entries.end()) {
            return core::Result<DerivativeCacheEntry,
                                DerivativeCacheError>::success(
                DerivativeCacheEntry{DerivativeState::Missing, target});
        }
        return core::Result<DerivativeCacheEntry,
                            DerivativeCacheError>::success(
            DerivativeCacheEntry{
                DerivativeState::Ready, target, payload_bytes,
                payload_bytes + envelope_size, found->second.pins, {}});
    } catch (const std::bad_alloc&) {
        return core::Result<DerivativeCacheEntry,
                            DerivativeCacheError>::failure(
            error(DerivativeCacheErrorCode::AllocationFailed));
    } catch (const fs::filesystem_error& caught) {
        return core::Result<DerivativeCacheEntry,
                            DerivativeCacheError>::failure(
            error(DerivativeCacheErrorCode::ReadFailed, caught.code()));
    } catch (...) {
        return core::Result<DerivativeCacheEntry,
                            DerivativeCacheError>::failure(
            error(DerivativeCacheErrorCode::ReadFailed));
    }
}

core::Result<DerivativeCacheStats, DerivativeCacheError>
DerivativeCache::unpin(const DerivativeKey& key) noexcept {
    try {
        if (!valid_derivative_key(key)) {
            return core::Result<DerivativeCacheStats,
                                DerivativeCacheError>::failure(
                error(DerivativeCacheErrorCode::InvalidKey));
        }
        std::scoped_lock lock{implementation_->mutex};
        const auto found = implementation_->entries.find(
            derivative_key_string(key));
        if (found != implementation_->entries.end() &&
            found->second.pins != 0U) {
            --found->second.pins;
        }
        if (const auto trim_error = implementation_->trim_locked();
            trim_error.has_value()) {
            return core::Result<DerivativeCacheStats,
                                DerivativeCacheError>::failure(*trim_error);
        }
        return core::Result<DerivativeCacheStats,
                            DerivativeCacheError>::success(
            implementation_->stats_locked());
    } catch (const std::bad_alloc&) {
        return core::Result<DerivativeCacheStats,
                            DerivativeCacheError>::failure(
            error(DerivativeCacheErrorCode::AllocationFailed));
    } catch (...) {
        return core::Result<DerivativeCacheStats,
                            DerivativeCacheError>::failure(
            error(DerivativeCacheErrorCode::RemoveFailed));
    }
}

core::Result<DerivativeCacheStats, DerivativeCacheError>
DerivativeCache::trim() noexcept {
    std::scoped_lock lock{implementation_->mutex};
    if (const auto trim_error = implementation_->trim_locked();
        trim_error.has_value()) {
        return core::Result<DerivativeCacheStats,
                            DerivativeCacheError>::failure(*trim_error);
    }
    return core::Result<DerivativeCacheStats,
                        DerivativeCacheError>::success(
        implementation_->stats_locked());
}

DerivativeCacheStats DerivativeCache::stats() const noexcept {
    std::scoped_lock lock{implementation_->mutex};
    return implementation_->stats_locked();
}

const fs::path& DerivativeCache::root() const noexcept {
    return implementation_->config.root;
}

} // namespace mediaserver::assets
