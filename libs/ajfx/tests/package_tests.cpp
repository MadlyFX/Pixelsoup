#include "check.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/field.hpp>
#include <ajfx/mesh.hpp>
#include <ajfx/package.hpp>
#include <ajfx/presets.hpp>
#include <ajfx/simulation.hpp>
#include <ajfx/texture.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace ajfx;

std::vector<std::uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

// FNV-1a 64, written out here rather than shared with the library so a
// change to the container's checksum is caught.
void reseal(std::vector<std::uint8_t>& package) {
    package.resize(package.size() - 8U);
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const std::uint8_t byte : package) {
        hash = (hash ^ byte) * 0x100000001b3ULL;
    }
    for (int i = 0; i < 8; ++i) {
        package.push_back(static_cast<std::uint8_t>(hash >> (8 * i)));
    }
}

template <class T>
bool fails_with(const Result<T>& result, ErrorCode code, const std::string& fragment) {
    return !result.ok() && result.error().code == code && result.error().message.find(fragment) != std::string::npos;
}

void presets_round_trip_to_identical_effects_and_bytes() {
    for (const Effect& effect : {presets::sparks(), presets::smoke(), presets::confetti()}) {
        const Result<std::vector<std::uint8_t>> first = write_package(effect);
        AJFX_CHECK(first.ok());
        const Result<Effect> read = read_package(first.value());
        AJFX_CHECK(read.ok());
        AJFX_CHECK(read.value() == effect);
        const Result<std::vector<std::uint8_t>> second = write_package(read.value());
        AJFX_CHECK(second.ok() && second.value() == first.value());
    }
}

void floats_survive_exactly() {
    EffectBuilder builder{"floats"};
    builder.float_parameter("p", 0.1F, -3.4e38F, 3.4e38F);
    builder.emitter("e")
        .capacity(100000U)
        .spawn(SpawnRate{.rate = 0.3F})
        .init(Lifetime{.min = 1.0e-7F, .max = 123456.789F})
        .init(Velocity{.direction = Vec3{-0.0F, 1.0F / 3.0F, 2.0F / 7.0F}, .speed_min = bind("p")});
    const Effect effect = builder.build().value();
    const Result<Effect> read = effect_from_json(effect_to_json(effect));
    AJFX_CHECK(read.ok());
    AJFX_CHECK(read.value() == effect);
    const Velocity& velocity = std::get<Velocity>(read.value().emitters[0].init[1]);
    AJFX_CHECK(velocity.direction.value.y == 1.0F / 3.0F);
    AJFX_CHECK(velocity.speed_min.parameter == "p");
}

void hand_written_json_fills_in_defaults() {
    const Result<Effect> effect = effect_from_json(R"({
        "format": 1,
        "name": "hand",
        "parameters": [{"name": "tint", "type": "color", "default": [1, 0.5, 0.25, 1], "min": 0, "max": 2}],
        "emitters": [{
            "name": "e",
            "spawn": [{"type": "spawn_burst", "id": 4, "count": 20}],
            "init": [{"type": "lifetime", "id": 5, "max": 3},
                     {"type": "color", "id": 6, "a": {"parameter": "tint"}}],
            "renderer": {"type": "sprite", "blend": "additive"}
        }]
    })");
    AJFX_CHECK(effect.ok());
    const Effect& e = effect.value();
    AJFX_CHECK(e.simulation_rate == 60U);
    AJFX_CHECK(e.parameters[0].default_value[2] == 0.25F);
    const Emitter& emitter = e.emitters[0];
    AJFX_CHECK(emitter.capacity == 1024U);
    AJFX_CHECK(std::get<SpawnBurst>(emitter.spawn[0]).count == 20U);
    AJFX_CHECK(std::get<SpawnBurst>(emitter.spawn[0]).id == 4U);
    AJFX_CHECK(std::get<Lifetime>(emitter.init[0]).min.value == 1.0F);
    AJFX_CHECK(std::get<Lifetime>(emitter.init[0]).max.value == 3.0F);
    AJFX_CHECK(std::get<InitColor>(emitter.init[1]).a.parameter == "tint");
    AJFX_CHECK(emitter.renderer.blend == BlendMode::Additive);
}

