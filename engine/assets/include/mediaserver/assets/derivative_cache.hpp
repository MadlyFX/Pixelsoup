#pragma once

#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <system_error>
#include <vector>

namespace mediaserver::assets {

struct DerivativeCacheConfig final {
    // A dedicated absolute machine-local root. Authored project data must not
    // point here and this path is never part of document identity.
    std::filesystem::path root;
    core::u64 maximum_bytes{};
};

enum class DerivativeCacheErrorCode : core::u8 {
    InvalidRoot,
    InvalidCapacity,
    InvalidKey,
    CreateDirectoryFailed,
    SourceMissing,
    SourceChanged,
    OpenFailed,
    ReadFailed,
    WriteFailed,
    PublishFailed,
    RemoveFailed,
    PayloadTooLarge,
    PinLimitExceeded,
    AllocationFailed,
};

struct DerivativeCacheError final {
    DerivativeCacheErrorCode code{DerivativeCacheErrorCode::OpenFailed};
    std::error_code system_error;

    bool operator==(const DerivativeCacheError&) const = default;
};

struct DerivativeCacheEntry final {
    DerivativeState state{DerivativeState::Missing};
    std::filesystem::path path;
    core::u64 payload_bytes{};
    core::u64 disk_bytes{};
    core::u32 pin_count{};
    std::error_code system_error;
};

struct DerivativeCacheLoad final {
    DerivativeCacheEntry entry;
    std::vector<core::u8> payload;
};

struct DerivativeCacheStats final {
    core::u64 maximum_bytes{};
    core::u64 resident_bytes{};
    core::u64 entry_count{};
    core::u64 pinned_entries{};
    bool over_budget{};

    bool operator==(const DerivativeCacheStats&) const = default;
};

class DerivativeCache final {
public:
    ~DerivativeCache();
    DerivativeCache(DerivativeCache&&) noexcept;
    DerivativeCache& operator=(DerivativeCache&&) noexcept;
    DerivativeCache(const DerivativeCache&) = delete;
    DerivativeCache& operator=(const DerivativeCache&) = delete;

    [[nodiscard]] static core::Result<DerivativeCache, DerivativeCacheError>
    open(DerivativeCacheConfig config) noexcept;

    // A successful Result always carries the semantic state. Missing, Stale,
    // Failed, and Cancelled are states rather than exceptional control flow.
    [[nodiscard]] core::Result<DerivativeCacheEntry, DerivativeCacheError>
    inspect(const DerivativeKey& key, bool pin = false) noexcept;
    [[nodiscard]] core::Result<DerivativeCacheLoad, DerivativeCacheError>
    load(const DerivativeKey& key, core::u64 maximum_payload_bytes,
         bool pin = false, std::stop_token stop = {}) noexcept;

    [[nodiscard]] core::Result<DerivativeCacheEntry, DerivativeCacheError>
    publish(const DerivativeKey& key, std::span<const core::u8> payload,
            bool pin = false, std::stop_token stop = {}) noexcept;
    // Streams a generator-owned payload file into the same validated atomic
    // envelope; it never loads a potentially large proxy into memory and does
    // not remove the source file.
    [[nodiscard]] core::Result<DerivativeCacheEntry, DerivativeCacheError>
    publish_file(const DerivativeKey& key,
                 const std::filesystem::path& payload_file,
                 bool pin = false, std::stop_token stop = {}) noexcept;

    [[nodiscard]] core::Result<DerivativeCacheStats, DerivativeCacheError>
    unpin(const DerivativeKey& key) noexcept;
    [[nodiscard]] core::Result<DerivativeCacheStats, DerivativeCacheError>
    trim() noexcept;
    [[nodiscard]] DerivativeCacheStats stats() const noexcept;
    [[nodiscard]] const std::filesystem::path& root() const noexcept;

private:
    struct Impl;
    explicit DerivativeCache(std::unique_ptr<Impl> implementation) noexcept;
    std::unique_ptr<Impl> implementation_;
};

} // namespace mediaserver::assets
