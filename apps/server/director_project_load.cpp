#include "director_project_load.hpp"

#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/persist/sealed_frame_log.hpp>
#include <mediaserver/render/media_source.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/show/show_clock.hpp>
#include <mediaserver/show/switcher_authority.hpp>
#include <mediaserver/show/switcher_state.hpp>
#include <mediaserver/wire/protocol_codec.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iostream>
#include <new>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace mediaserver::app {

[[nodiscard]] std::filesystem::path
native_path_from_utf8(std::string_view text) {
    std::u8string encoded;
    encoded.reserve(text.size());
    for (const char byte : text) {
        encoded.push_back(
            static_cast<char8_t>(static_cast<unsigned char>(byte)));
    }
    return std::filesystem::path{encoded};
}

[[nodiscard]] std::string
native_path_to_utf8(const std::filesystem::path &path) {
    const std::u8string encoded = path.u8string();
    std::string text;
    text.reserve(encoded.size());
    for (const char8_t byte : encoded) {
        text.push_back(std::bit_cast<char>(byte));
    }
    return text;
}

[[nodiscard]] bool project_content_changed(const doc::Root &saved,
                                           const doc::Root &current) {
    const doc::Delta delta = doc::diff(saved, current);
    return std::any_of(delta.changes.begin(), delta.changes.end(),
                       [](const doc::NodeChange &change) {
                           if (change.before && change.after &&
                               change.before->type == scene::schema::stage_type &&
                               change.after->type == scene::schema::stage_type) {
                               auto before = *change.before;
                               auto after = *change.after;
                               for (const auto key : {scene::schema::asset_commit_digest,
                                                      scene::schema::asset_commit_control,
                                                      scene::schema::asset_commit_show}) {
                                   before.properties.erase(std::string{key});
                                   after.properties.erase(std::string{key});
                               }
                               return before != after;
                           }
                           // Play/pause/seek re-anchor live transport state;
                           // they should not put an asterisk beside an
                           // otherwise unchanged project. Timeline range edits
                           // set the explicit dirty flag at the editor
                           // boundary.
                           return change.id != show::transport_node_id;
                       });
}

// Recovery may borrow a saved path only when all durable project content
// agrees. Transport playhead/state is live session state, but its direction,
// rate, range, and loop flag are authored project configuration and must match.
[[nodiscard]] bool
recovered_project_matches_saved_baseline(const doc::Root &saved,
                                         const doc::Root &recovered) {
    if (project_content_changed(saved, recovered)) {
        return false;
    }
    const doc::Node *const saved_node = saved.get(show::transport_node_id);
    const doc::Node *const recovered_node =
        recovered.get(show::transport_node_id);
    if (saved_node == nullptr || recovered_node == nullptr) {
        return saved_node == nullptr && recovered_node == nullptr;
    }
    const auto saved_transport = show::read_transport(saved);
    const auto recovered_transport = show::read_transport(recovered);
    if (!saved_transport.has_value() || !recovered_transport.has_value()) {
        return false;
    }
    return saved_transport->direction == recovered_transport->direction &&
           saved_transport->rate == recovered_transport->rate &&
           saved_transport->first_frame == recovered_transport->first_frame &&
           saved_transport->last_frame == recovered_transport->last_frame &&
           saved_transport->loop == recovered_transport->loop;
}

[[nodiscard]] std::string project_file_failure(std::string_view operation,
                                               unsigned code) {
    std::string message{"ERROR "};
    message.append(operation);
    message.append(" PROJECT (");
    message.append(std::to_string(code));
    message.push_back(')');
    return message;
}

[[nodiscard]] std::string_view
project_file_error_label(persist::ProjectFileErrorCode code) noexcept {
    switch (code) {
    case persist::ProjectFileErrorCode::InvalidPath:
        return "INVALID PROJECT PATH";
    case persist::ProjectFileErrorCode::EncodeFailed:
        return "PROJECT DATA CANNOT BE SAVED";
    case persist::ProjectFileErrorCode::OpenFailed:
        return "FILE COULD NOT BE OPENED";
    case persist::ProjectFileErrorCode::ReadFailed:
        return "FILE COULD NOT BE READ";
    case persist::ProjectFileErrorCode::WriteFailed:
        return "FILE COULD NOT BE WRITTEN";
    case persist::ProjectFileErrorCode::SyncFailed:
        return "FILE DURABILITY COULD NOT BE CONFIRMED";
    case persist::ProjectFileErrorCode::ReplaceFailed:
        return "EXISTING FILE COULD NOT BE REPLACED";
    case persist::ProjectFileErrorCode::InvalidFormat:
        return "NOT A MEDIA SERVER PROJECT";
    case persist::ProjectFileErrorCode::UnsupportedVersion:
        return "PROJECT VERSION IS NOT SUPPORTED";
    case persist::ProjectFileErrorCode::CorruptFile:
        return "PROJECT FILE IS DAMAGED OR INCOMPLETE";
    case persist::ProjectFileErrorCode::UnexpectedPayload:
    case persist::ProjectFileErrorCode::InvalidProjectCursor:
        return "NOT A STANDALONE PROJECT";
    case persist::ProjectFileErrorCode::SizeLimitExceeded:
        return "PROJECT EXCEEDS SAFETY LIMITS";
    case persist::ProjectFileErrorCode::AllocationFailed:
        return "NOT ENOUGH MEMORY";
    }
    return "UNKNOWN PROJECT FILE ERROR";
}

