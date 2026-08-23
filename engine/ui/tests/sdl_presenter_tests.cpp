#include <mediaserver/ui/sdl_presenter.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace show = mediaserver::show;
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

constexpr std::array<Case, 8U> cases{{
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
