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

json::Value view_json(const PageViewState& view) {
    json::Value out = json::Value::object();
    out.add("folded", uids(view.folded));
    out.add("open", uids(view.open));
    out.add("details", uids(view.details));
    out.add("focus", json::Value::of(static_cast<double>(view.focus)));
    out.add("layer", json::Value::of(static_cast<double>(view.layer)));
    out.add("scroll", json::Value::of(static_cast<double>(view.scroll)));
    return out;
}

PageViewState view_from(const json::Value* in) {
    PageViewState view;
    if (in == nullptr || in->type != json::Value::Type::Object) {
        return view;
    }
    view.folded = uids_from(in->find("folded"));
    view.open = uids_from(in->find("open"));
    view.details = uids_from(in->find("details"));
    view.focus = static_cast<Uid>(std::clamp(in->number_or("focus", 0.0), 0.0, 4.0e9));
    view.layer = static_cast<Uid>(std::clamp(in->number_or("layer", 0.0), 0.0, 4.0e9));
    const double scroll = in->number_or("scroll", 0.0);
    view.scroll = std::isfinite(scroll) ? static_cast<float>(std::clamp(scroll, 0.0, 1.0e7)) : 0.0F;
    return view;
}

} // namespace

std::string save_session(const SessionState& session) {
    json::Value root = json::Value::object();
    root.add("format", json::Value::of("ajfx-studio-session"));
    root.add("page", json::Value::of(session.page));
    if (!session.unsaved.empty()) {
        root.add("unsaved", json::Value::of(session.unsaved));
    }
    root.add("view", view_json(session.view));
    json::Value recent = json::Value::array();
    for (const auto& [page, view] : session.recent) {
        json::Value entry = view_json(view);
        entry.add("page", json::Value::of(page));
        recent.push(std::move(entry));
    }
    root.add("recent", std::move(recent));
    root.add("width", json::Value::of(static_cast<double>(session.width)));
    root.add("height", json::Value::of(static_cast<double>(session.height)));
    root.add("split", json::Value::of(static_cast<double>(session.split)));
    root.add("palette", json::Value::of(session.palette));
    json::Value folded = json::Value::array();
    for (const std::string& heading : session.palette_folded) {
        folded.push(json::Value::of(heading));
    }
    root.add("palette_folded", std::move(folded));
    root.add("guides", json::Value::of(session.guides));
    root.add("handles", json::Value::of(session.handles));
    root.add("paths", json::Value::of(session.paths));
    root.add("figure", json::Value::of(session.figure));
    root.add("expert", json::Value::of(session.expert));
    root.add("looping", json::Value::of(session.looping));
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
    session.view = view_from(root->find("view"));
    if (const json::Value* recent = root->find("recent"); recent != nullptr) {
        for (const json::Value& entry : recent->items) {
            const std::string page = entry.string_or("page", "");
            if (!page.empty() && session.recent.size() < remembered_pages) {
                session.recent.emplace_back(page, view_from(&entry));
            }
        }
    }
    const auto size = [&](std::string_view key) {
        const double v = root->number_or(key, 0.0);
        return std::isfinite(v) && v >= 320.0 && v <= 16384.0 ? static_cast<std::uint32_t>(v) : 0U;
    };
    session.width = size("width");
    session.height = size("height");
    const double split = root->number_or("split", 0.46);
    session.split = std::isfinite(split) ? static_cast<float>(std::clamp(split, 0.15, 0.85)) : 0.46F;
    session.palette = root->bool_or("palette", true);
    if (const json::Value* folded = root->find("palette_folded"); folded != nullptr) {
        for (const json::Value& item : folded->items) {
            if (item.type == json::Value::Type::String) {
                session.palette_folded.push_back(item.text);
            }
        }
    }
    session.guides = root->bool_or("guides", true);
    session.handles = root->bool_or("handles", true);
    session.paths = root->bool_or("paths", false);
    session.figure = root->bool_or("figure", true);
    session.expert = root->bool_or("expert", false);
    session.looping = root->bool_or("looping", true);
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

void remember_view(SessionState& session, const std::string& page, const PageViewState& view) {
    if (page.empty()) {
        return;
    }
    std::erase_if(session.recent, [&](const auto& entry) { return entry.first == page; });
    session.recent.insert(session.recent.begin(), {page, view});
    if (session.recent.size() > remembered_pages) {
        session.recent.resize(remembered_pages);
    }
}

std::optional<PageViewState> remembered_view(const SessionState& session, std::string_view page) {
    for (const auto& [path, view] : session.recent) {
        if (path == page) {
            return view;
        }
    }
    return std::nullopt;
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
