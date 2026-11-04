// ajfx-studio-cli: a scene's performance driven from the command line, with
// no window. Every hand Studio has (drop, paint and its brushes, grab and
// throw, poke, blast, deletion, re-recording) goes through the same calls
// the viewport uses (hands.hpp, performance.hpp), and the result is played
// on the CPU and checked to replay bit for bit, so what a performance does
// can be built and proved before there is a button for it.

#include "document_io.hpp"
#include "hands.hpp"
#include "performance.hpp"
#include "scene.hpp"
#include "stage.hpp"
#include "timeline.hpp"

#include <ajfx/rigid.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ajfx;
using namespace ajfx::studio;

constexpr const char* usage =
    "ajfx-studio-cli: acts on a scene's performance and replays it, without a window.\n"
    "\n"
    "  ajfx-studio-cli new <logs|wall|empty> <scene.ajfxstudio>\n"
    "      Writes a scene to act on: logs that smoulder (a Placed log kind with a\n"
    "      trail), a wall that breaks with a ball to throw at it, or just ground.\n"
    "  ajfx-studio-cli <scene> info           What can be acted on and what has been.\n"
    "  ajfx-studio-cli <scene> acts           Every act, one a line.\n"
    "  ajfx-studio-cli <scene> bodies <step>  The solid bodies where they stand at a step.\n"
    "  ajfx-studio-cli <scene> play <step>    Plays to a step: what stands, and a digest of it all.\n"
    "  ajfx-studio-cli <scene> verify <step>  Plays twice, and again after saving and\n"
    "                                         reopening the scene: the digests must agree.\n"
    "  ajfx-studio-cli <scene> run <script>   Does the hands a script lists, in order.\n"
    "  ajfx-studio-cli <scene> do <command...>  Does one hand, as a script line.\n"
    "\n"
    "Options: --out <file> writes the acted scene there rather than over the scene;\n"
    "         --seed <n> plays with that instance seed (default 0).\n"
    "\n"
    "Script lines (# starts a comment; points are x,y,z in metres; steps are\n"
    "simulation steps, 60 a second; a body is object:<uid>[:<count>] or drop:<serial>):\n"
    "  drop <step> <object> <point>\n"
    "  paint <step> <object> <point> [radius=0.5 count=3 falloff=0 turn=realistic|random|locked\n"
    "        normal=0,1,0 stroke=<serial>]\n"
    "  stroke <object> <from> <to> <point> <point> [every=6 and the paint options]\n"
    "        Dabs along the line from the first point to the second, every few steps.\n"
    "  erase|push|pull <step> <point> [radius=0.5 strength=3 falloff=0]\n"
    "  poke <step> <body> <direction> [speed=4 at=<point on the body>]\n"
    "  blast <step> <point> [radius=3 power=10]\n"
    "  grab <body> <from> <to> <point> [<point> ...] [camera=0,3,10]\n"
    "        Takes hold at <from>, carries it through the points evenly, lets go at <to>.\n"
    "  delete <step> <body>\n"
    "  remove <serial>      retime <serial> <step>      rerecord <step>      clear\n";

struct Failure final {
    std::string message;
};

[[noreturn]] void fail(std::string message) {
    throw Failure{std::move(message)};
}

std::vector<std::string> split(std::string_view text, char by) {
    std::vector<std::string> parts;
    std::size_t start = 0U;
    while (start <= text.size()) {
        const std::size_t end = text.find(by, start);
        parts.emplace_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1U;
    }
    return parts;
}

float to_float(std::string_view text) {
    float value = 0.0F;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        fail("not a number: " + std::string{text});
    }
    return value;
}

std::uint64_t to_u64(std::string_view text) {
    std::uint64_t value = 0U;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        fail("not a whole number: " + std::string{text});
    }
    return value;
}

std::uint32_t to_u32(std::string_view text) {
    const std::uint64_t value = to_u64(text);
    if (value > 0xffffffffULL) {
        fail("too large: " + std::string{text});
    }
    return static_cast<std::uint32_t>(value);
}

Vec3 to_vec3(std::string_view text) {
    const std::vector<std::string> parts = split(text, ',');
    if (parts.size() != 3U) {
        fail("not a point (x,y,z): " + std::string{text});
    }
    return Vec3{to_float(parts[0]), to_float(parts[1]), to_float(parts[2])};
}

