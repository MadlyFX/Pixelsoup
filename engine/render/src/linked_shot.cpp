#include <mediaserver/render/linked_shot.hpp>
#include <mediaserver/scene/linked_light.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <array>
#include <iomanip>
#include <span>
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
// The overrides an occurrence carries for `keys`, as `key=value,...`.
std::string overrides(const doc::Node& node, std::span<const std::string_view> keys) {
    std::ostringstream out; out << std::setprecision(9);
    bool first = true;
    for (const auto key : keys) {
        const auto found = node.properties.find(key);
        if (found == node.properties.end()) continue;
        if (!first) out << ',';
        first = false;
        out << key << '=';
        if (const auto* number = std::get_if<double>(&found->second)) out << *number;
        else if (const auto* flag = std::get_if<bool>(&found->second)) out << (*flag ? 1 : 0);
        else if (const auto* integer = std::get_if<core::i64>(&found->second)) out << *integer;
    }
    return out.str();
}
constexpr std::array<std::string_view, 8> material_keys{scene::schema::color_r, scene::schema::color_g,
    scene::schema::color_b, scene::schema::roughness, scene::schema::metallic, scene::schema::emissive_r,
    scene::schema::emissive_g, scene::schema::emissive_b};
}
std::string linked_shot_state(const DirectorLinkContext& c, const std::string& source) {
    const auto state = scene::read_linked_scene(c.document);
    if (!state) throw std::runtime_error{state.error().message};
    std::ostringstream out; out << std::setprecision(9) << "MSSHOT 2\n";
    std::size_t count{};
    for (const auto& [id, o] : state.value().occurrences) {
        if (o.scene != c.target) continue;
        const auto& object = *c.document.get(o.object);
        const auto* src = c.document.get(ref(object, ls::source));
        if (!src || text(*src, ls::uuid) != source) continue;
        if (++count > 512) throw std::runtime_error{"writeback_capacity_512"};
        const auto& node = *c.document.get(id);
        const auto& instance = *c.document.get(o.instance);
        out << id.value() << ' ' << o.instance.value() << ' ' << std::quoted(text(object, ls::uuid))
            << ' ' << o.held << ' ' << visibility(node) << ' ' << o.visible << ' ' << o.removed;
        matrix(out, matrix(node, ls::offset)); matrix(out, matrix(node, ls::held_world));
        matrix(out, matrix(instance, ls::alignment)); matrix(out, o.world);
        // Light and material overrides, and the source parent: a mesh split
        // into parts by material is one object in Blender.
        const bool light = node.type == scene::schema::light_type;
        const auto* parent = c.document.get(ref(object, ls::parent));
        out << ' ' << std::quoted(light ? overrides(node, scene::linked_light_overrides) : std::string{})
            << ' ' << std::quoted(light ? std::string{} : overrides(node, material_keys))
            << ' ' << std::quoted(parent ? text(*parent, ls::uuid) : std::string{}) << '\n';
    }
    return out.str();
}
}
