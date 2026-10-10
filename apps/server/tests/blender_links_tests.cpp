#include "blender_links.hpp"

#include <mediaserver/scene/linked_publication.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/scene_project.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace app = mediaserver::app;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define LINKS_CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

std::string read(const fs::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

unsigned long this_process() {
#if defined(_WIN32)
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

doc::Node stage(core::u64 id, std::string name) {
    doc::Node node{doc::NodeId{id}, std::string{scene::schema::stage_type}, {}};
    node.properties[std::string{scene::schema::scene_name}] = std::move(name);
    return node;
}

void instance_records_carry_file_token_process_and_scenes() {
    // Python writes text mode: CRLF on Windows. The record names the scenes
    // the file holds, the one shown, and a scene the artist asked to add.
    const auto a = app::new_link_source(), b = app::new_link_source(), c = app::new_link_source();
    render::BlenderInstance item;
    LINKS_CHECK(render::parse_blender_instance("MSBLENDER 2\r\nShow.blend - Blender 5.2\r\nDesert\r\nReady\r\nready\r\n"
        "C:\\Shows\\Show.blend\r\n0123456789abcdef0123456789abcdef\r\n4242\r\n" + b + "\r\n" + a + "," + b
        + ",bogus\r\n" + c + "\tNight\r\n", item));
    LINKS_CHECK(item.label == "Show.blend - Blender 5.2" && item.scene == "Desert" && item.status == "Ready");
#if defined(_WIN32)
    LINKS_CHECK(item.file == "C:\\Shows\\Show.blend");
#endif
    LINKS_CHECK(item.token == "0123456789abcdef0123456789abcdef" && item.pid == 4242U && !item.resync_requested);
    LINKS_CHECK(item.active_source == b && item.scene_sources == std::vector<std::string>{a, b});
    LINKS_CHECK(item.create_source == c && item.create_name == "Night");

    // An unsaved file, no tagged scenes, a resync request.
    render::BlenderInstance unsaved;
    LINKS_CHECK(render::parse_blender_instance("MSBLENDER 2\nUnsaved\nScene\nReady\nresync\n\n\n\n\n\n\n", unsaved));
    LINKS_CHECK(unsaved.resync_requested && unsaved.file.empty() && unsaved.token.empty() && unsaved.pid == 0U
        && unsaved.active_source.empty() && unsaved.scene_sources.empty() && unsaved.create_source.empty());

    // A relative file, malformed token or source is never trusted.
    render::BlenderInstance odd;
    LINKS_CHECK(render::parse_blender_instance("MSBLENDER 2\nx\nScene\nReady\nready\nshot.blend\nNOTHEX\n12x\nbogus\n\nbogus\tName\n", odd));
    LINKS_CHECK(odd.file.empty() && odd.token.empty() && odd.pid == 0U && odd.active_source.empty() && odd.create_source.empty());
    // A record missing fields is not a Blender.
    LINKS_CHECK(!render::parse_blender_instance("MSBLENDER 2\nx\nScene\nReady\n", odd));
    LINKS_CHECK(!render::parse_blender_instance("MSBLENDER 1\nx\nScene\nReady\nready\n\n\n1\n\n\n\n", odd));
}

void sources_are_valid_and_distinct() {
    const auto first = app::new_link_source();
    const auto second = app::new_link_source();
    LINKS_CHECK(render::valid_link_source(first) && render::valid_link_source(second) && first != second);
    LINKS_CHECK(first[14] == '4');
    LINKS_CHECK(!render::valid_link_source("00000000-0000-0000-0000-000000000000"));
    LINKS_CHECK(!render::valid_link_source("not-a-uuid"));
}

void default_files_follow_the_project() {
#if defined(_WIN32)
    const std::string project = "D:\\Shows\\Car Spot.mstage";
    // One .blend holds the whole project.
    const auto file = app::project_blender_file(project, "ignored");
    LINKS_CHECK(app::path_utf8(file) == "D:\\Shows\\Car Spot Blender\\Car Spot.blend");
    LINKS_CHECK(app::path_utf8(app::link_resource_directory(project)) == "D:\\Shows\\Car Spot Blender\\Link Resources");
    LINKS_CHECK(app::same_blender_file("C:\\A\\b.blend", "c:/a/B.blend"));
    LINKS_CHECK(app::default_publication_storage(project) == "D:\\Shows");
#endif
    const auto unsaved = app::project_blender_file({}, "Unsaved Project 2026-10-09 / 10-00");
    LINKS_CHECK(unsaved.filename() == "Unsaved Project 2026-10-09 _ 10-00.blend");
    LINKS_CHECK(unsaved.parent_path().filename() == "Blender");
    LINKS_CHECK(!app::same_blender_file("", ""));
    // A scene opening in Blender joins the file the project's other scenes
    // use; the scene itself and scenes with no file do not count.
    const std::array<app::BlenderAssociation, 4> associations{{
        {doc::NodeId{1}, "Program", "C:/Shows/Show.blend", {}},
        {doc::NodeId{2}, "Desert", "c:/shows/show.blend", {}},
        {doc::NodeId{3}, "Legacy", "C:/Shows/Legacy.blend", {}},
        {doc::NodeId{4}, "New", {}, {}}}};
    LINKS_CHECK(app::same_blender_file(app::shared_blender_file(associations, doc::NodeId{4}, "C:/Other.blend"),
        "C:/Shows/Show.blend"));
    LINKS_CHECK(app::shared_blender_file(std::span{associations}.subspan(3), doc::NodeId{4}, "C:/Other.blend")
        == "C:/Other.blend");
}

void associations_track_stage_edits() {
    doc::Root root;
    auto first = stage(1, "Program");
    auto second = stage(2, "Desert");
    second.properties[std::string{scene::schema::blender_file}] = std::string{"C:/Shows/Desert.blend"};
    root = root.set(first).set(second).set({doc::NodeId{3}, std::string{scene::link_schema::source_type}, {}});
    std::vector<app::BlenderAssociation> associations;
    app::refresh_blender_associations(associations, {}, root);
    LINKS_CHECK(associations.size() == 2U);
    auto renamed = second;
    renamed.properties[std::string{scene::schema::scene_name}] = std::string{"Desert Night"};
    auto next = root.set(renamed);
    app::refresh_blender_associations(associations, root, next);
    const auto found = std::find_if(associations.begin(), associations.end(),
        [](const auto &item) { return item.scene == doc::NodeId{2}; });
    LINKS_CHECK(found != associations.end() && found->name == "Desert Night" && found->file == "C:/Shows/Desert.blend");
    auto erased = *next.erase(doc::NodeId{2});
    app::refresh_blender_associations(associations, next, erased);
    LINKS_CHECK(associations.size() == 1U && associations.front().scene == doc::NodeId{1});
}

void scene_copies_leave_the_blender_file_behind() {
    doc::Root root;
    auto source = stage(1, "Desert");
    source.properties[std::string{scene::schema::blender_file}] = std::string{"C:/Shows/Desert.blend"};
    source.properties[std::string{scene::schema::blender_link_source}] = app::new_link_source();
    root = root.set(source);
    auto clone = scene::plan_clone_scene(root, doc::NodeId{1}, doc::NodeId{10});
    LINKS_CHECK(static_cast<bool>(clone));
    for (const auto &node : clone.value().puts) {
        if (node.id != clone.value().destination) continue;
        LINKS_CHECK(!node.properties.contains(scene::schema::blender_file));
        LINKS_CHECK(!node.properties.contains(scene::schema::blender_link_source));
    }
}

// A Blender that quit or crashed leaves its record until it expires; it must
// not stand in for an open Blender (Open in Blender would only try to raise it).
void exited_blender_records_are_ignored() {
    const auto root = fs::temp_directory_path() / ("mediaserver-blender-exited-" + app::new_link_source());
    fs::create_directories(root);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); } } cleanup{root};
    const std::string live = "0123456789abcdef0123456789abcdef", gone = "fedcba9876543210fedcba9876543210";
    for (const auto &[id, pid] : {std::pair{live, this_process()}, std::pair{gone, 999999990UL}}) {
        std::ofstream out{root / (id + ".instance"), std::ios::binary};
        out << "MSBLENDER 2\nScene.blend\nScene\nReady\nready\n\n\n" << pid << "\n\n\n\n";
    }
    render::BlenderDiscovery discovery{root};
    std::shared_ptr<const render::BlenderDiscoveryState> state;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while ((!state || state->instances.empty()) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
        state = discovery.status();
    }
    LINKS_CHECK(state && state->instances.size() == 1U && state->instances.front().id == live);
    LINKS_CHECK(!fs::exists(root / (gone + ".instance")));
}