BodyRef to_body(std::string_view text) {
    const std::vector<std::string> parts = split(text, ':');
    if (parts.size() == 2U && parts[0] == "drop") {
        return BodyRef{.object = 0U, .count = 0U, .drop = to_u32(parts[1])};
    }
    if ((parts.size() == 2U || parts.size() == 3U) && parts[0] == "object") {
        return BodyRef{.object = to_u32(parts[1]), .count = parts.size() == 3U ? to_u32(parts[2]) : 0U, .drop = 0U};
    }
    fail("not a body (object:<uid>[:<count>] or drop:<serial>): " + std::string{text});
}

std::string show(Vec3 v) {
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, "%.3f,%.3f,%.3f", v.x, v.y, v.z);
    return buffer;
}

std::string show(const BodyRef& ref) {
    return ref.drop != 0U ? "drop:" + std::to_string(ref.drop)
                          : "object:" + std::to_string(ref.object) + ":" + std::to_string(ref.count);
}

std::string_view kind_name(ActKind kind) {
    switch (kind) {
    case ActKind::Drop: return "drop";
    case ActKind::Delete: return "delete";
    case ActKind::Push: return "push";
    case ActKind::Blast: return "blast";
    case ActKind::Grab: return "grab";
    case ActKind::Paint: return "paint";
    }
    return "?";
}

// A script line's words: the positional ones, and the name=value options.
struct Words final {
    std::vector<std::string> args;
    std::map<std::string, std::string, std::less<>> options;

    [[nodiscard]] float number(std::string_view name, float fallback) const {
        const auto found = options.find(name);
        return found == options.end() ? fallback : to_float(found->second);
    }
    [[nodiscard]] std::optional<std::string> text(std::string_view name) const {
        const auto found = options.find(name);
        return found == options.end() ? std::nullopt : std::optional<std::string>{found->second};
    }
    void need(std::size_t count, std::string_view form) const {
        if (args.size() < count) {
            fail("expected: " + std::string{form});
        }
    }
};

Words words_of(const std::vector<std::string>& tokens) {
    Words words;
    for (const std::string& token : tokens) {
        const std::size_t equals = token.find('=');
        if (equals != std::string::npos && equals > 0U) {
            words.options[token.substr(0U, equals)] = token.substr(equals + 1U);
        } else {
            words.args.push_back(token);
        }
    }
    return words;
}

// The scene being acted on, and what acting on it needs.
struct Session final {
    Document document;
    Prepared prepared;
    std::unique_ptr<RigidSolver> solver = rigid::make_jolt_solver();
    std::uint32_t seed{0U};

    [[nodiscard]] Played played_to(std::uint64_t tick) const {
        Played played = play_scene(document, prepared, solver.get(), seed, tick);
        if (!played.error.empty()) {
            fail(played.error);
        }
        return played;
    }
};

void say(const std::string& line) {
    std::cout << line << '\n';
}

void refuse_unless(const std::string& refusal) {
    if (!refusal.empty()) {
        fail(refusal);
    }
}

Brush brush_of(const Words& words, BrushMode mode) {
    Brush brush;
    brush.mode = mode;
    brush.radius = words.number("radius", brush.radius);
    brush.count = static_cast<std::uint32_t>(std::lround(words.number("count", static_cast<float>(brush.count))));
    brush.falloff = words.number("falloff", brush.falloff);
    brush.strength = words.number("strength", brush.strength);
    if (const std::optional<std::string> turn = words.text("turn")) {
        if (*turn == "realistic") {
            brush.turn = PaintTurn::Realistic;
        } else if (*turn == "random") {
            brush.turn = PaintTurn::Random;
        } else if (*turn == "locked") {
            brush.turn = PaintTurn::Locked;
        } else {
            fail("turn is realistic, random or locked");
        }
    }
    return brush;
}

