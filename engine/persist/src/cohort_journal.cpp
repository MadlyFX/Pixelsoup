#include <mediaserver/persist/cohort_journal.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mediaserver::persist {
namespace {

constexpr std::uint16_t kFormatVersion = 1U;
constexpr std::size_t kHeaderSize = 128U;
constexpr std::size_t kPrefixSize = 64U;
constexpr std::size_t kTrailerSize = 24U;
constexpr std::size_t kHeaderCrcOffset = 124U;
constexpr std::size_t kPrefixCrcOffset = 40U;
constexpr std::size_t kMinimumRecordSize = kPrefixSize + kTrailerSize;
constexpr std::uint32_t kAbsoluteMaxRecords = 1'000'000U;
constexpr std::uint32_t kAbsoluteMaxCohortMembers = 65'536U;
constexpr std::uint32_t kAbsoluteMaxRecordBytes = 16U * 1024U * 1024U;
constexpr std::uint64_t kAbsoluteMaxFileBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;

constexpr std::array<std::byte, 8U> kHeaderMagic{
    std::byte{0x4d}, std::byte{0x53}, std::byte{0x43}, std::byte{0x4f},
    std::byte{0x48}, std::byte{0x4f}, std::byte{0x52}, std::byte{0x54}}; // MSCOHORT
constexpr std::array<std::byte, 8U> kRecordMagic{
    std::byte{0x4d}, std::byte{0x53}, std::byte{0x43}, std::byte{0x4f},
    std::byte{0x48}, std::byte{0x52}, std::byte{0x54}, std::byte{0x31}}; // MSCOHRT1
constexpr std::array<std::byte, 8U> kCommitMagic{
    std::byte{0x4d}, std::byte{0x53}, std::byte{0x43}, std::byte{0x4f},
    std::byte{0x4d}, std::byte{0x4d}, std::byte{0x49}, std::byte{0x54}}; // MSCOMMIT

using JournalStatus = core::Result<std::monostate, CohortJournalError>;

constexpr std::array<std::uint32_t, 256U> make_crc32c_table() noexcept {
    std::array<std::uint32_t, 256U> table{};
    for (std::size_t index = 0U; index < table.size(); ++index) {
        auto value = static_cast<std::uint32_t>(index);
        for (std::uint32_t bit = 0U; bit < 8U; ++bit) {
            value = (value >> 1U)
                ^ ((value & 1U) != 0U ? 0x82f63b78U : 0U);
        }
        table[index] = value;
    }
    return table;
}

constexpr auto kCrc32cTable = make_crc32c_table();

[[nodiscard]] std::uint32_t crc32c(
    std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        const auto lookup = static_cast<std::uint8_t>(
            (crc ^ std::to_integer<std::uint8_t>(byte)) & 0xffU);
        crc = kCrc32cTable[lookup] ^ (crc >> 8U);
    }
    return ~crc;
}

void put_u16(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void put_u32(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint32_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        bytes[offset + static_cast<std::size_t>(shift / 8U)] =
            static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

void put_u64(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint64_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        bytes[offset + static_cast<std::size_t>(shift / 8U)] =
            static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

[[nodiscard]] std::uint16_t get_u16(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(bytes[offset]))
        | static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(
                std::to_integer<std::uint8_t>(bytes[offset + 1U]))
            << 8U));
}

[[nodiscard]] std::uint32_t get_u32(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    std::uint32_t value = 0U;
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(
                         bytes[offset + static_cast<std::size_t>(shift / 8U)]))
            << shift;
    }
    return value;
}

[[nodiscard]] std::uint64_t get_u64(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    std::uint64_t value = 0U;
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(
                         bytes[offset + static_cast<std::size_t>(shift / 8U)]))
            << shift;
    }
    return value;
}

template <std::size_t Size>
[[nodiscard]] bool has_magic(
    std::span<const std::byte> bytes,
    std::size_t offset,
    const std::array<std::byte, Size>& magic) noexcept {
    return bytes.size() >= offset + Size
        && std::equal(
            magic.begin(),
            magic.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

[[nodiscard]] bool is_zero(std::span<const std::byte> bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](std::byte value) {
        return value == std::byte{0U};
    });
}

[[nodiscard]] CohortJournalError make_error(
    CohortJournalErrorCode code,
    std::uint64_t offset,
    std::string detail,
    std::error_code system_error = {}) {
    return CohortJournalError{
        code,
        offset,
        system_error,
        std::move(detail),
    };
}

[[nodiscard]] JournalStatus success_status() {
    return JournalStatus::success(std::monostate{});
}

[[nodiscard]] std::error_code current_io_error() noexcept {
    return std::error_code{errno, std::generic_category()};
}

[[nodiscard]] bool cursor_valid(const LogCursor& cursor) noexcept {
    return cursor.next_command != 0U;
}

[[nodiscard]] bool cursor_not_after(
    const LogCursor& left,
    const LogCursor& right) noexcept {
    return left.through_frame <= right.through_frame
        && left.revision <= right.revision
        && left.next_command <= right.next_command;
}

[[nodiscard]] bool valid_cursor_transition(
    const LogCursor& before,
    const LogCursor& after) noexcept {
    return cursor_valid(before) && cursor_valid(after)
        && before.through_frame != std::numeric_limits<std::uint64_t>::max()
        && after.through_frame == before.through_frame + 1U
        && after.revision >= before.revision
        && after.next_command >= before.next_command
        && after.revision - before.revision
            == after.next_command - before.next_command;
}

[[nodiscard]] bool valid_frame_identity(
    const WalFrameIdentity& frame) noexcept {
    return valid_cursor_transition(frame.before, frame.after)
        && frame.bytes != 0U
        && frame.bytes
            <= std::numeric_limits<std::uint64_t>::max() - frame.offset;
}

template <typename Tag>
[[nodiscard]] bool id_less(
    const CohortJournalId128<Tag>& left,
    const CohortJournalId128<Tag>& right) noexcept {
    return left.high < right.high
        || (left.high == right.high && left.low < right.low);
}

[[nodiscard]] bool member_less(
    const RecoveryCohortMember& left,
    const RecoveryCohortMember& right) noexcept {
    if (left.node_id != right.node_id) {
        return id_less(left.node_id, right.node_id);
    }
    return id_less(left.incarnation, right.incarnation);
}

[[nodiscard]] bool valid_empty_confirmation(
    const RecoveryCohort& cohort,
    EmptyCohortConfirmation confirmation) noexcept {
    return cohort.empty()
        ? confirmation == EmptyCohortConfirmation::ExplicitlyEmpty
        : confirmation == EmptyCohortConfirmation::None;
}

[[nodiscard]] bool valid_limits(const CohortJournalLimits& limits) noexcept {
    return limits.max_records != 0U
        && limits.max_records <= kAbsoluteMaxRecords
        && limits.max_cohort_members <= kAbsoluteMaxCohortMembers
        && limits.max_record_bytes >= kMinimumRecordSize
        && limits.max_record_bytes <= kAbsoluteMaxRecordBytes
        && limits.max_file_bytes >= kHeaderSize
        && limits.max_file_bytes <= kAbsoluteMaxFileBytes;
}

