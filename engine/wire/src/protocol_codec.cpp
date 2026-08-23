#include <mediaserver/wire/protocol_codec.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <flatbuffers/flatbuffers.h>

#include "protocol_generated.h"

namespace mediaserver::wire {
namespace {

namespace fb = mediaserver::wire::fb::v1;

struct EncodeBudget final {
    const DecodeLimits& limits;
    std::size_t total_properties{};
    std::size_t estimated_bytes{};

    [[nodiscard]] bool reserve_bytes(std::size_t bytes) noexcept {
        if (estimated_bytes > limits.max_message_bytes
            || bytes > limits.max_message_bytes - estimated_bytes) {
            return false;
        }
        estimated_bytes += bytes;
        return true;
    }
};

struct DecodeBudget final {
    const DecodeLimits& limits;
    std::size_t total_properties{};
};

struct EncodedPayload final {
    fb::Payload type{fb::Payload::NONE};
    flatbuffers::Offset<void> value;
};

[[nodiscard]] bool valid_utf8(std::string_view value) noexcept {
    std::size_t index = 0U;
    while (index < value.size()) {
        const auto first = static_cast<std::uint8_t>(
            static_cast<unsigned char>(value[index]));
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        std::size_t continuation_count{};
        std::uint32_t code_point{};
        std::uint32_t minimum{};
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1U;
            code_point = static_cast<std::uint32_t>(first & 0x1fU);
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2U;
            code_point = static_cast<std::uint32_t>(first & 0x0fU);
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3U;
            code_point = static_cast<std::uint32_t>(first & 0x07U);
            minimum = 0x10000U;
        } else {
            return false;
        }

        if (continuation_count > value.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= continuation_count; ++offset) {
            const auto continuation = static_cast<std::uint8_t>(
                static_cast<unsigned char>(value[index + offset]));
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U)
                | static_cast<std::uint32_t>(continuation & 0x3fU);
        }
        if (code_point < minimum || code_point > 0x10ffffU
            || (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

[[nodiscard]] std::optional<EncodeError> validate_node(
    const doc::Node& node,
    EncodeBudget& budget) noexcept {
    if (node.type.size() > budget.limits.max_string_bytes) {
        return EncodeError::SizeLimitExceeded;
    }
    if (!valid_utf8(node.type)) {
        return EncodeError::InvalidNode;
    }
    if (node.properties.size() > budget.limits.max_properties_per_node
        || budget.total_properties > budget.limits.max_total_properties
        || node.properties.size()
            > budget.limits.max_total_properties - budget.total_properties) {
        return EncodeError::SizeLimitExceeded;
    }
    // This intentionally overestimates FlatBuffers table, vector, vtable, and
    // alignment overhead. It bounds builder growth before any large strings or
    // vectors are allocated; the exact finished size is still checked later.
    if (!budget.reserve_bytes(128U) || !budget.reserve_bytes(node.type.size())) {
        return EncodeError::SizeLimitExceeded;
    }

    for (const auto& [name, value] : node.properties) {
        if (name.size() > budget.limits.max_string_bytes) {
            return EncodeError::SizeLimitExceeded;
        }
        if (!valid_utf8(name)) {
            return EncodeError::InvalidProperty;
        }
        if (!budget.reserve_bytes(128U) || !budget.reserve_bytes(name.size())) {
            return EncodeError::SizeLimitExceeded;
        }
        if (const auto* text = std::get_if<std::string>(&value)) {
            if (text->size() > budget.limits.max_string_bytes) {
                return EncodeError::SizeLimitExceeded;
            }
            if (!valid_utf8(*text)) {
                return EncodeError::InvalidProperty;
            }
            if (!budget.reserve_bytes(text->size())) {
                return EncodeError::SizeLimitExceeded;
            }
        } else if (const auto* number = std::get_if<double>(&value)) {
            if (!std::isfinite(*number)) {
                return EncodeError::InvalidProperty;
            }
        }
    }
    budget.total_properties += node.properties.size();
    return std::nullopt;
}

[[nodiscard]] core::Result<flatbuffers::Offset<fb::Node>, EncodeError> encode_node(
    flatbuffers::FlatBufferBuilder& builder,
    const doc::Node& node,
    EncodeBudget& budget) {
    if (const auto error = validate_node(node, budget)) {
        return core::Result<flatbuffers::Offset<fb::Node>, EncodeError>::failure(*error);
    }

    std::vector<flatbuffers::Offset<fb::Property>> properties;
    properties.reserve(node.properties.size());
    for (const auto& [name, value] : node.properties) {
        const auto name_offset = builder.CreateString(name.data(), name.size());
        fb::PropertyPayload type = fb::PropertyPayload::NONE;
        flatbuffers::Offset<void> encoded_value;
        if (const auto* boolean = std::get_if<bool>(&value)) {
            type = fb::PropertyPayload::BoolProperty;
            encoded_value = fb::CreateBoolProperty(builder, *boolean).Union();
        } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
            type = fb::PropertyPayload::Int64Property;
            encoded_value = fb::CreateInt64Property(builder, *integer).Union();
        } else if (const auto* number = std::get_if<double>(&value)) {
            type = fb::PropertyPayload::Float64Property;
            encoded_value = fb::CreateFloat64Property(builder, *number).Union();
        } else if (const auto* text = std::get_if<std::string>(&value)) {
            type = fb::PropertyPayload::StringProperty;
            const auto text_offset = builder.CreateString(text->data(), text->size());
            encoded_value = fb::CreateStringProperty(builder, text_offset).Union();
        } else {
            return core::Result<flatbuffers::Offset<fb::Node>, EncodeError>::failure(
                EncodeError::InvalidProperty);
        }
        properties.push_back(fb::CreateProperty(builder, name_offset, type, encoded_value));
    }

    const auto type_offset = builder.CreateString(node.type.data(), node.type.size());
    const auto properties_offset = builder.CreateVector(properties);
    return core::Result<flatbuffers::Offset<fb::Node>, EncodeError>::success(
        fb::CreateNode(builder, node.id.value(), type_offset, properties_offset));
}

[[nodiscard]] core::Result<EncodedPayload, EncodeError> encode_payload(
    flatbuffers::FlatBufferBuilder& builder,
    const cmd::Payload& payload,
    EncodeBudget& budget) {
    if (const auto* put = std::get_if<cmd::PutNode>(&payload)) {
        auto node = encode_node(builder, put->node, budget);
        if (!node) {
            return core::Result<EncodedPayload, EncodeError>::failure(node.error());
        }
        return core::Result<EncodedPayload, EncodeError>::success(EncodedPayload{
            fb::Payload::PutNode,
            fb::CreatePutNode(builder, node.value()).Union(),
        });
    }
    if (const auto* erase = std::get_if<cmd::EraseNode>(&payload)) {
        return core::Result<EncodedPayload, EncodeError>::success(EncodedPayload{
            fb::Payload::EraseNode,
            fb::CreateEraseNode(builder, erase->id.value()).Union(),
        });
    }
    return core::Result<EncodedPayload, EncodeError>::failure(
        EncodeError::InvalidCommand);
}

[[nodiscard]] bool valid_command_header(const cmd::Command& command) noexcept {
    return static_cast<bool>(command.id)
        && static_cast<bool>(command.apply_at)
        && command.base.value() != std::numeric_limits<core::u64>::max()
        && command.result.value() == command.base.value() + 1U;
}

[[nodiscard]] bool valid_seal(const cmd::FrameSeal& seal, const DecodeLimits& limits) noexcept {
    if (!seal.frame || seal.commands.size() > limits.max_sealed_commands
        || seal.commands.size()
            > static_cast<std::size_t>(
                std::numeric_limits<core::u64>::max() - seal.base.value())
        || seal.result.value()
            != seal.base.value() + static_cast<core::u64>(seal.commands.size())) {
        return false;
    }

    core::u64 previous{};
    for (std::size_t index = 0U; index < seal.commands.size(); ++index) {
        const auto id = seal.commands[index].id.value();
        if (id == 0U
            || (index != 0U
                && (previous == std::numeric_limits<core::u64>::max()
                    || id != previous + 1U))) {
            return false;
        }
        previous = id;
    }
    return true;
}

[[nodiscard]] bool valid_cursor(const cmd::StreamCursor& cursor) noexcept {
    return static_cast<bool>(cursor.next_command);
}

[[nodiscard]] core::Result<doc::Node, DecodeError> decode_node(
    const fb::Node& source,
    DecodeBudget& budget) {
    const auto* node_type = source.node_type();
    const auto* properties = source.properties();
    if (node_type == nullptr || properties == nullptr) {
        return core::Result<doc::Node, DecodeError>::failure(
            DecodeError::MissingRequiredField);
    }
    const std::string_view type_view(node_type->c_str(), node_type->size());
    if (type_view.size() > budget.limits.max_string_bytes) {
        return core::Result<doc::Node, DecodeError>::failure(
            DecodeError::SizeLimitExceeded);
    }
    if (!valid_utf8(type_view)) {
        return core::Result<doc::Node, DecodeError>::failure(DecodeError::InvalidUtf8);
    }
    const auto property_count = static_cast<std::size_t>(properties->size());
    if (property_count > budget.limits.max_properties_per_node
        || budget.total_properties > budget.limits.max_total_properties
        || property_count
            > budget.limits.max_total_properties - budget.total_properties) {
        return core::Result<doc::Node, DecodeError>::failure(
            DecodeError::SizeLimitExceeded);
    }

    doc::PropertyMap decoded_properties;
    std::string previous_name;
    bool have_previous = false;
    for (const auto* property : *properties) {
        if (property == nullptr || property->name() == nullptr || property->value() == nullptr) {
            return core::Result<doc::Node, DecodeError>::failure(
                DecodeError::MissingRequiredField);
        }
        const std::string_view name_view(
            property->name()->c_str(), property->name()->size());
        if (name_view.size() > budget.limits.max_string_bytes) {
            return core::Result<doc::Node, DecodeError>::failure(
                DecodeError::SizeLimitExceeded);
        }
        if (!valid_utf8(name_view)) {
            return core::Result<doc::Node, DecodeError>::failure(
                DecodeError::InvalidUtf8);
        }
        std::string name(name_view);
        if (have_previous && name <= previous_name) {
            return core::Result<doc::Node, DecodeError>::failure(
                name == previous_name ? DecodeError::DuplicateProperty
                                      : DecodeError::InvalidNode);
        }

        doc::PropertyValue value;
        switch (property->value_type()) {
        case fb::PropertyPayload::BoolProperty: {
            const auto* encoded = property->value_as_BoolProperty();
            if (encoded == nullptr) {
                return core::Result<doc::Node, DecodeError>::failure(
                    DecodeError::InvalidUnion);
            }
            value = encoded->value();
            break;
        }
        case fb::PropertyPayload::Int64Property: {
            const auto* encoded = property->value_as_Int64Property();
            if (encoded == nullptr) {
                return core::Result<doc::Node, DecodeError>::failure(
                    DecodeError::InvalidUnion);
            }
            value = encoded->value();
            break;
        }
        case fb::PropertyPayload::Float64Property: {
            const auto* encoded = property->value_as_Float64Property();
            if (encoded == nullptr || !std::isfinite(encoded->value())) {
                return core::Result<doc::Node, DecodeError>::failure(
                    DecodeError::InvalidProperty);
            }
            value = encoded->value();
            break;
        }
        case fb::PropertyPayload::StringProperty: {
            const auto* encoded = property->value_as_StringProperty();
            if (encoded == nullptr || encoded->value() == nullptr) {
                return core::Result<doc::Node, DecodeError>::failure(
                    DecodeError::InvalidUnion);
            }
            const std::string_view text(
                encoded->value()->c_str(), encoded->value()->size());
            if (text.size() > budget.limits.max_string_bytes) {
                return core::Result<doc::Node, DecodeError>::failure(
                    DecodeError::SizeLimitExceeded);
            }
            if (!valid_utf8(text)) {
                return core::Result<doc::Node, DecodeError>::failure(
                    DecodeError::InvalidUtf8);
            }
            value = std::string(text);
            break;
        }
        case fb::PropertyPayload::NONE:
        default:
            return core::Result<doc::Node, DecodeError>::failure(
                DecodeError::InvalidUnion);
        }

        const auto [unused, inserted] = decoded_properties.emplace(name, std::move(value));
        static_cast<void>(unused);
        if (!inserted) {
            return core::Result<doc::Node, DecodeError>::failure(
                DecodeError::DuplicateProperty);
        }
        previous_name = std::move(name);
        have_previous = true;
    }
    budget.total_properties += property_count;
    return core::Result<doc::Node, DecodeError>::success(doc::Node{
        doc::NodeId{source.id()},
        std::string(type_view),
        std::move(decoded_properties),
    });
}

[[nodiscard]] core::Result<cmd::Payload, DecodeError> decode_payload(
    fb::Payload type,
    const void* value,
    DecodeBudget& budget) {
    if (value == nullptr) {
        return core::Result<cmd::Payload, DecodeError>::failure(
            DecodeError::MissingRequiredField);
    }
    switch (type) {
    case fb::Payload::PutNode: {
        const auto* put = static_cast<const fb::PutNode*>(value);
        if (put->node() == nullptr) {
            return core::Result<cmd::Payload, DecodeError>::failure(
                DecodeError::MissingRequiredField);
        }
        auto node = decode_node(*put->node(), budget);
        if (!node) {
            return core::Result<cmd::Payload, DecodeError>::failure(node.error());
        }
        return core::Result<cmd::Payload, DecodeError>::success(
            cmd::PutNode{std::move(node).value()});
    }
    case fb::Payload::EraseNode: {
        const auto* erase = static_cast<const fb::EraseNode*>(value);
        return core::Result<cmd::Payload, DecodeError>::success(
            cmd::EraseNode{doc::NodeId{erase->node_id()}});
    }
    case fb::Payload::NONE:
    default:
        return core::Result<cmd::Payload, DecodeError>::failure(DecodeError::InvalidUnion);
    }
}

[[nodiscard]] core::Result<cmd::Command, DecodeError> decode_command(
    const fb::Command& source,
    DecodeBudget& budget) {
    const cmd::Command header{
        cmd::CommandId{source.id()},
        doc::Version{source.base_revision()},
        doc::Version{source.result_revision()},
        cmd::FrameNumber{source.apply_at_frame()},
        cmd::TransactionId{source.transaction_id()},
        cmd::UndoContextId{source.undo_context_id()},
        cmd::EraseNode{doc::NodeId{}},
        cmd::EraseNode{doc::NodeId{}},
    };
    if (!valid_command_header(header)) {
        return core::Result<cmd::Command, DecodeError>::failure(
            DecodeError::InvalidCommand);
    }

    auto forward = decode_payload(source.forward_type(), source.forward(), budget);
    if (!forward) {
        return core::Result<cmd::Command, DecodeError>::failure(forward.error());
    }
    auto inverse = decode_payload(source.inverse_type(), source.inverse(), budget);
    if (!inverse) {
        return core::Result<cmd::Command, DecodeError>::failure(inverse.error());
    }
    return core::Result<cmd::Command, DecodeError>::success(cmd::Command{
        header.id,
        header.base,
        header.result,
        header.apply_at,
        header.transaction,
        header.undo_context,
        std::move(forward).value(),
        std::move(inverse).value(),
    });
}

[[nodiscard]] core::Result<cmd::FrameSeal, DecodeError> decode_seal(
    const fb::FrameSeal& source,
    const DecodeLimits& limits) {
    const auto* commands = source.commands();
    if (commands == nullptr) {
        return core::Result<cmd::FrameSeal, DecodeError>::failure(
            DecodeError::MissingRequiredField);
    }
    if (static_cast<std::size_t>(commands->size()) > limits.max_sealed_commands) {
        return core::Result<cmd::FrameSeal, DecodeError>::failure(
            DecodeError::SizeLimitExceeded);
    }

    cmd::FrameSeal seal{
        cmd::FrameNumber{source.frame()},
        doc::Version{source.base_revision()},
        doc::Version{source.result_revision()},
        {},
    };
    seal.commands.reserve(commands->size());
    for (const auto* command : *commands) {
        if (command == nullptr) {
            return core::Result<cmd::FrameSeal, DecodeError>::failure(
                DecodeError::MissingRequiredField);
        }
        seal.commands.push_back(cmd::SealedCommand{
            cmd::CommandId{command->id()},
            cmd::CommandFingerprint{command->fingerprint()},
        });
    }
    if (!valid_seal(seal, limits)) {
        return core::Result<cmd::FrameSeal, DecodeError>::failure(DecodeError::InvalidSeal);
    }
    return core::Result<cmd::FrameSeal, DecodeError>::success(std::move(seal));
}

[[nodiscard]] core::Result<cmd::StreamCursor, DecodeError> decode_cursor(
    const fb::StreamCursor& source) {
    cmd::StreamCursor cursor{
        cmd::FrameNumber{source.through_frame()},
        doc::Version{source.revision()},
        cmd::CommandId{source.next_command_id()},
    };
    if (!valid_cursor(cursor)) {
        return core::Result<cmd::StreamCursor, DecodeError>::failure(
            DecodeError::InvalidCursor);
    }
    return core::Result<cmd::StreamCursor, DecodeError>::success(cursor);
}

[[nodiscard]] flatbuffers::uoffset_t verifier_limit(std::size_t value) noexcept {
    const auto maximum = static_cast<std::size_t>(
        std::numeric_limits<flatbuffers::uoffset_t>::max());
    return static_cast<flatbuffers::uoffset_t>(std::min(value, maximum));
}

} // namespace

namespace fbv1 = mediaserver::wire::fb::v1;

core::Result<std::vector<std::byte>, EncodeError> encode(
    const Message& message,
    const DecodeLimits& limits) {
    auto effective_limits = limits;
    effective_limits.max_message_bytes = std::min(
        effective_limits.max_message_bytes,
        static_cast<std::size_t>(FLATBUFFERS_MAX_BUFFER_SIZE) - 1U);
    EncodeBudget budget{effective_limits};
    constexpr std::size_t envelope_overhead_estimate = 256U;
    if (!budget.reserve_bytes(envelope_overhead_estimate)) {
        return core::Result<std::vector<std::byte>, EncodeError>::failure(
            EncodeError::SizeLimitExceeded);
    }
    flatbuffers::FlatBufferBuilder builder(
        std::min<std::size_t>(1024U, effective_limits.max_message_bytes));
    fbv1::EnvelopeBody body_type = fbv1::EnvelopeBody::NONE;
    flatbuffers::Offset<void> body;

    if (const auto* command = std::get_if<cmd::Command>(&message)) {
        if (!valid_command_header(*command)) {
            return core::Result<std::vector<std::byte>, EncodeError>::failure(
                EncodeError::InvalidCommand);
        }
        auto forward = encode_payload(builder, command->forward, budget);
        if (!forward) {
            return core::Result<std::vector<std::byte>, EncodeError>::failure(
                forward.error());
        }
        auto inverse = encode_payload(builder, command->inverse, budget);
        if (!inverse) {
            return core::Result<std::vector<std::byte>, EncodeError>::failure(
                inverse.error());
        }
        body_type = fbv1::EnvelopeBody::Command;
        body = fbv1::CreateCommand(
            builder,
            command->id.value(),
            command->base.value(),
            command->result.value(),
            command->apply_at.value(),
            command->transaction.value(),
            command->undo_context.value(),
            forward.value().type,
            forward.value().value,
            inverse.value().type,
            inverse.value().value)
                   .Union();
    } else if (const auto* seal = std::get_if<cmd::FrameSeal>(&message)) {
        if (!valid_seal(*seal, effective_limits)) {
            return core::Result<std::vector<std::byte>, EncodeError>::failure(
                EncodeError::InvalidSeal);
        }
        std::vector<flatbuffers::Offset<fbv1::SealedCommand>> commands;
        for (std::size_t index = 0U; index < seal->commands.size(); ++index) {
            if (!budget.reserve_bytes(64U)) {
                return core::Result<std::vector<std::byte>, EncodeError>::failure(
                    EncodeError::SizeLimitExceeded);
            }
        }
        commands.reserve(seal->commands.size());
        for (const auto& descriptor : seal->commands) {
            commands.push_back(fbv1::CreateSealedCommand(
                builder, descriptor.id.value(), descriptor.fingerprint.value()));
        }
        body_type = fbv1::EnvelopeBody::FrameSeal;
        body = fbv1::CreateFrameSeal(
            builder,
            seal->frame.value(),
            seal->base.value(),
            seal->result.value(),
            builder.CreateVector(commands))
                   .Union();
    } else if (const auto* cursor = std::get_if<cmd::StreamCursor>(&message)) {
        if (!valid_cursor(*cursor)) {
            return core::Result<std::vector<std::byte>, EncodeError>::failure(
                EncodeError::InvalidCursor);
        }
        body_type = fbv1::EnvelopeBody::StreamCursor;
        body = fbv1::CreateStreamCursor(
            builder,
            cursor->through_frame.value(),
            cursor->revision.value(),
            cursor->next_command.value())
                   .Union();
    } else if (const auto* checkpoint = std::get_if<cmd::StreamCheckpoint>(&message)) {
        if (!valid_cursor(checkpoint->cursor)
            || checkpoint->cursor.revision != checkpoint->document.version()) {
            return core::Result<std::vector<std::byte>, EncodeError>::failure(
                EncodeError::InvalidCheckpoint);
        }
        if (checkpoint->document.size() > effective_limits.max_nodes) {
            return core::Result<std::vector<std::byte>, EncodeError>::failure(
                EncodeError::SizeLimitExceeded);
        }
        const auto delta = doc::diff(doc::Root{}, checkpoint->document);

        std::vector<flatbuffers::Offset<fbv1::Node>> nodes;
        nodes.reserve(delta.changes.size());
        for (const auto& change : delta.changes) {
            if (!change.after) {
                return core::Result<std::vector<std::byte>, EncodeError>::failure(
                    EncodeError::InvalidCheckpoint);
            }
            auto node = encode_node(builder, *change.after, budget);
            if (!node) {
                return core::Result<std::vector<std::byte>, EncodeError>::failure(
                    node.error());
            }
            nodes.push_back(node.value());
        }
        const auto encoded_cursor = fbv1::CreateStreamCursor(
            builder,
            checkpoint->cursor.through_frame.value(),
            checkpoint->cursor.revision.value(),
            checkpoint->cursor.next_command.value());
        body_type = fbv1::EnvelopeBody::DocumentSnapshot;
        body = fbv1::CreateDocumentSnapshot(
            builder, encoded_cursor, builder.CreateVector(nodes)).Union();
    } else {
        return core::Result<std::vector<std::byte>, EncodeError>::failure(
            EncodeError::InvalidCheckpoint);
    }

    const auto envelope = fbv1::CreateEnvelope(
        builder, current_format_version, body_type, body);
    fbv1::FinishSizePrefixedEnvelopeBuffer(builder, envelope);
    if (builder.GetSize() > effective_limits.max_message_bytes) {
        return core::Result<std::vector<std::byte>, EncodeError>::failure(
            EncodeError::SizeLimitExceeded);
    }

    std::vector<std::byte> bytes(builder.GetSize());
    std::memcpy(bytes.data(), builder.GetBufferPointer(), builder.GetSize());
    return core::Result<std::vector<std::byte>, EncodeError>::success(std::move(bytes));
}

core::Result<Message, DecodeError> decode(
    std::span<const std::byte> bytes,
    const DecodeLimits& limits) {
    constexpr std::size_t size_prefix_bytes = sizeof(flatbuffers::uoffset_t);
    const auto flatbuffer_limit = static_cast<std::size_t>(FLATBUFFERS_MAX_BUFFER_SIZE);
    if (bytes.size() < size_prefix_bytes || bytes.size() > limits.max_message_bytes
        || bytes.size() >= flatbuffer_limit) {
        return core::Result<Message, DecodeError>::failure(
            bytes.size() > limits.max_message_bytes || bytes.size() >= flatbuffer_limit
                ? DecodeError::SizeLimitExceeded
                : DecodeError::InvalidBuffer);
    }

    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const auto payload_bytes = static_cast<std::size_t>(flatbuffers::GetPrefixedSize(data));
    if (payload_bytes > bytes.size() - size_prefix_bytes
        || payload_bytes + size_prefix_bytes != bytes.size()) {
        return core::Result<Message, DecodeError>::failure(DecodeError::InvalidBuffer);
    }

    flatbuffers::Verifier::Options options;
    options.max_depth = verifier_limit(limits.verifier_max_depth);
    options.max_tables = verifier_limit(limits.verifier_max_tables);
    const auto configured_verifier_size =
        limits.max_message_bytes == std::numeric_limits<std::size_t>::max()
        ? limits.max_message_bytes
        : limits.max_message_bytes + 1U;
    options.max_size = std::min(configured_verifier_size, flatbuffer_limit);
    flatbuffers::Verifier verifier(data, bytes.size(), options);
    if (!fbv1::VerifySizePrefixedEnvelopeBuffer(verifier)) {
        return core::Result<Message, DecodeError>::failure(DecodeError::InvalidBuffer);
    }

    const auto* envelope = fbv1::GetSizePrefixedEnvelope(data);
    if (envelope == nullptr) {
        return core::Result<Message, DecodeError>::failure(DecodeError::InvalidBuffer);
    }
    if (envelope->format_version() != current_format_version) {
        return core::Result<Message, DecodeError>::failure(
            DecodeError::UnsupportedVersion);
    }
    if (envelope->body() == nullptr
        || envelope->body_type() == fbv1::EnvelopeBody::NONE) {
        return core::Result<Message, DecodeError>::failure(DecodeError::MissingBody);
    }

    DecodeBudget budget{limits};
    switch (envelope->body_type()) {
    case fbv1::EnvelopeBody::Command: {
        const auto* source = envelope->body_as_Command();
        if (source == nullptr) {
            return core::Result<Message, DecodeError>::failure(DecodeError::InvalidUnion);
        }
        auto command = decode_command(*source, budget);
        if (!command) {
            return core::Result<Message, DecodeError>::failure(command.error());
        }
        return core::Result<Message, DecodeError>::success(
            Message{std::move(command).value()});
    }
    case fbv1::EnvelopeBody::FrameSeal: {
        const auto* source = envelope->body_as_FrameSeal();
        if (source == nullptr) {
            return core::Result<Message, DecodeError>::failure(DecodeError::InvalidUnion);
        }
        auto seal = decode_seal(*source, limits);
        if (!seal) {
            return core::Result<Message, DecodeError>::failure(seal.error());
        }
        return core::Result<Message, DecodeError>::success(Message{std::move(seal).value()});
    }
    case fbv1::EnvelopeBody::StreamCursor: {
        const auto* source = envelope->body_as_StreamCursor();
        if (source == nullptr) {
            return core::Result<Message, DecodeError>::failure(DecodeError::InvalidUnion);
        }
        auto cursor = decode_cursor(*source);
        if (!cursor) {
            return core::Result<Message, DecodeError>::failure(cursor.error());
        }
        return core::Result<Message, DecodeError>::success(
            Message{std::move(cursor).value()});
    }
    case fbv1::EnvelopeBody::DocumentSnapshot: {
        const auto* source = envelope->body_as_DocumentSnapshot();
        if (source == nullptr || source->cursor() == nullptr || source->nodes() == nullptr) {
            return core::Result<Message, DecodeError>::failure(
                DecodeError::MissingRequiredField);
        }
        auto cursor = decode_cursor(*source->cursor());
        if (!cursor) {
            return core::Result<Message, DecodeError>::failure(
                DecodeError::InvalidCheckpoint);
        }
        const auto node_count = static_cast<std::size_t>(source->nodes()->size());
        if (node_count > limits.max_nodes) {
            return core::Result<Message, DecodeError>::failure(
                DecodeError::SizeLimitExceeded);
        }

        std::vector<doc::Node> nodes;
        nodes.reserve(node_count);
        core::u64 previous_id{};
        bool have_previous = false;
        for (const auto* source_node : *source->nodes()) {
            if (source_node == nullptr) {
                return core::Result<Message, DecodeError>::failure(
                    DecodeError::MissingRequiredField);
            }
            if (have_previous && source_node->id() <= previous_id) {
                return core::Result<Message, DecodeError>::failure(
                    source_node->id() == previous_id ? DecodeError::DuplicateNode
                                                     : DecodeError::InvalidCheckpoint);
            }
            auto node = decode_node(*source_node, budget);
            if (!node) {
                return core::Result<Message, DecodeError>::failure(node.error());
            }
            previous_id = source_node->id();
            have_previous = true;
            nodes.push_back(std::move(node).value());
        }

        auto document = doc::Root::restore(nodes, cursor.value().revision);
        if (!document || document->version() != cursor.value().revision) {
            return core::Result<Message, DecodeError>::failure(
                DecodeError::InvalidCheckpoint);
        }
        return core::Result<Message, DecodeError>::success(Message{cmd::StreamCheckpoint{
            cursor.value(),
            std::move(*document),
        }});
    }
    case fbv1::EnvelopeBody::NONE:
    default:
        return core::Result<Message, DecodeError>::failure(DecodeError::InvalidUnion);
    }
}

} // namespace mediaserver::wire