// One script line, done to the session's scene.
void act(Session& session, const std::vector<std::string>& tokens) {
    if (tokens.empty()) {
        return;
    }
    const std::string verb = tokens.front();
    const Words words = words_of(std::vector<std::string>{tokens.begin() + 1, tokens.end()});
    Document& document = session.document;
    Performance& performance = document.scene->performance;
    const ActTargets& targets = session.prepared.targets;

    if (verb == "drop") {
        words.need(3U, "drop <step> <object> <point>");
        const Uid object = to_u32(words.args[1]);
        const Vec3 at = to_vec3(words.args[2]);
        if (!droppable(targets, object)) {
            fail("object " + words.args[1] + " cannot be dropped: it has to be a Dynamic object");
        }
        const std::uint32_t serial = add_drop(performance, to_u64(words.args[0]), object, at,
                                              std::get<Object>(document.scene->item(object)->data).rotation, {});
        say("drop " + std::to_string(serial));
    } else if (verb == "paint") {
        words.need(3U, "paint <step> <object> <point> [options]");
        const Brush brush = brush_of(words, BrushMode::Paint);
        const Vec3 normal = words.text("normal") ? to_vec3(*words.text("normal")) : Vec3{0.0F, 1.0F, 0.0F};
        const auto stroke = words.text("stroke") ? to_u32(*words.text("stroke")) : 0U;
        const BrushDab dab = brush_dab(document, targets, brush, to_u32(words.args[1]), to_vec3(words.args[2]),
                                       normal, {}, to_u64(words.args[0]), stroke);
        refuse_unless(dab.refusal);
        say("paint " + std::to_string(dab.serials.front()) + " stroke " + std::to_string(dab.stroke) + " at step " +
            std::to_string(dab.tick));
    } else if (verb == "stroke") {
        words.need(5U, "stroke <object> <from> <to> <point> <point> [options]");
        const Uid object = to_u32(words.args[0]);
        const std::uint64_t from = to_u64(words.args[1]);
        const std::uint64_t to = to_u64(words.args[2]);
        const Vec3 a = to_vec3(words.args[3]);
        const Vec3 b = to_vec3(words.args[4]);
        const std::uint64_t every = std::max<std::uint64_t>(1U, to_u64(words.text("every").value_or("6")));
        const Brush brush = brush_of(words, BrushMode::Paint);
        const Vec3 normal = words.text("normal") ? to_vec3(*words.text("normal")) : Vec3{0.0F, 1.0F, 0.0F};
        if (to < from) {
            fail("a stroke ends after it starts");
        }
        std::uint32_t stroke = 0U;
        std::size_t dabs = 0U;
        for (std::uint64_t tick = from; tick <= to; tick += every) {
            const float t = to == from ? 0.0F : static_cast<float>(tick - from) / static_cast<float>(to - from);
            const BrushDab dab =
                brush_dab(document, targets, brush, object, a + (b - a) * t, normal, {}, tick, stroke);
            refuse_unless(dab.refusal);
            stroke = dab.stroke;
            ++dabs;
        }
        say("stroke " + std::to_string(stroke) + ": " + std::to_string(dabs) + " dabs");
    } else if (verb == "erase" || verb == "push" || verb == "pull") {
        words.need(2U, verb + " <step> <point> [options]");
        const std::uint64_t tick = to_u64(words.args[0]);
        const Brush brush =
            brush_of(words, verb == "erase" ? BrushMode::Erase : verb == "push" ? BrushMode::Push : BrushMode::Pull);
        const Vec3 normal = words.text("normal") ? to_vec3(*words.text("normal")) : Vec3{0.0F, 1.0F, 0.0F};
        const Played played = session.played_to(tick);
        const std::vector<StageBody> bodies = played_stage(played, session.prepared);
        const BrushDab dab = brush_dab(document, targets, brush, 0U, to_vec3(words.args[1]), normal, bodies, tick);
        refuse_unless(dab.refusal);
        say(verb + ": " + std::to_string(dab.serials.size()) + " bodies");
    } else if (verb == "poke") {
        words.need(3U, "poke <step> <body> <direction> [speed= at=]");
        const std::uint64_t tick = to_u64(words.args[0]);
        const BodyRef ref = to_body(words.args[1]);
        const Played played = session.played_to(tick);
        const std::optional<WorldBody> body = find_body(played, session.prepared, ref);
        if (!body) {
            fail("no " + show(ref) + " stands at step " + std::to_string(tick));
        }
        const StageHit hit{.point = words.text("at") ? to_vec3(*words.text("at")) : body->position,
                           .center = body->position,
                           .layer = 0U,
                           .block = 0U,
                           .on_body = true,
                           .spawn_index = body->spawn_index};
        const StageAct made = poke(document, targets, hit, to_vec3(words.args[2]), words.number("speed", poke_speed),
                                   body->size * 0.5F, tick);
        refuse_unless(made.refusal);
        say("push " + std::to_string(made.serial));
    } else if (verb == "blast") {
        words.need(2U, "blast <step> <point> [radius= power=]");
        const StageAct made = set_off_blast(document, targets, to_vec3(words.args[1]), words.number("radius", 3.0F),
                                            words.number("power", 10.0F), to_u64(words.args[0]));
        refuse_unless(made.refusal);
        say("blast " + std::to_string(made.serial));
    } else if (verb == "grab") {
        words.need(4U, "grab <body> <from> <to> <point> [<point> ...]");
        const BodyRef ref = to_body(words.args[0]);
        const std::uint64_t from = to_u64(words.args[1]);
        const std::uint64_t to = to_u64(words.args[2]);
        if (to <= from) {
            fail("a grab lets go after it takes hold");
        }
        std::vector<Vec3> points;
        for (std::size_t i = 3U; i < words.args.size(); ++i) {
            points.push_back(to_vec3(words.args[i]));
        }
        const Vec3 camera = words.text("camera") ? to_vec3(*words.text("camera")) : Vec3{0.0F, 3.0F, 10.0F};
        const Played played = session.played_to(from);
        const std::optional<WorldBody> body = find_body(played, session.prepared, ref);
        if (!body) {
            fail("no " + show(ref) + " stands at step " + std::to_string(from));
        }
        // Pressed on its middle, looking at it from the camera.
        const StageHit hit{.point = body->position,
                           .center = body->position,
                           .layer = 0U,
                           .block = 0U,
                           .on_body = true,
                           .spawn_index = body->spawn_index};
        GrabStart start = begin_grab(document, targets, hit, body->orientation, body->position - camera, from);
        refuse_unless(start.refusal);
        // Through the points evenly, a step at a time: the wheel takes the
        // plane to each point's depth, the pointer's ray goes through it.
        points.insert(points.begin(), body->position);
        for (std::uint64_t tick = from + 1U; tick <= to; ++tick) {
            const float along = static_cast<float>(tick - from) / static_cast<float>(to - from) *
                                static_cast<float>(points.size() - 1U);
            const std::size_t leg = std::min(static_cast<std::size_t>(along), points.size() - 2U);
            const Vec3 aim = points[leg] + (points[leg + 1U] - points[leg]) * (along - static_cast<float>(leg));
            push_deeper(start.hold, dot(aim - start.hold.plane.point, start.hold.plane.normal));
            const Vec3 ray = aim - camera;
            const float l = std::sqrt(dot(ray, ray));
            if (!carry(document, start.hold, camera, ray * (1.0F / l), tick)) {
                fail("the pointer missed the plane at step " + std::to_string(tick));
            }
        }
        say("grab " + std::to_string(start.hold.serial));
    } else if (verb == "delete") {
        words.need(2U, "delete <step> <body>");
        say("delete " + std::to_string(add_delete(performance, to_u64(words.args[0]), to_body(words.args[1]))));
    } else if (verb == "remove") {
        words.need(1U, "remove <serial>");
        const std::uint32_t serial = to_u32(words.args[0]);
        const Act* found = find_act(performance, serial);
        if (found == nullptr) {
            fail("no act " + words.args[0]);
        }
        if (found->kind == ActKind::Paint ? !remove_stroke(performance, found->stroke)
                                          : !remove_act(performance, serial)) {
            fail("no act " + words.args[0]);
        }
        say("removed " + words.args[0]);
    } else if (verb == "retime") {
        words.need(2U, "retime <serial> <step>");
        if (!retime_act(performance, to_u32(words.args[0]), to_u64(words.args[1]))) {
            fail("no act " + words.args[0]);
        }
        say("retimed " + words.args[0]);
    } else if (verb == "rerecord") {
        words.need(1U, "rerecord <step>");
        say("rerecord: " + std::to_string(rerecord_from(performance, to_u64(words.args[0]))) + " acts changed");
    } else if (verb == "clear") {
        performance.acts.clear();
        say("cleared");
    } else {
        fail("no such hand: " + verb);
    }
    if (const std::string wrong = validate_performance(*document.scene); !wrong.empty()) {
        fail("the performance is now invalid: " + wrong);
    }
}

