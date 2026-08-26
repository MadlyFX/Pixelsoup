#include <mediaserver/net/cluster_lifecycle.hpp>

#include <mediaserver/net/tcp_framed_stream.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <cerrno>
#include <sys/random.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
#include <cstdlib>
#endif

namespace mediaserver::net {
namespace {

constexpr std::size_t prefix_bytes = 4U;
constexpr std::size_t digest_bytes = 32U;
constexpr std::size_t nonce_bytes = 32U;
constexpr std::size_t max_lifecycle_message_bytes = 512U;
constexpr std::size_t max_status_text_bytes = 160U;
constexpr std::uint8_t protocol_version = 1U;
constexpr std::array<std::byte, 4U> protocol_magic{
    std::byte{'M'}, std::byte{'S'}, std::byte{'L'}, std::byte{'C'}};
constexpr std::uint8_t challenge_kind = 1U;
constexpr std::uint8_t request_kind = 2U;
constexpr std::uint8_t response_kind = 3U;
constexpr std::uint8_t machine_info_request_kind = 4U;
constexpr std::uint8_t machine_info_response_kind = 5U;
constexpr std::string_view request_domain{"media-server lifecycle request v1"};
constexpr std::string_view response_domain{"media-server lifecycle response v1"};
constexpr std::string_view machine_info_request_domain{
    "media-server machine info request v1"};
constexpr std::string_view machine_info_response_domain{
    "media-server machine info response v1"};
constexpr auto accept_poll_interval = std::chrono::milliseconds{2};
constexpr auto connect_poll_interval = std::chrono::milliseconds{2};

[[nodiscard]] bool valid_secret(std::string_view secret) noexcept {
    return secret.size() >= 32U && secret.size() <= 256U;
}

[[nodiscard]] bool continuation(unsigned char value) noexcept {
    return (value & 0xC0U) == 0x80U;
}

// Protocol text is displayed to an operator. Strict UTF-8 keeps every decoder
// in agreement, while rejecting C0/C1 controls prevents a compromised agent
// from injecting terminal/UI control text into diagnostics.
[[nodiscard]] bool valid_operator_text(std::string_view value) noexcept {
    std::size_t offset{};
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        std::uint32_t codepoint{};
        std::size_t length{};
        if (first <= 0x7FU) {
            codepoint = first;
            length = 1U;
        } else if (first >= 0xC2U && first <= 0xDFU) {
            if (offset + 1U >= value.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(value[offset + 1U]);
            if (!continuation(second)) {
                return false;
            }
            codepoint = (static_cast<std::uint32_t>(first & 0x1FU) << 6U) |
                        static_cast<std::uint32_t>(second & 0x3FU);
            length = 2U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            if (offset + 2U >= value.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(value[offset + 1U]);
            const auto third = static_cast<unsigned char>(value[offset + 2U]);
            if (!continuation(second) || !continuation(third) ||
                (first == 0xE0U && second < 0xA0U) ||
                (first == 0xEDU && second >= 0xA0U)) {
                return false;
            }
            codepoint = (static_cast<std::uint32_t>(first & 0x0FU) << 12U) |
                        (static_cast<std::uint32_t>(second & 0x3FU) << 6U) |
                        static_cast<std::uint32_t>(third & 0x3FU);
            length = 3U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            if (offset + 3U >= value.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(value[offset + 1U]);
            const auto third = static_cast<unsigned char>(value[offset + 2U]);
            const auto fourth = static_cast<unsigned char>(value[offset + 3U]);
            if (!continuation(second) || !continuation(third) ||
                !continuation(fourth) ||
                (first == 0xF0U && second < 0x90U) ||
                (first == 0xF4U && second >= 0x90U)) {
                return false;
            }
            codepoint = (static_cast<std::uint32_t>(first & 0x07U) << 18U) |
                        (static_cast<std::uint32_t>(second & 0x3FU) << 12U) |
                        (static_cast<std::uint32_t>(third & 0x3FU) << 6U) |
                        static_cast<std::uint32_t>(fourth & 0x3FU);
            length = 4U;
        } else {
            return false;
        }
        if (codepoint <= 0x1FU ||
            (codepoint >= 0x7FU && codepoint <= 0x9FU)) {
            return false;
        }
        offset += length;
    }
    return true;
}

void truncate_utf8(std::string& value, std::size_t maximum,
                   bool& truncated) noexcept {
    if (value.size() <= maximum) {
        return;
    }
    std::size_t cut = maximum;
    while (cut > 0U &&
           continuation(static_cast<unsigned char>(value[cut]))) {
        --cut;
    }
    value.resize(cut);
    truncated = true;
}

void put_u16(std::vector<std::byte>& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>(value & 0xffU));
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void put_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        output.push_back(
            static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void put_u64(std::vector<std::byte>& output, std::uint64_t value) {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        output.push_back(
            static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

[[nodiscard]] std::uint16_t get_u16(std::span<const std::byte> input,
                                    std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(input[offset])) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(
                std::to_integer<std::uint8_t>(input[offset + 1U]))
            << 8U);
}

[[nodiscard]] std::uint32_t get_u32(std::span<const std::byte> input,
                                    std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(
                         input[offset + static_cast<std::size_t>(shift / 8U)]))
                 << shift;
    }
    return value;
}

[[nodiscard]] std::uint64_t get_u64(std::span<const std::byte> input,
                                    std::size_t offset) noexcept {
    std::uint64_t value{};
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(
                         input[offset + static_cast<std::size_t>(shift / 8U)]))
                 << shift;
    }
    return value;
}

void put_text(std::vector<std::byte>& output, std::string_view value) {
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(character)));
    }
}

void put_bytes(std::vector<std::byte>& output,
               std::span<const std::byte> value) {
    output.insert(output.end(), value.begin(), value.end());
}

[[nodiscard]] std::vector<std::byte> framed(std::vector<std::byte> body) {
    std::vector<std::byte> result;
    result.reserve(prefix_bytes + body.size());
    put_u32(result, static_cast<std::uint32_t>(body.size()));
    put_bytes(result, body);
    return result;
}

[[nodiscard]] bool header_is(std::span<const std::byte> body,
                             std::uint8_t kind) noexcept {
    return body.size() >= 6U &&
        std::equal(protocol_magic.begin(), protocol_magic.end(), body.begin()) &&
        std::to_integer<std::uint8_t>(body[4U]) == protocol_version &&
        std::to_integer<std::uint8_t>(body[5U]) == kind;
}

// Compact SHA-256 used only for HMAC authentication of this fixed control
// protocol. It is kept private so no caller mistakes it for a general crypto
// package.
class Sha256 final {
public:
    void update(std::span<const std::byte> input) noexcept {
        for (const std::byte byte : input) {
            block_[block_size_++] = std::to_integer<std::uint8_t>(byte);
            bit_count_ += 8U;
            if (block_size_ == block_.size()) {
                transform();
                block_size_ = 0U;
            }
        }
    }

    [[nodiscard]] std::array<std::byte, digest_bytes> finish() noexcept {
        block_[block_size_++] = 0x80U;
        if (block_size_ > 56U) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
                      block_.end(), 0U);
            transform();
            block_size_ = 0U;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
                  block_.begin() + 56, 0U);
        for (std::size_t index = 0U; index < 8U; ++index) {
            block_[63U - index] = static_cast<std::uint8_t>(
                (bit_count_ >> static_cast<std::uint32_t>(index * 8U)) &
                0xffU);
        }
        transform();

        std::array<std::byte, digest_bytes> output{};
        for (std::size_t word = 0U; word < state_.size(); ++word) {
            for (std::size_t byte = 0U; byte < 4U; ++byte) {
                output[word * 4U + byte] = static_cast<std::byte>(
                    (state_[word] >> static_cast<std::uint32_t>(
                                          (3U - byte) * 8U)) &
                    0xffU);
            }
        }
        return output;
    }

