#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <mediaserver/core/result.hpp>

namespace mediaserver::persist {

// The WAL deliberately stores opaque records. The wire layer is responsible
// for encoding commands and the final seal, in that canonical order.
using RecordBytes = std::vector<std::byte>;

struct LogCursor final {
    std::uint64_t through_frame{};
    std::uint64_t revision{};
    std::uint64_t next_command{};

    bool operator==(const LogCursor&) const = default;
};

struct SealedFrameBytes final {
    std::uint64_t frame{};
    std::uint64_t base_revision{};
    std::uint64_t result_revision{};
    std::uint64_t first_command_id{};
    std::uint64_t next_command_id{};
    std::vector<RecordBytes> records;

    bool operator==(const SealedFrameBytes&) const = default;
};

enum class SyncPolicy : std::uint8_t {
    // The frame body is still Data-synced before its commit trailer so a torn
    // tail stays recoverable. None skips only the final commit-trailer sync;
    // receipts therefore report durable=false and flush() is a no-op.
    None,
    // Data-sync both the ordered body and final commit trailer.
    Data,
    // Full-sync the final trailer and ensure the WAL path's directory entry is
    // durable when opening the log.
    Full,
};

struct LogLimits final {
    std::uint32_t max_record_bytes{16U * 1024U * 1024U};
    std::uint32_t max_payload_bytes{64U * 1024U * 1024U};
    std::uint32_t max_records_per_frame{65'536U};
};

struct OpenOptions final {
    SyncPolicy sync_policy{SyncPolicy::Full};
    bool repair_torn_tail{true};
    LogLimits limits{};
};

enum class LogErrorCode : std::uint8_t {
    OpenFailed,
    ReadFailed,
    WriteFailed,
    SyncFailed,
    TruncateFailed,
    InvalidHeader,
    UnsupportedVersion,
    BaseCursorMismatch,
    TornTail,
    CorruptEntry,
    ChainMismatch,
    LimitExceeded,
    CursorNotFound,
    VisitorFailed,
    Unusable,
};

struct LogError final {
    LogErrorCode code{LogErrorCode::OpenFailed};
    std::uint64_t offset{};
    std::error_code system_error{};
    std::string detail;
};

struct AppendReceipt final {
    std::uint64_t offset{};
    std::uint64_t bytes_written{};
    LogCursor cursor{};
    bool durable{};
    // The CRC stored in the authenticated commit trailer. Exposing it binds
    // sidecar metadata to the exact committed WAL entry without changing the
    // version 1 byte format.
    std::uint32_t entry_crc32c{};
};

struct WalFrameIdentity final {
    LogCursor before{};
    LogCursor after{};
    std::uint64_t offset{};
    std::uint64_t bytes{};
    std::uint32_t entry_crc32c{};

    bool operator==(const WalFrameIdentity&) const = default;
};

struct RecoveryReport final {
    LogCursor base_cursor{};
    LogCursor tail_cursor{};
    std::uint64_t entry_count{};
    std::uint64_t valid_bytes{};
    std::uint64_t truncated_bytes{};
    bool repaired_torn_tail{};
};

using LogStatus = core::Result<std::monostate, LogError>;
using ReplayVisitor = std::function<LogStatus(const SealedFrameBytes&)>;
using IdentityReplayVisitor = std::function<LogStatus(
    const SealedFrameBytes&,
    const WalFrameIdentity&)>;

// Immutable view of one committed WAL prefix. The backing file may grow after
// capture; replay is permanently bounded by valid_bytes and never consults the
// live writer's tail, recovery report, usability flag, or entry index.
class SealedFrameLogSnapshot final {
public:
    SealedFrameLogSnapshot(SealedFrameLogSnapshot&&) noexcept = default;
    SealedFrameLogSnapshot& operator=(SealedFrameLogSnapshot&&) noexcept = default;
    SealedFrameLogSnapshot(const SealedFrameLogSnapshot&) = delete;
    SealedFrameLogSnapshot& operator=(const SealedFrameLogSnapshot&) = delete;
    ~SealedFrameLogSnapshot() = default;

