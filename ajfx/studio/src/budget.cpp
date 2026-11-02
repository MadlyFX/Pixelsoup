#include "budget.hpp"

#include "catalog.hpp"
#include "compile.hpp"

#include <ajfx/cache.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ajfx::studio {
namespace {

// "16384" -> "16,384".
std::string with_commas(std::uint64_t value) {
    std::string digits = std::to_string(value);
    for (std::size_t at = digits.size(); at > 3U; at -= 3U) {
        digits.insert(at - 3U, ",");
    }
    return digits;
}

// A number of milliseconds or a factor, as the page reads it: "3.7", "4",
// "0.45".
std::string short_number(double value) {
    if (!std::isfinite(value)) {
        return "?";
    }
    return format_number(static_cast<float>(value), value < 1.0 ? 2 : 1);
}

// One amount on a block: the control, and whether it counts whole things.
struct Knob final {
    std::string_view control;
    bool whole{false};
};

// The amounts a block has, as the page stands: what a Stream or Burst makes,
// what is born where things hit or behind them, the pieces of a heap or a
// stack, and the pieces a Break cuts a thing into. A count that is not
// playing (a lone Object's, a Break of placed parts) is left alone.
std::vector<Knob> knobs_of(const Block& block, LayerKind layer) {
    const std::optional<BlockKind> kind = block_kind_from(block.kind);
    if (!kind) {
        return {};
    }
    switch (*kind) {
    case BlockKind::Stream: return {{"rate", false}};
    case BlockKind::Burst: return {{"count", true}};
    case BlockKind::HitPuffs:
    case BlockKind::HitSparks:
        if (layer != LayerKind::Particles) {
            return {};
        }
        return {{"per_hit", false}, {"per_speed", false}, {"most", true}};
    case BlockKind::Trail:
        if (layer != LayerKind::Particles) {
            return {};
        }
        return {{"per_speed", false}, {"rate", false}, {"most", true}};
    case BlockKind::Object:
        if (const std::string arrange = choice_of(block, "arrange"); arrange == "heap" || arrange == "stack") {
            return {{"count", true}};
        }
        return {};
    case BlockKind::Break:
        if (choice_of(block, "into") == "cut") {
            return {{"cells", true}};
        }
        return {};
    case BlockKind::Shape:
    case BlockKind::Launch:
    case BlockKind::Gravity:
    case BlockKind::Rise:
    case BlockKind::Wind:
    case BlockKind::Turbulence:
    case BlockKind::Drag:
    case BlockKind::Swirl:
    case BlockKind::DrawIn:
    case BlockKind::Attract:
    case BlockKind::Explosion:
    case BlockKind::Push:
    case BlockKind::Stirred:
    case BlockKind::Appearance:
    case BlockKind::Color:
    case BlockKind::Size:
    case BlockKind::Spin:
    case BlockKind::Fade:
    case BlockKind::Heat:
    case BlockKind::Ground:
    case BlockKind::PassesThrough:
    case BlockKind::Sphere:
    case BlockKind::Box:
    case BlockKind::MeshCollider:
    case BlockKind::Tumble:
    case BlockKind::Part:
    case BlockKind::Physics:
    case BlockKind::Light:
    case BlockKind::Module: return {};
    }
    return {};
}

// What stops a control from growing further, in a few words.
std::string knob_limit(const Block& block, const ControlDef& control) {
    if (block.kind == BlockKind::Break) {
        return "the " + with_commas(static_cast<std::uint64_t>(control.hard_max)) + "-piece limit of Break";
    }
    return block_title(block) + "'s limit of " + with_commas(static_cast<std::uint64_t>(control.hard_max));
}

void scale_knob(Block& block, const ControlDef& control, const Knob& knob, double factor) {
    const auto scaled = [&](float value) {
        const double grown = static_cast<double>(value) * factor;
        return static_cast<float>(knob.whole ? std::round(grown) : grown);
    };
    const Value value = value_of(block, control);
    block.set(control.id, sanitize(control, number(scaled(value.x))));
    // A published amount is sized for its whole range, so the range grows
    // with it: the budget an operator can reach grows by the same factor.
    if (const Published* published = block.publication(control.id)) {
        Published grown = *published;
        grown.low = std::clamp(scaled(published->low), control.hard_min, control.hard_max);
        grown.high = std::clamp(scaled(published->high), grown.low, control.hard_max);
        block.publish(std::move(grown));
    }
}

// Particles made smaller as there are more: a particle layer's Size, by the
// square root of the factor, so the many cover what the few did.
void shrink(Block& block, double factor) {
    const BlockDef* def = find_block_def(BlockKind::Size);
    const ControlDef* control = def != nullptr ? find_control(*def, "size") : nullptr;
    if (control == nullptr) {
        return;
    }
    const auto by = static_cast<float>(1.0 / std::sqrt(factor));
    const Value size = value_of(block, *control);
    block.set(control->id, sanitize(*control, range(size.x * by, size.y * by)));
    if (const Published* published = block.publication(control->id)) {
        Published smaller = *published;
        smaller.low = published->low * by;
        smaller.high = published->high * by;
        block.publish(std::move(smaller));
    }
}

// Something the compiled page at a factor would break that the page as it is
// does not: a layer the compiler refuses, a solid world past what particles
// can meet, a baked layer past what a cache holds. Empty when nothing does.
std::string breach(const Compiled& base, const Compiled& scaled, const Document& page) {
    if (base.error.empty() && !scaled.error.empty()) {
        return scaled.error;
    }
    for (const LayerReport& report : scaled.layers) {
        const LayerReport* before = base.layer(report.layer);
        if (!report.error.empty() && (before == nullptr || before->error.empty())) {
            const Layer* layer = page.layer(report.layer);
            return (layer != nullptr && !layer->name.empty() ? layer->name + ": " : std::string{}) + report.error;
        }
        const Layer* layer = page.layer(report.layer);
        if (layer != nullptr && layer->baked && layer->kind == LayerKind::Particles && before != nullptr &&
            before->capacity <= max_cached_particles && report.capacity > max_cached_particles) {
            return "the " + with_commas(max_cached_particles) + "-particle limit of a bake";
        }
    }
    if (!base.solids.emitter.empty() && base.solids.meetable() && !scaled.solids.meetable()) {
        return "the " + with_commas(max_body_colliders) + "-piece limit";
    }
    return {};
}

} // namespace

