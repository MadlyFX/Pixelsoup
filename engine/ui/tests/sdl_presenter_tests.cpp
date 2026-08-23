#include <mediaserver/rhi/device.hpp>
#include <mediaserver/ui/gpu_output_target.hpp>
#include <mediaserver/ui/sdl_presenter.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <span>
#include <vector>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace show = mediaserver::show;
namespace rhi = mediaserver::rhi;
namespace ui = mediaserver::ui;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define UI_CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr core::u32 window_width = 128U;
constexpr core::u32 window_height = 96U;

[[nodiscard]] ui::SdlPresenterConfig config(
    std::string_view title = "presenter-test") {
    ui::SdlPresenterConfig created{};
    created.title = std::string{title};
    created.width = window_width;
    created.height = window_height;
    // A test window must not be resizable: a window manager that resized it
    // would scale the readback and make the comparison meaningless.
    created.resizable = false;
    created.accepts_commands = true;
    return created;
}

[[nodiscard]] show::TestPatternInput frame(core::u64 index) {
    show::TestPatternInput input{};
    input.show_frame = show::ShowFrame{index};
    input.control_frame = cmd::FrameNumber{index};
    input.rate = show::FrameRate{50U, 1U};
    input.state = show::TransportState::Playing;
    return input;
}

[[nodiscard]] std::unique_ptr<show::Presenter> open(
    const ui::SdlPresenterConfig& settings) {
    auto created = ui::SdlPresenter::create(settings);
    UI_CHECK(created.has_value());
    return std::move(created).value();
}

// Synthesises a key press addressed at one window. This is how the operator
// input path is exercised without a human at a keyboard.
void press(const show::Presenter& presenter, SDL_Scancode scancode) {
    // A static downcast, not a dynamic one: the engine libraries are built
    // without RTTI, and every presenter this test opens is an SdlPresenter.
    const auto& window = static_cast<const ui::SdlPresenter&>(presenter);

    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = static_cast<SDL_WindowID>(window.window_id());
    event.key.scancode = scancode;
    event.key.down = true;
    event.key.repeat = false;
    UI_CHECK(SDL_PushEvent(&event));
}

void a_malformed_surface_is_refused_before_touching_the_platform() {
    auto zero_width = config();
    zero_width.width = 0U;
    const auto refused = ui::SdlPresenter::create(zero_width);
    UI_CHECK(!refused.has_value());
    UI_CHECK(refused.error() == show::PresenterError::InvalidConfiguration);
}

void what_reaches_the_display_is_what_was_rasterised() {
    auto presenter = open(config());
    UI_CHECK(presenter->windowed());

    UI_CHECK(presenter->present(frame(1'234U)) == show::PresenterStatus::Presented);

    // The whole path is checked here, not just the rasteriser: the readback
    // comes from the renderer, so a wrong pixel order, a wrong pitch, or a
    // failed upload all show up as a digest mismatch.
    const auto presented = presenter->readback();
    UI_CHECK(static_cast<bool>(presented));
    UI_CHECK(presented.width == window_width);
    UI_CHECK(presented.height == window_height);
    UI_CHECK(presented.digest == show::frame_buffer_digest(presenter->surface()));
}

void every_frame_reaches_the_display_in_turn() {
    auto presenter = open(config());

    core::u64 previous = 0U;
    for (core::u64 index = 100U; index < 105U; ++index) {
        UI_CHECK(presenter->present(frame(index)) == show::PresenterStatus::Presented);
        const auto presented = presenter->readback();
        UI_CHECK(static_cast<bool>(presented));
        UI_CHECK(
            presented.digest
            == show::frame_buffer_digest(presenter->surface()));
        UI_CHECK(presented.digest != previous);
        previous = presented.digest;
    }
}

void two_windows_present_independently() {
    auto first = open(config("presenter-test-a"));
    auto second = open(config("presenter-test-b"));

    UI_CHECK(first->present(frame(7U)) == show::PresenterStatus::Presented);
    UI_CHECK(second->present(frame(8U)) == show::PresenterStatus::Presented);

    const auto left = first->readback();
    const auto right = second->readback();
    UI_CHECK(static_cast<bool>(left));
    UI_CHECK(static_cast<bool>(right));
    UI_CHECK(left.digest == show::frame_buffer_digest(first->surface()));
    UI_CHECK(right.digest == show::frame_buffer_digest(second->surface()));
    // Different show frames must not collapse to the same picture, which is
    // the property the whole two-node comparison rests on.
    UI_CHECK(left.digest != right.digest);

    // Closing one window must not disturb the other; the event queue is shared
    // and each presenter is addressed by its own window id.
    first.reset();
    UI_CHECK(second->present(frame(9U)) == show::PresenterStatus::Presented);
}

void an_operator_window_reports_transport_keys_in_order() {
    auto presenter = open(config());
    UI_CHECK(presenter->present(frame(1U)) == show::PresenterStatus::Presented);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::None);

    press(*presenter, SDL_SCANCODE_SPACE);
    press(*presenter, SDL_SCANCODE_RIGHT);
    press(*presenter, SDL_SCANCODE_LEFT);
    press(*presenter, SDL_SCANCODE_UP);
    press(*presenter, SDL_SCANCODE_DOWN);
    press(*presenter, SDL_SCANCODE_HOME);
    // The queue is drained by presenting, which is where the shell pumps.
    UI_CHECK(presenter->present(frame(2U)) == show::PresenterStatus::Presented);

    UI_CHECK(presenter->take_command() == show::PresenterCommand::TogglePlay);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::StepForward);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::StepBack);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::JumpForward);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::JumpBack);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::GoToStart);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::None);
}

