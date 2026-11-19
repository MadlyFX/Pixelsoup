#include <mediaserver/render/model_package.hpp>
#include <mediaserver/render/model_ingest_queue.hpp>
#include <mediaserver/render/scene_bridge.hpp>
#include <mediaserver/render/texture_streaming.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/math.hpp>

#include <array>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;
namespace schema = mediaserver::scene::schema;

// The one Stage every fixture object names.
doc::Root staged() {
    doc::Node stage; stage.id = doc::NodeId{41U}; stage.type = std::string{schema::stage_type};
    return doc::Root{}.set(stage);
}

void check(bool v, const char* message, int line) { if (!v) throw std::runtime_error{std::to_string(line) + ": " + message}; }
#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

void fields_are_authored_clamped_and_digested() {
    scene::Scene baseline;
    baseline.instances.resize(1U);
    baseline.instances[0].source = doc::NodeId{42U};
    auto bridge = render::SceneBridge::create(4U); CHECK(bridge);
    auto initial = bridge.value().update(baseline); CHECK(initial);
    const auto test_number = [&](std::string_view key, float expected, auto set, auto get, float low, float high) {
        auto changed = baseline; set(changed.instances[0].material, expected);
        CHECK(scene::digest(changed) != scene::digest(baseline));
        auto update = bridge.value().prepare(changed); CHECK(update);
        CHECK(update.value().visual_digest != initial.value().visual_digest);
        doc::Node object; object.id = doc::NodeId{42U}; object.type = std::string{schema::object_type}; object.properties.emplace(std::string{schema::scene_id}, std::int64_t{41});
        object.properties.emplace(std::string{key}, doc::PropertyValue{static_cast<double>(expected)});
        doc::Root root = staged(); root = root.set(object);
        scene::SceneModel model; model.reset(root);
        const auto value = model.evaluate({0U, 60U, 1U});
        CHECK(value.instances.size() == 1U);
        CHECK(get(value.instances[0].material) == expected);
        for (const auto clamp : {std::pair{-100.0, low}, std::pair{100.0, high}}) {
            object.properties[std::string{key}] = doc::PropertyValue{clamp.first};
            root = root.set(object); model.reset(root);
            CHECK(get(model.evaluate({0U, 60U, 1U}).instances[0].material) == clamp.second);
        }
    };
    test_number(schema::base_alpha, 0.125F, [](auto& m, float v) { m.base_alpha = v; }, [](const auto& m) { return m.base_alpha; }, 0.0F, 1.0F);
    test_number(schema::clearcoat, 0.15625F, [](auto& m, float v) { m.clearcoat = v; }, [](const auto& m) { return m.clearcoat; }, 0.0F, 1.0F);
    test_number(schema::clearcoat_roughness, 0.1875F, [](auto& m, float v) { m.clearcoat_roughness = v; }, [](const auto& m) { return m.clearcoat_roughness; }, 0.0F, 1.0F);
    test_number(schema::clearcoat_normal_scale, 0.21875F, [](auto& m, float v) { m.clearcoat_normal_scale = v; }, [](const auto& m) { return m.clearcoat_normal_scale; }, 0.0F, 4.0F);
    test_number(schema::sheen_roughness, 0.25F, [](auto& m, float v) { m.sheen_roughness = v; }, [](const auto& m) { return m.sheen_roughness; }, 0.0F, 1.0F);
    test_number(schema::transmission, 0.28125F, [](auto& m, float v) { m.transmission = v; }, [](const auto& m) { return m.transmission; }, 0.0F, 1.0F);
    test_number(schema::ior, 1.625F, [](auto& m, float v) { m.ior = v; }, [](const auto& m) { return m.ior; }, 1.0F, 3.0F);
    test_number(schema::specular, 0.34375F, [](auto& m, float v) { m.specular = v; }, [](const auto& m) { return m.specular; }, 0.0F, 1.0F);
    test_number(schema::anisotropy_strength, 0.375F, [](auto& m, float v) { m.anisotropy_strength = v; }, [](const auto& m) { return m.anisotropy_strength; }, 0.0F, 1.0F);
    test_number(schema::anisotropy_rotation, -0.75F, [](auto& m, float v) { m.anisotropy_rotation = v; }, [](const auto& m) { return m.anisotropy_rotation; }, -6.283185307F, 6.283185307F);
    test_number(schema::sheen_color_r, 0.4375F, [](auto& m, float v) { m.sheen_color.x = v; }, [](const auto& m) { return m.sheen_color.x; }, 0.0F, 1.0F);
    test_number(schema::sheen_color_g, 0.46875F, [](auto& m, float v) { m.sheen_color.y = v; }, [](const auto& m) { return m.sheen_color.y; }, 0.0F, 1.0F);
    test_number(schema::sheen_color_b, 0.5F, [](auto& m, float v) { m.sheen_color.z = v; }, [](const auto& m) { return m.sheen_color.z; }, 0.0F, 1.0F);
    test_number(schema::specular_color_r, 0.53125F, [](auto& m, float v) { m.specular_color.x = v; }, [](const auto& m) { return m.specular_color.x; }, 0.0F, 1.0F);
    test_number(schema::specular_color_g, 0.5625F, [](auto& m, float v) { m.specular_color.y = v; }, [](const auto& m) { return m.specular_color.y; }, 0.0F, 1.0F);
    test_number(schema::specular_color_b, 0.59375F, [](auto& m, float v) { m.specular_color.z = v; }, [](const auto& m) { return m.specular_color.z; }, 0.0F, 1.0F);
    {
        auto changed = baseline; changed.instances[0].material.clearcoat_texture = "assets/clearcoat_texture.png";
        CHECK(scene::digest(changed) != scene::digest(baseline));
        auto update = bridge.value().prepare(changed); CHECK(update);
        CHECK(update.value().visual_digest != initial.value().visual_digest);
        doc::Node object; object.id = doc::NodeId{42U}; object.type = std::string{schema::object_type}; object.properties.emplace(std::string{schema::scene_id}, std::int64_t{41});
        object.properties.emplace(std::string{schema::clearcoat_texture}, doc::PropertyValue{std::string{"assets/clearcoat_texture.png"}});
        doc::Root root = staged(); root = root.set(object); scene::SceneModel model; model.reset(root);
        CHECK(model.evaluate({0U, 60U, 1U}).instances[0].material.clearcoat_texture == "assets/clearcoat_texture.png");
    }
    {
        auto changed = baseline; changed.instances[0].material.clearcoat_roughness_texture = "assets/clearcoat_roughness_texture.png";
        CHECK(scene::digest(changed) != scene::digest(baseline));
        auto update = bridge.value().prepare(changed); CHECK(update);
        CHECK(update.value().visual_digest != initial.value().visual_digest);
        doc::Node object; object.id = doc::NodeId{42U}; object.type = std::string{schema::object_type}; object.properties.emplace(std::string{schema::scene_id}, std::int64_t{41});
        object.properties.emplace(std::string{schema::clearcoat_roughness_texture}, doc::PropertyValue{std::string{"assets/clearcoat_roughness_texture.png"}});
        doc::Root root = staged(); root = root.set(object); scene::SceneModel model; model.reset(root);
        CHECK(model.evaluate({0U, 60U, 1U}).instances[0].material.clearcoat_roughness_texture == "assets/clearcoat_roughness_texture.png");
    }
    {
        auto changed = baseline; changed.instances[0].material.clearcoat_normal_texture = "assets/clearcoat_normal_texture.png";
        CHECK(scene::digest(changed) != scene::digest(baseline));
        auto update = bridge.value().prepare(changed); CHECK(update);
        CHECK(update.value().visual_digest != initial.value().visual_digest);
        doc::Node object; object.id = doc::NodeId{42U}; object.type = std::string{schema::object_type}; object.properties.emplace(std::string{schema::scene_id}, std::int64_t{41});
        object.properties.emplace(std::string{schema::clearcoat_normal_texture}, doc::PropertyValue{std::string{"assets/clearcoat_normal_texture.png"}});
        doc::Root root = staged(); root = root.set(object); scene::SceneModel model; model.reset(root);
        CHECK(model.evaluate({0U, 60U, 1U}).instances[0].material.clearcoat_normal_texture == "assets/clearcoat_normal_texture.png");
    }
    {
        auto changed = baseline; changed.instances[0].material.sheen_color_texture = "assets/sheen_color_texture.png";
        CHECK(scene::digest(changed) != scene::digest(baseline));
        auto update = bridge.value().prepare(changed); CHECK(update);
        CHECK(update.value().visual_digest != initial.value().visual_digest);
        doc::Node object; object.id = doc::NodeId{42U}; object.type = std::string{schema::object_type}; object.properties.emplace(std::string{schema::scene_id}, std::int64_t{41});
        object.properties.emplace(std::string{schema::sheen_color_texture}, doc::PropertyValue{std::string{"assets/sheen_color_texture.png"}});
        doc::Root root = staged(); root = root.set(object); scene::SceneModel model; model.reset(root);
        CHECK(model.evaluate({0U, 60U, 1U}).instances[0].material.sheen_color_texture == "assets/sheen_color_texture.png");
    }
    {
        auto changed = baseline; changed.instances[0].material.sheen_roughness_texture = "assets/sheen_roughness_texture.png";
        CHECK(scene::digest(changed) != scene::digest(baseline));
        auto update = bridge.value().prepare(changed); CHECK(update);
        CHECK(update.value().visual_digest != initial.value().visual_digest);
        doc::Node object; object.id = doc::NodeId{42U}; object.type = std::string{schema::object_type}; object.properties.emplace(std::string{schema::scene_id}, std::int64_t{41});
        object.properties.emplace(std::string{schema::sheen_roughness_texture}, doc::PropertyValue{std::string{"assets/sheen_roughness_texture.png"}});
        doc::Root root = staged(); root = root.set(object); scene::SceneModel model; model.reset(root);
        CHECK(model.evaluate({0U, 60U, 1U}).instances[0].material.sheen_roughness_texture == "assets/sheen_roughness_texture.png");
    }
    {
        auto changed = baseline; changed.instances[0].material.transmission_texture = "assets/transmission_texture.png";
        CHECK(scene::digest(changed) != scene::digest(baseline));
        auto update = bridge.value().prepare(changed); CHECK(update);
        CHECK(update.value().visual_digest != initial.value().visual_digest);
        doc::Node object; object.id = doc::NodeId{42U}; object.type = std::string{schema::object_type}; object.properties.emplace(std::string{schema::scene_id}, std::int64_t{41});
        object.properties.emplace(std::string{schema::transmission_texture}, doc::PropertyValue{std::string{"assets/transmission_texture.png"}});
        doc::Root root = staged(); root = root.set(object); scene::SceneModel model; model.reset(root);
        CHECK(model.evaluate({0U, 60U, 1U}).instances[0].material.transmission_texture == "assets/transmission_texture.png");
    }
    {
        auto changed = baseline; changed.instances[0].material.specular_texture = "assets/specular_texture.png";
        CHECK(scene::digest(changed) != scene::digest(baseline));
        auto update = bridge.value().prepare(changed); CHECK(update);
        CHECK(update.value().visual_digest != initial.value().visual_digest);
        doc::Node object; object.id = doc::NodeId{42U}; object.type = std::string{schema::object_type}; object.properties.emplace(std::string{schema::scene_id}, std::int64_t{41});
        object.properties.emplace(std::string{schema::specular_texture}, doc::PropertyValue{std::string{"assets/specular_texture.png"}});
        doc::Root root = staged(); root = root.set(object); scene::SceneModel model; model.reset(root);
        CHECK(model.evaluate({0U, 60U, 1U}).instances[0].material.specular_texture == "assets/specular_texture.png");
    }
    {
        auto changed = baseline; changed.instances[0].material.specular_color_texture = "assets/specular_color_texture.png";
        CHECK(scene::digest(changed) != scene::digest(baseline));
        auto update = bridge.value().prepare(changed); CHECK(update);
        CHECK(update.value().visual_digest != initial.value().visual_digest);
        doc::Node object; object.id = doc::NodeId{42U}; object.type = std::string{schema::object_type}; object.properties.emplace(std::string{schema::scene_id}, std::int64_t{41});
        object.properties.emplace(std::string{schema::specular_color_texture}, doc::PropertyValue{std::string{"assets/specular_color_texture.png"}});
        doc::Root root = staged(); root = root.set(object); scene::SceneModel model; model.reset(root);
        CHECK(model.evaluate({0U, 60U, 1U}).instances[0].material.specular_color_texture == "assets/specular_color_texture.png");
    }
    {
        auto changed = baseline; changed.instances[0].material.anisotropy_texture = "assets/anisotropy_texture.png";
        CHECK(scene::digest(changed) != scene::digest(baseline));
        auto update = bridge.value().prepare(changed); CHECK(update);
        CHECK(update.value().visual_digest != initial.value().visual_digest);
        doc::Node object; object.id = doc::NodeId{42U}; object.type = std::string{schema::object_type}; object.properties.emplace(std::string{schema::scene_id}, std::int64_t{41});
        object.properties.emplace(std::string{schema::anisotropy_texture}, doc::PropertyValue{std::string{"assets/anisotropy_texture.png"}});
        doc::Root root = staged(); root = root.set(object); scene::SceneModel model; model.reset(root);
        CHECK(model.evaluate({0U, 60U, 1U}).instances[0].material.anisotropy_texture == "assets/anisotropy_texture.png");
    }
}

