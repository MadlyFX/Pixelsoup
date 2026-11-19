#include <mediaserver/show/pointer_overlay.hpp>

#include <cmath>
#include <utility>

namespace mediaserver::show {
namespace {

[[nodiscard]] bool valid(PointerPoint point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y)
        && std::fabs(point.x) <= max_pointer_coordinate
        && std::fabs(point.y) <= max_pointer_coordinate;
}

} // namespace

bool valid(const PointerOverlay& overlay) noexcept {
    if (overlay.points.size() > max_pointer_points
        || overlay.stroke_ends.size() > max_pointer_strokes) {
        return false;
    }
    if (overlay.dot.has_value() && !valid(*overlay.dot)) {
        return false;
    }
    core::u32 start = 0U;
    for (const core::u32 end : overlay.stroke_ends) {
        if (end <= start) {
            return false;
        }
        start = end;
    }
    if (start != overlay.points.size()) {
        return false;
    }
    for (const PointerPoint point : overlay.points) {
        if (!valid(point)) {
            return false;
        }
    }
    return true;
}

bool PointerOverlayChannel::accept(PointerOverlay overlay) {
    const PointerOverlay* newest = !pending_.empty()
        ? &pending_.back()
        : (current_.has_value() ? &*current_ : nullptr);
    if (newest != nullptr && overlay.control_frame < newest->control_frame) {
        return false;
    }
    if (!pending_.empty()
        && pending_.back().control_frame == overlay.control_frame) {
        // Two pictures for one frame: the later one is what the operator
        // last did on it.
        pending_.back() = std::move(overlay);
        return true;
    }
    if (pending_.size() == capacity) {
        pending_.erase(pending_.begin());
    }
    pending_.push_back(std::move(overlay));
    return true;
}

const PointerOverlay* PointerOverlayChannel::at(core::u64 control_frame) {
    std::size_t due = 0U;
    while (due < pending_.size()
           && pending_[due].control_frame <= control_frame) {
        ++due;
    }
    if (due != 0U) {
        current_ = std::move(pending_[due - 1U]);
        pending_.erase(pending_.begin(),
                       pending_.begin() + static_cast<std::ptrdiff_t>(due));
    }
    return current_.has_value() ? &*current_ : nullptr;
}

} // namespace mediaserver::show