void a_quit_key_closes_the_output_rather_than_queueing() {
    auto presenter = open(config());
    UI_CHECK(presenter->present(frame(1U)) == show::PresenterStatus::Presented);

    press(*presenter, SDL_SCANCODE_ESCAPE);
    UI_CHECK(presenter->present(frame(2U)) == show::PresenterStatus::Closed);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::None);
    // Closing latches: a later boundary must not resume presenting.
    UI_CHECK(presenter->present(frame(3U)) == show::PresenterStatus::Closed);
}

void an_output_window_queues_no_operator_commands_by_default() {
    auto settings = config();
    settings.accepts_commands = false;
    auto presenter = open(settings);

    press(*presenter, SDL_SCANCODE_SPACE);
    UI_CHECK(presenter->present(frame(1U)) == show::PresenterStatus::Presented);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::None);

    // Quit is not a transport command, so it still closes an input-inert
    // output window.
    press(*presenter, SDL_SCANCODE_ESCAPE);
    UI_CHECK(presenter->present(frame(2U)) == show::PresenterStatus::Closed);
}

void the_video_subsystem_is_released_with_the_last_window() {
    // Repeated open/close cycles must not leak the subsystem reference count,
    // which would eventually refuse a window.
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto presenter = open(config());
        UI_CHECK(presenter->present(frame(42U)) == show::PresenterStatus::Presented);
    }
    auto presenter = open(config());
    UI_CHECK(presenter->present(frame(43U)) == show::PresenterStatus::Presented);
}

struct Case final {
    const char* name;
    void (*function)();
};


// ---------------------------------------------------------------------------
// The GPU output path.
//
// The offscreen target is deliberately separable from the window, so these run
// on a build machine with a GPU and no display. Only the swapchain needs a real
// window, and that is checked separately where one exists.

[[nodiscard]] bool gpu_render_testable() {
    return ui::GpuOutputTarget::shaders_available() && rhi::Device::available();
}

// A real window is required to claim a swapchain, and SDL's dummy video driver
// cannot back one. Asked once, by trying: sniffing the driver name would be a
// guess about which drivers can present, and the answer this needs is simply
// whether this one did.
[[nodiscard]] bool gpu_window_testable() {
    static const bool testable = [] {
        if (!gpu_render_testable()) {
            return false;
        }
        auto settings = config("gpu-probe");
        settings.gpu = true;
        const auto created = ui::SdlPresenter::create(settings);
        return created.has_value();
    }();
    return testable;
}

[[nodiscard]] std::array<core::u8, 4U> sample(
    const show::FrameBufferView& view,
    core::u32 x,
    core::u32 y) {
    const auto offset = static_cast<std::size_t>(y) * view.stride
        + static_cast<std::size_t>(x) * 4U;
    return {
        view.pixels[offset],
        view.pixels[offset + 1U],
        view.pixels[offset + 2U],
        view.pixels[offset + 3U],
    };
}

