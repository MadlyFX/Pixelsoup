#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <mediaserver/wire/protocol_codec.hpp>

namespace {

namespace command = mediaserver::cmd;
namespace document = mediaserver::doc;
namespace wire = mediaserver::wire;

[[nodiscard]] document::Node fixture_node(std::uint64_t id = 17U) {
    return document::Node{
        document::NodeId{id},
        "surface",
        {
            {"boolean", true},
            {"float64", 0.5},
            {"int64", std::int64_t{-73}},
            {"string", std::string{"camera A"}},
        },
    };
}

[[nodiscard]] command::Command fixture_command() {
    return command::Command{
        command::CommandId{7U},
        document::Version{11U},
        document::Version{12U},
        command::FrameNumber{15U},
        command::TransactionId{3U},
        command::UndoContextId{4U},
        command::PutNode{fixture_node()},
        command::EraseNode{document::NodeId{17U}},
    };
}

[[nodiscard]] command::FrameSeal fixture_seal() {
    auto first = fixture_command();
    auto second = first;
    second.id = command::CommandId{8U};
    second.base = document::Version{12U};
    second.result = document::Version{13U};
    second.forward = command::EraseNode{document::NodeId{17U}};
    second.inverse = command::PutNode{fixture_node()};
    return command::FrameSeal{
        command::FrameNumber{15U},
        document::Version{11U},
        document::Version{13U},
        {
            {first.id, command::fingerprint(first)},
            {second.id, command::fingerprint(second)},
        },
    };
}

[[nodiscard]] command::StreamCursor fixture_cursor() {
    return command::StreamCursor{
        command::FrameNumber{15U},
        document::Version{13U},
        command::CommandId{9U},
    };
}

[[nodiscard]] command::StreamCheckpoint fixture_checkpoint() {
    const std::array nodes{
        fixture_node(2U),
        document::Node{
            document::NodeId{99U},
            "output",
            {{"name", std::string{"program"}}},
        },
    };
    auto restored = document::Root::restore(nodes, document::Version{77U});
    MS_CHECK(restored.has_value());
    return command::StreamCheckpoint{
        {
            command::FrameNumber{32U},
            document::Version{77U},
            command::CommandId{18U},
        },
        std::move(*restored),
    };
}

[[nodiscard]] std::vector<std::byte> encode_message(const wire::Message& message) {
    auto encoded = wire::encode(message);
    MS_CHECK(encoded.has_value());
    return std::move(encoded).value();
}

[[nodiscard]] wire::Message decode_message(std::span<const std::byte> bytes) {
    auto decoded = wire::decode(bytes);
    MS_CHECK(decoded.has_value());
    return std::move(decoded).value();
}

[[nodiscard]] std::uint32_t read_u32_le(std::span<const std::byte> bytes) {
    MS_CHECK(bytes.size() >= 4U);
    return static_cast<std::uint32_t>(std::to_integer<unsigned int>(bytes[0]))
        | (static_cast<std::uint32_t>(std::to_integer<unsigned int>(bytes[1])) << 8U)
        | (static_cast<std::uint32_t>(std::to_integer<unsigned int>(bytes[2])) << 16U)
        | (static_cast<std::uint32_t>(std::to_integer<unsigned int>(bytes[3])) << 24U);
}

void write_u32_le(std::span<std::byte> bytes, std::uint32_t value) {
    MS_CHECK(bytes.size() >= 4U);
    for (unsigned int index = 0U; index < 4U; ++index) {
        bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

[[nodiscard]] std::string to_hex(std::span<const std::byte> bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned int>(byte);
        result.push_back(digits[(value >> 4U) & 0x0fU]);
        result.push_back(digits[value & 0x0fU]);
    }
    return result;
}

MS_TEST("wire round trips commands and every property alternative") {
    const auto original = fixture_command();
    const auto decoded = decode_message(encode_message(wire::Message{original}));
    const auto* round_tripped = std::get_if<command::Command>(&decoded);

    MS_CHECK(round_tripped != nullptr);
    MS_CHECK(*round_tripped == original);
    const auto* put = std::get_if<command::PutNode>(&round_tripped->forward);
    MS_CHECK(put != nullptr);
    MS_CHECK(std::holds_alternative<bool>(put->node.properties.at("boolean")));
    MS_CHECK(std::holds_alternative<double>(put->node.properties.at("float64")));
    MS_CHECK(std::holds_alternative<std::int64_t>(put->node.properties.at("int64")));
    MS_CHECK(std::holds_alternative<std::string>(put->node.properties.at("string")));
}

MS_TEST("wire round trips frame seals and stream cursors") {
    const auto seal = fixture_seal();
    const auto decoded_seal = decode_message(encode_message(wire::Message{seal}));
    const auto* round_tripped_seal = std::get_if<command::FrameSeal>(&decoded_seal);
    MS_CHECK(round_tripped_seal != nullptr);
    MS_CHECK(*round_tripped_seal == seal);

    const auto cursor = fixture_cursor();
    const auto decoded_cursor = decode_message(encode_message(wire::Message{cursor}));
    const auto* round_tripped_cursor = std::get_if<command::StreamCursor>(&decoded_cursor);
    MS_CHECK(round_tripped_cursor != nullptr);
    MS_CHECK(*round_tripped_cursor == cursor);
}

MS_TEST("wire restores checkpoints at their historical revision") {
    const auto checkpoint = fixture_checkpoint();
    const auto decoded = decode_message(encode_message(wire::Message{checkpoint}));
    const auto* round_tripped = std::get_if<command::StreamCheckpoint>(&decoded);

    MS_CHECK(round_tripped != nullptr);
    MS_CHECK(round_tripped->cursor == checkpoint.cursor);
    MS_CHECK(round_tripped->document.version() == document::Version{77U});
    MS_CHECK(round_tripped->document.size() == 2U);
    const auto* first_node = round_tripped->document.get(document::NodeId{2U});
    const auto* second_node = round_tripped->document.get(document::NodeId{99U});
    const auto* original_second_node = checkpoint.document.get(document::NodeId{99U});
    MS_CHECK(first_node != nullptr);
    MS_CHECK(second_node != nullptr);
    MS_CHECK(original_second_node != nullptr);
    MS_CHECK(*first_node == fixture_node(2U));
    MS_CHECK(*second_node == *original_second_node);
}

MS_TEST("wire encoding is canonical after decode and re-encode") {
    const std::array<wire::Message, 4U> messages{
        wire::Message{fixture_command()},
        wire::Message{fixture_seal()},
        wire::Message{fixture_cursor()},
        wire::Message{fixture_checkpoint()},
    };

    for (const auto& message : messages) {
        const auto first = encode_message(message);
        const auto decoded = decode_message(first);
        const auto second = encode_message(decoded);
        MS_CHECK(second == first);
    }
}

MS_TEST("wire enforces the exact size prefix and rejects every truncation") {
    const auto encoded = encode_message(wire::Message{fixture_cursor()});
    MS_CHECK(static_cast<std::size_t>(read_u32_le(encoded)) == encoded.size() - 4U);

    auto trailing = encoded;
    trailing.push_back(std::byte{0x00});
    const auto trailing_result = wire::decode(trailing);
    MS_CHECK(!trailing_result.has_value());
    MS_CHECK(trailing_result.error() == wire::DecodeError::InvalidBuffer);

    auto short_prefix = encoded;
    write_u32_le(short_prefix, read_u32_le(short_prefix) - 1U);
    const auto short_prefix_result = wire::decode(short_prefix);
    MS_CHECK(!short_prefix_result.has_value());
    MS_CHECK(short_prefix_result.error() == wire::DecodeError::InvalidBuffer);

    auto long_prefix = encoded;
    write_u32_le(long_prefix, read_u32_le(long_prefix) + 1U);
    const auto long_prefix_result = wire::decode(long_prefix);
    MS_CHECK(!long_prefix_result.has_value());
    MS_CHECK(long_prefix_result.error() == wire::DecodeError::InvalidBuffer);

    for (std::size_t size = 0U; size < encoded.size(); ++size) {
        const auto result = wire::decode(std::span{encoded.data(), size});
        MS_CHECK(!result.has_value());
    }
}

MS_TEST("wire rejects invalid public protocol values during encode") {
    auto invalid_command = fixture_command();
    invalid_command.id = command::CommandId{0U};
    auto command_result = wire::encode(wire::Message{invalid_command});
    MS_CHECK(!command_result.has_value());
    MS_CHECK(command_result.error() == wire::EncodeError::InvalidCommand);

    invalid_command = fixture_command();
    invalid_command.result = invalid_command.base;
    command_result = wire::encode(wire::Message{invalid_command});
    MS_CHECK(!command_result.has_value());
    MS_CHECK(command_result.error() == wire::EncodeError::InvalidCommand);

    auto invalid_property = fixture_command();
    std::get<command::PutNode>(invalid_property.forward).node.properties["float64"] =
        std::numeric_limits<double>::infinity();
    auto property_result = wire::encode(wire::Message{invalid_property});
    MS_CHECK(!property_result.has_value());
    MS_CHECK(property_result.error() == wire::EncodeError::InvalidProperty);

    invalid_property = fixture_command();
    std::get<command::PutNode>(invalid_property.forward).node.properties["string"] =
        std::string(1U, static_cast<char>(0xff));
    property_result = wire::encode(wire::Message{invalid_property});
    MS_CHECK(!property_result.has_value());
    MS_CHECK(property_result.error() == wire::EncodeError::InvalidProperty);

    auto invalid_seal = fixture_seal();
    invalid_seal.commands[1].id = command::CommandId{9U};
    auto seal_result = wire::encode(wire::Message{invalid_seal});
    MS_CHECK(!seal_result.has_value());
    MS_CHECK(seal_result.error() == wire::EncodeError::InvalidSeal);

    auto invalid_cursor = fixture_cursor();
    invalid_cursor.next_command = command::CommandId{0U};
    auto cursor_result = wire::encode(wire::Message{invalid_cursor});
    MS_CHECK(!cursor_result.has_value());
    MS_CHECK(cursor_result.error() == wire::EncodeError::InvalidCursor);

    auto invalid_checkpoint = fixture_checkpoint();
    invalid_checkpoint.cursor.revision = document::Version{76U};
    auto checkpoint_result = wire::encode(wire::Message{invalid_checkpoint});
    MS_CHECK(!checkpoint_result.has_value());
    MS_CHECK(checkpoint_result.error() == wire::EncodeError::InvalidCheckpoint);
}

MS_TEST("wire decode applies message and semantic resource limits") {
    const auto command_bytes = encode_message(wire::Message{fixture_command()});
    auto limits = wire::DecodeLimits{};
    limits.max_message_bytes = command_bytes.size() - 1U;
    auto message_limited = wire::decode(command_bytes, limits);
    MS_CHECK(!message_limited.has_value());
    MS_CHECK(message_limited.error() == wire::DecodeError::SizeLimitExceeded);

    limits = wire::DecodeLimits{};
    limits.max_string_bytes = 3U;
    auto string_limited = wire::decode(command_bytes, limits);
    MS_CHECK(!string_limited.has_value());
    MS_CHECK(string_limited.error() == wire::DecodeError::SizeLimitExceeded);

    limits = wire::DecodeLimits{};
    limits.max_properties_per_node = 3U;
    auto property_limited = wire::decode(command_bytes, limits);
    MS_CHECK(!property_limited.has_value());
    MS_CHECK(property_limited.error() == wire::DecodeError::SizeLimitExceeded);

    const auto seal_bytes = encode_message(wire::Message{fixture_seal()});
    limits = wire::DecodeLimits{};
    limits.max_sealed_commands = 1U;
    auto seal_limited = wire::decode(seal_bytes, limits);
    MS_CHECK(!seal_limited.has_value());
    MS_CHECK(seal_limited.error() == wire::DecodeError::SizeLimitExceeded);

    const auto checkpoint_bytes = encode_message(wire::Message{fixture_checkpoint()});
    limits = wire::DecodeLimits{};
    limits.max_nodes = 1U;
    auto nodes_limited = wire::decode(checkpoint_bytes, limits);
    MS_CHECK(!nodes_limited.has_value());
    MS_CHECK(nodes_limited.error() == wire::DecodeError::SizeLimitExceeded);

    limits = wire::DecodeLimits{};
    limits.max_total_properties = 4U;
    auto total_properties_limited = wire::decode(checkpoint_bytes, limits);
    MS_CHECK(!total_properties_limited.has_value());
    MS_CHECK(total_properties_limited.error() == wire::DecodeError::SizeLimitExceeded);
}

MS_TEST("wire encode enforces its aggregate allocation budget") {
    auto limits = wire::DecodeLimits{};
    limits.max_message_bytes = 512U;
    auto command = fixture_command();
    std::get<command::PutNode>(command.forward).node.properties["string"] =
        std::string(400U, 'x');

    const auto encoded = wire::encode(wire::Message{command}, limits);
    MS_CHECK(!encoded.has_value());
    MS_CHECK(encoded.error() == wire::EncodeError::SizeLimitExceeded);
}

MS_TEST("wire version 1 command encoding matches its frozen golden bytes") {
    const auto encoded = encode_message(wire::Message{fixture_command()});
    constexpr std::string_view expected_hex =
        "bc010000180000004d5357500000000000000a000c000600050008000a000000"
        "000101001c000000180046001000180020002800300038000600080007000c00"
        "1800000000000102580000004000000007000000000000000b00000000000000"
        "0c000000000000000f0000000000000003000000000000000400000000000000"
        "00000000000006000e000400060000001100000000000000000006000a000400"
        "060000001000000000000a0014000c00040008000a000000240000000c000000"
        "110000000000000004000000c800000084000000500000001000000007000000"
        "73757266616365005effffff00000004280000000c0000000000060008000400"
        "06000000040000000800000063616d6572612041000000000600000073747269"
        "6e6700009affffff000000021c0000000c000000000006000c00040006000000"
        "b7ffffffffffffff05000000696e743634000000caffffff0000000320000000"
        "0c000000000006001000040006000000000000000000e03f0000000007000000"
        "666c6f617436340000000a001200080007000c000a0000000000000118000000"
        "0c0000000000060008000700060000000000000107000000626f6f6c65616e00";
    MS_CHECK(to_hex(encoded) == expected_hex);
}

MS_TEST("wire version 1 frame seal encoding matches its frozen golden bytes") {
    const auto encoded = encode_message(wire::Message{fixture_seal()});
    constexpr std::string_view expected_hex =
        "94000000140000004d53575000000a000c000600050008000a00000000020100"
        "100000000c00200008001000180004000c0000001c0000000f00000000000000"
        "0b000000000000000d0000000000000002000000300000000c00000008001800"
        "04000c000800000008000000000000005c0bac8bbd2fe0230000000008001400"
        "04000c0008000000070000000000000015bcf237e2c88153";
    MS_CHECK(to_hex(encoded) == expected_hex);
}

MS_TEST("wire version 1 checkpoint encoding matches its frozen golden bytes") {
    const auto encoded = encode_message(wire::Message{fixture_checkpoint()});
    constexpr std::string_view expected_hex =
        "f4010000180000004d5357500000000000000a000c000600050008000a000000"
        "000401000c00000008000c000400080008000000200000000400000002000000"
        "ac0000003800000000000a001e0004000c0014000a0000002000000000000000"
        "4d00000000000000120000000000000000000a0018000c00040008000a000000"
        "1c00000010000000630000000000000000000000010000001c00000006000000"
        "6f7574707574000000000a001000080007000c000a000000000000041c000000"
        "0400000092ffffff040000000700000070726f6772616d00040000006e616d65"
        "00000a0014000c00040008000a000000240000000c0000000200000000000000"
        "04000000c8000000840000005000000010000000070000007375726661636500"
        "5effffff00000004280000000c00000000000600080004000600000004000000"
        "0800000063616d65726120410000000006000000737472696e6700009affffff"
        "000000021c0000000c000000000006000c00040006000000b7ffffffffffffff"
        "05000000696e743634000000caffffff00000003200000000c00000000000600"
        "1000040006000000000000000000e03f0000000007000000666c6f6174363400"
        "00000a001200080007000c000a00000000000001180000000c00000000000600"
        "08000700060000000000000107000000626f6f6c65616e00";
    MS_CHECK(to_hex(encoded) == expected_hex);
}

MS_TEST("wire version 1 cursor encoding matches its frozen golden bytes") {
    const auto encoded = encode_message(wire::Message{fixture_cursor()});
    constexpr std::string_view expected_hex =
        "4c000000180000004d5357500000000000000a000e000600050008000a00000000"
        "0301001000000000000a001c0004000c0014000a0000000f000000000000000d00"
        "0000000000000900000000000000";
    MS_CHECK(to_hex(encoded) == expected_hex);
}

} // namespace
