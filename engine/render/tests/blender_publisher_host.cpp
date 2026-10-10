// Integration fixture for the actual Blender publisher. No test-only wire behavior:
// this drives the same service, ingest planner and Director scheduling as playback.
#include <mediaserver/render/director_link_service.hpp>
#include <mediaserver/render/blender_discovery.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>

using namespace mediaserver;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        const fs::path directory{argv[1]};
        fs::create_directories(directory);
        render::DirectorLinkService service;
        render::BlenderDiscovery discovery{directory / "discovery"};
        std::string selected_instance, waiting_instance, previous_code;
        // The scene this Blender syncs, as the Director names it in its scene
        // list; the first listing also asks Blender to adopt the scene shown.
        std::string bound_source, bound_name;
        bool adopt{}, resync_handled{};
        cmd::DirectorScheduler director;
        render::DirectorLinkContext context;
        context.document = context.document.set({doc::NodeId{1}, std::string{scene::schema::stage_type}, {}});
        context.target = doc::NodeId{1}; context.project = 1;
        if (!director.schedule_document_replace(context.document, cmd::UndoContextId{2},
                cmd::TransactionId{1}, cmd::FrameNumber{1})) return 3;
        service.start((directory / "resources").string(), context);
        bool announced = false;
        std::size_t commits{};
        core::u64 local_edits{1000};
        // The runner always touches `stop`; this is only a runaway-fixture ceiling.
        const auto deadline = std::chrono::steady_clock::now() + 600s;
        while (std::chrono::steady_clock::now() < deadline && !fs::exists(directory / "stop")) {
            scene::Scene reference;
            reference.id=context.target; reference.show_frame=12; reference.show_rate={24,1};
            reference.camera.eye={0,2,5}; reference.camera.target={0,1,0};
            service.sample(render::linked_stage_sample(reference,++local_edits));
            const auto status = service.status();
            if (const auto found = discovery.status(); found && !found->instances.empty()) {
                for (const auto& item : found->instances) {
                    if (item.id != selected_instance) continue;
                    if (item.resync_requested && !resync_handled && status) {
                        previous_code = status->pairing_code;
                        waiting_instance = selected_instance;
                        service.start((directory / "resources").string(), context);
                        resync_handled = true;
                    } else if (!item.resync_requested) resync_handled = false;
                }
                std::ofstream out{directory / "instances.tmp"};
                for (const auto& item : found->instances) out << item.id << '\n' << item.label << '\n' << item.scene << '\n';
                out.close();
                fs::remove(directory / "instances.txt");
                fs::rename(directory / "instances.tmp", directory / "instances.txt");
            }
            if (fs::exists(directory / "connect-instance")) {
                std::ifstream in{directory / "connect-instance"};
                std::getline(in, waiting_instance);
                std::getline(in, bound_source); std::getline(in, bound_name);
                for (auto* line : {&waiting_instance, &bound_source, &bound_name})
                    if (!line->empty() && line->back() == '\r') line->pop_back();
                in.close(); fs::remove(directory / "connect-instance");
                adopt = true;
            }
            if (fs::exists(directory / "restart-link") && status) {
                fs::remove(directory / "restart-link");
                previous_code = status->pairing_code;
                waiting_instance = selected_instance;
                service.start((directory / "resources").string(), context);
            }
            if (!waiting_instance.empty() && status && status->listening && status->pairing_code != previous_code) {
                discovery.link_scenes(waiting_instance, {{bound_source, bound_name, status->pairing_code, status->port}});
                if (adopt) discovery.activate(waiting_instance, bound_source, bound_name, true);
                adopt = false;
                selected_instance = std::move(waiting_instance); waiting_instance.clear();
            }
            if (fs::exists(directory / "local-shot.txt")) {
                std::ifstream input{directory / "local-shot.txt"};
                core::u64 id{}; std::string action; float x{}, y{}, z{};
                input >> id >> action >> x >> y >> z; input.close();
                fs::remove(directory / "local-shot.txt");
                const auto node = doc::NodeId{id};
                // An operator's light edit: an override on the occurrence.
                const auto light_edit = [&](std::string_view key, double value) {
                    auto edited = *context.document.get(node);
                    edited.properties[std::string{key}] = value;
                    return scene::plan_linked_change(context.document, context.document.set(std::move(edited)));
                };
                auto plan = action == "world" ? scene::plan_linked_world(context.document,node,scene::translation({x,y,z}))
                    : action == "intensity" ? light_edit(scene::schema::intensity, x)
                    : scene::plan_linked_edit(context.document,node,action == "hold" ? scene::LinkedEdit::Hold
                        : action == "hide" ? scene::LinkedEdit::Hide
                        : action == "show" ? scene::LinkedEdit::Show
                        : action == "visibility" ? scene::LinkedEdit::FollowVisibility
                        : action == "pause" ? scene::LinkedEdit::Pause
                        : action == "resume" ? scene::LinkedEdit::Resume
                        : action == "follow" ? scene::LinkedEdit::Follow : scene::LinkedEdit::ResetPlacement);
                if (!plan) throw std::runtime_error{plan.error().message};
                if (!director.schedule_document_replace(plan.value(),cmd::UndoContextId{2},
                        cmd::TransactionId{++local_edits},cmd::FrameNumber{1})) throw std::runtime_error{"local shot scheduling failed"};
                context.document = director.projected_document(); service.update(context);
                { std::ofstream ack{directory / "local-shot-done.txt"}; ack << action; }
            }
            if (!announced && status && status->listening) {
                std::ofstream out{directory / "pairing.tmp"};
                out << "{\"port\":" << status->port << ",\"code\":" << std::quoted(status->pairing_code) << '}';
                out.close(); fs::rename(directory / "pairing.tmp", directory / "pairing.json");
                announced = true;
            }
            if (auto candidate = service.take()) {
                auto plan = render::DirectorLinkService::plan(*candidate, context);
                std::string error;
                if (!plan) error = plan.error().message;
                else {
                    const auto result = director.schedule_document_replace(plan.value(), cmd::UndoContextId{render::director_link_undo_context},
                        cmd::TransactionId{(core::u64{1} << 63U) | candidate->ticket}, cmd::FrameNumber{1});
                    if (!result) error = "test scheduling failed";
                    else {
                        context.document = director.projected_document();
                        service.update(context);
                        if (!persist::save_project_file((directory/"accepted.mstage").string(),context.document))
                            throw std::runtime_error{"Cannot save accepted animation fixture"};
                        std::ofstream out{directory / "commit.tmp"};
                        scene::SceneModel evaluated; evaluated.reset(context.document);
                        out << "{\"revision\":" << std::quoted(candidate->manifest.revision)
                            << ",\"instances\":" << evaluated.evaluate(context.target,{}).models.size()
                            << ",\"ingested\":" << candidate->manifest.ingested
                            << ",\"reused\":" << candidate->manifest.reused;
                        const auto link = service.status();
                        out << ",\"transfers\":" << (link ? link->metrics.transfers : 0) << ",\"objects\":[";
                        bool first = true;
                        const auto state = scene::read_linked_scene(context.document);
                        if (!state) throw std::runtime_error{state.error().message};
                        for (const auto& [id, occurrence] : state.value().occurrences) {
                            static_cast<void>(id);
                            if (!first) out << ',';
                            first = false;
                            const auto* object = context.document.get(occurrence.object);
                            out << "{\"name\":" << std::quoted(occurrence.name)
                                << ",\"id\":" << std::quoted(std::get<std::string>(object->properties.at("link_uuid")))
                                << ",\"removed\":" << (occurrence.removed ? "true" : "false")
                                << ",\"matrix\":[";
                            for (std::size_t i = 0; i < 16; ++i) {
                                if (i) out << ',';
                                out << occurrence.world.m[i];
                            }
                            out << "]}";
                        }
                        out << "]}"; out.close();
                        fs::rename(directory / "commit.tmp", directory / ("commit-" + std::to_string(++commits) + ".json"));
                    }
                }
                service.resolve(candidate->ticket, error);
            }
            std::this_thread::sleep_for(2ms);
        }
        service.stop();
        std::cout << "Publisher host accepted " << commits << " revisions\n";
        return announced && commits ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
