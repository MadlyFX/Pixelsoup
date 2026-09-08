#include <mediaserver/scene/lighting.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/persist/project_file.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace scene = mediaserver::scene;
namespace schema = scene::schema;
namespace doc = mediaserver::doc;
namespace persist = mediaserver::persist;
namespace core = mediaserver::core;
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error(std::to_string(__LINE__) + ": " + #__VA_ARGS__); } while (false)

void temperature_and_units() {
    CHECK(scene::light_temperature_rgb(6500) == scene::Vec3{1, 1, 1});
    const auto warm = scene::light_temperature_rgb(3200);
    const auto cool = scene::light_temperature_rgb(8000);
    CHECK(warm.x > warm.y * 1.5F && warm.y > warm.z * 1.5F);
    CHECK(cool.z > cool.y * 1.15F && cool.y > cool.x * 1.05F);
    for (float kelvin : {1000.0F, 1650.0F, 3200.0F, 6555.0F, 8000.0F, 25000.0F, 40000.0F}) {
        const auto rgb = scene::light_temperature_rgb(kelvin);
        CHECK(std::isfinite(rgb.x) && rgb.x >= 0 && rgb.y >= 0 && rgb.z >= 0);
        CHECK(std::abs(0.2126F * rgb.x + 0.7152F * rgb.y + 0.0722F * rgb.z - 1) < 0.00001F);
    }
    CHECK(scene::light_temperature_rgb(-100) == scene::light_temperature_rgb(1000));
    CHECK(scene::light_temperature_rgb(90000) == scene::light_temperature_rgb(40000));
    CHECK(scene::light_temperature_rgb(std::numeric_limits<float>::quiet_NaN()) == scene::Vec3{1, 1, 1});
    scene::PointLight point;
    point.color = {0.25F, 0.5F, 0.75F};
    point.intensity = 123.5F;
    point.color_temperature_kelvin = 3200;
    CHECK(scene::light_emission(point, 0.8F).color == point.color);
    CHECK(scene::light_emission(point, 0.8F).intensity == 123.5F);
    point.use_color_temperature = true;
    point.color_temperature_kelvin = 6500;
    CHECK(scene::light_emission(point, 0.8F).color == point.color);
    point.unit = scene::LightUnit::Lumens;
    point.intensity = 1256.6370614F; // an isotropic 100 cd source
    CHECK(std::abs(scene::light_emission(point, 1).intensity - 100) < 0.00002F);
    point.unit = scene::LightUnit::Candela;
    point.intensity = 100;
    CHECK(scene::light_emission(point, 0.25F).intensity == 25);
    point.unit = scene::LightUnit::Lux;
    CHECK(!scene::supported_light_unit(point));
    point.unit = scene::LightUnit::Nits;
    CHECK(!scene::supported_light_unit(point));
    scene::DirectionalLight sun;
    sun.unit = scene::LightUnit::Lux;
    sun.intensity = 1000;
    CHECK(std::abs(scene::light_emission(sun, scene::RendererSettings{}.photometric_scale).intensity - 3.14159265F) < 0.000001F);
    sun.unit = scene::LightUnit::Lumens;
    CHECK(!scene::supported_light_unit(sun));
}

void cones_conserve_flux() {
    scene::PointLight spot;
    spot.kind = scene::LocalLightKind::Spot;
    spot.unit = scene::LightUnit::Lumens;
    spot.intensity = 1200;
    spot.inner_cone = 0.0625F; // 22.5 degrees
    spot.outer_cone = 0.125F; // 45 degrees
    CHECK(scene::spot_cone_attenuation(spot, 1) == 1);
    CHECK(scene::spot_cone_attenuation(spot, 0.95F) == 1);
    CHECK(scene::spot_cone_attenuation(spot, 0.7F) == 0);
    // Solid-angle quadrature over a sphere is independent of the analytic
    // packer's formula: 2*pi*d(cos theta), sampled at interval midpoints.
    constexpr unsigned samples = 20000;
    double integral = 0;
    float previous = 0;
    for (unsigned i = 0; i < samples; ++i) {
        const float alignment = -1 + (static_cast<float>(i) + 0.5F) * 2 / samples;
        const float weight = scene::spot_cone_attenuation(spot, alignment);
        CHECK(weight >= previous && weight <= 1);
        previous = weight;
        integral += weight * (4.0 * 3.141592653589793 / samples);
    }
    CHECK(std::abs(integral * scene::light_emission(spot, 1).intensity - 1200) < 0.1);
    spot.inner_cone = spot.outer_cone;
    CHECK(scene::spot_cone_attenuation(spot, scene::deterministic::cos_turns(spot.inner_cone)) == 1);
    CHECK(scene::spot_cone_attenuation(spot, 0.70F) == 0);
    CHECK(std::abs(scene::spot_solid_angle(0.25F, 0.25F) - 6.283185307F) < 0.000001F);
}

doc::Node node(core::u64 id, std::string_view type) {
    doc::Node result;
    result.id = doc::NodeId{id}; result.type = type; return result;
}
void authored_fields_survive_save_and_replication() {
    auto lamp = node(2, schema::light_type);
    lamp.properties.emplace(std::string{schema::light_kind}, std::string{schema::spot_kind});
    lamp.properties.emplace(std::string{schema::light_unit}, std::string{schema::lumens_unit});
    lamp.properties.emplace(std::string{schema::color_temperature_kelvin}, 3200.0);
    lamp.properties.emplace(std::string{schema::use_color_temperature}, true);
    lamp.properties.emplace(std::string{schema::inner_cone_turns}, 0.04);
    lamp.properties.emplace(std::string{schema::outer_cone_turns}, 0.09);
    lamp.properties.emplace(std::string{schema::intensity}, 750.0);
    lamp.properties.emplace(std::string{schema::range}, 17.0);
    lamp.properties.emplace(std::string{schema::direction_x}, 0.2);
    lamp.properties.emplace(std::string{schema::direction_y}, -0.8);
    lamp.properties.emplace(std::string{schema::direction_z}, 0.4);
    auto sun = node(3, schema::light_type);
    sun.properties.emplace(std::string{schema::light_kind}, std::string{schema::directional_kind});
    sun.properties.emplace(std::string{schema::light_unit}, std::string{schema::lux_unit});
    sun.properties.emplace(std::string{schema::color_temperature_kelvin}, 8000.0);
    sun.properties.emplace(std::string{schema::use_color_temperature}, true);
    sun.properties.emplace(std::string{schema::intensity}, 2700.0);
    scene::RendererSettings settings;
    settings.photometric_scale = 0.0125F;
    auto policy = node(4, schema::settings_type);
    scene::write_renderer_settings(policy, settings);
    doc::Root root;
    root = root.set(node(1, schema::stage_type)).set(lamp).set(sun).set(policy);
    scene::SceneModel model; model.reset(root);
    CHECK(model.counters().rejected == 0);
    const scene::SceneTime time{0, 60, 1};
    const auto before = model.evaluate(time);
    CHECK(before.point_lights.size() == 1 && before.directional_lights.size() == 1);
    const auto &p = before.point_lights.front();
    CHECK(p.kind == scene::LocalLightKind::Spot);
    CHECK(p.unit == scene::LightUnit::Lumens);
    CHECK(p.color_temperature_kelvin == 3200 && p.use_color_temperature);
    CHECK(p.inner_cone == 0.04F && p.outer_cone == 0.09F);
    CHECK(p.intensity == 750 && p.range == 17);
    CHECK(std::abs(scene::dot(p.direction, p.direction) - 1) < 0.00001F);
    CHECK(std::abs(p.direction.y / p.direction.x + 4) < 0.00001F);
    CHECK(std::abs(p.direction.z / p.direction.x - 2) < 0.00001F);
    CHECK(before.directional_lights[0].unit == scene::LightUnit::Lux);
    CHECK(before.directional_lights[0].color_temperature_kelvin == 8000);
    CHECK(before.directional_lights[0].use_color_temperature);
    CHECK(before.renderer_settings.photometric_scale == 0.0125F);
    const auto path = std::filesystem::temp_directory_path() / ("photometric-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".mstage");
    CHECK(persist::save_project_file(path, root));
    const auto loaded = persist::load_project_file(path); CHECK(loaded);
    // Project files carry the wire StreamCheckpoint; this proves its field
    // transport as well as the model's reconstruction, with no default fixture.
    scene::SceneModel replay; replay.reset(loaded.value());
    const auto after = replay.evaluate(time);
    CHECK(after.point_lights == before.point_lights);
    CHECK(after.directional_lights == before.directional_lights);
    CHECK(after.renderer_settings == before.renderer_settings);
    CHECK(scene::digest(after) == scene::digest(before));
    std::filesystem::remove(path);
    const auto digest = scene::digest(before);
    const auto changed = [&](auto change) {
        auto candidate = before; change(candidate); CHECK(scene::digest(candidate) != digest);
    };
    changed([](auto& s){ s.point_lights[0].unit = scene::LightUnit::Candela; });
    changed([](auto& s){ s.point_lights[0].use_color_temperature = false; });
    changed([](auto& s){ s.point_lights[0].color_temperature_kelvin = 5600; });
    changed([](auto& s){ s.point_lights[0].inner_cone = 0.05F; });
    changed([](auto& s){ s.point_lights[0].outer_cone = 0.10F; });
    changed([](auto& s){ s.point_lights[0].direction.x = 0.3F; });
    changed([](auto& s){ s.directional_lights[0].unit = scene::LightUnit::Unitless; });
    changed([](auto& s){ s.directional_lights[0].use_color_temperature = false; });
    changed([](auto& s){ s.directional_lights[0].color_temperature_kelvin = 5600; });
    changed([](auto& s){ s.renderer_settings.photometric_scale = 0.02F; });
    auto other = settings; other.photometric_scale = 0.02F;
    CHECK(scene::renderer_settings_digest(settings) != scene::renderer_settings_digest(other));
    other.photometric_scale = -1; CHECK(scene::clamped(other).photometric_scale == 0.000001F);
    other.photometric_scale = 100; CHECK(scene::clamped(other).photometric_scale == 1);
    other.photometric_scale = std::numeric_limits<float>::infinity();
    CHECK(scene::clamped(other).photometric_scale == scene::RendererSettings{}.photometric_scale);
    lamp.properties.insert_or_assign(std::string{schema::outer_cone_turns}, 0.4);
    lamp.properties.insert_or_assign(std::string{schema::inner_cone_turns}, 0.3);
    lamp.properties.insert_or_assign(std::string{schema::color_temperature_kelvin}, 90000.0);
    model.reset(root.set(lamp));
    const auto clamped = model.evaluate(time);
    CHECK(clamped.point_lights[0].outer_cone == 0.249F);
    CHECK(clamped.point_lights[0].inner_cone == 0.249F);
    CHECK(clamped.point_lights[0].color_temperature_kelvin == 40000);
}

int main() {
    try {
        temperature_and_units();
        cones_conserve_flux();
        authored_fields_survive_save_and_replication();
        std::cout << "Photometric math, flux, persistence and field digests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
