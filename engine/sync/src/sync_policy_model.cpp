#include <mediaserver/sync/sync_policy_model.hpp>

#include <mediaserver/sync/sync_schema.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace mediaserver::sync {
namespace {

[[nodiscard]] const doc::PropertyValue* find(
    const doc::PropertyMap& properties, std::string_view name) noexcept {
    const auto found = properties.find(name);
    return found == properties.end() ? nullptr : &found->second;
}

class Reader final {
public:
    explicit Reader(const doc::PropertyMap& properties) noexcept
        : properties_(&properties) {}

    [[nodiscard]] std::int64_t integer(std::string_view name,
                                       std::int64_t fallback) noexcept {
        const doc::PropertyValue* const value = find(*properties_, name);
        if (value == nullptr) {
            return fallback;
        }
        if (const auto* const whole = std::get_if<std::int64_t>(value)) {
            return *whole;
        }
        ok_ = false;
        return fallback;
    }

    [[nodiscard]] std::string text(std::string_view name,
                                   std::string_view fallback) noexcept {
        const doc::PropertyValue* const value = find(*properties_, name);
        if (value == nullptr) {
            return std::string{fallback};
        }
        if (const auto* const text_value = std::get_if<std::string>(value)) {
            return *text_value;
        }
        ok_ = false;
        return std::string{fallback};
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    void fail() noexcept { ok_ = false; }

private:
    const doc::PropertyMap* properties_{};
    bool ok_{true};
};

[[nodiscard]] bool as_u32(std::int64_t value, core::u32& output) noexcept {
    if (value < 0 ||
        value > static_cast<std::int64_t>(
                    std::numeric_limits<core::u32>::max())) {
        return false;
    }
    output = static_cast<core::u32>(value);
    return true;
}

[[nodiscard]] std::optional<SyncMode> parse_mode(
    std::string_view value) noexcept {
    if (value == schema::unsynced_mode) {
        return SyncMode::Unsynced;
    }
    if (value == schema::framelock_mode) {
        return SyncMode::Framelock;
    }
    if (value == schema::external_genlock_mode) {
        return SyncMode::ExternalGenlock;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<SyncFailurePolicy> parse_failure(
    std::string_view value) noexcept {
    if (value == schema::strict_failure) {
        return SyncFailurePolicy::Strict;
    }
    if (value == schema::degraded_failure) {
        return SyncFailurePolicy::Degraded;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<LateFramePolicy> parse_late_frame(
    std::string_view value) noexcept {
    if (value == schema::hold_group_late_frame) {
        return LateFramePolicy::HoldGroup;
    }
    if (value == schema::advance_late_frame) {
        return LateFramePolicy::AdvanceIndependently;
    }
    return std::nullopt;
}

[[nodiscard]] std::string_view mode_text(SyncMode mode) noexcept {
    switch (mode) {
    case SyncMode::Unsynced:
        return schema::unsynced_mode;
    case SyncMode::Framelock:
        return schema::framelock_mode;
    case SyncMode::ExternalGenlock:
        return schema::external_genlock_mode;
    }
    return schema::unsynced_mode;
}

[[nodiscard]] std::string_view failure_text(SyncFailurePolicy policy) noexcept {
    return policy == SyncFailurePolicy::Degraded ? schema::degraded_failure
                                                 : schema::strict_failure;
}

[[nodiscard]] std::string_view late_frame_text(LateFramePolicy policy)
    noexcept {
    return policy == LateFramePolicy::AdvanceIndependently
        ? schema::advance_late_frame
        : schema::hold_group_late_frame;
}

} // namespace

void SyncPolicyModel::forget(doc::NodeId id) noexcept {
    policies_.erase(id);
}

void SyncPolicyModel::put(const doc::Node& node) {
    forget(node.id);
    if (node.type != schema::policy_type) {
        return;
    }

    Reader reader{node.properties};
    const auto mode = parse_mode(reader.text(schema::mode,
                                             schema::unsynced_mode));
    const auto failure = parse_failure(
        reader.text(schema::failure_policy, schema::strict_failure));
    const auto late_frame = parse_late_frame(
        reader.text(schema::late_frame_policy, schema::hold_group_late_frame));
    SyncPolicy policy{};
    policy.node = node.id;
    policy.authored = true;
    if (!as_u32(reader.integer(schema::width, 1920), policy.required_timing.width) ||
        !as_u32(reader.integer(schema::height, 1080),
                policy.required_timing.height) ||
        !as_u32(reader.integer(schema::refresh_numerator, 60),
                policy.required_timing.refresh_numerator) ||
        !as_u32(reader.integer(schema::refresh_denominator, 1),
                policy.required_timing.refresh_denominator) ||
        !as_u32(reader.integer(schema::group, 1), policy.group) ||
        !as_u32(reader.integer(schema::barrier, 1), policy.barrier)) {
        reader.fail();
    }
    if (!mode.has_value() || !failure.has_value() || !late_frame.has_value()) {
        reader.fail();
    } else {
        policy.mode = *mode;
        policy.failure = *failure;
        policy.late_frame = *late_frame;
    }
    if (!reader.ok() || !valid_policy(policy)) {
        ++counters_.rejected;
        return;
    }
    policies_.insert_or_assign(node.id, std::move(policy));
}

void SyncPolicyModel::recount() noexcept {
    counters_.policies = static_cast<core::u32>(policies_.size());
}

void SyncPolicyModel::reset(const doc::Root& root) {
    policies_.clear();
    counters_ = SyncPolicyCounters{};
    apply(diff(doc::Root{}, root));
}

void SyncPolicyModel::apply(const doc::Delta& delta) {
    for (const doc::NodeChange& change : delta.changes) {
        switch (change.kind) {
        case doc::ChangeKind::Removed:
            forget(change.id);
            break;
        case doc::ChangeKind::Added:
        case doc::ChangeKind::Modified:
            if (change.after) {
                put(*change.after);
            }
            break;
        }
    }
    recount();
}

SyncPolicy SyncPolicyModel::evaluate() const {
    if (policies_.empty()) {
        return SyncPolicy{};
    }
    return policies_.begin()->second;
}

void write_sync_policy_properties(doc::Node& node, const SyncPolicy& policy) {
    node.type = std::string{schema::policy_type};
    node.properties[std::string{schema::mode}] =
        std::string{mode_text(policy.mode)};
    node.properties[std::string{schema::width}] =
        static_cast<std::int64_t>(policy.required_timing.width);
    node.properties[std::string{schema::height}] =
        static_cast<std::int64_t>(policy.required_timing.height);
    node.properties[std::string{schema::refresh_numerator}] =
        static_cast<std::int64_t>(policy.required_timing.refresh_numerator);
    node.properties[std::string{schema::refresh_denominator}] =
        static_cast<std::int64_t>(policy.required_timing.refresh_denominator);
    node.properties[std::string{schema::group}] =
        static_cast<std::int64_t>(policy.group);
    node.properties[std::string{schema::barrier}] =
        static_cast<std::int64_t>(policy.barrier);
    node.properties[std::string{schema::failure_policy}] =
        std::string{failure_text(policy.failure)};
    node.properties[std::string{schema::late_frame_policy}] =
        std::string{late_frame_text(policy.late_frame)};
}

} // namespace mediaserver::sync
