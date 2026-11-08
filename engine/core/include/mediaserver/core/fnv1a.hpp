#pragma once

#include <mediaserver/core/types.hpp>

#include <cstddef>
#include <span>
#include <string_view>

// 64-bit FNV-1a, for digests and stable identities that must not depend on the
// compiler, the platform or struct layout. Not a cryptographic hash: use
// core::Sha256Hasher where an adversary or a content address is involved.
//
// The helpers update a running state in place so a caller can feed values in a
// fixed, documented order. Multi-byte values go in little-endian byte order.
namespace mediaserver::core {

inline constexpr u64 fnv1a_offset = 14695981039346656037ULL;
inline constexpr u64 fnv1a_prime = 1099511628211ULL;
// The standard offset basis with its last digit missing. Several identities
// (asset, compositor, hardware and sync digests, widget ids) were first written
// with it and their values are stored or compared across machines, so it stays
// as its own named seed rather than being corrected in place.
inline constexpr u64 fnv1a_offset_short = 1469598103934665603ULL;

constexpr void fnv1a_byte(u64& state, u8 value) noexcept {
    state ^= value;
    state *= fnv1a_prime;
}

constexpr void fnv1a_bytes(u64& state, std::span<const u8> bytes) noexcept {
    for (const u8 value : bytes) {
        fnv1a_byte(state, value);
    }
}

inline void fnv1a_bytes(u64& state, const void* data, std::size_t size) noexcept {
    fnv1a_bytes(state, std::span{static_cast<const u8*>(data), size});
}

// The characters alone, with no length: for a single name.
constexpr void fnv1a_chars(u64& state, std::string_view text) noexcept {
    for (const char c : text) {
        fnv1a_byte(state, static_cast<u8>(c));
    }
}

constexpr void fnv1a_u64(u64& state, u64 value) noexcept {
    for (u32 shift = 0U; shift < 64U; shift += 8U) {
        fnv1a_byte(state, static_cast<u8>((value >> shift) & 0xffU));
    }
}

// Length first, then the characters, so adjacent strings cannot run together.
constexpr void fnv1a_text(u64& state, std::string_view text) noexcept {
    fnv1a_u64(state, static_cast<u64>(text.size()));
    fnv1a_chars(state, text);
}

[[nodiscard]] constexpr u64 fnv1a(
    std::span<const u8> bytes, u64 seed = fnv1a_offset) noexcept {
    fnv1a_bytes(seed, bytes);
    return seed;
}

} // namespace mediaserver::core
