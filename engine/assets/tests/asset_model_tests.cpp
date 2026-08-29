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

// Asset sources are absolute paths. These tests author them under one fixed
// root so the interesting part of each name - case, spacing, extension - stays
// readable at the call site.
[[nodiscard]] std::string src(std::string_view relative) {
#if defined(_WIN32)
    return std::string{"D:\\show\\"} + std::string{relative};
#else
    return std::string{"/show/"} + std::string{relative};
#endif
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
    return {media(7U, "Opening Plate", src("media/opening_####.exr")),
            model(11U, "Hero Car", src("models/hero_car.glb"))};
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
    ASSET_CHECK(assets::asset_source(*held_model) == src("models/hero_car.glb"));

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
    ASSET_CHECK(registry.asset_for_source(src("models/hero_car.glb")) ==
                held_model);
}

void optional_probe_metadata_can_be_absent() {
    doc::Root root;
    // A missing display_name falls back to the exact source. This keeps sparse
    // or older asset nodes usable without synthesising a platform path name.
    root = root.set(
        node(1U, schema::media_type, {text(schema::source, src("still.png"))}));
    root = root.set(
        node(2U, schema::model_type, {text(schema::source, src("set.obj"))}));

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
    ASSET_CHECK(media_value->display_name == src("still.png"));
    ASSET_CHECK(!media_value->dimensions.has_value());
    ASSET_CHECK(!media_value->frame_count.has_value());
    ASSET_CHECK(!media_value->frame_rate.has_value());
    ASSET_CHECK(!model_value->triangle_count.has_value());
    ASSET_CHECK(!model_value->material_count.has_value());
    ASSET_CHECK(!model_value->bounds.has_value());
}

