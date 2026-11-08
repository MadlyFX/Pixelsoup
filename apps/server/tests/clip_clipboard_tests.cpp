#include "clip_clipboard.hpp"

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define CLIP_CHECK(...)                                                        \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] doc::PropertyValue whole(std::int64_t value) {
    return doc::PropertyValue{value};
}

[[nodiscard]] doc::Node media(
    core::u64 id,
    std::int64_t start,
    std::int64_t duration,
    bool locked = false) {
    doc::Node node{};
    node.id = doc::NodeId{id};
    node.type = std::string{scene::schema::media_type};
    node.properties.emplace(std::string{scene::schema::scene_id}, whole(1));
    node.properties.emplace(std::string{scene::schema::clip_start}, whole(start));
    node.properties.emplace(
        std::string{scene::schema::clip_duration}, whole(duration));
    node.properties.emplace(std::string{scene::schema::source_in}, whole(7));
    node.properties.emplace(std::string{scene::schema::fade_in}, whole(3));
    node.properties.emplace(std::string{scene::schema::fade_out}, whole(4));
    node.properties.emplace(std::string{scene::schema::locked}, locked);
    node.properties.emplace(std::string{scene::schema::asset_id}, whole(501));
    node.properties.emplace(
        std::string{scene::schema::source}, std::string{"original.mov"});
    node.properties.emplace(
        std::string{scene::schema::source_rate_numerator}, whole(24));
    node.properties.emplace(
        std::string{scene::schema::source_rate_denominator}, whole(1));
    node.properties.emplace(std::string{scene::schema::position_x}, 12.5);
    node.properties.emplace(std::string{scene::schema::opacity}, 0.75);
    return node;
}

[[nodiscard]] doc::Node effect(core::u64 id, core::u64 target) {
    doc::Node node{};
    node.id = doc::NodeId{id};
    node.type = std::string{scene::schema::effect_type};
    node.properties.emplace(std::string{scene::schema::scene_id}, whole(1));
    node.properties.emplace(
        std::string{scene::schema::effect_target},
        whole(static_cast<std::int64_t>(target)));
    node.properties.emplace(
        std::string{scene::schema::effect_kind},
        std::string{scene::schema::brightness_kind});
    node.properties.emplace(std::string{scene::schema::brightness}, 1.25);
    return node;
}

[[nodiscard]] doc::Node channel(
    core::u64 id,
    core::u64 target,
    std::int64_t first,
    std::int64_t second) {
    doc::Node node{};
    node.id = doc::NodeId{id};
    node.type = std::string{scene::schema::channel_type};
    node.properties.emplace(std::string{scene::schema::scene_id}, whole(1));
    node.properties.emplace(
        std::string{scene::schema::channel_target},
        whole(static_cast<std::int64_t>(target)));
    node.properties.emplace(
        std::string{scene::schema::channel_component},
        std::string{scene::schema::position_x});
    node.properties.emplace("key0_frame", whole(first));
    node.properties.emplace("key0_value", 1.0);
    node.properties.emplace("key0_interp", std::string{"linear"});
    node.properties.emplace("key1_frame", whole(second));
    node.properties.emplace("key1_value", 2.0);
    node.properties.emplace("key1_interp", std::string{"hold"});
    return node;
}

[[nodiscard]] std::int64_t integer(
    const doc::Node& node,
    std::string_view property) {
    const auto found = node.properties.find(property);
    CLIP_CHECK(found != node.properties.end());
    const auto* const held = std::get_if<std::int64_t>(&found->second);
    CLIP_CHECK(held != nullptr);
    return *held;
}

[[nodiscard]] std::string text(
    const doc::Node& node,
    std::string_view property) {
    const auto found = node.properties.find(property);
    CLIP_CHECK(found != node.properties.end());
    const auto* const held = std::get_if<std::string>(&found->second);
    CLIP_CHECK(held != nullptr);
    return *held;
}

[[nodiscard]] doc::Root source_document() {
    doc::Root root;
    root = root.set(doc::Node{doc::NodeId{1U},
                             std::string{scene::schema::stage_type}, {}});
    root = root.set(doc::Node{doc::NodeId{2U},
                             std::string{scene::schema::stage_type}, {}});
    root = root.set(doc::Node{doc::NodeId{99U},
                             std::string{scene::schema::stage_type}, {}});
    root = root.set(media(10U, 20, 40));
    root = root.set(media(20U, 60, 10));
    root = root.set(media(15U, 10, 30, true));
    root = root.set(media(17U, 5, 0));
    root = root.set(effect(30U, 10U));
    root = root.set(effect(40U, 20U));
    root = root.set(effect(41U, 15U));
    root = root.set(channel(50U, 10U, 5, 30));
    root = root.set(channel(60U, 30U, 25, 35));
    root = root.set(channel(61U, 40U, 45, 70));
    root = root.set(channel(70U, 41U, 2, 3));
    return root;
}