[[nodiscard]] std::string
project_file_failure(std::string_view operation,
                     const persist::ProjectFileError &error) {
    std::string message{"ERROR "};
    message.append(operation);
    message.append(" PROJECT: ");
    message.append(project_file_error_label(error.code));
    return message;
}

// Projects authored before native video cadence was persisted remain readable
// as 1:1 sequences in SceneModel. At the Director's project boundary, enrich
// only nodes with *neither* rate field. A partial/malformed authored pair is
// deliberately left intact for the schema model to reject, rather than being
// silently repaired. probe_media_frame_rate itself rejects non-video paths, and
// this cache opens each distinct recognized video at most once per load.
[[nodiscard]] MediaRateEnrichment
enrich_media_frame_rates(const doc::Root &project) {
    struct CachedProbe final {
        std::string source;
        std::optional<render::MediaFrameRate> rate;
    };

    MediaRateEnrichment result{project, 0U};
    std::vector<CachedProbe> probes;
    const auto rate_for =
        [&](std::string_view source) -> std::optional<render::MediaFrameRate> {
        const auto found = std::find_if(
            probes.begin(), probes.end(),
            [&](const CachedProbe &entry) { return entry.source == source; });
        if (found != probes.end()) {
            return found->rate;
        }
        const std::string owned{source};
        const auto rate = render::probe_media_frame_rate(owned);
        probes.push_back(CachedProbe{owned, rate});
        return rate;
    };

    const doc::Delta contents = doc::diff(doc::Root{}, project);
    for (const doc::NodeChange &change : contents.changes) {
        if (!change.after) {
            continue;
        }
        const doc::Node &node = *change.after;
        const bool placement =
            std::string_view{node.type} == scene::schema::media_type;
        const bool asset =
            std::string_view{node.type} == assets::schema::media_type;
        if (!placement && !asset) {
            continue;
        }

        const std::string_view numerator_name =
            placement ? scene::schema::source_rate_numerator
                      : assets::schema::media_rate_numerator;
        const std::string_view denominator_name =
            placement ? scene::schema::source_rate_denominator
                      : assets::schema::media_rate_denominator;
        // Any authored member makes this pair intentional. In particular, do
        // not hide a partial pair by filling in its missing half.
        if (node.properties.find(numerator_name) != node.properties.end() ||
            node.properties.find(denominator_name) != node.properties.end()) {
            continue;
        }
        const auto source = node.properties.find(
            placement ? scene::schema::source : assets::schema::source);
        if (source == node.properties.end()) {
            continue;
        }
        const auto *const path = std::get_if<std::string>(&source->second);
        if (path == nullptr || path->empty()) {
            continue;
        }
        const auto rate = rate_for(*path);
        if (!rate.has_value()) {
            continue;
        }

        doc::Node enriched = node;
        enriched.properties.emplace(
            std::string{numerator_name},
            doc::PropertyValue{static_cast<std::int64_t>(rate->numerator)});
        enriched.properties.emplace(
            std::string{denominator_name},
            doc::PropertyValue{static_cast<std::int64_t>(rate->denominator)});
        result.project = result.project.set(std::move(enriched));
        ++result.updated_nodes;
    }
    return result;
}

[[nodiscard]] bool
restore_project_transport(show::TransportAuthority &transport,
                          const show::TransportAnchor &loaded,
                          cmd::FrameNumber at) noexcept {
    if (!show::valid_transport_anchor(loaded)) {
        return false;
    }
    if (transport.state() == show::TransportState::Playing &&
        !transport.pause(at)) {
        return false;
    }
    if (!transport.set_rate(loaded.rate, at) ||
        !transport.set_range(loaded.first_frame, loaded.last_frame, loaded.loop,
                             at) ||
        !transport.set_cues_armed(loaded.cues_armed, at) ||
        !transport.set_direction(loaded.direction, at) ||
        !transport.seek(loaded.anchor_show_frame, at)) {
        return false;
    }
    switch (loaded.state) {
    case show::TransportState::Stopped:
        return static_cast<bool>(transport.stop(at));
    case show::TransportState::Paused:
        if (transport.state() == show::TransportState::Paused) {
            return true;
        }
        if (transport.state() == show::TransportState::Stopped &&
            !transport.play(at)) {
            return false;
        }
        return static_cast<bool>(transport.pause(at));
    case show::TransportState::Playing:
        return transport.state() == show::TransportState::Playing ||
               static_cast<bool>(transport.play(at));
    }
    return false;
}

