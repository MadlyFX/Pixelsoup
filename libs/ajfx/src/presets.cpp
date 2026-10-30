#include <ajfx/presets.hpp>

#include <ajfx/builder.hpp>
#include <ajfx/random.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace ajfx::presets {
namespace {

// Presets are fixed, tested data; a failure here is a bug in this file.
Effect finish(const EffectBuilder& builder) {
    Result<Effect> effect = builder.build();
    return effect ? std::move(effect).value() : Effect{};
}

// Smooth value noise in [0, 1] on a lattice of `cells` per unit.
float value_noise(float x, float y, float cells, std::uint32_t seed) {
    const float fx = x * cells;
    const float fy = y * cells;
    const float ix = std::floor(fx);
    const float iy = std::floor(fy);
    const auto corner = [&](float cx, float cy) {
        return hash_unit(hash_mix(hash_mix(seed, static_cast<std::uint32_t>(static_cast<std::int32_t>(cx) + 4096)),
                                  static_cast<std::uint32_t>(static_cast<std::int32_t>(cy) + 4096)));
    };
    const auto smooth = [](float t) { return t * t * (3.0F - 2.0F * t); };
    const float tx = smooth(fx - ix);
    const float ty = smooth(fy - iy);
    const float top = corner(ix, iy) + (corner(ix + 1.0F, iy) - corner(ix, iy)) * tx;
    const float bottom = corner(ix, iy + 1.0F) + (corner(ix + 1.0F, iy + 1.0F) - corner(ix, iy + 1.0F)) * tx;
    return top + (bottom - top) * ty;
}

// Four 64-texel cells: a dense puff that loosens and spreads with each one.
TextureData puff() {
    constexpr std::uint32_t cell = 64U;
    constexpr std::uint32_t size = 2U * cell;
    std::vector<std::uint8_t> pixels(std::size_t{size} * size * 4U);
    for (std::uint32_t frame = 0U; frame < 4U; ++frame) {
        const float spread = static_cast<float>(frame) / 3.0F;
        const std::uint32_t ox = (frame % 2U) * cell;
        const std::uint32_t oy = (frame / 2U) * cell;
        for (std::uint32_t y = 0U; y < cell; ++y) {
            for (std::uint32_t x = 0U; x < cell; ++x) {
                const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(cell) * 2.0F - 1.0F;
                const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(cell) * 2.0F - 1.0F;
                const float lumps = 0.6F * value_noise(u, v, 3.0F, 11U + frame) +
                                    0.4F * value_noise(u, v, 7.0F, 23U + frame);
                const float radius = std::sqrt(u * u + v * v) / (0.75F + 0.2F * lumps);
                const float body = std::clamp(1.0F - radius, 0.0F, 1.0F);
                const float alpha = body * body * (0.55F + 0.45F * lumps) * (1.0F - 0.35F * spread);
                const float shade = 0.85F + 0.15F * lumps;
                std::uint8_t* texel = pixels.data() + (std::size_t{oy + y} * size + ox + x) * 4U;
                texel[0] = texel[1] = texel[2] = static_cast<std::uint8_t>(std::lround(shade * 255.0F));
                texel[3] = static_cast<std::uint8_t>(std::lround(std::clamp(alpha, 0.0F, 1.0F) * 255.0F));
            }
        }
    }
    Result<TextureData> texture = make_texture(size, size, pixels);
    return texture ? std::move(texture).value() : TextureData{};
}

} // namespace

TextureSet textures() {
    TextureSet set;
    set.emplace("puff", puff());
    return set;
}

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
        .init(InitSize{.min = 0.02F, .max = 0.04F})
        .update(Gravity{})
        .update(Drag{.coefficient = 0.4F})
        .update(PlaneCollision{.restitution = 0.35F, .friction = 0.2F})
        .update(ColorOverLife{.keys = {{0.0F, Color{1.0F, 1.0F, 1.0F, 1.0F}}, {1.0F, Color{0.4F, 0.1F, 0.0F, 0.0F}}}})
        .renderer(SpriteRenderer{.blend = BlendMode::Additive, .facing = Facing::Velocity, .sort = false,
                                 .stretch = 0.04F});
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
        .update(SizeOverLife{.keys = {{0.0F, 0.5F}, {1.0F, 2.5F}}})
        .renderer(SpriteRenderer{.texture = "puff", .flipbook_columns = 2U, .flipbook_rows = 2U,
                                 .flipbook_blend = true, .soft_distance = 0.4F});
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
        .init(InitSize{.min = 0.06F, .max = 0.1F})
        .init(InitRotation{})
        .update(Gravity{.acceleration = Vec3{0.0F, -4.0F, 0.0F}})
        .update(Drag{.coefficient = 1.5F})
        .update(CurlNoise{.strength = 3.0F, .frequency = 0.8F, .evolution = 0.5F, .seed = 11U})
        .update(ColorOverLife{.keys = {{0.0F, Color{}}, {0.8F, Color{}}, {1.0F, Color{1.0F, 1.0F, 1.0F, 0.0F}}}});
    return finish(builder);
}

} // namespace ajfx::presets