std::vector<std::string> tokens_of(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream in{line.substr(0U, line.find('#'))};
    for (std::string token; in >> token;) {
        tokens.push_back(token);
    }
    return tokens;
}

void print_play(const Session& session, std::uint64_t tick) {
    const Played played = session.played_to(tick);
    const Simulation& simulation = *played.simulation;
    std::size_t pieces = 0U;
    const std::vector<WorldBody> bodies = world_bodies(played, session.prepared);
    for (const WorldBody& body : bodies) {
        pieces += body.ref ? 0U : 1U;
    }
    say("step " + std::to_string(simulation.tick()));
    if (played.world) {
        say("bodies " + std::to_string(bodies.size()) + " (pieces of broken things " + std::to_string(pieces) + ")");
    }
    for (std::size_t e = 0U; e < simulation.effect().emitters.size(); ++e) {
        const EmitterState& s = simulation.emitter(e);
        say("emitter " + simulation.effect().emitters[e].name + ": " + std::to_string(s.alive()) + " alive, " +
            std::to_string(s.spawned) + " born, " + std::to_string(s.refused) + " refused");
    }
    if (played.skipped != 0U) {
        say("acts this scene cannot apply: " + std::to_string(played.skipped));
    }
    char digest[32];
    std::snprintf(digest, sizeof digest, "%016llx", static_cast<unsigned long long>(state_digest(simulation)));
    say(std::string{"digest "} + digest);
}

