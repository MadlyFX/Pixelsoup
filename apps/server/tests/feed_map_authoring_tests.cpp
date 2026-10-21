#include "director_feed_map_authoring.hpp"

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/output/feed_map_model.hpp>
#include <mediaserver/output/feed_map_schema.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace output = mediaserver::output;
namespace schema = mediaserver::output::schema;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define AUTHOR_CHECK(...)                                                      \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr cmd::UndoContextId feed_context{4U};

void put(doc::Node& node, std::string_view key, doc::PropertyValue value) {
    node.properties.insert_or_assign(std::string{key}, std::move(value));
}

[[nodiscard]] doc::Node viewport(core::u64 id, std::int64_t window) {
    doc::Node node{};
    node.id = doc::NodeId{id};
    node.type = std::string{schema::map_type};
    put(node, schema::node_index, window);
    put(node, schema::source_x, std::int64_t{0});
    put(node, schema::source_y, std::int64_t{0});
    put(node, schema::source_width, std::int64_t{2'880});
    put(node, schema::source_height, std::int64_t{1'536});
    put(node, schema::viewport_x, std::int64_t{0});
    put(node, schema::viewport_y, std::int64_t{0});
    put(node, schema::viewport_width, std::int64_t{2'880});
    put(node, schema::viewport_height, std::int64_t{1'536});
    return node;
}

[[nodiscard]] doc::Node surface(core::u64 id, std::string name) {
    doc::Node node{};
    node.id = doc::NodeId{id};
    node.type = std::string{schema::surface_type};
    put(node, schema::surface_name, std::move(name));
    put(node, schema::physical_width, 4.0);
    put(node, schema::physical_height, 2.0);
    put(node, schema::raster_width, std::int64_t{2'880});
    put(node, schema::raster_height, std::int64_t{1'536});
    return node;
}

// Three viewports in window 0, one in window 1, and two surfaces.
[[nodiscard]] doc::Root wall() {
    doc::Root root;
    for (const doc::Node& node :
         {viewport(10U, 0), viewport(11U, 0), viewport(12U, 0),
          viewport(13U, 1), surface(20U, "Wall A"), surface(21U, "Wall B")}) {
        root = root.set(node);
    }
    return root;
}

[[nodiscard]] output::FeedMap evaluate(const doc::Root& root) {
    output::FeedMapModel model;
    model.reset(root);
    return model.evaluate();
}

[[nodiscard]] std::vector<core::u64> window_ids(const output::FeedMap& map,
                                                core::u32 window) {
    std::vector<core::u64> ids;
    for (const output::OutputMap& viewport : map.maps_for(window)) {
        ids.push_back(viewport.node.value());
    }
    return ids;
}

[[nodiscard]] std::vector<core::u64> surface_ids(const output::FeedMap& map) {
    std::vector<core::u64> ids;
    for (const output::ProjectionSurface& surface : map.surfaces) {
        ids.push_back(surface.node.value());
    }
    return ids;
}

void rename_writes_the_name_and_undoes() {
    const doc::Root root = wall();
    cmd::DirectorScheduler scheduler{root};
    AUTHOR_CHECK(app::schedule_feed_organize(
        scheduler,
        editor::FeedOrganizeRequest{editor::FeedOrganizeAction::Rename,
                                    doc::NodeId{11U}, "Processor B", {}, 1U},
        doc::NodeId{90U}, cmd::FrameNumber{1U}));
    AUTHOR_CHECK(app::schedule_feed_organize(
        scheduler,
        editor::FeedOrganizeRequest{editor::FeedOrganizeAction::Rename,
                                    doc::NodeId{21U}, "Stage Right", {}, 2U},
        doc::NodeId{90U}, cmd::FrameNumber{1U}));
    const output::FeedMap named = evaluate(scheduler.projected_document());
    AUTHOR_CHECK(named.map_node(doc::NodeId{11U})->name == "Processor B");
    AUTHOR_CHECK(named.surface_for(doc::NodeId{21U})->name == "Stage Right");
    AUTHOR_CHECK(scheduler.undo_depth(feed_context) == 2U);

    // A blank name, or one naming nothing, authors no edit.
    AUTHOR_CHECK(app::schedule_feed_organize(
        scheduler,
        editor::FeedOrganizeRequest{editor::FeedOrganizeAction::Rename,
                                    doc::NodeId{11U}, "", {}, 3U},
        doc::NodeId{90U}, cmd::FrameNumber{1U}));
    AUTHOR_CHECK(app::schedule_feed_organize(
        scheduler,
        editor::FeedOrganizeRequest{editor::FeedOrganizeAction::Rename,
                                    doc::NodeId{99U}, "Ghost", {}, 4U},
        doc::NodeId{90U}, cmd::FrameNumber{1U}));
    AUTHOR_CHECK(scheduler.undo_depth(feed_context) == 2U);
}

void reorder_writes_one_window_in_one_undo() {
    const doc::Root root = wall();
    cmd::DirectorScheduler scheduler{root};
    const std::vector<doc::NodeId> order{doc::NodeId{12U}, doc::NodeId{10U},
                                         doc::NodeId{11U}};
    AUTHOR_CHECK(app::schedule_feed_organize(
        scheduler,
        editor::FeedOrganizeRequest{editor::FeedOrganizeAction::Reorder,
                                    doc::NodeId{12U}, {}, order, 5U},
        doc::NodeId{90U}, cmd::FrameNumber{1U}));
    const output::FeedMap moved = evaluate(scheduler.projected_document());
    AUTHOR_CHECK(window_ids(moved, 0U) ==
                 (std::vector<core::u64>{12U, 10U, 11U}));
    AUTHOR_CHECK(window_ids(moved, 1U) == (std::vector<core::u64>{13U}));
    AUTHOR_CHECK(scheduler.undo_depth(feed_context) == 1U);

    auto undone = scheduler.schedule_undo(feed_context, cmd::TransactionId{6U},
                                          cmd::FrameNumber{1U});
    AUTHOR_CHECK(undone.has_value());
    AUTHOR_CHECK(doc::diff(scheduler.projected_document(), root).changes.empty());

    // A list that is not exactly the window's viewports is stale: a viewport
    // from another window, a missing one, or a repeat all author nothing.
    for (const std::vector<doc::NodeId>& stale :
         {std::vector<doc::NodeId>{doc::NodeId{13U}, doc::NodeId{10U},
                                   doc::NodeId{11U}},
          std::vector<doc::NodeId>{doc::NodeId{10U}, doc::NodeId{11U}},
          std::vector<doc::NodeId>{doc::NodeId{10U}, doc::NodeId{10U},
                                   doc::NodeId{11U}}}) {
        AUTHOR_CHECK(app::schedule_feed_organize(
            scheduler,
            editor::FeedOrganizeRequest{editor::FeedOrganizeAction::Reorder,
                                        doc::NodeId{10U}, {}, stale, 7U},
            doc::NodeId{90U}, cmd::FrameNumber{1U}));
    }
    AUTHOR_CHECK(doc::diff(scheduler.projected_document(), root).changes.empty());
}

void duplicate_lands_directly_after_its_original() {
    const doc::Root root = wall();
    cmd::DirectorScheduler scheduler{root};
    AUTHOR_CHECK(app::schedule_feed_organize(
        scheduler,
        editor::FeedOrganizeRequest{editor::FeedOrganizeAction::Duplicate,
                                    doc::NodeId{10U}, {}, {}, 8U},
        doc::NodeId{90U}, cmd::FrameNumber{1U}));
    AUTHOR_CHECK(app::schedule_feed_organize(
        scheduler,
        editor::FeedOrganizeRequest{editor::FeedOrganizeAction::Duplicate,
                                    doc::NodeId{20U}, {}, {}, 9U},
        doc::NodeId{91U}, cmd::FrameNumber{1U}));
    const output::FeedMap copied = evaluate(scheduler.projected_document());
    AUTHOR_CHECK(window_ids(copied, 0U) ==
                 (std::vector<core::u64>{10U, 90U, 11U, 12U}));
    AUTHOR_CHECK(surface_ids(copied) == (std::vector<core::u64>{20U, 91U, 21U}));
    const output::OutputMap* const copy = copied.map_node(doc::NodeId{90U});
    const output::OutputMap* const original = copied.map_node(doc::NodeId{10U});
    AUTHOR_CHECK(copy != nullptr && original != nullptr);
    AUTHOR_CHECK(copy->source == original->source);
    AUTHOR_CHECK(copy->viewport == original->viewport);
    // An unnamed viewport stays unnamed; a named surface says it is a copy.
    AUTHOR_CHECK(copy->name.empty());
    AUTHOR_CHECK(copied.surface_for(doc::NodeId{91U})->name == "Wall A copy");
    AUTHOR_CHECK(copied.surface_for(doc::NodeId{91U})->geometry ==
                 copied.surface_for(doc::NodeId{20U})->geometry);

    // Each duplicate, with the positions it moved, is one undo step.
    AUTHOR_CHECK(scheduler.undo_depth(feed_context) == 2U);
    AUTHOR_CHECK(scheduler
                     .schedule_undo(feed_context, cmd::TransactionId{10U},
                                    cmd::FrameNumber{1U})
                     .has_value());
    AUTHOR_CHECK(scheduler
                     .schedule_undo(feed_context, cmd::TransactionId{11U},
                                    cmd::FrameNumber{1U})
                     .has_value());
    AUTHOR_CHECK(doc::diff(scheduler.projected_document(), root).changes.empty());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 3U> cases{{
    {"rename writes the name and undoes", &rename_writes_the_name_and_undoes},
    {"reorder writes one window in one undo",
     &reorder_writes_one_window_in_one_undo},
    {"duplicate lands directly after its original",
     &duplicate_lands_directly_after_its_original},
}};

} // namespace

int main() {
    int failures = 0;
    for (const Case& test : cases) {
        try {
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[fail] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - static_cast<std::size_t>(failures)) << '/'
              << cases.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
