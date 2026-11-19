#include "blender_links.hpp"

#include <mediaserver/net/authoring_link_protocol.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cwctype>
#include <random>
#include <sstream>

namespace mediaserver::app {
namespace {
namespace fs = std::filesystem;
namespace ls = scene::link_schema;
using Clock = std::chrono::steady_clock;

std::string random_hex(std::size_t bytes) {
    std::random_device device;
    std::string text;
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto value = static_cast<unsigned>(device()) & 0xffU;
        text.push_back(digits[value >> 4U]);
        text.push_back(digits[value & 0x0fU]);
    }
    return text;
}

std::string text(const doc::Node& node, std::string_view key) {
    const auto found = node.properties.find(key);
    if (found == node.properties.end()) return {};
    const auto* value = std::get_if<std::string>(&found->second);
    return value ? *value : std::string{};
}


fs::path documents_directory() {
#if defined(_WIN32)
    char* base{};
    std::size_t length{};
    static_cast<void>(_dupenv_s(&base, &length, "USERPROFILE"));
    const auto root = base ? utf8_path(base) : fs::temp_directory_path();
    std::free(base);
    return root / "Documents";
#else
    const auto* base = std::getenv("HOME");
    return (base ? fs::path{base} : fs::temp_directory_path()) / "Documents";
#endif
}

// A scene name becomes a file name: no separators, reserved characters or
// trailing dots, and short enough to stay under legacy path limits.
std::string file_stem(const std::string& name) {
    std::string stem;
    for (const char character : name) {
        const auto byte = static_cast<unsigned char>(character);
        stem.push_back(byte < 32U || std::string_view{"<>:\"/\\|?*"}.find(character) != std::string_view::npos
            ? '_' : character);
    }
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) stem.pop_back();
    while (!stem.empty() && stem.front() == ' ') stem.erase(stem.begin());
    if (stem.size() > 80) stem.resize(80);
    return stem.empty() ? std::string{"Scene"} : stem;
}
} // namespace

std::string path_utf8(const fs::path& path) {
    const auto encoded = path.u8string();
    return {encoded.begin(), encoded.end()};
}

fs::path utf8_path(const std::string& value) {
    return fs::path{std::u8string(value.begin(), value.end())};
}

fs::path blender_project_folder(const std::string& project_path) {
    if (project_path.empty()) return documents_directory() / "Pixelsoup" / "Blender";
    const auto project = fs::absolute(utf8_path(project_path));
    return project.parent_path() / (project.stem().u8string() + u8" Blender");
}

fs::path link_resource_directory(const std::string& project_path) {
    return blender_project_folder(project_path) / "Link Resources";
}

fs::path project_blender_file(const std::string& project_path, const std::string& unsaved_name) {
    const auto stem = project_path.empty() ? file_stem(unsaved_name)
        : file_stem(path_utf8(utf8_path(project_path).stem()));
    return blender_project_folder(project_path) / utf8_path(stem + ".blend");
}

std::string shared_blender_file(std::span<const BlenderAssociation> associations,
    doc::NodeId scene, const std::string& fallback) {
    std::string best;
    std::size_t best_count{};
    for (const auto& item : associations) {
        if (item.scene == scene || item.file.empty()) continue;
        const auto count = static_cast<std::size_t>(std::count_if(associations.begin(), associations.end(),
            [&](const BlenderAssociation& other) { return other.scene != scene && same_blender_file(other.file, item.file); }));
        if (count > best_count) { best = item.file; best_count = count; }
    }
    return best.empty() ? fallback : best;
}

std::string default_publication_storage(const std::string& project_path) {
    if (!project_path.empty()) return path_utf8(fs::absolute(utf8_path(project_path)).parent_path());
    return path_utf8(documents_directory() / "Pixelsoup");
}

