#pragma once

#include <array>
#include <cstddef>
#include <memory>

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/show/test_pattern.hpp>

namespace mediaserver::show {

enum class PresenterStatus : core::u8 {
    Presented,
    // Nothing was drawn this boundary and nothing is wrong: the window is
    // minimised or otherwise not showing. Playback continues.
    Skipped,
    // The viewer closed the output. Playback should stop.
    Closed,
    Failed,
};

enum class PresenterError : core::u8 {
    InvalidConfiguration,
    AllocationFailed,
    // The platform refused a window, renderer, or texture.
    DisplayUnavailable,
};

// Operator intent read from an output window. A node ignores these: what it
// presents is replicated state, so only the Director acts on them.
enum class PresenterCommand : core::u8 {
    None,
    Quit,
    TogglePlay,
    StepBack,
    StepForward,
    JumpBack,
    JumpForward,
    GoToStart,
};

// A caller-owned 32-bit surface allocated once. Both the headless and windowed
// presenters rasterise into one of these, so the presented pixels are always
// available for a digest without reading back a GPU texture.
class PresenterSurface final {
public:
    [[nodiscard]] static core::Result<PresenterSurface, PresenterError> create(
        core::u32 width,
        core::u32 height) noexcept;

    PresenterSurface(const PresenterSurface&) = delete;
    PresenterSurface& operator=(const PresenterSurface&) = delete;
    PresenterSurface(PresenterSurface&& other) noexcept;
    PresenterSurface& operator=(PresenterSurface&& other) noexcept;
    ~PresenterSurface() = default;

    [[nodiscard]] const FrameBufferView& view() const noexcept { return view_; }
    [[nodiscard]] core::u32 width() const noexcept { return view_.width; }
    [[nodiscard]] core::u32 height() const noexcept { return view_.height; }
    [[nodiscard]] core::u32 stride() const noexcept { return view_.stride; }
    [[nodiscard]] const core::u8* pixels() const noexcept {
        return view_.pixels.data();
    }

private:
    PresenterSurface(
        std::unique_ptr<core::u8[]> storage,
        std::size_t bytes,
        core::u32 width,
        core::u32 height,
        core::u32 stride) noexcept;

    std::unique_ptr<core::u8[]> storage_;
    FrameBufferView view_{};
};

// The result of reading the presented output back from the platform. This is a
// start-up self-check, not a per-frame operation: it proves the whole upload
// and present path - including pixel order - actually put the rasterised bytes
// on the display, which no amount of testing the rasteriser alone can show.
struct PresenterReadback final {
    core::u32 width{};
    core::u32 height{};
    core::u64 digest{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return width != 0U && height != 0U;
    }
    bool operator==(const PresenterReadback&) const = default;
};

// The presentation seam.
//
// present() runs immediately after the commit boundary, never inside it. That
// ordering is the point: the headless presenter allocates nothing, but a
// windowed one hands pixels to a platform driver that may allocate or block,
// and none of that is allowed to delay a commit the cluster is waiting on.
//
// The interface deliberately carries no document reference. Everything a
// window needs has already been resolved from the committed root, so a
// presenter can never walk the trie or hold the document past a frame.
//
// Implementations are created behind a unique_ptr, which keeps a windowed
// presenter free to own raw platform handles without being movable.
class Presenter {
public:
    Presenter() = default;
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    Presenter(Presenter&&) = delete;
    Presenter& operator=(Presenter&&) = delete;
    virtual ~Presenter() = default;

    [[nodiscard]] virtual PresenterStatus present(
        const TestPatternInput& frame) noexcept = 0;

    // The pixels most recently presented. Stable until the next present().
    [[nodiscard]] virtual const FrameBufferView& surface() const noexcept = 0;

    // Pops one queued operator command. A presenter with no input surface
    // never produces any.
    [[nodiscard]] virtual PresenterCommand take_command() noexcept {
        return PresenterCommand::None;
    }

    // True when this presenter drives a real window. Used only for reporting.
    [[nodiscard]] virtual bool windowed() const noexcept { return false; }

    // Reads back what the platform actually presented. A presenter with no
    // display pipeline reports nothing, which is not a failure.
    [[nodiscard]] virtual PresenterReadback readback() noexcept { return {}; }
};

using PresenterResult =
    core::Result<std::unique_ptr<Presenter>, PresenterError>;

// Rasterises to memory and nothing else. This is the presenter a headless node
// uses, and the one that makes an automated run checkable: the surface digest
// is exactly what a window would have shown.
class HeadlessPresenter final : public Presenter {
public:
    [[nodiscard]] static PresenterResult create(
        core::u32 width,
        core::u32 height) noexcept;

    [[nodiscard]] PresenterStatus present(
        const TestPatternInput& frame) noexcept override;
    [[nodiscard]] const FrameBufferView& surface() const noexcept override {
        return surface_.view();
    }

private:
    explicit HeadlessPresenter(PresenterSurface surface) noexcept
        : surface_(std::move(surface)) {}

    PresenterSurface surface_;
};

} // namespace mediaserver::show