void malformed_json_is_reported_with_its_path() {
    const std::string head = R"({"format": 1, "name": "x", "emitters": [{"name": "e", )";
    AJFX_CHECK(fails_with(effect_from_json(head + R"("init": [{"type": "lifetime", "id": 1, "mn": 2}]}]})"),
                          ErrorCode::MalformedPackage, "emitters[0].init[0] has unknown field 'mn'"));
    AJFX_CHECK(fails_with(effect_from_json(head + R"("init": [{"type": "gravity", "id": 1}]}]})"),
                          ErrorCode::MalformedPackage, "unknown module type 'gravity' for this stack"));
    AJFX_CHECK(fails_with(effect_from_json(head + R"("init": [{"type": "lifetime", "id": 1, "min": "2"}]}]})"),
                          ErrorCode::MalformedPackage, "emitters[0].init[0].min must be a number"));
    AJFX_CHECK(fails_with(effect_from_json(R"({"format": 1, "name": )"), ErrorCode::MalformedPackage, "at byte"));
    AJFX_CHECK(fails_with(effect_from_json(R"({"name": "x"})"), ErrorCode::MalformedPackage, "no format version"));
    AJFX_CHECK(fails_with(effect_from_json(R"({"format": 2, "name": "x"})"), ErrorCode::UnsupportedVersion,
                          "format 2 is not supported"));
    // Valid syntax, but the effect itself is refused.
    AJFX_CHECK(fails_with(effect_from_json(head + R"("capacity": 5, "spawn": [{"type": "spawn_burst", "id": 1,
                          "count": 6}], "init": [{"type": "lifetime", "id": 2}]}]})"),
                          ErrorCode::BudgetExceeded, "capacity is 5"));
}

void container_layout_is_fixed() {
    const std::vector<std::uint8_t> bytes =
        write_container({{"b", bytes_of("2")}, {"a", bytes_of("1")}});
    const std::vector<std::uint8_t> expected_body = {
        'A', 'J', 'F', 'X', 1, 0, 0, 0, 2, 0, 0, 0,     // magic, version, two entries
        1,   0,   0,   0,   'a', 1, 0, 0, 0, 0, 0, 0, 0, '1', // "a", sorted first
        1,   0,   0,   0,   'b', 1, 0, 0, 0, 0, 0, 0, 0, '2',
    };
    AJFX_CHECK(bytes.size() == expected_body.size() + 8U);
    AJFX_CHECK(std::vector<std::uint8_t>(bytes.begin(), bytes.end() - 8) == expected_body);
    std::vector<std::uint8_t> resealed = bytes;
    reseal(resealed);
    AJFX_CHECK(resealed == bytes);

    const Result<std::vector<PackageEntry>> entries = read_container(bytes);
    AJFX_CHECK(entries.ok() && entries.value().size() == 2U);
    AJFX_CHECK(entries.value()[0].name == "a" && entries.value()[1].data == bytes_of("2"));
}

void damaged_packages_are_refused() {
    const std::vector<std::uint8_t> good = write_package(presets::sparks()).value();

    std::vector<std::uint8_t> flipped = good;
    flipped[good.size() / 2U] ^= 0x01U;
    AJFX_CHECK(fails_with(read_package(flipped), ErrorCode::MalformedPackage, "checksum"));

    const std::vector<std::uint8_t> truncated(good.begin(), good.begin() + 20);
    AJFX_CHECK(fails_with(read_package(truncated), ErrorCode::MalformedPackage, "checksum"));

    AJFX_CHECK(fails_with(read_package(bytes_of("not a package")), ErrorCode::MalformedPackage,
                          "not an ajfx package"));

    std::vector<std::uint8_t> future = good;
    future[4] = 2U;
    reseal(future);
    AJFX_CHECK(fails_with(read_package(future), ErrorCode::UnsupportedVersion, "container version 2"));

    // A consistent container whose entry claims more data than there is.
    std::vector<std::uint8_t> overlong = write_container({{"effect.json", bytes_of("{}")}});
    overlong[4 + 4 + 4 + 4 + 11] = 0xFFU;
    reseal(overlong);
    AJFX_CHECK(fails_with(read_package(overlong), ErrorCode::MalformedPackage, "truncated"));

    AJFX_CHECK(fails_with(read_package(write_container({{"other", bytes_of("{}")}})), ErrorCode::MalformedPackage,
                          "no effect.json"));
}

