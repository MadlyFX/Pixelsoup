#include "session.hpp"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ajfx::studio {
namespace {

json::Value uids(const std::vector<Uid>& values) {
    json::Value array = json::Value::array();
    for (const Uid uid : values) {
        array.push(json::Value::of(static_cast<double>(uid)));
    }
    return array;
}

std::vector<Uid> uids_from(const json::Value* value) {
    std::vector<Uid> out;
    if (value == nullptr) {
        return out;
    }
    for (const json::Value& item : value->items) {
        if (item.type == json::Value::Type::Number && item.number >= 1.0 && item.number < 4.0e9) {
            out.push_back(static_cast<Uid>(item.number));
        }
    }
    return out;
}

} // namespace

std::string save_session(const SessionState& session) {
    json::Value root = json::Value::object();
    root.add("format", json::Value::of("ajfx-studio-session"));
    root.add("page", json::Value::of(session.page));
    if (!session.unsaved.empty()) {
        root.add("unsaved", json::Value::of(session.unsaved));
    }
    root.add("width", json::Value::of(static_cast<double>(session.width)));
    root.add("height", json::Value::of(static_cast<double>(session.height)));
    root.add("guides", json::Value::of(session.guides));
    root.add("handles", json::Value::of(session.handles));
    root.add("paths", json::Value::of(session.paths));
    root.add("figure", json::Value::of(session.figure));
    root.add("night", json::Value::of(session.night));
    root.add("zoom", json::Value::of(static_cast<double>(session.zoom)));
    root.add("looping", json::Value::of(session.looping));
    root.add("scene_tree_split", json::Value::of(static_cast<double>(session.scene_tree_split)));
    root.add("scene_inspector_split", json::Value::of(static_cast<double>(session.scene_inspector_split)));
    root.add("scene_selection", uids(session.scene_selection));
    root.add("scene_folded", uids(session.scene_folded));
    if (session.camera) {
        json::Value camera = json::Value::array();
        for (const float v : *session.camera) {
            camera.push(json::Value::of(static_cast<double>(v)));
        }
        root.add("camera", std::move(camera));
    }
    root.add("seconds", json::Value::of(session.seconds));
    root.add("playing", json::Value::of(session.playing));
    return json::write(root);
}

SessionState load_session(std::string_view text) {
    SessionState session;
    json::ParseError error;
    const std::optional<json::Value> root = json::parse(text, error);
    if (!root || root->type != json::Value::Type::Object ||
        root->string_or("format", "") != "ajfx-studio-session") {
        return session;
    }
    session.page = root->string_or("page", "");
    session.unsaved = root->string_or("unsaved", "");
    const auto size = [&](std::string_view key) {
        const double v = root->number_or(key, 0.0);
        return std::isfinite(v) && v >= 320.0 && v <= 16384.0 ? static_cast<std::uint32_t>(v) : 0U;
    };
    session.width = size("width");
    session.height = size("height");
    session.guides = root->bool_or("guides", true);
    session.handles = root->bool_or("handles", true);
    session.paths = root->bool_or("paths", false);
    session.figure = root->bool_or("figure", true);
    session.night = root->bool_or("night", false);
    const double zoom = root->number_or("zoom", 1.0);
    session.zoom = std::isfinite(zoom) ? static_cast<float>(std::clamp(zoom, 0.75, 1.5)) : 1.0F;
    session.looping = root->bool_or("looping", true);
    const double tree = root->number_or("scene_tree_split", 0.19);
    const double inspector = root->number_or("scene_inspector_split", 0.66);
    session.scene_tree_split = std::isfinite(tree) ? static_cast<float>(std::clamp(tree, 0.12, 0.35)) : 0.19F;
    session.scene_inspector_split = std::isfinite(inspector) ? static_cast<float>(std::clamp(inspector, 0.4, 0.8)) : 0.66F;
    session.scene_selection = uids_from(root->find("scene_selection"));
    session.scene_folded = uids_from(root->find("scene_folded"));
    if (const json::Value* camera = root->find("camera"); camera != nullptr && camera->items.size() == 6U) {
        std::array<float, 6> values{};
        bool ok = true;
        for (std::size_t i = 0U; i < 6U; ++i) {
            const json::Value& item = camera->items[i];
            ok = ok && item.type == json::Value::Type::Number && std::isfinite(item.number) &&
                 std::fabs(item.number) < 1.0e6;
            values[i] = static_cast<float>(item.number);
        }
        if (ok && values[2] > 0.0F) {
            session.camera = values;
        }
    }
    const double seconds = root->number_or("seconds", 0.0);
    session.seconds = std::isfinite(seconds) ? std::clamp(seconds, 0.0, 120.0) : 0.0;
    session.playing = root->bool_or("playing", true);
    return session;
}

std::filesystem::path session_path() {
    std::filesystem::path base;
#if defined(_WIN32)
    char* appdata = nullptr;
    std::size_t length = 0U;
    if (_dupenv_s(&appdata, &length, "APPDATA") == 0 && appdata != nullptr) {
        if (*appdata != '\0') {
            base = appdata;
        }
        std::free(appdata);
    }
#else
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config != nullptr && *config != '\0') {
        base = config;
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        base = std::filesystem::path{home} / ".config";
    }
#endif
    if (base.empty()) {
        return {};
    }
    return base / "ajfx Studio" / "session.json";
}

} // namespace ajfx::studio
