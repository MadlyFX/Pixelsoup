#include <mediaserver/assets/asset_transfer.hpp>
#include <mediaserver/assets/content_resolver.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mediaserver::assets {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] AssetTransferError make_error(
    AssetTransferErrorCode code,
    std::error_code system_error = {}) noexcept {
    return AssetTransferError{code, system_error};
}

[[nodiscard]] std::error_code stream_error() noexcept {
    if (errno != 0) {
        return std::error_code{errno, std::generic_category()};
    }
    return std::make_error_code(std::errc::io_error);
}

[[nodiscard]] bool valid_limits(const AssetTransferLimits& limits) noexcept {
    return limits.max_asset_bytes > 0U &&
        limits.max_asset_bytes <= absolute_max_transfer_asset_bytes &&
        limits.max_chunk_bytes > 0U &&
        limits.max_chunk_bytes <= absolute_max_transfer_chunk_bytes;
}

[[nodiscard]] bool valid_identity(
    const ContentIdentity& identity,
    const AssetTransferLimits& limits) noexcept {
    return identity.byte_size > 0U &&
        identity.byte_size <= limits.max_asset_bytes &&
        identity.byte_size <= absolute_max_transfer_asset_bytes;
}

[[nodiscard]] AssetTransferError verification_error(
    const ContentVerificationResult& verified,
    AssetTransferErrorCode io_code) noexcept {
    if (!verified) {
        if (verified.error().code ==
            ContentIntegrityErrorCode::Cancelled) {
            return make_error(AssetTransferErrorCode::Cancelled);
        }
        return make_error(io_code, verified.error().system_error);
    }
    return make_error(
        verified.value() == ContentVerification::SizeMismatch
            ? AssetTransferErrorCode::SizeMismatch
            : AssetTransferErrorCode::HashMismatch);
}

[[nodiscard]] bool is_reparse_point(const fs::path& path,
                                    std::error_code& error) noexcept {
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = std::error_code{
            static_cast<int>(GetLastError()), std::system_category()};
        return false;
    }
    error.clear();
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
#else
    static_cast<void>(path);
    error.clear();
    return false;
#endif
}

[[nodiscard]] core::Result<std::monostate, AssetTransferError>
require_plain_directory(const fs::path& path) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (error) {
        return core::Result<std::monostate, AssetTransferError>::failure(
            make_error(AssetTransferErrorCode::UnsafeCacheRoot, error));
    }
    if (!fs::is_directory(status) || fs::is_symlink(status)) {
        return core::Result<std::monostate, AssetTransferError>::failure(
            make_error(AssetTransferErrorCode::UnsafeCacheRoot));
    }
    if (is_reparse_point(path, error) || error) {
        return core::Result<std::monostate, AssetTransferError>::failure(
            make_error(AssetTransferErrorCode::UnsafeCacheRoot, error));
    }
    return core::Result<std::monostate, AssetTransferError>::success(
        std::monostate{});
}

[[nodiscard]] core::Result<std::monostate, AssetTransferError>
create_plain_directory(const fs::path& path) {
    std::error_code error;
    const bool exists = fs::exists(path, error);
    if (error) {
        return core::Result<std::monostate, AssetTransferError>::failure(
            make_error(AssetTransferErrorCode::UnsafeCacheRoot, error));
    }
    if (!exists && !fs::create_directory(path, error)) {
        return core::Result<std::monostate, AssetTransferError>::failure(
            make_error(AssetTransferErrorCode::UnsafeCacheRoot, error));
    }
    return require_plain_directory(path);
}

[[nodiscard]] core::Result<bool, AssetTransferError> regular_file_if_present(
    const fs::path& path,
    bool partial) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        return core::Result<bool, AssetTransferError>::failure(
            make_error(AssetTransferErrorCode::UnsafeCacheRoot, error));
    }
    if (status.type() == fs::file_type::not_found) {
        return core::Result<bool, AssetTransferError>::success(false);
    }
    if (error || !fs::is_regular_file(status) || fs::is_symlink(status)) {
        return core::Result<bool, AssetTransferError>::failure(
            make_error(AssetTransferErrorCode::UnsafeCacheRoot, error));
    }
    if (is_reparse_point(path, error) || error) {
        return core::Result<bool, AssetTransferError>::failure(
            make_error(AssetTransferErrorCode::UnsafeCacheRoot, error));
    }
    if (partial) {
        const auto links = fs::hard_link_count(path, error);
        if (error || links != 1U) {
            return core::Result<bool, AssetTransferError>::failure(
                make_error(AssetTransferErrorCode::UnsafeCacheRoot, error));
        }
    }
    return core::Result<bool, AssetTransferError>::success(true);
}