Document make_scene(std::string_view kind) {
    Document document = new_scene();
    if (kind == "empty") {
        return document;
    }
    if (kind == "logs") {
        // A log that is only ever painted in, which smoulders: a trail of
        // smoke and embers behind it while it moves.
        const Uid log = add_object(document, "slab");
        SceneItem* item = document.scene->item(log);
        item->name = "Log";
        auto& object = std::get<Object>(item->data);
        object.distribution = Distribution::Placed;
        object.size = 0.6F;
        object.position = {0.0F, 0.3F, 0.0F};
        const Uid smoke = add_bound_system(document, RelationKind::TrailFrom, log);
        if (smoke != 0U) {
            auto& particles = std::get<ParticleSystem>(document.scene->item(smoke)->data);
            particles.look = ParticleLook::Smoke;
            particles.tint = {0.9F, 0.45F, 0.15F, 1.0F};
        }
        return document;
    }
    if (kind == "wall") {
        // A wall that breaks when hit hard, and a ball (a heavy cube) to
        // throw at it: pick the ball up, swing it, let go.
        const Uid wall = add_object(document);
        SceneItem* w = document.scene->item(wall);
        w->name = "Wall";
        auto& block = std::get<Object>(w->data);
        block.size = 1.6F;
        block.position = {0.0F, 0.8F, -2.0F};
        block.fracture = true;
        block.fragments = 8U;
        block.fracture_speed = 4.0F;
        const Uid ball = add_object(document);
        SceneItem* b = document.scene->item(ball);
        b->name = "Ball";
        auto& thrown = std::get<Object>(b->data);
        thrown.size = 0.4F;
        thrown.position = {0.0F, 0.2F, 2.0F};
        thrown.density = 3000.0F;
        return document;
    }
    fail("new makes logs, wall or empty");
}

void write_scene(const Document& document, const std::filesystem::path& path) {
    const std::string text = save_document(document);
    std::string error;
    if (!write_file(path, std::span{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, error)) {
        fail(error);
    }
}

Document read_scene(const std::filesystem::path& path) {
    std::string error;
    const std::optional<std::vector<std::uint8_t>> bytes = read_file(path, error);
    if (!bytes) {
        fail(error);
    }
    Loaded loaded = load_document(std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()});
    if (!loaded.document) {
        fail(path.string() + ": " + loaded.error);
    }
    return std::move(*loaded.document);
}

