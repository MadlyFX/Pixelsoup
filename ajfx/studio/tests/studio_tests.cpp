#include "catalog.hpp"
#include "coach.hpp"
#include "compile.hpp"
#include "document_io.hpp"
#include "guides.hpp"
#include "library.hpp"
#include "raw_modules.hpp"
#include "recipes.hpp"
#include "session.hpp"
#include "user_assets.hpp"

#include <ajfx/package.hpp>
#include <ajfx/rigid.hpp>
#include <ajfx/simulation.hpp>

#if AJFX_STUDIO_TEST_KERNELS
#include <ajfx/d3d12.hpp>
#include <ajfx/gpu/kernels.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// ajfx Studio's page model and compiler, headless: every recipe builds a valid
// effect within its budget, block order and unrelated edits leave an effect
// alone, page scale scales, what blocks share is merged, pages save and load
// unchanged, and the coach's fixes fix what it points at.
namespace {

using namespace ajfx::studio;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"line " + std::to_string(line) + ": check failed: " + expression};
    }
}

#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

Document recipe(std::string_view id) {
    std::optional<Document> document = make_recipe(id);
    CHECK(document.has_value());
    return std::move(*document);
}

Layer& layer_named(Document& document, std::string_view name) {
    for (Layer& layer : document.layers) {
        if (layer.name == name) {
            return layer;
        }
    }
    throw std::runtime_error{"no layer " + std::string{name}};
}

Block& block_of(Layer& layer, std::string_view kind) {
    for (Block& block : layer.blocks) {
        if (block.kind == kind) {
            return block;
        }
    }
    throw std::runtime_error{"no block " + std::string{kind}};
}

const ajfx::Emitter& emitter_of(const Compiled& compiled, const Layer& layer) {
    const LayerReport* report = compiled.layer(layer.uid);
    CHECK(report != nullptr && compiled.effect.has_value());
    for (const ajfx::Emitter& emitter : compiled.effect->emitters) {
        if (emitter.name == report->emitter) {
            return emitter;
        }
    }
    throw std::runtime_error{"layer not in the effect"};
}

template <class T, class V>
struct in_variant;
template <class T, class... Ts>
struct in_variant<T, std::variant<Ts...>> : std::bool_constant<(std::is_same_v<T, Ts> || ...)> {};

template <class M>
const M* find_module(const ajfx::Emitter& emitter) {
    if constexpr (in_variant<M, ajfx::SpawnModule>::value) {
        for (const ajfx::SpawnModule& module : emitter.spawn) {
            if (const M* found = std::get_if<M>(&module)) {
                return found;
            }
        }
    } else if constexpr (in_variant<M, ajfx::InitModule>::value) {
        for (const ajfx::InitModule& module : emitter.init) {
            if (const M* found = std::get_if<M>(&module)) {
                return found;
            }
        }
    } else {
        for (const ajfx::UpdateModule& module : emitter.update) {
            if (const M* found = std::get_if<M>(&module)) {
                return found;
            }
        }
    }
    return nullptr;
}

bool near(float a, float b) {
    return std::fabs(a - b) <= 1.0e-5F * std::max(1.0F, std::fabs(b));
}

bool near(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

void every_recipe_compiles_within_its_budget() {
    for (const Recipe& entry : recipes()) {
        const Document document = recipe(entry.id);
        const Compiled compiled = compile(document, Purpose::Export);
        if (!compiled.error.empty()) {
            std::cout << "  " << entry.id << ": " << compiled.error << '\n';
        }
        CHECK(compiled.error.empty());
        CHECK(compiled.effect.has_value());
        CHECK(compiled.effect->emitters.size() == document.layers.size());
        CHECK(ajfx::validate(*compiled.effect).ok());
        for (const ajfx::Emitter& emitter : compiled.effect->emitters) {
            CHECK(ajfx::particle_bound(*compiled.effect, emitter) <= emitter.capacity);
            CHECK(emitter.capacity % 64U == 0U);
        }
        // Every texture and mesh it names travels with it.
        CHECK(ajfx::check_textures(*compiled.effect, compiled.textures).ok());
        CHECK(ajfx::check_meshes(*compiled.effect, compiled.meshes).ok());
        CHECK(ajfx::write_package(*compiled.effect, asset_entries(compiled)).ok());
    }
    // Every layer preset compiles on its own too.
    for (const LayerPreset& preset : layer_presets()) {
        Document document;
        std::optional<Layer> layer = make_layer_preset(document, preset.id);
        CHECK(layer.has_value());
        document.layers.push_back(std::move(*layer));
        CHECK(compile(document, Purpose::Export).error.empty());
    }
}

void block_order_does_not_change_the_effect() {
    Document document = recipe("campfire");
    const Compiled before = compile(document, Purpose::Export);
    for (Layer& layer : document.layers) {
        std::reverse(layer.blocks.begin(), layer.blocks.end());
    }
    const Compiled after = compile(document, Purpose::Export);
    CHECK(before.effect.has_value() && after.effect.has_value());
    CHECK(*before.effect == *after.effect);
}

void a_new_block_leaves_the_others_alone() {
    Document document = recipe("campfire");
    const Compiled before = compile(document, Purpose::Export);
    Layer& flames = layer_named(document, "Flames");
    std::optional<Block> wind = make_block(document, "wind");
    CHECK(wind.has_value());
    insert_block(flames, 0U, std::move(*wind));
    const Compiled after = compile(document, Purpose::Export);

    // Every module of the old effect is in the new one, with the same id and
    // the same values, so its random streams are where they were.
    const ajfx::Emitter& old_emitter = emitter_of(before, flames);
    const ajfx::Emitter& new_emitter = emitter_of(after, flames);
    CHECK(new_emitter.update.size() == old_emitter.update.size() + 1U);
    for (const ajfx::InitModule& module : old_emitter.init) {
        CHECK(std::find(new_emitter.init.begin(), new_emitter.init.end(), module) != new_emitter.init.end());
    }
    for (const ajfx::UpdateModule& module : old_emitter.update) {
        CHECK(std::find(new_emitter.update.begin(), new_emitter.update.end(), module) != new_emitter.update.end());
    }
    // The other layers did not change at all.
    CHECK(emitter_of(before, layer_named(document, "Embers")) == emitter_of(after, layer_named(document, "Embers")));
}

void page_scale_scales_the_whole_effect() {
    Document document = recipe("campfire");
    const Compiled one = compile(document, Purpose::Export);
    document.scale = 2.0F;
    const Compiled two = compile(document, Purpose::Export);
    const Layer& flames = layer_named(document, "Flames");
    const ajfx::Emitter& a = emitter_of(one, flames);
    const ajfx::Emitter& b = emitter_of(two, flames);

    CHECK(near(find_module<ajfx::ShapeLocation>(b)->radius_max.value, find_module<ajfx::ShapeLocation>(a)->radius_max.value * 2.0F));
    CHECK(near(find_module<ajfx::Velocity>(b)->speed_max.value, find_module<ajfx::Velocity>(a)->speed_max.value * 2.0F));
    CHECK(near(find_module<ajfx::InitSize>(b)->max.value, find_module<ajfx::InitSize>(a)->max.value * 2.0F));
    CHECK(near(find_module<ajfx::Gravity>(b)->acceleration.value.y, find_module<ajfx::Gravity>(a)->acceleration.value.y * 2.0F));
    CHECK(near(find_module<ajfx::CurlNoise>(b)->frequency.value, find_module<ajfx::CurlNoise>(a)->frequency.value * 0.5F));
    CHECK(near(b.renderer.light_range_min, a.renderer.light_range_min * 2.0F));
    CHECK(near(b.renderer.light_range_max, a.renderer.light_range_max * 2.0F));
    // Time is not a distance: life and spawn rates stay.
    CHECK(a.spawn == b.spawn);
    CHECK(*find_module<ajfx::Lifetime>(a) == *find_module<ajfx::Lifetime>(b));
}

void blocks_that_shape_one_curve_are_merged() {
    Document document;
    Layer layer;
    layer.uid = document.allocate();
    layer.name = "Merge";
    for (const char* kind : {"stream", "color", "fade", "size"}) {
        layer.blocks.push_back(*make_block(document, kind));
    }
    block_of(layer, "color").set("over_life", choice("fire"));
    block_of(layer, "fade").set("in", number(25.0F));
    block_of(layer, "fade").set("out", number(25.0F));
    document.layers.push_back(layer);
    Compiled compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty());
    const ajfx::Emitter& emitter = emitter_of(compiled, layer);
    const std::size_t color_curves = static_cast<std::size_t>(std::count_if(
        emitter.update.begin(), emitter.update.end(),
        [](const ajfx::UpdateModule& m) { return std::holds_alternative<ajfx::ColorOverLife>(m); }));
    CHECK(color_curves == 1U);
    const auto& keys = find_module<ajfx::ColorOverLife>(emitter)->keys;
    CHECK(keys.size() == 17U);
    // Faded in from nothing and out to nothing; burning from white at birth
    // to a dark ember at death.
    CHECK(keys.front().value.a == 0.0F && keys.back().value.a == 0.0F);
    CHECK(near(keys[8].value.a, 1.0F));
    CHECK(keys.front().value.r == 1.0F && keys.front().value.b == 1.0F);
    CHECK(keys.back().value.r < 0.2F);
    // A Size block that stays the same adds no curve.
    CHECK(find_module<ajfx::SizeOverLife>(emitter) == nullptr);

    // A raw colour curve replaces the merged one.
    const ajfx::ColorOverLife raw{.id = 0U, .keys = {{0.0F, ajfx::Color{0.5F, 0.5F, 0.5F, 1.0F}}, {1.0F, ajfx::Color{}}}};
    insert_block(document.layers[0], 9U, make_raw_block(document, RawModule{ajfx::UpdateModule{raw}}));
    compiled = compile(document, Purpose::Export);
    CHECK(find_module<ajfx::ColorOverLife>(emitter_of(compiled, document.layers[0]))->keys == raw.keys);
}

void pages_save_and_load_unchanged() {
    for (const Recipe& entry : recipes()) {
        Document document = recipe(entry.id);
        if (entry.id == "campfire") {
            // Values off the default, a disabled block, a solo and a raw module.
            Layer& flames = layer_named(document, "Flames");
            flames.solo = true;
            block_of(flames, "drag").enabled = false;
            block_of(flames, "stream").publish(Published{"rate", "fire_rate", 10.0F, 400.0F});
            block_of(flames, "size").publish(Published{"size", "fire_size", 0.05F, 1.25F});
            block_of(flames, "color").set("color", color(0.1F, 0.2F, 0.3F, 0.4F));
            ajfx::Vortex vortex{};
            vortex.strength = ajfx::bind("spin");
            vortex.center = ajfx::Vec3{0.25F, 1.5F, -2.0F};
            insert_block(flames, 2U, make_raw_block(document, RawModule{ajfx::UpdateModule{vortex}}));
            insert_block(flames, 3U,
                         make_raw_block(document, RawModule{ajfx::UpdateModule{ajfx::SizeOverLife{
                                                      .id = 0U, .keys = {{0.0F, 0.1F}, {0.3F, 1.7F}, {1.0F, 0.0F}}}}}));
            document.scale = 1.75F;
            document.backdrop = Backdrop::Void;
        }
        const Loaded loaded = load_document(save_document(document));
        CHECK(loaded.error.empty());
        CHECK(loaded.document.has_value());
        if (!(*loaded.document == document)) {
            throw std::runtime_error{std::string{entry.id} + " changed on its way through a file:\n" +
                                     save_document(document) + "\nbecame\n" + save_document(*loaded.document)};
        }
    }
}

