#include "model_source_authoring.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
namespace app = mediaserver::app;
namespace cmd = mediaserver::cmd;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;

#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error{#__VA_ARGS__}; } while (false)

doc::Node model() {
    doc::Node node{doc::NodeId{73U}, std::string{scene::schema::model_type}, {}};
    node.properties.emplace(std::string{scene::schema::source}, std::string{"T:/stage/model original.msmodel"});
    node.properties.emplace(std::string{scene::schema::position_x}, 3.75);
    return node;
}

void replacement_is_textual_committed_and_undoable() {
    const auto original = model();
    cmd::DirectorScheduler scheduler{doc::Root{}.set(original)};
    const std::string package{"T:/stage/out/material-edits/model.1234567890.msmodel"};
    CHECK(app::schedule_model_source_edit(scheduler, original.id, package,
        cmd::TransactionId{101U}, cmd::FrameNumber{1U}));
    CHECK(*scheduler.sealed_document().get(original.id) == original);
    CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
    const auto replacement = *scheduler.sealed_document().get(original.id);
    CHECK(std::get<std::string>(replacement.properties.at(std::string{scene::schema::source})) == package);
    CHECK(std::get<double>(replacement.properties.at(std::string{scene::schema::position_x})) == 3.75);
    CHECK(scheduler.undo_depth(cmd::UndoContextId{2U}) == 1U);
    CHECK(scheduler.schedule_undo(cmd::UndoContextId{2U}, cmd::TransactionId{102U}, cmd::FrameNumber{2U}));
    CHECK(scheduler.seal_frame(cmd::FrameNumber{2U}));
    CHECK(*scheduler.sealed_document().get(original.id) == original);
    CHECK(scheduler.schedule_redo(cmd::UndoContextId{2U}, cmd::TransactionId{103U}, cmd::FrameNumber{3U}));
    CHECK(scheduler.seal_frame(cmd::FrameNumber{3U}));
    CHECK(*scheduler.sealed_document().get(original.id) == replacement);
}

void stale_or_noneditable_targets_keep_their_source() {
    for (int condition = 0; condition < 4; ++condition) {
        auto original = model();
        if (condition == 0) original.properties.emplace(std::string{scene::schema::locked}, true);
        if (condition == 1) original.properties.emplace(std::string{scene::link_schema::instance}, std::int64_t{12});
        if (condition == 2) original.type = std::string{scene::schema::media_type};
        const auto root = condition == 3 ? doc::Root{} : doc::Root{}.set(original);
        cmd::DirectorScheduler scheduler{root};
        CHECK(app::schedule_model_source_edit(scheduler, original.id, "out/prepared.msmodel",
            cmd::TransactionId{201U}, cmd::FrameNumber{1U}));
        CHECK(scheduler.undo_depth(cmd::UndoContextId{2U}) == 0U);
        CHECK(scheduler.seal_frame(cmd::FrameNumber{1U}));
        if (condition == 3) CHECK(scheduler.sealed_document().get(original.id) == nullptr);
        else CHECK(*scheduler.sealed_document().get(original.id) == original);
    }
}
} // namespace

int main() {
    try {
        replacement_is_textual_committed_and_undoable();
        stale_or_noneditable_targets_keep_their_source();
        std::cout << "[pass] model texture package source commit, undo, redo and authority guards\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
}