void copy_paste_carries_the_clip_subgraph_and_rewrites_time_and_ids() {
    doc::Root source = source_document();
    app::ClipClipboard clipboard;
    constexpr std::array selection{
        doc::NodeId{20U}, doc::NodeId{15U}, doc::NodeId{10U},
        doc::NodeId{17U}, doc::NodeId{10U}};
    CLIP_CHECK(clipboard.copy(source, selection));
    CLIP_CHECK(clipboard.size() == 7U);
    CLIP_CHECK(clipboard.anchor() == 20);

    // The clipboard is a snapshot, not a delayed query against the source.
    doc::Node changed = *source.get(doc::NodeId{10U});
    changed.properties.insert_or_assign(
        std::string{scene::schema::source}, std::string{"changed.mov"});
    source = source.set(std::move(changed));
    source = source.set(doc::Node{doc::NodeId{1001U}, "occupied", {}});

    auto pasted = clipboard.paste(source, scene::SceneId{99U}, 100U, 1000U);
    CLIP_CHECK(pasted);
    const app::ClipPastePlan& plan = pasted.value();
    CLIP_CHECK(plan.created.size() == 7U);
    CLIP_CHECK(plan.next_node == 1008U);
    constexpr std::array<core::u64, 7U> expected_ids{
        1000U, 1002U, 1003U, 1004U, 1005U, 1006U, 1007U};
    for (std::size_t i = 0U; i < expected_ids.size(); ++i) {
        CLIP_CHECK(plan.created[i].id == doc::NodeId{expected_ids[i]});
        CLIP_CHECK(integer(plan.created[i], scene::schema::scene_id) == 99);
        CLIP_CHECK(plan.document.contains(plan.created[i].id));
    }

    const doc::Node& first = plan.created[0];
    const doc::Node& second = plan.created[1];
    CLIP_CHECK(integer(first, scene::schema::clip_start) == 100);
    CLIP_CHECK(integer(second, scene::schema::clip_start) == 140);
    CLIP_CHECK(integer(first, scene::schema::source_in) == 7);
    CLIP_CHECK(integer(first, scene::schema::fade_in) == 3);
    CLIP_CHECK(integer(first, scene::schema::fade_out) == 4);
    CLIP_CHECK(integer(first, scene::schema::asset_id) == 501);
    CLIP_CHECK(text(first, scene::schema::source) == "original.mov");

    CLIP_CHECK(integer(plan.created[2], scene::schema::effect_target) == 1000);
    CLIP_CHECK(integer(plan.created[3], scene::schema::effect_target) == 1002);
    CLIP_CHECK(integer(plan.created[4], scene::schema::channel_target) == 1000);
    CLIP_CHECK(integer(plan.created[5], scene::schema::channel_target) == 1003);
    CLIP_CHECK(integer(plan.created[6], scene::schema::channel_target) == 1004);
    CLIP_CHECK(integer(plan.created[4], "key0_frame") == 85);
    CLIP_CHECK(integer(plan.created[4], "key1_frame") == 110);
    CLIP_CHECK(integer(plan.created[5], "key0_frame") == 105);
}

void paste_schedules_as_one_atomic_scene_history_transaction() {
    const doc::Root source = source_document();
    app::ClipClipboard clipboard;
    constexpr std::array selection{doc::NodeId{10U}, doc::NodeId{20U}};
    CLIP_CHECK(clipboard.copy(source, selection));
    auto pasted = clipboard.paste(source, scene::SceneId{2U}, 200U, 1000U);
    CLIP_CHECK(pasted);

    cmd::DirectorScheduler scheduler{source};
    auto scheduled = scheduler.schedule_document_replace(
        pasted.value().document, cmd::UndoContextId{2U},
        cmd::TransactionId{77U}, cmd::FrameNumber{1U});
    CLIP_CHECK(scheduled);
    CLIP_CHECK(scheduled.value().size() == pasted.value().created.size());
    for (const cmd::Command& command : scheduled.value()) {
        CLIP_CHECK(command.undo_context == cmd::UndoContextId{2U});
        CLIP_CHECK(command.transaction == cmd::TransactionId{77U});
        CLIP_CHECK(std::holds_alternative<cmd::PutNode>(command.forward));
    }
    CLIP_CHECK(scheduler.undo_depth(cmd::UndoContextId{2U}) == 1U);

    auto undone = scheduler.schedule_undo(
        cmd::UndoContextId{2U}, cmd::TransactionId{78U},
        cmd::FrameNumber{1U});
    CLIP_CHECK(undone);
    for (const doc::Node& created : pasted.value().created) {
        CLIP_CHECK(!scheduler.projected_document().contains(created.id));
    }
}