void pages_from_elsewhere_are_refused_with_a_reason() {
    CHECK(!load_document("{").error.empty());
    CHECK(load_document(R"({"format": "something-else", "version": 1})").error.find("not an ajfx Studio page") !=
          std::string::npos);
    CHECK(load_document(R"({"format": "ajfx-studio", "version": 99})").error.find("newer") != std::string::npos);
    CHECK(load_document(R"({"format": "ajfx-studio", "version": 1, "layers": [{"uid": 1, "blocks": [{"uid": 2, "kind": "teleport"}]}]})")
              .error.find("teleport") != std::string::npos);
    // Out-of-range values are brought back into range; unknown controls dropped.
    const Loaded loaded = load_document(
        R"({"format": "ajfx-studio", "version": 1, "layers": [{"uid": 1, "name": "A", "blocks": [{"uid": 2, "kind": "launch", "values": {"spread": 400, "warp": 3}}]}]})");
    CHECK(loaded.error.empty());
    const Block& launch = loaded.document->layers[0].blocks[0];
    // Spread was one number; a page saved then reads it as the cone's edge.
    CHECK(get(launch, "spread") == range(0.0F, 180.0F));
    CHECK(launch.find("warp") == nullptr);
    CHECK(loaded.document->next_uid == 3U);
}

void the_coach_fixes_what_it_points_at() {
    Document document = recipe("smoke");
    Layer& plume = document.layers[0];
    block_of(plume, "appearance").set("soft", number(0.0F));
    remove_block(document, block_of(plume, "stream").uid);

    std::vector<Advice> notes = advise(document, compile(document, Purpose::Preview));
    const auto find = [&](std::string_view label) -> const Advice* {
        for (const Advice& note : notes) {
            if (note.fix_label == label) {
                return &note;
            }
        }
        return nullptr;
    };
    // With nothing born, that is all it says.
    CHECK(notes.size() == 1U && find("Add a stream") != nullptr);
    find("Add a stream")->fix(document);
    CHECK(plume.blocks.front().kind == "stream");

    notes = advise(document, compile(document, Purpose::Preview));
    CHECK(find("Soften edges") != nullptr);
    find("Soften edges")->fix(document);
    notes = advise(document, compile(document, Purpose::Preview));
    CHECK(find("Soften edges") == nullptr);

    // Sparks with their ground taken away fall through the floor.
    Document sparks = recipe("sparks");
    remove_block(sparks, block_of(sparks.layers[0], "ground").uid);
    notes = advise(sparks, compile(sparks, Purpose::Preview));
    CHECK(find("Add ground") != nullptr);
    find("Add ground")->fix(sparks);
    notes = advise(sparks, compile(sparks, Purpose::Preview));
    CHECK(find("Add ground") == nullptr);
}

void solo_and_broken_layers_leave_the_rest_playing() {
    Document document = recipe("campfire");
    layer_named(document, "Embers").solo = true;
    Compiled preview = compile(document, Purpose::Preview);
    CHECK(preview.effect->emitters.size() == 1U);
    CHECK(compile(document, Purpose::Export).effect->emitters.size() == 3U);

    // A layer that cannot compile is named, with its block, and the others
    // still play.
    layer_named(document, "Embers").solo = false;
    Layer& smoke = layer_named(document, "Smoke");
    const Uid broken = insert_block(smoke, 0U,
                                    make_raw_block(document, RawModule{ajfx::UpdateModule{ajfx::SdfCollision{}}}));
    preview = compile(document, Purpose::Preview);
    CHECK(preview.effect->emitters.size() == 2U);
    CHECK(preview.layer(smoke.uid)->error_block == broken);
    CHECK(!preview.error.empty());

    // A raw module the runtime refuses is traced to its block.
    remove_block(document, broken);
    ajfx::Velocity bad{};
    bad.spread = 400.0F;
    const Uid refused = insert_block(smoke, 0U, make_raw_block(document, RawModule{ajfx::InitModule{bad}}));
    preview = compile(document, Purpose::Preview);
    CHECK(preview.layer(smoke.uid)->error_block == refused);
}

std::multiset<std::string> described(const Compiled& compiled, Uid layer) {
    std::multiset<std::string> out;
    for (const ModuleLine& line : compiled.layer(layer)->modules) {
        out.insert(std::string{raw_kind(line.module)} + ": " + describe_module(line.module));
    }
    return out;
}

void detaching_a_block_keeps_the_picture() {
    for (const char* kind : {"launch", "turbulence", "color", "fade", "size", "tumble", "pile"}) {
        const std::string_view k{kind};
        const bool rocks = k == "pile";
        Document document = recipe(k == "tumble" ? "explosion" : rocks ? "rock_pile" : "campfire");
        Layer& layer = k == "tumble" ? layer_named(document, "Debris")
                       : rocks       ? layer_named(document, "Rock pile")
                                     : layer_named(document, "Flames");
        const Compiled before = compile(document, Purpose::Export);
        const Uid uid = block_of(layer, kind).uid;
        std::vector<Block> raw = detach_block(document, before, uid);
        CHECK(!raw.empty());
        std::optional<Document::BlockPlace> place = document.find_block(uid);
        const std::size_t at = place->index;
        remove_block(document, uid);
        for (std::size_t i = 0U; i < raw.size(); ++i) {
            insert_block(layer, at + i, std::move(raw[i]));
        }
        const Compiled after = compile(document, Purpose::Export);
        CHECK(after.error.empty());
        CHECK(described(before, layer.uid) == described(after, layer.uid));
    }
}

const ajfx::ParameterDesc* parameter_of(const Compiled& compiled, std::string_view name) {
    for (const ajfx::ParameterDesc& parameter : compiled.effect->parameters) {
        if (parameter.name == name) {
            return &parameter;
        }
    }
    return nullptr;
}

void published_controls_become_parameters_with_budgets_for_their_range() {
    Document document = recipe("campfire");
    document.scale = 2.0F;
    Layer& flames = layer_named(document, "Flames");
    block_of(flames, "stream").publish(Published{"rate", "fire_rate", 0.0F, 500.0F});
    block_of(flames, "color").publish(Published{"color", "fire_color", 0.0F, 20.0F});
    block_of(flames, "size").publish(Published{"size", "fire_size", 0.1F, 1.0F});
    Compiled compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty());

    const ajfx::ParameterDesc* rate = parameter_of(compiled, "fire_rate");
    CHECK(rate != nullptr && rate->type == ajfx::ParameterType::Float);
    CHECK(rate->default_value[0] == 90.0F && rate->min == 0.0F && rate->max == 500.0F);
    const ajfx::Emitter& emitter = emitter_of(compiled, flames);
    CHECK(std::get<ajfx::SpawnRate>(emitter.spawn[0]).rate.parameter == "fire_rate");
    // Sized for the top of the range: 500 a second for up to 1.1 seconds.
    CHECK(emitter.capacity >= 550U);

    // A colour is the whole starting colour, brightness and opacity in it.
    const ajfx::ParameterDesc* tint = parameter_of(compiled, "fire_color");
    CHECK(tint != nullptr && tint->type == ajfx::ParameterType::Color);
    CHECK(near(tint->default_value[0], 5.0F) && near(tint->default_value[1], 3.1F) && tint->default_value[3] == 1.0F);
    const ajfx::InitColor& color = *find_module<ajfx::InitColor>(emitter);
    CHECK(color.a.parameter == "fire_color" && color.b.parameter == "fire_color");

    // A range publishes both ends, in scaled units like the value.
    const ajfx::ParameterDesc* low = parameter_of(compiled, "fire_size_min");
    const ajfx::ParameterDesc* high = parameter_of(compiled, "fire_size_max");
    CHECK(low != nullptr && high != nullptr);
    CHECK(near(low->min, 0.2F) && near(high->max, 2.0F) && near(low->default_value[0], 0.76F));
    CHECK(find_module<ajfx::InitSize>(emitter)->max.parameter == "fire_size_max");
    CHECK(ajfx::write_package(*compiled.effect, asset_entries(compiled)).ok());

    // Two layers sharing a name share one parameter, sized for both ranges.
    Layer& embers = layer_named(document, "Embers");
    block_of(embers, "stream").publish(Published{"rate", "fire_rate", 0.0F, 800.0F});
    compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty());
    CHECK(parameter_of(compiled, "fire_rate")->max == 800.0F);
    CHECK(emitter_of(compiled, flames).capacity >= 880U);
    CHECK(std::get<ajfx::SpawnRate>(emitter_of(compiled, embers).spawn[0]).rate.parameter == "fire_rate");

    // ...unless they publish different kinds of value, which names the block.
    const Uid clash = block_of(embers, "color").uid;
    block_of(embers, "color").publish(Published{"color", "fire_rate", 0.0F, 1.0F});
    compiled = compile(document, Purpose::Export);
    CHECK(compiled.layer(embers.uid)->error_block == clash);
    block_of(embers, "color").unpublish("color");

    // A layer that does not play offers nothing to the host.
    Layer& smoke = layer_named(document, "Smoke");
    block_of(smoke, "drag").publish(Published{"amount", "smoke_drag", 0.0F, 3.0F});
    smoke.enabled = false;
    compiled = compile(document, Purpose::Export);
    CHECK(parameter_of(compiled, "smoke_drag") == nullptr);

    // Detached, a published control keeps its value and names nothing.
    std::vector<Block> raw = detach_block(document, compiled, block_of(flames, "color").uid);
    const auto* init = std::get_if<ajfx::InitModule>(&*raw.front().raw);
    CHECK(init != nullptr && !std::get<ajfx::InitColor>(*init).a.bound());
    CHECK(near(std::get<ajfx::InitColor>(*init).a.value.r, 5.0F));
}

