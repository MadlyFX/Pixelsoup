#include <mediaserver/editor/laser_pointer.hpp>

#include <algorithm>
#include <cmath>

namespace mediaserver::editor {

void LaserPointer::press() {
    active_ = true;
    drawing_ = false;
    dot_.reset();
    if (!strokes_.empty()) {
        strokes_.clear();
        point_count_ = 0U;
    }
    ++revision_;
}

void LaserPointer::release() {
    active_ = false;
    drawing_ = false;
    dot_.reset();
    std::erase_if(strokes_, [](const LaserStroke& stroke) {
        return stroke.ink == LaserInk::Fleeting;
    });
    point_count_ = 0U;
    for (const LaserStroke& stroke : strokes_) {
        point_count_ += stroke.points.size();
    }
    ++revision_;
}

void LaserPointer::point_at(std::optional<LaserMark> dot) {
    if (!active_) {
        dot.reset();
    }
    if (dot != dot_) {
        dot_ = dot;
        ++revision_;
    }
}

bool LaserPointer::full() const noexcept {
    return point_count_ >= show::max_pointer_points
        || strokes_.size() >= show::max_pointer_strokes;
}

bool LaserPointer::begin_stroke(LaserInk ink, LaserMark at) {
    if (!active_ || drawing_ || full()) {
        return false;
    }
    strokes_.push_back(LaserStroke{ink, at.space, {at.at}});
    ++point_count_;
    drawing_ = true;
    ++revision_;
    return true;
}

bool LaserPointer::extend(scene::Vec3 at) {
    if (!active_ || !drawing_ || strokes_.empty()
        || point_count_ >= show::max_pointer_points) {
        return false;
    }
    strokes_.back().points.push_back(at);
    ++point_count_;
    ++revision_;
    return true;
}

LaserSpace LaserPointer::drawing_space() const noexcept {
    return strokes_.empty() ? LaserSpace::Canvas : strokes_.back().space;
}

show::PointerOverlay wall_pointer_overlay(const LaserPointer& pointer,
                                          const scene::Camera& camera,
                                          scene::Canvas canvas) {
    show::PointerOverlay overlay{};
    if (!pointer.active()) {
        return overlay;
    }
    const auto on_canvas =
        [&](LaserSpace space,
            scene::Vec3 at) -> std::optional<show::PointerPoint> {
        if (space == LaserSpace::Canvas) {
            return show::PointerPoint{at.x, at.y};
        }
        const scene::CanvasProjection projected =
            scene::project_to_canvas(camera, canvas, at);
        if (!projected.visible || !std::isfinite(projected.x)
            || !std::isfinite(projected.y)
            || std::fabs(projected.x) > show::max_pointer_coordinate
            || std::fabs(projected.y) > show::max_pointer_coordinate) {
            return std::nullopt;
        }
        return show::PointerPoint{projected.x, projected.y};
    };
    if (const auto& dot = pointer.dot()) {
        overlay.dot = on_canvas(dot->space, dot->at);
    }
    overlay.points.reserve(pointer.point_count());
    const auto close_stroke = [&](std::size_t first) {
        if (overlay.points.size() > first
            && overlay.stroke_ends.size() < show::max_pointer_strokes) {
            overlay.stroke_ends.push_back(
                static_cast<core::u32>(overlay.points.size()));
        } else {
            overlay.points.resize(first);
        }
    };
    for (const LaserStroke& stroke : pointer.strokes()) {
        std::size_t first = overlay.points.size();
        for (const scene::Vec3& point : stroke.points) {
            const auto at = on_canvas(stroke.space, point);
            if (!at.has_value()) {
                close_stroke(first);
                first = overlay.points.size();
                continue;
            }
            if (overlay.points.size() >= show::max_pointer_points) {
                break;
            }
            overlay.points.push_back(*at);
        }
        close_stroke(first);
    }
    return overlay;
}

} // namespace mediaserver::editor