[[nodiscard]] bool inventory_consistent(
    const UnresolvedReleaseInventory& inventory) noexcept {
    constexpr std::uint64_t wal_header_bytes = 64U;
    constexpr std::uint64_t release_header_bytes = 64U;
    constexpr std::uint64_t release_record_bytes = 128U;
    if (!cursor_valid(inventory.base_cursor)
        || !cursor_valid(inventory.released_frontier)
        || !cursor_valid(inventory.wal_tail)
        || inventory.released_frame_count > inventory.wal_frame_count
        || inventory.unresolved_frame_count
            != inventory.wal_frame_count - inventory.released_frame_count
        || inventory.released_wal_bytes > inventory.wal_valid_bytes
        || inventory.released_wal_bytes < wal_header_bytes
        || inventory.wal_valid_bytes < wal_header_bytes
        || inventory.release_ledger_valid_bytes < release_header_bytes
        || !cursor_not_after(
            inventory.base_cursor, inventory.released_frontier)
        || !cursor_not_after(
            inventory.released_frontier, inventory.wal_tail)) {
        return false;
    }
    if (inventory.wal_frame_count
            != inventory.wal_tail.through_frame
                - inventory.base_cursor.through_frame
        || inventory.released_frame_count
            != inventory.released_frontier.through_frame
                - inventory.base_cursor.through_frame
        || inventory.unresolved_wal_bytes
            != inventory.wal_valid_bytes - inventory.released_wal_bytes
        || inventory.released_frame_count
            > (std::numeric_limits<std::uint64_t>::max()
                   - release_header_bytes)
                / release_record_bytes
        || inventory.release_ledger_valid_bytes
            != release_header_bytes
                + inventory.released_frame_count * release_record_bytes) {
        return false;
    }
    const auto expected_next =
        inventory.released_frame_count
                == std::numeric_limits<std::uint64_t>::max()
            ? 0U
            : inventory.released_frame_count + 1U;
    if (inventory.next_release_ordinal != expected_next
        || ((inventory.unresolved_frame_count == 0U)
            != !inventory.next_unresolved)
        || ((inventory.unresolved_frame_count == 0U)
            != (inventory.unresolved_wal_bytes == 0U))) {
        return false;
    }
    if (inventory.next_unresolved) {
        const auto& next = *inventory.next_unresolved;
        if (!valid_frame_identity(next)
            || next.before != inventory.released_frontier
            || next.offset != inventory.released_wal_bytes
            || next.offset + next.bytes > inventory.wal_valid_bytes
            || !cursor_not_after(next.after, inventory.wal_tail)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint8_t kind_value(
    const CohortJournalMutation& mutation) noexcept {
    return static_cast<std::uint8_t>(mutation.index() + 1U);
}

[[nodiscard]] bool valid_abort_reason(AbortFrameReason reason) noexcept {
    switch (reason) {
    case AbortFrameReason::OperatorCancelled:
    case AbortFrameReason::Superseded:
    case AbortFrameReason::PlanInvalidated:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_reconfigure_reason(
    ReconfigureFrameReason reason) noexcept {
    switch (reason) {
    case ReconfigureFrameReason::NodeUnavailable:
    case ReconfigureFrameReason::IncarnationLost:
    case ReconfigureFrameReason::OperatorOverride:
        return true;
    }
    return false;
}

class PayloadWriter final {
public:
    void u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }

    void u32(std::uint32_t value) {
        const auto offset = bytes_.size();
        bytes_.resize(offset + 4U);
        put_u32(std::span<std::byte>{bytes_}, offset, value);
    }

    void u64(std::uint64_t value) {
        const auto offset = bytes_.size();
        bytes_.resize(offset + 8U);
        put_u64(std::span<std::byte>{bytes_}, offset, value);
    }

    void zeros(std::size_t count) {
        bytes_.insert(bytes_.end(), count, std::byte{0U});
    }

    template <typename Tag>
    void id(CohortJournalId128<Tag> value) {
        u64(value.high);
        u64(value.low);
    }

    void cursor(const LogCursor& value) {
        u64(value.through_frame);
        u64(value.revision);
        u64(value.next_command);
    }

    void frame(const WalFrameIdentity& value) {
        cursor(value.before);
        cursor(value.after);
        u64(value.offset);
        u64(value.bytes);
        u32(value.entry_crc32c);
        zeros(4U);
    }

    void cohort(const RecoveryCohort& value) {
        for (const auto& member : value) {
            id(member.node_id);
            id(member.incarnation);
        }
    }

    [[nodiscard]] std::vector<std::byte> finish() && {
        return std::move(bytes_);
    }

private:
    std::vector<std::byte> bytes_;
};

class PayloadReader final {
public:
    explicit PayloadReader(std::span<const std::byte> bytes) noexcept
        : bytes_(bytes) {}

    [[nodiscard]] bool u8(std::uint8_t& value) noexcept {
        if (remaining() < 1U) {
            return false;
        }
        value = std::to_integer<std::uint8_t>(bytes_[offset_]);
        ++offset_;
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& value) noexcept {
        if (remaining() < 4U) {
            return false;
        }
        value = get_u32(bytes_, offset_);
        offset_ += 4U;
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& value) noexcept {
        if (remaining() < 8U) {
            return false;
        }
        value = get_u64(bytes_, offset_);
        offset_ += 8U;
        return true;
    }

    [[nodiscard]] bool zeros(std::size_t count) noexcept {
        if (remaining() < count
            || !is_zero(bytes_.subspan(offset_, count))) {
            return false;
        }
        offset_ += count;
        return true;
    }

    template <typename Tag>
    [[nodiscard]] bool id(CohortJournalId128<Tag>& value) noexcept {
        return u64(value.high) && u64(value.low);
    }

    [[nodiscard]] bool cursor(LogCursor& value) noexcept {
        return u64(value.through_frame) && u64(value.revision)
            && u64(value.next_command);
    }

    [[nodiscard]] bool frame(WalFrameIdentity& value) noexcept {
        return cursor(value.before) && cursor(value.after)
            && u64(value.offset) && u64(value.bytes)
            && u32(value.entry_crc32c) && zeros(4U);
    }

    [[nodiscard]] bool cohort(
        RecoveryCohort& value,
        std::uint32_t count) {
        if (static_cast<std::uint64_t>(count)
            > static_cast<std::uint64_t>(remaining()) / 32U) {
            return false;
        }
        value.clear();
        value.reserve(count);
        for (std::uint32_t index = 0U; index < count; ++index) {
            RecoveryCohortMember member;
            if (!id(member.node_id) || !id(member.incarnation)) {
                return false;
            }
            value.push_back(member);
        }
        return true;
    }

    [[nodiscard]] bool complete() const noexcept {
        return offset_ == bytes_.size();
    }

private:
    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

void encode_cohort_header(
    PayloadWriter& writer,
    const RecoveryCohort& cohort,
    EmptyCohortConfirmation confirmation) {
    writer.u32(static_cast<std::uint32_t>(cohort.size()));
    writer.u8(static_cast<std::uint8_t>(confirmation));
    writer.zeros(11U);
}

[[nodiscard]] std::vector<std::byte> encode_payload(
    const CohortJournalMutation& mutation) {
    PayloadWriter writer;
    std::visit(
        [&writer](const auto& record) {
            using Record = std::decay_t<decltype(record)>;
            if constexpr (std::is_same_v<Record, BeginFrame>) {
                writer.id(record.plan_token);
                writer.cursor(record.expected_before);
                writer.u64(record.next_frame);
                writer.id(record.cut_director_epoch);
                writer.u64(record.plan_generation);
                writer.u64(record.plan_fingerprint);
                encode_cohort_header(
                    writer, record.cohort, record.empty_confirmation);
                writer.cohort(record.cohort);
            } else if constexpr (std::is_same_v<Record, BindFrame>) {
                writer.id(record.begin_token);
                writer.u64(record.begin_ordinal);
                writer.frame(record.frame);
                writer.u64(record.plan_generation);
                writer.id(record.plan_token);
                writer.u64(record.plan_fingerprint);
            } else if constexpr (std::is_same_v<Record, AbortFrame>) {
                writer.id(record.begin_token);
                writer.u64(record.begin_ordinal);
                writer.u8(static_cast<std::uint8_t>(record.reason));
                writer.zeros(7U);
            } else if constexpr (std::is_same_v<Record, AdoptFrame>) {
                writer.frame(record.frame);
                writer.id(record.operator_request_id);
                writer.u8(static_cast<std::uint8_t>(record.provenance));
                writer.u8(static_cast<std::uint8_t>(record.empty_confirmation));
                writer.zeros(6U);
                writer.u64(record.plan_generation);
                writer.id(record.plan_token);
                writer.u64(record.plan_fingerprint);
                writer.u32(static_cast<std::uint32_t>(record.cohort.size()));
                writer.zeros(12U);
                writer.cohort(record.cohort);
            } else if constexpr (std::is_same_v<Record, ReconfigureFrame>) {
                writer.frame(record.frame);
                writer.u64(record.expected_plan_generation);
                writer.id(record.expected_plan_token);
                writer.u64(record.expected_plan_fingerprint);
                writer.u64(record.expected_plan_journal_ordinal);
                writer.cursor(record.expected_release_frontier);
                writer.cursor(record.expected_wal_tail);
                writer.u64(record.new_plan_generation);
                writer.id(record.new_plan_token);
                writer.u64(record.new_plan_fingerprint);
                writer.id(record.operator_request_id);
                writer.u8(static_cast<std::uint8_t>(record.reason));
                writer.u8(static_cast<std::uint8_t>(record.empty_confirmation));
                writer.zeros(2U);
                writer.u32(static_cast<std::uint32_t>(record.cohort.size()));
                writer.zeros(8U);
                writer.cohort(record.cohort);
            } else {
                writer.frame(record.frame);
                writer.u64(record.attempt_generation);
                writer.id(record.attempt_token);
                writer.id(record.director_epoch);
                writer.id(record.session_namespace);
                writer.cursor(record.release_frontier);
                writer.cursor(record.wal_tail);
                writer.u64(record.active_plan_generation);
                writer.id(record.active_plan_token);
                writer.u64(record.active_plan_journal_ordinal);
            }
        },
        mutation);
    return std::move(writer).finish();
}

template <typename Record>
[[nodiscard]] core::Result<CohortJournalMutation, CohortJournalError>
decoded_result(Record record) {
    return core::Result<CohortJournalMutation, CohortJournalError>::success(
        CohortJournalMutation{std::move(record)});
}

[[nodiscard]] core::Result<CohortJournalMutation, CohortJournalError>
decode_payload(
    CohortJournalRecordKind kind,
    std::span<const std::byte> bytes,
    std::uint64_t record_offset,
    const CohortJournalLimits& limits) {
    using Result = core::Result<CohortJournalMutation, CohortJournalError>;
    PayloadReader reader{bytes};
    std::uint32_t cohort_count = 0U;
    std::uint8_t enum_value = 0U;
    std::uint8_t confirmation = 0U;
    switch (kind) {
    case CohortJournalRecordKind::BeginFrame: {
        BeginFrame record;
        if (!reader.id(record.plan_token)
            || !reader.cursor(record.expected_before)
            || !reader.u64(record.next_frame)
            || !reader.id(record.cut_director_epoch)
            || !reader.u64(record.plan_generation)
            || !reader.u64(record.plan_fingerprint)
            || !reader.u32(cohort_count)
            || !reader.u8(confirmation) || !reader.zeros(11U)
            || cohort_count > limits.max_cohort_members
            || !reader.cohort(record.cohort, cohort_count)
            || !reader.complete()) {
            return Result::failure(make_error(
                CohortJournalErrorCode::CorruptRecord,
                record_offset,
                "BeginFrame payload is malformed or exceeds its cohort cap"));
        }
        record.empty_confirmation =
            static_cast<EmptyCohortConfirmation>(confirmation);
        return decoded_result(std::move(record));
    }
    case CohortJournalRecordKind::BindFrame: {
        BindFrame record;
        if (!reader.id(record.begin_token)
            || !reader.u64(record.begin_ordinal)
            || !reader.frame(record.frame)
            || !reader.u64(record.plan_generation)
            || !reader.id(record.plan_token)
            || !reader.u64(record.plan_fingerprint)
            || !reader.complete()) {
            return Result::failure(make_error(
                CohortJournalErrorCode::CorruptRecord,
                record_offset,
                "BindFrame payload is malformed"));
        }
        return decoded_result(std::move(record));
    }
    case CohortJournalRecordKind::AbortFrame: {
        AbortFrame record;
        if (!reader.id(record.begin_token)
            || !reader.u64(record.begin_ordinal)
            || !reader.u8(enum_value) || !reader.zeros(7U)
            || !reader.complete()) {
            return Result::failure(make_error(
                CohortJournalErrorCode::CorruptRecord,
                record_offset,
                "AbortFrame payload is malformed"));
        }
        record.reason = static_cast<AbortFrameReason>(enum_value);
        return decoded_result(std::move(record));
    }
    case CohortJournalRecordKind::AdoptFrame: {
        AdoptFrame record;
        if (!reader.frame(record.frame)
            || !reader.id(record.operator_request_id)
            || !reader.u8(enum_value) || !reader.u8(confirmation)
            || !reader.zeros(6U)
            || !reader.u64(record.plan_generation)
            || !reader.id(record.plan_token)
            || !reader.u64(record.plan_fingerprint)
            || !reader.u32(cohort_count) || !reader.zeros(12U)
            || cohort_count > limits.max_cohort_members
            || !reader.cohort(record.cohort, cohort_count)
            || !reader.complete()) {
            return Result::failure(make_error(
                CohortJournalErrorCode::CorruptRecord,
                record_offset,
                "AdoptFrame payload is malformed or exceeds its cohort cap"));
        }
        record.provenance = static_cast<AdoptionProvenance>(enum_value);
        record.empty_confirmation =
            static_cast<EmptyCohortConfirmation>(confirmation);
        return decoded_result(std::move(record));
    }
    case CohortJournalRecordKind::ReconfigureFrame: {
        ReconfigureFrame record;
        if (!reader.frame(record.frame)
            || !reader.u64(record.expected_plan_generation)
            || !reader.id(record.expected_plan_token)
            || !reader.u64(record.expected_plan_fingerprint)
            || !reader.u64(record.expected_plan_journal_ordinal)
            || !reader.cursor(record.expected_release_frontier)
            || !reader.cursor(record.expected_wal_tail)
            || !reader.u64(record.new_plan_generation)
            || !reader.id(record.new_plan_token)
            || !reader.u64(record.new_plan_fingerprint)
            || !reader.id(record.operator_request_id)
            || !reader.u8(enum_value) || !reader.u8(confirmation)
            || !reader.zeros(2U) || !reader.u32(cohort_count)
            || !reader.zeros(8U)
            || cohort_count > limits.max_cohort_members
            || !reader.cohort(record.cohort, cohort_count)
            || !reader.complete()) {
            return Result::failure(make_error(
                CohortJournalErrorCode::CorruptRecord,
                record_offset,
                "ReconfigureFrame payload is malformed or exceeds its cohort cap"));
        }
        record.reason = static_cast<ReconfigureFrameReason>(enum_value);
        record.empty_confirmation =
            static_cast<EmptyCohortConfirmation>(confirmation);
        return decoded_result(std::move(record));
    }
    case CohortJournalRecordKind::AttemptStarted: {
        AttemptStarted record;
        if (!reader.frame(record.frame)
            || !reader.u64(record.attempt_generation)
            || !reader.id(record.attempt_token)
            || !reader.id(record.director_epoch)
            || !reader.id(record.session_namespace)
            || !reader.cursor(record.release_frontier)
            || !reader.cursor(record.wal_tail)
            || !reader.u64(record.active_plan_generation)
            || !reader.id(record.active_plan_token)
            || !reader.u64(record.active_plan_journal_ordinal)
            || !reader.complete()) {
            return Result::failure(make_error(
                CohortJournalErrorCode::CorruptRecord,
                record_offset,
                "AttemptStarted payload is malformed"));
        }
        return decoded_result(std::move(record));
    }
    }
    return Result::failure(make_error(
        CohortJournalErrorCode::CorruptRecord,
        record_offset,
        "unknown cohort-journal record kind"));
}

struct PendingPlan final {
    std::uint64_t ordinal{};
    RecoveryPlanToken token;
    LogCursor expected_before{};
    std::uint64_t next_frame{};
    std::uint64_t generation{};
    std::uint64_t fingerprint{};
};

struct ActivePlan final {
    WalFrameIdentity frame{};
    std::uint64_t generation{};
    RecoveryPlanToken token;
    std::uint64_t fingerprint{};
    std::uint64_t journal_ordinal{};
    std::uint64_t attempt_generation{};
    std::vector<RecoveryPlanToken> prior_plan_tokens;
    std::vector<RecoveryAttemptToken> used_attempt_tokens;
};

struct OperatorRequest final {
    RecoveryOperatorRequestId request_id;
};

struct Lifecycle final {
    LogCursor release_frontier{};
    LogCursor journal_wal_tail{};
    std::uint64_t journal_wal_bytes{};
    std::optional<PendingPlan> pending;
    std::vector<ActivePlan> active;
    std::vector<OperatorRequest> operator_requests;
};

struct AuthoritativeState final {
    LogCursor release_frontier{};
    LogCursor wal_tail{};
    std::uint64_t wal_valid_bytes{};
};

[[nodiscard]] std::optional<RecoveryOperatorRequestId> operator_request_id(
    const CohortJournalMutation& mutation) noexcept {
    if (const auto* adopted = std::get_if<AdoptFrame>(&mutation)) {
        return adopted->operator_request_id;
    }
    if (const auto* reconfigured = std::get_if<ReconfigureFrame>(&mutation)) {
        return reconfigured->operator_request_id;
    }
    return std::nullopt;
}

[[nodiscard]] ActivePlan* find_active(
    Lifecycle& lifecycle,
    const WalFrameIdentity& frame) noexcept {
    const auto found = std::find_if(
        lifecycle.active.begin(),
        lifecycle.active.end(),
        [&frame](const ActivePlan& plan) { return plan.frame == frame; });
    return found == lifecycle.active.end() ? nullptr : &*found;
}

[[nodiscard]] PendingPlan* find_pending(
    Lifecycle& lifecycle,
    std::uint64_t ordinal,
    RecoveryPlanToken token) noexcept {
    return lifecycle.pending
            && lifecycle.pending->ordinal == ordinal
            && lifecycle.pending->token == token
        ? &*lifecycle.pending
        : nullptr;
}

[[nodiscard]] CohortJournalError invalid_record(
    std::uint64_t offset,
    std::string detail) {
    return make_error(
        CohortJournalErrorCode::InvalidRecord, offset, std::move(detail));
}

[[nodiscard]] JournalStatus validate_observed_cursors(
    Lifecycle& lifecycle,
    const LogCursor& release_frontier,
    const LogCursor& wal_tail,
    const AuthoritativeState* authoritative,
    std::uint64_t offset) {
    if (!cursor_valid(release_frontier) || !cursor_valid(wal_tail)
        || !cursor_not_after(release_frontier, wal_tail)) {
        return JournalStatus::failure(invalid_record(
            offset, "record contains an invalid release/WAL cursor pair"));
    }
    if (authoritative != nullptr) {
        if (release_frontier != authoritative->release_frontier
            || wal_tail != authoritative->wal_tail
            || authoritative->wal_tail != lifecycle.journal_wal_tail) {
            return JournalStatus::failure(make_error(
                CohortJournalErrorCode::StaleWrite,
                offset,
                "record's expected release frontier or WAL tail is stale"));
        }
    } else {
        if (!cursor_not_after(lifecycle.release_frontier, release_frontier)
            || wal_tail != lifecycle.journal_wal_tail) {
            return JournalStatus::failure(invalid_record(
                offset,
                "record regresses the release frontier or names an unjournaled WAL tail"));
        }
    }
    return success_status();
}

[[nodiscard]] JournalStatus validate_and_apply(
    Lifecycle& lifecycle,
    const CohortJournalMutation& mutation,
    std::uint64_t ordinal,
    std::uint64_t offset,
    const CohortJournalLimits& limits,
    const AuthoritativeState* authoritative,
    bool apply) {
    if (const auto request_id = operator_request_id(mutation)) {
        if (!static_cast<bool>(*request_id)) {
            return JournalStatus::failure(invalid_record(
                offset, "operator request ID is zero"));
        }
        const auto reused = std::find_if(
            lifecycle.operator_requests.begin(),
            lifecycle.operator_requests.end(),
            [request_id](const OperatorRequest& request) {
                return request.request_id == *request_id;
            });
        if (reused != lifecycle.operator_requests.end()) {
            return JournalStatus::failure(invalid_record(
                offset, "operator request ID appears in two physical records"));
        }
    }

    auto status = std::visit(
        [&lifecycle, ordinal, offset, &limits, authoritative, apply](const auto& record) {
            using Record = std::decay_t<decltype(record)>;
            if constexpr (std::is_same_v<Record, BeginFrame>) {
                if (!static_cast<bool>(record.plan_token)
                    || !static_cast<bool>(record.cut_director_epoch)
                    || record.plan_generation != 1U
                    || record.next_frame == 0U
                    || record.expected_before.through_frame
                        == std::numeric_limits<std::uint64_t>::max()
                    || record.next_frame
                        != record.expected_before.through_frame + 1U
                    || !cursor_valid(record.expected_before)
                    || record.expected_before != lifecycle.journal_wal_tail
                    || (authoritative != nullptr
                        && record.expected_before != authoritative->wal_tail)
                    || lifecycle.pending
                    || !is_canonical_recovery_cohort(
                        record.cohort,
                        limits.max_cohort_members,
                        record.empty_confirmation)
                    || record.plan_fingerprint
                        != recovery_plan_fingerprint(
                            record.plan_generation,
                            record.plan_token,
                            record.cohort,
                            record.empty_confirmation)) {
                    return JournalStatus::failure(invalid_record(
                        offset,
                        "BeginFrame violates its cursor, token, generation, cohort, or fingerprint invariants"));
                }
                if (apply) {
                    lifecycle.pending.emplace(PendingPlan{
                        ordinal,
                        record.plan_token,
                        record.expected_before,
                        record.next_frame,
                        record.plan_generation,
                        record.plan_fingerprint,
                    });
                }
                return success_status();
            } else if constexpr (std::is_same_v<Record, BindFrame>) {
                auto* pending = find_pending(
                    lifecycle, record.begin_ordinal, record.begin_token);
                if (pending == nullptr
                    || !valid_frame_identity(record.frame)
                    || record.frame.before != pending->expected_before
                    || record.frame.after.through_frame
                        != pending->next_frame
                    || record.frame.offset != lifecycle.journal_wal_bytes
                    || record.plan_generation
                        != pending->generation
                    || record.plan_token != pending->token
                    || record.plan_fingerprint != pending->fingerprint) {
                    return JournalStatus::failure(invalid_record(
                        offset,
                        "BindFrame does not exactly bind one live BeginFrame and contiguous WAL identity"));
                }
                const auto frame_end = record.frame.offset + record.frame.bytes;
                const auto in_captured_wal = authoritative == nullptr
                    || (cursor_not_after(
                            record.frame.after, authoritative->wal_tail)
                        && frame_end <= authoritative->wal_valid_bytes);
                const auto appended_after_capture = authoritative != nullptr
                    && record.frame.before == authoritative->wal_tail
                    && record.frame.offset
                        == authoritative->wal_valid_bytes;
                if (!in_captured_wal && !appended_after_capture) {
                    return JournalStatus::failure(make_error(
                        CohortJournalErrorCode::StaleWrite,
                        offset,
                        "BindFrame is not present in the captured WAL prefix"));
                }
                if (apply) {
                    lifecycle.journal_wal_tail = record.frame.after;
                    lifecycle.journal_wal_bytes = frame_end;
                    lifecycle.active.push_back(ActivePlan{
                        record.frame,
                        record.plan_generation,
                        record.plan_token,
                        record.plan_fingerprint,
                        ordinal,
                        0U,
                        {},
                        {},
                    });
                    lifecycle.pending.reset();
                }
                return success_status();
            } else if constexpr (std::is_same_v<Record, AbortFrame>) {
                auto* pending = find_pending(
                    lifecycle, record.begin_ordinal, record.begin_token);
                if (pending == nullptr
                    || !valid_abort_reason(record.reason)) {
                    return JournalStatus::failure(invalid_record(
                        offset,
                        "AbortFrame does not terminate one live unbound BeginFrame"));
                }
                if (apply) {
                    lifecycle.pending.reset();
                }
                return success_status();
            } else if constexpr (std::is_same_v<Record, AdoptFrame>) {
                if (!valid_frame_identity(record.frame)
                    || record.frame.before != lifecycle.journal_wal_tail
                    || record.frame.offset != lifecycle.journal_wal_bytes
                    || !static_cast<bool>(record.operator_request_id)
                    || record.provenance
                        != AdoptionProvenance::LegacyUncovered
                    || record.plan_generation != 1U
                    || !static_cast<bool>(record.plan_token)
                    || lifecycle.pending
                    || !is_canonical_recovery_cohort(
                        record.cohort,
                        limits.max_cohort_members,
                        record.empty_confirmation)
                    || record.plan_fingerprint
                        != recovery_plan_fingerprint(
                            record.plan_generation,
                            record.plan_token,
                            record.cohort,
                            record.empty_confirmation)) {
                    return JournalStatus::failure(invalid_record(
                        offset,
                        "AdoptFrame violates its provenance, WAL, token, cohort, or fingerprint invariants"));
                }
                const auto frame_end = record.frame.offset + record.frame.bytes;
                const auto in_captured_wal = authoritative == nullptr
                    || (cursor_not_after(
                            record.frame.after, authoritative->wal_tail)
                        && frame_end <= authoritative->wal_valid_bytes);
                if (!in_captured_wal) {
                    return JournalStatus::failure(make_error(
                        CohortJournalErrorCode::StaleWrite,
                        offset,
                        "AdoptFrame is not present in the captured WAL prefix"));
                }
                if (apply) {
                    lifecycle.journal_wal_tail = record.frame.after;
                    lifecycle.journal_wal_bytes = frame_end;
                    lifecycle.active.push_back(ActivePlan{
                        record.frame,
                        record.plan_generation,
                        record.plan_token,
                        record.plan_fingerprint,
                        ordinal,
                        0U,
                        {},
                        {},
                    });
                }
                return success_status();
            } else if constexpr (std::is_same_v<Record, ReconfigureFrame>) {
                auto* active = find_active(lifecycle, record.frame);
                auto observed = validate_observed_cursors(
                    lifecycle,
                    record.expected_release_frontier,
                    record.expected_wal_tail,
                    authoritative,
                    offset);
                if (!observed) {
                    return observed;
                }
                if (active == nullptr
                    || active->generation != record.expected_plan_generation
                    || active->token != record.expected_plan_token
                    || active->fingerprint
                        != record.expected_plan_fingerprint
                    || active->journal_ordinal
                        != record.expected_plan_journal_ordinal
                    || record.expected_plan_generation
                        == std::numeric_limits<std::uint64_t>::max()
                    || record.new_plan_generation
                        != record.expected_plan_generation + 1U
                    || !static_cast<bool>(record.new_plan_token)
                    || record.new_plan_token == active->token
                    || std::find(
                        active->prior_plan_tokens.begin(),
                        active->prior_plan_tokens.end(),
                        record.new_plan_token)
                        != active->prior_plan_tokens.end()
                    || !static_cast<bool>(record.operator_request_id)
                    || !valid_reconfigure_reason(record.reason)
                    || !cursor_not_after(
                        record.frame.after, record.expected_wal_tail)
                    || record.expected_release_frontier
                        != record.frame.before
                    || !is_canonical_recovery_cohort(
                        record.cohort,
                        limits.max_cohort_members,
                        record.empty_confirmation)
                    || record.new_plan_fingerprint
                        != recovery_plan_fingerprint(
                            record.new_plan_generation,
                            record.new_plan_token,
                            record.cohort,
                            record.empty_confirmation)) {
                    return JournalStatus::failure(invalid_record(
                        offset,
                        "ReconfigureFrame failed its full active-plan or inventory CAS"));
                }
                if (apply) {
                    lifecycle.release_frontier =
                        record.expected_release_frontier;
                    active->prior_plan_tokens.push_back(active->token);
                    active->generation = record.new_plan_generation;
                    active->token = record.new_plan_token;
                    active->fingerprint = record.new_plan_fingerprint;
                    active->journal_ordinal = ordinal;
                    active->attempt_generation = 0U;
                }
                return success_status();
            } else {
                auto* active = find_active(lifecycle, record.frame);
                auto observed = validate_observed_cursors(
                    lifecycle,
                    record.release_frontier,
                    record.wal_tail,
                    authoritative,
                    offset);
                if (!observed) {
                    return observed;
                }
                if (active == nullptr
                    || active->generation != record.active_plan_generation
                    || active->token != record.active_plan_token
                    || active->journal_ordinal
                        != record.active_plan_journal_ordinal
                    || active->attempt_generation
                        == std::numeric_limits<std::uint64_t>::max()
                    || record.attempt_generation
                        != active->attempt_generation + 1U
                    || !static_cast<bool>(record.attempt_token)
                    || std::find(
                        active->used_attempt_tokens.begin(),
                        active->used_attempt_tokens.end(),
                        record.attempt_token)
                        != active->used_attempt_tokens.end()
                    || !static_cast<bool>(record.director_epoch)
                    || !static_cast<bool>(record.session_namespace)
                    || !cursor_not_after(record.frame.after, record.wal_tail)
                    || record.release_frontier != record.frame.before) {
                    return JournalStatus::failure(invalid_record(
                        offset,
                        "AttemptStarted failed its attempt, session, plan, or inventory fencing"));
                }
                if (apply) {
                    lifecycle.release_frontier = record.release_frontier;
                    active->used_attempt_tokens.push_back(
                        record.attempt_token);
                    active->attempt_generation = record.attempt_generation;
                }
                return success_status();
            }
        },
        mutation);
    if (!status) {
        return status;
    }
    if (const auto request_id = operator_request_id(mutation)) {
        if (apply) {
            lifecycle.operator_requests.push_back(OperatorRequest{*request_id});
        }
    }
    return success_status();
}

[[nodiscard]] Lifecycle initial_lifecycle(
    const CohortJournalBinding& binding) {
    Lifecycle lifecycle;
    lifecycle.release_frontier = binding.anchor_release_cursor;
    lifecycle.journal_wal_tail = binding.anchor_release_cursor;
    lifecycle.journal_wal_bytes = binding.anchor_wal_byte_offset;
    return lifecycle;
}

void reserve_lifecycle_append(
    Lifecycle& lifecycle,
    const CohortJournalMutation& mutation) {
    std::visit(
        [&lifecycle](const auto& record) {
            using Record = std::decay_t<decltype(record)>;
            if constexpr (std::is_same_v<Record, BindFrame>) {
                lifecycle.active.reserve(lifecycle.active.size() + 1U);
            } else if constexpr (std::is_same_v<Record, AdoptFrame>) {
                lifecycle.active.reserve(lifecycle.active.size() + 1U);
                lifecycle.operator_requests.reserve(
                    lifecycle.operator_requests.size() + 1U);
            } else if constexpr (std::is_same_v<Record, ReconfigureFrame>) {
                lifecycle.operator_requests.reserve(
                    lifecycle.operator_requests.size() + 1U);
                if (auto* active = find_active(lifecycle, record.frame)) {
                    active->prior_plan_tokens.reserve(
                        active->prior_plan_tokens.size() + 1U);
                }
            } else if constexpr (std::is_same_v<Record, AttemptStarted>) {
                if (auto* active = find_active(lifecycle, record.frame)) {
                    active->used_attempt_tokens.reserve(
                        active->used_attempt_tokens.size() + 1U);
                }
            }
        },
        mutation);
}

[[nodiscard]] std::array<std::byte, kHeaderSize> make_header(
    const CohortJournalBinding& binding) noexcept {
    std::array<std::byte, kHeaderSize> bytes{};
    std::copy(kHeaderMagic.begin(), kHeaderMagic.end(), bytes.begin());
    auto span = std::span<std::byte>{bytes};
    put_u16(span, 8U, kFormatVersion);
    put_u16(span, 10U, static_cast<std::uint16_t>(kHeaderSize));
    put_u32(span, 12U, 0U);
    put_u64(span, 16U, binding.journal_uuid.high);
    put_u64(span, 24U, binding.journal_uuid.low);
    put_u64(span, 32U, binding.show_id.high);
    put_u64(span, 40U, binding.show_id.low);
    put_u64(span, 48U, binding.wal_base.through_frame);
    put_u64(span, 56U, binding.wal_base.revision);
    put_u64(span, 64U, binding.wal_base.next_command);
    put_u64(span, 72U, binding.anchor_release_cursor.through_frame);
    put_u64(span, 80U, binding.anchor_release_cursor.revision);
    put_u64(span, 88U, binding.anchor_release_cursor.next_command);
    put_u64(span, 96U, binding.anchor_wal_byte_offset);
    put_u64(span, 104U, binding.release_decision_count);
    put_u64(span, 112U, binding.release_ledger_valid_bytes);
    put_u32(span, kHeaderCrcOffset, crc32c(
        std::span<const std::byte>{bytes}.first(kHeaderCrcOffset)));
    return bytes;
}

[[nodiscard]] core::Result<CohortJournalBinding, CohortJournalError>
decode_header(std::span<const std::byte, kHeaderSize> bytes) {
    using Result = core::Result<CohortJournalBinding, CohortJournalError>;
    if (!has_magic(bytes, 0U, kHeaderMagic)
        || get_u16(bytes, 10U) != static_cast<std::uint16_t>(kHeaderSize)
        || get_u32(bytes, 12U) != 0U
        || !is_zero(bytes.subspan(120U, 4U))
        || get_u32(bytes, kHeaderCrcOffset)
            != crc32c(bytes.first(kHeaderCrcOffset))) {
        return Result::failure(make_error(
            CohortJournalErrorCode::InvalidHeader,
            0U,
            "cohort-journal header failed structural or CRC validation"));
    }
    if (get_u16(bytes, 8U) != kFormatVersion) {
        return Result::failure(make_error(
            CohortJournalErrorCode::UnsupportedVersion,
            0U,
            "unsupported cohort-journal format version"));
    }
    CohortJournalBinding binding{
        CohortJournalUuid{get_u64(bytes, 16U), get_u64(bytes, 24U)},
        RecoveryShowId{get_u64(bytes, 32U), get_u64(bytes, 40U)},
        LogCursor{
            get_u64(bytes, 48U),
            get_u64(bytes, 56U),
            get_u64(bytes, 64U),
        },
        LogCursor{
            get_u64(bytes, 72U),
            get_u64(bytes, 80U),
            get_u64(bytes, 88U),
        },
        get_u64(bytes, 96U),
        get_u64(bytes, 104U),
        get_u64(bytes, 112U),
    };
    if (!static_cast<bool>(binding.journal_uuid)
        || !static_cast<bool>(binding.show_id)
        || !cursor_valid(binding.wal_base)
        || !cursor_valid(binding.anchor_release_cursor)
        || !cursor_not_after(
            binding.wal_base, binding.anchor_release_cursor)
        || binding.anchor_wal_byte_offset < 64U
        || binding.release_decision_count
            != binding.anchor_release_cursor.through_frame
                - binding.wal_base.through_frame
        || binding.release_decision_count
            > (std::numeric_limits<std::uint64_t>::max() - 64U) / 128U
        || binding.release_ledger_valid_bytes
            != 64U + binding.release_decision_count * 128U) {
        return Result::failure(make_error(
            CohortJournalErrorCode::InvalidHeader,
            0U,
            "cohort-journal header binding is invalid"));
    }
    return Result::success(binding);
}

[[nodiscard]] JournalStatus sync_file(
    const std::filesystem::path& path,
    SyncPolicy policy) {
    if (policy == SyncPolicy::None) {
        return JournalStatus::failure(make_error(
            CohortJournalErrorCode::NonDurablePolicy,
            0U,
            "cohort journal requires Data or Full sync policy"));
    }
#ifdef _WIN32
    const auto handle = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return JournalStatus::failure(make_error(
            CohortJournalErrorCode::SyncFailed,
            0U,
            "could not open cohort journal for durable sync",
            std::error_code{
                static_cast<int>(GetLastError()), std::system_category()}));
    }
    const auto flushed = FlushFileBuffers(handle) != 0;
    const auto flush_error = flushed
        ? std::error_code{}
        : std::error_code{
            static_cast<int>(GetLastError()), std::system_category()};
    const auto closed = CloseHandle(handle) != 0;
    if (!flushed || !closed) {
        return JournalStatus::failure(make_error(
            CohortJournalErrorCode::SyncFailed,
            0U,
            "durable cohort-journal sync failed",
            flushed
                ? std::error_code{
                    static_cast<int>(GetLastError()), std::system_category()}
                : flush_error));
    }
#else
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDWR);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return JournalStatus::failure(make_error(
            CohortJournalErrorCode::SyncFailed,
            0U,
            "could not open cohort journal for durable sync",
            current_io_error()));
    }
    int sync_result = 0;
    do {
        sync_result = policy == SyncPolicy::Data ? ::fdatasync(descriptor)
                                                  : ::fsync(descriptor);
    } while (sync_result != 0 && errno == EINTR);
    const auto sync_error = sync_result == 0
        ? std::error_code{}
        : current_io_error();
    const auto close_result = ::close(descriptor);
    if (sync_result != 0 || close_result != 0) {
        return JournalStatus::failure(make_error(
            CohortJournalErrorCode::SyncFailed,
            0U,
            "durable cohort-journal sync failed",
            sync_result != 0 ? sync_error : current_io_error()));
    }
#endif
    return success_status();
}

[[nodiscard]] JournalStatus sync_parent_directory(
    const std::filesystem::path& path) {
#ifdef _WIN32
    // FlushFileBuffers on the newly created file is the durable primitive
    // available for ordinary files. Windows has no portable directory fsync.
    static_cast<void>(path);
    return success_status();
#else
    auto parent = path.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    auto flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    int descriptor = -1;
    do {
        descriptor = ::open(parent.c_str(), flags);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return JournalStatus::failure(make_error(
            CohortJournalErrorCode::SyncFailed,
            0U,
            "could not open cohort-journal parent directory for sync",
            current_io_error()));
    }
    int sync_result = 0;
    do {
        sync_result = ::fsync(descriptor);
    } while (sync_result != 0 && errno == EINTR);
    const auto sync_error = sync_result == 0
        ? std::error_code{}
        : current_io_error();
    const auto close_result = ::close(descriptor);
    if (sync_result != 0 || close_result != 0) {
        return JournalStatus::failure(make_error(
            CohortJournalErrorCode::SyncFailed,
            0U,
            "durable cohort-journal parent-directory sync failed",
            sync_result != 0 ? sync_error : current_io_error()));
    }
    return success_status();
#endif
}

[[nodiscard]] core::Result<std::uint64_t, CohortJournalError>
checked_file_size(
    const std::filesystem::path& path,
    const CohortJournalLimits& limits) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return core::Result<std::uint64_t, CohortJournalError>::failure(
            make_error(
                CohortJournalErrorCode::ReadFailed,
                0U,
                "could not determine cohort-journal size",
                error));
    }
    if (size > limits.max_file_bytes
        || size > std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<std::uint64_t, CohortJournalError>::failure(
            make_error(
                CohortJournalErrorCode::LimitExceeded,
                0U,
                "cohort journal exceeds its configured file cap"));
    }
    return core::Result<std::uint64_t, CohortJournalError>::success(
        static_cast<std::uint64_t>(size));
}