void ranges_give_each_particle_its_own_value() {
    Document document = recipe("campfire");
    document.scale = 2.0F;
    Layer& flames = layer_named(document, "Flames");
    block_of(flames, "shape").set("size", range(0.1F, 0.3F));
    block_of(flames, "launch").set("spread", range(10.0F, 40.0F));
    block_of(flames, "rise").set("strength", range(3.0F, 6.0F));
    block_of(flames, "drag").set("amount", range(0.5F, 2.0F));
    block_of(flames, "color").set("brightness", range(2.0F, 8.0F));
    const Compiled compiled = compile(document, Purpose::Export);
    const ajfx::Emitter& emitter = emitter_of(compiled, flames);

    // A radius between the two ends, in metres at the page's scale.
    const auto* shape = find_module<ajfx::ShapeLocation>(emitter);
    CHECK(near(shape->radius_min.value, 0.2F) && near(shape->radius_max.value, 0.6F));
    // A spread of 10 to 40 degrees is a cone hollowed to 10.
    const auto* velocity = find_module<ajfx::Velocity>(emitter);
    CHECK(velocity->spread_min.value == 10.0F && velocity->spread.value == 40.0F);
    // A force takes each particle by its own factor between the two ends.
    const Uid rise = block_of(flames, "rise").uid;
    bool found_rise = false;
    for (const ajfx::UpdateModule& module : emitter.update) {
        if (const auto* gravity = std::get_if<ajfx::Gravity>(&module); gravity != nullptr && gravity->acceleration.value.y > 0.0F) {
            CHECK(near(gravity->acceleration.value.y, 2.0F));
            CHECK(gravity->scale_min.value == 3.0F && gravity->scale_max.value == 6.0F);
            found_rise = true;
        }
    }
    CHECK(found_rise && rise != 0U);
    const auto* drag = find_module<ajfx::Drag>(emitter);
    CHECK(drag->coefficient.value == 1.0F && drag->scale_min.value == 0.5F && drag->scale_max.value == 2.0F);
    // The starting colour carries the brightest end; each particle a share of it.
    const auto* color = find_module<ajfx::InitColor>(emitter);
    const Value base = get(block_of(flames, "color"), "color");
    CHECK(near(color->a.value.r, base.x * 8.0F));
    CHECK(near(color->brightness_min.value, 0.25F) && color->brightness_max.value == 1.0F);

    // Equal ends are the one value, exactly as a single number was.
    Document plain = recipe("campfire");
    const Compiled plain_compiled = compile(plain, Purpose::Export);
    const ajfx::Emitter& plain_flames = emitter_of(plain_compiled, layer_named(plain, "Flames"));
    const auto* plain_drag = find_module<ajfx::Drag>(plain_flames);
    CHECK(plain_drag->scale_min.value == plain_drag->scale_max.value);
    CHECK(find_module<ajfx::InitColor>(plain_flames)->brightness_min.value == 1.0F);

    // A published range is two parameters, sized for both ends.
    block_of(flames, "drag").publish(Published{"amount", "flames_drag", 0.0F, 4.0F});
    const Compiled published = compile(document, Purpose::Export);
    const auto* bound = find_module<ajfx::Drag>(emitter_of(published, flames));
    CHECK(bound->scale_min.parameter == "flames_drag_min" && bound->scale_max.parameter == "flames_drag_max");

    // Pages saved while these were single numbers read them sensibly: a
    // strength as both ends, a spread as the cone's edge from 0.
    const Loaded old = load_document(
        R"({"format": "ajfx-studio", "version": 1, "layers": [{"uid": 1, "name": "A", "blocks": [)"
        R"({"uid": 2, "kind": "gravity", "values": {"strength": 0.75}},)"
        R"({"uid": 3, "kind": "launch", "values": {"spread": 35}},)"
        R"({"uid": 4, "kind": "shape", "values": {"size": 0.4}}]}]})");
    CHECK(old.error.empty());
    CHECK(get(old.document->layers[0].blocks[0], "strength") == range(0.75F, 0.75F));
    CHECK(get(old.document->layers[0].blocks[1], "spread") == range(0.0F, 35.0F));
    CHECK(get(old.document->layers[0].blocks[2], "size") == range(0.4F, 0.4F));
}

void seeds_deal_new_values_to_their_block_alone() {
    Document document = recipe("campfire");
    Layer& flames = layer_named(document, "Flames");
    const Compiled before = compile(document, Purpose::Export);
    block_of(flames, "launch").set("seed", number(7.0F));
    const Compiled after = compile(document, Purpose::Export);

    // The launch's module has a new id, so its particles draw new values...
    const ajfx::Velocity* old_velocity = find_module<ajfx::Velocity>(emitter_of(before, flames));
    const ajfx::Velocity* new_velocity = find_module<ajfx::Velocity>(emitter_of(after, flames));
    CHECK(old_velocity->id != new_velocity->id && (new_velocity->id & 0x80000000U) != 0U);
    ajfx::Velocity same = *new_velocity;
    same.id = old_velocity->id;
    CHECK(same == *old_velocity);
    // ...and every other module is untouched.
    for (const ajfx::UpdateModule& module : emitter_of(before, flames).update) {
        const auto& update = emitter_of(after, flames).update;
        CHECK(std::find(update.begin(), update.end(), module) != update.end());
    }
    // Each seed is its own pattern; seed 0 is the block's own.
    block_of(flames, "launch").set("seed", number(8.0F));
    CHECK(find_module<ajfx::Velocity>(emitter_of(compile(document, Purpose::Export), flames))->id != new_velocity->id);
    block_of(flames, "launch").clear("seed");
    CHECK(compile(document, Purpose::Export).effect == before.effect);

    // Turbulence's seed is its field's: a different pattern of swirls.
    block_of(flames, "turbulence").set("seed", number(3.0F));
    CHECK(find_module<ajfx::CurlNoise>(emitter_of(compile(document, Purpose::Export), flames))->seed !=
          find_module<ajfx::CurlNoise>(emitter_of(before, flames))->seed);
    block_of(flames, "turbulence").clear("seed");

    // A layer's seed reseeds its emitter and nothing else; it saves.
    flames.seed = 12U;
    const Compiled reseeded = compile(document, Purpose::Export);
    CHECK(emitter_of(reseeded, flames).seed != emitter_of(before, flames).seed);
    ajfx::Emitter restored = emitter_of(reseeded, flames);
    restored.seed = emitter_of(before, flames).seed;
    CHECK(restored == emitter_of(before, flames));
    CHECK(emitter_of(reseeded, layer_named(document, "Embers")) == emitter_of(before, layer_named(document, "Embers")));
    const Loaded loaded = load_document(save_document(document));
    Document reloaded = *loaded.document;
    CHECK(layer_named(reloaded, "Flames").seed == 12U);

    // Every block that deals random values offers a seed under More settings.
    for (const char* kind : {"shape", "launch", "gravity", "rise", "wind", "turbulence", "drag", "swirl", "draw_in",
                             "attract", "color", "size", "spin", "tumble"}) {
        const ControlDef* seed = find_control(*find_block_def(kind), "seed");
        CHECK(seed != nullptr && seed->level == Level::Detail);
    }
}

void guides_show_the_focused_block_with_handles() {
    Document document = recipe("campfire");
    document.scale = 2.0F;
    Layer& flames = layer_named(document, "Flames");
    Block& shape = block_of(flames, "shape");
    shape.set("offset", vec3(0.5F, 0.25F, 0.0F));
    shape.set("size", range(0.1F, 0.2F));

    // Nothing worked on, nothing drawn: the stage stays clear.
    CHECK(build_guides(document, GuideOptions{}).guides.empty());
    // A layer worked on with no block focused: only where its things are -
    // its spawn shape, faintly, with no handles - and no other layer's.
    GuideSet idle = build_guides(document, GuideOptions{.layer = flames.uid});
    CHECK(idle.handles.empty() && idle.guides.size() == 1U);
    for (const Guide& guide : idle.guides) {
        CHECK(!guide.focused && guide.layer == flames.uid && document.block(guide.block)->kind == "shape");
    }

    // Focused, the shape carries a handle for its offset and each end of its
    // radius, in metres at the page's scale.
    const GuideSet focused = build_guides(document, GuideOptions{.focus = shape.uid});
    for (const Guide& guide : focused.guides) {
        CHECK(guide.layer == flames.uid);
    }
    const Handle* offset = nullptr;
    const Handle* high = nullptr;
    const Handle* low = nullptr;
    for (const Handle& handle : focused.handles) {
        CHECK(handle.block == shape.uid);
        if (handle.control == "offset") {
            offset = &handle;
        } else if (handle.control == "size") {
            (handle.end == 1 ? high : low) = &handle;
        }
    }
    CHECK(offset != nullptr && high != nullptr && low != nullptr);
    CHECK(offset->at == (ajfx::Vec3{1.0F, 0.5F, 0.0F}));
    CHECK(near(high->at.x - offset->at.x, 0.4F) && near(offset->at.x - low->at.x, 0.2F));

    // Dragging a handle sets its control, back in the page's units.
    CHECK(handle_value(document, *offset, ajfx::Vec3{2.0F, 0.5F, -1.0F}) == vec3(1.0F, 0.25F, -0.5F));
    const std::optional<Value> pulled = handle_value(document, *high, offset->at + ajfx::Vec3{0.0F, 0.0F, 1.0F});
    CHECK(pulled && pulled->x == 0.1F && near(pulled->y, 0.5F));
    // The low end dragged past the high takes it along.
    const std::optional<Value> pushed = handle_value(document, *low, offset->at + ajfx::Vec3{0.8F, 0.0F, 0.0F});
    CHECK(pushed && near(pushed->x, 0.4F) && pushed->y == pushed->x);

    // Wind turns by its heading handle, round the arrow's start.
    std::optional<Block> wind = make_block(document, "wind");
    const Uid wind_uid = insert_block(flames, 0U, std::move(*wind));
    const GuideSet windy = build_guides(document, GuideOptions{.focus = wind_uid});
    CHECK(windy.handles.size() == 1U && windy.handles[0].motion == HandleMotion::Heading);
    const Handle& heading = windy.handles[0];
    CHECK(handle_value(document, heading, heading.origin + ajfx::Vec3{0.0F, 0.0F, -1.0F}) == number(90.0F));

    // Draw in fades by a height above its own centre, wherever that is.
    Block& draw_in = block_of(flames, "draw_in");
    draw_in.set("center", vec3(0.0F, 0.5F, 0.0F));
    std::size_t heights = 0U;
    for (const Handle& handle : build_guides(document, GuideOptions{.focus = draw_in.uid}).handles) {
        if (handle.motion == HandleMotion::Height) {
            ++heights;
            CHECK(near(handle.at.y, (0.5F + number_of(draw_in, "height")) * 2.0F));
            const std::optional<Value> raised = handle_value(document, handle, handle.origin + ajfx::Vec3{0.0F, 3.0F, 0.0F});
            CHECK(raised && near(raised->x, 1.5F));
        }
    }
    CHECK(heights == 1U);

    // A muted layer draws nothing.
    for (Layer& layer : document.layers) {
        layer.enabled = false;
    }
    CHECK(build_guides(document, GuideOptions{.focus = shape.uid}).guides.empty());
}

void the_turbulence_guide_follows_its_field() {
    Document document = recipe("campfire");
    Layer& flames = layer_named(document, "Flames");
    const Uid turbulence = block_of(flames, "turbulence").uid;
    const auto flow = [&](float seconds) {
        for (const Guide& guide : build_guides(document, GuideOptions{.focus = turbulence, .seconds = seconds}).guides) {
            if (guide.block == turbulence) {
                return guide.strokes;
            }
        }
        return std::vector<GuideStroke>{};
    };
    const std::vector<GuideStroke> now = flow(0.0F);
    CHECK(now.size() == 64U && now[0].points.size() == 11U);
    // The field churns: later, the same streamline goes elsewhere.
    const std::vector<GuideStroke> later = flow(2.0F);
    CHECK(later[0].points.front() == now[0].points.front() && !(later[0].points.back() == now[0].points.back()));
    // Only the focused turbulence draws its field.
    for (const Guide& guide : build_guides(document, GuideOptions{.layer = flames.uid}).guides) {
        CHECK(guide.block != turbulence);
    }
}

void flipbook_cells_fade_out_at_their_edges() {
    // A cell drawn up to its border shows as a hard line across the sprite.
    for (const char* id : {"flame", "smoke"}) {
        const LibraryTexture* texture = library_texture(id);
        CHECK(texture != nullptr);
        const ajfx::TextureData* data = library_texture_data(texture->entry);
        CHECK(data != nullptr);
        const std::uint32_t cell_w = data->width / texture->columns;
        const std::uint32_t cell_h = data->height / texture->rows;
        const std::vector<std::uint8_t>& pixels = data->mips.front();
        for (std::uint32_t cy = 0U; cy < texture->rows; ++cy) {
            for (std::uint32_t cx = 0U; cx < texture->columns; ++cx) {
                for (std::uint32_t i = 0U; i < cell_w; ++i) {
                    for (const std::uint32_t row : {cy * cell_h, cy * cell_h + cell_h - 1U}) {
                        const std::size_t at = (std::size_t{row} * data->width + cx * cell_w + i) * 4U + 3U;
                        if (pixels[at] > 40U) {
                            throw std::runtime_error{std::string{id} + " cell " + std::to_string(cy * texture->columns + cx) +
                                                     " is opaque at its edge"};
                        }
                    }
                }
            }
        }
    }
}