// Renders one frame into an offscreen target and hands back the pixels.
[[nodiscard]] std::vector<core::u8> render_gpu_frame(
    rhi::Device& device,
    ui::GpuOutputTarget& output,
    const show::TestPatternInput& input) {
    auto commands = device.acquire_commands();
    UI_CHECK(commands.has_value());
    auto list = std::move(commands).value();
    UI_CHECK(output.record(list, input));

    const auto bytes = static_cast<core::u32>(output.width()) * output.height() * 4U;
    auto readback = device.create_readback(bytes);
    UI_CHECK(readback.has_value());
    UI_CHECK(list.download(output.texture(), readback.value()));

    auto submitted = device.submit(std::move(list));
    UI_CHECK(submitted.has_value());
    UI_CHECK(std::move(submitted).value().wait());

    const auto mapped = readback.value().map();
    UI_CHECK(mapped.size() >= bytes);
    std::vector<core::u8> pixels(mapped.begin(), mapped.begin() + bytes);
    readback.value().unmap();
    return pixels;
}

// The GPU path has to draw the frame the CPU path describes. Comparing whole
// digests would be a promise about rasterisation rules that neither API makes,
// so this compares the landmarks the pattern exists to convey - the tint, the
// border, the inner field - plus the overall agreement rate.
void the_gpu_path_draws_the_frame_the_cpu_path_describes() {
    if (!gpu_render_testable()) {
        std::cout << "      (skipped: no GPU or no shader bytecode)\n";
        return;
    }

    auto device = rhi::Device::create(rhi::DeviceConfig{});
    UI_CHECK(device.has_value());
    auto output = ui::GpuOutputTarget::create(
        device.value(), window_width, window_height);
    UI_CHECK(output.has_value());

    constexpr core::u64 index = 321U;
    const auto pixels =
        render_gpu_frame(device.value(), output.value(), frame(index));

    auto reference = show::PresenterSurface::create(window_width, window_height);
    UI_CHECK(reference.has_value());
    UI_CHECK(show::render_test_pattern(reference.value().view(), frame(index)));

    const show::FrameBufferView rendered{
        std::span<core::u8>{const_cast<core::u8*>(pixels.data()), pixels.size()},
        window_width,
        window_height,
        window_width * 4U,
        show::PixelFormat::Rgba8888,
    };
    const auto& expected = reference.value().view();

    // Flat regions must agree exactly: a wrong channel order, a wrong clip
    // transform, or a missing quad all show up here.
    const auto border = window_height / 64U == 0U ? 4U : window_height / 64U;
    UI_CHECK(
        sample(rendered, window_width / 2U, border / 2U)
        == sample(expected, window_width / 2U, border / 2U));
    UI_CHECK(
        sample(rendered, window_width / 2U, window_height - border / 2U - 1U)
        == sample(expected, window_width / 2U, window_height - border / 2U - 1U));
    // The node tint is behind the border, so an interior point of the inner
    // field proves the field quad landed on top of it.
    UI_CHECK(
        sample(rendered, border + 2U, window_height / 2U)
        == sample(expected, border + 2U, window_height / 2U));

    // And the frame as a whole: text antialiasing rules are the only place the
    // two can legitimately differ, and that is a small minority of pixels.
    std::size_t matching = 0U;
    const auto total = static_cast<std::size_t>(window_width) * window_height;
    for (core::u32 y = 0U; y < window_height; ++y) {
        for (core::u32 x = 0U; x < window_width; ++x) {
            if (sample(rendered, x, y) == sample(expected, x, y)) {
                ++matching;
            }
        }
    }
    UI_CHECK(matching * 100U >= total * 97U);
}

