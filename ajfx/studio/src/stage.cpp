#include "stage.hpp"

#include <ajfx/random.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <vector>

namespace ajfx::studio {
namespace {

constexpr float degrees = 180.0F / std::numbers::pi_v<float>;

// A thing the tools move keeps where it was put the first time.
void remember(Act& act) {
    if (!act.touched) {
        act.touched = true;
        act.home = act.position;
        act.home_turn = act.turn;
    }
}

// Between -1 and 1, the same for the same seed, serial and stream.
float signed_unit(std::uint32_t seed, std::uint32_t serial, std::uint32_t stream) {
    return hash_unit(particle_hash(seed, serial, stream)) * 2.0F - 1.0F;
}

} // namespace

std::size_t stage_count(const Performance& performance) {
    return static_cast<std::size_t>(std::count_if(performance.acts.begin(), performance.acts.end(), on_stage));
}

Vec3 turn_of(Quat q) noexcept {
    // The rotation matrix of q. prop_orientation turns about y (yaw), then x
    // (pitch), then z (roll), which makes m21 the sine of the pitch.
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    const float m00 = 1.0F - 2.0F * (yy + zz);
    const float m01 = 2.0F * (xy - wz);
    const float m02 = 2.0F * (xz + wy);
    const float m11 = 1.0F - 2.0F * (xx + zz);
    const float m20 = 2.0F * (xz - wy);
    const float m21 = 2.0F * (yz + wx);
    const float m22 = 1.0F - 2.0F * (xx + yy);
    const float pitch = std::asin(std::clamp(m21, -1.0F, 1.0F));
    float yaw = 0.0F;
    float roll = 0.0F;
    if (std::abs(m21) > 0.99999F) {
        // Looking straight up or down: yaw and roll turn the same way, so
        // the whole of it is called yaw.
        yaw = std::atan2(m02, m00);
    } else {
        yaw = std::atan2(-m20, m22);
        roll = std::atan2(-m01, m11);
    }
    return Vec3{pitch * degrees, yaw * degrees, roll * degrees};
}

PoseMatrix pose_matrix(Vec3 position, Vec3 turn) noexcept {
    const Quat q = prop_orientation(turn);
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    // Row r, column c of the rotation, stored at [c * 4 + r].
    return PoseMatrix{1.0F - 2.0F * (yy + zz), 2.0F * (xy + wz),        2.0F * (xz - wy),        0.0F,
                      2.0F * (xy - wz),        1.0F - 2.0F * (xx + zz), 2.0F * (yz + wx),        0.0F,
                      2.0F * (xz + wy),        2.0F * (yz - wx),        1.0F - 2.0F * (xx + yy), 0.0F,
                      position.x,              position.y,              position.z,              1.0F};
}

void pose_from_matrix(const PoseMatrix& m, Vec3& position, Vec3& turn) noexcept {
    position = Vec3{m[12], m[13], m[14]};
    // The columns are the rotated axes; unit length takes any scale out.
    const auto column = [&](std::size_t c) {
        const Vec3 v{m[c * 4U], m[c * 4U + 1U], m[c * 4U + 2U]};
        const float l = std::sqrt(dot(v, v));
        return l > 1.0e-9F ? v * (1.0F / l) : v;
    };
    const Vec3 x = column(0U), y = column(1U), z = column(2U);
    // Row r, column c.
    const float r00 = x.x, r10 = x.y, r20 = x.z;
    const float r01 = y.x, r11 = y.y, r21 = y.z;
    const float r02 = z.x, r12 = z.y, r22 = z.z;
    Quat q{};
    const float trace = r00 + r11 + r22;
    if (trace > 0.0F) {
        const float s = std::sqrt(trace + 1.0F) * 2.0F;
        q = Quat{(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25F * s};
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0F + r00 - r11 - r22) * 2.0F;
        q = Quat{0.25F * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0F + r11 - r00 - r22) * 2.0F;
        q = Quat{(r01 + r10) / s, 0.25F * s, (r12 + r21) / s, (r02 - r20) / s};
    } else {
        const float s = std::sqrt(1.0F + r22 - r00 - r11) * 2.0F;
        q = Quat{(r02 + r20) / s, (r12 + r21) / s, 0.25F * s, (r10 - r01) / s};
    }
    turn = turn_of(q);
}

std::vector<Uid> add_dropped_models(Document& document, std::span<const std::string> files) {
    std::vector<Uid> kinds;
    if (!document.scene) {
        return kinds;
    }
    for (const std::string& file : files) {
        std::string extension = std::filesystem::path{file}.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".obj" && extension != ".gltf" && extension != ".glb") {
            continue;
        }
        const Uid uid = add_object(document, std::filesystem::path{file}.generic_string());
        if (uid == 0U) {
            continue;
        }
        Object& object = std::get<Object>(document.scene->item(uid)->data);
        object.distribution = Distribution::Placed;
        object.motion = ObjectMotion::Dynamic;
        kinds.push_back(uid);
    }
    return kinds;
}