#if AJFX_STUDIO_TEST_KERNELS
bool kernels_compile_for_every_recipe() {
    ajfx::Result<std::unique_ptr<ajfx::gpu::ShaderCompiler>> compiler = ajfx::d3d12::create_shader_compiler();
    if (!compiler) {
        std::cout << "[skip] kernels_compile_for_every_recipe: " << compiler.error().message << '\n';
        return false;
    }
    for (const Recipe& entry : recipes()) {
        const Compiled compiled = compile(recipe(entry.id), Purpose::Export);
        const ajfx::Result<ajfx::gpu::KernelSet> kernels = ajfx::gpu::compile_kernels(*compiled.effect, *compiler.value());
        if (!kernels) {
            throw std::runtime_error{std::string{entry.id} + ": " + kernels.error().message};
        }
    }
    return true;
}
#endif


// Detaches `kind` in `layer` the way the block menu does.
std::vector<Uid> detach_in_place(Document& document, Layer& layer, Uid uid) {
    const Compiled compiled = compile(document, Purpose::Export);
    std::vector<Block> raw = detach_block(document, compiled, uid);
    std::vector<Uid> uids;
    const std::size_t at = document.find_block(uid)->index;
    remove_block(document, uid);
    for (std::size_t i = 0U; i < raw.size(); ++i) {
        uids.push_back(raw[i].uid);
        insert_block(layer, at + i, std::move(raw[i]));
    }
    return uids;
}

void detached_modules_recombine_into_their_block() {
    for (const char* kind : {"launch", "turbulence", "color", "tumble"}) {
        Document document = recipe(std::string_view{kind} == "tumble" ? "explosion" : "campfire");
        Layer& layer = std::string_view{kind} == "tumble" ? layer_named(document, "Debris") : layer_named(document, "Flames");
        Block& block = block_of(layer, kind);
        block.set("seed", number(3.0F));
        const Block original = block;
        const std::size_t place = document.find_block(original.uid)->index;
        const auto before = described(compile(document, Purpose::Export), layer.uid);
        const std::vector<Uid> modules = detach_in_place(document, layer, original.uid);
        CHECK(!modules.empty());

        // Every module knows the block it came from, and that it is as it was.
        const std::optional<DetachedGroup> group = detached_group(document, modules.back());
        CHECK(group && group->from == original.uid && group->modules == modules.size() && !group->changed);
        CHECK(group->title == block_title(original));

        // Through a file and back, it still does.
        const Loaded loaded = load_document(save_document(document));
        CHECK(loaded.document && *loaded.document == document);

        // Recombined, the block is back where it was, as it was, with its
        // own uid, so its particles are the very ones it had.
        const std::optional<Uid> restored = recombine_block(document, modules.front());
        CHECK(restored && *restored == original.uid);
        Layer& again = *document.layer(layer.uid);
        CHECK(document.find_block(original.uid)->index == place);
        CHECK(*document.block(original.uid) == original);
        for (const Uid uid : modules) {
            CHECK(document.block(uid) == nullptr);
        }
        CHECK(described(compile(document, Purpose::Export), again.uid) == before);
    }

    // An edited module still recombines, and says the edit will be dropped.
    Document document = recipe("campfire");
    Layer& flames = layer_named(document, "Flames");
    const Uid launch = block_of(flames, "launch").uid;
    const std::vector<Uid> modules = detach_in_place(document, flames, launch);
    Block& module = flames.blocks[document.find_block(modules[0])->index];
    std::get<ajfx::Velocity>(std::get<ajfx::InitModule>(*module.raw)).speed_max = 9.0F;
    CHECK(detached_group(document, modules[0])->changed);
    // A module deleted from the group counts as a change too.
    Document deleted = recipe("campfire");
    Layer& deleted_flames = layer_named(deleted, "Flames");
    const std::vector<Uid> color_modules = detach_in_place(deleted, deleted_flames, block_of(deleted_flames, "color").uid);
    CHECK(color_modules.size() >= 2U);
    remove_block(deleted, color_modules[0]);
    CHECK(detached_group(deleted, color_modules[1])->changed);
    // A duplicated module is its own: it is not part of the detach.
    const std::optional<Uid> copy = duplicate_block(document, modules[0]);
    CHECK(copy && !document.block(*copy)->detached);
    CHECK(recombine_block(document, modules[0]) == launch);
    CHECK(document.block(*copy) != nullptr);
}

void the_loop_is_as_long_as_the_effect_plays() {
    // A stream builds up, then shows a whole generation at its full.
    Document document;
    Layer layer;
    layer.uid = document.allocate();
    layer.name = "Sparks";
    layer.life_min = 0.5F;
    layer.life_max = 1.5F;
    layer.blocks.push_back(*make_block(document, "stream"));
    document.layers.push_back(layer);
    CHECK(document.loop_auto);
    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(near(natural_loop_seconds(compiled), 3.2F));
    CHECK(near(loop_seconds(document, compiled), 3.2F));

    // A burst plays until its last particle dies; repeats wait for the last.
    Document bursts = document;
    Layer& burst_layer = bursts.layers[0];
    burst_layer.blocks.clear();
    Block burst = *make_block(bursts, "burst");
    burst.set("time", number(0.5F));
    burst.set("every", number(1.0F));
    burst.set("repeats", number(3.0F));
    burst_layer.blocks.push_back(burst);
    burst_layer.life_max = 2.0F;
    compiled = compile(bursts, Purpose::Preview);
    // The third burst at 2.5 s, its particles gone by 4.5 s.
    CHECK(near(natural_loop_seconds(compiled), 4.7F));

    // The longest layer sets it, a published life at the top of its range.
    Document both = document;
    both.layers.push_back(bursts.layers[0]);
    both.layers.back().uid = both.allocate();
    both.layers.back().blocks[0].uid = both.allocate();
    CHECK(near(natural_loop_seconds(compile(both, Purpose::Preview)), 4.7F));

    // A page's own loop is kept when it sets one.
    document.loop_auto = false;
    document.loop_seconds = 9.0F;
    CHECK(loop_seconds(document, compile(document, Purpose::Preview)) == 9.0F);
    // An effect with nothing born still loops.
    CHECK(natural_loop_seconds(Compiled{}) == 2.0F);
    // Every recipe gets a loop that covers its longest life. A pile lives
    // on after it falls; its loop covers the fall and settling.
    for (const Recipe& entry : recipes()) {
        const Document page = recipe(entry.id);
        const Compiled built = compile(page, Purpose::Preview);
        float longest = 0.0F;
        for (const Layer& l : page.layers) {
            const bool piled = std::any_of(l.blocks.begin(), l.blocks.end(), [](const Block& b) { return b.kind == "pile"; });
            longest = std::max(longest, l.enabled ? (piled ? 5.0F : l.life_max) : 0.0F);
        }
        const float loop = loop_seconds(page, built);
        CHECK(loop >= 1.0F && loop <= 120.0F && (built.effect.has_value() ? loop >= longest : true));
    }
}

// A Prop block becomes an object standing in the layer's physics world, and
// the Bucket of stones recipe plays the whole shot: the balls start inside the
// bucket, it tips over, and they end on the floor.
void props_stand_in_the_world_and_spill_what_they_hold() {
    Document document = recipe("bucket_spill");
    const Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const ajfx::Emitter& balls = emitter_of(compiled, layer_named(document, "Bucket of stones"));
    const auto* physics = find_module<ajfx::RigidPhysics>(balls);
    CHECK(physics != nullptr && physics->props.size() == 1U);
    const ajfx::RigidProp& bucket = physics->props[0];
    // A bucket is hollow, so it is built of convex pieces rather than wrapped,
    // and it is moved rather than knocked about so the tip-over is authored.
    CHECK(bucket.shape == ajfx::PropShape::HollowCylinder);
    CHECK(bucket.motion == ajfx::PropMotion::Driven);
    CHECK(near(bucket.radius, 0.5F) && near(bucket.height, 0.8F) && near(bucket.wall, 0.05F));
    CHECK(near(bucket.position.y, 0.4F));
    CHECK(near(bucket.to_turn.z, 115.0F));
    // An untouched "moves to" leaves the prop where it stands.
    CHECK(bucket.to_position == bucket.position);

    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
    ajfx::Simulation simulation =
        ajfx::Simulation::create(*compiled.effect, 1U,
                                 ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = solver.get()})
            .value();
    std::size_t index = 0U;
    while (compiled.effect->emitters[index].name != balls.name) {
        ++index;
    }
    const auto highest = [&] {
        float top = 0.0F;
        for (const ajfx::Vec3 p : simulation.emitter(index).position) {
            top = std::max(top, p.y);
        }
        return top;
    };
    // The prop stands in the world before the first step, and the balls are
    // inside it: above its floor at 0.05, within its 0.45 inside radius.
    CHECK(simulation.emitter(index).props.size() == 1U);
    CHECK(near(simulation.emitter(index).props[0].position.y, 0.4F));
    CHECK(simulation.emitter(index).alive() == 24U);
    for (const ajfx::Vec3 p : simulation.emitter(index).position) {
        CHECK(p.y > 0.05F && std::sqrt(p.x * p.x + p.z * p.z) < 0.45F);
    }
    const float held = highest();
    simulation.advance_to(300U);
    const float spilled = highest();
    std::cout << "  the bucket holds its balls at " << held << " m and spills them to " << spilled << " m\n";
    // Everything ends on the floor, a ball's radius up, not still cradled.
    CHECK(spilled < 0.2F && held > 0.05F);
    CHECK(simulation.emitter(index).alive() == 24U);
}

// Whether any note says this.
bool said(const std::vector<Advice>& notes, std::string_view fragment) {
    return std::any_of(notes.begin(), notes.end(), [&](const Advice& note) {
        return note.message.find(fragment) != std::string::npos;
    });
}