void invalid_copy_replaces_the_clipboard_and_paste_reports_bounds() {
    const doc::Root source = source_document();
    app::ClipClipboard clipboard;
    constexpr std::array valid{doc::NodeId{10U}};
    CLIP_CHECK(clipboard.copy(source, valid));
    constexpr std::array invalid{doc::NodeId{15U}, doc::NodeId{17U}};
    CLIP_CHECK(!clipboard.copy(source, invalid));
    CLIP_CHECK(clipboard.empty());
    auto empty = clipboard.paste(source, scene::SceneId{2U}, 0U, 1000U);
    CLIP_CHECK(!empty);
    CLIP_CHECK(empty.error() == app::ClipPasteError::Empty);

    CLIP_CHECK(clipboard.copy(source, valid));
    auto late = clipboard.paste(
        source, scene::SceneId{2U},
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max()) + 1U,
        1000U);
    CLIP_CHECK(!late);
    CLIP_CHECK(late.error() == app::ClipPasteError::FrameOutOfRange);

    auto missing_scene = clipboard.paste(
        source, scene::SceneId{55U}, 0U, 1000U);
    CLIP_CHECK(!missing_scene);
    CLIP_CHECK(missing_scene.error() == app::ClipPasteError::InvalidTargetScene);

    auto ids = clipboard.paste(
        source, scene::SceneId{2U}, 0U,
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max()) + 1U);
    CLIP_CHECK(!ids);
    CLIP_CHECK(ids.error() == app::ClipPasteError::IdExhausted);

    clipboard.clear();
    CLIP_CHECK(clipboard.empty());
    CLIP_CHECK(!clipboard.anchor().has_value());

    // A document with no Stage has no scene to paste into.
    doc::Root stage_less;
    stage_less = stage_less.set(media(5U, 0, 10));
    constexpr std::array stage_less_selection{doc::NodeId{5U}};
    CLIP_CHECK(clipboard.copy(stage_less, stage_less_selection));
    CLIP_CHECK(!clipboard.paste(stage_less, scene::SceneId{}, 10U, 100U));
}

void negative_clipboard_anchors_preserve_spacing_and_reject_overflow() {
    auto root = source_document().set(media(10U, -60, 40)).set(media(20U, -5, 10));
    app::ClipClipboard clipboard;
    constexpr std::array selection{doc::NodeId{10U}, doc::NodeId{20U}};
    CLIP_CHECK(clipboard.copy(root, selection));
    CLIP_CHECK(clipboard.anchor() == -60);
    CLIP_CHECK(clipboard.span_duration() == 65);
    const auto pasted = clipboard.paste(root, scene::SceneId{99U}, 100U, 1000U);
    CLIP_CHECK(pasted);
    std::vector<core::i64> starts;
    for (const auto& node : pasted.value().created) {
        if (node.type == scene::schema::media_type) {
            starts.push_back(integer(node, scene::schema::clip_start));
        }
    }
    CLIP_CHECK(starts == std::vector<core::i64>({100, 155}));

    const core::i64 low = std::numeric_limits<core::i64>::min();
    const core::i64 high = std::numeric_limits<core::i64>::max();
    root = root.set(media(10U, low, 1)).set(media(20U, high - 1, 1));
    CLIP_CHECK(!clipboard.copy(root, selection));
    CLIP_CHECK(clipboard.empty());
    constexpr std::array one{doc::NodeId{10U}};
    root = root.set(media(10U, -20, 10));
    CLIP_CHECK(clipboard.copy(root, one));
    const auto overflowing_end = clipboard.paste(root, scene::SceneId{99U},
        static_cast<core::u64>(high - 5), 1000U);
    CLIP_CHECK(!overflowing_end);
    CLIP_CHECK(overflowing_end.error() == app::ClipPasteError::FrameOutOfRange);
}

} // namespace

int main() {
    const std::array cases{
        copy_paste_carries_the_clip_subgraph_and_rewrites_time_and_ids,
        paste_schedules_as_one_atomic_scene_history_transaction,
        invalid_copy_replaces_the_clipboard_and_paste_reports_bounds,
        negative_clipboard_anchors_preserve_spacing_and_reject_overflow,
    };
    std::size_t passed = 0U;
    for (const auto test : cases) {
        try {
            test();
            ++passed;
        } catch (const std::exception& error) {
            std::cerr << "clip clipboard test failed: " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << passed << " clip clipboard tests passed\n";
    return 0;
}
