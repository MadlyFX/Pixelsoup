#include "director_documents.hpp"

#include <mediaserver/compositor/compositor_schema.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/show/demo_edit_scene.hpp>
#include <mediaserver/show/demo_feed_map.hpp>
#include <mediaserver/show/demo_scene.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace mediaserver::app {

// Schedules the built-in scene as one transaction of node creations applied at
// `apply_at`. Every edit is an ordinary command, so it replicates, journals and
// undoes through the paths everything else does - there is no second mechanism
// for 3D. Returns false if the scheduler refuses any edit, which is a
// programming error here rather than a runtime condition.
[[nodiscard]] doc::Node default_compositor_output_node(doc::NodeId id,
                                                       doc::NodeId scene) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{compositor::schema::output_type};
    node.properties.emplace(
        std::string{compositor::schema::scene_id},
        doc::PropertyValue{static_cast<std::int64_t>(scene.value())});
    node.properties.emplace(std::string{compositor::schema::label},
                            doc::PropertyValue{std::string{"Output"}});
    node.properties.emplace(std::string{compositor::schema::editor_x},
                            doc::PropertyValue{420.0});
    node.properties.emplace(std::string{compositor::schema::editor_y},
                            doc::PropertyValue{120.0});
    return node;
}

[[nodiscard]] bool author_scene_document(cmd::DirectorScheduler &scheduler,
                                         cmd::FrameNumber apply_at) {
    const cmd::UndoContextId undo_context{1U};
    const cmd::TransactionId transaction{1U};
    for (const auto &node : show::demo_scene()) {
        const auto node_id = node.id;
        auto scheduled =
            scheduler.schedule_edit(cmd::PutNode{node}, cmd::EraseNode{node_id},
                                    undo_context, transaction, apply_at);
        if (!scheduled) {
            return false;
        }
    }
    const doc::Node output = default_compositor_output_node(
        doc::NodeId{show::demo_scene_node_base + 8U},
        doc::NodeId{show::demo_scene_node_base});
    if (!scheduler.schedule_edit(cmd::PutNode{output},
                                 cmd::EraseNode{output.id}, undo_context,
                                 transaction, apply_at)) {
        return false;
    }
    return true;
}

