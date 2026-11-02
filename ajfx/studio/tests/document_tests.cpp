#include "test_support.hpp"

// Files Studio reads, the workspace it comes back to, and knowing when the
// document has changed.
namespace {

using namespace studio_test;

// Only scenes open. A layer page, or anything else, is refused with a reason.
void files_other_than_scenes_are_refused_with_a_reason() {
    CHECK(load_document("{").error.find("Not a readable scene") != std::string::npos);
    CHECK(load_document(R"({"format": "something-else", "version": 1})").error.find("not an ajfx Studio scene") !=
          std::string::npos);
    CHECK(load_document(R"({"format": "ajfx-studio", "version": 5, "layers": []})").error.find(
              "not an ajfx Studio scene") != std::string::npos);
    const Loaded scene = load_document(save_document(new_scene()));
    CHECK(scene.error.empty() && scene.document && scene.document->scene);
}

void studio_comes_back_as_it_was_left() {
    SessionState session;
    session.page = "C:/effects/fire.ajfxstudio";
    session.unsaved = save_document(new_scene());
    session.width = 1800U;
    session.height = 1000U;
    session.guides = false;
    session.handles = false;
    session.paths = true;
    session.figure = false;
    session.night = true;
    session.zoom = 1.25F;
    session.looping = false;
    session.scene_tree_split = 0.25F;
    session.scene_inspector_split = 0.6F;
    session.scene_selection = {33U, 65U};
    session.scene_folded = {97U};
    session.camera = std::array<float, 6>{0.2F, 0.05F, 7.5F, 0.0F, 1.2F, -0.5F};
    session.seconds = 2.5;
    session.playing = false;
    const SessionState loaded = load_session(save_session(session));
    CHECK(loaded == session);
    // Anything unreadable is the default, and nothing is held against it.
    CHECK(load_session("not json") == SessionState{});
    const SessionState odd =
        load_session(R"({"format":"ajfx-studio-session","scene_tree_split":7,"zoom":9,"camera":[1,2]})");
    CHECK(odd.scene_tree_split == 0.35F && odd.zoom == 1.5F && !odd.camera);
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

} // namespace

void add_document_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"files_other_than_scenes_are_refused_with_a_reason", files_other_than_scenes_are_refused_with_a_reason});
    cases.push_back({"studio_comes_back_as_it_was_left", studio_comes_back_as_it_was_left});
    cases.push_back({"the_page_revision_moves_only_when_the_page_does", the_page_revision_moves_only_when_the_page_does});
}
