#include "guides.hpp"

#include "catalog.hpp"

#include <ajfx/noise.hpp>
#include <ajfx/random.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ajfx::studio {
namespace {

constexpr float pi = std::numbers::pi_v<float>;
constexpr int circle_segments = 48;
// How far a launch cone reaches: the distance its average particle covers
// in this many seconds, so a fast launch draws a long cone.
constexpr float launch_seconds = 0.25F;

Vec3 to_vec3(const Value& v) {
    return Vec3{v.x, v.y, v.z};
}

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v, Vec3 fallback) {
    const float l = length(v);
    return l > 1.0e-6F ? v * (1.0F / l) : fallback;
}

// Two unit vectors at right angles to `axis` and to each other.
std::pair<Vec3, Vec3> basis(Vec3 axis) {
    const Vec3 helper = std::fabs(axis.y) < 0.99F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
    const Vec3 a = normalized(cross(helper, axis), Vec3{1.0F, 0.0F, 0.0F});
    return {a, cross(axis, a)};
}

GuideStroke circle(Vec3 centre, Vec3 u, Vec3 v, float radius, bool faint = false) {
    GuideStroke stroke{.closed = true, .faint = faint};
    for (int i = 0; i < circle_segments; ++i) {
        const float angle = 2.0F * pi * static_cast<float>(i) / static_cast<float>(circle_segments);
        stroke.points.push_back(centre + u * (std::cos(angle) * radius) + v * (std::sin(angle) * radius));
    }
    return stroke;
}

GuideStroke line(Vec3 a, Vec3 b, bool faint = false, bool arrow = false) {
    return GuideStroke{.points = {a, b}, .closed = false, .faint = faint, .arrow = arrow};
}

// A ball's outline: its three great circles.
void ball(Guide& guide, Vec3 centre, float radius, bool faint) {
    const Vec3 x{1.0F, 0.0F, 0.0F};
    const Vec3 y{0.0F, 1.0F, 0.0F};
    const Vec3 z{0.0F, 0.0F, 1.0F};
    guide.strokes.push_back(circle(centre, x, z, radius, faint));
    guide.strokes.push_back(circle(centre, x, y, radius, faint));
    guide.strokes.push_back(circle(centre, z, y, radius, faint));
}

void box(Guide& guide, Vec3 centre, Vec3 half) {
    const auto corner = [&](int i) {
        return centre + Vec3{(i & 1) != 0 ? half.x : -half.x, (i & 2) != 0 ? half.y : -half.y,
                             (i & 4) != 0 ? half.z : -half.z};
    };
    for (int i = 0; i < 8; ++i) {
        for (const int bit : {1, 2, 4}) {
            if ((i & bit) == 0) {
                guide.strokes.push_back(line(corner(i), corner(i | bit)));
            }
        }
    }
}

// A box standing as `turn` leaves it, for a prop that is turned where it
// stands. Drawn faint: it is the shape the pieces hit, over the model.
void turned_box(Guide& guide, Vec3 centre, Vec3 half, Quat turn) {
    const auto corner = [&](int i) {
        return centre + rotate(turn, Vec3{(i & 1) != 0 ? half.x : -half.x, (i & 2) != 0 ? half.y : -half.y,
                                          (i & 4) != 0 ? half.z : -half.z});
    };
    for (int i = 0; i < 8; ++i) {
        for (const int bit : {1, 2, 4}) {
            if ((i & bit) == 0) {
                guide.strokes.push_back(line(corner(i), corner(i | bit), true));
            }
        }
    }
}

// An upright round shape's outline: a circle at each end and four lines down
// the side, turned as the prop is.
void turned_tube(Guide& guide, Vec3 centre, float radius, float height, Quat turn) {
    const Vec3 axis = rotate(turn, Vec3{0.0F, 1.0F, 0.0F});
    const Vec3 x = rotate(turn, Vec3{1.0F, 0.0F, 0.0F});
    const Vec3 z = rotate(turn, Vec3{0.0F, 0.0F, 1.0F});
    const Vec3 top = centre + axis * (height * 0.5F);
    const Vec3 bottom = centre - axis * (height * 0.5F);
    guide.strokes.push_back(circle(top, x, z, radius, true));
    guide.strokes.push_back(circle(bottom, x, z, radius, true));
    for (const Vec3& out : {x, z, x * -1.0F, z * -1.0F}) {
        guide.strokes.push_back(line(bottom + out * radius, top + out * radius, true));
    }
}

