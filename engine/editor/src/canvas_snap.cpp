#include <mediaserver/editor/canvas_snap.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace mediaserver::editor {

AxisSnap snap_axis(const std::array<float, 3U>& edges,
                   std::span<const float> lines, float reach) noexcept {
    AxisSnap best{};
    for (const float line : lines) {
        if (!std::isfinite(line)) {
            continue;
        }
        for (const float edge : edges) {
            const float shift = line - edge;
            if (std::abs(shift) <= reach &&
                (!best.line.has_value() ||
                 std::abs(shift) < std::abs(best.shift))) {
                best.shift = shift;
                best.line = line;
            }
        }
    }
    return best;
}

void add_snap_lines(std::vector<float>& lines, float low, float high) {
    lines.push_back(low);
    lines.push_back((low + high) * 0.5F);
    lines.push_back(high);
}

CanvasBox bounds_of(std::span<const CanvasBox> boxes) noexcept {
    if (boxes.empty()) {
        return CanvasBox{};
    }
    CanvasBox all = boxes.front();
    for (const CanvasBox& box : boxes) {
        all.left = std::min(all.left, box.left);
        all.top = std::min(all.top, box.top);
        all.right = std::max(all.right, box.right);
        all.bottom = std::max(all.bottom, box.bottom);
    }
    return all;
}

std::vector<CanvasMove> align_boxes(std::span<const CanvasBox> boxes,
                                    const CanvasBox& reference,
                                    CanvasAlign align) {
    std::vector<CanvasMove> moves;
    moves.reserve(boxes.size());
    for (const CanvasBox& box : boxes) {
        CanvasMove move{};
        switch (align) {
        case CanvasAlign::Left:
            move.x = reference.left - box.left;
            break;
        case CanvasAlign::CentreAcross:
            move.x = reference.centre_x() - box.centre_x();
            break;
        case CanvasAlign::Right:
            move.x = reference.right - box.right;
            break;
        case CanvasAlign::Top:
            move.y = reference.top - box.top;
            break;
        case CanvasAlign::CentreDown:
            move.y = reference.centre_y() - box.centre_y();
            break;
        case CanvasAlign::Bottom:
            move.y = reference.bottom - box.bottom;
            break;
        }
        moves.push_back(move);
    }
    return moves;
}

std::vector<CanvasMove> distribute_boxes(std::span<const CanvasBox> boxes,
                                         bool across) {
    std::vector<CanvasMove> moves(boxes.size());
    if (boxes.size() < 3U) {
        return moves;
    }
    const auto middle = [&](std::size_t index) {
        return across ? boxes[index].centre_x() : boxes[index].centre_y();
    };
    std::vector<std::size_t> order(boxes.size());
    std::iota(order.begin(), order.end(), std::size_t{0U});
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) {
                         return middle(a) < middle(b);
                     });
    const float first = middle(order.front());
    const float last = middle(order.back());
    const float step =
        (last - first) / static_cast<float>(boxes.size() - 1U);
    for (std::size_t place = 0U; place < order.size(); ++place) {
        const float target = first + step * static_cast<float>(place);
        const float shift = target - middle(order[place]);
        if (across) {
            moves[order[place]].x = shift;
        } else {
            moves[order[place]].y = shift;
        }
    }
    return moves;
}

float ruler_step(float scale, float minimum) noexcept {
    if (!(scale > 0.0F) || !(minimum > 0.0F)) {
        return 1.0F;
    }
    const float wanted = minimum / scale;
    float decade = std::pow(10.0F, std::floor(std::log10(wanted)));
    if (!std::isfinite(decade) || decade < 1.0F) {
        decade = 1.0F;
    }
    for (const float multiple : {1.0F, 2.0F, 5.0F, 10.0F}) {
        if (decade * multiple >= wanted) {
            return decade * multiple;
        }
    }
    return decade * 10.0F;
}

}  // namespace mediaserver::editor