// One Blender holding the project's file: every Media Server scene in it is
// linked at once through one scene list, the Blender is switched to a scene
// on Open in Blender, a scene the artist added there is created once, and a
// scene disconnected leaves the list.
void one_blender_holds_every_scene() {
    const auto root = fs::temp_directory_path() / ("mediaserver-blender-scenes-" + app::new_link_source());
    const auto discovery = root / "discovery";
    fs::create_directories(discovery);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); } } cleanup{root};
    const auto blend = app::path_utf8(root / "Show.blend");
    const std::string id = "00112233445566778899aabbccddeeff";
    const auto desert = app::new_link_source(), night = app::new_link_source(), added = app::new_link_source();
    const auto advertise = [&] {
        {
            std::ofstream out{discovery / (id + ".tmp"), std::ios::binary | std::ios::trunc};
            out << "MSBLENDER 2\nShow.blend - Blender 5.2\nDesert\nReady\nready\n" << blend << "\n\n" << this_process()
                << '\n' << desert << '\n' << desert << ',' << night << '\n' << added << "\tSunrise\n";
        }
        std::error_code busy;
        fs::rename(discovery / (id + ".tmp"), discovery / (id + ".instance"), busy);
    };
    doc::Root document;
    for (const auto& [node, name, source] : {std::tuple{2U, "Desert", desert}, std::tuple{3U, "Night", night}}) {
        auto linked = stage(node, name);
        linked.properties[std::string{scene::schema::blender_file}] = blend;
        linked.properties[std::string{scene::schema::blender_link_source}] = source;
        document = document.set(linked);
    }
    document = document.set(stage(1, "Program"));
    std::vector<app::BlenderAssociation> associations;
    app::refresh_blender_associations(associations, {}, document);
    const auto context_for = [&](doc::NodeId target) {
        return render::DirectorLinkContext{document, target, doc::NodeId{1}, 1U, false};
    };
    app::BlenderLinks links{app::path_utf8(root / "cache"), discovery};
    const auto list = discovery / (id + ".links");
    const auto pump = [&](auto done) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            advertise();
            links.poll(associations, context_for);
            std::this_thread::sleep_for(20ms);
        }
        return done();
    };
    LINKS_CHECK(pump([&] { const auto text = read(list); return text.find(desert) != std::string::npos
        && text.find(night) != std::string::npos; }));
    const auto text = read(list);
    LINKS_CHECK(text.starts_with("MSLINKS 1\n"));
    for (const auto scene : {doc::NodeId{2}, doc::NodeId{3}}) {
        const auto status = links.status_for_scene(scene);
        LINKS_CHECK(status && status->listening && text.find(status->pairing_code) != std::string::npos);
        LINKS_CHECK(links.instance_for_scene(scene) == id);
    }
    LINKS_CHECK(text.find("\tDesert\n") != std::string::npos && text.find("\tNight\n") != std::string::npos);
    LINKS_CHECK(!links.status_for_scene(doc::NodeId{1}));

    const auto created = links.take_created();
    LINKS_CHECK(created.size() == 1U && created.front().source == added && created.front().name == "Sunrise"
        && created.front().file == blend);
    // The record keeps asking until the scene exists; it is created once.
    for (int i = 0; i < 15; ++i) { advertise(); links.poll(associations, context_for); std::this_thread::sleep_for(20ms); }
    LINKS_CHECK(links.take_created().empty());

    // Open in Blender on a linked scene switches that Blender to it.
    const auto night_association = *std::find_if(associations.begin(), associations.end(),
        [](const app::BlenderAssociation& item) { return item.name == "Night"; });
    fs::remove(list);
    links.open(night_association, context_for(doc::NodeId{3}));
    LINKS_CHECK(pump([&] { return fs::exists(discovery / (id + ".activate")); }));
    LINKS_CHECK(read(discovery / (id + ".activate")) == "MSACTIVATE 1\n" + night + "\nNight\nshow\n");

    // Disconnecting one scene leaves it out of the list; the other stays.
    links.disconnect_scene(doc::NodeId{3});
    LINKS_CHECK(pump([&] { const auto now = read(list); return now.find(desert) != std::string::npos
        && now.find(night) == std::string::npos; }));
    LINKS_CHECK(links.instance_for_scene(doc::NodeId{2}) == id && links.instance_for_scene(doc::NodeId{3}).empty());
    links.stop_all();
}

} // namespace

int main() {
    try {
        instance_records_carry_file_token_process_and_scenes();
        sources_are_valid_and_distinct();
        default_files_follow_the_project();
        associations_track_stage_edits();
        scene_copies_leave_the_blender_file_behind();
        exited_blender_records_are_ignored();
        one_blender_holds_every_scene();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "blender links tests passed\n";
    return 0;
}
