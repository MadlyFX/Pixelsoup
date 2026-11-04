#include "guides.hpp"

#include "library.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ajfx::studio {
namespace {

constexpr float pi = std::numbers::pi_v<float>;
constexpr int circle_segments = 48;
const Vec3 x_axis{1.0F, 0.0F, 0.0F};
const Vec3 up{0.0F, 1.0F, 0.0F};
const Vec3 z_axis{0.0F, 0.0F, 1.0F};
const Vec3 six_ways[6] = {Vec3{1.0F, 0.0F, 0.0F},  Vec3{-1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F},
                          Vec3{0.0F, -1.0F, 0.0F}, Vec3{0.0F, 0.0F, 1.0F},  Vec3{0.0F, 0.0F, -1.0F}};

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v, Vec3 fallback) {
    const float l = length(v);
    return l > 1.0e-6F ? v * (1.0F / l) : fallback;
}

// Two unit vectors at right angles to `axis` and to each other.
std::pair<Vec3, Vec3> basis(Vec3 axis) {
    const Vec3 helper = std::fabs(axis.y) < 0.99F ? up : x_axis;
    const Vec3 a = normalized(cross(helper, axis), x_axis);
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
    guide.strokes.push_back(circle(centre, x_axis, z_axis, radius, faint));
    guide.strokes.push_back(circle(centre, x_axis, up, radius, faint));
    guide.strokes.push_back(circle(centre, z_axis, up, radius, faint));
}