private:
    [[nodiscard]] static std::uint32_t choose(std::uint32_t x,
                                               std::uint32_t y,
                                               std::uint32_t z) noexcept {
        return (x & y) ^ (~x & z);
    }
    [[nodiscard]] static std::uint32_t majority(std::uint32_t x,
                                                 std::uint32_t y,
                                                 std::uint32_t z) noexcept {
        return (x & y) ^ (x & z) ^ (y & z);
    }

    void transform() noexcept {
        static constexpr std::array<std::uint32_t, 64U> constants{{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
        }};
        std::array<std::uint32_t, 64U> schedule{};
        for (std::size_t index = 0U; index < 16U; ++index) {
            const std::size_t offset = index * 4U;
            schedule[index] =
                (static_cast<std::uint32_t>(block_[offset]) << 24U) |
                (static_cast<std::uint32_t>(block_[offset + 1U]) << 16U) |
                (static_cast<std::uint32_t>(block_[offset + 2U]) << 8U) |
                static_cast<std::uint32_t>(block_[offset + 3U]);
        }
        for (std::size_t index = 16U; index < schedule.size(); ++index) {
            const std::uint32_t s0 =
                std::rotr(schedule[index - 15U], 7) ^
                std::rotr(schedule[index - 15U], 18) ^
                (schedule[index - 15U] >> 3U);
            const std::uint32_t s1 =
                std::rotr(schedule[index - 2U], 17) ^
                std::rotr(schedule[index - 2U], 19) ^
                (schedule[index - 2U] >> 10U);
            schedule[index] = schedule[index - 16U] + s0 +
                              schedule[index - 7U] + s1;
        }

        std::uint32_t a = state_[0U];
        std::uint32_t b = state_[1U];
        std::uint32_t c = state_[2U];
        std::uint32_t d = state_[3U];
        std::uint32_t e = state_[4U];
        std::uint32_t f = state_[5U];
        std::uint32_t g = state_[6U];
        std::uint32_t h = state_[7U];
        for (std::size_t index = 0U; index < schedule.size(); ++index) {
            const std::uint32_t sigma1 =
                std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const std::uint32_t first = h + sigma1 + choose(e, f, g) +
                                        constants[index] + schedule[index];
            const std::uint32_t sigma0 =
                std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const std::uint32_t second = sigma0 + majority(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + first;
            d = c;
            c = b;
            b = a;
            a = first + second;
        }
        state_[0U] += a;
        state_[1U] += b;
        state_[2U] += c;
        state_[3U] += d;
        state_[4U] += e;
        state_[5U] += f;
        state_[6U] += g;
        state_[7U] += h;
    }

    std::array<std::uint32_t, 8U> state_{{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    }};
    std::array<std::uint8_t, 64U> block_{};
    std::size_t block_size_{};
    std::uint64_t bit_count_{};
};

[[nodiscard]] std::array<std::byte, digest_bytes>
sha256(std::span<const std::byte> bytes) noexcept {
    Sha256 hash;
    hash.update(bytes);
    return hash.finish();
}

[[nodiscard]] std::array<std::byte, digest_bytes>
hmac(std::string_view secret, std::span<const std::byte> message) noexcept {
    constexpr std::size_t block_size = 64U;
    std::array<std::byte, block_size> key{};
    if (secret.size() > block_size) {
        const auto secret_bytes = std::as_bytes(std::span{secret});
        const auto reduced = sha256(secret_bytes);
        std::copy(reduced.begin(), reduced.end(), key.begin());
    } else {
        for (std::size_t index = 0U; index < secret.size(); ++index) {
            key[index] = static_cast<std::byte>(
                static_cast<unsigned char>(secret[index]));
        }
    }
    std::array<std::byte, block_size> inner_pad{};
    std::array<std::byte, block_size> outer_pad{};
    for (std::size_t index = 0U; index < block_size; ++index) {
        inner_pad[index] = key[index] ^ std::byte{0x36U};
        outer_pad[index] = key[index] ^ std::byte{0x5cU};
    }
    Sha256 inner;
    inner.update(inner_pad);
    inner.update(message);
    const auto inner_digest = inner.finish();
    Sha256 outer;
    outer.update(outer_pad);
    outer.update(inner_digest);
    return outer.finish();
}

[[nodiscard]] bool constant_time_equal(std::span<const std::byte> left,
                                       std::span<const std::byte> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    std::uint8_t difference{};
    for (std::size_t index = 0U; index < left.size(); ++index) {
        difference |= std::to_integer<std::uint8_t>(left[index] ^ right[index]);
    }
    return difference == 0U;
}

[[nodiscard]] std::vector<std::byte> request_mac_input(
    core::u32 node_index, ClusterLifecycleCommand command,
    core::u64 request_id,
    std::span<const std::byte, nonce_bytes> nonce) {
    std::vector<std::byte> input;
    input.reserve(request_domain.size() + 1U + 4U + 8U + nonce.size());
    put_text(input, request_domain);
    input.push_back(static_cast<std::byte>(command));
    put_u32(input, node_index);
    put_u64(input, request_id);
    put_bytes(input, nonce);
    return input;
}

[[nodiscard]] std::vector<std::byte> response_mac_input(
    std::span<const std::byte> response_without_tag,
    std::span<const std::byte, nonce_bytes> nonce) {
    std::vector<std::byte> input;
    input.reserve(response_domain.size() + nonce.size() +
                  response_without_tag.size());
    put_text(input, response_domain);
    put_bytes(input, nonce);
    put_bytes(input, response_without_tag);
    return input;
}

[[nodiscard]] std::vector<std::byte> machine_info_request_mac_input(
    core::u32 node_index, core::u64 request_id,
    std::span<const std::byte, nonce_bytes> nonce) {
    std::vector<std::byte> input;
    input.reserve(machine_info_request_domain.size() + 4U + 8U +
                  nonce.size());
    put_text(input, machine_info_request_domain);
    put_u32(input, node_index);
    put_u64(input, request_id);
    put_bytes(input, nonce);
    return input;
}

[[nodiscard]] std::vector<std::byte> machine_info_response_mac_input(
    std::span<const std::byte> response_without_tag,
    std::span<const std::byte, nonce_bytes> nonce) {
    std::vector<std::byte> input;
    input.reserve(machine_info_response_domain.size() + nonce.size() +
                  response_without_tag.size());
    put_text(input, machine_info_response_domain);
    put_bytes(input, nonce);
    put_bytes(input, response_without_tag);
    return input;
}

[[nodiscard]] std::vector<std::byte> challenge_packet(
    std::span<const std::byte, nonce_bytes> nonce) {
    std::vector<std::byte> body;
    body.reserve(40U);
    put_bytes(body, protocol_magic);
    body.push_back(static_cast<std::byte>(protocol_version));
    body.push_back(static_cast<std::byte>(challenge_kind));
    put_u16(body, 0U);
    put_bytes(body, nonce);
    return framed(std::move(body));
}

[[nodiscard]] bool parse_challenge(
    std::span<const std::byte> packet,
    std::array<std::byte, nonce_bytes>& nonce) noexcept {
    if (packet.size() != prefix_bytes + 40U) {
        return false;
    }
    const auto body = packet.subspan(prefix_bytes);
    if (!header_is(body, challenge_kind) || get_u16(body, 6U) != 0U) {
        return false;
    }
    std::copy(body.begin() + 8, body.end(), nonce.begin());
    return true;
}

