#include <mediaserver/render/model_package.hpp>
#include <mediaserver/render/model_animation.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>

namespace mediaserver::render {
namespace {
constexpr core::u64 byte_limit = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr core::u64 offset_basis = 14695981039346656037ULL;
struct Archive {
    std::istream* input{};
    std::ostream* output{};
    core::u64 remaining{byte_limit};
    core::u64 hash{offset_basis};
    void bytes(void* memory, std::size_t size) {
        if (size > remaining) throw std::runtime_error{"Model package capacity exceeded"};
        if (input && !input->read(static_cast<char*>(memory), static_cast<std::streamsize>(size))) throw std::runtime_error{"Truncated model package"};
        if (output && !output->write(static_cast<const char*>(memory), static_cast<std::streamsize>(size))) throw std::runtime_error{"Cannot write model package"};
        for (std::size_t i = 0U; i < size; ++i) { hash ^= static_cast<core::u8*>(memory)[i]; hash *= 1099511628211ULL; }
        remaining -= size;
    }
    void integer(core::u32& value) {
        std::array<core::u8, 4U> encoded{};
        for (unsigned i = 0U; i < 4U; ++i) encoded[i] = static_cast<core::u8>(value >> (8U * i));
        bytes(encoded.data(), encoded.size());
        value = 0U;
        for (unsigned i = 0U; i < 4U; ++i) value |= static_cast<core::u32>(encoded[i]) << (8U * i);
    }
    void number(float& value) {
        auto bits = std::bit_cast<core::u32>(value); integer(bits); value = std::bit_cast<float>(bits);
        if (!std::isfinite(value)) throw std::runtime_error{"Nonfinite model value"};
    }
    void vector(scene::Vec3& value) { number(value.x); number(value.y); number(value.z); }
    void vector(scene::Vec4& value) { number(value.x); number(value.y); number(value.z); number(value.w); }
    void flag(bool& value) { core::u32 held = value ? 1U : 0U; integer(held); if (held > 1U) throw std::runtime_error{"Invalid model flag"}; value = held != 0U; }
    core::u32 count(std::size_t size, core::u32 maximum, core::u32 stride) {
        if (size > maximum) throw std::runtime_error{"Model package count exceeds capacity"};
        auto held = static_cast<core::u32>(size); integer(held);
        if (held > maximum || static_cast<core::u64>(held) * stride > remaining) throw std::runtime_error{"Invalid model package count"};
        return held;
    }
    void text(std::string& value) {
        const auto size = count(value.size(), 16384U, 1U); if (input) value.resize(size); bytes(value.data(), size);
    }
    void payload(std::vector<core::u8>& value) {
        const auto size = count(value.size(), static_cast<core::u32>(byte_limit), 1U);
        if (input) value.resize(size); bytes(value.data(), size);
    }
    void index(std::optional<core::u32>& value, std::size_t textures) {
        auto held = value.value_or(std::numeric_limits<core::u32>::max()); integer(held);
        if (held == std::numeric_limits<core::u32>::max()) value.reset();
        else { if (held >= textures) throw std::runtime_error{"Invalid material texture index"}; value = held; }
    }
};

void material(Archive& a, scene::Material& m) {
    a.number(m.roughness);
    a.number(m.metallic);
    a.number(m.emissive_strength);
    a.number(m.normal_scale);
    a.number(m.occlusion_strength);
    a.number(m.alpha_cutoff);
    a.number(m.base_alpha);
    a.number(m.clearcoat);
    a.number(m.clearcoat_roughness);
    a.number(m.clearcoat_normal_scale);
    a.number(m.sheen_roughness);
    a.number(m.transmission);
    a.number(m.ior);
    a.number(m.specular);
    a.number(m.anisotropy_strength);
    a.number(m.anisotropy_rotation);
    a.vector(m.base_color);
    a.vector(m.emissive);
    a.vector(m.sheen_color);
    a.vector(m.specular_color);
    a.text(m.base_color_texture);
    a.text(m.metallic_roughness_texture);
    a.text(m.normal_texture);
    a.text(m.occlusion_texture);
    a.text(m.emissive_texture);
    a.text(m.clearcoat_texture);
    a.text(m.clearcoat_roughness_texture);
    a.text(m.clearcoat_normal_texture);
    a.text(m.sheen_color_texture);
    a.text(m.sheen_roughness_texture);
    a.text(m.transmission_texture);
    a.text(m.specular_texture);
    a.text(m.specular_color_texture);
    a.text(m.anisotropy_texture);
    auto alpha = static_cast<core::u32>(m.alpha_mode); a.integer(alpha);
    if (alpha > 2U) throw std::runtime_error{"Invalid material alpha mode"};
    m.alpha_mode = static_cast<scene::MaterialAlphaMode>(alpha); a.flag(m.double_sided);
}

void contents(Archive& a, ModelAsset& asset) {
    core::u32 magic = 0x504d534dU, version = 3U;
    a.integer(magic); a.integer(version);
    if (magic != 0x504d534dU || (version != 2U && version != 3U)) throw std::runtime_error{"Unsupported model package version"};
    const auto vertices = a.count(asset.mesh.vertices.size(), 16000000U, 32U);
    if (a.input) asset.mesh.vertices.resize(vertices);
    for (auto& v : asset.mesh.vertices) { a.vector(v.position); a.vector(v.normal); a.number(v.u); a.number(v.v); }
    const auto indices = a.count(asset.mesh.indices.size(), 48000000U, 4U);
    if (indices == 0U || indices % 3U != 0U) throw std::runtime_error{"Invalid triangle count"};
    if (a.input) asset.mesh.indices.resize(indices);
    for (auto& i : asset.mesh.indices) { a.integer(i); if (i >= vertices) throw std::runtime_error{"Invalid mesh index"}; }
    const auto metadata = a.count(asset.vertex_metadata.size(), 16000000U, 24U);
    if (metadata != vertices) throw std::runtime_error{"Invalid tangent stream"};
    if (a.input) asset.vertex_metadata.resize(metadata);
    for (auto& v : asset.vertex_metadata) { a.flag(v.has_uv0); a.flag(v.has_tangent); a.vector(v.tangent); }
    const auto textures = a.count(asset.textures.size(), 16384U, 16U);
    if (a.input) asset.textures.resize(textures);
    for (auto& t : asset.textures) {
        a.integer(t.width); a.integer(t.height);
        if (t.width == 0U || t.height == 0U || t.width > 16384U || t.height > 16384U) throw std::runtime_error{"Invalid texture extent"};
        auto encoding = static_cast<core::u32>(t.encoding); a.integer(encoding);
        if (encoding > 2U) throw std::runtime_error{"Unsupported texture encoding"};
        t.encoding = static_cast<ModelTextureEncoding>(encoding);
        a.flag(t.requires_base_level);
        a.payload(t.rgba);
        if (!t.valid()) throw std::runtime_error{"Invalid texture payload"};
        const auto mips = a.count(t.lower_mips.size(), 14U, 12U);
        if (a.input) t.lower_mips.resize(mips);
        auto width = t.width, height = t.height;
        for (auto& mip : t.lower_mips) {
            if (width == 1U && height == 1U) throw std::runtime_error{"Excess texture mips"};
            width = std::max(width / 2U, 1U); height = std::max(height / 2U, 1U);
            a.integer(mip.width); a.integer(mip.height); a.payload(mip.rgba);
            const auto expected = t.encoding == ModelTextureEncoding::Rgba8
                ? static_cast<std::size_t>(width) * height * 4U
                : static_cast<std::size_t>((width + 3U) / 4U) * ((height + 3U) / 4U) * 16U;
            if (mip.width != width || mip.height != height || mip.rgba.size() != expected) throw std::runtime_error{"Invalid texture mip"};
        }
        if (width != 1U || height != 1U) throw std::runtime_error{"Incomplete texture mip chain"};
    }
    const auto parts = a.count(asset.parts.size(), 65536U, 16U);
    if (parts == 0U) throw std::runtime_error{"Missing material parts"};
    if (a.input) asset.parts.resize(parts);
    for (auto& part : asset.parts) {
        a.integer(part.first_index); a.integer(part.index_count); a.flag(part.has_material);
        if (part.first_index > indices || part.index_count > indices - part.first_index || part.index_count % 3U != 0U) throw std::runtime_error{"Invalid material range"};
        material(a, part.material);
        a.index(part.base_color_texture, textures);
        a.index(part.metallic_roughness_texture, textures);
        a.index(part.normal_texture, textures);
        a.index(part.occlusion_texture, textures);
        a.index(part.emissive_texture, textures);
        a.index(part.clearcoat_texture, textures);
        a.index(part.clearcoat_roughness_texture, textures);
        a.index(part.clearcoat_normal_texture, textures);
        a.index(part.sheen_color_texture, textures);
        a.index(part.sheen_roughness_texture, textures);
        a.index(part.transmission_texture, textures);
        a.index(part.specular_texture, textures);
        a.index(part.specular_color_texture, textures);
        a.index(part.anisotropy_texture, textures);
        for (auto& sampler : part.samplers) {
            auto u = static_cast<core::u32>(sampler.u), v = static_cast<core::u32>(sampler.v);
            auto min = static_cast<core::u32>(sampler.min), mag = static_cast<core::u32>(sampler.mag);
            auto mip = static_cast<core::u32>(sampler.mip);
            a.integer(u); a.integer(v); a.integer(min); a.integer(mag); a.integer(mip);
            if (u > 2U || v > 2U || min > 1U || mag > 1U || mip > 2U)
                throw std::runtime_error{"Invalid material sampler"};
            sampler = {static_cast<ModelAddressMode>(u), static_cast<ModelAddressMode>(v),
                static_cast<ModelFilter>(min), static_cast<ModelFilter>(mag), static_cast<ModelMipFilter>(mip)};
        }
    }
    a.vector(asset.bounds.minimum); a.vector(asset.bounds.maximum);
    const auto lods = a.count(asset.lods.size(), max_model_lods, 12U);
    if (a.input) asset.lods.resize(lods);
    float previous = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0U; i < asset.lods.size(); ++i) {
        auto& lod = asset.lods[i]; a.integer(lod.first_part); a.integer(lod.part_count); a.number(lod.max_pixels);
        if (lod.first_part > parts || lod.part_count == 0U || lod.part_count > parts - lod.first_part
            || (i != 0U && (lod.max_pixels <= 0.0F || lod.max_pixels >= previous))) throw std::runtime_error{"Invalid model LOD"};
        if (i != 0U) previous = lod.max_pixels;
    }
    if (version >= 3U) {
        const auto nodes=a.count(asset.animation_nodes.size(),max_model_animation_nodes,112U);
        if (a.input) asset.animation_nodes.resize(nodes);
        for (auto& n:asset.animation_nodes) {
            a.integer(n.parent); a.vector(n.translation); a.vector(n.rotation); a.vector(n.scale);
            a.flag(n.has_matrix); for (auto& v:n.matrix.m) a.number(v);
        }
        const auto bindings=a.count(asset.pose_bindings.size(),max_model_pose_bindings,68U);
        if (a.input) asset.pose_bindings.resize(bindings);
        for (auto& b:asset.pose_bindings) { a.integer(b.node); for (auto& v:b.inverse_bind.m) a.number(v); }
        if (nodes) for (auto& v:asset.vertex_metadata) {
            for (auto& j:v.joints) a.integer(j); a.vector(v.weights);
        }
        const auto clips=a.count(asset.animations.size(),64U,12U);
        if (a.input) asset.animations.resize(clips);
        core::u32 remaining_keys=1000000U;
        for (auto& clip:asset.animations) {
            a.text(clip.name); a.number(clip.duration);
            const auto channels=a.count(clip.channels.size(),max_model_animation_nodes*3U,20U);
            if (a.input) clip.channels.resize(channels);
            for (auto& c:clip.channels) {
                a.integer(c.node);
                auto path=static_cast<core::u32>(c.path), interpolation=static_cast<core::u32>(c.interpolation);
                a.integer(path); a.integer(interpolation);
                if (path>2 || interpolation>2) throw std::runtime_error{"Invalid animation channel"};
                c.path=static_cast<ModelAnimationPath>(path); c.interpolation=static_cast<ModelInterpolation>(interpolation);
                const auto times=a.count(c.times.size(),remaining_keys,4U);
                remaining_keys-=times;
                if (a.input) c.times.resize(times);
                for (auto& t:c.times) a.number(t);
                const auto values=a.count(c.values.size(),times*3U,16U);
                if (a.input) c.values.resize(values);
                for (auto& v:c.values) a.vector(v);
            }
        }
        if (!validate_model_animation(asset)) throw std::runtime_error{"Invalid model animation"};
        if (nodes && asset.lods.size()>1U) throw std::runtime_error{"Animated external LOD chains are unsupported"};
    }
    const auto hash = a.hash;
    auto low = static_cast<core::u32>(hash), high = static_cast<core::u32>(hash >> 32U);
    a.integer(low); a.integer(high);
    if ((static_cast<core::u64>(high) << 32U | low) != hash) throw std::runtime_error{"Model package checksum mismatch"};
}
} // namespace