void invalid_effects_are_not_written() {
    Effect effect = presets::sparks();
    effect.emitters[0].capacity = 1U;
    const Result<std::vector<std::uint8_t>> written = write_package(effect);
    AJFX_CHECK(!written.ok() && written.error().code == ErrorCode::BudgetExceeded);
    Effect soft = presets::sparks();
    soft.emitters[0].renderer.soft_distance = -1.0F;
    AJFX_CHECK(fails_with(write_package(soft), ErrorCode::InvalidEffect, "field 'soft_distance' must not be negative"));
}

// A 4x2 texture: a red left half, a blue right half.
TextureData halves(ColorSpace space = ColorSpace::Srgb) {
    std::vector<std::uint8_t> pixels;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            const bool left = x < 2;
            pixels.insert(pixels.end(), {static_cast<std::uint8_t>(left ? 255 : 0), 0,
                                         static_cast<std::uint8_t>(left ? 0 : 255), 255});
        }
    }
    return make_texture(4U, 2U, pixels, space).value();
}

void textures_carry_a_full_mip_chain() {
    const TextureData texture = halves(ColorSpace::Linear);
    // 4x2, 2x1, 1x1.
    AJFX_CHECK(texture.mips.size() == 3U);
    AJFX_CHECK(texture.mips[1].size() == 8U && texture.mips[2].size() == 4U);
    // Level 1 keeps the halves apart; level 2 averages them.
    AJFX_CHECK(texture.mips[1][0] == 255 && texture.mips[1][2] == 0 && texture.mips[1][4] == 0 &&
               texture.mips[1][6] == 255);
    AJFX_CHECK(texture.mips[2][0] == 128 && texture.mips[2][2] == 128 && texture.mips[2][3] == 255);
    // sRGB averages in linear light: half of full red is about 188 encoded.
    const TextureData srgb = halves();
    AJFX_CHECK(srgb.mips[2][0] >= 186 && srgb.mips[2][0] <= 189);
    // Clear texels do not darken the average.
    const std::vector<std::uint8_t> edge{255, 255, 255, 255, 0, 0, 0, 0};
    AJFX_CHECK(make_texture(2U, 1U, edge, ColorSpace::Linear).value().mips[1] ==
               (std::vector<std::uint8_t>{255, 255, 255, 128}));
    AJFX_CHECK(fails_with(make_texture(0U, 1U, {}), ErrorCode::InvalidEffect, "1 to 4096"));
    AJFX_CHECK(fails_with(make_texture(2U, 2U, edge), ErrorCode::InvalidEffect, "do not match"));
}

void textures_round_trip_through_packages() {
    Effect effect = presets::smoke();
    const TextureSet textures = presets::textures();
    std::vector<PackageEntry> extra = texture_package_entries(textures);
    AJFX_CHECK(extra.size() == 1U && extra[0].name == "textures/puff");
    const Result<std::vector<std::uint8_t>> package = write_package(effect, extra);
    AJFX_CHECK(package.ok());
    const Result<TextureSet> read = read_package_textures(package.value(), effect);
    AJFX_CHECK(read.ok() && read.value() == textures);
    // Written bytes are the same every time.
    AJFX_CHECK(write_package(effect, texture_package_entries(presets::textures())).value() == package.value());
}