Document scale_density(const Document& page, double factor, const DensityOptions& options) {
    Document out = page;
    if (!std::isfinite(factor) || factor <= 0.0) {
        return out;
    }
    for (Layer& layer : out.layers) {
        for (Block& block : layer.blocks) {
            if (options.keep_look && layer.kind == LayerKind::Particles && block.kind == BlockKind::Size) {
                shrink(block, factor);
                continue;
            }
            const BlockDef* def = find_block_def(block.kind);
            if (def == nullptr) {
                continue;
            }
            for (const Knob& knob : knobs_of(block, layer.kind)) {
                if (const ControlDef* control = find_control(*def, knob.control)) {
                    scale_knob(block, *control, knob, factor);
                }
            }
        }
    }
    return out;
}

std::size_t density_knobs(const Document& page) {
    std::size_t count = 0U;
    for (const Layer& layer : page.layers) {
        if (!layer.enabled) {
            continue;
        }
        for (const Block& block : layer.blocks) {
            if (!block.enabled) {
                continue;
            }
            const BlockDef* def = find_block_def(block.kind);
            for (const Knob& knob : knobs_of(block, layer.kind)) {
                const ControlDef* control = def != nullptr ? find_control(*def, knob.control) : nullptr;
                if (control != nullptr && value_of(block, *control).x > 0.0F) {
                    ++count;
                }
            }
        }
    }
    return count;
}

