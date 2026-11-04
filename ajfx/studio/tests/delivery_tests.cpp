#include "delivery.hpp"
#include "export_gltf.hpp"
#include "hands.hpp"
#include "json.hpp"
#include "performance.hpp"
#include "test_support.hpp"

#include <cstring>
#include <filesystem>

// Delivery: a performed scene baked, written as a package and its cache, and
// played as the media server plays it; and the performance written as glTF
// from that cache. Expectations are independent of what is tested: the
// delivered package, read back and played from its cache at a seek, holds
// the bodies the scene plays live, bit for bit; a glTF's keys are read back
// out of its own buffer and compared with the cache.
namespace {
using namespace studio_test;

Object& object(Document& document, Uid uid) { return std::get<Object>(document.scene->item(uid)->data); }

// A wall that breaks when hit at 4 m/s, and a heavy ball thrown at it.
struct Wall final {
    Document document = new_scene();
    Uid wall{};
    Uid ball{};
};

Wall thrown_at_the_wall() {
    Wall scene;
    scene.wall = add_object(scene.document);
    Object& wall = object(scene.document, scene.wall);
    wall.size = 1.6F;
    wall.position = {0.0F, 0.8F, -2.0F};
    wall.fracture = true;
    wall.fragments = 8U;
    wall.fracture_speed = 4.0F;
    scene.ball = add_object(scene.document);
    Object& ball = object(scene.document, scene.ball);
    ball.size = 0.4F;
    ball.position = {0.0F, 0.2F, 2.0F};
    ball.density = 3000.0F;
    // Thrown hard, a little up, at the wall.
    add_push(scene.document.scene->performance, 20U, BodyRef{.object = scene.ball, .count = 0U},
             {0.0F, 2.0F, -12.0F}, {});
    return scene;
}

// The solid world's place among the effect's emitters.
std::size_t world_of(const Prepared& ready) {
    const std::vector<ajfx::Emitter>& emitters = ready.compiled->effect->emitters;
    for (std::size_t e = 0U; e < emitters.size(); ++e) {
        if (emitters[e].name == ready.targets.emitter) {
            return e;
        }
    }
    throw std::runtime_error{"the scene has no solid world"};
}

Prepared prepared(const Document& document) {
    Prepared ready = prepare_scene(document);
    if (!ready.compiled) {
        throw std::runtime_error{ready.error};
    }
    return ready;
}

BakeRecord baked(const Document& document, const Prepared& ready, const ajfx::RigidSolver& solver,
                 std::uint32_t seed) {
    ajfx::Result<BakeRecord> record = bake_page(document, *ready.compiled, seed, &solver);
    if (!record) {
        throw std::runtime_error{record.error().message};
    }
    return std::move(record).value();
}

std::shared_ptr<const ajfx::Cache> as_baked(const BakeRecord& record, const Prepared& ready) {
    const Compiled& compiled = *ready.compiled;
    ajfx::Result<ajfx::Cache> cache =
        ajfx::Cache::open_as_baked(record.bytes, *compiled.effect, compiled.meshes, compiled.fields);
    if (!cache) {
        throw std::runtime_error{cache.error().message};
    }
    return std::make_shared<const ajfx::Cache>(std::move(cache).value());
}

// The effect played from its cache, seeking straight to `step`, as a node
// does with nothing but the package and its cache.
ajfx::Simulation from_cache(const ajfx::Effect& effect, const ajfx::MeshSet& meshes, const ajfx::FieldSet& fields,
                            std::shared_ptr<const ajfx::Cache> cache, std::uint64_t step) {
    const std::uint32_t seed = cache->identity().instance_seed;
    const ajfx::ActionProvider* performance = cache->performance().get();
    ajfx::Simulation simulation =
        ajfx::Simulation::create(effect, seed,
                                 ajfx::SimulationOptions{.fields = fields, .meshes = meshes, .cache = std::move(cache)})
            .value();
    simulation.reset(nullptr, performance);
    simulation.advance_to(step, nullptr, performance);
    return simulation;
}

void a_performed_wall_plays_from_its_cache_at_a_seek() {
    Wall scene = thrown_at_the_wall();
    const Prepared ready = prepared(scene.document);
    auto solver = ajfx::rigid::make_jolt_solver();
    const BakeRecord record = baked(scene.document, ready, *solver, 77U);
    const std::shared_ptr<const ajfx::Cache> cache = as_baked(record, ready);
    // The cache is placed as it was baked, and carries the throw.
    CHECK(cache->identity().instance_seed == 77U);
    CHECK(cache->performance()->size() == 1U);
    const std::size_t world = world_of(ready);
    CHECK(cache->mode(world) == ajfx::CacheMode::Rigid);

    // Played from the cache with no solver, seeking straight there, it holds
    // what the scene plays live with that seed: the ball in flight, and the
    // wall in pieces.
    for (const std::uint64_t step : {25U, 150U, 300U}) {
        const ajfx::Simulation delivered = from_cache(*ready.compiled->effect, ready.compiled->meshes,
                                                      ready.compiled->fields, cache, step);
        const Played live = play_scene(scene.document, ready, solver.get(), 77U, step);
        CHECK(live.error.empty());
        const ajfx::EmitterState& a = delivered.emitter(world);
        const ajfx::EmitterState& b = live.simulation->emitter(world);
        CHECK(a.alive() == b.alive() && a.spawn_index == b.spawn_index);
        CHECK(a.position == b.position && a.orientation == b.orientation);
    }
    const Played broken = play_scene(scene.document, ready, solver.get(), 77U, 150U);
    std::size_t pieces = 0U;
    for (const WorldBody& body : world_bodies(broken, ready)) {
        pieces += body.ref ? 0U : 1U;
    }
    std::cout << "  " << pieces << " pieces of the wall, played from the cache as live\n";
    CHECK(pieces == 8U);
}

#if AJFX_STUDIO_TEST_KERNELS
ajfx::gpu::KernelSet kernels_of(const ajfx::Effect& effect) {
    auto compiler = ajfx::d3d12::create_shader_compiler();
    CHECK(compiler.ok());
    ajfx::Result<ajfx::gpu::KernelSet> kernels = ajfx::gpu::compile_kernels(effect, *compiler.value());
    CHECK(kernels.ok());
    return std::move(kernels).value();
}

void a_delivery_is_checked_as_the_host_checks_it() {
    Wall scene = thrown_at_the_wall();
    const Prepared ready = prepared(scene.document);
    auto solver = ajfx::rigid::make_jolt_solver();
    const BakeRecord record = baked(scene.document, ready, *solver, 5U);
    const ajfx::gpu::KernelSet kernels = kernels_of(*ready.compiled->effect);

    // Delivered, the package reads back and the cache opens against it as
    // the host opens it.
    ajfx::Result<Delivery> delivery = deliver(scene.document, *ready.compiled, kernels, &record);
    CHECK(delivery.ok());
    CHECK(delivery.value().played != nullptr && delivery.value().cache == record.bytes);
    CHECK(delivery.value().played->identity().instance_seed == 5U);
    const std::size_t world = world_of(ready);
    const ajfx::Simulation played = from_cache(delivery.value().effect, delivery.value().meshes,
                                               delivery.value().fields, delivery.value().played, 200U);
    const Played live = play_scene(scene.document, ready, solver.get(), 5U, 200U);
    CHECK(played.emitter(world).position == live.simulation->emitter(world).position);

    // Written side by side.
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "ajfx_delivery_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    std::string error;
    CHECK(write_delivery(delivery.value(), directory / "wall.ajfx", error));
    CHECK(std::filesystem::file_size(directory / "wall.ajfx") == delivery.value().package.size());
    CHECK(std::filesystem::file_size(directory / "wall.ajfxcache") == record.bytes.size());

    // No bake, or a bake of another performance: refused, saying so.
    const ajfx::Result<Delivery> none = deliver(scene.document, *ready.compiled, kernels, nullptr);
    CHECK(!none.ok() && none.error().message.find("bake") != std::string::npos);
    Document thrown_again = scene.document;
    add_push(thrown_again.scene->performance, 60U, BodyRef{.object = scene.ball, .count = 0U}, {0.0F, 5.0F, 0.0F},
             {});
    const ajfx::Result<Delivery> stale = deliver(thrown_again, *ready.compiled, kernels, &record);
    CHECK(!stale.ok() && stale.error().message.find("out of date") != std::string::npos);
    // An act after the cache's last step is not what it holds: it still
    // delivers.
    Document later = scene.document;
    add_push(later.scene->performance, record.frames + 30U, BodyRef{.object = scene.ball, .count = 0U},
             {0.0F, 5.0F, 0.0F}, {});
    CHECK(deliver(later, *ready.compiled, kernels, &record).ok());
    std::filesystem::remove_all(directory);
}
#endif

// The floats of a glTF accessor, read back out of its own buffer.
std::vector<float> floats(const json::Value& gltf, const std::vector<std::uint8_t>& bin, std::size_t accessor) {
    const json::Value& a = gltf.find("accessors")->items[accessor];
    const json::Value& view = gltf.find("bufferViews")->items[static_cast<std::size_t>(a.number_or("bufferView", 0))];
    const std::size_t offset = static_cast<std::size_t>(view.number_or("byteOffset", 0));
    const std::size_t length = static_cast<std::size_t>(view.number_or("byteLength", 0));
    CHECK(offset + length <= bin.size() && length % 4U == 0U);
    std::vector<float> out(length / 4U);
    std::memcpy(out.data(), bin.data() + offset, length);
    return out;
}

void a_performance_is_written_as_animated_gltf() {
    Wall scene = thrown_at_the_wall();
    const Prepared ready = prepared(scene.document);
    auto solver = ajfx::rigid::make_jolt_solver();
    const BakeRecord record = baked(scene.document, ready, *solver, 0U);
    const std::shared_ptr<const ajfx::Cache> cache = as_baked(record, ready);
    const Compiled& compiled = *ready.compiled;
    const std::size_t world = world_of(ready);
    GltfOptions options;
    options.name = [&](std::size_t, std::uint32_t spawn) -> std::string {
        const std::optional<BodyRef> ref = body_ref(ready.targets, spawn);
        return ref && ref->object == scene.ball ? "ball" : ref && ref->object == scene.wall ? "wall" : "";
    };
    const GltfFile file = gltf_from_cache(*compiled.effect, *cache, compiled.meshes, options, "wall.bin");
    CHECK(file.error.empty());
    // The ball, the wall and its eight pieces; the ground is a prop.
    CHECK(file.bodies == 2U && file.pieces == 8U && file.props == 1U && file.frames == record.frames);

    json::ParseError error;
    const std::optional<json::Value> gltf = json::parse(file.json, error);
    CHECK(gltf.has_value());
    CHECK(gltf->find("asset")->string_or("version", "") == "2.0");
    CHECK(static_cast<std::size_t>(gltf->find("buffers")->items[0].number_or("byteLength", 0)) == file.bin.size());
    // Every accessor fits its view.
    for (const json::Value& a : gltf->find("accessors")->items) {
        const json::Value& view = gltf->find("bufferViews")->items[static_cast<std::size_t>(a.number_or("bufferView", 0))];
        const std::string type = a.string_or("type", "");
        const std::size_t width = type == "SCALAR" ? 1U : type == "VEC2" ? 2U : type == "VEC3" ? 3U : 4U;
        CHECK(static_cast<std::size_t>(a.number_or("count", 0)) * width * 4U ==
              static_cast<std::size_t>(view.number_or("byteLength", -1)));
    }

    // The ball's translation keys are where the cache has it, step by step,
    // at the simulation rate; the wall's pieces are a group under it.
    const json::Value& nodes = *gltf->find("nodes");
    std::optional<std::size_t> ball;
    std::optional<std::size_t> wall_group;
    for (std::size_t n = 0U; n < nodes.items.size(); ++n) {
        if (nodes.items[n].string_or("name", "") == "ball") {
            ball = n;
        }
        if (nodes.items[n].string_or("name", "") == "wall") {
            wall_group = n;
        }
    }
    CHECK(ball && wall_group);
    CHECK(nodes.items[*wall_group].find("children")->items.size() == 9U);
    const json::Value& animation = gltf->find("animations")->items[0];
    std::optional<std::size_t> sampler;
    for (const json::Value& channel : animation.find("channels")->items) {
        const json::Value& target = *channel.find("target");
        if (static_cast<std::size_t>(target.number_or("node", -1)) == *ball &&
            target.string_or("path", "") == "translation") {
            sampler = static_cast<std::size_t>(channel.number_or("sampler", -1));
        }
    }
    CHECK(sampler.has_value());
    const json::Value& s = animation.find("samplers")->items[*sampler];
    const std::vector<float> times = floats(*gltf, file.bin, static_cast<std::size_t>(s.number_or("input", 0)));
    const std::vector<float> keys = floats(*gltf, file.bin, static_cast<std::size_t>(s.number_or("output", 0)));
    CHECK(times.size() == record.frames && keys.size() == 3U * times.size());
    const std::uint32_t ball_spawn = *body_index(ready.targets, BodyRef{.object = scene.ball, .count = 0U});
    for (const std::uint64_t step : {0U, 21U, 40U, 200U}) {
        CHECK(times[step] == static_cast<float>(step) / 60.0F);
        const ajfx::EmitterState state = cache->emitter_at(world, step);
        bool found = false;
        for (std::size_t i = 0U; i < state.alive(); ++i) {
            if (state.spawn_index[i] == ball_spawn) {
                found = true;
                CHECK(keys[3U * step] == state.position[i].x && keys[3U * step + 1U] == state.position[i].y &&
                      keys[3U * step + 2U] == state.position[i].z);
            }
        }
        CHECK(found);
    }
    // A cache of no solid layer has nothing to write.
    std::cout << "  " << file.bodies << " bodies, " << file.pieces << " pieces, " << file.bin.size()
              << " bytes of keys and models\n";
}

} // namespace

void add_delivery_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"a_performed_wall_plays_from_its_cache_at_a_seek", a_performed_wall_plays_from_its_cache_at_a_seek});
#if AJFX_STUDIO_TEST_KERNELS
    cases.push_back({"a_delivery_is_checked_as_the_host_checks_it", a_delivery_is_checked_as_the_host_checks_it});
#endif
    cases.push_back({"a_performance_is_written_as_animated_gltf", a_performance_is_written_as_animated_gltf});
}