bool load_model_package(const std::string& source, ModelAsset& into) {
    into = {};
    try {
        std::ifstream input{source, std::ios::binary | std::ios::ate};
        const auto size = input.tellg();
        if (size <= 0 || static_cast<core::u64>(size) > byte_limit) throw std::runtime_error{"Invalid model package size"};
        input.seekg(0);
        Archive a{&input, nullptr, static_cast<core::u64>(size)};
        contents(a, into);
        if (a.remaining != 0U) throw std::runtime_error{"Unexpected model package data"};
        return true;
    } catch (const std::exception& e) { into = {}; into.diagnostic = e.what(); return false; }
}

std::string write_model_package(const std::string& source, const ModelAsset& asset) {
    const auto temporary = source + ".ingest-" + std::to_string(std::random_device{}()) + ".tmp";
    try {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        Archive a{nullptr, &output};
        // Serialization canonicalises no authored value. Its interface is shared
        // with the reader, so write from a worker-local copy.
        auto copy = asset;
        contents(a, copy);
        output.close();
        if (!output) throw std::runtime_error{"Cannot finish model package"};
        std::ostringstream name;
        name << source << '.' << std::hex << std::setfill('0') << std::setw(16) << a.hash << ".msmodel";
        const auto destination = name.str();
        if (std::filesystem::exists(destination)) {
            ModelAsset verified;
            if (!load_model_package(destination, verified)) throw std::runtime_error{"Existing package is corrupt"};
            std::filesystem::remove(temporary);
        } else { std::filesystem::rename(temporary, destination); }
        return destination;
    } catch (...) { std::error_code ignored; std::filesystem::remove(temporary, ignored); throw; }
}

ModelIngestResult ingest_model(const std::string& source) {
    ModelIngestResult result;
    try {
        ModelAsset asset;
        if (!load_model(source, asset)) { result.diagnostic = asset.diagnostic; return result; }
        if (!compress_model_textures(asset)) { result.diagnostic = "Cannot prepare model textures"; return result; }
        result.source = std::filesystem::path{source}.extension() == ".msmodel" ? source : write_model_package(source, asset);
        result.vertex_count = asset.mesh.vertices.size();
        result.triangle_count = asset.mesh.indices.size() / 3U;
        result.material_count = static_cast<core::u32>(asset.parts.size());
        result.bounds = asset.bounds;
    } catch (const std::exception& e) { result.diagnostic = e.what(); }
    return result;
}
} // namespace mediaserver::render