[[nodiscard]] JournalStatus write_bytes(
    const std::filesystem::path& path,
    std::span<const std::byte> bytes,
    bool truncate) {
    auto mode = std::ios::binary | std::ios::out;
    mode |= truncate ? std::ios::trunc : std::ios::app;
    std::ofstream stream(path, mode);
    if (!stream) {
        return JournalStatus::failure(make_error(
            CohortJournalErrorCode::OpenFailed,
            0U,
            "could not open cohort journal for writing",
            current_io_error()));
    }
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) {
        return JournalStatus::failure(make_error(
            CohortJournalErrorCode::WriteFailed,
            0U,
            "could not write cohort-journal bytes",
            current_io_error()));
    }
    stream.close();
    if (!stream) {
        return JournalStatus::failure(make_error(
            CohortJournalErrorCode::WriteFailed,
            0U,
            "could not close cohort journal after writing",
            current_io_error()));
    }
    return success_status();
}

[[nodiscard]] JournalStatus read_exact(
    const std::filesystem::path& path,
    std::uint64_t offset,
    std::span<std::byte> destination) {
    if (offset > static_cast<std::uint64_t>(
                     std::numeric_limits<std::streamoff>::max())
        || destination.size()
            > static_cast<std::size_t>(
                std::numeric_limits<std::streamsize>::max())) {
        return JournalStatus::failure(make_error(
            CohortJournalErrorCode::LimitExceeded,
            offset,
            "cohort-journal read is not addressable"));
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return JournalStatus::failure(make_error(
            CohortJournalErrorCode::OpenFailed,
            offset,
            "could not open cohort journal for reading",
            current_io_error()));
    }
    stream.seekg(static_cast<std::streamoff>(offset));
    stream.read(
        reinterpret_cast<char*>(destination.data()),
        static_cast<std::streamsize>(destination.size()));
    if (stream.gcount() != static_cast<std::streamsize>(destination.size())) {
        return JournalStatus::failure(make_error(
            CohortJournalErrorCode::ReadFailed,
            offset,
            "could not read complete cohort-journal bytes",
            current_io_error()));
    }
    return success_status();
}

