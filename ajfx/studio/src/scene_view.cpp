#include "scene_view.hpp"

#include <tinyfiledialogs.h>

#include <algorithm>
#include <filesystem>

namespace ajfx::studio {
namespace {
bool visible_property(const SceneItem &item, const SceneProperty &p) {
    if (const auto *o = std::get_if<Object>(&item.data)) {
        if (p.section == "Animation" && o->motion != ObjectMotion::Animated) {
            return false;
        }
        if (p.section == "Fracture" && p.control.id != "fracture" && !o->fracture) {
            return false;
        }
        if ((o->distribution == Distribution::Single || o->distribution == Distribution::Placed) &&
            (p.control.id == "count" || p.control.id == "width" || p.control.id == "depth" ||
             p.control.id == "spacing" || p.control.id == "variation")) {
            return false;
        }
    }
    if (const auto *s = std::get_if<ParticleSystem>(&item.data); s && s->persistent && p.control.id == "lifetime") {
        return false;
    }
    if (const auto *s = std::get_if<ParticleSystem>(&item.data);
        s && !s->casts_light &&
        (p.control.id == "light_count" || p.control.id == "light_strength" || p.control.id == "light_reach")) {
        return false;
    }
    if (const auto *f = std::get_if<Force>(&item.data)) {
        const std::string_view id = p.control.id;
        const bool blast = f->kind == ForceKind::Blast;
        if (id == "position") { return f->kind != ForceKind::Wind; }
        if (id == "strength") { return !blast; }
        if (id == "heading") { return f->kind == ForceKind::Wind; }
        if (id == "axis") { return f->kind == ForceKind::Vortex; }
        if (id == "reach") { return f->kind == ForceKind::Attract; }
        if (id == "height") { return f->kind == ForceKind::DrawIn; }
        if (id == "power" || id == "radius" || id == "time") { return blast; }
        if (id == "all_systems") { return !blast; }
    }
    if (const auto *s = std::get_if<ParticleSystem>(&item.data);
        s && p.control.id == "persistent" &&
        std::any_of(s->sources.begin(), s->sources.end(),
                    [](const ParticleSource &source) { return source.mode == SourceMode::Continuous; })) {
        return false;
    }
    return true;
}
} // namespace

bool property_visible(const SceneItem &item, const SceneProperty &p) { return visible_property(item, p); }

void SceneView::forget() {
    selection_.clear();
    folded_.clear();
    im_live_ = false;
}
void SceneView::select(Uid uid, bool extend) {
    if (!extend) {
        selection_.clear();
    }
    const auto at = std::find(selection_.begin(), selection_.end(), uid);
    if (at == selection_.end() && uid != 0U) {
        selection_.push_back(uid);
    } else if (at != selection_.end() && extend) {
        selection_.erase(at);
    }
}
void SceneView::import_models(Document &document) {
    const char *filters[] = {"*.obj", "*.gltf", "*.glb"};
    if (const char *paths = tinyfd_openFileDialog("Import models", "", 3, filters, "3D models", 1)) {
        selection_.clear();
        const std::string files{paths};
        for (std::size_t at = 0U; at < files.size();) {
            const auto end = files.find('|', at);
            const std::string file = files.substr(at, end == std::string::npos ? end : end - at);
            const Uid uid = add_object(document, std::filesystem::path{file}.generic_string());
            if (uid != 0U) {
                selection_.push_back(uid);
            }
            if (end == std::string::npos) {
                break;
            }
            at = end + 1U;
        }
    }
}
} // namespace ajfx::studio
