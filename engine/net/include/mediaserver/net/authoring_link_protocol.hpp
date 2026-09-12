#pragma once
#include <mediaserver/core/result.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mediaserver::net {
inline constexpr std::uint16_t authoring_link_version = 1;
inline constexpr std::size_t link_manifest_limit = 1024U * 1024U;
inline constexpr std::size_t link_chunk_limit = 256U * 1024U;
inline constexpr std::size_t link_packet_limit = link_manifest_limit + 4096U;
enum class LinkMessageKind : std::uint16_t {
    Hello = 1, Welcome, Register, Snapshot, Delta, ResourceAnnounce,
    ResourceChunk, ResourceComplete, Ack, Reject, Resync, Status,
    ShotState, ShotClaim, ShotCommit, ShotRelease, StageContext,
};
// Application-neutral envelope. All strings are bounded; payload is text for
// controls/manifests and opaque bytes only for ResourceChunk. See protocol-v1.md.
struct LinkMessage {
    LinkMessageKind kind{LinkMessageKind::Hello};
    std::uint16_t version{authoring_link_version};
    std::string epoch, scene, transaction, origin, source, base, revision, resource;
    std::uint64_t offset{}, length{};
    std::string payload;
};
using LinkDecodeResult = core::Result<LinkMessage, std::string>;
[[nodiscard]] std::vector<std::byte> encode_link_message(const LinkMessage& message);
[[nodiscard]] LinkDecodeResult decode_link_message(std::span<const std::byte> packet);
// OS entropy, fail closed if unavailable. Used only by the service worker.
[[nodiscard]] std::string new_link_secret();

// Capability negotiation. The frame version stays 1; optional wire features are
// named so a mismatched publisher and Director refuse the specific operation
// with a diagnostic instead of failing inside a record they cannot parse.
// `base` covers objects, prototypes, material records and resource transfer.
// `shot` covers kinds 13-16, `stage` kind 17, and the remaining names cover the
// manifest suffixes added by steps 09-11. A Hello that declares nothing
// negotiates link_default_capabilities(); see qualification-v1.md.
[[nodiscard]] std::span<const std::string_view> link_capabilities() noexcept;
[[nodiscard]] std::span<const std::string_view> link_default_capabilities() noexcept;
[[nodiscard]] bool link_capability_known(std::string_view name) noexcept;
// Client declaration. `declared` is false when the payload carried no
// capabilities line, which keeps a pre-negotiation publisher working.
struct LinkHelloBody {
    std::string code, client;
    std::vector<std::string> capabilities;
    bool declared{};
};
using LinkHelloResult = core::Result<LinkHelloBody, std::string>;
[[nodiscard]] LinkHelloResult parse_link_hello(const std::string& payload);
// Director Welcome text, including this build's advertised capability list.
[[nodiscard]] std::string link_welcome_payload();
[[nodiscard]] std::string link_capability_list(std::span<const std::string> names);
} // namespace mediaserver::net
