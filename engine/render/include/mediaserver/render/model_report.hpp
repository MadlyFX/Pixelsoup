#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/render/model_loader.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What an imported model package holds, and what about it an operator should
// look at before placing it: the Asset Viewer's Checks list.
//
// Two halves. model_package_facts walks every index of the package, which for
// a large scan is millions of reads, so it runs on a worker, once per decoded
// package. model_asset_checks is cheap and pure over those facts and the
// project, so it runs wherever it is shown. Neither changes the package or
// anything authored.
namespace mediaserver::render {

// A material texture slot's name, in ModelPart's role order: base colour,
// metal/roughness, normal, occlusion, emissive, then the nine extension
// lobes. Empty past the last role.
[[nodiscard]] std::string_view model_texture_role_name(std::size_t role) noexcept;

struct ModelPartTexture final {
    // Index into the roles named by model_texture_role_name.
    core::u32 role{};
    core::u32 width{};
    core::u32 height{};
    ModelTextureEncoding encoding{ModelTextureEncoding::Rgba8};

    bool operator==(const ModelPartTexture&) const = default;
};

// One full-detail part: a material run of the package.
struct ModelPartFacts final {
    core::u64 triangles{};
    scene::Material material{};
    // The source named a material for it; otherwise the placement's authored
    // fallback colours it.
    bool has_material{};
    std::vector<ModelPartTexture> textures;
    // It samples a texture but some of its vertices carry no UV0.
    bool missing_uv{};
    // It has a normal map but some of its vertices carry no tangent frame.
    bool missing_tangent{};

    bool operator==(const ModelPartFacts&) const = default;
};

struct ModelClipFacts final {
    std::string name;
    float duration{};

    bool operator==(const ModelClipFacts&) const = default;
};

struct ModelPackageFacts final {
    core::u64 vertices{};
    core::u64 triangles{};
    // Parts are counted at full detail; a LOD chain's lower levels are not.
    std::vector<ModelPartFacts> parts;
    core::u32 materials{};
    core::u32 textures{};
    core::u32 lods{1U};
    core::u32 joints{};
    std::vector<ModelClipFacts> clips;
    ModelBounds bounds{};
    // Every texture resident at full resolution with its whole mip chain, in
    // its prepared encoding: what it costs before streaming lowers anything.
    core::u64 texture_bytes{};
    core::u32 largest_texture_width{};
    core::u32 largest_texture_height{};
    core::u32 textures_over_4096{};
    // Vertices without UV0, which a projection mesh cannot map.
    core::u64 vertices_without_uv{};

    bool operator==(const ModelPackageFacts&) const = default;
};

// Worker-side: reads every part's indices once.
[[nodiscard]] ModelPackageFacts model_package_facts(const ModelAsset& asset);

enum class ModelCheckLevel : core::u8 {
    // Worth knowing; nothing is wrong.
    Note,
    // Likely to look or behave wrong once placed.
    Warning,
};

struct ModelCheck final {
    ModelCheckLevel level{ModelCheckLevel::Note};
    std::string text;

    bool operator==(const ModelCheck&) const = default;
};

struct ModelCheckContext final {
    // The project's model texture budget (Project Settings > Geometry).
    core::u64 texture_budget_bytes{512ULL * 1024ULL * 1024ULL};
    // The asset is a projection mesh, whose UV0 is its calibration mapping.
    bool projection_mesh{};
    // Placements of this asset in the Stage scene still set to Bake
    // Geometry: Static.
    core::u32 static_placements{};

    bool operator==(const ModelCheckContext&) const = default;
};

// Largest dimensions beyond which a model is probably in the wrong units:
// over 100 m, or under 1 cm, across its largest side.
inline constexpr float model_units_too_large_m = 100.0F;
inline constexpr float model_units_too_small_m = 0.01F;
inline constexpr core::u32 model_texture_size_warning = 4096U;

// Every check that fires, warnings first, each once. Empty for a clean model.
[[nodiscard]] std::vector<ModelCheck>
model_asset_checks(const ModelPackageFacts& facts,
                   const ModelCheckContext& context);

} // namespace mediaserver::render