DensityLimit density_limit(const Document& page, const DensityOptions& options, double most) {
    DensityLimit limit{std::max(most, 1.0), {}};
    // The controls' own ranges, read straight off the page.
    for (const Layer& layer : page.layers) {
        for (const Block& block : layer.blocks) {
            const BlockDef* def = find_block_def(block.kind);
            for (const Knob& knob : knobs_of(block, layer.kind)) {
                const ControlDef* control = def != nullptr ? find_control(*def, knob.control) : nullptr;
                if (control == nullptr) {
                    continue;
                }
                float highest = value_of(block, *control).x;
                if (const Published* published = block.publication(control->id)) {
                    highest = std::max(highest, published->high);
                }
                if (highest <= 0.0F) {
                    continue;
                }
                const double room = static_cast<double>(control->hard_max) / static_cast<double>(highest);
                if (room < limit.factor) {
                    limit = DensityLimit{std::max(room, 1.0), knob_limit(block, *control)};
                }
            }
        }
    }
    // What the compiler and the runtime hold to, by compiling: the whole
    // page as exported, every enabled layer, not just a soloed one.
    const Compiled base = compile(page, Purpose::Export);
    const auto fails = [&](double factor) {
        return breach(base, compile(scale_density(page, factor, options), Purpose::Export), page);
    };
    if (limit.factor <= 1.0) {
        return limit;
    }
    std::string reason = fails(limit.factor);
    if (reason.empty()) {
        return limit;
    }
    // The amounts grow with the factor, so a breach stays a breach above it:
    // halve the gap between a factor that holds and one that does not.
    double holds = 1.0;
    double breaks = limit.factor;
    for (int step = 0; step < 12 && breaks / holds > 1.01; ++step) {
        const double middle = std::sqrt(holds * breaks);
        std::string at = fails(middle);
        if (at.empty()) {
            holds = middle;
        } else {
            breaks = middle;
            reason = std::move(at);
        }
    }
    return DensityLimit{holds, std::move(reason)};
}

// ------------------------------------------------------------ the search

BudgetSearch::BudgetSearch(Settings settings) : settings_{settings} {
    settings_.most = std::max(settings_.most, 1.0);
    settings_.least = std::clamp(settings_.least, 1.0e-4, 1.0);
    settings_.probes = std::max<std::size_t>(settings_.probes, 2U);
}

void BudgetSearch::report(double ms) {
    if (!next_) {
        return;
    }
    points_.push_back(BudgetPoint{*next_, std::isnan(ms) ? std::numeric_limits<double>::infinity() : ms});
    next_.reset();
    decide();
}

void BudgetSearch::finish(BudgetStop stop, BudgetPoint chosen) {
    stop_ = stop;
    chosen_ = chosen;
    next_.reset();
}

void BudgetSearch::decide() {
    const double budget = settings_.budget_ms;
    const double low = budget * settings_.low;
    const double high = budget * settings_.high;
    const double target = (low + high) * 0.5;
    const BudgetPoint first = points_.front();

    if (points_.size() == 1U) {
        if (first.ms >= low && first.ms <= budget) {
            finish(BudgetStop::Fits, first);
            return;
        }
        if (first.ms > budget && !settings_.allow_down) {
            finish(BudgetStop::Over, first);
            return;
        }
    }
    // The largest factor measured under the band's top, and the smallest
    // over it: the answer lies between them.
    std::optional<BudgetPoint> under;
    std::optional<BudgetPoint> over;
    for (const BudgetPoint& point : points_) {
        if (point.ms <= high) {
            if (!under || point.factor > under->factor) {
                under = point;
            }
        } else if (!over || point.factor < over->factor) {
            over = point;
        }
    }
    if (under && under->ms >= low) {
        finish(BudgetStop::InBand, *under);
        return;
    }
    if (under && under->factor >= settings_.most * (1.0 - 1.0e-9)) {
        finish(BudgetStop::Limit, *under);
        return;
    }
    if (!under && over && over->factor <= settings_.least * (1.0 + 1.0e-9)) {
        finish(BudgetStop::CannotFit, *over);
        return;
    }
    if (points_.size() >= settings_.probes || (under && over && over->factor / under->factor < 1.02)) {
        if (under) {
            finish(BudgetStop::Closest, *under);
        } else {
            finish(BudgetStop::CannotFit, *over);
        }
        return;
    }

    // The next guess: cost grows about in proportion to the factor, so
    // between two measures it is read off the line through them.
    const auto through = [target](const BudgetPoint& a, const BudgetPoint& b) -> std::optional<double> {
        if (!std::isfinite(a.ms) || !std::isfinite(b.ms) || b.ms <= a.ms || b.factor == a.factor) {
            return std::nullopt;
        }
        return a.factor + (target - a.ms) * (b.factor - a.factor) / (b.ms - a.ms);
    };
    std::optional<double> guess;
    if (under && over) {
        guess = through(*under, *over);
    } else if (under) {
        // Only under: from the two largest, else in proportion. Proportion
        // undershoots when part of the cost does not grow, which is the
        // safe side.
        std::optional<BudgetPoint> second;
        for (const BudgetPoint& point : points_) {
            if (point.factor < under->factor && (!second || point.factor > second->factor)) {
                second = point;
            }
        }
        if (second) {
            guess = through(*second, *under);
        }
        if (!guess) {
            guess = under->factor * target / std::max(under->ms, 1.0e-3);
        }
        guess = std::min(*guess, under->factor * 16.0);
    } else {
        std::optional<BudgetPoint> second;
        for (const BudgetPoint& point : points_) {
            if (point.factor > over->factor && (!second || point.factor < second->factor)) {
                second = point;
            }
        }
        if (second) {
            guess = through(*over, *second);
        }
        if (!guess) {
            guess = std::isfinite(over->ms) ? over->factor * target / over->ms : over->factor * 0.5;
        }
        guess = std::max(*guess, over->factor / 16.0);
    }
    // Inside what is known, or halfway (by ratio) across it.
    double factor = guess.value_or(0.0);
    const bool inside = std::isfinite(factor) && factor > 0.0 && (!under || factor > under->factor) &&
                        (!over || factor < over->factor);
    if (!inside) {
        factor = under && over ? std::sqrt(under->factor * over->factor)
                 : under       ? under->factor * 2.0
                               : over->factor * 0.5;
    }
    factor = std::clamp(factor, settings_.least, settings_.most);
    for (const BudgetPoint& point : points_) {
        if (std::fabs(point.factor - factor) <= 1.0e-3 * factor) {
            // Measured already: nothing left between.
            if (under) {
                finish(under->factor >= settings_.most * (1.0 - 1.0e-9) ? BudgetStop::Limit : BudgetStop::Closest,
                       *under);
            } else {
                finish(BudgetStop::CannotFit, *over);
            }
            return;
        }
    }
    next_ = factor;
}

