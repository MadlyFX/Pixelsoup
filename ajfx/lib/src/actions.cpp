#include <ajfx/actions.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>

namespace ajfx {
namespace {

void mix(std::uint64_t& hash, std::uint64_t value) {
    for (std::uint32_t i = 0U; i < 8U; ++i) {
        hash ^= static_cast<std::uint8_t>(value >> (8U * i));
        hash *= 0x100000001b3ULL;
    }
}

void mix(std::uint64_t& hash, float value) {
    mix(hash, static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(value)));
}

void mix(std::uint64_t& hash, Vec3 v) {
    mix(hash, v.x);
    mix(hash, v.y);
    mix(hash, v.z);
}

} // namespace

std::uint64_t action_digest(std::span<const Action> actions, std::uint64_t hash) {
    mix(hash, static_cast<std::uint64_t>(actions.size()));
    for (const Action& a : actions) {
        mix(hash, static_cast<std::uint64_t>(a.kind));
        mix(hash, a.tick);
        mix(hash, static_cast<std::uint64_t>(a.serial));
        mix(hash, static_cast<std::uint64_t>(a.body));
        mix(hash, a.position);
        mix(hash, a.orientation.x);
        mix(hash, a.orientation.y);
        mix(hash, a.orientation.z);
        mix(hash, a.orientation.w);
        mix(hash, a.velocity);
        mix(hash, a.angular_velocity);
        mix(hash, a.size);
        mix(hash, a.release);
        mix(hash, static_cast<std::uint64_t>(a.target));
        mix(hash, a.push);
        mix(hash, a.twist);
        mix(hash, a.gain);
        mix(hash, a.max_speed);
        mix(hash, a.radius);
        mix(hash, a.power);
    }
    return hash;
}

void ActionList::add(std::string_view emitter, const Action& action) {
    auto found = lists_.find(emitter);
    if (found == lists_.end()) {
        found = lists_.emplace(std::string{emitter}, std::vector<Action>{}).first;
    }
    std::vector<Action>& list = found->second;
    const auto at = std::upper_bound(list.begin(), list.end(), action.tick,
                                     [](std::uint64_t tick, const Action& held) { return tick < held.tick; });
    list.insert(at, action);
}

std::span<const Action> ActionList::actions(std::string_view emitter) const {
    const auto found = lists_.find(emitter);
    return found == lists_.end() ? std::span<const Action>{} : std::span<const Action>{found->second};
}

std::size_t ActionList::size() const noexcept {
    std::size_t total = 0U;
    for (const auto& [name, list] : lists_) {
        total += list.size();
    }
    return total;
}

std::span<const Action> ActionList::actions_at(std::string_view emitter, std::uint64_t tick) const {
    const auto found = lists_.find(emitter);
    if (found == lists_.end()) {
        return {};
    }
    const std::vector<Action>& list = found->second;
    const auto first = std::lower_bound(list.begin(), list.end(), tick,
                                        [](const Action& held, std::uint64_t t) { return held.tick < t; });
    const auto last = std::upper_bound(first, list.end(), tick,
                                       [](std::uint64_t t, const Action& held) { return t < held.tick; });
    return std::span<const Action>{list}.subspan(static_cast<std::size_t>(first - list.begin()),
                                                 static_cast<std::size_t>(last - first));
}

std::uint64_t ActionList::digest(std::string_view emitter) const {
    return action_digest(actions(emitter));
}

} // namespace ajfx
