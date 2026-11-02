#include "blast.hpp"

#include "catalog.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>

namespace ajfx::studio {
namespace {

// Weight in kg/m^3; grip and bounce from 0 to 1; how hard a hit and a blast
// must be to break it, in m/s; and how many pieces it cuts into. Glass breaks
// at a touch into many shards, metal hardly at all and into a few, concrete
// and wood in between. These are starting points for an artist's eye, not
// measurements.
constexpr std::array<Material, 4> table{{
    Material{"concrete", "Concrete", 2400.0F, 0.8F, 0.05F, 8.0F, 10.0F, 10.0F},
    Material{"wood", "Wood", 600.0F, 0.6F, 0.25F, 4.0F, 6.0F, 6.0F},
    Material{"glass", "Glass", 2500.0F, 0.3F, 0.15F, 1.5F, 2.5F, 24.0F},
    Material{"metal", "Metal", 7800.0F, 0.4F, 0.3F, 25.0F, 18.0F, 4.0F},
}};

// Sets a control through the catalog, so a value is held to its range as one
// typed would be.
void put(Block& block, std::string_view control, Value value) {
    const BlockDef* def = find_block_def(block.kind);
    const ControlDef* found = def != nullptr ? find_control(*def, control) : nullptr;
    if (found != nullptr) {
        block.set(control, sanitize(*found, std::move(value)));
    }
}

struct Spec final {
    BlockKind kind;
    std::initializer_list<std::pair<std::string_view, Value>> values;
};

Layer build_layer(Document& document, LayerWhat what, std::string name, float life_min, float life_max,
                  std::initializer_list<Spec> blocks) {
    Layer layer = make_layer(document, what, std::move(name));
    layer.life_min = life_min;
    layer.life_max = life_max;
    for (const Spec& spec : blocks) {
        std::optional<Block> block = make_block(document, spec.kind);
        if (!block) {
            continue;
        }
        for (const auto& [control, value] : spec.values) {
            put(*block, control, value);
        }
        conform_block(*block, layer.kind);
        layer.blocks.push_back(std::move(*block));
    }
    return layer;
}

// `base`, or "base 2", "base 3"... where the page has a layer of that name.
std::string unused_name(const Document& document, const std::vector<Layer>& adding, std::string_view base) {
    const auto taken = [&](const std::string& name) {
        const auto named = [&](const Layer& layer) { return layer.name == name; };
        return std::any_of(document.layers.begin(), document.layers.end(), named) ||
               std::any_of(adding.begin(), adding.end(), named);
    };
    std::string name{base};
    for (int n = 2; taken(name); ++n) {
        name = std::string{base} + " " + std::to_string(n);
    }
    return name;
}

std::optional<std::size_t> index_in(const Layer& layer, Uid block) {
    for (std::size_t i = 0U; i < layer.blocks.size(); ++i) {
        if (layer.blocks[i].uid == block) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace

const std::array<Material, 4>& materials() {
    return table;
}

const Material* find_material(std::string_view id) {
    for (const Material& material : table) {
        if (material.id == id) {
            return &material;
        }
    }
    return nullptr;
}

bool apply_material(Document& document, Uid object) {
    const std::optional<Document::BlockPlace> place = document.find_block(object);
    if (!place) {
        return false;
    }
    Layer& layer = *place->layer;
    Block& block = layer.blocks[place->index];
    if (block.kind != BlockKind::Object) {
        return false;
    }
    const Material* material = find_material(choice_of(block, "material"));
    if (material == nullptr) {
        // The layer's: made of what its Bodies row says again.
        put(block, "own_material", toggle(false));
        return true;
    }
    put(block, "own_material", toggle(true));
    put(block, "density", number(material->density));
    put(block, "friction", number(material->friction));
    put(block, "bounce", number(material->bounce));
    // The Break blocks it owns: those after it, up to the next Object.
    for (std::size_t i = place->index + 1U; i < layer.blocks.size() && layer.blocks[i].kind != BlockKind::Object;
         ++i) {
        Block& breaking = layer.blocks[i];
        if (breaking.kind != BlockKind::Break) {
            continue;
        }
        put(breaking, "force", number(material->breaks_at));
        put(breaking, "blast", number(material->blasted_at));
        put(breaking, "cells", number(material->pieces));
    }
    return true;
}

Uid explodable_object(const Document& document, Uid layer_uid, Uid object) {
    const Layer* layer = document.layer(layer_uid);
    if (layer == nullptr || layer->kind != LayerKind::Rigid) {
        return 0U;
    }
    Uid first = 0U;
    for (const Block& block : layer->blocks) {
        if (block.kind != BlockKind::Object) {
            continue;
        }
        if (object != 0U && block.uid == object) {
            return block.uid;
        }
        if (first == 0U && block.enabled) {
            first = block.uid;
        }
    }
    return first;
}

std::optional<Exploded> explode_this(Document& document, Uid layer_uid, Uid object_uid) {
    const Uid target = explodable_object(document, layer_uid, object_uid);
    Layer* layer = document.layer(layer_uid);
    if (target == 0U || layer == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::size_t> found = index_in(*layer, target);
    if (!found) {
        return std::nullopt;
    }
    Exploded out;
    out.object = target;

    // Only a piece the world moves can come apart, and a hollow thing is a
    // prop: it is let be knocked about, as its own shape.
    std::string material_id;
    {
        Block& object = layer->blocks[*found];
        object.enabled = true;
        put(object, "motion", choice("loose"));
        const std::string collides = choice_of(object, "collides");
        if (collides == "bucket" || collides == "crate" || collides == "tube") {
            put(object, "collides", choice("own"));
        }
        material_id = choice_of(object, "material");
    }
    const Material* chosen = find_material(material_id);
    const Material& material = chosen != nullptr ? *chosen : table[0];

    // Its Break: the one it has, set to shatter when blasted, or one cut
    // from its own model, directly under it.
    std::optional<std::size_t> breaking;
    std::size_t parts = 0U;
    for (std::size_t i = *found + 1U; i < layer->blocks.size() && layer->blocks[i].kind != BlockKind::Object; ++i) {
        const Block& block = layer->blocks[i];
        if (block.kind == BlockKind::Break && !breaking) {
            breaking = i;
        } else if (block.kind == BlockKind::Part && block.enabled) {
            ++parts;
        }
    }
    float blasted_at = material.blasted_at;
    if (breaking) {
        Block& block = layer->blocks[*breaking];
        block.enabled = true;
        // Parts it was given stay what it comes apart into; with none, it
        // cuts its own model.
        if (choice_of(block, "into") != "cut" && parts == 0U) {
            put(block, "into", choice("cut"));
            put(block, "cells", number(material.pieces));
        }
        if (number_of(block, "blast") <= 0.0F) {
            put(block, "blast", number(material.blasted_at));
        }
        blasted_at = number_of(block, "blast");
        out.breaking = block.uid;
    } else {
        std::optional<Block> made = make_block(document, BlockKind::Break);
        if (!made) {
            return std::nullopt;
        }
        put(*made, "into", choice("cut"));
        put(*made, "cells", number(material.pieces));
        put(*made, "force", number(material.breaks_at));
        put(*made, "blast", number(material.blasted_at));
        put(*made, "scatter", number(1.0F));
        put(*made, "spin", number(240.0F));
        out.breaking = insert_block(*layer, *found + 1U, std::move(*made));
    }

    // How big it is and where its middle is. One object stands at its
    // middle; a heap or a stack at the middle of its base.
    const Block& object = layer->blocks[*index_in(*layer, target)];
    const float size = std::max(number_of(object, "size"), 0.05F);
    const Value at = get(object, "at");
    Vec3 centre{at.x, at.y, at.z};
    float reach = size;
    if (choice_of(object, "arrange") != "one") {
        const float count = std::max(number_of(object, "count"), 1.0F);
        reach = size * std::cbrt(count);
        if (choice_of(object, "arrange") == "stack") {
            reach = std::max({reach, number_of(object, "width"), number_of(object, "depth")});
        }
        centre.y += reach * 0.5F;
    }
    // Everything added is sized against a thing half a metre across.
    const float k = std::clamp(reach / 0.5F, 0.2F, 10.0F);
    // Half a second after it is let go, so it is seen standing first.
    const float goes_off = std::max(number_of(object, "release"), 0.0F) + 0.5F;
    // Strong enough to shatter it where it stands, and reaching well past it.
    const float power = std::max(1.6F * blasted_at, 10.0F * std::sqrt(k));
    const float radius = std::max(4.0F * reach, 1.0F);

    {
        std::optional<Block> blast = make_block(document, BlockKind::Explosion);
        if (!blast) {
            return std::nullopt;
        }
        put(*blast, "at", vec3(centre.x, centre.y, centre.z));
        put(*blast, "power", number(power));
        put(*blast, "radius", number(radius));
        put(*blast, "time", number(goes_off));
        out.explosion = insert_block(*layer, layer->blocks.size(), std::move(*blast));
    }

    // Born just before the blast, at the middle, so it is the blast that
    // throws them: three steps ahead of it.
    const float born = std::max(goes_off - 0.05F, 0.0F);
    const Value middle = vec3(centre.x, centre.y, centre.z);
    const float ground = document.world.ground_height;
    const float root_k = std::sqrt(k);
    const auto many = [&](float n) { return number(std::round(n * std::clamp(root_k, 0.5F, 3.0F))); };
    std::vector<Layer> adding;
    adding.push_back(build_layer(
        document, LayerWhat::Fire, unused_name(document, adding, "Fireball"), 0.7F, 1.2F,
        {{BlockKind::Burst, {{"count", many(40.0F)}, {"time", number(born)}}},
         {BlockKind::Shape, {{"shape", choice("ball")}, {"size", range(0.25F * reach, 0.5F * reach)}, {"offset", middle}}},
         {BlockKind::Launch, {{"direction", choice("out")}, {"speed", range(0.3F * k, 1.0F * k)}}},
         {BlockKind::Rise, {{"strength", range(2.0F * root_k, 2.0F * root_k)}}},
         {BlockKind::Turbulence,
          {{"strength", range(1.5F * root_k, 1.5F * root_k)}, {"size", number(0.4F * k)}, {"roll", number(0.8F)},
           {"over_life", choice("grows")}}},
         {BlockKind::Drag, {{"amount", range(2.5F, 2.5F)}}},
         {BlockKind::Appearance, {{"material", choice("glow")}, {"texture", choice("flame")}, {"spin", toggle(true)}}},
         {BlockKind::Spin, {{"speed", range(60.0F, 200.0F)}, {"slows", number(1.2F)}}},
         {BlockKind::Color,
          {{"color", color(1.0F, 0.75F, 0.4F)}, {"brightness", range(6.0F, 6.0F)}, {"over_life", choice("fire")}}},
         {BlockKind::Size, {{"size", range(0.35F * k, 0.6F * k)}, {"over_life", choice("grow")}, {"change", number(2.2F)}}},
         {BlockKind::Fade, {{"in", number(0.0F)}, {"out", number(40.0F)}}},
         {BlockKind::Light,
          {{"count", number(8.0F)}, {"intensity", range(3.0F, 5.0F)}, {"reach", range(3.0F * k, 5.0F * k)},
           {"over_life", choice("flare")}}}}));
    adding.push_back(build_layer(
        document, LayerWhat::Sparks, unused_name(document, adding, "Sparks"), 0.6F, 1.4F,
        {{BlockKind::Burst, {{"count", many(150.0F)}, {"time", number(born)}}},
         {BlockKind::Shape, {{"shape", choice("ball")}, {"size", range(0.2F * reach, 0.2F * reach)}, {"offset", middle}}},
         {BlockKind::Launch, {{"direction", choice("out")}, {"speed", range(1.0F * k, 3.0F * k)}}},
         {BlockKind::Drag, {{"amount", range(0.6F, 0.6F)}}},
         {BlockKind::Appearance,
          {{"material", choice("glow")}, {"texture", choice("spark")}, {"facing", choice("motion")},
           {"stretch", number(0.03F)}}},
         {BlockKind::Color,
          {{"color", color(1.0F, 0.75F, 0.4F)}, {"brightness", range(12.0F, 12.0F)}, {"over_life", choice("fire")}}},
         {BlockKind::Size, {{"size", range(0.015F * root_k, 0.03F * root_k)}}},
         {BlockKind::Fade, {{"in", number(0.0F)}, {"out", number(30.0F)}}}}));
    adding.push_back(build_layer(
        document, LayerWhat::Smoke, unused_name(document, adding, "Smoke"), 3.0F, 4.5F,
        {{BlockKind::Burst, {{"count", many(30.0F)}, {"time", number(born)}}},
         {BlockKind::Shape, {{"shape", choice("ball")}, {"size", range(0.35F * reach, 0.35F * reach)}, {"offset", middle}}},
         {BlockKind::Launch, {{"direction", choice("out")}, {"speed", range(0.2F * k, 0.6F * k)}}},
         {BlockKind::Rise, {{"strength", range(0.8F, 0.8F)}}},
         {BlockKind::Turbulence, {{"strength", range(0.7F, 0.7F)}, {"size", number(0.8F * k)}}},
         {BlockKind::Drag, {{"amount", range(1.5F, 1.5F)}}},
         {BlockKind::Appearance,
          {{"material", choice("smoke")}, {"texture", choice("smoke")}, {"spin", toggle(true)}, {"soft", number(0.3F)}}},
         {BlockKind::Spin, {{"speed", range(8.0F, 35.0F)}, {"slows", number(0.15F)}}},
         {BlockKind::Color, {{"color", color(0.18F, 0.17F, 0.16F)}, {"opacity", number(0.65F)}}},
         {BlockKind::Size, {{"size", range(0.6F * k, 1.0F * k)}, {"over_life", choice("swell")}, {"change", number(3.0F)}}},
         {BlockKind::Fade, {{"in", number(10.0F)}, {"out", number(50.0F)}}}}));
    // A ring around it, level with its middle, so the blast throws it out
    // sideways rather than down into the ground, where dust sticks. It
    // settles slowly as it spreads, as dust does.
    adding.push_back(build_layer(
        document, LayerWhat::Dust, unused_name(document, adding, "Dust"), 2.0F, 3.5F,
        {{BlockKind::Burst, {{"count", many(40.0F)}, {"time", number(born)}}},
         {BlockKind::Shape,
          {{"shape", choice("disc")},
           {"size", range(0.5F * reach, 1.5F * reach)},
           {"offset", vec3(centre.x, std::max(centre.y, ground + 0.05F * k), centre.z)}}},
         {BlockKind::Launch, {{"direction", choice("out")}, {"speed", range(0.3F * k, 1.0F * k)}}},
         {BlockKind::Turbulence, {{"strength", range(0.4F, 0.4F)}, {"size", number(0.7F * k)}}},
         {BlockKind::Drag, {{"amount", range(2.0F, 2.0F)}}},
         {BlockKind::Appearance,
          {{"material", choice("smoke")}, {"texture", choice("smoke")}, {"spin", toggle(true)}, {"soft", number(0.3F)}}},
         {BlockKind::Color, {{"color", color(0.56F, 0.52F, 0.46F)}, {"opacity", number(0.4F)}}},
         {BlockKind::Size, {{"size", range(0.4F * k, 0.7F * k)}, {"over_life", choice("grow")}, {"change", number(3.0F)}}},
         {BlockKind::Fade, {{"in", number(10.0F)}, {"out", number(60.0F)}}}}));

    // What its pieces make as they fly. Born of its own layer's bodies, so a
    // second solid layer on the page does not puff or trail too.
    const auto from_it = number(static_cast<float>(layer_uid));
    // Dust where a piece lands or strikes: a hit at walking pace makes a
    // little, a fragment flung down a lot.
    adding.push_back(build_layer(
        document, LayerWhat::Dust, unused_name(document, adding, "Impact dust"), 1.2F, 2.2F,
        {{BlockKind::HitPuffs,
          {{"solid", from_it},
           {"hardness", number(1.0F)},
           {"per_hit", number(2.0F)},
           {"per_speed", number(1.5F * std::clamp(root_k, 0.5F, 3.0F))},
           {"spread", number(70.0F)}}},
         {BlockKind::Turbulence, {{"strength", range(0.3F, 0.3F)}, {"size", number(0.5F * k)}}},
         {BlockKind::Drag, {{"amount", range(2.5F, 2.5F)}}},
         {BlockKind::Appearance,
          {{"material", choice("smoke")}, {"texture", choice("smoke")}, {"spin", toggle(true)}, {"soft", number(0.2F)}}},
         {BlockKind::Color, {{"color", color(0.56F, 0.52F, 0.46F)}, {"opacity", number(0.45F)}}},
         {BlockKind::Size, {{"size", range(0.08F * k, 0.16F * k)}, {"over_life", choice("grow")}, {"change", number(3.0F)}}},
         {BlockKind::Fade, {{"in", number(5.0F)}, {"out", number(60.0F)}}}}));
    // Embers shed behind the broken pieces as they fly, more the faster.
    adding.push_back(build_layer(
        document, LayerWhat::Fire, unused_name(document, adding, "Embers"), 0.5F, 1.1F,
        {{BlockKind::Trail, {{"solid", from_it}, {"pieces", toggle(true)}, {"per_speed", number(4.0F)}, {"min_speed", number(1.0F)}}},
         {BlockKind::Rise, {{"strength", range(0.4F, 0.4F)}}},
         {BlockKind::Drag, {{"amount", range(1.0F, 1.0F)}}},
         {BlockKind::Appearance, {{"material", choice("glow")}, {"texture", choice("glow")}}},
         {BlockKind::Color,
          {{"color", color(1.0F, 0.55F, 0.18F)}, {"brightness", range(8.0F, 8.0F)}, {"over_life", choice("fire")}}},
         {BlockKind::Size, {{"size", range(0.012F * root_k, 0.025F * root_k)}}},
         {BlockKind::Fade, {{"in", number(0.0F)}, {"out", number(40.0F)}}}}));

    // Its broken pieces glow and cool. A piece is as old as what it broke
    // from, so to glow at "How hot" when the blast breaks it the heat starts
    // as high as the time it stood takes off.
    {
        const bool has_heat = std::any_of(layer->blocks.begin(), layer->blocks.end(),
                                          [](const Block& b) { return b.kind == BlockKind::Heat; });
        if (!has_heat) {
            std::optional<Block> heat = make_block(document, BlockKind::Heat);
            if (heat) {
                constexpr float half_life = 0.8F;
                constexpr float hot = 1.4F;
                put(*heat, "what", choice("pieces"));
                put(*heat, "half_life", number(half_life));
                put(*heat, "start", number(hot * std::exp2(goes_off / half_life)));
                put(*heat, "glow", number(3.0F));
                // With the look, before the Explosion, which stays last.
                const std::size_t before_blast = index_in(*layer, out.explosion).value_or(layer->blocks.size());
                out.heat = insert_block(*layer, before_blast, std::move(*heat));
            }
        }
    }

    // After the layer it blows up, in that order.
    std::size_t at_layer = document.layers.size();
    for (std::size_t i = 0U; i < document.layers.size(); ++i) {
        if (document.layers[i].uid == layer_uid) {
            at_layer = i + 1U;
        }
    }
    for (Layer& made : adding) {
        out.layers.push_back(made.uid);
        document.layers.insert(document.layers.begin() + static_cast<std::ptrdiff_t>(at_layer), std::move(made));
        ++at_layer;
    }
    return out;
}

} // namespace ajfx::studio