// ------------------------------------------------------------ a run

BudgetRun::BudgetRun(Document page, DensityOptions options, DensityLimit limit, bool allow_down)
    : page_{std::move(page)}, options_{options}, limit_{std::move(limit)},
      search_{BudgetSearch::Settings{.budget_ms = static_cast<double>(page_.budget_ms),
                                     .most = std::max(limit_.factor, 1.0),
                                     .allow_down = allow_down}},
      nothing_{density_knobs(page_) == 0U} {}

Document BudgetRun::page_at(double factor) const {
    return scale_density(page_, factor, options_);
}

BudgetOutcome BudgetRun::outcome() const {
    BudgetOutcome out;
    out.budget_ms = static_cast<double>(page_.budget_ms);
    if (nothing_) {
        out.stop = BudgetStop::Nothing;
        out.words = "Nothing to add more of";
        return out;
    }
    out.stop = search_.stop();
    out.factor = search_.factor();
    out.ms = search_.ms();
    const std::string uses = "Uses " + short_number(out.ms) + " of " + short_number(out.budget_ms) + " ms";
    const std::string more = short_number(out.factor) + "x the particles";
    const bool changes = std::fabs(out.factor - 1.0) > 1.0e-6;
    switch (out.stop) {
    case BudgetStop::Searching: out.words = progress(); break;
    case BudgetStop::Fits: out.words = uses; break;
    case BudgetStop::InBand:
    case BudgetStop::Closest: out.words = changes ? uses + ", " + more : uses; break;
    case BudgetStop::Limit:
        out.limit = limit_.reason;
        out.words = (changes ? uses + ", " + more : uses) +
                    (limit_.reason.empty() ? std::string{", as far as it goes"} : ". Held by " + limit_.reason);
        break;
    case BudgetStop::Over:
        out.words = std::isfinite(out.ms) ? "Over: uses " + short_number(out.ms) + " of " + short_number(out.budget_ms) + " ms"
                                          : std::string{"Over: too heavy to draw"};
        break;
    case BudgetStop::CannotFit: out.words = "Over even at " + more; break;
    case BudgetStop::Nothing: out.words = "Nothing to add more of"; break;
    }
    const bool taken =
        out.stop == BudgetStop::InBand || out.stop == BudgetStop::Closest || out.stop == BudgetStop::Limit;
    if (taken && changes) {
        out.page = page_at(out.factor);
    }
    return out;
}

std::string BudgetRun::progress() const {
    const std::optional<double> factor = next();
    if (!factor) {
        return {};
    }
    if (search_.points().empty()) {
        return "Measuring";
    }
    return "Trying " + short_number(*factor) + "x";
}

} // namespace ajfx::studio