struct CachePaths final {
    fs::path partial;
    fs::path blob;
    fs::path complete;
};

using CachePathsResult = core::Result<CachePaths, AssetTransferError>;

[[nodiscard]] CachePathsResult prepare_cache_paths(
    const fs::path& cache_root,
    const ContentIdentity& identity,
    std::string_view logical_source) {
    if (cache_root.empty()) {
        return CachePathsResult::failure(
            make_error(AssetTransferErrorCode::UnsafeCacheRoot));
    }
    std::error_code error;
    fs::path absolute_root = fs::absolute(cache_root, error);
    if (error) {
        return CachePathsResult::failure(
            make_error(AssetTransferErrorCode::UnsafeCacheRoot, error));
    }
    absolute_root = absolute_root.lexically_normal();
    static_cast<void>(fs::create_directories(absolute_root, error));
    if (error) {
        return CachePathsResult::failure(
            make_error(AssetTransferErrorCode::UnsafeCacheRoot, error));
    }
    fs::path canonical_root = fs::canonical(absolute_root, error);
    if (error) {
        return CachePathsResult::failure(
            make_error(AssetTransferErrorCode::UnsafeCacheRoot, error));
    }
    auto root_ok = require_plain_directory(canonical_root);
    if (!root_ok) {
        return CachePathsResult::failure(std::move(root_ok.error()));
    }

    const std::string digest = format_sha256(identity.sha256);
    if (digest.size() != sha256_hex_characters) {
        return CachePathsResult::failure(
            make_error(AssetTransferErrorCode::InvalidIdentity));
    }

    // Reuse the resolver's path vocabulary so a completed transfer is exactly
    // where SharedStore resolution will look. Requiring a retained extension
    // prevents a verified MP4/MOV/model from being misrouted by today's
    // extension-based dispatchers.
    const auto relative =
        shared_store_relative_path(identity, logical_source);
    if (!relative.has_value()) {
        return CachePathsResult::failure(
            make_error(AssetTransferErrorCode::InvalidLogicalSource));
    }
    const std::string filename = relative->filename().string();
    const fs::path blob_relative = shared_store_blob_relative_path(identity);
    const std::string blob_filename = blob_relative.filename().string();
    const fs::path store = canonical_root / "sha256";
    auto store_ok = create_plain_directory(store);
    if (!store_ok) {
        return CachePathsResult::failure(std::move(store_ok.error()));
    }
    const fs::path shard = store / digest.substr(0U, 2U);
    auto shard_ok = create_plain_directory(shard);
    if (!shard_ok) {
        return CachePathsResult::failure(std::move(shard_ok.error()));
    }

    return CachePathsResult::success(CachePaths{
        shard / (blob_filename + ".part"), shard / blob_filename,
        shard / filename});
}

[[nodiscard]] core::Result<std::monostate, AssetTransferError> remove_file(
    const fs::path& path) {
    std::error_code error;
    static_cast<void>(fs::remove(path, error));
    if (error) {
        return core::Result<std::monostate, AssetTransferError>::failure(
            make_error(AssetTransferErrorCode::WriteFailed, error));
    }
    return core::Result<std::monostate, AssetTransferError>::success(
        std::monostate{});
}