void package_freezes_materials_and_compressed_mips() {
    const auto directory = std::filesystem::temp_directory_path() / "mediaserver-material-ingest";
    std::filesystem::create_directories(directory);
    const auto source = (directory / "package-test.obj").string();
    { std::ofstream f{source}; f << "v -1 -1 0\nv 1 -1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0.5 1\nf 1/1 2/2 3/3\n"; }
    render::ModelAsset asset; CHECK(render::load_model(source, asset));
    auto& m = asset.parts.front().material;
    m.base_alpha = 0.0625F;
    m.clearcoat = 0.09375F;
    m.clearcoat_roughness = 0.125F;
    m.clearcoat_normal_scale = 0.15625F;
    m.sheen_roughness = 0.1875F;
    m.transmission = 0.21875F;
    m.ior = 1.75F;
    m.specular = 0.28125F;
    m.anisotropy_strength = 0.3125F;
    m.anisotropy_rotation = -1.25F;
    m.sheen_color.x = 0.375F;
    m.sheen_color.y = 0.40625F;
    m.sheen_color.z = 0.4375F;
    m.specular_color.x = 0.46875F;
    m.specular_color.y = 0.5F;
    m.specular_color.z = 0.53125F;
    m.clearcoat_texture = "texture/clearcoat_texture";
    m.clearcoat_roughness_texture = "texture/clearcoat_roughness_texture";
    m.clearcoat_normal_texture = "texture/clearcoat_normal_texture";
    m.sheen_color_texture = "texture/sheen_color_texture";
    m.sheen_roughness_texture = "texture/sheen_roughness_texture";
    m.transmission_texture = "texture/transmission_texture";
    m.specular_texture = "texture/specular_texture";
    m.specular_color_texture = "texture/specular_color_texture";
    m.anisotropy_texture = "texture/anisotropy_texture";
    render::ModelTexture color; color.width = color.height = 16U;
    color.rgba.resize(1024U);
    for (std::size_t i = 0U; i < color.rgba.size(); i += 4U) {
        color.rgba[i] = 64U; color.rgba[i + 1U] = 128U; color.rgba[i + 2U] = 192U; color.rgba[i + 3U] = 255U;
    }
    auto normal = color;
    for (std::size_t i = 0U; i < normal.rgba.size(); i += 4U) {
        normal.rgba[i] = 128U; normal.rgba[i + 1U] = 128U; normal.rgba[i + 2U] = 255U;
    }
    asset.textures = {color, normal};
    asset.parts[0].base_color_texture = 0U;
    asset.parts[0].normal_texture = 1U;
    asset.parts[0].clearcoat_normal_texture = 1U;
    asset.parts[0].sheen_color_texture = 0U;
    const auto authored = m;
    CHECK(render::compress_model_textures(asset));
    CHECK(asset.textures[0].encoding == render::ModelTextureEncoding::Bc7);
    CHECK(asset.textures[1].encoding == render::ModelTextureEncoding::Bc5);
    CHECK(asset.textures[0].rgba.size() == 256U);
    CHECK(render::texture_mip_bytes(asset.textures[0], 0U) == 368U);
    CHECK(asset.textures[0].lower_mips.back().rgba.size() == 16U);
    const auto frozen = render::write_model_package(source, asset);
    CHECK(frozen.ends_with(".msmodel"));
    CHECK(render::write_model_package(source, asset) == frozen);
    // Destroy the source: the package must retain all decoded values and mips.
    { std::ofstream f{source}; f << "This is no longer a model"; }
    render::ModelAsset loaded; CHECK(render::load_model(frozen, loaded));
    CHECK(loaded.parts[0].material == authored);
    CHECK(loaded.parts == asset.parts);
    CHECK(loaded.textures == asset.textures);
    CHECK(loaded.mesh.vertices == asset.mesh.vertices);
    CHECK(loaded.mesh.indices == asset.mesh.indices);
    CHECK(loaded.vertex_metadata == asset.vertex_metadata);
    CHECK(loaded.lods == asset.lods);
    const auto damaged = (directory / "damaged.msmodel").string();
    std::filesystem::copy_file(frozen, damaged, std::filesystem::copy_options::overwrite_existing);
    { std::fstream file{damaged, std::ios::binary | std::ios::in | std::ios::out}; file.seekp(16); file.put('X'); }
    CHECK(!render::load_model(damaged, loaded));
    CHECK(loaded.mesh.vertices.empty()); CHECK(!loaded.diagnostic.empty());
    auto success = render::ModelIngestJob::start(frozen); CHECK(success);
    const auto ready_until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (!success->result() && std::chrono::steady_clock::now() < ready_until) std::this_thread::sleep_for(std::chrono::milliseconds{1});
    CHECK(success->result()); CHECK(success->stage() == render::ModelIngestStage::Ready);
    CHECK(success->result()->source == frozen); CHECK(success->result()->vertex_count == asset.mesh.vertices.size());
    auto job = render::ModelIngestJob::start(source); CHECK(job);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (!job->result() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds{1});
    CHECK(job->result()); CHECK(job->stage() == render::ModelIngestStage::Failed);
    CHECK(!job->result()->diagnostic.empty());
}

