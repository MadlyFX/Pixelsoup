#include <mediaserver/render/model_loader.hpp>
#include <mediaserver/render/model_package.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/persist/settings.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/show/transport_state.hpp>
#include <mediaserver/show/switcher_state.hpp>
#include <mediaserver/compositor/compositor_model.hpp>
#include <mediaserver/compositor/compositor_schema.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Offline step-01 qualification. No networking, live-link schema, or render-loop
// instrumentation is introduced by this tool. CTest runs its CPU checks only.
namespace {
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;
namespace schema = scene::schema;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace persist = mediaserver::persist;
namespace compositor = mediaserver::compositor;
using Clock = std::chrono::steady_clock;
using Corner = std::array<float, 11>;
constexpr core::u64 fixture_node_base = 100'000U;
static_assert(fixture_node_base > mediaserver::show::transport_node_id.value()
           && fixture_node_base > mediaserver::show::switcher_node_id.value());
doc::NodeId fixture_id(core::u64 value) { return doc::NodeId{fixture_node_base + value}; }

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
bool near(float a, float b) { return std::abs(a - b) < 2e-5F; }
bool same_corner(const Corner& a, const Corner& b) {
    for (std::size_t component = 0; component < a.size(); ++component) {
        // Blender 5.2's exporter rounds normals to four decimal places before
        // normalization (io_scene_gltf2 ROUNDING_DIGIT). Keep positions and
        // material factors strict; allow that documented normal quantization.
        const float tolerance = component >= 3 && component < 6 ? 2e-4F : 2e-5F;
        if (!(std::abs(a[component] - b[component]) < tolerance)) return false;
    }
    return true;
}

render::ModelAsset check_export(const std::filesystem::path& directory, int revision) {
    const auto stem = "reference-r" + std::to_string(revision);
    render::ModelAsset asset;
    require(render::load_model((directory / (stem + ".glb")).string(), asset), "load " + stem);
    std::ifstream oracle{directory / (stem + ".expected")};
    std::size_t count{};
    require(static_cast<bool>(oracle >> count) && count == 114U, "reference has 38 triangles");
    std::vector<Corner> expected(count);
    for (auto& row : expected)
        for (auto& value : row) require(static_cast<bool>(oracle >> value), "complete oracle");
    require(asset.mesh.indices.size() == count && asset.parts.size() == 7U,
            "three shared occurrences, two materials each, plus floor; hidden mesh excluded");
    std::vector<bool> matched(count);
    for (const auto& part : asset.parts) {
        require(part.has_material, "material binding retained");
        const auto& m = part.material;
        for (core::u32 i = part.first_index; i < part.first_index + part.index_count; ++i) {
            const auto& v = asset.mesh.vertices.at(asset.mesh.indices.at(i));
            const Corner actual{v.position.x, v.position.y, v.position.z,
                v.normal.x, v.normal.y, v.normal.z, m.base_color.x, m.base_color.y,
                m.base_color.z, m.metallic, m.roughness};
            bool found{};
            for (std::size_t j = 0; j < count; ++j) {
                if (!matched[j] && same_corner(actual, expected[j])) {
                    matched[j] = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::cerr << stem << " unmatched corner:";
                for (const auto value : actual) std::cerr << ' ' << value;
                std::cerr << '\n';
                throw std::runtime_error("Blender world position, inverse-transpose normal or PBR factor differs");
            }
        }
    }
    require(near(asset.bounds.minimum.x, -5) && near(asset.bounds.maximum.x, 5)
         && near(asset.bounds.minimum.z, -4) && near(asset.bounds.maximum.z, 3), "meter/axis bounds");
    return asset;
}

doc::Node node(core::u64 id, std::string_view type,
               std::initializer_list<std::pair<std::string_view, doc::PropertyValue>> properties) {
    doc::Node value{};
    value.id = fixture_id(id);
    value.type = type;
    for (const auto& [key, property] : properties) {
        if (key == schema::scene_id || key == schema::parent_group
            || key == compositor::schema::scene_node || key == compositor::schema::from_node
            || key == compositor::schema::to_node) {
            value.properties.emplace(key, std::get<core::i64>(property) + static_cast<core::i64>(fixture_node_base));
        } else {
            value.properties.emplace(key, property);
        }
    }
    return value;
}

doc::Root project(const std::string& source) {
    doc::Root root;
    root = root.set(node(1, schema::stage_type, {{schema::scene_name, std::string{"Blender reference"}},
        {schema::canvas_width, core::i64{1280}}, {schema::canvas_height, core::i64{720}},
        {schema::ambient, 0.4}}));
    scene::Camera aim{};
    aim.eye = scene::Vec3{7.0F, 6.0F, 10.0F};
    aim.target = scene::Vec3{0.0F, 0.75F, -1.0F};
    const scene::Vec3 turns = scene::camera_rotation(aim);
    root = root.set(node(2, schema::camera_type, {{schema::scene_id, core::i64{1}},
        {schema::group_name, std::string{"Composition camera"}},
        {schema::position_x, 7.0}, {schema::position_y, 6.0}, {schema::position_z, 10.0},
        {schema::rotation_yaw, (static_cast<double>(turns.x)+0.0)},
        {schema::rotation_pitch, (static_cast<double>(turns.y)+0.0)},
        {schema::rotation_roll, (static_cast<double>(turns.z)+0.0)},
        {schema::fov_turns, 0.07044657495455456}, {schema::near_plane, 0.05}, {schema::far_plane, 200.0}}));
    root = root.set(node(3, schema::settings_type, {{schema::renderer_backend, std::string{"advanced"}},
        {schema::renderer_required, true}, {schema::renderer_quality, std::string{"interactive"}}}));
    root = root.set(node(4, schema::group_type, {{schema::scene_id, core::i64{1}},
        {schema::group_name, std::string{"Placement parent"}}, {schema::position_x, 1.0},
        {schema::rotation_yaw, 0.25}}));
    root = root.set(node(10, schema::model_type, {{schema::scene_id, core::i64{1}},
        {schema::group_name, std::string{"Hero prop"}}, {schema::source, source}, {schema::parent_group, core::i64{4}},
        {schema::position_x, 1.0}, {schema::position_y, 1.0},
        {schema::scale_x, 2.0}, {schema::scale_y, 0.5}, {schema::scale_z, 1.0}}));
    root = root.set(node(11, schema::model_type, {{schema::scene_id, core::i64{1}},
        {schema::group_name, std::string{"Shared prop"}}, {schema::source, source},
        {schema::position_x, -2.0}, {schema::position_y, 0.5}}));
    root = root.set(node(12, schema::model_type, {{schema::scene_id, core::i64{1}},
        {schema::group_name, std::string{"Mirrored prop"}}, {schema::source, source},
        {schema::position_x, 3.0}, {schema::position_y, 0.5}, {schema::scale_x, -1.0}}));
    root = root.set(node(13, schema::model_type, {{schema::scene_id, core::i64{1}},
        {schema::group_name, std::string{"Hidden prop"}}, {schema::source, source}, {schema::visible, false},
        {schema::position_y, 0.5}, {schema::position_z, -4.0}}));
    root = root.set(node(14, schema::object_type, {{schema::scene_id, core::i64{1}},
        {schema::group_name, std::string{"Floor"}},
        {schema::primitive, std::string{"plane"}}, {schema::scale_x, 10.0}, {schema::scale_z, 7.0},
        {schema::position_z, -0.5}, {schema::color_r, 0.18}, {schema::color_g, 0.18}, {schema::color_b, 0.18}}));
    root = root.set(node(15, schema::light_type, {{schema::scene_id, core::i64{1}},
        {schema::group_name, std::string{"Reference key"}},
        {schema::light_kind, std::string{"directional"}}, {schema::direction_x, -0.3},
        {schema::direction_y, -1.0}, {schema::direction_z, -0.5}, {schema::intensity, 3.0}}));
    // A connected selective graph survives the editor's Preview scene copy.
    // Unrepresented drawables are preserved by the ordinary Output policy.
    root = root.set(node(20, compositor::schema::scene_layer_type,
        {{schema::scene_id, core::i64{1}}, {compositor::schema::scene_node, core::i64{14}}}));
    root = root.set(node(21, compositor::schema::output_type,
        {{schema::scene_id, core::i64{1}}, {compositor::schema::position_x, 300.0}}));
    root = root.set(node(22, compositor::schema::edge_type,
        {{compositor::schema::from_node, core::i64{20}}, {compositor::schema::to_node, core::i64{21}},
         {compositor::schema::from_port, core::i64{0}}, {compositor::schema::to_port, core::i64{0}}}));
    return root;
}

doc::Root replace_geometry(const doc::Root& root, const std::string& source) {
    auto result = root;
    for (core::u64 id : {10U, 11U, 12U, 13U}) {
        auto changed = *root.get(fixture_id(id));
        changed.properties[std::string{schema::source}] = source;
        result = result.set(std::move(changed));
    }
    return result;
}

void check_placement(const std::filesystem::path& fixtures, const std::filesystem::path& scratch) {
    auto before = project((fixtures / "prop-r1.glb").string());
    compositor::CompositorModel graph;
    graph.reset(before);
    require(graph.evaluate().valid(), "reference compositor graph is complete");
    // A real local document edit moves the hero to camera before source changes.
    auto hero = *before.get(fixture_id(10));
    hero.properties[std::string{schema::position_x}] = 1.5;
    before = before.set(std::move(hero));
    const auto after = replace_geometry(before, (fixtures / "prop-r2.glb").string());
    scene::SceneModel model;
    model.reset(before);
    const auto first = model.evaluate({});
    model.apply(doc::diff(before, after));
    const auto second = model.evaluate({});
    require(first.models.size() == 3 && second.models.size() == 3, "hidden occurrence stays hidden");
    for (std::size_t i = 0; i < first.models.size(); ++i) {
        require(first.models[i].world == second.models[i].world
            && first.models[i].source_node == second.models[i].source_node
            && first.models[i].material == second.models[i].material
            && first.models[i].source != second.models[i].source, "source replacement preserves local placement/identity/material");
    }
    require(near(second.models[0].world.at(0, 3), 1)
         && near(second.models[0].world.at(1, 3), 1)
         && near(second.models[0].world.at(2, 3), -1.5F), "parent then adjusted local transform");
    require(persist::save_project_file(scratch / "placement-retained.mstage", after).has_value(), "save placement");
    auto reopened = persist::load_project_file(scratch / "placement-retained.mstage");
    require(reopened.has_value(), "reopen placement");
    model.reset(reopened.value());
    require(model.evaluate({}).models == second.models, "placement/source survive project reopen");
    render::ModelAsset a, b;
    require(render::load_model((fixtures / "prop-r1.glb").string(), a)
         && render::load_model((fixtures / "prop-r2.glb").string(), b), "both prototype revisions load");
    require(near(a.bounds.maximum.y, 0.5F) && near(b.bounds.maximum.y, 0.75F), "local geometry revision actually changes");
}

void report(std::ofstream& output, const char* phase, const std::vector<double>& samples) {
    auto ordered = samples;
    std::sort(ordered.begin(), ordered.end());
    std::cout << phase << " n=" << samples.size() << " p50_ms=" << ordered[(ordered.size() - 1U) / 2U]
              << " p95_ms=" << ordered[static_cast<std::size_t>(std::ceil(ordered.size() * 0.95)) - 1U] << '\n';
    for (std::size_t i = 0; i < samples.size(); ++i) output << phase << ',' << i << ',' << samples[i] << '\n';
}

void benchmark(const std::filesystem::path& fixtures, const std::filesystem::path& scratch,
               const std::string& filename, std::size_t occurrences) {
    // Isolated tool thread only: blocking completion here is a measurement boundary.
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto created = rhi::Device::create(config);
    require(created.has_value(), "D3D12 device required for requested benchmark");
    auto device = std::move(created).value();
    render::ModelAsset asset;
    require(render::load_model((fixtures / filename).string(), asset), "benchmark dataset loads");
    const auto vb = asset.mesh.vertices.size() * sizeof(scene::MeshVertex);
    const auto ib = asset.mesh.indices.size() * sizeof(core::u32);
    auto vertices = device.create_storage_buffer(static_cast<core::u32>(vb));
    auto indices = device.create_storage_buffer(static_cast<core::u32>(ib));
    require(vertices.has_value() && indices.has_value(), "upload buffers");
    std::vector<double> ingest, upload, adoption;
    const auto input = scratch / ("benchmark-" + filename);
    std::filesystem::copy_file(fixtures / filename, input, std::filesystem::copy_options::overwrite_existing);
    for (int i = 0; i < 20; ++i) {
        auto start = Clock::now();
        const auto package = render::ingest_model(input.string());
        require(!package.source.empty(), "ingest: " + package.diagnostic);
        ingest.push_back(elapsed(start));
        start = Clock::now();
        auto acquired = device.acquire_commands();
        require(acquired.has_value(), "acquire upload");
        auto commands = std::move(acquired).value();
        require(commands.upload(vertices.value(), {reinterpret_cast<const core::u8*>(asset.mesh.vertices.data()), vb})
            && commands.upload(indices.value(), {reinterpret_cast<const core::u8*>(asset.mesh.indices.data()), ib}), "upload geometry");
        auto submitted = device.submit(std::move(commands));
        require(submitted.has_value() && submitted.value().wait(), "upload fence completion");
        upload.push_back(elapsed(start));
        auto before = project("old.msmodel");
        for (std::size_t n = 3; n < occurrences; ++n) {
            auto extra = *before.get(fixture_id(11));
            extra.id = fixture_id(1000U + n);
            before = before.set(std::move(extra));
        }
        auto after = replace_geometry(before, package.source);
        for (std::size_t n = 3; n < occurrences; ++n) {
            auto extra = *before.get(fixture_id(1000U + n));
            extra.properties[std::string{schema::source}] = package.source;
            after = after.set(std::move(extra));
        }
        scene::SceneModel model;
        model.reset(before);
        start = Clock::now();
        model.apply(doc::diff(before, after));
        const auto adopted = model.evaluate({});
        adoption.push_back(elapsed(start));
        require(adopted.models.size() == occurrences, "adopt all visible occurrences");
    }
    std::ofstream output{scratch / (filename + ".baseline.csv")};
    output << std::setprecision(9) << "phase,sample,ms\n";
    std::cout << "dataset=" << filename << " document_occurrences=" << occurrences << '\n';
    report(output, "ingest_package", ingest);
    report(output, "geometry_upload_fence", upload);
    report(output, "document_scene_adoption", adoption);
    require(static_cast<bool>(output), "write timings");
    std::cout << "triangles=" << asset.mesh.indices.size()/3 << " vertices=" << asset.mesh.vertices.size()
              << " materials=" << asset.parts.size() << " textures=" << asset.textures.size()
              << " source_bytes=" << std::filesystem::file_size(input) << " geometry_upload_bytes=" << vb + ib << '\n';
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3 || (argc == 4 && std::string{argv[3]} == "--benchmark"),
                "usage: blender_scene_reference_tests FIXTURES SCRATCH [--benchmark]");
        const auto fixtures = std::filesystem::absolute(argv[1]);
        const auto scratch = std::filesystem::absolute(argv[2]);
        std::filesystem::create_directories(scratch);
        static_cast<void>(check_export(fixtures, 1));
        static_cast<void>(check_export(fixtures, 2));
        check_placement(fixtures, scratch);
        for (int revision : {1, 2}) {
            const auto stem = "prop-r" + std::to_string(revision);
            const auto input = scratch / (stem + ".glb");
            std::filesystem::copy_file(fixtures / (stem + ".glb"), input, std::filesystem::copy_options::overwrite_existing);
            const auto package = render::ingest_model(input.string());
            require(!package.source.empty(), "prototype ingest: " + package.diagnostic);
            require(persist::save_project_file(scratch / ("reference-r" + std::to_string(revision) + ".mstage"),
                project(package.source)).has_value(), "write UI reference project");
            require(persist::save_recent_project_path(
                scratch / ("ui-r" + std::to_string(revision)) / "MediaServer/settings/recent-project.bin",
                scratch / ("reference-r" + std::to_string(revision) + ".mstage")).has_value(),
                "write isolated reference-editor startup settings");
        }
        if (argc == 4) {
            benchmark(fixtures, scratch, "reference-r2.glb", 3);
            benchmark(fixtures, scratch, "workload-256.glb", 256);
        }
        std::cout << "Blender scene reference checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