void exact_source_deduplication_is_kind_scoped_and_byte_for_byte() {
    doc::Root root;
    // Each manifest role may refer to the same bytes. Only the later media
    // record is a duplicate; case and whitespace variants remain distinct.
    root = root.set(model(3U, "Scene Model", src("shared/source.obj")));
    root = root.set(media(5U, "Case Variant", src("shared/Source.obj")));
    root = root.set(media(7U, "Whitespace Variant", src("shared/source.obj ")));
    root = root.set(media(9U, "First Media", src("shared/source.obj")));
    root = root.set(projection_mesh(
        10U, "Projection Mesh", src("shared/source.obj")));
    root = root.set(media(12U, "Later Media", src("shared/source.obj")));

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
    ASSET_CHECK(registry.asset_for_source(src("shared/source.obj")) ==
                registry.asset(doc::NodeId{3U}));
    ASSET_CHECK(registry.asset_for_source(src("shared/source.obj"),
                                          assets::AssetKind::Media) ==
                registry.asset(doc::NodeId{9U}));
    ASSET_CHECK(registry.asset_for_source(src("shared/source.obj"),
                                          assets::AssetKind::Model) ==
                registry.asset(doc::NodeId{3U}));
    ASSET_CHECK(registry.asset_for_source(src("shared/source.obj"),
                    assets::AssetKind::ProjectionMesh) ==
                registry.asset(doc::NodeId{10U}));
    ASSET_CHECK(registry.asset_for_source(src("shared/Source.obj")) != nullptr);
    ASSET_CHECK(registry.asset_for_source(src("shared/source.obj ")) != nullptr);
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
                          text(schema::source, src("blank-name.mov"))}));
    root = root.set(node(3U, schema::media_type,
                         {text(schema::source, src("partial.mov")),
                          whole(schema::media_width, 1'920)}));
    root = root.set(
        node(4U, schema::media_type,
             {text(schema::source, src("zero.mov")), whole(schema::media_width, 0),
              whole(schema::media_height, 1'080)}));
    root = root.set(node(5U, schema::media_type,
                         {text(schema::source, src("frames.mov")),
                          whole(schema::media_frame_count, 0)}));
    root = root.set(node(6U, schema::model_type,
                         {text(schema::source, src("empty.obj")),
                          whole(schema::model_triangle_count, -2)}));
    root = root.set(node(7U, schema::model_type,
                         {text(schema::source, src("materials.obj")),
                          whole(schema::model_material_count, -1)}));
    root = root.set(node(8U, schema::model_type,
                         {text(schema::source, src("partial-bounds.glb")),
                          number(schema::bounds_minimum_x, -1.0)}));
    root = root.set(node(9U, schema::model_type,
                         {text(schema::source, src("nan.glb")),
                          number(schema::bounds_minimum_x,
                                 std::numeric_limits<double>::quiet_NaN()),
                          number(schema::bounds_minimum_y, -1.0),
                          number(schema::bounds_minimum_z, -1.0),
                          number(schema::bounds_maximum_x, 1.0),
                          number(schema::bounds_maximum_y, 1.0),
                          number(schema::bounds_maximum_z, 1.0)}));
    root = root.set(node(10U, schema::model_type,
                         {text(schema::source, src("inverted.glb")),
                          number(schema::bounds_minimum_x, 2.0),
                          number(schema::bounds_minimum_y, -1.0),
                          number(schema::bounds_minimum_z, -1.0),
                          number(schema::bounds_maximum_x, 1.0),
                          number(schema::bounds_maximum_y, 1.0),
                          number(schema::bounds_maximum_z, 1.0)}));
    root = root.set(node(11U, schema::media_type,
                         {text(schema::source, src("wrong-type.mov")),
                          text(schema::media_frame_count, "many")}));
    root = root.set(node(12U, schema::media_type,
                         {text(schema::source, src("partial-rate.mov")),
                          whole(schema::media_rate_numerator, 24'000)}));
    root = root.set(node(13U, schema::media_type,
                         {text(schema::source, src("zero-rate.mov")),
                          whole(schema::media_rate_numerator, 24'000),
                          whole(schema::media_rate_denominator, 0)}));
    // A source that is not an absolute path could name a different file on
    // the Director than on a render node, so it is refused at the model.
    root = root.set(node(14U, schema::media_type,
                         {text(schema::source, "clip.mov")}));
    root = root.set(node(15U, schema::media_type,
                         {text(schema::source, "media/clip.mov")}));
    root = root.set(node(16U, schema::media_type,
                         {text(schema::source, "../escape.mov")}));
    root = root.set(node(17U, schema::media_type,
                         {text(schema::source, src("..\\escape.mov"))}));

    assets::AssetModel model_view;
    model_view.reset(root);
    ASSET_CHECK(model_view.counters().rejected == 17U);
    ASSET_CHECK(model_view.counters().media == 0U);
    ASSET_CHECK(model_view.counters().models == 0U);
    ASSET_CHECK(model_view.evaluate().assets.empty());
}

void assets_survive_without_scene_use_but_follow_their_own_nodes() {
    doc::Root root;
    root = root.set(media(1U, "Unused Plate", src("unused.mov")));
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
    root = root.set(media(2U, "A", src("a.mov")));
    root = root.set(media(5U, "B", src("b.mov")));
    assets::AssetModel model_view;
    model_view.reset(root);
    const core::u64 before = assets::digest(model_view.evaluate());

    const doc::Root duplicated = root.set(media(5U, "B", src("a.mov")));
    model_view.apply(diff(root, duplicated));
    ASSET_CHECK(model_view.counters().duplicate_sources == 1U);
    ASSET_CHECK(model_view.evaluate().assets.size() == 1U);
    ASSET_CHECK(model_view.evaluate().asset(doc::NodeId{2U}) != nullptr);
    ASSET_CHECK(assets::digest(model_view.evaluate()) != before);

    const doc::Root distinct = duplicated.set(media(2U, "A", src("c.mov")));
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

    changed = base_nodes;
    changed[0U].properties.insert_or_assign(
        std::string{schema::source}, doc::PropertyValue{src("renamed.exr")});
    assets::AssetModel resourced;
    resourced.reset(root_of(changed));
    ASSET_CHECK(assets::digest(resourced.evaluate()) != reference);

    auto projection = projection_mesh(77U, "Warp", src("warp.glb"));
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

void fingerprint_metadata_is_paired_parsed_and_digested() {
    constexpr std::string_view abc_sha256 =
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad";
    doc::Node fingerprinted = media(91U, "Revision", src("revision.mov"));
    fingerprinted.properties.emplace(
        std::string{schema::expected_byte_length},
        doc::PropertyValue{std::int64_t{3}});
    fingerprinted.properties.emplace(
        std::string{schema::expected_sha256},
        doc::PropertyValue{std::string{abc_sha256}});

    assets::AssetModel model_view;
    model_view.reset(root_of({fingerprinted}));
    const assets::AssetRegistry registry = model_view.evaluate();
    ASSET_CHECK(model_view.counters().rejected == 0U);
    const assets::Asset* const parsed = registry.asset(doc::NodeId{91U});
    ASSET_CHECK(parsed != nullptr);
    ASSET_CHECK(assets::asset_fingerprint(*parsed).has_value());
    ASSET_CHECK(assets::asset_fingerprint(*parsed)->byte_length == 3U);
    ASSET_CHECK(assets::sha256_hex(
                    assets::asset_fingerprint(*parsed)->sha256) == abc_sha256);
    const core::u64 reference = assets::digest(registry);

    doc::Node changed = fingerprinted;
    std::string other_sha256{abc_sha256};
    other_sha256[0U] = 'c';
    changed.properties.insert_or_assign(
        std::string{schema::expected_sha256},
        doc::PropertyValue{std::move(other_sha256)});
    assets::AssetModel changed_model;
    changed_model.reset(root_of({changed}));
    ASSET_CHECK(assets::digest(changed_model.evaluate()) != reference);

    doc::Node partial = fingerprinted;
    partial.properties.erase(std::string{schema::expected_sha256});
    assets::AssetModel partial_model;
    partial_model.reset(root_of({partial}));
    ASSET_CHECK(partial_model.counters().rejected == 1U);
    ASSET_CHECK(partial_model.evaluate().assets.empty());

    doc::Node malformed = fingerprinted;
    malformed.properties.insert_or_assign(
        std::string{schema::expected_sha256},
        doc::PropertyValue{std::string{"not-a-sha256"}});
    assets::AssetModel malformed_model;
    malformed_model.reset(root_of({malformed}));
    ASSET_CHECK(malformed_model.counters().rejected == 1U);
}

void proxy_preferences_are_typed_validated_defaulted_and_digested() {
    doc::Node imported = media(92U, "Imported", src("imported.mov"));
    assets::AssetModel imported_model;
    imported_model.reset(root_of({imported}));
    const assets::AssetRegistry imported_registry = imported_model.evaluate();
    const auto* const imported_asset = std::get_if<assets::MediaAsset>(
        imported_registry.asset(doc::NodeId{92U}));
    ASSET_CHECK(imported_asset != nullptr);
    ASSET_CHECK(!imported_asset->requested_proxy_profile.has_value());
    ASSET_CHECK(imported_asset->proxy_playback_policy ==
                assets::ProxyPlaybackPolicy::Source);
    const core::u64 imported_digest = assets::digest(imported_registry);

    doc::Node preferred = imported;
    preferred.properties.insert_or_assign(
        std::string{schema::requested_proxy_profile},
        doc::PropertyValue{std::string{"proxy-720p-cfr-h264"}});
    preferred.properties.insert_or_assign(
        std::string{schema::proxy_playback_policy},
        doc::PropertyValue{std::string{"prefer_proxy"}});
    assets::AssetModel preferred_model;
    preferred_model.reset(root_of({preferred}));
    const auto preferred_registry = preferred_model.evaluate();
    const auto* const preferred_asset = std::get_if<assets::MediaAsset>(
        preferred_registry.asset(doc::NodeId{92U}));
    ASSET_CHECK(preferred_model.counters().rejected == 0U);
    ASSET_CHECK(preferred_asset != nullptr);
    ASSET_CHECK(preferred_asset->requested_proxy_profile ==
                assets::DerivativeProfileId::Proxy720pCfrH264);
    ASSET_CHECK(preferred_asset->proxy_playback_policy ==
                assets::ProxyPlaybackPolicy::PreferProxy);
    ASSET_CHECK(assets::digest(preferred_registry) != imported_digest);

    doc::Node required = preferred;
    required.properties.insert_or_assign(
        std::string{schema::proxy_playback_policy},
        doc::PropertyValue{std::string{"require_proxy"}});
    assets::AssetModel required_model;
    required_model.reset(root_of({required}));
    ASSET_CHECK(required_model.counters().rejected == 0U);
    ASSET_CHECK(assets::digest(required_model.evaluate()) !=
                assets::digest(preferred_registry));

    doc::Node source_with_request = preferred;
    source_with_request.properties.insert_or_assign(
        std::string{schema::proxy_playback_policy},
        doc::PropertyValue{std::string{"source"}});
    assets::AssetModel source_with_request_model;
    source_with_request_model.reset(root_of({source_with_request}));
    ASSET_CHECK(source_with_request_model.counters().rejected == 0U);

    doc::Node unknown_profile = preferred;
    unknown_profile.properties.insert_or_assign(
        std::string{schema::requested_proxy_profile},
        doc::PropertyValue{std::string{"proxy-future"}});
    doc::Node non_proxy_profile = preferred;
    non_proxy_profile.id = doc::NodeId{93U};
    non_proxy_profile.properties.insert_or_assign(
        std::string{schema::requested_proxy_profile},
        doc::PropertyValue{std::string{"thumb-320x180-srgb"}});
    doc::Node unknown_policy = preferred;
    unknown_policy.id = doc::NodeId{94U};
    unknown_policy.properties.insert_or_assign(
        std::string{schema::proxy_playback_policy},
        doc::PropertyValue{std::string{"automatic"}});
    doc::Node wrong_type = preferred;
    wrong_type.id = doc::NodeId{95U};
    wrong_type.properties.insert_or_assign(
        std::string{schema::proxy_playback_policy},
        doc::PropertyValue{std::int64_t{1}});
    doc::Node unconfigured_prefer = imported;
    unconfigured_prefer.id = doc::NodeId{96U};
    unconfigured_prefer.properties.insert_or_assign(
        std::string{schema::proxy_playback_policy},
        doc::PropertyValue{std::string{"prefer_proxy"}});

    assets::AssetModel malformed_model;
    malformed_model.reset(root_of({unknown_profile, non_proxy_profile,
                                   unknown_policy, wrong_type}));
    ASSET_CHECK(malformed_model.counters().rejected == 4U);
    ASSET_CHECK(malformed_model.evaluate().assets.empty());

    assets::AssetModel unconfigured_model;
    unconfigured_model.reset(root_of({unconfigured_prefer}));
    ASSET_CHECK(unconfigured_model.counters().rejected == 0U);
    const auto unconfigured_registry = unconfigured_model.evaluate();
    const auto* const unconfigured_asset = std::get_if<assets::MediaAsset>(
        unconfigured_registry.asset(doc::NodeId{96U}));
    ASSET_CHECK(unconfigured_asset != nullptr);
    ASSET_CHECK(!unconfigured_asset->requested_proxy_profile.has_value());
    ASSET_CHECK(unconfigured_asset->proxy_playback_policy ==
                assets::ProxyPlaybackPolicy::PreferProxy);
}

void capacity_truncation_is_node_ordered_and_replicable() {
    doc::Root root;
    const core::u32 count = assets::max_project_assets + 8U;
    for (core::u32 index = 0U; index < count; ++index) {
        const std::string source = src("asset_" + std::to_string(index) + ".mov");
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
    {"fingerprint metadata is paired parsed and digested",
     &fingerprint_metadata_is_paired_parsed_and_digested},
    {"proxy preferences are typed validated defaulted and digested",
     &proxy_preferences_are_typed_validated_defaulted_and_digested},
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