void a_break_uses_the_parts_beside_it() {
    // A crate of its own, broken into four parts an artist placed: a lid,
    // two walls and a base, as the Crates recipe did before it cut instead.
    Document document;
    Layer crates = make_layer(document, LayerKind::Rigid, "Crates");
    insert_block(crates, 0U, *make_block(document, "pile"));
    insert_block(crates, 1U, *make_block(document, "gravity"));
    insert_block(crates, 2U, *make_block(document, "appearance"));
    insert_block(crates, 3U, *make_block(document, "break"));
    for (std::size_t i = 0U; i < 4U; ++i) {
        insert_block(crates, crates.blocks.size(), *make_block(document, "part"));
    }
    document.layers.push_back(std::move(crates));
    Layer& layer = document.layers[0];
    block_of(layer, "pile").set("count", number(4.0F));
    block_of(layer, "appearance").set("material", choice("debris"));
    block_of(layer, "appearance").set("mesh", choice("cube"));
    const ajfx::Vec3 faces[4] = {{0.0F, 0.5F, 0.0F}, {0.5F, 0.0F, 0.0F}, {-0.5F, 0.0F, 0.0F}, {0.0F, -0.5F, 0.0F}};
    std::size_t at = 0U;
    for (Block& block : layer.blocks) {
        if (block.kind == "part") {
            block.set("at", vec3(faces[at].x, faces[at].y, faces[at].z));
            block.set("size", number(0.45F));
            ++at;
        }
    }

    const Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    // No cut was asked for, so the parts are what it breaks into, and they
    // are in the effect from the compiler rather than from the builder.
    CHECK(compiled.cuts.empty());
    const ajfx::Emitter& bodies = compiled.effect->emitters[0];
    const auto* physics = find_module<ajfx::RigidPhysics>(bodies);
    const auto* breaking = find_module<ajfx::Break>(bodies);
    CHECK(physics != nullptr && breaking != nullptr);
    CHECK(physics->bodies.size() == 4U && breaking->pieces.size() == 4U);
    CHECK(breaking->body == 0U);
    for (std::size_t i = 0U; i < 4U; ++i) {
        CHECK(breaking->pieces[i].body == static_cast<std::uint32_t>(i + 1U));
        CHECK(physics->bodies[i].shape == ajfx::RigidShape::Hull);
        CHECK(!physics->bodies[i].mesh.empty() && physics->bodies[i].draw == physics->bodies[i].mesh);
        // A part is made of what the layer's pieces are made of.
        CHECK(near(physics->bodies[i].density, number_of(*layer.world, "density")));
    }
    CHECK(near(breaking->pieces[0].offset.y, 0.5F));
    CHECK(near(breaking->pieces[1].offset.x, 0.5F));
    CHECK(ajfx::check_meshes(*compiled.effect, compiled.meshes).ok());
}

// The Crates recipe cuts its crates rather than placing parts, so the pieces
// are the crate: the cut is named when it compiles and made when the models
// are read.
void the_crates_recipe_cuts_its_crates() {
    Document document = recipe("breaking_crates");
    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(compiled.cuts.size() == 1U && compiled.cuts[0].cells == 6U);
    std::string issue;
    make_cuts(compiled, issue);
    CHECK(issue.empty() && compiled.effect.has_value());

    const ajfx::Emitter& crates = emitter_of(compiled, layer_named(document, "Crates and a dropped ball"));
    const auto* physics = find_module<ajfx::RigidPhysics>(crates);
    const auto* breaking = find_module<ajfx::Break>(crates);
    CHECK(physics != nullptr && breaking != nullptr);
    CHECK(physics->bodies.size() == 6U && breaking->pieces.size() == 6U);
    // Nine crates into six apiece is 54, which the layer has room for.
    CHECK(crates.capacity >= 54U);

    // And it breaks when the ball lands: more bodies than the nine that
    // stood, every one of them a crate or one of its cells.
    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
    std::size_t index = 0U;
    while (compiled.effect->emitters[index].name != crates.name) {
        ++index;
    }
    ajfx::Simulation simulation =
        ajfx::Simulation::create(*compiled.effect, 1U,
                                 ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = solver.get()})
            .value();
    simulation.advance_to(12U);
    CHECK(simulation.emitter(index).alive() == 9U);
    simulation.advance_to(240U);
    const ajfx::EmitterState& s = simulation.emitter(index);
    std::cout << "  nine cut crates became " << s.alive() << " bodies, " << s.refused << " refused\n";
    CHECK(s.alive() > 9U);
    CHECK(s.refused == 0U);
    for (const std::uint32_t kind : s.body_kind) {
        CHECK(kind <= 6U);
    }
}

void snow_lands_on_the_pieces_of_the_layer_above() {
    Document document = recipe("breaking_crates");
    const Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const Layer& crates = layer_named(document, "Crates and a dropped ball");
    const Layer& snow = layer_named(document, "Snow");
    const ajfx::Emitter& flakes = emitter_of(compiled, snow);
    const auto* lands = find_module<ajfx::BodyCollision>(flakes);
    CHECK(lands != nullptr);
    // It lands on the crates: the rigid layer above it on the page, named by
    // the emitter that layer compiled to.
    CHECK(lands->emitter == emitter_of(compiled, crates).name);
    // Sticking is friction 1 and no bounce, as the ground's Stick is.
    CHECK(near(lands->friction.value, 1.0F) && near(lands->restitution.value, 0.0F));
    CHECK(!lands->kill);
    // And the crates come first in the effect, which is what the runtime
    // needs: the bodies met have already moved this step.
    std::size_t crates_at = 0U;
    std::size_t snow_at = 0U;
    for (std::size_t i = 0U; i < compiled.effect->emitters.size(); ++i) {
        if (compiled.effect->emitters[i].name == lands->emitter) {
            crates_at = i;
        }
        if (compiled.effect->emitters[i].name == flakes.name) {
            snow_at = i;
        }
    }
    CHECK(crates_at < snow_at);

    // The flakes settle on the crates rather than passing through them: with
    // nothing to land on they would all end on the floor.
    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
    ajfx::Simulation simulation =
        ajfx::Simulation::create(*compiled.effect, 1U,
                                 ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = solver.get()})
            .value();
    // The flakes drift down slowly, so this is ten seconds in: long after
    // the ball has landed and the crates have come apart.
    simulation.advance_to(600U);
    const ajfx::EmitterState& settled = simulation.emitter(snow_at);
    std::size_t aloft = 0U;
    for (std::size_t i = 0U; i < settled.alive(); ++i) {
        if (settled.position[i].y > 0.2F && std::sqrt(ajfx::dot(settled.velocity[i], settled.velocity[i])) < 0.02F) {
            ++aloft;
        }
    }
    std::cout << "  " << aloft << " flakes rest above the floor, on what the crates left\n";
    CHECK(aloft > 5U);
}

