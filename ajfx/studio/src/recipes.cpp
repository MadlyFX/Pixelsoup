#include "recipes.hpp"

#include "catalog.hpp"

#include <initializer_list>
#include <string>
#include <utility>

namespace ajfx::studio {
namespace {

struct Spec final {
    std::string_view kind;
    std::initializer_list<std::pair<std::string_view, Value>> values;
};

// Sets a block's (or a layer's world's) values from a spec.
void fill(Block& block, const std::vector<std::pair<std::string_view, Value>>& values) {
    const BlockDef* def = find_block_def(block.kind);
    if (def == nullptr) {
        return;
    }
    for (const auto& [control, value] : values) {
        if (const ControlDef* found = find_control(*def, control)) {
            block.set(control, sanitize(*found, value));
        }
    }
}

Layer build_layer(Document& document, LayerKind kind, std::string name, float life_min, float life_max,
                  std::initializer_list<Spec> blocks,
                  std::initializer_list<std::pair<std::string_view, Value>> world = {}) {
    Layer layer = make_layer(document, kind, std::move(name));
    layer.life_min = life_min;
    layer.life_max = life_max;
    if (layer.world) {
        fill(*layer.world, {world.begin(), world.end()});
    }
    for (const Spec& spec : blocks) {
        std::optional<Block> block = make_block(document, spec.kind);
        if (!block) {
            continue;
        }
        fill(*block, {spec.values.begin(), spec.values.end()});
        conform_block(*block, kind);
        layer.blocks.push_back(std::move(*block));
    }
    return layer;
}

Layer starter(Document& d) {
    return build_layer(d, LayerKind::Particles, "Particles", 1.0F, 2.0F,
                       {{"stream", {{"rate", number(40.0F)}}},
                        {"shape", {{"shape", choice("ball")}, {"size", range(0.1F, 0.1F)}}},
                        {"launch", {{"speed", range(0.6F, 1.2F)}, {"spread", range(0.0F, 25.0F)}}},
                        {"appearance", {{"material", choice("glow")}, {"texture", choice("glow")}}},
                        {"color", {{"color", color(0.45F, 0.7F, 1.0F)}, {"brightness", range(3.0F, 3.0F)}}},
                        {"size", {{"size", range(0.05F, 0.1F)}}},
                        {"fade", {}}});
}

Layer flames(Document& d) {
    return build_layer(d, LayerKind::Particles, "Flames", 0.6F, 1.1F,
                       {{"stream", {{"rate", number(90.0F)}}},
                        {"shape", {{"shape", choice("disc")}, {"size", range(0.22F, 0.22F)}}},
                        {"launch", {{"speed", range(0.2F, 0.5F)}, {"spread", range(0.0F, 15.0F)}}},
                        {"rise", {{"strength", range(5.0F, 5.0F)}, {"over_life", choice("cools")}}},
                        {"draw_in", {{"strength", range(6.0F, 6.0F)}, {"height", number(0.6F)}}},
                        {"turbulence",
                         {{"strength", range(1.6F, 1.6F)}, {"size", number(0.3F)}, {"churn", number(0.6F)},
                          {"roll", number(1.6F)}, {"over_life", choice("grows")}}},
                        {"drag", {{"amount", range(1.2F, 1.2F)}}},
                        {"appearance", {{"material", choice("glow")}, {"texture", choice("flame")}, {"spin", toggle(true)}}},
                        {"spin", {{"speed", range(40.0F, 160.0F)}, {"slows", number(0.6F)}}},
                        {"color",
                         {{"color", color(1.0F, 0.62F, 0.3F)}, {"brightness", range(5.0F, 5.0F)}, {"over_life", choice("fire")}}},
                        {"size", {{"size", range(0.38F, 0.58F)}, {"over_life", choice("shrink")}}},
                        {"fade", {{"in", number(8.0F)}, {"out", number(35.0F)}}},
                        {"light", {{"count", number(6.0F)}, {"intensity", range(1.2F, 2.2F)}, {"reach", range(1.8F, 2.8F)}}}});
}

Layer embers(Document& d) {
    return build_layer(d, LayerKind::Particles, "Embers", 1.5F, 3.0F,
                       {{"stream", {{"rate", number(14.0F)}}},
                        {"shape", {{"shape", choice("disc")}, {"size", range(0.2F, 0.2F)}}},
                        {"launch", {{"speed", range(0.8F, 1.8F)}, {"spread", range(0.0F, 25.0F)}}},
                        {"rise", {{"strength", range(0.6F, 0.6F)}}},
                        {"turbulence", {{"strength", range(2.5F, 2.5F)}, {"size", number(0.5F)}, {"churn", number(0.6F)}}},
                        {"drag", {{"amount", range(0.8F, 0.8F)}}},
                        {"appearance",
                         {{"material", choice("glow")}, {"texture", choice("glow")}, {"facing", choice("motion")},
                          {"stretch", number(0.03F)}}},
                        {"color",
                         {{"color", color(1.0F, 0.55F, 0.18F)}, {"brightness", range(8.0F, 8.0F)}, {"over_life", choice("fire")}}},
                        {"size", {{"size", range(0.012F, 0.025F)}}},
                        {"fade", {{"in", number(5.0F)}, {"out", number(30.0F)}}}});
}

Layer fire_smoke(Document& d) {
    return build_layer(d, LayerKind::Particles, "Smoke", 3.5F, 5.5F,
                       {{"stream", {{"rate", number(10.0F)}}},
                        {"shape", {{"shape", choice("disc")}, {"size", range(0.2F, 0.2F)}, {"offset", vec3(0.0F, 0.5F, 0.0F)}}},
                        {"launch", {{"speed", range(0.3F, 0.6F)}, {"spread", range(0.0F, 12.0F)}}},
                        {"rise", {{"strength", range(0.6F, 0.6F)}}},
                        {"wind", {{"strength", range(0.25F, 0.25F)}, {"heading", number(20.0F)}}},
                        {"turbulence",
                         {{"strength", range(0.6F, 0.6F)}, {"size", number(0.9F)}, {"churn", number(0.25F)}, {"roll", number(0.5F)}}},
                        {"drag", {{"amount", range(0.5F, 0.5F)}}},
                        {"appearance",
                         {{"material", choice("smoke")}, {"texture", choice("smoke")}, {"spin", toggle(true)},
                          {"soft", number(0.3F)}}},
                        {"spin", {{"speed", range(8.0F, 35.0F)}, {"slows", number(0.15F)}}},
                        {"color", {{"color", color(0.075F, 0.07F, 0.065F)}, {"opacity", number(0.5F)}}},
                        {"size", {{"size", range(0.5F, 0.8F)}, {"over_life", choice("swell")}, {"change", number(3.5F)}}},
                        {"fade", {{"in", number(15.0F)}, {"out", number(50.0F)}}}});
}

Layer plume(Document& d) {
    Layer layer = fire_smoke(d);
    layer.name = "Plume";
    for (Block& block : layer.blocks) {
        if (block.kind == "stream") {
            block.set("rate", number(16.0F));
        } else if (block.kind == "shape") {
            block.set("offset", vec3(0.0F, 0.05F, 0.0F));
            block.set("size", range(0.3F, 0.3F));
        } else if (block.kind == "color") {
            block.set("color", color(0.55F, 0.54F, 0.52F));
            block.set("opacity", number(0.5F));
        } else if (block.kind == "rise") {
            block.set("strength", range(0.9F, 0.9F));
        }
    }
    return layer;
}

Layer sparks(Document& d) {
    return build_layer(d, LayerKind::Particles, "Sparks", 0.8F, 1.6F,
                       {{"stream", {{"rate", number(140.0F)}}},
                        {"shape", {{"shape", choice("ball")}, {"size", range(0.03F, 0.03F)}}},
                        {"launch", {{"speed", range(3.0F, 6.0F)}, {"spread", range(0.0F, 28.0F)}}},
                        {"gravity", {}},
                        {"drag", {{"amount", range(0.3F, 0.3F)}}},
                        {"ground", {{"bounce", number(0.35F)}, {"friction", number(0.25F)}}},
                        {"appearance",
                         {{"material", choice("glow")}, {"texture", choice("spark")}, {"facing", choice("motion")},
                          {"stretch", number(0.035F)}}},
                        {"color",
                         {{"color", color(1.0F, 0.7F, 0.35F)}, {"brightness", range(10.0F, 10.0F)}, {"over_life", choice("fire")}}},
                        {"size", {{"size", range(0.015F, 0.03F)}}},
                        {"fade", {{"in", number(0.0F)}, {"out", number(25.0F)}}}});
}

Layer spark_burst(Document& d) {
    return build_layer(d, LayerKind::Particles, "Spark burst", 0.6F, 1.4F,
                       {{"burst", {{"count", number(160.0F)}, {"every", number(3.0F)}}},
                        {"shape", {{"shape", choice("ball")}, {"size", range(0.15F, 0.15F)}, {"offset", vec3(0.0F, 0.4F, 0.0F)}}},
                        {"launch", {{"direction", choice("out")}, {"speed", range(4.0F, 9.0F)}}},
                        {"gravity", {}},
                        {"drag", {{"amount", range(1.0F, 1.0F)}}},
                        {"ground", {{"bounce", number(0.3F)}, {"friction", number(0.3F)}}},
                        {"appearance",
                         {{"material", choice("glow")}, {"texture", choice("spark")}, {"facing", choice("motion")},
                          {"stretch", number(0.03F)}}},
                        {"color",
                         {{"color", color(1.0F, 0.75F, 0.4F)}, {"brightness", range(12.0F, 12.0F)}, {"over_life", choice("fire")}}},
                        {"size", {{"size", range(0.015F, 0.03F)}}},
                        {"fade", {{"in", number(0.0F)}, {"out", number(30.0F)}}}});
}

Layer glow_core(Document& d) {
    return build_layer(d, LayerKind::Particles, "Glow core", 0.4F, 0.6F,
                       {{"stream", {{"rate", number(25.0F)}}},
                        {"shape", {{"shape", choice("ball")}, {"size", range(0.05F, 0.05F)}, {"offset", vec3(0.0F, 1.0F, 0.0F)}}},
                        {"launch", {{"speed", range(0.0F, 0.05F)}}},
                        {"appearance", {{"material", choice("glow")}, {"texture", choice("glow")}}},
                        {"color", {{"color", color(0.4F, 0.65F, 1.0F)}, {"brightness", range(4.0F, 4.0F)}}},
                        {"size", {{"size", range(0.5F, 0.7F)}, {"over_life", choice("pop")}}},
                        {"fade", {{"in", number(20.0F)}, {"out", number(60.0F)}}},
                        {"light", {{"count", number(2.0F)}, {"intensity", range(2.5F, 3.5F)}, {"reach", range(2.0F, 3.0F)}}}});
}

Layer magic_motes(Document& d) {
    return build_layer(
        d, LayerKind::Particles, "Motes", 2.0F, 3.0F,
        {{"stream", {{"rate", number(60.0F)}}},
         {"shape", {{"shape", choice("shell")}, {"size", range(0.6F, 0.6F)}, {"offset", vec3(0.0F, 1.0F, 0.0F)}}},
         {"launch", {{"direction", choice("out")}, {"speed", range(0.1F, 0.3F)}}},
         {"swirl", {{"strength", range(4.0F, 4.0F)}, {"center", vec3(0.0F, 1.0F, 0.0F)}}},
         {"attract", {{"strength", range(2.0F, 2.0F)}, {"point", vec3(0.0F, 1.0F, 0.0F)}}},
         {"turbulence", {{"strength", range(0.8F, 0.8F)}, {"size", number(0.4F)}}},
         {"drag", {{"amount", range(1.5F, 1.5F)}}},
         {"appearance", {{"material", choice("glow")}, {"texture", choice("star")}, {"spin", toggle(true)}}},
         {"color",
          {{"color", color(0.45F, 0.6F, 1.0F)}, {"brightness", range(6.0F, 6.0F)}, {"over_life", choice("cool")},
           {"mix", toggle(true)}, {"second", color(0.85F, 0.4F, 1.0F)}}},
         {"size", {{"size", range(0.03F, 0.07F)}, {"over_life", choice("pop")}}},
         {"fade", {{"in", number(20.0F)}, {"out", number(40.0F)}}}});
}

Layer debris(Document& d) {
    return build_layer(d, LayerKind::Particles, "Debris", 2.6F, 3.0F,
                       {{"burst", {{"count", number(50.0F)}, {"every", number(3.0F)}}},
                        {"shape", {{"shape", choice("disc")}, {"size", range(0.15F, 0.15F)}}},
                        {"launch", {{"speed", range(2.0F, 6.0F)}, {"spread", range(0.0F, 35.0F)}}},
                        {"gravity", {}},
                        {"tumble", {{"spin", range(180.0F, 900.0F)}, {"damping", number(1.5F)}, {"contact", number(0.3F)}}},
                        {"ground", {{"bounce", number(0.3F)}, {"friction", number(0.6F)}}},
                        {"appearance", {{"material", choice("debris")}, {"mesh", choice("chip")}}},
                        {"color",
                         {{"color", color(0.2F, 0.19F, 0.17F)}, {"mix", toggle(true)}, {"second", color(0.34F, 0.31F, 0.27F)}}},
                        {"size", {{"size", range(0.06F, 0.16F)}}},
                        {"fade", {{"in", number(0.0F)}, {"out", number(12.0F)}}}});
}

// Already standing when the effect starts; let go at once, so it collapses
// as the effect plays. Physics makes each rock a body the others rest on.
Layer rock_pile(Document& d) {
    return build_layer(d, LayerKind::Rigid, "Rock pile", 60.0F, 60.0F,
                       {{"pile",
                         {{"count", number(220.0F)}, {"slope", number(62.0F)}, {"spacing", number(0.75F)},
                          {"jitter", number(0.25F)}}},
                        {"gravity", {}},
                        // A pile starts still: Tumble only turns each rock to lie its own way.
                        {"tumble", {{"spin", range(0.0F, 0.0F)}}},
                        {"ground", {{"bounce", number(0.1F)}, {"friction", number(0.8F)}}},
                        {"appearance", {{"material", choice("debris")}, {"mesh", choice("rock")}}},
                        {"color",
                         {{"color", color(0.2F, 0.19F, 0.17F)}, {"mix", toggle(true)}, {"second", color(0.36F, 0.33F, 0.29F)}}},
                        {"size", {{"size", range(0.12F, 0.22F)}}}},
                       {{"friction", number(0.8F)}, {"bounce", number(0.1F)}, {"density", number(2600.0F)}});
}

// A bucket of stones that tips over and spills them. The bucket collides as a
// bucket rather than as its own shape, so it can be a solid object the balls
// sit inside; it is moved rather than knocked, so the tip-over is authored
// and lands the same way every time. Measurements agree with the prop's: the
// balls start inside the bucket's floor, 0.05 up, in its 0.45 inside radius.
Layer bucket_spill(Document& d) {
    return build_layer(d, LayerKind::Rigid, "Bucket of stones", 60.0F, 60.0F,
                       {{"prop",
                         {{"motion", choice("driven")},
                          {"shape", choice("bucket")},
                          {"at", vec3(0.0F, 0.4F, 0.0F)},
                          {"radius", number(0.5F)},
                          {"height", number(0.8F)},
                          {"wall", number(0.05F)},
                          {"sides", number(16.0F)},
                          // Past 90 degrees it is upside down and empties.
                          {"turn_to", vec3(0.0F, 0.0F, 115.0F)},
                          {"move_at", number(1.0F)},
                          {"move_over", number(0.6F)},
                          {"friction", number(0.7F)},
                          {"bounce", number(0.05F)}}},
                        {"pile",
                         {{"count", number(24.0F)},
                          {"shape", choice("stack")},
                          {"width", number(0.5F)},
                          {"depth", number(0.5F)},
                          {"at", vec3(0.0F, 0.05F, 0.0F)},
                          {"jitter", number(0.0F)}}},
                        {"gravity", {}},
                        {"ground", {{"bounce", number(0.1F)}, {"friction", number(0.8F)}}},
                        // Drawn as rocks; they collide as balls, so they roll
                        // out of the bucket rather than catching on a corner.
                        {"appearance", {{"material", choice("debris")}, {"mesh", choice("rock")}}},
                        {"color",
                         {{"color", color(0.42F, 0.36F, 0.30F)}, {"mix", toggle(true)},
                          {"second", color(0.58F, 0.50F, 0.42F)}}},
                        {"size", {{"size", range(0.1F, 0.1F)}}}},
                       {{"shape", choice("ball")}, {"friction", number(0.5F)}, {"bounce", number(0.2F)}});
}

// A stack of crates with a ball dropped on them, the crates breaking into
// panels where the ball lands. The crates are the layer's own bodies and the
// ball is a loose prop, so one physics world holds them both. The parts sit
// at the faces of the crate they came from: a lid, two walls and a base, each
// a little under half the crate across, which is what a crate in pieces
// looks like. The ball falls from two and a half metres and arrives at about
// seven, well past the four the crates come apart at.
Layer breaking_crates(Document& d) {
    return build_layer(d, LayerKind::Rigid, "Crates and a dropped ball", 60.0F, 60.0F,
                       {{"pile",
                         {{"count", number(9.0F)},
                          {"shape", choice("stack")},
                          {"width", number(1.2F)},
                          {"depth", number(1.2F)},
                          {"jitter", number(0.0F)}}},
                        {"prop",
                         {{"motion", choice("loose")},
                          {"shape", choice("ball")},
                          {"at", vec3(0.0F, 2.5F, 0.0F)},
                          {"radius", number(0.3F)},
                          {"size", number(0.6F)},
                          {"density", number(1200.0F)},
                          {"friction", number(0.3F)},
                          {"bounce", number(0.8F)}}},
                        {"break", {{"force", number(4.0F)}, {"scatter", number(1.2F)}, {"spin", number(220.0F)}}},
                        {"part", {{"model", choice("cube")}, {"size", number(0.45F)}, {"at", vec3(0.0F, 0.5F, 0.0F)}}},
                        {"part",
                         {{"model", choice("shard")},
                          {"size", number(0.45F)},
                          {"at", vec3(0.5F, 0.0F, 0.0F)},
                          {"turn", vec3(0.0F, 0.0F, 90.0F)}}},
                        {"part",
                         {{"model", choice("shard")},
                          {"size", number(0.45F)},
                          {"at", vec3(-0.5F, 0.0F, 0.0F)},
                          {"turn", vec3(0.0F, 0.0F, 90.0F)}}},
                        {"part", {{"model", choice("cube")}, {"size", number(0.45F)}, {"at", vec3(0.0F, -0.5F, 0.0F)}}},
                        {"gravity", {}},
                        {"ground", {{"bounce", number(0.05F)}, {"friction", number(0.9F)}}},
                        {"appearance", {{"material", choice("debris")}, {"mesh", choice("cube")}}},
                        {"color",
                         {{"color", color(0.44F, 0.30F, 0.16F)}, {"mix", toggle(true)},
                          {"second", color(0.56F, 0.40F, 0.23F)}}},
                        {"size", {{"size", range(0.4F, 0.4F)}}}},
                       {{"shape", choice("cube")},
                        {"friction", number(0.8F)},
                        {"bounce", number(0.05F)},
                        {"density", number(600.0F)}});
}

// Snow falling on whatever the layer above it is made of. It is a particle
// layer, so it sits after the crates on the page: the pieces it lands on
// have to have moved before the flakes meet them, and reading down the page
// is that order. Sticking is what makes it settle rather than skate off.
Layer settling_snow(Document& d) {
    return build_layer(d, LayerKind::Particles, "Snow", 7.0F, 9.0F,
                       {{"stream", {{"rate", number(70.0F)}}},
                        {"shape",
                         {{"shape", choice("disc")}, {"size", range(1.6F, 1.6F)}, {"offset", vec3(0.0F, 4.0F, 0.0F)}}},
                        {"launch", {{"direction", choice("down")}, {"speed", range(0.5F, 0.9F)}}},
                        {"gravity", {{"strength", range(0.04F, 0.06F)}}},
                        {"turbulence", {{"strength", range(0.25F, 0.25F)}, {"size", number(1.2F)}}},
                        {"pieces", {{"response", choice("stick")}}},
                        {"ground", {{"response", choice("stick")}}},
                        {"appearance", {{"material", choice("solid")}, {"texture", choice("dot")}}},
                        {"color", {{"color", color(0.95F, 0.96F, 1.0F)}, {"opacity", number(0.9F)}}},
                        {"size", {{"size", range(0.03F, 0.06F)}}},
                        {"fade", {{"in", number(10.0F)}, {"out", number(10.0F)}}}});
}

// One puff as a pile comes down, spreading low along the ground.
Layer pile_dust(Document& d) {
    return build_layer(d, LayerKind::Particles, "Collapse dust", 2.0F, 3.5F,
                       {{"burst", {{"count", number(40.0F)}, {"time", number(0.25F)}}},
                        {"shape", {{"shape", choice("disc")}, {"size", range(0.3F, 0.9F)}, {"offset", vec3(0.0F, 0.15F, 0.0F)}}},
                        {"launch", {{"direction", choice("out")}, {"speed", range(0.3F, 1.2F)}}},
                        {"gravity", {{"strength", range(0.03F, 0.03F)}}},
                        {"turbulence", {{"strength", range(0.4F, 0.4F)}, {"size", number(0.7F)}}},
                        {"drag", {{"amount", range(2.0F, 2.0F)}}},
                        {"appearance",
                         {{"material", choice("smoke")}, {"texture", choice("smoke")}, {"spin", toggle(true)},
                          {"soft", number(0.3F)}}},
                        {"color", {{"color", color(0.56F, 0.52F, 0.46F)}, {"opacity", number(0.4F)}}},
                        {"size", {{"size", range(0.4F, 0.7F)}, {"over_life", choice("grow")}, {"change", number(3.0F)}}},
                        {"fade", {{"in", number(10.0F)}, {"out", number(60.0F)}}}});
}

Layer dust(Document& d) {
    return build_layer(d, LayerKind::Particles, "Dust", 1.5F, 2.5F,
                       {{"burst", {{"count", number(30.0F)}, {"every", number(3.0F)}}},
                        {"shape", {{"shape", choice("disc")}, {"size", range(0.25F, 0.25F)}, {"offset", vec3(0.0F, 0.1F, 0.0F)}}},
                        {"launch", {{"direction", choice("out")}, {"speed", range(0.6F, 1.8F)}}},
                        {"gravity", {{"strength", range(0.05F, 0.05F)}}},
                        {"turbulence", {{"strength", range(0.5F, 0.5F)}, {"size", number(0.6F)}}},
                        {"drag", {{"amount", range(2.2F, 2.2F)}}},
                        {"appearance",
                         {{"material", choice("smoke")}, {"texture", choice("smoke")}, {"spin", toggle(true)},
                          {"soft", number(0.25F)}}},
                        {"color", {{"color", color(0.55F, 0.5F, 0.44F)}, {"opacity", number(0.45F)}}},
                        {"size", {{"size", range(0.3F, 0.5F)}, {"over_life", choice("grow")}, {"change", number(3.0F)}}},
                        {"fade", {{"in", number(5.0F)}, {"out", number(60.0F)}}}});
}

Layer fireball(Document& d) {
    return build_layer(d, LayerKind::Particles, "Fireball", 0.8F, 1.4F,
                       {{"burst", {{"count", number(40.0F)}, {"every", number(3.0F)}}},
                        {"shape", {{"shape", choice("ball")}, {"size", range(0.25F, 0.25F)}, {"offset", vec3(0.0F, 0.4F, 0.0F)}}},
                        {"launch", {{"direction", choice("out")}, {"speed", range(0.5F, 2.0F)}}},
                        {"rise", {{"strength", range(2.0F, 2.0F)}}},
                        {"turbulence",
                         {{"strength", range(1.5F, 1.5F)}, {"size", number(0.4F)}, {"roll", number(0.8F)},
                          {"over_life", choice("grows")}}},
                        {"drag", {{"amount", range(2.5F, 2.5F)}}},
                        {"appearance", {{"material", choice("glow")}, {"texture", choice("flame")}, {"spin", toggle(true)}}},
                        {"spin", {{"speed", range(60.0F, 200.0F)}, {"slows", number(1.2F)}}},
                        {"color",
                         {{"color", color(1.0F, 0.75F, 0.4F)}, {"brightness", range(6.0F, 6.0F)}, {"over_life", choice("fire")}}},
                        {"size", {{"size", range(0.4F, 0.7F)}, {"over_life", choice("grow")}, {"change", number(2.2F)}}},
                        {"fade", {{"in", number(0.0F)}, {"out", number(40.0F)}}},
                        {"light", {{"count", number(8.0F)}, {"intensity", range(3.0F, 5.0F)}, {"reach", range(3.0F, 5.0F)}, {"over_life", choice("flare")}}}});
}

Layer blast_smoke(Document& d) {
    return build_layer(d, LayerKind::Particles, "Blast smoke", 3.0F, 4.5F,
                       {{"burst", {{"count", number(25.0F)}, {"time", number(0.1F)}, {"every", number(3.0F)}}},
                        {"shape", {{"shape", choice("ball")}, {"size", range(0.3F, 0.3F)}, {"offset", vec3(0.0F, 0.5F, 0.0F)}}},
                        {"launch", {{"direction", choice("out")}, {"speed", range(0.3F, 1.2F)}}},
                        {"rise", {{"strength", range(0.8F, 0.8F)}}},
                        {"turbulence", {{"strength", range(0.7F, 0.7F)}, {"size", number(0.8F)}}},
                        {"drag", {{"amount", range(1.5F, 1.5F)}}},
                        {"appearance",
                         {{"material", choice("smoke")}, {"texture", choice("smoke")}, {"spin", toggle(true)},
                          {"soft", number(0.3F)}}},
                        {"color", {{"color", color(0.18F, 0.17F, 0.16F)}, {"opacity", number(0.65F)}}},
                        {"size", {{"size", range(0.6F, 1.0F)}, {"over_life", choice("swell")}, {"change", number(3.0F)}}},
                        {"fade", {{"in", number(10.0F)}, {"out", number(50.0F)}}}});
}

Layer snow(Document& d) {
    return build_layer(d, LayerKind::Particles, "Snow", 6.0F, 8.0F,
                       {{"stream", {{"rate", number(80.0F)}}},
                        {"shape",
                         {{"shape", choice("box")}, {"box", vec3(3.0F, 0.1F, 3.0F)},
                          {"offset", vec3(0.0F, 3.0F, 0.0F)}}},
                        {"launch", {{"direction", choice("down")}, {"speed", range(0.1F, 0.3F)}, {"spread", range(0.0F, 10.0F)}}},
                        {"gravity", {{"strength", range(0.04F, 0.04F)}}},
                        {"turbulence", {{"strength", range(0.6F, 0.6F)}, {"size", number(1.2F)}, {"churn", number(0.3F)}}},
                        {"drag", {{"amount", range(1.0F, 1.0F)}}},
                        {"ground", {{"response", choice("stick")}}},
                        {"appearance", {{"material", choice("solid")}, {"texture", choice("dot")}}},
                        {"color", {{"color", color(0.95F, 0.97F, 1.0F)}, {"opacity", number(0.9F)}}},
                        {"size", {{"size", range(0.02F, 0.04F)}}},
                        {"fade", {{"in", number(10.0F)}, {"out", number(10.0F)}}}});
}

Layer rain(Document& d) {
    return build_layer(d, LayerKind::Particles, "Rain", 1.2F, 1.6F,
                       {{"stream", {{"rate", number(400.0F)}}},
                        {"shape",
                         {{"shape", choice("box")}, {"box", vec3(3.0F, 0.1F, 3.0F)},
                          {"offset", vec3(0.0F, 4.0F, 0.0F)}}},
                        {"launch", {{"direction", choice("down")}, {"speed", range(6.0F, 7.0F)}, {"spread", range(0.0F, 2.0F)}}},
                        {"gravity", {}},
                        {"ground", {{"response", choice("vanish")}}},
                        {"appearance",
                         {{"material", choice("solid")}, {"texture", choice("spark")}, {"facing", choice("motion")},
                          {"stretch", number(0.02F)}}},
                        {"color", {{"color", color(0.7F, 0.8F, 0.95F)}, {"opacity", number(0.35F)}}},
                        {"size", {{"size", range(0.008F, 0.012F)}}}});
}

struct PresetEntry final {
    LayerPreset preset;
    Layer (*make)(Document&);
};

const std::vector<PresetEntry>& preset_entries() {
    static const std::vector<PresetEntry> entries = {
        {{"particles", "Particles", "A simple stream of glowing particles to start from."}, starter},
        {{"flames", "Flames", "Licking flame tongues that rise and burn out, lighting the ground."}, flames},
        {{"embers", "Embers", "Hot specks that drift up on the heat and cool."}, embers},
        {{"smoke", "Smoke", "Dark smoke rising above a fire, curling in the wind."}, fire_smoke},
        {{"plume", "Smoke plume", "A soft grey column of smoke from the ground."}, plume},
        {{"sparks", "Sparks", "A fountain of bright sparks that fall and bounce."}, sparks},
        {{"spark_burst", "Spark burst", "A ball of sparks thrown out at once."}, spark_burst},
        {{"glow_core", "Glow core", "A soft, bright light at the heart of an effect."}, glow_core},
        {{"motes", "Magic motes", "Twinkling motes swirling around a centre."}, magic_motes},
        {{"fireball", "Fireball", "A rolling burst of flame that rises and burns out."}, fireball},
        {{"blast_smoke", "Blast smoke", "A dark cloud left behind by an explosion."}, blast_smoke},
        {{"debris", "Debris", "Chunks thrown up that tumble, bounce and settle."}, debris},
        {{"dust", "Dust", "A puff of dust kicked up along the ground."}, dust},
        {{"snow", "Snow", "Flakes drifting down and settling."}, snow},
        {{"breaking_crates", "Crates and a dropped ball", "A stack of crates breaking where a ball lands."},
         breaking_crates},
        {{"settling_snow", "Settling snow", "Snow that lands on the pieces above it."}, settling_snow},
        {{"rain", "Rain", "Fast streaks of rain that vanish on the ground."}, rain},
        {{"rock_pile", "Rock pile", "A heap of rocks, standing when the effect starts, that collapses as it plays.", LayerKind::Rigid},
         rock_pile},
        {{"pile_dust", "Collapse dust", "One low puff of dust as a pile comes down."}, pile_dust},
        {{"bucket_spill", "Bucket of stones",
          "A bucket standing full of stones, which tips over and spills them across the floor.", LayerKind::Rigid},
         bucket_spill},
    };
    return entries;
}

struct RecipeEntry final {
    Recipe recipe;
    std::vector<std::string_view> layers;
    float loop_seconds;
    Backdrop backdrop;
    float glow;
};

const std::vector<RecipeEntry>& recipe_entries() {
    static const std::vector<RecipeEntry> entries = {
        {{"blank", "Blank page", "One simple layer to build from."}, {"particles"}, 6.0F, Backdrop::Night, 0.6F},
        {{"campfire", "Campfire", "Flames, embers and smoke that light the ground."},
         {"flames", "embers", "smoke"}, 8.0F, Backdrop::Night, 0.8F},
        {{"sparks", "Spark fountain", "Bright sparks that arc, fall and bounce."}, {"sparks"}, 4.0F, Backdrop::Night, 0.8F},
        {{"explosion", "Explosion", "Fireball, sparks, smoke and debris, every three seconds."},
         {"fireball", "spark_burst", "blast_smoke", "debris", "dust"}, 3.0F, Backdrop::Studio, 0.7F},
        {{"magic", "Magic orb", "A glowing core with motes swirling around it."},
         {"glow_core", "motes"}, 6.0F, Backdrop::Night, 1.0F},
        {{"smoke", "Smoke plume", "A soft column of rising smoke."}, {"plume"}, 10.0F, Backdrop::Studio, 0.3F},
        {{"snow", "Snowfall", "Flakes drifting down and settling."}, {"snow"}, 10.0F, Backdrop::Night, 0.4F},
        {{"breaking_crates", "Crates in the snow", "Crates breaking where a ball lands, with snow settling "
                                                    "on them."},
         {"breaking_crates", "settling_snow"},
         6.0F,
         Backdrop::Studio,
         0.0F},
        {{"rain", "Rain", "Streaks of rain."}, {"rain"}, 4.0F, Backdrop::Night, 0.3F},
        {{"rock_pile", "Rock pile", "A heap of rocks that collapses when the effect plays, with a puff of dust."},
         {"rock_pile", "pile_dust"}, 6.0F, Backdrop::Studio, 0.3F},
        {{"bucket_spill", "Bucket of stones",
          "A bucket of stones that tips over a second in and spills them across the floor."},
         {"bucket_spill"}, 6.0F, Backdrop::Studio, 0.3F},
    };
    return entries;
}

} // namespace

const std::vector<Recipe>& recipes() {
    static const std::vector<Recipe> list = [] {
        std::vector<Recipe> out;
        for (const RecipeEntry& entry : recipe_entries()) {
            out.push_back(entry.recipe);
        }
        return out;
    }();
    return list;
}

const std::vector<LayerPreset>& layer_presets() {
    static const std::vector<LayerPreset> list = [] {
        std::vector<LayerPreset> out;
        for (const PresetEntry& entry : preset_entries()) {
            out.push_back(entry.preset);
        }
        return out;
    }();
    return list;
}

std::optional<Layer> make_layer_preset(Document& document, std::string_view id) {
    for (const PresetEntry& entry : preset_entries()) {
        if (entry.preset.id == id) {
            return entry.make(document);
        }
    }
    return std::nullopt;
}

std::optional<Document> make_recipe(std::string_view id) {
    for (const RecipeEntry& entry : recipe_entries()) {
        if (entry.recipe.id != id) {
            continue;
        }
        Document document;
        document.name = std::string{entry.recipe.title};
        document.loop_seconds = entry.loop_seconds;
        document.backdrop = entry.backdrop;
        document.glow = entry.glow;
        for (const std::string_view layer : entry.layers) {
            if (std::optional<Layer> made = make_layer_preset(document, layer)) {
                document.layers.push_back(std::move(*made));
            }
        }
        return document;
    }
    return std::nullopt;
}

} // namespace ajfx::studio