[[nodiscard]] std::vector<std::byte> request_packet(
    std::string_view secret, core::u32 node_index,
    ClusterLifecycleCommand command, core::u64 request_id,
    std::span<const std::byte, nonce_bytes> nonce) {
    std::vector<std::byte> body;
    body.reserve(84U);
    put_bytes(body, protocol_magic);
    body.push_back(static_cast<std::byte>(protocol_version));
    body.push_back(static_cast<std::byte>(request_kind));
    body.push_back(static_cast<std::byte>(command));
    body.push_back(std::byte{});
    put_u32(body, node_index);
    put_u64(body, request_id);
    put_bytes(body, nonce);
    const auto mac_input = request_mac_input(node_index, command, request_id,
                                             nonce);
    const auto tag = hmac(secret, mac_input);
    put_bytes(body, tag);
    return framed(std::move(body));
}

struct ParsedRequest final {
    core::u32 node_index{};
    ClusterLifecycleCommand command{ClusterLifecycleCommand::Status};
    core::u64 request_id{};
    std::array<std::byte, digest_bytes> tag{};
};

[[nodiscard]] bool parse_request(
    std::span<const std::byte> packet,
    std::span<const std::byte, nonce_bytes> expected_nonce,
    ParsedRequest& output) noexcept {
    if (packet.size() != prefix_bytes + 84U) {
        return false;
    }
    const auto body = packet.subspan(prefix_bytes);
    if (!header_is(body, request_kind) || body[7U] != std::byte{}) {
        return false;
    }
    const auto command = std::to_integer<std::uint8_t>(body[6U]);
    if (command > static_cast<std::uint8_t>(ClusterLifecycleCommand::Stop)) {
        return false;
    }
    const auto nonce = body.subspan(20U, nonce_bytes);
    if (!constant_time_equal(nonce, expected_nonce)) {
        return false;
    }
    output.node_index = get_u32(body, 8U);
    output.command = static_cast<ClusterLifecycleCommand>(command);
    output.request_id = get_u64(body, 12U);
    std::copy(body.begin() + 52, body.end(), output.tag.begin());
    return true;
}

[[nodiscard]] std::vector<std::byte> machine_info_request_packet(
    std::string_view secret, core::u32 node_index, core::u64 request_id,
    std::span<const std::byte, nonce_bytes> nonce) {
    std::vector<std::byte> body;
    body.reserve(84U);
    put_bytes(body, protocol_magic);
    body.push_back(static_cast<std::byte>(protocol_version));
    body.push_back(static_cast<std::byte>(machine_info_request_kind));
    put_u16(body, 0U);
    put_u32(body, node_index);
    put_u64(body, request_id);
    put_bytes(body, nonce);
    const auto mac_input =
        machine_info_request_mac_input(node_index, request_id, nonce);
    put_bytes(body, hmac(secret, mac_input));
    return framed(std::move(body));
}

struct ParsedMachineInfoRequest final {
    core::u32 node_index{};
    core::u64 request_id{};
    std::array<std::byte, digest_bytes> tag{};
};

[[nodiscard]] bool parse_machine_info_request(
    std::span<const std::byte> packet,
    std::span<const std::byte, nonce_bytes> expected_nonce,
    ParsedMachineInfoRequest& output) noexcept {
    if (packet.size() != prefix_bytes + 84U) {
        return false;
    }
    const auto body = packet.subspan(prefix_bytes);
    if (!header_is(body, machine_info_request_kind) ||
        get_u16(body, 6U) != 0U) {
        return false;
    }
    const auto nonce = body.subspan(20U, nonce_bytes);
    if (!constant_time_equal(nonce, expected_nonce)) {
        return false;
    }
    output.node_index = get_u32(body, 8U);
    output.request_id = get_u64(body, 12U);
    std::copy(body.begin() + 52, body.end(), output.tag.begin());
    return true;
}

[[nodiscard]] std::vector<std::byte> response_packet(
    std::string_view secret, ClusterLifecycleReply reply,
    core::u64 request_id,
    std::span<const std::byte, nonce_bytes> nonce) {
    if (reply.message.size() > max_status_text_bytes) {
        reply.message.resize(max_status_text_bytes);
    }
    std::vector<std::byte> body;
    body.reserve(62U + reply.message.size());
    put_bytes(body, protocol_magic);
    body.push_back(static_cast<std::byte>(protocol_version));
    body.push_back(static_cast<std::byte>(response_kind));
    body.push_back(static_cast<std::byte>(reply.outcome));
    body.push_back(static_cast<std::byte>(reply.state));
    put_u32(body, reply.node_index);
    put_u64(body, reply.generation);
    put_u64(body, request_id);
    put_u16(body, static_cast<std::uint16_t>(reply.message.size()));
    put_text(body, reply.message);
    const auto mac_input = response_mac_input(body, nonce);
    const auto tag = hmac(secret, mac_input);
    put_bytes(body, tag);
    return framed(std::move(body));
}

enum class ResponseParse : std::uint8_t {
    Ok,
    Protocol,
    Authentication,
};

[[nodiscard]] ResponseParse parse_response(
    std::span<const std::byte> packet, std::string_view secret,
    core::u64 expected_request_id,
    std::span<const std::byte, nonce_bytes> nonce,
    ClusterLifecycleReply& output) {
    if (packet.size() < prefix_bytes + 62U ||
        packet.size() > max_lifecycle_message_bytes) {
        return ResponseParse::Protocol;
    }
    const auto body = packet.subspan(prefix_bytes);
    if (!header_is(body, response_kind)) {
        return ResponseParse::Protocol;
    }
    const auto outcome = std::to_integer<std::uint8_t>(body[6U]);
    const auto state = std::to_integer<std::uint8_t>(body[7U]);
    if (outcome > static_cast<std::uint8_t>(ClusterLifecycleOutcome::Busy) ||
        state > static_cast<std::uint8_t>(ClusterProcessState::Faulted) ||
        get_u64(body, 20U) != expected_request_id) {
        return ResponseParse::Protocol;
    }
    const std::size_t message_size = get_u16(body, 28U);
    if (message_size > max_status_text_bytes ||
        body.size() != 30U + message_size + digest_bytes) {
        return ResponseParse::Protocol;
    }
    const auto without_tag = body.first(body.size() - digest_bytes);
    const auto received_tag = body.last(digest_bytes);
    const auto mac_input = response_mac_input(without_tag, nonce);
    const auto expected_tag = hmac(secret, mac_input);
    if (!constant_time_equal(received_tag, expected_tag)) {
        return ResponseParse::Authentication;
    }
    output.outcome = static_cast<ClusterLifecycleOutcome>(outcome);
    output.state = static_cast<ClusterProcessState>(state);
    output.node_index = get_u32(body, 8U);
    output.generation = get_u64(body, 12U);
    output.message.assign(
        reinterpret_cast<const char*>(body.data() + 30U), message_size);
    return ResponseParse::Ok;
}

[[nodiscard]] ClusterMachineInfo failed_machine_info(core::u32 node_index,
                                                      std::string message) {
    ClusterMachineInfo result{};
    result.availability = ClusterMachineInfoAvailability::Failed;
    result.display_state = ClusterDisplayEnumerationState::Failed;
    result.node_index = node_index;
    result.message = std::move(message);
    return result;
}

[[nodiscard]] ClusterMachineInfo unavailable_machine_info(
    core::u32 node_index) {
    ClusterMachineInfo result{};
    result.availability = ClusterMachineInfoAvailability::Unavailable;
    result.display_state = ClusterDisplayEnumerationState::Unsupported;
    result.node_index = node_index;
    result.message = "machine inventory unavailable";
    return result;
}

