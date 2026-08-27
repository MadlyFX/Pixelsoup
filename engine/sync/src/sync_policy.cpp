#include <mediaserver/sync/sync_policy.hpp>

namespace mediaserver::sync {
namespace {

constexpr core::u64 fnv_offset = 1469598103934665603ULL;
constexpr core::u64 fnv_prime = 1099511628211ULL;

void hash_byte(core::u64& hash, core::u8 byte) noexcept {
    hash ^= byte;
    hash *= fnv_prime;
}

void hash_u64(core::u64& hash, core::u64 value) noexcept {
    for (core::u32 shift = 0U; shift < 64U; shift += 8U) {
        hash_byte(hash, static_cast<core::u8>((value >> shift) & 0xFFU));
    }
}

void hash_u32(core::u64& hash, core::u32 value) noexcept {
    hash_u64(hash, static_cast<core::u64>(value));
}

} // namespace

bool same_raster(const SyncTiming& left, const SyncTiming& right) noexcept {
    return left.width == right.width && left.height == right.height;
}

bool same_refresh(const SyncTiming& left, const SyncTiming& right) noexcept {
    if (left.refresh_numerator == 0U || left.refresh_denominator == 0U ||
        right.refresh_numerator == 0U || right.refresh_denominator == 0U) {
        return false;
    }
    // Cross-multiplied in 64 bits. Both factors are bounded by the authoring
    // limits, so this cannot overflow.
    const core::u64 left_scaled =
        static_cast<core::u64>(left.refresh_numerator) *
        static_cast<core::u64>(right.refresh_denominator);
    const core::u64 right_scaled =
        static_cast<core::u64>(right.refresh_numerator) *
        static_cast<core::u64>(left.refresh_denominator);
    return left_scaled == right_scaled;
}

bool same_timing(const SyncTiming& left, const SyncTiming& right) noexcept {
    return same_raster(left, right) && same_refresh(left, right);
}

std::string_view sync_mode_name(SyncMode mode) noexcept {
    switch (mode) {
    case SyncMode::Unsynced:
        return "UNSYNCED";
    case SyncMode::Framelock:
        return "FRAMELOCK";
    case SyncMode::ExternalGenlock:
        return "EXTERNAL GENLOCK";
    }
    return "UNSYNCED";
}

std::string_view sync_failure_policy_name(SyncFailurePolicy policy) noexcept {
    switch (policy) {
    case SyncFailurePolicy::Degraded:
        return "DEGRADED";
    case SyncFailurePolicy::Strict:
        return "STRICT";
    }
    return "STRICT";
}

std::string_view late_frame_policy_name(LateFramePolicy policy) noexcept {
    switch (policy) {
    case LateFramePolicy::HoldGroup:
        return "HOLD GROUP";
    case LateFramePolicy::AdvanceIndependently:
        return "ADVANCE INDEPENDENTLY";
    }
    return "HOLD GROUP";
}

bool valid_policy(const SyncPolicy& policy) noexcept {
    return policy.required_timing.valid() &&
        policy.required_timing.width <= max_sync_timing_width &&
        policy.required_timing.height <= max_sync_timing_height &&
        policy.required_timing.refresh_numerator <=
            max_sync_refresh_numerator &&
        policy.required_timing.refresh_denominator <=
            max_sync_refresh_denominator &&
        policy.group != 0U && policy.group <= max_sync_group_id &&
        policy.barrier != 0U && policy.barrier <= max_sync_barrier_id;
}

core::u64 digest(const SyncPolicy& policy) noexcept {
    core::u64 hash = fnv_offset;
    hash_byte(hash, policy.authored ? 1U : 0U);
    hash_u64(hash, policy.node.value());
    hash_byte(hash, static_cast<core::u8>(policy.mode));
    hash_u32(hash, policy.required_timing.width);
    hash_u32(hash, policy.required_timing.height);
    hash_u32(hash, policy.required_timing.refresh_numerator);
    hash_u32(hash, policy.required_timing.refresh_denominator);
    hash_u32(hash, policy.group);
    hash_u32(hash, policy.barrier);
    hash_byte(hash, static_cast<core::u8>(policy.failure));
    hash_byte(hash, static_cast<core::u8>(policy.late_frame));
    return hash;
}

} // namespace mediaserver::sync
