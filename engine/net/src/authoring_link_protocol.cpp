#include <mediaserver/net/authoring_link_protocol.hpp>
#include <algorithm>
#include <array>
#include <sstream>
#include <stdexcept>
#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <sys/random.h>
#include <cerrno>
#elif defined(__APPLE__)
#include <cstdlib>
#endif

namespace mediaserver::net {
std::string new_link_secret() {
    std::array<unsigned char, 32> data{};
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, data.data(), static_cast<ULONG>(data.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error{"OS entropy unavailable"};
#elif defined(__linux__)
    std::size_t cursor{};
    while (cursor < data.size()) {
        const auto n = getrandom(data.data()+cursor, data.size()-cursor, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error{"OS entropy unavailable"};
        cursor += static_cast<std::size_t>(n);
    }
#elif defined(__APPLE__)
    arc4random_buf(data.data(), data.size());
#else
    throw std::runtime_error{"OS entropy unavailable"};
#endif
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto c : data) { result += hex[c >> 4U]; result += hex[c & 15U]; }
    return result;
}
namespace {
using namespace std::string_view_literals;
// Order is stable so both sides can compare advertised text directly.
constexpr std::array served{"base"sv, "stage"sv, "environment"sv, "materials"sv, "animation"sv};
constexpr std::size_t capability_limit = 16;
}
std::span<const std::string_view> link_capabilities() noexcept { return served; }
bool link_capability_known(std::string_view name) noexcept {
    return std::find(served.begin(), served.end(), name) != served.end();
}
std::string link_capability_list(std::span<const std::string> names) {
    std::string out;
    for (const auto& name : names) { if (!out.empty()) out += ','; out += name; }
    return out;
}
namespace {
std::string advertised() {
    std::string out;
    for (const auto name : served) { if (!out.empty()) out += ','; out += name; }
    return out;
}
}
std::string link_welcome_payload() {
    return "version=1; live_scene; one_inflight; idle_timeout_ms=30000; capabilities=" + advertised();
}
LinkHelloResult parse_link_hello(const std::string& payload) {
    const auto fail = [](std::string reason) { return LinkHelloResult::failure(std::move(reason)); };
    LinkHelloBody body;
    std::istringstream input{payload};
    std::string line;
    if (!std::getline(input, line)) return fail("pairing_required");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    body.code = line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto split = line.find('=');
        if (split == std::string::npos) return fail("invalid_hello_declaration");
        const auto key = line.substr(0, split), value = line.substr(split+1);
        if (key == "client") {
            if (value.size() > 96) return fail("invalid_client_identity");
            body.client = value;
        } else if (key == "capabilities") {
            std::istringstream list{value};
            std::string name;
            while (std::getline(list, name, ',')) {
                if (name.empty()) continue;
                if (!link_capability_known(name))
                    return fail("unsupported_capability: " + name + "; supported=" + advertised());
                if (std::find(body.capabilities.begin(), body.capabilities.end(), name) == body.capabilities.end())
                    body.capabilities.push_back(name);
                if (body.capabilities.size() > capability_limit) return fail("capability_capacity");
            }
        }
        // Unknown keys are ignored so a newer publisher can add provenance
        // fields. Unknown capability names are refused above, by name.
    }
    // `base` is always negotiated; a declaration may omit it.
    if (std::find(body.capabilities.begin(), body.capabilities.end(), "base") == body.capabilities.end())
        body.capabilities.emplace(body.capabilities.begin(), "base");
    return LinkHelloResult::success(std::move(body));
}
namespace {
void integer(std::vector<std::byte>& out, std::uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) out.push_back(static_cast<std::byte>((value >> (8U*i)) & 255U));
}
void bytes(std::vector<std::byte>& out, const std::string& text) {
    for (const unsigned char c : text) out.push_back(static_cast<std::byte>(c));
}
}
std::vector<std::byte> encode_link_message(const LinkMessage& m) {
    std::vector<std::byte> out(4);
    bytes(out, "MSL4"); integer(out, m.version, 2); integer(out, static_cast<std::uint16_t>(m.kind), 2);
    for (const auto* s : {&m.epoch, &m.scene, &m.transaction, &m.origin, &m.source, &m.base, &m.revision, &m.resource}) {
        if (s->size() > 128) throw std::runtime_error{"Envelope string exceeds 128 bytes"};
        integer(out, s->size(), 2); bytes(out, *s);
    }
    if (m.payload.size() > link_manifest_limit) throw std::runtime_error{"Payload exceeds 1 MiB"};
    integer(out, m.offset, 8); integer(out, m.length, 8);
    integer(out, m.payload.size(), 4); bytes(out, m.payload);
    const auto size = out.size() - 4;
    for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<std::byte>((size >> (8U*i)) & 255U);
    return out;
}
LinkDecodeResult decode_link_message(std::span<const std::byte> packet) {
    try {
        if (packet.size() > link_packet_limit) throw std::runtime_error{"Packet capacity exceeded"};
        std::size_t cursor{};
        const auto read = [&](unsigned count) {
            if (count > packet.size() - cursor) throw std::runtime_error{"Truncated envelope"};
            std::uint64_t n{};
            for (unsigned i = 0; i < count; ++i) n |= static_cast<std::uint64_t>(packet[cursor++]) << (8U*i);
            return n;
        };
        const auto str = [&](std::size_t count) {
            if (count > packet.size() - cursor) throw std::runtime_error{"Truncated payload"};
            const std::string s{reinterpret_cast<const char*>(packet.data()+cursor), count}; cursor += count; return s;
        };
        if (read(4) != packet.size()-4 || str(4) != "MSL4") throw std::runtime_error{"Invalid link frame"};
        LinkMessage m;
        m.version = static_cast<std::uint16_t>(read(2));
        const auto kind = read(2);
        if (kind < 1 || kind > static_cast<unsigned>(LinkMessageKind::StageContext)) throw std::runtime_error{"Unknown message kind"};
        m.kind = static_cast<LinkMessageKind>(kind);
        for (auto* s : {&m.epoch, &m.scene, &m.transaction, &m.origin, &m.source, &m.base, &m.revision, &m.resource}) {
            const auto count = read(2);
            if (count > 128) throw std::runtime_error{"Envelope string exceeds 128 bytes"};
            *s = str(static_cast<std::size_t>(count));
            for (const unsigned char c : *s) if (c < 32 || c > 126) throw std::runtime_error{"Envelope requires printable ASCII"};
        }
        m.offset = read(8); m.length = read(8);
        const auto count = read(4);
        const auto limit = m.kind == LinkMessageKind::ResourceChunk ? link_chunk_limit
            : (m.kind == LinkMessageKind::Snapshot || m.kind == LinkMessageKind::Delta || m.kind == LinkMessageKind::ShotState ? link_manifest_limit : 4096U);
        if (count > limit) throw std::runtime_error{"Message payload capacity exceeded"};
        m.payload = str(static_cast<std::size_t>(count));
        if (cursor != packet.size()) throw std::runtime_error{"Unexpected trailing bytes"};
        return LinkDecodeResult::success(std::move(m));
    } catch (const std::exception& e) { return LinkDecodeResult::failure(e.what()); }
}
} // namespace mediaserver::net
