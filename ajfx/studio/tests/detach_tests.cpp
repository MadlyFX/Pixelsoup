#include "test_support.hpp"

// Turning a block into the modules it compiles to, and back.
namespace {

using namespace studio_test;

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

void detaching_a_block_keeps_the_picture() {
    for (const char* kind : {"launch", "turbulence", "color", "fade", "size", "tumble"}) {
        const std::string_view k{kind};
        const bool rocks = false;
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

} // namespace

void add_detach_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"detaching_a_block_keeps_the_picture", detaching_a_block_keeps_the_picture});
    cases.push_back({"detached_modules_recombine_into_their_block", detached_modules_recombine_into_their_block});
}