// Landing on pieces needs pieces above: the coach says so rather than the
// flakes quietly falling through.
void landing_on_pieces_needs_a_layer_above() {
    Document document;
    Layer snow = make_layer(document, LayerKind::Particles, "Snow");
    insert_block(snow, 0U, *make_block(document, "stream"));
    insert_block(snow, 1U, *make_block(document, "pieces"));
    document.layers.push_back(std::move(snow));
    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    // Nothing to land on, so the block compiles to nothing and the layer
    // still plays.
    CHECK(find_module<ajfx::BodyCollision>(compiled.effect->emitters[0]) == nullptr);
    CHECK(said(advise(document, compiled), "no physics layer above this one"));

    // With a rigid layer above it, the block binds to that layer.
    Document ordered;
    Layer crates = make_layer(ordered, LayerKind::Rigid, "Crates");
    insert_block(crates, 0U, *make_block(ordered, "burst"));
    ordered.layers.push_back(std::move(crates));
    Layer flakes = make_layer(ordered, LayerKind::Particles, "Snow");
    insert_block(flakes, 0U, *make_block(ordered, "stream"));
    insert_block(flakes, 1U, *make_block(ordered, "pieces"));
    ordered.layers.push_back(std::move(flakes));
    compiled = compile(ordered, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const auto* lands = find_module<ajfx::BodyCollision>(compiled.effect->emitters[1]);
    CHECK(lands != nullptr && lands->emitter == compiled.effect->emitters[0].name);
    CHECK(advise(ordered, compiled).empty() || !said(advise(ordered, compiled), "no physics layer above"));
}

// A Break block set to cut its model names the cut and places nothing: the
// model is not read until the package is built, so the cells are made there.
void a_break_can_cut_its_own_model_into_pieces() {
    Document document;
    Layer crates = make_layer(document, LayerKind::Rigid, "Crates");
    insert_block(crates, 0U, *make_block(document, "pile"));
    insert_block(crates, 1U, *make_block(document, "gravity"));
    insert_block(crates, 2U, *make_block(document, "appearance"));
    insert_block(crates, 3U, *make_block(document, "break"));
    document.layers.push_back(std::move(crates));
    Layer& layer = document.layers[0];
    block_of(layer, "appearance").set("material", choice("debris"));
    block_of(layer, "appearance").set("mesh", choice("cube"));
    block_of(layer, "break").set("into", choice("cut"));
    block_of(layer, "break").set("cells", number(6.0F));

    // The compiler names the cut and leaves the layer unbroken, since it has
    // not read the model.
    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(compiled.cuts.size() == 1U);
    CHECK(compiled.cuts[0].cells == 6U && compiled.cuts[0].model == "cube");
    CHECK(compiled.cuts[0].emitter == emitter_of(compiled, layer).name);
    CHECK(find_module<ajfx::Break>(compiled.effect->emitters[0]) == nullptr);

    // Making the cut puts the cells in: a body kind and a mesh each, and the
    // Break that scatters them.
    std::string issue;
    make_cuts(compiled, issue);
    CHECK(issue.empty());
    CHECK(compiled.effect.has_value());
    const ajfx::Emitter& bodies = compiled.effect->emitters[0];
    const auto* physics = find_module<ajfx::RigidPhysics>(bodies);
    const auto* breaking = find_module<ajfx::Break>(bodies);
    CHECK(physics != nullptr && breaking != nullptr);
    CHECK(physics->bodies.size() == 6U && breaking->pieces.size() == 6U);
    for (std::size_t i = 0U; i < 6U; ++i) {
        CHECK(breaking->pieces[i].body == static_cast<std::uint32_t>(i + 1U));
        CHECK(physics->bodies[i].shape == ajfx::RigidShape::Hull);
        CHECK(physics->bodies[i].mesh == "cube.cell." + std::to_string(i));
        CHECK(physics->bodies[i].draw == physics->bodies[i].mesh);
        // A cell is placed at its own middle inside the model it was cut
        // from, which is a unit across, and is left at the size it was cut.
        CHECK(near(breaking->pieces[i].size, 1.0F));
        CHECK(std::fabs(breaking->pieces[i].offset.x) < 0.5F);
        CHECK(std::fabs(breaking->pieces[i].offset.y) < 0.5F);
        CHECK(std::fabs(breaking->pieces[i].offset.z) < 0.5F);
    }
    // Every cell's mesh is in the package, and what came out still validates.
    CHECK(ajfx::check_meshes(*compiled.effect, compiled.meshes).ok());
    CHECK(ajfx::validate(*compiled.effect).ok());

    // The same page cut again gives the same cells, as a bake must.
    Compiled again = compile(document, Purpose::Preview);
    std::string second;
    make_cuts(again, second);
    CHECK(second.empty() && again.effect.has_value());
    CHECK(*find_module<ajfx::RigidPhysics>(again.effect->emitters[0]) == *physics);
    CHECK(*find_module<ajfx::Break>(again.effect->emitters[0]) == *breaking);

    // And a different cut seed cuts it differently.
    block_of(document.layers[0], "break").set("cut_seed", number(5.0F));
    Compiled other = compile(document, Purpose::Preview);
    std::string third;
    make_cuts(other, third);
    CHECK(third.empty() && other.effect.has_value());
    CHECK(!(*find_module<ajfx::Break>(other.effect->emitters[0]) == *breaking));
    std::cout << "  six cells cut from the cube, the same twice over and different on another seed\n";
}

// Breaking is two halves, and half of it alone does nothing: the coach says
// which half is missing rather than the compiler guessing at the other.
void half_a_break_is_refused_with_the_missing_half() {
    const auto page = [&](bool with_break, bool with_part) {
        Document document;
        Layer layer = make_layer(document, LayerKind::Rigid, "Crate");
        insert_block(layer, 0U, *make_block(document, "burst"));
        insert_block(layer, layer.blocks.size(), *make_block(document, "appearance"));
        if (with_break) {
            insert_block(layer, layer.blocks.size(), *make_block(document, "break"));
        }
        if (with_part) {
            insert_block(layer, layer.blocks.size(), *make_block(document, "part"));
        }
        document.layers.push_back(std::move(layer));
        return document;
    };
    // A Break with nothing to break into leaves the pieces whole.
    Document only_break = page(true, false);
    Compiled compiled = compile(only_break, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(find_module<ajfx::Break>(compiled.effect->emitters[0]) == nullptr);
    std::vector<Advice> notes = advise(only_break, compiled);
    CHECK(said(notes, "Nothing is set to break off yet"));

    // A Part with nothing that breaks is never made.
    Document only_part = page(false, true);
    compiled = compile(only_part, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(find_module<ajfx::Break>(compiled.effect->emitters[0]) == nullptr);
    notes = advise(only_part, compiled);
    CHECK(said(notes, "Nothing breaks, so this part is never made"));

    // Both halves, and the layer breaks.
    Document both = page(true, true);
    compiled = compile(both, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const auto* breaking = find_module<ajfx::Break>(compiled.effect->emitters[0]);
    CHECK(breaking != nullptr && breaking->pieces.size() == 1U);
    // A layer that draws sprites cannot draw a part as its own model, so the
    // part still collides as its shape and the coach offers the fix.
    const auto* physics = find_module<ajfx::RigidPhysics>(compiled.effect->emitters[0]);
    CHECK(physics != nullptr && physics->bodies.size() == 1U);
    CHECK(physics->bodies[0].draw.empty() && !physics->bodies[0].mesh.empty());
    CHECK(said(advise(both, compiled), "Set Appearance to debris"));
}

// A prop needs no Physics block of its own, and what it cannot be is said on
// the block at fault rather than silently approximated.
void a_prop_takes_default_physics_and_names_what_it_cannot_be() {
    Document document;
    Layer layer = make_layer(document, LayerKind::Rigid, "Crate");
    insert_block(layer, 0U, *make_block(document, "prop"));
    document.layers.push_back(std::move(layer));
    const Uid prop = document.layers[0].blocks[0].uid;

    // A rigid layer always has a world, whatever is on its list: that is
    // what its kind means, so a prop stands in one without being asked.
    const Compiled bare = compile(document, Purpose::Preview);
    CHECK(bare.error.empty() && bare.effect.has_value());
    const auto* physics = find_module<ajfx::RigidPhysics>(bare.effect->emitters[0]);
    CHECK(physics != nullptr && physics->props.size() == 1U);
    // With no model picked it stands in as a plain block, so the layer still
    // plays, and the coach asks for one.
    CHECK(physics->props[0].shape == ajfx::PropShape::Box);
    const std::vector<Advice> notes = advise(document, bare);
    CHECK(std::any_of(notes.begin(), notes.end(), [&](const Advice& a) {
        return a.block == prop && a.message.find("Pick a model") != std::string::npos;
    }));

    // A prop that moves cannot collide as its own shape, whichever way it
    // moves: a collision mesh can only stand still. The coach says so on the
    // block and its fix is a shape that can move.
    document.layers[0].blocks[0].set("model", choice("bucket.glb"));
    for (const char* motion : {"loose", "driven"}) {
        document.layers[0].blocks[0].set("motion", choice(motion));
        const Compiled moving = compile(document, Purpose::Preview);
        const std::vector<Advice> notes_for = advise(document, moving);
        CHECK(std::any_of(notes_for.begin(), notes_for.end(), [&](const Advice& a) {
            return a.block == prop && a.level == AdviceLevel::Error && a.fix &&
                   a.message.find("cannot collide as its own shape") != std::string::npos;
        }));
    }
    // Standing still it is fine, which is what a model-shaped prop is for.
    document.layers[0].blocks[0].set("motion", choice("fixed"));
    const Compiled standing = compile(document, Purpose::Preview);
    CHECK(standing.error.empty());
    CHECK(find_module<ajfx::RigidPhysics>(standing.effect->emitters[0])->props[0].shape ==
          ajfx::PropShape::Mesh);

    document.layers[0].blocks[0].set("motion", choice("loose"));
    const Compiled loose = compile(document, Purpose::Preview);
    std::vector<Advice> refused = advise(document, loose);
    const auto hollow = std::find_if(refused.begin(), refused.end(), [&](const Advice& a) {
        return a.block == prop && a.level == AdviceLevel::Error && a.fix;
    });
    CHECK(hollow != refused.end());
    hollow->fix(document);
    const Compiled fixed = compile(document, Purpose::Preview);
    CHECK(fixed.error.empty());
    const auto* mended = find_module<ajfx::RigidPhysics>(fixed.effect->emitters[0]);
    CHECK(mended != nullptr && mended->props[0].shape == ajfx::PropShape::HollowCylinder);
    CHECK(mended->props[0].motion == ajfx::PropMotion::Loose);
    // The model it names travels in the package, for the host to draw by.
    CHECK(!mended->props[0].mesh.empty());
    CHECK(std::any_of(fixed.requests.begin(), fixed.requests.end(), [&](const AssetRequest& r) {
        return r.block == prop && r.kind == AssetRequest::Kind::Mesh && r.path == "bucket.glb";
    }));
}

void piles_stand_until_the_effect_plays() {
    Document document = recipe("rock_pile");
    const Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const ajfx::Emitter& rocks = emitter_of(compiled, layer_named(document, "Rock pile"));
    const auto* pile = find_module<ajfx::SpawnPile>(rocks);
    CHECK(pile != nullptr && pile->count == 220U && pile->release == 0.0F);
    CHECK(rocks.capacity == 256U);
    // Physics turns them; Tumble only sets how each rock lies.
    CHECK(find_module<ajfx::RigidPhysics>(rocks) != nullptr);
    CHECK(find_module<ajfx::RigidBody>(rocks) == nullptr);
    CHECK(find_module<ajfx::InitTumble>(rocks) != nullptr);
    CHECK(near(natural_loop_seconds(compiled), 5.2F));

    // Standing at the start, collapsing as it plays.
    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
    ajfx::Simulation simulation =
        ajfx::Simulation::create(*compiled.effect, 1U,
                                 ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = solver.get()})
            .value();
    std::size_t index = 0U;
    while (compiled.effect->emitters[index].name != rocks.name) {
        ++index;
    }
    const auto top = [&] {
        float highest = 0.0F;
        for (const ajfx::Vec3 p : simulation.emitter(index).position) {
            highest = std::max(highest, p.y);
        }
        return highest;
    };
    CHECK(simulation.emitter(index).alive() == 220U);
    const float standing = top();
    simulation.advance_to(180U);
    std::cout << "  the rock pile stands " << standing << " m and comes down to " << top() << " m\n";
    CHECK(standing > 0.8F && top() < standing - 0.2F);

    // The world is the layer's own, and its values reach the module. A
    // rigid layer cannot be without one, so there is no "no physics" case.
    Layer& layer = layer_named(document, "Rock pile");
    CHECK(layer.kind == LayerKind::Rigid && layer.world.has_value());
    CHECK(near(find_module<ajfx::RigidPhysics>(rocks)->friction, 0.8F));
    layer.world->set("friction", number(0.25F));
    const Compiled changed = compile(document, Purpose::Export);
    CHECK(changed.error.empty());
    CHECK(near(find_module<ajfx::RigidPhysics>(emitter_of(changed, layer))->friction, 0.25F));
    CHECK(find_module<ajfx::RigidBody>(emitter_of(changed, layer)) == nullptr);
}

ImportedMesh unit_cube() {
    const ajfx::MeshData cube = ajfx::cube_mesh();
    ImportedMesh mesh;
    for (const ajfx::MeshVertex& v : cube.vertices) {
        mesh.positions.push_back(v.position);
    }
    mesh.indices = cube.indices;
    return mesh;
}

void own_pictures_and_models_go_into_the_package() {
    // A picture is a sprite texture, halved until it fits.
    ImportedImage image;
    image.width = 2048U;
    image.height = 512U;
    image.rgba.assign(static_cast<std::size_t>(image.width) * image.height * 4U, 200U);
    std::string error;
    const std::optional<ajfx::TextureData> texture = sprite_texture(image, error);
    CHECK(texture && texture->width == max_sprite_size && texture->height == 256U && texture->mips.size() > 1U);
    CHECK(!sprite_texture(ImportedImage{}, error) && !error.empty());

    // A model for debris is centred and fitted to a metre.
    ImportedMesh brick = unit_cube();
    for (ajfx::Vec3& p : brick.positions) {
        p = ajfx::Vec3{p.x * 4.0F + 10.0F, p.y * 2.0F, p.z * 1.0F - 3.0F};
    }
    const std::optional<ajfx::MeshData> debris = debris_mesh(brick, error);
    CHECK(debris.has_value());
    float lo = 1.0F;
    float hi = -1.0F;
    for (const ajfx::MeshVertex& v : debris->vertices) {
        lo = std::min(lo, v.position.x);
        hi = std::max(hi, v.position.x);
        CHECK(std::fabs(v.position.y) <= 0.2501F && std::fabs(v.position.z) <= 0.1251F);
    }
    CHECK(near(lo, -0.5F) && near(hi, 0.5F));

    // A page naming files compiles to entries the builder fills, and two
    // blocks using one file share its entry.
    Document document = recipe("explosion");
    Layer& sparks = document.layers[0];
    Block& look = block_of(sparks, "appearance");
    look.set("material", choice("smoke"));
    look.set("texture", choice("file"));
    look.set("texture_file", choice("C:/art/puff.png"));
    look.set("columns", number(4.0F));
    look.set("rows", number(2.0F));
    Block& debris_look = block_of(layer_named(document, "Debris"), "appearance");
    debris_look.set("mesh", choice("file"));
    debris_look.set("mesh_file", choice("C:/art/rock.glb"));
    Block collider = *make_block(document, "mesh_collider");
    collider.set("file", choice("C:/art/table.obj"));
    collider.set("position", vec3(0.0F, 0.5F, 0.0F));
    insert_block(sparks, sparks.blocks.size(), collider);
    const Compiled compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty() && compiled.requests.size() == 3U);
    std::map<AssetRequest::Kind, const AssetRequest*> by_kind;
    for (const AssetRequest& request : compiled.requests) {
        by_kind[request.kind] = &request;
        CHECK(request.entry.starts_with("user."));
    }
    CHECK(by_kind.at(AssetRequest::Kind::Texture)->path == "C:/art/puff.png");
    CHECK(by_kind.at(AssetRequest::Kind::Mesh)->path == "C:/art/rock.glb");
    const AssetRequest& field = *by_kind.at(AssetRequest::Kind::Field);
    CHECK(field.path == "C:/art/table.obj" && field.cells == 72U && near(field.scale, document.scale));
    const ajfx::Emitter& emitter = emitter_of(compiled, sparks);
    CHECK(emitter.renderer.texture == by_kind.at(AssetRequest::Kind::Texture)->entry);
    CHECK(emitter.renderer.flipbook_columns == 4U && emitter.renderer.flipbook_rows == 2U);
    bool collides = false;
    for (const ajfx::UpdateModule& module : emitter.update) {
        if (const auto* sdf = std::get_if<ajfx::SdfCollision>(&module)) {
            collides = sdf->field == field.entry;
        }
    }
    CHECK(collides);
    // No file picked yet: a soft dot, chips, nothing to collide with.
    look.set("texture_file", choice(""));
    debris_look.set("mesh_file", choice(""));
    sparks.blocks.back().set("file", choice(""));
    const Compiled waiting = compile(document, Purpose::Export);
    CHECK(waiting.error.empty() && waiting.requests.empty());
    CHECK(emitter_of(waiting, layer_named(document, "Debris")).renderer.mesh == "chip");
    // Saved and loaded, the files stay picked.
    look.set("texture_file", choice("C:/art/puff.png"));
    const Loaded loaded = load_document(save_document(document));
    CHECK(loaded.document && *loaded.document == document);
}

void model_colliders_bake_to_distance_fields() {
    const ImportedMesh cube = unit_cube();
    CHECK(closed_mesh(cube));
    std::string error;
    const std::optional<ColliderBake> bake = bake_collider(cube, 2.0F, 32U, ColliderSolid::Auto, error);
    CHECK(bake && bake->solid);
    const ajfx::DistanceField& field = bake->field;
    // A 2 m cube: inside below zero, outside above, about right in metres.
    const auto at = [&](ajfx::Vec3 p) { return ajfx::sample_field(field, p)->distance; };
    CHECK(near(at(ajfx::Vec3{0.0F, 0.0F, 0.0F}), -1.0F, 0.03F));
    CHECK(near(at(ajfx::Vec3{1.15F, 0.0F, 0.0F}), 0.15F, 0.03F));
    CHECK(near(at(ajfx::Vec3{0.3F, -1.1F, 0.2F}), 0.1F, 0.03F));
    // Its gradient points out of the surface.
    CHECK(ajfx::sample_field(field, ajfx::Vec3{1.1F, 0.1F, 0.0F})->gradient.x > 0.5F);
    // Within the runtime's limits, and readable from a package.
    CHECK(field.nx <= ajfx::max_field_size && field.distances.size() <= ajfx::max_field_samples);
    CHECK(ajfx::read_field(ajfx::write_field(field)).ok());

    // An open model is a thin surface: kept off both sides.
    ImportedMesh floor;
    floor.positions = {{-1.0F, 0.0F, -1.0F}, {1.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 1.0F}, {-1.0F, 0.0F, 1.0F}};
    floor.indices = {0U, 2U, 1U, 0U, 3U, 2U};
    CHECK(!closed_mesh(floor));
    const std::optional<ColliderBake> sheet = bake_collider(floor, 1.0F, 32U, ColliderSolid::Auto, error);
    CHECK(sheet && !sheet->solid);
    CHECK(ajfx::sample_field(sheet->field, ajfx::Vec3{0.0F, 0.0F, 0.0F})->distance < 0.0F);
    CHECK(near(ajfx::sample_field(sheet->field, ajfx::Vec3{0.1F, 0.15F, 0.2F})->distance, 0.15F - sheet->field.spacing * 0.75F, 0.05F));

    // Particles thrown at a baked model bounce off it on the CPU reference.
    Document document;
    Layer layer;
    layer.uid = document.allocate();
    layer.name = "Rain";
    layer.life_min = layer.life_max = 2.0F;
    Block burst = *make_block(document, "burst");
    burst.set("count", number(50.0F));
    layer.blocks.push_back(burst);
    Block shape = *make_block(document, "shape");
    shape.set("shape", choice("point"));
    shape.set("offset", vec3(0.0F, 3.0F, 0.0F));
    layer.blocks.push_back(shape);
    layer.blocks.push_back(*make_block(document, "gravity"));
    Block collider = *make_block(document, "mesh_collider");
    collider.set("file", choice("cube.obj"));
    collider.set("scale", number(2.0F));
    layer.blocks.push_back(collider);
    document.layers.push_back(layer);
    Compiled compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty() && compiled.requests.size() == 1U);
    const AssetRequest& request = compiled.requests[0];
    const std::optional<ColliderBake> baked = bake_collider(cube, request.scale, request.cells, request.solid, error);
    CHECK(baked.has_value());
    compiled.fields.emplace(request.entry, baked->field);
    ajfx::Result<ajfx::Simulation> simulation = ajfx::Simulation::create(*compiled.effect, 1U, compiled.fields);
    CHECK(simulation.ok());
    for (int step = 0; step < 90; ++step) {
        simulation.value().step();
    }
    const ajfx::EmitterState& state = simulation.value().emitter(0U);
    CHECK(state.alive() == 50U);
    for (std::size_t i = 0U; i < state.alive(); ++i) {
        // Resting on the cube's top, a metre up, rather than through it.
        CHECK(state.position[i].y > 0.9F);
    }
    // The outline draws the cube's twelve edges.
    CHECK(mesh_outline(cube, 100U).size() == 24U);
}

void studio_comes_back_as_it_was_left() {
    SessionState session;
    session.page = "C:/effects/fire.ajfxstudio";
    session.unsaved = save_document(recipe("campfire"));
    session.view = PageViewState{{3U, 9U}, {12U}, {12U}, 12U, 3U, 840.5F};
    remember_view(session, "C:/effects/other.ajfxstudio", PageViewState{{1U}, {}, {}, 0U, 1U, 20.0F});
    remember_view(session, session.page, session.view);
    session.width = 1800U;
    session.height = 1000U;
    session.split = 0.4F;
    session.palette = false;
    session.palette_folded = {"Contact", "Under the hood"};
    session.guides = false;
    session.handles = false;
    session.paths = true;
    session.figure = false;
    session.expert = true;
    session.looping = false;
    session.camera = std::array<float, 6>{0.2F, 0.05F, 7.5F, 0.0F, 1.2F, -0.5F};
    session.seconds = 2.5;
    session.playing = false;
    const SessionState loaded = load_session(save_session(session));
    CHECK(loaded == session);
    CHECK(loaded.recent.front().first == session.page && loaded.recent.size() == 2U);
    CHECK(remembered_view(loaded, "C:/effects/other.ajfxstudio")->scroll == 20.0F);
    CHECK(!remembered_view(loaded, "C:/nowhere.ajfxstudio"));
    // Anything unreadable is the default, and nothing is held against it.
    CHECK(load_session("not json") == SessionState{});
    CHECK(load_session(R"({"format":"ajfx-studio-session","split":7,"camera":[1,2]})").split == 0.85F);
    for (int i = 0; i < 40; ++i) {
        remember_view(session, "page" + std::to_string(i), PageViewState{});
    }
    CHECK(session.recent.size() == remembered_pages && session.recent.front().first == "page39");
}

// A layer's kind is what decides which blocks it can hold, so the catalog's
// answer, what the page offers, what a drop accepts and what the compiler
// will build must be one answer. Checked against the runtime's own refusal
// list: every block the catalog keeps out of a rigid layer is one
// ajfx::validate would refuse there, and every block it lets in compiles.
void a_layer_kind_decides_which_blocks_it_can_hold() {
    // Every block in the catalog belongs to at least one kind, except the
    // world, which is the layer's own settings and is never offered.
    std::size_t offered_in_rigid = 0U;
    for (const BlockDef& def : block_catalog()) {
        if (def.kind == world_block_kind) {
            CHECK(def.layers == in_no_layer && !def.detachable);
            continue;
        }
        CHECK(def.layers != in_no_layer);
        // A block kept out of a kind says why, for the artist who looks.
        CHECK(def.layers == in_any_layer || !def.only_in.empty());
        offered_in_rigid += fits(def.layers, LayerKind::Rigid) ? 1U : 0U;
    }
    CHECK(offered_in_rigid > 0U);

    // The forces a rigid layer refuses are kept out of it, and the ones it
    // takes are let in.
    for (const char* kept_out : {"turbulence", "swirl", "attract", "draw_in", "spin", "mesh_collider"}) {
        CHECK(block_fits(kept_out, LayerKind::Particles));
        CHECK(!block_fits(kept_out, LayerKind::Rigid));
    }
    for (const char* rigid_only : {"prop", "pile"}) {
        CHECK(block_fits(rigid_only, LayerKind::Rigid));
        CHECK(!block_fits(rigid_only, LayerKind::Particles));
    }
    for (const char* both : {"stream", "burst", "gravity", "wind", "drag", "ground", "tumble", "color", "light"}) {
        CHECK(block_fits(both, LayerKind::Particles) && block_fits(both, LayerKind::Rigid));
    }

    // Every block a rigid layer offers really does compile there: the
    // catalog's list and the runtime's refusals agree, which is the whole
    // point of filtering rather than explaining afterwards.
    for (const BlockDef& def : block_catalog()) {
        if (def.kind == raw_block_kind || def.kind == world_block_kind || !fits(def.layers, LayerKind::Rigid)) {
            continue;
        }
        Document document;
        Layer layer = make_layer(document, LayerKind::Rigid, "Bodies");
        insert_block(layer, 0U, *make_block(document, "pile"));
        insert_block(layer, 1U, *make_block(document, "gravity"));
        if (def.kind != "pile" && def.kind != "gravity") {
            insert_block(layer, 2U, *make_block(document, def.kind));
        }
        document.layers.push_back(std::move(layer));
        const Compiled compiled = compile(document, Purpose::Export);
        if (!compiled.error.empty()) {
            throw std::runtime_error{std::string{def.title} + " is offered in a rigid layer but will not compile: " +
                                     compiled.error};
        }
    }

    // A block the kind does not have cannot be moved into such a layer, and
    // adding one by hand is reported rather than quietly ignored.
    Document document;
    Layer rigid = make_layer(document, LayerKind::Rigid, "Rocks");
    insert_block(rigid, 0U, *make_block(document, "pile"));
    document.layers.push_back(std::move(rigid));
    Layer sparks = make_layer(document, LayerKind::Particles, "Sparks");
    insert_block(sparks, 0U, *make_block(document, "stream"));
    const Uid swirl = insert_block(sparks, 1U, *make_block(document, "swirl"));
    document.layers.push_back(std::move(sparks));
    CHECK(!move_block(document, swirl, document.layers[0].uid, 0U));
    CHECK(document.find_block(swirl)->layer->uid == document.layers[1].uid);
    document.layers[0].blocks.push_back(*make_block(document, "swirl"));
    const Compiled smuggled = compile(document, Purpose::Export);
    CHECK(smuggled.error.find("Swirl does not belong") != std::string::npos);
}

// Changing a layer's kind takes its blocks with it, and refuses rather than
// throwing away anything the artist authored.
void changing_a_layer_kind_keeps_what_fits_and_refuses_the_rest() {
    Document document;
    Layer layer = make_layer(document, LayerKind::Particles, "Embers");
    insert_block(layer, 0U, *make_block(document, "stream"));
    insert_block(layer, 1U, *make_block(document, "gravity"));
    const Uid sized = insert_block(layer, 2U, *make_block(document, "size"));
    document.layers.push_back(std::move(layer));
    Layer& work = document.layers[0];
    work.blocks[2].set("over_life", choice("shrink"));

    // Nothing is in the way, so it changes: it gains a world, and the
    // settings a rigid layer cannot act on are cleared rather than kept
    // where no one can see them.
    CHECK(blocks_against_kind(work, LayerKind::Rigid).empty());
    CHECK(set_layer_kind(document, work, LayerKind::Rigid));
    CHECK(work.kind == LayerKind::Rigid && work.world && work.world->uid != 0U);
    CHECK(document.block(work.world->uid) == &*work.world);
    CHECK(document.block(sized)->find("over_life") == nullptr);
    CHECK(compile(document, Purpose::Export).error.empty());

    // A block the other kind does not have stops the change and is named,
    // so nothing an artist made is dropped behind their back.
    insert_block(work, work.blocks.size(), *make_block(document, "prop"));
    const std::vector<std::string> blocking = blocks_against_kind(work, LayerKind::Particles);
    CHECK(blocking.size() == 1U && blocking[0] == "Prop");
    CHECK(!set_layer_kind(document, work, LayerKind::Particles));
    CHECK(work.kind == LayerKind::Rigid);

    // Going back drops the world, since a particle layer has none.
    remove_block(document, work.blocks.back().uid);
    CHECK(set_layer_kind(document, work, LayerKind::Particles));
    CHECK(!work.world.has_value());
}

// A page saved before layers had a kind still opens as what it was: the
// Physics, Pile or Prop block that put it in a physics world is what made it
// a rigid layer, and the Physics block becomes the layer's own world.
void older_pages_open_as_the_kind_their_blocks_made_them() {
    const std::string older =
        R"({"format":"ajfx-studio","version":1,"name":"Old","next_uid":9,"layers":[
           {"uid":1,"name":"Rocks","life":[60,60],"blocks":[
             {"uid":2,"kind":"pile","values":{"count":12}},
             {"uid":3,"kind":"gravity"},
             {"uid":4,"kind":"physics","values":{"friction":0.75}}]},
           {"uid":5,"name":"Smoke","life":[2,3],"blocks":[
             {"uid":6,"kind":"stream"},
             {"uid":7,"kind":"swirl"}]}]})";
    const Loaded loaded = load_document(older);
    CHECK(loaded.error.empty() && loaded.document.has_value());
    const Layer& rocks = loaded.document->layers[0];
    CHECK(rocks.kind == LayerKind::Rigid);
    // The Physics block left the list and became the layer's world, values
    // and all, so the page plays exactly as it did.
    CHECK(rocks.world && near(number_of(*rocks.world, "friction"), 0.75F));
    CHECK(std::none_of(rocks.blocks.begin(), rocks.blocks.end(),
                       [](const Block& b) { return b.kind == world_block_kind; }));
    CHECK(loaded.document->layers[1].kind == LayerKind::Particles);
    CHECK(!loaded.document->layers[1].world.has_value());

    Document document = *loaded.document;
    const Compiled compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty());
    CHECK(near(find_module<ajfx::RigidPhysics>(emitter_of(compiled, rocks))->friction, 0.75F));

    // Saved and read back, the kind and the world are what they were.
    const Loaded again = load_document(save_document(document));
    CHECK(again.error.empty() && again.document.has_value() && *again.document == document);
}