// A box standing as `turn` leaves it.
void box(Guide& guide, Vec3 centre, Vec3 half, Quat turn, bool faint) {
    const auto corner = [&](int i) {
        return centre + rotate(turn, Vec3{(i & 1) != 0 ? half.x : -half.x, (i & 2) != 0 ? half.y : -half.y,
                                          (i & 4) != 0 ? half.z : -half.z});
    };
    for (int i = 0; i < 8; ++i) {
        for (const int bit : {1, 2, 4}) {
            if ((i & bit) == 0) {
                guide.strokes.push_back(line(corner(i), corner(i | bit), faint));
            }
        }
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

// A small cross where something stands whose shape is not known yet.
void marker(Guide& guide, Vec3 at, float s) {
    const float r = 0.08F * s;
    guide.strokes.push_back(line(at - x_axis * r, at + x_axis * r));
    guide.strokes.push_back(line(at - z_axis * r, at + z_axis * r));
    guide.strokes.push_back(line(at, at + up * (r * 2.0F)));
}

// The bounds of a library shape in its own space; a unit box for an
// artist's model.
std::pair<Vec3, Vec3> shape_bounds(const std::string& mesh) {
    Vec3 low{-0.5F, -0.5F, -0.5F};
    Vec3 high{0.5F, 0.5F, 0.5F};
    if (const MeshData* primitive = library_mesh(mesh); primitive != nullptr && !primitive->vertices.empty()) {
        low = high = primitive->vertices.front().position;
        for (const auto& vertex : primitive->vertices) {
            const Vec3 v = vertex.position;
            low = Vec3{std::min(low.x, v.x), std::min(low.y, v.y), std::min(low.z, v.z)};
            high = Vec3{std::max(high.x, v.x), std::max(high.y, v.y), std::max(high.z, v.z)};
        }
    }
    return {low, high};
}

class Builder final {
public:
    Builder(const Document& document, const GuideOptions& options, GuideSet& out)
        : scene_{*document.scene}, options_{options}, out_{out}, s_{document.scale} {}

    void item(const SceneItem& item) {
        if (const auto* object = std::get_if<Object>(&item.data)) {
            add_object(item, *object);
        } else if (const auto* system = std::get_if<ParticleSystem>(&item.data)) {
            add_system(item, *system, true);
        } else if (const auto* force = std::get_if<Force>(&item.data)) {
            add_force(item, *force);
        } else if (const auto* grid = std::get_if<SmokeGrid>(&item.data)) {
            add_grid(item, *grid);
        }
    }

private:
    void keep(Guide guide) {
        if (!guide.strokes.empty()) {
            out_.guides.push_back(std::move(guide));
        }
    }

    void handle(Uid item, HandleTarget target, HandleMotion motion, Vec3 at, Vec3 origin, std::string label,
                int end = 1) {
        out_.handles.push_back(Handle{.item = item,
                                      .target = target,
                                      .motion = motion,
                                      .end = end,
                                      .at = at,
                                      .origin = origin,
                                      .scale = s_,
                                      .label = std::move(label)});
    }

    const std::vector<Vec3>* outline(const std::string& path) const {
        if (options_.outlines == nullptr) {
            return nullptr;
        }
        const auto found = options_.outlines->find(path);
        return found != options_.outlines->end() && !found->second.empty() ? &found->second : nullptr;
    }

    // The model, placed and turned as it stands, and the shape it collides
    // as when that is not the model: the one thing about it an artist
    // cannot otherwise see.
    void add_object(const SceneItem& item, const Object& object) {
        Guide guide{.item = item.uid, .kind = GuideKind::Collider, .focused = true};
        const Vec3 at = object.position * s_;
        const Quat turn = prop_orientation(object.rotation);
        const float size = std::max(object.size, 0.001F) * s_;
        if (const std::vector<Vec3>* lines = outline(object.mesh)) {
            for (std::size_t i = 0U; i + 1U < lines->size(); i += 2U) {
                guide.strokes.push_back(line(at + rotate(turn, (*lines)[i] * size), at + rotate(turn, (*lines)[i + 1U] * size)));
            }
        } else {
            marker(guide, at, s_);
        }
        // Only for one on its own: a heap's shapes are not at its base.
        const bool single = object.distribution == Distribution::Single;
        if (single && object.collider == ObjectCollider::Sphere) {
            ball(guide, at, size * 0.5F, true);
        } else if (single && object.collider == ObjectCollider::Box) {
            box(guide, at, Vec3{size, size, size} * 0.5F, turn, true);
        }
        // Where a driven object is headed, so its move reads at a glance.
        if (object.motion == ObjectMotion::Animated) {
            const Vec3 ends = object.move_to * s_;
            if (length(ends - at) > 0.01F) {
                guide.strokes.push_back(line(at, ends, true, true));
            }
        }
        drop(guide, at);
        keep(std::move(guide));
        handle(item.uid, HandleTarget::Position, HandleMotion::Floor, at, at, "Position");
        if (single && object.collider == ObjectCollider::Sphere) {
            handle(item.uid, HandleTarget::Radius, HandleMotion::Radius, at + x_axis * (size * 0.5F), at, "Radius");
        }
    }

    // Where its particles are born, both ends of the radius, and the Static
    // things they meet.
    void add_system(const SceneItem& item, const ParticleSystem& system, bool focused) {
        Guide guide{.item = item.uid, .kind = GuideKind::Shape, .focused = focused};
        const Vec3 centre = system.position * s_;
        const float low = std::max(system.radius.min, 0.0F) * s_;
        const float high = std::max(system.radius.max, 0.0F) * s_;
        ball(guide, centre, high, false);
        if (low < high) {
            guide.strokes.push_back(circle(centre, x_axis, z_axis, low, true));
        }
        drop(guide, centre);
        keep(std::move(guide));
        if (focused) {
            // Pulling the low end in from the high one softens the edge.
            handle(item.uid, HandleTarget::Radius, HandleMotion::Radius, centre + x_axis * high, centre,
                   "Radius, high end", 1);
            handle(item.uid, HandleTarget::Radius, HandleMotion::Radius, centre - x_axis * low, centre,
                   "Radius, low end", 0);
            handle(item.uid, HandleTarget::Position, HandleMotion::Floor, centre, centre, "Position");
        }
        if (!system.collide) {
            return;
        }
        for (const SceneItem& other : scene_.items) {
            const auto* still = std::get_if<Object>(&other.data);
            if (still == nullptr || !other.simulate || still->motion != ObjectMotion::Static) {
                continue;
            }
            Guide met{.item = other.uid, .kind = GuideKind::Collider, .focused = false};
            const auto [low_corner, high_corner] = shape_bounds(still->mesh);
            if (still->mesh == "floor") {
                // A floor at the ground says nothing the stage does not.
                const float height = (still->position.y + high_corner.y * still->size) * s_;
                if (std::fabs(height) < 1.0e-3F * s_) {
                    continue;
                }
                const float half = 1.5F * s_;
                const Vec3 middle{0.0F, height, 0.0F};
                GuideStroke edge{.closed = true};
                edge.points = {middle + (x_axis + z_axis) * half, middle + (x_axis - z_axis) * half,
                               middle - (x_axis + z_axis) * half, middle - (x_axis - z_axis) * half};
                met.strokes.push_back(std::move(edge));
                met.strokes.push_back(line(middle - x_axis * half, middle + x_axis * half, true));
                met.strokes.push_back(line(middle - z_axis * half, middle + z_axis * half, true));
            } else if (still->collider == ObjectCollider::Sphere) {
                ball(met, still->position * s_, still->size * 0.5F * s_, false);
            } else if (library_mesh(still->mesh) == nullptr && still->collider == ObjectCollider::Hull) {
                // Met as its own model, which its own guide draws where it
                // stands: nothing to add.
                continue;
            } else {
                const Quat turn = prop_orientation(still->rotation);
                const Vec3 middle = rotate(turn, (low_corner + high_corner) * (0.5F * still->size));
                const Vec3 half = (high_corner - low_corner) * (0.5F * still->size * s_);
                box(met, (still->position + middle) * s_, half, turn, still->rotation != Vec3{});
            }
            keep(std::move(met));
        }
    }

    // A force is drawn where it acts: beside the first system it acts on.
    void add_force(const SceneItem& item, const Force& force) {
        Guide guide{.item = item.uid, .kind = GuideKind::Force, .focused = true};
        if (force.kind == ForceKind::Blast) {
            const Vec3 centre = force.position * s_;
            const float radius = std::max(force.radius, 0.0F) * s_;
            // How far it reaches, faintly, and a burst of arrows out from
            // where it goes off.
            ball(guide, centre, radius, true);
            const float inner = 0.06F * s_;
            const float outer = std::max(std::min(0.3F * s_, radius), inner * 2.0F);
            for (const Vec3& d : six_ways) {
                arrow(guide, centre + d * inner, centre + d * outer);
            }
            drop(guide, centre);
            keep(std::move(guide));
            handle(item.uid, HandleTarget::Radius, HandleMotion::Radius, centre + x_axis * radius, centre, "Radius");
            handle(item.uid, HandleTarget::Position, HandleMotion::Floor, centre, centre, "Where it goes off");
            return;
        }
        Vec3 origin{};
        const SceneItem* target = nullptr;
        for (const SceneItem& other : scene_.items) {
            if (std::holds_alternative<ParticleSystem>(other.data) && force_targets(force, other.uid) &&
                (target == nullptr || other.uid < target->uid)) {
                target = &other;
            }
        }
        if (target != nullptr) {
            const auto& system = std::get<ParticleSystem>(target->data);
            origin = system.position * s_;
            add_system(*target, system, false);
        }
        const float sign = force.strength.max < 0.0F ? -1.0F : 1.0F;
        switch (force.kind) {
        case ForceKind::Wind: {
            const float heading = force.heading * pi / 180.0F;
            const Vec3 along{std::cos(heading), 0.0F, -std::sin(heading)};
            const Vec3 start = origin + up * (0.3F * s_);
            const float reach = std::max(force.strength.max * 0.15F, 0.2F) * s_;
            arrow(guide, start, start + along * reach);
            keep(std::move(guide));
            handle(item.uid, HandleTarget::Heading, HandleMotion::Heading, start + along * reach, start, "Heading");
            return;
        }
        case ForceKind::Vortex: {
            const Vec3 centre = force.position * s_;
            const Vec3 axis = force.axis == VortexAxis::Horizontal ? x_axis : up;
            guide.strokes.push_back(line(centre - axis * s_, centre + axis * s_, true));
            const auto [u, v] = basis(axis);
            const float radius = 0.3F * s_;
            // A ring round the axis, with arrows the way particles turn
            // (counter-clockwise looking down it for positive strength).
            for (int i = 0; i < 3; ++i) {
                GuideStroke turn;
                for (int k = 0; k <= 10; ++k) {
                    const float angle =
                        sign * (2.0F * pi * (static_cast<float>(i) + static_cast<float>(k) * 0.07F) / 3.0F);
                    turn.points.push_back(centre + u * (std::cos(angle) * radius) + v * (std::sin(angle) * radius));
                }
                turn.arrow = true;
                guide.strokes.push_back(std::move(turn));
            }
            drop(guide, centre);
            keep(std::move(guide));
            handle(item.uid, HandleTarget::Position, HandleMotion::Floor, centre, centre, "Center");
            return;
        }
        case ForceKind::DrawIn: {
            const Vec3 centre = force.position * s_;
            const float height = force.height * s_;
            const float top = height > 0.0F ? height : 1.5F * s_;
            guide.strokes.push_back(line(centre, centre + up * top, height <= 0.0F));
            const float radius = 0.3F * s_;
            guide.strokes.push_back(circle(centre, x_axis, z_axis, radius, true));
            for (const Vec3 d : {x_axis, x_axis * -1.0F, z_axis, z_axis * -1.0F}) {
                arrow(guide, centre + d * radius, centre + d * (radius * 0.45F));
            }
            if (height > 0.0F) {
                guide.strokes.push_back(circle(centre + up * height, x_axis, z_axis, radius * 0.25F, true));
            }
            keep(std::move(guide));
            if (height > 0.0F) {
                handle(item.uid, HandleTarget::Height, HandleMotion::Height, centre + up * height, centre, "Fades by");
            }
            handle(item.uid, HandleTarget::Position, HandleMotion::Floor, centre, centre, "Center");
            return;
        }
        case ForceKind::Attract: {
            const Vec3 point = force.position * s_;
            const float reach = force.reach * s_;
            const float inner = 0.08F * s_;
            const float outer = 0.25F * s_;
            for (const Vec3& d : six_ways) {
                if (sign > 0.0F) {
                    arrow(guide, point + d * outer, point + d * inner);
                } else {
                    arrow(guide, point + d * inner, point + d * outer);
                }
            }
            if (reach > 0.0F) {
                ball(guide, point, reach, true);
            }
            drop(guide, point);
            keep(std::move(guide));
            if (reach > 0.0F) {
                handle(item.uid, HandleTarget::Reach, HandleMotion::Radius, point + x_axis * reach, point, "Reach");
            }
            handle(item.uid, HandleTarget::Position, HandleMotion::Floor, point, point, "Point");
            return;
        }
        case ForceKind::Blast: return;
        }
    }

    // The box of air, drawn as a field: a corner sizes it and its centre
    // moves it.
    void add_grid(const SceneItem& item, const SmokeGrid& grid) {
        Guide guide{.item = item.uid, .kind = GuideKind::Field, .focused = true};
        const Vec3 centre = grid.position * s_;
        const Vec3 half = Vec3{std::fabs(grid.size.x), std::fabs(grid.size.y), std::fabs(grid.size.z)} * (0.5F * s_);
        box(guide, centre, half, Quat{}, false);
        drop(guide, centre);
        keep(std::move(guide));
        handle(item.uid, HandleTarget::Size, HandleMotion::Corner, centre + half, centre, "Size");
        handle(item.uid, HandleTarget::Position, HandleMotion::Floor, centre, centre, "Position");
    }

    const Scene& scene_;
    const GuideOptions& options_;
    GuideSet& out_;
    float s_;
};

} // namespace

GuideSet build_guides(const Document& document, const GuideOptions& options) {
    GuideSet set;
    const SceneItem* item = document.scene && options.item != 0U ? document.scene->item(options.item) : nullptr;
    if (item == nullptr || !item->simulate) {
        return set;
    }
    Builder{document, options, set}.item(*item);
    return set;
}

bool drag_handle(Document& document, const Handle& handle, Vec3 world) {
    SceneItem* item = document.scene ? document.scene->item(handle.item) : nullptr;
    if (item == nullptr) {
        return false;
    }
    const float s = handle.scale > 1.0e-6F ? handle.scale : 1.0F;
    const Vec3 point = world * (1.0F / s);
    const float distance = length(world - handle.origin) / s;
    const SceneItem before = *item;
    const auto heading = [&]() {
        const Vec3 d = world - handle.origin;
        float degrees = std::atan2(-d.z, d.x) * 180.0F / pi;
        if (degrees < 0.0F) {
            degrees += 360.0F;
        }
        return std::round(degrees);
    };
    if (auto* object = std::get_if<Object>(&item->data)) {
        if (handle.target == HandleTarget::Position) {
            object->position = point;
        } else if (handle.target == HandleTarget::Radius) {
            object->size = std::max(2.0F * distance, 0.01F);
        }
    } else if (auto* system = std::get_if<ParticleSystem>(&item->data)) {
        if (handle.target == HandleTarget::Position) {
            system->position = point;
        } else if (handle.target == HandleTarget::Radius) {
            // The end dragged past the other takes it along.
            if (handle.end == 0) {
                system->radius.min = distance;
                system->radius.max = std::max(system->radius.max, distance);
            } else {
                system->radius.max = distance;
                system->radius.min = std::min(system->radius.min, distance);
            }
        }
    } else if (auto* force = std::get_if<Force>(&item->data)) {
        switch (handle.target) {
        case HandleTarget::Position: force->position = point; break;
        case HandleTarget::Radius: force->radius = std::max(distance, 0.01F); break;
        case HandleTarget::Heading: force->heading = heading(); break;
        case HandleTarget::Reach: force->reach = distance; break;
        case HandleTarget::Height: force->height = std::max((world.y - handle.origin.y) / s, 0.0F); break;
        case HandleTarget::Size: break;
        }
    } else if (auto* grid = std::get_if<SmokeGrid>(&item->data)) {
        if (handle.target == HandleTarget::Position) {
            grid->position = point;
        } else if (handle.target == HandleTarget::Size) {
            const Vec3 d = world - handle.origin;
            grid->size = Vec3{std::max(std::fabs(d.x) * 2.0F / s, 0.05F), std::max(std::fabs(d.y) * 2.0F / s, 0.05F),
                              std::max(std::fabs(d.z) * 2.0F / s, 0.05F)};
        }
    }
    return !(before == *item);
}

} // namespace ajfx::studio