int run(std::vector<std::string> args) {
    std::optional<std::filesystem::path> out;
    std::uint32_t seed = 0U;
    std::vector<std::string> rest;
    for (std::size_t i = 0U; i < args.size(); ++i) {
        if (args[i] == "--out" && i + 1U < args.size()) {
            out = args[++i];
        } else if (args[i] == "--seed" && i + 1U < args.size()) {
            seed = to_u32(args[++i]);
        } else if (args[i] == "--help" || args[i] == "-h") {
            std::cout << usage;
            return 0;
        } else {
            rest.push_back(args[i]);
        }
    }
    if (rest.size() >= 3U && rest[0] == "new") {
        write_scene(make_scene(rest[1]), rest[2]);
        say("wrote " + rest[2]);
        return 0;
    }
    if (rest.size() < 2U) {
        std::cout << usage;
        return 2;
    }
    const std::filesystem::path path = rest[0];
    const std::string command = rest[1];
    Session session{.document = read_scene(path), .prepared = {}, .seed = seed};
    session.prepared = prepare_scene(session.document);
    if (!session.prepared.compiled) {
        fail(session.prepared.error);
    }
    const ActTargets& targets = session.prepared.targets;
    const Performance& performance = session.document.scene->performance;

    if (command == "info") {
        say("solid world: " + (targets.emitter.empty() ? std::string{"none"} : targets.emitter));
        say("most dropped and painted at once: " + std::to_string(session.document.scene->physics.max_dropped) +
            " (" + std::to_string(dropped_count(performance)) + " held)");
        for (const SceneItem& item : session.document.scene->items) {
            if (const auto* object = std::get_if<Object>(&item.data)) {
                say("object " + std::to_string(item.uid) + " " + item.name +
                    (droppable(targets, item.uid) ? " (can be dropped and painted)" : "") +
                    (object->distribution == Distribution::Placed ? " [placed only]" : ""));
            }
        }
        say("performance: " + (performance.acts.empty() ? std::string{"empty"} : performance_summary(performance)));
        return 0;
    }
    if (command == "acts") {
        for (const Act& a : performance.acts) {
            std::string line = std::to_string(a.serial) + " " + std::string{kind_name(a.kind)} + " step " +
                               std::to_string(a.tick);
            switch (a.kind) {
            case ActKind::Drop: line += " object " + std::to_string(a.object) + " at " + show(a.position); break;
            case ActKind::Paint:
                line += " object " + std::to_string(a.object) + " x" + std::to_string(a.count) + " at " +
                        show(a.position) + " stroke " + std::to_string(a.stroke);
                break;
            case ActKind::Blast:
                line += " at " + show(a.position) + " radius " + std::to_string(a.radius) + " power " +
                        std::to_string(a.power);
                break;
            case ActKind::Grab:
                line += " " + show(a.target) + " until step " + std::to_string(act_end(a)) + " (" +
                        std::to_string(a.path.size()) + " samples) to " + show(a.path.back().point);
                break;
            case ActKind::Push: line += " " + show(a.target) + " by " + show(a.push); break;
            case ActKind::Delete: line += " " + show(a.target); break;
            }
            say(line);
        }
        return 0;
    }
    if (command == "bodies" && rest.size() >= 3U) {
        const Played played = session.played_to(to_u64(rest[2]));
        for (const WorldBody& body : world_bodies(played, session.prepared)) {
            say((body.ref ? show(*body.ref) : std::string{"piece"}) + " at " + show(body.position) + " moving " +
                show(body.velocity) + " size " + std::to_string(body.size));
        }
        return 0;
    }
    if (command == "play" && rest.size() >= 3U) {
        print_play(session, to_u64(rest[2]));
        return 0;
    }
    if (command == "verify" && rest.size() >= 3U) {
        const std::uint64_t tick = to_u64(rest[2]);
        const std::uint64_t first = state_digest(*session.played_to(tick).simulation);
        const std::uint64_t again = state_digest(*session.played_to(tick).simulation);
        Session reopened{.document = {}, .prepared = {}, .seed = seed};
        Loaded loaded = load_document(save_document(session.document));
        if (!loaded.document) {
            fail("the scene does not reopen: " + loaded.error);
        }
        reopened.document = std::move(*loaded.document);
        reopened.prepared = prepare_scene(reopened.document);
        const std::uint64_t saved = state_digest(*reopened.played_to(tick).simulation);
        char line[128];
        std::snprintf(line, sizeof line, "%016llx %016llx %016llx", static_cast<unsigned long long>(first),
                      static_cast<unsigned long long>(again), static_cast<unsigned long long>(saved));
        say(line);
        if (first != again || first != saved) {
            say("DIFFERENT: the performance does not replay exactly");
            return 1;
        }
        say("identical: replays and the reopened scene agree bit for bit");
        return 0;
    }
    if (command == "run" || command == "do") {
        if (command == "run") {
            if (rest.size() < 3U) {
                fail("run <script>");
            }
            std::ifstream script{rest[2]};
            if (!script) {
                fail("cannot read " + rest[2]);
            }
            std::size_t number = 0U;
            for (std::string line; std::getline(script, line);) {
                ++number;
                try {
                    act(session, tokens_of(line));
                } catch (const Failure& failure) {
                    fail(rest[2] + ":" + std::to_string(number) + ": " + failure.message);
                }
            }
        } else {
            act(session, std::vector<std::string>{rest.begin() + 2, rest.end()});
        }
        write_scene(session.document, out.value_or(path));
        say("performance: " + performance_summary(session.document.scene->performance));
        return 0;
    }
    std::cout << usage;
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(std::vector<std::string>{argv + 1, argv + argc});
    } catch (const Failure& failure) {
        std::cerr << "ajfx-studio-cli: " << failure.message << '\n';
        return 1;
    }
}