void refresh_blender_associations(std::vector<BlenderAssociation>& associations,
    const doc::Root& before, const doc::Root& after) {
    for (const auto& change : doc::diff(before, after).changes) {
        const bool stage = change.after && change.after->type == scene::schema::stage_type;
        const auto found = std::find_if(associations.begin(), associations.end(),
            [&](const BlenderAssociation& item) { return item.scene == change.id; });
        if (!stage) {
            if (found != associations.end()) associations.erase(found);
            continue;
        }
        BlenderAssociation updated{change.id, text(*change.after, scene::schema::scene_name),
            text(*change.after, scene::schema::blender_file), text(*change.after, scene::schema::blender_link_source)};
        if (found != associations.end()) *found = std::move(updated);
        else associations.push_back(std::move(updated));
    }
}

bool same_blender_file(const std::string& left, const std::string& right) {
    if (left.empty() || right.empty()) return false;
    auto a = utf8_path(left).lexically_normal().make_preferred().wstring();
    auto b = utf8_path(right).lexically_normal().make_preferred().wstring();
#if defined(_WIN32)
    const auto lower = [](std::wstring& value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    };
    lower(a);
    lower(b);
#endif
    return a == b;
}

std::string new_link_source() {
    auto bytes = random_hex(16);
    bytes[12] = '4';
    bytes[16] = "89ab"[static_cast<unsigned>(bytes[16]) % 4U];
    return bytes.substr(0, 8) + '-' + bytes.substr(8, 4) + '-' + bytes.substr(12, 4) + '-'
        + bytes.substr(16, 4) + '-' + bytes.substr(20, 12);
}

std::string link_diagnostics(const render::DirectorLinkStatus& state) {
    if (!state.connected && !state.metrics.accepted) return {};
    const auto& m = state.metrics;
    std::ostringstream evidence;
    evidence << "Publisher: " << (m.client.empty() ? "unidentified" : m.client)
        << "\nNegotiated: " << (m.capabilities.empty() ? "none" : m.capabilities)
        << "\nThis build serves:";
    for (const auto name : net::link_capabilities()) evidence << ' ' << name;
    evidence << "\nAccepted " << m.accepted << " revisions, refused " << m.refused
        << ", resync " << m.resynced << ", transfers " << m.transfers
        << "\nEvent to Preview: p50 " << m.median_ms << " ms, p95 " << m.tail_ms
        << " ms, last " << m.last_ms << " ms"
        << "\nLast revision: transfer " << m.last_transfer_ms << " ms, ingest "
        << m.last_ingest_ms << " ms, Director " << m.last_admit_ms << " ms (worst "
        << m.worst_admit_ms << " ms)"
        << "\nPeak pending " << m.peak_pending_bytes << " bytes, retained "
        << m.retained_resources << " resources / " << m.retained_bytes << " bytes, "
        << m.retained_receipts << " receipts";
    return evidence.str();
}

struct BlenderLinks::Slot final {
    doc::NodeId scene{};
    std::string source, file, name;
    std::unique_ptr<render::DirectorLinkService> service;
    std::shared_ptr<const render::DirectorLinkStatus> status;
    // The Blender holding this scene.
    std::string instance;
    bool resync_handled{}, operator_disconnected{};
    Clock::time_point bound_at{}, missing_since{};
};

struct BlenderLinks::Launch final {
    std::string token;
    BlenderAssociation scene;
    Clock::time_point deadline{};
};

BlenderLinks::BlenderLinks(std::string cache_directory, fs::path discovery_directory)
    : cache_{std::move(cache_directory)}, discovery_{std::move(discovery_directory)} {}

BlenderLinks::~BlenderLinks() = default;

BlenderLinks::Slot* BlenderLinks::slot_for(doc::NodeId scene, bool create) {
    for (auto& slot : slots_) if (slot->scene == scene) return slot.get();
    if (!create) return nullptr;
    if (slots_.size() >= max_links) {
        // Reuse the place of a link with no Blender attached.
        const auto idle = std::find_if(slots_.begin(), slots_.end(), [](const auto& slot) {
            return slot->instance.empty() && !(slot->status && slot->status->connected);
        });
        if (idle == slots_.end()) {
            message_ = "Up to " + std::to_string(max_links)
                + " scenes can be linked to Blender at once; disconnect one first.";
            return nullptr;
        }
        if ((*idle)->service) (*idle)->service->stop();
        slots_.erase(idle);
    }
    slots_.push_back(std::make_unique<Slot>());
    slots_.back()->scene = scene;
    return slots_.back().get();
}