// A drop line from a point in the air to the floor beneath it, so its
// height reads in a perspective view.
void drop(Guide& guide, Vec3 point) {
    if (std::fabs(point.y) > 0.02F) {
        guide.strokes.push_back(line(point, Vec3{point.x, 0.0F, point.z}, true));
    }
}

void arrow(Guide& guide, Vec3 from, Vec3 to) {
    if (length(to - from) > 1.0e-4F) {
        guide.strokes.push_back(line(from, to, false, true));
    }
}

const Block* first(const Layer& layer, std::string_view kind) {
    for (const Block& block : layer.blocks) {
        if (block.enabled && block.kind == kind) {
            return &block;
        }
    }
    return nullptr;
}

bool plays(const Document& document, const Layer& layer) {
    const bool soloing = std::any_of(document.layers.begin(), document.layers.end(),
                                     [](const Layer& l) { return l.solo && l.enabled; });
    return layer.enabled && (!soloing || layer.solo);
}

class Builder final {
public:
    Builder(const Document& document, const GuideOptions& options, GuideSet& out)
        : options_{options}, out_{out}, s_{document.scale} {}

    void layer(const Layer& layer) {
        const Block* shape = first(layer, "shape");
        // Where the layer's particles start: the middle of its spawn shape.
        origin_ = shape != nullptr ? to_vec3(get(*shape, "offset")) * s_ : Vec3{};
        for (const Block& block : layer.blocks) {
            if (!block.enabled) {
                continue;
            }
            Guide guide{.layer = layer.uid, .block = block.uid, .focused = block.uid == options_.focus};
            focused_ = guide.focused;
            // Out of focus, only where things are - spawn shapes and
            // colliders - so the stage still reads; what a block does
            // (pushes, cones, fields) shows while it is worked on.
            if (!focused_ && !always_shown(block)) {
                continue;
            }
            add(block, guide);
            if (!guide.strokes.empty()) {
                out_.guides.push_back(std::move(guide));
            }
        }
    }

private:
    static bool always_shown(const Block& block) {
        const std::string_view kind = block.kind;
        // An object is scenery: it reads as part of the stage whether or
        // not it is the block being worked on.
        return kind == "shape" || kind == "sphere" || kind == "box" || kind == "mesh_collider" ||
               kind == "object" || (kind == "ground" && number_of(block, "height") != 0.0F);
    }

    void handle(const Block& block, std::string control, HandleMotion motion, Vec3 at, Vec3 origin, std::string label,
                int end = 1) {
        if (!focused_) {
            return;
        }
        out_.handles.push_back(Handle{.block = block.uid,
                                      .control = std::move(control),
                                      .motion = motion,
                                      .end = end,
                                      .at = at,
                                      .origin = origin,
                                      .scale = s_,
                                      .label = std::move(label)});
    }