StageAct place(Document& document, const ActTargets& targets, Uid kind, Vec3 position, Vec3 turn) {
    if (!document.scene) {
        return StageAct{0U, "Only a scene has a stage"};
    }
    Scene& scene = *document.scene;
    const SceneItem* item = scene.item(kind);
    if (item == nullptr || !std::holds_alternative<Object>(item->data)) {
        return StageAct{0U, "Only an object can be placed on the stage"};
    }
    if (!targets.drops || scene.physics.max_dropped == 0U) {
        return StageAct{0U, "This scene takes no placed things: raise Most dropped at once in Scene settings"};
    }
    if (!droppable(targets, kind)) {
        return StageAct{0U, item->name + " stands still or is moved: only a Dynamic object can be placed by hand"};
    }
    if (dropped_count(scene.performance) >= scene.physics.max_dropped) {
        return StageAct{0U, "The scene holds at most " + std::to_string(scene.physics.max_dropped) +
                                " placed and dropped things at once: delete some, or raise Most dropped at once"};
    }
    return StageAct{add_drop(scene.performance, 0U, kind, position, turn, Vec3{}), {}};
}

bool move_placement(Performance& performance, std::uint32_t serial, Vec3 position, Vec3 turn) {
    for (Act& act : performance.acts) {
        if (act.serial == serial && on_stage(act)) {
            act.position = position;
            act.turn = turn;
            act.touched = false;
            act.home = {};
            act.home_turn = {};
            return true;
        }
    }
    return false;
}

std::size_t jitter_stage(Performance& performance, std::uint32_t seed, float shift, float tilt) {
    std::size_t moved = 0U;
    for (Act& act : performance.acts) {
        if (!on_stage(act)) {
            continue;
        }
        remember(act);
        act.position = act.position + Vec3{signed_unit(seed, act.serial, 0U) * shift,
                                           signed_unit(seed, act.serial, 1U) * shift,
                                           signed_unit(seed, act.serial, 2U) * shift};
        // The tilt composes with the pose the thing has, so a jitter on a
        // settled thing leans it rather than discarding how it lies.
        const Quat leaned = times(prop_orientation(Vec3{signed_unit(seed, act.serial, 3U) * tilt,
                                                        signed_unit(seed, act.serial, 4U) * tilt,
                                                        signed_unit(seed, act.serial, 5U) * tilt}),
                                  prop_orientation(act.turn));
        act.turn = turn_of(leaned);
        ++moved;
    }
    return moved;
}

std::size_t reset_stage(Performance& performance) {
    std::size_t moved = 0U;
    for (Act& act : performance.acts) {
        if (on_stage(act) && act.touched) {
            act.position = act.home;
            act.turn = act.home_turn;
            act.touched = false;
            act.home = {};
            act.home_turn = {};
            ++moved;
        }
    }
    return moved;
}