[[nodiscard]] ClusterMachineInfo mismatch_machine_info(core::u32 node_index) {
    ClusterMachineInfo result{};
    result.availability =
        ClusterMachineInfoAvailability::ConfigurationMismatch;
    result.display_state = ClusterDisplayEnumerationState::Unsupported;
    result.node_index = node_index;
    result.message = "listener owns a different node index";
    return result;
}

[[nodiscard]] bool normalize_machine_info(ClusterMachineInfo& info,
                                          core::u32 node_index) {
    info.node_index = node_index;
    if (static_cast<std::uint8_t>(info.availability) >
            static_cast<std::uint8_t>(ClusterMachineInfoAvailability::Failed) ||
        static_cast<std::uint8_t>(info.display_state) >
            static_cast<std::uint8_t>(ClusterDisplayEnumerationState::Failed) ||
        (info.physical_memory_bytes != 0U &&
         info.available_memory_bytes > info.physical_memory_bytes)) {
        info = failed_machine_info(node_index,
                                   "invalid machine inventory state");
        return true;
    }
    bool valid = valid_operator_text(info.host_name) &&
                 valid_operator_text(info.operating_system) &&
                 valid_operator_text(info.message);
    for (const ClusterGpuAdapterInfo& adapter : info.adapters) {
        valid = valid && valid_operator_text(adapter.name);
    }
    for (const ClusterDisplayTargetInfo& target : info.display_targets) {
        valid = valid && valid_operator_text(target.name) &&
                static_cast<std::uint8_t>(target.technology) <=
                    static_cast<std::uint8_t>(
                        ClusterVideoOutputTechnology::Other) &&
                ((target.width == 0U) == (target.height == 0U)) &&
                ((target.refresh_numerator == 0U) ==
                 (target.refresh_denominator == 0U));
    }
    if (!valid) {
        info = failed_machine_info(
            node_index, "machine inventory contained invalid text");
        return true;
    }

    if (info.adapters.size() > max_cluster_machine_gpu_adapters) {
        info.adapters.resize(max_cluster_machine_gpu_adapters);
        info.truncated = true;
    }
    if (info.display_targets.size() >
        max_cluster_machine_display_targets) {
        info.display_targets.resize(max_cluster_machine_display_targets);
        info.truncated = true;
    }
    truncate_utf8(info.host_name, max_cluster_machine_host_name_bytes,
                  info.truncated);
    truncate_utf8(info.operating_system,
                  max_cluster_machine_operating_system_bytes,
                  info.truncated);
    truncate_utf8(info.message, max_cluster_machine_message_bytes,
                  info.truncated);
    for (ClusterGpuAdapterInfo& adapter : info.adapters) {
        truncate_utf8(adapter.name, max_cluster_machine_adapter_name_bytes,
                      info.truncated);
    }
    for (ClusterDisplayTargetInfo& target : info.display_targets) {
        truncate_utf8(target.name, max_cluster_machine_display_name_bytes,
                      info.truncated);
    }
    return true;
}

[[nodiscard]] std::vector<std::byte> machine_info_response_packet(
    std::string_view secret, ClusterMachineInfo info, core::u64 request_id,
    std::span<const std::byte, nonce_bytes> nonce) {
    std::vector<std::byte> body;
    body.reserve(max_cluster_machine_info_message_bytes);
    put_bytes(body, protocol_magic);
    body.push_back(static_cast<std::byte>(protocol_version));
    body.push_back(static_cast<std::byte>(machine_info_response_kind));
    body.push_back(static_cast<std::byte>(info.availability));
    body.push_back(static_cast<std::byte>(info.display_state));
    put_u32(body, info.node_index);
    put_u64(body, info.sample_generation);
    put_u64(body, request_id);
    put_u16(body, info.truncated ? 1U : 0U);
    put_u32(body, info.logical_cpu_count);
    put_u64(body, info.uptime_ms);
    put_u64(body, info.physical_memory_bytes);
    put_u64(body, info.available_memory_bytes);
    body.push_back(static_cast<std::byte>(info.adapters.size()));
    body.push_back(static_cast<std::byte>(info.display_targets.size()));
    body.push_back(static_cast<std::byte>(info.host_name.size()));
    body.push_back(static_cast<std::byte>(info.operating_system.size()));
    body.push_back(static_cast<std::byte>(info.message.size()));
    body.push_back(std::byte{});
    put_text(body, info.host_name);
    put_text(body, info.operating_system);
    put_text(body, info.message);

    for (const ClusterGpuAdapterInfo& adapter : info.adapters) {
        put_u64(body, adapter.adapter_id);
        put_u32(body, adapter.vendor_id);
        put_u32(body, adapter.device_id);
        put_u32(body, adapter.subsystem_id);
        put_u32(body, adapter.revision);
        put_u64(body, adapter.dedicated_video_memory_bytes);
        put_u64(body, adapter.shared_system_memory_bytes);
        const std::uint8_t flags =
            static_cast<std::uint8_t>((adapter.software ? 1U : 0U) |
                                      (adapter.remote ? 2U : 0U));
        body.push_back(static_cast<std::byte>(flags));
        body.push_back(static_cast<std::byte>(adapter.name.size()));
        put_u16(body, 0U);
        put_text(body, adapter.name);
    }
    for (const ClusterDisplayTargetInfo& target : info.display_targets) {
        put_u64(body, target.adapter_id);
        put_u32(body, target.target_id);
        put_u32(body, target.connector_instance);
        put_u16(body, target.edid_manufacturer_id);
        put_u16(body, target.edid_product_code_id);
        body.push_back(static_cast<std::byte>(target.technology));
        const std::uint8_t flags =
            static_cast<std::uint8_t>((target.active ? 1U : 0U) |
                                      (target.available ? 2U : 0U));
        body.push_back(static_cast<std::byte>(flags));
        put_u16(body, 0U);
        put_u32(body, target.width);
        put_u32(body, target.height);
        put_u32(body, target.refresh_numerator);
        put_u32(body, target.refresh_denominator);
        body.push_back(static_cast<std::byte>(target.name.size()));
        body.push_back(std::byte{});
        body.push_back(std::byte{});
        body.push_back(std::byte{});
        put_text(body, target.name);
    }
    const auto mac_input = machine_info_response_mac_input(body, nonce);
    put_bytes(body, hmac(secret, mac_input));
    return framed(std::move(body));
}

[[nodiscard]] bool parse_bounded_text(std::span<const std::byte> body,
                                      std::size_t tag_offset,
                                      std::size_t& cursor,
                                      std::size_t length,
                                      std::size_t maximum,
                                      std::string& output) {
    if (length > maximum || cursor > tag_offset ||
        length > tag_offset - cursor) {
        return false;
    }
    const std::string_view value{
        reinterpret_cast<const char*>(body.data() + cursor), length};
    if (!valid_operator_text(value)) {
        return false;
    }
    output.assign(value);
    cursor += length;
    return true;
}