void parallel_compression_matches_serial_blocks() {
    render::ModelAsset asset;
    asset.parts.resize(1U);
    asset.parts[0].base_color_texture = 0U;
    render::ModelTexture image;
    image.width = image.height = 256U;
    image.rgba.resize(256U * 256U * 4U);
    for (std::size_t i = 0; i < image.rgba.size(); ++i)
        image.rgba[i] = static_cast<core::u8>((i * 17U + i / 103U) % 256U);
    asset.textures.push_back(image);
    CHECK(render::compress_model_textures(asset));
    for (std::size_t row = 0; row < 64U; ++row) {
        render::ModelAsset stripe;
        stripe.parts.resize(1U);
        stripe.parts[0].base_color_texture = 0U;
        render::ModelTexture texture;
        texture.width = 256U; texture.height = 4U;
        const auto start = image.rgba.begin() + row * 4096U;
        texture.rgba.assign(start, start + 4096U);
        stripe.textures.push_back(std::move(texture));
        CHECK(render::compress_model_textures(stripe));
        CHECK(std::equal(stripe.textures[0].rgba.begin(), stripe.textures[0].rgba.end(),
            asset.textures[0].rgba.begin() + row * 1024U));
    }
}

int main() {
    try { fields_are_authored_clamped_and_digested(); package_freezes_materials_and_compressed_mips(); parallel_compression_matches_serial_blocks();
        std::cout << "[pass] extended material document, digests, immutable compressed ingest and async failure\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