[[nodiscard]] JournalStatus initialize_file(
    const std::filesystem::path& path,
    const CohortJournalBinding& binding,
    SyncPolicy policy) {
    const auto header = make_header(binding);
    auto written = write_bytes(path, header, true);
    if (!written) {
        return written;
    }
    auto synced = sync_file(path, policy);
    if (!synced) {
        return synced;
    }
    return sync_parent_directory(path);
}

struct EncodedRecord final {
    std::vector<std::byte> bytes;
    std::uint32_t record_crc32c{};
};

[[nodiscard]] core::Result<EncodedRecord, CohortJournalError> encode_record(
    std::uint64_t ordinal,
    std::uint32_t previous_record_crc32c,
    const CohortJournalMutation& mutation,
    const CohortJournalLimits& limits,
    std::uint64_t offset) {
    using Result = core::Result<EncodedRecord, CohortJournalError>;
    auto payload = encode_payload(mutation);
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()
        || payload.size()
            > std::numeric_limits<std::size_t>::max() - kMinimumRecordSize) {
        return Result::failure(make_error(
            CohortJournalErrorCode::LimitExceeded,
            offset,
            "cohort-journal payload length is not representable"));
    }
    const auto total_size = kMinimumRecordSize + payload.size();
    if (total_size > limits.max_record_bytes
        || total_size > std::numeric_limits<std::uint32_t>::max()) {
        return Result::failure(make_error(
            CohortJournalErrorCode::LimitExceeded,
            offset,
            "cohort-journal record exceeds its configured byte cap"));
    }
    std::vector<std::byte> bytes(total_size);
    std::copy(kRecordMagic.begin(), kRecordMagic.end(), bytes.begin());
    auto span = std::span<std::byte>{bytes};
    put_u16(span, 8U, kFormatVersion);
    put_u16(span, 10U, static_cast<std::uint16_t>(kPrefixSize));
    span[12U] = static_cast<std::byte>(kind_value(mutation));
    span[13U] = std::byte{0U};
    put_u16(span, 14U, 0U);
    put_u64(span, 16U, static_cast<std::uint64_t>(total_size));
    put_u64(span, 24U, ordinal);
    put_u32(span, 32U, previous_record_crc32c);
    put_u32(span, 36U, static_cast<std::uint32_t>(payload.size()));
    put_u32(
        span,
        kPrefixCrcOffset,
        crc32c(std::span<const std::byte>{bytes}.first(kPrefixCrcOffset)));
    std::copy(
        payload.begin(), payload.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(kPrefixSize));
    const auto trailer_offset = total_size - kTrailerSize;
    std::copy(
        kCommitMagic.begin(), kCommitMagic.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(trailer_offset));
    put_u64(
        span,
        trailer_offset + 8U,
        static_cast<std::uint64_t>(total_size));
    const auto record_crc = crc32c(
        std::span<const std::byte>{bytes}.first(trailer_offset + 16U));
    put_u32(span, trailer_offset + 16U, record_crc);
    put_u32(
        span,
        trailer_offset + 20U,
        crc32c(std::span<const std::byte>{bytes}.subspan(
            trailer_offset, 20U)));
    return Result::success(EncodedRecord{std::move(bytes), record_crc});
}

