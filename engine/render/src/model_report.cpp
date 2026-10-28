#include <mediaserver/render/model_report.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace mediaserver::render {
namespace {

constexpr std::array<std::string_view, model_material_texture_count> role_names{
    "Base colour",     "Metal / rough",  "Normal",
    "Occlusion",       "Emissive",       "Clearcoat",
    "Clearcoat rough", "Clearcoat normal", "Sheen colour",
    "Sheen rough",     "Transmission",   "Specular",
    "Specular colour", "Anisotropy"};

// The part's texture indices in role order.
[[nodiscard]] std::array<std::optional<core::u32>, model_material_texture_count>
part_textures(const ModelPart& part) noexcept {
    return {part.base_color_texture,         part.metallic_roughness_texture,
            part.normal_texture,             part.occlusion_texture,
            part.emissive_texture,           part.clearcoat_texture,
            part.clearcoat_roughness_texture, part.clearcoat_normal_texture,
            part.sheen_color_texture,        part.sheen_roughness_texture,
            part.transmission_texture,       part.specular_texture,
            part.specular_color_texture,     part.anisotropy_texture};
}

[[nodiscard]] core::u64 texture_bytes(const ModelTexture& texture) noexcept {
    core::u64 bytes = texture.rgba.size();
    if (texture.lower_mips.empty()) {
        // Not yet filtered: the chain the residency path builds adds a third.
        return bytes + bytes / 3U;
    }
    for (const ModelTextureMip& mip : texture.lower_mips) {
        bytes += mip.rgba.size();
    }
    return bytes;
}

[[nodiscard]] std::string mebibytes(core::u64 bytes) {
    std::array<char, 32U> text{};
    std::snprintf(text.data(), text.size(), "%.0f MiB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text.data();
}

[[nodiscard]] std::string metres(float value) {
    std::array<char, 32U> text{};
    if (value >= 1.0F) {
        std::snprintf(text.data(), text.size(), "%.1f m",
                      static_cast<double>(value));
    } else {
        std::snprintf(text.data(), text.size(), "%.1f mm",
                      static_cast<double>(value) * 1000.0);
    }
    return text.data();
}

} // namespace

std::string_view model_texture_role_name(std::size_t role) noexcept {
    return role < role_names.size() ? role_names[role] : std::string_view{};
}

ModelPackageFacts model_package_facts(const ModelAsset& asset) {
    ModelPackageFacts facts{};
    facts.vertices = asset.mesh.vertices.size();
    facts.bounds = asset.bounds;
    facts.lods = static_cast<core::u32>(std::max<std::size_t>(asset.lods.size(), 1U));
    facts.joints = static_cast<core::u32>(asset.pose_bindings.size());
    for (const ModelAnimationClip& clip : asset.animations) {
        facts.clips.push_back(ModelClipFacts{clip.name, clip.duration});
    }
    for (const ModelTexture& texture : asset.textures) {
        if (!texture.valid()) {
            continue;
        }
        ++facts.textures;
        facts.texture_bytes += texture_bytes(texture);
        if (static_cast<core::u64>(texture.width) * texture.height >
            static_cast<core::u64>(facts.largest_texture_width) *
                facts.largest_texture_height) {
            facts.largest_texture_width = texture.width;
            facts.largest_texture_height = texture.height;
        }
        if (std::max(texture.width, texture.height) >
            model_texture_size_warning) {
            ++facts.textures_over_4096;
        }
    }
    for (const ModelVertexMetadata& vertex : asset.vertex_metadata) {
        facts.vertices_without_uv += vertex.has_uv0 ? 0U : 1U;
    }

    const std::size_t full_detail =
        asset.lods.empty()
            ? asset.parts.size()
            : std::min<std::size_t>(asset.lods.front().first_part +
                                        asset.lods.front().part_count,
                                    asset.parts.size());
    const std::size_t first =
        asset.lods.empty() ? 0U
                           : std::min<std::size_t>(asset.lods.front().first_part,
                                                   full_detail);
    for (std::size_t index = first; index < full_detail; ++index) {
        const ModelPart& part = asset.parts[index];
        ModelPartFacts row{};
        row.triangles = part.index_count / 3U;
        row.material = part.material;
        row.has_material = part.has_material;
        facts.materials += part.has_material ? 1U : 0U;
        // Only a texture larger than one texel depends on where it is read:
        // an importer's 1x1 stand-in for a factor reads the same everywhere,
        // so a part that samples nothing else needs no UVs or tangents.
        const auto textures = part_textures(part);
        bool samples = false;
        bool normal_mapped = false;
        for (std::size_t role = 0U; role < textures.size(); ++role) {
            if (!textures[role].has_value() ||
                *textures[role] >= asset.textures.size() ||
                !asset.textures[*textures[role]].valid()) {
                continue;
            }
            const ModelTexture& texture = asset.textures[*textures[role]];
            row.textures.push_back(ModelPartTexture{
                static_cast<core::u32>(role), texture.width, texture.height,
                texture.encoding});
            const bool varies = texture.width > 1U || texture.height > 1U;
            samples = samples || varies;
            // Normal and clearcoat normal, in role order.
            normal_mapped = normal_mapped || (varies && (role == 2U || role == 7U));
        }
        const std::size_t end = std::min<std::size_t>(
            static_cast<std::size_t>(part.first_index) + part.index_count,
            asset.mesh.indices.size());
        for (std::size_t i = part.first_index;
             i < end && (samples || normal_mapped); ++i) {
            const core::u32 vertex = asset.mesh.indices[i];
            if (vertex >= asset.vertex_metadata.size()) {
                continue;
            }
            const ModelVertexMetadata& metadata = asset.vertex_metadata[vertex];
            row.missing_uv = row.missing_uv || (samples && !metadata.has_uv0);
            row.missing_tangent =
                row.missing_tangent || (normal_mapped && !metadata.has_tangent);
            if ((row.missing_uv || !samples) &&
                (row.missing_tangent || !normal_mapped)) {
                break;
            }
        }
        facts.triangles += row.triangles;
        facts.parts.push_back(std::move(row));
    }
    return facts;
}

std::vector<ModelCheck> model_asset_checks(const ModelPackageFacts& facts,
                                           const ModelCheckContext& context) {
    std::vector<ModelCheck> warnings;
    std::vector<ModelCheck> notes;
    const auto warn = [&](std::string text) {
        warnings.push_back(ModelCheck{ModelCheckLevel::Warning, std::move(text)});
    };
    const auto note = [&](std::string text) {
        notes.push_back(ModelCheck{ModelCheckLevel::Note, std::move(text)});
    };

    if (facts.texture_bytes > context.texture_budget_bytes) {
        warn("Its textures need " + mebibytes(facts.texture_bytes) +
             " at full resolution, over the project's " +
             mebibytes(context.texture_budget_bytes) +
             " texture budget, so they stream at lower detail");
    } else if (facts.texture_bytes * 4U > context.texture_budget_bytes &&
               context.texture_budget_bytes != 0U) {
        note("Its textures need " + mebibytes(facts.texture_bytes) + " of the "
             "project's " + mebibytes(context.texture_budget_bytes) +
             " texture budget");
    }

    for (std::size_t index = 0U; index < facts.parts.size(); ++index) {
        const ModelPartFacts& part = facts.parts[index];
        const std::string name = "Part " + std::to_string(index + 1U);
        if (part.missing_tangent) {
            warn(name + " has a normal map but no tangent frame on some "
                        "vertices, so the map cannot apply there");
        }
        if (part.missing_uv) {
            warn(name + " samples textures but has no UVs on some vertices");
        }
    }

    if (facts.textures_over_4096 != 0U) {
        warn(std::to_string(facts.textures_over_4096) +
             (facts.textures_over_4096 == 1U ? " texture is" : " textures are") +
             " larger than 4096 px (the largest is " +
             std::to_string(facts.largest_texture_width) + " x " +
             std::to_string(facts.largest_texture_height) + ")");
    }

    const scene::Vec3 size = facts.bounds.maximum - facts.bounds.minimum;
    const float largest = std::max({size.x, size.y, size.z});
    if (std::isfinite(largest) && largest > model_units_too_large_m) {
        warn("It is " + metres(largest) +
             " across. If it was authored in centimetres or millimetres, it "
             "needs scaling down");
    } else if (std::isfinite(largest) && largest > 0.0F &&
               largest < model_units_too_small_m) {
        warn("It is " + metres(largest) +
             " across. If it was authored in metres where centimetres were "
             "meant, it needs scaling up");
    }

    // How far the origin, its pivot, lies outside its own box.
    const auto outside = [](float value, float low, float high) {
        return value < low ? low - value : value > high ? value - high : 0.0F;
    };
    const float dx = outside(0.0F, facts.bounds.minimum.x, facts.bounds.maximum.x);
    const float dy = outside(0.0F, facts.bounds.minimum.y, facts.bounds.maximum.y);
    const float dz = outside(0.0F, facts.bounds.minimum.z, facts.bounds.maximum.z);
    const float pivot_gap = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (std::isfinite(pivot_gap) && std::isfinite(largest) &&
        pivot_gap > std::max(largest, 0.5F)) {
        warn("Its pivot is " + metres(pivot_gap) +
             " outside the geometry, so it rotates and scales about a distant "
             "point");
    }

    const bool animated = !facts.clips.empty() || facts.joints != 0U;
    if (animated && context.static_placements != 0U) {
        warn(std::to_string(context.static_placements) +
             (context.static_placements == 1U ? " placement" : " placements") +
             " on the Stage animate this model but are still set to Bake "
             "Geometry: Static");
    }

    if (context.projection_mesh && facts.vertices_without_uv != 0U) {
        warn(std::to_string(facts.vertices_without_uv) +
             " vertices have no UV0, so the projection cannot map them");
    }

    if (facts.lods > 1U) {
        note("It has " + std::to_string(facts.lods) +
             " levels of detail; the parts listed are the full-detail ones");
    }

    warnings.insert(warnings.end(), notes.begin(), notes.end());
    return warnings;
}

} // namespace mediaserver::render
