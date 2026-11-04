#include "export_gltf.hpp"

#include <ajfx/rigid_solver.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <utility>

namespace ajfx::studio {
namespace {

// glTF's numbers for what it reads.
constexpr int component_float = 5126;
constexpr int component_uint = 5125;
constexpr int target_array = 34962;
constexpr int target_elements = 34963;

std::string number(float v) {
    if (!std::isfinite(v)) {
        return "0";
    }
    char text[32];
    std::snprintf(text, sizeof text, "%.9g", static_cast<double>(v));
    return text;
}

std::string json_text(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20U) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += escaped;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string vec(std::initializer_list<float> values) {
    std::string out = "[";
    bool first = true;
    for (const float v : values) {
        out += first ? "" : ",";
        out += number(v);
        first = false;
    }
    return out + "]";
}

// One body of a rigid layer through the steps it stands.
struct Track final {
    std::size_t emitter{0U};
    std::uint32_t spawn{0U};
    std::uint32_t kind{0U};
    float size{1.0F};
    Color color{};
    std::uint64_t first{0U};
    std::vector<Vec3> position;
    std::vector<Quat> orientation;
    std::optional<std::size_t> parent;
    std::vector<std::size_t> pieces;

    [[nodiscard]] std::uint64_t last() const noexcept { return first + position.size() - 1U; }
};

struct PropTrack final {
    std::size_t emitter{0U};
    std::size_t prop{0U};
    std::vector<Vec3> position;
    std::vector<Quat> orientation;
};

// The binary buffer and the views and accessors into it.
class Buffer final {
public:
    std::vector<std::uint8_t> bytes;
    std::vector<std::string> views;
    std::vector<std::string> accessors;

    // An accessor of `count` `type` items of `component`, with an optional
    // min and max (POSITION and animation input need them).
    std::size_t add(std::span<const std::uint8_t> data, int component, std::size_t count, const char* type,
                    int target = 0, const std::string& min = {}, const std::string& max = {}) {
        while (bytes.size() % 4U != 0U) {
            bytes.push_back(0U);
        }
        const std::size_t offset = bytes.size();
        bytes.insert(bytes.end(), data.begin(), data.end());
        std::string view = "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) +
                           ",\"byteLength\":" + std::to_string(data.size());
        if (target != 0) {
            view += ",\"target\":" + std::to_string(target);
        }
        views.push_back(view + "}");
        std::string accessor = "{\"bufferView\":" + std::to_string(views.size() - 1U) +
                               ",\"componentType\":" + std::to_string(component) +
                               ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"";
        if (!min.empty()) {
            accessor += ",\"min\":" + min + ",\"max\":" + max;
        }
        accessors.push_back(accessor + "}");
        return accessors.size() - 1U;
    }