// A rigid layer's forces are the world's. Their strength has to reach the
// engine through the force itself, because the engine reads neither a
// force's per-particle scale nor its curve over life.
void a_rigid_layer_force_carries_its_strength() {
    Document document;
    Layer layer = make_layer(document, LayerKind::Rigid, "Rocks");
    insert_block(layer, 0U, *make_block(document, "pile"));
    const Uid gravity = insert_block(layer, 1U, *make_block(document, "gravity"));
    const Uid wind = insert_block(layer, 2U, *make_block(document, "wind"));
    const Uid drag = insert_block(layer, 3U, *make_block(document, "drag"));
    document.layers.push_back(std::move(layer));
    document.block(gravity)->set("strength", range(0.5F, 0.5F));
    document.block(wind)->set("strength", range(4.0F, 4.0F));
    document.block(drag)->set("amount", range(3.0F, 3.0F));

    const Compiled compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty());
    const ajfx::Emitter& bodies = compiled.effect->emitters[0];
    // Half a g down and 4 m/s^2 sideways, in the accelerations themselves.
    float down = 0.0F;
    float sideways = 0.0F;
    for (const ajfx::UpdateModule& module : bodies.update) {
        if (const auto* g = std::get_if<ajfx::Gravity>(&module)) {
            down += g->acceleration.value.y;
            sideways += g->acceleration.value.x;
            // Nothing is promised per particle that the engine cannot give.
            CHECK(near(g->scale_min.value, 1.0F) && near(g->scale_max.value, 1.0F));
        } else if (const auto* d = std::get_if<ajfx::Drag>(&module)) {
            CHECK(near(d->coefficient.value, 3.0F));
        }
    }
    CHECK(near(down, -9.81F * 0.5F) && near(sideways, 4.0F));

    // The same blocks in a particle layer keep the per-particle spread they
    // have always had: this is the rigid layer's rule, not a new one.
    Document other;
    Layer particles = make_layer(other, LayerKind::Particles, "Sparks");
    insert_block(particles, 0U, *make_block(other, "stream"));
    const Uid pull = insert_block(particles, 1U, *make_block(other, "gravity"));
    other.layers.push_back(std::move(particles));
    other.block(pull)->set("strength", range(0.5F, 2.0F));
    const Compiled loose = compile(other, Purpose::Export);
    CHECK(loose.error.empty());
    const auto* spread = find_module<ajfx::Gravity>(loose.effect->emitters[0]);
    CHECK(spread != nullptr && near(spread->scale_min.value, 0.5F) && near(spread->scale_max.value, 2.0F));
}