[[nodiscard]] ResponseParse parse_machine_info_response(
    std::span<const std::byte> packet, std::string_view secret,
    core::u64 expected_request_id,
    std::span<const std::byte, nonce_bytes> nonce,
    ClusterMachineInfo& output) {
    constexpr std::size_t fixed_body_bytes = 64U;
    if (packet.size() < prefix_bytes + fixed_body_bytes + digest_bytes ||
        packet.size() > max_cluster_machine_info_message_bytes) {
        return ResponseParse::Protocol;
    }
    const auto body = packet.subspan(prefix_bytes);
    if (!header_is(body, machine_info_response_kind) ||
        get_u64(body, 20U) != expected_request_id || body[63U] != std::byte{}) {
        return ResponseParse::Protocol;
    }
    const auto availability = std::to_integer<std::uint8_t>(body[6U]);
    const auto display_state = std::to_integer<std::uint8_t>(body[7U]);
    const auto flags = get_u16(body, 28U);
    const std::size_t adapter_count =
        std::to_integer<std::uint8_t>(body[58U]);
    const std::size_t target_count =
        std::to_integer<std::uint8_t>(body[59U]);
    if (availability > static_cast<std::uint8_t>(
                           ClusterMachineInfoAvailability::ConfigurationMismatch) ||
        display_state > static_cast<std::uint8_t>(
                            ClusterDisplayEnumerationState::Failed) ||
        (flags & ~1U) != 0U ||
        adapter_count > max_cluster_machine_gpu_adapters ||
        target_count > max_cluster_machine_display_targets) {
        return ResponseParse::Protocol;
    }
    const std::size_t tag_offset = body.size() - digest_bytes;
    const auto without_tag = body.first(tag_offset);
    const auto received_tag = body.last(digest_bytes);
    const auto expected_tag =
        hmac(secret, machine_info_response_mac_input(without_tag, nonce));
    if (!constant_time_equal(received_tag, expected_tag)) {
        return ResponseParse::Authentication;
    }

    ClusterMachineInfo decoded{};
    decoded.availability =
        static_cast<ClusterMachineInfoAvailability>(availability);
    decoded.display_state =
        static_cast<ClusterDisplayEnumerationState>(display_state);
    decoded.node_index = get_u32(body, 8U);
    decoded.sample_generation = get_u64(body, 12U);
    decoded.truncated = (flags & 1U) != 0U;
    decoded.logical_cpu_count = get_u32(body, 30U);
    decoded.uptime_ms = get_u64(body, 34U);
    decoded.physical_memory_bytes = get_u64(body, 42U);
    decoded.available_memory_bytes = get_u64(body, 50U);
    if (decoded.physical_memory_bytes != 0U &&
        decoded.available_memory_bytes > decoded.physical_memory_bytes) {
        return ResponseParse::Protocol;
    }

    std::size_t cursor = fixed_body_bytes;
    if (!parse_bounded_text(body, tag_offset, cursor,
                            std::to_integer<std::uint8_t>(body[60U]),
                            max_cluster_machine_host_name_bytes,
                            decoded.host_name) ||
        !parse_bounded_text(body, tag_offset, cursor,
                            std::to_integer<std::uint8_t>(body[61U]),
                            max_cluster_machine_operating_system_bytes,
                            decoded.operating_system) ||
        !parse_bounded_text(body, tag_offset, cursor,
                            std::to_integer<std::uint8_t>(body[62U]),
                            max_cluster_machine_message_bytes,
                            decoded.message)) {
        return ResponseParse::Protocol;
    }

    decoded.adapters.reserve(adapter_count);
    for (std::size_t index = 0U; index < adapter_count; ++index) {
        constexpr std::size_t fixed_adapter_bytes = 44U;
        if (cursor > tag_offset || fixed_adapter_bytes > tag_offset - cursor) {
            return ResponseParse::Protocol;
        }
        const auto record = body.subspan(cursor, fixed_adapter_bytes);
        const auto record_flags = std::to_integer<std::uint8_t>(record[40U]);
        const std::size_t name_length =
            std::to_integer<std::uint8_t>(record[41U]);
        if ((record_flags & ~3U) != 0U || get_u16(record, 42U) != 0U) {
            return ResponseParse::Protocol;
        }
        ClusterGpuAdapterInfo adapter{};
        adapter.adapter_id = get_u64(record, 0U);
        adapter.vendor_id = get_u32(record, 8U);
        adapter.device_id = get_u32(record, 12U);
        adapter.subsystem_id = get_u32(record, 16U);
        adapter.revision = get_u32(record, 20U);
        adapter.dedicated_video_memory_bytes = get_u64(record, 24U);
        adapter.shared_system_memory_bytes = get_u64(record, 32U);
        adapter.software = (record_flags & 1U) != 0U;
        adapter.remote = (record_flags & 2U) != 0U;
        cursor += fixed_adapter_bytes;
        if (!parse_bounded_text(body, tag_offset, cursor, name_length,
                                max_cluster_machine_adapter_name_bytes,
                                adapter.name)) {
            return ResponseParse::Protocol;
        }
        decoded.adapters.push_back(std::move(adapter));
    }

    decoded.display_targets.reserve(target_count);
    for (std::size_t index = 0U; index < target_count; ++index) {
        constexpr std::size_t fixed_target_bytes = 44U;
        if (cursor > tag_offset || fixed_target_bytes > tag_offset - cursor) {
            return ResponseParse::Protocol;
        }
        const auto record = body.subspan(cursor, fixed_target_bytes);
        const auto technology = std::to_integer<std::uint8_t>(record[20U]);
        const auto record_flags = std::to_integer<std::uint8_t>(record[21U]);
        const std::size_t name_length =
            std::to_integer<std::uint8_t>(record[40U]);
        if (technology > static_cast<std::uint8_t>(
                             ClusterVideoOutputTechnology::Other) ||
            (record_flags & ~3U) != 0U || get_u16(record, 22U) != 0U ||
            record[41U] != std::byte{} || record[42U] != std::byte{} ||
            record[43U] != std::byte{}) {
            return ResponseParse::Protocol;
        }
        ClusterDisplayTargetInfo target{};
        target.adapter_id = get_u64(record, 0U);
        target.target_id = get_u32(record, 8U);
        target.connector_instance = get_u32(record, 12U);
        target.edid_manufacturer_id = get_u16(record, 16U);
        target.edid_product_code_id = get_u16(record, 18U);
        target.technology =
            static_cast<ClusterVideoOutputTechnology>(technology);
        target.active = (record_flags & 1U) != 0U;
        target.available = (record_flags & 2U) != 0U;
        target.width = get_u32(record, 24U);
        target.height = get_u32(record, 28U);
        target.refresh_numerator = get_u32(record, 32U);
        target.refresh_denominator = get_u32(record, 36U);
        if ((target.width == 0U) != (target.height == 0U) ||
            (target.refresh_numerator == 0U) !=
                (target.refresh_denominator == 0U)) {
            return ResponseParse::Protocol;
        }
        cursor += fixed_target_bytes;
        if (!parse_bounded_text(body, tag_offset, cursor, name_length,
                                max_cluster_machine_display_name_bytes,
                                target.name)) {
            return ResponseParse::Protocol;
        }
        decoded.display_targets.push_back(std::move(target));
    }
    if (cursor != tag_offset) {
        return ResponseParse::Protocol;
    }
    if (decoded.availability ==
        ClusterMachineInfoAvailability::ConfigurationMismatch) {
        const bool disclosed = decoded.logical_cpu_count != 0U ||
                               decoded.sample_generation != 0U ||
                               decoded.uptime_ms != 0U ||
                               decoded.physical_memory_bytes != 0U ||
                               decoded.available_memory_bytes != 0U ||
                               decoded.truncated || !decoded.host_name.empty() ||
                               !decoded.operating_system.empty() ||
                               !decoded.adapters.empty() ||
                               !decoded.display_targets.empty();
        if (disclosed) {
            return ResponseParse::Protocol;
        }
    }
    output = std::move(decoded);
    return ResponseParse::Ok;
}