[[nodiscard]] core::Result<bool, CohortJournalError>
has_authenticated_commit_suffix_at_or_after(
    const std::filesystem::path& path,
    std::uint64_t scan_offset,
    std::uint64_t file_size) {
    using Result = core::Result<bool, CohortJournalError>;
    if (scan_offset > file_size || file_size - scan_offset < kTrailerSize) {
        return Result::success(false);
    }
    if (file_size > static_cast<std::uint64_t>(
                        std::numeric_limits<std::streamoff>::max())) {
        return Result::failure(make_error(
            CohortJournalErrorCode::LimitExceeded,
            scan_offset,
            "cohort-journal suffix exceeds stream addressing range"));
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return Result::failure(make_error(
            CohortJournalErrorCode::OpenFailed,
            scan_offset,
            "could not open cohort journal while scanning commit evidence",
            current_io_error()));
    }
    stream.seekg(static_cast<std::streamoff>(scan_offset));
    constexpr std::size_t chunk_bytes = 64U * 1024U;
    constexpr std::size_t overlap_bytes = kTrailerSize - 1U;
    std::array<std::byte, chunk_bytes + overlap_bytes> buffer{};
    std::size_t overlap = 0U;
    auto read_offset = scan_offset;
    while (read_offset < file_size) {
        const auto remaining = file_size - read_offset;
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, chunk_bytes));
        stream.read(
            reinterpret_cast<char*>(buffer.data() + overlap),
            static_cast<std::streamsize>(requested));
        if (stream.gcount() != static_cast<std::streamsize>(requested)) {
            return Result::failure(make_error(
                CohortJournalErrorCode::ReadFailed,
                read_offset,
                "could not scan cohort-journal suffix for commit evidence",
                current_io_error()));
        }
        const auto available = overlap + requested;
        const auto buffer_offset = read_offset - overlap;
        const auto view = std::span<const std::byte>{buffer}.first(available);
        for (std::size_t local = 0U;
             local + kTrailerSize <= available;
             ++local) {
            const auto trailer = view.subspan(local, kTrailerSize);
            if (!has_magic(trailer, 0U, kCommitMagic)
                || get_u32(trailer, 20U) != crc32c(trailer.first(20U))) {
                continue;
            }
            const auto total = get_u64(trailer, 8U);
            const auto trailer_offset =
                buffer_offset + static_cast<std::uint64_t>(local);
            const auto candidate_end = trailer_offset + kTrailerSize;
            if (total >= kMinimumRecordSize
                && total <= kAbsoluteMaxRecordBytes
                && total <= candidate_end
                && candidate_end - total >= scan_offset) {
                return Result::success(true);
            }
        }
        read_offset += requested;
        const auto next_overlap = std::min(overlap_bytes, available);
        for (std::size_t index = 0U; index < next_overlap; ++index) {
            buffer[index] = buffer[available - next_overlap + index];
        }
        overlap = next_overlap;
    }
    return Result::success(false);
}

