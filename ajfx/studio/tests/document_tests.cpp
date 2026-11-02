#include "test_support.hpp"

// Pages: saving, loading, older pages, and knowing when a page has changed.
namespace {

using namespace studio_test;

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

// Pile and Prop were folded into Object and are no longer converted as a page
// loads: a page that still holds one says which block it cannot read, and
// opens nothing, rather than guessing at what it meant.
void a_page_holding_a_removed_block_is_refused_by_name() {
    for (const char* removed : {"pile", "prop"}) {
        const std::string older =
            std::string{R"({"format":"ajfx-studio","version":2,"name":"Old","next_uid":9,"layers":[
               {"uid":1,"name":"Yard","kind":"rigid","life":[60,60],"blocks":[
                 {"uid":2,"kind":")"} + removed + R"("}]}]})";
        const Loaded loaded = load_document(older);
        CHECK(!loaded.document.has_value());
        CHECK(loaded.error.find(std::string{"'"} + removed + "'") != std::string::npos);
        CHECK(find_block_def(removed) == nullptr);
    }
}

// A page saved when the material that drew a model was called "debris"
// opens drawing models, rather than falling back to the first material on
// the list and turning a crate into a glowing dot.
void older_pages_open_their_debris_as_models() {
    const std::string older =
        R"({"format":"ajfx-studio","version":3,"name":"Old","next_uid":9,"layers":[
           {"uid":1,"name":"Chips","kind":"particles","life":[2,3],"blocks":[
             {"uid":2,"kind":"burst"},
             {"uid":3,"kind":"appearance","values":{"material":"debris","mesh":"rock"}}]}]})";
    const Loaded loaded = load_document(older);
    CHECK(loaded.error.empty() && loaded.document.has_value());
    const Block& look = loaded.document->layers[0].blocks[1];
    CHECK(look.kind == BlockKind::Appearance && choice_of(look, "material") == "model");
    CHECK(choice_of(look, "mesh") == "rock");

    Document document = *loaded.document;
    const Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(compiled.effect->emitters[0].renderer.geometry == ajfx::Geometry::Mesh);
    CHECK(compiled.effect->emitters[0].renderer.mesh == "rock");
    std::cout << "  a page of debris opened drawing its models\n";
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
    session.night = true;
    session.zoom = 1.25F;
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

// The application learns the page has changed by looking at it once a frame;
// everything else asks the number that look leaves. The number moves with a
// change and only with one, and a mark says whether the page has moved from a
// copy kept of it, which an edit taken back has not.
void the_page_revision_moves_only_when_the_page_does() {
    Document document = recipe("campfire");
    Block& rate = block_of(layer_named(document, "Flames"), "stream");
    PageRevision changes;
    const std::uint64_t first = changes.observe(document);
    CHECK(changes.observe(document) == first && changes.value() == first);

    const float was = number_of(rate, "rate");
    rate.set("rate", number(was + 25.0F));
    CHECK(changes.value() == first); // not seen until looked at
    const std::uint64_t edited = changes.observe(document);
    CHECK(edited > first && changes.observe(document) == edited);

    // Replaced as a whole, it has changed without a comparison to say so.
    CHECK(changes.touch(document) > edited);

    PageMark saved;
    PageMark compiled;
    saved.set(document, changes.value());
    CHECK(!saved.differs(document, changes.value()));
    // A mark that has never been set differs from a real page.
    CHECK(compiled.differs(document, changes.value()));

    rate.set("rate", number(was));
    CHECK(saved.differs(document, changes.observe(document)));
    // Taken back, the page is the page it was saved as.
    rate.set("rate", number(was + 25.0F));
    CHECK(!saved.differs(document, changes.observe(document)));
    // The answer is kept: asked again at the same number it is the same,
    // whatever the page is then, since the page is not looked at again.
    rate.set("rate", number(was + 90.0F));
    CHECK(!saved.differs(document, changes.value()));
    CHECK(saved.differs(document, changes.observe(document)));
    // A new copy answers for itself at once.
    saved.set(document, changes.value());
    CHECK(!saved.differs(document, changes.value()) && saved.copy() == document);
}

// A page saved before layers had a kind still opens as what it was: the
// Physics block that put it in a physics world is what made it a rigid layer,
// and the Physics block becomes the layer's own world.
void older_pages_open_as_the_kind_their_blocks_made_them() {
    const std::string older =
        R"({"format":"ajfx-studio","version":1,"name":"Old","next_uid":9,"layers":[
           {"uid":1,"name":"Rocks","life":[60,60],"blocks":[
             {"uid":2,"kind":"object","values":{"count":12}},
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
                       [](const Block& b) { return b.kind == BlockKind::Physics; }));
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

} // namespace

void add_document_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"pages_save_and_load_unchanged", pages_save_and_load_unchanged});
    cases.push_back({"pages_from_elsewhere_are_refused_with_a_reason", pages_from_elsewhere_are_refused_with_a_reason});
    cases.push_back({"a_page_holding_a_removed_block_is_refused_by_name", a_page_holding_a_removed_block_is_refused_by_name});
    cases.push_back({"older_pages_open_their_debris_as_models", older_pages_open_their_debris_as_models});
    cases.push_back({"studio_comes_back_as_it_was_left", studio_comes_back_as_it_was_left});
    cases.push_back({"the_page_revision_moves_only_when_the_page_does", the_page_revision_moves_only_when_the_page_does});
    cases.push_back({"older_pages_open_as_the_kind_their_blocks_made_them", older_pages_open_as_the_kind_their_blocks_made_them});
}