[[nodiscard]] bool random_nonce(
    std::array<std::byte, nonce_bytes>& output) noexcept {
#if defined(_WIN32)
    return BCryptGenRandom(
               nullptr, reinterpret_cast<PUCHAR>(output.data()),
               static_cast<ULONG>(output.size()),
               BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__linux__)
    std::size_t offset{};
    while (offset < output.size()) {
        const ssize_t received = getrandom(output.data() + offset,
                                           output.size() - offset, 0U);
        if (received > 0) {
            offset += static_cast<std::size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
    arc4random_buf(output.data(), output.size());
    return true;
#else
    // Fail closed on an unported platform rather than silently substituting a
    // deterministic or implementation-defined random source for an auth nonce.
    static_cast<void>(output);
    return false;
#endif
}

[[nodiscard]] ClusterLifecycleClientError transport_error(
    const FramedStreamError& error) noexcept {
    return ClusterLifecycleClientError{
        error.code == FramedStreamErrorCode::TimedOut
            ? ClusterLifecycleClientErrorCode::TimedOut
            : (error.code == FramedStreamErrorCode::AllocationFailed
                   ? ClusterLifecycleClientErrorCode::AllocationFailed
                   : ClusterLifecycleClientErrorCode::Transport),
        error.system_error};
}

[[nodiscard]] core::Result<asio::ip::tcp::socket,
                           ClusterLifecycleClientError>
connect_with_timeout(const asio::ip::tcp::endpoint& endpoint,
                     std::chrono::milliseconds timeout,
                     asio::io_context& context) {
    using Result = core::Result<asio::ip::tcp::socket,
                                ClusterLifecycleClientError>;
    asio::ip::tcp::socket socket{context};
    std::error_code open_error;
    socket.open(endpoint.protocol(), open_error);
    if (open_error) {
        return Result::failure({ClusterLifecycleClientErrorCode::OpenFailed,
                                open_error});
    }
    bool complete = false;
    std::error_code connect_error;
    socket.async_connect(endpoint, [&](const std::error_code& error) {
        connect_error = error;
        complete = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool timed_out = false;
    while (!complete) {
        if (!timed_out && std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            socket.close(open_error);
        }
        static_cast<void>(context.run_for(connect_poll_interval));
        if (context.stopped() && !complete) {
            context.restart();
        }
    }
    if (timed_out) {
        return Result::failure(
            {ClusterLifecycleClientErrorCode::TimedOut, {}});
    }
    if (connect_error) {
        return Result::failure(
            {ClusterLifecycleClientErrorCode::ConnectFailed, connect_error});
    }
    return Result::success(std::move(socket));
}

} // namespace

namespace detail {

bool cluster_lifecycle_crypto_known_answer() noexcept {
    // RFC 4231, test case 1: HMAC-SHA256 with a 20-byte 0x0b key.
    const std::string secret(20U, static_cast<char>(0x0b));
    constexpr std::string_view data{"Hi There"};
    constexpr std::array<std::byte, digest_bytes> expected{
        std::byte{0xb0}, std::byte{0x34}, std::byte{0x4c}, std::byte{0x61},
        std::byte{0xd8}, std::byte{0xdb}, std::byte{0x38}, std::byte{0x53},
        std::byte{0x5c}, std::byte{0xa8}, std::byte{0xaf}, std::byte{0xce},
        std::byte{0xaf}, std::byte{0x0b}, std::byte{0xf1}, std::byte{0x2b},
        std::byte{0x88}, std::byte{0x1d}, std::byte{0xc2}, std::byte{0x00},
        std::byte{0xc9}, std::byte{0x83}, std::byte{0x3d}, std::byte{0xa7},
        std::byte{0x26}, std::byte{0xe9}, std::byte{0x37}, std::byte{0x6c},
        std::byte{0x2e}, std::byte{0x32}, std::byte{0xcf}, std::byte{0xf7}};
    const auto actual = hmac(secret, std::as_bytes(std::span{data}));
    return constant_time_equal(actual, expected);
}

struct AtomicClusterLifecycleCounters final {
    std::atomic_uint64_t accepted_connections{};
    std::atomic_uint64_t authenticated_requests{};
    std::atomic_uint64_t authentication_failures{};
    std::atomic_uint64_t malformed_requests{};
    std::atomic_uint64_t config_mismatches{};
    std::atomic_uint64_t completed_requests{};
};

class ClusterLifecycleServiceState final {
public:
    ClusterLifecycleServiceState(ClusterLifecycleServiceConfig configured,
                                 ClusterProcessHost& process_host,
                                 ClusterMachineInfoProvider* inventory_provider)
        : config(std::move(configured)), host(&process_host),
          machine_info(inventory_provider), acceptor(context) {
    }

    void handle(asio::ip::tcp::socket socket) noexcept {
        try {
            handle_impl(std::move(socket));
        } catch (const std::bad_alloc&) {
            counters.malformed_requests.fetch_add(1U,
                                                  std::memory_order_relaxed);
        } catch (...) {
            counters.malformed_requests.fetch_add(1U,
                                                  std::memory_order_relaxed);
        }
    }

    void handle_impl(asio::ip::tcp::socket socket) {
        TcpFramedStream stream{
            std::move(socket),
            FramedStreamLimits{max_cluster_machine_info_message_bytes,
                               config.handshake_timeout,
                               config.handshake_timeout,
                               config.write_timeout}};
        std::array<std::byte, nonce_bytes> nonce{};
        if (!random_nonce(nonce)) {
            return;
        }
        const auto challenge = challenge_packet(nonce);
        if (!stream.send(challenge)) {
            return;
        }
        auto received = stream.receive();
        if (!received) {
            return;
        }
        const auto packet = std::span<const std::byte>{received.value()};
        if (packet.size() < prefix_bytes + 6U) {
            counters.malformed_requests.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        const auto body = packet.subspan(prefix_bytes);
        if (header_is(body, request_kind)) {
            ParsedRequest request{};
            if (!parse_request(packet, nonce, request)) {
                counters.malformed_requests.fetch_add(
                    1U, std::memory_order_relaxed);
                return;
            }
            const auto mac_input = request_mac_input(
                request.node_index, request.command, request.request_id, nonce);
            const auto expected_tag = hmac(config.shared_secret, mac_input);
            if (!constant_time_equal(request.tag, expected_tag)) {
                counters.authentication_failures.fetch_add(
                    1U, std::memory_order_relaxed);
                const ClusterLifecycleReply rejected{
                    ClusterLifecycleOutcome::Failed,
                    ClusterProcessState::Faulted,
                    config.node_index,
                    0U,
                    "authentication failed"};
                static_cast<void>(stream.send(response_packet(
                    config.shared_secret, rejected, request.request_id, nonce)));
                return;
            }
            counters.authenticated_requests.fetch_add(
                1U, std::memory_order_relaxed);
            ClusterLifecycleReply reply{};
            if (request.node_index != config.node_index) {
                counters.config_mismatches.fetch_add(
                    1U, std::memory_order_relaxed);
                reply = ClusterLifecycleReply{
                    ClusterLifecycleOutcome::ConfigMismatch,
                    ClusterProcessState::Faulted,
                    config.node_index,
                    0U,
                    "listener owns a different node index"};
            } else {
                reply = host->apply(request.command);
                reply.node_index = config.node_index;
                counters.completed_requests.fetch_add(
                    1U, std::memory_order_relaxed);
            }
            static_cast<void>(stream.send(response_packet(
                config.shared_secret, std::move(reply), request.request_id,
                nonce)));
            return;
        }

        if (header_is(body, machine_info_request_kind)) {
            ParsedMachineInfoRequest request{};
            if (!parse_machine_info_request(packet, nonce, request)) {
                counters.malformed_requests.fetch_add(
                    1U, std::memory_order_relaxed);
                return;
            }
            const auto mac_input = machine_info_request_mac_input(
                request.node_index, request.request_id, nonce);
            const auto expected_tag = hmac(config.shared_secret, mac_input);
            if (!constant_time_equal(request.tag, expected_tag)) {
                counters.authentication_failures.fetch_add(
                    1U, std::memory_order_relaxed);
                auto rejected = failed_machine_info(
                    request.node_index, "authentication failed");
                static_cast<void>(stream.send(machine_info_response_packet(
                    config.shared_secret, std::move(rejected),
                    request.request_id, nonce)));
                return;
            }
            counters.authenticated_requests.fetch_add(
                1U, std::memory_order_relaxed);
            ClusterMachineInfo reply{};
            if (request.node_index != config.node_index) {
                counters.config_mismatches.fetch_add(
                    1U, std::memory_order_relaxed);
                reply = mismatch_machine_info(config.node_index);
            } else if (machine_info == nullptr) {
                reply = unavailable_machine_info(config.node_index);
                counters.completed_requests.fetch_add(
                    1U, std::memory_order_relaxed);
            } else {
                const auto sample = machine_info->snapshot();
                reply = sample != nullptr
                    ? *sample
                    : failed_machine_info(config.node_index,
                                          "machine inventory snapshot missing");
                static_cast<void>(normalize_machine_info(reply,
                                                         config.node_index));
                counters.completed_requests.fetch_add(
                    1U, std::memory_order_relaxed);
            }
            static_cast<void>(stream.send(machine_info_response_packet(
                config.shared_secret, std::move(reply), request.request_id,
                nonce)));
            return;
        }

        counters.malformed_requests.fetch_add(1U, std::memory_order_relaxed);
    }

    void run() noexcept {
        while (!stop_requested.load(std::memory_order_acquire)) {
            asio::ip::tcp::socket socket{context};
            std::error_code error;
            acceptor.accept(socket, error);
            if (!error) {
                counters.accepted_connections.fetch_add(
                    1U, std::memory_order_relaxed);
                handle(std::move(socket));
                continue;
            }
            if (stop_requested.load(std::memory_order_acquire)) {
                break;
            }
            if (error == asio::error::would_block ||
                error == asio::error::try_again) {
                std::this_thread::sleep_for(accept_poll_interval);
                continue;
            }
            std::this_thread::sleep_for(accept_poll_interval);
        }
        running.store(false, std::memory_order_release);
    }

    ClusterLifecycleServiceConfig config;
    ClusterProcessHost* host{};
    ClusterMachineInfoProvider* machine_info{};
    asio::io_context context;
    asio::ip::tcp::acceptor acceptor;
    std::thread thread;
    std::atomic_bool stop_requested{};
    std::atomic_bool running{};
    mutable std::mutex endpoint_mutex;
    std::optional<asio::ip::tcp::endpoint> endpoint;
    AtomicClusterLifecycleCounters counters;
};

} // namespace detail

ClusterLifecycleService::ClusterLifecycleService(
    ClusterLifecycleServiceConfig config, ClusterProcessHost& host,
    ClusterMachineInfoProvider* machine_info)
    : state_(std::make_unique<detail::ClusterLifecycleServiceState>(
          std::move(config), host, machine_info)) {
}

ClusterLifecycleService::~ClusterLifecycleService() {
    stop();
}

bool ClusterLifecycleService::valid_configuration(
    const ClusterLifecycleServiceConfig& config) noexcept {
    const auto zero = std::chrono::milliseconds::zero();
    const auto& address = config.bind_endpoint.address();
    return !address.is_multicast() &&
        (!address.is_v4() ||
         address.to_v4() != asio::ip::address_v4::broadcast()) &&
        valid_secret(config.shared_secret) && config.handshake_timeout > zero &&
        config.write_timeout > zero;
}

core::Result<asio::ip::tcp::endpoint, ClusterLifecycleServiceError>
ClusterLifecycleService::start() {
    using Result = core::Result<asio::ip::tcp::endpoint,
                                ClusterLifecycleServiceError>;
    if (!valid_configuration(state_->config)) {
        return Result::failure(
            {ClusterLifecycleServiceErrorCode::InvalidConfiguration, {}});
    }
    bool expected = false;
    if (!state_->running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return Result::failure(
            {ClusterLifecycleServiceErrorCode::AlreadyRunning, {}});
    }
    state_->stop_requested.store(false, std::memory_order_release);
    std::error_code error;
    state_->acceptor.open(state_->config.bind_endpoint.protocol(), error);
    if (error) {
        state_->running.store(false, std::memory_order_release);
        return Result::failure(
            {ClusterLifecycleServiceErrorCode::OpenFailed, error});
    }
    state_->acceptor.set_option(asio::socket_base::reuse_address{true}, error);
    if (!error) {
        state_->acceptor.bind(state_->config.bind_endpoint, error);
    }
    if (error) {
        state_->acceptor.close();
        state_->running.store(false, std::memory_order_release);
        return Result::failure(
            {ClusterLifecycleServiceErrorCode::BindFailed, error});
    }
    state_->acceptor.listen(asio::socket_base::max_listen_connections, error);
    if (error) {
        state_->acceptor.close();
        state_->running.store(false, std::memory_order_release);
        return Result::failure(
            {ClusterLifecycleServiceErrorCode::ListenFailed, error});
    }
    state_->acceptor.non_blocking(true, error);
    if (error) {
        state_->acceptor.close();
        state_->running.store(false, std::memory_order_release);
        return Result::failure(
            {ClusterLifecycleServiceErrorCode::ListenFailed, error});
    }
    const auto endpoint = state_->acceptor.local_endpoint(error);
    if (error) {
        state_->acceptor.close();
        state_->running.store(false, std::memory_order_release);
        return Result::failure(
            {ClusterLifecycleServiceErrorCode::ListenFailed, error});
    }
    {
        const std::lock_guard lock{state_->endpoint_mutex};
        state_->endpoint = endpoint;
    }
    try {
        state_->thread = std::thread{[state = state_.get()] { state->run(); }};
    } catch (const std::bad_alloc&) {
        state_->acceptor.close();
        state_->running.store(false, std::memory_order_release);
        {
            const std::lock_guard lock{state_->endpoint_mutex};
            state_->endpoint.reset();
        }
        return Result::failure(
            {ClusterLifecycleServiceErrorCode::AllocationFailed, {}});
    } catch (const std::system_error& thread_error) {
        state_->acceptor.close();
        state_->running.store(false, std::memory_order_release);
        {
            const std::lock_guard lock{state_->endpoint_mutex};
            state_->endpoint.reset();
        }
        return Result::failure({ClusterLifecycleServiceErrorCode::ThreadStartFailed,
                                thread_error.code()});
    }
    return Result::success(endpoint);
}

void ClusterLifecycleService::stop() noexcept {
    if (!state_) {
        return;
    }
    state_->stop_requested.store(true, std::memory_order_release);
    std::error_code ignored;
    state_->acceptor.close(ignored);
    if (state_->thread.joinable()) {
        state_->thread.join();
    }
    state_->running.store(false, std::memory_order_release);
    const std::lock_guard lock{state_->endpoint_mutex};
    state_->endpoint.reset();
}

bool ClusterLifecycleService::running() const noexcept {
    return state_->running.load(std::memory_order_acquire);
}

std::optional<asio::ip::tcp::endpoint>
ClusterLifecycleService::local_endpoint() const noexcept {
    const std::lock_guard lock{state_->endpoint_mutex};
    return state_->endpoint;
}

ClusterLifecycleServiceCounters ClusterLifecycleService::counters() const noexcept {
    return ClusterLifecycleServiceCounters{
        state_->counters.accepted_connections.load(std::memory_order_relaxed),
        state_->counters.authenticated_requests.load(std::memory_order_relaxed),
        state_->counters.authentication_failures.load(std::memory_order_relaxed),
        state_->counters.malformed_requests.load(std::memory_order_relaxed),
        state_->counters.config_mismatches.load(std::memory_order_relaxed),
        state_->counters.completed_requests.load(std::memory_order_relaxed)};
}

bool valid_lifecycle_target(
    const asio::ip::tcp::endpoint& endpoint) noexcept {
    const auto& address = endpoint.address();
    return endpoint.port() != 0U && !address.is_unspecified() &&
        !address.is_multicast() &&
        (!address.is_v4() ||
         address.to_v4() != asio::ip::address_v4::broadcast());
}

std::optional<asio::ip::tcp::endpoint>
parse_lifecycle_endpoint(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::string_view address_text;
    std::string_view port_text;
    if (text.front() == '[') {
        const auto close = text.find(']');
        if (close == std::string_view::npos || close + 1U >= text.size() ||
            text[close + 1U] != ':') {
            return std::nullopt;
        }
        address_text = text.substr(1U, close - 1U);
        port_text = text.substr(close + 2U);
    } else {
        const auto colon = text.rfind(':');
        if (colon == std::string_view::npos ||
            text.find(':') != colon) {
            return std::nullopt;
        }
        address_text = text.substr(0U, colon);
        port_text = text.substr(colon + 1U);
    }
    if (address_text.empty() || port_text.empty() || port_text.size() > 5U) {
        return std::nullopt;
    }
    core::u32 port{};
    for (const char character : port_text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        port = port * 10U + static_cast<core::u32>(character - '0');
        if (port > std::numeric_limits<std::uint16_t>::max()) {
            return std::nullopt;
        }
    }
    std::error_code error;
    const auto address = asio::ip::make_address(address_text, error);
    if (error) {
        return std::nullopt;
    }
    const asio::ip::tcp::endpoint endpoint{
        address, static_cast<std::uint16_t>(port)};
    return valid_lifecycle_target(endpoint)
        ? std::optional<asio::ip::tcp::endpoint>{endpoint}
        : std::nullopt;
}

core::Result<ClusterLifecycleReply, ClusterLifecycleClientError>
request_cluster_lifecycle(const asio::ip::tcp::endpoint& endpoint,
                          core::u32 node_index,
                          ClusterLifecycleCommand command,
                          const ClusterLifecycleClientConfig& config) {
    using Result = core::Result<ClusterLifecycleReply,
                                ClusterLifecycleClientError>;
    const auto zero = std::chrono::milliseconds::zero();
    if (!valid_lifecycle_target(endpoint) ||
        !valid_secret(config.shared_secret) || config.connect_timeout <= zero ||
        config.handshake_timeout <= zero || config.write_timeout <= zero) {
        return Result::failure(
            {ClusterLifecycleClientErrorCode::InvalidConfiguration, {}});
    }
    // The socket's executor refers to this context for its entire lifetime.
    // Keep it beside the framed stream rather than returning a socket backed
    // by a context that has already been destroyed.
    asio::io_context context;
    auto connected =
        connect_with_timeout(endpoint, config.connect_timeout, context);
    if (!connected) {
        return Result::failure(connected.error());
    }
    TcpFramedStream stream{
        std::move(connected).value(),
        FramedStreamLimits{max_cluster_machine_info_message_bytes,
                           config.handshake_timeout,
                           config.handshake_timeout, config.write_timeout}};
    auto challenge = stream.receive();
    if (!challenge) {
        return Result::failure(transport_error(challenge.error()));
    }
    std::array<std::byte, nonce_bytes> nonce{};
    if (!parse_challenge(challenge.value(), nonce)) {
        return Result::failure(
            {ClusterLifecycleClientErrorCode::Protocol, {}});
    }
    static std::atomic_uint64_t request_sequence{1U};
    const core::u64 request_id =
        request_sequence.fetch_add(1U, std::memory_order_relaxed);
    const auto request = request_packet(config.shared_secret, node_index,
                                        command, request_id, nonce);
    auto sent = stream.send(request);
    if (!sent) {
        return Result::failure(transport_error(sent.error()));
    }
    auto response = stream.receive();
    if (!response) {
        return Result::failure(transport_error(response.error()));
    }
    ClusterLifecycleReply reply{};
    switch (parse_response(response.value(), config.shared_secret, request_id,
                           nonce, reply)) {
    case ResponseParse::Ok:
        return Result::success(std::move(reply));
    case ResponseParse::Authentication:
        return Result::failure(
            {ClusterLifecycleClientErrorCode::AuthenticationFailed, {}});
    case ResponseParse::Protocol:
        return Result::failure(
            {ClusterLifecycleClientErrorCode::Protocol, {}});
    }
    return Result::failure(
        {ClusterLifecycleClientErrorCode::Protocol, {}});
}

core::Result<ClusterMachineInfo, ClusterLifecycleClientError>
request_cluster_machine_info(const asio::ip::tcp::endpoint& endpoint,
                             core::u32 node_index,
                             const ClusterLifecycleClientConfig& config) {
    using Result =
        core::Result<ClusterMachineInfo, ClusterLifecycleClientError>;
    const auto zero = std::chrono::milliseconds::zero();
    if (!valid_lifecycle_target(endpoint) ||
        !valid_secret(config.shared_secret) || config.connect_timeout <= zero ||
        config.handshake_timeout <= zero || config.write_timeout <= zero) {
        return Result::failure(
            {ClusterLifecycleClientErrorCode::InvalidConfiguration, {}});
    }

    asio::io_context context;
    auto connected =
        connect_with_timeout(endpoint, config.connect_timeout, context);
    if (!connected) {
        return Result::failure(connected.error());
    }
    TcpFramedStream stream{
        std::move(connected).value(),
        FramedStreamLimits{max_cluster_machine_info_message_bytes,
                           config.handshake_timeout,
                           config.handshake_timeout, config.write_timeout}};
    auto challenge = stream.receive();
    if (!challenge) {
        return Result::failure(transport_error(challenge.error()));
    }
    std::array<std::byte, nonce_bytes> nonce{};
    if (!parse_challenge(challenge.value(), nonce)) {
        return Result::failure(
            {ClusterLifecycleClientErrorCode::Protocol, {}});
    }
    static std::atomic_uint64_t request_sequence{1U};
    const core::u64 request_id =
        request_sequence.fetch_add(1U, std::memory_order_relaxed);
    const auto request = machine_info_request_packet(
        config.shared_secret, node_index, request_id, nonce);
    auto sent = stream.send(request);
    if (!sent) {
        return Result::failure(transport_error(sent.error()));
    }
    auto response = stream.receive();
    if (!response) {
        return Result::failure(transport_error(response.error()));
    }
    ClusterMachineInfo info{};
    switch (parse_machine_info_response(response.value(), config.shared_secret,
                                        request_id, nonce, info)) {
    case ResponseParse::Ok:
        return Result::success(std::move(info));
    case ResponseParse::Authentication:
        return Result::failure(
            {ClusterLifecycleClientErrorCode::AuthenticationFailed, {}});
    case ResponseParse::Protocol:
        return Result::failure(
            {ClusterLifecycleClientErrorCode::Protocol, {}});
    }
    return Result::failure(
        {ClusterLifecycleClientErrorCode::Protocol, {}});
}

} // namespace mediaserver::net
