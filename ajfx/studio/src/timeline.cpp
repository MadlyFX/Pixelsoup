#include "timeline.hpp"

#include "document.hpp"
#include "performance.hpp"

#include <algorithm>
#include <cmath>

namespace ajfx::studio {

std::string_view TrackMarker::label() const {
    switch (kind) {
    case MarkerKind::Release:
        return "Release";
    case MarkerKind::Move:
        return "Move";
    case MarkerKind::Initial:
        return "Initial";
    case MarkerKind::Continuous:
        return "Continuous";
    case MarkerKind::Burst:
        return "Burst";
    case MarkerKind::Blast:
        return "Blast";
    case MarkerKind::Drop:
        return "Dropped";
    case MarkerKind::Delete:
        return "Deleted";
    }
    return "";
}

std::vector<Track> collect_tracks(const Scene &scene, float loop_seconds) {
    std::vector<Track> out;
    for (const SceneItem &item : scene.items) {
        Track track{.item = item.uid, .name = item.name};
        if (const auto *object = std::get_if<Object>(&item.data)) {
            if (object->release > 0.0F) {
                track.markers.push_back({MarkerKind::Release, item.uid, 0U, object->release, 0.0F, true});
            }
            if (object->motion == ObjectMotion::Animated) {
                track.markers.push_back(
                    {MarkerKind::Move, item.uid, 0U, object->move_at, std::max(object->move_over, 0.01F), true});
            }
            // What was done to it while the scene played: its copies
            // dropped, and its bodies (or those copies) deleted.
            for (const Act &act : scene.performance.acts) {
                const float at = static_cast<float>(act.tick) / static_cast<float>(scene_step_rate);
                if (act.kind == ActKind::Drop && act.object == item.uid) {
                    track.markers.push_back({MarkerKind::Drop, item.uid, act.serial, at, 0.0F, true});
                } else if (act.kind == ActKind::Delete) {
                    const Act *dropped = act.target.drop != 0U ? find_act(scene.performance, act.target.drop) : nullptr;
                    if (act.target.object == item.uid || (dropped != nullptr && dropped->object == item.uid)) {
                        track.markers.push_back({MarkerKind::Delete, item.uid, act.serial, at, 0.0F, true});
                    }
                }
            }
        } else if (const auto *system = std::get_if<ParticleSystem>(&item.data)) {
            std::vector<ParticleSource> sources = system->sources;
            std::sort(sources.begin(), sources.end(),
                      [](const ParticleSource &a, const ParticleSource &b) { return a.uid < b.uid; });
            for (const ParticleSource &source : sources) {
                switch (source.mode) {
                case SourceMode::Initial:
                    track.markers.push_back({MarkerKind::Initial, item.uid, source.uid, 0.0F, 0.0F, false});
                    break;
                case SourceMode::Continuous:
                    track.markers.push_back(
                        {MarkerKind::Continuous, item.uid, source.uid, 0.0F, std::max(loop_seconds, 0.01F), false});
                    break;
                case SourceMode::Burst:
                    track.markers.push_back({MarkerKind::Burst, item.uid, source.uid, source.time, 0.0F, true});
                    break;
                }
            }
        } else if (const auto *force = std::get_if<Force>(&item.data)) {
            if (force->kind == ForceKind::Blast) {
                track.markers.push_back({MarkerKind::Blast, item.uid, 0U, force->time, 0.0F, true});
            }
        }
        if (!track.markers.empty()) {
            out.push_back(std::move(track));
        }
    }
    return out;
}

bool set_marker_time(Document &document, const TrackMarker &marker, float seconds) {
    SceneItem *item = document.scene ? document.scene->item(marker.item) : nullptr;
    if (item == nullptr || !marker.draggable) {
        return false;
    }
    seconds = std::clamp(seconds, 0.0F, 3600.0F);
    if (marker.kind == MarkerKind::Drop || marker.kind == MarkerKind::Delete) {
        const Act *act = find_act(document.scene->performance, marker.part);
        const auto tick = static_cast<std::uint64_t>(std::llround(seconds * static_cast<float>(scene_step_rate)));
        if (act == nullptr || act->tick == tick) {
            return false;
        }
        return retime_act(document.scene->performance, marker.part, tick);
    }
    if (auto *object = std::get_if<Object>(&item->data)) {
        float &target = marker.kind == MarkerKind::Release ? object->release : object->move_at;
        if (marker.kind != MarkerKind::Release && marker.kind != MarkerKind::Move) {
            return false;
        }
        // A release at 0 would be no release at all, and its marker would go.
        if (marker.kind == MarkerKind::Release) {
            seconds = std::max(seconds, 1.0F / 60.0F);
        }
        const bool changed = target != seconds;
        target = seconds;
        return changed;
    }
    if (auto *system = std::get_if<ParticleSystem>(&item->data)) {
        for (ParticleSource &source : system->sources) {
            if (source.uid == marker.part && source.mode == SourceMode::Burst && marker.kind == MarkerKind::Burst) {
                const bool changed = source.time != seconds;
                source.time = seconds;
                return changed;
            }
        }
        return false;
    }
    if (auto *force = std::get_if<Force>(&item->data)) {
        if (force->kind != ForceKind::Blast || marker.kind != MarkerKind::Blast) {
            return false;
        }
        const bool changed = force->time != seconds;
        force->time = seconds;
        return changed;
    }
    return false;
}

bool set_marker_duration(Document &document, const TrackMarker &marker, float seconds) {
    SceneItem *item = document.scene ? document.scene->item(marker.item) : nullptr;
    auto *object = item ? std::get_if<Object>(&item->data) : nullptr;
    if (object == nullptr || marker.kind != MarkerKind::Move || !marker.draggable) {
        return false;
    }
    seconds = std::clamp(seconds, 0.01F, 3600.0F);
    const bool changed = object->move_over != seconds;
    object->move_over = seconds;
    return changed;
}

} // namespace ajfx::studio