void texture_problems_are_named() {
    const Effect smoke = presets::smoke();
    // Missing from the package.
    AJFX_CHECK(fails_with(read_package_textures(write_package(smoke).value(), smoke), ErrorCode::MissingTexture,
                          "emitter 'smoke' uses texture 'puff'"));
    // Damaged.
    std::vector<std::uint8_t> bad = write_texture(halves());
    bad.pop_back();
    AJFX_CHECK(fails_with(read_texture(bad), ErrorCode::MalformedPackage, "shorter than its mips"));
    AJFX_CHECK(fails_with(read_package_textures(write_package(smoke, {{"textures/puff", bad}}).value(), smoke),
                          ErrorCode::MalformedPackage, "'textures/puff'"));
    std::vector<std::uint8_t> newer = write_texture(halves());
    newer[4] = 2U;
    AJFX_CHECK(fails_with(read_texture(newer), ErrorCode::UnsupportedVersion, "texture version 2"));
    // A flipbook with more cells than texels.
    Effect crowded = smoke;
    crowded.emitters[0].renderer.flipbook_columns = 5U;
    AJFX_CHECK(fails_with(check_textures(crowded, TextureSet{{"puff", halves()}}), ErrorCode::InvalidEffect,
                          "5x2 flipbook"));
    // Names and flipbooks are checked with the effect.
    Effect named = smoke;
    named.emitters[0].renderer.texture = "../puff";
    AJFX_CHECK(fails_with(write_package(named),
                          ErrorCode::InvalidEffect, "field 'texture'"));
    Effect plain = presets::sparks();
    plain.emitters[0].renderer.flipbook_rows = 2U;
    AJFX_CHECK(fails_with(write_package(plain), ErrorCode::InvalidEffect, "needs a texture"));
}

// A field of distance to the plane y = 0 over a 4x4x4 grid a unit apart.
DistanceField floor_field() {
    return make_distance_field(4U, 4U, 4U, Vec3{-1.5F, -1.5F, -1.5F}, 1.0F, [](Vec3 p) { return p.y; }).value();
}

Effect floored() {
    EffectBuilder builder{"floored"};
    builder.emitter("e")
        .spawn(SpawnRate{.rate = 10.0F})
        .init(Lifetime{.min = 1.0F, .max = 1.0F})
        .update(SdfCollision{.field = "floor"});
    return builder.build().value();
}

void distance_fields_round_trip_and_sample() {
    const DistanceField field = floor_field();
    AJFX_CHECK(read_field(write_field(field)).value() == field);
    // Between samples: the interpolated distance and its gradient.
    const std::optional<FieldSample> sample = sample_field(field, Vec3{0.25F, -0.75F, 0.5F});
    AJFX_CHECK(sample && sample->distance == -0.75F);
    AJFX_CHECK(sample->gradient == (Vec3{0.0F, 1.0F, 0.0F}));
    // Outside the grid there is nothing.
    AJFX_CHECK(!sample_field(field, Vec3{0.0F, 2.0F, 0.0F}));
    const Effect effect = floored();
    const FieldSet fields{{"floor", field}};
    const Result<std::vector<std::uint8_t>> package = write_package(effect, field_package_entries(fields));
    AJFX_CHECK(package.ok());
    AJFX_CHECK(read_package_fields(package.value(), effect).value() == fields);
}

void distance_field_problems_are_named() {
    const Effect effect = floored();
    AJFX_CHECK(fails_with(read_package_fields(write_package(effect).value(), effect), ErrorCode::MissingField,
                          "emitter 'e' collides with field 'floor'"));
    std::vector<std::uint8_t> short_field = write_field(floor_field());
    short_field.pop_back();
    AJFX_CHECK(fails_with(read_field(short_field), ErrorCode::MalformedPackage, "not as long as its grid"));
    std::vector<std::uint8_t> newer = write_field(floor_field());
    newer[4] = 2U;
    AJFX_CHECK(fails_with(read_field(newer), ErrorCode::UnsupportedVersion, "distance field version 2"));
    AJFX_CHECK(fails_with(make_distance_field(1U, 4U, 4U, Vec3{}, 1.0F, [](Vec3) { return 0.0F; }),
                          ErrorCode::InvalidEffect, "2 to 256"));
    AJFX_CHECK(fails_with(Simulation::create(effect), ErrorCode::MissingField, "field 'floor'"));
    Effect unnamed = effect;
    std::get<SdfCollision>(unnamed.emitters[0].update[0]).field = "Floor/1";
    AJFX_CHECK(fails_with(write_package(unnamed), ErrorCode::InvalidEffect, "field 'field'"));
}