    void add(const Block& block, Guide& guide) {
        const std::string_view kind = block.kind;
        const Vec3 up{0.0F, 1.0F, 0.0F};
        if (kind == "shape") {
            guide.kind = GuideKind::Shape;
            const std::string shape = choice_of(block, "shape");
            const Vec3 centre = to_vec3(get(block, "offset")) * s_;
            const Value radius = get(block, "size");
            const float low = std::max(radius.x, 0.0F) * s_;
            const float high = std::max(radius.y, 0.0F) * s_;
            const Vec3 x{1.0F, 0.0F, 0.0F};
            const Vec3 z{0.0F, 0.0F, 1.0F};
            if (shape == "point") {
                const float r = 0.04F;
                guide.strokes.push_back(line(centre - x * r, centre + x * r));
                guide.strokes.push_back(line(centre - z * r, centre + z * r));
                guide.strokes.push_back(line(centre - up * r, centre + up * r));
            } else if (shape == "box") {
                const Value size = get(block, "box");
                const Vec3 half = Vec3{std::fabs(size.x), std::fabs(size.y), std::fabs(size.z)} * (0.5F * s_);
                box(guide, centre, half);
                handle(block, "box", HandleMotion::Corner, centre + half, centre, "Size");
            } else {
                const bool flat = shape == "disc" || shape == "ring";
                if (flat) {
                    guide.strokes.push_back(circle(centre, x, z, high));
                    if (low < high) {
                        guide.strokes.push_back(circle(centre, x, z, low, true));
                    }
                } else {
                    ball(guide, centre, high, false);
                    if (low < high) {
                        guide.strokes.push_back(circle(centre, x, z, low, true));
                    }
                }
                // Both ends of the radius, either side: pulling the low end in
                // from the high one softens the edge.
                handle(block, "size", HandleMotion::Radius, centre + x * high, centre, "Radius, high end", 1);
                handle(block, "size", HandleMotion::Radius, centre - x * low, centre, "Radius, low end", 0);
            }
            drop(guide, centre);
            handle(block, "offset", HandleMotion::Floor, centre, centre, "Offset");
        } else if (kind == "launch") {
            guide.kind = GuideKind::Shape;
            const std::string direction = choice_of(block, "direction");
            const Value speed = get(block, "speed");
            const float reach = std::max((speed.x + speed.y) * 0.5F * launch_seconds * s_, 0.12F);
            if (direction == "out") {
                for (const Vec3 d : {Vec3{1.0F, 0.0F, 0.0F}, Vec3{-1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F},
                                     Vec3{0.0F, -1.0F, 0.0F}, Vec3{0.0F, 0.0F, 1.0F}, Vec3{0.0F, 0.0F, -1.0F}}) {
                    arrow(guide, origin_ + d * (reach * 0.25F), origin_ + d * reach);
                }
                return;
            }
            const Vec3 axis = direction == "down"     ? Vec3{0.0F, -1.0F, 0.0F}
                              : direction == "side"   ? Vec3{1.0F, 0.0F, 0.0F}
                              : direction == "custom" ? normalized(to_vec3(get(block, "custom")), up)
                                                      : up;
            arrow(guide, origin_, origin_ + axis * reach);
            const auto [u, v] = basis(axis);
            const Value spread = get(block, "spread");
            const auto cone = [&](float degrees, bool faint) {
                const float angle = std::clamp(degrees, 0.0F, 180.0F) * pi / 180.0F;
                const Vec3 centre = origin_ + axis * (std::cos(angle) * reach);
                const float radius = std::sin(angle) * reach;
                if (radius > 1.0e-3F) {
                    guide.strokes.push_back(circle(centre, u, v, radius, faint));
                }
                for (int i = 0; i < 4; ++i) {
                    const float around = pi * 0.5F * static_cast<float>(i);
                    const Vec3 rim = centre + u * (std::cos(around) * radius) + v * (std::sin(around) * radius);
                    guide.strokes.push_back(line(origin_, rim, true));
                }
            };
            cone(spread.y, false);
            if (spread.x > 0.5F) {
                cone(spread.x, true);
            }
        } else if (kind == "gravity" || kind == "rise") {
            guide.kind = GuideKind::Force;
            const Value strength = get(block, "strength");
            // Gravity draws half a metre a g; lift a tenth of a metre per m/s^2.
            const float per = kind == "gravity" ? -0.5F : 0.1F;
            const Vec3 start = origin_ + Vec3{0.25F * s_, 0.0F, 0.0F};
            arrow(guide, start, start + up * (strength.y * per * s_));
            if (strength.x < strength.y) {
                guide.strokes.push_back(line(start, start + up * (strength.x * per * s_), true));
            }
        } else if (kind == "wind") {
            guide.kind = GuideKind::Force;
            const Value strength = get(block, "strength");
            const float heading = number_of(block, "heading") * pi / 180.0F;
            const Vec3 along{std::cos(heading), 0.0F, -std::sin(heading)};
            const Vec3 start = origin_ + up * (0.3F * s_);
            const float reach = std::max(strength.y * 0.15F, 0.2F) * s_;
            arrow(guide, start, start + along * reach);
            handle(block, "heading", HandleMotion::Heading, start + along * reach, start, "Heading");
        } else if (kind == "turbulence") {
            guide.kind = GuideKind::Field;
            if (focused_) {
                field(block, guide);
            }
        } else if (kind == "swirl") {
            guide.kind = GuideKind::Force;
            const Vec3 centre = to_vec3(get(block, "center")) * s_;
            const bool horizontal = choice_of(block, "axis") == "horizontal";
            const Vec3 axis = horizontal ? Vec3{1.0F, 0.0F, 0.0F} : up;
            const float half = 1.0F * s_;
            guide.strokes.push_back(line(centre - axis * half, centre + axis * half, true));
            const auto [u, v] = basis(axis);
            const float radius = 0.3F * s_;
            const float sign = get(block, "strength").y < 0.0F ? -1.0F : 1.0F;
            // A ring round the axis, with arrows the way particles turn
            // (counter-clockwise looking down it for positive strength).
            for (int i = 0; i < 3; ++i) {
                GuideStroke turn;
                for (int k = 0; k <= 10; ++k) {
                    const float angle = sign * (2.0F * pi * (static_cast<float>(i) + static_cast<float>(k) * 0.07F) / 3.0F);
                    turn.points.push_back(centre + u * (std::cos(angle) * radius) + v * (std::sin(angle) * radius));
                }
                turn.arrow = true;
                guide.strokes.push_back(std::move(turn));
            }
            drop(guide, centre);
            handle(block, "center", HandleMotion::Floor, centre, centre, "Center");
        } else if (kind == "draw_in") {
            guide.kind = GuideKind::Force;
            const Vec3 centre = to_vec3(get(block, "center")) * s_;
            const float height = number_of(block, "height") * s_;
            const float top = height > 0.0F ? height : 1.5F * s_;
            guide.strokes.push_back(line(centre, centre + up * top, height <= 0.0F));
            const float radius = 0.3F * s_;
            const Vec3 x{1.0F, 0.0F, 0.0F};
            const Vec3 z{0.0F, 0.0F, 1.0F};
            guide.strokes.push_back(circle(centre, x, z, radius, true));
            for (const Vec3 d : {x, x * -1.0F, z, z * -1.0F}) {
                arrow(guide, centre + d * radius, centre + d * (radius * 0.45F));
            }
            if (height > 0.0F) {
                guide.strokes.push_back(circle(centre + up * height, x, z, radius * 0.25F, true));
                handle(block, "height", HandleMotion::Height, centre + up * height, centre, "Fades by");
            }
            handle(block, "center", HandleMotion::Floor, centre, centre, "Center");
        } else if (kind == "attract") {
            guide.kind = GuideKind::Force;
            const Vec3 point = to_vec3(get(block, "point")) * s_;
            const float reach = number_of(block, "reach") * s_;
            const float push = get(block, "strength").y < 0.0F ? -1.0F : 1.0F;
            const float inner = 0.08F * s_;
            const float outer = 0.25F * s_;
            for (const Vec3 d : {Vec3{1.0F, 0.0F, 0.0F}, Vec3{-1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F},
                                 Vec3{0.0F, -1.0F, 0.0F}, Vec3{0.0F, 0.0F, 1.0F}, Vec3{0.0F, 0.0F, -1.0F}}) {
                if (push > 0.0F) {
                    arrow(guide, point + d * outer, point + d * inner);
                } else {
                    arrow(guide, point + d * inner, point + d * outer);
                }
            }
            if (reach > 0.0F) {
                ball(guide, point, reach, true);
                handle(block, "reach", HandleMotion::Radius, point + Vec3{reach, 0.0F, 0.0F}, point, "Reach");
            }
            drop(guide, point);
            handle(block, "point", HandleMotion::Floor, point, point, "Point");
        } else if (kind == "ground") {
            guide.kind = GuideKind::Collider;
            const float height = number_of(block, "height") * s_;
            const float half = 1.5F * s_;
            const Vec3 x{1.0F, 0.0F, 0.0F};
            const Vec3 z{0.0F, 0.0F, 1.0F};
            const Vec3 centre{0.0F, height, 0.0F};
            GuideStroke edge{.closed = true};
            edge.points = {centre + (x + z) * half, centre + (x - z) * half, centre - (x + z) * half,
                           centre - (x - z) * half};
            guide.strokes.push_back(std::move(edge));
            guide.strokes.push_back(line(centre - x * half, centre + x * half, true));
            guide.strokes.push_back(line(centre - z * half, centre + z * half, true));
            handle(block, "height", HandleMotion::Height, centre, Vec3{}, "Height");
        } else if (kind == "sphere") {
            guide.kind = GuideKind::Collider;
            const Vec3 centre = to_vec3(get(block, "center")) * s_;
            const float radius = number_of(block, "radius") * s_;
            ball(guide, centre, radius, false);
            drop(guide, centre);
            handle(block, "radius", HandleMotion::Radius, centre + Vec3{radius, 0.0F, 0.0F}, centre, "Radius");
            handle(block, "center", HandleMotion::Floor, centre, centre, "Center");
        } else if (kind == "box") {
            guide.kind = GuideKind::Collider;
            const Vec3 centre = to_vec3(get(block, "center")) * s_;
            const Value size = get(block, "size");
            const Vec3 half = Vec3{std::fabs(size.x), std::fabs(size.y), std::fabs(size.z)} * (0.5F * s_);
            box(guide, centre, half);
            handle(block, "size", HandleMotion::Corner, centre + half, centre, "Size");
            handle(block, "center", HandleMotion::Floor, centre, centre, "Center");
        } else if (kind == "mesh_collider") {
            guide.kind = GuideKind::Collider;
            const Vec3 at = to_vec3(get(block, "position")) * s_;
            const float scale = std::max(number_of(block, "scale"), 0.001F) * s_;
            const std::string path = file_of(block, "file");
            const std::vector<Vec3>* outline = nullptr;
            if (options_.outlines != nullptr) {
                if (const auto found = options_.outlines->find(path); found != options_.outlines->end()) {
                    outline = &found->second;
                }
            }
            if (outline != nullptr && !outline->empty()) {
                for (std::size_t i = 0U; i + 1U < outline->size(); i += 2U) {
                    guide.strokes.push_back(line(at + (*outline)[i] * scale, at + (*outline)[i + 1U] * scale));
                }
            } else {
                const float r = 0.08F * s_;
                guide.strokes.push_back(line(at - Vec3{r, 0.0F, 0.0F}, at + Vec3{r, 0.0F, 0.0F}));
                guide.strokes.push_back(line(at - Vec3{0.0F, 0.0F, r}, at + Vec3{0.0F, 0.0F, r}));
                guide.strokes.push_back(line(at, at + up * (r * 2.0F)));
            }
            drop(guide, at);
            handle(block, "position", HandleMotion::Floor, at, at, "Position");
        } else if (kind == "object") {
            guide.kind = GuideKind::Collider;
            const Vec3 at = to_vec3(get(block, "at")) * s_;
            const Quat turn = prop_orientation(to_vec3(get(block, "turn")));
            const float size = std::max(number_of(block, "size"), 0.001F) * s_;
            // The model, where one is picked: what the audience will see,
            // placed and turned exactly as the prop is.
            const std::string path = file_of(block, "model_file");
            const std::vector<Vec3>* outline = nullptr;
            if (options_.outlines != nullptr) {
                if (const auto found = options_.outlines->find(path); found != options_.outlines->end()) {
                    outline = &found->second;
                }
            }
            if (outline != nullptr && !outline->empty()) {
                for (std::size_t i = 0U; i + 1U < outline->size(); i += 2U) {
                    guide.strokes.push_back(line(at + rotate(turn, (*outline)[i] * size),
                                                 at + rotate(turn, (*outline)[i + 1U] * size)));
                }
            } else {
                const float r = 0.08F * s_;
                guide.strokes.push_back(line(at - Vec3{r, 0.0F, 0.0F}, at + Vec3{r, 0.0F, 0.0F}));
                guide.strokes.push_back(line(at - Vec3{0.0F, 0.0F, r}, at + Vec3{0.0F, 0.0F, r}));
                guide.strokes.push_back(line(at, at + up * (r * 2.0F)));
            }
            // And, when it does not collide as its model, the shape it does
            // collide as, faintly over the top: that difference is the one
            // thing about an object an artist cannot otherwise see. Only for
            // one on its own, since a heap's shapes are not at its base.
            const std::string shape = choice_of(block, "arrange") == "one" ? choice_of(block, "collides")
                                                                           : std::string{};
            if (shape == "bucket" || shape == "tube") {
                turned_tube(guide, at, number_of(block, "radius") * s_, number_of(block, "height") * s_, turn);
            } else if (shape == "ball") {
                ball(guide, at, number_of(block, "radius") * s_, true);
            } else if (shape == "crate" || shape == "block") {
                turned_box(guide, at, to_vec3(get(block, "extent")) * (0.5F * s_), turn);
            }
            // Where a driven object is headed, so its move reads at a glance.
            if (choice_of(block, "motion") == "driven") {
                const Value* to = block.find("move_to");
                const Vec3 ends = to != nullptr ? to_vec3(*to) * s_ : at;
                if (length(ends - at) > 0.01F) {
                    guide.strokes.push_back(line(at, ends, true, true));
                }
            }
            drop(guide, at);
            handle(block, "at", HandleMotion::Floor, at, at, "Position");
            if (shape == "bucket" || shape == "tube" || shape == "ball") {
                const float radius = number_of(block, "radius") * s_;
                handle(block, "radius", HandleMotion::Radius, at + Vec3{radius, 0.0F, 0.0F}, at, "Radius");
            }
        }
    }