struct DecodedRecord final {
    CohortJournalRecord record;
};

[[nodiscard]] core::Result<DecodedRecord, CohortJournalError> decode_record(
    const std::filesystem::path& path,
    std::uint64_t offset,
    std::uint64_t file_size,
    const CohortJournalLimits& limits) {
    using Result = core::Result<DecodedRecord, CohortJournalError>;
    if (offset > file_size || file_size - offset < kPrefixSize) {
        return Result::failure(make_error(
            CohortJournalErrorCode::TornTail,
            offset,
            "cohort journal ends inside a record prefix"));
    }
    std::array<std::byte, kPrefixSize> prefix{};
    auto read_prefix = read_exact(path, offset, prefix);
    if (!read_prefix) {
        return Result::failure(std::move(read_prefix).error());
    }
    const auto prefix_view = std::span<const std::byte>{prefix};
    const auto structurally_valid =
        has_magic(prefix_view, 0U, kRecordMagic)
        && get_u16(prefix_view, 10U)
            == static_cast<std::uint16_t>(kPrefixSize)
        && std::to_integer<std::uint8_t>(prefix_view[13U]) == 0U
        && get_u16(prefix_view, 14U) == 0U
        && is_zero(prefix_view.subspan(44U, 20U))
        && get_u32(prefix_view, kPrefixCrcOffset)
            == crc32c(prefix_view.first(kPrefixCrcOffset));
    if (!structurally_valid) {
        auto evidence = has_authenticated_commit_suffix_at_or_after(
            path, offset, file_size);
        if (!evidence) {
            return Result::failure(std::move(evidence).error());
        }
        return Result::failure(make_error(
            evidence.value() ? CohortJournalErrorCode::CorruptRecord
                             : CohortJournalErrorCode::TornTail,
            offset,
            evidence.value()
                ? "invalid cohort-journal prefix precedes commit evidence"
                : "final cohort-journal append has no authenticated commit marker"));
    }
    if (get_u16(prefix_view, 8U) != kFormatVersion) {
        return Result::failure(make_error(
            CohortJournalErrorCode::UnsupportedVersion,
            offset,
            "unsupported cohort-journal record version"));
    }
    const auto total = get_u64(prefix_view, 16U);
    const auto payload_bytes = get_u32(prefix_view, 36U);
    if (total < kMinimumRecordSize
        || total > limits.max_record_bytes
        || total != kMinimumRecordSize + payload_bytes) {
        return Result::failure(make_error(
            CohortJournalErrorCode::CorruptRecord,
            offset,
            "cohort-journal record length is invalid"));
    }
    if (total > file_size - offset) {
        auto evidence = has_authenticated_commit_suffix_at_or_after(
            path, offset, file_size);
        if (!evidence) {
            return Result::failure(std::move(evidence).error());
        }
        return Result::failure(make_error(
            evidence.value() ? CohortJournalErrorCode::CorruptRecord
                             : CohortJournalErrorCode::TornTail,
            offset,
            evidence.value()
                ? "record length crosses later authenticated commit evidence"
                : "cohort journal ends before the final record commit trailer"));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(total));
    auto read_record = read_exact(path, offset, bytes);
    if (!read_record) {
        return Result::failure(std::move(read_record).error());
    }
    const auto view = std::span<const std::byte>{bytes};
    const auto trailer_offset = static_cast<std::size_t>(total) - kTrailerSize;
    const auto trailer = view.subspan(trailer_offset, kTrailerSize);
    const auto valid_commit = has_magic(trailer, 0U, kCommitMagic)
        && get_u64(trailer, 8U) == total
        && get_u32(trailer, 20U) == crc32c(trailer.first(20U));
    if (!valid_commit) {
        const auto final_record = offset + total == file_size;
        if (final_record) {
            auto evidence = has_authenticated_commit_suffix_at_or_after(
                path, offset, file_size);
            if (!evidence) {
                return Result::failure(std::move(evidence).error());
            }
            if (evidence.value()) {
                return Result::failure(make_error(
                    CohortJournalErrorCode::CorruptRecord,
                    offset,
                    "invalid final trailer crosses authenticated commit evidence"));
            }
        }
        return Result::failure(make_error(
            final_record ? CohortJournalErrorCode::TornTail
                         : CohortJournalErrorCode::CorruptRecord,
            offset,
            final_record
                ? "final cohort-journal record has no valid commit marker"
                : "interior cohort-journal record has an invalid commit marker"));
    }
    const auto stored_record_crc = get_u32(trailer, 16U);
    if (stored_record_crc != crc32c(view.first(trailer_offset + 16U))) {
        return Result::failure(make_error(
            CohortJournalErrorCode::CorruptRecord,
            offset,
            "committed cohort-journal record failed body CRC validation"));
    }
    const auto raw_kind = std::to_integer<std::uint8_t>(view[12U]);
    if (raw_kind < static_cast<std::uint8_t>(
                       CohortJournalRecordKind::BeginFrame)
        || raw_kind > static_cast<std::uint8_t>(
            CohortJournalRecordKind::AttemptStarted)) {
        return Result::failure(make_error(
            CohortJournalErrorCode::CorruptRecord,
            offset,
            "cohort-journal record kind is unknown"));
    }
    auto mutation = decode_payload(
        static_cast<CohortJournalRecordKind>(raw_kind),
        view.subspan(kPrefixSize, payload_bytes),
        offset,
        limits);
    if (!mutation) {
        return Result::failure(std::move(mutation).error());
    }
    return Result::success(DecodedRecord{CohortJournalRecord{
        get_u64(view, 24U),
        offset,
        total,
        stored_record_crc,
        std::move(mutation).value(),
    }});
}

struct JournalScan final {
    CohortJournalRecoveryReport recovery{};
    std::vector<CohortJournalRecord> records;
    Lifecycle lifecycle;
};

