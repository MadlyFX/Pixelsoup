#include <mediaserver/render/linked_shot.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace mediaserver::render {
namespace {
namespace ls = scene::link_schema;
std::string text(const doc::Node& node, std::string_view key) {
    const auto p = node.properties.find(key);
    if (p != node.properties.end()) if (const auto* v = std::get_if<std::string>(&p->second)) return *v;
    return {};
}
doc::NodeId ref(const doc::Node& node, std::string_view key) {
    const auto p = node.properties.find(key);
    if (p != node.properties.end()) if (const auto* v = std::get_if<core::i64>(&p->second); v && *v > 0)
        return doc::NodeId{static_cast<core::u64>(*v)};
    return {};
}
void matrix(std::ostream& out, const scene::Mat4& value) {
    for (const auto v : value.m) out << ' ' << v;
}
scene::Mat4 matrix(const doc::Node& node, std::string_view prefix) {
    auto result = scene::identity();
    for (std::size_t i = 0; i < 16; ++i) {
        const auto p = node.properties.find(std::string{prefix} + std::to_string(i));
        if (p != node.properties.end()) result.m[i] = static_cast<float>(std::get<double>(p->second));
    }
    return result;
}
int visibility(const doc::Node& node) {
    const auto p = node.properties.find(scene::schema::visible);
    return p == node.properties.end() ? -1 : (std::get<bool>(p->second) ? 1 : 0);
}
scene::LinkedOccurrence occurrence(const DirectorLinkContext& c, const std::string& source, doc::NodeId id) {
    const auto state = scene::read_linked_scene(c.document);
    if (!state || !state.value().occurrences.contains(id)) throw std::runtime_error{"shot_occurrence_missing"};
    const auto& o = state.value().occurrences.at(id);
    const auto* src = c.document.get(ref(*c.document.get(o.object), ls::source));
    if (o.scene != c.target || !src || text(*src, ls::uuid) != source || o.removed)
        throw std::runtime_error{"shot_target_mismatch_or_removed"};
    return o;
}
std::string token(const DirectorLinkContext& c, const scene::LinkedOccurrence& o, const std::string& property) {
    const auto& node = *c.document.get(o.node);
    std::ostringstream out; out << std::setprecision(9) << o.node.value() << ' ' << o.instance.value();
    if (property == "placement") {
        out << ' ' << o.held; matrix(out, matrix(node, ls::offset)); matrix(out, matrix(node, ls::held_world));
        // Parent/source/stage changes can alter conversion, but geometry cannot.
        matrix(out, o.basis); matrix(out, o.world);
    } else if (property == "visibility") out << ' ' << visibility(node) << ' ' << o.visible;
    else throw std::runtime_error{"unknown_shot_property"};
    const auto value = out.str();
    return assets::sha256_hex(assets::sha256_digest({reinterpret_cast<const core::u8*>(value.data()), value.size()}));
}
}
std::string linked_shot_revision(const DirectorLinkContext& c, const std::string& source,
        doc::NodeId id, const std::string& property) {
    return token(c, occurrence(c, source, id), property);
}
std::string linked_shot_state(const DirectorLinkContext& c, const std::string& source) {
    const auto state = scene::read_linked_scene(c.document);
    if (!state) throw std::runtime_error{state.error().message};
    std::ostringstream out; out << std::setprecision(9) << "MSSHOT 1\n";
    std::size_t count{};
    for (const auto& [id, o] : state.value().occurrences) {
        if (o.scene != c.target) continue;
        const auto& object = *c.document.get(o.object);
        const auto* src = c.document.get(ref(object, ls::source));
        if (!src || text(*src, ls::uuid) != source) continue;
        if (++count > 512) throw std::runtime_error{"reverse_placement_capacity_512"};
        const auto& node = *c.document.get(id);
        const auto& instance = *c.document.get(o.instance);
        out << id.value() << ' ' << o.instance.value() << ' ' << std::quoted(text(object, ls::uuid))
            << ' ' << std::quoted(token(c, o, "placement")) << ' ' << std::quoted(token(c, o, "visibility"))
            << ' ' << o.held << ' ' << visibility(node) << ' ' << o.visible << ' ' << o.removed;
        matrix(out, matrix(node, ls::offset)); matrix(out, matrix(node, ls::held_world));
        matrix(out, matrix(instance, ls::alignment)); matrix(out, o.world);
        out << '\n';
    }
    return out.str();
}
scene::LinkedPlanResult plan_linked_shot(const DirectorLinkCandidate& c, const DirectorLinkContext& now) {
    const auto fail = [&](const std::string& message) { return scene::LinkedPlanResult::failure({c.shot_node, message}); };
    try {
        if (std::chrono::steady_clock::now() >= c.shot_deadline) return fail("shot_claim_expired: retry_gesture");
        if (linked_shot_revision(now, c.manifest.source_uuid, c.shot_node, c.shot_property) != c.shot_base)
            return fail("shot_conflict: Director_changed_property; accept_Director_or_retry");
        const auto* old = now.document.get(c.shot_node);
        const auto locked = old->properties.find(scene::schema::locked);
        if (locked != old->properties.end() && std::get<bool>(locked->second)) return fail("shot_occurrence_locked");
        std::istringstream input{c.shot_edit}; std::string action; input >> action;
        if (c.shot_property == "placement" && action == "world") {
            scene::Mat4 world; for (auto& value : world.m) input >> value;
            if (input.fail()) return fail("invalid_shot_world");
            input >> std::ws; if (!input.eof()) return fail("unexpected_shot_fields");
            return scene::plan_linked_world(now.document, c.shot_node, world);
        }
        if (c.shot_property == "placement" && action == "restore") {
            int held{}; scene::Mat4 offset, world;
            input >> held; for (auto& v : offset.m) input >> v; for (auto& v : world.m) input >> v;
            if (input.fail() || (held != 0 && held != 1)) return fail("invalid_shot_restore");
            input >> std::ws; if (!input.eof()) return fail("unexpected_shot_fields");
            auto node = *old; node.properties[std::string{ls::policy}] = std::string{held ? ls::hold : ls::follow};
            scene::write_link_matrix(node, ls::offset, offset); scene::write_link_matrix(node, ls::held_world, world);
            return scene::plan_linked_change(now.document, now.document.set(std::move(node)));
        }
        input >> std::ws; if (!input.eof()) return fail("unexpected_shot_fields");
        if (c.shot_property == "placement") {
            if (action == "hold") return scene::plan_linked_edit(now.document, c.shot_node, scene::LinkedEdit::Hold);
            if (action == "follow") return scene::plan_linked_edit(now.document, c.shot_node, scene::LinkedEdit::Follow);
            if (action == "reset") return scene::plan_linked_edit(now.document, c.shot_node, scene::LinkedEdit::ResetPlacement);
        } else if (c.shot_property == "visibility") {
            if (action == "follow") return scene::plan_linked_edit(now.document, c.shot_node, scene::LinkedEdit::FollowVisibility);
            if (action == "hide" || action == "show") {
                auto node = *old; node.properties[std::string{scene::schema::visible}] = action == "show";
                return scene::plan_linked_change(now.document, now.document.set(std::move(node)));
            }
        }
        return fail("invalid_shot_edit");
    } catch (const std::exception& e) { return fail(e.what()); }
}
}