    // The turbulence field's flow, as short streamlines through where the
    // layer's particles go, drifted and churned to the preview's time: their
    // curl shows the swirl size, and they move as the field does.
    void field(const Block& block, Guide& guide) {
        const float size = std::max(number_of(block, "size"), 0.01F) * s_;
        const float frequency = 1.0F / size;
        const float t = options_.seconds;
        const Vec3 drift{0.0F, number_of(block, "roll") * s_ * t, 0.0F};
        const float scroll = t * number_of(block, "churn") * s_;
        const std::uint32_t seed = hash_mix(block.uid, static_cast<std::uint32_t>(number_of(block, "seed")));
        Vec3 low = origin_ - Vec3{size, size * 0.5F, size};
        Vec3 high = origin_ + Vec3{size, size * 2.0F, size};
        if (options_.bounds) {
            low = options_.bounds->first;
            high = options_.bounds->second;
        }
        constexpr int across = 4;
        constexpr int steps = 10;
        const float step = size * 0.08F;
        for (int i = 0; i < across; ++i) {
            for (int j = 0; j < across; ++j) {
                for (int k = 0; k < across; ++k) {
                    const auto at = [](float a, float b, int n) {
                        return a + (b - a) * ((static_cast<float>(n) + 0.5F) / static_cast<float>(across));
                    };
                    Vec3 p{at(low.x, high.x, i), at(low.y, high.y, j), at(low.z, high.z, k)};
                    GuideStroke flow{.arrow = true};
                    flow.points.push_back(p);
                    for (int n = 0; n < steps; ++n) {
                        const Vec3 q = (p - drift) * frequency + Vec3{0.0F, 0.0F, scroll};
                        p += normalized(curl_noise(q, seed), Vec3{}) * step;
                        flow.points.push_back(p);
                    }
                    guide.strokes.push_back(std::move(flow));
                }
            }
        }
    }

