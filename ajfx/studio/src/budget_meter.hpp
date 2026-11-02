#pragma once

#include "document.hpp"
#include "package_builder.hpp"
#include "preview.hpp"

#include <mediaserver/rhi/device.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Measuring what a page costs a frame, for "Maximize budget" (budget.hpp).
//
// The meter is a second preview, never shown: the same media server
// renderer and stage as the one the artist looks at, drawing the page to be
// measured into a command list of its own at the visible preview's size and
// camera. The GPU times that list with its timestamp queries
// (rhi::SubmittedWork::gpu_execution), so what is measured is GPU work alone:
// the particle simulation's steps, sorting and drawing, and the stage around
// them. The stage alone is measured first, and a page's cost is what it adds
// to that.
//
// A page is played from a little before its busiest step (Analysis::peak_step)
// one show frame a frame, as it plays on the wall, and the middle of a
// short window of frames is taken. Frames still loading or catching up are
// not counted. Rigid physics runs on its own threads, off the frame: a page
// whose physics cannot keep up falls behind, and that counts as over any
// budget rather than as a number.
//
// The page under edit is never touched: the meter builds its own package
// from a copy, on a worker of its own.
namespace ajfx::studio {

struct BudgetMeasure final {
    // Median GPU milliseconds a frame; infinite when it could not be drawn
    // or could not keep up (`issue` says which).
    double ms{0.0};
    std::string issue;
};

class BudgetMeter final {
public:
    // Null, with the reason in `error`, when the second preview cannot start.
    [[nodiscard]] static std::unique_ptr<BudgetMeter> create(mediaserver::rhi::Device& device,
                                                             std::filesystem::path package, std::string& error);
    ~BudgetMeter();
    BudgetMeter(const BudgetMeter&) = delete;
    BudgetMeter& operator=(const BudgetMeter&) = delete;

    // Starts measuring `page` (none: the stage alone) from show frame
    // `from`. A measure under way is dropped.
    void measure(const Document* page, std::uint64_t from, const std::filesystem::path& base);
    // Stops measuring; anything in flight is let go.
    void cancel();
    [[nodiscard]] bool measuring() const noexcept { return active_; }
    // One frame: reads back finished frames' GPU times and records and
    // submits the next probe frame, at the visible preview's size, camera and
    // backdrop (`look`).
    void frame(mediaserver::rhi::Device& device, const PreviewFrame& look);
    // The measure, once there is one.
    [[nodiscard]] std::optional<BudgetMeasure> take();

private:
    BudgetMeter(std::unique_ptr<Preview> preview, std::filesystem::path package);
    void finish(double ms, std::string issue);

    struct Flight final {
        mediaserver::rhi::SubmittedWork work;
        std::uint64_t measure{0U};
        bool counted{false};
    };

    std::unique_ptr<Preview> preview_;
    PackageBuilder builder_;
    std::array<Flight, 2> flights_{};
    std::uint64_t measure_{0U};
    std::uint64_t revision_{0U};
    bool active_{false};
    bool stage_only_{false};
    bool has_package_{false};
    std::uint64_t package_generation_{0U};
    std::uint64_t from_{0U};
    std::uint64_t played_{0U};
    std::uint32_t ready_frames_{0U};
    std::uint32_t fell_behind_{0U};
    bool was_ready_{false};
    // A measure has been taken: the renderer has warmed up.
    bool warmed_{false};
    std::vector<double> samples_;
    std::optional<BudgetMeasure> result_;
};

} // namespace ajfx::studio