// Everything the palette and the pickers offer must be something that can
// actually be made. An id neither side recognised once produced a layer with
// no uid, which looked like a layer but shared every widget id with every
// other such layer, so its menu never opened.
void every_layer_the_palette_offers_can_be_made() {
    Document document;
    const auto made = [&](const std::string& id) {
        if (const std::optional<LayerKind> empty = empty_layer_kind(id)) {
            return std::optional<Layer>{make_layer(document, *empty, "Layer")};
        }
        return make_layer_preset(document, id);
    };
    std::vector<std::string> ids;
    for (const LayerKind kind : {LayerKind::Particles, LayerKind::Rigid}) {
        ids.push_back(empty_layer_id(kind));
    }
    for (const LayerPreset& preset : layer_presets()) {
        ids.emplace_back(preset.id);
    }
    std::vector<Uid> seen;
    for (const std::string& id : ids) {
        const std::optional<Layer> layer = made(id);
        if (!layer) {
            throw std::runtime_error{"the palette offers '" + id + "', which cannot be made"};
        }
        // A uid of its own: without one every widget on it collides with
        // every other layer's, and the layer menu never opens.
        CHECK(layer->uid != 0U);
        CHECK(std::find(seen.begin(), seen.end(), layer->uid) == seen.end());
        seen.push_back(layer->uid);
        if (layer->kind == LayerKind::Rigid) {
            CHECK(layer->world && layer->world->uid != 0U && layer->world->uid != layer->uid);
        } else {
            CHECK(!layer->world.has_value());
        }
        for (const Block& block : layer->blocks) {
            CHECK(block.uid != 0U);
            CHECK(block.kind == raw_block_kind || block_fits(block.kind, layer->kind));
        }
    }
    // An empty layer's id round-trips, and nothing else is read as one.
    for (const LayerKind kind : {LayerKind::Particles, LayerKind::Rigid}) {
        CHECK(empty_layer_kind(empty_layer_id(kind)) == kind);
    }
    CHECK(empty_layer_kind("") == LayerKind::Particles);
    CHECK(!empty_layer_kind("rock_pile").has_value());
    CHECK(!empty_layer_kind("empty:volumes").has_value());
}

} // namespace

int main() {
    struct Case final {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"every_recipe_compiles_within_its_budget", every_recipe_compiles_within_its_budget},
        {"block_order_does_not_change_the_effect", block_order_does_not_change_the_effect},
        {"a_new_block_leaves_the_others_alone", a_new_block_leaves_the_others_alone},
        {"page_scale_scales_the_whole_effect", page_scale_scales_the_whole_effect},
        {"blocks_that_shape_one_curve_are_merged", blocks_that_shape_one_curve_are_merged},
        {"pages_save_and_load_unchanged", pages_save_and_load_unchanged},
        {"pages_from_elsewhere_are_refused_with_a_reason", pages_from_elsewhere_are_refused_with_a_reason},
        {"the_coach_fixes_what_it_points_at", the_coach_fixes_what_it_points_at},
        {"solo_and_broken_layers_leave_the_rest_playing", solo_and_broken_layers_leave_the_rest_playing},
        {"detaching_a_block_keeps_the_picture", detaching_a_block_keeps_the_picture},
        {"published_controls_become_parameters_with_budgets_for_their_range",
         published_controls_become_parameters_with_budgets_for_their_range},
        {"flipbook_cells_fade_out_at_their_edges", flipbook_cells_fade_out_at_their_edges},
        {"ranges_give_each_particle_its_own_value", ranges_give_each_particle_its_own_value},
        {"seeds_deal_new_values_to_their_block_alone", seeds_deal_new_values_to_their_block_alone},
        {"guides_show_the_focused_block_with_handles", guides_show_the_focused_block_with_handles},
        {"the_turbulence_guide_follows_its_field", the_turbulence_guide_follows_its_field},
        {"detached_modules_recombine_into_their_block", detached_modules_recombine_into_their_block},
        {"the_loop_is_as_long_as_the_effect_plays", the_loop_is_as_long_as_the_effect_plays},
        {"piles_stand_until_the_effect_plays", piles_stand_until_the_effect_plays},
        {"props_stand_in_the_world_and_spill_what_they_hold",
         props_stand_in_the_world_and_spill_what_they_hold},
        {"a_break_uses_the_parts_beside_it", a_break_uses_the_parts_beside_it},
        {"the_crates_recipe_cuts_its_crates", the_crates_recipe_cuts_its_crates},
        {"half_a_break_is_refused_with_the_missing_half", half_a_break_is_refused_with_the_missing_half},
        {"a_break_can_cut_its_own_model_into_pieces", a_break_can_cut_its_own_model_into_pieces},
        {"snow_lands_on_the_pieces_of_the_layer_above", snow_lands_on_the_pieces_of_the_layer_above},
        {"landing_on_pieces_needs_a_layer_above", landing_on_pieces_needs_a_layer_above},
        {"a_prop_takes_default_physics_and_names_what_it_cannot_be",
         a_prop_takes_default_physics_and_names_what_it_cannot_be},
        {"own_pictures_and_models_go_into_the_package", own_pictures_and_models_go_into_the_package},
        {"model_colliders_bake_to_distance_fields", model_colliders_bake_to_distance_fields},
        {"studio_comes_back_as_it_was_left", studio_comes_back_as_it_was_left},
        {"a_layer_kind_decides_which_blocks_it_can_hold", a_layer_kind_decides_which_blocks_it_can_hold},
        {"changing_a_layer_kind_keeps_what_fits_and_refuses_the_rest",
         changing_a_layer_kind_keeps_what_fits_and_refuses_the_rest},
        {"older_pages_open_as_the_kind_their_blocks_made_them",
         older_pages_open_as_the_kind_their_blocks_made_them},
        {"a_rigid_layer_force_carries_its_strength", a_rigid_layer_force_carries_its_strength},
        {"every_layer_the_palette_offers_can_be_made", every_layer_the_palette_offers_can_be_made},
    };
    int failures = 0;
    for (const Case& test : cases) {
        try {
            test.run();
            // Flushed, so a crash still shows the last test that passed.
            std::cout << "[pass] " << test.name << std::endl;
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
#if AJFX_STUDIO_TEST_KERNELS
    try {
        if (kernels_compile_for_every_recipe()) {
            std::cout << "[pass] kernels_compile_for_every_recipe\n";
        }
    } catch (const std::exception& error) {
        ++failures;
        std::cout << "[FAIL] kernels_compile_for_every_recipe: " << error.what() << '\n';
    }
#endif
    return failures == 0 ? 0 : 1;
}