// A switcher dissolve is transient control-frame state, not a timeline asset.
// Loading it verbatim can leave Rehearsal locked until a control-frame ordinal
// from another run is reached. Preserve the stable Program/Rehearsal choices,
// validate that both still name scenes, and settle the transition at this
// Director's load boundary.
[[nodiscard]] bool normalize_project_switcher(doc::Root &project,
                                              cmd::FrameNumber at) {
    const doc::Node *const raw = project.get(show::switcher_node_id);
    const auto loaded = show::read_switcher(project);
    if (!loaded.has_value()) {
        return raw == nullptr ||
               std::string_view{raw->type} != show::switcher_node_type;
    }

    scene::SceneModel scenes;
    scenes.reset(project);
    const auto scene_infos = scenes.scenes();
    const auto contains = [&](scene::SceneId id) {
        return std::any_of(
            scene_infos.begin(), scene_infos.end(),
            [id](const scene::SceneInfo &info) { return info.id == id; });
    };
    if (!contains(loaded->program_scene) ||
        !contains(loaded->rehearsal_scene)) {
        return false;
    }

    auto settled = show::SwitcherAuthority::create(loaded->program_scene,
                                                   loaded->rehearsal_scene, at);
    if (!settled) {
        return false;
    }
    project = project.set(
        show::make_switcher_node(std::move(settled).value().anchor()));
    return true;
}

[[nodiscard]] bool
command_log_limit_error(const persist::CommandLogError &error) noexcept {
    if (const auto *const failure = std::get_if<persist::LogFailure>(&error)) {
        return failure->error.code == persist::LogErrorCode::LimitExceeded;
    }
    if (const auto *const failure =
            std::get_if<persist::EncodeFailure>(&error)) {
        return failure->error == wire::EncodeError::SizeLimitExceeded;
    }
    if (const auto *const failure =
            std::get_if<persist::DecodeFailure>(&error)) {
        return failure->error == wire::DecodeError::SizeLimitExceeded;
    }
    return false;
}

// Build the replacement and its exact next-frame seal away from the live
// scheduler. EditsOnly verifies the replicated wire batch without imposing a
// stale WAL tail; EveryFrame instead runs the complete CommandLog append gate.
// The prepared seal is aborted before returning so the normal barrier remains
// the sole live seal owner.
[[nodiscard]] ProjectLoadStageResult
stage_project_load(const cmd::DirectorScheduler &current,
                   const doc::Root &replacement, cmd::FrameNumber frame,
                   persist::CommandLog &command_log,
                   persist::FrameDurability frame_durability) {
    try {
        cmd::DirectorScheduler staged = current;
        auto replaced = staged.schedule_document_replace(
            replacement, cmd::UndoContextId{7U},
            cmd::TransactionId{frame.value()}, frame);
        if (!replaced) {
            return ProjectLoadStageResult::failure(ProjectLoadStageError{
                ProjectLoadStageErrorCode::ScheduleFailed,
                replaced.error(),
                std::nullopt,
                std::nullopt,
            });
        }

        staged.clear_undo_history();
        auto prepared = staged.prepare_seal(frame);
        if (!prepared) {
            return ProjectLoadStageResult::failure(ProjectLoadStageError{
                ProjectLoadStageErrorCode::PrepareFailed,
                std::nullopt,
                prepared.error(),
                std::nullopt,
            });
        }

        const auto token = prepared.value().token;
        auto preflight =
            frame_durability == persist::FrameDurability::EveryFrame
                ? command_log.preflight(staged.commands_for(frame),
                                        prepared.value().seal)
                : command_log.preflight_wire(staged.commands_for(frame),
                                             prepared.value().seal);
        if (!preflight) {
            return ProjectLoadStageResult::failure(ProjectLoadStageError{
                frame_durability == persist::FrameDurability::EveryFrame
                    ? ProjectLoadStageErrorCode::WalPreflightFailed
                    : ProjectLoadStageErrorCode::WirePreflightFailed,
                std::nullopt,
                std::nullopt,
                preflight.error(),
            });
        }
        if (!staged.abort_prepared_seal(token)) {
            return ProjectLoadStageResult::failure(ProjectLoadStageError{
                ProjectLoadStageErrorCode::AbortFailed,
                std::nullopt,
                std::nullopt,
                std::nullopt,
            });
        }
        return ProjectLoadStageResult::success(std::move(staged));
    } catch (const std::bad_alloc &) {
        return ProjectLoadStageResult::failure(ProjectLoadStageError{
            ProjectLoadStageErrorCode::AllocationFailed,
            std::nullopt,
            std::nullopt,
            std::nullopt,
        });
    } catch (const std::length_error &) {
        return ProjectLoadStageResult::failure(ProjectLoadStageError{
            ProjectLoadStageErrorCode::AllocationFailed,
            std::nullopt,
            std::nullopt,
            std::nullopt,
        });
    }
}


void report_recent_project_error(std::string_view operation,
                                 const persist::SettingsError &error) {
    std::cerr << operation << " recent-project setting (code "
              << static_cast<unsigned>(error.code) << ')';
    if (error.system_error) {
        std::cerr << ": " << error.system_error.message();
    }
    std::cerr << '\n';
}

} // namespace mediaserver::app
