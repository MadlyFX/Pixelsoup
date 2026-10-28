#include <mediaserver/render/model_report.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// CPU-only coverage of the Asset Viewer's model checks. Each case fabricates
// a package with exactly one fault and expects exactly that check, and a clean
// package expects none.
namespace {

namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": " + expression};
    }
}

#define REPORT_CHECK(condition) check((condition), #condition, __LINE__)

// A 1 m cube standing on the origin: one part, one material, UVs and tangents
// on every vertex, two 1024 px BC7 textures with their whole mip chains.
[[nodiscard]] render::ModelAsset clean_package() {
    render::ModelAsset asset{};
    for (int corner = 0; corner < 8; ++corner) {
        scene::MeshVertex vertex{};
        vertex.position = scene::Vec3{(corner & 1) != 0 ? 0.5F : -0.5F,
                                      (corner & 2) != 0 ? 1.0F : 0.0F,
                                      (corner & 4) != 0 ? 0.5F : -0.5F};
        asset.mesh.vertices.push_back(vertex);
        render::ModelVertexMetadata metadata{};
        metadata.has_uv0 = true;
        metadata.has_tangent = true;
        asset.vertex_metadata.push_back(metadata);
    }
    asset.mesh.indices = {0U, 1U, 3U, 0U, 3U, 2U, 4U, 5U, 7U, 4U, 7U, 6U};
    asset.bounds = render::ModelBounds{scene::Vec3{-0.5F, 0.0F, -0.5F},
                                       scene::Vec3{0.5F, 1.0F, 0.5F}};
    render::ModelPart part{};
    part.first_index = 0U;
    part.index_count = 12U;
    part.has_material = true;
    part.material.base_color = scene::Vec3{0.3F, 0.4F, 0.5F};
    part.base_color_texture = 0U;
    part.normal_texture = 1U;
    asset.parts.push_back(part);
    for (int index = 0; index < 2; ++index) {
        render::ModelTexture texture{};
        texture.width = 1024U;
        texture.height = 1024U;
        texture.encoding = index == 0 ? render::ModelTextureEncoding::Bc7
                                      : render::ModelTextureEncoding::Bc5;
        texture.rgba.resize(256U * 256U * 16U);
        for (core::u32 size = 512U; size >= 1U; size /= 2U) {
            render::ModelTextureMip mip{};
            mip.width = size;
            mip.height = size;
            mip.rgba.resize(static_cast<std::size_t>((size + 3U) / 4U) *
                            ((size + 3U) / 4U) * 16U);
            texture.lower_mips.push_back(std::move(mip));
        }
        asset.textures.push_back(std::move(texture));
    }
    return asset;
}

[[nodiscard]] std::vector<render::ModelCheck>
checks_of(const render::ModelAsset& asset,
          render::ModelCheckContext context = {}) {
    return render::model_asset_checks(render::model_package_facts(asset),
                                      context);
}

// Exactly one check, a warning, saying `phrase`.
void expect_one_warning(const std::vector<render::ModelCheck>& checks,
                        std::string_view phrase, int line) {
    check(checks.size() == 1U, "exactly one check", line);
    check(checks.front().level == render::ModelCheckLevel::Warning,
          "the check is a warning", line);
    check(checks.front().text.find(phrase) != std::string::npos,
          "the check names the fault", line);
}

void a_clean_package_has_no_checks_and_counts_what_it_holds() {
    const render::ModelPackageFacts facts =
        render::model_package_facts(clean_package());
    REPORT_CHECK(facts.vertices == 8U);
    REPORT_CHECK(facts.triangles == 4U);
    REPORT_CHECK(facts.parts.size() == 1U);
    REPORT_CHECK(facts.materials == 1U);
    REPORT_CHECK(facts.textures == 2U);
    REPORT_CHECK(facts.lods == 1U);
    REPORT_CHECK(facts.joints == 0U);
    REPORT_CHECK(facts.largest_texture_width == 1024U);
    // 1024 px BC7 and BC5: 1 MiB at the top level, and every lower level
    // down to 1x1 at one block (16 bytes) or more.
    core::u64 one_chain = 256U * 256U * 16U;
    for (core::u32 size = 512U; size >= 1U; size /= 2U) {
        one_chain += static_cast<core::u64>((size + 3U) / 4U) * ((size + 3U) / 4U) * 16U;
    }
    REPORT_CHECK(facts.texture_bytes == one_chain * 2U);
    const render::ModelPartFacts& part = facts.parts.front();
    REPORT_CHECK(part.triangles == 4U);
    REPORT_CHECK(part.has_material);
    REPORT_CHECK(part.material.base_color == (scene::Vec3{0.3F, 0.4F, 0.5F}));
    REPORT_CHECK(part.textures.size() == 2U);
    REPORT_CHECK(part.textures[0U].role == 0U);
    REPORT_CHECK(part.textures[0U].encoding == render::ModelTextureEncoding::Bc7);
    REPORT_CHECK(part.textures[1U].role == 2U);
    REPORT_CHECK(part.textures[1U].encoding == render::ModelTextureEncoding::Bc5);
    REPORT_CHECK(render::model_texture_role_name(2U) == "Normal");
    REPORT_CHECK(!part.missing_uv && !part.missing_tangent);
    REPORT_CHECK(checks_of(clean_package()).empty());
}