    template <class T>
    std::size_t add_floats(const std::vector<T>& values, const char* type, int target = 0,
                           const std::string& min = {}, const std::string& max = {}) {
        return add(std::span{reinterpret_cast<const std::uint8_t*>(values.data()), values.size() * sizeof(T)},
                   component_float, values.size(), type, target, min, max);
    }
};

struct Model final {
    std::size_t positions{0U};
    std::size_t normals{0U};
    std::size_t uvs{0U};
    std::size_t indices{0U};
};

float quat_dot(Quat a, Quat b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

} // namespace

GltfFile gltf_from_cache(const Effect& effect, const Cache& cache, const MeshSet& meshes, const GltfOptions& options,
                         std::string bin_name) {
    GltfFile file;
    file.bin_name = std::move(bin_name);
    std::vector<std::size_t> layers;
    for (std::size_t e = 0U; e < effect.emitters.size(); ++e) {
        if (cache.mode(e) == CacheMode::Rigid) {
            layers.push_back(e);
        }
    }
    if (layers.empty()) {
        file.error = "the cache holds no solid world, so there are no bodies to write";
        return file;
    }
    const std::uint64_t frames = options.frames == 0U ? cache.frames() : std::min(options.frames, cache.frames());
    if (frames == 0U) {
        file.error = "the cache holds no step";
        return file;
    }
    file.frames = frames;
    const float rate = static_cast<float>(effect.simulation_rate == 0U ? 60U : effect.simulation_rate);

    // Every body through every step, read a frame at a time.
    std::vector<Track> tracks;
    std::vector<PropTrack> props;
    for (const std::size_t e : layers) {
        const RigidPhysics& physics = *rigid_physics(effect.emitters[e]);
        std::map<std::uint32_t, std::size_t> standing;
        std::vector<std::size_t> prop_of(physics.props.size());
        for (std::size_t p = 0U; p < physics.props.size(); ++p) {
            prop_of[p] = props.size();
            props.push_back(PropTrack{.emitter = e, .prop = p, .position = {}, .orientation = {}});
        }
        EmitterState state;
        for (std::uint64_t tick = 0U; tick < frames; ++tick) {
            if (tick == 0U) {
                state = cache.emitter_at(e, 0U);
            } else {
                cache.advance(e, tick, state);
            }
            std::map<std::uint32_t, std::size_t> now;
            std::vector<std::size_t> born;
            for (std::size_t i = 0U; i < state.alive(); ++i) {
                const std::uint32_t spawn = state.spawn_index[i];
                std::size_t at = 0U;
                if (const auto found = standing.find(spawn); found != standing.end()) {
                    at = found->second;
                } else {
                    at = tracks.size();
                    const std::uint32_t kind = i < state.body_kind.size() ? state.body_kind[i] : 0U;
                    const float scale =
                        kind >= 1U && kind <= physics.bodies.size() ? physics.bodies[kind - 1U].scale : 1.0F;
                    tracks.push_back(Track{.emitter = e,
                                           .spawn = spawn,
                                           .kind = kind,
                                           .size = state.size[i] * scale,
                                           .color = state.color[i],
                                           .first = tick,
                                           .position = {},
                                           .orientation = {},
                                           .parent = std::nullopt,
                                           .pieces = {}});
                    born.push_back(at);
                }
                tracks[at].position.push_back(state.position[i]);
                Quat q = state.orientation[i];
                // Rotations keep to one hemisphere, so each step turns the
                // short way between its keys.
                if (!tracks[at].orientation.empty() && quat_dot(tracks[at].orientation.back(), q) < 0.0F) {
                    q = Quat{-q.x, -q.y, -q.z, -q.w};
                }
                tracks[at].orientation.push_back(q);
                now.emplace(spawn, at);
            }
            // A body born on the step another left, near where it stood, is a
            // piece of it.
            std::vector<std::size_t> left;
            for (const auto& [spawn, at] : standing) {
                if (!now.contains(spawn)) {
                    left.push_back(at);
                }
            }
            if (!left.empty()) {
                for (const std::size_t piece : born) {
                    std::optional<std::size_t> nearest;
                    float best = 0.0F;
                    for (const std::size_t whole : left) {
                        const Vec3 d = tracks[piece].position.front() - tracks[whole].position.back();
                        const float distance = std::sqrt(dot(d, d));
                        if (distance <= tracks[whole].size * 1.5F && (!nearest || distance < best)) {
                            nearest = whole;
                            best = distance;
                        }
                    }
                    if (nearest) {
                        // A piece of a piece belongs to the first whole thing.
                        const std::size_t whole = tracks[*nearest].parent.value_or(*nearest);
                        tracks[piece].parent = whole;
                        tracks[whole].pieces.push_back(piece);
                    }
                }
            }
            standing = std::move(now);
            for (std::size_t p = 0U; p < physics.props.size(); ++p) {
                PropTrack& prop = props[prop_of[p]];
                const RigidBodyState pose = p < state.props.size() ? state.props[p] : RigidBodyState{};
                Quat q = pose.orientation;
                if (!prop.orientation.empty() && quat_dot(prop.orientation.back(), q) < 0.0F) {
                    q = Quat{-q.x, -q.y, -q.z, -q.w};
                }
                prop.position.push_back(pose.position);
                prop.orientation.push_back(q);
            }
        }
    }

    Buffer buffer;
    std::vector<std::string> mesh_json;
    std::vector<std::string> materials;
    std::map<std::string, Model, std::less<>> models;
    std::map<std::string, std::size_t, std::less<>> mesh_of;
    std::map<std::uint32_t, std::size_t> material_of;
    const MeshData cube = cube_mesh();
    const auto model = [&](const std::string& name) -> const Model& {
        if (const auto found = models.find(name); found != models.end()) {
            return found->second;
        }
        const auto held = meshes.find(name);
        const MeshData& data = held != meshes.end() ? held->second : cube;
        std::vector<Vec3> positions;
        std::vector<Vec3> normals;
        std::vector<float> uvs;
        Vec3 lo = data.vertices.empty() ? Vec3{} : data.vertices.front().position;
        Vec3 hi = lo;
        for (const MeshVertex& v : data.vertices) {
            positions.push_back(v.position);
            normals.push_back(v.normal);
            uvs.insert(uvs.end(), {v.u, v.v});
            lo = Vec3{std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
            hi = Vec3{std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
        }
        Model made;
        made.positions = buffer.add_floats(positions, "VEC3", target_array, vec({lo.x, lo.y, lo.z}),
                                           vec({hi.x, hi.y, hi.z}));
        made.normals = buffer.add_floats(normals, "VEC3", target_array);
        made.uvs = buffer.add(std::span{reinterpret_cast<const std::uint8_t*>(uvs.data()), uvs.size() * 4U},
                              component_float, data.vertices.size(), "VEC2", target_array);
        made.indices = buffer.add(std::span{reinterpret_cast<const std::uint8_t*>(data.indices.data()),
                                            data.indices.size() * 4U},
                                  component_uint, data.indices.size(), "SCALAR", target_elements);
        return models.emplace(name, made).first->second;
    };
    const auto material = [&](Color c) {
        const auto channel = [](float v) {
            return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
        };
        const std::uint32_t key = channel(c.r) << 24U | channel(c.g) << 16U | channel(c.b) << 8U | channel(c.a);
        if (const auto found = material_of.find(key); found != material_of.end()) {
            return found->second;
        }
        char name[16];
        std::snprintf(name, sizeof name, "#%08x", key);
        materials.push_back("{\"name\":" + json_text(name) + ",\"pbrMetallicRoughness\":{\"baseColorFactor\":" +
                            vec({c.r, c.g, c.b, std::clamp(c.a, 0.0F, 1.0F)}) +
                            ",\"metallicFactor\":0,\"roughnessFactor\":0.8}" +
                            (c.a < 1.0F ? std::string{",\"alphaMode\":\"BLEND\""} : std::string{}) + "}");
        return material_of.emplace(key, materials.size() - 1U).first->second;
    };
    const auto mesh = [&](const std::string& name, Color c) {
        const std::size_t m = material(c);
        const std::string key = name + "\x1f" + std::to_string(m);
        if (const auto found = mesh_of.find(key); found != mesh_of.end()) {
            return found->second;
        }
        const Model& geometry = model(name);
        mesh_json.push_back("{\"name\":" + json_text(name.empty() ? std::string{"cube"} : name) +
                            ",\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(geometry.positions) +
                            ",\"NORMAL\":" + std::to_string(geometry.normals) +
                            ",\"TEXCOORD_0\":" + std::to_string(geometry.uvs) +
                            "},\"indices\":" + std::to_string(geometry.indices) +
                            ",\"material\":" + std::to_string(m) + "}]}");
        return mesh_of.emplace(key, mesh_json.size() - 1U).first->second;
    };

    std::vector<std::string> nodes;
    std::vector<std::string> samplers;
    std::vector<std::string> channels;
    const auto animate = [&](std::size_t node, std::uint64_t first, const std::vector<Vec3>& positions,
                             const std::vector<Quat>& orientations) {
        std::vector<float> times(positions.size());
        for (std::size_t k = 0U; k < times.size(); ++k) {
            times[k] = static_cast<float>(first + k) / rate;
        }
        const std::size_t input =
            buffer.add_floats(times, "SCALAR", 0, vec({times.front()}), vec({times.back()}));
        const std::size_t translation = buffer.add_floats(positions, "VEC3");
        const std::size_t rotation = buffer.add_floats(orientations, "VEC4");
        samplers.push_back("{\"input\":" + std::to_string(input) + ",\"output\":" + std::to_string(translation) +
                           ",\"interpolation\":\"LINEAR\"}");
        channels.push_back("{\"sampler\":" + std::to_string(samplers.size() - 1U) + ",\"target\":{\"node\":" +
                           std::to_string(node) + ",\"path\":\"translation\"}}");
        samplers.push_back("{\"input\":" + std::to_string(input) + ",\"output\":" + std::to_string(rotation) +
                           ",\"interpolation\":\"LINEAR\"}");
        channels.push_back("{\"sampler\":" + std::to_string(samplers.size() - 1U) + ",\"target\":{\"node\":" +
                           std::to_string(node) + ",\"path\":\"rotation\"}}");
    };
    // Shown from the step it is born to the step after it last stands.
    const auto show = [&](std::size_t node, const Track& track) {
        std::vector<float> times;
        std::vector<Vec3> scales;
        const Vec3 full{track.size, track.size, track.size};
        if (track.first > 0U) {
            times.push_back(0.0F);
            scales.push_back(Vec3{});
        }
        times.push_back(static_cast<float>(track.first) / rate);
        scales.push_back(full);
        if (track.last() + 1U < frames) {
            times.push_back(static_cast<float>(track.last() + 1U) / rate);
            scales.push_back(Vec3{});
        }
        if (times.size() == 1U) {
            return; // there all along: its node's scale is enough
        }
        const std::size_t input =
            buffer.add_floats(times, "SCALAR", 0, vec({times.front()}), vec({times.back()}));
        const std::size_t output = buffer.add_floats(scales, "VEC3");
        samplers.push_back("{\"input\":" + std::to_string(input) + ",\"output\":" + std::to_string(output) +
                           ",\"interpolation\":\"STEP\"}");
        channels.push_back("{\"sampler\":" + std::to_string(samplers.size() - 1U) + ",\"target\":{\"node\":" +
                           std::to_string(node) + ",\"path\":\"scale\"}}");
    };
    const auto name_of = [&](const Track& track) {
        if (options.name) {
            if (std::string named = options.name(track.emitter, track.spawn); !named.empty()) {
                return named;
            }
        }
        return effect.emitters[track.emitter].name + " body " + std::to_string(track.spawn);
    };
    const auto body_node = [&](std::size_t t, const std::string& name) {
        const Track& track = tracks[t];
        const RigidPhysics& physics = *rigid_physics(effect.emitters[track.emitter]);
        const std::string& own = effect.emitters[track.emitter].renderer.mesh;
        const std::string drawn = track.kind >= 1U && track.kind <= physics.bodies.size() &&
                                          !physics.bodies[track.kind - 1U].draw.empty()
                                      ? physics.bodies[track.kind - 1U].draw
                                      : own;
        const float s = track.first > 0U ? 0.0F : track.size;
        const Vec3 p = track.position.front();
        const Quat q = track.orientation.front();
        nodes.push_back("{\"name\":" + json_text(name) + ",\"mesh\":" + std::to_string(mesh(drawn, track.color)) +
                        ",\"translation\":" + vec({p.x, p.y, p.z}) + ",\"rotation\":" + vec({q.x, q.y, q.z, q.w}) +
                        ",\"scale\":" + vec({s, s, s}) + "}");
        const std::size_t node = nodes.size() - 1U;
        animate(node, track.first, track.position, track.orientation);
        show(node, track);
        return node;
    };
    const auto group = [&](const std::string& name, const std::vector<std::size_t>& children) {
        std::string list;
        for (const std::size_t child : children) {
            list += (list.empty() ? "" : ",") + std::to_string(child);
        }
        nodes.push_back("{\"name\":" + json_text(name) + ",\"children\":[" + list + "]}");
        return nodes.size() - 1U;
    };

    std::vector<std::size_t> roots;
    for (const std::size_t e : layers) {
        std::vector<std::size_t> children;
        for (std::size_t t = 0U; t < tracks.size(); ++t) {
            if (tracks[t].emitter != e || tracks[t].parent) {
                continue;
            }
            const std::string name = name_of(tracks[t]);
            ++file.bodies;
            if (tracks[t].pieces.empty()) {
                children.push_back(body_node(t, name));
                continue;
            }
            // The whole thing, then what it broke into, side by side under
            // one group so each keeps the world's frame.
            std::vector<std::size_t> parts{body_node(t, name + " (whole)")};
            for (std::size_t k = 0U; k < tracks[t].pieces.size(); ++k) {
                parts.push_back(body_node(tracks[t].pieces[k], name + " piece " + std::to_string(k + 1U)));
                ++file.pieces;
            }
            children.push_back(group(name, parts));
        }
        const RigidPhysics& physics = *rigid_physics(effect.emitters[e]);
        for (const PropTrack& prop : props) {
            if (prop.emitter != e) {
                continue;
            }
            const RigidProp& authored = physics.props[prop.prop];
            const Vec3 p = prop.position.front();
            const Quat q = prop.orientation.front();
            const Color tint = authored.draw_tint == Color{} ? Color{0.8F, 0.8F, 0.8F, 1.0F} : authored.draw_tint;
            nodes.push_back("{\"name\":" + json_text(effect.emitters[e].name + " prop " + std::to_string(prop.prop + 1U)) +
                            ",\"mesh\":" + std::to_string(mesh(authored.mesh, tint)) + ",\"translation\":" +
                            vec({p.x, p.y, p.z}) + ",\"rotation\":" + vec({q.x, q.y, q.z, q.w}) + ",\"scale\":" +
                            vec({authored.size, authored.size, authored.size}) + "}");
            const bool moves = std::any_of(prop.position.begin(), prop.position.end(),
                                           [&](Vec3 v) { return !(v == p); }) ||
                               std::any_of(prop.orientation.begin(), prop.orientation.end(),
                                           [&](Quat r) { return !(r == q); });
            if (moves) {
                animate(nodes.size() - 1U, 0U, prop.position, prop.orientation);
            }
            children.push_back(nodes.size() - 1U);
            ++file.props;
        }
        roots.push_back(group(effect.emitters[e].name, children));
    }

    const auto join = [](const std::vector<std::string>& items) {
        std::string out;
        for (const std::string& item : items) {
            out += (out.empty() ? "" : ",\n") + item;
        }
        return out;
    };
    std::string root_list;
    for (const std::size_t root : roots) {
        root_list += (root_list.empty() ? "" : ",") + std::to_string(root);
    }
    std::string& json = file.json;
    json = "{\n\"asset\":{\"version\":\"2.0\",\"generator\":\"ajfx Studio\"},\n";
    json += "\"scene\":0,\n\"scenes\":[{\"name\":" + json_text(options.scene) + ",\"nodes\":[" + root_list + "]}],\n";
    json += "\"nodes\":[\n" + join(nodes) + "],\n";
    json += "\"meshes\":[\n" + join(mesh_json) + "],\n";
    json += "\"materials\":[\n" + join(materials) + "],\n";
    if (!channels.empty()) {
        json += "\"animations\":[{\"name\":\"performance\",\"samplers\":[\n" + join(samplers) +
                "],\n\"channels\":[\n" + join(channels) + "]}],\n";
    }
    json += "\"accessors\":[\n" + join(buffer.accessors) + "],\n";
    json += "\"bufferViews\":[\n" + join(buffer.views) + "],\n";
    json += "\"buffers\":[{\"uri\":" + json_text(file.bin_name) + ",\"byteLength\":" +
            std::to_string(buffer.bytes.size()) + "}]\n}\n";
    file.bin = std::move(buffer.bytes);
    return file;
}

bool write_gltf(const GltfFile& file, const std::filesystem::path& path, std::string& error) {
    const std::filesystem::path bin = path.parent_path() / file.bin_name;
    {
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        out.write(file.json.data(), static_cast<std::streamsize>(file.json.size()));
        if (!out) {
            error = "cannot write " + path.string();
            return false;
        }
    }
    std::ofstream out{bin, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(file.bin.data()), static_cast<std::streamsize>(file.bin.size()));
    if (!out) {
        error = "cannot write " + bin.string();
        return false;
    }
    return true;
}

} // namespace ajfx::studio
