#pragma once

#include <mediaserver/assets/content_identity.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <vector>

namespace mediaserver::assets {

// These are protocol ceilings, not tuning defaults. A peer-supplied limit is
// rejected if it tries to raise either ceiling, so a malformed request cannot
// turn one chunk into an unbounded allocation or make a node accept an
// implausibly large cache object.
inline constexpr core::u64 absolute_max_transfer_asset_bytes =
    16ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t absolute_max_transfer_chunk_bytes =
    4U * 1024U * 1024U;

struct AssetTransferLimits final {
    core::u64 max_asset_bytes{2ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL};
    std::size_t max_chunk_bytes{1U * 1024U * 1024U};

    bool operator==(const AssetTransferLimits&) const = default;
};

enum class AssetTransferErrorCode {
    InvalidLimits,
    InvalidIdentity,
    InvalidLogicalSource,
    SizeLimitExceeded,
    SizeMismatch,
    UnsafeCacheRoot,
    SourceNotRegularFile,
    OpenFailed,
    ReadFailed,
    WriteFailed,
    InvalidOffset,
    ChunkTooLarge,
    DataBeyondEnd,
    HashMismatch,
    Incomplete,
    AlreadyComplete,
    ReplaceFailed,
    Cancelled,
    AllocationFailed,
};

struct AssetTransferError final {
    AssetTransferErrorCode code{AssetTransferErrorCode::OpenFailed};
    std::error_code system_error;

    bool operator==(const AssetTransferError&) const = default;
};

struct AssetChunk final {
    core::u64 offset{};
    std::vector<std::byte> bytes;
    // True only when offset + bytes.size() is the descriptor's exact size.
    bool last{};

    bool operator==(const AssetChunk&) const = default;
};

struct AssetTransferReceipt final {
    ContentIdentity identity;
    // This path is local-only. It is derived from identity.sha256 beneath the
    // configured cache root and must never be serialized as a peer request.
    std::filesystem::path local_path;

    bool operator==(const AssetTransferReceipt&) const = default;
};

// Resolves only already-verified store content. The canonical digest-only blob
// is the readiness/integrity authority; the returned extension-preserving
// alias is materialized from that blob using a hard link or staged atomic copy.
// A valid legacy alias may be promoted to the canonical blob. Missing or
// corrupt content fails and no resumable partial file is created.
[[nodiscard]] core::Result<AssetTransferReceipt, AssetTransferError>
resolve_verified_asset(const std::filesystem::path& cache_root,
                       const ContentIdentity& identity,
                       std::string_view logical_source,
                       const AssetTransferLimits& limits = {},
                       std::stop_token stop = {});

class VerifiedAssetSource final {
public:
    VerifiedAssetSource(const VerifiedAssetSource&) = delete;
    VerifiedAssetSource& operator=(const VerifiedAssetSource&) = delete;
    VerifiedAssetSource(VerifiedAssetSource&&) noexcept;
    VerifiedAssetSource& operator=(VerifiedAssetSource&&) noexcept;
    ~VerifiedAssetSource();

    // source_path is supplied by the trusted Director-side manifest resolver.
    // It is verified against identity before this handle can serve any bytes.
    // Neither AssetTransferError nor AssetChunk discloses the source path.
    [[nodiscard]] static core::Result<VerifiedAssetSource, AssetTransferError>
    open_registered(const std::filesystem::path& source_path,
                    const ContentIdentity& identity,
                    const AssetTransferLimits& limits = {});

    [[nodiscard]] const ContentIdentity& identity() const noexcept;

    // Reads an exact bounded range for a transport response. Any offset from
    // zero through byte_size is valid, so a node can resume without replaying
    // earlier chunks. requested_bytes must be in 1..limits.max_chunk_bytes.
    [[nodiscard]] core::Result<AssetChunk, AssetTransferError>
    read_chunk(core::u64 offset, std::size_t requested_bytes);

private:
    struct State;
    explicit VerifiedAssetSource(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

class ResumableAssetSink final {
public:
    ResumableAssetSink(const ResumableAssetSink&) = delete;
    ResumableAssetSink& operator=(const ResumableAssetSink&) = delete;
    ResumableAssetSink(ResumableAssetSink&&) noexcept;
    ResumableAssetSink& operator=(ResumableAssetSink&&) noexcept;
    ~ResumableAssetSink();

    // The only destination names created below cache_root are derived from the
    // lowercase SHA-256 digest plus a bounded lowercase ASCII extension from
    // logical_source. Keeping that extension is required by the current media
    // and model dispatchers; the rest of logical_source never becomes part of
    // the cache path. Existing symlinks/reparse points, non-regular partial
    // files, and multiply-linked partial files are rejected. A valid completed
    // object is recognized idempotently; an incomplete .part file is reopened
    // at its exact on-disk size.
    [[nodiscard]] static core::Result<ResumableAssetSink, AssetTransferError>
    open(const std::filesystem::path& cache_root,
         const ContentIdentity& identity,
         std::string_view logical_source,
         const AssetTransferLimits& limits = {},
         std::stop_token stop = {});

    [[nodiscard]] const ContentIdentity& identity() const noexcept;
    [[nodiscard]] core::u64 resume_offset() const noexcept;
    [[nodiscard]] bool complete() const noexcept;

    // Chunks must be contiguous and exact: offset must equal resume_offset(),
    // the chunk may not exceed the negotiated limit, and it may not cross the
    // descriptor size. Each successful append is flushed before the new resume
    // offset is reported.
    [[nodiscard]] core::Result<core::u64, AssetTransferError>
    append(core::u64 offset, std::span<const std::byte> bytes);

    // Finalization is the trust boundary. The complete staged file is hashed,
    // a mismatch is deleted, and only verified bytes are atomically renamed to
    // the digest-only .blob name. The loader alias is then materialized and
    // verified before its local path is returned.
    [[nodiscard]] core::Result<AssetTransferReceipt, AssetTransferError>
    finalize(std::stop_token stop = {});

private:
    struct State;
    explicit ResumableAssetSink(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

} // namespace mediaserver::assets