[[nodiscard]] doc::Node editor_canvas_feed_map_node(core::u32 width,
                                                    core::u32 height) {
    namespace schema = output::schema;

    doc::Node node{};
    node.id = doc::NodeId{show::demo_feed_map_node_base};
    node.type = std::string{schema::map_type};
    node.properties.emplace(std::string{schema::node_index},
                            doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(std::string{schema::source_x},
                            doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(std::string{schema::source_y},
                            doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(
        std::string{schema::source_width},
        doc::PropertyValue{static_cast<std::int64_t>(width)});
    node.properties.emplace(
        std::string{schema::source_height},
        doc::PropertyValue{static_cast<std::int64_t>(height)});
    node.properties.emplace(std::string{schema::viewport_x},
                            doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(std::string{schema::viewport_y},
                            doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(
        std::string{schema::viewport_width},
        doc::PropertyValue{static_cast<std::int64_t>(width)});
    node.properties.emplace(
        std::string{schema::viewport_height},
        doc::PropertyValue{static_cast<std::int64_t>(height)});
    return node;
}

// The initial feed map, scheduled the same way as the scene: ordinary
// node-creation commands in one transaction, so it replicates, journals, and
// undoes through the same paths. Explicit output demos keep their two slices;
// a fresh editor gets one full-canvas node-0 slice for its supervised output.
// File > New Project: an untitled startup without the demo content. One empty
// scene at the editor's default canvas, with the camera and compositor output
// a created scene gets, and the node-0 feed map an untitled editor seeds so a
// supervised output mirrors the canvas. No transport or switcher node: the
// load path gives the project the launch cadence and the scene Program.
[[nodiscard]] doc::Root new_project_document() {
    const doc::NodeId scene_id{show::demo_edit_node_base};
    doc::Node stage{};
    stage.id = scene_id;
    stage.type = std::string{scene::schema::stage_type};
    stage.properties.emplace(std::string{scene::schema::scene_name},
                             doc::PropertyValue{std::string{"Scene 1"}});
    stage.properties.emplace(
        std::string{scene::schema::canvas_width},
        doc::PropertyValue{
            static_cast<std::int64_t>(show::demo_edit_canvas_width)});
    stage.properties.emplace(
        std::string{scene::schema::canvas_height},
        doc::PropertyValue{
            static_cast<std::int64_t>(show::demo_edit_canvas_height)});
    doc::Node camera{};
    camera.id = doc::NodeId{show::demo_edit_node_base + 1U};
    camera.type = std::string{scene::schema::camera_type};
    camera.properties.emplace(
        std::string{scene::schema::scene_id},
        doc::PropertyValue{static_cast<std::int64_t>(scene_id.value())});
    // A scene's default camera starts locked so a stray drag cannot move it.
    camera.properties.emplace(std::string{scene::schema::locked},
                              doc::PropertyValue{true});
    camera.properties.emplace(std::string{scene::schema::position_z},
                              doc::PropertyValue{6.0});
    doc::Root root;
    for (doc::Node &node :
         std::array{std::move(stage), std::move(camera),
                    default_compositor_output_node(
                        doc::NodeId{show::demo_edit_node_base + 2U}, scene_id),
                    editor_canvas_feed_map_node(
                        show::demo_edit_canvas_width,
                        show::demo_edit_canvas_height)}) {
        root = root.set(std::move(node));
    }
    return root;
}

[[nodiscard]] bool author_feed_map_document(cmd::DirectorScheduler &scheduler,
                                            cmd::FrameNumber apply_at,
                                            InitialFeedMapPlan plan) {
    std::vector<doc::Node> nodes;
    switch (plan.kind) {
    case InitialFeedMapKind::None:
        return true;
    case InitialFeedMapKind::OutputDemo:
        nodes = show::demo_feed_map();
        break;
    case InitialFeedMapKind::EditorCanvas:
        nodes.push_back(
            editor_canvas_feed_map_node(plan.source_width, plan.source_height));
        break;
    }

    const cmd::UndoContextId undo_context{1U};
    const cmd::TransactionId transaction{1U};
    for (const auto &node : nodes) {
        const auto node_id = node.id;
        auto scheduled =
            scheduler.schedule_edit(cmd::PutNode{node}, cmd::EraseNode{node_id},
                                    undo_context, transaction, apply_at);
        if (!scheduled) {
            return false;
        }
    }
    return true;
}

// Writes the demo clip as a short image sequence of TGAs to a temp directory
// and returns a '#'-run template the renderer resolves per frame. A stand-in
// for real media the operator would drop in: enough to have something on the
// wall to drag while the show plays. TGA because it needs no encoder library.
[[nodiscard]] std::string write_demo_clip() {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() / "mediaserver-editclip";
    std::error_code ec;
    fs::create_directories(directory, ec);

    constexpr core::u32 width = 320U;
    constexpr core::u32 height = 180U;
    std::array<core::u8, 18U> header{};
    header[2U] = 2U; // uncompressed true-colour
    header[12U] = static_cast<core::u8>(width & 0xffU);
    header[13U] = static_cast<core::u8>((width >> 8U) & 0xffU);
    header[14U] = static_cast<core::u8>(height & 0xffU);
    header[15U] = static_cast<core::u8>((height >> 8U) & 0xffU);
    header[16U] = 32U;
    header[17U] = 0x28U; // top-left origin, 8 alpha bits

    std::vector<core::u8> pixels(static_cast<std::size_t>(width) * height * 4U,
                                 0U);
    for (core::u32 frame = 0U; frame < show::demo_clip_frames; ++frame) {
        const auto bar = (frame * width) / show::demo_clip_frames;
        for (core::u32 y = 0U; y < height; ++y) {
            for (core::u32 x = 0U; x < width; ++x) {
                const auto index =
                    (static_cast<std::size_t>(y) * width + x) * 4U;
                auto r = static_cast<core::u8>((x * 255U) / (width - 1U));
                auto g = static_cast<core::u8>((y * 255U) / (height - 1U));
                auto b = static_cast<core::u8>((frame * 255U) /
                                               (show::demo_clip_frames - 1U));
                if (x >= bar && x < bar + 14U) {
                    r = 255U;
                    g = 255U;
                    b = 255U;
                }
                pixels[index + 0U] = b; // TGA is BGRA
                pixels[index + 1U] = g;
                pixels[index + 2U] = r;
                pixels[index + 3U] = 255U;
            }
        }
        char name[32]{};
        std::snprintf(name, sizeof(name), "editclip_%04u.tga", frame);
        std::ofstream out{directory / name, std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char *>(header.data()),
                  static_cast<std::streamsize>(header.size()));
        out.write(reinterpret_cast<const char *>(pixels.data()),
                  static_cast<std::streamsize>(pixels.size()));
    }
    return (directory / "editclip_####.tga").string();
}

// Authors the edit scene the same way the other demos are authored: one
// transaction of node creations, replicating and undoing through the ordinary
// paths.
[[nodiscard]] bool author_edit_document(cmd::DirectorScheduler &scheduler,
                                        cmd::FrameNumber apply_at,
                                        const std::string &clip_source) {
    for (const auto &node : show::demo_edit_scene(clip_source)) {
        const auto node_id = node.id;
        auto scheduled = scheduler.schedule_edit(
            cmd::PutNode{node}, cmd::EraseNode{node_id}, cmd::UndoContextId{1U},
            cmd::TransactionId{1U}, apply_at);
        if (!scheduled) {
            return false;
        }
    }
    const doc::Node output = default_compositor_output_node(
        doc::NodeId{show::demo_edit_node_base + 7U},
        doc::NodeId{show::demo_edit_node_base});
    if (!scheduler.schedule_edit(
            cmd::PutNode{output}, cmd::EraseNode{output.id},
            cmd::UndoContextId{1U}, cmd::TransactionId{1U}, apply_at)) {
        return false;
    }
    return true;
}

} // namespace mediaserver::app
