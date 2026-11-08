#pragma once

#include <mediaserver/core/types.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <span>

// SHA-256 (FIPS 180-4), shared by content fingerprints, derivative cache
// envelopes and the cluster control protocol's HMAC.
namespace mediaserver::core {

struct Sha256Digest final {
    std::array<u8, 32U> bytes{};

    bool operator==(const Sha256Digest&) const = default;
};

// Incremental SHA-256 for bounded streaming callers. finish() does not consume
// the hasher, so validation can inspect a digest without making the state
// dynamically allocated or platform-dependent.
class Sha256Hasher final {
public:
    void update(std::span<const u8> bytes) noexcept {
        for (const u8 byte : bytes) {
            buffer_[buffer_size_++] = byte;
            ++byte_length_;
            if (buffer_size_ == buffer_.size()) {
                transform(buffer_);
                buffer_size_ = 0U;
            }
        }
    }
    void update(std::span<const std::byte> bytes) noexcept {
        update(std::span{reinterpret_cast<const u8*>(bytes.data()), bytes.size()});
    }

    [[nodiscard]] Sha256Digest finish() const noexcept {
        Sha256Hasher copy = *this;
        const u64 bit_length = copy.byte_length_ * 8U;
        copy.buffer_[copy.buffer_size_++] = 0x80U;
        if (copy.buffer_size_ > 56U) {
            std::fill(copy.buffer_.begin() +
                          static_cast<std::ptrdiff_t>(copy.buffer_size_),
                      copy.buffer_.end(), u8{0U});
            copy.transform(copy.buffer_);
            copy.buffer_size_ = 0U;
        }
        std::fill(copy.buffer_.begin() +
                      static_cast<std::ptrdiff_t>(copy.buffer_size_),
                  copy.buffer_.begin() + 56, u8{0U});
        for (std::size_t index = 0U; index < 8U; ++index) {
            copy.buffer_[63U - index] = static_cast<u8>(
                (bit_length >> static_cast<u32>(index * 8U)) & 0xffU);
        }
        copy.transform(copy.buffer_);

        Sha256Digest result{};
        for (std::size_t word = 0U; word < copy.state_.size(); ++word) {
            for (std::size_t byte = 0U; byte < 4U; ++byte) {
                result.bytes[word * 4U + byte] = static_cast<u8>(
                    (copy.state_[word] >> static_cast<u32>((3U - byte) * 8U)) &
                    0xffU);
            }
        }
        return result;
    }

private:
    static constexpr std::array<u32, 64U> round_constants{{
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

    void transform(const std::array<u8, 64U>& block) noexcept {
        std::array<u32, 64U> words{};
        for (std::size_t index = 0U; index < 16U; ++index) {
            const std::size_t offset = index * 4U;
            words[index] = (static_cast<u32>(block[offset]) << 24U) |
                (static_cast<u32>(block[offset + 1U]) << 16U) |
                (static_cast<u32>(block[offset + 2U]) << 8U) |
                static_cast<u32>(block[offset + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const u32 s0 = std::rotr(words[index - 15U], 7) ^
                std::rotr(words[index - 15U], 18) ^ (words[index - 15U] >> 3U);
            const u32 s1 = std::rotr(words[index - 2U], 17) ^
                std::rotr(words[index - 2U], 19) ^ (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
        }

        u32 a = state_[0U];
        u32 b = state_[1U];
        u32 c = state_[2U];
        u32 d = state_[3U];
        u32 e = state_[4U];
        u32 f = state_[5U];
        u32 g = state_[6U];
        u32 h = state_[7U];
        for (std::size_t index = 0U; index < words.size(); ++index) {
            const u32 sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const u32 choice = (e & f) ^ ((~e) & g);
            const u32 temporary1 =
                h + sum1 + choice + round_constants[index] + words[index];
            const u32 sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const u32 majority = (a & b) ^ (a & c) ^ (b & c);
            const u32 temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
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

    std::array<u32, 8U> state_{{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    }};
    std::array<u8, 64U> buffer_{};
    std::size_t buffer_size_{};
    u64 byte_length_{};
};

[[nodiscard]] inline Sha256Digest sha256_digest(std::span<const u8> bytes) noexcept {
    Sha256Hasher hash;
    hash.update(bytes);
    return hash.finish();
}

} // namespace mediaserver::core