void BlenderLinks::start_service(Slot& slot, const render::DirectorLinkContext& context) {
    if (!slot.service) slot.service = std::make_unique<render::DirectorLinkService>();
    slot.service->start(cache_, context);
}

const render::BlenderInstance* BlenderLinks::instance(const std::string& id) const {
    if (!state_ || id.empty()) return nullptr;
    const auto found = std::find_if(state_->instances.begin(), state_->instances.end(),
        [&](const auto& item) { return item.id == id; });
    return found == state_->instances.end() ? nullptr : &*found;
}

bool BlenderLinks::launching(doc::NodeId scene) const {
    return std::any_of(launches_.begin(), launches_.end(), [&](const Launch& launch) { return launch.scene.scene == scene; });
}

void BlenderLinks::bind(const std::string& id, const BlenderAssociation& scene,
    const render::DirectorLinkContext& context, bool adopt) {
    auto* slot = slot_for(scene.scene, true);
    if (!slot) return;
    slot->source = scene.source;
    slot->file = scene.file;
    slot->name = scene.name;
    slot->operator_disconnected = false;
    slot->resync_handled = false;
    // The scene list carries the pairing once the service is listening. A
    // running pairing is kept: its resources and revision carry over.
    const auto status = slot->service ? slot->service->status() : nullptr;
    if (!(status && status->listening)) start_service(*slot, context);
    slot->instance = id;
    slot->bound_at = Clock::now();
    slot->missing_since = {};
    discovery_.activate(id, scene.source, scene.name, adopt);
}

void BlenderLinks::open(const BlenderAssociation& scene, const render::DirectorLinkContext& context) {
    state_ = discovery_.status();
    if (auto* slot = slot_for(scene.scene, false)) {
        if (const auto* bound = instance(slot->instance)) {
            discovery_.focus(bound->pid);
            discovery_.activate(bound->id, scene.source, scene.name);
            message_ = "Showing " + scene.name + " in Blender.";
            return;
        }
    }
    if (state_) {
        // A Blender whose file holds this scene, else one with the file open.
        const render::BlenderInstance* found{};
        for (const auto& item : state_->instances)
            if (std::find(item.scene_sources.begin(), item.scene_sources.end(), scene.source) != item.scene_sources.end())
                found = &item;
        if (!found)
            for (const auto& item : state_->instances)
                if (same_blender_file(item.file, scene.file)) { found = &item; break; }
        if (found) {
            bind(found->id, scene, context);
            discovery_.focus(found->pid);
            message_ = "Showing " + scene.name + " in the open " + path_utf8(utf8_path(found->file).filename()) + ".";
            return;
        }
    }
    if (launching(scene.scene)) {
        message_ = "Blender is starting for " + scene.name + "...";
        return;
    }
    if (launches_.size() >= 4) {
        message_ = "Several Blender windows are still starting; try again in a moment.";
        return;
    }
    auto token = random_hex(16);
    discovery_.launch(token, utf8_path(scene.file), scene.source, scene.name);
    launches_.push_back({std::move(token), scene, Clock::now() + std::chrono::seconds{90}});
    message_ = "Opening " + path_utf8(utf8_path(scene.file).filename()) + " in Blender...";
}

bool BlenderLinks::resync(const std::string& id, const ContextFor& context_for) {
    const auto* item = instance(id);
    Slot* chosen{};
    for (auto& slot : slots_) {
        if (slot->instance != id) continue;
        // The scene Blender shows resyncs.
        if (!chosen || (item && slot->source == item->active_source)) chosen = slot.get();
    }
    if (!chosen) return false;
    start_service(*chosen, context_for(chosen->scene));
    chosen->operator_disconnected = false;
    chosen->bound_at = Clock::now();
    message_ = "Resyncing " + (chosen->name.empty() ? std::string{"the scene"} : chosen->name) + " from Blender...";
    return true;
}