void textures_over_the_budget_warn_and_a_large_share_notes() {
    render::ModelCheckContext tight{};
    tight.texture_budget_bytes = 1024U * 1024U;
    expect_one_warning(checks_of(clean_package(), tight), "texture budget",
                       __LINE__);
    render::ModelCheckContext roomy{};
    roomy.texture_budget_bytes = 4U * 1024U * 1024U;
    const auto checks = checks_of(clean_package(), roomy);
    REPORT_CHECK(checks.size() == 1U);
    REPORT_CHECK(checks.front().level == render::ModelCheckLevel::Note);
}

void a_normal_map_without_tangents_warns() {
    render::ModelAsset asset = clean_package();
    asset.vertex_metadata[3U].has_tangent = false;
    const auto facts = render::model_package_facts(asset);
    REPORT_CHECK(facts.parts.front().missing_tangent);
    REPORT_CHECK(!facts.parts.front().missing_uv);
    expect_one_warning(checks_of(asset), "tangent", __LINE__);
}

void a_textured_part_without_uvs_warns() {
    render::ModelAsset asset = clean_package();
    asset.vertex_metadata[5U].has_uv0 = false;
    expect_one_warning(checks_of(asset), "no UVs", __LINE__);
}

void one_texel_textures_need_no_uvs_or_tangents() {
    // An importer's 1x1 stand-ins for material factors read the same at every
    // UV, so a part with only those textures does not need UVs or tangents.
    render::ModelAsset asset = clean_package();
    for (render::ModelTexture& texture : asset.textures) {
        texture.width = 1U;
        texture.height = 1U;
        texture.encoding = render::ModelTextureEncoding::Rgba8;
        texture.rgba.assign(4U, 255U);
        texture.lower_mips.clear();
    }
    for (render::ModelVertexMetadata& vertex : asset.vertex_metadata) {
        vertex.has_uv0 = false;
        vertex.has_tangent = false;
    }
    const auto facts = render::model_package_facts(asset);
    REPORT_CHECK(facts.parts.front().textures.size() == 2U);
    REPORT_CHECK(!facts.parts.front().missing_uv);
    REPORT_CHECK(!facts.parts.front().missing_tangent);
    REPORT_CHECK(checks_of(asset).empty());
}

void a_texture_above_4096_px_warns() {
    render::ModelAsset asset = clean_package();
    render::ModelTexture& texture = asset.textures[0U];
    texture.width = 8192U;
    texture.height = 4096U;
    texture.rgba.assign(2048U * 1024U * 16U, 0U);
    texture.lower_mips.clear();
    render::ModelCheckContext roomy{};
    roomy.texture_budget_bytes = 1024ULL * 1024ULL * 1024ULL;
    expect_one_warning(checks_of(asset, roomy), "8192 x 4096", __LINE__);
}

void bounds_in_the_wrong_units_warn_both_ways() {
    render::ModelAsset huge = clean_package();
    huge.bounds.minimum = scene::Vec3{-150.0F, 0.0F, -1.0F};
    huge.bounds.maximum = scene::Vec3{150.0F, 2.0F, 1.0F};
    expect_one_warning(checks_of(huge), "scaling down", __LINE__);
    render::ModelAsset tiny = clean_package();
    tiny.bounds.minimum = scene::Vec3{-0.002F, 0.0F, -0.002F};
    tiny.bounds.maximum = scene::Vec3{0.002F, 0.004F, 0.002F};
    expect_one_warning(checks_of(tiny), "scaling up", __LINE__);
}

