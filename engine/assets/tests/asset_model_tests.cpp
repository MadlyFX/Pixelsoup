#include <mediaserver/assets/asset_model.hpp>

#include <mediaserver/assets/asset_schema.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace schema = mediaserver::assets::schema;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define ASSET_CHECK(...)                                                       \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] Property text(std::string_view name, std::string_view value) {
    return Property{std::string{name}, doc::PropertyValue{std::string{value}}};
}

[[nodiscard]] Property whole(std::string_view name, std::int64_t value) {
    return Property{std::string{name}, doc::PropertyValue{value}};
}

[[nodiscard]] Property number(std::string_view name, double value) {
    return Property{std::string{name}, doc::PropertyValue{value}};
}

[[nodiscard]] doc::Node node(core::u64 id, std::string_view type,
                             std::initializer_list<Property> properties) {
    doc::Node built{};
    built.id = doc::NodeId{id};
    built.type = std::string{type};
    for (const auto& property : properties) {
        built.properties.emplace(property.first, property.second);
    }
    return built;
}

[[nodiscard]] doc::Node media(core::u64 id, std::string_view name,
                              std::string_view source) {
    return node(id, schema::media_type,
                {text(schema::display_name, name), text(schema::source, source),
                 whole(schema::media_width, 3'840),
                 whole(schema::media_height, 2'160),
                 whole(schema::media_frame_count, 900),
                 whole(schema::media_rate_numerator, 24'000),
                 whole(schema::media_rate_denominator, 1'001)});
}

[[nodiscard]] doc::Node model(core::u64 id, std::string_view name,
                              std::string_view source) {
    return node(id, schema::model_type,
                {text(schema::display_name, name), text(schema::source, source),
                 whole(schema::model_triangle_count, 123'456),
                 whole(schema::model_material_count, 7),
                 number(schema::bounds_minimum_x, -2.0),
                 number(schema::bounds_minimum_y, -1.0),
                 number(schema::bounds_minimum_z, -3.0),
                 number(schema::bounds_maximum_x, 2.0),
                 number(schema::bounds_maximum_y, 5.0),
                 number(schema::bounds_maximum_z, 3.0)});
}

[[nodiscard]] doc::Node identified(
    doc::Node value, std::string_view sha256, core::u64 bytes,
    std::string_view policy = schema::policy_shared_store) {
    value.properties.insert_or_assign(
        std::string{schema::content_sha256},
        doc::PropertyValue{std::string{sha256}});
    value.properties.insert_or_assign(
        std::string{schema::content_size_bytes},
        doc::PropertyValue{static_cast<std::int64_t>(bytes)});
    value.properties.insert_or_assign(
        std::string{schema::distribution_policy},
        doc::PropertyValue{std::string{policy}});
    return value;
}

[[nodiscard]] doc::Node projection_mesh(
    core::u64 id, std::string_view name, std::string_view source) {
    return node(id, schema::projection_mesh_type,
                {text(schema::display_name, name), text(schema::source, source),
                 whole(schema::projection_vertex_count, 4'096),
                 whole(schema::projection_triangle_count, 7'938),
                 number(schema::bounds_minimum_x, -4.0),
                 number(schema::bounds_minimum_y, -2.0),
                 number(schema::bounds_minimum_z, -0.5),
                 number(schema::bounds_maximum_x, 4.0),
                 number(schema::bounds_maximum_y, 2.0),
                 number(schema::bounds_maximum_z, 0.5)});
}

[[nodiscard]] doc::Root root_of(const std::vector<doc::Node>& nodes) {
    doc::Root root;
    for (const doc::Node& one : nodes) {
        root = root.set(one);
    }
    return root;
}

[[nodiscard]] std::vector<doc::Node> demo_assets() {
    return {media(7U, "Opening Plate", "media/opening_####.exr"),
            model(11U, "Hero Car", "models/hero_car.glb")};
}

void a_document_becomes_the_asset_registry_it_describes() {
    assets::AssetModel model_view;
    model_view.reset(root_of(demo_assets()));

    ASSET_CHECK(model_view.counters().rejected == 0U);
    ASSET_CHECK(model_view.counters().media == 1U);
    ASSET_CHECK(model_view.counters().models == 1U);
    ASSET_CHECK(model_view.counters().duplicate_sources == 0U);

    const assets::AssetRegistry registry = model_view.evaluate();
    ASSET_CHECK(registry.assets.size() == 2U);
    const assets::Asset* const held_media = registry.asset(doc::NodeId{7U});
    const assets::Asset* const held_model = registry.asset(doc::NodeId{11U});
    ASSET_CHECK(held_media != nullptr && held_model != nullptr);
    ASSET_CHECK(assets::asset_kind(*held_media) == assets::AssetKind::Media);
    ASSET_CHECK(assets::asset_display_name(*held_media) == "Opening Plate");
    ASSET_CHECK(assets::asset_source(*held_model) == "models/hero_car.glb");

    const auto* const media_value = std::get_if<assets::MediaAsset>(held_media);
    const auto* const model_value = std::get_if<assets::ModelAsset>(held_model);
    ASSET_CHECK(media_value != nullptr && model_value != nullptr);
    ASSET_CHECK(media_value->id == doc::NodeId{7U});
    ASSET_CHECK(media_value->dimensions ==
                assets::PixelDimensions{3'840U, 2'160U});
    ASSET_CHECK(media_value->frame_count == 900U);
    ASSET_CHECK(media_value->frame_rate ==
                assets::MediaFrameRate{24'000U, 1'001U});
    ASSET_CHECK(model_value->triangle_count == 123'456U);
    ASSET_CHECK(model_value->material_count == 7U);
    ASSET_CHECK(model_value->bounds.has_value());
    ASSET_CHECK(model_value->bounds->minimum_z == -3.0);
    ASSET_CHECK(model_value->bounds->maximum_y == 5.0);
    ASSET_CHECK(registry.asset_for_source("models/hero_car.glb") == held_model);
}

void optional_probe_metadata_can_be_absent() {
    doc::Root root;
    // A missing display_name falls back to the exact source. This keeps sparse
    // or older asset nodes usable without synthesising a platform path name.
    root = root.set(
        node(1U, schema::media_type, {text(schema::source, "still.png")}));
    root = root.set(
        node(2U, schema::model_type, {text(schema::source, "set.obj")}));

    assets::AssetModel model_view;
    model_view.reset(root);
    ASSET_CHECK(model_view.counters().rejected == 0U);
    const auto registry = model_view.evaluate();
    ASSET_CHECK(registry.assets.size() == 2U);

    const auto* const media_value =
        std::get_if<assets::MediaAsset>(registry.asset(doc::NodeId{1U}));
    const auto* const model_value =
        std::get_if<assets::ModelAsset>(registry.asset(doc::NodeId{2U}));
    ASSET_CHECK(media_value != nullptr && model_value != nullptr);
    ASSET_CHECK(media_value->display_name == "still.png");
    ASSET_CHECK(!media_value->dimensions.has_value());
    ASSET_CHECK(!media_value->frame_count.has_value());
    ASSET_CHECK(!media_value->frame_rate.has_value());
    ASSET_CHECK(!model_value->triangle_count.has_value());
    ASSET_CHECK(!model_value->material_count.has_value());
    ASSET_CHECK(!model_value->bounds.has_value());
}

void identified_content_and_projection_meshes_form_a_portable_manifest() {
    constexpr std::string_view media_hash =
        "11111111111111111111111111111111"
        "11111111111111111111111111111111";
    constexpr std::string_view model_hash =
        "22222222222222222222222222222222"
        "22222222222222222222222222222222";
    constexpr std::string_view projection_hash =
        "33333333333333333333333333333333"
        "33333333333333333333333333333333";

    doc::Root root;
    root = root.set(identified(
        media(20U, "Program Plate", "plate.mp4"), media_hash, 4'000U));
    root = root.set(identified(
        model(21U, "Set", "models/set.glb"), model_hash, 8'000U,
        schema::policy_project_relative));
    root = root.set(identified(
        projection_mesh(22U, "LED Calibration", "wall.OBJ"),
        projection_hash, 2'000U));

    assets::AssetModel model_view;
    model_view.reset(root);
    ASSET_CHECK(model_view.counters().rejected == 0U);
    ASSET_CHECK(model_view.counters().media == 1U);
    ASSET_CHECK(model_view.counters().models == 1U);
    ASSET_CHECK(model_view.counters().projection_meshes == 1U);

    const auto manifest = model_view.evaluate();
    ASSET_CHECK(manifest.assets.size() == 3U);
    const assets::Asset* const media_asset =
        manifest.asset(doc::NodeId{20U});
    const assets::Asset* const model_asset =
        manifest.asset(doc::NodeId{21U});
    const assets::Asset* const projection_asset =
        manifest.asset(doc::NodeId{22U});
    ASSET_CHECK(media_asset != nullptr && model_asset != nullptr &&
                projection_asset != nullptr);
    ASSET_CHECK(assets::asset_kind(*media_asset) == assets::AssetKind::Media);
    ASSET_CHECK(assets::asset_kind(*model_asset) == assets::AssetKind::Model);
    ASSET_CHECK(assets::asset_kind(*projection_asset) ==
                assets::AssetKind::ProjectionMesh);
    ASSET_CHECK(assets::asset_is_portable(*media_asset));
    ASSET_CHECK(assets::asset_is_portable(*model_asset));
    ASSET_CHECK(assets::asset_is_portable(*projection_asset));

    const auto* const held_projection =
        std::get_if<assets::ModelAsset>(projection_asset);
    ASSET_CHECK(held_projection != nullptr);
    ASSET_CHECK(held_projection->projection_mesh);
    ASSET_CHECK(held_projection->vertex_count == 4'096U);
    ASSET_CHECK(held_projection->triangle_count == 7'938U);
    ASSET_CHECK(!held_projection->material_count.has_value());
    ASSET_CHECK(held_projection->bounds.has_value());

    const auto parsed_media_hash = assets::parse_sha256(media_hash);
    ASSET_CHECK(parsed_media_hash.has_value());
    const assets::ContentIdentity media_identity{*parsed_media_hash, 4'000U};
    ASSET_CHECK(assets::asset_content(*media_asset).identity == media_identity);
    ASSET_CHECK(manifest.asset_for_content(media_identity) == media_asset);
}

void compound_sources_cannot_claim_single_file_shared_store_readiness() {
    constexpr std::string_view hash =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    doc::Root root;
    root = root.set(identified(
        media(1U, "Sequence", "plates/frame_####.png"), hash, 100U));
    root = root.set(identified(
        model(2U, "Text glTF", "models/set.gltf"), hash, 200U));
    root = root.set(identified(
        model(3U, "OBJ Set", "models/set.obj"), hash, 300U));
    // Projection OBJ consumes only geometry and UV0, so this primary file is
    // complete for the output warp and remains safely portable.
    root = root.set(identified(
        projection_mesh(4U, "Warp", "calibration/wall.obj"), hash, 400U));

    assets::AssetModel model_view;
    model_view.reset(root);
    ASSET_CHECK(model_view.counters().rejected == 3U);
    ASSET_CHECK(model_view.counters().projection_meshes == 1U);
    const auto manifest = model_view.evaluate();
    ASSET_CHECK(manifest.assets.size() == 1U);
    ASSET_CHECK(assets::asset_is_portable(manifest.assets.front()));

    auto external_gltf = model(8U, "Local glTF", "models/local.gltf");
    assets::AssetModel external_model;
    external_model.reset(root_of({identified(
        std::move(external_gltf), hash, 500U, schema::policy_external)}));
    const auto external_manifest = external_model.evaluate();
    ASSET_CHECK(external_manifest.assets.size() == 1U);
    ASSET_CHECK(assets::asset_portability(external_manifest.assets.front()) ==
                assets::AssetPortability::DependencyManifestRequired);
}

void exact_source_deduplication_is_kind_scoped_and_byte_for_byte() {
    doc::Root root;
    // Each manifest role may refer to the same bytes. Only the later media
    // record is a duplicate; case and whitespace variants remain distinct.
    root = root.set(model(3U, "Scene Model", "shared/source.obj"));
    root = root.set(media(5U, "Case Variant", "shared/Source.obj"));
    root = root.set(media(7U, "Whitespace Variant", "shared/source.obj "));
    root = root.set(media(9U, "First Media", "shared/source.obj"));
    root = root.set(projection_mesh(
        10U, "Projection Mesh", "shared/source.obj"));
    root = root.set(media(12U, "Later Media", "shared/source.obj"));

    assets::AssetModel model_view;
    model_view.reset(root);
    ASSET_CHECK(model_view.counters().media == 4U);
    ASSET_CHECK(model_view.counters().models == 1U);
    ASSET_CHECK(model_view.counters().projection_meshes == 1U);
    ASSET_CHECK(model_view.counters().duplicate_sources == 1U);

    const auto registry = model_view.evaluate();
    ASSET_CHECK(registry.assets.size() == 5U);
    ASSET_CHECK(registry.asset(doc::NodeId{3U}) != nullptr);
    ASSET_CHECK(registry.asset(doc::NodeId{9U}) != nullptr);
    ASSET_CHECK(registry.asset(doc::NodeId{10U}) != nullptr);
    ASSET_CHECK(registry.asset(doc::NodeId{12U}) == nullptr);
    // The legacy source-only lookup remains deterministic by NodeId.
    ASSET_CHECK(registry.asset_for_source("shared/source.obj") ==
                registry.asset(doc::NodeId{3U}));
    ASSET_CHECK(registry.asset_for_source(
                    "shared/source.obj", assets::AssetKind::Media) ==
                registry.asset(doc::NodeId{9U}));
    ASSET_CHECK(registry.asset_for_source(
                    "shared/source.obj", assets::AssetKind::Model) ==
                registry.asset(doc::NodeId{3U}));
    ASSET_CHECK(registry.asset_for_source(
                    "shared/source.obj",
                    assets::AssetKind::ProjectionMesh) ==
                registry.asset(doc::NodeId{10U}));
    ASSET_CHECK(registry.asset_for_source("shared/Source.obj") != nullptr);
    ASSET_CHECK(registry.asset_for_source("shared/source.obj ") != nullptr);
}

void checkpoint_and_out_of_order_deltas_agree_exactly() {
    const std::vector<doc::Node> nodes = demo_assets();
    assets::AssetModel checkpointed;
    checkpointed.reset(root_of(nodes));

    assets::AssetModel streamed;
    doc::Root running;
    for (auto one = nodes.rbegin(); one != nodes.rend(); ++one) {
        const doc::Root next = running.set(*one);
        streamed.apply(diff(running, next));
        running = next;
    }

    ASSET_CHECK(assets::digest(checkpointed.evaluate()) ==
                assets::digest(streamed.evaluate()));
    ASSET_CHECK(checkpointed.counters() == streamed.counters());
}

void malformed_asset_nodes_are_rejected_not_defaulted() {
    doc::Root root;
    root = root.set(node(1U, schema::media_type,
                         {text(schema::display_name, "Missing Source")}));
    root = root.set(node(2U, schema::media_type,
                         {text(schema::display_name, ""),
                          text(schema::source, "blank-name.mov")}));
    root = root.set(node(3U, schema::media_type,
                         {text(schema::source, "partial.mov"),
                          whole(schema::media_width, 1'920)}));
    root = root.set(
        node(4U, schema::media_type,
             {text(schema::source, "zero.mov"), whole(schema::media_width, 0),
              whole(schema::media_height, 1'080)}));
    root = root.set(node(5U, schema::media_type,
                         {text(schema::source, "frames.mov"),
                          whole(schema::media_frame_count, 0)}));
    root = root.set(node(6U, schema::model_type,
                         {text(schema::source, "empty.obj"),
                          whole(schema::model_triangle_count, -2)}));
    root = root.set(node(7U, schema::model_type,
                         {text(schema::source, "materials.obj"),
                          whole(schema::model_material_count, -1)}));
    root = root.set(node(8U, schema::model_type,
                         {text(schema::source, "partial-bounds.glb"),
                          number(schema::bounds_minimum_x, -1.0)}));
    root = root.set(node(9U, schema::model_type,
                         {text(schema::source, "nan.glb"),
                          number(schema::bounds_minimum_x,
                                 std::numeric_limits<double>::quiet_NaN()),
                          number(schema::bounds_minimum_y, -1.0),
                          number(schema::bounds_minimum_z, -1.0),
                          number(schema::bounds_maximum_x, 1.0),
                          number(schema::bounds_maximum_y, 1.0),
                          number(schema::bounds_maximum_z, 1.0)}));
    root = root.set(node(10U, schema::model_type,
                         {text(schema::source, "inverted.glb"),
                          number(schema::bounds_minimum_x, 2.0),
                          number(schema::bounds_minimum_y, -1.0),
                          number(schema::bounds_minimum_z, -1.0),
                          number(schema::bounds_maximum_x, 1.0),
                          number(schema::bounds_maximum_y, 1.0),
                          number(schema::bounds_maximum_z, 1.0)}));
    root = root.set(node(11U, schema::media_type,
                         {text(schema::source, "wrong-type.mov"),
                          text(schema::media_frame_count, "many")}));
    root = root.set(node(12U, schema::media_type,
                         {text(schema::source, "partial-rate.mov"),
                          whole(schema::media_rate_numerator, 24'000)}));
    root = root.set(node(13U, schema::media_type,
                         {text(schema::source, "zero-rate.mov"),
                          whole(schema::media_rate_numerator, 24'000),
                          whole(schema::media_rate_denominator, 0)}));
    constexpr std::string_view valid_hash =
        "0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef";
    root = root.set(node(14U, schema::media_type,
                         {text(schema::source, "partial-hash.mov"),
                          text(schema::content_sha256, valid_hash)}));
    root = root.set(node(15U, schema::media_type,
                         {text(schema::source, "uppercase-hash.mov"),
                          text(schema::content_sha256,
                               "A123456789abcdef0123456789abcdef"
                               "0123456789abcdef0123456789abcdef"),
                          whole(schema::content_size_bytes, 10)}));
    root = root.set(node(16U, schema::media_type,
                         {text(schema::source, "bad-policy.mov"),
                          text(schema::distribution_policy, "copy_somehow")}));
    root = root.set(node(17U, schema::media_type,
                         {text(schema::source, "../escape.mov"),
                          text(schema::content_sha256, valid_hash),
                          whole(schema::content_size_bytes, 10),
                          text(schema::distribution_policy,
                               schema::policy_project_relative)}));
    root = root.set(node(18U, schema::media_type,
                         {text(schema::source, "unidentified.mov"),
                          text(schema::distribution_policy,
                               schema::policy_shared_store)}));
    root = root.set(node(19U, schema::media_type,
                         {text(schema::source, "no-extension"),
                          text(schema::content_sha256, valid_hash),
                          whole(schema::content_size_bytes, 10),
                          text(schema::distribution_policy,
                               schema::policy_shared_store)}));

    assets::AssetModel model_view;
    model_view.reset(root);
    ASSET_CHECK(model_view.counters().rejected == 19U);
    ASSET_CHECK(model_view.counters().media == 0U);
    ASSET_CHECK(model_view.counters().models == 0U);
    ASSET_CHECK(model_view.evaluate().assets.empty());
}

void assets_survive_without_scene_use_but_follow_their_own_nodes() {
    doc::Root root;
    root = root.set(media(1U, "Unused Plate", "unused.mov"));
    root = root.set(node(99U, "scene.object", {}));

    assets::AssetModel model_view;
    model_view.reset(root);
    ASSET_CHECK(model_view.size() == 1U);
    ASSET_CHECK(model_view.evaluate().assets.size() == 1U);

    const doc::Root retyped = root.set(node(1U, "scene.media", {}));
    model_view.apply(diff(root, retyped));
    ASSET_CHECK(model_view.size() == 0U);
    ASSET_CHECK(model_view.evaluate().assets.empty());

    assets::AssetModel erased_model;
    erased_model.reset(root);
    const auto erased = root.erase(doc::NodeId{1U});
    ASSET_CHECK(erased.has_value());
    erased_model.apply(diff(root, *erased));
    ASSET_CHECK(erased_model.evaluate().assets.empty());
}

void source_edits_recompute_deduplication_deterministically() {
    doc::Root root;
    root = root.set(media(2U, "A", "a.mov"));
    root = root.set(media(5U, "B", "b.mov"));
    assets::AssetModel model_view;
    model_view.reset(root);
    const core::u64 before = assets::digest(model_view.evaluate());

    const doc::Root duplicated = root.set(media(5U, "B", "a.mov"));
    model_view.apply(diff(root, duplicated));
    ASSET_CHECK(model_view.counters().duplicate_sources == 1U);
    ASSET_CHECK(model_view.evaluate().assets.size() == 1U);
    ASSET_CHECK(model_view.evaluate().asset(doc::NodeId{2U}) != nullptr);
    ASSET_CHECK(assets::digest(model_view.evaluate()) != before);

    const doc::Root distinct = duplicated.set(media(2U, "A", "c.mov"));
    model_view.apply(diff(duplicated, distinct));
    ASSET_CHECK(model_view.counters().duplicate_sources == 0U);
    ASSET_CHECK(model_view.evaluate().assets.size() == 2U);
}

void every_kind_of_stored_metadata_contributes_to_the_digest() {
    const std::vector<doc::Node> base_nodes = demo_assets();
    assets::AssetModel base;
    base.reset(root_of(base_nodes));
    const core::u64 reference = assets::digest(base.evaluate());

    auto changed = base_nodes;
    changed[0U].properties.insert_or_assign(
        std::string{schema::media_width},
        doc::PropertyValue{std::int64_t{3'839}});
    assets::AssetModel resized;
    resized.reset(root_of(changed));
    ASSET_CHECK(assets::digest(resized.evaluate()) != reference);

    changed = base_nodes;
    changed[0U].properties.insert_or_assign(
        std::string{schema::media_rate_denominator},
        doc::PropertyValue{std::int64_t{1'000}});
    assets::AssetModel rerated;
    rerated.reset(root_of(changed));
    ASSET_CHECK(assets::digest(rerated.evaluate()) != reference);

    changed = base_nodes;
    changed[1U].properties.insert_or_assign(
        std::string{schema::model_material_count},
        doc::PropertyValue{std::int64_t{8}});
    assets::AssetModel repartitioned;
    repartitioned.reset(root_of(changed));
    ASSET_CHECK(assets::digest(repartitioned.evaluate()) != reference);

    changed = base_nodes;
    changed[1U].properties.insert_or_assign(
        std::string{schema::bounds_maximum_z}, doc::PropertyValue{3.0001});
    assets::AssetModel bounded;
    bounded.reset(root_of(changed));
    ASSET_CHECK(assets::digest(bounded.evaluate()) != reference);

    changed = base_nodes;
    changed[1U].properties.erase(std::string{schema::bounds_maximum_z});
    // Partial metadata rejects the model rather than hashing it as absent.
    assets::AssetModel partial;
    partial.reset(root_of(changed));
    ASSET_CHECK(partial.counters().rejected == 1U);

    constexpr std::string_view hash =
        "abcdefabcdefabcdefabcdefabcdefab"
        "cdefabcdefabcdefabcdefabcdefabcd";
    changed = base_nodes;
    changed[0U] =
        identified(std::move(changed[0U]), hash, 9'999U,
                   schema::policy_external);
    assets::AssetModel identified_model;
    identified_model.reset(root_of(changed));
    ASSET_CHECK(assets::digest(identified_model.evaluate()) != reference);

    auto projection = identified(
        projection_mesh(77U, "Warp", "warp.glb"), hash, 3'000U);
    assets::AssetModel projection_model;
    projection_model.reset(root_of({projection}));
    const core::u64 projection_reference =
        assets::digest(projection_model.evaluate());
    projection.properties.insert_or_assign(
        std::string{schema::projection_vertex_count},
        doc::PropertyValue{std::int64_t{4'097}});
    assets::AssetModel reprobed_projection;
    reprobed_projection.reset(root_of({projection}));
    ASSET_CHECK(assets::digest(reprobed_projection.evaluate()) !=
                projection_reference);
}

void capacity_truncation_is_node_ordered_and_replicable() {
    doc::Root root;
    const core::u32 count = assets::max_project_assets + 8U;
    for (core::u32 index = 0U; index < count; ++index) {
        const std::string source = "asset_" + std::to_string(index) + ".mov";
        root = root.set(node(static_cast<core::u64>(index) + 1U,
                             schema::media_type,
                             {text(schema::display_name, source),
                              text(schema::source, source)}));
    }

    assets::AssetModel checkpointed;
    checkpointed.reset(root);
    ASSET_CHECK(checkpointed.counters().media == count);
    ASSET_CHECK(checkpointed.counters().over_capacity() == 8U);
    const assets::AssetRegistry registry = checkpointed.evaluate();
    ASSET_CHECK(registry.assets.size() == assets::max_project_assets);
    ASSET_CHECK(assets::asset_id(registry.assets.front()) == doc::NodeId{1U});
    ASSET_CHECK(assets::asset_id(registry.assets.back()) ==
                doc::NodeId{assets::max_project_assets});

    assets::AssetModel streamed;
    doc::Root running;
    for (core::u32 index = count; index > 0U; --index) {
        const doc::Node* const one = root.get(doc::NodeId{index});
        ASSET_CHECK(one != nullptr);
        const doc::Root next = running.set(*one);
        streamed.apply(diff(running, next));
        running = next;
    }
    ASSET_CHECK(assets::digest(streamed.evaluate()) ==
                assets::digest(registry));
    ASSET_CHECK(streamed.counters() == checkpointed.counters());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 11U> cases{{
    {"a document becomes the asset registry it describes",
     &a_document_becomes_the_asset_registry_it_describes},
    {"optional probe metadata can be absent",
     &optional_probe_metadata_can_be_absent},
    {"identified content and projection meshes form a portable manifest",
     &identified_content_and_projection_meshes_form_a_portable_manifest},
    {"compound sources require a dependency manifest",
     &compound_sources_cannot_claim_single_file_shared_store_readiness},
    {"exact source deduplication is kind scoped and byte for byte",
     &exact_source_deduplication_is_kind_scoped_and_byte_for_byte},
    {"checkpoint and out of order deltas agree exactly",
     &checkpoint_and_out_of_order_deltas_agree_exactly},
    {"malformed asset nodes are rejected not defaulted",
     &malformed_asset_nodes_are_rejected_not_defaulted},
    {"assets survive without scene use but follow their own nodes",
     &assets_survive_without_scene_use_but_follow_their_own_nodes},
    {"source edits recompute deduplication deterministically",
     &source_edits_recompute_deduplication_deterministically},
    {"every kind of stored metadata contributes to the digest",
     &every_kind_of_stored_metadata_contributes_to_the_digest},
    {"capacity truncation is node ordered and replicable",
     &capacity_truncation_is_node_ordered_and_replicable},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