void BlenderLinks::disconnect_scene(doc::NodeId scene) {
    std::erase_if(launches_, [&](const Launch& launch) { return launch.scene.scene == scene; });
    auto* slot = slot_for(scene, false);
    if (!slot) return;
    // Its Blender is sent its scene list without this one.
    slot->instance.clear();
    slot->operator_disconnected = true;
    if (slot->service) slot->service->stop();
    message_ = "Blender disconnected from " + (slot->name.empty() ? std::string{"the scene"} : slot->name)
        + ". The last synced content is retained.";
}

void BlenderLinks::disconnect_instance(const std::string& id) {
    for (auto& slot : slots_)
        if (slot->instance == id) disconnect_scene(slot->scene);
}

void BlenderLinks::stop_all() {
    for (const auto& [id, sent] : published_) if (instance(id)) discovery_.link_scenes(id, {});
    for (auto& slot : slots_) if (slot->service) slot->service->stop();
    slots_.clear();
    launches_.clear();
    published_.clear();
}

void BlenderLinks::publish_links(Clock::time_point now) {
    std::map<std::string, std::vector<render::BlenderSceneLink>> lists;
    for (const auto& slot : slots_) {
        if (slot->instance.empty() || slot->operator_disconnected) continue;
        auto& list = lists[slot->instance];
        if (slot->status && slot->status->listening)
            list.push_back({slot->source, slot->name, slot->status->pairing_code, slot->status->port});
    }
    for (auto& [id, list] : lists) {
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.source < b.source; });
        auto& sent = published_[id];
        // Sent again if the scene Blender shows has not connected after a
        // while: the add-on may have missed the file.
        const auto* item = instance(id);
        const bool stalled = item && now - sent.at > std::chrono::seconds{10}
            && std::any_of(slots_.begin(), slots_.end(), [&](const auto& slot) {
                return slot->instance == id && slot->source == item->active_source
                    && slot->status && slot->status->listening && !slot->status->connected;
            });
        if (sent.scenes == list && !stalled) continue;
        discovery_.link_scenes(id, list);
        sent = {list, now};
    }
    for (auto it = published_.begin(); it != published_.end();) {
        if (lists.contains(it->first)) { ++it; continue; }
        if (instance(it->first)) discovery_.link_scenes(it->first, {});
        it = published_.erase(it);
    }
}

