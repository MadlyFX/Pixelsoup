#pragma once

#include "document.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Filling a page's frame-time budget: "Maximize budget".
//
// A page has a budget, milliseconds of GPU a frame the effect may take
// (Document::budget_ms). Maximizing scales every amount on the page together
// by one factor -- a Stream's amount, a Burst's count, the puffs and sparks
// born where things hit, a trail's amounts, the pieces in a heap or a stack
// and the pieces a Break cuts a thing into -- and, to keep the look, makes a
// particle layer's particles smaller as there are more of them. The factor
// is found by measuring: the application draws the page at a few factors in
// a preview of its own and reports what each frame cost, and the search here
// picks the next factor to try until the page sits just under its budget.
//
// - **Caps first.** Scaling stops where a runtime limit would bind: a
//   control's own range, a layer the compiler would refuse, a solid world
//   past what particles can meet (ajfx::max_body_colliders) or a baked layer
//   past what a cache holds. The outcome names it, and nothing is ever
//   thinned or dropped to fit.
// - **Never quietly less.** A page already over its budget is not scaled
//   down unless asked to (`allow_down`), and the outcome says it is over.
// - **One edit.** The page is not touched while it is measured: every probe
//   is a copy. The outcome carries the page to put in place, once.
// - **Deterministic.** Scaling changes values only. Uids, seeds and module
//   ids are the page's own, so the scaled page reruns to the same bits.
namespace ajfx::studio {

struct DensityOptions final {
    // Particle layers' particles shrink as there are more (by the square root
    // of the factor), so they cover about the same area and the look holds.
    bool keep_look{true};
};

// The page with every amount scaled by `factor`. Whole numbers (counts,
// pieces) are rounded; every value stays within its control's range.
[[nodiscard]] Document scale_density(const Document& page, double factor, const DensityOptions& options = {});
// How many amounts the page has to scale; 0 when there is nothing to add
// more of (Module blocks are taken literally, and so are not scaled).
[[nodiscard]] std::size_t density_knobs(const Document& page);

// The largest factor the page can be scaled by, and what stops it there.
struct DensityLimit final {
    double factor{1.0};
    // Empty when nothing binds before `most`.
    std::string reason;
};
// Compiles the page at factors up to `most` (a dozen compiles at most), so
// it belongs off the interface thread.
[[nodiscard]] DensityLimit density_limit(const Document& page, const DensityOptions& options, double most = 256.0);

// Why the search stopped.
enum class BudgetStop : std::uint8_t {
    Searching,
    // Already within the budget's band; nothing to change.
    Fits,
    // Measured within the band.
    InBand,
    // A limit binds before the budget does (DensityLimit::reason).
    Limit,
    // The factors under and over the budget closed in, or the probes ran
    // out: the largest measured under it is taken.
    Closest,
    // Over the budget at the page as it is, and not asked to scale down.
    Over,
    // Over the budget even at the smallest factor.
    CannotFit,
    // Nothing on the page to scale.
    Nothing,
};

// One measured factor.
struct BudgetPoint final {
    double factor{1.0};
    // Milliseconds a frame; infinite when the page could not be drawn at all
    // or could not keep up.
    double ms{0.0};
};

// The search, numbers only: which factor to measure next, given what the
// factors measured so far cost. The page at factor 1 is measured first.
class BudgetSearch final {
public:
    struct Settings final {
        double budget_ms{default_budget_ms};
        // The band to land in, as shares of the budget.
        double low{0.90};
        double high{0.95};
        // The largest factor allowed (DensityLimit::factor) and the least.
        double most{256.0};
        double least{0.02};
        bool allow_down{false};
        std::size_t probes{8U};
    };

    explicit BudgetSearch(Settings settings);

    // The factor to measure next; nothing once the search is done.
    [[nodiscard]] std::optional<double> next() const noexcept { return next_; }
    // What the factor `next()` gave cost, in milliseconds a frame.
    void report(double ms);

    [[nodiscard]] bool done() const noexcept { return stop_ != BudgetStop::Searching; }
    [[nodiscard]] BudgetStop stop() const noexcept { return stop_; }
    // The factor taken, and what it measured. 1 (and the first measure)
    // where nothing changes.
    [[nodiscard]] double factor() const noexcept { return chosen_.factor; }
    [[nodiscard]] double ms() const noexcept { return chosen_.ms; }
    [[nodiscard]] const std::vector<BudgetPoint>& points() const noexcept { return points_; }
    [[nodiscard]] const Settings& settings() const noexcept { return settings_; }

private:
    void decide();
    void finish(BudgetStop stop, BudgetPoint chosen);

    Settings settings_;
    std::vector<BudgetPoint> points_;
    std::optional<double> next_{1.0};
    BudgetStop stop_{BudgetStop::Searching};
    BudgetPoint chosen_{};
};

// What a run came to.
struct BudgetOutcome final {
    BudgetStop stop{BudgetStop::Searching};
    double factor{1.0};
    double ms{0.0};
    double budget_ms{default_budget_ms};
    // What binds, for BudgetStop::Limit.
    std::string limit;
    // The page to put in place, as one edit; nothing where nothing changes.
    std::optional<Document> page;
    // A few words for the page: "Uses 3.7 of 4 ms, 2.3x the particles".
    std::string words;
};

// A whole "Maximize budget": the page, its limit (worked out beforehand,
// off the interface thread) and the search. Probes are copies of the page;
// the page itself is only replaced by the outcome.
class BudgetRun final {
public:
    BudgetRun(Document page, DensityOptions options, DensityLimit limit, bool allow_down);

    // The factor to measure next, and the page to draw for it; nothing once
    // the run is done.
    [[nodiscard]] std::optional<double> next() const noexcept {
        return nothing_ ? std::nullopt : search_.next();
    }
    [[nodiscard]] Document page_at(double factor) const;
    void report(double ms) { search_.report(ms); }

    [[nodiscard]] bool done() const noexcept { return nothing_ || search_.done(); }
    [[nodiscard]] const BudgetSearch& search() const noexcept { return search_; }
    [[nodiscard]] BudgetOutcome outcome() const;
    // A few words while it runs: "Trying 2.3x".
    [[nodiscard]] std::string progress() const;

private:
    Document page_;
    DensityOptions options_;
    DensityLimit limit_;
    BudgetSearch search_;
    bool nothing_{false};
};

// What the page shows of a run: under way, and what the last one came to.
struct BudgetView final {
    bool running{false};
    // Progress while running, else the last outcome's words.
    std::string status;
    // The last run found the page over its budget: the button offers to
    // scale it down.
    bool over{false};
};

// What the page asks of the application when the button is pressed.
struct BudgetRequest final {
    // Stop the run under way.
    bool stop{false};
    // Scale down when over, as offered.
    bool allow_down{false};
    DensityOptions options;
};

} // namespace ajfx::studio
