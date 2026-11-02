#include "budget_meter.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

namespace ajfx::studio {
namespace {

namespace rhi = mediaserver::rhi;

// Frames counted for a measure, and the frames after loading and catching
// up that are not: the first frames after a package is adopted pay for
// catching up with the start frame and for first uploads.
constexpr std::size_t samples_wanted = 16U;
constexpr std::uint32_t warm_up_frames = 6U;
// A new meter's first measure warms up longer: its renderer builds its
// pipelines and fills its histories over its first frames.
constexpr std::uint32_t first_warm_up_frames = 90U;
// A page that falls behind this often after it first caught up cannot keep
// up at all.
constexpr std::uint32_t behind_limit = 4U;
// Frames played before a measure gives up.
constexpr std::uint64_t give_up_frames = 900U;

} // namespace

std::unique_ptr<BudgetMeter> BudgetMeter::create(rhi::Device& device, std::filesystem::path package,
                                                 std::string& error) {
    std::unique_ptr<Preview> preview = Preview::create(device, error);
    if (!preview) {
        return nullptr;
    }
    return std::unique_ptr<BudgetMeter>{new BudgetMeter{std::move(preview), std::move(package)}};
}

BudgetMeter::BudgetMeter(std::unique_ptr<Preview> preview, std::filesystem::path package)
    : preview_{std::move(preview)}, builder_{std::move(package), preview_instance_seed(), false} {}

BudgetMeter::~BudgetMeter() {
    for (Flight& flight : flights_) {
        if (flight.work) {
            static_cast<void>(flight.work.wait());
        }
    }
    std::error_code error;
    std::filesystem::remove(builder_.preview_path(), error);
}

void BudgetMeter::measure(const Document* page, std::uint64_t from, const std::filesystem::path& base) {
    ++measure_;
    active_ = true;
    stage_only_ = page == nullptr;
    has_package_ = false;
    from_ = from;
    played_ = 0U;
    ready_frames_ = 0U;
    fell_behind_ = 0U;
    was_ready_ = false;
    samples_.clear();
    result_.reset();
    if (page != nullptr) {
        builder_.submit(*page, ++revision_, base);
    }
}

void BudgetMeter::cancel() {
    ++measure_;
    active_ = false;
    result_.reset();
}

std::optional<BudgetMeasure> BudgetMeter::take() {
    std::optional<BudgetMeasure> out = std::move(result_);
    result_.reset();
    return out;
}

void BudgetMeter::finish(double ms, std::string issue) {
    result_ = BudgetMeasure{ms, std::move(issue)};
    warmed_ = true;
    active_ = false;
    ++measure_;
}

void BudgetMeter::frame(rhi::Device& device, const PreviewFrame& look) {
    // Finished frames give their GPU time, if they were frames to count.
    for (Flight& flight : flights_) {
        if (flight.work && flight.work.complete()) {
            const auto gpu = flight.work.gpu_execution();
            if (flight.counted && flight.measure == measure_ && active_ && gpu.count() > 0) {
                samples_.push_back(std::chrono::duration<double, std::milli>(gpu).count());
            }
            flight.work = rhi::SubmittedWork{};
        }
    }
    if (std::optional<BuildResult> built = builder_.take_build()) {
        if (active_ && !stage_only_ && built->revision == revision_) {
            if (built->written) {
                has_package_ = true;
                ++package_generation_;
            } else {
                std::string issue = !built->error.empty() ? built->error : built->compiled.error;
                finish(std::numeric_limits<double>::infinity(),
                       issue.empty() ? std::string{"It could not be built"} : std::move(issue));
                return;
            }
        }
    }
    if (!active_) {
        return;
    }
    if (samples_.size() >= samples_wanted) {
        std::vector<double> sorted = samples_;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2U),
                         sorted.end());
        finish(sorted[sorted.size() / 2U], {});
        return;
    }
    if (!stage_only_ && !has_package_) {
        return; // still building
    }
    if (played_ > give_up_frames) {
        finish(std::numeric_limits<double>::infinity(), "It never settled");
        return;
    }
    Flight* slot = nullptr;
    for (Flight& flight : flights_) {
        if (!flight.work) {
            slot = &flight;
            break;
        }
    }
    if (slot == nullptr) {
        return;
    }
    PreviewFrame frame = look;
    frame.package = stage_only_ ? std::string{} : builder_.preview_path().string();
    frame.package_generation = package_generation_;
    frame.revision = package_generation_;
    frame.frame = from_ + played_;
    frame.paused = false;
    // The defaults: what is measured is the page, not what is being tried.
    frame.parameters.clear();
    auto acquired = device.acquire_commands();
    if (!acquired) {
        return;
    }
    rhi::CommandList commands = std::move(acquired).value();
    static_cast<void>(preview_->render(commands, frame));
    auto submitted = device.submit(std::move(commands));
    if (!submitted) {
        finish(std::numeric_limits<double>::infinity(), "The GPU refused it");
        return;
    }
    ++played_;
    const PreviewStatus& status = preview_->status();
    if (!status.issue.empty()) {
        finish(std::numeric_limits<double>::infinity(), status.issue);
        return;
    }
    // Ready: drawn at this frame's own step, with nothing left to load.
    const bool ready = !status.waiting && (stage_only_ || status.drawing);
    if (ready) {
        ++ready_frames_;
        was_ready_ = true;
    } else if (was_ready_) {
        ++fell_behind_;
        if (fell_behind_ > behind_limit) {
            finish(std::numeric_limits<double>::infinity(), "Its physics cannot keep up");
            return;
        }
    }
    slot->work = std::move(submitted).value();
    slot->measure = measure_;
    slot->counted = ready && ready_frames_ > (warmed_ ? warm_up_frames : first_warm_up_frames);
}

} // namespace ajfx::studio
