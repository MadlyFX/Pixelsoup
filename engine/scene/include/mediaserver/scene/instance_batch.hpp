#pragma once
#include <mediaserver/scene/linked_scene.hpp>
#include <bit>

namespace mediaserver::scene {
// One document object per prototype/group. Little-endian binary32 affine
// columns (xyz only), hex encoded for ordinary document/wire persistence.
// No per-occurrence shot identity is inferred from generator order.
inline constexpr std::string_view instance_batch_property = "model_instance_batch";
inline constexpr std::size_t instance_batch_stride = 96;
inline constexpr std::size_t max_batch_instances = 4096;
inline bool decode_instance_batch(std::string_view encoded, std::vector<Mat4>& out) {
    out.clear();
    if (encoded.empty() || encoded.size() % instance_batch_stride
        || encoded.size() / instance_batch_stride > max_batch_instances) return false;
    const auto nibble = [](char c) { return c >= '0' && c <= '9' ? c-'0'
        : c >= 'a' && c <= 'f' ? c-'a'+10 : -1; };
    out.reserve(encoded.size() / instance_batch_stride);
    for (std::size_t offset = 0; offset < encoded.size();) {
        auto m = identity();
        for (std::size_t column = 0; column < 4; ++column) for (std::size_t row = 0; row < 3; ++row) {
            core::u32 bits{};
            for (unsigned byte = 0; byte < 4; ++byte) {
                const auto a = nibble(encoded[offset++]), b = nibble(encoded[offset++]);
                if (a < 0 || b < 0) return false;
                bits |= static_cast<core::u32>(a*16+b) << (byte*8);
            }
            m.m[column*4+row] = std::bit_cast<float>(bits);
        }
        const Vec3 x{m.m[0],m.m[1],m.m[2]}, y{m.m[4],m.m[5],m.m[6]}, z{m.m[8],m.m[9],m.m[10]};
        if (!valid_link_affine(m) || dot(x,cross(y,z)) <= 0) return false;
        out.push_back(m);
    }
    return true;
}
inline std::string encode_instance_batch(std::span<const Mat4> transforms) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result; result.reserve(transforms.size()*instance_batch_stride);
    for (const auto& m : transforms) for (std::size_t c=0;c<4;++c) for (std::size_t r=0;r<3;++r) {
        const auto bits=std::bit_cast<core::u32>(m.m[c*4+r]);
        for (unsigned b=0;b<4;++b) { const auto v=(bits>>(b*8))&255U; result+=digits[v>>4]; result+=digits[v&15U]; }
    }
    return result;
}
// Render history identity only; picking and editing always address the group.
inline doc::NodeId batch_motion_identity(doc::NodeId group, const Mat4& local) {
    core::u64 hash = 1469598103934665603ULL ^ group.value();
    for (auto v : local.m) { hash ^= std::bit_cast<core::u32>(v); hash *= 1099511628211ULL; }
    return doc::NodeId{hash | (1ULL<<63)};
}
} // namespace mediaserver::scene