void BlenderLinks::poll(std::span<const BlenderAssociation> associations, const ContextFor& context_for) {
    state_ = discovery_.status();
    const auto now = Clock::now();
    const auto association = [&](doc::NodeId scene) -> const BlenderAssociation* {
        const auto found = std::find_if(associations.begin(), associations.end(),
            [&](const BlenderAssociation& item) { return item.scene == scene; });
        return found == associations.end() ? nullptr : &*found;
    };
    for (auto it = launches_.begin(); it != launches_.end();) {
        const render::BlenderLaunchResult* failed{};
        const render::BlenderInstance* started{};
        if (state_) {
            for (const auto& result : state_->launches)
                if (result.token == it->token && !result.error.empty()) failed = &result;
            for (const auto& item : state_->instances)
                if (item.token == it->token) started = &item;
        }
        if (failed) {
            message_ = failed->error;
            it = launches_.erase(it);
            continue;
        }
        if (started) {
            const auto* saved = association(it->scene.scene);
            const auto scene = saved ? *saved : it->scene;
            bind(started->id, scene, context_for(scene.scene));
            message_ = "Blender opened " + path_utf8(utf8_path(scene.file).filename())
                + "; changes sync to " + scene.name + ".";
            it = launches_.erase(it);
        } else if (now > it->deadline) {
            message_ = "Blender did not report in. Check that it opened and that the Pixelsoup Live Link add-on is enabled.";
            it = launches_.erase(it);
        } else ++it;
    }
    for (auto it = slots_.begin(); it != slots_.end();) {
        auto& slot = **it;
        const auto context = context_for(slot.scene);
        if (!context.document.get(slot.scene)) {
            // The scene was deleted; its Blender has nothing left to edit.
            if (slot.service) slot.service->stop();
            it = slots_.erase(it);
            continue;
        }
        if (const auto* saved = association(slot.scene)) {
            slot.name = saved->name;
            if (!saved->source.empty()) slot.source = saved->source;
            if (!saved->file.empty()) slot.file = saved->file;
        }
        if (slot.service) {
            slot.service->update(context);
            if (auto status = slot.service->status(); status != slot.status) {
                slot.status = status;
                changed_.emplace_back(slot.scene, std::move(status));
                if (changed_.size() > 64) changed_.erase(changed_.begin());
            }
        }
        if (!slot.instance.empty()) {
            const auto* found = instance(slot.instance);
            if (!found) {
                // Blender closed (its record expired). Keep the pairing for
                // the next Open in Blender; forget the process.
                if (slot.missing_since == Clock::time_point{}) slot.missing_since = now;
                else if (now - slot.missing_since > std::chrono::seconds{15}) {
                    slot.instance.clear();
                    slot.missing_since = {};
                }
            } else {
                slot.missing_since = {};
                // The add-on asks for a fresh session for the scene it shows
                // when its pairing is full.
                if (found->resync_requested && found->active_source == slot.source && !slot.resync_handled) {
                    start_service(slot, context);
                    slot.resync_handled = true;
                } else if (!found->resync_requested) slot.resync_handled = false;
                const bool held = std::find(found->scene_sources.begin(), found->scene_sources.end(), slot.source)
                    != found->scene_sources.end();
                if (held && !found->file.empty() && !same_blender_file(found->file, slot.file)) {
                    // Saved As: the scene follows its Blender file.
                    slot.file = found->file;
                    moved_.push_back({slot.scene, found->file});
                }
                // A Blender that no longer has this scene - another file
                // opened, or the scene deleted there - lets it go. A scene just
                // linked gets time to be created there first.
                if (!held && now - slot.bound_at > std::chrono::seconds{15}) {
                    slot.instance.clear();
                    slot.missing_since = {};
                }
            }
        }
        ++it;
    }
    if (state_) {
        // A Blender whose file holds linked scenes is linked to all of them -
        // reopened by hand, or after Pixelsoup restarted.
        for (const auto& item : state_->instances) {
            if (!item.token.empty() && std::any_of(launches_.begin(), launches_.end(),
                    [&](const Launch& launch) { return launch.token == item.token; })) continue;
            for (const auto& source : item.scene_sources) {
                const auto saved = std::find_if(associations.begin(), associations.end(),
                    [&](const BlenderAssociation& entry) { return entry.source == source; });
                if (saved == associations.end()) continue;
                auto* slot = slot_for(saved->scene, false);
                if (slot && (slot->operator_disconnected || slot->instance == item.id
                    || (!slot->instance.empty() && instance(slot->instance)))) continue;
                // Relinked without switching the scene the artist is in.
                auto* bound = slot_for(saved->scene, true);
                if (!bound) continue;
                bound->source = saved->source;
                bound->file = saved->file;
                bound->name = saved->name;
                const auto status = bound->service ? bound->service->status() : nullptr;
                if (!(status && status->listening)) start_service(*bound, context_for(saved->scene));
                bound->instance = item.id;
                bound->bound_at = now;
                bound->missing_since = {};
            }
            // A scene the artist added in Blender: Pixelsoup creates it once.
            if (!item.create_source.empty()
                && std::none_of(associations.begin(), associations.end(),
                    [&](const BlenderAssociation& entry) { return entry.source == item.create_source; })
                && std::find(requested_sources_.begin(), requested_sources_.end(), item.create_source)
                    == requested_sources_.end()) {
                requested_sources_.push_back(item.create_source);
                if (requested_sources_.size() > 64) requested_sources_.erase(requested_sources_.begin());
                created_.push_back({item.create_source, item.create_name.empty() ? std::string{"Blender Scene"}
                    : item.create_name, item.file});
            }
        }
    }
    publish_links(now);
}