[[nodiscard]] core::Result<JournalScan, CohortJournalError> scan_journal(
    const std::filesystem::path& path,
    const CohortJournalBinding& binding,
    const CohortJournalOpenOptions& options,
    std::uint64_t file_size) {
    using Result = core::Result<JournalScan, CohortJournalError>;
    JournalScan scan;
    scan.lifecycle = initial_lifecycle(binding);
    scan.recovery.valid_bytes = kHeaderSize;
    scan.records.reserve(std::min<std::uint32_t>(
        options.limits.max_records, 1024U));
    auto offset = static_cast<std::uint64_t>(kHeaderSize);
    while (offset < file_size) {
        if (scan.recovery.record_count >= options.limits.max_records) {
            return Result::failure(make_error(
                CohortJournalErrorCode::LimitExceeded,
                offset,
                "cohort journal exceeds its configured record cap"));
        }
        auto decoded = decode_record(path, offset, file_size, options.limits);
        if (!decoded) {
            if (decoded.error().code == CohortJournalErrorCode::TornTail
                && options.repair_torn_tail) {
                break;
            }
            return Result::failure(std::move(decoded).error());
        }
        auto& record = decoded.value().record;
        if (scan.recovery.record_count
            == std::numeric_limits<std::uint64_t>::max()) {
            return Result::failure(make_error(
                CohortJournalErrorCode::LimitExceeded,
                offset,
                "cohort-journal ordinal space is exhausted"));
        }
        const auto expected_ordinal = scan.recovery.record_count + 1U;
        std::array<std::byte, kPrefixSize> prefix{};
        auto reread = read_exact(path, offset, prefix);
        if (!reread) {
            return Result::failure(std::move(reread).error());
        }
        const auto previous_crc = get_u32(
            std::span<const std::byte>{prefix}, 32U);
        if (record.ordinal != expected_ordinal
            || previous_crc != scan.recovery.last_record_crc32c) {
            return Result::failure(make_error(
                CohortJournalErrorCode::ChainMismatch,
                offset,
                "cohort-journal ordinal or previous-record CRC chain is broken"));
        }
        auto applied = validate_and_apply(
            scan.lifecycle,
            record.mutation,
            record.ordinal,
            record.offset,
            options.limits,
            nullptr,
            true);
        if (!applied) {
            return Result::failure(std::move(applied).error());
        }
        scan.recovery.record_count = expected_ordinal;
        scan.recovery.last_ordinal = record.ordinal;
        scan.recovery.last_record_crc32c = record.record_crc32c;
        if (record.bytes
            > std::numeric_limits<std::uint64_t>::max() - offset) {
            return Result::failure(make_error(
                CohortJournalErrorCode::LimitExceeded,
                offset,
                "cohort-journal record offset overflows"));
        }
        offset += record.bytes;
        scan.recovery.valid_bytes = offset;
        scan.records.push_back(std::move(record));
    }
    if (offset != file_size) {
        if (!options.repair_torn_tail) {
            return Result::failure(make_error(
                CohortJournalErrorCode::TornTail,
                offset,
                "cohort journal has a torn final record"));
        }
        std::error_code truncate_error;
        std::filesystem::resize_file(path, offset, truncate_error);
        if (truncate_error) {
            return Result::failure(make_error(
                CohortJournalErrorCode::TruncateFailed,
                offset,
                "could not remove torn cohort-journal tail",
                truncate_error));
        }
        auto synced = sync_file(path, SyncPolicy::Data);
        if (!synced) {
            return Result::failure(std::move(synced).error());
        }
        scan.recovery.repaired_torn_tail = true;
        scan.recovery.truncated_bytes = file_size - offset;
    }
    return Result::success(std::move(scan));
}

[[nodiscard]] JournalStatus validate_existing_binding(
    const CohortJournalBinding& binding,
    CohortJournalUuid expected_uuid,
    RecoveryShowId expected_show,
    const UnresolvedReleaseInventory& inventory,
    const Lifecycle& lifecycle) {
    if (binding.journal_uuid != expected_uuid
        || binding.show_id != expected_show
        || binding.wal_base != inventory.base_cursor) {
        return JournalStatus::failure(make_error(
            CohortJournalErrorCode::BindingMismatch,
            0U,
            "cohort journal UUID, show ID, or WAL base does not match"));
    }
    if (binding.release_decision_count > inventory.released_frame_count
        || binding.release_ledger_valid_bytes
            > inventory.release_ledger_valid_bytes
        || binding.anchor_wal_byte_offset > inventory.released_wal_bytes
        || !cursor_not_after(
            binding.anchor_release_cursor, inventory.released_frontier)
        || !cursor_not_after(lifecycle.release_frontier,
                             inventory.released_frontier)
        || !cursor_not_after(lifecycle.journal_wal_tail, inventory.wal_tail)
        || lifecycle.journal_wal_bytes > inventory.wal_valid_bytes) {
        return JournalStatus::failure(make_error(
            CohortJournalErrorCode::BindingMismatch,
            0U,
            "captured WAL or release-ledger state regresses the journal binding"));
    }
    return success_status();
}

struct IdempotentLookup final {
    const CohortJournalRecord* record{};
    bool conflict{};
};

[[nodiscard]] IdempotentLookup find_idempotent_request(
    const std::vector<CohortJournalRecord>& records,
    const CohortJournalMutation& mutation) noexcept {
    const auto request_id = operator_request_id(mutation);
    if (!request_id) {
        return {};
    }
    for (const auto& record : records) {
        const auto existing_id = operator_request_id(record.mutation);
        if (!existing_id || *existing_id != *request_id) {
            continue;
        }
        if (record.mutation != mutation) {
            return IdempotentLookup{&record, true};
        }
        return IdempotentLookup{&record, false};
    }
    return {};
}

} // namespace

struct CohortJournal::State final {
    AuthoritativeState authoritative{};
    Lifecycle lifecycle{};
};

bool is_canonical_recovery_cohort(
    const RecoveryCohort& cohort,
    std::uint32_t max_members,
    EmptyCohortConfirmation empty_confirmation) noexcept {
    if (cohort.size() > max_members
        || !valid_empty_confirmation(cohort, empty_confirmation)) {
        return false;
    }
    for (std::size_t index = 0U; index < cohort.size(); ++index) {
        if (!static_cast<bool>(cohort[index].node_id)
            || !static_cast<bool>(cohort[index].incarnation)) {
            return false;
        }
        if (index != 0U) {
            const auto& previous = cohort[index - 1U];
            const auto& current = cohort[index];
            if (!member_less(previous, current)
                || previous.node_id == current.node_id) {
                return false;
            }
        }
    }
    return true;
}

std::uint64_t recovery_plan_fingerprint(
    std::uint64_t generation,
    RecoveryPlanToken token,
    const RecoveryCohort& cohort,
    EmptyCohortConfirmation empty_confirmation) noexcept {
    constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    auto hash = offset_basis;
    const auto mix_u8 = [&hash](std::uint8_t value) {
        hash ^= value;
        hash *= prime;
    };
    const auto mix_u64 = [&mix_u8](std::uint64_t value) {
        for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
            mix_u8(static_cast<std::uint8_t>((value >> shift) & 0xffU));
        }
    };
    constexpr std::array<std::uint8_t, 12U> domain{
        'M', 'S', 'C', 'O', 'H', 'O', 'R', 'T', 'P', 'L', 'A', 'N'};
    for (const auto value : domain) {
        mix_u8(value);
    }
    mix_u64(generation);
    mix_u64(token.high);
    mix_u64(token.low);
    mix_u8(static_cast<std::uint8_t>(empty_confirmation));
    mix_u64(static_cast<std::uint64_t>(cohort.size()));
    for (const auto& member : cohort) {
        mix_u64(member.node_id.high);
        mix_u64(member.node_id.low);
        mix_u64(member.incarnation.high);
        mix_u64(member.incarnation.low);
    }
    return hash;
}

CohortJournalReceipt::CohortJournalReceipt(
    CohortJournalReceipt&& other) noexcept
    : ordinal_(other.ordinal_),
      offset_(other.offset_),
      bytes_written_(other.bytes_written_),
      record_crc32c_(other.record_crc32c_),
      replayed_(other.replayed_),
      valid_(std::exchange(other.valid_, false)) {}

CohortJournalReceipt& CohortJournalReceipt::operator=(
    CohortJournalReceipt&& other) noexcept {
    if (this != &other) {
        ordinal_ = other.ordinal_;
        offset_ = other.offset_;
        bytes_written_ = other.bytes_written_;
        record_crc32c_ = other.record_crc32c_;
        replayed_ = other.replayed_;
        valid_ = std::exchange(other.valid_, false);
    }
    return *this;
}

CohortJournal::CohortJournal(
    std::filesystem::path path,
    CohortJournalOpenOptions options,
    CohortJournalBinding binding,
    CohortJournalRecoveryReport recovery,
    std::vector<CohortJournalRecord> records,
    std::unique_ptr<State> state) noexcept
    : path_(std::move(path)),
      options_(options),
      binding_(binding),
      recovery_(recovery),
      records_(std::move(records)),
      state_(std::move(state)) {}

CohortJournal::CohortJournal(CohortJournal&& other) noexcept
    : path_(std::move(other.path_)),
      options_(other.options_),
      binding_(other.binding_),
      recovery_(other.recovery_),
      records_(std::move(other.records_)),
      state_(std::move(other.state_)),
      usable_(std::exchange(other.usable_, false)) {}

CohortJournal& CohortJournal::operator=(CohortJournal&& other) noexcept {
    if (this != &other) {
        path_ = std::move(other.path_);
        options_ = other.options_;
        binding_ = other.binding_;
        recovery_ = other.recovery_;
        records_ = std::move(other.records_);
        state_ = std::move(other.state_);
        usable_ = std::exchange(other.usable_, false);
    }
    return *this;
}

CohortJournal::~CohortJournal() = default;

