#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <mediaserver/cmd/command.hpp>

namespace {

namespace command = mediaserver::cmd;
namespace document = mediaserver::doc;

[[nodiscard]] command::Command command_with_property(document::PropertyValue property) {
    document::Node node{
        document::NodeId{17U},
        "fixture",
        {{"property", std::move(property)}},
    };
    return command::Command{
        command::CommandId{23U},
        document::Version{41U},
        document::Version{42U},
        command::FrameNumber{99U},
        command::TransactionId{7U},
        command::UndoContextId{5U},
        command::PutNode{std::move(node)},
        command::EraseNode{document::NodeId{17U}},
    };
}

static_assert(
    static_cast<mediaserver::core::u8>(command::fingerprint_v1::PropertyTag::Boolean) == 1U);
static_assert(
    static_cast<mediaserver::core::u8>(command::fingerprint_v1::PropertyTag::Int64) == 2U);
static_assert(
    static_cast<mediaserver::core::u8>(command::fingerprint_v1::PropertyTag::Float64) == 3U);
static_assert(
    static_cast<mediaserver::core::u8>(command::fingerprint_v1::PropertyTag::String) == 4U);
static_assert(
    static_cast<mediaserver::core::u8>(command::fingerprint_v1::PayloadTag::PutNode) == 1U);
static_assert(
    static_cast<mediaserver::core::u8>(command::fingerprint_v1::PayloadTag::EraseNode) == 2U);

MS_TEST("version 1 command fingerprints match frozen values") {
    const auto integer = command_with_property(std::int64_t{0});
    const auto text = command_with_property(std::string{"stable"});

    MS_CHECK(
        command::fingerprint(integer) == command::CommandFingerprint{0x7d723216b191c7e7ULL});
    MS_CHECK(
        command::fingerprint(text) == command::CommandFingerprint{0xfba58d4e466fb106ULL});
}

MS_TEST("version 1 property tags distinguish equal underlying bit patterns") {
    const std::array fingerprints{
        command::fingerprint(command_with_property(false)),
        command::fingerprint(command_with_property(std::int64_t{0})),
        command::fingerprint(command_with_property(0.0)),
        command::fingerprint(command_with_property(std::string{})),
    };

    for (std::size_t left = 0U; left < fingerprints.size(); ++left) {
        for (std::size_t right = left + 1U; right < fingerprints.size(); ++right) {
            MS_CHECK(fingerprints[left] != fingerprints[right]);
        }
    }
}

MS_TEST("version 1 payload tags distinguish put and erase alternatives") {
    auto put_then_erase = command_with_property(std::int64_t{0});
    auto erase_then_put = put_then_erase;
    erase_then_put.forward = command::EraseNode{document::NodeId{17U}};
    erase_then_put.inverse = std::get<command::PutNode>(put_then_erase.forward);

    MS_CHECK(command::fingerprint(put_then_erase) != command::fingerprint(erase_then_put));
}

} // namespace
