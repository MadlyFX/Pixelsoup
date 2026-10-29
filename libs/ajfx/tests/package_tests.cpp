#include "check.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/package.hpp>
#include <ajfx/presets.hpp>

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

bool fails_with(const Result<Effect>& result, ErrorCode code, const std::string& fragment) {
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
    };
    return ajfx::test::run(cases);
}
