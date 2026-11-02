#include "test_support.hpp"

// The compiler: recipes, merging, scale, ranges, seeds, packages and loops.
namespace {

using namespace studio_test;

ImportedMesh unit_cube() {
    const ajfx::MeshData cube = ajfx::cube_mesh();
    ImportedMesh mesh;
    for (const ajfx::MeshVertex& v : cube.vertices) {
        mesh.positions.push_back(v.position);
    }
    mesh.indices = cube.indices;
    return mesh;
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
        // One emitter a layer, but the solid layers are one world together.
        const auto solid_layers = static_cast<std::size_t>(std::count_if(
            document.layers.begin(), document.layers.end(), [](const Layer& l) { return l.kind == LayerKind::Rigid; }));
        CHECK(compiled.effect->emitters.size() ==
              document.layers.size() - solid_layers + (solid_layers > 0U ? 1U : 0U));
        CHECK(compiled.solids.layers.size() == solid_layers);
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
            const bool piled =
                std::any_of(l.blocks.begin(), l.blocks.end(), [](const Block& b) { return b.kind == BlockKind::Object; });
            longest = std::max(longest, l.enabled ? (piled ? 5.0F : l.life_max) : 0.0F);
        }
        const float loop = loop_seconds(page, built);
        CHECK(loop >= 1.0F && loop <= 120.0F && (built.effect.has_value() ? loop >= longest : true));
    }
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
    // No file picked yet: a soft dot, slabs, nothing to collide with.
    look.set("texture_file", choice(""));
    debris_look.set("mesh_file", choice(""));
    sparks.blocks.back().set("file", choice(""));
    const Compiled waiting = compile(document, Purpose::Export);
    CHECK(waiting.error.empty() && waiting.requests.empty());
    CHECK(emitter_of(waiting, layer_named(document, "Debris")).renderer.mesh == "slab");
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

} // namespace

void add_compile_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"every_recipe_compiles_within_its_budget", every_recipe_compiles_within_its_budget});
    cases.push_back({"block_order_does_not_change_the_effect", block_order_does_not_change_the_effect});
    cases.push_back({"a_new_block_leaves_the_others_alone", a_new_block_leaves_the_others_alone});
    cases.push_back({"page_scale_scales_the_whole_effect", page_scale_scales_the_whole_effect});
    cases.push_back({"blocks_that_shape_one_curve_are_merged", blocks_that_shape_one_curve_are_merged});
    cases.push_back({"solo_and_broken_layers_leave_the_rest_playing", solo_and_broken_layers_leave_the_rest_playing});
    cases.push_back({"published_controls_become_parameters_with_budgets_for_their_range", published_controls_become_parameters_with_budgets_for_their_range});
    cases.push_back({"ranges_give_each_particle_its_own_value", ranges_give_each_particle_its_own_value});
    cases.push_back({"seeds_deal_new_values_to_their_block_alone", seeds_deal_new_values_to_their_block_alone});
    cases.push_back({"flipbook_cells_fade_out_at_their_edges", flipbook_cells_fade_out_at_their_edges});
    cases.push_back({"the_loop_is_as_long_as_the_effect_plays", the_loop_is_as_long_as_the_effect_plays});
    cases.push_back({"own_pictures_and_models_go_into_the_package", own_pictures_and_models_go_into_the_package});
    cases.push_back({"model_colliders_bake_to_distance_fields", model_colliders_bake_to_distance_fields});
}

#if AJFX_STUDIO_TEST_KERNELS
// Whether the GPU compiler was there to run; the kernels of every recipe compile.
bool kernels_compile_for_every_recipe() {
    using namespace studio_test;
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