    [[nodiscard]] LogStatus replay_after(
        LogCursor cursor,
        const ReplayVisitor& visitor) const;

    // Visits the complete captured prefix once, in physical order, exposing
    // the exact identity of each validated committed entry. Unlike
    // replay_after(), this recovery-oriented API does not perform a preflight
    // pass because callers publish no partial result when it returns failure.
    [[nodiscard]] LogStatus visit_committed(
        const IdentityReplayVisitor& visitor) const;

    [[nodiscard]] LogStatus replay_after_with_identity(
        LogCursor cursor,
        const IdentityReplayVisitor& visitor) const;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] LogCursor base_cursor() const noexcept { return base_cursor_; }
    [[nodiscard]] LogCursor tail_cursor() const noexcept { return tail_cursor_; }
    [[nodiscard]] std::uint64_t valid_bytes() const noexcept { return valid_bytes_; }
    [[nodiscard]] const LogLimits& limits() const noexcept { return limits_; }

private:
    friend class SealedFrameLog;

    SealedFrameLogSnapshot(
        std::filesystem::path path,
        LogCursor base_cursor,
        LogCursor tail_cursor,
        std::uint64_t valid_bytes,
        LogLimits limits) noexcept
        : path_(std::move(path)),
          base_cursor_(base_cursor),
          tail_cursor_(tail_cursor),
          valid_bytes_(valid_bytes),
          limits_(limits) {}

    std::filesystem::path path_;
    LogCursor base_cursor_{};
    LogCursor tail_cursor_{};
    std::uint64_t valid_bytes_{};
    LogLimits limits_{};
};

class SealedFrameLog final {
public:
    // Opens a caller-serialized single-writer WAL. Concurrent process opens
    // are unsupported until native exclusive locking is added. A new or
    // zero-length file is initialized with base_cursor; a populated file must
    // contain the exact same base cursor.
    [[nodiscard]] static core::Result<SealedFrameLog, LogError> open_or_create(
        std::filesystem::path path,
        LogCursor base_cursor,
        OpenOptions options = {});

    SealedFrameLog(SealedFrameLog&&) noexcept = default;
    SealedFrameLog& operator=(SealedFrameLog&&) noexcept = default;
    SealedFrameLog(const SealedFrameLog&) = delete;
    SealedFrameLog& operator=(const SealedFrameLog&) = delete;
    ~SealedFrameLog() = default;

    // records must contain one opaque record per command followed by one seal
    // record. Metadata is validated against tail_cursor before any bytes are
    // written. The body ordering barrier always runs; sync_policy controls the
    // final commit sync and is reflected by AppendReceipt::durable.
    [[nodiscard]] core::Result<AppendReceipt, LogError> append(
        const SealedFrameBytes& frame);
    [[nodiscard]] LogStatus flush() const;
    // cursor must be the log base or the result cursor of a complete frame.
    // The visitor is invoked in ascending frame order after that boundary.
    [[nodiscard]] LogStatus replay_after(
        LogCursor cursor,
        const ReplayVisitor& visitor) const;
    [[nodiscard]] core::Result<SealedFrameLogSnapshot, LogError>
    capture_committed() const;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] LogCursor base_cursor() const noexcept { return base_cursor_; }
    [[nodiscard]] LogCursor tail_cursor() const noexcept { return tail_cursor_; }
    [[nodiscard]] const LogLimits& limits() const noexcept { return options_.limits; }
    [[nodiscard]] SyncPolicy sync_policy() const noexcept {
        return options_.sync_policy;
    }
    [[nodiscard]] const RecoveryReport& recovery_report() const noexcept { return recovery_; }

private:
    SealedFrameLog(
        std::filesystem::path path,
        LogCursor base_cursor,
        OpenOptions options,
        RecoveryReport recovery,
        std::vector<std::uint64_t> entry_offsets);

    std::filesystem::path path_;
    LogCursor base_cursor_{};
    LogCursor tail_cursor_{};
    OpenOptions options_{};
    RecoveryReport recovery_{};
    std::vector<std::uint64_t> entry_offsets_;
    bool usable_{true};
};

} // namespace mediaserver::persist