[[nodiscard]] core::Result<std::monostate, AssetTransferError>
copy_file_cancellable(const fs::path& source, const fs::path& target,
                      std::stop_token stop) {
    using Result = core::Result<std::monostate, AssetTransferError>;
    const auto cancelled = [] {
        return Result::failure(
            make_error(AssetTransferErrorCode::Cancelled));
    };
    if (stop.stop_requested()) {
        return cancelled();
    }

    constexpr std::size_t buffer_bytes = 64U * 1024U;
    std::array<std::byte, buffer_bytes> buffer{};
#if defined(_WIN32)
    struct Handle final {
        HANDLE value{INVALID_HANDLE_VALUE};
        ~Handle() {
            if (value != INVALID_HANDLE_VALUE) {
                static_cast<void>(CloseHandle(value));
            }
        }
    };
    Handle input{CreateFileW(
        source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
            FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
    if (input.value == INVALID_HANDLE_VALUE) {
        return Result::failure(make_error(
            AssetTransferErrorCode::OpenFailed,
            std::error_code{static_cast<int>(GetLastError()),
                            std::system_category()}));
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(
            input.value, FileAttributeTagInfo, &attributes,
            static_cast<DWORD>(sizeof(attributes)))) {
        return Result::failure(make_error(
            AssetTransferErrorCode::OpenFailed,
            std::error_code{static_cast<int>(GetLastError()),
                            std::system_category()}));
    }
    if ((attributes.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        return Result::failure(make_error(
            AssetTransferErrorCode::SourceNotRegularFile));
    }
    Handle output{CreateFileW(target.c_str(), GENERIC_WRITE, 0U, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (output.value == INVALID_HANDLE_VALUE) {
        return Result::failure(make_error(
            AssetTransferErrorCode::WriteFailed,
            std::error_code{static_cast<int>(GetLastError()),
                            std::system_category()}));
    }
    for (;;) {
        if (stop.stop_requested()) {
            return cancelled();
        }
        DWORD read{};
        if (!ReadFile(input.value, buffer.data(),
                      static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            if (stop.stop_requested()) {
                return cancelled();
            }
            return Result::failure(make_error(
                AssetTransferErrorCode::ReadFailed,
                std::error_code{static_cast<int>(GetLastError()),
                                std::system_category()}));
        }
        if (read == 0U) {
            break;
        }
        DWORD written_total{};
        while (written_total < read) {
            if (stop.stop_requested()) {
                return cancelled();
            }
            DWORD written{};
            if (!WriteFile(
                    output.value,
                    buffer.data() + static_cast<std::size_t>(written_total),
                    read - written_total, &written, nullptr) ||
                written == 0U) {
                if (stop.stop_requested()) {
                    return cancelled();
                }
                return Result::failure(make_error(
                    AssetTransferErrorCode::WriteFailed,
                    std::error_code{static_cast<int>(GetLastError()),
                                    std::system_category()}));
            }
            written_total += written;
        }
    }
#else
    struct Descriptor final {
        int value{-1};
        ~Descriptor() {
            if (value >= 0) {
                static_cast<void>(::close(value));
            }
        }
    };
#ifdef O_CLOEXEC
    constexpr int close_on_exec = O_CLOEXEC;
#else
    constexpr int close_on_exec = 0;
#endif
#ifdef O_NOFOLLOW
    constexpr int no_follow = O_NOFOLLOW;
#else
    constexpr int no_follow = 0;
#endif
    Descriptor input{::open(source.c_str(),
                            O_RDONLY | close_on_exec | no_follow)};
    if (input.value < 0) {
        return Result::failure(make_error(
            AssetTransferErrorCode::OpenFailed,
            std::error_code{errno, std::generic_category()}));
    }
    struct stat source_status{};
    if (::fstat(input.value, &source_status) != 0 ||
        !S_ISREG(source_status.st_mode)) {
        return Result::failure(make_error(
            AssetTransferErrorCode::SourceNotRegularFile,
            errno == 0 ? std::error_code{}
                       : std::error_code{errno, std::generic_category()}));
    }
    Descriptor output{::open(target.c_str(),
                             O_WRONLY | O_CREAT | O_EXCL | close_on_exec |
                                 no_follow,
                             S_IRUSR | S_IWUSR)};
    if (output.value < 0) {
        return Result::failure(make_error(
            AssetTransferErrorCode::WriteFailed,
            std::error_code{errno, std::generic_category()}));
    }
    for (;;) {
        if (stop.stop_requested()) {
            return cancelled();
        }
        const ssize_t read = ::read(input.value, buffer.data(), buffer.size());
        if (read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Result::failure(make_error(
                AssetTransferErrorCode::ReadFailed,
                std::error_code{errno, std::generic_category()}));
        }
        if (read == 0) {
            break;
        }
        ssize_t written_total{};
        while (written_total < read) {
            if (stop.stop_requested()) {
                return cancelled();
            }
            const ssize_t written = ::write(
                output.value,
                buffer.data() + static_cast<std::size_t>(written_total),
                static_cast<std::size_t>(read - written_total));
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return Result::failure(make_error(
                    AssetTransferErrorCode::WriteFailed,
                    std::error_code{errno, std::generic_category()}));
            }
            if (written == 0) {
                return Result::failure(make_error(
                    AssetTransferErrorCode::WriteFailed,
                    std::make_error_code(std::errc::io_error)));
            }
            written_total += written;
        }
    }
#endif
    if (stop.stop_requested()) {
        return cancelled();
    }
    return Result::success(std::monostate{});
}

[[nodiscard]] core::Result<std::monostate, AssetTransferError>
materialize_alias(const fs::path& source, const fs::path& target,
                  std::stop_token stop = {}) {
    using Result = core::Result<std::monostate, AssetTransferError>;
    if (stop.stop_requested()) {
        return Result::failure(
            make_error(AssetTransferErrorCode::Cancelled));
    }
    std::error_code error;
    fs::create_hard_link(source, target, error);
    if (stop.stop_requested()) {
        return Result::failure(
            make_error(AssetTransferErrorCode::Cancelled));
    }
    if (!error) {
        return Result::success(std::monostate{});
    }

    // Hard links are not available on every configured cache filesystem. Copy
    // to a unique sibling and rename only after the copy is complete, so an
    // extension-routed loader never observes a partial alias.
    static std::atomic_uint64_t nonce{1U};
    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        fs::path staging = target;
        staging += ".alias." + std::to_string(stamp) + "." +
            std::to_string(nonce.fetch_add(1U, std::memory_order_relaxed)) +
            ".part";
        auto copied = copy_file_cancellable(source, staging, stop);
        if (stop.stop_requested()) {
            std::error_code ignored;
            static_cast<void>(fs::remove(staging, ignored));
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        if (!copied) {
            if (copied.error().system_error ==
                std::errc::file_exists) {
                continue;
            }
            std::error_code exists_error;
            if (fs::exists(target, exists_error) && !exists_error) {
                return Result::success(std::monostate{});
            }
            return Result::failure(std::move(copied.error()));
        }
        error.clear();
        fs::rename(staging, target, error);
        if (!error) {
            return Result::success(std::monostate{});
        }
        std::error_code ignored;
        static_cast<void>(fs::remove(staging, ignored));
        std::error_code exists_error;
        if (fs::exists(target, exists_error) && !exists_error) {
            return Result::success(std::monostate{});
        }
        return Result::failure(
            make_error(AssetTransferErrorCode::ReplaceFailed, error));
    }
    return Result::failure(make_error(AssetTransferErrorCode::WriteFailed));
}

using CompletionResult = core::Result<bool, AssetTransferError>;

[[nodiscard]] CompletionResult verified_candidate(
    const fs::path& path,
    const ContentIdentity& identity,
    bool remove_mismatch,
    std::stop_token stop = {}) {
    if (stop.stop_requested()) {
        return CompletionResult::failure(
            make_error(AssetTransferErrorCode::Cancelled));
    }
    auto present = regular_file_if_present(path, false);
    if (!present) {
        return CompletionResult::failure(std::move(present.error()));
    }
    if (!present.value()) {
        return CompletionResult::success(false);
    }
    const auto verified = verify_file(path, identity, stop);
    if (!verified) {
        if (verified.error().code ==
            ContentIntegrityErrorCode::Cancelled) {
            return CompletionResult::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        return CompletionResult::failure(make_error(
            AssetTransferErrorCode::ReadFailed,
            verified.error().system_error));
    }
    if (verified.value() == ContentVerification::Verified) {
        return CompletionResult::success(true);
    }
    if (remove_mismatch) {
        auto removed = remove_file(path);
        if (!removed) {
            return CompletionResult::failure(std::move(removed.error()));
        }
    }
    return CompletionResult::success(false);
}

[[nodiscard]] CompletionResult ensure_completed(
    const CachePaths& paths,
    const ContentIdentity& identity,
    std::stop_token stop = {}) {
    auto blob = verified_candidate(paths.blob, identity, true, stop);
    if (!blob) {
        return CompletionResult::failure(std::move(blob.error()));
    }
    auto alias = verified_candidate(paths.complete, identity, true, stop);
    if (!alias) {
        return CompletionResult::failure(std::move(alias.error()));
    }

    if (!blob.value() && alias.value()) {
        auto materialized = materialize_alias(
            paths.complete, paths.blob, stop);
        if (!materialized) {
            return CompletionResult::failure(
                std::move(materialized.error()));
        }
        blob = verified_candidate(paths.blob, identity, true, stop);
        if (!blob) {
            return CompletionResult::failure(std::move(blob.error()));
        }
        if (!blob.value()) {
            return CompletionResult::failure(
                make_error(AssetTransferErrorCode::HashMismatch));
        }
    }
    if (blob.value() && !alias.value()) {
        auto materialized = materialize_alias(
            paths.blob, paths.complete, stop);
        if (!materialized) {
            return CompletionResult::failure(
                std::move(materialized.error()));
        }
        alias = verified_candidate(paths.complete, identity, true, stop);
        if (!alias) {
            return CompletionResult::failure(std::move(alias.error()));
        }
        if (!alias.value()) {
            return CompletionResult::failure(
                make_error(AssetTransferErrorCode::HashMismatch));
        }
    }
    return CompletionResult::success(blob.value() && alias.value());
}

} // namespace

core::Result<AssetTransferReceipt, AssetTransferError>
resolve_verified_asset(const fs::path& cache_root,
                       const ContentIdentity& identity,
                       std::string_view logical_source,
                       const AssetTransferLimits& limits,
                       std::stop_token stop) {
    using Result = core::Result<AssetTransferReceipt, AssetTransferError>;
    try {
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        if (!valid_limits(limits)) {
            return Result::failure(
                make_error(AssetTransferErrorCode::InvalidLimits));
        }
        if (!valid_identity(identity, limits)) {
            return Result::failure(make_error(
                identity.byte_size > limits.max_asset_bytes
                    ? AssetTransferErrorCode::SizeLimitExceeded
                    : AssetTransferErrorCode::InvalidIdentity));
        }
        auto paths = prepare_cache_paths(cache_root, identity, logical_source);
        if (!paths) {
            return Result::failure(std::move(paths.error()));
        }
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        auto completed = ensure_completed(paths.value(), identity, stop);
        if (!completed) {
            return Result::failure(std::move(completed.error()));
        }
        if (!completed.value()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Incomplete));
        }
        return Result::success(
            AssetTransferReceipt{identity, paths.value().complete});
    } catch (const std::bad_alloc&) {
        return Result::failure(
            make_error(AssetTransferErrorCode::AllocationFailed));
    } catch (const std::length_error&) {
        return Result::failure(
            make_error(AssetTransferErrorCode::SizeLimitExceeded));
    } catch (const fs::filesystem_error& error) {
        return Result::failure(
            make_error(AssetTransferErrorCode::ReadFailed, error.code()));
    }
}

struct VerifiedAssetSource::State final {
    ContentIdentity descriptor;
    AssetTransferLimits limits;
    std::ifstream stream;
};

VerifiedAssetSource::VerifiedAssetSource(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

VerifiedAssetSource::VerifiedAssetSource(VerifiedAssetSource&&) noexcept =
    default;
VerifiedAssetSource& VerifiedAssetSource::operator=(
    VerifiedAssetSource&&) noexcept = default;
VerifiedAssetSource::~VerifiedAssetSource() = default;

core::Result<VerifiedAssetSource, AssetTransferError>
VerifiedAssetSource::open_registered(const fs::path& source_path,
                                     const ContentIdentity& identity,
                                     const AssetTransferLimits& limits) {
    using Result = core::Result<VerifiedAssetSource, AssetTransferError>;
    try {
        if (!valid_limits(limits)) {
            return Result::failure(
                make_error(AssetTransferErrorCode::InvalidLimits));
        }
        if (!valid_identity(identity, limits)) {
            return Result::failure(make_error(
                identity.byte_size > limits.max_asset_bytes
                    ? AssetTransferErrorCode::SizeLimitExceeded
                    : AssetTransferErrorCode::InvalidIdentity));
        }

        std::error_code error;
        const fs::file_status status = fs::status(source_path, error);
        if (error) {
            return Result::failure(
                make_error(AssetTransferErrorCode::OpenFailed, error));
        }
        if (!fs::is_regular_file(status)) {
            return Result::failure(
                make_error(AssetTransferErrorCode::SourceNotRegularFile));
        }
        const auto verified = verify_file(source_path, identity);
        if (!verified || verified.value() != ContentVerification::Verified) {
            return Result::failure(
                verification_error(verified, AssetTransferErrorCode::ReadFailed));
        }

        auto state = std::make_unique<State>();
        state->descriptor = identity;
        state->limits = limits;
        errno = 0;
        state->stream.open(source_path, std::ios::binary);
        if (!state->stream) {
            return Result::failure(make_error(
                AssetTransferErrorCode::OpenFailed, stream_error()));
        }
        return Result::success(VerifiedAssetSource{std::move(state)});
    } catch (const std::bad_alloc&) {
        return Result::failure(
            make_error(AssetTransferErrorCode::AllocationFailed));
    } catch (const std::length_error&) {
        return Result::failure(
            make_error(AssetTransferErrorCode::SizeLimitExceeded));
    } catch (const fs::filesystem_error& error) {
        return Result::failure(
            make_error(AssetTransferErrorCode::OpenFailed, error.code()));
    }
}

const ContentIdentity& VerifiedAssetSource::identity() const noexcept {
    return state_->descriptor;
}

core::Result<AssetChunk, AssetTransferError>
VerifiedAssetSource::read_chunk(core::u64 offset,
                                std::size_t requested_bytes) {
    using Result = core::Result<AssetChunk, AssetTransferError>;
    try {
        if (requested_bytes == 0U ||
            requested_bytes > state_->limits.max_chunk_bytes ||
            requested_bytes > absolute_max_transfer_chunk_bytes) {
            return Result::failure(
                make_error(AssetTransferErrorCode::ChunkTooLarge));
        }
        if (offset > state_->descriptor.byte_size) {
            return Result::failure(
                make_error(AssetTransferErrorCode::InvalidOffset));
        }
        const core::u64 remaining = state_->descriptor.byte_size - offset;
        const std::size_t amount = static_cast<std::size_t>(
            std::min<core::u64>(remaining,
                                static_cast<core::u64>(requested_bytes)));
        AssetChunk chunk;
        chunk.offset = offset;
        chunk.bytes.resize(amount);
        chunk.last = amount == static_cast<std::size_t>(remaining);
        if (amount == 0U) {
            return Result::success(std::move(chunk));
        }
        state_->stream.clear();
        state_->stream.seekg(static_cast<std::streamoff>(offset),
                             std::ios::beg);
        if (!state_->stream) {
            return Result::failure(
                make_error(AssetTransferErrorCode::ReadFailed, stream_error()));
        }
        state_->stream.read(reinterpret_cast<char*>(chunk.bytes.data()),
                            static_cast<std::streamsize>(amount));
        if (state_->stream.gcount() != static_cast<std::streamsize>(amount)) {
            return Result::failure(
                make_error(AssetTransferErrorCode::ReadFailed, stream_error()));
        }
        return Result::success(std::move(chunk));
    } catch (const std::bad_alloc&) {
        return Result::failure(
            make_error(AssetTransferErrorCode::AllocationFailed));
    } catch (const std::length_error&) {
        return Result::failure(
            make_error(AssetTransferErrorCode::SizeLimitExceeded));
    }
}

struct ResumableAssetSink::State final {
    ContentIdentity descriptor;
    AssetTransferLimits limits;
    fs::path partial_path;
    fs::path blob_path;
    fs::path complete_path;
    std::ofstream stream;
    core::u64 received{};
    bool installed{};
    bool failed{};
};

ResumableAssetSink::ResumableAssetSink(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

ResumableAssetSink::ResumableAssetSink(ResumableAssetSink&&) noexcept = default;
ResumableAssetSink& ResumableAssetSink::operator=(
    ResumableAssetSink&&) noexcept = default;
ResumableAssetSink::~ResumableAssetSink() = default;

core::Result<ResumableAssetSink, AssetTransferError>
ResumableAssetSink::open(const fs::path& cache_root,
                         const ContentIdentity& identity,
                         std::string_view logical_source,
                         const AssetTransferLimits& limits,
                         std::stop_token stop) {
    using Result = core::Result<ResumableAssetSink, AssetTransferError>;
    try {
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        if (!valid_limits(limits)) {
            return Result::failure(
                make_error(AssetTransferErrorCode::InvalidLimits));
        }
        if (!valid_identity(identity, limits)) {
            return Result::failure(make_error(
                identity.byte_size > limits.max_asset_bytes
                    ? AssetTransferErrorCode::SizeLimitExceeded
                    : AssetTransferErrorCode::InvalidIdentity));
        }
        auto paths = prepare_cache_paths(cache_root, identity, logical_source);
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        if (!paths) {
            return Result::failure(std::move(paths.error()));
        }
        auto state = std::make_unique<State>();
        state->descriptor = identity;
        state->limits = limits;
        state->partial_path = std::move(paths.value().partial);
        state->blob_path = std::move(paths.value().blob);
        state->complete_path = std::move(paths.value().complete);

        const CachePaths state_paths{
            state->partial_path, state->blob_path, state->complete_path};
        auto completed = ensure_completed(state_paths, identity, stop);
        if (!completed) {
            return Result::failure(std::move(completed.error()));
        }
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        if (completed.value()) {
            auto partial_present =
                regular_file_if_present(state->partial_path, true);
            if (!partial_present) {
                return Result::failure(std::move(partial_present.error()));
            }
            if (partial_present.value()) {
                if (stop.stop_requested()) {
                    return Result::failure(
                        make_error(AssetTransferErrorCode::Cancelled));
                }
                auto removed = remove_file(state->partial_path);
                if (!removed) {
                    return Result::failure(std::move(removed.error()));
                }
            }
            if (stop.stop_requested()) {
                return Result::failure(
                    make_error(AssetTransferErrorCode::Cancelled));
            }
            state->received = identity.byte_size;
            state->installed = true;
            return Result::success(ResumableAssetSink{std::move(state)});
        }

        auto partial_present =
            regular_file_if_present(state->partial_path, true);
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        if (!partial_present) {
            return Result::failure(std::move(partial_present.error()));
        }
        if (partial_present.value()) {
            std::error_code error;
            core::u64 partial_size = fs::file_size(state->partial_path, error);
            if (stop.stop_requested()) {
                return Result::failure(
                    make_error(AssetTransferErrorCode::Cancelled));
            }
            if (error) {
                return Result::failure(
                    make_error(AssetTransferErrorCode::ReadFailed, error));
            }
            if (partial_size > identity.byte_size) {
                fs::resize_file(state->partial_path, identity.byte_size, error);
                if (stop.stop_requested()) {
                    return Result::failure(
                        make_error(AssetTransferErrorCode::Cancelled));
                }
                if (error) {
                    return Result::failure(
                        make_error(AssetTransferErrorCode::WriteFailed, error));
                }
                partial_size = identity.byte_size;
            }
            if (partial_size == identity.byte_size) {
                const auto verified =
                    verify_file(state->partial_path, identity, stop);
                if (stop.stop_requested()) {
                    return Result::failure(
                        make_error(AssetTransferErrorCode::Cancelled));
                }
                if (verified &&
                    verified.value() == ContentVerification::Verified) {
                    fs::rename(state->partial_path, state->blob_path, error);
                    if (error) {
                        const auto winner = verified_candidate(
                            state->blob_path, identity, false, stop);
                        if (stop.stop_requested()) {
                            return Result::failure(make_error(
                                AssetTransferErrorCode::Cancelled));
                        }
                        if (!winner || !winner.value()) {
                            return Result::failure(make_error(
                                AssetTransferErrorCode::ReplaceFailed, error));
                        }
                        std::error_code ignored;
                        static_cast<void>(
                            fs::remove(state->partial_path, ignored));
                    }
                    auto alias = materialize_alias(state->blob_path,
                                                   state->complete_path,
                                                   stop);
                    if (!alias) {
                        return Result::failure(std::move(alias.error()));
                    }
                    const auto alias_verified = verified_candidate(
                        state->complete_path, identity, true, stop);
                    if (!alias_verified) {
                        return Result::failure(
                            std::move(alias_verified.error()));
                    }
                    if (stop.stop_requested()) {
                        return Result::failure(make_error(
                            AssetTransferErrorCode::Cancelled));
                    }
                    if (!alias_verified.value()) {
                        return Result::failure(make_error(
                            AssetTransferErrorCode::HashMismatch));
                    }
                    state->received = identity.byte_size;
                    state->installed = true;
                    return Result::success(
                        ResumableAssetSink{std::move(state)});
                }
                auto removed = remove_file(state->partial_path);
                if (!removed) {
                    return Result::failure(std::move(removed.error()));
                }
                partial_size = 0U;
            }
            state->received = partial_size;
        }

        errno = 0;
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        state->stream.open(state->partial_path,
                           std::ios::binary | std::ios::app);
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        if (!state->stream) {
            return Result::failure(make_error(
                AssetTransferErrorCode::OpenFailed, stream_error()));
        }
        return Result::success(ResumableAssetSink{std::move(state)});
    } catch (const std::bad_alloc&) {
        return Result::failure(
            make_error(AssetTransferErrorCode::AllocationFailed));
    } catch (const std::length_error&) {
        return Result::failure(
            make_error(AssetTransferErrorCode::SizeLimitExceeded));
    } catch (const fs::filesystem_error& error) {
        return Result::failure(
            make_error(AssetTransferErrorCode::OpenFailed, error.code()));
    }
}

const ContentIdentity& ResumableAssetSink::identity() const noexcept {
    return state_->descriptor;
}

core::u64 ResumableAssetSink::resume_offset() const noexcept {
    return state_->received;
}

bool ResumableAssetSink::complete() const noexcept {
    return state_->installed;
}

core::Result<core::u64, AssetTransferError> ResumableAssetSink::append(
    core::u64 offset,
    std::span<const std::byte> bytes) {
    using Result = core::Result<core::u64, AssetTransferError>;
    if (state_->installed) {
        return Result::failure(
            make_error(AssetTransferErrorCode::AlreadyComplete));
    }
    if (state_->failed) {
        return Result::failure(
            make_error(AssetTransferErrorCode::WriteFailed));
    }
    if (offset != state_->received) {
        return Result::failure(
            make_error(AssetTransferErrorCode::InvalidOffset));
    }
    if (bytes.empty() || bytes.size() > state_->limits.max_chunk_bytes ||
        bytes.size() > absolute_max_transfer_chunk_bytes) {
        return Result::failure(
            make_error(AssetTransferErrorCode::ChunkTooLarge));
    }
    const core::u64 count = static_cast<core::u64>(bytes.size());
    if (count > state_->descriptor.byte_size - state_->received) {
        return Result::failure(
            make_error(AssetTransferErrorCode::DataBeyondEnd));
    }
    errno = 0;
    state_->stream.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
    state_->stream.flush();
    if (!state_->stream) {
        state_->failed = true;
        state_->stream.close();
        return Result::failure(
            make_error(AssetTransferErrorCode::WriteFailed, stream_error()));
    }
    state_->received += count;
    return Result::success(state_->received);
}

core::Result<AssetTransferReceipt, AssetTransferError>
ResumableAssetSink::finalize(std::stop_token stop) {
    using Result = core::Result<AssetTransferReceipt, AssetTransferError>;
    try {
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        if (state_->installed) {
            return Result::success(
                AssetTransferReceipt{state_->descriptor,
                                     state_->complete_path});
        }
        const CachePaths state_paths{
            state_->partial_path, state_->blob_path, state_->complete_path};
        auto completed =
            ensure_completed(state_paths, state_->descriptor, stop);
        if (!completed) {
            return Result::failure(std::move(completed.error()));
        }
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        if (completed.value()) {
            state_->stream.close();
            auto partial_present =
                regular_file_if_present(state_->partial_path, true);
            if (!partial_present) {
                return Result::failure(std::move(partial_present.error()));
            }
            if (partial_present.value()) {
                if (stop.stop_requested()) {
                    return Result::failure(
                        make_error(AssetTransferErrorCode::Cancelled));
                }
                auto removed = remove_file(state_->partial_path);
                if (!removed) {
                    return Result::failure(std::move(removed.error()));
                }
            }
            if (stop.stop_requested()) {
                return Result::failure(
                    make_error(AssetTransferErrorCode::Cancelled));
            }
            state_->received = state_->descriptor.byte_size;
            state_->installed = true;
            return Result::success(
                AssetTransferReceipt{state_->descriptor,
                                     state_->complete_path});
        }
        if (state_->failed) {
            return Result::failure(
                make_error(AssetTransferErrorCode::WriteFailed));
        }
        if (state_->received != state_->descriptor.byte_size) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Incomplete));
        }
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        state_->stream.flush();
        state_->stream.close();
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        if (state_->stream.fail()) {
            state_->failed = true;
            return Result::failure(
                make_error(AssetTransferErrorCode::WriteFailed, stream_error()));
        }

        const auto verified =
            verify_file(state_->partial_path, state_->descriptor, stop);
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        if (!verified || verified.value() != ContentVerification::Verified) {
            const AssetTransferError error = verification_error(
                verified, AssetTransferErrorCode::ReadFailed);
            if (verified) {
                std::error_code ignored;
                static_cast<void>(fs::remove(state_->partial_path, ignored));
                state_->received = 0U;
            }
            return Result::failure(error);
        }

        std::error_code error;
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        fs::rename(state_->partial_path, state_->blob_path, error);
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        if (error) {
            // Another same-digest receiver may have won the blob rename. Treat
            // it as success only after independently verifying the winner.
            const auto blob_present =
                regular_file_if_present(state_->blob_path, false);
            if (!blob_present || !blob_present.value()) {
                return Result::failure(make_error(
                    AssetTransferErrorCode::ReplaceFailed, error));
            }
            const auto winner =
                verify_file(state_->blob_path, state_->descriptor, stop);
            if (stop.stop_requested()) {
                return Result::failure(
                    make_error(AssetTransferErrorCode::Cancelled));
            }
            if (!winner || winner.value() != ContentVerification::Verified) {
                return Result::failure(make_error(
                    AssetTransferErrorCode::ReplaceFailed, error));
            }
            std::error_code ignored;
            static_cast<void>(fs::remove(state_->partial_path, ignored));
        }
        auto alias =
            materialize_alias(state_->blob_path, state_->complete_path, stop);
        if (!alias) {
            return Result::failure(std::move(alias.error()));
        }
        const auto alias_verified = verified_candidate(
            state_->complete_path, state_->descriptor, true, stop);
        if (!alias_verified) {
            return Result::failure(std::move(alias_verified.error()));
        }
        if (stop.stop_requested()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::Cancelled));
        }
        if (!alias_verified.value()) {
            return Result::failure(
                make_error(AssetTransferErrorCode::HashMismatch));
        }
        state_->installed = true;
        return Result::success(
            AssetTransferReceipt{state_->descriptor, state_->complete_path});
    } catch (const std::bad_alloc&) {
        return Result::failure(
            make_error(AssetTransferErrorCode::AllocationFailed));
    } catch (const fs::filesystem_error& error) {
        return Result::failure(
            make_error(AssetTransferErrorCode::ReplaceFailed, error.code()));
    }
}

} // namespace mediaserver::assets