std::optional<BlenderLinks::Candidate> BlenderLinks::take(const std::function<bool(doc::NodeId)>& waiting) {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        auto& slot = *slots_[(next_take_ + i) % slots_.size()];
        if (!slot.service || (waiting && waiting(slot.scene))) continue;
        if (auto candidate = slot.service->take()) {
            next_take_ = (next_take_ + i + 1) % slots_.size();
            return Candidate{slot.scene, std::move(candidate)};
        }
    }
    return std::nullopt;
}

void BlenderLinks::resolve(doc::NodeId scene, core::u64 ticket, std::string rejection,
    const render::DirectorLinkContext& context) {
    if (auto* slot = slot_for(scene, false); slot && slot->service) {
        slot->service->update(context);
        slot->service->resolve(ticket, std::move(rejection));
    }
}

void BlenderLinks::sample(const render::LinkedStageSample& sample) noexcept {
    for (auto& slot : slots_)
        if (slot->service && slot->status && slot->status->connected) slot->service->sample(sample);
}

std::shared_ptr<const render::BlenderDiscoveryState> BlenderLinks::discovery() const { return state_; }

std::shared_ptr<const render::DirectorLinkStatus> BlenderLinks::status_for_source(const std::string& uuid) const {
    for (const auto& slot : slots_)
        if (slot->status && !uuid.empty() && slot->status->source_uuid == uuid) return slot->status;
    return nullptr;
}

std::shared_ptr<const render::DirectorLinkStatus> BlenderLinks::status_for_scene(doc::NodeId scene) const {
    for (const auto& slot : slots_) if (slot->scene == scene) return slot->status;
    return nullptr;
}

std::string BlenderLinks::instance_for_scene(doc::NodeId scene) const {
    for (const auto& slot : slots_) if (slot->scene == scene) return slot->instance;
    return {};
}

std::vector<BlenderLinks::View> BlenderLinks::views() const {
    std::vector<View> result;
    for (const auto& slot : slots_) {
        View view;
        view.scene = slot->scene;
        view.running = instance(slot->instance) != nullptr;
        view.opening = launching(slot->scene);
        if (slot->status && !slot->operator_disconnected) {
            view.connected = slot->status->connected;
            view.pending = slot->status->pending;
            view.failed = slot->status->failed;
            view.status = slot->status->message;
        }
        result.push_back(std::move(view));
    }
    for (const auto& launch : launches_)
        if (std::none_of(result.begin(), result.end(), [&](const View& view) { return view.scene == launch.scene.scene; }))
            result.push_back(View{launch.scene.scene, false, true, false, false, false, "Opening Blender..."});
    return result;
}

std::vector<std::pair<std::string, doc::NodeId>> BlenderLinks::bound_instances() const {
    std::vector<std::pair<std::string, doc::NodeId>> result;
    for (const auto& slot : slots_)
        if (!slot->instance.empty()) result.emplace_back(slot->instance, slot->scene);
    return result;
}

bool BlenderLinks::any_connected() const noexcept {
    return std::any_of(slots_.begin(), slots_.end(),
        [](const auto& slot) { return slot->status && slot->status->connected; });
}

std::string BlenderLinks::take_message() { return std::exchange(message_, {}); }

std::vector<std::pair<doc::NodeId, std::shared_ptr<const render::DirectorLinkStatus>>>
BlenderLinks::take_status_changes() { return std::exchange(changed_, {}); }

std::vector<BlenderLinks::Moved> BlenderLinks::take_moved() { return std::exchange(moved_, {}); }

std::vector<BlenderLinks::Created> BlenderLinks::take_created() { return std::exchange(created_, {}); }

} // namespace mediaserver::app