core::Result<CohortJournal, CohortJournalError>
CohortJournal::open_or_create(
    std::filesystem::path path,
    CohortJournalUuid journal_uuid,
    RecoveryShowId show_id,
    const UnresolvedReleaseInventory& inventory,
    CohortJournalOpenOptions options) {
    using Result = core::Result<CohortJournal, CohortJournalError>;
    if (options.sync_policy == SyncPolicy::None) {
        return Result::failure(make_error(
            CohortJournalErrorCode::NonDurablePolicy,
            0U,
            "cohort journal requires Data or Full sync policy"));
    }
    if (!static_cast<bool>(journal_uuid) || !static_cast<bool>(show_id)
        || path.empty() || !valid_limits(options.limits)
        || !inventory_consistent(inventory)) {
        return Result::failure(make_error(
            CohortJournalErrorCode::InvalidArgument,
            0U,
            "cohort-journal identity, limits, path, or inventory is invalid"));
    }

    std::error_code existence_error;
    const auto existed = std::filesystem::exists(path, existence_error);
    if (existence_error) {
        return Result::failure(make_error(
            CohortJournalErrorCode::OpenFailed,
            0U,
            "could not inspect cohort-journal path",
            existence_error));
    }
    std::uint64_t file_size = 0U;
    if (existed) {
        auto checked_size = checked_file_size(path, options.limits);
        if (!checked_size) {
            return Result::failure(std::move(checked_size).error());
        }
        file_size = checked_size.value();
    }

    const auto creating = !existed || file_size == 0U;
    CohortJournalBinding binding;
    if (creating) {
        if (inventory.wal_tail != inventory.released_frontier
            || inventory.unresolved_frame_count != 0U
            || inventory.unresolved_wal_bytes != 0U
            || inventory.next_unresolved
            || inventory.wal_frame_count
                != inventory.released_frame_count
            || inventory.wal_valid_bytes
                != inventory.released_wal_bytes) {
            return Result::failure(make_error(
                CohortJournalErrorCode::UncoveredWalSuffix,
                0U,
                "automatic journal creation refuses a pre-existing uncovered WAL suffix"));
        }
        binding = CohortJournalBinding{
            journal_uuid,
            show_id,
            inventory.base_cursor,
            inventory.released_frontier,
            inventory.released_wal_bytes,
            inventory.released_frame_count,
            inventory.release_ledger_valid_bytes,
        };
        auto initialized = initialize_file(path, binding, options.sync_policy);
        if (!initialized) {
            return Result::failure(std::move(initialized).error());
        }
        file_size = kHeaderSize;
    } else {
        if (file_size < kHeaderSize) {
            return Result::failure(make_error(
                CohortJournalErrorCode::InvalidHeader,
                0U,
                "cohort-journal header is truncated"));
        }
        std::array<std::byte, kHeaderSize> header_bytes{};
        auto read = read_exact(path, 0U, header_bytes);
        if (!read) {
            return Result::failure(std::move(read).error());
        }
        auto decoded = decode_header(
            std::span<const std::byte, kHeaderSize>{header_bytes});
        if (!decoded) {
            return Result::failure(std::move(decoded).error());
        }
        binding = decoded.value();
    }

    auto scanned = scan_journal(path, binding, options, file_size);
    if (!scanned) {
        return Result::failure(std::move(scanned).error());
    }
    auto compatible = validate_existing_binding(
        binding,
        journal_uuid,
        show_id,
        inventory,
        scanned.value().lifecycle);
    if (!compatible) {
        return Result::failure(std::move(compatible).error());
    }
    auto& live_plans = scanned.value().lifecycle.active;
    live_plans.erase(
        std::remove_if(
            live_plans.begin(),
            live_plans.end(),
            [&inventory](const ActivePlan& plan) {
                return cursor_not_after(
                    plan.frame.after, inventory.released_frontier);
            }),
        live_plans.end());
    scanned.value().lifecycle.release_frontier =
        inventory.released_frontier;
    auto synced = sync_file(path, options.sync_policy);
    if (!synced) {
        return Result::failure(std::move(synced).error());
    }
    auto directory_synced = sync_parent_directory(path);
    if (!directory_synced) {
        return Result::failure(std::move(directory_synced).error());
    }

    auto state = std::make_unique<State>();
    state->authoritative = AuthoritativeState{
        inventory.released_frontier,
        inventory.wal_tail,
        inventory.wal_valid_bytes,
    };
    state->lifecycle = std::move(scanned.value().lifecycle);
    return Result::success(CohortJournal{
        std::move(path),
        options,
        binding,
        scanned.value().recovery,
        std::move(scanned.value().records),
        std::move(state),
    });
}

std::uint64_t CohortJournal::next_ordinal() const noexcept {
    if (!usable_ || recovery_.last_ordinal
            == std::numeric_limits<std::uint64_t>::max()) {
        return 0U;
    }
    return recovery_.last_ordinal + 1U;
}

core::Result<CohortJournalReceipt, CohortJournalError>
CohortJournal::append(
    std::uint64_t expected_next_ordinal,
    const CohortJournalMutation& mutation) {
    using Result = core::Result<CohortJournalReceipt, CohortJournalError>;
    if (!usable_ || state_ == nullptr) {
        return Result::failure(make_error(
            CohortJournalErrorCode::Unusable,
            recovery_.valid_bytes,
            "cohort journal is unusable and must be reopened"));
    }
    auto physical_size = checked_file_size(path_, options_.limits);
    if (!physical_size) {
        usable_ = false;
        return Result::failure(std::move(physical_size).error());
    }
    if (physical_size.value() != recovery_.valid_bytes) {
        usable_ = false;
        return Result::failure(make_error(
            CohortJournalErrorCode::ChainMismatch,
            recovery_.valid_bytes,
            "physical cohort-journal size changed after open"));
    }

    // Operator retries are resolved before the global ordinal CAS. A caller
    // that lost the first response can therefore prove the original durable
    // result even after later records advanced the journal.
    const auto idempotent = find_idempotent_request(records_, mutation);
    if (idempotent.record != nullptr) {
        if (idempotent.conflict) {
            return Result::failure(make_error(
                CohortJournalErrorCode::RequestConflict,
                idempotent.record->offset,
                "operator request ID was reused with a different payload"));
        }
        return Result::success(CohortJournalReceipt{
            idempotent.record->ordinal,
            idempotent.record->offset,
            idempotent.record->bytes,
            idempotent.record->record_crc32c,
            true,
        });
    }

    const auto expected = next_ordinal();
    if (expected == 0U || expected_next_ordinal != expected) {
        return Result::failure(make_error(
            CohortJournalErrorCode::StaleWrite,
            recovery_.valid_bytes,
            "expected journal ordinal is stale or exhausted"));
    }
    if (records_.size() >= options_.limits.max_records) {
        return Result::failure(make_error(
            CohortJournalErrorCode::LimitExceeded,
            recovery_.valid_bytes,
            "cohort journal reached its configured record cap"));
    }

    core::Result<EncodedRecord, CohortJournalError> encoded =
        core::Result<EncodedRecord, CohortJournalError>::failure(make_error(
            CohortJournalErrorCode::LimitExceeded,
            recovery_.valid_bytes,
            "cohort-journal append preparation did not complete"));
    std::optional<CohortJournalRecord> pending_record;
    try {
        auto valid = validate_and_apply(
            state_->lifecycle,
            mutation,
            expected,
            recovery_.valid_bytes,
            options_.limits,
            &state_->authoritative,
            false);
        if (!valid) {
            return Result::failure(std::move(valid).error());
        }
        encoded = encode_record(
            expected,
            recovery_.last_record_crc32c,
            mutation,
            options_.limits,
            recovery_.valid_bytes);
        if (!encoded) {
            return Result::failure(std::move(encoded).error());
        }
        records_.reserve(records_.size() + 1U);
        reserve_lifecycle_append(state_->lifecycle, mutation);
        pending_record.emplace(CohortJournalRecord{
            expected,
            recovery_.valid_bytes,
            static_cast<std::uint64_t>(encoded.value().bytes.size()),
            encoded.value().record_crc32c,
            mutation,
        });
    } catch (const std::bad_alloc&) {
        return Result::failure(make_error(
            CohortJournalErrorCode::LimitExceeded,
            recovery_.valid_bytes,
            "memory allocation failed while preparing cohort-journal append"));
    } catch (const std::length_error&) {
        return Result::failure(make_error(
            CohortJournalErrorCode::LimitExceeded,
            recovery_.valid_bytes,
            "cohort-journal append allocation exceeds container limits"));
    }
    const auto record_bytes =
        static_cast<std::uint64_t>(encoded.value().bytes.size());
    if (record_bytes
            > std::numeric_limits<std::uint64_t>::max()
                - recovery_.valid_bytes
        || recovery_.valid_bytes + record_bytes
            > options_.limits.max_file_bytes) {
        return Result::failure(make_error(
            CohortJournalErrorCode::LimitExceeded,
            recovery_.valid_bytes,
            "cohort-journal append exceeds its configured file cap"));
    }
    const auto body_size = encoded.value().bytes.size() - kTrailerSize;
    auto wrote_body = write_bytes(
        path_,
        std::span<const std::byte>{encoded.value().bytes}.first(body_size),
        false);
    if (!wrote_body) {
        usable_ = false;
        return Result::failure(std::move(wrote_body).error());
    }
    auto ordered = sync_file(path_, SyncPolicy::Data);
    if (!ordered) {
        usable_ = false;
        return Result::failure(std::move(ordered).error());
    }
    auto wrote_trailer = write_bytes(
        path_,
        std::span<const std::byte>{encoded.value().bytes}.last(kTrailerSize),
        false);
    if (!wrote_trailer) {
        usable_ = false;
        return Result::failure(std::move(wrote_trailer).error());
    }
    auto committed = sync_file(path_, options_.sync_policy);
    if (!committed) {
        usable_ = false;
        return Result::failure(std::move(committed).error());
    }

    const auto record_crc = encoded.value().record_crc32c;
    const auto record_offset = recovery_.valid_bytes;
    static_assert(std::is_nothrow_move_constructible_v<CohortJournalRecord>);
    static_assert(std::is_nothrow_move_constructible_v<PendingPlan>);
    static_assert(std::is_nothrow_move_constructible_v<ActivePlan>);
    static_assert(std::is_nothrow_move_constructible_v<RecoveryPlanToken>);
    static_assert(std::is_nothrow_move_constructible_v<RecoveryAttemptToken>);
    static_assert(std::is_nothrow_move_constructible_v<OperatorRequest>);
    auto applied = validate_and_apply(
        state_->lifecycle,
        mutation,
        expected,
        record_offset,
        options_.limits,
        &state_->authoritative,
        true);
    if (!applied) {
        // All inputs were prevalidated, the writer is caller-serialized, and
        // every growing container was reserved before the first byte write.
        // Continuing after violating that proof could expose stale state for
        // a record whose commit trailer is already durable.
        std::terminate();
    }
    records_.push_back(std::move(*pending_record));
    recovery_.record_count = expected;
    recovery_.last_ordinal = expected;
    recovery_.last_record_crc32c = record_crc;
    recovery_.valid_bytes += record_bytes;

    if (const auto* bound = std::get_if<BindFrame>(&mutation)) {
        const auto end = bound->frame.offset + bound->frame.bytes;
        if (cursor_not_after(
                state_->authoritative.wal_tail, bound->frame.after)) {
            state_->authoritative.wal_tail = bound->frame.after;
            state_->authoritative.wal_valid_bytes = end;
        }
    } else if (const auto* adopted = std::get_if<AdoptFrame>(&mutation)) {
        const auto end = adopted->frame.offset + adopted->frame.bytes;
        if (cursor_not_after(
                state_->authoritative.wal_tail, adopted->frame.after)) {
            state_->authoritative.wal_tail = adopted->frame.after;
            state_->authoritative.wal_valid_bytes = end;
        }
    }

    return Result::success(CohortJournalReceipt{
        expected,
        record_offset,
        record_bytes,
        record_crc,
        false,
    });
}

} // namespace mediaserver::persist