void a_pivot_far_from_the_geometry_warns() {
    render::ModelAsset asset = clean_package();
    asset.bounds.minimum = scene::Vec3{40.0F, 0.0F, 40.0F};
    asset.bounds.maximum = scene::Vec3{41.0F, 1.0F, 41.0F};
    expect_one_warning(checks_of(asset), "pivot", __LINE__);
}

void an_animated_model_baked_static_warns_only_when_placed_so() {
    render::ModelAsset asset = clean_package();
    render::ModelAnimationClip clip{};
    clip.name = "Walk";
    clip.duration = 1.25F;
    asset.animations.push_back(clip);
    const auto facts = render::model_package_facts(asset);
    REPORT_CHECK(facts.clips.size() == 1U);
    REPORT_CHECK(facts.clips.front().name == "Walk");
    REPORT_CHECK(facts.clips.front().duration == 1.25F);
    REPORT_CHECK(checks_of(asset).empty());
    render::ModelCheckContext placed{};
    placed.static_placements = 3U;
    expect_one_warning(checks_of(asset, placed), "3 placements", __LINE__);
}

void a_projection_mesh_without_uv0_warns_and_a_model_does_not() {
    render::ModelAsset asset = clean_package();
    asset.parts.front().base_color_texture.reset();
    asset.parts.front().normal_texture.reset();
    asset.vertex_metadata[1U].has_uv0 = false;
    asset.vertex_metadata[6U].has_uv0 = false;
    REPORT_CHECK(render::model_package_facts(asset).vertices_without_uv == 2U);
    REPORT_CHECK(checks_of(asset).empty());
    render::ModelCheckContext mesh{};
    mesh.projection_mesh = true;
    expect_one_warning(checks_of(asset, mesh), "2 vertices", __LINE__);
}

void only_full_detail_parts_are_listed() {
    render::ModelAsset asset = clean_package();
    render::ModelPart coarse = asset.parts.front();
    coarse.index_count = 6U;
    asset.parts.push_back(coarse);
    asset.lods = {render::ModelLod{0U, 1U, 0.0F},
                  render::ModelLod{1U, 1U, 64.0F}};
    const auto facts = render::model_package_facts(asset);
    REPORT_CHECK(facts.parts.size() == 1U);
    REPORT_CHECK(facts.parts.front().triangles == 4U);
    REPORT_CHECK(facts.lods == 2U);
    const auto checks = render::model_asset_checks(facts, {});
    REPORT_CHECK(checks.size() == 1U);
    REPORT_CHECK(checks.front().level == render::ModelCheckLevel::Note);
}

void warnings_come_before_notes() {
    render::ModelAsset asset = clean_package();
    asset.lods = {render::ModelLod{0U, 1U, 0.0F}, render::ModelLod{0U, 1U, 8.0F}};
    asset.vertex_metadata[0U].has_tangent = false;
    const auto checks = checks_of(asset);
    REPORT_CHECK(checks.size() == 2U);
    REPORT_CHECK(checks[0U].level == render::ModelCheckLevel::Warning);
    REPORT_CHECK(checks[1U].level == render::ModelCheckLevel::Note);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 12U> cases{{
    {"a clean package has no checks and counts what it holds",
     &a_clean_package_has_no_checks_and_counts_what_it_holds},
    {"textures over the budget warn and a large share notes",
     &textures_over_the_budget_warn_and_a_large_share_notes},
    {"a normal map without tangents warns", &a_normal_map_without_tangents_warns},
    {"a textured part without UVs warns", &a_textured_part_without_uvs_warns},
    {"one-texel textures need no UVs or tangents",
     &one_texel_textures_need_no_uvs_or_tangents},
    {"a texture above 4096 px warns", &a_texture_above_4096_px_warns},
    {"bounds in the wrong units warn both ways",
     &bounds_in_the_wrong_units_warn_both_ways},
    {"a pivot far from the geometry warns", &a_pivot_far_from_the_geometry_warns},
    {"an animated model baked static warns only when placed so",
     &an_animated_model_baked_static_warns_only_when_placed_so},
    {"a projection mesh without UV0 warns and a model does not",
     &a_projection_mesh_without_uv0_warns_and_a_model_does_not},
    {"only full-detail parts are listed", &only_full_detail_parts_are_listed},
    {"warnings come before notes", &warnings_come_before_notes},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
