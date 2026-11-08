#include <mediaserver/cmd/command.hpp>

#include <mediaserver/core/fnv1a.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace mediaserver::cmd {
namespace {

class FingerprintBuilder final {
public:
    void add_byte(core::u8 value) noexcept { core::fnv1a_byte(value_, value); }
    void add_u64(core::u64 value) noexcept { core::fnv1a_u64(value_, value); }
    void add_string(std::string_view value) noexcept { core::fnv1a_text(value_, value); }

    [[nodiscard]] core::u64 value() const noexcept { return value_; }

private:
    core::u64 value_{core::fnv1a_offset};
};

void add_property(FingerprintBuilder& builder, const doc::PropertyValue& property) noexcept {
    std::visit(
        [&builder](const auto& value) noexcept {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, bool>) {
                builder.add_byte(
                    static_cast<core::u8>(fingerprint_v1::PropertyTag::Boolean));
                builder.add_byte(value ? 1U : 0U);
            } else if constexpr (std::is_same_v<Value, std::int64_t>) {
                builder.add_byte(static_cast<core::u8>(fingerprint_v1::PropertyTag::Int64));
                builder.add_u64(std::bit_cast<std::uint64_t>(value));
            } else if constexpr (std::is_same_v<Value, double>) {
                builder.add_byte(static_cast<core::u8>(fingerprint_v1::PropertyTag::Float64));
                builder.add_u64(std::bit_cast<std::uint64_t>(value));
            } else {
                builder.add_byte(static_cast<core::u8>(fingerprint_v1::PropertyTag::String));
                builder.add_string(value);
            }
        },
        property);
}

void add_payload(FingerprintBuilder& builder, const Payload& payload) noexcept {
    if (const auto* put = std::get_if<PutNode>(&payload)) {
        builder.add_byte(static_cast<core::u8>(fingerprint_v1::PayloadTag::PutNode));
        builder.add_u64(put->node.id.value());
        builder.add_string(put->node.type);
        builder.add_u64(static_cast<core::u64>(put->node.properties.size()));
        for (const auto& [name, value] : put->node.properties) {
            builder.add_string(name);
            add_property(builder, value);
        }
        return;
    }
    builder.add_byte(static_cast<core::u8>(fingerprint_v1::PayloadTag::EraseNode));
    builder.add_u64(std::get<EraseNode>(payload).id.value());
}

} // namespace

CommandFingerprint fingerprint(const Command& command) noexcept {
    FingerprintBuilder builder;
    builder.add_u64(command.id.value());
    builder.add_u64(command.base.value());
    builder.add_u64(command.result.value());
    builder.add_u64(command.apply_at.value());
    builder.add_u64(command.transaction.value());
    builder.add_u64(command.undo_context.value());
    add_payload(builder, command.forward);
    add_payload(builder, command.inverse);
    return CommandFingerprint{builder.value()};
}

} // namespace mediaserver::cmd