Effect boxes() {
    EffectBuilder builder{"boxes"};
    builder.emitter("e")
        .spawn(SpawnRate{.rate = 10.0F})
        .init(Lifetime{.min = 1.0F, .max = 1.0F})
        .renderer(SpriteRenderer{.geometry = Geometry::Mesh, .mesh = "cube"});
    return builder.build().value();
}

void meshes_round_trip_and_are_checked() {
    const MeshData cube = cube_mesh();
    AJFX_CHECK(cube.vertices.size() == 24U && cube.indices.size() == 36U);
    AJFX_CHECK(make_mesh(cube.vertices, cube.indices).ok());
    AJFX_CHECK(read_mesh(write_mesh(cube)).value() == cube);
    const Effect effect = boxes();
    const MeshSet meshes{{"cube", cube}};
    const Result<std::vector<std::uint8_t>> package = write_package(effect, mesh_package_entries(meshes));
    AJFX_CHECK(package.ok() && read_package_meshes(package.value(), effect).value() == meshes);

    AJFX_CHECK(fails_with(read_package_meshes(write_package(effect).value(), effect), ErrorCode::MissingMesh,
                          "emitter 'e' draws mesh 'cube'"));
    AJFX_CHECK(fails_with(make_mesh(cube.vertices, {0U, 1U, 99U}), ErrorCode::InvalidEffect, "index past"));
    AJFX_CHECK(fails_with(make_mesh(cube.vertices, {0U, 1U}), ErrorCode::InvalidEffect, "whole triangles"));
    std::vector<std::uint8_t> short_mesh = write_mesh(cube);
    short_mesh.pop_back();
    AJFX_CHECK(fails_with(read_mesh(short_mesh), ErrorCode::MalformedPackage, "not as long"));
    std::vector<std::uint8_t> newer = write_mesh(cube);
    newer[4] = 2U;
    AJFX_CHECK(fails_with(read_mesh(newer), ErrorCode::UnsupportedVersion, "mesh version 2"));
    // A mesh emitter needs its mesh named, and only mesh emitters name one.
    Effect unnamed = effect;
    unnamed.emitters[0].renderer.mesh.clear();
    AJFX_CHECK(fails_with(write_package(unnamed), ErrorCode::InvalidEffect, "field 'mesh'"));
    Effect stray = presets::sparks();
    stray.emitters[0].renderer.mesh = "cube";
    AJFX_CHECK(fails_with(write_package(stray), ErrorCode::InvalidEffect, "only for mesh geometry"));
}

} // namespace

int main() {
    const ajfx::test::Case cases[] = {
        {"presets_round_trip_to_identical_effects_and_bytes", presets_round_trip_to_identical_effects_and_bytes},
        {"floats_survive_exactly", floats_survive_exactly},
        {"hand_written_json_fills_in_defaults", hand_written_json_fills_in_defaults},
        {"malformed_json_is_reported_with_its_path", malformed_json_is_reported_with_its_path},
        {"container_layout_is_fixed", container_layout_is_fixed},
        {"damaged_packages_are_refused", damaged_packages_are_refused},
        {"invalid_effects_are_not_written", invalid_effects_are_not_written},
        {"textures_carry_a_full_mip_chain", textures_carry_a_full_mip_chain},
        {"textures_round_trip_through_packages", textures_round_trip_through_packages},
        {"texture_problems_are_named", texture_problems_are_named},
        {"distance_fields_round_trip_and_sample", distance_fields_round_trip_and_sample},
        {"distance_field_problems_are_named", distance_field_problems_are_named},
        {"meshes_round_trip_and_are_checked", meshes_round_trip_and_are_checked},
    };
    return ajfx::test::run(cases);
}