SettleResult settle_stage(Document& document, const Compiled& compiled, const RigidSolver* solver,
                          const SettleOptions& options) {
    SettleResult result;
    if (!document.scene || !compiled.effect) {
        result.refusal = "There is no scene to settle";
        return result;
    }
    Performance& performance = document.scene->performance;
    if (stage_count(performance) == 0U) {
        result.refusal = "Nothing is placed on the stage yet";
        return result;
    }
    const ActTargets targets = act_targets(compiled);
    if (!targets.drops) {
        result.refusal = "This scene takes no placed things";
        return result;
    }
    if (solver == nullptr) {
        result.refusal = "There is no physics to settle with";
        return result;
    }
    // Only the stage plays: what is done later, or taken away, is not part
    // of how things come to rest.
    Performance stage_only;
    stage_only.next_serial = performance.next_serial;
    for (const Act& act : performance.acts) {
        if (on_stage(act)) {
            stage_only.acts.push_back(act);
        }
    }
    const std::shared_ptr<const ActionList> actions = performance_actions(stage_only, targets);
    auto created = Simulation::create(*compiled.effect, options.instance_seed,
                                      SimulationOptions{.fields = compiled.fields,
                                                        .meshes = compiled.meshes,
                                                        .rigid = solver});
    if (!created) {
        result.refusal = created.error().message;
        return result;
    }
    Simulation simulation = std::move(created).value();
    simulation.reset(nullptr, actions.get());
    const auto& emitters = compiled.effect->emitters;
    std::size_t emitter = emitters.size();
    for (std::size_t i = 0U; i < emitters.size(); ++i) {
        if (emitters[i].name == targets.emitter) {
            emitter = i;
            break;
        }
    }
    if (emitter == emitters.size()) {
        result.refusal = "The scene has no physics world";
        return result;
    }

    const double step = simulation.step_seconds();
    const std::uint64_t most = static_cast<std::uint64_t>(std::ceil(options.most_seconds / step));
    const std::uint64_t quiet =
        std::max<std::uint64_t>(1U, static_cast<std::uint64_t>(std::ceil(options.still_for / step)));
    std::vector<std::uint32_t> indices;
    for (const Act& act : stage_only.acts) {
        indices.push_back(spawn_index_of(*targets.drops, act.serial));
    }
    const auto still = [&](const EmitterState& state, std::uint32_t index) {
        for (std::size_t i = 0U; i < state.alive(); ++i) {
            if (state.spawn_index[i] == index) {
                const Vec3 v = state.velocity[i];
                const Vec3 w = state.angular_velocity[i];
                return std::sqrt(dot(v, v)) <= options.still_speed && std::sqrt(dot(w, w)) <= options.still_spin;
            }
        }
        return true; // gone: nothing to wait for
    };
    std::uint64_t calm = 0U;
    while (simulation.tick() < most) {
        simulation.step(nullptr, actions.get());
        const EmitterState& state = simulation.emitter(emitter);
        bool all = true;
        for (const std::uint32_t index : indices) {
            all = all && still(state, index);
        }
        calm = all ? calm + 1U : 0U;
        if (calm >= quiet) {
            break;
        }
        if (options.progress && (simulation.tick() % 30U) == 0U &&
            !options.progress(static_cast<double>(simulation.tick()) / static_cast<double>(most))) {
            result.refusal = "Stopped";
            return result;
        }
    }
    result.steps = simulation.tick();

    const EmitterState& state = simulation.emitter(emitter);
    for (Act& act : performance.acts) {
        if (!on_stage(act)) {
            continue;
        }
        const std::uint32_t index = spawn_index_of(*targets.drops, act.serial);
        std::size_t at = state.alive();
        for (std::size_t i = 0U; i < state.alive(); ++i) {
            if (state.spawn_index[i] == index) {
                at = i;
                break;
            }
        }
        if (at == state.alive()) {
            continue; // refused, or broken apart
        }
        if (!still(state, index)) {
            ++result.moving;
            continue;
        }
        remember(act);
        act.position = state.position[at];
        act.turn = turn_of(state.orientation[at]);
        ++result.settled;
    }
    return result;
}

} // namespace ajfx::studio