    const GuideOptions& options_;
    GuideSet& out_;
    float s_;
    Vec3 origin_{};
    bool focused_{false};
};

} // namespace

Uid working_layer(const Document& document, const GuideOptions& options) {
    if (options.layer != 0U && document.layer(options.layer) != nullptr) {
        return options.layer;
    }
    for (const Layer& layer : document.layers) {
        for (const Block& block : layer.blocks) {
            if (options.focus != 0U && block.uid == options.focus) {
                return layer.uid;
            }
        }
    }
    return 0U;
}

GuideSet build_guides(const Document& document, const GuideOptions& options) {
    GuideSet set;
    const Uid working = working_layer(document, options);
    if (working == 0U) {
        return set;
    }
    Builder builder{document, options, set};
    for (const Layer& layer : document.layers) {
        if (layer.uid == working && plays(document, layer)) {
            builder.layer(layer);
        }
    }
    return set;
}

std::optional<Value> handle_value(const Document& document, const Handle& handle, Vec3 world) {
    const Block* block = document.block(handle.block);
    const BlockDef* def = block != nullptr ? find_block_def(block->kind) : nullptr;
    const ControlDef* control = def != nullptr ? find_control(*def, handle.control) : nullptr;
    if (control == nullptr) {
        return std::nullopt;
    }
    const float s = handle.scale > 1.0e-6F ? handle.scale : 1.0F;
    Value value = value_of(*block, *control);
    switch (handle.motion) {
    case HandleMotion::Floor:
        value = vec3(world.x / s, world.y / s, world.z / s);
        break;
    case HandleMotion::Height:
        value = number((world.y - handle.origin.y) / s);
        break;
    case HandleMotion::Radius: {
        const float r = length(world - handle.origin) / s;
        if (control->type == ControlType::Range) {
            (handle.end == 0 ? value.x : value.y) = r;
            // The end dragged past the other takes it along.
            if (handle.end == 0) {
                value.y = std::max(value.y, r);
            } else {
                value.x = std::min(value.x, r);
            }
        } else {
            value = number(r);
        }
        break;
    }
    case HandleMotion::Heading: {
        const Vec3 d = world - handle.origin;
        float degrees = std::atan2(-d.z, d.x) * 180.0F / pi;
        if (degrees < 0.0F) {
            degrees += 360.0F;
        }
        value = number(std::round(degrees));
        break;
    }
    case HandleMotion::Corner: {
        const Vec3 d = world - handle.origin;
        value = vec3(std::fabs(d.x) * 2.0F / s, std::fabs(d.y) * 2.0F / s, std::fabs(d.z) * 2.0F / s);
        break;
    }
    }
    return sanitize(*control, std::move(value));
}

} // namespace ajfx::studio