// Two targets given the same frame must produce identical pixels, which is the
// property two nodes showing one wall depend on.
void two_gpu_targets_render_one_frame_identically() {
    if (!gpu_render_testable()) {
        std::cout << "      (skipped: no GPU or no shader bytecode)\n";
        return;
    }

    auto device = rhi::Device::create(rhi::DeviceConfig{});
    UI_CHECK(device.has_value());
    auto left =
        ui::GpuOutputTarget::create(device.value(), window_width, window_height);
    auto right =
        ui::GpuOutputTarget::create(device.value(), window_width, window_height);
    UI_CHECK(left.has_value());
    UI_CHECK(right.has_value());

    const auto input = frame(9'876U);
    const auto first = render_gpu_frame(device.value(), left.value(), input);
    const auto second = render_gpu_frame(device.value(), right.value(), input);
    UI_CHECK(first == second);

    // And a different frame must actually differ, so the comparison above is
    // not passing on two identically blank targets.
    const auto other = render_gpu_frame(device.value(), right.value(), frame(9'877U));
    UI_CHECK(other != first);
}

void the_gpu_path_reports_per_frame_submission_time() {
    if (!gpu_window_testable()) {
        std::cout << "      (skipped: needs a real window for a swapchain)\n";
        return;
    }

    auto settings = config("gpu-timing");
    settings.gpu = true;
    auto presenter = open(settings);
    UI_CHECK(presenter->timing().gpu);
    UI_CHECK(presenter->timing().frames == 0U);

    for (core::u64 index = 0U; index < 8U; ++index) {
        const auto status = presenter->present(frame(index));
        UI_CHECK(
            status == show::PresenterStatus::Presented
            || status == show::PresenterStatus::Skipped);
    }

    const auto timing = presenter->timing();
    UI_CHECK(timing.gpu);
    UI_CHECK(timing.frames == 8U);
    // Every frame after the first retires the one before it, so by now a real
    // submission has been measured.
    UI_CHECK(timing.last_submit_to_observed_ns > 0U);
    UI_CHECK(
        timing.peak_submit_to_observed_ns >= timing.last_submit_to_observed_ns);
    // The exact number: time actually spent inside present().
    UI_CHECK(timing.last_present_ns > 0U);
    UI_CHECK(timing.peak_present_ns >= timing.last_present_ns);

    const auto presented = presenter->readback();
    UI_CHECK(static_cast<bool>(presented));
    UI_CHECK(presented.digest != 0U);
}

// The point of the whole slice: the frame path stops uploading pixels. The
// instance count does not depend on resolution, so the gap widens with it.
void the_gpu_frame_path_uploads_instances_rather_than_pixels() {
    std::array<show::PatternQuad, show::max_pattern_quads> quads{};
    const auto small =
        show::build_pattern_quads(frame(4'321U), 640U, 360U, quads);
    const auto large =
        show::build_pattern_quads(frame(4'321U), 3'840U, 2'160U, quads);

    UI_CHECK(small.count > 0U);
    UI_CHECK(!small.truncated);
    UI_CHECK(!large.truncated);

    const auto instance_bytes = large.count * sizeof(show::PatternQuad);
    const auto surface_bytes = static_cast<std::size_t>(3'840U) * 2'160U * 4U;
    // Three orders of magnitude at 4K, and the CPU path would send the whole
    // surface every frame per output.
    UI_CHECK(instance_bytes * 1'000U < surface_bytes);
}

void a_gpu_request_is_refused_rather_than_downgraded() {
    // The refusal only has a meaning where the GPU path cannot be provided,
    // which is exactly the headless lane: no swapchain, so no windowed output.
    if (gpu_window_testable()) {
        return;
    }
    auto settings = config("gpu-refused");
    settings.gpu = true;
    const auto refused = ui::SdlPresenter::create(settings);
    UI_CHECK(!refused.has_value());
    UI_CHECK(refused.error() == show::PresenterError::DisplayUnavailable);
}

constexpr std::array<Case, 13U> cases{{
    {"malformed surface refused",
     &a_malformed_surface_is_refused_before_touching_the_platform},
    {"display shows what was rasterised",
     &what_reaches_the_display_is_what_was_rasterised},
    {"each frame reaches the display", &every_frame_reaches_the_display_in_turn},
    {"two windows are independent", &two_windows_present_independently},
    {"operator keys in order",
     &an_operator_window_reports_transport_keys_in_order},
    {"quit closes the output", &a_quit_key_closes_the_output_rather_than_queueing},
    {"output windows ignore transport keys",
     &an_output_window_queues_no_operator_commands_by_default},
    {"video subsystem is released",
     &the_video_subsystem_is_released_with_the_last_window},
    {"the GPU path draws the frame the CPU path describes",
     &the_gpu_path_draws_the_frame_the_cpu_path_describes},
    {"two GPU targets render one frame identically",
     &two_gpu_targets_render_one_frame_identically},
    {"the GPU path reports per-frame submission time",
     &the_gpu_path_reports_per_frame_submission_time},
    {"the GPU frame path uploads instances rather than pixels",
     &the_gpu_frame_path_uploads_instances_rather_than_pixels},
    {"a GPU request is refused rather than downgraded",
     &a_gpu_request_is_refused_rather_than_downgraded},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
