#include <ajfx/presets.hpp>

#include <ajfx/builder.hpp>

#include <utility>

namespace ajfx::presets {
namespace {

// Presets are fixed, tested data; a failure here is a bug in this file.
Effect finish(const EffectBuilder& builder) {
    Result<Effect> effect = builder.build();
    return effect ? std::move(effect).value() : Effect{};
}

} // namespace

Effect sparks() {
    EffectBuilder builder{"sparks"};
    builder.float_parameter("rate", 120.0F, 0.0F, 1000.0F);
    builder.color_parameter("color", Color{4.0F, 1.6F, 0.4F, 1.0F}, 0.0F, 16.0F);
    builder.emitter("sparks")
        .capacity(4096U)
        .seed(1U)
        .spawn(SpawnRate{.rate = bind("rate")})
        .init(Lifetime{.min = 0.6F, .max = 1.4F})
        .init(ShapeLocation{.shape = Shape::Sphere, .radius = 0.05F})
        .init(Velocity{.speed_min = 3.0F, .speed_max = 6.0F, .spread = 35.0F})
        .init(InitColor{.a = bind("color"), .b = bind("color")})
        .init(InitSize{.min = 0.01F, .max = 0.025F})
        .update(Gravity{})
        .update(Drag{.coefficient = 0.4F})
        .update(PlaneCollision{.restitution = 0.35F, .friction = 0.2F})
        .update(ColorOverLife{.keys = {{0.0F, Color{1.0F, 1.0F, 1.0F, 1.0F}}, {1.0F, Color{0.4F, 0.1F, 0.0F, 0.0F}}}})
        .renderer(SpriteRenderer{.blend = BlendMode::Additive, .facing = Facing::Velocity, .sort = false});
    return finish(builder);
}

Effect smoke() {
    EffectBuilder builder{"smoke"};
    builder.float_parameter("rate", 30.0F, 0.0F, 200.0F);
    builder.vec3_parameter("wind", Vec3{0.3F, 0.0F, 0.0F}, -10.0F, 10.0F);
    builder.emitter("smoke")
        .capacity(2048U)
        .seed(2U)
        .spawn(SpawnRate{.rate = bind("rate")})
        .init(Lifetime{.min = 4.0F, .max = 6.0F})
        .init(ShapeLocation{.shape = Shape::Disc, .radius = 0.4F})
        .init(Velocity{.speed_min = 0.4F, .speed_max = 0.8F, .spread = 15.0F})
        .init(InitColor{.a = Color{0.5F, 0.5F, 0.5F, 0.6F}, .b = Color{0.7F, 0.7F, 0.7F, 0.4F}})
        .init(InitSize{.min = 0.4F, .max = 0.7F})
        .init(InitRotation{})
        .update(Gravity{.acceleration = bind("wind")})
        .update(CurlNoise{.strength = 0.6F, .frequency = 0.5F, .evolution = 0.2F, .seed = 7U})
        .update(Drag{.coefficient = 0.3F})
        .update(ColorOverLife{.keys = {{0.0F, Color{1.0F, 1.0F, 1.0F, 0.0F}},
                                       {0.15F, Color{1.0F, 1.0F, 1.0F, 1.0F}},
                                       {1.0F, Color{1.0F, 1.0F, 1.0F, 0.0F}}}})
        .update(SizeOverLife{.keys = {{0.0F, 0.5F}, {1.0F, 2.5F}}});
    return finish(builder);
}

Effect confetti() {
    EffectBuilder builder{"confetti"};
    builder.color_parameter("color_a", Color{1.0F, 0.2F, 0.4F, 1.0F});
    builder.color_parameter("color_b", Color{0.2F, 0.6F, 1.0F, 1.0F});
    builder.emitter("confetti")
        .capacity(2048U)
        .seed(3U)
        .spawn(SpawnBurst{.time = 0.0F, .count = 300U, .interval = 2.0F, .cycles = 0U})
        .init(Lifetime{.min = 2.5F, .max = 3.5F})
        .init(ShapeLocation{.shape = Shape::Point})
        .init(Velocity{.speed_min = 4.0F, .speed_max = 8.0F, .spread = 40.0F})
        .init(InitColor{.a = bind("color_a"), .b = bind("color_b")})
        .init(InitSize{.min = 0.03F, .max = 0.05F})
        .init(InitRotation{})
        .update(Gravity{.acceleration = Vec3{0.0F, -4.0F, 0.0F}})
        .update(Drag{.coefficient = 1.5F})
        .update(CurlNoise{.strength = 3.0F, .frequency = 0.8F, .evolution = 0.5F, .seed = 11U})
        .update(ColorOverLife{.keys = {{0.0F, Color{}}, {0.8F, Color{}}, {1.0F, Color{1.0F, 1.0F, 1.0F, 0.0F}}}});
    return finish(builder);
}

} // namespace ajfx::presets
